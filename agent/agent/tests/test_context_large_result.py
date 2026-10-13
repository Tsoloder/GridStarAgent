"""大工具结果落盘 / 历史中段折叠的回归测试。

现场：各向异性处理一次返回 2 万字级的 steps 明细，旧阈值 30000 让这些结果
全程常驻历史、每轮重新计费一次（实测单会话因此多花约 10~15K input token/请求）。
阈值降到 8192 后当场落盘，历史里只留一行 stub；stub 必须带头部预览，
因为关键状态（哪一步 failed）通常就在开头，只留路径会让模型为了看结论再查一次。
"""
import copy
import json

import pytest

from config import config_from_dict
from context import ContextManager


def test_large_result_persists_with_head_preview(tmp_path):
    ctx = ContextManager()
    ctx._sessions_dir = tmp_path

    big = json.dumps({"steps": {"step4_aniso_line_group": "failed"}}) + "x" * 20000
    stub = ctx.persist_large_result(big, "sess-1")

    assert "Result persisted to file" in stub
    assert "%d chars" % len(big) in stub
    # 关键状态留在预览里，模型不必为了看结论再去读文件
    assert "step4_aniso_line_group" in stub
    # 正文没有常驻：stub 远小于原文
    assert len(stub) < 3000

    saved = list((tmp_path / "sess-1" / "results").iterdir())
    assert len(saved) == 1
    assert saved[0].read_text(encoding="utf-8") == big


def test_small_result_passes_through_untouched(tmp_path):
    ctx = ContextManager()
    ctx._sessions_dir = tmp_path

    small = "网格块 ID = 1"
    assert ctx.persist_large_result(small, "sess-1") == small
    assert not (tmp_path / "sess-1").exists()


def _window_config(window: int):
    return config_from_dict({
        "version": 1,
        "default_model": "t/m",
        "providers": [{
            "id": "t", "name": "T", "base_url": "http://localhost:11434",
            "api_key": "k", "default_api": "openai-chat",
        }],
        "models": [{"provider": "t", "id": "m", "context_window": window}],
    })


@pytest.mark.asyncio
async def test_stale_snip_only_fires_under_window_pressure():
    """历史中段折叠只在窗口吃紧时才做。

    它是唯一会改写历史中段的层，而缓存按前缀命中：折叠一条旧结果，它后面
    整段（约 12 条消息、10~20K token）下一轮就要全价重算。实测那个
    88K/1M 的会话里，这类改写贡献了约 45 万 input token 的重算，换下来的
    却是本来就命中缓存的常驻内容。
    """
    big = "网格结果 " * 4000
    base = [
        {"role": "user", "content": "开始"},
        {"role": "assistant", "content": "", "tool_calls": []},
        {"role": "tool", "tool_call_id": "c1", "tool_name": "GetModelParameters",
         "content": big},
    ] + [{"role": "user", "content": "填充 %d" % i} for i in range(20)]

    ctx = ContextManager()
    # 窗口远未吃紧（88K/1M 就是这种情形）：历史必须逐字节保持不变，
    # 否则每轮都在历史中段改写前缀，缓存全废。
    relaxed = await ctx.compress(copy.deepcopy(base), "s", _window_config(1000000))
    assert relaxed[2]["content"] == big

    # 窗口吃紧（但还没到 Layer 1 砍头的程度）：旧结果被截成头尾
    tight = await ctx.compress(copy.deepcopy(base), "s", _window_config(6000))
    assert "已省略" in tight[2]["content"]
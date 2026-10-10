"""回归测试：工具参数记忆不能把已经废弃的参数注回调用。

现场故障：CaptureGridStarWindow 改成零参数（schema 是 {"type": "object",
"properties": {}}）之后，数据目录里的 tool_parameter_memory.json 仍留着
{"window_title": "", "activate": true, "clean": false}。_filter_params 原先只在
`if properties and key not in properties` 时丢弃过期字段，空 properties 被当成
"没有 schema 信息"，于是这三个键每次都被 merge() 注回调用，服务端参数校验以
Unexpected keyword argument 直接拒绝整次调用（会话 9fd16ecf 连撞四次）。
"""
import json

import pytest

import tool_memory


ZERO_ARG_SCHEMA = {
    "type": "object",
    "properties": {},
    "title": "CaptureGridStarWindowArguments",
}
WINDOW_SCHEMA = {
    "type": "object",
    "properties": {
        "window_title": {"type": "string", "default": "GridStar"},
        "activate": {"type": "boolean", "default": True},
        "depth": {"type": "integer"},
    },
}
STALE_ENTRY = {
    "CaptureGridStarWindow": {
        "params": {"window_title": "", "activate": True, "clean": False},
        "updated_at": "2026-10-10T22:01:57.108384",
    }
}


@pytest.fixture()
def memory_path(tmp_path, monkeypatch):
    path = tmp_path / "tool_parameter_memory.json"
    monkeypatch.setattr(tool_memory, "MEMORY_PATH", path)
    return path


def _write(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    return path


def test_zero_argument_schema_discards_stale_remembered_params(memory_path):
    _write(memory_path, STALE_ENTRY)
    schema = ZERO_ARG_SCHEMA

    assert tool_memory.get("CaptureGridStarWindow", schema) == {}
    assert tool_memory.merge("CaptureGridStarWindow", {}, schema) == {}
    # 调用方哪怕自己塞了多余键（模型自由发挥、审批回填），也一并丢掉
    assert tool_memory.merge(
        "CaptureGridStarWindow",
        {"window_title": "", "activate": True, "clean": False},
        schema,
    ) == {}
    assert tool_memory.catalog({"CaptureGridStarWindow": schema}) == ""


def test_merge_discards_undeclared_explicit_args(memory_path):
    merged = tool_memory.merge(
        "FindGridStarWindow",
        {"window_title": "NNW-GridStar", "clean": False, "bogus": 1},
        WINDOW_SCHEMA,
    )

    assert merged == {"window_title": "NNW-GridStar", "activate": True}


def test_merge_keeps_everything_when_schema_is_unknown(memory_path):
    assert tool_memory.merge("Whatever", {"a": 1, "b": "x"}, None) == {"a": 1, "b": "x"}
    assert tool_memory.merge("Whatever", {"a": 1}, {}) == {"a": 1}


def test_merge_prefers_explicit_args_then_memory_then_defaults(memory_path):
    _write(memory_path, {"SomeTool": {"params": {"activate": False, "window_title": "记忆值"}}})

    merged = tool_memory.merge(
        "SomeTool", {"depth": 4, "window_title": "显式值"}, WINDOW_SCHEMA
    )

    assert merged == {"depth": 4, "window_title": "显式值", "activate": False}


def test_remember_forgets_fields_the_schema_no_longer_declares(memory_path):
    _write(memory_path, STALE_ENTRY)

    assert tool_memory.remember("CaptureGridStarWindow", {}, ZERO_ARG_SCHEMA) == {}
    # 陈旧条目被顺手清掉，不再留在文件里等着下一次踩坑
    assert "CaptureGridStarWindow" not in json.loads(memory_path.read_text(encoding="utf-8"))


def test_remember_keeps_declared_params(memory_path):
    values = tool_memory.remember(
        "FindGridStarWindow", {"window_title": "NNW-GridStar", "clean": False}, WINDOW_SCHEMA
    )

    assert values == {"window_title": "NNW-GridStar"}
    assert json.loads(memory_path.read_text(encoding="utf-8"))["FindGridStarWindow"]["params"] == {
        "window_title": "NNW-GridStar"
    }
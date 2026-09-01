# P0 阶段：回归测试 fixture
# 覆盖 5 条核心路径：
# 1. simple_chat — 纯文本问答
# 2. tool_call_flow — 工具调用流程
# 3. auto_mode_flow — auto 模式 update_plan 计划工具 + 工具执行
# 4. format_retry_flow — 格式重试
# 5. structured_continuation — tool_params 确认后 LLM 重出拦截

import json
import os

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), "fixtures")


def load_fixture(name: str) -> dict:
    path = os.path.join(FIXTURES_DIR, f"{name}.json")
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


ALL_FIXTURES = [
    "simple_chat",
    "tool_call_flow",
    "auto_mode_flow",
    "format_retry_flow",
    "structured_continuation",
]

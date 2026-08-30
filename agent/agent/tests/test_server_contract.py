import ast
import hashlib
import inspect
import json
import sys
from pathlib import Path

AGENT_DIR = Path(__file__).resolve().parents[2]
if str(AGENT_DIR) not in sys.path:
    sys.path.insert(0, str(AGENT_DIR))

import registry


def test_tool_registration_contract_is_unchanged():
    tools = registry.TOOLS
    assert len(tools) == 103
    order = "\n".join(tool.__name__ for tool in tools)
    assert hashlib.sha256(order.encode()).hexdigest() == "1d712f479d287a487a20b9274d80f448fed64254a4692c01d101b1411bdfa095"
    contracts = [(tool.__name__, str(inspect.signature(tool)), tool.__doc__) for tool in tools]
    # inspect 与 AST 参数文本格式不同，逐项验证名称、签名存在和 docstring，再冻结源码契约。
    assert all(name == tool.__name__ and signature for (name, signature, _), tool in zip(contracts, tools))
    source_contracts = []
    mappings = []
    for tool in tools:
        node = ast.parse(inspect.getsource(tool)).body[0]
        source_contracts.append((tool.__name__, ast.unparse(node.args), ast.get_docstring(node, clean=False)))
        calls = [n for n in ast.walk(node) if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id == "send_post_request"]
        mappings.append((tool.__name__, ast.unparse(calls[0]) if calls else ""))
    payload = json.dumps(source_contracts, ensure_ascii=False, separators=(",", ":"))
    assert hashlib.sha256(payload.encode()).hexdigest() == "64e295015cd7267e87ba278d7a9c04fa9d8e64162839acfa77c21dd69e7e1793"
    payload = json.dumps(mappings, ensure_ascii=False, separators=(",", ":"))
    assert hashlib.sha256(payload.encode()).hexdigest() == "1d0e8dc8d86411bdebd40c3dbf8f2101709d0cfa9e7f5d205a5facff158ad095"


def test_segment_part_keeps_extended_timeout(monkeypatch):
    calls = []
    monkeypatch.setattr(registry.advanced, "send_post_request", lambda *args, **kwargs: calls.append((args, kwargs)))
    registry.advanced.SegmentPart("out")
    assert calls == [(('SegmentPart', {'outputDir': 'out'}), {'timeout': 360})]

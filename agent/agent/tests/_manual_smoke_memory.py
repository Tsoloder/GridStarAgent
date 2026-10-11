"""P0a 影子采集端到端冒烟：拿真实台账跑一遍提炼，检查落库内容。

手动运行，不进 pytest 采集（文件名不以 test_ 开头）：

    cd agent/agent && python tests/_manual_smoke_memory.py

只读真实台账与配置；候选写进临时库，不动用户数据目录里的 memory.db。
它会打印脱敏后的提炼输入、落库记录、影子期隔离结果和真实 app 的路由响应——
用来回答「模型到底看到了什么」「有没有把路径和 IP 带进去」这两个问题。
"""
import asyncio
import json
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import experience_extractor as ex  # noqa: E402
import experience_memory as em  # noqa: E402
import memory_sanitize as ms  # noqa: E402
import paths  # noqa: E402

line = "=" * 72


def show(title):
    print("\n" + line)
    print(title)
    print(line)


show("1. 真实数据目录")
print("DATA_DIR   :", paths.DATA_DIR)
print("MEMORY_DIR :", paths.MEMORY_DIR)
print("memory.db 存在:", paths.MEMORY_DB_PATH.exists())

ledgers = []
for path in paths.SESSIONS_DIR.rglob("ledger.json"):
    try:
        ledgers.append((path.stat().st_size, path))
    except OSError:
        continue
ledgers.sort(key=lambda pair: pair[0], reverse=True)
print("会话目录数:", len(list(paths.SESSIONS_DIR.iterdir())))
print("ledger.json 数（含回收站）:", len(ledgers))
live = [p for _, p in ledgers if ".trash" not in p.parts]
print("在线会话里的 ledger.json 数:", len(live))
size, target = ledgers[0]
session_id = target.parent.name
raw = json.loads(target.read_text(encoding="utf-8"))
print("选中台账:", session_id, "|", size, "bytes |",
      len(raw.get("calls", [])), "条调用")

show("2. 脱敏后的提炼输入（模型实际看到的东西）")
sanitized = ms.sanitize_ledger(raw)
blob = json.dumps(sanitized, ensure_ascii=False, indent=1)
print(blob[:1400])
print("...")
print("脱敏输入长度:", len(blob))

# 把真实台账里出现过的每一个字符串字面量拿出来，逐个查是否泄漏进输入
leaked = []
def walk(node):
    if isinstance(node, dict):
        for value in node.values():
            walk(value)
    elif isinstance(node, list):
        for value in node:
            walk(value)
    elif isinstance(node, str):
        if len(node) >= 6 and node in blob:
            leaked.append(node)
walk(raw)
print("从原始台账逐字泄漏进输入的字符串数:", len(leaked))
if leaked:
    print("泄漏样本:", leaked[:10])

known = ["10.76.19.98", "A_GridStarCode", "output", ".igs", "face", "solid_", "domain_"]
print("已知敏感片段命中:", {k: (k in blob) for k in known})

show("3. 用真实台账跑一次提炼（脚本化模型）")
real_tools = sorted({c.get("tool", "") for c in sanitized["calls"] if c.get("tool", "")})
print("台账里的真实工具:", real_tools)

scripted = json.dumps({
    "experiences": [{
        "title": "水密修复前先定位自由边再局部缝合",
        "scope": "global",
        "task_type": "watertight",
        "trigger_cues": ["导入 CAD 后自由边计数不为零"],
        "preconditions": ["已成功导入模型并拿到自由边统计"],
        "strategy_steps": [
            "先用 DealWatertight 查询自由边数量，确认缺陷规模",
            "按自由边位置局部修补，不做全局重划",
        ],
        "tool_sequence": [real_tools[0]] if real_tools else [],
        "failure_patterns": ["直接全局重划会放大网格量"],
    }],
    "facts": [{
        "fact_text": "该环境下网格生成服务需要显式提供主机参数",
        "fact_type": "environment",
        "tags": ["网格服务"],
    }],
}, ensure_ascii=False)


class ScriptedRuntime:
    def __init__(self, payload):
        self.payload = payload
        self.calls = []

    async def stream(self, model_key, messages, tools=(), system_prompt=""):
        self.calls.append({"model_key": model_key, "messages": messages,
                           "system_prompt": system_prompt})
        yield SimpleNamespace(type="text_delta", delta=self.payload)


async def main():
    with tempfile.TemporaryDirectory() as tmp:
        store = em.MemoryStore(Path(tmp) / "memory.db")
        runtime = ScriptedRuntime(scripted)
        extractor = ex.ExperienceExtractor(store, runtime, "fake/model",
                                           known_tools=tuple(real_tools))
        result = await extractor.extract_session(
            session_id, sanitized, trigger=ex.TRIGGER_MANUAL, force=True)
        print("提炼结果:", json.dumps(result, ensure_ascii=False))

        print("\n模型收到的 system_prompt 片段:")
        prompt = runtime.calls[0]["messages"][-1]["content"] if runtime.calls else ""
        print(prompt[:900])

        show("4. 落库内容（candidate 阶段）")
        listing = store.list_items(kind="experience", status="candidate")
        print("候选数:", listing["total"])
        for summary in listing["items"]:
            item = store.get(summary["id"])
            print(json.dumps(item, ensure_ascii=False, indent=1)[:1600])

        show("5. 影子期隔离验证")
        print("候选是否可被 recall 检索到:",
              store.recall(query="水密修复 自由边", scopes=["global"], task_type="watertight"))

        ids = [row["id"] for row in listing["items"]]
        for item_id in ids:
            store.transition(item_id, "approve", reason="冒烟验证")
        print("审核通过后 recall 结果数:",
              len(store.recall(query="水密修复 自由边", scopes=["global"],
                               task_type="watertight", tool_names=tuple(real_tools))))
        hit = store.recall(query="自由边 局部缝合", scopes=["global"],
                           task_type="watertight", tool_names=tuple(real_tools))
        if hit:
            print("命中标题:", hit[0]["item"]["title"])
            print("分值组成:", hit[0]["parts"])
            print("confidence:", hit[0]["item"]["confidence"])

        show("6. 审计与计数")
        print("审计:", json.dumps(store.audit_tail(10), ensure_ascii=False, indent=1))
        print("计数:", store.counts())
        store.close()

    show("7. 真实 app 启动 + 路由（用真实数据目录与前缀）")
    import app as server
    from fastapi.testclient import TestClient
    with TestClient(server.app) as client:
        for path in ("/memory/summary", "/memory/experiences?status=candidate",
                     "/memory/tool-parameters", "/memory/status"):
            response = client.get(path)
            body = response.json()
            print(path, "->", response.status_code)
            print("   ", json.dumps(body, ensure_ascii=False)[:400])
    print("memory.db 现在存在:", paths.MEMORY_DB_PATH.exists())


asyncio.run(main())
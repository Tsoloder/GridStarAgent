import sys
import os
import json
import tempfile
import shutil
sys.path.insert(0, r'D:\Code\nnwgridstar_chat\GridStar\bin\mcp\agent')

from paths import SESSIONS_DIR

# 备份原始 SESSIONS_DIR
original_sessions_dir = SESSIONS_DIR

# 创建临时目录做测试
tmp_dir = tempfile.mkdtemp(prefix="session_test_")
os.environ.setdefault("SESSIONS_DIR_OVERRIDE", tmp_dir)

# 重新设置 SESSIONS_DIR
import paths
paths.SESSIONS_DIR = type(paths.SESSIONS_DIR)(tmp_dir)

# 重新 import
import importlib
import session as session_mod
importlib.reload(session_mod)
from session import Session, create_session, load_session, save_session, _append_jsonl, _read_jsonl, _write_jsonl

print("=== 测试 1: 创建 session + 追加消息 ===")
s = create_session("Test Session", model_id="test-model")
print("创建 session: %s" % s.id)

s.append_user("你好", active_skills=["cfd-meshing-workflow"])
s.append_assistant_with_tool_calls("正在处理", [
    {"id": "tc_1", "name": "MergeEdgesByDomain", "args": {"id": 78}}
])
s.append_tool_result("tc_1", '{"longids":[175,208],"shortids":[135,132]}', "MergeEdgesByDomain")
s.append_assistant("处理完成")

save_session(s)
print("消息数: %d" % len(s.messages))

# 验证 JSONL 文件存在
jsonl_path = os.path.join(tmp_dir, s.id, "messages.jsonl")
assert os.path.exists(jsonl_path), "FAIL: messages.jsonl not created!"

# 验证 JSONL 行数 = 消息数
with open(jsonl_path, "r", encoding="utf-8") as f:
    lines = [l.strip() for l in f if l.strip()]
assert len(lines) == 4, "FAIL: expected 4 lines, got %d" % len(lines)
print("JSONL 行数: %d (正确)" % len(lines))

# 验证每行是合法 JSON
for i, line in enumerate(lines):
    obj = json.loads(line)
    print("  行 %d: role=%s" % (i+1, obj.get("role")))

print("测试1 通过\n")

print("=== 测试 2: 重新加载 session ===")
s2 = load_session(s.id)
assert s2 is not None, "FAIL: load_session returned None"
assert len(s2.messages) == 4, "FAIL: expected 4 messages, got %d" % len(s2.messages)
assert s2.messages[0]["content"] == "你好", "FAIL: first message content mismatch"
assert s2.messages[1]["tool_calls"][0]["function"]["name"] == "MergeEdgesByDomain", "FAIL: tool call name mismatch"
assert s2.messages[2]["tool_name"] == "MergeEdgesByDomain", "FAIL: tool_name mismatch"
print("测试2 通过\n")

print("=== 测试 3: 旧版 messages.json 兼容迁移 ===")
sid_old = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
old_dir = os.path.join(tmp_dir, sid_old)
os.makedirs(old_dir, exist_ok=True)
os.makedirs(os.path.join(old_dir, "results"), exist_ok=True)

# 写旧版 messages.json
old_messages = [
    {"role": "user", "content": "旧消息1"},
    {"role": "assistant", "content": "旧回复1"},
]
with open(os.path.join(old_dir, "messages.json"), "w", encoding="utf-8") as f:
    json.dump(old_messages, f, ensure_ascii=False)

# 写 meta.json
meta = {"id": sid_old, "title": "Old Session", "created_at": "2026-01-01T00:00:00", "updated_at": "2026-01-01T00:00:00", "archived": False, "model_id": ""}
with open(os.path.join(old_dir, "meta.json"), "w", encoding="utf-8") as f:
    json.dump(meta, f, ensure_ascii=False)

# 加载旧 session
s_old = load_session(sid_old)
assert s_old is not None, "FAIL: old session load returned None"
assert len(s_old.messages) == 2, "FAIL: expected 2 old messages, got %d" % len(s_old.messages)

# 验证自动迁移为 JSONL
jsonl_old = os.path.join(old_dir, "messages.jsonl")
assert os.path.exists(jsonl_old), "FAIL: migration did not create messages.jsonl!"
print("旧 session 自动迁移为 JSONL: 通过")

# 再次加载，应该从 JSONL 读
s_old2 = load_session(sid_old)
assert len(s_old2.messages) == 2, "FAIL: reloaded old session has wrong message count"
print("测试3 通过\n")

print("=== 测试 4: 原地修改触发全量重写 ===")
s3 = create_session("Dirty Test")
s3.append_user("test")
s3.append_assistant_with_tool_calls("", [
    {"id": "tc_dirty", "name": "SomeTool", "args": {"key": "old_value"}}
])
save_session(s3)

jsonl_path3 = os.path.join(tmp_dir, s3.id, "messages.jsonl")
file_size_before = os.path.getsize(jsonl_path3)

# 原地修改
s3.update_tool_call_args("tc_dirty", {"key": "new_value"})
assert s3._dirty_full_write == True, "FAIL: _dirty_full_write not set!"
save_session(s3)
assert s3._dirty_full_write == False, "FAIL: _dirty_full_write not cleared after save!"

# 重新加载验证修改生效
s3_reloaded = load_session(s3.id)
found = False
for msg in s3_reloaded.messages:
    for tc in msg.get("tool_calls", []):
        if tc["id"] == "tc_dirty":
            args = json.loads(tc["function"]["arguments"])
            assert args["key"] == "new_value", "FAIL: update_tool_call_args not persisted!"
            found = True
assert found, "FAIL: tool call not found after reload"
print("测试4 通过\n")

print("=== 测试 5: clear_session 清空 ===")
clear_result = session_mod.clear_session(s3.id)
assert clear_result == True, "FAIL: clear_session returned False"
s3_cleared = load_session(s3.id)
assert len(s3_cleared.messages) == 0, "FAIL: messages not cleared"
print("测试5 通过\n")

print("=== 测试 6: JSONL 损坏行容错 ===")
sid_corrupt = "11111111-2222-3333-4444-555555555555"
corrupt_dir = os.path.join(tmp_dir, sid_corrupt)
os.makedirs(os.path.join(corrupt_dir, "results"), exist_ok=True)
jsonl_corrupt = os.path.join(corrupt_dir, "messages.jsonl")
with open(jsonl_corrupt, "w", encoding="utf-8") as f:
    f.write('{"role":"user","content":"good line"}\n')
    f.write('this is not json\n')  # 损坏行
    f.write('{"role":"assistant","content":"also good"}\n')
meta_corrupt = {"id": sid_corrupt, "title": "Corrupt", "created_at": "2026-01-01T00:00:00", "updated_at": "2026-01-01T00:00:00", "archived": False, "model_id": ""}
with open(os.path.join(corrupt_dir, "meta.json"), "w", encoding="utf-8") as f:
    json.dump(meta_corrupt, f)

s_corrupt = load_session(sid_corrupt)
assert s_corrupt is not None, "FAIL: corrupt session should still load"
assert len(s_corrupt.messages) == 2, "FAIL: expected 2 valid messages, got %d" % len(s_corrupt.messages)
print("测试6 通过\n")

print("=== 测试 7: content 落盘格式统一为空串 ===")
s7 = create_session("Content Format")
s7.append_user("跑一下")
s7.append_assistant_with_tool_calls("", [
    {"id": "tc_null", "name": "SomeTool", "args": {"k": "v"}}
])
assert s7.messages[1]["content"] == "", "FAIL: in-memory content should be empty string"
save_session(s7)

jsonl_path7 = os.path.join(tmp_dir, s7.id, "messages.jsonl")
with open(jsonl_path7, "r", encoding="utf-8") as f:
    persisted = f.read()
assert '"content": null' not in persisted and '"content":null' not in persisted, \
    "FAIL: null content still persisted"
assert load_session(s7.id).messages[1]["content"] == "", "FAIL: reloaded content mismatch"

# 历史数据里的 content: null 必须在读取时被归一
sid_legacy = "66666666-7777-8888-9999-aaaaaaaaaaaa"
legacy_dir = os.path.join(tmp_dir, sid_legacy)
os.makedirs(legacy_dir, exist_ok=True)
with open(os.path.join(legacy_dir, "messages.jsonl"), "w", encoding="utf-8") as f:
    f.write('{"role":"user","content":"旧输入"}\n')
    f.write('{"role":"assistant","content":null,"tool_calls":[{"id":"c1"}]}\n')
    f.write('{"role":"workflow","run_id":"r1","steps":[]}\n')
with open(os.path.join(legacy_dir, "meta.json"), "w", encoding="utf-8") as f:
    json.dump({"id": sid_legacy, "title": "Legacy", "created_at": "2026-01-01T00:00:00",
               "updated_at": "2026-01-01T00:00:00", "archived": False, "model_id": ""}, f)

legacy = load_session(sid_legacy)
assert legacy.messages[1]["content"] == "", "FAIL: legacy null content not normalized"
assert "content" not in legacy.messages[2], "FAIL: 无 content 字段的消息不应被凭空补一个"
print("测试7 通过\n")

# 清理
shutil.rmtree(tmp_dir)
print("全部验证通过!")

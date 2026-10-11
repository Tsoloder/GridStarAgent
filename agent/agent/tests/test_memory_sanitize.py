"""回归测试：长期记忆的脱敏层必须真的挡住工程数据。

动机来自现场：真实的 `ledger.json` 里 `args_digest` 原样留着
`{"serverHost": "10.76.19.98", "outputDir": "D:/A_GridStarCode/output"}`，
`tool_parameter_memory.json` 里也存着同样的主机 IP 与绝对路径。台账是
"事实记录"，长期记忆是"跨会话结论"——后者一旦落库就会在别的项目、别的机器上
被反复注入。所以 `sanitize_ledger` 必须先把它压成形状，`assert_clean` 再在
落盘前扫一遍。

另一个同样重要的方向是**不要过度脱敏**：本项目的工具名很长
（`GetGenerateSurMeshDefaultParam` 30 字符），一刀切会把正常术语判成泄漏，
让提炼层持续丢记录——那是另一种故障，只是更安静。
"""
import json

import pytest

import memory_sanitize as ms


# 贴近真实 `sessions/<id>/ledger.json` 的样本：失败调用、主机 IP、绝对路径、
# 工程文件名、对象 ID、坐标级小数一应俱全。
LEDGER = {
    "plan": {
        "id": "plan_01",
        "title": "机翼网格生成",
        "phases": [
            {"id": "ph1", "title": "CAD 导入", "status": "done",
             "note": "源文件 D:/A_GridStarCode/output/wing.igs"},
            {"id": "ph2", "title": "水密性处理", "status": "in_progress", "note": ""},
        ],
    },
    "calls": [
        {"ts": "2026-10-10T22:01:57", "phase": "CAD 导入", "tool": "ImportCADFile",
         "ok": True, "args_digest": {"filePath": "D:/A_GridStarCode/output/f6.igs", "unit": "mm"},
         "result_digest": '{"objectId": 12, "message": "ok"}', "file_ref": ""},
        {"ts": "2026-10-10T22:02:31", "phase": "水密性处理", "tool": "ProcessWithServer",
         "ok": False,
         "args_digest": {"serverHost": "10.76.19.98", "outputDir": "D:/A_GridStarCode/output",
                         "port": 8080, "faceIds": [12, 13, 14]},
         # 现场实证：台账把这次失败记成了 "success"，必须以 ok 为准
         "result_digest": "success", "file_ref": ""},
        {"ts": "2026-10-10T22:03:02", "phase": "水密性处理",
         "tool": "GetGenerateSurMeshDefaultParam", "ok": True,
         "args_digest": {"componentId": "solid_7"}, "result_digest": '{"size": 2.0}',
         "file_ref": ""},
    ],
}


def _json(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True)


# --------------------------------------------------------------------------- #
# 台账二次脱敏
# --------------------------------------------------------------------------- #

def test_sanitized_ledger_contains_no_raw_values():
    shaped = ms.sanitize_ledger(LEDGER)
    blob = _json(shaped)

    for leaked in ("10.76.19.98", "A_GridStarCode", "f6.igs", "wing.igs",
                   "D:/", "C:\\", "solid_7", "objectId"):
        assert leaked not in blob, "台账脱敏漏了 %r" % leaked


def test_sanitized_ledger_keeps_the_shape_that_matters():
    shaped = ms.sanitize_ledger(LEDGER)

    assert shaped["plan"]["title"] == "机翼网格生成"
    assert shaped["plan"]["phases"][0]["title"] == "CAD 导入"
    # note 只保留"有没有"，不留原文
    assert shaped["plan"]["phases"][0]["has_note"] is True
    assert shaped["plan"]["phases"][1]["has_note"] is False
    assert shaped["totals"] == {"phase_count": 2, "call_count": 3, "failed_count": 1}

    call = shaped["calls"][1]
    assert call["tool"] == "ProcessWithServer"
    assert call["ok"] is False
    assert call["phase"] == "水密性处理"
    assert call["arg_keys"] == ["faceIds", "outputDir", "port", "serverHost"]
    assert call["arg_shapes"]["serverHost"] == "host:redacted"
    assert call["arg_shapes"]["outputDir"] == "path:redacted"
    assert call["arg_shapes"]["faceIds"] == "list[3]:int:small"
    assert call["result_kind"] == "error"

    # 长工具名必须原样留下，否则提炼出的经验没有可执行的步骤
    assert shaped["calls"][2]["tool"] == "GetGenerateSurMeshDefaultParam"


def test_result_kind_trusts_ok_flag_not_result_digest():
    # 真实台账里 ok=False 但 result_digest="success"，只看文本会提炼出错误结论
    assert ms.result_kind("success", False) == "error"
    assert ms.result_kind('{"objectId": 12}', True) == "success_with_ids"
    assert ms.result_kind('{"size": 2.0}', True) == "success"
    assert ms.result_kind("", True) == "empty"
    assert ms.result_kind("", False) == "error"
    assert ms.result_kind('{"status": "error"}', True) == "error"
    assert ms.result_kind("x" * 9000, True) == "large"


def test_compact_calls_never_drops_failures():
    calls = [{"tool": "T%d" % i, "ok": i % 7 != 0} for i in range(60)]
    failed = [call for call in calls if not call["ok"]]

    kept = ms.compact_calls(calls, limit=10)

    assert len(kept) == 10
    assert all(call in kept for call in failed), "失败调用被截掉了，负样本丢失"
    # 保持原始时序，提炼模型才看得到"先做了什么、后来怎么失败"
    assert [call["tool"] for call in kept] == sorted(
        (call["tool"] for call in kept), key=lambda name: int(name[1:])
    )


def test_sanitize_ledger_tolerates_garbage():
    assert ms.sanitize_ledger({})["totals"]["call_count"] == 0
    assert ms.sanitize_ledger("not a ledger")["calls"] == []
    assert ms.sanitize_ledger({"calls": [None, 3, {"tool": ""}]})["calls"] == []


# --------------------------------------------------------------------------- #
# 正文扫描：该挡的挡住
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("text,pattern", [
    ("先打开 D:/A_GridStarCode/output/f6.igs", "windows_path"),
    ("源文件在 \\\\10.76.19.98\\share\\f6.igs", "unc_path"),
    ("读取 /opt/gridstar/data/f6.step", "posix_path"),
    ("联系 zhang@example.com 获取", "email"),
    ("任务 550e8400-e29b-41d4-a716-446655440000 失败", "uuid"),
    ("服务端在 10.76.19.98 上", "host"),
    ("api_key=sk-abcdefghijklmnopqrstuvwxyz", "credential"),
    ("导入 wing.igs 时超时", "filename"),
    ("第 face_12 个面质量不达标", "entity_id"),
    ("顶点坐标 1234.5678901 超出范围", "high_precision_number"),
    ("指纹 a3f9c2e1b7d40568a1b2c3d4e5f60718", "long_token"),
])
def test_find_sensitive_names_the_pattern(text, pattern):
    assert pattern in ms.find_sensitive(text)


def test_assert_clean_rejects_engineering_data():
    record = {
        "kind": "experience",
        "scope": "global",
        "task_type": "cad_import",
        "title": "导入 wing.igs 前先确认单位",
        "strategy_steps": ["确认单位是 mm"],
    }

    with pytest.raises(ms.SensitiveContentError) as info:
        ms.assert_clean(record)

    # 只暴露字段名与模式名，命中的原文绝不进日志
    assert info.value.patterns == ("filename",)
    assert "wing.igs" not in str(info.value)
    assert info.value.field.endswith("title")


def test_assert_clean_rejects_object_ids_in_steps():
    record = {
        "kind": "experience",
        "scope": "global",
        "task_type": "mesh_quality",
        "title": "质量检查超标时先重划局部面",
        "strategy_steps": ["对 face_12 单独加密"],
    }

    hits = ms.inspect_record(record)
    assert [pattern for _, pattern in hits] == ["entity_id"]


def test_assert_clean_rejects_invalid_tool_names():
    record = {
        "kind": "experience",
        "scope": "global",
        "task_type": "mesh_quality",
        "title": "质量检查超标时先重划局部面",
        "tool_sequence": ["ExamineMeshQuality", "rm -rf /"],
    }

    hits = dict((pattern, field) for field, pattern in ms.inspect_record(record))
    assert "invalid_tool_name" in hits
    assert hits["invalid_tool_name"] == "tool_sequence[1]"


# --------------------------------------------------------------------------- #
# 正文扫描：不该挡的别挡
# --------------------------------------------------------------------------- #

def test_assert_clean_accepts_a_normal_experience():
    record = {
        "kind": "experience",
        "scope": "skill:cfd-meshing-workflow",
        "task_type": "mesh_quality",
        "title": "质量检查超标时先做局部处理再全局重划",
        "trigger_cues": ["质量检查报错", "单元质量低于阈值"],
        "preconditions": ["已完成表面网格生成"],
        "strategy_steps": ["先用质量检查定位问题区域", "再对问题区域做局部重划"],
        "tool_sequence": ["GetGenerateSurMeshDefaultParam", "UGReDimensionBySize"],
        "failure_patterns": ["直接全局重划会把网格量放大数倍"],
        "source_kind": "validated_workflow",
    }

    assert ms.inspect_record(record) == []
    ms.assert_clean(record)


def test_long_tool_names_and_chinese_slashes_survive():
    # 30 位纯字母是真实工具名；"水密/分割" 里的斜杠不是路径；"2/3-gram" 同理
    assert ms.find_sensitive("GetGenerateSurMeshDefaultParam") == []
    assert ms.find_sensitive("ProcessAllTrailingEdges") == []
    assert ms.find_sensitive("先做水密/分割，再抽样 2/3 的单元") == []


def test_code_generated_fields_are_exempt():
    record = {
        "kind": "fact",
        "scope": "global",
        "status": "candidate",
        "source_kind": "auto_extraction",
        "source_version": {"skill_hash": "a3f9c2e1b7d40568a1b2c3d4e5f60718"
                                          "9e0d1c2b3a49586775869a0b1c2d3e4f"},
        "fingerprint": "b" * 64,
        "flags": ["possible_duplicate"],
        "fact_text": "用户偏好先导出中间结果再继续",
        "fact_type": "user_preference",
    }

    assert ms.inspect_record(record) == []


def test_iter_record_texts_skips_exempt_subtrees():
    record = {
        "id": "exp_0123456789abcdef",
        "title": "标题",
        "source_version": {"extractor": "x" * 40},
        "strategy_steps": ["步骤一"],
    }

    fields = [field for field, _ in ms.iter_record_texts(record)]
    assert fields == ["title", "strategy_steps[0]"]


# --------------------------------------------------------------------------- #
# 形状与键名
# --------------------------------------------------------------------------- #

def test_shape_of_never_echoes_the_value():
    cases = {
        "D:/A_GridStarCode/output": "path:redacted",
        "\\\\server\\share": "path:redacted",
        "10.76.19.98": "host:redacted",
        "sk-abcdefghijklmnopqrstuvwxyz": "credential:redacted",
        "550e8400-e29b-41d4-a716-446655440000": "uuid:redacted",
        "f6.igs": "filename:redacted",
        "solid_7": "id:redacted",
        "zhang@example.com": "email:redacted",
        "https://example.com/a": "url:redacted",
    }

    for value, expected in cases.items():
        shape = ms.shape_of(value)
        assert shape == expected, "%r → %r" % (value, shape)
        assert value not in shape

    assert ms.shape_of(0) == "int:zero"
    assert ms.shape_of(0.05) == "float:fraction"
    assert ms.shape_of(2.0) == "float:small"
    assert ms.shape_of(8080) == "int:medium"
    assert ms.shape_of(123456) == "int:large"
    assert ms.shape_of(None) == "null"
    assert ms.shape_of(True) == "boolean"
    assert ms.shape_of("mm") == "text:tiny"
    assert ms.shape_of([1, 2, 3]) == "list[3]:int:small"
    assert ms.shape_of({"a": 1, "b": 2}) == "object:keys=[a,b]"


def test_sanitize_args_drops_every_value():
    shaped = ms.sanitize_args({"serverHost": "10.76.19.98", "size": 2.0, "unit": "mm"})

    assert shaped["arg_keys"] == ["serverHost", "size", "unit"]
    assert shaped["arg_shapes"] == {
        "serverHost": "host:redacted", "size": "float:small", "unit": "text:tiny",
    }
    assert "10.76" not in _json(shaped)


def test_scrub_arg_key_keeps_identifiers_only():
    assert ms.scrub_arg_key("input_ids") == "input_ids"
    assert ms.scrub_arg_key("size") == "size"
    assert ms.scrub_arg_key("带空格 的键") == ms.REDACTED
    assert ms.scrub_arg_key("键") == ms.REDACTED
    assert len(ms.scrub_arg_key("头" + "x" * 100)) <= 64


def test_scrub_text_replaces_only_the_sensitive_part():
    assert ms.scrub_text("先导入 D:/a/b/f6.igs 再检查") == "先导入 %s 再检查" % ms.REDACTED
    assert ms.scrub_text("水密处理正常") == "水密处理正常"


def test_safe_tool_name_rejects_paths_and_spaces():
    assert ms.safe_tool_name("ProcessWithServer") == "ProcessWithServer"
    assert ms.safe_tool_name("../../etc/passwd") == ms.REDACTED
    assert ms.safe_tool_name("rm -rf /") == ms.REDACTED
    assert ms.safe_tool_name("") == ms.REDACTED
    assert ms.safe_tool_name(None) == ms.REDACTED
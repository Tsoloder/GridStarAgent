"""提炼器的回归测试。

重点守三件事，按重要性排序：

1. 泄漏：参数原值、路径、文件名、主机地址一个都不许进提示词，也不许落库；
2. 隔离：提炼只产出 candidate，任何时候都不能被 recall 召回；
3. 幂等与成本：同一触发不重复提炼，失败有次数上限，模型编造工具名整条丢弃。
"""
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

import experience_memory as em
import experience_extractor as ex


# ------------------------------------------------------------------ 夹具


class ScriptedRuntime:
    """按顺序吐出脚本化输出；脚本是 Exception 就抛出，模拟模型调用失败。"""

    def __init__(self, scripts, *, default_model="test/test-model"):
        self.scripts = list(scripts)
        self.default_model = default_model
        self.calls = []

    async def stream(self, model_key, messages, tools=(), system_prompt=""):
        self.calls.append({
            "model_key": model_key,
            "messages": list(messages),
            "system_prompt": system_prompt,
        })
        script = self.scripts.pop(0) if self.scripts else ""
        if isinstance(script, BaseException):
            raise script
        yield SimpleNamespace(type="text_delta", delta=script)


class BlockingRuntime:
    """永远卡在模型调用上的运行时，用来把 worker 钉住，好观察队列满的行为。"""

    def __init__(self, default_model="test/test-model"):
        self.default_model = default_model
        self.started = asyncio.Event()
        self.calls = 0

    async def stream(self, model_key, messages, tools=(), system_prompt=""):
        self.calls += 1
        self.started.set()
        await asyncio.Event().wait()
        yield  # pragma: no cover - 上面那行永不返回


def _settings():
    return {
        "plan": {
            "id": "plan_1",
            "title": "机翼表面网格生成",
            "phases": [
                {"id": "p1", "title": "CAD 导入与水密处理", "status": "done",
                 "note": "自由边已修复"},
                {"id": "p2", "title": "表面网格", "status": "done", "note": ""},
                {"id": "p3", "title": "质量检查", "status": "failed",
                 "note": "两个单元超标"},
            ],
        },
        "calls": [
            {
                "ts": "2026-09-28T10:00:00+08:00",
                "phase": "CAD 导入与水密处理",
                "tool": "ImportCADFile",
                "args_digest": {"path": "D:/A_GridStarCode/input/f6.igs"},
                "ok": True,
                "result_digest": "imported",
                "file_ref": "",
            },
            {
                "ts": "2026-09-28T10:01:00+08:00",
                "phase": "CAD 导入与水密处理",
                "tool": "DealWatertight",
                "args_digest": {"serverHost": "10.76.19.98",
                                "outputDir": "D:/A_GridStarCode/output",
                                "solid_ids": ["solid_7", "face_12"]},
                "ok": False,
                "result_digest": "success",
                "file_ref": "",
            },
            {
                "ts": "2026-09-28T10:02:00+08:00",
                "phase": "表面网格",
                "tool": "GenerateSurfaceMesh",
                "args_digest": {"size": 2.0, "component_ids": ["domain_3"]},
                "ok": True,
                "result_digest": '{"status": "ok", "message": "网格已生成"}',
                "file_ref": "",
            },
            {
                "ts": "2026-09-28T10:03:00+08:00",
                "phase": "质量检查",
                "tool": "ExamineMeshQuality",
                "args_digest": {"threshold": 0.05},
                "ok": True,
                "result_digest": '{"failed_cell_ids": ["cell_88", "cell_91"]}',
                "file_ref": "",
            },
        ],
    }


LEDGER = _settings()


def _good_output(**overrides):
    experience = {
        "title": "水密处理失败后先查自由边再做表面网格",
        "scope": "global",
        "task_type": "watertight_repair",
        "trigger_cues": ["CAD 导入后水密工具报错", "表面网格前需要确认封闭性"],
        "preconditions": ["导入的 CAD 存在开放边界"],
        "strategy_steps": ["先跑水密检查拿到自由边计数", "修复后再生成表面网格"],
        "tool_sequence": ["ImportCADFile", "DealWatertight", "GenerateSurfaceMesh"],
        "failure_patterns": ["跳过水密检查直接生成表面网格会导致体网格失败"],
        "evidence": 3,
    }
    fact = {
        "fact_text": "用户偏好先做小尺寸试算再放大到全尺寸网格",
        "fact_type": "user_preference",
        "tags": ["网格尺寸", "流程偏好"],
    }
    payload = {
        "experiences": [experience],
        "facts": [fact],
    }
    payload.update(overrides)
    return json.dumps(payload, ensure_ascii=False)


@pytest.fixture()
def store(tmp_path):
    instance = em.MemoryStore(tmp_path / "memory.db")
    yield instance
    instance.close()


@pytest.fixture()
def state_root(tmp_path):
    def root(session_id):
        path = tmp_path / "sessions" / session_id
        path.mkdir(parents=True, exist_ok=True)
        return path

    return root


def _extractor(store, scripts, state_root, **kwargs):
    runtime = ScriptedRuntime(scripts)
    extractor = ex.ExperienceExtractor(
        store, runtime, "test/test-model", state_root=state_root, **kwargs
    )
    return extractor, runtime


def _run(extractor, **kwargs):
    return asyncio.run(
        extractor.extract_session("sess-0001", LEDGER, **kwargs)
    )


# ------------------------------------------------------------------ 提示词脱敏


def test_prompt_carries_no_raw_parameters(store, state_root):
    extractor, runtime = _extractor(store, [_good_output()], state_root)

    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["status"] == "completed"
    sent = runtime.calls[0]
    blob = sent["messages"][0]["content"] + sent["system_prompt"]
    for leaked in ("10.76.19.98", "A_GridStarCode", "f6.igs", "solid_7",
                   "face_12", "domain_3", "cell_88", "cell_91"):
        assert leaked not in blob, leaked
    # 工具名和参数键名要留着，否则模型没法判断流程
    assert "DealWatertight" in blob
    assert "serverHost" in blob


def test_focus_is_scrubbed_before_it_reaches_the_model(store, state_root):
    extractor, runtime = _extractor(store, [_good_output()], state_root)

    _run(
        extractor,
        trigger=ex.TRIGGER_USER_CORRECTION,
        focus=r"以后别把结果写到 D:\A_GridStarCode\output\wing_f6.igs 里",
    )

    blob = runtime.calls[0]["messages"][0]["content"]
    assert "A_GridStarCode" not in blob
    assert "wing_f6.igs" not in blob
    assert "user_correction" in blob


def test_prompt_states_task_type_and_switch_state(store, state_root):
    extractor, runtime = _extractor(
        store, [_good_output(facts=[])], state_root, extract_fact=False
    )

    _run(extractor, trigger=ex.TRIGGER_MANUAL, skills=("cfd-meshing-workflow",))

    body = runtime.calls[0]["messages"][0]["content"]
    assert "skill:<技能名>" in body
    assert "cfd-meshing-workflow" in body
    assert "facts 返回空数组" in body


def test_no_loaded_skills_forces_global_scope(store, state_root):
    extractor, runtime = _extractor(store, [_good_output()], state_root)

    _run(extractor, trigger=ex.TRIGGER_MANUAL)

    assert 'scope 一律写 "global"' in runtime.calls[0]["messages"][0]["content"]


# ------------------------------------------------------------------ 只产出候选


def test_extraction_only_produces_candidates(store, state_root):
    extractor, _ = _extractor(store, [_good_output()], state_root)

    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["added"] == {"experience": 1, "fact": 1}
    items = store.list_items()["items"]
    assert {item["kind"] for item in items} == {"experience", "fact"}
    assert all(item["status"] == "candidate" for item in items)
    # 这是整套设计的地基：没审核过的候选一条都不许被召回
    assert store.recall(query="水密 自由边 表面网格", scopes=["global"]) == []


def test_approved_experience_becomes_recallable(store, state_root):
    extractor, _ = _extractor(store, [_good_output(facts=[])], state_root)
    _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)
    item_id = store.list_items()["items"][0]["id"]
    store.transition(item_id, "approve")

    hits = store.recall(
        query="水密处理 自由边 表面网格",
        scopes=["global"],
        tool_names=["DealWatertight"],
    )

    assert [hit["item"]["id"] for hit in hits] == [item_id]


def test_fact_disabled_keeps_facts_out_of_the_store(store, state_root):
    extractor, _ = _extractor(store, [_good_output()], state_root,
                              extract_fact=False)

    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["added"] == {"experience": 1, "fact": 0}
    kinds = {item["kind"] for item in store.list_items()["items"]}
    assert kinds == {"experience"}


# ------------------------------------------------------------------ 编造与泄漏


def test_unknown_tool_name_drops_the_whole_candidate(store, state_root):
    output = _good_output(experiences=[{
        "title": "先跑自适应加密",
        "scope": "global",
        "task_type": "mesh_quality",
        "strategy_steps": ["调用自适应加密"],
        "tool_sequence": ["RunAdaptiveRefinement"],
        "evidence": 1,
    }], facts=[])

    extractor, _ = _extractor(store, [output], state_root)
    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["added"]["experience"] == 0
    assert result["dropped"]["unknown_tool"] == 1
    assert store.list_items()["total"] == 0


def test_registered_tool_not_in_ledger_is_allowed(store, state_root):
    output = _good_output(experiences=[{
        "title": "生成体网格前先做表面质量复核",
        "scope": "global",
        "task_type": "mesh_quality",
        "strategy_steps": ["先复核表面质量", "再生成体网格"],
        "tool_sequence": ["GenerateVolumeMesh"],
        "evidence": 2,
    }], facts=[])

    extractor, _ = _extractor(store, [output], state_root,
                              known_tools=["GenerateVolumeMesh"])
    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["added"]["experience"] == 1


def test_illegal_scope_drops_the_candidate(store, state_root):
    output = _good_output(experiences=[{
        "title": "机翼项目专用的加密策略",
        "scope": "project:wing",
        "task_type": "mesh_quality",
        "strategy_steps": ["先加密后缘"],
        "tool_sequence": ["GenerateSurfaceMesh"],
        "evidence": 1,
    }], facts=[])

    extractor, _ = _extractor(store, [output], state_root)
    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["dropped"]["invalid_scope"] == 1
    assert store.list_items()["total"] == 0


def test_leaky_candidate_is_rejected_at_the_store_door(store, state_root):
    output = _good_output(experiences=[{
        "title": "把结果写到固定目录再复检",
        "scope": "global",
        "task_type": "mesh_quality",
        "strategy_steps": [r"生成后写到 D:\A_GridStarCode\output 再读回来"],
        "tool_sequence": ["GenerateSurfaceMesh"],
        "evidence": 1,
    }], facts=[])

    extractor, _ = _extractor(store, [output], state_root)
    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["added"]["experience"] == 0
    assert any(key.startswith("sanitized") for key in result["dropped"])
    assert store.list_items()["total"] == 0
    assert store.counter(em.COUNTER_SANITIZER) == 0  # 没走到存储层就被拦了


def test_fact_with_unknown_type_is_dropped(store, state_root):
    output = _good_output(experiences=[], facts=[{
        "fact_text": "用户名字叫张三",
        "fact_type": "user_identity",
    }])

    extractor, _ = _extractor(store, [output], state_root)
    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["dropped"]["invalid_fact_type"] == 1
    assert store.list_items()["total"] == 0


def test_evidence_and_task_type_are_coerced(store, state_root):
    output = _good_output(experiences=[{
        "title": "质量检查超标时先做局部加密",
        "scope": "global",
        "task_type": "Mesh Quality",
        "strategy_steps": ["定位超标单元", "只加密超标区域"],
        "tool_sequence": ["ExamineMeshQuality"],
        "evidence": 99,
    }], facts=[])

    extractor, _ = _extractor(store, [output], state_root)
    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["added"]["experience"] == 1
    item = store.list_items()["items"][0]
    assert item["task_type"] == ""          # 非法任务类型不猜，留空
    full = store.get(item["id"])
    assert full["evidence_count"] == 10     # 上限 10


def test_candidate_cap_per_run(store, state_root):
    many = [{
        "title": "第 %d 条互不相同的结论" % index,
        "scope": "global",
        "task_type": "mesh_quality",
        "strategy_steps": ["步骤 A%d" % index, "步骤 B%d" % index],
        "evidence": 1,
    } for index in range(5)]
    output = _good_output(experiences=many, facts=[])

    extractor, _ = _extractor(store, [output], state_root)
    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["added"]["experience"] == ex.MAX_EXPERIENCES_PER_RUN
    assert result["dropped"]["overflow"] == 3


# ------------------------------------------------------------------ 幂等与重试


def test_same_trigger_does_not_extract_twice(store, state_root):
    extractor, runtime = _extractor(
        store, [_good_output(), _good_output()], state_root
    )

    first = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)
    second = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert first["status"] == "completed"
    assert second["status"] == "skipped"
    assert second["reason"] == "already_done"
    assert len(runtime.calls) == 1
    assert store.list_items()["total"] == 2


def test_plan_change_opens_a_new_job(store, state_root):
    extractor, runtime = _extractor(
        store, [_good_output(), _good_output()], state_root
    )
    _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    changed = json.loads(json.dumps(LEDGER))
    changed["plan"]["phases"][2]["status"] = "done"
    result = asyncio.run(
        extractor.extract_session("sess-0001", changed,
                                  trigger=ex.TRIGGER_PLAN_TERMINAL)
    )

    assert result["status"] == "completed"
    assert len(runtime.calls) == 2


def test_empty_ledger_is_marked_done_not_retried(store, state_root):
    extractor, runtime = _extractor(store, [_good_output()], state_root)
    blank = {"plan": {"id": "p", "phases": [{"id": "a", "status": "done"}]},
             "calls": []}

    first = asyncio.run(
        extractor.extract_session("sess-0002", blank, trigger=ex.TRIGGER_MANUAL)
    )
    second = asyncio.run(
        extractor.extract_session("sess-0002", blank, trigger=ex.TRIGGER_MANUAL)
    )

    assert first["status"] == "empty"
    assert second["reason"] == "already_done"
    assert runtime.calls == []


def test_failure_is_retried_then_gives_up(store, state_root):
    boom = RuntimeError("model exploded")
    extractor, runtime = _extractor(
        store, [boom, boom, boom, _good_output()], state_root
    )

    results = [_run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)
               for _ in range(3)]
    after = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert [item["status"] for item in results] == ["failed"] * 3
    assert [item["attempts"] for item in results] == [1, 2, 3]
    assert results[-1]["error"] == "RuntimeError"
    assert after["reason"] == "already_failed"
    assert len(runtime.calls) == 3
    assert store.list_items()["total"] == 0
    # 断点里记的是终态，重启进程也不会再试第四次
    state = ex.load_state("sess-0001", root=state_root)
    assert [job["step"] for job in state["jobs"].values()] == ["failed_terminal"]


def test_retryable_failure_then_success(store, state_root):
    extractor, runtime = _extractor(
        store, [RuntimeError("flaky"), _good_output()], state_root
    )

    first = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)
    second = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert first["status"] == "failed"
    assert second["status"] == "completed"
    assert second["attempts"] == 2
    assert second["added"]["experience"] == 1


def test_unparsable_output_counts_as_failure(store, state_root):
    extractor, _ = _extractor(
        store, ["抱歉，我无法从这段记录中提炼出可靠的经验。", "还是不行。"],
        state_root,
    )

    first = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert first["status"] == "failed"
    assert first["reason"] == "no_json"
    assert store.list_items()["total"] == 0


def test_force_reruns_a_done_job(store, state_root):
    extractor, runtime = _extractor(
        store, [_good_output(), _good_output()], state_root
    )
    _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    forced = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL, force=True)

    assert forced["status"] == "completed"
    assert len(runtime.calls) == 2
    # 指纹相同，第二次是合并证据而不是新增记录
    assert forced["added"]["experience"] == 0
    assert store.list_items()["total"] == 2


def test_state_file_survives_a_fresh_extractor(store, state_root):
    extractor, _ = _extractor(store, [_good_output()], state_root)
    _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    reopened, runtime = _extractor(store, [_good_output()], state_root)
    again = _run(reopened, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert again["reason"] == "already_done"
    assert runtime.calls == []
    state_file = Path(state_root("sess-0001")) / ex.STATE_FILENAME
    assert state_file.exists()
    raw = state_file.read_text(encoding="utf-8")
    assert "sess-0001" not in raw       # 断点里不留会话 id
    assert "watertight" not in raw      # 也不留经验正文


def test_corrupt_state_file_degrades_to_empty(tmp_path):
    root = tmp_path / "sessions"
    (root / "sess-9").mkdir(parents=True)
    (root / "sess-9" / ex.STATE_FILENAME).write_text("{ not json",
                                                     encoding="utf-8")

    state = ex.load_state("sess-9", root=lambda sid: root / sid)

    assert state["jobs"] == {}


def test_state_write_never_resurrects_a_deleted_session(tmp_path):
    """会话目录没了就别写断点：写下去会把删掉的会话造回一个空壳。

    提炼本身照常完成，只是这次没有断点可用——代价是下次可能重复提炼一次，
    比在数据目录里留下一个删不掉的幽灵目录划算。
    """
    root = tmp_path / "sessions"

    def state_root(session_id):
        return root / session_id

    ex.save_state("sess-gone", {"jobs": {"job": {"step": "done"}}}, root=state_root)

    assert not (root / "sess-gone").exists()
    assert list(root.iterdir() if root.is_dir() else []) == []


def test_state_write_lands_when_the_session_dir_exists(tmp_path):
    root = tmp_path / "sessions"
    (root / "sess-live").mkdir(parents=True)

    ex.save_state("sess-live", {"jobs": {"job": {"step": "done"}}},
                  root=lambda sid: root / sid)

    assert (root / "sess-live" / ex.STATE_FILENAME).exists()
    assert ex.load_state("sess-live", root=lambda sid: root / sid)["jobs"]["job"]["step"] == "done"


# ------------------------------------------------------------------ 开关与门禁


def test_both_switches_off_skips_everything(store, state_root):
    extractor, runtime = _extractor(
        store, [_good_output()], state_root,
        extract_experience=False, extract_fact=False,
    )

    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["reason"] == "disabled"
    assert runtime.calls == []


def test_missing_runtime_skips(store, state_root):
    extractor = ex.ExperienceExtractor(store, None, "", state_root=state_root)

    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["reason"] == "no_runtime"


def test_unavailable_store_skips(tmp_path, state_root):
    broken = em.MemoryStore(tmp_path / "memory.db", enabled=False)
    extractor, runtime = _extractor(broken, [_good_output()], state_root)

    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["status"] == "skipped"
    assert result["reason"] == "store_unavailable"
    assert runtime.calls == []


def test_oversized_model_output_is_rejected(store, state_root):
    extractor, _ = _extractor(store, ["x" * 50], state_root,
                              result_char_limit=10)

    result = _run(extractor, trigger=ex.TRIGGER_PLAN_TERMINAL)

    assert result["status"] == "failed"
    assert result["error"] == "RuntimeError"


# ------------------------------------------------------------------ 触发判断


def test_detect_trigger_prefers_user_correction():
    assert ex.detect_trigger(LEDGER, focus="以后先做水密检查") == \
        ex.TRIGGER_USER_CORRECTION


def test_detect_trigger_plan_terminal():
    assert ex.detect_trigger(LEDGER) == ex.TRIGGER_PLAN_TERMINAL


def test_detect_trigger_watermark_only_after_enough_turns():
    open_ledger = {"plan": {"id": "p", "phases": [
        {"id": "a", "status": "in_progress"}]}, "calls": []}

    assert ex.detect_trigger(open_ledger, turn_count=9) is None
    assert ex.detect_trigger(open_ledger, turn_count=10) == \
        ex.TRIGGER_TURN_WATERMARK
    assert ex.detect_trigger(open_ledger, turn_count=25, last_watermark=20) is None
    assert ex.detect_trigger(open_ledger, turn_count=30, last_watermark=20) == \
        ex.TRIGGER_TURN_WATERMARK


def test_plan_without_phases_is_not_terminal():
    assert not ex.is_plan_terminal({"plan": {"id": "p", "phases": []}})
    assert not ex.is_plan_terminal({})


def test_plan_signature_ignores_notes():
    first = ex.plan_signature(LEDGER)
    tweaked = json.loads(json.dumps(LEDGER))
    tweaked["plan"]["phases"][0]["note"] = "换了个说法"

    assert ex.plan_signature(tweaked) == first


def test_job_key_separates_triggers_and_focus():
    base = ex.job_key("s1", ex.TRIGGER_PLAN_TERMINAL, plan_sig="abc")
    other_trigger = ex.job_key("s1", ex.TRIGGER_MANUAL, plan_sig="abc")
    focus_a = ex.job_key("s1", ex.TRIGGER_USER_CORRECTION, focus="先做水密")
    focus_b = ex.job_key("s1", ex.TRIGGER_USER_CORRECTION, focus="先做加密")

    assert len({base, other_trigger, focus_a, focus_b}) == 4
    assert "s1" not in base


# ------------------------------------------------------------------ JSON 解析


def test_find_json_object_tolerates_fences_and_prose():
    assert ex.find_json_object('```json\n{"a": 1}\n```') == '{"a": 1}'
    assert ex.find_json_object('好的：\n{"a": 1}\n以上。') == '{"a": 1}'
    assert ex.find_json_object('{"a": "}"}') == '{"a": "}"}'
    assert ex.find_json_object("没有 JSON") is None


def test_parse_output_reports_reason_for_garbage():
    parsed = ex.parse_extraction_output("完全不是 JSON")

    assert parsed["experiences"] == []
    assert parsed["reason"] == "no_json"


def test_parse_output_accepts_a_single_string_step():
    parsed = ex.parse_extraction_output(json.dumps({
        "experiences": [{
            "title": "先查自由边",
            "strategy_steps": "只做水密检查",
            "evidence": "2",
        }],
    }, ensure_ascii=False))

    assert parsed["reason"] == ""
    assert parsed["experiences"][0]["strategy_steps"] == ["只做水密检查"]
    assert parsed["experiences"][0]["evidence_count"] == 2


# ------------------------------------------------------------------ 投递队列


def test_queue_processes_a_submitted_job(store, state_root):
    async def scenario():
        extractor, runtime = _extractor(store, [_good_output()], state_root)
        queue = ex.ExtractionQueue(extractor)
        queue.start()
        accepted = queue.submit("sess-q1", LEDGER,
                                trigger=ex.TRIGGER_PLAN_TERMINAL)
        await asyncio.wait_for(queue._queue.join(), timeout=5)
        stats = queue.stats
        await queue.stop()
        return accepted, stats, runtime

    accepted, stats, runtime = asyncio.run(scenario())

    assert accepted is True
    assert stats["queued"] == 1
    assert stats["completed"] == 1
    assert stats["pending"] == 0
    assert len(runtime.calls) == 1
    assert store.list_items()["total"] == 2


def test_queue_drops_when_full_without_blocking(store, state_root):
    async def scenario():
        runtime = BlockingRuntime()
        extractor = ex.ExperienceExtractor(store, runtime, "test/test-model",
                                           state_root=state_root)
        queue = ex.ExtractionQueue(extractor, maxsize=1)
        queue.start()
        first = queue.submit("sess-q2", LEDGER, trigger=ex.TRIGGER_MANUAL)
        # 等 worker 把第一条取走并卡在模型调用上，队列才有确定的容量边界
        await asyncio.wait_for(runtime.started.wait(), timeout=5)
        second = queue.submit("sess-q2", LEDGER, trigger=ex.TRIGGER_MANUAL)
        third = queue.submit("sess-q2", LEDGER, trigger=ex.TRIGGER_MANUAL)
        stats = queue.stats
        await queue.stop()
        return first, second, third, stats

    first, second, third, stats = asyncio.run(scenario())

    assert first is True
    assert second is True      # 刚好填满
    assert third is False      # 满了就丢，绝不阻塞调用方
    assert stats["queued"] == 2
    assert stats["dropped"] == 1


def test_queue_survives_a_failing_job(store, state_root):
    async def scenario():
        extractor, _ = _extractor(
            store, [RuntimeError("boom"), _good_output()], state_root
        )
        queue = ex.ExtractionQueue(extractor)
        queue.start()
        queue.submit("sess-q3", LEDGER, trigger=ex.TRIGGER_MANUAL)
        await asyncio.wait_for(queue._queue.join(), timeout=5)
        queue.submit("sess-q4", LEDGER, trigger=ex.TRIGGER_MANUAL)
        await asyncio.wait_for(queue._queue.join(), timeout=5)
        stats = queue.stats
        await queue.stop()
        return stats

    stats = asyncio.run(scenario())

    assert stats["failed"] == 1
    assert stats["completed"] == 1
    assert stats["last_error"] == "RuntimeError"


def test_submit_without_start_is_refused(store, state_root):
    extractor, _ = _extractor(store, [_good_output()], state_root)
    queue = ex.ExtractionQueue(extractor)

    assert queue.submit("sess-q5", LEDGER, trigger=ex.TRIGGER_MANUAL) is False
    assert queue.stats["queued"] == 0
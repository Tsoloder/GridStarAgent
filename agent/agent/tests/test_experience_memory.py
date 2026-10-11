"""回归测试：长期记忆库的状态机、隔离与检索契约。

这里的每一条断言都对应一处"错误记忆比没有记忆更危险"的具体失效模式：
候选未经审核就被注入、已拒绝的记录被重新提案、曝光次数冒充置信度、
Skill 改版后旧经验照旧生效。存储层必须自己守住这些，不能指望调用方自觉。
"""
import sqlite3
from datetime import datetime, timedelta, timezone

import pytest

import experience_memory as em
from experience_memory import MemoryStore


@pytest.fixture()
def store(tmp_path):
    instance = MemoryStore(tmp_path / "memory.db")
    yield instance
    instance.close()


def experience(**overrides):
    record = {
        "kind": "experience",
        "scope": "global",
        "task_type": "mesh_quality",
        "title": "质量检查超标时先做局部处理再考虑全局重划",
        "trigger_cues": ["质量检查报错", "单元质量低于阈值"],
        "preconditions": ["表面网格已经生成完毕"],
        "strategy_steps": ["先用质量检查定位问题区域", "只对问题区域做局部加密"],
        "tool_sequence": ["ExamineMeshQuality", "UGReDimensionBySize"],
        "failure_patterns": ["直接全局重划会把网格量放大数倍"],
        "source_kind": "validated_workflow",
    }
    record.update(overrides)
    return record


def fact(**overrides):
    record = {
        "kind": "fact",
        "scope": "global",
        "task_type": "environment",
        "fact_text": "导出目录习惯放在工程根目录下的 exports",
        "fact_type": "user_preference",
        "source_kind": "user_correction",
    }
    record.update(overrides)
    return record


def _add(store, record):
    result = store.add_item(record)
    assert result["created"] is True, result
    return result["id"]


# 造一条与其他记录几乎不共享措辞的经验，用于体积/限流类测试：共用同一份
# trigger_cues 会让所有记录被判成近似重复，那是别的测试要覆盖的行为。
_TOPICS = [
    "水密修复", "部件切分", "后缘加密", "各向异性",
    "边界条件", "质量复核", "网格导出", "坐标变换",
]


def distinct_experience(index, **overrides):
    topic = _TOPICS[index % len(_TOPICS)]
    record = experience(
        title="关于%s的处理顺序" % topic,
        trigger_cues=["观察到%s异常" % topic],
        preconditions=["已经完成%s" % topic],
        strategy_steps=["执行%s的第一步" % topic, "执行%s的第二步" % topic],
        tool_sequence=["ToolNumber%d" % index],
        failure_patterns=["%s失败时的表现" % topic],
    )
    record.update(overrides)
    return record


def _backdate(store, item_id, days):
    stamp = (datetime.now(timezone.utc) - timedelta(days=days)).isoformat(
        timespec="microseconds"
    )
    with store._lock:
        conn = store._connect()
        conn.execute(
            "UPDATE memory_item SET created_at = ?, updated_at = ? WHERE id = ?",
            (stamp, stamp, item_id),
        )
        conn.commit()
    return stamp


# --------------------------------------------------------------------------- #
# 第一契约：候选绝不可注入
# --------------------------------------------------------------------------- #

def test_candidate_is_written_but_never_recalled(store):
    item_id = _add(store, experience())

    stored = store.get(item_id)
    assert stored["status"] == "candidate"
    assert stored["id"].startswith("exp_")

    # 这正是 P0a 的全部意义：影子采集期只入库，不参与任何注入
    assert store.recall(
        query="质量检查", scopes=["global"], task_type="mesh_quality"
    ) == []


def test_recall_opens_only_after_approval(store):
    item_id = _add(store, experience())

    store.transition(item_id, "approve", reason="已复现")

    hits = store.recall(query="质量检查", scopes=["global"], task_type="mesh_quality")
    assert [hit["item"]["id"] for hit in hits] == [item_id]
    assert set(hits[0]["parts"]) == {"lexical", "metadata", "confidence", "recency"}


def test_facts_stay_out_of_experience_recall(store):
    fact_id = _add(store, fact())
    store.transition(fact_id, "approve")

    assert store.recall(
        query="导出目录", scopes=["global"], task_type="environment"
    ) == []


def test_recall_requires_metadata_overlap(store):
    item_id = _add(store, experience())
    store.transition(item_id, "approve")

    # 措辞命中但任务类型与工具都对不上：宁可漏召回，也不注入似是而非的经验
    assert store.recall(
        query="质量检查", scopes=["global"],
        task_type="cad_import", tool_names=["ImportCADFile"],
    ) == []

    hits = store.recall(
        query="质量检查", scopes=["global"], tool_names=["ExamineMeshQuality"]
    )
    assert len(hits) == 1


def test_recall_respects_scope(store):
    item_id = _add(store, experience(scope="skill:cfd-meshing-workflow"))
    store.transition(item_id, "approve")

    assert store.recall(
        query="质量检查", scopes=["global"], task_type="mesh_quality"
    ) == []
    assert store.recall(
        query="质量检查", scopes=["skill:cfd-meshing-workflow"],
        task_type="mesh_quality",
    )


def test_recall_skips_excluded_ids(store):
    item_id = _add(store, experience())
    store.transition(item_id, "approve")

    assert store.recall(
        query="质量检查", scopes=["global"], task_type="mesh_quality",
        exclude_ids=[item_id],
    ) == []


def test_stale_skill_version_blocks_injection(store):
    item_id = _add(store, experience(
        scope="skill:cfd-meshing-workflow",
        source_version={"skill_hash": "aaaa1111"},
    ))
    store.transition(item_id, "approve")

    changed = {"cfd-meshing-workflow": "bbbb2222"}
    assert store.recall(
        query="质量检查", scopes=["skill:cfd-meshing-workflow"],
        task_type="mesh_quality", skill_hashes=changed,
    ) == []
    assert store.recall(
        query="质量检查", scopes=["skill:cfd-meshing-workflow"],
        task_type="mesh_quality",
        skill_hashes={"cfd-meshing-workflow": "aaaa1111"},
    )


# --------------------------------------------------------------------------- #
# 状态机
# --------------------------------------------------------------------------- #

def test_illegal_transition_raises_state_conflict(store):
    item_id = _add(store, experience())
    store.transition(item_id, "reject")

    # 已拒绝的记录不能直接通过，必须先 reopen —— 否则"拒绝"形同虚设
    with pytest.raises(em.StateConflict):
        store.transition(item_id, "approve")
    with pytest.raises(em.StateConflict):
        store.transition(item_id, "restore")
    with pytest.raises(em.StateConflict):
        store.transition(item_id, "deprecate")


def test_full_lifecycle_transitions(store):
    item_id = _add(store, experience())

    store.transition(item_id, "approve")
    assert store.get(item_id)["status"] == "approved"
    store.transition(item_id, "deprecate", reason="Skill 改版")
    assert store.get(item_id)["status"] == "deprecated"
    store.transition(item_id, "restore")
    assert store.get(item_id)["status"] == "approved"

    other = _add(store, experience(title="另一条完全不同的经验标题"))
    store.transition(other, "reject")
    store.transition(other, "reopen")
    assert store.get(other)["status"] == "candidate"


def test_transition_rejects_unknown_action(store):
    item_id = _add(store, experience())

    with pytest.raises(ValueError):
        store.transition(item_id, "delete_everything")


def test_stale_revision_is_rejected(store):
    item_id = _add(store, experience())
    stale = store.get(item_id)["updated_at"]

    store.transition(item_id, "approve")

    with pytest.raises(em.StateConflict):
        store.transition(item_id, "deprecate", expected_updated_at=stale)


def test_unknown_id_raises_not_found(store):
    with pytest.raises(em.RecordNotFound):
        store.get("exp_does_not_exist")
    with pytest.raises(em.RecordNotFound):
        store.transition("exp_does_not_exist", "approve")


# --------------------------------------------------------------------------- #
# 去重与抑制
# --------------------------------------------------------------------------- #

def test_duplicate_candidate_accumulates_evidence(store):
    first = store.add_item(experience(evidence_count=2))
    second = store.add_item(experience(evidence_count=3))

    assert first["created"] is True
    assert second == {"id": first["id"], "created": False, "reason": "duplicate"}
    assert store.get(first["id"])["evidence_count"] == 5


def test_rejected_record_is_never_resurrected(store):
    item_id = _add(store, experience())
    store.transition(item_id, "reject", reason="证据不足")

    noise = store.add_item(experience(evidence_count=9))

    assert noise["reason"] == "duplicate"
    assert store.get(item_id)["status"] == "rejected"
    assert store.get(item_id)["evidence_count"] == 1
    assert store.counter(em.COUNTER_DUPLICATE) == 1


def test_deprecated_record_is_never_resurrected(store):
    item_id = _add(store, experience())
    store.transition(item_id, "approve")
    store.transition(item_id, "deprecate")

    assert store.add_item(experience())["reason"] == "duplicate"
    assert store.get(item_id)["status"] == "deprecated"


# --------------------------------------------------------------------------- #
# 脱敏与校验
# --------------------------------------------------------------------------- #

def test_sanitizer_drops_the_whole_record(store):
    result = store.add_item(experience(
        title="导入 wing.igs 之前先确认单位",
        strategy_steps=["确认源文件单位是 mm"],
    ))

    assert result["created"] is False
    assert result["reason"] == "sanitized"
    # detail 只带字段名，绝不回传命中的原文
    assert "wing.igs" not in result["detail"]
    assert store.counter(em.COUNTER_SANITIZER) == 1
    assert store.counts()["total"] == 0


def test_invalid_record_is_rejected_without_raising(store):
    assert store.add_item({"kind": "nonsense", "title": "x"})["reason"] == "invalid"
    assert store.add_item({"kind": "experience", "scope": "global"})["reason"] == "invalid"
    assert store.add_item(experience(source_kind="made_up"))["reason"] == "invalid"
    assert store.add_item({"kind": "fact", "fact_text": ""})["reason"] == "invalid"
    assert store.counter(em.COUNTER_INVALID) == 4


@pytest.mark.parametrize("bad_scope", [
    "D:/A_GridStarCode/output",
    "/opt/gridstar",
    "project:myproject",
    "skill:",
    "skill:has space",
])
def test_scope_must_not_be_a_path(store, bad_scope):
    assert store.add_item(experience(scope=bad_scope))["reason"] == "invalid"


def test_missing_scope_defaults_to_global(store):
    record = experience()
    record.pop("scope")

    result = store.add_item(record)

    assert result["created"] is True
    assert store.get(result["id"])["scope"] == "global"


def test_auto_extraction_cannot_self_approve(store):
    # 提炼模型就算把 status 写成 approved，也只能落成候选
    result = store.add_item(experience(status="approved", source_kind="auto_extraction"))

    assert store.get(result["id"])["status"] == "candidate"


def test_manual_curation_may_land_approved(store):
    result = store.add_item(experience(source_kind="manual_curated", status="approved"))

    assert store.get(result["id"])["status"] == "approved"
    assert store.get(result["id"])["confidence"] == em.SOURCE_PRIORS["manual_curated"]


# --------------------------------------------------------------------------- #
# 中文检索
# --------------------------------------------------------------------------- #

def test_chinese_query_finds_the_right_record(store):
    target = _add(store, experience(title="质量检查超标时先做局部处理"))
    other = _add(store, experience(
        title="导入 CAD 前先确认单位",
        task_type="cad_import",
        trigger_cues=["单位不一致"],
        strategy_steps=["确认源文件单位"],
        tool_sequence=["ImportCADFile"],
        failure_patterns=["单位错会导致整体尺寸偏差"],
    ))

    page = store.list_items(q="质量检查")
    assert [item["id"] for item in page["items"]] == [target]
    assert page["total"] == 1

    page = store.list_items(q="单位")
    assert [item["id"] for item in page["items"]] == [other]


def test_fts_query_ors_chinese_grams():
    expression = em.build_fts_query("质量检查")

    assert '"质量"' in expression
    assert expression.count(" OR ") >= 2
    assert " AND " not in expression


def test_search_text_carries_grams():
    text = em.build_search_text("质量检查")

    assert text.startswith("质量检查")
    assert "量检" in text.split(" ", 1)[1]


def test_unknown_term_returns_nothing(store):
    _add(store, experience())

    assert store.list_items(q="完全不相关的术语xyz")["total"] == 0


def test_fts_degradation_still_searches(store, tmp_path, monkeypatch):
    monkeypatch.setattr(
        em, "_FTS_SCHEMA",
        "CREATE VIRTUAL TABLE memory_item_fts USING fts5_missing_module(x);",
    )
    degraded = MemoryStore(tmp_path / "degraded.db")
    try:
        item_id = _add(degraded, experience())
        degraded.transition(item_id, "approve")

        assert degraded.fts_available is False
        assert degraded.health()["status"] == "healthy"
        # FTS 不可用时退回 LIKE，中文检索照旧
        assert [item["id"] for item in degraded.list_items(q="质量检查")["items"]] == [item_id]
        assert degraded.recall(
            query="质量检查", scopes=["global"], task_type="mesh_quality"
        )
    finally:
        degraded.close()


# --------------------------------------------------------------------------- #
# 置信度与曝光
# --------------------------------------------------------------------------- #

def test_exposure_never_moves_confidence(store):
    item_id = _add(store, experience())
    store.transition(item_id, "approve")
    before = store.get(item_id)["confidence"]

    store.record_exposure([item_id, item_id])

    after = store.get(item_id)
    # "被看到的次数多"不等于"被验证过"：曝光只用于观测
    assert after["confidence"] == before
    assert after["exposure_count"] == 1


def test_feedback_moves_confidence_in_the_right_direction(store):
    item_id = _add(store, experience())
    store.transition(item_id, "approve")
    base = store.get(item_id)["confidence"]

    up = store.record_feedback(item_id, "helpful")["confidence"]
    assert up > base

    down = store.record_feedback(item_id, "harmful")["confidence"]
    assert down < up

    verified = store.record_feedback(item_id, "verified_success")["confidence"]
    assert verified > down

    with pytest.raises(ValueError):
        store.record_feedback(item_id, "被检索到了")


def test_confidence_is_clamped(store):
    item_id = _add(store, experience(confidence=9.0))
    assert store.get(item_id)["confidence"] == 1.0

    item_id = _add(store, experience(title="另一条", confidence=-3))
    assert store.get(item_id)["confidence"] == 0.0


# --------------------------------------------------------------------------- #
# 治理
# --------------------------------------------------------------------------- #

def test_governance_merges_near_duplicates(store):
    # 两条标题相差一个字（指纹不同），但步骤/触发/失败模式完全一致，
    # Jaccard 约 0.90 —— 落在自动合并带里
    first = _add(store, experience(title="质量检查超标时先做局部加密处理"))
    second = _add(store, experience(title="质量检查超标时先做局部加密修复"))

    report = store.run_governance()

    assert report["merged"] == 1
    survivors = store.list_items(status="candidate")["items"]
    assert len(survivors) == 1
    keeper = survivors[0]
    # 合并只改证据与更新时间，不自动提升置信度
    assert keeper["evidence_count"] == 2
    assert keeper["confidence"] == em.SOURCE_PRIORS["validated_workflow"]
    # 删除不是静默的：被合并的那条留下一条 merge 审计，指向存活的那条
    merges = [row for row in store.audit_tail() if row["action"] == "merge"]
    assert len(merges) == 1
    assert merges[0]["reason"] == keeper["id"]
    assert {merges[0]["item_id"], keeper["id"]} == {first, second}


def test_governance_flags_possible_duplicates(store):
    # 标题措辞差得更多（Jaccard 约 0.77），落在"疑似重复，交给人判断"带里
    _add(store, experience(title="质量检查超标时先做局部处理"))
    _add(store, experience(
        title="质量检查未达标时先做局部处理",
        failure_patterns=["直接做全局重划会放大网格量"],
    ))

    store.run_governance()

    flagged = store.list_items(status="candidate")["items"]
    assert sum(1 for item in flagged if item["possible_duplicate"]) == 2
    assert store.counts()["possible_duplicate"] == 2
    # 疑似重复不是删除理由，两条都还在，只是打了标记
    assert len(flagged) == 2


def test_governance_archives_stale_candidates(store):
    old = _add(store, distinct_experience(0))
    fresh = _add(store, distinct_experience(1))
    _backdate(store, old, em.CANDIDATE_MAX_AGE_DAYS + 5)

    report = store.run_governance()

    assert report["archived"] == 1
    assert store.get(old)["status"] == "rejected"
    assert store.get(fresh)["status"] == "candidate"


def test_governance_never_touches_approved(store):
    approved = _add(store, experience())
    store.transition(approved, "approve")
    _backdate(store, approved, 400)

    report = store.run_governance()

    assert report["archived"] == 0
    assert store.get(approved)["status"] == "approved"


def test_governance_enforces_candidate_cap(store, monkeypatch):
    monkeypatch.setattr(em, "CANDIDATE_CAP_PER_SCOPE", 3)
    ids = [_add(store, distinct_experience(i, evidence_count=i + 1)) for i in range(6)]

    report = store.run_governance()

    assert report["archived"] == 3
    survivors = store.list_items(status="candidate")["items"]
    assert len(survivors) == 3
    # 淘汰按「证据最少、最旧」先走，证据多的一批要留下来
    kept = {item["id"] for item in survivors}
    assert kept == set(ids[3:])


# --------------------------------------------------------------------------- #
# 批量、删除、审计
# --------------------------------------------------------------------------- #

def test_bulk_transition_allows_partial_success(store):
    a = _add(store, experience())
    b = _add(store, experience(title="第二条互不相同的经验"))
    store.transition(b, "approve")

    results = store.bulk_transition([a, b, "exp_missing"], "approve")

    by_id = {row["id"]: row for row in results}
    assert by_id[a]["ok"] is True
    # 已经在目标状态上：算成功，重放不会报错
    assert by_id[b] == {"id": b, "ok": True, "code": "unchanged"}
    assert by_id["exp_missing"]["code"] == "not_found"


def test_bulk_rejects_dangerous_actions(store):
    item_id = _add(store, experience())

    for action in ("deprecate", "restore", "reopen", "delete"):
        with pytest.raises(ValueError):
            store.bulk_transition([item_id], action)


def test_delete_removes_record_and_returns_false_next_time(store):
    item_id = _add(store, experience())

    assert store.delete(item_id) is True
    assert store.delete(item_id) is False
    assert store.counts()["total"] == 0

    actions = [row["action"] for row in store.audit_tail()]
    assert actions[-1] == "delete"


def test_audit_records_action_not_text(store):
    item_id = _add(store, experience())
    store.transition(item_id, "reject", reason="证据不足，且来源是单次偶然成功")

    tail = store.audit_tail()
    # audit_tail 按 seq 倒序，最新的在最前
    assert [(row["action"], row["item_id"]) for row in tail] == [
        ("reject", item_id), ("create", item_id),
    ]
    assert tail[0]["reason"].startswith("证据不足")
    # create 的 reason 只记来源类别，不记正文
    assert tail[-1]["reason"] == "validated_workflow"
    # 审计不含经验正文，删掉经验后也不会留下副本
    assert "局部加密" not in str(tail)


def test_audit_reason_is_truncated(store):
    item_id = _add(store, experience())
    store.transition(item_id, "reject", reason="x" * 500)

    assert len(store.audit_tail()[-1]["reason"]) <= 200


# --------------------------------------------------------------------------- #
# 列表、分面与健康
# --------------------------------------------------------------------------- #

def test_list_pages_and_orders(store):
    ids = [_add(store, experience(title="经验条目 %d" % i)) for i in range(5)]

    page = store.list_items(status="candidate", page=1, page_size=2)
    assert page["total"] == 5
    assert page["page_size"] == 2
    assert len(page["items"]) == 2
    # 候选按创建时间倒序：最新的排在最前
    assert page["items"][0]["id"] == ids[-1]

    assert store.list_items(page=1, page_size=999)["page_size"] == 100


def test_facets_expose_scopes_and_task_types(store):
    _add(store, experience(scope="global"))
    _add(store, experience(scope="skill:cfd-meshing-workflow",
                           title="另一个作用域下的经验"))
    _add(store, fact())

    facets = store.facets()

    assert {row["value"] for row in facets["scopes"]} == {
        "global", "skill:cfd-meshing-workflow",
    }
    assert "mesh_quality" in {row["value"] for row in facets["task_types"]}


def test_counts_group_by_kind_and_status(store):
    item_id = _add(store, experience())
    store.transition(item_id, "approve")
    _add(store, fact())

    counts = store.counts()

    assert counts["experience"]["approved"] == 1
    assert counts["fact"]["candidate"] == 1
    assert counts["total"] == 2


def test_health_reports_unwritable_store(tmp_path):
    blocker = tmp_path / "blocker"
    blocker.write_text("not a directory", encoding="utf-8")
    broken = MemoryStore(blocker / "memory.db")

    with pytest.raises(em.StoreUnavailable):
        broken.add_item(experience())

    health = broken.health()
    assert health["status"] == "unavailable"
    assert health["writable"] is False
    assert health["last_error"]


def test_disabled_store_refuses_writes(tmp_path):
    disabled = MemoryStore(tmp_path / "memory.db", enabled=False)

    with pytest.raises(em.StoreUnavailable):
        disabled.add_item(experience())


def test_set_flag_marks_record(store):
    item_id = _add(store, experience())

    store.set_flag(item_id, "needs_review")
    assert "needs_review" in store.get(item_id)["flags"]

    store.set_flag(item_id, "needs_review", enabled=False)
    assert "needs_review" not in store.get(item_id)["flags"]


def test_wipe_clears_everything(store):
    _add(store, experience())
    _add(store, fact())

    assert store.wipe() == {"experience": 1, "fact": 1}
    assert store.counts()["total"] == 0


def test_item_payload_survives_roundtrip(store):
    item_id = _add(store, experience())

    item = store.get(item_id)
    assert item["trigger_cues"] == ["质量检查报错", "单元质量低于阈值"]
    assert item["strategy_steps"][0].startswith("先用质量检查")
    assert item["tool_sequence"] == ["ExamineMeshQuality", "UGReDimensionBySize"]
    assert item["source_kind"] == "validated_workflow"
    assert item["schema_version"] == em.SCHEMA_VERSION


def test_database_has_expected_schema(store, tmp_path):
    _add(store, experience())
    conn = sqlite3.connect(str(tmp_path / "memory.db"))
    try:
        tables = {row[0] for row in conn.execute(
            "SELECT name FROM sqlite_master WHERE type IN ('table','view')"
        )}
    finally:
        conn.close()

    assert {"memory_item", "memory_audit", "memory_counter"} <= tables
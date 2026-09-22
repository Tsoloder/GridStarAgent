"""usage_stats 单元测试：小时切桶、实测/估算分离、供应商归属、缓存与筛选。"""
import json
from datetime import datetime

import pytest
from fastapi.testclient import TestClient

import usage_stats
from usage_stats import UNKNOWN_MODEL, UNKNOWN_PROVIDER, collect_usage


@pytest.fixture
def sessions_root(monkeypatch, tmp_path):
    """隔离会话目录，并清掉模块级缓存，避免用例之间互相污染。"""
    monkeypatch.setattr(usage_stats, "SESSIONS_DIR", tmp_path)
    usage_stats._session_cache.clear()
    usage_stats._response_cache.clear()
    yield tmp_path
    usage_stats._session_cache.clear()
    usage_stats._response_cache.clear()


def write_session(root, sid, messages, model_id=""):
    path = root / sid
    path.mkdir(parents=True, exist_ok=True)
    (path / "messages.jsonl").write_text(
        "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in messages),
        encoding="utf-8",
    )
    (path / "meta.json").write_text(
        json.dumps({"id": sid, "title": "t", "created_at": "2026-09-01T00:00:00",
                    "updated_at": "2026-09-01T00:00:00", "model_id": model_id},
                   ensure_ascii=False),
        encoding="utf-8",
    )
    return path


def assistant(ts, **usage):
    return {"role": "assistant", "content": "x", "ts": ts, "usage": usage}


# ---------- 切桶 ----------

def test_buckets_are_hourly_and_zero_filled(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:03:14.882", input=100, output=20, total=120, model="openai/gpt-4o"),
        assistant("2026-09-22T12:30:00.000", input=5, output=5, total=10, model="openai/gpt-4o"),
    ])
    payload = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 12))

    assert payload["range"]["resolution"] == "hour"
    assert [item["t"] for item in payload["buckets"]] == [
        "2026-09-22T10", "2026-09-22T11", "2026-09-22T12"]
    assert payload["buckets"][0]["total"] == 120
    assert payload["buckets"][1]["total"] == 0
    assert payload["buckets"][1]["input"] == 0
    assert payload["buckets"][2]["total"] == 10


def test_hour_bucket_does_not_shift_across_day_boundary(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-21T23:59:59.999", input=1, output=0, total=1, model="a/m"),
        assistant("2026-09-22T00:00:00.000", input=1, output=0, total=1, model="a/m"),
    ])
    payload = collect_usage(datetime(2026, 9, 21, 23), datetime(2026, 9, 22, 0))
    totals = {item["t"]: item["total"] for item in payload["buckets"]}
    assert totals == {"2026-09-21T23": 1, "2026-09-22T00": 1}


def test_long_span_falls_back_to_day_resolution(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-02-01T10:00:00", input=3, output=0, total=3, model="a/m"),
    ])
    payload = collect_usage(datetime(2026, 1, 1), datetime(2026, 6, 1))

    assert payload["range"]["resolution"] == "day"
    assert payload["buckets"][0]["t"] == "2026-01-01"
    assert payload["buckets"][0]["total"] == 0
    assert next(item for item in payload["buckets"] if item["t"] == "2026-02-01")["total"] == 3


# ---------- 用量口径 ----------

def test_measured_and_estimated_are_separate(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=100, output=20, total=120, model="a/m"),
        assistant("2026-09-22T11:00:00", input=7, output=3, total=10, estimated=True, model="a/m"),
    ])
    totals = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 11))["totals"]

    assert totals["total"] == 130
    assert totals["measured"] == 120
    assert totals["estimated"] == 10
    assert totals["measured"] + totals["estimated"] == totals["total"]
    assert totals["turns"] == 2


def test_total_falls_back_to_input_plus_output(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=8, output=2, model="a/m"),
    ])
    totals = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))["totals"]
    assert totals["total"] == 10


def test_totals_count_distinct_sessions(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=1, output=0, total=1, model="a/m")])
    write_session(sessions_root, "s2", [
        assistant("2026-09-22T10:10:00", input=1, output=0, total=1, model="a/m"),
        assistant("2026-09-22T10:20:00", input=1, output=0, total=1, model="a/m"),
    ])
    totals = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))["totals"]

    assert totals["turns"] == 3
    assert totals["sessions"] == 2


# ---------- 供应商归属 ----------

def test_provider_from_model_prefix_and_fallback_to_session_model(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=1, output=0, total=2, model="openai/gpt-4o"),
        # 记录缺 model 字段：回落到会话级 model_id
        assistant("2026-09-22T11:00:00", input=1, output=0, total=2),
    ], model_id="anthropic/claude-sonnet-4")
    payload = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 11))

    providers = {row["provider"]: row["total"] for row in payload["providers"]}
    assert providers == {"openai": 2, "anthropic": 2}
    models = {row["model"]: row["total"] for row in payload["models"]}
    assert models == {"openai/gpt-4o": 2, "anthropic/claude-sonnet-4": 2}


def test_unknown_provider_when_no_model_information(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=1, output=0, total=2)])
    payload = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))

    assert payload["models"][0]["model"] == UNKNOWN_MODEL
    assert payload["models"][0]["provider"] == UNKNOWN_PROVIDER
    assert payload["models"][0]["label"] == "未知模型"
    assert payload["providers"][0]["provider"] == UNKNOWN_PROVIDER


def test_provider_label_uses_config_name(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=1, output=0, total=1, model="openai/gpt-4o")])
    payload = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10),
                            provider_names={"openai": "OpenAI"})
    assert payload["providers"][0]["label"] == "OpenAI"


# ---------- 筛选与窗口 ----------

def test_model_filter_limits_every_aggregate(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=10, output=0, total=10, model="a/m1"),
        assistant("2026-09-22T10:30:00", input=90, output=0, total=90, model="a/m2"),
    ])
    payload = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10), model="a/m1")

    assert payload["totals"]["total"] == 10
    assert [row["model"] for row in payload["models"]] == ["a/m1"]
    assert payload["buckets"][0]["total"] == 10


def test_provider_filter_limits_every_aggregate(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=10, output=0, total=10, model="alpha/m1"),
        assistant("2026-09-22T10:30:00", input=90, output=0, total=90, model="beta/m2"),
    ])
    payload = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10), provider="alpha")

    assert payload["totals"]["total"] == 10
    assert [row["provider"] for row in payload["providers"]] == ["alpha"]
    assert [row["model"] for row in payload["models"]] == ["alpha/m1"]
    assert payload["buckets"][0]["total"] == 10


def test_provider_and_model_filters_combine(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=10, output=0, total=10, model="alpha/m1"),
        assistant("2026-09-22T10:10:00", input=20, output=0, total=20, model="alpha/m2"),
        assistant("2026-09-22T10:20:00", input=30, output=0, total=30, model="beta/m1"),
    ])
    payload = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10),
                            model="alpha/m1", provider="alpha")

    assert payload["totals"]["total"] == 10
    # 矛盾条件（供应商 beta + 模型 alpha/m1）不会互相渗透出结果
    assert collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10),
                         model="alpha/m1", provider="beta")["totals"]["total"] == 0


def test_provider_filter_isolates_cache_entries(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=10, output=0, total=10, model="alpha/m1"),
        assistant("2026-09-22T10:10:00", input=90, output=0, total=90, model="beta/m2"),
    ])
    window = (datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))

    assert collect_usage(*window)["totals"]["total"] == 100
    assert collect_usage(*window, provider="alpha")["totals"]["total"] == 10
    assert collect_usage(*window, provider="beta")["totals"]["total"] == 90
    # 切回无筛选仍命中最初的结果，没有被后续筛选污染
    assert collect_usage(*window)["totals"]["total"] == 100


def test_candidates_ignore_filters(sessions_root):
    """候选列表只按时间窗过滤：下拉框要列有调用记录的模型，筛完不能缩水。"""
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=10, output=0, total=10, model="alpha/m1"),
        assistant("2026-09-22T10:10:00", input=20, output=0, total=20, model="alpha/m2"),
        assistant("2026-09-22T10:20:00", input=30, output=0, total=30, model="beta/m3"),
    ])
    window = (datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))

    payload = collect_usage(*window, model="alpha/m1")
    assert [row["model"] for row in payload["models"]] == ["alpha/m1"]
    assert payload["candidates"]["models"] and len(payload["candidates"]["models"]) == 3
    assert [row["model"] for row in payload["candidates"]["models"]] == ["beta/m3", "alpha/m2", "alpha/m1"]
    assert {row["provider"] for row in payload["candidates"]["providers"]} == {"alpha", "beta"}


def test_candidates_only_cover_the_window(sessions_root):
    """时间窗外的模型不进候选，避免下拉里出现必然查不到数据的选项。"""
    write_session(sessions_root, "s1", [
        assistant("2026-09-20T10:00:00", input=5, output=0, total=5, model="alpha/old"),
        assistant("2026-09-22T10:00:00", input=10, output=0, total=10, model="alpha/new"),
    ])
    payload = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))

    assert [row["model"] for row in payload["candidates"]["models"]] == ["alpha/new"]


def test_candidates_labels_use_provider_names(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=1, output=0, total=1, model="alpha/m1")])
    payload = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10),
                            provider_names={"alpha": "Alpha"})

    assert payload["candidates"]["providers"][0]["label"] == "Alpha"
    assert payload["candidates"]["models"][0]["label"] == "m1"


def test_candidates_empty_when_window_has_no_records(sessions_root):
    payload = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))

    assert payload["candidates"] == {"providers": [], "models": []}


def test_window_excludes_out_of_range_records(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-21T23:59:59", input=100, output=0, total=100, model="a/m"),
        assistant("2026-09-22T10:00:00", input=5, output=0, total=5, model="a/m"),
        assistant("2026-09-23T00:00:00", input=100, output=0, total=100, model="a/m"),
    ])
    totals = collect_usage(datetime(2026, 9, 22, 0), datetime(2026, 9, 22, 23, 59))["totals"]

    assert totals["total"] == 5
    assert totals["turns"] == 1


def test_records_without_usage_or_timestamp_are_skipped(sessions_root):
    write_session(sessions_root, "s1", [
        {"role": "user", "content": "hi", "ts": "2026-09-22T10:00:00"},
        {"role": "assistant", "content": "no usage", "ts": "2026-09-22T10:01:00"},
        {"role": "assistant", "content": "bad ts", "ts": "", "usage": {"total": 50}},
        assistant("2026-09-22T10:02:00", input=2, output=0, total=2, model="a/m"),
    ])
    totals = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))["totals"]

    assert totals["total"] == 2
    assert totals["turns"] == 1


def test_empty_window_returns_zero_structure(sessions_root):
    payload = collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 11))

    assert payload["totals"]["total"] == 0
    assert payload["totals"]["sessions"] == 0
    assert payload["providers"] == []
    assert payload["models"] == []
    assert len(payload["buckets"]) == 2


def test_reversed_window_is_normalized(sessions_root):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=4, output=0, total=4, model="a/m")])
    payload = collect_usage(datetime(2026, 9, 22, 12), datetime(2026, 9, 22, 10))

    assert payload["totals"]["total"] == 4
    assert payload["buckets"][0]["t"] == "2026-09-22T10"


# ---------- 旧格式与缓存 ----------

def test_legacy_messages_json_is_read(sessions_root):
    path = sessions_root / "s1"
    path.mkdir(parents=True)
    (path / "messages.json").write_text(
        json.dumps([assistant("2026-09-22T10:00:00", input=4, output=0, total=4, model="a/m")]),
        encoding="utf-8",
    )
    (path / "meta.json").write_text(json.dumps({"id": "s1", "model_id": "a/m"}), encoding="utf-8")

    assert collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))["totals"]["total"] == 4


def test_session_file_is_parsed_once_until_it_changes(sessions_root, monkeypatch):
    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=1, output=0, total=1, model="a/m")])
    calls = []
    original = usage_stats._read_jsonl

    def counting(path):
        calls.append(path)
        return original(path)

    monkeypatch.setattr(usage_stats, "_read_jsonl", counting)
    collect_usage(datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))
    assert len(calls) == 1

    # 换个窗口仍然复用解析结果，不重新读文件
    collect_usage(datetime(2026, 9, 21), datetime(2026, 9, 22, 23))
    assert len(calls) == 1


def test_new_message_invalidates_session_and_response_cache(sessions_root):
    path = write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=1, output=0, total=1, model="a/m")])
    window = (datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))
    assert collect_usage(*window)["totals"]["total"] == 1

    with open(path / "messages.jsonl", "a", encoding="utf-8") as handle:
        handle.write(json.dumps(
            assistant("2026-09-22T10:30:00", input=9, output=0, total=9, model="a/m"),
            ensure_ascii=False) + "\n")

    assert collect_usage(*window)["totals"]["total"] == 10


def test_removed_session_is_dropped_from_cache(sessions_root):
    import shutil

    path = write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=1, output=0, total=1, model="a/m")])
    window = (datetime(2026, 9, 22, 10), datetime(2026, 9, 22, 10))
    assert collect_usage(*window)["totals"]["total"] == 1

    shutil.rmtree(path)
    assert collect_usage(*window)["totals"]["total"] == 0
    assert "s1" not in usage_stats._session_cache


# ---------- HTTP 接口 ----------

def test_usage_stats_endpoint_returns_aggregates(sessions_root):
    import app as server

    write_session(sessions_root, "s1", [
        assistant("2026-09-22T10:00:00", input=30, output=10, total=40, model="openai/gpt-4o")])
    client = TestClient(server.app)
    response = client.get("/usage/stats", params={
        "start": "2026-09-22T10:00", "end": "2026-09-22T11:00"})

    assert response.status_code == 200
    body = response.json()
    assert body["range"]["resolution"] == "hour"
    assert body["totals"]["total"] == 40
    assert body["models"][0]["model"] == "openai/gpt-4o"
    assert len(body["buckets"]) == 2


def test_usage_stats_endpoint_defaults_to_last_seven_days(sessions_root):
    import app as server

    response = TestClient(server.app).get("/usage/stats")

    assert response.status_code == 200
    body = response.json()
    assert body["range"]["resolution"] == "hour"
    assert "totals" in body and "buckets" in body
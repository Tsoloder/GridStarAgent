"""Token 用量统计：聚合会话落盘记录中的 usage 字段。

只读扫描 sessions/*/messages.jsonl，按文件 (mtime, size) 复用解析结果，
不新增写入链路，也不改动消息持久化逻辑。
"""
import json
import logging
import threading
from datetime import datetime, timedelta
from pathlib import Path

from paths import SESSIONS_DIR
from session import _read_jsonl

logger = logging.getLogger(__name__)

# 跨度超过该天数时不再提供小时桶，避免长区间产生上万点位
HOUR_RESOLUTION_MAX_DAYS = 90
# 响应缓存条目上限，超过即整体清空
_RESPONSE_CACHE_LIMIT = 32
# 记录缺少 model 字段且会话也没有 model_id 时的归属
UNKNOWN_PROVIDER = "unknown"
UNKNOWN_MODEL = f"{UNKNOWN_PROVIDER}/unknown"

_SUM_FIELDS = ("input", "output", "total", "measured", "estimated",
               "cache_read", "cache_write", "reasoning")

_cache_lock = threading.RLock()
# sid -> {"key": (messages.jsonl / messages.json / meta.json 的 (mtime, size)), "records": [...]}
_session_cache = {}
# (start, end, model, resolution, provider_names) -> payload
_response_cache = {}


def _int(value) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _stat_key(path: Path):
    try:
        info = path.stat()
    except OSError:
        return None
    return (info.st_mtime_ns, info.st_size)


def _bucket_hour(ts) -> str:
    """消息 ts -> 小时桶标签（YYYY-MM-DDTHH）；无法识别时返回空串。"""
    text = str(ts or "").strip().replace(" ", "T", 1)
    if len(text) < 13:
        return ""
    head = text[:13]
    if head[4] != "-" or head[7] != "-" or head[10] != "T":
        return ""
    return head


def _split_model(model_key: str):
    """provider/model -> (provider, 模型显示名)。

    供应商 ID 不含 "/"，全仓的 provider/model 解析都依赖这一约定。
    """
    key = str(model_key or "").strip()
    if not key:
        return UNKNOWN_PROVIDER, ""
    if "/" in key:
        provider, label = key.split("/", 1)
        return provider or UNKNOWN_PROVIDER, label
    return UNKNOWN_PROVIDER, key


def _read_session_model(meta_path: Path) -> str:
    """会话级 model_id，用于记录缺少 usage.model 时回落到供应商归属。"""
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except Exception:
        return ""
    return str(meta.get("model_id", "") or "").strip()


def _read_session_records(session_path: Path, fallback_model: str) -> list:
    """把一个会话的消息文件解析成用量记录列表。"""
    jsonl_path = session_path / "messages.jsonl"
    json_path = session_path / "messages.json"
    try:
        if jsonl_path.exists():
            messages = _read_jsonl(str(jsonl_path))
        elif json_path.exists():
            # 旧版兼容：老会话只有 messages.json
            loaded = json.loads(json_path.read_text(encoding="utf-8"))
            messages = loaded if isinstance(loaded, list) else []
        else:
            return []
    except Exception as exc:
        logger.warning("usage stats: read %s failed: %s", session_path.name, exc)
        return []

    sid = session_path.name
    records = []
    for message in messages:
        if not isinstance(message, dict):
            continue
        usage = message.get("usage")
        if not isinstance(usage, dict):
            # 老会话没有 usage 字段，跳过而不是反推估算
            continue
        bucket = _bucket_hour(message.get("ts"))
        if not bucket:
            continue
        model_key = str(usage.get("model") or "").strip() or fallback_model
        provider, label = _split_model(model_key)
        if not label:
            # 既没有 model 也没有会话 model_id：归到「未知模型」，保证筛选项非空
            model_key, provider, label = UNKNOWN_MODEL, UNKNOWN_PROVIDER, "未知模型"
        estimated = bool(usage.get("estimated", False))
        input_tokens = _int(usage.get("input"))
        output_tokens = _int(usage.get("output"))
        total = _int(usage.get("total")) or (input_tokens + output_tokens)
        records.append({
            "h": bucket,
            "sid": sid,
            "model": model_key,
            "provider": provider,
            "label": label,
            "input": input_tokens,
            "output": output_tokens,
            "total": total,
            # 实测与估算分开记账，两者之和恒等于 total
            "measured": 0 if estimated else total,
            "estimated": total if estimated else 0,
            "cache_read": _int(usage.get("cache_read")),
            "cache_write": _int(usage.get("cache_write")),
            "reasoning": _int(usage.get("reasoning")),
        })
    return records


def _load_sessions() -> list:
    """返回 [(sid, records)] 快照。

    只有文件 (mtime, size) 变化、新增或删除会话时才重新解析，
    数据变化时清空响应缓存。
    """
    global _response_cache
    with _cache_lock:
        changed = False
        seen = set()
        try:
            entries = sorted(SESSIONS_DIR.iterdir())
        except OSError:
            entries = []
        for path in entries:
            if not path.is_dir():
                continue
            sid = path.name
            jsonl_path = path / "messages.jsonl"
            json_path = path / "messages.json"
            meta_path = path / "meta.json"
            key = (_stat_key(jsonl_path), _stat_key(json_path), _stat_key(meta_path))
            if key == (None, None, None):
                continue
            seen.add(sid)
            cached = _session_cache.get(sid)
            if cached is not None and cached["key"] == key:
                continue
            records = _read_session_records(path, _read_session_model(meta_path))
            _session_cache[sid] = {"key": key, "records": records}
            changed = True
        for sid in list(_session_cache):
            if sid not in seen:
                del _session_cache[sid]
                changed = True
        if changed:
            _response_cache = {}
        return [(sid, entry["records"]) for sid, entry in _session_cache.items()]


def _bucket_labels(start: datetime, end: datetime, resolution: str) -> list:
    labels = []
    if resolution == "hour":
        cursor = start.replace(minute=0, second=0, microsecond=0)
        last = end.replace(minute=0, second=0, microsecond=0)
        step, fmt = timedelta(hours=1), "%Y-%m-%dT%H"
    else:
        cursor = start.replace(hour=0, minute=0, second=0, microsecond=0)
        last = end.replace(hour=0, minute=0, second=0, microsecond=0)
        step, fmt = timedelta(days=1), "%Y-%m-%d"
    while cursor <= last:
        labels.append(cursor.strftime(fmt))
        cursor += step
    return labels


def _blank_sums() -> dict:
    return {name: 0 for name in _SUM_FIELDS}


def _collect_records(selected: list, resolution: str):
    """把记录累加成 totals、按小时或天切桶，以及供应商与模型两个维度的聚合。"""
    totals = _blank_sums()
    totals["turns"] = 0
    all_sessions = set()
    buckets, providers, models = {}, {}, {}

    for record in selected:
        bucket_key = record["h"] if resolution == "hour" else record["h"][:10]
        bucket = buckets.get(bucket_key)
        if bucket is None:
            bucket = _blank_sums()
            bucket["turns"] = 0
            buckets[bucket_key] = bucket

        provider_group = providers.get(record["provider"])
        if provider_group is None:
            provider_group = _blank_sums()
            provider_group.update({"turns": 0, "sessions": set()})
            providers[record["provider"]] = provider_group

        model_group = models.get(record["model"])
        if model_group is None:
            model_group = _blank_sums()
            model_group.update({"turns": 0, "sessions": set(), "label": record["label"]})
            models[record["model"]] = model_group

        for target in (totals, bucket, provider_group, model_group):
            for name in _SUM_FIELDS:
                target[name] += record[name]
            target["turns"] += 1
        all_sessions.add(record["sid"])
        provider_group["sessions"].add(record["sid"])
        model_group["sessions"].add(record["sid"])

    totals["sessions"] = len(all_sessions)
    return totals, buckets, providers, models


def _group_rows(groups: dict, id_key: str) -> list:
    rows = []
    for group_id, group in groups.items():
        rows.append({
            id_key: group_id,
            "total": group["total"], "input": group["input"], "output": group["output"],
            "measured": group["measured"], "estimated": group["estimated"],
            "cache_read": group["cache_read"], "cache_write": group["cache_write"],
            "reasoning": group["reasoning"], "turns": group["turns"],
            "sessions": len(group["sessions"]),
        })
    rows.sort(key=lambda item: (-item["total"], item[id_key]))
    return rows


def _label_providers(rows: list, names: dict) -> list:
    """供应商行补显示名：配置里查得到就用配置名，查不到保留原始 ID。"""
    for row in rows:
        row["label"] = names.get(row["provider"]) or row["provider"]
    return rows


def _label_models(rows: list, groups: dict) -> list:
    """模型行补显示名与所属供应商。"""
    for row in rows:
        row["label"] = groups[row["model"]]["label"]
        row["provider"] = row["model"].split("/", 1)[0] if "/" in row["model"] else UNKNOWN_PROVIDER
    return rows


def collect_usage(start: datetime, end: datetime, model: str = "", provider: str = "",
                  provider_names=None) -> dict:
    """聚合给定时间窗内的 token 用量。

    start / end 为本地时间，与消息 ts 同一时区口径；model 与 provider 非空时按该维度筛选。
    """
    start = start.replace(microsecond=0)
    end = end.replace(microsecond=0)
    if end < start:
        start, end = end, start
    span_days = (end - start).total_seconds() / 86400.0
    resolution = "hour" if span_days <= HOUR_RESOLUTION_MAX_DAYS else "day"

    names = dict(provider_names or {})
    cache_key = (start.isoformat(), end.isoformat(), model, provider, resolution,
                 tuple(sorted(names.items())))
    # 先做一次文件变更检查：会话有增删改时会清空响应缓存，避免返回过期结果
    snapshot = _load_sessions()
    with _cache_lock:
        cached = _response_cache.get(cache_key)
        if cached is not None:
            return cached

    first_bucket = start.strftime("%Y-%m-%dT%H")
    last_bucket = end.strftime("%Y-%m-%dT%H")
    target_model = str(model or "").strip()
    target_provider = str(provider or "").strip()

    # 时间窗内的全部记录（不受筛选影响），当前视图与候选列表都由它派生
    window_records = []
    for _sid, records in snapshot:
        for record in records:
            if first_bucket <= record["h"] <= last_bucket:
                window_records.append(record)

    selected = [
        record for record in window_records
        if (not target_model or record["model"] == target_model)
        and (not target_provider or record["provider"] == target_provider)
    ]

    totals, bucket_sums, provider_groups, model_groups = _collect_records(selected, resolution)
    # 候选列表只按时间窗过滤：下拉框要列「有调用记录」的模型与供应商，
    # 若跟着筛选缩水，用户筛完就再也切不回别的选项了。
    _, _, cand_provider_groups, cand_model_groups = _collect_records(window_records, resolution)

    buckets = []
    for label in _bucket_labels(start, end, resolution):
        sums = bucket_sums.get(label)
        if sums is None:
            sums = _blank_sums()
            sums["turns"] = 0
        buckets.append({"t": label, **sums})

    provider_rows = _label_providers(_group_rows(provider_groups, "provider"), names)
    model_rows = _label_models(_group_rows(model_groups, "model"), model_groups)

    payload = {
        "range": {
            "start": start.strftime("%Y-%m-%dT%H:%M"),
            "end": end.strftime("%Y-%m-%dT%H:%M"),
            "resolution": resolution,
        },
        "totals": totals,
        "buckets": buckets,
        "providers": provider_rows,
        "models": model_rows,
        "candidates": {
            "providers": _label_providers(_group_rows(cand_provider_groups, "provider"), names),
            "models": _label_models(_group_rows(cand_model_groups, "model"), cand_model_groups),
        },
    }

    with _cache_lock:
        if len(_response_cache) >= _RESPONSE_CACHE_LIMIT:
            _response_cache.clear()
        _response_cache[cache_key] = payload
    return payload
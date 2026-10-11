"""台账与模型输出的脱敏层。

长期记忆一旦写错就几乎不会被纠正，所以这里设两道防线，缺一不可：

1. **进提炼模型之前**（sanitize_ledger）：把执行痕迹压成「形状」。只保留工具名、
   参数键名、值的类型与量级、成败和结果类别；字符串原值、路径、实体 ID 一律
   不进模型上下文。台账的 args_digest 里真的躺着 ``10.76.19.98`` 和
   ``D:/A_GridStarCode/output`` 这类东西，不能直接喂给提炼模型。
2. **落盘之前**（assert_clean）：对模型生成的正文逐字段再扫一遍，命中敏感模式
   整条丢弃。第一层是「尽量少给」，但模型仍可能把看到的东西复述出来，或者在
   写标题时把实体名带进去；只做一层等于把泄漏风险全押在提示词上。

本模块是纯函数，不 import 任何项目内模块，方便单独测试。
"""
from __future__ import annotations

import json
import re
from typing import Any, Iterable, Mapping, Sequence

REDACTED = "[已脱敏]"

# 阶段状态取自 task_ledger.PLAN_STATUSES，这里复制一份以免引入会话依赖。
PLAN_STATUSES: tuple[str, ...] = ("pending", "in_progress", "done", "failed", "skipped")

# --------------------------------------------------------------------------
# 敏感模式
# --------------------------------------------------------------------------

_CAD_EXT = (
    "igs", "iges", "stp", "step", "stl", "obj", "ply", "x_t", "x_b", "prt", "sldprt",
    "sldasm", "catpart", "catproduct", "3dxml", "sat", "brep", "msh", "mshb", "cgns",
    "cas", "nas", "bdf", "inp", "unv", "vtk", "vtu", "neu",
)
_OTHER_EXT = (
    "docx", "doc", "xlsx", "xls", "pptx", "ppt", "pdf", "csv", "txt", "md", "log",
    "json", "xml", "yaml", "yml", "png", "jpg", "jpeg", "bmp", "gif", "mp4", "py",
    "js", "ts", "html", "css", "zip", "rar", "7z", "exe", "dll", "bat", "sh",
)
_FILE_EXT_PATTERN = "|".join(
    sorted(set(_CAD_EXT + _OTHER_EXT), key=len, reverse=True)
)

_PATH_CHARS = r"[^\s\"'<>|，。；、（）()\[\]{}]"

# 顺序即优先级：先吃掉 URL 与完整路径，再处理它们内部可能出现的主机名。
PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("url", re.compile(r"(?i)\b(?:https?|ftp|file)://" + _PATH_CHARS + "+")),
    ("windows_path", re.compile(r"[A-Za-z]:[\\/]" + _PATH_CHARS + "*")),
    ("unc_path", re.compile(r"\\\\" + _PATH_CHARS + "+")),
    ("posix_path", re.compile(r"(?<![\w:/])/(?:[A-Za-z0-9_.-]+/)+[A-Za-z0-9_.-]*")),
    ("email", re.compile(r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b")),
    ("uuid", re.compile(
        r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
        r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b"
    )),
    ("host", re.compile(r"\b\d{1,3}(?:\.\d{1,3}){3}(?::\d{1,5})?\b")),
    ("credential", re.compile(
        r"(?i)(?:\b(?:api[_-]?key|token|secret|password|passwd|pwd|credential|"
        r"authorization)\b\s*[:=]\s*\S+"
        # 裸令牌：没有 key= 前缀也要认出来，sk-xxx 这种本就是秘密本身
        r"|\b(?:sk|pk|rk|ghp|gho|ghu|ghs|ghr|xox[baprs]|akia|asia)[-_][A-Za-z0-9_\-]{16,}\b)"
    )),
    ("filename", re.compile(
        r"[\w\u4e00-\u9fff.\-]+\.(?:" + _FILE_EXT_PATTERN + r")\b", re.I
    )),
    ("entity_id", re.compile(
        r"(?i)\b(?:face|cell|node|edge|vertex|point|solid|surface|curve|part|body|"
        r"domain|region|boundary|group|wire|shell|loop|patch|block|zone)[_\-]?\d+\b"
    )),
    ("high_precision_number", re.compile(r"-?\b\d+\.\d{4,}\b")),
    # 长 token 必须是「24 位以上且含数字」。真实泄漏物是哈希、base64、临时标识，
    # 它们几乎必然含数字；而纯字母的长串多半是本项目的工具名
    # （GetGenerateSurMeshDefaultParam 30 位、ProcessAllTrailingEdges 23 位），
    # 一刀切会把正常术语判成泄漏，让提炼层持续丢记录。
    ("long_token", re.compile(r"\b(?=[A-Za-z0-9]{24,}\b)(?=[A-Za-z0-9]*\d)[A-Za-z0-9]{24,}\b")),
)

PATTERN_INDEX: Mapping[str, re.Pattern[str]] = {name: regex for name, regex in PATTERNS}

# 这些字段由代码生成或来自受控枚举，不参与正文敏感扫描。按「路径分段」豁免，
# 因此 source_version 下的 skill_hash 这类长摘要不会被误判成泄漏。
# tool_sequence 走单独的工具名白名单校验，因为它天然包含长标识符。
TEXT_FIELD_EXEMPT: frozenset[str] = frozenset({
    "id", "schema_version", "kind", "status", "scope", "task_type", "source_kind",
    "source_version", "fingerprint", "flags", "created_at", "updated_at",
    "last_exposed_at", "last_verified_at", "search_text", "tool_sequence",
    "confidence", "evidence_count", "exposure_count", "helpful_count",
    "harmful_count", "verified_success_count", "verified_failure_count",
})

# 工具名形如 GenerateANisoDomainGrid / create_mesh，允许长标识符但不许出现数字 ID。
_TOOL_NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]{1,63}$")
_SAFE_KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,63}$")
# 键名本身是凭据时，连名字都不留（api_key / token / password …）
_SECRET_KEY_NAMES = frozenset({
    "apikey", "api_key", "token", "accesstoken", "access_token", "secret",
    "password", "passwd", "pwd", "credential", "credentials", "authorization",
})
# 驼峰切分：objectId → ["object","Id"]，与 task_ledger._is_id_key 同一套规则，
# 避免「台账认为这是 ID、脱敏层认为不是」的两套判断。
_CAMEL_SPLIT_RE = re.compile(r"[^0-9A-Za-z]+|(?<=[a-z0-9])(?=[A-Z])")
_ID_TAILS = frozenset({"id", "ids", "identifier", "identifiers"})


class SensitiveContentError(ValueError):
    """落盘前的第二道防线命中。

    只携带字段名与模式名：这两样可以进日志，命中的原文绝不进日志，
    否则日志本身就成了泄漏通道。
    """

    def __init__(self, field: str, patterns: Sequence[str]) -> None:
        self.field = field
        self.patterns = tuple(patterns)
        super().__init__("sensitive content in %s: %s" % (field, ", ".join(self.patterns)))


# --------------------------------------------------------------------------
# 检测与遮蔽
# --------------------------------------------------------------------------


def find_sensitive(text: Any) -> list[str]:
    """返回命中的模式名列表（不含命中的原文）。"""
    if not isinstance(text, str) or not text:
        return []
    return [name for name, regex in PATTERNS if regex.search(text)]


def scrub_text(value: Any) -> str:
    """把文本里所有敏感片段替换成占位符，保留可读骨架。"""
    if value is None:
        return ""
    text = value if isinstance(value, str) else str(value)
    for _name, regex in PATTERNS:
        text = regex.sub(REDACTED, text)
    return text


def scrub_arg_key(key: Any) -> str:
    """参数键名通常是 schema 声明的标识符，原样保留；不像标识符的一律抹掉。

    这里不返回"部分清洗后的原文"：键名要么是可读的 schema 标识符，要么说明
    它根本不是 schema 声明的键，那就没有保留的价值。
    """
    text = "" if key is None else str(key)
    if _SAFE_KEY_RE.match(text) and text.lower() not in _SECRET_KEY_NAMES:
        return text
    return REDACTED


def safe_tool_name(value: Any) -> str:
    """工具名要么是合法标识符，要么整体丢弃——半截名字无法描述步骤。"""
    text = "" if value is None else str(value).strip()
    if _TOOL_NAME_RE.match(text):
        return text
    return REDACTED


# --------------------------------------------------------------------------
# 值 → 形状
# --------------------------------------------------------------------------


def _number_shape(value: Any) -> str:
    kind = "int" if isinstance(value, int) and not isinstance(value, bool) else "float"
    magnitude = abs(float(value))
    if magnitude == 0:
        bucket = "zero"
    elif magnitude < 1:
        bucket = "fraction"
    elif magnitude < 100:
        bucket = "small"
    elif magnitude < 10000:
        bucket = "medium"
    else:
        bucket = "large"
    return "%s:%s" % (kind, bucket)


def _looks_like_identifier(text: str) -> bool:
    """短、含数字、无空格的串按 ID 处理（f6、part-A12、P001）。"""
    if not text or len(text) > 64 or " " in text:
        return False
    if re.fullmatch(r"\d+", text):
        return True
    if re.fullmatch(r"[A-Za-z0-9_\-]+", text) and any(ch.isdigit() for ch in text):
        return True
    return False


def _string_shape(value: str) -> str:
    if not value:
        return "text:empty"
    if PATTERN_INDEX["url"].search(value):
        return "url:redacted"
    if (
        PATTERN_INDEX["windows_path"].search(value)
        or PATTERN_INDEX["unc_path"].search(value)
        or PATTERN_INDEX["posix_path"].search(value)
    ):
        return "path:redacted"
    if PATTERN_INDEX["host"].search(value):
        return "host:redacted"
    if PATTERN_INDEX["email"].search(value):
        return "email:redacted"
    if PATTERN_INDEX["credential"].search(value):
        return "credential:redacted"
    if PATTERN_INDEX["uuid"].fullmatch(value.strip()):
        return "uuid:redacted"
    if PATTERN_INDEX["filename"].search(value):
        return "filename:redacted"
    if _looks_like_identifier(value):
        return "id:redacted"
    length = len(value)
    if length <= 8:
        bucket = "tiny"
    elif length <= 40:
        bucket = "short"
    elif length <= 200:
        bucket = "medium"
    else:
        bucket = "long"
    return "text:%s" % bucket


def shape_of(value: Any) -> str:
    """把一个参数值压成不含任何原值的形状描述。"""
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, (int, float)):
        return _number_shape(value)
    if isinstance(value, str):
        return _string_shape(value)
    if isinstance(value, Mapping):
        keys = sorted(scrub_arg_key(key) for key in value)
        return "object:keys=[%s]" % ",".join(keys[:12])
    if isinstance(value, (list, tuple, set)):
        items = list(value)
        if not items:
            return "list:empty"
        return "list[%d]:%s" % (len(items), shape_of(items[0]))
    return "other:%s" % type(value).__name__


def sanitize_args(args: Any) -> dict[str, Any]:
    """参数 → 键名 + 形状。原值一个都不留。"""
    if not isinstance(args, Mapping):
        return {"arg_keys": [], "arg_shapes": {}}
    keys: list[str] = []
    shapes: dict[str, str] = {}
    for key, value in args.items():
        safe_key = scrub_arg_key(key)
        keys.append(safe_key)
        shapes[safe_key] = shape_of(value)
    return {"arg_keys": sorted(keys), "arg_shapes": shapes}


def _is_id_key(key: Any) -> bool:
    parts = [part for part in _CAMEL_SPLIT_RE.split(str(key)) if part]
    return bool(parts) and parts[-1].lower() in _ID_TAILS


def result_kind(result: Any, ok: bool) -> str:
    """结果 → 类别。不保留任何结果内容。

    以 ``ok`` 为准：真实台账里出现过 ``ok=false`` 但 result_digest 写着
    ``"success"`` 的记录（非 JSON 文本不参与失败判定），结果摘要会骗人。
    """
    if not ok:
        return "error"
    if result is None or result == "":
        return "empty"
    text = result if isinstance(result, str) else json.dumps(result, ensure_ascii=False, default=str)
    if len(text) > 8000:
        return "large"
    try:
        parsed = json.loads(text)
    except (TypeError, ValueError):
        return "success"
    if isinstance(parsed, Mapping):
        if str(parsed.get("status", "")).lower() == "error":
            return "error"
        for key in parsed:
            if _is_id_key(key):
                return "success_with_ids"
    return "success"


# --------------------------------------------------------------------------
# 台账 → 形状化输入
# --------------------------------------------------------------------------


def compact_calls(calls: Sequence[Mapping[str, Any]], limit: int = 40) -> list[Mapping[str, Any]]:
    """挑出最值得提炼的调用。

    失败调用全部保留（负样本是经验里最值钱的部分），其余按时间倒序补足到
    limit，最后按原顺序返回给人看/给模型看时才不会时序错乱。
    """
    if limit <= 0:
        return list(calls)
    if len(calls) <= limit:
        return list(calls)
    failed = [call for call in calls if not call.get("ok", True)]
    if len(failed) >= limit:
        return failed[-limit:]
    remaining = limit - len(failed)
    keep_ids = {id(call) for call in failed}
    others = [call for call in calls if id(call) not in keep_ids]
    tail = others[-remaining:]
    tail_ids = {id(call) for call in tail}
    selected = [
        call for call in calls
        if id(call) in keep_ids or id(call) in tail_ids
    ]
    return selected


def sanitize_ledger(ledger: Mapping[str, Any], *, max_calls: int = 40) -> dict[str, Any]:
    """把 ledger.json 压成可以安全交给提炼模型的结构。

    输出里不存在任何参数原值、结果文本、文件引用或对象 ID；路径与实体名
    即使在阶段标题里出现也会被替换成占位符。
    """
    plan_raw = ledger.get("plan") if isinstance(ledger, Mapping) else None
    plan = plan_raw if isinstance(plan_raw, Mapping) else {}
    phases: list[dict[str, Any]] = []
    for item in plan.get("phases") or []:
        if not isinstance(item, Mapping):
            continue
        status = item.get("status")
        phases.append({
            "title": scrub_text(item.get("title"))[:80],
            "status": status if status in PLAN_STATUSES else "pending",
            "has_note": bool(str(item.get("note") or "").strip()),
        })

    calls_raw = ledger.get("calls") if isinstance(ledger, Mapping) else None
    calls = [call for call in (calls_raw or []) if isinstance(call, Mapping)]
    shaped_calls: list[dict[str, Any]] = []
    for call in compact_calls(calls, max_calls):
        tool = safe_tool_name(call.get("tool"))
        if tool == REDACTED:
            # 连工具名都不是合法标识符，这条调用无法描述成步骤，留着只会
            # 让提炼模型编造一个名字出来。
            continue
        shaped = {
            "tool": tool,
            "ok": bool(call.get("ok", True)),
            "phase": scrub_text(call.get("phase"))[:60],
            "result_kind": result_kind(call.get("result_digest"), bool(call.get("ok", True))),
        }
        shaped.update(sanitize_args(call.get("args_digest")))
        shaped_calls.append(shaped)

    return {
        "plan": {
            "title": scrub_text(plan.get("title"))[:120],
            "phases": phases,
        },
        "calls": shaped_calls,
        "totals": {
            "phase_count": len(phases),
            "call_count": len(calls),
            "failed_count": sum(1 for call in calls if not call.get("ok", True)),
        },
    }


# --------------------------------------------------------------------------
# 落盘前检查
# --------------------------------------------------------------------------


def _walk_text(value: Any, field: str, out: list[tuple[str, str]]) -> None:
    if isinstance(value, str):
        out.append((field, value))
        return
    if isinstance(value, Mapping):
        for key, item in value.items():
            name = str(key)
            # 按分段豁免整棵子树，否则 source_version.skill_hash 这种长摘要
            # 会被 long_token 误判成泄漏，把每一条经验都挡在门外。
            if name in TEXT_FIELD_EXEMPT:
                continue
            _walk_text(item, name if not field else "%s.%s" % (field, name), out)
        return
    if isinstance(value, (list, tuple)):
        for index, item in enumerate(value):
            _walk_text(item, "%s[%d]" % (field, index), out)


def _field_leaf(path: str) -> str:
    leaf = path.rsplit(".", 1)[-1]
    return re.sub(r"\[\d+\]$", "", leaf)


def inspect_record(record: Mapping[str, Any]) -> list[tuple[str, str]]:
    """返回 [(字段路径, 模式名)]，用于日志与测试断言。"""
    findings: list[tuple[str, str]] = []
    texts: list[tuple[str, str]] = []
    _walk_text(record, "", texts)
    for path, text in texts:
        leaf = _field_leaf(path)
        if leaf in TEXT_FIELD_EXEMPT:
            continue
        for name in find_sensitive(text):
            findings.append((path, name))
    tool_sequence = record.get("tool_sequence")
    if isinstance(tool_sequence, (list, tuple)):
        for index, name in enumerate(tool_sequence):
            if not isinstance(name, str) or not _TOOL_NAME_RE.match(name):
                findings.append(("tool_sequence[%d]" % index, "invalid_tool_name"))
    return findings


def assert_clean(record: Mapping[str, Any]) -> None:
    """命中敏感模式就抛错，由调用方整条丢弃。

    这里刻意不提供「部分清洗后入库」的路径：一条经验少一个字段仍然是错的经验，
    而带着错误来源的经验比没有经验更危险。
    """
    findings = inspect_record(record)
    if findings:
        raise SensitiveContentError(findings[0][0], [name for _path, name in findings])


def iter_record_texts(record: Mapping[str, Any]) -> Iterable[tuple[str, str]]:
    texts: list[tuple[str, str]] = []
    _walk_text(record, "", texts)
    return texts
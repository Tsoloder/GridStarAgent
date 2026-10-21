"""本地知识库：分块索引、词法检索、参数表与定位回指。

设计取向，每一条都对应一次取舍：

* **不用 embedding**。中文检索走「写入时切 2/3-gram 进冗余列」的老办法（与
  ``experience_memory`` 同一套，直接复用它的函数，不复制第二份）。理由是这份
  资料里被查询的主体是术语、参数名、数值与报错，字面命中比语义近邻更准，而且
  索引确定可复现、可审计。
* **按文档自带的骨架切块**。PDF 有书签就按书签切，没有书签退回编号标题正则，
  再退回段落窗口。切块边界跟着文档结构走，比按字数硬切可检索性高一个量级。
* **存正文副本，同时存定位**。检索要全文，回源要页码。只存指针会让每次召回都
  重新解析 PDF，且偏移会随文件改版静默错位。
* **参数单独一张表**。「增长率默认多少、上限多少」是一次精确查表，丢进文本池
  会被周围长段落淹没。
* **FTS5 不可用就降级 LIKE**，知识库坏了不阻断主任务。
"""
from __future__ import annotations

import hashlib
import json
import re
import sqlite3
import threading
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable, Optional

from experience_memory import build_fts_query, build_search_text, cjk_ngrams
from paths import KB_DB_PATH, KB_DIR, KB_SOURCES_DIR

SCHEMA_VERSION = 2

# 一次会话里可以选一个库，也可以选「全部」或「不使用」。
# 库的物理隔离靠 kb_doc.library_id，检索时一律带上它，不靠调用方自觉。
DEFAULT_LIBRARY_ID = "default"
DEFAULT_LIBRARY_NAME = "默认知识库"
LIBRARY_ALL = "*"
# 文件夹只用来做界面上的收纳与折叠，不参与检索范围；检索范围仍然只有库。
# 文档 folder_id 为空串表示直接放在库根。
FOLDER_ROOT = ""

DOC_STATUS_OK = "ok"
DOC_STATUS_FAILED = "failed"
DOC_STATUS_STALE = "stale"

# 单块目标长度与重叠。块太长时，命中句会被大量无关字稀释，bm25 会把票投给
# 短而密的块（典型是图注）；块太碎又会切断步骤。600 字左右在实测里较均衡。
CHUNK_TARGET = 600
CHUNK_MAX = 950
CHUNK_OVERLAP = 100

SUPPORTED_SUFFIXES = {".pdf", ".txt", ".md", ".docx", ".xlsx", ".doc", ".xls"}

# 这些书签不是正文，索引进库只会稀释召回
_SKIP_TITLES = {"目录", "contents", "封面", "前言"}

_CJK = re.compile(r"[\u4e00-\u9fa5]")

_SCHEMA = """
CREATE TABLE IF NOT EXISTS kb_library (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    note TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT '',
    updated_at TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS kb_folder (
    id TEXT PRIMARY KEY,
    library_id TEXT NOT NULL,
    parent_id TEXT NOT NULL DEFAULT '',
    name TEXT NOT NULL,
    sort_order INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_kb_folder_lib ON kb_folder(library_id, parent_id);

CREATE TABLE IF NOT EXISTS kb_doc (
    id TEXT PRIMARY KEY,
    library_id TEXT NOT NULL DEFAULT '',
    folder_id TEXT NOT NULL DEFAULT '',
    path TEXT NOT NULL,
    name TEXT NOT NULL,
    suffix TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'ok',
    error TEXT NOT NULL DEFAULT '',
    pages INTEGER NOT NULL DEFAULT 0,
    chars INTEGER NOT NULL DEFAULT 0,
    chunk_count INTEGER NOT NULL DEFAULT 0,
    mtime REAL NOT NULL DEFAULT 0,
    size INTEGER NOT NULL DEFAULT 0,
    content_hash TEXT NOT NULL DEFAULT '',
    indexed_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_kb_doc_status ON kb_doc(status);

CREATE TABLE IF NOT EXISTS kb_chunk (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    doc_id TEXT NOT NULL,
    ordinal INTEGER NOT NULL DEFAULT 0,
    heading_path TEXT NOT NULL DEFAULT '',
    section TEXT NOT NULL DEFAULT '',
    page_start INTEGER NOT NULL DEFAULT 0,
    page_end INTEGER NOT NULL DEFAULT 0,
    char_start INTEGER NOT NULL DEFAULT 0,
    char_end INTEGER NOT NULL DEFAULT 0,
    kind TEXT NOT NULL DEFAULT 'para',
    text TEXT NOT NULL DEFAULT '',
    search_text TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_kb_chunk_doc ON kb_chunk(doc_id, ordinal);

CREATE TABLE IF NOT EXISTS kb_param (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    domain TEXT NOT NULL DEFAULT '',
    module TEXT NOT NULL DEFAULT '',
    default_value TEXT NOT NULL DEFAULT '',
    min_value REAL,
    max_value REAL,
    unit TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    doc_id TEXT NOT NULL DEFAULT '',
    heading_path TEXT NOT NULL DEFAULT '',
    page INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_kb_param_name ON kb_param(name);
CREATE INDEX IF NOT EXISTS idx_kb_param_doc ON kb_param(doc_id);
"""

_FTS_SCHEMA = """
CREATE VIRTUAL TABLE IF NOT EXISTS kb_chunk_fts USING fts5(
    heading_path, search_text, content='kb_chunk', content_rowid='id',
    tokenize='unicode61'
);
CREATE TRIGGER IF NOT EXISTS kb_chunk_ai AFTER INSERT ON kb_chunk BEGIN
    INSERT INTO kb_chunk_fts(rowid, heading_path, search_text)
    VALUES (new.id, new.heading_path, new.search_text);
END;
CREATE TRIGGER IF NOT EXISTS kb_chunk_ad AFTER DELETE ON kb_chunk BEGIN
    INSERT INTO kb_chunk_fts(kb_chunk_fts, rowid, heading_path, search_text)
    VALUES ('delete', old.id, old.heading_path, old.search_text);
END;
CREATE TRIGGER IF NOT EXISTS kb_chunk_au AFTER UPDATE ON kb_chunk BEGIN
    INSERT INTO kb_chunk_fts(kb_chunk_fts, rowid, heading_path, search_text)
    VALUES ('delete', old.id, old.heading_path, old.search_text);
    INSERT INTO kb_chunk_fts(rowid, heading_path, search_text)
    VALUES (new.id, new.heading_path, new.search_text);
END;
"""

# 参数名：中文/英文短词，后面跟冒号（中英文冒号都认）
_PARAM_RE = re.compile(
    r"(?:^|[。；;\n])\s*"
    r"([\u4e00-\u9fa5A-Za-z][\u4e00-\u9fa5A-Za-z0-9_\-/ ]{1,11}?)"
    r"\s*[：:]\s*"
    r"([^。；\n]{6,220})"
)
_PARAM_NOISE = {
    "注意", "说明", "提示", "适用", "例如", "方法", "操作方式", "功能描述",
    "图", "表", "步骤", "结果", "参考", "示意", "如图", "如下图", "如下",
    "学习目标", "限制条件", "输入条件", "参数", "关联", "复制", "创建",
    "应用", "预览", "删除", "取消", "保存", "选择", "设置", "勾选",
    "单击", "点击", "双击", "确定", "关闭", "打开", "下一步", "上一步",
    "自动完成", "全选", "重新装配", "下一组面", "操作步骤", "功能", "按钮",
    "菜单", "选项卡", "快捷栏", "状态", "任务", "问题", "原因", "处理",
}

# 参数名常见的尾词：命中才算「像个参数」，只靠冒号判定会把按钮名也收进来
_PARAM_SUFFIXES = (
    "高度", "厚度", "层数", "点数", "因子", "率", "比", "公差", "角度",
    "距离", "长度", "系数", "类型", "模式", "方式", "坐标", "阈值", "值",
    "间隔", "间隙", "密度", "个数", "次数", "精度", "半径", "面数", "尺寸",
)
# 描述里的「这是参数」证据：有单位、有数值、有默认值、有取值/范围字样
_PARAM_EVIDENCE = re.compile(
    r"默认|缺省|取值|范围|阈值|上限|下限|单位|不超过|不小于|支持|决定|表示|用于",
)
_NUMERIC = re.compile(r"\d")

_RANGE_RE = re.compile(
    r"(\d+(?:\.\d+)?)\s*(?:-|~|～|—|－|–|\u2010|\u2011|\u2012|\u2013|\u2014|\u2015|到|至)\s*(\d+(?:\.\d+)?)"
)
_BRACKET_RANGE_RE = re.compile(
    r"[\(（\[［]\s*(\d+(?:\.\d+)?)\s*[,，]\s*(\d+(?:\.\d+)?)\s*[\)）\]］]"
)
_UPPER_RE = re.compile(
    r"(?:不超过|不大于|不能超过|不应超过|上限为|最大(?:值)?(?:为)?)\s*(\d+(?:\.\d+)?)"
)
_LOWER_RE = re.compile(
    r"(?:不小于|不低于|不能低于|下限为|最小(?:值)?(?:为)?)\s*(\d+(?:\.\d+)?)"
)
_UNIT_RE = re.compile(r"^\(?\s*(mm|cm|m|μm|um|°|度|%|s|ms|Pa|kg|无量纲)")
# 默认值只在“默认值/默认为/默认取”这类明确句式后取，否则“默认公差”会把名词当值
_DEFAULT_RE = re.compile(
    r"(?:默认值|缺省值)\s*(?:为|是|取)?\s*[：:]?\s*([A-Za-z0-9_.+\-]{1,16}|[\u4e00-\u9fa5]{1,4})"
    r"|默认(?:为|是|取)\s*[：:]?\s*([A-Za-z0-9_.+\-]{1,16}|[\u4e00-\u9fa5]{1,4})"
)

_LOCK = threading.RLock()

# 提问里的口语填充词。它们会切成一堆无意义 gram（"怎么确定" -> "怎么"/"么确"/
# "确定"），把真正要查的术语选没。检索前先剥掉，只留名词性内容。
_QUERY_NOISE = (
    "请问", "想问", "告诉我", "帮我", "查一下", "看一下",
    "怎么样", "怎么", "如何", "为什么", "是什么", "什么意思", "啥意思",
    "哪些", "哪个", "在哪里", "在哪", "多少", "可以", "能否", "是否",
    "一般", "应该", "需要", "有没有", "支持", "怎么办", "吗", "呢", "的",
)


def clean_query(query: str) -> str:
    """剥掉疑问填充词，保留名词性成分。全剥空时退回原串。"""
    text = (query or "").strip()
    if not text:
        return ""
    stripped = text
    for word in _QUERY_NOISE:
        stripped = stripped.replace(word, " ")
    stripped = re.sub(r"[\s\u3000]+，?", " ", stripped).strip(" 　，。？！?!、")
    # 剥得太狠（只剩一两个字）就不要剥了，否则相当于没查
    if len(re.sub(r"\s", "", stripped)) < 2:
        return text
    return stripped


class KBUnavailable(RuntimeError):
    """索引库打不开或写入失败时抛出，调用方应降级而不是 500。"""


# --------------------------------------------------------------------------
# 连接与建表
# --------------------------------------------------------------------------


class _Store:
    def __init__(self, db_path: Path) -> None:
        self.db_path = Path(db_path)
        self._fts: Optional[bool] = None
        self._ready = False

    def connect(self) -> sqlite3.Connection:
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        conn = sqlite3.connect(str(self.db_path), timeout=15.0)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute("PRAGMA synchronous = NORMAL")
        self._ensure(conn)
        return conn

    def _ensure(self, conn: sqlite3.Connection) -> None:
        if self._ready:
            return
        conn.executescript(_SCHEMA)
        self._ready = True
        self._migrate(conn)
        if self._fts is None:
            try:
                conn.executescript(_FTS_SCHEMA)
                self._fts = True
            except sqlite3.Error:
                # 老版本 SQLite 没编进 FTS5：退回 LIKE 扫描，功能降级但不报错
                self._fts = False

    def _migrate(self, conn: sqlite3.Connection) -> None:
        """把单库时代的数据接进多库结构，并保证至少有一个库。

        ALTER TABLE 只能加列，不能加 NOT NULL 带默认值的旧行；所以先把列加成
        空串，再把旧文档全部归给默认库，不然升级后旧资料会从所有库里消失。
        """
        columns = {row[1] for row in conn.execute("PRAGMA table_info(kb_doc)")}
        if "library_id" not in columns:
            conn.execute("ALTER TABLE kb_doc ADD COLUMN library_id TEXT NOT NULL DEFAULT ''")
        if "folder_id" not in columns:
            conn.execute("ALTER TABLE kb_doc ADD COLUMN folder_id TEXT NOT NULL DEFAULT ''")
        # 索引建在打列之后：旧库先执行建索引会直接报 no such column
        conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_kb_doc_library ON kb_doc(library_id, status)"
        )
        conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_kb_doc_folder ON kb_doc(folder_id)"
        )
        conn.execute(
            "UPDATE kb_doc SET library_id = ? WHERE library_id = ''", (DEFAULT_LIBRARY_ID,)
        )
        total = conn.execute("SELECT COUNT(*) AS n FROM kb_library").fetchone()["n"]
        if not total:
            now = datetime.now().isoformat(timespec="seconds")
            conn.execute(
                "INSERT INTO kb_library (id, name, note, created_at, updated_at)"
                " VALUES (?,?,?,?,?)",
                (DEFAULT_LIBRARY_ID, DEFAULT_LIBRARY_NAME, "", now, now),
            )
        conn.commit()

    @property
    def fts(self) -> bool:
        if self._fts is None:
            try:
                with _LOCK:
                    conn = self.connect()
                    conn.close()
            except (sqlite3.Error, OSError):
                return False
        return bool(self._fts)

    def rebuild_fts(self, conn: sqlite3.Connection) -> bool:
        if not self._fts:
            return False
        try:
            conn.execute("INSERT INTO kb_chunk_fts(kb_chunk_fts) VALUES('rebuild')")
            return True
        except sqlite3.Error:
            self._fts = False
            return False


_STORE = _Store(KB_DB_PATH)


def configure(db_path: str | Path) -> None:
    """把索引指到另一个库文件。测试与将来多知识库（按领域拆库）共用这个入口。"""
    global _STORE
    with _LOCK:
        _STORE = _Store(Path(db_path))


def fts_available() -> bool:
    return _STORE.fts


# --------------------------------------------------------------------------
# 文本切分
# --------------------------------------------------------------------------


def _norm(text: str) -> str:
    return re.sub(r"\s+", "", text or "")


def _clean_block(text: str) -> str:
    lines = [line.rstrip() for line in (text or "").splitlines()]
    out: list[str] = []
    blank = False
    for line in lines:
        if not line.strip():
            if not blank and out:
                out.append("")
            blank = True
            continue
        blank = False
        out.append(line.strip())
    return "\n".join(out).strip()


def _split_long(text: str) -> list[str]:
    """把超长段落切成带重叠的块。段落优先，段落本身过长就按句号切。"""
    if len(text) <= CHUNK_MAX:
        return [text]
    paragraphs = [p for p in re.split(r"\n+", text) if p.strip()]
    pieces: list[str] = []
    buf = ""
    for para in paragraphs:
        if len(para) > CHUNK_MAX:
            sentences = re.split(r"(?<=[。！？；])", para)
        else:
            sentences = [para]
        for sentence in sentences:
            if not sentence:
                continue
            if len(buf) + len(sentence) + 1 > CHUNK_TARGET and buf:
                pieces.append(buf.strip())
                buf = buf[-CHUNK_OVERLAP:] if CHUNK_OVERLAP else ""
            buf += sentence + "\n"
    if buf.strip():
        pieces.append(buf.strip())
    return [p for p in pieces if p]


def _classify(text: str) -> str:
    head = text[:80]
    if re.match(r"^注意\s*[：:]", head) or head.startswith("说明"):
        return "note"
    if text.count("|") >= 3 or text.count("｜") >= 3:
        return "table"
    if re.search(r"[=＝]\s*[A-Za-zα-ωΑ-Ω]|\^|√|∑", text):
        return "formula"
    return "para"


def _percentile_page(offsets: list[tuple[int, int]], char_index: int) -> int:
    page = 1
    for start, page_no in offsets:
        if start <= char_index:
            page = page_no
        else:
            break
    return page


# --------------------------------------------------------------------------
# PDF：按书签切节
# --------------------------------------------------------------------------


def _pdf_pages(path: Path) -> tuple[list[str], list[tuple[int, int]], list[tuple[int, str, int]]]:
    """返回 (每页文本, 偏移→页码, 书签列表)。

    书签用 (文档顺序, 标题, 页码)。页码从 1 开始。
    """
    from pypdf import PdfReader

    reader = PdfReader(str(path))
    page_texts: list[str] = []
    offsets: list[tuple[int, int]] = []
    cursor = 0
    for index, page in enumerate(reader.pages):
        try:
            text = page.extract_text() or ""
        except Exception:  # noqa: BLE001 - 单页解析失败不该让整份文档失败
            text = ""
        page_texts.append(text)
        offsets.append((cursor, index + 1))
        cursor += len(text) + 1

    bookmarks: list[tuple[int, str, int]] = []

    def walk(items, depth: int = 0) -> None:
        for item in items:
            if isinstance(item, list):
                walk(item, depth + 1)
                continue
            try:
                title = str(item.title).strip()
            except Exception:  # noqa: BLE001
                continue
            try:
                page_no = reader.get_destination_page_number(item) + 1
            except Exception:  # noqa: BLE001
                page_no = 1
            if title:
                bookmarks.append((depth, title, page_no))

    try:
        walk(reader.outline)
    except Exception:  # noqa: BLE001 - 书签坏了就当没有，退回编号标题
        bookmarks = []
    return page_texts, offsets, bookmarks


def _pdf_sections(
    page_texts: list[str], offsets: list[tuple[int, int]], bookmarks: list[tuple[int, str, int]]
) -> list[dict[str, Any]]:
    """把「书签 → 正文区间」解成节。找不到标题出现位置就退回该页页首。"""
    full = "\n".join(page_texts)
    if not full.strip():
        return []

    norm_chars: list[str] = []
    norm_pos: list[int] = []
    for index, char in enumerate(full):
        if not char.isspace():
            norm_chars.append(char)
            norm_pos.append(index)
    norm = "".join(norm_chars)

    def norm_offset_of(raw: int) -> int:
        lo, hi = 0, len(norm_pos)
        while lo < hi:
            mid = (lo + hi) // 2
            if norm_pos[mid] < raw:
                lo = mid + 1
            else:
                hi = mid
        return lo

    page_start_raw = {page_no: start for start, page_no in offsets}

    ordered = [b for b in bookmarks if _norm(b[1]).lower() not in _SKIP_TITLES]
    starts: list[int] = []
    for _, title, page_no in ordered:
        raw_start = page_start_raw.get(page_no, 0)
        from_norm = norm_offset_of(raw_start)
        if starts:
            # 同一页上排在后面的书签，从上一节起点之后再找，避免匹配到前一节
            from_norm = max(from_norm, starts[-1] + 1)
        candidates = [_norm(title)]
        stripped = re.sub(r"^[\d.]+\s*", "", title).strip()
        if stripped and stripped != title:
            candidates.append(_norm(stripped))
        found = -1
        for candidate in candidates:
            if not candidate:
                continue
            hit = norm.find(candidate, from_norm)
            if hit >= 0:
                found = hit
                break
        starts.append(found if found >= 0 else from_norm)

    sections: list[dict[str, Any]] = []
    stack: list[tuple[int, str]] = []
    for index, (depth, title, page_no) in enumerate(ordered):
        start_norm = starts[index]
        end_norm = starts[index + 1] if index + 1 < len(starts) else len(norm)
        if end_norm <= start_norm:
            end_norm = min(len(norm), start_norm + 1)
        raw_start = norm_pos[start_norm] if start_norm < len(norm_pos) else len(full)
        raw_end = norm_pos[end_norm] if end_norm < len(norm_pos) else len(full)
        while stack and stack[-1][0] >= depth:
            stack.pop()
        stack.append((depth, title))
        heading_path = " / ".join(item[1] for item in stack)
        body = _clean_block(full[raw_start:raw_end])
        if not body:
            continue
        sections.append(
            {
                "title": title,
                "heading_path": heading_path,
                "text": body,
                "char_start": raw_start,
                "char_end": raw_end,
                "page_start": _percentile_page(offsets, raw_start),
                "page_end": _percentile_page(offsets, max(raw_start, raw_end - 1)),
            }
        )
    if not sections:
        body = _clean_block(full)
        if body:
            sections.append(
                {
                    "title": path_stem_placeholder(),
                    "heading_path": "",
                    "text": body,
                    "char_start": 0,
                    "char_end": len(full),
                    "page_start": 1,
                    "page_end": len(page_texts),
                }
            )
    return sections


def path_stem_placeholder() -> str:
    return "全文"


# --------------------------------------------------------------------------
# 纯文本 / Markdown：按标题切
# --------------------------------------------------------------------------

_MD_HEADING = re.compile(r"^(#{1,6})\s+(.+)$", re.M)
_NUM_HEADING = re.compile(r"^(\d+(?:\.\d+){0,4})\s+(\S[^\n]{0,60})$", re.M)


def _text_sections(text: str) -> list[dict[str, Any]]:
    matches = [(m.start(), len(m.group(1)), m.group(2).strip()) for m in _MD_HEADING.finditer(text)]
    if not matches:
        matches = [(m.start(), m.group(1).count(".") + 1, "%s %s" % (m.group(1), m.group(2).strip()))
                   for m in _NUM_HEADING.finditer(text)]
    if not matches:
        body = _clean_block(text)
        return [{"title": "全文", "heading_path": "", "text": body, "char_start": 0,
                 "char_end": len(text), "page_start": 1, "page_end": 1}] if body else []
    matches.append((len(text), 0, ""))
    sections: list[dict[str, Any]] = []
    stack: list[tuple[int, str]] = []
    for index in range(len(matches) - 1):
        start, depth, title = matches[index]
        end = matches[index + 1][0]
        while stack and stack[-1][0] >= depth:
            stack.pop()
        stack.append((depth, title))
        body = _clean_block(text[start:end])
        if not body:
            continue
        sections.append(
            {
                "title": title,
                "heading_path": " / ".join(item[1] for item in stack),
                "text": body,
                "char_start": start,
                "char_end": end,
                "page_start": 1,
                "page_end": 1,
            }
        )
    return sections


def _plain_sections(text: str) -> list[dict[str, Any]]:
    body = _clean_block(text)
    if not body:
        return []
    return [
        {
            "title": "全文",
            "heading_path": "",
            "text": body,
            "char_start": 0,
            "char_end": len(text),
            "page_start": 1,
            "page_end": 1,
        }
    ]


def _normalize_sections(sections: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """只做深拷贝，不合并。

    曾经把过短的节并进前一节，结果把「设置边界」这种只有一句话的小节连同它
    的标题一起吃掉了，检索再也回不到那一节。短块被 bm25 抬高的问题放到排序
    层用长度因子压，不动切块结构。
    """
    return [dict(section) for section in sections]


# --------------------------------------------------------------------------
# 参数抽取
# --------------------------------------------------------------------------


def _extract_params(
    text: str, doc_id: str, heading_path: str, page: int, module: str
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    for match in _PARAM_RE.finditer(text):
        name = match.group(1).strip(" 　\t")
        desc = match.group(2).strip(" 　\t")
        if not name or name in _PARAM_NOISE:
            continue
        if len(name) < 2 or name.startswith(("图", "表", "第", "如")):
            continue
        if any(noise in name for noise in ("如下", "所示", "如图", "学习")):
            continue
        # 名称得像参数名，或者描述里有参数证据；两者都不占就当按钮名丢掉
        looks_like_param = name.endswith(_PARAM_SUFFIXES)
        if not looks_like_param and not _PARAM_EVIDENCE.search(desc):
            continue
        key = (name, desc[:24])
        if key in seen:
            continue
        seen.add(key)

        # 数值常在下一句（"增长率：是先...之比。通常增长率在 1.1-1.3 之间"），
        # 所以同一段落的后续文字也纳入搜范围，否则只有定义没有范围。
        tail_start = match.end()
        tail_end = tail_start + 160
        tail = text[tail_start:tail_end]
        blank = tail.find("\n\n")
        if blank >= 0:
            tail = tail[:blank]
        scope_text = desc + " " + tail

        lower = upper = None
        hit = _RANGE_RE.search(scope_text) or _BRACKET_RANGE_RE.search(scope_text)
        if hit:
            try:
                lower = float(hit.group(1))
                upper = float(hit.group(2))
            except ValueError:
                lower = upper = None
        if lower is None:
            found = _LOWER_RE.search(scope_text)
            if found:
                lower = float(found.group(1))
        if upper is None:
            found = _UPPER_RE.search(scope_text)
            if found:
                upper = float(found.group(1))

        default = ""
        found = _DEFAULT_RE.search(scope_text)
        if found:
            default = (found.group(1) or found.group(2) or "").strip()
        unit = ""
        found = _UNIT_RE.search(desc)
        if found:
            unit = found.group(1)

        rows.append(
            {
                "name": name,
                "domain": _guess_domain(heading_path, desc),
                "module": module,
                "default_value": default,
                "min_value": lower,
                "max_value": upper,
                "unit": unit,
                "description": desc,
                "doc_id": doc_id,
                "heading_path": heading_path,
                "page": page,
            }
        )
        if len(rows) >= 400:  # 单节抽取上限，防止异常文本把表撑爆
            break
    return rows


_DOMAIN_RULES = (
    ("网格", "mesh"),
    ("附面层", "mesh"),
    ("边界", "solver"),
    ("数模", "geometry"),
    ("几何", "geometry"),
    ("视图", "view"),
    ("系统", "system"),
    ("脚本", "script"),
)


def _guess_domain(heading_path: str, desc: str) -> str:
    haystack = "%s %s" % (heading_path, desc[:60])
    for keyword, domain in _DOMAIN_RULES:
        if keyword in haystack:
            return domain
    return "general"


# --------------------------------------------------------------------------
# 知识库（库）管理
# --------------------------------------------------------------------------


def _new_library_id(name: str) -> str:
    """库 id 用名字拼音不可靠，直接用内容哈希，保证不受中文名影响。"""
    seed = "%s|%s" % (name or "kb", datetime.now().isoformat(timespec="microseconds"))
    return "lib-" + hashlib.sha1(seed.encode("utf-8")).hexdigest()[:10]


def _ensure_library(conn: sqlite3.Connection, library_id: str) -> str:
    """确认库存在，返回实际使用的库 id。空 id 落到默认库。"""
    target = (library_id or "").strip() or DEFAULT_LIBRARY_ID
    row = conn.execute("SELECT id FROM kb_library WHERE id = ?", (target,)).fetchone()
    if row is not None:
        return target
    if target == DEFAULT_LIBRARY_ID:
        now = datetime.now().isoformat(timespec="seconds")
        conn.execute(
            "INSERT OR IGNORE INTO kb_library (id, name, note, created_at, updated_at)"
            " VALUES (?,?,?,?,?)",
            (DEFAULT_LIBRARY_ID, DEFAULT_LIBRARY_NAME, "", now, now),
        )
        return target
    raise KBUnavailable("知识库不存在：%s" % target)


def _library_counts(conn: sqlite3.Connection) -> dict[str, dict[str, int]]:
    counts: dict[str, dict[str, int]] = {}
    for row in conn.execute(
        "SELECT library_id, COUNT(*) AS docs, SUM(chunk_count) AS chunks"
        " FROM kb_doc GROUP BY library_id"
    ).fetchall():
        counts[row["library_id"]] = {
            "documents": int(row["docs"] or 0),
            "chunks": int(row["chunks"] or 0),
        }
    for row in conn.execute(
        "SELECT d.library_id AS lib, COUNT(*) AS params FROM kb_param p"
        " JOIN kb_doc d ON d.id = p.doc_id GROUP BY d.library_id"
    ).fetchall():
        counts.setdefault(row["lib"], {}).setdefault("documents", 0)
        counts[row["lib"]]["params"] = int(row["params"] or 0)
    return counts


def list_libraries() -> list[dict[str, Any]]:
    """库列表 + 每个库的体量。前端下拉框与设置页都用它。"""
    with _LOCK:
        conn = _STORE.connect()
        try:
            rows = conn.execute(
                "SELECT * FROM kb_library ORDER BY created_at, id"
            ).fetchall()
            counts = _library_counts(conn)
        finally:
            conn.close()
    items = []
    for row in rows:
        stats = counts.get(row["id"], {})
        items.append(
            {
                "id": row["id"],
                "name": row["name"],
                "note": row["note"],
                "documents": stats.get("documents", 0),
                "chunks": stats.get("chunks", 0),
                "params": stats.get("params", 0),
                "created_at": row["created_at"],
                "updated_at": row["updated_at"],
            }
        )
    return items


def get_library(library_id: str) -> Optional[dict[str, Any]]:
    for item in list_libraries():
        if item["id"] == library_id:
            return item
    return None


def create_library(name: str, note: str = "") -> dict[str, Any]:
    label = (name or "").strip()
    if not label:
        raise KBUnavailable("知识库名称不能为空")
    if len(label) > 40:
        raise KBUnavailable("知识库名称过长（上限 40 字）")
    now = datetime.now().isoformat(timespec="seconds")
    with _LOCK:
        conn = _STORE.connect()
        try:
            existing = conn.execute(
                "SELECT id FROM kb_library WHERE name = ?", (label,)
            ).fetchone()
            if existing is not None:
                raise KBUnavailable("已存在同名知识库：%s" % label)
            library_id = _new_library_id(label)
            conn.execute(
                "INSERT INTO kb_library (id, name, note, created_at, updated_at)"
                " VALUES (?,?,?,?,?)",
                (library_id, label, (note or "").strip(), now, now),
            )
            conn.commit()
        finally:
            conn.close()
    return {"id": library_id, "name": label, "note": note or ""}


def rename_library(library_id: str, name: str = "", note: Optional[str] = None) -> dict[str, Any]:
    now = datetime.now().isoformat(timespec="seconds")
    with _LOCK:
        conn = _STORE.connect()
        try:
            row = conn.execute(
                "SELECT * FROM kb_library WHERE id = ?", (library_id,)
            ).fetchone()
            if row is None:
                raise KBUnavailable("知识库不存在")
            label = (name or "").strip() or row["name"]
            if len(label) > 40:
                raise KBUnavailable("知识库名称过长（上限 40 字）")
            clash = conn.execute(
                "SELECT id FROM kb_library WHERE name = ? AND id <> ?", (label, library_id)
            ).fetchone()
            if clash is not None:
                raise KBUnavailable("已存在同名知识库：%s" % label)
            conn.execute(
                "UPDATE kb_library SET name = ?, note = ?, updated_at = ? WHERE id = ?",
                (label, row["note"] if note is None else (note or "").strip(), now, library_id),
            )
            conn.commit()
        finally:
            conn.close()
    return {"id": library_id, "name": label}


def delete_library(library_id: str) -> dict[str, Any]:
    """删库连同库里的文档与分块一起删。不会动 sources 里的原文件。"""
    with _LOCK:
        conn = _STORE.connect()
        try:
            row = conn.execute(
                "SELECT name FROM kb_library WHERE id = ?", (library_id,)
            ).fetchone()
            if row is None:
                raise KBUnavailable("知识库不存在")
            doc_ids = [
                item["id"] for item in conn.execute(
                    "SELECT id FROM kb_doc WHERE library_id = ?", (library_id,)
                ).fetchall()
            ]
            for doc_id in doc_ids:
                conn.execute("DELETE FROM kb_chunk WHERE doc_id = ?", (doc_id,))
                conn.execute("DELETE FROM kb_param WHERE doc_id = ?", (doc_id,))
            conn.execute("DELETE FROM kb_doc WHERE library_id = ?", (library_id,))
            conn.execute("DELETE FROM kb_library WHERE id = ?", (library_id,))
            # 库删空了就补一个默认库：界面上「一个库都没有」比「空库」更难处理
            remaining = conn.execute("SELECT COUNT(*) AS n FROM kb_library").fetchone()["n"]
            if not remaining:
                now = datetime.now().isoformat(timespec="seconds")
                conn.execute(
                    "INSERT INTO kb_library (id, name, note, created_at, updated_at)"
                    " VALUES (?,?,?,?,?)",
                    (DEFAULT_LIBRARY_ID, DEFAULT_LIBRARY_NAME, "", now, now),
                )
            conn.commit()
        finally:
            conn.close()
    return {"deleted": library_id, "documents": len(doc_ids), "name": row["name"]}


# --------------------------------------------------------------------------
# 文件夹（只做归档，不影响检索）
# --------------------------------------------------------------------------


def _new_folder_id() -> str:
    seed = datetime.now().isoformat(timespec="microseconds")
    return "dir-" + hashlib.sha1(seed.encode("utf-8")).hexdigest()[:10]


def _folder_rows(conn: sqlite3.Connection, library_id: str) -> list[sqlite3.Row]:
    return conn.execute(
        "SELECT * FROM kb_folder WHERE library_id = ? ORDER BY sort_order, name",
        (library_id,),
    ).fetchall()


def _folder_doc_counts(conn: sqlite3.Connection, library_id: str) -> dict[str, int]:
    counts: dict[str, int] = {}
    for row in conn.execute(
        "SELECT folder_id, COUNT(*) AS n FROM kb_doc WHERE library_id = ? GROUP BY folder_id",
        (library_id,),
    ).fetchall():
        counts[row["folder_id"] or FOLDER_ROOT] = int(row["n"] or 0)
    return counts


def list_folders(library_id: str = DEFAULT_LIBRARY_ID) -> list[dict[str, Any]]:
    """扁平列表，带直属文档数与递归总数。前端建树自己拼。"""
    target = (library_id or DEFAULT_LIBRARY_ID).strip()
    with _LOCK:
        conn = _STORE.connect()
        try:
            rows = _folder_rows(conn, target)
            counts = _folder_doc_counts(conn, target)
        finally:
            conn.close()

    by_parent: dict[str, list[str]] = {}
    for row in rows:
        by_parent.setdefault(row["parent_id"] or FOLDER_ROOT, []).append(row["id"])

    def total(folder_id: str) -> int:
        value = counts.get(folder_id, 0)
        for child in by_parent.get(folder_id, []):
            value += total(child)
        return value

    return [
        {
            "id": row["id"],
            "library_id": row["library_id"],
            "parent_id": row["parent_id"],
            "name": row["name"],
            "documents": counts.get(row["id"], 0),
            "total": total(row["id"]),
        }
        for row in rows
    ]


def folder_tree(library_id: str = DEFAULT_LIBRARY_ID) -> dict[str, Any]:
    """一棵树，根节点是库根（id 为空串）。前端的折叠树直接吃这个结构。"""
    target = (library_id or DEFAULT_LIBRARY_ID).strip()
    folders = list_folders(target)
    with _LOCK:
        conn = _STORE.connect()
        try:
            root_docs = int(conn.execute(
                "SELECT COUNT(*) AS n FROM kb_doc WHERE library_id = ? AND (folder_id = ''"
                " OR folder_id IS NULL)",
                (target,),
            ).fetchone()["n"] or 0)
        finally:
            conn.close()

    children: dict[str, list[dict[str, Any]]] = {}
    for item in folders:
        children.setdefault(item["parent_id"] or FOLDER_ROOT, []).append(item)

    def build(node_id: str, depth: int) -> dict[str, Any]:
        kids = [build(item["id"], depth + 1) for item in children.get(node_id, [])]
        if node_id == FOLDER_ROOT:
            return {
                "id": "",
                "name": "库根",
                "depth": 0,
                "documents": root_docs,
                "total": root_docs + sum(kid["total"] for kid in kids),
                "children": kids,
            }
        info = next(item for item in folders if item["id"] == node_id)
        return {
            "id": node_id,
            "name": info["name"],
            "depth": depth,
            "documents": info["documents"],
            "total": info["total"],
            "children": kids,
        }

    return build(FOLDER_ROOT, 0)


def _folder_path_map(conn: sqlite3.Connection, library_id: str) -> dict[str, str]:
    rows = _folder_rows(conn, library_id)
    by_id = {row["id"]: row for row in rows}
    cache: dict[str, str] = {}

    def walk(folder_id: str) -> str:
        if folder_id in cache:
            return cache[folder_id]
        row = by_id.get(folder_id)
        if row is None:
            cache[folder_id] = ""
            return ""
        parent = row["parent_id"] or FOLDER_ROOT
        prefix = walk(parent) if parent else ""
        value = (prefix + " / " + row["name"]) if prefix else row["name"]
        cache[folder_id] = value
        return value

    for folder_id in by_id:
        walk(folder_id)
    cache[FOLDER_ROOT] = ""
    return cache


def folder_path(folder_id: str) -> str:
    """把文件夹 id 换成「结构模块 / 表面网格」这样的可读路径。"""
    if not folder_id:
        return ""
    with _LOCK:
        conn = _STORE.connect()
        try:
            row = conn.execute(
                "SELECT library_id FROM kb_folder WHERE id = ?", (folder_id,)
            ).fetchone()
            if row is None:
                return ""
            return _folder_path_map(conn, row["library_id"]).get(folder_id, "")
        finally:
            conn.close()


def _check_folder(conn: sqlite3.Connection, folder_id: str, library_id: str) -> None:
    if not folder_id:
        return
    row = conn.execute(
        "SELECT library_id FROM kb_folder WHERE id = ?", (folder_id,)
    ).fetchone()
    if row is None or row["library_id"] != library_id:
        raise KBUnavailable("文件夹不存在：%s" % folder_id)


def _safe_folder(conn: sqlite3.Connection, folder_id: str, library_id: str) -> str:
    """健壮版文件夹校验：指向已删文件夹时落回库根，而不是把资料写进一个看不见的位置。

    前端的归档位置是它自己记的状态，库或文件夹被删之后会短暂过期；导入不应
    因为一个过期的下拉选择而失败，所以这里就地降级。
    """
    target = (folder_id or "").strip()
    if not target:
        return FOLDER_ROOT
    row = conn.execute(
        "SELECT library_id FROM kb_folder WHERE id = ?", (target,)
    ).fetchone()
    if row is None or row["library_id"] != library_id:
        return FOLDER_ROOT
    return target


def _same_level_name_taken(
    conn: sqlite3.Connection, library_id: str, parent_id: str, name: str, exclude: str = ""
) -> bool:
    row = conn.execute(
        "SELECT id FROM kb_folder WHERE library_id = ? AND parent_id = ? AND name = ? AND id <> ?",
        (library_id, parent_id or FOLDER_ROOT, name, exclude),
    ).fetchone()
    return row is not None


def create_folder(
    library_id: str, name: str, parent_id: str = FOLDER_ROOT
) -> dict[str, Any]:
    label = (name or "").strip()
    if not label:
        raise KBUnavailable("文件夹名称不能为空")
    if len(label) > 60:
        raise KBUnavailable("文件夹名称过长（上限 60 字）")
    target = (library_id or DEFAULT_LIBRARY_ID).strip()
    with _LOCK:
        conn = _STORE.connect()
        try:
            _ensure_library(conn, target)
            _check_folder(conn, parent_id or "", target)
            if _same_level_name_taken(conn, target, parent_id, label):
                raise KBUnavailable("同级已有同名文件夹：%s" % label)
            folder_id = _new_folder_id()
            conn.execute(
                "INSERT INTO kb_folder (id, library_id, parent_id, name, sort_order, created_at)"
                " VALUES (?,?,?,?,?,?)",
                (folder_id, target, parent_id or FOLDER_ROOT, label, 0,
                 datetime.now().isoformat(timespec="seconds")),
            )
            conn.commit()
        finally:
            conn.close()
    return {"id": folder_id, "name": label, "parent_id": parent_id or FOLDER_ROOT}


def rename_folder(folder_id: str, name: str) -> dict[str, Any]:
    label = (name or "").strip()
    if not label:
        raise KBUnavailable("文件夹名称不能为空")
    with _LOCK:
        conn = _STORE.connect()
        try:
            row = conn.execute(
                "SELECT * FROM kb_folder WHERE id = ?", (folder_id,)
            ).fetchone()
            if row is None:
                raise KBUnavailable("文件夹不存在")
            if _same_level_name_taken(
                conn, row["library_id"], row["parent_id"], label, exclude=folder_id
            ):
                raise KBUnavailable("同级已有同名文件夹：%s" % label)
            conn.execute("UPDATE kb_folder SET name = ? WHERE id = ?", (label, folder_id))
            conn.commit()
        finally:
            conn.close()
    return {"id": folder_id, "name": label}


def delete_folder(folder_id: str) -> dict[str, Any]:
    """删文件夹不删资料：里面的文档与子文件夹全部上移到上一层。"""
    with _LOCK:
        conn = _STORE.connect()
        try:
            row = conn.execute(
                "SELECT * FROM kb_folder WHERE id = ?", (folder_id,)
            ).fetchone()
            if row is None:
                raise KBUnavailable("文件夹不存在")
            parent = row["parent_id"] or FOLDER_ROOT
            moved_docs = conn.execute(
                "UPDATE kb_doc SET folder_id = ? WHERE folder_id = ?", (parent, folder_id)
            ).rowcount
            moved_folders = conn.execute(
                "UPDATE kb_folder SET parent_id = ? WHERE parent_id = ?", (parent, folder_id)
            ).rowcount
            conn.execute("DELETE FROM kb_folder WHERE id = ?", (folder_id,))
            conn.commit()
        finally:
            conn.close()
    return {
        "deleted": folder_id,
        "name": row["name"],
        "moved_documents": int(moved_docs or 0),
        "moved_folders": int(moved_folders or 0),
        "parent_id": parent,
    }


def move_document(doc_id: str, folder_id: str = FOLDER_ROOT) -> dict[str, Any]:
    with _LOCK:
        conn = _STORE.connect()
        try:
            row = conn.execute(
                "SELECT library_id, name, folder_id FROM kb_doc WHERE id = ?", (doc_id,)
            ).fetchone()
            if row is None:
                raise KBUnavailable("文档不存在")
            _check_folder(conn, folder_id or "", row["library_id"])
            conn.execute(
                "UPDATE kb_doc SET folder_id = ? WHERE id = ?", (folder_id or FOLDER_ROOT, doc_id)
            )
            conn.commit()
        finally:
            conn.close()
    return {"doc_id": doc_id, "folder_id": folder_id or FOLDER_ROOT, "name": row["name"]}


def _ensure_folder_path(
    conn: sqlite3.Connection,
    library_id: str,
    parts: Sequence[str],
    root_id: str,
    cache: dict[tuple[str, str], str],
) -> str:
    """按目录段逐级建文件夹，已存在就复用。导入目录时靠它把原结构搬进库。"""
    cursor = root_id or FOLDER_ROOT
    for raw in parts:
        name = (raw or "").strip()
        if not name:
            continue
        key = (cursor, name)
        cached = cache.get(key)
        if cached:
            cursor = cached
            continue
        parent = cursor
        row = conn.execute(
            "SELECT id FROM kb_folder WHERE library_id = ? AND parent_id = ? AND name = ?",
            (library_id, parent, name),
        ).fetchone()
        if row is not None:
            cursor = row["id"]
        else:
            cursor = _new_folder_id()
            conn.execute(
                "INSERT INTO kb_folder (id, library_id, parent_id, name, sort_order, created_at)"
                " VALUES (?,?,?,?,?,?)",
                (cursor, library_id, parent, name, 0,
                 datetime.now().isoformat(timespec="seconds")),
            )
        cache[key] = cursor
    return cursor


# --------------------------------------------------------------------------
# 入库
# --------------------------------------------------------------------------


def _doc_id(path: Path, library_id: str = DEFAULT_LIBRARY_ID) -> str:
    """文档 id 带上库归属：同一份文件归到两个库是两份独立记录。"""
    key = "%s|%s" % (library_id or DEFAULT_LIBRARY_ID, str(Path(path).resolve()).lower())
    return hashlib.sha1(key.encode("utf-8")).hexdigest()[:16]


def _file_hash(path: Path) -> str:
    digest = hashlib.sha1()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _load_sections(path: Path) -> tuple[list[dict[str, Any]], int, int]:
    """返回 (节列表, 页数, 字符数)。"""
    suffix = path.suffix.lower()
    if suffix == ".pdf":
        page_texts, offsets, bookmarks = _pdf_pages(path)
        total_chars = sum(len(t) for t in page_texts)
        sections = _pdf_sections(page_texts, offsets, bookmarks)
        return _normalize_sections(sections), len(page_texts), total_chars

    if suffix in {".txt", ".md"}:
        text = path.read_text(encoding="utf-8", errors="replace")
        sections = _md_sections_if_any(text)
        return _normalize_sections(sections), 1, len(text)

    # docx / xlsx / doc / xls 交给已有解析器，结构拿不到就按段落切
    from document_loader import extract_content

    payload = extract_content(str(path))
    text = payload.get("text", "")
    return _normalize_sections(_plain_sections(text)), 1, len(text)


def _md_sections_if_any(text: str) -> list[dict[str, Any]]:
    return _text_sections(text) if ("#" in text or _NUM_HEADING.search(text)) else _plain_sections(text)


def index_file(
    path: str | Path,
    *,
    library_id: str = DEFAULT_LIBRARY_ID,
    folder_id: str = FOLDER_ROOT,
    db_path: Optional[Path] = None,
) -> dict[str, Any]:
    """把一份文件索引进指定知识库（可指定归档文件夹）。失败写进 kb_doc.status='failed'，不抛给调用方。"""
    source = Path(path)
    if not source.exists():
        raise KBUnavailable("文件不存在: %s" % source)
    library_id = (library_id or DEFAULT_LIBRARY_ID).strip()
    folder_id = (folder_id or FOLDER_ROOT).strip()
    doc_id = _doc_id(source, library_id)
    now = datetime.now().isoformat(timespec="seconds")
    mtime = 0.0
    size = 0
    try:
        mtime = source.stat().st_mtime
        size = source.stat().st_size
        content_hash = _file_hash(source)
        sections, pages, total_chars = _load_sections(source)
    except Exception as exc:  # noqa: BLE001 - 解析失败要留痕，让人看得见
        with _LOCK:
            store = _STORE if db_path is None else _Store(db_path)
            conn = store.connect()
            try:
                _ensure_library(conn, library_id)
                folder_id = _safe_folder(conn, folder_id, library_id)
                conn.execute(
                    "INSERT INTO kb_doc (id, library_id, folder_id, path, name, suffix, status, error,"
                    " mtime, size, indexed_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)"
                    " ON CONFLICT(id) DO UPDATE SET status='failed', error=excluded.error,"
                    " library_id=excluded.library_id, folder_id=excluded.folder_id,"
                    " indexed_at=excluded.indexed_at,"
                    " mtime=excluded.mtime, size=excluded.size",
                    (doc_id, library_id, folder_id, str(source), source.name,
                     source.suffix.lower(), DOC_STATUS_FAILED,
                     "%s: %s" % (type(exc).__name__, exc), mtime, size, now),
                )
                conn.commit()
            finally:
                conn.close()
        return {"doc_id": doc_id, "name": source.name, "status": DOC_STATUS_FAILED,
                "error": "%s: %s" % (type(exc).__name__, exc), "chunks": 0, "params": 0}

    with _LOCK:
        store = _STORE if db_path is None else _Store(db_path)
        conn = store.connect()
        try:
            conn.execute("DELETE FROM kb_chunk WHERE doc_id = ?", (doc_id,))
            conn.execute("DELETE FROM kb_param WHERE doc_id = ?", (doc_id,))
            _ensure_library(conn, library_id)
            folder_id = _safe_folder(conn, folder_id, library_id)
            conn.execute(
                "INSERT INTO kb_doc (id, library_id, folder_id, path, name, suffix, status, error,"
                " pages, chars, chunk_count, mtime, size, content_hash, indexed_at)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
                " ON CONFLICT(id) DO UPDATE SET library_id=excluded.library_id,"
                " folder_id=excluded.folder_id, path=excluded.path, name=excluded.name,"
                " status=excluded.status, error='', pages=excluded.pages, chars=excluded.chars,"
                " chunk_count=excluded.chunk_count, mtime=excluded.mtime, size=excluded.size,"
                " content_hash=excluded.content_hash, indexed_at=excluded.indexed_at",
                (doc_id, library_id, folder_id, str(source), source.name, source.suffix.lower(),
                 DOC_STATUS_OK, "", pages, total_chars, 0, mtime, size, content_hash, now),
            )

            chunk_rows: list[tuple] = []
            param_rows: list[dict[str, Any]] = []
            ordinal = 0
            for section in sections:
                for piece in _split_long(section["text"]):
                    chunk_rows.append(
                        (
                            doc_id, ordinal, section["heading_path"], section["title"],
                            section["page_start"], section["page_end"],
                            section["char_start"], section["char_end"],
                            _classify(piece), piece,
                            build_search_text(section["heading_path"], piece),
                        )
                    )
                    ordinal += 1
                param_rows.extend(
                    _extract_params(
                        section["text"], doc_id, section["heading_path"],
                        section["page_start"], section["title"],
                    )
                )

            if chunk_rows:
                conn.executemany(
                    "INSERT INTO kb_chunk (doc_id, ordinal, heading_path, section, page_start,"
                    " page_end, char_start, char_end, kind, text, search_text)"
                    " VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                    chunk_rows,
                )
            if param_rows:
                conn.executemany(
                    "INSERT INTO kb_param (name, domain, module, default_value, min_value,"
                    " max_value, unit, description, doc_id, heading_path, page)"
                    " VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                    [
                        (row["name"], row["domain"], row["module"], row["default_value"],
                         row["min_value"], row["max_value"], row["unit"], row["description"],
                         row["doc_id"], row["heading_path"], row["page"])
                        for row in param_rows
                    ],
                )
            conn.execute(
                "UPDATE kb_doc SET chunk_count = ? WHERE id = ?", (len(chunk_rows), doc_id)
            )
            conn.commit()
        finally:
            conn.close()

    return {
        "doc_id": doc_id,
        "library_id": library_id,
        "folder_id": folder_id,
        "name": source.name,
        "status": DOC_STATUS_OK,
        "error": "",
        "chunks": len(chunk_rows),
        "params": len(param_rows),
        "pages": pages,
        "chars": total_chars,
    }


def import_paths(
    paths: Iterable[str],
    *,
    recursive: bool = True,
    library_id: str = DEFAULT_LIBRARY_ID,
    folder_id: str = FOLDER_ROOT,
    mirror_tree: bool = True,
) -> dict[str, Any]:
    """批量索引。传目录时默认按原目录结构在库里建文件夹（mirror_tree）。

    先一次性把文件夹结构解出来，再逐个索引文件：每份文件索引都会自己开一条
    连接，边建文件夹边索引会让最深的目录被反复建很多次。
    """
    library_id = (library_id or DEFAULT_LIBRARY_ID).strip()
    root_folder = (folder_id or FOLDER_ROOT).strip()

    files: list[Path] = []
    missing: list[str] = []
    # 每份文件对应一段目录路径（相对导入根），用于镜像目录结构
    plan: list[tuple[Path, tuple[str, ...]]] = []
    for raw in paths:
        item = Path(str(raw)).expanduser()
        if item.is_dir():
            pattern = "**/*" if recursive else "*"
            for path in item.glob(pattern):
                if not path.is_file():
                    continue
                try:
                    rel = path.parent.relative_to(item)
                    parts = tuple(rel.parts) if str(rel) not in (".", "") else ()
                except ValueError:
                    parts = ()
                plan.append((path, parts))
        elif item.is_file():
            plan.append((item, ()))
        else:
            missing.append(str(raw))

    supported = [(path, parts) for path, parts in plan
                 if path.suffix.lower() in SUPPORTED_SUFFIXES]
    skipped = [path.name for path, _ in plan if path.suffix.lower() not in SUPPORTED_SUFFIXES]

    # 第一轮：建文件夹（只开一条连接）
    folder_of: dict[str, str] = {}
    if mirror_tree:
        with _LOCK:
            conn = _STORE.connect()
            try:
                _ensure_library(conn, library_id)
                _check_folder(conn, root_folder, library_id)
                cache: dict[tuple[str, str], str] = {}
                for path, parts in supported:
                    folder_of[str(path)] = _ensure_folder_path(
                        conn, library_id, parts, root_folder, cache
                    )
                conn.commit()
            finally:
                conn.close()

    results = []
    for path, _ in supported:
        try:
            results.append(
                index_file(
                    path, library_id=library_id,
                    folder_id=folder_of.get(str(path), root_folder),
                )
            )
        except KBUnavailable as exc:
            results.append({"name": path.name, "status": DOC_STATUS_FAILED, "error": str(exc),
                            "chunks": 0, "params": 0})
    optimize()
    return {
        "indexed": [r for r in results if r.get("status") == DOC_STATUS_OK],
        "failed": [r for r in results if r.get("status") != DOC_STATUS_OK],
        "missing": missing,
        "skipped": skipped[:50],
        "skipped_count": len(skipped),
    }


def optimize() -> bool:
    """合并 FTS5 段。增量多了之后不合并，查询延迟会成倍上涨。"""
    with _LOCK:
        try:
            conn = _STORE.connect()
        except (sqlite3.Error, OSError):
            return False
        try:
            if not _STORE.fts:
                return False
            conn.execute("INSERT INTO kb_chunk_fts(kb_chunk_fts) VALUES('optimize')")
            conn.commit()
            return True
        except sqlite3.Error:
            _STORE._fts = False
            return False
        finally:
            conn.close()


def remove_document(doc_id: str) -> bool:
    with _LOCK:
        conn = _STORE.connect()
        try:
            conn.execute("DELETE FROM kb_chunk WHERE doc_id = ?", (doc_id,))
            conn.execute("DELETE FROM kb_param WHERE doc_id = ?", (doc_id,))
            cursor = conn.execute("DELETE FROM kb_doc WHERE id = ?", (doc_id,))
            conn.commit()
            return cursor.rowcount > 0
        finally:
            conn.close()


# --------------------------------------------------------------------------
# 检索
# --------------------------------------------------------------------------


def _terms(query: str) -> list[str]:
    tokens = [t for t in re.split(r"\s+", query.strip()) if t]
    grams = cjk_ngrams(query)
    out: list[str] = []
    for item in tokens + grams:
        if item and item not in out:
            out.append(item)
    return out


def search(
    query: str,
    *,
    top_k: int = 5,
    doc_id: Optional[str] = None,
    library_id: Optional[str] = None,
    scope: Optional[str] = None,
    pool: int = 200,
    max_per_doc: int = 2,
) -> list[dict[str, Any]]:
    """词法检索：bm25 粗排 + 规则重排。

    library_id 为空或 `*` 表示搜全部库；否则只搜这个库。库隔离在 SQL 里做，
    不靠调用方自己过滤结果。
    scope 只是给调用方一个语义标签，当前实现只按 doc_id 过滤；保留参数是为了
    将来接向量召回时不用改工具签名。
    """
    text = clean_query(query)
    if not text:
        return []
    with _LOCK:
        try:
            conn = _STORE.connect()
        except (sqlite3.Error, OSError):
            return []
        try:
            rows = _search_rows(conn, text, pool, doc_id, library_id)
            if not rows:
                return []
            ids = [row["id"] for row in rows]
            placeholders = ",".join("?" for _ in ids)
            details = {
                row["id"]: row
                for row in conn.execute(
                    "SELECT c.id, c.doc_id, c.heading_path, c.section, c.page_start,"
                    " c.page_end, c.kind, c.text, c.ordinal, d.name AS doc_name,"
                    " d.path AS doc_path, d.library_id AS library_id,"
                    " COALESCE(l.name, '') AS library_name"
                    " FROM kb_chunk c JOIN kb_doc d ON d.id = c.doc_id"
                    " LEFT JOIN kb_library l ON l.id = d.library_id"
                    " WHERE c.id IN (%s) AND d.status = 'ok'" % placeholders,
                    ids,
                ).fetchall()
            }
        finally:
            conn.close()

    ranked = _rank(rows, details, text)
    picked: list[dict[str, Any]] = []
    per_doc: dict[str, int] = {}
    for item in ranked:
        key = item["doc_id"]
        if per_doc.get(key, 0) >= max_per_doc:
            continue
        per_doc[key] = per_doc.get(key, 0) + 1
        picked.append(item)
        if len(picked) >= max(1, top_k):
            break
    return picked


def _search_rows(
    conn: sqlite3.Connection,
    query: str,
    pool: int,
    doc_id: Optional[str],
    library_id: Optional[str] = None,
) -> list[sqlite3.Row]:
    clauses: list[str] = []
    params: list[Any] = []
    if doc_id:
        clauses.append("c.doc_id = ?")
        params.append(doc_id)
    target_lib = (library_id or "").strip()
    if target_lib and target_lib != LIBRARY_ALL:
        clauses.append("d.library_id = ?")
        params.append(target_lib)
    where = (" AND " + " AND ".join(clauses)) if clauses else ""

    if _STORE.fts:
        fts_query = build_fts_query(query)
        if fts_query:
            try:
                return conn.execute(
                    "SELECT c.id AS id, bm25(kb_chunk_fts) AS rank"
                    " FROM kb_chunk_fts f JOIN kb_chunk c ON c.id = f.rowid"
                    " JOIN kb_doc d ON d.id = c.doc_id"
                    " WHERE kb_chunk_fts MATCH ? AND d.status = 'ok'" + where +
                    " ORDER BY rank LIMIT ?",
                    [fts_query] + params + [pool],
                ).fetchall()
            except sqlite3.Error:
                _STORE._fts = False

    like = "%%%s%%" % query.replace("%", "").replace("_", "")
    return conn.execute(
        "SELECT c.id AS id, 0.0 AS rank FROM kb_chunk c"
        " JOIN kb_doc d ON d.id = c.doc_id"
        " WHERE d.status = 'ok' AND (c.text LIKE ? OR c.heading_path LIKE ?)" + where +
        " LIMIT ?",
        [like, like] + params + [pool],
    ).fetchall()


def _rank(
    rows: list[sqlite3.Row], details: dict[int, sqlite3.Row], query: str
) -> list[dict[str, Any]]:
    """bm25 分数归一到 0~1，再叠标题命中、节标题命中与精确短语三项规则。

    标题命中的权重给得高，是因为这份资料的提问经常直接报功能名（"设置边界"、
    "附面层推进"）。BM25 只看正文词频，会把长段落里顺带提到功能名的块顶上来；
    把标题命中提权，等于让「问哪节就回哪节」优先于「哪段提得多」。
    """
    ranks = [float(row["rank"]) for row in rows]
    best, worst = (min(ranks), max(ranks)) if ranks else (0.0, 0.0)
    span = worst - best
    norm_query = _norm(query)
    terms = [t for t in _terms(query) if len(t) >= 2][:24]
    term_count = max(1, len(terms))

    scored: list[dict[str, Any]] = []
    for row in rows:
        detail = details.get(row["id"])
        if detail is None:
            continue
        rank = float(row["rank"])
        lexical = 1.0 if span <= 0 else 1.0 - (rank - best) / span
        heading = _norm(detail["heading_path"])
        section = _norm(detail["section"])
        heading_hit = sum(1 for term in terms if term in heading) / term_count
        section_hit = sum(1 for term in terms if term in section) / term_count
        body = _norm(detail["text"])
        exact = 1.0 if len(norm_query) >= 3 and norm_query in body else 0.0
        # 短块（图注、单行书签）会被 bm25 因为长度归一化抬高，压一点回去
        length_factor = 0.88 if len(body) < 120 else 1.0
        score = length_factor * (
            0.55 * lexical + 0.25 * heading_hit + 0.12 * section_hit + 0.08 * exact
        )
        snippet = _snippet(detail["text"], query)
        scored.append(
            {
                "chunk_id": detail["id"],
                "doc_id": detail["doc_id"],
                "doc": detail["doc_name"],
                "library_id": detail["library_id"],
                "library": detail["library_name"],
                "heading_path": detail["heading_path"],
                "page": detail["page_start"],
                "page_end": detail["page_end"],
                "kind": detail["kind"],
                "score": round(score, 4),
                "snippet": snippet,
            }
        )
    scored.sort(key=lambda item: item["score"], reverse=True)
    return scored


def _snippet(text: str, query: str, width: int = 200) -> str:
    flat = _clean_block(text).replace("\n", " ")
    if len(flat) <= width:
        return flat
    grams = [t for t in _terms(query) if len(t) >= 2]
    hit = -1
    for gram in grams:
        index = flat.find(gram)
        if index >= 0:
            hit = index
            break
    if hit < 0:
        return flat[:width] + "…"
    start = max(0, hit - width // 3)
    end = min(len(flat), start + width)
    prefix = "…" if start > 0 else ""
    suffix = "…" if end < len(flat) else ""
    return prefix + flat[start:end] + suffix


def read_chunk(
    chunk_id: int, *, context: int = 0, library_id: Optional[str] = None
) -> Optional[dict[str, Any]]:
    """读回原文。传了 library_id 就校验收否属于该库，避免跳库读到别的资料。"""
    with _LOCK:
        conn = _STORE.connect()
        try:
            row = conn.execute(
                "SELECT c.*, d.name AS doc_name, d.path AS doc_path,"
                " d.library_id AS library_id, COALESCE(l.name,'') AS library_name"
                " FROM kb_chunk c JOIN kb_doc d ON d.id = c.doc_id"
                " LEFT JOIN kb_library l ON l.id = d.library_id"
                " WHERE c.id = ?",
                (int(chunk_id),),
            ).fetchone()
            if row is None:
                return None
            target_lib = (library_id or "").strip()
            if target_lib and target_lib != LIBRARY_ALL and row["library_id"] != target_lib:
                return None
            neighbours = []
            if context > 0:
                neighbours = conn.execute(
                    "SELECT id, text FROM kb_chunk WHERE doc_id = ? AND ordinal BETWEEN ? AND ?"
                    " ORDER BY ordinal",
                    (row["doc_id"], row["ordinal"] - context, row["ordinal"] + context),
                ).fetchall()
        finally:
            conn.close()

    payload = {
        "chunk_id": row["id"],
        "doc_id": row["doc_id"],
        "doc": row["doc_name"],
        "library_id": row["library_id"],
        "library": row["library_name"],
        "path": row["doc_path"],
        "heading_path": row["heading_path"],
        "page": row["page_start"],
        "page_end": row["page_end"],
        "char_start": row["char_start"],
        "char_end": row["char_end"],
        "kind": row["kind"],
        "text": row["text"],
    }
    if context > 0:
        payload["context_before"] = [
            {"chunk_id": n["id"], "text": n["text"][:400]}
            for n in neighbours if n["id"] < row["id"]
        ]
        payload["context_after"] = [
            {"chunk_id": n["id"], "text": n["text"][:400]}
            for n in neighbours if n["id"] > row["id"]
        ]
    return payload


# --------------------------------------------------------------------------
# 参数
# --------------------------------------------------------------------------


def find_params(
    query: str, *, limit: int = 8, domain: str = "", library_id: Optional[str] = None
) -> list[dict[str, Any]]:
    text = clean_query(query)
    if not text:
        return []
    with _LOCK:
        conn = _STORE.connect()
        try:
            sql = (
                "SELECT p.*, d.name AS doc_name, d.library_id AS library_id"
                " FROM kb_param p JOIN kb_doc d ON d.id = p.doc_id WHERE d.status = 'ok'"
            )
            params: list[Any] = []
            if domain:
                sql += " AND p.domain = ?"
                params.append(domain)
            target_lib = (library_id or "").strip()
            if target_lib and target_lib != LIBRARY_ALL:
                sql += " AND d.library_id = ?"
                params.append(target_lib)
            rows = conn.execute(sql, params).fetchall()
        finally:
            conn.close()

    hits: list[tuple[int, dict[str, Any]]] = []
    for row in rows:
        name = row["name"]
        score = 0
        if name and name in text:
            score = len(name) * 2
        elif text and text in name:
            score = len(text)
        if not score:
            continue
        hits.append(
            (
                score,
                {
                    "name": name,
                    "domain": row["domain"],
                    "module": row["module"],
                    "default": row["default_value"],
                    "min": row["min_value"],
                    "max": row["max_value"],
                    "unit": row["unit"],
                    "description": row["description"],
                    "doc": row["doc_name"],
                    "library_id": row["library_id"],
                    "heading_path": row["heading_path"],
                    "page": row["page"],
                },
            )
        )
    hits.sort(key=lambda item: item[0], reverse=True)
    out: list[dict[str, Any]] = []
    seen: set[str] = set()
    for _, item in hits:
        key = "%s@%s" % (item["name"], item["heading_path"])
        if key in seen:
            continue
        seen.add(key)
        out.append(item)
        if len(out) >= limit:
            break
    return out


def list_params(
    *, limit: int = 200, domain: str = "", library_id: Optional[str] = None
) -> dict[str, Any]:
    with _LOCK:
        conn = _STORE.connect()
        try:
            sql = (
                "SELECT p.*, d.name AS doc_name FROM kb_param p"
                " JOIN kb_doc d ON d.id = p.doc_id WHERE d.status = 'ok'"
            )
            params: list[Any] = []
            if domain:
                sql += " AND p.domain = ?"
                params.append(domain)
            target_lib = (library_id or "").strip()
            if target_lib and target_lib != LIBRARY_ALL:
                sql += " AND d.library_id = ?"
                params.append(target_lib)
            sql += " ORDER BY p.domain, p.name LIMIT ?"
            params.append(int(limit))
            rows = conn.execute(sql, params).fetchall()
            total = conn.execute(
                "SELECT COUNT(*) AS n FROM kb_param p JOIN kb_doc d ON d.id = p.doc_id"
                " WHERE d.status = 'ok'"
                + (" AND d.library_id = ?" if target_lib and target_lib != LIBRARY_ALL else ""),
                ([target_lib] if target_lib and target_lib != LIBRARY_ALL else []),
            ).fetchone()["n"]
        finally:
            conn.close()
    return {
        "total": int(total),
        "items": [
            {
                "name": row["name"], "domain": row["domain"], "module": row["module"],
                "default": row["default_value"], "min": row["min_value"],
                "max": row["max_value"], "unit": row["unit"],
                "description": row["description"], "doc": row["doc_name"],
                "heading_path": row["heading_path"], "page": row["page"],
            }
            for row in rows
        ],
    }


# --------------------------------------------------------------------------
# 目录与状态
# --------------------------------------------------------------------------


def catalog(
    prefix: str = "", limit: int = 120, library_id: Optional[str] = None
) -> dict[str, Any]:
    """给模型看的知识库全景：文档清单 + 章节树，不含正文。"""
    target_lib = (library_id or "").strip()
    scoped = bool(target_lib) and target_lib != LIBRARY_ALL
    with _LOCK:
        conn = _STORE.connect()
        try:
            doc_sql = (
                "SELECT d.id, d.name, d.pages, d.chunk_count, d.status, d.library_id,"
                " d.folder_id, COALESCE(l.name,'') AS library_name FROM kb_doc d"
                " LEFT JOIN kb_library l ON l.id = d.library_id WHERE d.status = 'ok'"
            )
            section_sql = (
                "SELECT c.doc_id, c.heading_path, MIN(c.page_start) AS page FROM kb_chunk c"
                " JOIN kb_doc d ON d.id = c.doc_id"
            )
            args: list[Any] = []
            if scoped:
                doc_sql += " AND d.library_id = ?"
                section_sql += " WHERE d.library_id = ?"
                args.append(target_lib)
            doc_sql += " ORDER BY d.name"
            section_sql += " GROUP BY c.doc_id, c.heading_path ORDER BY c.doc_id, MIN(c.ordinal)"
            docs = conn.execute(doc_sql, args).fetchall()
            rows = conn.execute(section_sql, args).fetchall()
            paths = _folder_path_map(conn, target_lib) if scoped else {}
        finally:
            conn.close()

    tree: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        heading = row["heading_path"] or "（无标题）"
        if prefix and prefix not in heading:
            continue
        tree.setdefault(row["doc_id"], []).append({"heading": heading, "page": row["page"]})

    items = []
    for doc in docs:
        if prefix and prefix not in doc["name"] and doc["id"] not in tree:
            continue
        items.append(
            {
                "doc_id": doc["id"],
                "doc": doc["name"],
                "library_id": doc["library_id"],
                "library": doc["library_name"],
                "folder": paths.get(doc["folder_id"] or FOLDER_ROOT, ""),
                "pages": doc["pages"],
                "chunks": doc["chunk_count"],
                "sections": tree.get(doc["id"], [])[:limit],
            }
        )
    return {"documents": items, "total": len(items), "library_id": target_lib}


def list_documents(
    library_id: Optional[str] = None,
    folder_id: Optional[str] = None,
    deep: bool = False,
) -> list[dict[str, Any]]:
    """列文档。folder_id 为 None 列全库；空串列库根；给 id 列该文件夹（deep 时包子文件夹）。"""
    target_lib = (library_id or "").strip()
    scoped = bool(target_lib) and target_lib != LIBRARY_ALL
    with _LOCK:
        conn = _STORE.connect()
        try:
            sql = (
                "SELECT d.*, (SELECT COUNT(*) FROM kb_param p WHERE p.doc_id = d.id) AS param_count"
                " FROM kb_doc d"
            )
            clauses: list[str] = []
            args: list[Any] = []
            if scoped:
                clauses.append("d.library_id = ?")
                args.append(target_lib)
            if folder_id is not None:
                if deep and folder_id:
                    ids = [folder_id]
                    if scoped:
                        rows = _folder_rows(conn, target_lib)
                        by_parent: dict[str, list[str]] = {}
                        for row in rows:
                            by_parent.setdefault(row["parent_id"] or FOLDER_ROOT, []).append(row["id"])
                        frontier = [folder_id]
                        while frontier:
                            current = frontier.pop()
                            for child in by_parent.get(current, []):
                                ids.append(child)
                                frontier.append(child)
                    placeholders = ",".join("?" for _ in ids)
                    clauses.append("d.folder_id IN (%s)" % placeholders)
                    args.extend(ids)
                else:
                    clauses.append("COALESCE(d.folder_id, '') = ?")
                    args.append(folder_id)
            if clauses:
                sql += " WHERE " + " AND ".join(clauses)
            sql += " ORDER BY d.indexed_at DESC, d.name"
            rows = conn.execute(sql, args).fetchall()
            paths = _folder_path_map(conn, target_lib) if scoped else {}
        finally:
            conn.close()
    return [
        {
            "doc_id": row["id"],
            "library_id": row["library_id"],
            "folder_id": row["folder_id"] or FOLDER_ROOT,
            "folder_path": paths.get(row["folder_id"] or FOLDER_ROOT, ""),
            "name": row["name"],
            "path": row["path"],
            "suffix": row["suffix"],
            "status": row["status"],
            "error": row["error"],
            "pages": row["pages"],
            "chars": row["chars"],
            "chunks": row["chunk_count"],
            "params": row["param_count"],
            "size": row["size"],
            "indexed_at": row["indexed_at"],
        }
        for row in rows
    ]


def get_document(doc_id: str) -> Optional[dict[str, Any]]:
    with _LOCK:
        conn = _STORE.connect()
        try:
            row = conn.execute(
                "SELECT d.*, (SELECT COUNT(*) FROM kb_param p WHERE p.doc_id = d.id) AS param_count"
                " FROM kb_doc d WHERE d.id = ?",
                (doc_id,),
            ).fetchone()
        finally:
            conn.close()
    if row is None:
        return None
    return {
        "doc_id": row["id"],
        "library_id": row["library_id"],
        "name": row["name"],
        "path": row["path"],
        "status": row["status"],
        "chunks": row["chunk_count"],
        "params": row["param_count"],
    }


def status(library_id: Optional[str] = None) -> dict[str, Any]:
    """不传 library_id 就是全局汇总；传了就看这个库的体量。"""
    target_lib = (library_id or "").strip()
    scoped = bool(target_lib) and target_lib != LIBRARY_ALL
    doc_filter = " AND d.library_id = ?" if scoped else ""
    args: list[Any] = [target_lib] if scoped else []
    try:
        with _LOCK:
            conn = _STORE.connect()
            try:
                docs = conn.execute(
                    "SELECT COUNT(*) AS n FROM kb_doc d WHERE d.status = 'ok'" + doc_filter, args
                ).fetchone()["n"]
                failed = conn.execute(
                    "SELECT COUNT(*) AS n FROM kb_doc d WHERE d.status = 'failed'" + doc_filter, args
                ).fetchone()["n"]
                chunks = conn.execute(
                    "SELECT COUNT(*) AS n FROM kb_chunk c JOIN kb_doc d ON d.id = c.doc_id"
                    " WHERE 1=1" + doc_filter, args
                ).fetchone()["n"]
                params = conn.execute(
                    "SELECT COUNT(*) AS n FROM kb_param p JOIN kb_doc d ON d.id = p.doc_id"
                    " WHERE 1=1" + doc_filter, args
                ).fetchone()["n"]
                last = conn.execute(
                    "SELECT MAX(d.indexed_at) AS ts FROM kb_doc d WHERE 1=1" + doc_filter, args
                ).fetchone()["ts"]
                libraries = conn.execute(
                    "SELECT COUNT(*) AS n FROM kb_library"
                ).fetchone()["n"]
            finally:
                conn.close()
    except (sqlite3.Error, OSError) as exc:
        return {
            "available": False,
            "error": "%s: %s" % (type(exc).__name__, exc),
            "db_path": str(KB_DB_PATH),
            "fts": False,
        }
    size = KB_DB_PATH.stat().st_size if KB_DB_PATH.exists() else 0
    return {
        "available": True,
        "error": "",
        "db_path": str(KB_DB_PATH),
        "sources_dir": str(KB_SOURCES_DIR),
        "kb_dir": str(KB_DIR),
        "fts": bool(_STORE.fts),
        "library_id": target_lib if scoped else LIBRARY_ALL,
        "libraries": int(libraries),
        "documents": int(docs),
        "failed": int(failed),
        "chunks": int(chunks),
        "params": int(params),
        "last_indexed": last or "",
        "db_bytes": size,
    }


def copy_into_sources(source: Path, *, name: str = "") -> Path:
    """把外部文件复制进知识库自己的 sources 目录，保证回源有个稳定落点。

    文件名优先保留原样；同名但内容不同才补一段内容哈希，避免两份同名文档
    互相覆盖、也避免重导同一份新版本时堆出一地副本。
    """
    KB_SOURCES_DIR.mkdir(parents=True, exist_ok=True)
    suffix = source.suffix.lower() or Path(name).suffix.lower()
    stem = Path(name or source.name).stem or "document"
    target = KB_SOURCES_DIR / (stem + suffix)
    if target.exists():
        if _file_hash(target) == _file_hash(source):
            return target
        digest = hashlib.sha1(_file_hash(source).encode("utf-8")).hexdigest()[:8]
        target = KB_SOURCES_DIR / ("%s-%s%s" % (stem, digest, suffix))
    if not target.exists():
        target.write_bytes(source.read_bytes())
    return target


def save_upload(data: bytes, name: str) -> Path:
    """前端上传的字节直接落到 sources 目录，返回可索引的路径。"""
    KB_SOURCES_DIR.mkdir(parents=True, exist_ok=True)
    safe = Path(str(name or "").replace("\\", "/")).name or "document"
    suffix = Path(safe).suffix.lower()
    if suffix not in SUPPORTED_SUFFIXES:
        raise KBUnavailable("不支持的文件类型：%s" % (suffix or "(无扩展名)"))
    stem = Path(safe).stem or "document"
    digest = hashlib.sha1(data).hexdigest()[:8]
    target = KB_SOURCES_DIR / (stem + suffix)
    if target.exists() and target.read_bytes() != data:
        target = KB_SOURCES_DIR / ("%s-%s%s" % (stem, digest, suffix))
    target.write_bytes(data)
    return target
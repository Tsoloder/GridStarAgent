"""Extract text/images from user-imported attachments for one chat request."""
import base64
import io
import threading
from collections import OrderedDict
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

MAX_FILE_BYTES = 10 * 1024 * 1024
MAX_TOTAL_CHARS = 120_000
MAX_ATTACHMENTS = 6
MAX_IMAGES = 4

try:  # Pillow 只在需要降采样时用；缺失时原图直传，不阻断发送
    from PIL import Image as _PILImage
except ImportError:  # pragma: no cover - 取决于环境是否装了 Pillow
    _PILImage = None

# 发送前降采样上限：长边对齐 Claude 原生上限，短边对齐 OpenAI 高细节的归一化目标。
# 超限的原图在 Claude 上按 ⌈宽/28⌉×⌈高/28⌉ 计费，一张 2560×1440 截图就是 4784 个
# 视觉 token；按长边 ≤1568、短边 ≤768 等比缩放后只剩 1372，画质对手动识别类任务
# 基本无感。
MAX_IMAGE_LONG_EDGE = 1568
MAX_IMAGE_SHORT_EDGE = 768
IMAGE_JPEG_QUALITY = 85

# 按编码字节再设一道闸：尺寸合规但体积巨大的图（截图、扫描件）仍要压下来。
# 单图预算与整请求预算都对齐 openhanako 的输入边界策略。
MAX_IMAGE_BASE64_BYTES = 4_500_000
TOTAL_IMAGE_BASE64_BUDGET = 24_000_000
MIN_IMAGE_BASE64_BYTES = 256_000

# 历史图片退化成文本占位时的说明。图片只在到达它的那一轮发给模型，之后随历史
# 每轮重传既是纯浪费、又挤占窗口，占位文本让模型知道"这里曾有一张图"。
HISTORICAL_IMAGE_NOTICE = "[图片已省略：历史图片保留为文件引用，避免重复发送原始 base64]"

PLAIN_TEXT_SUFFIXES = {
    ".txt", ".md", ".markdown", ".csv", ".tsv", ".json", ".xml", ".log",
    ".html", ".htm", ".ini", ".cfg", ".yaml", ".yml", ".toml", ".py",
    ".js", ".ts", ".css", ".sql", ".sh", ".bat",
}
DOCUMENT_SUFFIXES = PLAIN_TEXT_SUFFIXES | {".pdf", ".doc", ".docx", ".xlsx", ".xls"}
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"}
SUPPORTED = DOCUMENT_SUFFIXES | IMAGE_SUFFIXES
IMAGE_MEDIA_TYPES = {
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".webp": "image/webp",
    ".bmp": "image/bmp",
}


def _read_text(path: Path) -> str:
    data = path.read_bytes()
    for encoding in ("utf-8-sig", "utf-16", "gb18030"):
        try:
            return data.decode(encoding)
        except UnicodeDecodeError:
            continue
    return data.decode("utf-8", errors="replace")


def _read_pdf(path: Path) -> str:
    try:
        from pypdf import PdfReader
    except ImportError as exc:
        raise RuntimeError("读取 PDF 需要安装 pypdf") from exc
    reader = PdfReader(str(path))
    pages = [(page.extract_text() or "") for page in reader.pages]
    return "\n\n".join(pages)


def _read_doc(path: Path) -> str:
    # .doc 是旧式二进制格式，Windows 上优先使用已安装的 Microsoft Word。
    try:
        import pythoncom  # type: ignore
        import win32com.client  # type: ignore
    except ImportError as exc:
        raise RuntimeError("读取 .doc 需要 Windows Microsoft Word 或 pywin32") from exc

    pythoncom.CoInitialize()
    word = None
    document = None
    try:
        word = win32com.client.DispatchEx("Word.Application")
        word.Visible = False
        word.DisplayAlerts = 0
        word.AutomationSecurity = 3  # msoAutomationSecurityForceDisable
        document = word.Documents.Open(str(path), ReadOnly=True, AddToRecentFiles=False)
        return document.Content.Text
    finally:
        if document is not None:
            try:
                document.Close(False)
            except Exception:
                pass
        if word is not None:
            try:
                word.Quit()
            except Exception:
                pass
        pythoncom.CoUninitialize()


def _read_docx(path: Path) -> str:
    # .docx 本质是 zip 包，直接解析 word/document.xml，无需额外依赖。
    import zipfile
    import xml.etree.ElementTree as ElementTree

    namespace = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"
    try:
        with zipfile.ZipFile(str(path)) as archive:
            with archive.open("word/document.xml") as handle:
                tree = ElementTree.parse(handle)
    except (KeyError, zipfile.BadZipFile) as exc:
        raise RuntimeError("DOCX 文件已损坏或不是有效的 Office 文档") from exc

    paragraphs = []
    for paragraph in tree.getroot().iter("%sp" % namespace):
        texts = [node.text or "" for node in paragraph.iter("%st" % namespace)]
        paragraphs.append("".join(texts))
    return "\n".join(paragraphs)


def _read_xlsx(path: Path) -> str:
    try:
        from openpyxl import load_workbook
    except ImportError as exc:
        raise RuntimeError("读取 XLSX 需要安装 openpyxl") from exc

    workbook = load_workbook(str(path), read_only=True, data_only=True)
    sections = []
    try:
        for sheet in workbook.worksheets:
            rows = _format_sheet_rows(sheet.iter_rows(values_only=True))
            if rows:
                sections.append("工作表：%s\n%s" % (sheet.title, "\n".join(rows)))
    finally:
        try:
            workbook.close()
        except Exception:
            pass
    return "\n\n".join(sections)


def _read_xls(path: Path) -> str:
    # .xls 是旧式二进制格式，Windows 上优先使用已安装的 Microsoft Excel。
    try:
        import pythoncom  # type: ignore
        import win32com.client  # type: ignore
    except ImportError as exc:
        raise RuntimeError("读取 .xls 需要 Windows Microsoft Excel 或 pywin32") from exc

    pythoncom.CoInitialize()
    excel = None
    workbook = None
    try:
        excel = win32com.client.DispatchEx("Excel.Application")
        excel.Visible = False
        excel.DisplayAlerts = False
        excel.AutomationSecurity = 3
        workbook = excel.Workbooks.Open(str(path), ReadOnly=True, UpdateLinks=0)
        sections = []
        for sheet in workbook.Worksheets:
            values = sheet.UsedRange.Value
            if values is None:
                continue
            if not isinstance(values, tuple):
                values = ((values,),)
            rows = []
            for row in values:
                if not isinstance(row, tuple):
                    row = (row,)
                rows.append(row)
            formatted = _format_sheet_rows(rows)
            if formatted:
                sections.append("工作表：%s\n%s" % (sheet.Name, "\n".join(formatted)))
        return "\n\n".join(sections)
    finally:
        if workbook is not None:
            try:
                workbook.Close(False)
            except Exception:
                pass
        if excel is not None:
            try:
                excel.Quit()
            except Exception:
                pass
        pythoncom.CoUninitialize()


def _format_sheet_rows(rows: Iterable[Iterable]) -> List[str]:
    formatted = []
    for row in rows or ():
        cells = ["" if value is None else str(value).strip() for value in row]
        if any(cells):
            formatted.append("\t".join(cells).rstrip())
    return formatted


def _read_document(path: Path) -> str:
    suffix = path.suffix.lower()
    if suffix == ".pdf":
        return _read_pdf(path)
    if suffix == ".doc":
        return _read_doc(path)
    if suffix == ".docx":
        return _read_docx(path)
    if suffix == ".xlsx":
        return _read_xlsx(path)
    if suffix == ".xls":
        return _read_xls(path)
    return _read_text(path)


def _resolve(path_string: str) -> Path:
    path = Path(path_string).expanduser().resolve()
    if not path.is_file():
        raise RuntimeError("文件不存在")
    suffix = path.suffix.lower()
    if suffix not in SUPPORTED:
        raise RuntimeError("不支持的文件类型 %s" % (suffix or "(无扩展名)"))
    if path.stat().st_size > MAX_FILE_BYTES:
        raise RuntimeError("单个文件不能超过 10 MB")
    return path


def extract_content(path_string: str) -> Dict[str, str]:
    """Return ``{"kind": "image", ...}`` or ``{"kind": "text", ...}`` for one file."""
    path = _resolve(path_string)
    suffix = path.suffix.lower()
    if suffix in IMAGE_SUFFIXES:
        return {
            "kind": "image",
            "name": path.name,
            "media_type": IMAGE_MEDIA_TYPES[suffix],
            "path": str(path),
        }
    text = _read_document(path).strip()
    if not text:
        if suffix == ".pdf":
            raise RuntimeError("未提取到文字；扫描版 PDF 暂不支持 OCR")
        raise RuntimeError("文档中没有可读取的文字")
    return {"kind": "text", "name": path.name, "text": text, "path": str(path)}


# 图片编码结果按 (路径, mtime, 大小) 缓存：同一张图在一次对话的多轮请求里会被
# 反复取用（工具来回几轮都会重建 model_messages），每轮重新读盘 + base64 是纯浪费。
# 文件真变了或超出容量才失效；大图不进缓存，避免长跑进程把内存吃掉。
_IMAGE_CACHE: "OrderedDict[tuple, Dict[str, str]]" = OrderedDict()
_IMAGE_CACHE_MAX_ITEMS = 6
_IMAGE_CACHE_MAX_BYTES = 4_000_000  # 单张 base64 上限，约 3MB 原图
_IMAGE_CACHE_LOCK = threading.Lock()

# 尺寸解析结果同样按 (路径, mtime, 大小) 缓存：上下文预算每轮都要问一遍每张图的
# 尺寸，重复读盘解头是纯浪费。
_SIZE_CACHE: "OrderedDict[tuple, Optional[Tuple[int, int]]]" = OrderedDict()
_SIZE_CACHE_MAX_ITEMS = 32
_SIZE_READ_BYTES = 64 * 1024

# 视觉 token 估算取最贵的那条口径（Claude 的 28×28 patch）：预算宁高估勿低估，
# 高估最多让压缩早一点触发，低估会让真实请求悄悄顶出窗口被上游 400。
_VISUAL_PATCH = 28
_UNKNOWN_IMAGE_TOKENS = 1200  # 解析不出尺寸时按 OpenAI 高细节 1024×1024 兜底


def _decode_size(head: bytes) -> Optional[Tuple[int, int]]:
    """从图片文件头解析 (宽, 高)，不依赖 Pillow；认不出的格式返回 None。"""
    try:
        if head[:8] == b"\x89PNG\r\n\x1a\n" and head[12:16] == b"IHDR":
            return int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big")
        if head[:6] in (b"GIF87a", b"GIF89a"):
            return int.from_bytes(head[6:8], "little"), int.from_bytes(head[8:10], "little")
        if head[:2] == b"BM":
            return int.from_bytes(head[18:22], "little"), int.from_bytes(head[22:26], "little")
        if head[:4] == b"RIFF" and head[8:12] == b"WEBP":
            chunk = head[12:16]
            if chunk == b"VP8X":
                return (1 + int.from_bytes(head[24:27], "little"),
                        1 + int.from_bytes(head[27:30], "little"))
            if chunk == b"VP8 ":
                return (int.from_bytes(head[26:28], "little") & 0x3FFF,
                        int.from_bytes(head[28:30], "little") & 0x3FFF)
            if chunk == b"VP8L":
                bits = int.from_bytes(head[21:25], "little")
                return (bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1
        if head[:2] == b"\xff\xd8":
            return _jpeg_size(head)
    except (IndexError, ValueError):
        return None
    return None


def _jpeg_size(head: bytes) -> Optional[Tuple[int, int]]:
    """扫 SOF 段取 JPEG 尺寸；段长在 marker 之后，逐段跳过直到遇见 SOF。"""
    index = 2
    while index + 9 < len(head):
        if head[index] != 0xFF:
            index += 1
            continue
        marker = head[index + 1]
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            index += 2
            continue
        if marker == 0xD9:
            return None
        length = int.from_bytes(head[index + 2:index + 4], "big")
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                      0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            return (int.from_bytes(head[index + 7:index + 9], "big"),
                    int.from_bytes(head[index + 5:index + 7], "big"))
        index += 2 + length
    return None


def image_pixel_size(path_string: str) -> Optional[Tuple[int, int]]:
    """图片像素尺寸；路径不可用或格式认不出时返回 None。"""
    try:
        path = Path(path_string).expanduser().resolve()
        if path.suffix.lower() not in IMAGE_SUFFIXES or not path.is_file():
            return None
        stat = path.stat()
    except OSError:
        return None
    key = (str(path), stat.st_mtime_ns, stat.st_size)
    with _IMAGE_CACHE_LOCK:
        if key in _SIZE_CACHE:
            _SIZE_CACHE.move_to_end(key)
            return _SIZE_CACHE[key]
    try:
        with path.open("rb") as handle:
            head = handle.read(_SIZE_READ_BYTES)
    except OSError:
        return None
    size = _decode_size(head)
    with _IMAGE_CACHE_LOCK:
        _SIZE_CACHE[key] = size
        _SIZE_CACHE.move_to_end(key)
        while len(_SIZE_CACHE) > _SIZE_CACHE_MAX_ITEMS:
            _SIZE_CACHE.popitem(last=False)
    return size


def _send_size(width: int, height: int) -> Tuple[int, int]:
    """发送前实际送进 encoder 的尺寸；Pillow 缺失时不会缩放，原尺寸直传。"""
    if _PILImage is None:
        return width, height
    if max(width, height) <= MAX_IMAGE_LONG_EDGE and min(width, height) <= MAX_IMAGE_SHORT_EDGE:
        return width, height
    scale = min(
        MAX_IMAGE_LONG_EDGE / max(width, height),
        MAX_IMAGE_SHORT_EDGE / min(width, height),
    )
    return max(1, round(width * scale)), max(1, round(height * scale))


def _visual_tokens(width: int, height: int) -> int:
    across = (width + _VISUAL_PATCH - 1) // _VISUAL_PATCH
    down = (height + _VISUAL_PATCH - 1) // _VISUAL_PATCH
    return max(1, across * down)


def estimate_image_tokens(path_string: str) -> int:
    """一张图片附件在上下文里占用的视觉 token 估算值。

    按降采样后的尺寸算，而不是磁盘上的原图尺寸：真正计费的是缩放以后送进
    encoder 的那张图，两者混用会让预算高估 2~3 倍、压缩过早触发。
    """
    size = image_pixel_size(path_string)
    if size is None:
        return _UNKNOWN_IMAGE_TOKENS
    return _visual_tokens(*_send_size(*size))


def _resize_for_model(raw: bytes, max_bytes: int = None) -> Optional[Tuple[bytes, str]]:
    """尺寸或体积超限时重编码为 JPEG；无需处理、Pillow 缺失或结果更大时返回 None。

    两个触发条件互相独立：长边/短边超限要缩放，编码字节超预算只重编码（像素尺寸
    不变，视觉 token 也就不变）。压缩后反而变大时退回原图，宁可超一点预算。
    """
    limit = max_bytes or MAX_IMAGE_BASE64_BYTES
    size = _decode_size(raw[:_SIZE_READ_BYTES])
    if size is None or _PILImage is None:
        return None
    width, height = size
    within_dims = (max(width, height) <= MAX_IMAGE_LONG_EDGE
                   and min(width, height) <= MAX_IMAGE_SHORT_EDGE)
    if within_dims and len(raw) <= limit:
        return None
    try:
        image = _PILImage.open(io.BytesIO(raw))
        if image.mode != "RGB":
            image = image.convert("RGB")
        target = _send_size(width, height)
        if target != (width, height):
            resample = getattr(_PILImage, "Resampling", _PILImage).LANCZOS
            image = image.resize(target, resample)
        buffer = io.BytesIO()
        image.save(buffer, format="JPEG", quality=IMAGE_JPEG_QUALITY, optimize=True)
        data = buffer.getvalue()
    except Exception:
        return None
    if len(data) >= len(raw):
        return None
    return data, "image/jpeg"


def image_byte_budget(count: int) -> int:
    """本轮单张图片的编码字节预算：图越多分到的越少。

    对齐 openhanako 的 min(单图上限, 总预算 / 图片数)，让整请求不超总预算；
    下限兜住极端张数，避免预算被摊到压不出可用画质。
    """
    if count <= 0:
        return MAX_IMAGE_BASE64_BYTES
    return max(MIN_IMAGE_BASE64_BYTES,
               min(MAX_IMAGE_BASE64_BYTES, TOTAL_IMAGE_BASE64_BUDGET // count))


def images_to_send(messages: list, sent_paths: Iterable[str] = None) -> set:
    """本轮真正作为图片发送的 ``(消息序号, 图片路径)`` 集合。

    对齐 openhanako message-sanitizer 的"图片只活一轮"：图片只在到达它的那一轮
    发给模型，之后退化成文本占位。那条边界在 openhanako 里是"最后一条 assistant
    之后"，那里的图片挂在 toolResult 消息上；GridStar 的图片连同工具截图都挂在
    user 消息上，直接照搬会把刚截的图也剥掉，所以边界取"最后一条 user 消息"。

    ``sent_paths`` 拦住同一条消息里本轮已经发过的图：一次用户消息内的多轮工具
    调用会把截图不断追加到同一条 user 消息上，不加这道闸同一批图会随每轮请求
    重复发送。
    """
    already = set(sent_paths or ())
    last_user = -1
    for index, message in enumerate(messages):
        if isinstance(message, dict) and message.get("role") == "user":
            last_user = index
    keep = set()
    if last_user < 0:
        return keep
    for item in messages[last_user].get("attachments") or []:
        if isinstance(item, dict) and item.get("kind") == "image":
            path = str(item.get("path", ""))
            if path and path not in already:
                keep.add((last_user, path))
    return keep


def read_image_data(path_string: str, max_bytes: int = None) -> Optional[Dict[str, str]]:
    """Base64-encode a stored image at request time; return None when unusable.

    编码前先按长边/短边上限降采样：这张图随历史每轮重发、每轮计费，原图直传会
    让同一份成本反复支付。``max_bytes`` 是本轮分摊到的编码字节预算，不同预算会
    产出不同内容，所以进缓存键。
    """
    limit = max_bytes or MAX_IMAGE_BASE64_BYTES
    try:
        path = Path(path_string).expanduser().resolve()
        suffix = path.suffix.lower()
        if suffix not in IMAGE_SUFFIXES or not path.is_file():
            return None
        stat = path.stat()
        if stat.st_size > MAX_FILE_BYTES:
            return None
        key = (str(path), stat.st_mtime_ns, stat.st_size, limit)
        with _IMAGE_CACHE_LOCK:
            cached = _IMAGE_CACHE.get(key)
            if cached is not None:
                _IMAGE_CACHE.move_to_end(key)
                return cached
        raw = path.read_bytes()
        media_type = IMAGE_MEDIA_TYPES[suffix]
        resized = _resize_for_model(raw, limit)
        if resized is not None:
            raw, media_type = resized
        payload = {
            "media_type": media_type,
            "data": base64.b64encode(raw).decode("ascii"),
        }
        if len(payload["data"]) <= _IMAGE_CACHE_MAX_BYTES:
            with _IMAGE_CACHE_LOCK:
                _IMAGE_CACHE[key] = payload
                _IMAGE_CACHE.move_to_end(key)
                while len(_IMAGE_CACHE) > _IMAGE_CACHE_MAX_ITEMS:
                    _IMAGE_CACHE.popitem(last=False)
        return payload
    except OSError:
        return None


def load_attachments(attachments: Iterable[dict]) -> Tuple[List[dict], List[dict], List[str]]:
    """Split attachments into ``(documents, images, errors)`` for one request."""
    documents: List[dict] = []
    images: List[dict] = []
    errors: List[str] = []
    total_chars = 0
    for attachment in list(attachments or [])[:MAX_ATTACHMENTS]:
        if not isinstance(attachment, dict):
            continue
        try:
            item = extract_content(str(attachment.get("path", "")))
        except Exception as exc:
            errors.append("%s：%s" % (attachment.get("name", "文件"), exc))
            continue
        if item["kind"] == "image":
            if len(images) >= MAX_IMAGES:
                errors.append("%s：图片附件最多 %d 张" % (item["name"], MAX_IMAGES))
                continue
            images.append({
                "name": item["name"],
                "media_type": item["media_type"],
                "path": item["path"],
                "url": attachment.get("url", ""),
            })
            continue
        remaining = MAX_TOTAL_CHARS - total_chars
        if remaining <= 0:
            errors.append("附件文本总量超过限制")
            break
        text = item["text"][:remaining]
        documents.append({"name": item["name"], "text": text, "path": item["path"]})
        total_chars += len(text)
    return documents, images, errors

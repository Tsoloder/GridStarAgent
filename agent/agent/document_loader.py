"""Extract text from user-imported documents for one chat request."""
from pathlib import Path
from typing import Iterable, List, Tuple

MAX_FILE_BYTES = 10 * 1024 * 1024
MAX_TOTAL_CHARS = 120_000
SUPPORTED = {".doc", ".txt", ".pdf", ".md"}


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


def extract_text(path_string: str) -> Tuple[str, str]:
    path = Path(path_string).expanduser().resolve()
    if not path.is_file():
        raise RuntimeError("文件不存在")
    if path.suffix.lower() not in SUPPORTED:
        raise RuntimeError("仅支持 DOC、TXT、PDF、MD 文件")
    if path.stat().st_size > MAX_FILE_BYTES:
        raise RuntimeError("单个文件不能超过 10 MB")

    suffix = path.suffix.lower()
    if suffix in {".txt", ".md"}:
        text = _read_text(path)
    elif suffix == ".pdf":
        text = _read_pdf(path)
    else:
        text = _read_doc(path)
    text = text.strip()
    if not text:
        if suffix == ".pdf":
            raise RuntimeError("未提取到文字；扫描版 PDF 暂不支持 OCR")
        raise RuntimeError("文档中没有可读取的文字")
    return path.name, text


def load_attachments(attachments: Iterable[dict]) -> Tuple[List[dict], List[str]]:
    documents = []
    errors = []
    total_chars = 0
    for attachment in list(attachments or [])[:4]:
        try:
            name, text = extract_text(str(attachment.get("path", "")))
            remaining = MAX_TOTAL_CHARS - total_chars
            if remaining <= 0:
                errors.append("附件文本总量超过限制")
                break
            text = text[:remaining]
            documents.append({"name": name, "text": text})
            total_chars += len(text)
        except Exception as exc:
            errors.append("%s：%s" % (attachment.get("name", "文件"), exc))
    return documents, errors

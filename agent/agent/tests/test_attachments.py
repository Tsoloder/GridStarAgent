"""附件链路：上传落盘 → 文本/图片解析 → 会话持久化 → 三种协议发图。"""
import base64
import zipfile

import pytest
from fastapi.testclient import TestClient

import app as app_module
import document_loader
from document_loader import (
    MAX_ATTACHMENTS, MAX_FILE_BYTES, MAX_IMAGES, extract_content,
    load_attachments, read_image_data,
)
from llm_client.adapters.anthropic_messages import AnthropicMessagesAdapter
from llm_client.adapters.base import image_data_url
from llm_client.adapters.openai_chat import OpenAIChatAdapter
from llm_client.adapters.openai_responses import OpenAIResponsesAdapter
from llm_client.transform import MessageTransformer
from llm_client.types import ImageBlock, ModelConfig, TextBlock
from session import _stored_attachment

PNG_BYTES = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=="
)

DOCX_XML = (
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
    '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
    "<w:body>"
    "<w:p><w:r><w:t>馈线台账</w:t></w:r></w:p>"
    "<w:p><w:r><w:t>F101 线路</w:t></w:r><w:r><w:t> 全长 3.2km</w:t></w:r></w:p>"
    "</w:body></w:document>"
)


def _write_docx(path):
    with zipfile.ZipFile(str(path), "w") as archive:
        archive.writestr("word/document.xml", DOCX_XML)
    return path


# --- 文本类附件解析 ---

def test_plain_text_and_markdown_are_extracted(tmp_path):
    txt = tmp_path / "notes.txt"
    txt.write_bytes("三相不平衡报告\n".encode("gb18030"))
    markdown = tmp_path / "plan.md"
    markdown.write_bytes("# 方案\n- 步骤一".encode("utf-8"))

    first = extract_content(str(txt))
    second = extract_content(str(markdown))

    assert first["kind"] == "text" and first["text"] == "三相不平衡报告"
    assert second["text"] == "# 方案\n- 步骤一"


def test_csv_keeps_rows_as_is(tmp_path):
    table = tmp_path / "lines.csv"
    table.write_bytes("id,name\n7,F101\n".encode("utf-8"))

    assert extract_content(str(table))["text"] == "id,name\n7,F101"


def test_docx_is_parsed_without_word(tmp_path):
    result = extract_content(str(_write_docx(tmp_path / "report.docx")))

    assert result["kind"] == "text"
    assert result["text"] == "馈线台账\nF101 线路 全长 3.2km"


def test_xlsx_lists_every_sheet(tmp_path):
    openpyxl = pytest.importorskip("openpyxl")
    workbook = openpyxl.Workbook()
    workbook.active.title = "台账"
    workbook.active.append(["id", "name"])
    workbook.active.append([7, "F101"])
    workbook.active.append([None, None])
    extra = workbook.create_sheet("空表")
    extra.append([None])
    target = tmp_path / "grid.xlsx"
    workbook.save(str(target))

    text = extract_content(str(target))["text"]

    assert text == "工作表：台账\nid\tname\n7\tF101"


def test_unsupported_suffix_and_oversize_are_rejected(tmp_path):
    exe = tmp_path / "tool.exe"
    exe.write_bytes(b"MZ")
    big = tmp_path / "big.txt"
    big.write_bytes(b"a" * (MAX_FILE_BYTES + 1))

    with pytest.raises(RuntimeError, match="不支持的文件类型"):
        extract_content(str(exe))
    with pytest.raises(RuntimeError, match="10 MB"):
        extract_content(str(big))
    with pytest.raises(RuntimeError, match="文件不存在"):
        extract_content(str(tmp_path / "missing.txt"))


# --- 图片类附件 ---

def test_image_attachment_keeps_only_a_reference(tmp_path):
    picture = tmp_path / "grid.png"
    picture.write_bytes(PNG_BYTES)

    item = extract_content(str(picture))
    payload = read_image_data(str(picture))

    assert item["kind"] == "image" and item["media_type"] == "image/png"
    assert "text" not in item
    assert payload == {"media_type": "image/png",
                       "data": base64.b64encode(PNG_BYTES).decode("ascii")}


def test_read_image_data_returns_none_for_unusable_paths(tmp_path):
    text_file = tmp_path / "notes.txt"
    text_file.write_text("hello", encoding="utf-8")

    assert read_image_data(str(text_file)) is None
    assert read_image_data(str(tmp_path / "missing.png")) is None


def test_load_attachments_splits_documents_images_and_errors(tmp_path):
    document = tmp_path / "notes.md"
    document.write_text("# 台账", encoding="utf-8")
    picture = tmp_path / "grid.png"
    picture.write_bytes(PNG_BYTES)
    broken = tmp_path / "broken.exe"
    broken.write_bytes(b"MZ")

    documents, images, errors = load_attachments([
        {"name": "notes.md", "path": str(document)},
        {"name": "grid.png", "path": str(picture), "url": "/uploads/grid.png"},
        {"name": "broken.exe", "path": str(broken)},
        "not-a-dict",
    ])

    assert [item["name"] for item in documents] == ["notes.md"]
    assert documents[0]["text"] == "# 台账"
    assert images == [{"name": "grid.png", "media_type": "image/png",
                       "path": str(picture), "url": "/uploads/grid.png"}]
    assert len(errors) == 1 and "broken.exe" in errors[0]


def test_load_attachments_enforces_image_and_total_limits(tmp_path, monkeypatch):
    pictures = []
    for index in range(MAX_IMAGES + 1):
        picture = tmp_path / ("grid%d.png" % index)
        picture.write_bytes(PNG_BYTES)
        pictures.append({"name": picture.name, "path": str(picture)})

    documents, images, errors = load_attachments(pictures)

    assert len(images) == MAX_IMAGES and not documents
    assert errors == ["grid%d.png：图片附件最多 %d 张" % (MAX_IMAGES, MAX_IMAGES)]

    long_text = tmp_path / "long.txt"
    long_text.write_text("a" * 100, encoding="utf-8")
    monkeypatch.setattr(document_loader, "MAX_TOTAL_CHARS", 10)
    documents, images, errors = load_attachments(
        [{"name": "long.txt", "path": str(long_text)}] * MAX_ATTACHMENTS
    )

    assert len(documents) == 1 and len(documents[0]["text"]) == 10
    assert errors == ["附件文本总量超过限制"]


# --- 会话持久化 ---

def test_stored_attachment_drops_base64_and_keeps_replay_fields():
    image = _stored_attachment({"kind": "image", "name": "grid.png", "media_type": "image/png",
                                "path": "C:/data/uploads/grid.png", "url": "/uploads/grid.png",
                                "text": "ignored"})
    document = _stored_attachment({"kind": "text", "name": "notes.md", "text": "# 台账",
                                   "path": "C:/data/uploads/notes.md"})

    assert image == {"name": "grid.png", "kind": "image", "media_type": "image/png",
                     "path": "C:/data/uploads/grid.png", "url": "/uploads/grid.png"}
    assert document == {"name": "notes.md", "kind": "text", "text": "# 台账"}


# --- /upload 接口 ---

@pytest.fixture
def upload_client(tmp_path, monkeypatch):
    """把上传目录与 /uploads 静态挂载都指向 tmp_path，避免测试污染真实数据目录。"""
    from starlette.staticfiles import StaticFiles

    monkeypatch.setattr(app_module, "UPLOADS_DIR", tmp_path)
    for route in app_module.app.router.routes:
        if getattr(route, "name", "") == "uploads":
            monkeypatch.setattr(route, "app", StaticFiles(directory=str(tmp_path)))
    return TestClient(app_module.app)


def test_upload_stores_file_and_returns_preview_url(tmp_path, upload_client):
    response = upload_client.post("/upload?name=grid.png", content=PNG_BYTES)

    assert response.status_code == 200
    data = response.json()
    assert data["kind"] == "image" and data["media_type"] == "image/png"
    assert data["size"] == len(PNG_BYTES)
    assert data["url"].startswith("/uploads/") and data["url"].endswith(".png")
    stored = list(tmp_path.iterdir())
    assert len(stored) == 1 and stored[0].read_bytes() == PNG_BYTES
    # 落盘文件名不带原始名称，避免路径穿越与覆盖
    assert data["name"] == "grid.png" and stored[0].name != "grid.png"


def test_upload_rejects_unsupported_empty_and_oversize(tmp_path, upload_client):
    assert upload_client.post("/upload?name=tool.exe", content=b"MZ").status_code == 400
    assert upload_client.post("/upload?name=notes.txt", content=b"").status_code == 400
    assert upload_client.post("/upload?name=big.txt",
                              content=b"a" * (MAX_FILE_BYTES + 1)).status_code == 400
    assert list(tmp_path.iterdir()) == []


def test_uploaded_file_is_served_for_preview(upload_client):
    url = upload_client.post("/upload?name=notes.txt", content="台账".encode("utf-8")).json()["url"]

    preview = upload_client.get(url)

    assert preview.status_code == 200
    assert preview.content.decode("utf-8") == "台账"


# --- 图片内容块转换为各协议载荷 ---

def _model(api):
    return ModelConfig("test-model", "vendor", api)


def test_from_legacy_converts_image_parts_to_blocks():
    messages = MessageTransformer().from_legacy([
        {"role": "user", "content": [
            {"type": "text", "text": "看看这张图"},
            {"type": "image", "media_type": "image/png", "data": "AAAA"},
            {"type": "image", "data": ""},
            "not-a-dict",
        ]},
    ])

    blocks = messages[0].content

    assert isinstance(blocks[0], TextBlock) and blocks[0].text == "看看这张图"
    assert isinstance(blocks[1], ImageBlock)
    assert blocks[1].source == "AAAA" and blocks[1].media_type == "image/png"
    assert len(blocks) == 2


def test_image_data_url_passes_through_remote_sources():
    assert image_data_url(ImageBlock(source="AAAA", media_type="image/jpeg")) == "data:image/jpeg;base64,AAAA"
    assert image_data_url(ImageBlock(source="AAAA")) == "data:image/png;base64,AAAA"
    assert image_data_url(ImageBlock(source="https://example.test/a.png")) == "https://example.test/a.png"


def test_openai_chat_sends_image_url_parts_only_when_images_exist():
    plain = MessageTransformer().from_legacy([{"role": "user", "content": "只有文字"}])
    multimodal = MessageTransformer().from_legacy([{"role": "user", "content": [
        {"type": "text", "text": "看图"}, {"type": "image", "media_type": "image/png", "data": "AAAA"}]}])

    plain_body = OpenAIChatAdapter().build_request(_model("openai-chat"), plain, [])
    body = OpenAIChatAdapter().build_request(_model("openai-chat"), multimodal, [])

    assert plain_body["messages"][0]["content"] == "只有文字"
    assert body["messages"][0]["content"] == [
        {"type": "text", "text": "看图"},
        {"type": "image_url", "image_url": {"url": "data:image/png;base64,AAAA"}},
    ]


def test_openai_chat_keeps_image_only_message_without_empty_text_part():
    messages = MessageTransformer().from_legacy([{"role": "user", "content": [
        {"type": "image", "media_type": "image/png", "data": "AAAA"}]}])

    body = OpenAIChatAdapter().build_request(_model("openai-chat"), messages, [])

    assert body["messages"][0]["content"] == [
        {"type": "image_url", "image_url": {"url": "data:image/png;base64,AAAA"}}]


def test_anthropic_sends_base64_image_source():
    messages = MessageTransformer().from_legacy([{"role": "user", "content": [
        {"type": "text", "text": "看图"}, {"type": "image", "media_type": "image/jpeg", "data": "AAAA"}]}])

    body = AnthropicMessagesAdapter().build_request(_model("anthropic-messages"), messages, [])

    assert body["messages"][0]["content"] == [
        {"type": "text", "text": "看图"},
        {"type": "image", "source": {"type": "base64", "media_type": "image/jpeg", "data": "AAAA"}},
    ]


def test_openai_responses_sends_input_image_parts():
    messages = MessageTransformer().from_legacy([{"role": "user", "content": [
        {"type": "text", "text": "看图"}, {"type": "image", "media_type": "image/png", "data": "AAAA"}]}])

    body = OpenAIResponsesAdapter().build_request(_model("openai-responses"), messages, [])

    assert body["input"][0]["content"] == [
        {"type": "input_text", "text": "看图"},
        {"type": "input_image", "image_url": "data:image/png;base64,AAAA"},
    ]

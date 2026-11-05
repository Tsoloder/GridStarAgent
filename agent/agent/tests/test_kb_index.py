"""知识库检索内核的单元测试。

只用临时目录里造的小文档，不依赖真实手册：验证切块、检索、参数抽取与读回
这几条链路本身是通的。真实语料的召回质量另有手工试跑脚本，不放进这里。
"""
import pytest

import kb_index


@pytest.fixture()
def kb(tmp_path):
    """把索引指向临时库，测试结束后切回原库，避免污染真实数据目录。"""
    original = kb_index.KB_DB_PATH
    kb_index.configure(tmp_path / "kb.db")
    try:
        yield tmp_path
    finally:
        kb_index.configure(original)


def _write(tmp_path, name, text):
    path = tmp_path / name
    path.write_text(text, encoding="utf-8")
    return path


def test_index_and_search_markdown(kb):
    path = _write(
        kb,
        "手册.md",
        "# 网格生成\n\n## 附面层推进\n"
        "附面层推进用于沿物面法向逐层生成网格单元。\n\n"
        "首层高度：紧贴物面的第一层网格的厚度，直接决定壁面 y+ 值。\n\n"
        "增长率：相邻两层网格厚度之比，通常增长率在 1.1-1.3 之间。\n\n"
        "## 设置边界\n新建组：创建新的边界条件分组，支持修改名字与属性。\n",
    )

    result = kb_index.index_file(path)
    assert result["status"] == "ok"
    assert result["chunks"] > 0

    hits = kb_index.search("附面层 首层高度", top_k=3)
    assert hits, "应当命中附面层推进那一节"
    assert "附面层" in hits[0]["heading_path"]

    # 章节标题命中要能被抬上来：问「设置边界」不该先返回附面层
    boundary = kb_index.search("设置边界", top_k=1)
    assert boundary and "设置边界" in boundary[0]["heading_path"]


def test_read_chunk_returns_original_text(kb):
    path = _write(
        kb,
        "手册.md",
        "# 一\n\n首层高度：紧贴物面的第一层网格的厚度。\n",
    )
    kb_index.index_file(path)
    hit = kb_index.search("首层高度", top_k=1)[0]
    payload = kb_index.read_chunk(hit["chunk_id"])
    assert payload is not None
    assert "紧贴物面的第一层网格的厚度" in payload["text"]
    assert payload["doc"] == "手册.md"
    assert payload["page"] >= 1


def test_param_extraction_with_range_and_default(kb):
    path = _write(
        kb,
        "参数.md",
        "## 附面层推进\n\n"
        "增长率：是相邻两层网格厚度之比。增长率越接近1越平滑，通常增长率在 1.1-1.3 之间。\n\n"
        "网格平滑因子：取值范围控制在 (1, 1.5]，默认值为 1.2。\n\n"
        "复制：把选中的网格线复制一份。\n",
    )
    kb_index.index_file(path)

    rows = {row["name"]: row for row in kb_index.find_params("增长率")}
    assert "增长率" in rows
    assert rows["增长率"]["min"] == pytest.approx(1.1)
    assert rows["增长率"]["max"] == pytest.approx(1.3)

    smooth = kb_index.find_params("网格平滑因子")
    assert smooth and smooth[0]["max"] == pytest.approx(1.5)

    # 「复制」是按钮不是参数：抽取规则必须把它丢掉
    assert not kb_index.find_params("复制")


def test_missing_content_says_so(kb):
    path = _write(kb, "手册.md", "# 目录\n\n这里只有目录内容。\n")
    kb_index.index_file(path)
    assert kb_index.search("转子发动机点火提前角") == []


# --------------------------------------------------------------------------
# 静默失效：入库环节不能把「什么都没提到」记成「已收录」
# --------------------------------------------------------------------------


def test_empty_document_is_marked_failed(kb):
    """抽不出文字的文档不能记成 ok。

    扫描版 PDF 每页 extract_text() 都是空串，以前照样写 status='ok'，
    界面上看着已收录，问到它只回「未收录」，没人会想到是导入环节丢了内容。
    """
    path = _write(kb, "扫描件.md", "")
    result = kb_index.index_file(path)
    assert result["status"] == "failed"
    assert "文本" in result["error"]
    assert result["chunks"] == 0

    docs = kb_index.list_documents()
    assert docs[0]["status"] == "failed"
    assert docs[0]["error"]

    state = kb_index.status()
    assert state["failed"] == 1
    assert state["documents"] == 0


def test_blank_whitespace_document_is_marked_failed(kb):
    path = _write(kb, "空白.md", "   \n\n  \t \n")
    assert kb_index.index_file(path)["status"] == "failed"


def test_source_change_marks_document_stale(kb):
    """源文件改了却不重导：必须自己标出来，不能继续拿旧内容当最新。"""
    path = _write(kb, "手册.md", "# 网格\n\n首层高度：第一层厚度为 0.01。\n")
    kb_index.index_file(path)
    assert kb_index.search("首层高度")[0]["stale"] is False

    _write(kb, "手册.md", "# 网格\n\n首层高度：第一层厚度改成了 0.02，并且加了一句更长的说明。\n")
    report = kb_index.refresh_stale()
    assert len(report["marked"]) == 1
    assert report["marked"][0]["name"] == "手册.md"

    assert kb_index.list_documents()[0]["status"] == "stale"
    assert kb_index.status()["stale"] == 1

    # 库里是旧版，但仍要能检索到：查不到比给旧内容更糟，区别靠 stale 标记告知
    hits = kb_index.search("首层高度")
    assert hits and hits[0]["stale"] is True
    assert kb_index.read_chunk(hits[0]["chunk_id"])["stale"] is True

    # 重新导入后自动回到 ok
    kb_index.index_file(path)
    assert kb_index.list_documents()[0]["status"] == "ok"
    assert kb_index.status()["stale"] == 0
    assert kb_index.search("首层高度")[0]["stale"] is False


def test_missing_source_file_marks_stale(kb):
    path = _write(kb, "临时.md", "# 甲\n\n甲内容。\n")
    kb_index.index_file(path)
    path.unlink()

    report = kb_index.refresh_stale()
    assert report["marked"] and "不存在" in report["marked"][0]["reason"]
    assert kb_index.list_documents()[0]["status"] == "stale"
    # 源文件没了，库里的快照依旧可查
    assert kb_index.search("甲内容")


def test_stale_documents_follow_library_scope(kb):
    second = kb_index.create_library("别动这个库")
    keep = _write(kb, "a.md", "# 甲\n\n甲内容。\n")
    touch = _write(kb, "b.md", "# 乙\n\n乙内容。\n")
    kb_index.index_file(keep, library_id=kb_index.DEFAULT_LIBRARY_ID)
    kb_index.index_file(touch, library_id=second["id"])

    _write(kb, "b.md", "# 乙\n\n乙内容换了。\n")
    report = kb_index.refresh_stale(second["id"])
    assert [item["name"] for item in report["marked"]] == ["b.md"]
    assert kb_index.list_documents(kb_index.DEFAULT_LIBRARY_ID)[0]["status"] == "ok"


def test_default_value_with_scientific_notation(kb):
    """手册写作「默认公差为1*e‑7」，那个 ‑ 是 U+2011，不能因此把默认值丢掉。"""
    path = _write(
        kb,
        "系统参数.md",
        "## 系统参数\n\n系统公差：为软件的默认公差， 默认公差为1*e\u20117。\n",
    )
    kb_index.index_file(path)
    rows = kb_index.find_params("系统公差")
    assert rows, "系统公差应当被识别为参数"
    assert rows[0]["default"] == "1*e\u20117"


def test_status_counts(kb):
    path = _write(kb, "a.md", "# 甲\n\n内容甲。\n")
    kb_index.index_file(path)
    state = kb_index.status()
    assert state["available"] is True
    assert state["documents"] == 1
    assert state["chunks"] >= 1


def test_reindex_replaces_old_chunks(kb):
    path = _write(kb, "b.md", "# 乙\n\n第一次内容。\n")
    first = kb_index.index_file(path)
    _write(kb, "b.md", "# 乙\n\n第二次内容，换掉了。\n")
    second = kb_index.index_file(path)
    assert first["doc_id"] == second["doc_id"]
    docs = kb_index.list_documents()
    assert len(docs) == 1
    assert docs[0]["chunks"] == second["chunks"]


def test_route_to_rescue_when_ktools_missing():
    """工具层在索引缺失时必须给出可读提示，不能抛异常打断对话。"""
    import kb_tools

    out = kb_tools.execute("kb_read", {"chunk_id": "not-an-int"})
    assert "整数" in out

    out = kb_tools.execute("kb_catalog", {})
    assert isinstance(out, str) and out


# --------------------------------------------------------------------------
# 多知识库
# --------------------------------------------------------------------------


def test_default_library_is_bootstrapped(kb):
    libraries = kb_index.list_libraries()
    assert libraries, "空库启动时应当自动建一个默认库"
    assert libraries[0]["id"] == kb_index.DEFAULT_LIBRARY_ID


def test_documents_are_isolated_between_libraries(kb):
    second = kb_index.create_library("导弹规范")
    path_a = _write(kb, "a.md", "# 机翼\n\n首层高度：机翼的第一层厚度。\n")
    path_b = _write(kb, "b.md", "# 导弹\n\n首层高度：导弹舵前的第一层厚度。\n")
    kb_index.index_file(path_a, library_id=kb_index.DEFAULT_LIBRARY_ID)
    kb_index.index_file(path_b, library_id=second["id"])

    default_hits = kb_index.search("首层高度", library_id=kb_index.DEFAULT_LIBRARY_ID)
    assert default_hits and all("机翼" in hit["heading_path"] for hit in default_hits)

    second_hits = kb_index.search("首层高度", library_id=second["id"])
    assert second_hits and all("导弹" in hit["heading_path"] for hit in second_hits)

    # 跨库检索（全部）两边都应出现
    all_hits = kb_index.search("首层高度", library_id=kb_index.LIBRARY_ALL, top_k=10, max_per_doc=5)
    headings = " ".join(hit["heading_path"] for hit in all_hits)
    assert "机翼" in headings and "导弹" in headings


def test_same_file_can_live_in_two_libraries(kb):
    second = kb_index.create_library("副本库")
    path = _write(kb, "manual.md", "# 手册\n\n增长率：相邻两层之比。\n")
    first = kb_index.index_file(path, library_id=kb_index.DEFAULT_LIBRARY_ID)
    again = kb_index.index_file(path, library_id=second["id"])
    assert first["doc_id"] != again["doc_id"], "同一文件在两个库应当是两条独立记录"
    assert len(kb_index.list_documents(kb_index.DEFAULT_LIBRARY_ID)) == 1
    assert len(kb_index.list_documents(second["id"])) == 1


def test_read_chunk_respects_library_scope(kb):
    second = kb_index.create_library("只看这里")
    path = _write(kb, "x.md", "# 边界\n\n设置边界：新建组并指定类型。\n")
    kb_index.index_file(path, library_id=second["id"])
    hit = kb_index.search("设置边界", library_id=second["id"])[0]

    assert kb_index.read_chunk(hit["chunk_id"], library_id=second["id"]) is not None
    assert kb_index.read_chunk(hit["chunk_id"], library_id=kb_index.DEFAULT_LIBRARY_ID) is None


def test_params_follow_library_scope(kb):
    second = kb_index.create_library("只这里的参数")
    path = _write(kb, "p.md", "# 附面层\n\n增长率：相邻两层之比，范围 1.1-1.3。\n")
    kb_index.index_file(path, library_id=second["id"])

    assert kb_index.find_params("增长率", library_id=second["id"])
    assert not kb_index.find_params("增长率", library_id=kb_index.DEFAULT_LIBRARY_ID)


def test_create_and_rename_library(kb):
    created = kb_index.create_library("飞机手册")
    assert created["name"] == "飞机手册"
    assert kb_index.get_library(created["id"])["name"] == "飞机手册"

    kb_index.rename_library(created["id"], "飞机手册 v2")
    assert kb_index.get_library(created["id"])["name"] == "飞机手册 v2"

    with pytest.raises(kb_index.KBUnavailable):
        kb_index.create_library("")
    with pytest.raises(kb_index.KBUnavailable):
        kb_index.create_library("飞机手册 v2")


def test_delete_library_removes_its_documents_only(kb):
    second = kb_index.create_library("待删库")
    keep = _write(kb, "keep.md", "# 保留\n\n保留内容。\n")
    drop = _write(kb, "drop.md", "# 删除\n\n即将被删的内容。\n")
    kb_index.index_file(keep, library_id=kb_index.DEFAULT_LIBRARY_ID)
    kb_index.index_file(drop, library_id=second["id"])

    result = kb_index.delete_library(second["id"])
    assert result["documents"] == 1
    assert kb_index.get_library(second["id"]) is None
    assert len(kb_index.list_documents(kb_index.DEFAULT_LIBRARY_ID)) == 1
    assert kb_index.search("即将被删", library_id=kb_index.LIBRARY_ALL) == []


def test_delete_last_library_recreates_default(kb):
    kb_index.delete_library(kb_index.DEFAULT_LIBRARY_ID)
    libraries = kb_index.list_libraries()
    assert libraries and libraries[0]["id"] == kb_index.DEFAULT_LIBRARY_ID


def test_tools_report_disabled_library():
    """选了「不使用知识库」时，三个工具都要明说未启用，不能静默返回空。"""
    import kb_tools

    token = kb_tools.set_active_library("")
    try:
        for name, args in (("kb_search", {"query": "首层高度"}),
                           ("kb_read", {"chunk_id": 1}),
                           ("kb_catalog", {})):
            out = kb_tools.execute(name, args)
            assert "没有启用知识库" in out
    finally:
        kb_tools.reset_active_library(token)


# --------------------------------------------------------------------------
# 文件夹（只做归档，不影响检索）
# --------------------------------------------------------------------------


def test_folder_tree_and_document_counts(kb):
    top = kb_index.create_folder(kb_index.DEFAULT_LIBRARY_ID, "结构模块")
    child = kb_index.create_folder(kb_index.DEFAULT_LIBRARY_ID, "表面网格", top["id"])
    path = _write(kb, "a.md", "# 甲\n\n甲内容。\n")
    kb_index.index_file(path, library_id=kb_index.DEFAULT_LIBRARY_ID, folder_id=child["id"])

    tree = kb_index.folder_tree(kb_index.DEFAULT_LIBRARY_ID)
    assert tree["name"] == "库根"
    assert len(tree["children"]) == 1
    assert tree["children"][0]["name"] == "结构模块"
    assert tree["children"][0]["children"][0]["documents"] == 1
    # 父级 total 要把子级算进去，否则折叠状态下看不出里面有没有东西
    assert tree["children"][0]["total"] == 1
    assert tree["total"] == 1


def test_folder_does_not_change_retrieval_scope(kb):
    """文件夹只管摆，搜全库时不同文件夹的文件都能命中。"""
    folder = kb_index.create_folder(kb_index.DEFAULT_LIBRARY_ID, "归档区")
    inside = _write(kb, "in.md", "# 甲\n\n舵缝先用缝隙填充补面。\n")
    outside = _write(kb, "out.md", "# 乙\n\n舵缝处理前先检查间隙。\n")
    kb_index.index_file(inside, library_id=kb_index.DEFAULT_LIBRARY_ID, folder_id=folder["id"])
    kb_index.index_file(outside, library_id=kb_index.DEFAULT_LIBRARY_ID)

    hits = kb_index.search("舵缝", library_id=kb_index.DEFAULT_LIBRARY_ID, top_k=5, max_per_doc=5)
    docs = {hit["doc"] for hit in hits}
    assert {"in.md", "out.md"} <= docs


def test_list_documents_by_folder(kb):
    folder = kb_index.create_folder(kb_index.DEFAULT_LIBRARY_ID, "结构")
    child = kb_index.create_folder(kb_index.DEFAULT_LIBRARY_ID, "表面", folder["id"])
    root_doc = _write(kb, "root.md", "# 根\n\n根。\n")
    sub_doc = _write(kb, "sub.md", "# 子\n\n子。\n")
    deep_doc = _write(kb, "deep.md", "# 深\n\n深。\n")
    kb_index.index_file(root_doc, library_id=kb_index.DEFAULT_LIBRARY_ID)
    kb_index.index_file(sub_doc, library_id=kb_index.DEFAULT_LIBRARY_ID, folder_id=folder["id"])
    kb_index.index_file(deep_doc, library_id=kb_index.DEFAULT_LIBRARY_ID, folder_id=child["id"])

    all_docs = kb_index.list_documents(kb_index.DEFAULT_LIBRARY_ID)
    assert len(all_docs) == 3
    root_only = kb_index.list_documents(kb_index.DEFAULT_LIBRARY_ID, folder_id="")
    assert [d["name"] for d in root_only] == ["root.md"]
    one_level = kb_index.list_documents(kb_index.DEFAULT_LIBRARY_ID, folder_id=folder["id"])
    assert [d["name"] for d in one_level] == ["sub.md"]
    deep = kb_index.list_documents(kb_index.DEFAULT_LIBRARY_ID, folder_id=folder["id"], deep=True)
    assert {"sub.md", "deep.md"} == {d["name"] for d in deep}
    assert one_level[0]["folder_path"] == "结构"


def test_move_document_and_delete_folder_moves_up(kb):
    folder = kb_index.create_folder(kb_index.DEFAULT_LIBRARY_ID, "暂存")
    path = _write(kb, "x.md", "# 叉\n\n内容。\n")
    doc = kb_index.index_file(path, library_id=kb_index.DEFAULT_LIBRARY_ID)

    kb_index.move_document(doc["doc_id"], folder["id"])
    assert kb_index.list_documents(kb_index.DEFAULT_LIBRARY_ID, folder_id=folder["id"])[0]["name"] == "x.md"

    removed = kb_index.delete_folder(folder["id"])
    assert removed["moved_documents"] == 1
    # 删文件夹不能连带删资料
    assert len(kb_index.list_documents(kb_index.DEFAULT_LIBRARY_ID)) == 1
    assert kb_index.list_documents(kb_index.DEFAULT_LIBRARY_ID, folder_id="")[0]["name"] == "x.md"


def test_folder_name_conflict_and_empty(kb):
    kb_index.create_folder(kb_index.DEFAULT_LIBRARY_ID, "手册")
    with pytest.raises(kb_index.KBUnavailable):
        kb_index.create_folder(kb_index.DEFAULT_LIBRARY_ID, "手册")
    with pytest.raises(kb_index.KBUnavailable):
        kb_index.create_folder(kb_index.DEFAULT_LIBRARY_ID, "   ")


def test_import_mirrors_directory_tree(kb, tmp_path):
    root = tmp_path / "资料"
    (root / "导弹").mkdir(parents=True)
    (root / "飞机" / "机翼").mkdir(parents=True)
    (root / "导弹" / "舵面.md").write_text("# 舵面\n\n舵面内容。\n", encoding="utf-8")
    (root / "飞机" / "机翼" / "前缘.md").write_text("# 前缘\n\n前缘内容。\n", encoding="utf-8")

    report = kb_index.import_paths([str(root)], library_id=kb_index.DEFAULT_LIBRARY_ID)
    assert len(report["indexed"]) == 2

    tree = kb_index.folder_tree(kb_index.DEFAULT_LIBRARY_ID)
    names = {child["name"]: child for child in tree["children"]}
    assert set(names) == {"导弹", "飞机"}
    assert names["导弹"]["documents"] == 1
    assert names["飞机"]["children"][0]["name"] == "机翼"
    assert names["飞机"]["children"][0]["documents"] == 1


def test_tool_search_respects_active_library(kb):
    import kb_tools

    second = kb_index.create_library("激活库")
    path = _write(kb, "active.md", "# 专用\n\n缝隙填充：用于能缝区域。\n")
    kb_index.index_file(path, library_id=second["id"])

    token = kb_tools.set_active_library(second["id"])
    try:
        out = kb_tools.execute("kb_search", {"query": "缝隙填充", "top_k": 3})
        assert "缝隙填充" in out
    finally:
        kb_tools.reset_active_library(token)

    # 恢复默认（未设置）后按全部库检索，也应看得到
    token = kb_tools.set_active_library(None)
    try:
        out = kb_tools.execute("kb_search", {"query": "缝隙填充", "top_k": 3})
        assert "缝隙填充" in out
    finally:
        kb_tools.reset_active_library(token)


# --------------------------------------------------------------------------
# 2026-11 优化：参数去重、绝对分、降级路径、章节类型、源文件回收、检索日志
# --------------------------------------------------------------------------


def test_find_params_merges_same_name_across_sections(kb):
    """同名参数跨章节合并成一条：limit=3 不该被同一个参数占满。"""
    path = _write(
        kb,
        "手册.md",
        "# 网格生成\n\n## 附面层推进\n\n"
        "首层高度：紧贴物面的第一层网格厚度，默认值为 0.01 mm，范围 0.001-0.1。\n\n"
        "## 附面层参数\n\n"
        "首层高度：第一层网格高度，直接决定壁面 y+ 值。\n\n"
        "## 附面层设置\n\n"
        "首层高度：影响附面层分辨率与总层数，取值过大时边界层网格会明显变稀。\n",
    )
    kb_index.index_file(path)

    rows = kb_index.find_params("附面层首层高度")
    assert [row["name"] for row in rows] == ["首层高度"]
    merged = rows[0]
    assert merged["source_count"] >= 3
    assert len(merged["sources"]) == 3
    assert len({item["heading_path"] for item in merged["sources"]}) == 3
    # 主记录按描述最长选，补值按组内第一个非空取
    assert merged["default"] == "0.01"
    assert merged["min"] == pytest.approx(0.001)
    assert merged["max"] == pytest.approx(0.1)


def test_find_params_backfills_fields_from_group(kb):
    """主记录按描述长度选出，很可能恰好不带默认值；不补就等于合并后丢值。"""
    path = _write(
        kb,
        "参数.md",
        "# 网格\n\n## 参数表\n\n"
        "首层高度：默认值为 0.005 mm。\n\n"
        "## 操作步骤\n\n"
        "首层高度：第一层网格单元的高度，直接决定壁面 y+ 值，影响附面层分辨率的设置精度。\n",
    )
    kb_index.index_file(path)

    merged = kb_index.find_params("首层高度")[0]
    assert merged["source_count"] == 2
    assert "附面层分辨率" in merged["description"]
    assert merged["default"] == "0.005"


def test_find_params_sources_dedupe_and_sort_by_page(kb):
    """sources 按 (doc, heading, page) 去重、按页码升序，最多 3 条。"""
    import sqlite3

    path = _write(kb, "a.md", "# 网格\n\n## 甲\n\n首层高度：紧贴物面的第一层厚度。\n")
    doc_id = kb_index.index_file(path)["doc_id"]

    conn = sqlite3.connect(str(kb / "kb.db"))
    try:
        for heading, page in (("网格 / 甲", 1), ("网格 / 乙", 5), ("网格 / 丙", 3), ("网格 / 丁", 9)):
            conn.execute(
                "INSERT INTO kb_param (name, domain, module, default_value, min_value,"
                " max_value, unit, description, doc_id, heading_path, page)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                ("首层高度", "", "", "", None, None, "", "重复出处", doc_id, heading, page),
            )
        conn.commit()
    finally:
        conn.close()

    merged = kb_index.find_params("首层高度")[0]
    assert merged["source_count"] == 5
    # 甲 页被索引记录与手工记录各写一次，去重后只算一条
    assert [(item["page"], item["heading_path"]) for item in merged["sources"]] == [
        (1, "网格 / 甲"),
        (3, "网格 / 丙"),
        (5, "网格 / 乙"),
    ]


def test_find_params_limit_counts_merged_groups(kb):
    path = _write(
        kb,
        "两个参数.md",
        "# 网格\n\n## 甲\n\n首层高度：紧贴物面的第一层厚度。\n\n"
        "## 乙\n\n首层高度：第一层高度，默认值为 0.01。\n\n"
        "## 丙\n\n增长率：相邻两层厚度之比，范围 1.1-1.3。\n",
    )
    kb_index.index_file(path)

    assert len(kb_index.find_params("首层高度 增长率", limit=1)) == 1
    names = [row["name"] for row in kb_index.find_params("首层高度 增长率", limit=5)]
    assert names == ["首层高度", "增长率"]


def test_coverage_is_absolute_and_self_consistent(kb):
    """coverage 是绝对量：含全词的块必须明显高于只含一个词的块。"""
    full = _write(
        kb,
        "full.md",
        "# 几何\n\n## 导入\n\n几何导入支持 STEP、IGES 与 Parasolid 三种格式，导入后自动缝合。\n",
    )
    part = _write(kb, "part.md", "# 其他\n\n## 杂项\n\n这里只提到格式这两个字，别的不谈。\n")
    kb_index.index_file(full)
    kb_index.index_file(part)

    hits = kb_index.search("几何导入支持什么格式", top_k=5, max_per_doc=5)
    assert hits[0]["doc"] == "full.md"
    # 同一份文档可能命中多块，取最高覆盖率那块比（dict 直接覆盖会拿到最低的）
    best: dict[str, float] = {}
    for hit in hits:
        best[hit["doc"]] = max(best.get(hit["doc"], 0.0), hit["coverage"])
    assert best["full.md"] > 3 * best["part.md"]
    assert best["part.md"] < 0.2
    for hit in hits:
        assert hit["matched_terms"] <= hit["total_terms"]
        assert hit["coverage"] == pytest.approx(
            hit["matched_terms"] / hit["total_terms"], abs=1e-4
        )


def _rank_single(text, heading_path="网格生成 / 设置边界", query="舵缝填充"):
    """直接喂一条候选给 _rank，用来单独考察打分公式。

    走 search 测不了「正文完全不含 query词」的块：md 切出来的块文本带着标题行，
    标题命中就变成正文命中了。_rank 只按 row["id"] 下标记取，dict 可代替 sqlite3.Row。
    """
    rows = [{"id": 1, "rank": 0.0}]
    details = {
        1: {
            "id": 1,
            "doc_id": "doc-1",
            "doc_name": "手册.md",
            "library_id": kb_index.DEFAULT_LIBRARY_ID,
            "library_name": "默认知识库",
            "heading_path": heading_path,
            "section": heading_path.split(" / ")[-1],
            "page_start": 1,
            "page_end": 1,
            "kind": "para",
            "text": text,
            "doc_status": kb_index.DOC_STATUS_OK,
        }
    }
    return kb_index._rank(rows, details, query)[0]


def test_unrelated_body_has_zero_coverage_but_still_scores(kb):
    """只有标题对上的块 coverage 为 0，靠标题项拿分，不该被当成正文命中。"""
    hit = _rank_single("这里讲的是完全无关的排版约定。", heading_path="舵缝修复 / 舵缝填充")
    assert hit["coverage"] == 0.0
    assert hit["matched_terms"] == 0
    assert hit["total_terms"] > 0
    assert hit["score"] > 0


def test_search_long_question_survives_without_fts(kb):
    """FTS 不可用时，中文长问句仍要能命中；旧实现按整句 LIKE 必然 0 条。"""
    path = _write(
        kb,
        "手册.md",
        "# 网格生成\n\n## 附面层推进\n\n附面层首层高度由第一层网格厚度决定，直接影响壁面 y+。\n",
    )
    kb_index.index_file(path)

    original = kb_index._STORE._fts
    kb_index._STORE._fts = False
    try:
        hits = kb_index.search("附面层首层高度怎么确定")
    finally:
        kb_index._STORE._fts = original

    assert hits, "关掉 FTS 后长问句必须仍有命中"
    assert hits[0]["doc"] == "手册.md"
    assert kb_index._STORE._fts is original


def test_chapter_kind_classifies_case_and_reference(kb):
    assert kb_index._chapter_kind("某飞行器非结构网格 / 12.0.1 几何导入") == "case"
    assert kb_index._chapter_kind("M6机翼结构网格 / 7.0.1 几何导入") == "case"
    assert kb_index._chapter_kind("网格生成 / 7.4 辅助 / 7.4.7 设置边界") == ""
    assert kb_index._chapter_kind("") == ""
    assert kb_index._chapter_kind("F16全机 / 3.2 边界条件") == "case"


# 真实库里的顶层书签一半带章节序号，不剥就漏判。下面三个反例与正例同等重要：
# 判宽了会给功能页贴［案例］，模型据此降低引用，比漏标一个更贵。
@pytest.mark.parametrize(
    "heading, expected",
    [
        ("10 F6结构网格 / 10.0.1 几何导入", "case"),
        ("6.0.4 F6结构网格 / 6.0.1 几何导入", "case"),
        ("12.0.4 某弹结构网格 / 12.0.1 几何导入", "case"),
        ("6 Duct内流场结构网格 / 6.0.1 几何导入", ""),
        ("2D/3D结构网格 / 4.0.1 网格面", ""),
        ("网格生成 / 7.4 辅助 / 7.4.7 设置边界", ""),
    ],
)
def test_chapter_kind_strips_chapter_ordinal(kb, heading, expected):
    assert kb_index._chapter_kind(heading) == expected


def test_heading_hit_takes_best_segment_not_divided(kb):
    """深路径里标题正好对上功能名时，不能被路径段数除成噪声。

    除以段数时，3 段路径的 0.23 命中率只剩 0.077，乘 0.25 后基本归零；
    改成逐段取 max 后，深路径与短路径的标题分彼此可比。
    """
    deep = _rank_single(
        "附面层首层高度由第一层网格厚度决定。",
        heading_path="网格生成 / 7.3 网格体 / 7.3.3 附面层推进",
        query="附面层首层高度怎么确定",
    )
    shallow = _rank_single(
        "附面层首层高度由第一层网格厚度决定。",
        heading_path="附面层推进",
        query="附面层首层高度怎么确定",
    )
    # 同一段标题内容，路径加长不能改变标题项得分
    assert deep["score"] == shallow["score"]
    # 且标题项确实拿到了分：没有标题命中时分数应更低
    bare = _rank_single(
        "附面层首层高度由第一层网格厚度决定。",
        heading_path="系统设置 / 3.3.5 系统参数",
        query="附面层首层高度怎么确定",
    )
    assert deep["score"] > bare["score"]


def test_search_hits_carry_chapter_kind(kb):
    path = _write(
        kb,
        "算例.md",
        "# 某飞行器非结构网格\n\n## 12.0.1 几何导入\n\n几何导入选择 STEP 格式并自动缝合。\n",
    )
    kb_index.index_file(path)
    hit = kb_index.search("几何导入", top_k=1)[0]
    assert hit["chapter_kind"] == "case"

    plain = _write(kb, "说明.md", "# 网格生成\n\n## 7.4.7 几何导入\n\n几何导入选择 STEP 格式并自动缝合。\n")
    kb_index.index_file(plain)
    kinds = {
        item["doc"]: item["chapter_kind"]
        for item in kb_index.search("几何导入", top_k=5, max_per_doc=5)
    }
    assert kinds["说明.md"] == ""


def test_sources_gc_reports_orphans_without_deleting(kb, monkeypatch, tmp_path):
    sources = tmp_path / "sources"
    sources.mkdir()
    monkeypatch.setattr(kb_index, "KB_SOURCES_DIR", sources)

    used = sources / "used.md"
    used.write_text("# 甲\n\n甲内容。\n", encoding="utf-8")
    orphan = sources / "orphan.pdf"
    orphan.write_bytes(b"x" * 16)
    kb_index.index_file(used)

    report = kb_index.sources_gc()
    assert report["dry_run"] is True
    assert [item["name"] for item in report["orphans"]] == ["orphan.pdf"]
    assert report["total_size"] == 16
    assert report["deleted"] == []
    assert orphan.exists()

    purged = kb_index.sources_gc(dry_run=False)
    assert purged["deleted"] == [str(orphan)]
    assert not orphan.exists()
    assert used.exists()


def test_delete_library_purges_sources_only_when_asked(kb, monkeypatch, tmp_path):
    sources = tmp_path / "sources"
    sources.mkdir()
    monkeypatch.setattr(kb_index, "KB_SOURCES_DIR", sources)

    doc_path = sources / "doc.md"
    doc_path.write_text("# 甲\n\n甲内容。\n", encoding="utf-8")
    orphan = sources / "old.md"
    orphan.write_text("旧", encoding="utf-8")

    kb_index.index_file(doc_path)  # 默认库也引用它
    second = kb_index.create_library("试导库")
    kb_index.index_file(doc_path, library_id=second["id"])

    result = kb_index.delete_library(second["id"])
    assert "sources_gc" not in result
    assert orphan.exists(), "默认不许动 sources"

    third = kb_index.create_library("重导库")
    kb_index.index_file(doc_path, library_id=third["id"])
    result = kb_index.delete_library(third["id"], purge_sources=True)
    assert result["sources_gc"]["dry_run"] is False
    assert not orphan.exists()
    assert doc_path.exists(), "仍被其他库引用的文件不能被回收"


def test_query_log_and_report(kb):
    path = _write(kb, "手册.md", "# 网格\n\n## 附面层\n\n附面层首层高度决定壁面 y+。\n")
    kb_index.index_file(path)

    for query in ("首层高度", "附面层", "转子发动机点火提前角"):
        hits = kb_index.search(query, top_k=3)
        kb_index.log_query(
            query,
            cleaned=kb_index.clean_query(query),
            hits=len(hits),
            top=hits[0] if hits else None,
            library_id=kb_index.DEFAULT_LIBRARY_ID,
            source="tool",
        )

    report = kb_index.query_report()
    assert report["days"] == 30
    assert report["total"] == 3
    assert [item["query"] for item in report["empty"]] == ["转子发动机点火提前角"]
    assert {item["query"]: item["count"] for item in report["frequent"]} == {
        "首层高度": 1,
        "附面层": 1,
        "转子发动机点火提前角": 1,
    }


def test_query_report_flags_low_coverage(kb):
    kb_index.log_query(
        "几何导入支持什么格式",
        hits=2,
        top={"coverage": 0.12, "doc": "手册.md", "chunk_id": 7},
    )
    kb_index.log_query(
        "附面层首层高度",
        hits=1,
        top={"coverage": 0.85, "doc": "手册.md", "chunk_id": 3},
    )

    report = kb_index.query_report()
    assert report["low_coverage"] == [
        {"query": "几何导入支持什么格式", "coverage": 0.12, "doc": "手册.md"}
    ]


def test_prune_query_log_drops_old_rows(kb):
    import sqlite3

    kb_index.log_query("新查询", hits=1)
    conn = sqlite3.connect(str(kb / "kb.db"))
    try:
        conn.execute(
            "INSERT INTO kb_query_log (ts, query, hits) VALUES (?, ?, ?)",
            ("2000-01-01T00:00:00", "很久以前的查询", 0),
        )
        conn.commit()
    finally:
        conn.close()

    assert kb_index.prune_query_log(keep_days=90) == 1
    assert kb_index.query_report(days=36500)["total"] == 1
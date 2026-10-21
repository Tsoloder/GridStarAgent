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
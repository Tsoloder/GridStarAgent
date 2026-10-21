# -*- coding: utf-8 -*-
"""知识库冒烟验证：导入一批资料，跑一组提问，看召回是否落在该落的章节上。

用法：
    python verify/kb_smoke.py "D:\\资料\\GridStar手册"
    python verify/kb_smoke.py            # 不传路径就直接问现有索引

判断标准不看分数高低，看命中章节对不对。分数只用来比较同一问题内部的排序。
"""
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "agent" / "agent"))

import kb_index  # noqa: E402

QUESTIONS = [
    "附面层首层高度怎么确定",
    "增长率一般取多少",
    "设置边界条件怎么操作",
    "几何导入支持什么格式",
    "网格质量差怎么优化",
    "块装配怎么操作",
    "水密性处理",
]


def main() -> int:
    if len(sys.argv) > 1:
        targets = sys.argv[1:]
        # 默认归到「默认知识库」；想进到别的库就用 --library <id 或 名字>
        library_id = kb_index.DEFAULT_LIBRARY_ID
        if "--library" in targets:
            at = targets.index("--library")
            want = targets[at + 1] if at + 1 < len(targets) else ""
            targets = targets[:at] + targets[at + 2:]
            match = next(
                (item for item in kb_index.list_libraries() if want in (item["id"], item["name"])),
                None,
            )
            if match is None:
                print("找不到知识库：", want, "（现有：",
                      "、".join(item["name"] for item in kb_index.list_libraries()), "）")
                return 1
            library_id = match["id"]
        print("导入到知识库 %s：" % library_id, targets)
        report = kb_index.import_paths(targets, recursive=True, library_id=library_id)
        for item in report["indexed"]:
            print("  ok   %-40s chunks=%-5d params=%-4d pages=%d"
                  % (item["name"], item["chunks"], item["params"], item["pages"]))
        for item in report["failed"]:
            print("  FAIL %-40s %s" % (item["name"], item["error"]))
        if report["missing"]:
            print("  路径不存在：", report["missing"])
        print()

    state = kb_index.status()
    if state.get("documents", 0) == 0:
        print("知识库还是空的，先传一个目录或文件。")
        return 1
    print("知识库：%s" % "、".join(
        "%s(%s 份)" % (item["name"], item["documents"])
        for item in kb_index.list_libraries()
    ))
    print("索引：%s 份文档 / %s 块 / %s 条参数 / %.1f MB（FTS5=%s）\n"
          % (state["documents"], state["chunks"], state["params"],
             state["db_bytes"] / 1048576, state["fts"]))

    for question in QUESTIONS:
        hits = kb_index.search(question, top_k=3)
        params = kb_index.find_params(question, limit=2)
        print("Q: %s" % question)
        for row in params:
            print("   [参数] %s  %s" % (row["name"], row["description"][:60]))
        if not hits:
            print("   <未命中>")
        for hit in hits:
            print("   [%.2f] %s · P%s  %s"
                  % (hit["score"], hit["heading_path"].replace(" / ", " > "),
                     hit["page"], hit["snippet"][:60]))
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
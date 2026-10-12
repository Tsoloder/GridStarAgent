"""从 dumpbin /exports 的输出里抽出导出函数名，生成 .def 文件。

dumpbin 的导出段形如：

    ordinal hint RVA      name
          1    0 00012340 whisper_init_from_file
          2    1 00012450 whisper_free

序数导出（无 name 列）会被跳过：whisper.dll 是 C API，全部按名字导出。
"""
import re
import sys
from pathlib import Path

HEADER = re.compile(r"^\s*ordinal\s+hint\s+RVA\s+name\s*$", re.IGNORECASE)
# 例： "          1    0 00012340 whisper_init_from_file"
ROW = re.compile(r"^\s*(\d+)\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]{8})\s+(\S+)\s*$")


def main() -> int:
    if len(sys.argv) != 3:
        print("用法: make_def.py <exports_raw.txt> <out.def>")
        return 2

    raw = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
    names: list[str] = []
    in_section = False
    for line in raw.splitlines():
        if HEADER.match(line):
            in_section = True
            continue
        if not in_section:
            continue
        m = ROW.match(line)
        if m:
            names.append(m.group(4))

    # dumpbin 可能列出 forwarder/数据导出，去重并保序
    seen: set[str] = set()
    ordered = [n for n in names if not (n in seen or seen.add(n))]

    if not ordered:
        print("[make_def] 没解析到任何导出名，dumpbin 输出格式可能变了")
        return 1

    out = Path(sys.argv[2])
    with out.open("w", encoding="ascii", newline="\r\n") as fh:
        fh.write("LIBRARY whisper\n")
        fh.write("EXPORTS\n")
        for n in ordered:
            fh.write(f"    {n}\n")

    print(f"[make_def] 导出 {len(ordered)} 个符号 -> {out}")
    want = [n for n in ordered if n.startswith("whisper_full")]
    print(f"[make_def] 其中 whisper_full* : {len(want)} 个")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
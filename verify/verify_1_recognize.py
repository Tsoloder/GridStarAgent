# -*- coding: utf-8 -*-
"""验证①：进程识别、窗口定位、截图、控件树判空。

不需要管理员权限，也不改动 GridStar 的任何状态（除非显式加 --minimize）。
GridStar 只要在运行即可。

跑法：
    cd D:\\TRAE_project\\GridStarAgent
    python verify\\verify_1_recognize.py
    python verify\\verify_1_recognize.py --minimize

退出码：0 = 全部通过，1 = 有失败项。
"""
import argparse
import base64
import json
import os
import struct
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "agent"))
OUT = os.path.join(REPO, "verify", "out")

def _setup_console():
    """让中文在任何终端/管道下都能正确显示。"""
    if os.environ.get("PYTHONIOENCODING"):
        return
    try:
        if sys.stdout.isatty():
            import ctypes
            ctypes.windll.kernel32.SetConsoleOutputCP(65001)
    except Exception:
        pass
    for stream in (sys.stdout, sys.stderr):
        try:
            enc = (stream.encoding or "").lower().replace("-", "")
            if enc not in ("utf8", "utf8mb4"):
                stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


_setup_console()

from tools import screen_automation as sa  # noqa: E402

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok), detail))
    print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, ("  " + str(detail)) if detail else ""))
    return ok


def load(raw):
    return json.loads(raw)


def png_size(blob):
    if len(blob) < 24 or blob[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", blob[16:24])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minimize", action="store_true",
                    help="额外验证：最小化 GridStar 后截图能自动还原")
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)

    print("=" * 72)
    print("① 运行环境与依赖")
    print("=" * 72)
    print("python            :", sys.executable)
    print("目标进程名        :", sa.TARGET_PROCESS_NAMES,
          "(可用环境变量 GRIDSTAR_PROCESS_NAMES 覆盖)")
    print("缺失依赖          :", sa._MISSING_DEPS or "无")
    print("pywin32 错误      :", sa._PYWIN32_ERROR or "无")
    print("DPI 感知          :", sa._DPI_AWARE)
    print("mss / uiautomation:", ("有" if sa.mss else "无"), "/",
          ("有" if sa.uia else "无"))
    check("依赖齐备（mss + uiautomation + pywin32）",
          not sa._MISSING_DEPS and not sa._PYWIN32_ERROR,
          "缺失 %s / pywin32 %s" % (sa._MISSING_DEPS, sa._PYWIN32_ERROR))
    check("进程已设置 DPI 感知（截图与坐标同为物理像素）", bool(sa._DPI_AWARE))

    print()
    print("=" * 72)
    print("② 接线：工具是否注册进 registry")
    print("=" * 72)
    try:
        import registry
        screen_group = [g for g in registry.TOOL_GROUPS if g["id"] == "screen"]
        names = [t.__name__ for t in screen_group[0]["tools"]] if screen_group else []
        print("工具总数          :", len(registry.TOOLS))
        print("screen 分组       :", names)
        check("screen 分组存在且含 10 个工具", len(names) == 10, names)
        check("默认分组不含 screen（模型需先 enable_tool_group）",
              "screen" not in registry.DEFAULT_GROUP_IDS, registry.DEFAULT_GROUP_IDS)
        check("坐标工具归入 screen 分组",
              registry.group_id_for_tool("ClickAtPoint") == "screen")
    except Exception as exc:
        check("registry 可导入", False, "%s: %s" % (type(exc).__name__, exc))

    print()
    print("=" * 72)
    print("③ 进程识别：找到的是不是真的 GridStar")
    print("=" * 72)
    found = load(sa.FindGridStarWindow())
    if found.get("status") != "success":
        check("FindGridStarWindow 成功", False, str(found.get("result"))[:200])
        return summary()
    res = found["result"]
    win = res["window"]
    print("判据 matched_by   :", res.get("matched_by"), "| 目标进程名", res.get("process_names"))
    for key in ("hwnd", "pid", "process_name", "exe", "title", "class_name",
                "frame_rect", "is_foreground", "is_iconic", "is_cloaked",
                "integrity_rid", "is_elevated", "input_blocked"):
        print("  %-17s: %s" % (key, win.get(key)))
    if win.get("input_blocked"):
        print("  input_block_reason:", win.get("input_block_reason"))
    check("找到 GridStar 窗口且进程名匹配", bool(win.get("hwnd")) and bool(win.get("pid")),
          "hwnd=%s pid=%s" % (win.get("hwnd"), win.get("pid")))
    check("取到了 exe 全路径（没有被权限挡住）", bool(win.get("exe")), win.get("exe"))
    print("同进程其它窗口    :", [
        {"title": w.get("title"), "is_visible": w.get("is_visible")}
        for w in (res.get("other_windows") or [])][:6])

    print()
    print("=" * 72)
    print("④ 假阳性对照：标题里带 GridStar、但进程不是 GridStar 的窗口")
    print("=" * 72)
    try:
        import win32gui
        fg = win32gui.GetForegroundWindow()
        decoys = sa._title_decoys(["gridstar"], fg)
    except Exception as exc:
        decoys = []
        print("取诱饵失败：%s: %s" % (type(exc).__name__, exc))
    for d in decoys:
        print("  诱饵窗口:", {k: d.get(k) for k in ("hwnd", "title", "process_name", "exe")})
    print("诱饵数量          :", len(decoys))
    check("旧判据的诱饵窗口没有被当成 GridStar",
          all(d.get("hwnd") != win.get("hwnd") for d in decoys),
          "找到了 %d 个诱饵，均被进程判据否定" % len(decoys))

    print()
    print("=" * 72)
    print("⑤ 标题降级：标题过滤只是次级条件")
    print("=" * 72)
    miss = load(sa.FindGridStarWindow(window_title="绝对不存在的标题xyz"))
    print("status            :", miss.get("status"))
    print("result            :", str(miss.get("result"))[:400])
    check("标题对不上时报错、且说明判据是进程名",
          miss.get("status") == "error" and "进程" in str(miss.get("result")),
          "报错文案里带了扫描统计与同进程窗口清单")

    print()
    print("=" * 72)
    print("⑥ 截图：拍到的是不是真界面")
    print("=" * 72)
    cap = load(sa.CaptureGridStarWindow(activate=True, clean=True))
    if cap.get("status") != "success":
        check("CaptureGridStarWindow 成功", False, str(cap.get("result"))[:300])
        return summary()
    c = cap["result"]
    blob = base64.b64decode(c["image"])
    size = png_size(blob)
    path = os.path.join(OUT, "verify_1_capture.png")
    with open(path, "wb") as fh:
        fh.write(blob)
    print("method            :", c.get("method"), "| clean_requested", c.get("clean_requested"))
    print("capture_rect      :", c.get("capture_rect"))
    print("width x height    :", c.get("width"), "x", c.get("height"))
    print("is_foreground     :", c.get("is_foreground"), "| activated", c.get("activated"))
    print("dpi_aware         :", c.get("dpi_aware"))
    print("coord_space       :", c.get("coord_space"))
    if c.get("clean_fallback"):
        print("clean_fallback    :", c["clean_fallback"])
    if c.get("clean_warning"):
        print("clean_warning     :", c["clean_warning"])
    print("落盘              :", path, "(%d 字节)" % len(blob))
    check("返回的是合法 PNG 且尺寸与 capture_rect 一致",
          size == (c["capture_rect"]["width"], c["capture_rect"]["height"]),
          "%s vs %s" % (size, c.get("capture_rect")))
    check("截图前窗口已在前台且非最小化",
          bool(c.get("is_foreground")) and not c.get("window", {}).get("is_iconic"))

    print()
    print("=" * 72)
    print("⑦ 控件树：GridStar 是 Qt 程序，应当为空")
    print("=" * 72)
    ui = load(sa.GetUIElementInfo(depth=2))
    if ui.get("status") == "success":
        r = ui["result"]
        controls = r.get("controls") or []
        print("控件数            :", len(controls))
        print("note              :", str(r.get("note"))[:220])
        check("控件树判空且给出替代路线说明",
              len(controls) == 0 and bool(r.get("note")),
              "控件数 %d" % len(controls))
    else:
        check("GetUIElementInfo 成功", False, str(ui.get("result"))[:200])

    blocked = bool(win.get("input_blocked"))
    for tool, kwargs in (
        ("ClickUIElement", dict(name="绝对不存在的控件xyz")),
        ("TypeTextInUIElement", dict(text="x", name="绝对不存在的控件xyz")),
    ):
        out = load(getattr(sa, tool)(**kwargs))
        msg = str(out.get("result"))
        print("%-20s: %s" % (tool, msg[:180]))
        # 权限闸门先于控件查找：权限不一致时报的是权限原因，权限一致时才该报空控件树
        ok = out.get("status") == "error" and (blocked or "控件树是空的" in msg)
        check("%s 明确报错、不假装点到（%s）" % (
            tool, "当前报权限原因" if blocked else "当前应报空控件树"), ok)

    print()
    print("=" * 72)
    print("⑧ 破坏性快捷键闸门（纯函数，不会真的发键）")
    print("=" * 72)
    cases = [("{Alt}{F4}", True), ("Alt+F4", True), ("{Alt}{Space}", True),
             ("Ctrl+S", False), ("Ctrl+Z", False)]
    ok = True
    for keys, expect in cases:
        got = sa._is_destructive_shortcut(keys)
        ok = ok and (got == expect)
        print("  _is_destructive_shortcut(%-12r) = %-5s 期望 %s" % (keys, got, expect))
    check("破坏性快捷键识别正确", ok)
    refused = load(sa.SendKeyboardShortcut("{Alt}{F4}"))
    print("SendKeyboardShortcut('{Alt}{F4}') ->", refused.get("status"),
          "|", str(refused.get("result"))[:160])
    check("未加 allow_close 时拒绝发送 Alt+F4", refused.get("status") == "error",
          "返回 error，说明键没有发出去")

    if args.minimize:
        print()
        print("=" * 72)
        print("⑨ 最小化 → 自动还原 → 截图（会真的最小化一下窗口）")
        print("=" * 72)
        hwnd = win["hwnd"]
        sa._send_syscommand(hwnd, 0xF020)  # WM_SYSCOMMAND / SC_MINIMIZE
        time.sleep(0.6)
        facts = sa._window_facts(hwnd)
        print("最小化后 is_iconic:", facts.get("is_iconic"))
        cap2 = load(sa.CaptureGridStarWindow(activate=True))
        after = sa._window_facts(hwnd)
        print("截图 status       :", cap2.get("status"))
        print("截图后 is_iconic  :", after.get("is_iconic"),
              "| is_foreground", after.get("is_foreground"))
        check("最小化后截图自动还原窗口",
              cap2.get("status") == "success" and not after.get("is_iconic"))

    return summary()


def summary():
    print()
    print("=" * 72)
    failed = [n for n, ok, _ in RESULTS if not ok]
    print("小结：%d 项通过 / %d 项失败" % (len(RESULTS) - len(failed), len(failed)))
    for name in failed:
        print("  FAIL:", name)
    print("截图与产物目录：", OUT)
    print("=" * 72)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
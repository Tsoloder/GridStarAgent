# -*- coding: utf-8 -*-
"""验证②：坐标通道真的能把鼠标事件送进目标窗口。

不需要管理员权限，也不需要 GridStar：脚本自己开一个 tkinter 靶子窗口，
临时把目标进程名改成 python.exe（只影响本脚本进程），然后走**公开工具**
（ClickAtPoint / DragAtPoint / ScrollAtPoint）点它，用靶子自己的事件回调
核对是否真的收到。

跑法：
    cd D:\\TRAE_project\\GridStarAgent
    python verify\\verify_2_inject.py

退出码：0 = 全部通过，1 = 有失败项。
"""
import json
import os
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 必须在 import 之前设置：TARGET_PROCESS_NAMES 在导入时求值
os.environ["GRIDSTAR_PROCESS_NAMES"] = "python.exe"
sys.path.insert(0, os.path.join(REPO, "agent"))

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

TITLE = "gs-verify-target"
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok), detail))
    print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, ("  " + str(detail)) if detail else ""))
    return ok


def run_tool(root, fn, wait_s=8.0):
    """在后台线程里调工具，主线程持续泵 Tk 事件循环（否则靶子收不到注入的消息）。"""
    box = {}

    def work():
        try:
            box["raw"] = fn()
        except Exception as exc:
            box["exc"] = "%s: %s" % (type(exc).__name__, exc)

    thread = threading.Thread(target=work, daemon=True)
    thread.start()
    end = time.time() + wait_s
    while thread.is_alive() and time.time() < end:
        root.update()
        time.sleep(0.01)
    for _ in range(30):
        root.update()
        time.sleep(0.01)
    thread.join(timeout=2)
    if "exc" in box:
        raise RuntimeError(box["exc"])
    return json.loads(box["raw"])


def main():
    import tkinter as tk
    from tkinter import font as tkfont

    print("=" * 72)
    print("靶子进程的目标进程名:", sa.TARGET_PROCESS_NAMES, "（只影响本脚本）")
    print("=" * 72)

    root = tk.Tk()
    root.title(TITLE)
    root.geometry("640x400+300+300")
    # 置顶：靶子被浏览器/编辑器压住时，工具的命中测试会（正确地）拒绝注入，
    # 那样验证就跑不下去了，所以探测靶子自己置顶。
    root.attributes("-topmost", True)
    # 一个窗口分两半：左半是可滚动的 Text（验证滚轮），右半是 Canvas（验证点击/拖动）。
    # 只用一个窗口，多窗口会互相抢前台，注入容易送到别处去。
    scroll_lines = 400
    text = tk.Text(root, wrap="none", bg="#101820", fg="#d0d0d0")
    text.insert("1.0", "\n".join("line %03d" % i for i in range(1, scroll_lines + 1)))
    text.grid(row=0, column=0, sticky="nsew")
    canvas = tk.Canvas(root, bg="#2050a0", highlightthickness=0)
    canvas.grid(row=0, column=1, sticky="nsew")
    root.grid_rowconfigure(0, weight=1)
    root.grid_columnconfigure(0, weight=1)
    root.grid_columnconfigure(1, weight=1)
    canvas.create_text(160, 40, text="gs-verify-target", fill="white",
                       font=tkfont.Font(size=14))
    clicks, dblclicks, motions, releases, wheels, rights = [], [], [], [], [], []
    canvas.bind("<Button-1>", lambda e: clicks.append((e.x_root, e.y_root)))
    # Windows 上双击的第二次按下是 WM_LBUTTONDBLCLK，Tk 把它映射成 <Double-Button-1>，
    # 不会再触发 <Button-1>。所以要两个都数，否则会把「双击成功」误判成「只点了一次」。
    canvas.bind("<Double-Button-1>", lambda e: dblclicks.append((e.x_root, e.y_root)))
    canvas.bind("<ButtonRelease-1>", lambda e: releases.append((e.x_root, e.y_root)))
    canvas.bind("<B1-Motion>", lambda e: motions.append((e.x_root, e.y_root)))
    canvas.bind("<Button-3>", lambda e: rights.append((e.x_root, e.y_root)))
    canvas.bind("<MouseWheel>", lambda e: wheels.append(e.delta))
    root.update()
    root.after(50, lambda: root.focus_force())
    for _ in range(20):
        root.update()
        time.sleep(0.01)

    try:
        print()
        print("=" * 72)
        print("① 单击：归一化 (750,500) 应落在右半边的 Canvas 上")
        print("=" * 72)
        # 「没能切到前台」「像素上压着别的窗口」这两种拒绝是环境造成的（人正在用鼠标、
        # 别的窗口压在上面），工具拒绝是对的。这里重试几次，并把该怎么做说清楚。
        out = None
        for attempt in range(3):
            out = run_tool(root, lambda: sa.ClickAtPoint(750, 500, window_title=TITLE))
            if out.get("status") == "success":
                break
            print("第 %d 次被拒绝：%s" % (attempt + 1, out.get("result")))
            time.sleep(1.0)
        if out.get("status") != "success":
            print()
            print("这多半不是工具的问题，而是此刻机器正被人用着：工具的拒绝理由是上面那句。")
            print("请把手离开鼠标键盘、别点其它窗口，让靶子能拿到前台，然后重跑本脚本。")
            check("ClickAtPoint 成功", False, str(out.get("result"))[:300])
            return summary()
        r = out["result"]
        win = r["window"]
        print("选中窗口          :", win.get("title"), "| pid", win.get("pid"), "|",
              win.get("process_name"), "| hwnd", win.get("hwnd"))
        print("基准矩形          :", r["coord_space"]["rect"])
        print("换算结果          :", r["point"]["input"], "->", r["point"]["screen_pixel"],
              "(归一化回报", r["point"]["normalized"], ")")
        print("注入              :", r.get("injected"), "/", r.get("events_expected"),
              "| cursor_after", r.get("cursor_after"),
              "| cursor_on_target", r.get("cursor_on_target"))
        print("verified          :", r.get("verified"), "|", str(r.get("verification_note"))[:80])
        if not check("工具选中的是本脚本的靶子窗口", win.get("title") == TITLE,
                     "选中了 %r，请关掉其它 python 图形窗口后重跑" % win.get("title")):
            return summary()
        target_px = [r["point"]["screen_pixel"]["x"], r["point"]["screen_pixel"]["y"]]
        hit = [list(c) for c in clicks if list(c) == target_px]
        check("点了靶子，且靶子记录到的屏幕坐标与换算结果一致", bool(hit),
              "靶子收到 %s，其中落在目标像素 %s 的有 %d 次"
              % (list(clicks), target_px, len(hit)))
        check("光标确实落在目标像素上", bool(r.get("cursor_on_target")))
        check("所有鼠标事件都被系统接受（injected == events_expected）",
              r.get("injected") == r.get("events_expected"),
              "%s / %s" % (r.get("injected"), r.get("events_expected")))
        check("如实回报 verified=False（不假装知道对方响应了）",
              r.get("verified") is False and bool(r.get("verification_note")))

        print()
        print("=" * 72)
        print("② 双击")
        print("=" * 72)
        clicks.clear()
        dblclicks.clear()
        out = run_tool(root, lambda: sa.ClickAtPoint(750, 400, clicks=2, window_title=TITLE))
        r = out.get("result", {})
        downs = len(clicks) + len(dblclicks)
        print("注入              :", r.get("injected"), "/", r.get("events_expected"))
        print("靶子收到按下次数  :", downs, "（Button-1 %d + Double-Button-1 %d）"
              % (len(clicks), len(dblclicks)))
        check("双击送出了两个按下（第二次是 Windows 的双击消息）",
              out.get("status") == "success" and downs == 2, "靶子收到 %d 次" % downs)

        print()
        print("=" * 72)
        print("③ 滚轮：用带滚动的 Text 窗看位移，而不是只数 delta")
        print("=" * 72)
        wheels.clear()
        text.yview_moveto(0.5)
        root.update()
        # 左边这半是可滚动的 Text，右边那半是 Canvas：滚轮必须落在 Text 上才有位移。
        print("Text 可视范围     :", text.yview(), "| 光标:", sa._cursor_pos())
        one_before = float(text.yview()[0])
        out1 = run_tool(root, lambda: sa.ScrollAtPoint(250, 500, amount=1,
                                                      window_title=TITLE))
        one_after = float(text.yview()[0])
        per_notch = round((one_after - one_before) * scroll_lines)
        print("标定              : 1 格 = %s 行（Tk 每个滚轮格滚几行由 Tk 版本决定）" % per_notch)
        print("标定时的返回      :", out1.get("status"),
              (out1.get("result") or {}).get("injected") if isinstance(out1.get("result"), dict) else out1.get("result"),
              "| rect", ((out1.get("result") or {}).get("coord_space") or {}).get("rect")
              if isinstance(out1.get("result"), dict) else None)
        if per_notch == 0:
            check("滚轮能被目标窗口应用收到", False,
                  "1 格滚轮没有产生任何位移，注入没到窗口上")
        else:
            text.yview_moveto(0.5)
            root.update()
            before = float(text.yview()[0])
            out = run_tool(root, lambda: sa.ScrollAtPoint(250, 500, amount=3,
                                                         window_title=TITLE))
            r = out.get("result", {})
            after = float(text.yview()[0])
            moved = round((after - before) * scroll_lines)
            print("注入              :", r.get("injected"), "/", r.get("events_expected"))
            print("滚动窗口          :", r.get("window", {}).get("title"))
            print("Text 位移         :", moved, "行（3 格应为 %s）" % (3 * per_notch))
            check("三格滚轮真的被目标窗口应用收到",
                  out.get("status") == "success" and moved == 3 * per_notch,
                  "位移 %s 行，期望 %s 行" % (moved, 3 * per_notch))

        print()
        print("=" * 72)
        print("④ 拖动：中间移动必须逐个送达，否则三维视图转不动")
        print("=" * 72)
        motions.clear()
        out = run_tool(root, lambda: sa.DragAtPoint(600, 250, 950, 700, duration_ms=800,
                                                    window_title=TITLE))
        r = out.get("result", {})
        print("注入              :", r.get("injected"), "/", r.get("events_expected"))
        print("起止              :", r.get("point", {}).get("screen_pixel"),
              "->", r.get("point", {}).get("input_to"))
        print("靶子收到 B1-Motion:", len(motions), "个，首 %s 末 %s" %
              (motions[0] if motions else None, motions[-1] if motions else None))
        check("拖动送出了 down + 多个中间移动 + up",
              out.get("status") == "success" and len(motions) >= 5,
              "中间移动 %d 个" % len(motions))
        check("拖动事件全部被接受",
              r.get("injected") == r.get("events_expected"),
              "%s / %s" % (r.get("injected"), r.get("events_expected")))

        print()
        print("=" * 72)
        print("⑤ 越界与非法参数必须被拒绝（不能静默夹到边上乱点）")
        print("=" * 72)
        for label, fn in (
            ("归一化 x=1001", lambda: sa.ClickAtPoint(1001, 500, window_title=TITLE)),
            ("归一化 y=-1", lambda: sa.ClickAtPoint(500, -1, window_title=TITLE)),
            ("像素模式越界 x=99999",
             lambda: sa.ClickAtPoint(99999, 10, coord_space="pixel", window_title=TITLE)),
            ("coord_space 写错",
             lambda: sa.ClickAtPoint(500, 500, coord_space="grid", window_title=TITLE)),
            ("clicks=9", lambda: sa.ClickAtPoint(500, 500, clicks=9)),
            ("滚轮 amount=0", lambda: sa.ScrollAtPoint(500, 500, amount=0)),
            ("button 写错", lambda: sa.ClickAtPoint(500, 500, button="middle2")),
        ):
            out = json.loads(fn())
            msg = str(out.get("result"))
            print("  %-20s -> %-7s %s" % (label, out.get("status"), msg[:90]))
            check("%s 被拒绝" % label, out.get("status") == "error")
    finally:
        root.destroy()

    return summary()


def summary():
    print()
    print("=" * 72)
    failed = [n for n, ok, _ in RESULTS if not ok]
    print("小结：%d 项通过 / %d 项失败" % (len(RESULTS) - len(failed), len(failed)))
    for name in failed:
        print("  FAIL:", name)
    print("=" * 72)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
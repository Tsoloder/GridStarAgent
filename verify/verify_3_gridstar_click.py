# -*- coding: utf-8 -*-
"""验证③：在真 GridStar 上做一次「可逆的、看得见」的操作。

**需要管理员权限**：GridStar 以管理员运行时（完整性级别 0x3000），普通权限进程
发出的模拟输入会被 Windows UIPI 静默丢弃，窗口自绘截图也会被拒。用相同权限启动
本脚本才能验证注入那一层。

跑法（管理员 PowerShell）：
    cd D:\\TRAE_project\\GridStarAgent
    python verify\\verify_3_gridstar_click.py
    python verify\\verify_3_gridstar_click.py --no-scan         # 只做滚轮缩放
    python verify\\verify_3_gridstar_click.py --click 55,168    # 单点一次，前后对比

做的事：滚轮缩放进/出 + 逐个点功能区页签，每一步都前后截图算像素差异，
有变化才算操作真的被 GridStar 接受了。不涉及任何建模数据，全程可逆。
"""
import argparse
import base64
import io
import json
import os
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


def grab(tag, clean=True):
    """截一张图，落盘并返回 (payload, ndarray)。"""
    out = load(sa.CaptureGridStarWindow(activate=True, clean=clean))
    if out.get("status") != "success":
        raise RuntimeError("截图失败：%s" % str(out.get("result"))[:300])
    r = out["result"]
    blob = base64.b64decode(r["image"])
    path = os.path.join(OUT, "verify_3_%s.png" % tag)
    with open(path, "wb") as fh:
        fh.write(blob)
    import numpy as np
    from PIL import Image
    img = np.asarray(Image.open(io.BytesIO(blob)).convert("RGB"), dtype=np.int16)
    return r, img, path


def diff_ratio(a, b, box=None):
    """两张图在 box 区域内的「变化像素占比」（单通道差异 > 8 记一次）。"""
    import numpy as np
    if a.shape != b.shape:
        return 1.0
    if box:
        x0, y0, x1, y1 = box
        a = a[y0:y1, x0:x1]
        b = b[y0:y1, x0:x1]
    d = np.abs(a - b).max(axis=2) > 8
    return float(d.mean())


def viewport_box(shape):
    """三维视口的大致范围（避开左边的模型树面板和顶部功能区）。"""
    h, w = shape[0], shape[1]
    return (int(w * 0.21), int(h * 0.23), int(w * 0.97), int(h * 0.75))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-scan", action="store_true", help="不扫页签，只做滚轮缩放")
    ap.add_argument("--click", default="", help="单点一次，格式 归一化x,归一化y")
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)

    print("=" * 72)
    print("① 权限对齐检查")
    print("=" * 72)
    facts = load(sa.FindGridStarWindow())
    if facts.get("status") != "success":
        print("找不到 GridStar 窗口：", str(facts.get("result"))[:300])
        return 1
    win = facts["result"]["window"]
    self_rid = sa._self_integrity_rid()
    print("GridStar          : pid=%s %s  IL=0x%s elevated=%s" % (
        win.get("pid"), win.get("process_name"),
        format(win.get("integrity_rid") or 0, "X"), win.get("is_elevated")))
    print("本脚本进程        : pid=%s  IL=0x%s" % (
        os.getpid(), format(self_rid or 0, "X")))
    print("input_blocked     :", win.get("input_blocked"))
    if win.get("input_blocked"):
        print("  reason          :", win.get("input_block_reason"))
        print()
        print("!" * 72)
        print("权限没对齐，注入类验证做不了。请用**管理员**方式启动：")
        print("  1) 关掉当前的 MCP 服务进程（agent\\server.py，SSE 127.0.0.1:5656）")
        print("  2) 用「以管理员身份运行」打开的终端执行：")
        print("       cd D:\\TRAE_project\\GridStarAgent\\agent")
        print("       python server.py")
        print("  3) 用同一个管理员终端重跑本脚本")
        print("（或者把 GridStar 改成普通权限运行，两端权限一致即可）")
        print("!" * 72)
        check("权限对齐（两侧完整性级别相同）", False,
              "GridStar 0x%s > 本进程 0x%s" % (format(win.get("integrity_rid") or 0, "X"),
                                              format(self_rid or 0, "X")))
        return summary()
    check("权限对齐（两侧完整性级别相同）", True)

    if args.click:
        try:
            cx, cy = [float(v) for v in args.click.split(",")]
        except Exception:
            print("--click 格式应为 归一化x,归一化y")
            return 1
        _, before, _ = grab("click_before")
        out = load(sa.ClickAtPoint(cx, cy))
        print()
        print("单点 (%s,%s) ->" % (cx, cy), out.get("status"))
        print("  ", json.dumps(out.get("result"), ensure_ascii=False)[:400])
        _, after, _ = grab("click_after")
        ratio = diff_ratio(before, after)
        print("画面变化占比      : %.2f%%" % (ratio * 100))
        check("这次点击让画面发生了变化", ratio > 0.0005,
              "变化 %.2f%%（0 说明坐标没落在可点控件上，或被窗口拒绝）" % (ratio * 100))
        return summary()

    print()
    print("=" * 72)
    print("② 滚轮缩放：把光标放在三维视口上滚动，前后对比像素")
    print("=" * 72)
    base, img0, p0 = grab("wheel_before")
    box = viewport_box(img0.shape)
    print("基准矩形          :", base["capture_rect"], "| method", base.get("method"))
    print("对比区域(视口)    :", box)
    print("截图              :", p0)

    out = load(sa.ScrollAtPoint(500, 600, amount=4))
    r = out.get("result", {})
    print("ScrollAtPoint(+4) :", out.get("status"), "| 注入", r.get("injected"), "/",
          r.get("events_expected"), "| 落点", r.get("point", {}).get("screen_pixel"),
          "| cursor_on_target", r.get("cursor_on_target"))
    if out.get("status") != "success":
        print("  失败原因        :", str(out.get("result"))[:300])
        check("滚轮调用成功", False, str(out.get("result"))[:160])
    else:
        check("滚轮调用成功且事件全部被接受",
              r.get("injected") == r.get("events_expected"),
              "%s / %s" % (r.get("injected"), r.get("events_expected")))
        time.sleep(0.5)
        _, img1, p1 = grab("wheel_in")
        ratio_in = diff_ratio(img0, img1, box)
        print("放大后视口变化    : %.2f%%" % (ratio_in * 100), "|", p1)
        out2 = load(sa.ScrollAtPoint(500, 600, amount=-4))
        time.sleep(0.5)
        _, img2, p2 = grab("wheel_back")
        ratio_back = diff_ratio(img1, img2, box)
        print("滚回后视口变化    : %.2f%%" % (ratio_back * 100), "|", p2)
        check("滚轮让三维视口真的变了（GridStar 响应了注入）", ratio_in > 0.002,
              "视口变化 %.2f%%；若为 0，可能是光标没落在视口上或滚轮在该处未绑定" % (ratio_in * 100))
        check("反向滚动把视图滚回来了", ratio_back > 0.002,
              "变化 %.2f%%" % (ratio_back * 100))

    if not args.no_scan:
        print()
        print("=" * 72)
        print("③ 逐个点功能区页签（只切页签，不动任何建模数据）")
        print("=" * 72)
        # 功能区页签在窗口里的绝对像素位置（Qt 功能区左对齐、不随窗口宽度缩放），
        # 所以先写像素再按实际 capture_rect 换算成 0-1000，换了分辨率也不用改。
        rect = base["capture_rect"]
        fw = max(1, rect["width"] - 1)
        fh = max(1, rect["height"] - 1)
        tab_y_px = 44
        candidates = [
            (label, round(px * 1000 / fw), round(tab_y_px * 1000 / fh))
            for label, px in (("开始", 30), ("视图", 73), ("数模", 119),
                              ("离散几何", 180), ("网格生成", 245), ("帮助", 312))
        ]
        print("页签坐标(按 %s 换算)：" % rect,
              [(l, x, y) for l, x, y in candidates])
        _, ref, _ = grab("tab_ref")
        hits = []
        for label, x, y in candidates:
            out = load(sa.ClickAtPoint(x, y))
            time.sleep(0.35)
            _, cur, path = grab("tab_%s" % label)
            ratio = diff_ratio(ref, cur)
            ok = out.get("status") == "success"
            print("  点『%-6s』(%3d,%2d) -> %-7s 画面变化 %5.2f%%  %s" % (
                label, x, y, out.get("status"), ratio * 100, path))
            if not ok:
                print("      失败原因:", str(out.get("result"))[:200])
            if ok and ratio > 0.002:
                hits.append(label)
        check("至少有一个页签坐标点中了可点控件", bool(hits),
              "命中：%s；没命中的说明我给的坐标偏了，请按截图重新量" % (hits or "无"))
        if hits:
            print("把功能区点回『开始』页签，恢复标准状态：")
            back = load(sa.ClickAtPoint(candidates[0][1], candidates[0][2]))
            print("  ->", back.get("status"))

    print()
    print("产物目录：", OUT)
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
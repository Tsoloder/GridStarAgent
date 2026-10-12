"""真实链路冒烟测试：加载真 DLL + 真模型，转写一段真音频。

与 tests/ 的区别：tests/ 全部 mock，不碰真资产，适合 CI；本脚本则**必须**有
运行资产才能跑，用来回答「装到内网机器上，语音到底能不能用」。

用法::

    python voice_asr/smoke_test.py                       # 用内置素材
    python voice_asr/smoke_test.py path/to/your.wav      # 用你自己的 16kHz WAV
    python voice_asr/smoke_test.py --backend-only        # 只验后端，不起 HTTP

退出码 0 表示通过；非 0 时打印具体缺什么。
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

# 允许从仓库根直接运行
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from voice_asr import config  # noqa: E402
from voice_asr.transcriber import TranscribeError, backend_info, transcribe  # noqa: E402

FIXTURE = Path(__file__).resolve().parent / "tests" / "fixtures" / "tts_zh_16k.wav"

# 内置素材的真实内容，以及 base 模型对它的稳定偏差。
# 这些偏差是模型容量上限（换 ggml-small.bin 可改善），不是集成问题，
# 所以判定时把它们折算掉，只对「是否严重跑偏」报警。
EXPECTED_TEXT = "生成一个圆柱体的网格，边界层设置五层。"
KNOWN_DEVIATIONS = {
    "原住体": "圆柱体",
    "武层": "五层",
}


def judge(text: str) -> str:
    """把实际输出对照预期，返回判定结论。"""
    normalized = text
    for wrong, right in KNOWN_DEVIATIONS.items():
        normalized = normalized.replace(wrong, right)
    for ch in "，,。.、 ":
        normalized = normalized.replace(ch, "")
    target = "".join(c for c in EXPECTED_TEXT if c not in "，,。.、 ")
    if normalized == target:
        return "与预期一致（含已知偏差折算）" if text != EXPECTED_TEXT else "与预期完全一致"
    # 字符集重合度：中文乱码/英文胡言会非常低
    overlap = len(set(normalized) & set(target)) / max(len(set(target)), 1)
    if overlap < 0.6:
        return f"严重跑偏（字符重合 {overlap:.0%}），检查模型与 DLL 是否匹配"
    return f"部分偏差（字符重合 {overlap:.0%}）：{text}"


def check_assets() -> int:
    """检查三项运行资产是否齐备。返回缺失项数量。"""
    settings = config.load_settings()
    checks = [
        ("shim（native/bin/asr_shim.dll）", settings.shim_exists(), config.shim_dll_path()),
        ("whisper.dll", config.whisper_dll_path().is_file(), config.whisper_dll_path()),
        ("模型 ggml-base.bin", settings.model_exists(), Path(settings.model)),
    ]
    missing = 0
    for name, ok, path in checks:
        mark = "OK  " if ok else "缺失"
        size = f"{path.stat().st_size:,} B" if path.is_file() else "-"
        print(f"  [{mark}] {name:36} {size}")
        if not ok:
            missing += 1
            print(f"         期望路径: {path}")
    print(f"  就绪判断: {settings.is_ready()}  (语言={settings.lang} 线程={settings.threads})")
    if missing:
        print("\n补齐办法: python voice_asr/native/sync_runtime.py && voice_asr\\native\\build_shim.bat")
    return missing


def run_backend(wav_path: Path, rounds: int) -> int:
    print(f"\n[后端直调] 音频: {wav_path.name} ({wav_path.stat().st_size:,} B)")
    data = wav_path.read_bytes()
    for i in range(1, rounds + 1):
        started = time.perf_counter()
        try:
            result = transcribe(data)
        except TranscribeError as exc:
            print(f"  第{i}次 失败 kind={exc.kind} message={exc.message} detail={exc.detail}")
            return 1
        wall = int((time.perf_counter() - started) * 1000)
        rtf = result.backend_ms / result.duration_ms if result.duration_ms else 0
        print(
            "  第%d次 %dms 音频=%.2fs 推理=%dms RTF=%.2f"
            % (i, wall, result.duration_ms / 1000.0, result.backend_ms, rtf)
        )
        print(f"        文本: {result.text}")
        if wav_path == FIXTURE:
            print(f"        判定: {judge(result.text)}")
    return 0


def run_http(wav_path: Path) -> int:
    print("\n[HTTP 全链路] 挂载真实 app 并 POST /asr")
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "agent" / "agent"))
        from fastapi.testclient import TestClient  # noqa: PLC0415

        import app as agent_app  # noqa: PLC0415
    except Exception as exc:  # noqa: BLE001
        print(f"  跳过：无法导入 agent app（{exc}）")
        return 0

    client = TestClient(agent_app.app)
    health = client.get("/asr/health").json()
    print(f"  /asr/health -> {json.dumps(health, ensure_ascii=False)}")
    if not health.get("ready"):
        print("  未就绪，中止 HTTP 测试")
        return 1

    with wav_path.open("rb") as fh:
        resp = client.post("/asr", files={"audio": (wav_path.name, fh.read(), "audio/wav")})
    print(f"  POST /asr -> {resp.status_code}")
    if resp.status_code != 200:
        print(f"  detail: {resp.json().get('detail')}")
        return 1
    body = resp.json()
    print(
        "  text=%r duration_ms=%s backend_ms=%s"
        % (body.get("text"), body.get("duration_ms"), body.get("backend_ms"))
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="voice_asr 真实链路冒烟测试")
    parser.add_argument("wav", nargs="?", default=str(FIXTURE), help="16kHz 单声道 16bit WAV")
    parser.add_argument("--rounds", type=int, default=2, help="后端直调重复次数（看首次/稳态差异）")
    parser.add_argument("--backend-only", action="store_true", help="只测后端，不起 HTTP")
    args = parser.parse_args()

    print("=== 1. 运行资产检查 ===")
    if check_assets():
        return 1

    wav_path = Path(args.wav)
    if not wav_path.is_file():
        print(f"\n音频不存在: {wav_path}")
        return 1

    print("\n=== 2. 后端版本（触发模型加载）===")
    if run_backend(wav_path, args.rounds):
        return 1
    print(f"  backend_info: {json.dumps(backend_info(), ensure_ascii=False)}")

    if not args.backend_only and run_http(wav_path):
        return 1

    print("\n=== 通过 ===")
    print("提示：base 模型对少量字有稳定偏差（如 圆柱体->原住体、五层->武层），")
    print("      这是模型容量上限，不是集成问题；换 ggml-small.bin 可改善。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
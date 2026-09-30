"""Central writable-data paths for the Python runtime (Python 3.9+)."""
import os
import sys
from pathlib import Path


def data_dir() -> Path:
    configured = os.environ.get("CLINELIKECHAT_DATA_DIR", "").strip()
    if configured:
        root = Path(configured).expanduser()
    elif os.name == "nt":
        root = Path(os.environ.get("APPDATA", str(Path.home()))) / "ClineLikeChat"
    else:
        root = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local" / "share"))) / "ClineLikeChat"
    root = root.resolve()
    try:
        root.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        # 这里失败就是数据根不可用，任何后续写入都会失败，所以直接终止并给出可执行的出路。
        # 只写 "%APPDATA%" 的默认落点时提示环境变量，用户能自己换到可写目录。
        hint = os.environ.get("CLINELIKECHAT_DATA_DIR", "").strip()
        raise SystemExit(
            "数据目录不可写: %s (%s)\n"
            "请把 CLINELIKECHAT_DATA_DIR 指向可写目录，例如:\n"
            "  set CLINELIKECHAT_DATA_DIR=D:\\GridStarData\n"
            "%s" % (root, exc, "当前 CLINELIKECHAT_DATA_DIR=%s" % hint if hint else "")
        ) from exc
    return root


DATA_DIR = data_dir()
CONFIG_PATH = DATA_DIR / "config.json"
SESSIONS_DIR = DATA_DIR / "sessions"
LOG_DIR = DATA_DIR / "logs"
SKILLS_DIR = DATA_DIR / "skills"
UPLOADS_DIR = DATA_DIR / "uploads"

# 子目录建不出来不阻断启动：日志、会话、技能各自在真正写入时才有依赖，
# 让它们报到自己的调用点上，比在 import 阶段让整个进程崩掉更好定位。
for directory in (SESSIONS_DIR, LOG_DIR, SKILLS_DIR, UPLOADS_DIR):
    try:
        directory.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        sys.stderr.write("[paths] 数据子目录不可写: %s (%s)\n" % (directory, exc))

# 应用根目录：开发态为仓库根，打包态为 bin（res / webui 所在层）
APP_ROOT = Path(__file__).resolve().parents[2]
# 会话导出目录：与 res 同级，目录名即对外展示的相对路径前缀
EXPORT_DIR_NAME = "exports"
EXPORT_DIR = APP_ROOT / EXPORT_DIR_NAME

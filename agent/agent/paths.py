"""Central writable-data paths for the Python runtime (Python 3.9+)."""
import os
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
    root.mkdir(parents=True, exist_ok=True)
    return root


DATA_DIR = data_dir()
CONFIG_PATH = DATA_DIR / "config.json"
SESSIONS_DIR = DATA_DIR / "sessions"
LOG_DIR = DATA_DIR / "logs"
SKILLS_DIR = DATA_DIR / "skills"
UPLOADS_DIR = DATA_DIR / "uploads"

for directory in (SESSIONS_DIR, LOG_DIR, SKILLS_DIR, UPLOADS_DIR):
    directory.mkdir(parents=True, exist_ok=True)

# 应用根目录：开发态为仓库根，打包态为 bin（res / webui 所在层）
APP_ROOT = Path(__file__).resolve().parents[2]
# 会话导出目录：与 res 同级，目录名即对外展示的相对路径前缀
EXPORT_DIR_NAME = "exports"
EXPORT_DIR = APP_ROOT / EXPORT_DIR_NAME

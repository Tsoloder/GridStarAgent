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

for directory in (SESSIONS_DIR, LOG_DIR, SKILLS_DIR):
    directory.mkdir(parents=True, exist_ok=True)

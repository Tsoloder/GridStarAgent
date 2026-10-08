"""Central writable-data paths for the Python runtime (Python 3.9+).

数据目录固定在用户数据目录下的 ``.gridstar``：

* Windows：``%USERPROFILE%\\.gridstar``（即当前用户的漫游目录，默认
  ``C:\\Users\\<账号>\\AppData\\Roaming\\.gridstar``）
* Linux / macOS：``~/.local/share/.gridstar``

不再提供自定义存放路径的开关：内网多台机器、多个账号之间数据必须落在同一个
位置，路径可配会让「同代码换台机器就找不到数据」变成常态问题。写不进去时由
``data_dir_guard`` 当场修权限或请求提权，而不是换目录写。

``GRIDSTAR_DATA_DIR_PINNED`` 只服务一个场景：权限修不动时提权重启进程，提权后
``%APPDATA%`` 会变成提权账号的目录，需要把已解析好的目标目录钉给子进程，保证
它落回同一个位置。它不是对外的自定义路径入口。
"""
import os
import sys
from pathlib import Path

from data_dir_guard import PIN_ENV

DATA_DIR_NAME = ".gridstar"
_LEGACY_DIR_NAMES = ("ClineLikeChat",)

# 启动期发生的事都记在这里，交给 logging_setup 在当前进程能用的日志出口打出来。
STARTUP_NOTICES = []


def _user_data_parent() -> Path:
    """当前用户的数据目录父层。

    用 ``Path.home()``（Windows 上走 USERPROFILE）而不是 ``APPDATA``：提权重启后
    ``APPDATA`` 会指向提权账号的漫游目录，而 ``USERPROFILE`` 仍指向登录账号，
    这里的路径也正是提权前解析并钉住的那一个。
    """
    if os.name == "nt":
        return Path.home() / "AppData" / "Roaming"
    return Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local" / "share")))


def migrate_legacy_dir(root: Path) -> None:
    """把旧目录名（ClineLikeChat）搬迁到新目录名，保住已有的配置与会话。

    只在目标不存在时搬迁；两边都存在时保留旧目录并记一条提示，让使用者自己决
    定怎么合并，程序不替他做取舍。
    """
    if root.exists():
        for legacy_name in _LEGACY_DIR_NAMES:
            legacy = root.parent / legacy_name
            if legacy.is_dir():
                STARTUP_NOTICES.append(
                    "检测到旧数据目录 %s 与新目录 %s 同时存在，本次只用新目录" % (legacy, root)
                )
        return
    for legacy_name in _LEGACY_DIR_NAMES:
        legacy = root.parent / legacy_name
        if not legacy.is_dir():
            continue
        try:
            os.replace(str(legacy), str(root))
        except OSError as exc:
            STARTUP_NOTICES.append(
                "旧数据目录 %s 搬迁到 %s 失败(%s)，本次将新建空目录；旧数据未被修改"
                % (legacy, root, exc)
            )
            return
        STARTUP_NOTICES.append("已把旧数据目录 %s 搬迁到 %s" % (legacy, root))
        return


def data_dir() -> Path:
    configured = os.environ.get(PIN_ENV, "").strip()
    if configured:
        root = Path(configured).expanduser()
        root.mkdir(parents=True, exist_ok=True)
        return root.resolve()

    root = (_user_data_parent() / DATA_DIR_NAME).resolve()
    migrate_legacy_dir(root)
    try:
        root.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        # 数据根建不出来：后面的守卫会实测并尝试修权限，这里只记录。
        STARTUP_NOTICES.append("数据目录不可创建: %s (%s)" % (root, exc))
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
        STARTUP_NOTICES.append("数据子目录不可写: %s (%s)" % (directory, exc))

def ensure_subdirs() -> list:
    """确保数据根下的子目录都在，返回仍建不出来的目录描述。

    import 阶段就建过一次，但那一刻权限可能还没修好（修权限要等到启动守卫跑完），
    所以守卫之后再调一次，把先前失败的目录补上。少了这一步，uploads 会一直缺
    席，/uploads 静态挂载和附件落盘在本次运行里就永久失效了。
    """
    failures = []
    for directory in (SESSIONS_DIR, LOG_DIR, SKILLS_DIR, UPLOADS_DIR):
        try:
            directory.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            failures.append("%s (%s)" % (directory, exc))
    return failures


# 应用根目录：开发态为仓库根，打包态为 bin（res / webui 所在层）
APP_ROOT = Path(__file__).resolve().parents[2]
# 会话导出目录：与 res 同级，目录名即对外展示的相对路径前缀
EXPORT_DIR_NAME = "exports"
EXPORT_DIR = APP_ROOT / EXPORT_DIR_NAME


def startup_notices() -> list:
    return list(STARTUP_NOTICES)
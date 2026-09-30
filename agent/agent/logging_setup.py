import logging
import os
import sys
import tempfile
import weakref
from pathlib import Path

from paths import DATA_DIR, LOG_DIR

LOG_PATH = LOG_DIR / "agent.log"
# 数据目录不可写时的备选落点：仅当用户自己设了 CLINELIKECHAT_DATA_DIR，
# 而该目录又写不进去时才启用，避免日志和会话历史被拆到两个数据根下。
FALLBACK_LOG_PATH = Path(tempfile.gettempdir()) / "ClineLikeChat" / "logs" / "agent.log"

FORMAT = "%(asctime)s [%(levelname)s] %(name)s: %(message)s"


def _has_own_data_dir() -> bool:
    """用户是否显式指定了数据目录（决定要不要启用 TEMP 备选点）。"""
    return bool(os.environ.get("CLINELIKECHAT_DATA_DIR", "").strip())


def _already_configured() -> bool:
    """根 logger 是否已经有别人装好的 handler。

    不能只看 logging._handlerList 是否非空：logging 一开始就把自己的 lastResort
    兜底 handler 放进去，列表恒非空，本函数会被误判成「已配置」而直接返回，
    于是日志既没落盘也没接管，问题被原样藏起来。lastResort 的类型是
    _StderrHandler，既是 StreamHandler 的子类也不是 FileHandler，所以「列表里
    存在非 StreamHandler/FileHandler 的 handler」等价于「有人真的装过 handler」。
    """
    if logging.getLogger().handlers:
        return True
    for ref in list(getattr(logging, "_handlerList", ())):
        handler = ref() if isinstance(ref, weakref.ref) else ref
        if handler is None:
            continue
        if not isinstance(handler, (logging.FileHandler, logging.StreamHandler)):
            return True
    return False


def _candidate_log_paths():
    """按优先级返回候选日志文件。显式指定数据目录时允许回落到 TEMP。"""
    candidates = [LOG_PATH]
    if _has_own_data_dir():
        candidates.append(FALLBACK_LOG_PATH)
    return candidates


def _write_user_notice(message: str) -> None:
    try:
        sys.stderr.write(message + "\n")
        sys.stderr.flush()
    except Exception:
        pass


def setup_logging():
    """配置日志。

    落盘失败不能让进程起不来：内网机器上 %APPDATA% 可能是只读的、被组策略
    重定向的，或者上一次是以别的账号运行而留下的、当前账号写不进去的目录。
    早期实现直接把 FileHandler 的异常抛出去，import app 阶段就整个崩掉，
    报错落在 logging 内部，看到的人很难判断真正原因。
    """
    if _already_configured():
        return

    handlers = []
    for candidate in _candidate_log_paths():
        try:
            candidate.parent.mkdir(parents=True, exist_ok=True)
            handler = logging.FileHandler(str(candidate), encoding="utf-8")
        except OSError as exc:
            _write_user_notice("[logging] 无法写入日志文件 %s: %s" % (candidate, exc))
            continue
        handlers.append(handler)
        break

    if handlers:
        # 文件日志成功时同时挂一个 stderr handler：内网多为一台机器上人手一个
        # 控制台窗口，只看文件会漏掉当场就能看见的信息。
        handlers.append(logging.StreamHandler())
    else:
        _write_user_notice(
            "[logging] 日志文件不可写，仅输出到控制台。"
            "可设置 CLINELIKECHAT_DATA_DIR 指向可写目录，例如 D:\\GridStarData"
        )
    logging.basicConfig(level=logging.INFO, format=FORMAT, handlers=handlers)
    if handlers and isinstance(handlers[0], logging.FileHandler):
        logging.getLogger(__name__).warning(
            "日志落盘位置: %s (数据目录 %s)", handlers[0].baseFilename, DATA_DIR
        )

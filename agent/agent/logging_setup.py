import logging
import os
import sys
import weakref
from pathlib import Path

from paths import DATA_DIR, LOG_DIR, startup_notices

LOG_PATH = LOG_DIR / "agent.log"
# 数据目录不可写时只降级到控制台，不换目录落盘：日志与会话历史必须同源，
# 拆到两个数据根下会让「日志里有的会话，磁盘上找不到」变成新的排查陷阱。
# 目录不可写本身由 data_dir_guard 当场修权限或提权重启处理。

FORMAT = "%(asctime)s [%(levelname)s] %(name)s: %(message)s"


def _pinned_by_parent() -> bool:
    """数据目录是否由提权重启的父进程钉住（见 paths 的 PIN 说明）。"""
    from data_dir_guard import PIN_ENV

    return bool(os.environ.get(PIN_ENV, "").strip())


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


def _write_user_notice(message: str) -> None:
    try:
        sys.stderr.write(message + "\n")
        sys.stderr.flush()
    except Exception:
        pass


def setup_logging():
    """配置日志。

    落盘失败不能让进程起不来：早期实现直接把 FileHandler 的异常抛出去，import
    app 阶段就整个崩掉，报错落在 logging 内部，看到的人很难判断真正原因。
    权限本身由 paths / data_dir_guard 负责修，这里只保证「修不成也要有日志出口」。
    """
    if _already_configured():
        return

    handlers = []
    try:
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        handler = logging.FileHandler(str(LOG_PATH), encoding="utf-8")
    except OSError as exc:
        _write_user_notice("[logging] 无法写入日志文件 %s: %s" % (LOG_PATH, exc))
    else:
        handlers.append(handler)

    if handlers:
        # 文件日志成功时同时挂一个 stderr handler：内网多为一台机器上人手一个
        # 控制台窗口，只看文件会漏掉当场就能看见的信息。
        handlers.append(logging.StreamHandler())
    else:
        _write_user_notice("[logging] 日志只输出到控制台，数据目录: %s" % DATA_DIR)

    logging.basicConfig(level=logging.INFO, format=FORMAT, handlers=handlers)

    logger = logging.getLogger(__name__)
    if handlers and isinstance(handlers[0], logging.FileHandler):
        logger.warning("日志落盘位置: %s (数据目录 %s)", handlers[0].baseFilename, DATA_DIR)
    if _pinned_by_parent():
        logger.warning("数据目录由提权重启传入并钉住: %s", DATA_DIR)
    for notice in startup_notices():
        logger.warning("%s", notice)
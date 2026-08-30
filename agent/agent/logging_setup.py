import logging

from paths import LOG_DIR

LOG_PATH = LOG_DIR / "agent.log"


def setup_logging():
    logging.basicConfig(
        filename=str(LOG_PATH),
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        encoding="utf-8",
    )

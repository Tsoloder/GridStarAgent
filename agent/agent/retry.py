import asyncio
import functools
import logging
import random

from openai import RateLimitError, APIConnectionError, APITimeoutError

RETRYABLE = (RateLimitError, APIConnectionError, APITimeoutError, asyncio.TimeoutError)
try:
    import httpx
    RETRYABLE = RETRYABLE + (httpx.TimeoutException, httpx.NetworkError)
except ImportError:
    pass

logger = logging.getLogger(__name__)


def with_retry(max_retries=3, base_delay=1.0):
    def decorator(func):
        @functools.wraps(func)
        async def wrapper(*args, **kwargs):
            last_exc = None
            for attempt in range(max_retries):
                try:
                    return await func(*args, **kwargs)
                except RETRYABLE as e:
                    last_exc = e
                    if attempt < max_retries - 1:
                        delay = base_delay * (2 ** attempt) + random.uniform(0, 0.25 * base_delay)
                        logger.warning(f"retry {attempt + 1}/{max_retries} after {delay:.2f}s: {e}")
                        await asyncio.sleep(delay)
                    else:
                        logger.error(f"max retries reached: {e}")
            raise last_exc

        return wrapper

    return decorator


async def stream_with_retry(factory, max_retries=3, base_delay=1.0):
    """Retry a stream only before its first event, avoiding duplicated output."""
    for attempt in range(max_retries):
        emitted = False
        try:
            async for event in factory():
                emitted = True
                yield event
            return
        except RETRYABLE as exc:
            if emitted or attempt >= max_retries - 1:
                raise
            delay = base_delay * (2 ** attempt) + random.uniform(0, 0.25 * base_delay)
            logger.warning("stream retry %s/%s after %.2fs: %s", attempt + 1, max_retries, delay, exc)
            await asyncio.sleep(delay)

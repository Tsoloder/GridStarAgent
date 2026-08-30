"""Provider 抽象基类和共享配置。

v4 新增：
- _normalize_stop_reason()：stop_reason 归一化方法
- context_window 属性：替代 context.py 的硬编码 MODEL_LIMITS
- _post_process_tool_calls()：provider 特有的工具调用后处理
- retry_config 属性：per-provider 可配置重试策略
"""
import ssl
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import AsyncIterator, Optional


@dataclass
class ProviderConfig:
    """Provider 共享配置，通过构造函数注入。"""
    ssl_verify: bool = False           # 内网自签证书
    timeout: float = 120.0
    connect_timeout: float = 30.0


@dataclass
class RetryConfig:
    """v4 新增：per-provider 可配置重试策略。"""
    max_retries: int = 3
    base_delay: float = 1.0
    retryable_errors: tuple = ()  # provider 特有的可重试异常类型


class BaseProvider(ABC):
    """LLM Provider 抽象基类。

    每个 provider 负责把统一的 messages + tools 转换为对应 API 的请求格式，
    并把响应转换成统一的事件流。

    事件类型：
    - {"type": "text_chunk", "delta": "..."}
    - {"type": "reasoning_chunk", "delta": "..."}    # v4 新增
    - {"type": "tool_call", "id": "...", "name": "...", "args": {...}}
    - {"type": "usage", "input": N, "output": N, "total": N}
    - {"type": "done", "stop_reason": "stop|length|tool_calls|content_filter"}
    - {"type": "error", "message": "...", "retryable": false}
    """

    def __init__(self, config: "ApiConfig", provider_config: ProviderConfig):
        self._config = config
        self._provider_config = provider_config

    @abstractmethod
    def to_tools(self, mcp_tools: list) -> list:
        """把 MCP 工具列表转换为 provider 特有的工具格式。"""
        ...

    @abstractmethod
    def to_messages(self, messages: list, system_prompt: str):
        """把 OpenAI 格式的 messages 转换为 provider 特有的格式。
        返回 (system, messages) 元组。"""
        ...

    @abstractmethod
    async def stream_chat(self, messages: list, system_prompt: str,
                          tools: list, model_id: str) -> AsyncIterator[dict]:
        """流式调用 LLM API，yield 统一事件。"""
        ...

    @abstractmethod
    def model_name(self) -> str:
        """返回当前 provider 使用的模型名称。"""
        ...

    # ============================================================
    # v4 新增：可重写的钩子方法
    # ============================================================

    def _normalize_stop_reason(self, raw: str) -> str:
        """把 provider 特有的 stop_reason 归一化为标准值。

        标准值：'stop' | 'length' | 'tool_calls' | 'content_filter'
        子类重写此方法以处理 provider 特有的命名。
        """
        return raw or "stop"

    @property
    def context_window(self) -> int:
        """该 provider 支持的最大上下文 token 数。

        子类重写此属性，根据当前 model_id 返回对应的窗口大小。
        默认返回 32000。
        """
        return 32000

    def _post_process_tool_calls(self, tool_calls: list) -> list:
        """修复 provider 特有的工具调用问题。

        子类可重写此方法处理：
        - 工具调用参数 JSON 解析失败的宽松修复
        - tool_call id 重复的去重
        - 其他 provider 特有怪癖
        """
        return tool_calls

    @property
    def retry_config(self) -> RetryConfig:
        """该 provider 的重试策略。子类可重写。"""
        return RetryConfig()

    # ============================================================
    # 共享工具方法
    # ============================================================

    def _make_ssl_context(self) -> ssl.SSLContext:
        """创建 SSL 上下文（内网自签证书场景）。"""
        ctx = ssl.create_default_context()
        if not self._provider_config.ssl_verify:
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
        return ctx

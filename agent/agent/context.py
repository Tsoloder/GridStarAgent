import copy
import hashlib
import logging

from config import ApiConfig
from paths import SESSIONS_DIR

logger = logging.getLogger(__name__)

MAX_TURNS = 99999


def max_tokens_for_config(
    config: ApiConfig,
    model_key: str = "",
    runtime=None,
) -> int:
    """返回当前完整 provider/model 标识对应的上下文窗口。

    窗口大小只认 model catalog（runtime 里的 ModelConfig）。这里不再回落到按模型名
    猜的硬编码表：解析不出来就是配置问题，直接抛出来，别让压缩预算悄悄偏离真实窗口。
    """
    resolved_key = model_key or config.default_model
    if runtime is not None:
        return runtime.context_window(resolved_key)
    return config.model(resolved_key).context_window


class TokenCounter:
    def __init__(self):
        self._calibration = 4.0

    def estimate(self, text: str) -> int:
        if not text:
            return 0
        divisor = max(1.0, float(self._calibration))
        return max(1, int(len(text) / divisor))

    def calibrate(self, text: str, actual_tokens: int):
        if actual_tokens > 0 and len(text) > 0:
            ratio = len(text) / actual_tokens
            # Some providers report token counts whose ratio to Python string
            # length is below one. Keep the estimator divisor positive and
            # bounded so later context compression can never divide by zero.
            ratio = min(16.0, max(0.25, ratio))
            self._calibration = min(
                16.0, max(0.25, 0.7 * self._calibration + 0.3 * ratio)
            )


class ContextManager:
    STALE_TURNS = 6
    LARGE_RESULT_CHARS = 30000
    SHORT_MSG_CHARS = 100
    # Layer 4 压缩后保留最近消息的 token 预算（参考 pi-main keepRecentTokens=20000）
    KEEP_RECENT_TOKENS = 20000

    def __init__(self):
        self.counter = TokenCounter()
        self._sessions_dir = SESSIONS_DIR

    async def compress(
        self,
        messages: list,
        session_id: str,
        config: ApiConfig,
        model_key: str = "",
        runtime=None,
    ) -> list:
        messages = copy.deepcopy(messages)
        max_tokens = max_tokens_for_config(config, model_key, runtime)
        threshold_warn = int(max_tokens * 0.8)
        threshold_micro = int(max_tokens * 0.6)
        current = sum(self.counter.estimate(self._msg_text(m)) for m in messages)

        messages = self._layer1_budget_truncate(messages, max_tokens)
        messages = self._layer2_stale_snip(messages, self.STALE_TURNS)
        if current > threshold_micro:
            messages = self._layer3_microcompact(messages)
        if current > threshold_warn:
            messages = await self._layer4_auto_compact(
                messages, config, model_key, runtime
            )
        return messages

    def _layer1_budget_truncate(self, messages, max_tokens):
        """从头删除消息直到总 token 在预算内。

        参考 pi-main findCutPoint:只在有效消息边界切,
        不切在 tool result 和 assistant tool_calls 之间,
        避免破坏工具调用-结果的配对关系。
        """
        while sum(self.counter.estimate(self._msg_text(m)) for m in messages) > max_tokens * 0.9:
            if len(messages) <= 2:
                break
            # 找到下一个安全的切点:user 消息或 assistant 消息的开头
            # tool result(role=tool)必须与其前面的 assistant tool_calls 一起保留或一起删除
            cut_index = 0
            for i in range(1, len(messages)):
                role = messages[i].get("role", "")
                if role in ("user", "system"):
                    cut_index = i
                    break
                if role == "assistant" and not messages[i - 1].get("role") == "assistant":
                    cut_index = i
                    break
            if cut_index == 0:
                cut_index = 1
            del messages[:cut_index]
        return messages

    # tool_name 中带这些关键词的 tool result 不被 Layer 2 截断,
    # 因为它们包含 skill 指令全文,截断后会导致 LLM 丢失关键步骤信息。
    _PRESERVE_TOOL_NAMES = {"read_skill", "read_skill_resource"}

    # 单条 tool result 的最大字节数,超过此值才触发 head+tail 截断。
    # 参考 openhanako compaction-guard-ext L1 (32KB ≈ 8K token)。
    _MAX_TOOL_RESULT_BYTES = 32 * 1024

    @staticmethod
    def _truncate_head_tail(text: str, max_bytes: int = None) -> str:
        """对长文本做 head+tail 截断,保留头尾各 40%,中间塞省略标记。

        参考 openhanako compaction-utils.ts 的 truncateTextHeadTail。
        比 "前200字+省略号" 更优:头部保留开头的关键信息,
        尾部保留结尾的结论/状态,中间用省略标记衔接。
        """
        if max_bytes is None:
            max_bytes = ContextManager._MAX_TOOL_RESULT_BYTES
        original_bytes = len(text.encode("utf-8"))
        if original_bytes <= max_bytes:
            return text
        head_bytes = max_bytes * 2 // 5  # 40% 给头
        tail_bytes = max_bytes * 2 // 5  # 40% 给尾
        # 按 UTF-8 字节切,但要保证不切到多字节字符中间
        encoded = text.encode("utf-8")
        head = encoded[:head_bytes].decode("utf-8", errors="ignore").rstrip()
        tail = encoded[-tail_bytes:].decode("utf-8", errors="ignore").lstrip()
        omitted = original_bytes - len(head.encode("utf-8")) - len(tail.encode("utf-8"))
        return (
            f"{head}\n\n"
            f"[... {omitted}B 已省略 (原始长度 {original_bytes}B) ...]\n\n"
            f"{tail}"
        )

    def _layer2_stale_snip(self, messages, stale_turns):
        for i, m in enumerate(messages):
            if m.get("role") == "tool" and i < len(messages) - stale_turns * 2:
                if m.get("tool_name") in self._PRESERVE_TOOL_NAMES:
                    continue
                content = m.get("content", "")
                if isinstance(content, str):
                    m["content"] = self._truncate_head_tail(content)
        return messages

    def _layer3_microcompact(self, messages):
        result = []
        i = 0
        while i < len(messages):
            if (
                messages[i].get("role") == "assistant"
                and not messages[i].get("tool_calls")
                and len(self._msg_text(messages[i])) < self.SHORT_MSG_CHARS
            ):
                group = [messages[i]]
                j = i + 1
                while (
                    j < len(messages)
                    and messages[j].get("role") == "assistant"
                    and not messages[j].get("tool_calls")
                    and len(self._msg_text(messages[j])) < self.SHORT_MSG_CHARS
                ):
                    group.append(messages[j])
                    j += 1
                if len(group) >= 3:
                    merged = " ".join(self._msg_text(m) for m in group)
                    result.append({"role": "assistant", "content": merged})
                    i = j
                    continue
            result.append(messages[i])
            i += 1
        return result

    async def _layer4_auto_compact(
        self,
        messages,
        config,
        model_key="",
        runtime=None,
    ):
        try:
            summary = await self._summarize_via_llm(
                messages, config, model_key, runtime
            )
            # 保留最近 KEEP_RECENT_TOKENS token 的消息（参考 pi-main keepRecentTokens）
            # 而非固定 8 条,确保多步骤业务流程的上下文不会被过度截断
            recent = self._keep_recent_by_token_budget(messages, self.KEEP_RECENT_TOKENS)
            return [
                {"role": "system", "content": f"[Previous conversation summary]: {summary}"}
            ] + recent
        except Exception as e:
            logger.warning(f"auto-compact LLM call failed: {e}, fallback to truncation")
            return self._layer1_budget_truncate(
                messages, int(max_tokens_for_config(config, model_key, runtime) * 0.5)
            )

    def _keep_recent_by_token_budget(self, messages, budget_tokens):
        """从消息列表尾部向前保留,直到累积 token 超过预算。

        参考 pi-main 的 findCutPoint 逻辑:保留最近的上下文,
        确保多步骤业务流程的关键消息不会被过早丢弃。
        """
        recent = []
        accumulated = 0
        for m in reversed(messages):
            estimated = self.counter.estimate(self._msg_text(m))
            if accumulated + estimated > budget_tokens and recent:
                break
            recent.insert(0, m)
            accumulated += estimated
        return recent

    # 结构化摘要 prompt（参考 pi-main SUMMARIZATION_PROMPT）
    _SUMMARY_SYSTEM_PROMPT = "你是对话上下文总结助手。你的任务是阅读用户与 AI 助手之间的对话,然后按照指定格式输出结构化摘要。不要继续对话,不要回答对话中的问题,只输出结构化摘要。"

    _SUMMARY_PROMPT = """请用以下格式总结以下对话,另一个 LLM 将使用这个摘要继续工作。使用这个精确格式:\n\n## 目标\n[用户要完成什么?如果是多个任务请列出]\n\n## 约束与偏好\n- [用户提到的约束、偏好或要求]\n- [如果没有则写 "(无)"]\n\n## 进度\n### 已完成\n- [x] [已完成的任务/变更]\n### 进行中\n- [ ] [当前工作]\n### 阻塞\n- [阻碍进展的问题,如果有]\n\n## 关键决策\n- **[决策]**: [简要原因]\n\n## 下一步\n1. [接下来应该做什么的有序列表]\n\n## 关键上下文\n- [继续工作所需的数据、示例或引用]\n- [必须保留:已调用的工具名称及其返回值中的关键数据(如 ID、点数、坐标等),后续步骤依赖这些数据]\n- [如果没有则写 "(无)"]\n\n保持每个部分简洁。保留精确的文件路径、函数名、工具名称和错误信息。"""

    _SUMMARY_UPDATE_PROMPT = """以上是新的对话消息,请将其纳入已有的摘要中。规则:\n- 保留已有摘要中的所有信息\n- 从新消息中补充新的进度、决策和上下文\n- 更新进度部分:完成的任务从"进行中"移到"已完成"\n- 根据已完成的工作更新"下一步"\n- 保留精确的文件路径、函数名、工具名称和错误信息\n- 必须保留已调用工具的名称及其返回值中的关键数据(如 ID、点数、坐标等),后续步骤依赖这些数据\n- 如果某些信息不再相关,可以移除\n\n使用与之前相同的格式输出更新后的摘要。"""

    async def _summarize_via_llm(
        self,
        messages,
        config,
        model_key="",
        runtime=None,
    ) -> str:
        if runtime is None:
            raise RuntimeError("ModelRuntime is required for context summarization")
        resolved_key = model_key or config.default_model

        # 增量摘要:检查是否已有上次摘要（system 消息中带 Previous conversation summary）
        previous_summary = None
        history_messages = messages
        for idx, m in enumerate(messages):
            if m.get("role") == "system" and "[Previous conversation summary]" in str(m.get("content", "")):
                previous_summary = str(m.get("content", "")).replace("[Previous conversation summary]: ", "")
                # 只摘要上次摘要之后的消息
                history_messages = messages[idx + 1:]
                break

        # 摘要输入:取最近 40 条消息(覆盖一个完整业务流程单元),
        # 每条最多 1000 字(保留工具返回值中的关键数据如 new_id/longids/shortids)
        conversation_text = "\n".join(
            f"{m.get('role', '?')}: {self._msg_text(m)[:1000]}" for m in history_messages[-40:]
        )

        if previous_summary:
            prompt = (
                f"<conversation>\n{conversation_text}\n</conversation>\n\n"
                f"<previous-summary>\n{previous_summary}\n</previous-summary>\n\n"
                f"{self._SUMMARY_UPDATE_PROMPT}"
            )
        else:
            prompt = f"<conversation>\n{conversation_text}\n</conversation>\n\n{self._SUMMARY_PROMPT}"

        summary_messages = [
            {
                "role": "user",
                "content": prompt,
            }
        ]
        result_text = ""
        async for event in runtime.stream(
            resolved_key, summary_messages, system_prompt=self._SUMMARY_SYSTEM_PROMPT
        ):
            if event.type == "text_delta":
                result_text += event.delta
            elif event.type == "error":
                raise RuntimeError(event.message)
        return result_text.strip()

    def _msg_text(self, msg) -> str:
        content = msg.get("content") if isinstance(msg.get("content"), str) else ""
        attachments = msg.get("attachments", [])
        if attachments:
            content += " ".join(str(item.get("text", "")) for item in attachments)
        # tool result 消息:带上 tool_name 让 LLM 摘要知道这是哪个工具的返回值
        if msg.get("role") == "tool" and msg.get("tool_name"):
            return f"[tool_result from {msg['tool_name']}]: {content}"
        if content:
            return content
        if msg.get("tool_calls"):
            # 提取工具调用信息时,包含工具名和参数,让 LLM 摘要能看到调了什么工具
            parts = []
            for tc in msg["tool_calls"]:
                func = tc.get("function", {})
                name = func.get("name", "")
                args = func.get("arguments", "")
                if name:
                    parts.append(f"[tool_call: {name}({args})]")
                else:
                    parts.append(args)
            return " ".join(parts)
        if msg.get("role") == "workflow":
            parts = [str(msg.get("status", "")), str(msg.get("message", ""))]
            for step in msg.get("steps", []):
                if isinstance(step, dict):
                    parts.extend([
                        str(step.get("tool", "")),
                        str(step.get("params", "")),
                        str(step.get("status", "")),
                        str(step.get("result", "")),
                    ])
            return " ".join(part for part in parts if part)
        return ""

    def persist_large_result(self, result: str, session_id: str) -> str:
        if len(result) > self.LARGE_RESULT_CHARS:
            results_dir = self._sessions_dir / session_id / "results"
            path = results_dir / f"{hashlib.md5(result.encode()).hexdigest()}.txt"
            from session import atomic_write

            atomic_write(str(path), result)
            return f"[Result persisted to file: {path}, size: {len(result)} chars]"
        return result

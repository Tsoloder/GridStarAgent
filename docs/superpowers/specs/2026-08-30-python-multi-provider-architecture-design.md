# Python 多 Provider LLM 架构设计

## 1. 目标

将现有 LLM 客户端完整重构为协议无关的 Python 分层架构，使每个模型独立决定 Provider、API Adapter、认证、上下文窗口和兼容能力。

首批支持：

- OpenAI Chat Completions
- OpenAI Responses
- Anthropic Messages
- OpenAI-compatible 服务，包括 DeepSeek、Ollama、OpenRouter 等

本次采用全新配置和运行时架构，不保留旧 Provider 与旧配置的兼容层。WebUI 的聊天、会话和工具折叠体验保留，但模型标识升级为 `provider/model`。

## 2. 设计原则

- Provider 与 API 协议分离。
- 模型是运行时路由与能力判断的中心。
- 内部消息格式不从属于任一服务商。
- 协议差异通过 Adapter 和模型级 `compat` 表达。
- 跨 Provider 历史转换集中处理，不散落在各 Adapter。
- 流式工具参数只有完整解析并通过 Schema 校验后才能执行。
- 未知 Provider、Adapter 或配置字段立即报错，不静默回退。
- API Key 不得出现在日志、事件或配置响应中。

## 3. 模块结构

```text
llm_client/
├── types.py
├── config.py
├── runtime.py
├── registry.py
├── catalog.py
├── transform.py
├── retry.py
├── providers/
│   ├── base.py
│   ├── openai.py
│   ├── anthropic.py
│   └── openai_compatible.py
└── adapters/
    ├── base.py
    ├── openai_chat.py
    ├── openai_responses.py
    └── anthropic_messages.py
```

### 3.1 ModelRuntime

Agent Loop 只依赖 `ModelRuntime.stream(model_key, messages, tools)`。运行时完成：

1. 从 ModelCatalog 获取模型快照；
2. 从 ProviderRegistry 获取 Provider；
3. 从 AdapterRegistry 获取 Adapter；
4. 使用 MessageTransformer 规范化历史；
5. 使用 Provider 解析认证并提供 HTTP transport；
6. 使用 Adapter 构建请求、解析流并输出统一事件。

### 3.2 Provider

Provider 只负责：

- API Key 和环境变量解析；
- base URL 与请求头；
- HTTP 客户端生命周期；
- Provider 默认值；
- 模型自动发现。

Provider 不负责消息协议转换或响应解析。

### 3.3 API Adapter

Adapter 只负责：

- 将统一消息转换为目标 API 请求；
- 转换工具 Schema；
- 解析流式响应；
- 标准化 usage、stop reason 和错误。

### 3.4 ModelCatalog

ModelCatalog 负责加载配置、并发发现模型、合并模型来源，并向 WebUI 提供不可变模型目录快照。

### 3.5 MessageTransformer

MessageTransformer 负责跨 Provider 历史规范化，包括 thinking 降级、私有签名过滤、工具调用 ID 归一化、缺失工具结果补全和不完整 assistant turn 过滤。

## 4. 配置结构

使用单个 JSON 文件：

```json
{
  "version": 1,
  "default_model": "openai/gpt-5",
  "providers": [
    {
      "id": "openai",
      "name": "OpenAI",
      "base_url": "https://api.openai.com/v1",
      "api_key": "",
      "api_key_env": "OPENAI_API_KEY",
      "headers": {},
      "discover_models": true,
      "discovery_api": "openai",
      "default_api": "openai-responses",
      "enabled": true
    }
  ],
  "models": [
    {
      "id": "gpt-5",
      "provider": "openai",
      "api": "openai-responses",
      "name": "GPT-5",
      "enabled": true,
      "context_window": 400000,
      "max_output_tokens": 128000,
      "capabilities": {
        "tools": true,
        "parallel_tools": true,
        "reasoning": true,
        "vision": true,
        "stream_usage": true
      },
      "compat": {}
    }
  ]
}
```

### 4.1 模型标识

内部、WebUI 和会话统一使用 `provider_id/model_id`，避免不同 Provider 的同名模型冲突。

### 4.2 API Key

解析优先级：

1. `api_key_env` 指向的非空环境变量；
2. 配置中的 `api_key`；
3. 无认证。

配置 API 返回明文 Key 的脱敏值；客户端回传脱敏占位值时不覆盖原密钥。日志、异常和事件必须脱敏。

### 4.3 Capabilities 与 Compat

`capabilities` 表示模型是否支持 tools、parallel tools、reasoning、vision 和 stream usage。

`compat` 表示协议差异，例如：

- `max_tokens_field`
- `reasoning_field`
- `supports_developer_role`
- `supports_strict_tools`
- `tool_result_requires_name`
- `tool_result_requires_assistant_after`

Adapter 不按模型名称硬编码行为。

### 4.4 配置校验

加载和保存时检查：

- Provider ID 唯一；
- 完整模型标识唯一；
- 模型引用的 Provider 存在；
- API Adapter 已注册；
- base URL 使用 HTTP/HTTPS；
- token 限制为正数；
- 默认模型存在且启用；
- headers 为字符串键值；
- 未知字段报错。

## 5. 模型发现与合并

### 5.1 发现协议

- OpenAI 与 OpenAI-compatible：`GET {base_url}/models`
- Anthropic：使用 Anthropic 模型列表接口
- `discovery_api = none`：不发现

只采信服务端明确返回的模型 ID、显示名、创建时间和组织信息，不按模型名称推断能力。

### 5.2 合并优先级

```text
保守默认值 → Provider 默认值 → 自动发现结果 → JSON 手动配置
```

保守默认 capability 全部为 false；默认 `context_window=32768`、`max_output_tokens=4096`。手动配置按字段覆盖，`capabilities` 和 `compat` 按键合并。

自动发现的新模型加入目录；未发现到的手动模型继续保留。模型状态为：

- `configured`
- `discovered`
- `configured_and_discovered`
- `unavailable`

`unavailable` 不直接禁止调用。

### 5.3 失败与并发

发现失败不阻止启动或聊天，并保留手动模型。Provider 独立超时和失败。每个 Provider 同时最多一个刷新任务；旧 generation 结果不能覆盖新结果。刷新完成后原子替换目录快照，既有聊天继续使用请求开始时的模型快照。

首版不实现磁盘缓存、ETag 和远端目录持久化。

## 6. 统一消息模型

统一 Message 包含 `role`、有类型的 `content` 和可选 Provider 元数据。内容块包括：

- `TextBlock`
- `ThinkingBlock`
- `ImageBlock`
- `ToolCallBlock`
- `ToolResultBlock`

### 6.1 ThinkingBlock

保存可读文本、私有 signature、redacted 状态及来源 Provider/API/模型。私有签名仅在同 Provider、同 API、同模型时回放。跨模型时保留可读文本并删除签名；无可读文本的 redacted thinking 直接删除。

### 6.2 ToolCallBlock

保存 call ID、工具名、最终参数、可选原始参数和私有签名。原始参数只用于流式诊断。工具执行只使用完整且通过 Schema 校验的最终参数。

### 6.3 ToolResultBlock

保存 tool call ID、工具名、文本或图片内容和 `is_error`。

### 6.4 请求前转换

转换作用于请求副本，不修改持久化历史：

1. 丢弃错误或中断的 assistant turn；
2. 处理跨模型 thinking；
3. 归一化目标协议的工具调用 ID；
4. 同步更新工具结果 ID；
5. 为孤立工具调用补充 `No result provided` 错误结果；
6. 降级目标模型不支持的图片、reasoning 或工具内容。

## 7. 统一流式事件

所有 Adapter 返回 `AsyncIterator[StreamEvent]`。正常生命周期为：

```text
start
├─ text_start → text_delta* → text_end
├─ thinking_start → thinking_delta* → thinking_end
├─ tool_call_start → tool_call_delta* → tool_call_end
├─ usage
└─ done
```

失败以 `error` 终止。`done` 和 `error` 互斥、唯一且必须是最后一个事件。

所有事件包含严格递增的 `sequence` 和单次请求唯一的 `response_id`。

### 7.1 工具调用事件

每个工具调用按 index 独立缓冲。一个 Provider chunk 可以生成多个 `tool_call_delta`，不得互相覆盖。

`tool_call_delta` 可携带参数字符串增量和仅供 UI 使用的 partial arguments。只有 `tool_call_end.arguments` 是执行依据。解析失败时设置 `parse_error`，禁止执行。

### 7.2 Usage

统一记录 input、output、reasoning、cache read、cache write 和 total tokens。Provider 未返回的值保持 `None`。

### 7.3 Done

Done 包含标准 stop reason 和最终完整 assistant Message。stop reason 包括：

- `stop`
- `length`
- `tool_use`
- `content_filter`
- `cancelled`
- `unknown`

### 7.4 Error

错误类别包括 authentication、rate limit、timeout、connection、invalid request、context length、content filter、provider error、protocol error 和 cancelled，并携带 retryable、status code 与 retry after。

### 7.5 状态机约束

- start 只出现一次；
- 内容块必须按 start、delta、end 配对；
- call ID 唯一；
- 终止前关闭所有内容块；
- 终止后不得产生事件；
- Provider 流异常结束产生 protocol error，不伪造 done。

## 8. Adapter 行为

### 8.1 OpenAI Chat Completions

支持文本、兼容 reasoning 字段、并行工具调用、usage 和 finish reason。按 `tool_call.index` 独立累积参数。是否发送 stream options、developer role、strict tools 及 token 字段名由模型 compat 控制。

### 8.2 OpenAI Responses

支持 input item、developer instruction、text、reasoning、function call、function output、usage 和 response status。保存同模型回放需要的私有 item/call 元数据。

首版不实现 deferred tools、grammar tools、background response 和跨进程 encrypted reasoning 恢复。

### 8.3 Anthropic Messages

支持独立 system prompt、连续 user/tool result 合并、text、thinking、redacted thinking、并行 tool use、usage 和 stop reason。

SSE 按 event 行、一个或多个 data 行、空行提交事件的状态机解析。

## 9. 工具调用安全

工具调用必须同时满足以下条件才执行：

1. 已收到完整 ToolCallEnd；
2. 没有 parse error；
3. arguments 是 JSON object；
4. 工具名存在；
5. 参数通过工具 JSON Schema；
6. 响应未因 length 或协议错误终止。

查询工具可并发，操作工具顺序执行。操作状态未知时停止后续工具且不自动重试。不支持并行工具的模型每轮只接受一个调用，其余调用作为错误结果反馈模型重新规划。

## 10. 重试与错误恢复

### 10.1 配置

```json
{
  "max_attempts": 3,
  "base_delay": 1.0,
  "max_delay": 30.0,
  "jitter": true
}
```

`max_attempts` 包含首次请求。

### 10.2 可重试错误

重试 408、临时 409、429、500、502、503、504，以及首事件前的临时连接、DNS 和读取超时。

不重试 400、401、403、404、context length、content filter、工具参数错误和用户取消。

退避优先遵守 Retry-After 或 Provider 限流重置时间，否则使用有抖动的指数退避。

### 10.3 流式边界

- 尚无用户可见输出：可自动重试；
- 已输出文本或 thinking：不自动重试，避免重复内容；
- 工具调用未完成：不执行；
- 工具调用已完成：正常进入工具执行。

### 10.4 Context Length

使用当前模型的 context window 触发一次上下文压缩并重建请求。每次 Agent 运行最多自动恢复一次，状态保存在本次运行上下文，不跨会话共享。

### 10.5 429

解析 `Retry-After` 和 `retry-after-ms`，使用 Provider 级冷却时间协调并发请求。达到最大尝试次数后输出结构化 rate limit 错误，WebUI 显示明确提示。

### 10.6 取消

取消从 Agent Loop 传到 Runtime、Adapter 和 HTTP 流。取消后关闭响应，不重试、不执行未完成工具，并将 assistant 消息标记为中断。

## 11. Agent Loop 与 WebUI 衔接

Agent Loop 消费统一事件。为 WebUI 提供过渡映射：

- TextDelta → text
- ThinkingDelta → reasoning
- ToolCallEnd → tool_call
- Usage → usage
- Done → done
- Error → error

WebUI 模型选择和会话保存改用 `provider/model`。工具调用分组折叠继续使用完成的工具调用和工具执行结果。

## 12. 测试策略

### 12.1 配置和目录

覆盖配置校验、认证优先级、密钥脱敏、发现合并、发现失败、同名模型、generation 竞态和默认模型有效性。

### 12.2 消息转换

覆盖 OpenAI/Anthropic 双向历史转换、thinking 降级、私有签名过滤、工具 ID 同步、孤立结果补全、中断消息过滤以及 vision/tools 降级。

### 12.3 Adapter

使用录制事件或 mock transport，不访问真实 API：

- OpenAI Chat：文本、reasoning、同 chunk 多工具、跨 chunk 参数、usage、截断和非法 JSON；
- OpenAI Responses：text、reasoning、function call/output、usage 和 incomplete/failed；
- Anthropic：多行 SSE、text、thinking、redacted thinking、多 tool use、参数增量、usage 和流中断。

统一断言 sequence 递增、终止事件唯一且最后、内容块完整闭合、多工具不丢失。

### 12.4 重试

覆盖 429 Retry-After、5xx 退避、401 不重试、首事件前重试、输出后不重试、max attempts 语义、会话隔离和取消。测试注入虚拟时钟与随机函数，不真实等待。

### 12.5 集成和 WebUI

覆盖从模型选择到 Provider/Adapter 路由、流式输出、工具调用、工具结果、第二轮响应和会话保存的完整链路，以及模型刷新、发现错误、429 文案、跨 Provider 切换和工具折叠。

## 13. 验收标准

1. 模型选择同时决定 Provider、Adapter、URL、认证和能力；
2. 未知 Provider 或 Adapter 不静默回退；
3. 四类目标协议均可运行；
4. 同一流式 chunk 的多个工具调用不丢失；
5. 非法或截断工具参数绝不执行；
6. done/error 唯一且最后；
7. 429 和临时 5xx 按规则重试；
8. 重试和截断状态不跨会话污染；
9. 切换 Provider 后文本和完整工具历史可继续；
10. 私有 reasoning 元数据不发送到其他模型；
11. 自动发现失败不影响手动模型和启动；
12. 新增测试、现有 Agent Loop 测试及 WebUI 测试通过；
13. API Key 不出现在日志、事件、配置响应和快照；
14. HTTP 客户端在应用关闭时释放。

## 14. 明确不在首版范围

- 旧配置和旧 Provider 的兼容迁移层；
- 动态模型目录磁盘缓存、ETag 与离线持久化；
- OpenAI deferred tools、grammar tools 和 background responses；
- Provider OAuth 登录流程；
- 跨进程恢复加密 reasoning；
- 根据模型名称猜测 capability。

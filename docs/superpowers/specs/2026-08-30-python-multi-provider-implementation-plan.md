# Python 多 Provider LLM 重构实施计划

## 实施约束

- 依据已批准的 [架构设计](./2026-08-30-python-multi-provider-architecture-design.md) 实施。
- 使用全新配置，不提供旧配置迁移层。
- 每个阶段先补测试，再实现最小代码使测试通过。
- Provider 网络测试使用 mock transport 或录制事件，不访问真实 API。
- 不改动 MCP 工具定义与调用参数。
- 在所有新增测试通过前，不删除旧 Provider 文件；最终切换完成后再清理。

## 阶段 1：建立统一类型与配置模型

### 任务 1.1：定义统一模型、消息和事件类型

新增：

- `agent/agent/llm_client/types.py`
- `agent/agent/tests/test_llm_types.py`

实现不可变 dataclass：

- ProviderConfig、ModelConfig、ModelCapabilities、RetryPolicy
- TextBlock、ThinkingBlock、ImageBlock、ToolCallBlock、ToolResultBlock、Message
- Start、TextStart/Delta/End、ThinkingStart/Delta/End
- ToolCallStart/Delta/End、Usage、Done、Error

测试：

- 类型可以稳定序列化和反序列化；
- sequence、response ID 和终止事件字段完整；
- ToolCallEnd 明确区分最终 arguments 与 parse error。

验证：

```powershell
python -m pytest agent/agent/tests/test_llm_types.py -q
```

### 任务 1.2：实现严格配置加载与脱敏

修改：

- `agent/agent/config.py`
- `agent/agent/paths.py`（仅在需要新增模型配置路径常量时）

新增测试：

- `agent/agent/tests/test_model_config.py`

实现：

- 加载 version、default_model、providers、models；
- 完整模型键 `provider/model`；
- 环境变量优先于明文 Key；
- 配置输出脱敏；
- 脱敏占位值更新时保留原 Key；
- 严格校验重复 ID、未知字段、无效 URL、Adapter 引用和默认模型。

删除旧配置语义：

- `api_type`
- `api_url`
- 全局 `api_key`
- `default_model_index`
- `ResolveModelId()` 和 `ResolveProvider()`

验证：

```powershell
python -m pytest agent/agent/tests/test_model_config.py -q
```

## 阶段 2：Provider、注册表和模型目录

### 任务 2.1：实现 Provider transport 与注册表

新增：

- `agent/agent/llm_client/providers/__init__.py`
- `agent/agent/llm_client/providers/base.py`
- `agent/agent/llm_client/providers/openai.py`
- `agent/agent/llm_client/providers/anthropic.py`
- `agent/agent/llm_client/providers/openai_compatible.py`
- `agent/agent/llm_client/registry.py`
- `agent/agent/tests/test_provider_registry.py`

实现：

- ProviderRegistry 和 AdapterRegistry；
- 未知 ID 明确报错；
- Provider 解析认证头、base URL、timeout、SSL 与自定义 headers；
- 每个 Provider 复用 httpx.AsyncClient；
- 应用关闭时 `aclose()`；
- OpenAI-compatible 支持 Bearer 和无认证模式。

测试：

- 认证优先级；
- header 合并；
- 未知 Provider/Adapter；
- 客户端复用和释放；
- Key 不出现在异常文本。

验证：

```powershell
python -m pytest agent/agent/tests/test_provider_registry.py -q
```

### 任务 2.2：实现模型发现和目录原子快照

新增：

- `agent/agent/llm_client/catalog.py`
- `agent/agent/tests/test_model_catalog.py`

实现：

- OpenAI `/models` 发现；
- Anthropic 模型列表发现；
- 手动配置与发现结果按批准规则合并；
- 保守 capability 默认值；
- configured/discovered/configured_and_discovered/unavailable 状态；
- 单 Provider 刷新任务复用；
- generation 防止旧结果覆盖；
- Provider 失败隔离；
- 不可变目录快照。

验证：

```powershell
python -m pytest agent/agent/tests/test_model_catalog.py -q
```

## 阶段 3：统一消息与跨 Provider 转换

### 任务 3.1：实现现有会话消息到统一消息的映射

新增：

- `agent/agent/llm_client/transform.py`
- `agent/agent/tests/test_message_transform.py`

实现：

- 当前 user/assistant/tool 字典与统一 Message 双向转换；
- assistant 文本和 tool_calls 合并为内容块；
- tool result 保留 tool name 与错误状态；
- Provider 元数据独立保存；
- 不修改输入历史。

验证：

```powershell
python -m pytest agent/agent/tests/test_message_transform.py -q
```

### 任务 3.2：实现跨模型历史规范化

扩展：

- `agent/agent/llm_client/transform.py`
- `agent/agent/tests/test_message_transform.py`

实现：

- 同模型保留 reasoning signature；
- 跨模型删除私有 signature；
- redacted thinking 删除规则；
- 工具调用 ID 目标协议归一化并同步结果；
- orphan tool call 补错误结果；
- 丢弃 error/aborted assistant turn；
- vision、reasoning、tools 能力降级；
- 连续 Anthropic tool results 可聚合。

验证：

```powershell
python -m pytest agent/agent/tests/test_message_transform.py -q
```

## 阶段 4：统一流状态机和 Adapter

### 任务 4.1：实现流事件构建器与状态机

新增：

- `agent/agent/llm_client/stream.py`
- `agent/agent/tests/test_stream_state.py`

实现：

- response ID 与 sequence；
- 内容块 start/delta/end；
- 并行工具调用独立缓冲；
- partial JSON 仅供预览；
- 完整 JSON 解析失败生成 parse error；
- done/error 唯一且最后；
- 流意外结束生成 protocol error；
- 最终 assistant Message 构建。

验证：

```powershell
python -m pytest agent/agent/tests/test_stream_state.py -q
```

### 任务 4.2：实现 OpenAI Chat Completions Adapter

新增：

- `agent/agent/llm_client/adapters/__init__.py`
- `agent/agent/llm_client/adapters/base.py`
- `agent/agent/llm_client/adapters/openai_chat.py`
- `agent/agent/tests/test_openai_chat_adapter.py`

实现：

- 消息和工具转换；
- compat 控制 developer role、stream usage、token 字段、reasoning 字段和 strict tools；
- 一个 chunk 中多个 tool call delta 全部保留；
- 跨 chunk 参数拼接；
- usage 与 finish reason 归一化；
- 非法参数不降级为空字典。

验证：

```powershell
python -m pytest agent/agent/tests/test_openai_chat_adapter.py -q
```

### 任务 4.3：实现 OpenAI Responses Adapter

新增：

- `agent/agent/llm_client/adapters/openai_responses.py`
- `agent/agent/tests/test_openai_responses_adapter.py`

实现：

- Responses input items；
- developer instructions；
- text、reasoning、function call 与 function output；
- usage 和 response status；
- 同模型回放所需 item/call 元数据；
- failed/incomplete 状态映射。

不实现 deferred、grammar 和 background responses。

验证：

```powershell
python -m pytest agent/agent/tests/test_openai_responses_adapter.py -q
```

### 任务 4.4：实现 Anthropic Messages Adapter

新增：

- `agent/agent/llm_client/adapters/anthropic_messages.py`
- `agent/agent/tests/test_anthropic_messages_adapter.py`

实现：

- system、messages、tools 请求转换；
- 合并连续 tool results；
- SSE event/data/空行提交状态机；
- text、thinking、redacted thinking；
- 多个 tool use 和参数增量；
- usage 和 stop reason；
- 流异常协议错误。

验证：

```powershell
python -m pytest agent/agent/tests/test_anthropic_messages_adapter.py -q
```

## 阶段 5：重试、错误分类与运行时

### 任务 5.1：实现统一错误分类和重试

修改：

- `agent/agent/llm_client/retry.py`（若现文件职责不符则以新实现替换）
- `agent/agent/retry.py`（移除重复 LLM 重试职责，只保留非 LLM 用途）

新增：

- `agent/agent/tests/test_llm_retry.py`

实现：

- max_attempts 包含首次调用；
- 408、临时 409、429、5xx 和临时网络错误；
- Retry-After、retry-after-ms 与指数退避抖动；
- Provider 级冷却；
- 首个可见事件前可重试，之后不自动重试；
- 可注入 sleep、clock 和 random；
- 标准 ErrorEvent。

验证：

```powershell
python -m pytest agent/agent/tests/test_llm_retry.py -q
```

### 任务 5.2：实现 ModelRuntime

新增：

- `agent/agent/llm_client/runtime.py`

修改：

- `agent/agent/llm_client/__init__.py`

新增测试：

- `agent/agent/tests/test_model_runtime.py`

实现：

- 按完整模型键取得模型快照；
- 路由 Provider 与 Adapter；
- 调用 MessageTransformer；
- 统一重试包装；
- 请求取消传播；
- Runtime startup/shutdown；
- 暂时提供旧 Agent Loop 所需事件映射，但不保留旧配置兼容。

验证：

```powershell
python -m pytest agent/agent/tests/test_model_runtime.py -q
```

## 阶段 6：Agent Loop、上下文和工具安全集成

### 任务 6.1：Agent Loop 消费统一事件

修改：

- `agent/agent/agent_loop.py`
- `agent/agent/tests/test_agent_loop.py`
- `agent/agent/tests/conftest.py`
- 现有 JSON fixtures（仅按新事件契约调整）

实现：

- Runtime 替代全局 `config + model_override` Provider 选择；
- TextDelta、ThinkingDelta、ToolCallEnd、Usage、Done、Error 映射；
- tool calls 在 done 前完整收集；
- parse error 或截断调用禁止执行；
- 工具 Schema 校验后才执行；
- 截断恢复计数器移入单次运行；
- 不支持并行工具时只接受一个；
- 保持查询并发与操作串行政策。

验证：

```powershell
python -m pytest agent/agent/tests/test_agent_loop.py agent/agent/tests/test_reliability_policy.py -q
```

### 任务 6.2：上下文窗口使用当前模型

修改：

- `agent/agent/context.py`
- `agent/agent/tests/test_context_changes.py`

实现：

- ContextManager.compress 接收当前 ModelConfig 或 context window；
- 删除 Provider 猜测和硬编码模型名 fallback；
- context length 错误每次运行只压缩恢复一次；
- 修复测试中硬编码外部工程路径。

验证：

```powershell
python -m pytest agent/agent/tests/test_context_changes.py -q
```

### 任务 6.3：会话保存统一消息元数据

修改：

- `agent/agent/session.py`
- `agent/agent/tests/test_session_jsonl.py`

实现：

- session model key 使用 `provider/model`；
- assistant 消息可保存 thinking、Provider 元数据、完整工具调用状态；
- 工具结果保留 name 与 is_error；
- aborted/error assistant turn 可标记；
- JSONL round trip 不丢失统一内容块。

验证：

```powershell
python -m pytest agent/agent/tests/test_session_jsonl.py -q
```

## 阶段 7：应用生命周期与配置 API

### 任务 7.1：接入 Runtime 生命周期

修改：

- `agent/agent/app.py`
- `agent/agent/tests/test_server_contract.py`

实现：

- FastAPI lifespan 初始化 ModelCatalog 与 ModelRuntime；
- 关闭时释放所有 Provider HTTP 客户端；
- `/health` 报告配置与模型目录状态；
- 删除后台包装器重复 done；
- 取消传递到运行时；
- 后台会话保存完整模型键。

验证：

```powershell
python -m pytest agent/agent/tests/test_server_contract.py -q
```

### 任务 7.2：升级配置、连接测试和模型读取 API

修改：

- `agent/agent/app.py`
- `agent/agent/tests/test_webui.py`

接口：

- `GET /config`：返回 revision 与脱敏配置；
- `POST /config`：校验 revision，构建候选 Runtime 并原子保存和切换；
- `GET /config/models`：返回完整模型键、显示名、Provider、API、能力与状态；
- `POST /config/providers/test`：使用未保存供应商配置测试 API/Key，不推理、不读取模型；
- `POST /config/providers/models`：使用未保存供应商配置读取候选模型，不自动导入；
- 保留 `POST /config/models/refresh` 供运行时目录刷新使用，但不代替设置弹窗的独立读取操作。

测试：

- revision 冲突返回 409；
- 脱敏 Key 保留和明确清除；
- 测试连接与读取模型互相独立；
- 两接口均按 Provider 构造认证且不泄露 Key；
- 读取结果排序、去重；
- 配置失败不切换当前 Runtime。

验证：

```powershell
python -m pytest agent/agent/tests/test_webui.py agent/agent/tests/test_model_config.py -q
```

## 阶段 8：WebUI 模型设置中心和错误展示

### 任务 8.1：实现分组模型 Listbox 与设置入口

修改：

- `webui/app.js`
- `webui/index.html`
- `webui/style.css`
- `agent/agent/tests/test_webui.py`

实现：

- 设置按钮位于模型选择行最右侧；
- 原生 select 替换为可访问的 combobox/listbox；
- 模型按供应商分组，分组始终展开且标题不可选择；
- 值使用 `provider/model`，展示模型名称和 ID；
- 支持键盘导航、字符搜索、外部点击和 Escape 关闭；
- 保存后保持有效当前模型，否则回退默认模型；
- 切换 Provider 时继续当前会话；
- 不破坏工具调用组折叠。

### 任务 8.2：实现设置弹窗与供应商编辑

修改：

- `webui/index.html`
- `webui/app.js`
- `webui/style.css`
- `agent/agent/tests/test_webui.py`

实现：

- 模型、技能、MCP 工具三个 Tab；后两项首版显示空状态；
- 左侧供应商导航，右侧供应商表单；
- OpenAI、Anthropic、OpenAI-compatible、Ollama 预设；
- API 地址、Key、环境变量、默认协议和启用状态；
- 供应商含模型或被默认模型引用时禁止删除；
- 供应商含模型时禁止直接修改类型；
- draft/original/revision/dirty 状态；
- 未保存关闭确认和模态焦点管理。

### 任务 8.3：实现连接测试、读取模型和模型管理

修改：

- `webui/app.js`
- `webui/index.html`
- `webui/style.css`
- `agent/agent/tests/test_webui.py`

实现：

- “测试连接”只检查当前未保存 API/Key；
- “读取模型”独立获取候选列表；
- 候选按供应商暂存，不写入配置；
- 添加模型下拉框可搜索，已添加项显示勾选；
- 支持手动输入模型 ID；
- 模型默认继承供应商协议；
- 高级设置可覆盖协议、token 限制和 capabilities；
- 同供应商防止重复 ID；
- 保存失败保留草稿，关闭时取消网络请求；
- 网络结果不能串写到其他供应商。

### 任务 8.4：结构化错误展示

修改：

- `webui/app.js`
- `webui/style.css`

实现：

- rate limit 显示请求频率超限和 retry after；
- authentication、context length、connection、protocol error 分别显示；
- retryable 错误提供明确重试入口；
- 不显示 Provider 原始敏感响应。

验证：

```powershell
python -m pytest agent/agent/tests/test_webui.py -q
```

## 阶段 9：集成测试、清理和最终验收

### 任务 9.1：完整链路集成测试

新增：

- `agent/agent/tests/test_multi_provider_integration.py`

覆盖：

- OpenAI Chat、Responses、Anthropic、OpenAI-compatible 路由；
- 自动发现 + 手动覆盖；
- 跨 Provider 历史；
- 工具调用、结果和第二轮响应；
- 429 重试；
- 取消；
- session reload；
- API Key 脱敏。

### 任务 9.2：删除旧实现和重复代码

在新链路与测试全部通过后删除：

- `agent/agent/llm_client/openai_provider.py`
- `agent/agent/llm_client/anthropic_provider.py`
- `agent/agent/llm_client/base.py` 中旧 BaseProvider 架构
- 旧 registry fallback 与旧 StreamDelta
- `agent/agent/llm_client.py.bak`
- 旧模型窗口硬编码和重复重试配置

同时清理无用 import，不删除与本次任务无关代码。

### 任务 9.3：全量验证

执行：

```powershell
python -m pytest agent/agent/tests -q
python -m compileall agent/agent
```

验收核对：

- 四类协议路由正确；
- 多工具增量不丢；
- 非法参数不执行；
- done/error 唯一且最后；
- 429/5xx 重试符合策略；
- 跨 Provider 历史安全；
- 自动发现失败不阻断启动；
- API Key 全链路脱敏；
- HTTP 客户端正常释放；
- WebUI 工具调用折叠保持正常。

## 推荐执行批次

为控制风险，按以下批次实施并在每批结束运行相关测试：

1. 类型、配置、Provider 注册表；
2. ModelCatalog 和 MessageTransformer；
3. 流状态机及三个 Adapter；
4. Retry 与 ModelRuntime；
5. Agent Loop、Context、Session；
6. App API 和 WebUI；
7. 集成测试与旧代码清理。

任何批次失败时先修复该批，不跨批堆积未验证改动。

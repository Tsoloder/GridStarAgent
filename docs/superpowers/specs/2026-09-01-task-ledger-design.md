# 会话任务台账（Task Ledger）设计

日期：2026-09-01
状态：已与用户对齐全部关键决策，待实现

## 1. 背景与问题

智能体核心需求：用户提问后，根据 skill 使用工业软件 MCP 工具自动规划任务并执行。
工业软件工具必有返回值（无超时设定），无返回值即代表流程无继续必要。

长会话中的核心缺口：`context.compress()` 的 Layer 1/2 会裁剪旧消息，导致：

1. 模型丢失"干过什么"——工具调用记录只存在于消息历史（`tool_calls` + `tool` 消息）
2. 模型丢失"计划到哪了"——`phase_plan` 状态只存在于某条 assistant 消息的文本里，
   后端仅透传（`phase_plan` SSE 事件），从不解析、不持久化

压缩后模型无法回答"调用了什么工具、距离规划还差什么"，长任务执行会失忆漂移。

## 2. 关键决策（已与用户确认）

| 决策点 | 结论 |
|---|---|
| 计划进度由谁维护 | 专用计划工具（`update_plan`），模型显式调用更新，不依赖文本解析 |
| 与旧 `phase_plan` 文本协议的关系 | 工具完全取代文本协议；前端改为接收全新结构化 `plan_updated` 事件；删除后端全部文本拦截/提醒逻辑 |
| 工具调用账本粒度 | 后端自动结构化摘要打底（模型零负担）+ 计划步骤允许模型附 `note` |
| 参数/结果中的对象 ID | **全量记录**（工业场景需精确追溯操作了哪些对象），仅单参数超 500 项时安全截断 |
| 计划步骤粒度 | 阶段级（复用现有 `phases` 结构，5~8 个阶段）；阶段内细粒度由自动账本覆盖 |

## 3. 数据结构

台账独立于消息历史，落盘于 `sessions/<session_id>/ledger.json`：

```json
{
  "plan": {
    "id": "f6-mesh",
    "title": "f6.igs 网格生成",
    "phases": [
      {"id": "cad_import", "title": "CAD 模型导入", "status": "done",
       "note": "导入 f6.igs，共 12 个面"},
      {"id": "mesh", "title": "网格生成", "status": "in_progress", "note": ""}
    ]
  },
  "calls": [
    {
      "ts": "2026-09-01T14:23:05",
      "phase": "mesh",
      "tool": "create_mesh",
      "args_digest": {"input_ids": ["face_12", "face_15"], "size": 2.5},
      "ok": true,
      "result_digest": "创建成功，生成 84532 个单元...",
      "file_ref": null
    }
  ]
}
```

- `status` 枚举：`pending / in_progress / done / failed / skipped`
  （相比旧文本协议新增 `failed` / `skipped`，消除自由发挥字符串）
- 活动阶段 = 计划中第一个 `in_progress` 阶段，供账本自动关联
- 全量替换语义（同 TodoWrite）：`update_plan` 每次传完整计划，天然幂等

## 4. update_plan 计划工具

- 内置工具：本地注册进工具 schema，`_process_tool_call` 优先拦截，不走 MCP、不走审批
- 输入：`id`（首次创建必填）、`title`、`phases`（全量列表，每项 `id / title / status / note`）
- 后端一次调用完成三件事：
  1. 写入 `ledger.json`
  2. 发结构化 SSE 事件 `{"type": "plan_updated", "plan": {...}}`
  3. 返回简短确认文本（如"计划已更新：2/5 阶段完成"）
- 使用时机（写入 `cfd_workflow.md`）：
  - 确定整体规划后立即创建计划
  - 每个阶段开始/结束时更新状态并附 `note`（记录关键事实：对象数量、文件名等）
  - 被用户打断时把当前阶段标 `skipped` / `failed` 并注明原因

## 5. 自动执行账本

钩在 `agent_loop._process_tool_call`，每次工具调用（含 `workflow_runner`、
`update_plan` 自身、失败/异常）都记一条。

**参数摘要规则**：
- ID 类列表（字符串/数字数组）**全量记录**——必须能回答"对哪些对象做了操作"
- 单参数超 500 项：保留前 500 + 注明 `...+N 项已截断`
- 其余长文本参数（路径等）截到 100 字符；数值/布尔原样保留

**结果摘要规则**：
- `ok` 语义状态：复用 `_tool_result_failed` 规则（`status=error` /
  `result:"false"` → 失败），不依赖模型从返回文本自行判断
- 结果为 JSON：优先保留 ID 类字段（键名含 `id` / `ids`），再截取 `message` 等文本
- 纯文本结果：取头 200 字符
- 命中 `persist_large_result`（>30000 字符）：只记 `file_ref` 指针
  （`large_results/<call_id>.json`），与现有落盘机制打通
- 实现时需先对照真实 MCP 工具返回样例确定 ID 字段命名，避免拍脑袋

**持久化**：内存追加 + 节流落盘（会话锁释放或每 N 条刷一次），失败/异常必记。

## 6. 每轮注入（模型感知核心）

- 位置：请求组装时 `compress()` 之后、发往模型之前，追加一条 `user` 角色
  `<task_progress>` 消息
- **只存在于本轮请求，不写回 `session.messages`**——永不累积、天然免疫压缩
- 无条件每轮注入（开销 ≤800 token），无计划且空账本时不注入
- 注入格式：

```
<task_progress>
【当前计划】f6.igs 网格生成（2/5 完成）
  ✅ CAD 模型导入 — 导入 f6.igs，共 12 个面
  ▶ 网格生成（进行中）
  ⬜ 质量检查 / ⬜ 导出
【执行记录】最近 8 条：
  ✅ import_cad(path=.../f6.igs) → 导入成功，12 个面 [CAD 模型导入]
  ✅ create_mesh(size=2.5) → 84532 单元 [网格生成]
  ❌ fix_trailing_edges → status=error [网格生成]
【差距】未完成：网格生成(进行中)、质量检查、导出；上次失败：fix_trailing_edges
</task_progress>
```

- 预算折叠：账本超 ~800 token 时只保留最近 8 条明细，更早的折叠为一行统计
  （"另有 12 条调用：10 成功 / 2 失败"）；计划部分始终全量
- 磁盘记全量（可审计、可恢复），注入按预算（控上下文），两层职责分开

## 7. 协议清理与前端改造

### 前端（webui/app.js）

- 新增 SSE 分支：`plan_updated` → `renderPhase(event.plan)`
  （`extractPhase` 已支持结构化对象，渲染逻辑零改动）
- 会话恢复：`GET /session/{id}` 响应附带 `plan`，前端重载时恢复阶段面板
- `structuredBlocks` 中文本 `phase_plan` 块解析**保留**，仅用于渲染旧会话历史
  （向后兼容，不再是协议）

### 后端

| 文件 | 改动 |
|---|---|
| `agent/agent/task_ledger.py`（新增） | `TaskLedger` 类：计划更新、账本追加、落盘节流、活动阶段推导、摘要函数、`<task_progress>` 渲染与预算折叠 |
| `agent_loop.py` | 注册 `update_plan` 内置工具并优先拦截；每次工具调用后自动记账；`compress()` 后注入 `<task_progress>`；删除 `phase_plan` 文本事件发射；`_auto_plan_waits_for_choice` 重构为"本轮是否调过 update_plan + 文本是否含编号选项"；`_inject_replay_reminder` 提醒语改为 update_plan |
| `app.py` | 会话初始化加载台账传入 `agent_loop`；`GET /session/{id}` 附带 `plan` |
| `agent/prompt/cfd_workflow.md` | 5.1 节整段重写（文本协议 → update_plan 工具用法：时机、状态枚举、note 要求）；2.3.1 / 3.5 / 4.7 中 phase_plan 字样同步替换；`workflow_runner` 结果提醒文本改为"用 update_plan 更新阶段" |
| `res/skills/cfd-meshing-workflow/` | SKILL.md 与 references 中"输出 phase_plan"改为"调用 update_plan" |
| `tests/` | 新增 `test_task_ledger.py`（ID 全量/500+ 截断、`ok` 判定、注入折叠、持久化）；更新 `test_agent_loop.py`、`test_webui.py`、`fixtures/auto_mode_flow.json` 中 phase_plan 用例 |

### 不动的

- `package-*` 目录（打包快照，发布时重新打包）
- `session.py` 消息格式（台账独立文件，不混入消息历史）
- `mcp_bridge.py`（`update_plan` 在其之前被拦截）
- `_pending_reminders` 机制（与台账注入并行工作）

## 8. 实现衔接点（风险项）

1. `_auto_plan_waits_for_choice` 承担"①②流程等用户选项"职责，重构后语义必须等价，
   否则选择交互会断
2. 注入消息不得写回 `session.messages`，防止无限累积
3. `update_plan` 拦截必须在审批检查之前，且结果必须作为 `tool` 消息写入历史
   （OpenAI 协议要求 tool_calls 与 tool 响应配对）
4. 断线重连（`/chat/stream` 有后台任务时）回放路径需同样能收到 `plan_updated`
5. ID 字段提取规则需对照真实 MCP 工具返回样例确定

## 9. 范围外（本次不做）

- MCP 连接持久化 / 超时 / isError 读取（用户明确：工具必有返回值，无超时设定）
- 压缩机制改造（摘要写回、阈值修正等前期分析出的其他缺陷）
- 语义记忆 / 检索增强

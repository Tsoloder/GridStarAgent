# 设置中心“记忆”Tab 设计

日期：2026-09-28
状态：方案设计，待评审后进入实现
关联文档：[策略经验库（Experience Memory）设计](2026-09-28-experience-memory-design.md)

## 1. 背景与目标

缺口 5 的策略经验库方案已经把“怎么提炼、怎么存、怎么注入”定下来了，但一直没有定
“人来怎么管”。自动提炼如果只能靠 CLI 审核，实际落地时审核频率会降到零，候选区会变成
没人看的垃圾场；工具参数记忆目前也只有写入口，没有查看和清理入口，用户既不知道自己
被记住了什么，也无法单独撤销某一条。

本方案在设置中心新增第五个 Tab“记忆”，作为两类长期记忆的统一管理界面：

| 记忆类型 | 现有实现 | 本 Tab 的职责 |
|---|---|---|
| 策略经验 | 待实现的 `experience.db`（见关联文档） | 审核候选、查看已启用经验、停用、恢复、删除、批量操作 |
| 工具参数记忆 | 已实现的 `tool_memory.py` + `tool_parameter_memory.json` | 按工具和字段查看、删除单字段、清除单工具、清空全部 |

目标：

1. 用户能在设置里看清“系统记住了什么”，不需要翻数据目录。
2. 自动提炼的候选必须能在这里完成审核闭环，审核入口不以 CLI 为主。
3. 误记、过时、污染的记忆能在两三次点击内止损。
4. 交互参考 openhanako 的记忆页，但保留 GridStar 原生 HTML/CSS/JS 的实现方式。

非目标：

1. 不做 openhanako 的用户画像、pin、事实记忆；缺口 5 只解决“任务怎么做”。
2. 不在这里展示原始会话记录。`messages.jsonl`、`trajectory.jsonl`、`ledger.json`、
   当前会话台账都不属于“记忆”，继续在轨迹视图里查看。
3. 不引入 React、前端构建工具或新的 UI 框架。
4. 不在首版做 embedding、向量库、项目级 scope 和 Skill 自动生成。

## 2. openhanako 对照与取舍

### 2.1 可借鉴的交互

| openhanako 做法 | 对本 Tab 的启示 |
|---|---|
| 记忆健康条常驻顶部，区分正常 / 降级 / 失败 / 不可用 | 顶部放状态条，FTS 不可用、上次维护失败、数据库异常都要可见 |
| 记忆总开关带禁用原因（缺 Utility 模型时置灰并给提示） | 提炼依赖模型时开关置灰并说明原因，不能只给一个点不动的开关 |
| 分“当前编译记忆”“全部记忆”“修订历史”三层 | 分“策略经验（候选 / 已启用）”“工具参数”“维护与状态”三块 |
| 整理任务有自动开关、手动运行、运行中轮询、上次结果摘要 | 提供“立即提炼 / 维护”按钮和上次运行结果，运行中轮询进度 |
| 修订历史可查看和恢复 | 已停用经验可恢复；首版不做逐版本回滚，只做状态恢复 |
| 清空操作走独立确认浮层 | 删除、清空全部、批量审核、停用一律走站内确认浮层 |
| 列表、详情、编辑区各自滚动 | 设置弹窗高度固定，记忆面板内部各区块独立滚动，长文本不撑破布局 |

### 2.2 不能照搬的部分

| openhanako 做法 | 本项目的处理 |
|---|---|
| `agentId` 维度的多 Agent 记忆 API | GridStar 是单 Agent 本地部署，接口不带 agent 维度 |
| Utility 模型作为记忆前置条件 | 提炼复用当前默认模型即可，没有模型时才置灰；参数记忆不依赖模型 |
| pin（用户偏好）与事实记忆分开管理 | 首版不做，避免和缺口 5 的经验库混成一套 |
| Dream 的完整修订快照与回滚 | 首版只做状态流转和删除；修订历史等经验量上来再评估 |
| React 组件 + 全局事件（`window.dispatchEvent`） | 改成直接函数调用 + DOM 事件委托，复用现有 `showDialog` 与 `request` |

## 3. 记忆分类与边界

“记忆”Tab 管两类数据，界面文案上也要明确区分：

| 分类 | 内容 | 产生方式 | 可否注入请求 | 本 Tab 操作 |
|---|---|---|---|---|
| 策略经验 | 触发条件、前置条件、步骤、工具顺序、失败模式 | 任务台账自动提炼（人工录入走 CLI / 后续迭代） | 仅 `approved` 可注入 | 审核、停用、恢复、删除、批量操作 |
| 工具参数记忆 | 用户确认过的工具参数默认值（已排除路径、ID、密钥等） | 用户在工具审批中明确批准后写入 | 是，作为默认建议 | 查看、删除单字段、清除单工具、清空全部 |

边界规则：

1. 经验库不保存具体参数默认值，参数默认值的唯一入口仍是 `tool_memory`。
2. 参数记忆不显示原始调用参数，只显示当前持久化的键名和值；值本身在写入时已过滤
   敏感字段。
3. 本 Tab 不提供“手动新增经验”的首版入口；人工录入继续走审核 CLI 或后续迭代，
   避免在没有编辑器设计的情况下把任意文本写进可注入库。

## 4. Tab 信息架构

### 4.1 入口

在现有 `webui/index.html` 的设置分类里追加：

```html
<button id="tab-memory" type="button" role="tab" aria-selected="false"
        aria-controls="panel-memory" data-settings-tab="memory">记忆</button>
```

位置放在“MCP 工具”和“用量”之间，即顺序为：模型 → 技能 → MCP 工具 → 记忆 → 用量。
前三个 Tab 的既有顺序不变。

现有截图/契约里没有对 Tab 数量的硬编码，但 `test_webui.py` 的
`test_webui_contains_model_settings_center_contract` 需要同步断言新 Tab 与新接口。

### 4.2 面板分区

面板自上而下四块，整块可滚动：

```text
┌─ 状态条：记忆已启用 · 经验库正常 · FTS 可用 · 上次维护 09-28 15:42 ────────┐
│                                                                          │
│ 一、记忆开关                                                             │
│   总开关 / 自动提炼 / 经验注入            （立即保存，不依赖底部按钮）    │
│                                                                          │
│ 二、策略经验                                                             │
│   工具栏：状态筛选 · scope 筛选 · 任务类型筛选 · 搜索 · 刷新             │
│   批量栏：全选 · 本页选中 N 条 · 批量通过 / 批量拒绝                     │
│   列表：候选 / 已启用 / 已拒绝 / 已停用，卡片式，可展开详情              │
│                                                                          │
│ 三、工具参数记忆                                                         │
│   工具列表（details 折叠）：字段名 = 值 + 删除；工具级“清除”             │
│                                                                          │
│ 四、维护                                                                │
│   立即提炼 / 运行维护 · 运行中进度 · 上次结果摘要 · 危险区：清空全部     │
└──────────────────────────────────────────────────────────────────────────┘
```

### 4.3 与底部“保存设置”的关系

这是本方案里最容易踩坑的交互点，必须显式定义：

1. “模型”Tab 继续使用底部“保存设置 + 取消”草稿模型，行为不变。
2. 切换到“记忆”Tab 时隐藏底部保存按钮（现有 `switchSettingsTab()` 已按
   `tab !== "models"` 隐藏，沿用即可），但底部状态行仍保留，用于显示记忆操作结果。
3. 记忆 Tab 的所有操作**立即保存**：审核、停用、删除、开关变更都是一次独立请求，
   不需要用户再点保存。
4. 模型 Tab 的未保存草稿在切到记忆 Tab 时保留在 `state.settings.draft` 中，
   切回模型 Tab 仍可继续编辑和保存。
5. 关闭设置时只对模型草稿弹“放弃未保存的设置”；记忆 Tab 的开关和审核结果已经
   落盘，不参与这个确认。
6. “记忆”面板不写入 `state.settings.draft`，避免和模型草稿的脏标记互相污染。

## 5. 交互设计

### 5.1 状态条

数据来自 `GET /memory/summary`，只读展示，不在此处做操作：

- 状态灯：`healthy` / `degraded` / `unhealthy` / `disabled` / `unavailable`。
- 摘要文案：记忆开关状态、经验库路径是否可写、FTS5 是否可用、候选待审数量。
- 上次维护：时间、结果（成功/部分失败/失败）、耗时；失败时显示脱敏后的错误类别，
  不显示原始异常文本里的路径。
- 状态条本身是 `aria-live="polite"` 区域，维护完成或开关变化后更新。

### 5.2 记忆开关

| 开关 | 字段 | 默认 | 说明 |
|---|---|---|---|
| 启用记忆 | `memory.enabled` | 开 | 关闭后停止提炼、注入和参数记忆写入，已有数据保留 |
| 自动提炼经验 | `memory.experience_extract` | 开 | 关闭后只允许人工录入，不再从台账生成候选 |
| 注入经验提示 | `memory.experience_inject` | 开 | 关闭后继续采集和审核，但不再注入模型请求 |

交互规则：

- 三个开关即时生效，切换时按钮进入禁用态，成功后更新状态条，失败则回滚开关并显示错误。
- “启用记忆”关闭时，后两个开关置灰但保留各自取值；重新打开后恢复原取值。
- 提炼依赖模型配置：没有可用默认模型时“自动提炼经验”置灰，并显示
  “需要先配置默认模型”的行内说明。
- 开关文案下各有一行说明，不使用长段落解释。

### 5.3 策略经验列表

筛选条件：

- 状态：候选 / 已启用 / 已拒绝 / 已停用 / 全部，默认“候选”。
- scope：全部 / global / 具体 Skill（值来自数据）。
- 任务类型：全部 + 数据中出现的 `task_type`。
- 搜索：对 `title`、`trigger_cues`、`strategy_steps` 做 FTS 检索，中文 2/3-gram
  由后端处理，前端只传关键词。
- 排序固定为“候选按创建时间倒序，其余按更新时间倒序”，首版不提供排序选择器。

卡片内容：

```text
[ ] 候选 · scope=skill:cfd-meshing-workflow · 任务类型=mesh_quality
    自由边导致网格生成失败时先定位再局部重做
    置信度 0.35 · 证据 2 · 2026-09-28 15:20
    [查看详情] [通过] [拒绝]
```

- 状态用色彩区分但必须带文字标签，不能只靠颜色。
- 候选卡片显示复选框，支持本页全选；已启用、已拒绝、已停用卡片首版不参与批量选择。
- 行动作按状态给：候选 `通过 / 拒绝`；已启用 `停用`；已拒绝 `重新开启`；
  已停用 `恢复`。删除入口只在详情里提供，避免列表误触。
- 每页 20 条，底部“加载更多”追加；筛选条件变化时重置到第一页。

### 5.4 经验详情

点击卡片打开站内浮层，样式复用 `.modal-backdrop` / `.confirm-dialog` 系列的
视觉语言，但使用独立的 `.memory-detail-dialog`：

- 头部固定：状态标签、标题、scope、任务类型、创建/更新时间。
- 内容区独立滚动，按字段分组展示：触发条件、前置条件、建议步骤、工具顺序、
  失败模式、来源与版本、置信度与反馈计数。
- 底部固定操作栏：主要动作（通过 / 停用 / 恢复）、次要动作（关闭）、
  危险动作（删除，二次确认）。
- 所有字段渲染前走 `escapeHtml()`；经验文本来自模型提炼，必须当不可信内容处理。
- 详情里的“来源与版本”只展示 `source_kind`、extractor 版本、Skill hash 前 12 位，
  不展示会话 ID（长期库中也不存在该字段）。

### 5.5 批量操作

- 选中集合以经验 id 为准，翻页后保留选择，但只作用于当前筛选下的可见条目。
- 首版批量动作只有“通过 / 拒绝”，先弹确认：`将对 N 条经验执行该操作，是否继续？`
- 停用是已启用经验的止损动作，只逐条在详情中执行，避免批量勾选时把正常经验一起停掉。
- 前端顺序调用 `POST /memory/experiences/bulk`（后端一次事务批量处理），成功后
  清空选择并刷新列表与状态条。
- 部分失败时保留失败条目的选择，状态行显示“成功 N 条，失败 M 条”，不自动重试，
  避免重复通过。
- 首版不提供批量删除。删除不可逆，只能逐条在详情中执行。

### 5.6 工具参数记忆

数据来自 `GET /memory/tool-parameters`：

```text
▸ create_mesh                          3 个字段 · 更新于 09-28 15:20   [清除]
    size        = 2.0      [删除]
    algorithm   = "delaunay" [删除]
▸ export_model                         1 个字段 · 更新于 09-27 09:10   [清除]
```

- 工具按名称排序，字段按名称排序，值是标量，直接展示。
- “清除”弹确认：`将删除 create_mesh 记住的全部参数，后续调用不再自动建议。`
- 字段级“删除”同样弹轻量确认，避免误触。
- “清空全部”放在维护区危险区域，确认文案必须写明不可撤销，并提示影响工具数量。
- 删除后立即重渲染列表；若某工具字段被删空，工具条目整体消失。
- 空状态：`还没有记住的工具参数。你在工具审批中确认参数后，这里会出现记录。`

### 5.7 维护与危险区

- “立即提炼”：投递一次手动提炼任务，只生成候选，不自动通过。
- “运行维护”：执行候选合并、30 天淘汰、版本失效标记，产出结果摘要
  （合并数、归档数、待复核数），不展示经验正文。
- 运行中按钮禁用并显示进度（步骤名 + 当前状态），前端每 2 秒轮询
  `GET /memory/status`；离开记忆 Tab 时停止轮询，回到 Tab 再重新拉取。
- 上次结果摘要常驻显示时间和数字；失败时显示错误类别和“重试”。
- 危险区首版只放“清空全部工具参数记忆”。清空经验是更重的操作（会同时影响候选与
  已启用经验），首版不提供，需要时按状态筛选后逐条处理。

## 6. API 契约

全部挂在现有 FastAPI 应用（`agent/agent/app.py`）下，单 Agent 命名，不带 agent 维度。
错误响应统一为 `{"error": "...", "code": "..."}`，沿用现有 `request()` 前端封装。

### 6.1 摘要与状态

`GET /memory/summary`

```json
{
  "enabled": true,
  "config": {"extract": true, "inject": true},
  "experience": {
    "candidate": 12, "approved": 8, "rejected": 3, "deprecated": 1,
    "possible_duplicate": 2
  },
  "tool_parameters": {"tools": 5, "fields": 11},
  "store": {
    "status": "healthy",
    "fts": true,
    "writable": true,
    "path_hint": "用户数据目录",
    "last_error": null
  },
  "maintenance": {
    "state": "idle",
    "step": null,
    "started_at": null,
    "finished_at": "2026-09-28T15:42:11+08:00",
    "last_result": {"merged": 1, "archived": 0, "needs_review": 2},
    "last_error": null
  }
}
```

- `path_hint` 只给“用户数据目录”这类可读提示，不返回绝对路径。
- `last_error` 为 `null` 或 `{"code": "store_unavailable", "message": "经验库暂时不可读"}`
  形式；`message` 由后端白名单化生成，不直接透传异常文本，避免路径泄漏。
- 首版 summary 每次调用实时统计；经验量超过 5000 条后再引入缓存表。

`PATCH /memory/config`

请求体只允许以下键，至少一个：

```json
{"enabled": true, "extract": false, "inject": true}
```

- 立即持久化到 `config.json` 的 `memory` 节，与模型设置共用版本化落盘。
- 请求体的短名与配置键一一对应：`enabled` → `memory.enabled`、
  `extract` → `memory.experience_extract`、`inject` → `memory.experience_inject`。
- 返回更新后的 summary，便于前端一次性刷新。
- 幂等：重复设置同一值返回 200 且不改变 `updated_at`。

### 6.2 策略经验

`GET /memory/experiences?status=candidate&scope=&task_type=&q=&page=1&page_size=20`

```json
{
  "items": [
    {
      "id": "exp_01J...",
      "status": "candidate",
      "scope": "skill:cfd-meshing-workflow",
      "task_type": "mesh_quality_improvement",
      "title": "...",
      "confidence": 0.35,
      "evidence_count": 2,
      "possible_duplicate": false,
      "created_at": "...",
      "updated_at": "..."
    }
  ],
  "total": 42,
  "page": 1,
  "page_size": 20,
  "facets": {"scopes": ["global", "skill:cfd-meshing-workflow"], "task_types": ["..."]}
}
```

`GET /memory/experiences/{id}` 返回完整记录，字段见经验库设计的 Experience record，
另外追加 `exposure_count`、`helpful_count`、`harmful_count`、`verified_success_count`、
`verified_failure_count`、`possible_duplicate`。

`PATCH /memory/experiences/{id}`

```json
{"action": "approve", "reason": "已复现"}
```

- `action` 枚举：`approve` / `reject` / `deprecate` / `restore`。
- 非法状态转换返回 `409`，例如对 `rejected` 直接 `approve`。
- `reason` 可选，最多 200 字，只写入审计表，不进入经验正文。
- 同一记录的并发操作通过 `updated_at` 前置条件判断，过期写入返回 `409`。

`DELETE /memory/experiences/{id}`

- 删除记录、FTS 索引和审计引用；返回 204。
- 已启用经验删除前必须由前端二次确认；后端不提供“批量删除全部已启用经验”。

`POST /memory/experiences/bulk`

```json
{"ids": ["exp_...", "exp_..."], "action": "reject", "reason": "证据不足"}
```

- 单次最多 100 条，超出返回 `400`。
- 批量 `action` 只允许 `approve` / `reject`，与界面的批量按钮保持一致。
- 在一个事务里逐条执行；返回每条的 id 与结果，允许部分成功。整体请求在有部分失败时
  仍返回 200，由 `results` 逐条区分成败；只有请求本身非法时才返回 4xx。
- 幂等：已处于目标状态的记录计为成功，不重复写审计。

### 6.3 工具参数记忆

`GET /memory/tool-parameters`

```json
{
  "tools": [
    {"tool": "create_mesh", "updated_at": "...", "fields": {"size": 2.0, "algorithm": "delaunay"}}
  ],
  "tool_count": 5,
  "field_count": 11
}
```

- 工具名和字段名来自持久化文件，渲染时必须转义。
- 值只返回持久化后的标量；不返回历史调用参数。

`DELETE /memory/tool-parameters`

清空全部工具参数记忆，返回 200 与删除的工具数、字段数；前端使用危险确认浮层。

`DELETE /memory/tool-parameters/{tool}`

清除单个工具，成功返回 204；工具不存在时返回 404，便于前端提示状态已变化。

`DELETE /memory/tool-parameters/{tool}/{field}`

删除单个字段，成功返回 204；字段不存在时返回 404。工具名和字段名按 URL path
参数传递，前端使用 `encodeURIComponent()`。

### 6.4 维护

`GET /memory/status`

```json
{
  "maintenance": {"state": "running", "step": "extract", "started_at": "...", "finished_at": null,
                  "last_result": null, "last_error": null},
  "extraction": {"pending_jobs": 0, "last_job_at": null, "last_job_result": "success"}
}
```

`POST /memory/maintenance/runs`

```json
{"kind": "extract"}
```

- `kind` 枚举 `extract` / `governance`；默认 `governance`。
- 已有任务运行时返回 `409` 和当前状态，不排队堆积。
- 立即返回 202 + 当前状态，前端轮询 `/memory/status`，不占用长连接。

`tool_memory.py` 需要新增 `catalog_all()`、`delete_fields()`、给
`clear(tool)` 补一个返回被删数量的签名（保持向后兼容），并新增 `memory_store.py`
作为经验库与参数记忆的统一门面，供 API 层调用。

## 7. 状态、错误与并发

| 场景 | 处理 |
|---|---|
| 首次进入 Tab | 懒加载 summary，再按默认筛选拉候选；加载中显示骨架或“正在读取记忆…” |
| 请求失败 | 状态条转为 `unavailable`，面板显示错误与“重试”，不清空已有列表 |
| 单条操作失败 | 行内提示错误，列表不整体重载；成功后局部替换该卡片 |
| 409 状态冲突 | 提示“该条状态已变化，已刷新”，自动重新拉取当前页 |
| 维护运行中重复点击 | 按钮禁用；后端 409 兜底 |
| 数据库损坏 | `/memory/summary` 返回 `degraded` + 白名单化 `last_error`，页面仍可展示参数记忆与开关 |

错误码约定：

| HTTP | code | 含义 |
|---|---|---|
| 400 | `invalid_request` | 参数、action 或批量条数非法 |
| 404 | `not_found` | 经验、工具或字段不存在 |
| 409 | `state_conflict` | 非法状态转换、并发写入、维护任务已在运行 |
| 503 | `store_unavailable` | 经验库不可用；返回降级信息 |

## 8. 安全、隐私与审计

1. 本 Tab 只展示脱敏后的经验与参数记忆，不提供查看原始台账的入口。
2. 经验文本按不可信内容渲染，统一 `escapeHtml()`；禁止 `innerHTML` 直接拼接未转义文本。
3. 参数记忆只展示 `tool_memory` 已过滤的标量字段；页面不提供“导出全部原始参数”。
4. 删除、停用、审核写入 `experience_audit` 与参数记忆审计记录，只记录动作、
   目标 id/工具名/字段名和时间，不记录被删文本内容。
5. API 仅服务于本地 WebUI；若将来放开远程访问，这些接口必须加入与现有会话相同的
   访问控制，不能默认裸露。
6. 清空类操作全部走 `showDialog()`，禁止 `window.confirm`；`test_webui.py` 已有静态
   断言会持续守住这一点。

## 9. 响应式与无障碍

- 桌面端沿用 `.settings-dialog` 的 920×720 上限；记忆面板内部滚动，不撑高弹窗。
- `max-width: 640px` 时筛选栏换行、卡片单列、详情浮层全屏显示。
- Tab 列表在窄屏允许横向滚动，五个 Tab 文字不能被压缩换行。
- Tab 遵循 `role="tablist"` / `aria-selected` / `aria-controls`；方向键在 Tab 间移动，
  Enter/Space 激活。
- 详情浮层打开时焦点移入标题，Tab 键在浮层内循环，Esc 关闭并回到触发卡片。
- 状态条、操作结果用 `role="status"` + `aria-live="polite"`，错误用 `role="alert"`。
- 开关使用原生 `checkbox` 或带 `aria-checked` 的按钮，标签与说明用 `aria-describedby` 关联。

## 10. 分阶段实施

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0-1 只读面板 | summary + 经验列表 / 详情 + 参数记忆列表 + Tab 骨架 | 能看清当前记忆，无写操作 |
| P0-2 审核闭环 | 单条 approve/reject/deprecate/restore + 开关 PATCH + 审计 | 候选可审核，开关即时生效 |
| P0-3 清理能力 | 参数记忆字段/工具/全量删除 + 经验删除 + 确认浮层 | 误记数据可止损 |
| P1-1 批量与维护 | 批量审核 + extract/governance 维护任务 + 运行轮询 | 候选积压可批量处理 |
| P1-2 健康与错误处理 | 健康条降级、并发冲突提示、部分失败展示 | 库损坏时主流程不受影响 |
| P2 延期项 | 修订历史回滚、人工新增经验、项目级 scope、Skill 草稿 | 数据量或需求证明后再做 |

P0-1 到 P0-3 依赖经验库的存储与状态接口先用最小实现落地；参数记忆部分不依赖经验库，
可以先行合入，让“工具参数管理”比经验审核更早可用。

## 11. 测试与验收

### 11.1 静态契约（扩充 `test_webui.py`）

- 存在 `id="tab-memory"`、`data-settings-tab="memory"`、`id="panel-memory"`。
- 存在 `switchSettingsTab("memory")` 的懒加载分支。
- `app.js` 出现 `/memory/summary`、`/memory/experiences`、`/memory/tool-parameters`、
  `/memory/config`、`/memory/maintenance/runs` 等路径。
- 记忆操作不写入 `state.settings.draft`，并继续用 `request()` 发请求。
- 不使用 `confirm(` / `prompt(` / `alert(`。
- 新增 CSS 类：`.memory-panel`、`.memory-status`、`.memory-switches`、`.memory-toolbar`、
  `.memory-bulk`、`.memory-list`、`.memory-card`、`.memory-detail-dialog`、`.memory-danger`，
  且包含 `max-width:640px` 下的响应式规则。

### 11.2 行为测试

- 开关切换立即调用 PATCH，失败时开关回滚并显示错误。
- 审核一条候选后，候选计数减一、已启用计数加一，页面不整体刷新。
- 批量操作部分失败时保留失败条目选择并显示数量。
- 删除最后一个参数字段后工具条目消失；清空全部走确认浮层。
- 关闭设置时模型草稿仍触发“放弃未保存”确认，纯记忆操作不触发。
- 409 冲突时页面自动刷新当前页并给出提示。
- 维护任务运行中每 2 秒轮询，离开 Tab 后停止，回来后恢复状态。

### 11.3 无障碍与兼容

- 键盘可完成 Tab 切换、筛选、查看详情、审核和删除确认。
- 640px 与 360px 宽度下无文本溢出、无按钮压缩、无内容互相遮挡。
- 详情长文本滚动正常，弹窗高度不随内容增长。

## 12. 风险与待确认

| 风险 | 影响 | 缓解 |
|---|---|---|
| 经验正文仍可能包含未识别的敏感信息 | UI 会直接展示 | 入库双重脱敏不变；提供单条删除与单条停用快速止损 |
| 候选数量增长后列表变慢 | 审核体验下降 | 分页 + 状态默认候选；超过 5000 条时给 summary 加缓存 |
| 立即保存与模型草稿的语义混淆 | 用户以为关掉弹窗会丢审核结果 | 面板内提示“记忆操作即时生效”；开关旁标注保存状态 |
| 维护任务与主任务争用模型调用 | 任务变慢 | 提炼继续走异步单并发；UI 只投递任务，不等待 |
| 参数记忆被误删后无法恢复 | 用户需重新确认参数 | 删除前明确提示；参数在下一次审批中仍可重新记住 |

待确认：

1. 维护任务是否复用提炼 worker，还是单独线程；取决于经验库实现落点。
2. 首版是否需要“重新开启已拒绝经验”的入口；本文按需要保留，但默认藏在详情里。

已定：三个开关写入 `config.json` 的 `memory` 节（`memory.enabled` /
`memory.experience_extract` / `memory.experience_inject`），跟随现有配置的版本化
原子写，不另建文件。

# openhanako 记忆系统「注入与消费」侧分析

只读分析对象：`D:\TRAE_project\openhanako`（HanaAgent，Electron + TypeScript）。
目的：提炼可移植到 Python 项目的记忆注入/消费方案。所有行号为该仓库实测行号。

---

## 1. 注入位置与格式

**位置：system prompt 的最后一段**，紧贴「cache 分界线」之后、`Session started at:` 之前。
段序（core/agent.ts:1287-1553）：平台行 → `# 执行环境` → `# 用户档案` → AGENTS.md(personality) → `## 你的样子` → 行为指南若干（工具纪律/Session 交付/UI 上下文/subagent 协作/本机应用控制/行动纪律/网页优先级/主动技能/团队）→ **cache 分界线（core/agent.ts:1526）** → **记忆三段（:1531-1533）** → Session started at（:1536-1545）→ 时间快照说明 + 「一天从 04:00 开始」。

分界线注释原文（core/agent.ts:1273-1286）：「Prompt 拼接遵循『静态前缀在前、动态尾部在后』原则，最大化跨 session 的 prefix cache 命中率（KV cache / Anthropic prompt cache 都按严格前缀匹配）。顺序：平台 → 环境 → 用户档案 → AGENTS.md（依赖 userName）→ 样貌 → 行为指南 ── cache 分界线 ── 记忆规则/置顶/记忆 → 会话开始时间」。

记忆三段同一段内，顺序固定（tests/agent-system-prompt-section-order.test.ts:83-113 断言 index 单调递增）：
1. `## 记忆使用规则` / `## Memory Rules`（core/agent.ts:1343-1361）
2. `# 置顶记忆` / `# Pinned Memories`（core/agent.ts:1371-1376）
3. `# 记忆` / `# Memory`（core/agent.ts:1379-1384）

三条规则原文（zh，core/agent.ts:1343-1351）：
```
记忆和用户档案是你内化的背景知识。你和{userName}是认识很久的人，这些事你本来就知道。

- **只有当{userName}提到相关内容，记忆才参与**，而且方式是无声的：影响你的角度、语气、判断，不出现在文字里。{userName}没提起的话题，不要主动从记忆里翻出来讲。
- **永远不要让{userName}感觉到"记忆"这个东西的存在。** 禁止"我记得""你之前说过""根据记忆"这类表述，除非{userName}主动问"你还记得 xxx 吗"。
- **记忆可能过时，当前对话永远优先。** 信息冲突时以对话为准，不要用旧记忆纠正{userName}。
```
（en 版 core/agent.ts:1352-1361 同构）

section 渲染器（core/agent.ts:1271）：`const section = (title, content) => ["", "---", "", title, "", content];`
→ 每段产出 `\n---\n\n{title}\n\n{content}`，段间靠 `parts.join("\n")` 拼接。

**内容来源只有两个文件，全量读入**（core/agent.ts:1267-1268）：
```js
const pinnedMd = readFile(path.join(this.agentDir, "pinned.md"));   // safeReadFile(fp, "")
const memory   = readFile(this.memoryMdPath);                       // agentDir/memory/memory.md
```

**空值门**（core/agent.ts:1365-1387）：`hasMemory = trimmedMemory && trimmedMemory !== "（暂无记忆）" && trimmedMemory !== "(No memory yet)"`；`if (hasPinned || hasMemory) { memParts = [memoryRule, ...] }`。
→ 两个都空时，**连记忆规则段也不注入**（memoryBlock = null，整段消失）。

**token 预算：注入侧完全没有上限、没有任何截断。** 全文无 maxTokens / slice / truncate 作用于该块。护栏只存在于编译链：
- `WEEK_ASSEMBLY_MAX_CHARS = 1200`（lib/memory/compile.ts:66-69），超限从最老 daily 条目开始丢（lib/memory/compile.ts:572-589）
- 长期情况 prompt「必须控制在 400 字以内」（lib/memory/prompts/compile.ts:107）
- 重要事实 prompt「必须控制在 200 字以内」（lib/memory/prompts/compile.ts:160）

**per-session 快照冻结**（core/session-coordinator.ts:2002-2012）：新建会话时把当时构建的 systemPrompt 快照进 session，之后记忆编译、技能变更只影响新对话。注释：「快照当前 system prompt，per-session 隔离。后续记忆编译、技能变更只影响新对话，已有对话的 prompt 不变（保护 prefix cache）。」

> 移植建议：注入侧补上限（pinned 优先、longterm 次之），别照抄"无预算"这个弱点。

---

## 2. 检索式 vs 全量注入

| 类别 | 载体 | 注入方式 | 体量控制 |
|---|---|---|---|
| 置顶记忆 pinned | `agentDir/pinned.md` + `pinned-memory.json` | **无条件全量**注入 | 无上限；用户/pin_memory 手动增删 |
| 编译记忆 compiled | `agentDir/memory/memory.md`（facts/today/week/longterm 四段拼装） | **无条件全量**注入 | 仅编译侧字数约束（见上）；today 每日重置 |
| 事实库 facts | `memory/facts.db`（sqlite + FTS5） | **按需检索** `search_memory` | 单次调用 tag 命中 ≤15 条 + FTS ≤10 条 |
| 经验 experience | `agentDir/experience.md` 索引 + `experience/<slug>-<hash>.md` | **按需检索** `recall_experience`（渐进式披露：先给索引，再取单篇） | 索引每分类描述 ≤120 字（lib/tools/experience.ts:29-61） |

按需检索的两段式策略（lib/memory/memory-search.ts:92-106）：策略 1 标签匹配 `factStore.searchByTags(tags, dateRange, 15)`；仅当标签结果 < 3 条且有 query 时，策略 2 `factStore.searchFullText(query, 10)`，seenIds 去重。

经验是**两级披露**：`recall_experience()` 无参数只返回索引（每个分类一行：标题 + 前 20 字拼接、总长 ≤120 字 + 文件指针），带 `category` 才读整篇。这是可移植性最高的一个模式，值得优先抄。

---

## 3. context 压缩策略（三层，职责完全不同）

### 3.1 滚动摘要（记忆侧，per-session）
- 触发：每 10 轮 `TURNS_PER_SUMMARY = 10`（lib/memory/memory-ticker.ts:66），或分支替换 pending；`notifyTurn/session_end/branch_change` 三个入口（lib/memory/memory-ticker.ts:1036-1104、:1091-1104）
- 投影 + 增量：`_readSessionBranch(sessionPath, { since: resetAt })`（lib/memory/memory-ticker.ts:725）；本金是摘要覆盖游标 `cursor.coveredLeafId` + `messageCount`（lib/memory/session-summary.ts:531-547，注释「messageCount cannot establish branch ancestry」）
- 输出格式固定两节（lib/memory/rolling-summary-format.ts 契约，tests/memory-prompt-layout.test.ts 锁定 `templateVersion === "rolling-summary.v1"`）
- **预算**：`_rollingSummaryBudget(turnCount)` = `Math.min(400, Math.max(40, turnCount * 40))` 字，`visibleMaxTokens = Math.max(150, Math.min(750, round(totalBudget * 1.5)))`（lib/memory/session-summary.ts:709-715）；facts 占 30%、timeline 占 70%（:787-792）
- 写盘：`memory/summaries/<sessionId>.json`，字段 `{session_id, created_at, updated_at, summary, messageCount, cursor, source_time_range, snapshot, snapshotCursor, snapshot_at, factReplacementRequired}`（lib/memory/session-summary.ts:678-692）
- 落盘前过 PII 脱敏 + 格式校验，不合格走格式修复 LLM（`MAX_ROLLING_SUMMARY_FORMAT_REPAIRS`，lib/memory/session-summary.ts:672-676、:717-735）

### 3.2 每日传送带（记忆侧，agent 级）
每天一次（日期变化触发，另有 1h 备用 timer，lib/memory/memory-ticker.ts:1144-1150）：
`DAILY_STEP_KEYS = ["compileDaily", "compileToday", "rollDailyWindow", "compileFacts", "deepMemory"]`（lib/memory/memory-ticker.ts:78），每步完成写 `daily-state.json`（断点续跑：`_restoreDailyProgress` :414、`_markDailyStepCompleted` :458）。
顺序约束（lib/memory/memory-ticker.ts:870-871）：「compileDaily 必须先于 compileToday 执行：compileDaily 读取的是『昨天最终版 today.md 草稿』，一旦 compileToday 先跑，日期切换会把 today.md 重置成新的一天，昨天的内容就再也读不到了。」
week.md 零 LLM：`assembleWeekFromDaily` 纯文件装配最近 6 个逻辑日（`DAILY_WINDOW_RETENTION_DAYS = 6`，lib/memory/compile.ts:66）。

### 3.3 上下文压缩（LLM 上下文侧，与记忆流水线解耦）
`core/session-compactor.ts`（2019 行）：
- 硬截断阈值 `DEFAULT_HARD_TRUNCATE_THRESHOLD = 0.85`（:37），`threshold = floor(contextWindow * 0.85)`（:1418）
- 分级降级（`shouldHardTruncateCachePreservingCompaction` :1374-1430）：cache-preserving 预算装得下 → 用它；装不下但 native summary 装得下 → `shouldUseNativeFallback`；都装不下 → `shouldHardTruncate`，硬截断保留 `keepRecentTokens ?? 20_000`（:1436）
- 请求预留 `COMPACTION_REQUEST_BUFFER_TOKENS = 1024`（:50），压缩输出上限 `max(512, floor(reserveTokens * 0.8))`（:370-371）

**lossy-local-compaction 解决什么**：上下文溢出时**不花模型请求**就重建检查点。`createLossyLocalCompactionResult({branchEntries, preparation, summarySource})`（core/lossy-local-compaction.ts:113）：尾部原文由 Pi 的 `firstKeptEntryId` 精确保证（:129-136 校验，边界无法证明就 throw「Instant local compaction could not prove the retained-tail boundary」）；被压缩区丢弃 tool call **与** tool result 两侧、thinking、非文本块（`OmissionCounts`，:14-20），避免 provider 工具协议半开。滚动摘要复用判据：`useRollingSummary = !!sourceSummary && cursorIndex !== null && cursorIndex < keptIndex && summaryIsAfterReset`（:145-156），reset 之后的部分会单独渲染成 `## Earlier Transcript Before Memory Reset`。
文档注释（:105-112）：「Rebuild a compaction checkpoint without inference or network access. The recent tail remains represented by Pi's exact firstKeptEntryId.」

**fresh-compact-maintainer 解决什么**：每日批处理**调度**，不是压缩算法。`FreshCompactMaintainer.runDaily({now})`（hub/fresh-compact-maintainer.ts:34）：逐 agent 取 `bridgeSessionManager.listDailyFreshCompactTargets(agent, {now})` → 已满足则 `markFreshCompactSatisfied(sessionKey, {reason:"daily"})`（:50-61）→ 否则先 `await agent.memoryTicker.flushSessionAndCompile(target.sessionPath)` 再 `freshCompactSession(target.sessionKey, {reason:"daily"})`（:66-73）→ 每目标间隔 `delayBetweenJobsMs = 5_000`（构造默认值 :10）。

**摘要下次怎么复用**：全量摘要作为 `prevSummary` 喂给下一次滚动调用（lib/memory/session-summary.ts:922-928，`## 已有摘要` + `## 新增对话` + `## 本次摘要预算`），并同时供两条下游链路消费（文件头注释 lib/memory/session-summary.ts:10-12）：「普通记忆（compile.js 读摘要 → 递归压缩 → memory.md）」与「深度记忆（deep-memory.js 读 snapshot diff → 拆元事实）」。

---

## 4. tools 暴露

| 工具名 | 参数 | 生效条件 | 权限 |
|---|---|---|---|
| `search_memory` | `query`(必填) `tags?` `date_from?` `date_to?`；仅当 conversationScope 存在时多 `cross_channel?` | `master && session` 都开（core/agent.ts:922-925） | `BUILT_IN_PERMISSION_GATEWAY_TOOL_NAMES`，由 host classifier 把关 |
| `pin_memory` | `content`(必填) | 同上 | `kind:"routine"` + `pin_memory.pin`，免审（lib/tools/pinned-memory.ts:34-44） |
| `unpin_memory` | `id?` `keyword?` | 同上 | `kind:"review"`，**要审批**（:87-97） |
| `recall_experience` | `category?` | `config.yaml experience.enabled === true` | `kind:"read"` + `recall_experience.read` |
| `record_experience` | `category`(2-4 词) `content`(一句话) | 同上 | `kind:"routine"` + `record_experience.record` |

**条件注入**：`getToolsSnapshot`（core/agent.ts:908-965）里 `const memTools = memoryEnabled ? [this._memorySearchTool, ...this._pinnedMemoryTools] : []`（:922-925）；`experienceTools = experienceEnabled ? this._experienceTools : []`（:926）。工具不在快照里，模型既看不见也调不到（tests/agent-tools-conditional-injection.test.ts 就是锁这件事）。
两层开关：接口层 `getToolsSnapshot({forceMemoryEnabled, forceExperienceEnabled})`（session 冻结态覆盖用）+ 执行层 `createExperienceTools(agentDir, {isEnabled})` 每次 execute 再判一次（lib/tools/experience.ts:193、:267），返回 `t("error.expPaused")` + `details:{paused:true}`。

工具分类（shared/tool-categories.ts）：
- `search_memory` / `pin_memory` / `unpin_memory` ∈ `CORE_TOOL_NAMES`（:24-28）。CORE 的定义是「Removing breaks the model. Never user-toggleable, never in UI.」→ 记忆三件套**不可被用户关掉**，只能被 agent 级/会话级记忆开关整体摘除。
- `record_experience` / `recall_experience` ∈ `STANDARD_TOOL_NAMES`（:30-50），always-on 内置、不进 UI。
- 启动断言 `assertAllToolsCategorized`（:156-172）、`assertAllBuiltInToolsPermissionCovered`（:220-236）保证新工具必须归类，否则启动就炸。

**agent-experience-toggle 测试说明什么**（tests/agent-experience-toggle.test.ts）：experience 默认 disabled（:64-70）；关闭时快照里没有 record/recall（:72-81）；开启时有（:83-90）；`forceExperienceEnabled` 能覆盖（:128-139）。关键注释（:76-77）：「经验引导已收口进 recall/record 工具的 description，随工具进出…不再单独在 system prompt 注入」→ **能力引导跟着工具走**，别在 prompt 里另写一段（否则工具关了 prompt 就成了孤儿噪音）。这条同样适用于 `forSubagent`：core/agent.ts:1240-1242 注释「Subagent 是隔离子会话，不注入长期记忆和多 agent 协作上下文」，整块跳过。

---

## 5. 会话与记忆的绑定

**存储 per-agent**：`agentDir/memory/facts.db`（FactStore）、`agentDir/memory/summaries/<sessionId>.json`、`agentDir/memory/{memory.md,today.md,week.md,longterm.md,facts.md}`、`agentDir/pinned.md`（core/agent.ts:189-199 产物路径全部从 this.agentDir 派生）。多 agent 靠 agentDir 物理隔离，不靠字段过滤。

**检索作用域 per-session**：事实行带 `session_id`（可空），见 tests/memory-search-channel-scope.test.ts 夹具。
- 开关三层：master（`config.yaml memory.enabled`，core/agent.ts:344）→ session（session meta `memoryEnabled`，core/session-coordinator.ts:4567/4606-4618）→ 检索作用域（channel）。
- 冻结：per-session 快照（core/session-coordinator.ts:2002-2012），同 agent 不同会话各持自己的 prompt 快照。
- 会话开关会**掐断整条记忆流水线**（lib/memory/memory-ticker.ts:165-168 `_isSessionMemoryOn`：「session 关闭记忆时，整条记忆流水线都应跳过，避免被写入 summary/facts」，文件头注释 :17）。

**channel scope**（issue #1670「群聊记忆混淆」）：
`factVisibleInConversationScope(row, scope, crossChannel)`（lib/memory/memory-search.ts:24-30）四条规则：① scope.kind !== "channel" → 全可见；② `row.session_id` 不以 `CHANNEL_SESSION_PREFIX = "channel-"` 开头 → 通用事实，全可见；③ `session_id === "channel-" + scope.channelId` → 可见；④ 其余 → 仅 `cross_channel === true` 时可见。
频道会话运行时替换（hub/agent-executor.ts:491-499）：`createConversationScopedMemorySearchTool({kind:"channel", channelId: conversationId})`，且只做同名替换、不凭空注入（lib/conversations/agent-phone-session.ts:82-90 注释「记忆关闭时快照里本来就没有 search_memory，此时不得凭空注入 scoped 实例（保持记忆开关语义）」）。

**隔离边界**：subagent 不注入记忆（core/agent.ts:1240-1242 + `buildSystemPrompt({forSubagent:true})`，core/session-coordinator.ts:7878-7883）；bridge guest 快照不含记忆（core/bridge-session-manager.ts:874-882，只有 yuanPrompt + publicAgentsMd + contextTag + bridgePromptLine）；bridge owner 带记忆（:844-872）。

---

## 6. 前端可视化与 HTTP 路由

| 组件 | 展示 | 用户可做 | 路由 |
|---|---|---|---|
| `AgentMemory.tsx`（MemorySection） | 记忆总开关（title 右侧 Toggle）、健康状态 `MemoryHealthNotice`（healthy/degraded/unhealthy/disabled/unavailable）、置顶列表 + 输入框、两个入口按钮、内嵌 Dream 面板 | **开关**（需 hasUtilityModel）、**加/删 pin**（防抖 `savePins()`） | `GET /api/memories/health`、`GET/PUT /api/agents/:id/pinned`、`PUT /api/agents/:id/config` |
| `MemoryViewer.tsx`（78 行） | `hana-view-memories` 触发的 Overlay，按日期分组倒序列出 `mem.fact` + tags | **纯只读**（无编辑/删除/钉住） | `GET /api/memories` |
| `CompiledMemoryViewer.tsx`（321 行） | `hana-view-compiled-memory` 触发的四段（facts/today/week/longterm）markdown | **编辑保存**四段；week 按天粒度编辑；**清空** | `GET /api/memories/compiled`、`GET /api/memories/compiled/week/days`、`PUT .../compiled/{facts,today,longterm}`、`PUT .../compiled/week/days/:date`、`DELETE /api/memories/compiled` |
| `AgentMemoryDream.tsx` + `DreamRevisionBrowser.tsx` | Dream 自动开关、最近一次运行报告（finishedAt/beforeChars/afterChars/mergedCount/forgottenCount/changed）、历史修订列表 | **开关** `memory.dream.auto_enabled`、**手动运行**、**回滚**到某个 revision | `GET /api/memories/dream/status`、`POST /api/memories/dream/runs`、`GET .../dream/revisions[/:id]`、`POST .../dream/revisions/:id/restore` |

其他记忆路由（server/routes/config.ts:429-676）：`GET /memories`（exportAll）、`DELETE /memories`（clearAll）、`GET /memories/export`、`POST /memories/import`（兼容 v1 `memories` 与 v2 `facts`）。
Dream 路由错误码映射（server/routes/memory-dream.ts:26-42）：not_found=404；memory_disabled/memory_busy/already_running/no_memory/memory_changed=409；unavailable=503；restore_failed/run_failed=500。`memoryMasterEnabled === false` 时 `POST /memories/dream/runs` 直接 409（:64-83）。
权限白名单：server/http/route-security.ts:371-376（GET 只读）、:440-460（PUT/DELETE/POST 写集合）。

---

## 7. 实验开关

lib/experiments/registry.ts（213 行）里**唯一 `memory.*` 命名的 flag 是已退役的** `CACHE_SNAPSHOT_EXPERIMENT_ID = "memory.cache_snapshot_reflection"`（:13）：`RETIRED_EXPERIMENT_VALUES = new Map([[CACHE_SNAPSHOT_EXPERIMENT_ID, "off"]])`（:18-20），`getResolvedExperimentValue` :177 恒返回 "off"，`setExperimentValue` :192-197 强制写回 "off"。core/agent.ts:447-450 仍把它接进 `getCacheSnapshotReflectionMode`（现状恒 off）。

其余 flag 与记忆无关：`session.compaction_mode`（默认 `COMPACTION_MODES.AUTO`）、`session.instant_simple_compaction`（默认 false，对应 lossy_local）、`subagent.proactive_delegation`（false）、`provider.deepseek_roleplay_reasoning_patch`（false）。compaction 相关常量在 shared/compaction-mode.ts（`INSTANT_SIMPLE_COMPACTION_RUNTIME_MODE = "lossy_local"`；`getResolvedInstantSimpleCompactionEnabled` :41-51 兼容 v0.444.0 旧写法）。

**结论：记忆功能不走实验开关**，走 `config.yaml` 的 `memory.enabled` / `memory.dream.auto_enabled` / `experience.enabled` 与 per-session meta。

---

## 移植到 Python 的取用优先级

1. **先抄渐进式披露**（experience 索引 + 分类文件 + `recall(category)` 两级），成本最低、收益最直观。
2. **再抄「规则跟着工具走」**：能力引导写进 tool description，关掉工具就整段消失，不要留在 system prompt。
3. **抄 cache 分界线**：稳定段（人格/用户档案/环境）在前，漂移段（记忆/时间）在尾部，并用测试锁住顺序（openhanako 用 tests/agent-system-prompt-section-order.test.ts 锁）。
4. **滚动摘要要带游标与覆盖计数**（`coveredLeafId` + `messageCount`），否则增量失败会重复总结或漏总结；预算按轮数线性缩放，别写固定值。
5. **两段式检索**（标签优先、不足阈值才全文），并把「作用域可见性」做成检索层的一个纯函数（openhanako 的 `factVisibleInConversationScope`），别把隔离散在调用点。
6. **补 openhanako 的短板**：注入侧加字符/token 上限与截断策略；摘要游标与分支哈希校验可以简化，但「摘要必须声明它覆盖到哪」这条不能省。
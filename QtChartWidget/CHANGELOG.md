# 变更记录

版本号遵循语义化版本：**公开头 `include/chartwidget.h` 有破坏性改动时主版本号 +1**。
版本字符串在 `src/chartwidget.cpp` 的 `qtchartwidget_version()`，改这里时同步改本文件。

## 2.2.0

对齐 `webui/` 自 `2.1.0` 之后的四个提交：`bf57771`（输入框控件改为下拉与斜杠面板）、
`dfbb564`（计划面板指示器与统计串）、`ec0c76a`（计划面板只在有未收尾阶段时显示）、
`adf220d`（内网 HTTPS SSL 证书验证配置）。公开头只做加法（新增信号 `sessionExportRequested`），
故次版本号 +1。

### 新增

- 公开信号 `sessionExportRequested()`：斜杠面板里的「/ 导出对话」。宿主 `POST
  /sessions/{id}/export`，成功提示 `已导出：<path>`、失败提示 `导出失败：<error>`
- 斜杠面板（`#slash-menu`）：输入框正文以 `/` 开头即浮在输入框上方；根级两组 ——
  指令（模型 / 导出对话）在前、技能（不启用技能 + 各技能）在后；选「模型」切成带搜索框的
  可筛选模型列表。键盘在输入框事件过滤器里驱动（上下走候选、回车选中、Esc 收起），
  面板自身不抢焦点；过滤后只在可见行之间移动
- 技能芯片（`#skill-chip`）：选中技能后出现在输入框左侧，点击清除；没有技能时整块隐藏
- 交互模式下拉（`.mode-control`）：每项一行标题 + 一行说明（手动逐步确认 / 自动连续执行），
  选中项右侧打勾

### 行为变化（签名不变，但宿主需要知道）

- **交互模式**由分段按钮改为下拉（`.model-control.mode-control`）；`setMode` / `modeChanged` 不变
- **技能选择**由第三个下拉改为 `/` 面板 + 芯片：原 skill 触发器与下拉删除，
  `setSkills` / `setCurrentSkill` / `skillSelected` 签名不变
- **模型下拉只列配置里写过的模型**（`configuredModels`）：供应商发现来的整份目录
  （`status === "discovered"`）不进选择范围；按名字回显仍走 `visibleModels`，否则既有会话
  选中的发现模型会显示成「未配置」。选中勾从最左移到最右（`modelKey` / `modelName` / `visibleModels` 不变）。
  触发按钮不再带箭头，`.model-control` 内边距改为左右对称。模型 / 模式两个下拉改为
  **悬停展开、点击开合**（webui `bindHoverDropdown`）：指针移入 `.model-control` /
  `.mode-control` 即展开，离开「控件 ∪ 浮层」合计 150ms 收起（跨过触发器与浮层间的 7px 缝隙）；
  只在光标位于控件上时打开的下拉才归悬停逻辑管，程序化 / 键盘打开的不受影响
- **计划面板**：运行指示器统一为旋转开口环（`spinRing` 0.7s，替换原呼吸点）；步骤状态改为
  轻量圆点语言（待处理灰点 / 已完成绿点 / 失败红点 / 跳过虚线点）；标题后的分数徽标换成
  统计串（`N 已完成 · N 进行中 · N 待处理 · N 失败 · N 跳过`，只列非零项、按状态着色，
  宽度不够从尾部丢段）。计划全终态就在渲染口收起，收起时连面板内容一起清
  （`hidePhasePanel` / `clearPlan`），空轮收尾那条早退路径也要结算
- **过程行运行指示器**（`.run-dot`）同步改为 8px 旋转开口环
- **设置中心**提供商草稿新增 `ssl_verify`：默认（系统证书）/ 跳过验证 / 自定义 CA 证书路径
  三档，自定义时右侧多出「CA 证书路径」输入行，切走时该行收起并记住上次路径
- 输入框占位文案补上「，输入 / 选择技能」

### 内部

- `makeLabel` 顺带把首个类名写进 `objectName`（按名 `findChild` 命中）
- 技能芯片最小宽按名字实测（`QPushButton` 不吃子控件最小宽），缩放变更由 `refreshZoom` 重算
- demo 宿主：`GET /sessions` 的 `storage_issues` 改为弹一次 Toast（webui `warnStorageIssues`，
  同一条只弹一次）

## 2.1.0

对齐 `webui/` 自 `4d5e742` 之后的四个提交（轮次导航轨改为悬浮展开整轮列表、
设置中心新增「用量」Tab、用量统一以 M 显示、会话流状态清理）。公开头只增不改，故次版本号 +1。

### 新增

- 设置中心第四个 Tab「用量」（`#panel-usage`）：筛选（分组 / 供应商 / 模型 / 粒度 / 日期）、
  概览卡、折线（输入 / 输出趋势）、环形（占比，选中单个模型时改为看该模型的 token 构成）、
  堆叠柱（实测 vs 估算）、可排序明细表
- 公开 API：`setUsageStats(requestId, data)` / `setUsageLoadFailed(requestId, error)`
  （`requestId` 由 `usageStatsRequested` 下发，回填时必须原样带回）
- 公开信号：`usageStatsRequested(requestId, start, end, provider, model)`（宿主拉 `GET /usage/stats` 后回填）
- 图表为自绘 `QWidget`：滚轮以鼠标为锚点缩放时间轴、拖拽平移、双击复位、
  Shift + 滚轮缩放纵轴；折线与柱状共享同一时间视窗；图例可点击隐藏序列
- 日期区间浮层：预设（最近 12 小时 / 24 小时 / 3 天 / 7 天 / 30 天）+ 双月日历自选

### 行为变化（签名不变，但宿主需要知道）

- **轮次导航轨**：由「悬浮单个白点浮出该轮摘要」改为「鼠标移入轨道即向右展开整轮列表
  （序号 + 提问摘要，一行一轮）」，点击行或白点都能直达该轮，当前轮次在点与行上同步高亮；
  轨道与面板各自算悬停区，跨间隙留 180ms 延迟收起；行可 Tab 聚焦，Enter / Space 与点击等价
- 用量数值一律以百万（M）显示（`Charts.formatMillions` 口径：不足 1M 也写小数，不退回 K）；
  轮次是计数，仍按原值显示；模型设置页的上下文窗口保持 K/M 自适应（两者口径不同是刻意的）
- 宿主若要在用量页看到正确的模型 / 供应商显示名，需在打开设置前推过 `setModels`
  （库按 `provider_name` 与模型名映射；历史遗留模型标注「未在配置中」）
- 用量请求按 `requestId` 去重：晚到的旧响应 / 旧失败一律丢弃（同区间连续刷新也能分辨先后）
  结果缓存与 app.js 同口径：满 32 条整体清空，长时间切换筛选不会无限累积
- **样式表根因修复**：`#choiceSubmit:hover:not(:disabled)` 里的 `:not(...)` 是 Qt QSS 不支持的选择器，
  会让 Qt **丢弃其后所有规则** —— 于是设置中心 / 用量页 / 各浮层等半张样式表长时间静默失效
  （这些区域的控件落回平台原生样式）。已把三处不受支持的选择器改成 Qt 语法：`:not(:disabled)` /
  `:!disabled` → `:enabled`；`trajViewButton:hover:not([active="true"])` → `:hover`，并把 `[active="true"]`
  规则同时挂上 `:hover`，让它优先级更高、不再依赖书写顺序。用量页浮层随之恢复主题皮肤；新增回归测试
  `appStyleSheetParsesFully` 兜底

## 2.0.0

相对 `1.0.0`（b17d5ea「新增 QtChartWidget Qt5 原生聊天界面动态库」）的全部改动。
这一版把库从「初版骨架」拉到与 `webui/` 逐项对齐，公开头也随之发生破坏性变化。

### 破坏性变更

- **删除信号** `toolParamsConfirmed(const QString &tool, bool confirmed, const QVariantMap &params, const QString &label)`：
  工具参数确认不再发独立信号，改为按 webui 口径打包成一轮 `<structured_interaction>` 消息，
  由 `sendMessage` 带出（第 2 个参数 `display` 为所选按钮文案）
- **删除信号** `approvalJsonInvalid()`
- **`appendApproval()` 语义变化**：从「在消息流里插一张审批卡」改为「弹出输入框上方的审批浮层」；
  `resolveApproval()` / `reEnableApproval()` 相应地作用于浮层卡片，不再作用于消息卡
- 内部类 `ApprovalCard` / `ToolGroupWidget` 移除（不进公开头，但若宿主曾前向声明过需清理）

### 新增

- 皮肤：`setTheme` / `theme`（`dark` / `silver` / `blue`）+ 信号 `themeChanged`
- 视图页签：`setViewTab` / `viewTab` + 信号 `viewTabChanged`
- 选择浮层：`showChoice` / `closeChoice` + 信号 `choiceOpenChanged`
- 用量与用时：`setTokenUsageDetail` / `setTurnTiming` / `startLiveTiming`
- 消息流：`markCurrentStopped`
- 轨迹视图：`setTrajectoryEvents` / `appendTrajectoryEvents` + 信号 `trajectoryReloadRequested`

### 行为变化（签名不变，但宿主需要知道）

- 消息流改为「一轮一张卡」：用户 / 助手卡片不再显示身份行与 Skill 名，角色靠左右对齐与
  渐变浮层区分；用量 / 用时 / 时刻收到底部信息行
- 消息区改为「贴底才跟随」：流式期间用户上滚查看历史时，新分片不会把视图拽回底部
- `setSessions` 支持可选 `status` 字段（`running|done|stopped|error|waiting`），
  缺省时回退到服务端的 `waiting` / `active` 布尔字段
- **线程契约**：所有公开 API 必须在 GUI 线程调用。消息流 / 轨迹 / 会话 / 设置等数据推送入口
  带运行期校验，非 GUI 线程调用会被忽略并 `qWarning`（开发构建下 `Q_ASSERT` 中断）
- **键盘可达性**：按钮 / 页签 / 下拉框统一 `Qt::TabFocus`（Tab 能到达，鼠标点完不留焦点环——
  近似 webui 的 `:focus-visible`）；自绘可点区域（过程行标题、工具项标题、计划窗头、选择项、
  轨迹行 / 分组 / 摘要、`.session-select`）支持 `Enter` / `Space` 等价于点击
- 过程行与工具项标题补齐 webui 的悬停反馈：摘要 / 工具名与折叠箭头悬停转青；
  运行态的青色 / 橙色和失败态红色保持更高优先级
- 动画时长与 webui 对齐：选择浮层入场 180ms、**离场 240ms**（`app.js:842`），
  消息卡入场 180ms；浮层的 `QGraphicsOpacityEffect` 只在动画期间启用
- 轨迹视图：`appendTrajectoryEvents` 改为**增量追加**（只构建新增行，不再整账本重建）；
  点某一行只移动高亮，不再重建账本；搜索框改为停止输入 200ms 后刷新一次
  （长会话下这几处原本每来一个事件/每敲一个字都会重建上千行控件，详见 README「性能口径」）
- 轨迹账本改为**只构建可视区行**（视窗化）：条目列表与控件解耦，控件只为视口那几十行实例化。
  上千条记录的轨迹首次打开由「秒级停顿」降到几十毫秒；滚动、追加、选中都不再与总行数成正比

## 1.0.0

- 首个版本（b17d5ea）：顶栏 + 会话栏 + 消息区 + 阶段面板 + 输入区，
  叠加会话面板 / Toast / 拖拽遮罩；含 `qtchartwidget_create()` C 工厂

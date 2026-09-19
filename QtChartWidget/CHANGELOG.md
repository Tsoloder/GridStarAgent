# 变更记录

版本号遵循语义化版本：**公开头 `include/chartwidget.h` 有破坏性改动时主版本号 +1**。
版本字符串在 `src/chartwidget.cpp` 的 `qtchartwidget_version()`，改这里时同步改本文件。

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
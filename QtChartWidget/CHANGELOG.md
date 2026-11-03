# 变更记录

版本号遵循语义化版本：**公开头 `include/chartwidget.h` 有破坏性改动时主版本号 +1**。
版本字符串在 `src/chartwidget.cpp` 的 `qtchartwidget_version()`，改这里时同步改本文件。

## 2.3.0

对齐 `webui/` 自 `bf57771` 之后的 18 个提交（`webui` 侧共 `app.js +2359`、
`index.html +141`、`style.css +448`）。公开头全部是加法，故次版本号 +1。

### 新增：设置中心两个新页

- **记忆**（`#panel-memory`，第五个提交 9ae9641 / f4e5ab4 / b102431）：状态条（启用状态 / 经验库健康 /
  全量检索 / 待审核数 / 维护进度，按 ok·off·warn·error 上色）、三个开关（启用记忆 / 自动提炼 /
  注入提示，后两个随主开关置灰）、四件筛选（状态 / 类型 / 作用域 / 关键词，搜索去抖 300ms）、
  批量条（本页全选 + 批量通过 / 拒绝）、卡片列表（状态 chip、标签串、置信度与证据数、
  查看详情 + 按状态的动作）、加载更多（追加而非替换）；参数记忆（按工具折叠，字段值可就地编辑，
  类型由原值锁定）；维护任务（立即提炼 / 运行维护 / 清空全部工具参数）
- **知识库**（`#panel-kb`，350abab / f32f166）：多库下拉 + 新建 / 重命名 / 删除 / 刷新、
  添加文件 / 导入目录、统计卡（文档 / 分块 / 参数 / 索引体积）、归档树（缩进层级、新建 / 重命名 /
  删除文件夹）、资料清单（拖到文件夹归档、重建 / 删除、失败态红字）、检索试跑（参数命中 + 文本命中）

### 新增：输入区

- **专业分类下拉**（`#category-select`）：首项「全部」由库自补，选中发 `categorySelected`；
  `syncCategoryFromSession` 只在用户未显式选过时覆盖（`syncCategoryFromMeta` 口径）
- **知识库下拉**（`#kb-select`）：自带「不使用知识库」（`__none__`）与「全部知识库」（`*`）两项，
  各库带「N 份 · M 块」副文本
- **上下文用量圆环**（`#context-ring`）：自绘 22px 环 + 悬停浮层（上限 / 已用 / 剩余 / 进度条 /
  本地估算），占比 0.7 转橙、0.9 转红，无数据时只留灰底环
- 四个下拉与斜杠面板**五者互斥**，统一收在 `openDropdown()`，悬停 / 键盘 / 点击都走同一条路
- 斜杠面板的技能按专业分类分组（选具体分类时只列本分类 + common + 不启用技能）

### 新增：模型设置页

- 行式表单（`.srow` / `.srow-2`）；上下文与最大输出带预设档位下拉（64K / 128K / 200K / 256K / 1M）
- 四个能力位（视觉 / 视频 / 音频 / 推理）+ 折叠头小图标 + 上下文 chip（配置值优先，回落到快照值并标虚线）
- 思考强度下拉（`compat.thinking="disabled"` 与 `compat.reasoning_effort` 双向映射）
- 模型信息快照状态行 + 手动刷新（`setModelMetadata` / `setModelMetadataFailed` /
  `modelMetadataRefreshRequested`）。注意 `GET /config/model-metadata` 里的
  `models`/`providers`/`hosts` 是**计数**，每个模型的上限与能力来自
  `POST /config/model-metadata/resolve`，宿主把结果放进快照的 `limits` 键

### 新增：审批卡 JSON 表格编辑

- 参数文本能解析为对象或数组时，输入框旁多一个「表格」按钮，把四种形状开成表格改：
  分部件组（一行一组，改属性会同时应用到该组 domain 与 line）、对象键值对、值列表、对象数组
- 单元格按原值类型找回（number / boolean / null / 嵌套 JSON），嵌套 JSON 解析不开时标红并拦住写回
- 与 webui 一致：**分部件组表格以 domain 的值为准**，即使用户未改，点应用也会把 line 上同名属性
  归一成 domain 的值（如果业务上 line 允许独立取值，需要在宿主侧避开这个参数用表格编辑）

### 新增：公开 API 与信号

- 分类 / 知识库 / 上下文：`setCategories` / `setCurrentCategory` / `currentCategory` /
  `syncCategoryFromSession` / `setKnowledgeBases` / `setCurrentKnowledgeBase` /
  `currentKnowledgeBase` / `setContextUsage` / `clearContextUsage`；
  信号 `categorySelected` / `knowledgeBaseSelected`
- 记忆：`setMemorySummary` / `setMemoryExperiences` / `setMemoryListFailed` / `setMemoryTools` /
  `setMemoryStatusText`；信号 `memoryRefreshRequested` / `memoryListRequested` /
  `memoryConfigPatchRequested` / `memoryActionRequested` / `memoryBulkRequested` /
  `memoryDetailRequested` / `memoryFieldSaveRequested` / `memoryFieldDeleteRequested` /
  `memoryToolClearRequested` / `memoryToolsClearAllRequested` / `memoryMaintenanceRequested`
- 知识库：`setKbLibraries` / `setKbStatus` / `setKbFolders` / `setKbDocuments` /
  `setKbSearchResult` / `setKbStatusText`；信号 `kbRefreshRequested` / `kbLibraryCreateRequested` /
  `kbLibraryRenameRequested` / `kbLibraryDeleteRequested` / `kbFilesImportRequested` /
  `kbDirImportRequested` / `kbDocumentDeleteRequested` / `kbDocumentReindexRequested` /
  `kbDocumentMoveRequested` / `kbFolderCreateRequested` / `kbFolderRenameRequested` /
  `kbFolderDeleteRequested` / `kbSearchRequested` / `kbFolderChanged`
- 其他：`launchAppRequested`（顶栏品牌图标）、`endTurnRequested`（「结束」类选项）、
  `setModelMetadata` / `setModelMetadataFailed` / `modelMetadataRefreshRequested` /
  `modelLimitsResolveRequested(providerId, modelId)`（新增模型后拉上限的时机）、
  `settingsOpened(tab)`（设置中心被打开，含模型 / 技能 / MCP / 记忆 / 知识库 / 用量）、
  `kbDirImportToFolderRequested(libraryId, dirPath, folderId)`（目录导入落当前归档目录）

### 行为变化（签名不变，但宿主需要知道）

- **`setHistory(messages, resume = false)`**：新增第二个参数。`resume = true` 表示这批历史后面
  还要接上正在跑的新一轮（重连场景），末尾那轮的询问不再弹回输入框上方（webui c4a6d84）。
  老调用点不传这个参数，行为不变
- **「结束」类选项不再开新一轮**（webui 5948605）：命中收尾措辞的选项（「本轮到此结束」「不用了」等
  或 value 为 stop/end/finish 一类）只发 `endTurnRequested` 并在本地收尾；含「继续 / 下一步 / 再试」
  的措辞一票否决，宁可当普通选择发回去
- **选项提交在忙时不锁卡**（同属 webui 5948605）：上一轮还在收尾时点选项，卡片不再被锁死
  （原来宿主的 sendMessage 会默默丢掉这次选择，用户只能刷新），改为退回未提交态并提示
  「上一轮还在收尾，略等片刻再选」。为此 `ChoiceOverlay` 新增公开方法 `resetSubmitted()`
- **发消息先收浮层**：`sendMessage` 发出前，上一轮遗留的确认浮层立即收起（webui c4a6d84）
- **顶栏品牌图标**从 `QLabel` 换成 `QPushButton`（objectName 仍为 `brandMark`），点击发
  `launchAppRequested`；宿主自行弹确认并 POST `/gridstar/ensure`
- **正文排版**：正文 14px、标题 15px / 14px、表格 13px，正文与标题统一到微软雅黑；
  底部用量与用时行由 9px 提到 10.5px
- **记忆页翻页不再清空选择**（向 webui 对齐）：只有回到第 1 页（首查 / 改筛选 / 点刷新）才清，
  加载更多时保留，否则「本页全选」一翻页就失效；已不在列表里的选中项由 `setListing()` 里的
  `pruneSelection()` 收掉
- **目录导入改发 `kbDirImportToFolderRequested`**：webui 的目录导入只带库、一律落库根，实际使用里
  导入子目录往往就是想归到当前选中的归档目录。新信号多了 folderId（空串 = 库根）。
  旧 `kbDirImportRequested` 保留声明但**不再 emit**，宿主需改接新信号（签名不同，需同步改）
- **`settingsOpened(tab)`**：以前用户点控件自带的设置按钮，宿主完全无感，模型信息快照、记忆摘要、
  知识库列表这些「打开才需要」的数据拉不到；现在无论从哪个入口打开都会带 Tab 名通知宿主
- **行内解析先护住反斜杠转义**（webui f47d83d）：模型会把通配符组名写成 `engine\*`、
  `verticalTail\*/horizontalTail\*`，这些星号不先抽成占位符，`**…**` 就匹配不上，粗体失配后
  单星号规则再把两个 `**` 各拆一半，正文里会冒出字面星号与错位斜体；同时粗体 / 斜体正则改成
  懒惰匹配（`[\s\S]+?` / `[^*]+?`），强调区里带占位符时不再整体失配
- **`syncCategoryFromSession` 改为无条件覆盖**（webui syncCategoryFromMeta）：会话 meta 里的分类是
  权威值，切会话与后端 `session_meta` 事件都会直接盖掉下拉当前值；早先那个「用户显式选过就跳过」
  的守卫与 webui 不符（webui 的 `categoryTouched` 只记账、不参与判定）
- **面板英文标签中文化**：`CONFIGURATION`→配置、`MODELS`→模型、`SKILLS`→技能库、
  `MCP TOOLS`→MCP 工具、`TREND`→趋势、`SHARE`→构成、`MEASURED`→实测校验、`DETAIL`→明细数据
- **设置中心从 4 个 Tab 扩到 6 个**（模型 / 技能 / MCP 工具 / 记忆 / 知识库 / 用量），
  `openSettings(tab)` 的可用值相应增加 `memory` / `kb`
- `QComboBox` 的下拉箭头改用一张已着色的小 chevron：Qt 的 `::down-arrow` 只认 `image`，
  原先写的 CSS `border` 三角会画成一个实心方块
- **斜杠面板两条旧缺陷一并修掉**：面板重排时新旧行会在同一帧叠着（先 `hide()` 再 `deleteLater()`）；
  键盘焦点从未被真正设上（`focusRow()` 先清空再查询，永远查不到），导致 ↓ 与 Enter 一直是死的
- **`ComboTrigger` 宽度自锁**：尺寸提示改为按全文实测、`minimumSizeHint` 允许压窄，
  长模型名不再塌成 16px 的省略号（此前模型 / 模式下拉都中招）

### 内部

- 新增 `src/memorypanel.*`（记忆页）、`src/kbpanel.*`（知识库页）、`src/jsoneditor.*`（JSON 表格编辑器）
- **记忆页与知识库页的样式已全部迁入 `theme.cpp`**（两个面板里的 `setStyleSheet` 局部表与 `StyleChange` 重烤逻辑删除），
  跟全局 QSS 一起重建；新增 `%OKLINE%` 调色板令牌（以前只有 OKTINT/OKEXT）。
  迁移中补了两处遗漏：三个滚动区的 viewport 透明（不然深色皮肤下树区露白底）、
  `.slashGroupLabel` 那条 Qt 会直接忽略的 `letter-spacing` 声明（改成走字距补偿表）
- **模型页改了并与 webui 对齐**：provider 侧栏按 `default_api` 分三组（OpenAI / Anthropic / 其它协议，
  空组不显示）、折叠头加 `::after` 小箭头（由 `ExpandCard::openChanged` 镜像 `Chevron`，
  开合状态只有一个来源）、「读取模型」降为 link-button；`addModel()` 后发
  `modelLimitsResolveRequested`（只有能力项未知时才问），并用 `m_openModelKeys` 保证回填后
  正在展开的模型卡不被收回
- **信号转发修复**：记忆页 11 个与知识库页 13 个意图信号先前只到 `SettingsDialog` 就断了，
  没在 `ChartWidget` 里接通，宿主一个都收不到。已补 24 条 `connect`
- 新增四个能力图标 `cap-vision` / `cap-video` / `cap-audio` / `cap-reasoning` 与
  `chevron-down-muted`（已注册进 `resources/icons.qrc`）。`QComboBox` 的 `::down-arrow`
  只认 `image`，原先的 CSS `border` 三角会画成一个实心方块
- **字体族与 webui 对齐到微软雅黑**（`uiFont()` 不再优先 Bahnschrift）。
  webui 的 `style.css` `:root` 本来就是 `"Microsoft YaHei"` 优先，Qt 侧还留着 Bahnschrift：
  中文会回退到雅黑渲染，但 QLabel 的高度按**主字体**的 `fontMetrics()` 算。
  Bahnschrift 的 ascent/em 是 0.794，雅黑是 1.058，12px 下相差 3.2px，
  于是标签框按 9.5px 留上方、中文却要 12.7px，字顶/字底被裁。
  webui 在浏览器里没这个问题，因为 CSS 靠 `line-height` 撑行盒，与字体 metrics 无关。
  另给 `ElidedLabel` 的 `sizeHint`/`minimumSizeHint` 加了 `labelLineHeight()`（行高 + 4px、随缩放），
  把 CSS 的 `line-height` 余量补进 Qt 侧。字宽整变宽后重跑了全套用例与三套皮肤截图，版面正常
- `demo.exe` 新增 `--tab memory` / `--tab kb` / `--tab model-form` / `--tab ring`，
  并带上记忆、知识库、分类、上下文用量的演示数据；**live 模式补齐了 `/memory/*` 与 `/kb/*` 的
  真实接线**（含分类 / 知识库随消息带上、切会话回填三样、`/gridstar/ensure`、`end-turn`、
  两个模型元数据接口），`examples/host_example.cpp` 同步补了注释级接线示例
- **回归用例从 55 项增到 72 项**（新增 17 项、约 240 条断言，覆盖上下文圆环、两个新下拉、
  五浮层互斥、记忆页状态/开关/筛选/批量/字段编辑、知识库页归档树/资料/检索、JSON 表格编辑器、
  结束类选项一票否决、`setHistory(resume)`、品牌按钮、两个模型元数据信号、`settingsOpened`、
  目录导入带归档目录）
- 测试里要注意 MSVC + Qt5.12 的一个坑：`QCOMPARE(map.keys(), QStringList{...})` 会被选到泛型
  `qCompare`，展开到 `QList::op_eq_impl` → `stdext::make_checked_array_iterator`，
  而 VS2022 的 STL 里这个符号已不可见，报 C2653/C3861（加 `<iterator>` 也没用）；
  改用 `QCOMPARE(size, n)` + `QVERIFY(contains(...))`，或 `QStringList({...})` 走 `QList<T>` 专载

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
- 下拉触发器聚焦时 **Enter / Space / 方向键** 也能展开（webui 触发器是原生 button，
  `handleModelKeys` / `handleModeKeys` 又允许方向键展开）；会话栏触发器同样吃 Enter / Space
- 斜杠面板不是 `Qt::Popup`，点面板外要自己收（webui 的 document 点击处理）：
  `ChartWidget` 的全局事件过滤在按下时调 `Composer::dismissSlashForClick`，面板内、输入区
  与上溯到 Composer 祖先的冒泡事件都不算点外面
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

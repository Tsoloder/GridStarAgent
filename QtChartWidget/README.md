# QtChartWidget 构建与运行说明

用 Qt 5.12.2 (MSVC 2017 x64) 原生复刻 `webui/`（GridStar AI 聊天界面）的 UI 库，
编译产物为动态库 `QtChartWidget.dll`，并附带一个可交互的示例宿主 `demo.exe`。

库本身**不做任何网络请求**：宿主通过 setter 推数据、通过信号收交互。

## 1. 目录结构

```
QtChartWidget/
├── QtChartWidget.pro        动态库工程（TEMPLATE = lib）
├── build.bat                一键构建脚本（vcvars64 + qmake + nmake）
├── README.md                本文档
├── CHANGELOG.md             版本与破坏性变更记录（改公开头时同步）
├── include/
│   ├── chartwidget.h        唯一公开头文件：gs::ChartWidget + C 工厂
│   └── qtchartwidget_global.h  导出宏 QTCHARTWIDGET_EXPORT
├── src/                     库实现（内部类不导出）
│   ├── chartwidget.cpp      主组合部件（顶栏 / 会话栏 / 视图页签 / 消息流 / 轮次导航轨与列表面板 / 事件分发）
│   ├── theme.cpp            三套皮肤调色板 + QSS（对应 webui/style.css 的 :root 变量表）
│   ├── markdownview.cpp     Markdown / 代码块 / 表格 / 引用 / 任务列表 / ```json 结构化块
│   ├── messagewidgets.*     一轮一张卡：正文 + 过程区（思考/工具）+ 底部信息行 + 工具参数表
│   ├── choiceoverlay.*      选择浮层（模型提问 / 工具参数确认 / 审批共用，浮在输入框上方）
│   ├── composer.cpp         输入区（三行输入框 / 流式工具栏 / 附件芯片 / 分类与知识库下拉 /
│   │                        上下文用量圆环 / 斜杠面板与浮层宿主）
│   ├── trajectoryview.*     轨迹视图（时间轴概览 + 事件账本 + 记录详情，对应 webui 轨迹 tab）
│   ├── popups.cpp           会话面板（含状态徽标）、模式与模型下拉、斜杠面板、皮肤下拉、Toast
│   ├── phasepanel.cpp       阶段计划面板（圆点指示器 + 统计串，跑完即收起）
│   ├── settingsdialog.cpp   设置中心对话框（模型 / 技能 / MCP 工具 / 记忆 / 知识库 / 用量 六个 Tab）
│   ├── memorypanel.*        记忆页（状态条 / 开关 / 筛选 / 卡片列表 / 参数记忆 / 维护，对应 #panel-memory）
│   ├── kbpanel.*            知识库页（多库 / 归档树 / 资料清单 / 拖拽归档 / 检索试跑，对应 #panel-kb）
│   ├── jsoneditor.*         审批参数的 JSON 表格编辑器（四种形状：分部件组 / 键值对 / 值列表 / 对象数组）
│   ├── usagepanel.*         用量面板（筛选下拉 / 概览卡 / 日历区间浮层 / 明细表，对应 #panel-usage）
│   ├── usagecharts.*        自绘图表（折线 / 环形 / 堆叠柱 + 缩放平移，对应 webui/charts.js）
│   └── commonwidgets.*      FlowLayout、Chevron、StatusDot、PulseDot、ThemeSwatch、
│                            TurnRailDot、TurnRailRow、MiniBar、ConnectionButton、IconPushButton 等
├── resources/
│   ├── icons/               feather 风格 SVG 图标（viewBox 24×24，运行时着色），
│   │                        含四个模型能力图标 cap-vision / -video / -audio / -reasoning
│   ├── Logo.ico             顶栏品牌标（与 webui/Logo.ico 同一张）
│   └── icons.qrc            资源清单（前缀 /icons，编译进 DLL）
├── demo/
│   ├── demo.pro             示例宿主工程（链接 ../bin/QtChartWidget.lib）
│   └── main.cpp             演示数据 + 全部信号接线 + --shot 离屏截图
├── examples/
│   ├── examples.pro         接线示例工程（链接 ../bin/QtChartWidget.lib）
│   └── host_example.cpp     最小宿主接线示例：推数据 + 全信号 connect + 假流式
├── tests/
│   ├── tests.pro            测试工程（QtTest，链接 ../bin/QtChartWidget.lib）
│   └── test_chartwidget.cpp 公开 API 回归测试（只用 chartwidget.h + QObject 树观察）
├── bin/                     构建产物输出目录
└── build/                   qmake/nmake 中间产物（obj、moc）
```

## 2. 环境要求

| 组件 | 版本 / 路径 |
| --- | --- |
| Qt | 5.12.2 msvc2017_64，默认 `D:\Application\Qt\Qt5.12.2\5.12.2\msvc2017_64` |
| 编译器 | MSVC x64（vcvars64），默认 `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat` |
| 构建工具 | qmake + nmake（随 Qt / VS 自带） |
| C++ 标准 | C++17，MSVC 需 `/utf-8`（两个 .pro 已配置） |
| Qt 模块 | core / gui / widgets / svg（`QtChartWidget.pro` 中 `QT += ... svg`，图标由 QtSvg 渲染） |

若本机 Qt / VS 安装路径不同，修改 [build.bat](file:///d:/TRAE_project/GridStarAgent/QtChartWidget/build.bat)
顶部的 `QTDIR` 与 `VCVARS` 两个变量即可。

> 注意：`build.bat` 必须保持**纯 ASCII、无中文注释**。cmd 解析含多字节字符的批处理
> 会出现行错位（表现为 `'Visual' is not recognized ...` 一类报错）。

## 3. 构建

在任意终端执行（脚本内部会自行 call vcvars64，无需提前开 VS 命令行）：

```bat
rem 构建 DLL、demo 与接线示例 host_example
build.bat

rem 构建后再离屏截图到 QtChartWidget\shot.png
build.bat shot

rem 构建后再跑公开 API 回归测试
build.bat test
```

等价的手动步骤（用于排查问题）：

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set PATH=D:\Application\Qt\Qt5.12.2\5.12.2\msvc2017_64\bin;%PATH%
cd build\lib
qmake ..\..\QtChartWidget.pro -spec win32-msvc "CONFIG+=release"
nmake
cd ..\demo
qmake ..\..\demo\demo.pro -spec win32-msvc "CONFIG+=release"
nmake
cd ..\example
qmake ..\..\examples\examples.pro -spec win32-msvc "CONFIG+=release"
nmake
```

构建产物（`bin/`）：

| 文件 | 说明 |
| --- | --- |
| `QtChartWidget.dll` | UI 动态库 |
| `QtChartWidget.lib` | 导入库（宿主链接用） |
| `demo.exe` | 示例宿主（含演示指令与离屏截图） |
| `host_example.exe` | 最小接线示例宿主（`examples/host_example.cpp`） |
| `qtchartwidget_tests.exe` | 公开 API 回归测试（`tests/test_chartwidget.cpp`） |

## 4. 运行 demo

`demo.exe` 依赖 Qt 运行时 DLL（含图标渲染所需的 `Qt5Svg.dll`）与本目录的 `QtChartWidget.dll`，
运行前把两者加入 PATH：

也可以直接双击 `start_demo.bat` 一键启动；缺少 `bin` 构建产物时，脚本会先调用 `build.bat`。
需要传参时可在终端执行 `.\start_demo.bat --tab usage`。不带 `--live` 时使用内置假后端，
用于 UI 和交互回归；连接真实 Agent 后端时使用下面的 live 模式。

```powershell
$env:PATH = 'D:\Application\Qt\Qt5.12.2\5.12.2\msvc2017_64\bin;<工程>\QtChartWidget\bin;' + $env:PATH
demo.exe                 # 交互窗口，预载一段示例历史
demo.exe --empty         # 空态（欢迎页）
demo.exe --theme silver  # 皮肤：dark / silver / blue
demo.exe --tab traj      # 直接打开「轨迹」页签（含示例轨迹事件）
demo.exe --tab models    # 直接打开设置中心的「模型」页（skills / mcp 同理）
demo.exe --tab usage     # 直接打开设置中心的「用量」页（含示例用量数据）
demo.exe --tab memory    # 记忆页（状态条 / 开关 / 筛选 / 卡片 / 参数记忆 / 维护）
demo.exe --tab kb        # 知识库页（多库 / 归档树 / 资料清单 / 检索试跑）
demo.exe --tab model-form # 模型页并展开第一张模型卡（行式表单 / 数值预设 / 四个能力开关）
demo.exe --tab ring      # 展开上下文用量圆环的浮层（离屏无法悬停，等价派发 Enter）
demo.exe --tab usage-cal # 用量页 + 展开日期区间浮层（离屏没有真实点击，等价派发一次点击）
demo.exe --tab theme     # 展开皮肤下拉浮层（核对浮层样式用）
demo.exe --tab rail      # 展开轮次列表面板（离屏无法悬停，等价派发 Enter）
demo.exe --shot out.png  # 离屏渲染并截图后退出
demo.exe --shot out.png --size 1440x900  # 指定截图尺寸（验收主栏宽度 / 宽屏排版）
```

`--theme` / `--tab` / `--size` 可与 `--shot` 组合，用于按皮肤、页签或窗口尺寸出对照图；
`--tab usage` 时截图目标是设置对话框（内容比窗口高，截图前会把对话框拉高以拍全三图与明细表）。

离屏截图（`--shot`）使用 `offscreen` 平台插件，该平台默认不加载系统字体，
会出现"有框无字"的截图；运行前设置字体目录即可：

```powershell
$env:QT_QPA_FONTDIR = 'C:\Windows\Fonts'
```

要看**真正带文字的截图**，直接指定 Windows 平台插件（窗口会一闪而过）：

```powershell
$env:QT_QPA_PLATFORM = 'windows'
demo.exe --shot out.png --size 1440x900
```

### 连接真实后台（live 模式）

先按根目录 README 启动 MCP 服务和 Agent 后端（两个独立终端）：

```powershell
# 终端 1：MCP 工具服务（默认 127.0.0.1:5656）
cd .\agent
python server.py
```

```powershell
# 终端 2：Agent 后端（默认 127.0.0.1:1231）
cd .\agent\agent
python app.py --host 127.0.0.1 --port 1231
```

然后从工程根目录启动连接真实后端的 Qt Demo：

```powershell
.\QtChartWidget\start_demo.bat --live --api http://127.0.0.1:1231
```

`--api` 可省略，默认即为 `http://127.0.0.1:1231`；`start_demo.bat` 会把参数原样转发给
`demo.exe`。live 模式启动后会真实调用 `/health`、`/config`、`/config/models`、`/skills`、
`/mcp/tools`、`/sessions`，普通对话走 `POST /chat/stream` 的 SSE；会话切换、清空、重命名、
轨迹、停止、审批、配置保存、Provider 测试、模型发现、Skill/MCP 刷新、附件上传和用量统计
也都连接真实接口。

窗口顶部的连接状态含义：

| 状态 | 含义 |
| --- | --- |
| 红色 `offline / 服务不可用` | `/health` 无响应或返回非 2xx，后端不可达 |
| 橙色 `checking` | 后端已响应，但 config、模型 runtime 或 MCP 尚未就绪 |
| 绿色 `online / Agent 已连接` | `/health` 正常，且 config、runtime、MCP 均已就绪 |

live 模式下 `/fail`、`/approve`、`/workflow`、`/options`、`/params` 这些假数据指令不会执行；
`/history` 改为重新载入当前真实会话，`/clear` 改为清空后台会话。

### 演示指令（在输入框中输入后回车）

| 指令 | 效果 |
| --- | --- |
| `/history` | 载入示例历史（附件、思考过程、工具组、结构化卡、工作流、token 统计） |
| `/clear` | 清空消息流 |
| `/fail` | 追加失败气泡（带「重发这条消息」） |
| `/approve` | 追加审批卡（批准/拒绝后宿主回调 `resolveApproval`） |
| `/workflow` | 追加工作流提案卡并模拟逐步执行 |
| `/options` | 追加选项卡（```json 结构化块） |
| `/params` | 追加工具参数确认卡 |
| `/toast <文本>` | 弹出 Toast |
| `/settings [tab]` | 打开设置中心（可选 tab：models/skills/mcp/usage，与 webui 的四个页签一致） |

不带指令的普通文本会走一轮模拟流式回复：思考过程 → 正文分块 → 工具调用 →
工具结果 → token 统计。

### 快捷键（VSCode 式界面缩放）

| 快捷键 | 效果 |
| --- | --- |
| `Ctrl+=`（或 `Ctrl++`） | 放大一档 |
| `Ctrl+-` | 缩小一档 |
| `Ctrl+0` | 重置为 100% |

缩放档位：75% / 80% / 90% / 100% / 110% / 125% / 140% / 160%。缩放会活体刷新
全部字号（QSS、自绘控件、内联 HTML 标签、等宽字体区），设置中心对话框同步生效。

## 5. 在自己的宿主中集成 DLL

1. 头文件与链接：

   ```qmake
   INCLUDEPATH += <工程>/QtChartWidget/include
   LIBS        += -L<工程>/QtChartWidget/bin -lQtChartWidget
   ```

2. 创建部件并接线。完整可编译的最小示例见
   [examples/host_example.cpp](file:///d:/TRAE_project/GridStarAgent/QtChartWidget/examples/host_example.cpp)
   （覆盖全部交互信号 + QTimer 假流式，构建后运行 `bin\host_example.exe` 即可体验；
   真实工程只需把 `startTurn()`/`streamAnswer()` 换成自己的网络回调）。
   以下节选自 [demo/main.cpp](file:///d:/TRAE_project/GridStarAgent/QtChartWidget/demo/main.cpp)：

   ```cpp
   #include <chartwidget.h>

   auto *chart = new gs::ChartWidget(parent);
   chart->setConnectionState(QStringLiteral("online"), QStringLiteral("服务在线"));
   chart->setModels(models);            // {provider, id, name, enabled, ...}
   chart->setCurrentModel(QStringLiteral("gridstar/gs-pro-32k"));
   chart->setHistory(historyMessages);  // 与后端 /sessions/{id} 的消息同构

   QObject::connect(chart, &gs::ChartWidget::sendMessage, chart,
                    [](const QString &text, const QString &display, const QVariantList &atts) {
                        // 宿主在这里发请求；流式回包时调用
                        // appendAssistantText / appendToolCall / appendToolResult / finishAssistant
                    });
   QObject::connect(chart, &gs::ChartWidget::approvalDecided, chart,
                    [](const QString &callId, bool approved, const QVariantMap &args) {
                        // POST 审批结果；成功后 chart->resolveApproval(callId, approved)
                        // 失败则 chart->reEnableApproval(callId)
                    });
   ```

3. 非 C++ 宿主可用 C 工厂：

   ```c
   QWidget *qtchartwidget_create(void);   // 返回 new gs::ChartWidget()
   const char *qtchartwidget_version(void); // "2.3.0"
   ```

### 公开 API 分组（include/chartwidget.h）

- 皮肤：`setTheme` / `theme`（`dark` / `silver` / `blue`，对应 webui 的右上角皮肤切换；
  切换后 QSS、自绘控件与已着色图标一起刷新）
- 顶栏/会话栏：`setConnectionState` / `setSessions` / `setCurrentSessionTitle`
- 视图页签：`setViewTab` / `viewTab`（`chat` / `traj`）
- 输入区：`setModels` / `setCurrentModel` / `setSkills` / `setCurrentSkill` /
  `setMode` / `setBusy` / `setConfigLoaded` / `setConfigWarning` / `setInputText`。
  交互模式与模型都是**触发器 + 下拉**（`modeTrigger` / `modelTrigger`，外层 `.modelControl`
  框），指针移入即展开、移开 150ms 收起（webui `bindHoverDropdown`），点击仍是开合切换；
  触发器聚焦时 Enter / Space / 方向键也能展开（webui 原生 button + `handleModelKeys`）；
  正文以 `/` 开头就弹出**斜杠面板**（`#slash-menu`）：根级是「指令」组（模型 / 导出对话）
  加按专业分类分组的技能（选具体分类时只列本分类 + common + 不启用技能），
  选「模型」会换成带搜索框的可筛选模型列表。选中技能后走 `skillSelected`，
  输入框上方出现可清除的技能芯片（`skillChip`）；面板里的「导出对话」发 `sessionExportRequested`
- 专业分类 / 知识库下拉（webui `#category-select` / `#kb-select`）：`setCategories` /
  `setCurrentCategory` / `currentCategory` / `syncCategoryFromSession` /
  `setKnowledgeBases` / `setCurrentKnowledgeBase` / `currentKnowledgeBase`，
  信号 `categorySelected` / `knowledgeBaseSelected`。知识库下拉自带
  「不使用知识库」（`__none__`）与「全部知识库」（`*`）两项；
  **五个浮层（模式 / 模型 / 分类 / 知识库 / 斜杠面板）互斥**，统一收在 `openDropdown()`
- 上下文用量圆环（webui `#context-ring`）：`setContextUsage(usage)` / `clearContextUsage()`，
  `usage` 形如 `{max, used, left, percent, estimated, model}`；悬停或聚焦时在上方弹浮层
  （上限 / 已用 / 剩余 / 进度条 / 「本地估算」小注），无数据时只留灰底环
- 附件：`addAttachments` / `clearAttachments` / `setVoiceEnabled` / `setVoiceRecording`
  （条目与 webui 同构：`{id, name, size, ext, kind, path, uploading}`；
  `kind=image` 且 `path`/`url` 指向本地文件时芯片显示 22×22 缩略图，其余显示扩展名）
- 选择浮层（模型提问 / 工具参数确认 / 审批共用输入框上方那张卡）：
  `showChoice` / `closeChoice` / `resolveApproval` / `reEnableApproval`；
  历史/结构化块里带 `options`、`tool_params` 时自动弹出。
  工具参数确认**没有独立信号**：按 webui 口径把参数与选择打包成
  `<structured_interaction>{...}</structured_interaction>` 一轮消息由 `sendMessage` 带出
  （display 为所选按钮文案），宿主按普通发消息流程处理即可
- 消息流：`setHistory` / `appendUserMessage` / `appendAssistantMessage` /
  `appendAssistantText` / `appendReasoning` / `appendToolCall` / `appendToolResult` /
  `appendApproval` / `appendWorkflowEvent` / `appendFailure` /
  `appendToolResultMessage` / `markCurrentStopped` / `finishAssistant` /
  `setPhasePlan` / `showToast` / `clearMessages`
- 用量与用时：`setTokenUsage(total, input, output, estimated)`（兼容旧签名）/
  `setTokenUsageDetail(usage)` / `setTurnTiming(timing)` / `startLiveTiming(startMs)`
- 轨迹视图：`setTrajectoryEvents` / `appendTrajectoryEvents`
- 设置中心：`setSettingsDraft` / `setDiscoveredModels` / `setProviderBusy` /
  `setSettingsSkills` / `setMcpTools` / `setUsageStats` / `setUsageLoadFailed` /
  `setModelMetadata` / `setModelMetadataFailed` / `openSettings` / `settingsSaved`
  （六个 Tab：模型 / 技能 / MCP 工具 / 记忆 / 知识库 / 用量），信号 `settingsOpened(tab)`
  会在设置中心被打开时告知当前 Tab（含用户点控件自带设置按钮那一路）
- 模型信息快照：`setModelMetadata(snapshot)` / `setModelMetadataFailed(kind, error, httpStatus)`，
  信号 `modelMetadataRefreshRequested()`。注意 `GET /config/model-metadata` 返回的
  `models`/`providers`/`hosts` 是**计数**，每个模型的上限与能力来自
  `POST /config/model-metadata/resolve`；宿主把 resolve 结果放进快照的 `limits` 键
  （或 `models` 数组）就能回填上限与能力（只填空字段、能力只正向勾选）
- 记忆页：`setMemorySummary` / `setMemoryExperiences` / `setMemoryListFailed` /
  `setMemoryTools` / `setMemoryStatusText`；信号 `memoryRefreshRequested` /
  `memoryListRequested(page, filters)` / `memoryConfigPatchRequested` /
  `memoryActionRequested(id, action)` / `memoryBulkRequested(ids, action)` /
  `memoryDetailRequested` / `memoryFieldSaveRequested` / `memoryFieldDeleteRequested` /
  `memoryToolClearRequested` / `memoryToolsClearAllRequested` /
  `memoryMaintenanceRequested(kind)`
- 知识库页：`setKbLibraries` / `setKbStatus` / `setKbFolders(folders, tree)` /
  `setKbDocuments(documents, total)` / `setKbSearchResult` / `setKbStatusText`；
  信号 `kbRefreshRequested` / `kbFolderChanged` / `kbLibraryCreateRequested` /
  `kbLibraryRenameRequested` / `kbLibraryDeleteRequested` / `kbFilesImportRequested` /
  `kbDirImportRequested` / `kbDocumentDeleteRequested` / `kbDocumentReindexRequested` /
  `kbDocumentMoveRequested` / `kbFolderCreateRequested` / `kbFolderRenameRequested` /
  `kbFolderDeleteRequested` / `kbSearchRequested`。
  **目录导入请接 `kbDirImportToFolderRequested(libraryId, dirPath, folderId)`**：
  它带当前选中的归档目录（空串 = 库根），KbPanel 现在只发这个；旧的
  `kbDirImportRequested` 保留声明但不再 emit
- provider 草稿里的 `ssl_verify` 支持三档（与 webui `data-provider-field="ssl_verify"` 同构）：
  `true`（默认，系统证书）/ `false`（跳过验证）/ 字符串（自定义 CA 证书路径，此时右侧多出
  「CA 证书路径」输入行，切回前两档时该行收起并记住上次路径）。
  `placeFormFields` 会跳过低可见字段，判据带父级（`field->parentWidget() && field->isHidden()`）——
  刚 `new` 出来还没挂父级的字段 `isHidden()` 也是真，只看它会把整张表单跳过
- 界面缩放：`zoomIn` / `zoomOut` / `zoomReset` / `zoomFactor`
  （快捷键 `Ctrl+=` / `Ctrl+-` / `Ctrl+0` 已内置于部件）
- 信号：`sendMessage`、`stopRequested`、`modeChanged`、`modelSelected`、`skillSelected`、
  `newSessionRequested`、`sessionSelected/Renamed/Cleared/Deleted`、`sessionExportRequested`、
  `connectionCheckRequested`、`optionChosen`、`approvalDecided`、
  `workflowRunRequested`、`retryRequested`、`trajectoryReloadRequested`、
  `viewTabChanged`、`themeChanged`、`choiceOpenChanged`、`settingsSaveRequested`、
  `testProviderRequested`、`readModelsRequested`、`refreshSkillsRequested`、
  `refreshMcpRequested`、`usageStatsRequested`、`attachRequested`、`voiceRequested`、
  `attachmentsAdded`、`attachmentRemoved`

### 主要数据形状（与 webui/app.js 同构）

- sessions：`{id, title, updated_at, created_at[, status]}`，`status` 取
  `running|done|stopped|error|waiting`（缺省时回退到服务端的 `waiting` / `active` 布尔字段）
- models：`{provider, id, name, enabled, provider_enabled}`，模型 key = `provider/id`
- skills：`{id, name, description[, version, source, allowed_tools]}`
- 历史消息：user `{role, content, display_content, attachments[{name, kind, url}], ts}`；
  assistant `{content, reasoning_content, tool_calls[{id, function{name, arguments}}],
  usage{total,input,output,estimated,cache_read,reasoning,model}, ts,
  elapsed_ms, think_ms, ttft_ms, tps, interrupted}`；
  tool `{tool_call_id, tool_name, content}`
- 用量明细 `setTokenUsageDetail`：`{total, input, output, estimated, cache_read, reasoning, model}`
  对应底部「用量」弹层的 缓存命中 / 未缓存输入 / 缓存读取 / 输出（含推理）各行。
  弹层的「提供方 / 模型」会按 `model`（形如 `provider/model`）到 `setModels` 里查
  `provider_name` 自动映射成供应商名称（与 app.js `usageModelLabel` 一致）；
  需要自定义文案时直接传 `model_label` 覆盖
- 用时明细 `setTurnTiming`：`{elapsed, think, ttft, tps}`（毫秒 / tok/s）
- 结构化块：正文中 ```` ```json ```` 围栏内含 `options / tool_params / workflow / phase_plan`
  任一键时按类型处理 —— `options`/`tool_params` 进输入框上方的选择浮层，
  `workflow` 渲染成工作流提案卡，`phase_plan` 进计划窗口；其余围栏按代码块原样显示
- 轨迹事件 `setTrajectoryEvents`：`{type, turn, request, step, ts, ...}`，`type` 取
  `user / traj_system_prompt / traj_context / traj_request_start / traj_request_end /
  tool_call / tool_result`，与后端 `/sessions/{id}/trajectory` 的 events 同构；
  时间轴两态投影（等宽操作 / 实际时长）、账本三种分组（平铺 / 轮次 / 调用）都在控件内部完成
- 用量统计 `setUsageStats(requestId, data)`：与后端 `GET /usage/stats` 的响应同构 ——
  `{range{start, end, resolution}, totals{total, input, output, measured, estimated,
  cache_read, cache_write, reasoning, turns, sessions}, buckets[{t, input, output, total,
  measured, estimated, turns}], providers[...], models[...], candidates{providers, models}}`。
  `buckets` 始终按小时（`t` 形如 `YYYY-MM-DDTHH`）；粒度选「按天」时由控件按自然日上卷，
  `resolution` 为 `day`（跨度 > 90 天）时「按小时」档位置灰。筛选项来自 `candidates`
  （只按时间窗过滤，不随当前筛选缩水）；显示名优先取 `setModels` 里的 `provider_name` / `name`
  请求以 `usageStatsRequested` 下发的 `requestId` 对账（回填时必须原样带回）：晚到的旧响应、
  旧失败都会被丢弃，不会覆盖当前视图；结果缓存与 app.js 同口径，满 32 条整体清空

### 视觉口径（对齐 webui/refined 精修层）

- **调色板**：三套皮肤都改成中性冷黑底（`--bg #0d1014`），边界一律用低透明描边
  （`--line: rgba(255,255,255,.055)`），强调色只出现在可交互焦点上（`--cyan #6fb0cc`）；
  发送键是全站唯一亮色块
- **字体层级**：全站微软雅黑，字重只有 400 / 700 两档（雅黑只能这两档）；
  正文 15px / 表头 11px / 单元格 13px；标题 h1 19 / h2 17 / h3 15 / h4~h6 14
- **主栏宽度**：`clamp(680px, 86vw, 1320px)`（`gs::contentMaxWidth()`）。
  消息区 / 输入区 / 阶段条 / 问答卡都居中到这一条竖线；窗口窄到这个下限以下时满宽显示
- **骨架**：顶栏 52px；会话栏 44px，视图切换（对话 / 轨迹）是它右侧的胶囊，不再单独占一行
- **卡片**：消息卡实底 14px 圆角；用户卡 `min(84%, 620px)` 靠右并切掉右下角；
  输入框 16px 圆角 + 深投影；浮层统一玻璃底 + 12px 圆角
- **表格**：去外框、去竖线、表头无底色，只留行线；首列用 `--text-bright` 强调
- **问答卡**：底边贴输入区上方 14px，高度上限到宿主顶边上方 8px；
  参数超 6 项时只展开必填与前几项，其余收成「展开其余 N 个参数」

### 其它行为口径（与 webui 对齐）

- 消息区**贴底才跟随**：流式期间用户上滚查看历史时不再被新分片拽回底部（底部余量 24px）
- 一轮一张卡：用户 / 助手卡片不再显示身份行与 Skill 名，角色靠左右对齐 + 渐变浮层区分；
  卡片底部信息行左侧为复制按钮，右侧依次是用量、用时、收到时刻
- 思考与工具调用收在卡内「过程区」：运行态只把折叠箭头换成呼吸点，位置与高度不变；
  工具调用展开后参数以两列表格呈现、结果按 JSON 缩进美化
- 轮次导航轨：页面最左边缘竖排白点，一轮一个；鼠标移入轨道即向右展开整轮列表面板
  （「对话轮次 · N 轮」头 + 序号徽标 + 提问摘要，一行一轮），点击行或白点都跳转到该轮，
  当前轮次在点与行上同步高亮；轨道与面板各自算悬停区，跨间隙留 180ms 延迟收起，
  点击行会一并收起面板（与 `app.js` 一致）。行摘要在正文回填之后再就地刷新，
  不会因为 `createMessage` 收尾时的那次重建而停在占位文案
- 设置中心「用量」页（`#panel-usage`，与 webui 第四个 Tab 对齐）：
  - 筛选：分组（模型 / 供应商）、供应商、模型、粒度（按天 / 按小时）、日期（预设 + 双月日历）；
    选项来自 `candidates`（只列有调用记录的项），已选项即使落在时间窗外也不静默清空
  - 概览卡六格：总用量 / 输入 / 输出 / 实测 · 估算 / 缓存命中率 / 会话数，
    缓存命中率 = `cache_read / (cache_read + input)`
  - **数值一律以百万（M）显示**：不足 1M 也写小数（`0.5M` / `0.075M`），位数按数量级补足，
    不退回 K；只有 token 类列这样显示，轮次按原值（模型设置页的上下文窗口仍是 K/M 自适应）
  - 折线（输入 / 输出）与堆叠柱（实测 / 估算）共享同一时间视窗，缩放平移联动；
    可见跨度不足 3 个桶时下钻到小时桶（`resolution` 为 `day` 时没有小时桶可下钻）
  - 环形图：分组为模型时取前 6 个 + 「其他 N 个」；选中单个模型时改看该模型的 token 构成
  - 明细表：列随分组切换，点表头排序（数值列默认降序），名称列 toolTip 给出完整键
  - 请求按 `(start, end, provider, model)` 在库内缓存 32 条；每次发起都换新的
    `requestId`，晚到的旧响应 / 旧失败按 `requestId` 丢弃
- 计划窗口只服务执行过程：本轮结束时计划全部完成就收起（`finishAssistantInternal` 一进来就
  结算，空轮兜底那条早退路径也要过），收起时连面板内容一起清；下一条 `plan_updated`
  再亮出来。选择浮层展开时自动上移让位。阶段指示器是圆点（当前项换成 1.5px 开口环旋转），
  标题右侧的统计串按「N 已完成 · N 进行中 · N 待处理」分段着色，宽度不够时从尾部丢段
- 斜杠面板展开时会接管输入框的上下键与回车（`Escape` 关、`Enter` 选中当前行），
  所以它开着时回车不会把正文以 `/` 发出去
- 排版细节（QSS 表达不了、库内用代码补）：
  - **单行省略**：轨迹行进账本与分组头用自绘 `ElidedLabel`（`sizeHint` 按全文、
    `minimumSizeHint` 为 0），省略后原文仍保留在 toolTip
  - **`letter-spacing`**：QSS 无此属性，由部件在 `QEvent::Polish` 时按 class / objectName
    设 `QFont::AbsoluteSpacing`（表见 `theme.cpp` 的 `letterSpacingBase`），随缩放系数重算。
    设置对话框是独立顶级窗口，里面不少容器是**先无父创建、之后才挂进来**的
    （`providerSidebar` / `providerEditor` / `skillsPanel` …）——它们作为临时顶级窗口时就已经
    被 polish 过，之后收不到该事件，所以 `openSettings()` 会整棵子树再补一次
  - **卡片悬浮显形复制按钮**：`.turn-card:hover .bubble-copy` 在 Qt 里不成立（父 `:hover`
    不驱动子选择器），改由 `enterEvent/leaveEvent` 调 opacity（常态 .5 → 悬浮 1）
- 动效（webui 的 CSS 动画 → `QPropertyAnimation`）：选择浮层入场 180ms / 离场 240ms 淡出
  （离场时长对齐 webui `app.js:842`，过渡结束才隐藏并清空内容）、
  消息卡入场 180ms 淡入（结束后卸掉 opacity 效果，避免长期离屏渲染）、
  计划进度条 450ms `OutCubic` 过渡；呼吸点（badgePulse / phasePulse）由 QTimer 驱动。
  浮层的 opacity 效果常驻但**只在动画期间 `setEnabled(true)`**，跑完即停用，否则这张卡每帧
  都要多一次离屏合成
- **键盘可达性**（webui 里这些控件都是原生 `button` / `tabindex="0"`，Qt 默认不是）：
  按钮 / 页签 / 下拉框在**创建时就设 `Qt::TabFocus`**（`Tab` 能到达，又不会像 `StrongFocus`
  那样鼠标点完留一圈焦点环——webui 用的是 `:focus-visible`，Qt 没有这个伪态，
  库内统一用 `TabFocus` 近似，polish 时还会再收敛一次）；
  自绘的可点区域（过程行标题、工具项标题、计划窗头、选择项、轨迹行 / 分组 / 摘要、
  `.session-select`）同样 `TabFocus`，并在 `Enter` / `Space` 上等价于点击；
  QSS 里给这些控件补 `:focus` 焦点环（Qt 没有浏览器那样的默认 outline）；
  无文本按钮的 `accessibleName` 取 `toolTip`（对应 webui 的 `aria-label`）。
  焦点策略在创建处就设好，不依赖 polish 时机（设置对话框那次补救见上一节）
- 轨迹详情面板响应式：宽度 >760 用 380px、760~560 用 300px、<560 脱离布局改 88vw 覆盖浮层
  （对应 webui 的两个 `@media` 断点）
- 未做：`backdrop-filter` 毛玻璃（Qt 无该类 API，只能自绘背景快照 + 模糊，见下）
- 样式表完整性（根因修复）：Qt QSS 遇到它不认识的选择器（CSS3 的 `:not(...)`、`:!state`）时，
  **不只是跳过那一条，而是丢弃其后所有规则**。`#choiceSubmit:hover:not(:disabled)` 恰好落在中段，
  于是它之后的半张样式表（设置中心、用量页、各浮层、状态栏按钮…）长时间静默失效 —— 表现为这些
  区域的控件落回平台原生样式（浅色、原生边框）。修复：把三处不受支持的选择器改成 Qt 语法
  （`:hover:not(:disabled)` 与 `:hover:!disabled` → `:hover:enabled`；`trajViewButton:hover:not([active="true"])`
  → `:hover`，并把 `[active="true"]` 规则同时挂上 `:hover` 以提高优先级）。测试 `appStyleSheetParsesFully`
  静态拦截 `:not(` / `:!`，并在样式表末尾追加探针规则动态复核，防止再退化。
- 浮层样式：用量页的两个浮层（筛选项下拉、日期区间面板）是 `Qt::Popup` 子窗口，以 ChartWidget
  为父窗口创建，**直接继承其样式表**（与既有模型 / 皮肤下拉一致），故上面这条修好后即恢复主题皮肤。
  注意别把它们先挂到设置对话框、事后再 `setParent` 重挂 —— Qt 会把样式重置回原生样式。
- 与 webui 逐像素核对：仓库根上的 `_qt_visual/` 是一台独立小工作台（自建 .pro，用示例数据
  把聊天 / 模式下拉 / 斜杠面板 / 计划面板抓成 PNG），`_qt_visual/qt_visual.pro` 的
  `INCLUDEPATH` / `LIBS` 指向本库的 `include` / `bin`，产物也落在 `QtChartWidget/bin`。
  **必须用 `D:\Application\Qt\Qt5.12.2\5.12.2\msvc2017_64\bin\qmake.exe` 显式生成 Makefile**：
  PATH 里 Anaconda 的 `qmake` 会链接到 `Qt5Widgets_conda.dll`，两套 Qt 并存时表现为
  `QWidget: Must construct a QApplication before a QWidget` 直接崩掉（qApp 认不出来）。
  下拉浮层是独立的 `Qt::Popup` 顶层窗口，`chart.grab()` 抓不到，得遍历
  `QApplication::topLevelWidgets()` 找可见的 Popup，按 `mapToGlobal` 差值贴回主图。
  这台工作台查出过两个真缺陷：`makeLabel` 从不设 `objectName`（全仓 66 处标签按名查不到，
  已按类名首段补上）、技能芯片被压成 22px（`QPushButton` 最小宽取自自己的空 text，
  已改成按名字实测最小宽，`refreshZoom` 负责随缩放重算，`skillChipSizedToName` 钉住）

### 性能口径

`tests` 里的 `renderPerformance` 会把关键数字打到日志（`qInfo`），断言只压在**库自己的代码**上
（全量重建一次账本约 230ms，一旦退回「每个事件重建一次」立刻会被拦住）。
1500 条记录的账本下（`调用` = 库自己的代码，`事件循环` = Qt 的布局/重绘）：

| 场景 | 最初 | 现在 |
| --- | --- | --- |
| 轨迹账本首次加载 1500 条 | 建控件 229ms + 首次布局约 3.3s | **43ms**（只建可视区那几十行） |
| 实时追加 1 条（平铺） | 212ms（整账本重建） | 调用 **4.2ms** + 事件循环 4.3ms |
| 实时追加 1 条（轮次·续在最后一组） | 336ms | 调用 **5ms** + 事件循环 3ms |
| 实时追加 1 条（轮次·新起一轮） | 336ms | 调用 **4.8ms** + 事件循环 3ms |
| 滚一屏 | 全量重排（与行数成正比） | **21ms**（换掉进入视口的那几十行） |
| 逐字搜索 8 个字符 | 8 次整账本重建 | **0ms**（200ms 去抖，只重建 1 次） |
| 点一行看详情 | 整账本重建（约 230ms） | **1ms**（只挪高亮 + 刷详情面板） |
| 流式 400 分片（约 16KB 正文） | — | 150ms（0.37ms/分片，未见卡顿） |

做了什么：

- **轨迹账本视窗化（只构建可视区行）**：条目列表是纯数据（分组头 / 数据行 / 收起摘要 / 空占位），
  控件只为「视口 + 上下 240px 余量」那几十行实例化，其余用上下两个 spacer 占位。
  行高按条目种类实测并缓存（缩放变更会自动重测），滚动位置用「前缀高度 + 二分」换算，
  所以「滚到第 N 条」不需要那一行存在
- **选中不再重建**：`selectRecord` 只把 `selected` 描边从旧行挪到新行（行不在可视区就只记状态，
  滚回来时按状态重建）+ 刷新详情面板
- **搜索去抖 200ms**：本地账本几千行时逐字重建会把输入卡住
- **增量追加**：`appendTrajectoryEvents` 只把新增记录换算成条目并补建可视区内的行，
  分组头的「N 条 · X tok · 耗时」就地刷新。以下情况退回全量重建（保证与 `setTrajectoryEvents`
  逐行一致，`trajectoryViewTabSwitch` 用「结构快照」对比两边）：带查询/框选过滤、末尾分组已收起、
  本批含系统提示词、事件乱序（新记录落在前面某个已有分组里——分组按「连续段」归并）
- 时间轴仍是单块自绘画布，重算只是纯计算，所以保持全量重算

换取这些的代价（都写进了 `trajectoryLedgerVirtualization` 用例守着）：

- **消息区（对话）没有虚拟化**：它是「一轮一张卡」，量级是轮数（几十~几百）而不是记录数，
  且只在 `setHistory` / 新消息时重建，代价可控；真需要时再按同样的思路做
- 滚动会重建约 40 行控件（21ms/屏）——这是「不再有秒级首次布局」的交换
- 行高依赖「同类条目高度一致」（单行文本、不换行）：新增行样式时若高度会变，实测机制会自动跟上

### 宿主职责（这些在 webui 里由前端完成，库不做）

- 全部网络：`/sessions`、`/chat/stream`（SSE）、`/upload`、`/asr`、`/config`、
  `/workflows/run`、`/sessions/{id}/trajectory`、`/usage/stats`、
  `POST /sessions/{id}/export`（斜杠面板的「导出对话」→ `sessionExportRequested`，
  成功提示 `已导出：<path>`、失败 `导出失败：<error>`）等；库只推数据、收交互
- `GET /sessions` 响应里的 `storage_issues`（会话目录不可访问 / 索引不可读 / 索引损坏）
  也由宿主提示，同一条只弹一次（webui `warnStorageIssues`；库侧 `setSessions` 只收会话数组，
  不接这个字段）
- 附件：数量/类型/大小校验（webui 为 ≤6 个、≤10MB、≤4 张图）与上传，再把结果 `addAttachments`
- 语音：录音、重采样到 16kHz、WAV 编码与 `POST /asr`，再把识别文本 `setInputText`；
  录音时长上限也在宿主侧（webui `app.js:1862,1906` 为 5 分钟，到点提示并自动停止）——
  库只按 `setVoiceRecording` 切换按钮形态，不掐表
- 会话重命名 / 清空 / 删除的**二次确认**：库只发 `sessionRenamed/Cleared/Deleted`，
  确认对话框由宿主自己弹（webui 在前端用 `showDialog`；库不管会话业务）
- 会话状态徽标的数据来源：`setSessions` 里每条的 `status`
  （`running|done|stopped|error|waiting`）；浮层带来的「待确认」可用 `choiceOpenChanged` 驱动
- 后台续跑 / 断线重连 / 停止时的 `POST /sessions/{id}/cancel`
- 皮肤持久化：webui 存 localStorage，宿主自行落盘并在启动时 `setTheme`
- 轨迹：收到 `trajectoryReloadRequested` 后拉 `/trajectory` 并 `setTrajectoryEvents`
- 专业分类（输入区）：用户改分类发 `categorySelected`，宿主把它存成当前会话的
  `meta.category` 镜像，并在下一轮 `/chat/stream` 的 body 里带 `selected_category` +
  `category_explicit=true`（见下面「切会话要推的三样」）。分类目录本身不是接口，
  webui 是把 `/skills` 的 `category` 去重后拼出来的（demo 的 `categoriesFromSkills`）
- 知识库（输入区）：`knowledgeBaseSelected` 的值为 `__none__`（不用）/ `*`（跨库）/ 具体库 id，
  宿主在发起本轮时带上 `kb_library`；**后端用空串表示“不使用知识库”，
  所以 `__none__` 必须映射成空串再发**，`*` 原样发
- 切会话要推的三样（webui `loadSession`）：
  1. **`meta.category`** —— 用 `syncCategoryFromSession(meta.category)` 回填（它无条件覆盖，
     对应 app.js `syncCategoryFromMeta`），同时清掉宿主自己的“用户显式选过”标记；
  2. **`meta.kb_library`** —— `setCurrentKnowledgeBase()`，注意把后端的空串映射回 `__none__`
     （`null`/缺失按 `*` 处理）；
  3. **上下文用量圆环** —— 从历史里**最后一条**带 `usage.context_used` 的消息恢复
     `setContextUsage({used, max, left, percent, model})`，一条都没有就 `clearContextUsage()`
     （对应 app.js `restoreContextRing`）。`max` 取当前模型的 `context_window`
- 顶栏品牌图标：`launchAppRequested` → 宿主弹确认并 `POST /gridstar/ensure`，
  成功提示 `正在启动 GridStar…`、已在跑则提示 `GridStar 已在运行`
- 「结束」类选项：`endTurnRequested(label)` → 宿主 `POST /sessions/{id}/end-turn` 并落一条
  用户消息；库侧已在本地收尾（关浮层、标停止、藏计划面板、追加用户消息），不开新一轮
- 记忆页 / 知识库页：共二十多条信号，逐条对照见下面两张表
- 模型信息快照：`modelMetadataRefreshRequested` → `POST /config/model-metadata/refresh`
  后回填；失败用 `setModelMetadataFailed("refresh", msg, httpStatus)`。
  `modelLimitsResolveRequested(provider, model)` → `POST /config/model-metadata/resolve`
  （body `{models:[{id, base_url}]}`），把返回的 `limits` **并进上一次的快照**再
  `setModelMetadata`（resolve 只给 `{limits:{...}}`，直接拿去覆盖会把状态行顶掉）。
  注意：`GET /config/model-metadata` 没有对应信号（“打开设置中心”库不发通知），
  宿主应在每次自己打开设置前拉一次

#### 记忆页：信号 → 请求对照（`/memory/*`）

| 信号 | 宿主发什么请求 | 回填 |
| --- | --- | --- |
| `memoryRefreshRequested()` | 并发 `GET /memory/summary` + `GET /memory/tool-parameters` | `setMemorySummary` / `setMemoryTools` |
| `memoryListRequested(page, filters)` | `GET /memory/experiences?status=&kind=&scope=&q=&page=&page_size=`（空值不下发；`filters` 里已带 `page_size`） | `setMemoryExperiences`，失败 `setMemoryListFailed` |
| `memoryConfigPatchRequested(updates)` | `PATCH /memory/config`，body 就是 `updates` | `setMemorySummary`（+ 重新 `GET /config` 记下新 revision） |
| `memoryActionRequested(id, action)` | `PATCH /memory/experiences/{id}`，body `{action}`（409 = 状态已变） | 重拉列表 + summary |
| `memoryBulkRequested(ids, action)` | `POST /memory/experiences/bulk`，body `{ids, action}` | 同上 |
| `memoryDetailRequested(id)` | `GET /memory/experiences/{id}`（demo 直接从列表里的 `payload` 弹框，省一次请求） | 宿主自弹详情 |
| `memoryFieldSaveRequested(tool, field, value)` | `PATCH /memory/tool-parameters/{tool}/{field}`，body `{value}`（只改值，不新增字段） | 重拉 `GET /memory/tool-parameters` → `setMemoryTools` |
| `memoryFieldDeleteRequested(tool, field)` | `DELETE /memory/tool-parameters/{tool}/{field}`（204） | 同上 |
| `memoryToolClearRequested(tool)` | `DELETE /memory/tool-parameters/{tool}`（204） | 同上 |
| `memoryToolsClearAllRequested()` | `DELETE /memory/tool-parameters` | 同上 |
| `memoryMaintenanceRequested(kind)` | `POST /memory/maintenance/runs`，body `{kind}`（`extract`/`governance`，返回 202） | 重拉 summary；进度靠面板自己的轮询（它会反复发 `memoryRefreshRequested`） |

#### 知识库页：信号 → 请求对照（`/kb/*`）

| 信号 | 宿主发什么请求 | 回填 |
| --- | --- | --- |
| `kbRefreshRequested(libraryId)` | `GET /kb/libraries`，再 `GET /kb/status?library_id=`，最后并发 `GET /kb/folders?library_id=` / `GET /kb/documents?library_id=&folder=` | `setKbLibraries` / `setKbStatus` / `setKbFolders(folders, tree)` / `setKbDocuments(docs, status.documents)` |
| `kbFolderChanged(libraryId, folderId)` | **只**重拉 `GET /kb/documents?library_id=&folder=` | `setKbDocuments` |
| `kbLibraryCreateRequested(name)` | `POST /kb/libraries` `{name}` | `setKbLibraries`（响应里的 `library.id` 就是新库，面板会自己选过去） |
| `kbLibraryRenameRequested(id, name)` | `PATCH /kb/libraries/{id}` `{name}` | `setKbLibraries` |
| `kbLibraryDeleteRequested(id)` | `DELETE /kb/libraries/{id}` | `setKbLibraries` |
| `kbFilesImportRequested(paths, lib, folder)` | **逐份** `POST /kb/upload?name=&library_id=&folder_id=`（正文 = 文件字节） | 全部完成后重拉 documents + libraries |
| `kbDirImportRequested(lib, dir)` | `POST /kb/import`，body `{paths:[dir], recursive:true, mirror_tree:true, library_id}` | 完成后重拉 folders + documents |
| `kbDocumentDeleteRequested(docId)` | `DELETE /kb/documents/{id}` | 重拉 |
| `kbDocumentReindexRequested(docId)` | `POST /kb/documents/{id}/reindex` | 重拉 |
| `kbDocumentMoveRequested(docId, folderId)` | `PATCH /kb/documents/{id}` `{folder_id}`（空串 = 库根） | 重拉 folders + documents |
| `kbFolderCreateRequested(lib, parent, name)` | `POST /kb/folders` `{library_id, name, parent_id}` | 重拉 folders |
| `kbFolderRenameRequested(folderId, name)` | `PATCH /kb/folders/{id}` `{name}` | 重拉 folders + documents |
| `kbFolderDeleteRequested(folderId)` | `DELETE /kb/folders/{id}`（里面的资料上移一层，不删文件） | 重拉，并把当前目录收回 `all` |
| `kbSearchRequested(lib, q)` | `GET /kb/search?q=&top_k=6&library_id=` | `setKbSearchResult` |

两个容易踩的点：

- **`/kb/import` 与 `/kb/upload` 不是一回事**。`/kb/import` 收的是**后端机器上的本机路径**
  （JSON 里的 `paths`，后端自己递归扫描 + 建索引），适合目录导入；`/kb/upload` 收的是
  **文件字节**（`Content-Type: application/octet-stream`，文件名与目标库/目录走 query），
  适合宿主传本地文件。后端**不支持 multipart 表单**，别按表单发
- `folder` 参数的三种取值：`all` → **不传** `folder`（后端列全库）；`root` → 传 `folder=`
  （空串，只列库根）；其余 → 传真实文件夹 id。「全部资料」那行的数字来自 `status.documents`，
  所以 `setKbDocuments` 的第二个参数要给库级总量，不是当前列表长度
- 上传进度：`/kb/upload` 是整份 POST，没有分片，宿主只能在状态行上报「第 N/M 份」；
  单文件上限 64 MB（后端 `KB_MAX_FILE_BYTES`）。demo 就是这样处理的，没做百分比进度条

#### 最小接线：分类 + 知识库 + 上下文圆环

```cpp
// 宿主侧状态（对应 webui state.selectedCategory / categoryTouched / kbState.selected）
QString category;            // 空串 = 全部
bool categoryTouched = false;
QString kbLibrary = QStringLiteral("*");   // __none__ / * / 具体库 id

// 1) 两个下拉：只记在宿主侧（分类与知识库都没有独立的 meta 写接口）
connect(chart, &ChartWidget::categorySelected, chart, [&](const QString &id) {
    category = id;
    categoryTouched = true;   // 用户自己点的，等价 webui categoryTouched
});
connect(chart, &ChartWidget::knowledgeBaseSelected, chart, [&](const QString &id) {
    kbLibrary = id;
});

// 2) 发消息时带出去（POST /chat/stream 的 body）
QJsonObject body;
body.insert(QStringLiteral("selected_category"), category);
body.insert(QStringLiteral("category_explicit"), categoryTouched);
body.insert(QStringLiteral("kb_library"),
            kbLibrary == QLatin1String("__none__") ? QString() : kbLibrary);

// 3) 切会话：三样一起回填（meta / messages 来自 GET /sessions/{id}）
categoryTouched = false;
category = meta.value(QStringLiteral("category")).toString();
chart->syncCategoryFromSession(category);          // 无条件覆盖
const QString kb = meta.value(QStringLiteral("kb_library")).toString();
kbLibrary = kb.isEmpty() ? QStringLiteral("__none__") : kb;   // 后端空串 = 不使用
chart->setCurrentKnowledgeBase(kbLibrary);

// 4) 圆环恢复：历史里最后一条带 usage.context_used 的消息，一条都没有就清空
double used = 0.0;
for (int i = messages.size() - 1; i >= 0; --i) {
    const QVariantMap usage = messages.at(i).toMap().value("usage").toMap();
    if (usage.value("context_used").toDouble() > 0.0) {
        used = usage.value("context_used").toDouble();
        break;
    }
}
if (used <= 0.0) {
    chart->clearContextUsage();
} else {
    const qint64 window = 32768;            // 当前模型配置的 context_window
    chart->setContextUsage(QVariantMap{
        { QStringLiteral("max"), window },
        { QStringLiteral("used"), qint64(used) },
        { QStringLiteral("left"), qMax<qint64>(0, window - qint64(used)) },
        { QStringLiteral("percent"), int(qRound(used / double(window) * 100.0)) },
        { QStringLiteral("estimated"), false },
        { QStringLiteral("model"), chart->currentModel() },
    });
}
```

一轮聊完之后同理：`done` / `token_usage` 事件里就带着 `context_used`（最近一次请求实际
送出的 input，**含缓存读**，不是本轮累计）、`context_window`、`context_estimated`，直接拿它们
组装上面那个 map 调 `setContextUsage` 即可（demo 的 `applyContextUsage` 就是这三行）。

#### 接线现状

上面两张表里所有信号都已经打通：`include/chartwidget.h` 声明 → `SettingsDialog` emit →
`ChartWidget` 转发，三段齐全（记忆页 11 个、知识库页 14 个，共 24 条 `connect`）。
如果你在更早的 2.3.0 构建上看到记忆页 / 知识库页點刷新没反应，那是转发还没补的时候；
重新构建一次即可。

### 线程模型

**所有公开 API 都必须在 GUI 线程调用。** 库内部直接操作 `QWidget` 与 QSS（换肤、缩放、
布局、动画都是同步改部件状态），而 Qt 的部件体系没有跨线程保护——在工作线程里调用不会报错，
只会得到难查的内存 / 绘制损坏。

宿主在 HTTP 响应或 SSE 回调里拿到数据时，请先切回 GUI 线程再推：

```cpp
// 宿主侧（Qt 5.12，没有 Qt::connect 的简单 lambda 重载就用 invokeMethod）
QNetworkReply *reply = manager.get(request);
QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
    const QVariantList events = parseTrajectory(reply->readAll());
    QMetaObject::invokeMethod(m_chart, "setTrajectoryEvents", Qt::QueuedConnection,
                              Q_ARG(QVariantList, events));
    reply->deleteLater();
});
```

「消息流 / 轨迹 / 会话 / 设置」这类**数据推送入口**自带运行期校验
（`assertGuiThread`，见 `src/chartwidget.cpp`）：非 GUI 线程调用会被忽略并 `qWarning`
打出线程 id（开发构建下直接 `Q_ASSERT` 中断）。被校验的入口包括
`setSessions` / `setHistory` / `clearMessages` / `appendUserMessage` / `appendAssistantMessage` /
`appendAssistantText` / `appendReasoning` / `finishAssistant` / `appendToolCall` /
`appendToolResult` / `appendApproval` / `appendWorkflowEvent` / `setTokenUsage*` /
`setTurnTiming` / `startLiveTiming` / `appendFailure` / `setPhasePlan` / `showChoice` /
`showToast` / `setTrajectoryEvents` / `appendTrajectoryEvents` / `setSettingsDraft` /
`setDiscoveredModels` / `setProviderBusy` / `setSettingsSkills` / `setMcpTools` /
`setUsageStats` / `setUsageLoadFailed` /
`setSettingsStatus` / `setConnectionState` / `setCurrentSessionTitle`。

信号是跨线程安全的（Qt 默认 `AutoConnection` 会自动排队），宿主可以放心从工作线程 `emit`
自己那边的信号，但**不要**在工作线程里直接调库的方法。

## 6. 测试

`tests/` 是一组基于 QtTest 的公开 API 回归测试。它**只通过 `include/chartwidget.h`** 驱动，
再用 QObject 树（`objectName` 与 `class` 动态属性）观察渲染结果——内部类不导出，
所以这组测试同时也在守「公开契约」这条边界。

```powershell
build.bat test                                   # 构建并运行全部用例
bin\qtchartwidget_tests.exe -functions           # 列出用例
bin\qtchartwidget_tests.exe optionsOverlayFreeText   # 只跑一个用例
bin\qtchartwidget_tests.exe -v2                   # 打印每条断言
bin\qtchartwidget_tests.exe -o report.txt,txt     # 报告写文件（CI 用）
```

测试进程自行设置 `QT_QPA_PLATFORM=offscreen`，不需要显示器；也不需要把 Qt 的 `bin`
加进 PATH（`QtChartWidget.dll` 与测试同目录）。

覆盖范围（72 项，含 init/cleanup）：

| 分组 | 用例 |
| --- | --- |
| 皮肤 / 缩放 | `themeSwitchAndSignal`（含未知皮肤回退、三套皮肤的 QSS 确实重设）、`appStyleSheetParsesFully`（整张样式表被 Qt 完整解析：无 `:not(` / `:!` 等不受支持的选择器，末尾追加的探针规则仍命中）、`zoomSteps`（档位步进与封顶） |
| 输入区 | `modeModelSkillSignals`、`dropdownHoverOpens`（模型 / 模式下拉悬停展开、点击 / 键盘开合）、`slashMenuClosesOnOutsideClick`（斜杠面板点外面收起、点输入框保留）、`inputSendRoundTrip`（发送态 / 信号 / 清空）、`attachmentChipPreview`（图片缩略图 / 扩展名 / 上传中文案）、`skillChipSizedToName`（芯片高 28、宽度按名字实测、清空技能后整块隐藏）、`inputAutoGrowOnResize`（宽度变了高度跟着重算） |
| 历史渲染 | `historyMergesTurnWithUsageAndTiming`（一轮一张卡 + 用量/用时/时刻）、`historyToolResultBackfill`（结果回填、摘要、参数表）、`expandKeepsScrollPosition`（展开工具项后重判贴底，流式分片不抢滚动） |
| 选择浮层 | `optionsOverlayChooseAndEsc`（点选项即确认 + Esc 收起）、`optionsOverlayFreeText`（「其他」自由作答、空文本拦截）、`toolParamsOverlaySubmit`（参数回填成结构化消息）、`approvalOverlayRoundTrip`（批准回执 + 宿主收卡） |
| 卡片 | `workflowProposalRun`、`phasePlanCollapsesWhenComplete`（跑完收起 / 执行中不收起）、`phasePlanSurvivesViewTabSwitch`（切轨迹再切回仍恢复） |
| 轨迹 / 会话 | `trajectoryViewTabSwitch`（页签互斥 + 账本渲染）、`trajectoryInspectorResponsive`（380/300/88vw 三档）、`sessionBadgeStatus`（状态徽标含布尔回退） |
| 轮次轨 / 用量 | `turnRailPanelHover`（移入轨道展开整轮列表、行序与当前轮高亮、点行/Enter/Space 跳转并收起、离开 180ms 延迟收起、行不越出面板）、`usagePanelTab`（切 Tab 才请求、默认筛选、M 单位概览、三图渲染、明细表显示名与排序、轮次不换算成 M）、`usageStaleResponseIgnored`（同区间刷新换 requestId；晚到的旧响应/旧失败不覆盖当前视图） |
| 排版与动效 | `letterSpacingApplied`（Polish 时补字距、其他控件不受影响）、`trajectoryRowElides`（省略并保留全文）、`hoverRevealsCopyButton`（悬浮 .5→1）、`processAndToolHoverAccent`（过程摘要 / 工具名悬停转青，运行态橙色优先）、`overlayAndCardTransitions`（浮层淡入淡出 + 卡片一次性 effect） |
| 工具项与口径 | `toolArgsTableAndResultFormat`（参数表、JSON 美化、失败判定、限高滚动容器）、`askUserToolCallIsNotRendered`（询问类调用不落成工具条目）、`usageModelLabelMapping`（用量弹层自动映射供应商名称） |
| 嵌入作用域 | `escScopedToOwnWidget`（宿主窗口的 Esc 不被吞、也不误关浮层）、`zoomShortcutScopedToWidget`（快捷键限定 WidgetWithChildren） |
| 窄宿主 / 浮层布局 | `overlayGeometryMatchesWebui`（浮层几何与 webui 对齐）、`choiceOverlayFitsNarrowWindow` / `choiceOverlayShowsAboveComposer`、`openOptionsOverlay`、`settingsDialogCompactLayout` / `confirmDialogFitsNarrowSettings`（设置中心窄屏单列、确认框不越界）、`usagePopupsFitNarrowHost`、`sessionPanelReflowsWhileOpen`、`attachChipFitsNarrowWindow` / `bubbleAttachmentThumbFitsNarrowWindow`、`bubbleDetailPopupFitsNarrowHost` / `bubbleDetailPopupAnchoring`、`toastFitsWrappedText`、`markdownTableSizesToContent` |
| 可行超 / 线程 | `keyboardReachability`（图标按钮可 Tab + `accessibleName` 取 toolTip、过程行 Enter 展开、`.session-select` 键盘选中）、`guiThreadGuard`（非 GUI 线程推送被丢弃，仅发布构建有效） |
| 输入区新件 | `contextRingUsage`（圆环与浮层：百分比 / 上限 / 已用 / 剩余 / 本地估算，空态全变「—」）、`categoryDropdown`（选中发信号；`syncCategoryFromSession` 无条件覆盖）、`knowledgeBaseDropdown`（内置「不使用 / 全部」与各库编号）、`dropdownsMutuallyExclusive`（模式→模型、分类→知识库两组互斥）、`brandMarkLaunchesApp`（品牌图标是按钮、点击发 `launchAppRequested`） |
| 记忆页 | `memoryPanelStatus`（正常 / 降级 / 关闭三态的文案与 tone，notice 追加）、`memoryPanelSwitchesAndFilters`（三开关联动禁用、工具栏变化回 page=1 且空值键不下发、搜索 300ms 去抖）、`memoryPanelBulkAndCards`（批量条只在有 candidate 时出现、本页全选、卡片动作穿二次确认、加载更多）、`memoryPanelToolsEditing`（字段编辑、类型校验：原值布尔填 `abc` 不发信号且标 invalid） |
| 知识库页 | `kbPanelTreeAndDocs`（归档树层级与选中态、切目录发 `kbFolderChanged`、资料行重建 / 删除）、`kbPanelSearch`（回车发请求、参数命中与文本命中两块） |
| 审批与结束语义 | `jsonTableEditor`（参数旁的「表格」按钮 → 对话框 → 改单元格 → 回写可解析的 JSON）、`endTurnOptionChosen`（命中收尾措辞只发 `endTurnRequested`；含「继续」一票否决）、`historyResumeSuppressesAsk`（`setHistory(msg, true)` 不弹已答询问） |
| 模型元数据 | `modelMetadataSignals`（「更新模型信息」发刷新；新增模型发 `modelLimitsResolveRequested` 并带对 provider / id）、`settingsOpenedNotified`（打开设置中心告知当前 Tab）、`kbDirImportCarriesFolder`（目录导入带当前归档目录） |
| 性能 | `renderPerformance`（长会话下的流式分片 / 轨迹首次建账本 / 增量追加 / 逐字搜索 / 滚一屏 / 选中一行：数字打日志，断言拦「退回全量重建」）、`trajectoryLedgerVirtualization`（控件数与记录数解耦、滚动换行、选中态跨窗口保留、滚动到底仍能命中） |

约定：新增公开 API 或改动其可观察行为时，同步补/改用例；测试断言的是**行为**（信号、文本、
可见性、字体字距、effect 状态），不是像素，所以换皮肤不会误报。涉及动画的用例会等过渡
跑完（入场 `qWait(250)`、离场用 `waitOverlayClosed()`，对应 180ms / 240ms 时长）再断言终态。

## 7. 嵌入到已有窗口

库可以直接放进宿主的布局（`setCentralWidget`、`QSplitter`、`QDockWidget` 都可），以下是宿主需要知道的边界：

- **最小尺寸 420 × 460**（构造里 `setMinimumSize`）：比这更小的面板会挤压消息区，顶栏与会话栏仍完整
- **事件作用域只在自己身上**（`qApp` 级过滤器只在事件落在库内部时才处理）：
  - `Esc`：只当事件目标是库的后代、且设置对话框未打开时，才收起浮层 / 会话面板；
    不会吞掉宿主窗口或宿主对话框的 `Esc`
  - 「点面板外」收起会话面板只在**同一窗口**内生效，别的窗口的点击不影响它
  - 字距补齐（`QEvent::Polish`）只作用于库自己的后代
- **缩放快捷键**是 `WidgetWithChildrenShortcut`：焦点在库内（或其子控件）时才响应
  `Ctrl+=` / `Ctrl+-` / `Ctrl+0`，不会在宿主窗口的其他位置触发
- **皮肤与缩放是进程级全局**（`gs::themeId()` / `gs::zoomFactor()`）：同一进程内的多个实例会联动，
  目前没有实例级 API；确实需要各自独立时，得在宿主侧隔离（例如分进程）
- **样式表**：库把主题 `setStyleSheet` 在自身子树上，不影响宿主控件；反过来宿主若给
  `ChartWidget` 另设样式表，会在库换肤 / 缩放时被覆盖
- **拖放**：库已开 `setAcceptDrops`，落在库范围内的拖拽由库接收（发 `attachmentsAdded`），
  宿主在同一区域的拖放收不到
- **设置对话框**是 `QDialog(parent = ChartWidget)` 的独立顶级窗口（窗口级模态），不会进入宿主布局，
  随父对象析构；库不提供内嵌形式的设置页

## 8. 常见问题

- **构建报 `'Visual' / 'VARS' / 'ATH"' is not recognized`**：`build.bat` 被写入了中文注释或
  非 CRLF 换行，恢复为纯 ASCII 内容（见第 2 节注意事项）。
- **截图/离屏运行无文字**：设置 `QT_QPA_FONTDIR=C:\Windows\Fonts`（见第 4 节）。
- **demo 启动报缺少 Qt5Core.dll / Qt5Svg.dll 等**：未把 Qt 的 `bin` 加入 PATH，或未用
  `windeployqt demo.exe` 收集依赖（图标走 QtSvg，部署时勿漏 `Qt5Svg.dll`）。
- **链接报 `LNK1181: 无法打开 QtChartWidget.lib`**：先构建 lib 再构建 demo；
  两者输出目录均为 `bin/`。

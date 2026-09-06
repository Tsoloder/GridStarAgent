# QtChartWidget 构建与运行说明

用 Qt 5.12.2 (MSVC 2017 x64) 原生复刻 `webui/`（GridStar AI 聊天界面）的 UI 库，
编译产物为动态库 `QtChartWidget.dll`，并附带一个可交互的示例宿主 `demo.exe`。

库本身**不做任何网络请求**：宿主通过 setter 推数据、通过信号收交互。

## 1. 目录结构

```
QtChartWidget/
├── QtChartWidget.pro        动态库工程（TEMPLATE = lib）
├── build.bat                一键构建脚本（vcvars64 + qmake + nmake）
├── include/
│   ├── chartwidget.h        唯一公开头文件：gs::ChartWidget + C 工厂
│   └── qtchartwidget_global.h  导出宏 QTCHARTWIDGET_EXPORT
├── src/                     库实现（内部类不导出）
│   ├── chartwidget.cpp      主组合部件（三栏骨架 / 消息流 / 事件分发）
│   ├── theme.cpp            QSS 调色板（对应 webui/style.css）
│   ├── markdownview.cpp     Markdown / 代码块 / ```json 结构化块
│   ├── messagewidgets.*     消息气泡、工具组、审批卡、选项卡、参数卡、工作流卡
│   ├── composer.cpp         输入区（模式 / 模型 / Skill / 附件芯片）
│   ├── popups.cpp           会话面板、模型与 Skill 下拉
│   ├── phasepanel.cpp       阶段计划面板
│   ├── settingsdialog.cpp   设置中心对话框
│   └── commonwidgets.*      FlowLayout、Chevron、StatusDot、ConnectionButton 等
├── resources/
│   ├── icons/               17 个 feather 风格 SVG 图标（viewBox 24×24，运行时着色）
│   └── icons.qrc            资源清单（前缀 /icons，编译进 DLL）
├── demo/
│   ├── demo.pro             示例宿主工程（链接 ../bin/QtChartWidget.lib）
│   └── main.cpp             演示数据 + 全部信号接线 + --shot 离屏截图
├── examples/
│   ├── examples.pro         接线示例工程（链接 ../bin/QtChartWidget.lib）
│   └── host_example.cpp     最小宿主接线示例：推数据 + 全信号 connect + 假流式
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

## 4. 运行 demo

`demo.exe` 依赖 Qt 运行时 DLL（含图标渲染所需的 `Qt5Svg.dll`）与本目录的 `QtChartWidget.dll`，
运行前把两者加入 PATH：

```powershell
$env:PATH = 'D:\Application\Qt\Qt5.12.2\5.12.2\msvc2017_64\bin;<工程>\QtChartWidget\bin;' + $env:PATH
demo.exe                 # 交互窗口，预载一段示例历史
demo.exe --empty         # 空态（欢迎页）
demo.exe --shot out.png  # 离屏渲染并截图后退出
```

离屏截图（`--shot`）使用 `offscreen` 平台插件，该平台默认不加载系统字体，
会出现"有框无字"的截图；运行前设置字体目录即可：

```powershell
$env:QT_QPA_FONTDIR = 'C:\Windows\Fonts'
```

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
| `/settings [tab]` | 打开设置中心（可选 tab：providers/models/skills/mcp） |

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
   const char *qtchartwidget_version(void); // "1.0.0"
   ```

### 公开 API 分组（include/chartwidget.h）

- 顶栏/会话栏：`setConnectionState` / `setSessions` / `setCurrentSessionTitle`
- 输入区：`setModels` / `setCurrentModel` / `setSkills` / `setCurrentSkill` /
  `setMode` / `setBusy` / `setConfigLoaded` / `setConfigWarning` / `setInputText`
- 附件：`addAttachments` / `clearAttachments` / `setVoiceEnabled` / `setVoiceRecording`
- 消息流：`setHistory` / `appendUserMessage` / `appendAssistantMessage` /
  `appendAssistantText` / `appendReasoning` / `appendToolCall` / `appendToolResult` /
  `appendApproval` / `resolveApproval` / `reEnableApproval` / `appendWorkflowEvent` /
  `setTokenUsage` / `appendFailure` / `setPhasePlan` / `showToast` / `clearMessages`
- 设置中心：`setSettingsDraft` / `setDiscoveredModels` / `setProviderBusy` /
  `setSettingsSkills` / `setMcpTools` / `openSettings` / `settingsSaved`
- 界面缩放：`zoomIn` / `zoomOut` / `zoomReset` / `zoomFactor`
  （快捷键 `Ctrl+=` / `Ctrl+-` / `Ctrl+0` 已内置于部件）
- 信号：`sendMessage`、`stopRequested`、`modeChanged`、`modelSelected`、`skillSelected`、
  `newSessionRequested`、`sessionSelected/Renamed/Cleared/Deleted`、
  `connectionCheckRequested`、`optionChosen`、`toolParamsConfirmed`、`approvalDecided`、
  `workflowRunRequested`、`retryRequested`、`settingsSaveRequested`、
  `testProviderRequested`、`readModelsRequested`、`refreshSkillsRequested`、
  `refreshMcpRequested`、`attachRequested`、`voiceRequested`、`attachmentsAdded`、
  `attachmentRemoved`

### 主要数据形状（与 webui/app.js 同构）

- sessions：`{id, title, updated_at, created_at}`
- models：`{provider, id, name, enabled, provider_enabled}`，模型 key = `provider/id`
- skills：`{id, name, description[, version, source, allowed_tools]}`
- 历史消息：user `{role, content, display_content, attachments[{name, kind, url}]}`；
  assistant `{content, reasoning_content, active_skills[], tool_calls[{id, function{name, arguments}}], usage{...}}`；
  tool `{tool_call_id, tool_name, content}`
- 结构化块：正文中 ```` ```json ```` 围栏内含 `options / tool_params / workflow / phase_plan`
  任一键时渲染为交互卡片，其余围栏按代码块原样显示

## 6. 常见问题

- **构建报 `'Visual' / 'VARS' / 'ATH"' is not recognized`**：`build.bat` 被写入了中文注释或
  非 CRLF 换行，恢复为纯 ASCII 内容（见第 2 节注意事项）。
- **截图/离屏运行无文字**：设置 `QT_QPA_FONTDIR=C:\Windows\Fonts`（见第 4 节）。
- **demo 启动报缺少 Qt5Core.dll / Qt5Svg.dll 等**：未把 Qt 的 `bin` 加入 PATH，或未用
  `windeployqt demo.exe` 收集依赖（图标走 QtSvg，部署时勿漏 `Qt5Svg.dll`）。
- **链接报 `LNK1181: 无法打开 QtChartWidget.lib`**：先构建 lib 再构建 demo；
  两者输出目录均为 `bin/`。

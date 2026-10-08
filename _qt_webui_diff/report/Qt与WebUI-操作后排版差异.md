# Qt 原生界面 vs WebUI —— 「操作后」排版差异（第二轮 + 第三轮修复复验）

> **本报告随仓库提交，但配套测量工作区 `_qt_webui_diff/` 未提交**（含沙箱会话原文、probe 编译产物等）：已提交的图片只有 `round3-*.png` 四张；正文提到的 `round2-*.png`、`chat-对照.png` 等与 `probe2`、`webui_capture.py`、`compose_round3.py` 等脚本均属本地测量工作区。需要时可另行提交（注意 `_qt_webui_diff/sandbox/data/sessions/*/messages.jsonl` 是复制出的真实会话原文）。

- 对比条件：同一后端、同一会话数据、同视口（聊天 460×820、设置 920×720、用量浮层 560×252），Qt 5.12.2 原生渲染 vs 本机 Edge（Chromium）真实渲染。
- 会话数据：`b83df586-27a7-41c7-9f0b-9bf46f9f7ec4`（16 条消息，含一次 `ask_user_question`，最后一条是 `interrupted` 的空助手消息）。沙箱数据目录 `_qt_webui_diff/sandbox/data*`，后端 1232（原始）/1233（截断到「停在未作答询问」）/1234（删掉询问，无浮层）。
- Qt 侧几何由 `_qt_webui_diff/probe2`（demo 副本 + `--dump/--open`）导出，webui 侧由 Playwright + Edge 读取 DOM 矩形（`webui_capture.py`、`webui_parts_probe.py`、`webui_ask_probe.py`）。

## 1. 结论摘要

1. **主界面容器排版基本对齐**：输入区 `460×135 @ y685` 两侧完全一致；无浮层干扰时消息区 `519` vs `516`、相位面板 `645` vs `642`，只差 3px。
2. **但「操作后」有三处真实差异，且都是 Qt 侧行为造成**：
   - Qt 把**历史里已经作答过的询问**重新弹成选择浮层，webui 只在**最后一轮**仍未作答时才弹 → 会话一旦历史里出现过询问，Qt 就多出一张 `442×282` 的浮层，把消息区从 516px 压到 360px，相位面板整体上移 156px。
   - 载入历史后 Qt **停在最早的消息**（`messageList.y=126`，scrollTop=0），webui 停在最新消息（scrollTop=673）→ 首屏可见内容完全不同。
   - 选择浮层内部：Qt 候选列表可视高度只有 **94px**（内容 187px，需滚动，第 4 项「其他」在视口外），webui 为 **187.8px**，4 项一屏全见。
3. 子窗口尺寸差异一览（详见第 4 节）：设置中心头部 −13px、会话面板 −12px、用量模型下拉 +28px、轮次轨面板 −6px、Toast −2px、相位面板 −3px；皮肤浮层 `132×102`、设置对话框 `920×720`、选择卡宽 `442`、作答输入行 `338×32` 完全一致。

## 2. 主界面（操作后）

| 部件 | Qt | webui | 差 |
|---|---|---|---|
| 消息区 `messages` | 460×360 @ y126 | 460×516 @ y126 | **高 −156px**（受浮层挤压） |
| 相位面板 `#phase-panel` | 442×32 @ (9,486) | 442×35 @ (9,642) | 高 −3px、位置 −156px |
| 输入区 `composer` | 460×135 @ y685 | 460×135 @ y685 | **完全一致** |
| 轮次轨 `turn-rail` | 12×66 @ (0,377) | 12×60 @ (0,380) | 高 +6px |
| 消息滚动位置 | scrollTop = 0（最早） | scrollTop = 673（最新） | **行为差异** |
| 选择浮层 | 打开 442×282（历史询问） | 关闭 | **行为差异** |

去掉询问干扰后（沙箱 1234，14 条消息）：

| 部件 | Qt | webui | 差 |
|---|---|---|---|
| 消息区 | 460×519 @ y126 | 460×516 @ y126 | 3px |
| 相位面板 | 442×32 @ (9,645) | 442×35 @ (9,642) | 3px |
| 输入区 | 460×135 @ y685 | 460×135 @ y685 | 0 |
| 滚动位置 | scrollTop = 0 | scrollTop = 673 | **差异** |

对照图：`round2-聊天-无询问-对照.png`（左 Qt / 右 webui）、`round2-聊天-全量会话-对照.png`。

### 根因 1：浮层恢复策略不同（Qt 缺陷）

- Qt `chartwidget.cpp:1554-1570`：`lastAsk` 在 `finishTurn()` 里只在「该轮 awaiting」时被**赋值**，后续不再重置 → 早期轮次里已作答的询问会一直粘住；`1700-1703` 用它决定是否 `showChoice()`。
- webui `app.js:1418`：`if (last && last.awaitingInput && last.pendingAsk) openChoiceOverlay(...)`，只认**最后一轮**。
- 实测：同一会话，Qt 弹出 `442×282` 的浮层且内容正是第 1 轮的「开始您的网格任务 / 请问您想从哪一步开始？」（该询问已在第 4 条消息被回答 `import_cad`），webui 不弹。

### 根因 2：载入历史后未滚动到底部（Qt 缺陷）

- Qt `chartwidget.cpp:722-732`：`scrollToEnd(true)` 用 `QTimer::singleShot(0, ...) { bar->setValue(bar->maximum()); }`，触发时滚动条范围尚未建立（`maximum()` 仍为 0）→ 停在顶部。
- 实测：Qt `messageList` 停在 y=126（应有偏移 −472），第 5、6 张卡片被完全裁在视口外；webui `scrollTop=673 = 1189−516`。
- 结果：同一会话同一视口，Qt 首屏显示「你好 / 初始问候…」，webui 首屏显示「好的，您选择 CAD 数模文件…」及后续长消息。

## 3. 选择浮层（子窗口，同一「停在未作答询问」会话）

| 部件 | Qt | webui | 差 |
|---|---|---|---|
| 卡片 `choiceCard` / `#choice-overlay` | 442×282 @ (9,526) | 442×316.3 @ (9,491.7) | **高 −34px** |
| 头部 | `choiceHead` 440×93 | `.choice-head` 440×40 + `.choice-question` 440×28.5 | Qt 头部高 53px（问题行并入头部） |
| 候选列表**可视高** | `qt_scrollarea_viewport` 432×94 | `.choice-list` 440×187.8 | **−94px** |
| 候选列表**内容高** | `choiceBody` 432×187 | 内容 187.8（3×49.9 + 32） | **一致** |
| 可见候选数 | 3 项（第 4 项「其他」需滚动） | 4 项（3 项 + 「其他」） | **差异** |
| 作答输入行 | `choiceInput` 338×32 @ (21,718) | `.choice-input` 338×32 @ (21,749) | 尺寸一致、位置 +31 |
| 提交按钮 | `choiceSubmit` 74×32 @ (365,718) | — | — |
| 卡底边 | y = 808 | y = 808 | 一致 |

根因：`composer.cpp:310-323` 先用 `heightForCardWidth()` 量高再定位，量高发生在正文按最终宽度重排之前（`choiceoverlay.cpp:523-539` 注释里也承认这个坑），得到 282px；正文需要 93+187+95≈375px，卡片放不下 → 正文滚动区被压到 94px 出现滚动条。webui 直接按内容展开到 316.3px。

对照图：`round2-选择浮层-对照.png`（左右两侧各一张待答浮层）。

## 4. 其他子窗口尺寸对照

| 子窗口 | Qt | webui | 差 |
|---|---|---|---|
| 轮次轨面板 `turnRailPanel` | 300×122 @ (22,349) | 300×128 @ (22,346) | 宽一致、高 −6px |
| ├ 面板头 | — | 298×28 | |
| ├ 列表 | `turnRailScroll` 298×96 / 内容 290×98 | 298×98 | 2px |
| 会话面板 `sessionPanel` | 444×100 @ (8,126) | 444×112 @ (8,126) | **高 −12px** |
| ├ 搜索框 | 390×30 @ (17,135) | 390×30 @ (17,135) | **完全一致** |
| ├ 列表 | 434×54 @ (9,173) | 442×63 @ (9,174) | 宽 −8、高 −9 |
| 皮肤浮层 `themeListbox` | 132×102 | 132×102 | **完全一致** |
| Toast | 440×34 @ (10,639) | 440×36 @ (10,659) | 高 −2、位置 −20 |
| 设置对话框 | 920×720 | 920×720 | **完全一致** |
| ├ 头部 | 62 | 75 | **−13px** |
| ├ 页签 | 42 | 43 | −1px |
| ├ 内容区 | 556 | 544 | +12px |
| ├ 操作栏 | 60 | 56 | +4px |
| 用量：模型下拉 `usageListPopup` | 196×72 | 167.8×71 | **宽 +28px** |
| 用量：日历浮层 `usageRangePopup` | 558×250 | 560×252 | −2px |
| └ 日历本体 | 内容 558×250 | 408×226 @ (394,233) | — |

对照图：`round2-轮次轨面板-对照.png`、`round2-子窗口-对照.png`（皮肤浮层 / 会话面板 / Toast / 用量下拉，自上而下四行）。

## 5. 修复建议（第二轮结论；第三轮已全部落地，见第 7 节）

| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| 1 | `chartwidget.cpp:1556-1571` | `lastAsk` 不随轮次重置，弹出已作答的询问 | `finishTurn()` 里改为每轮覆盖（`lastAsk = awaiting ? pendingAsk : QVariantMap()`） |
| 2 | `chartwidget.cpp:722-732` | `singleShot(0)` 与滚动条范围竞态，载入历史后停在顶部 | 用 `QTimer::singleShot(0)` 后再校验一次，或改在 `rangeChanged` 时贴底 |
| 3 | `composer.cpp:310-323` / `choiceoverlay.cpp:523-539` | 候选列表被压到 94px，需要滚动 | 量高前先按最终宽度 `activate()`；或在 `syncBodyHeight()` 后重算一次卡片高度 |
| 4 | `chartwidget.cpp:641-646` | 空状态提示被裁（首轮发现） | 宽度放到 270+，并按 `heightForWidth` 设最小高 |
| 5 | `settingsdialog.cpp:272-352` | 设置中心头部矮 13px | `eyebrow->setFixedHeight(16); title->setFixedHeight(24);` |
| 6 | `sessionpanel` / 用量下拉 | 会话面板矮 12px、用量模型下拉宽 28px | 按元素内容/`sizeHint` 校正 |

## 6. 复现方式

```powershell
# 三个沙箱后端（数据隔离，不动用户数据）
# GRIDSTAR_DATA_DIR_PINNED 是提权重启时钉住数据目录用的内部变量，
# 数据目录本身固定为 %USERPROFILE%\AppData\Roaming\.gridstar，不对使用者开放自定义。
$env:GRIDSTAR_DATA_DIR_PINNED='...\_qt_webui_diff\sandbox\data'      ; python app.py --port 1232   # 16 条，含已作答询问
$env:GRIDSTAR_DATA_DIR_PINNED='...\_qt_webui_diff\sandbox\data-ask'  ; python app.py --port 1233   # 截断到停在未作答询问
$env:GRIDSTAR_DATA_DIR_PINNED='...\_qt_webui_diff\sandbox\data-noask'; python app.py --port 1234   # 无询问

# Qt 侧
probe2.exe --live --api http://127.0.0.1:1232 --dump dumps\base.tsv            # 全部具名子控件几何
probe2.exe --live --api http://127.0.0.1:1232 --open rail --dump dumps\rail-live.tsv
$env:PROBE_SHOT_DELAY=1900; probe2.exe --live --api http://127.0.0.1:1232 --open rail --shot qt_ops\rail-live.png

# webui 侧
python _qt_webui_diff\webui_capture.py _qt_webui_diff\webui_ops      # Playwright + 本机 Edge
python _qt_webui_diff\webui_parts_probe.py                          # 子窗口内部分区
python _qt_webui_diff\webui_ask_probe.py http://127.0.0.1:1233/ui/ _qt_webui_diff\webui_ask
```

---

# 7. 第三轮：6 处修复落地与复验

第 5 节的 6 条建议已全部落地（用户选择「全修 6 处」），并跑通了 52 项 Qt 测试与量化复验。

## 7.1 改动清单（9 个文件，+144 / −14）

| # | 文件:位置 | 根因 | 改动 |
|---|---|---|---|
| 1 | `chartwidget.cpp` `setHistory()`（原 1566-1571） | `lastAsk` 只在「本轮 awaiting」时写入，之后**从不清理**，于是历史里早期**已作答**的询问一直粘着 | `lastAsk = (turn && awaiting) ? pendingAsk : QVariantMap()`，每轮覆盖（对齐 `app.js:1418` 的 `last.awaitingInput`） |
| 2 | `chartwidget.cpp` `scrollToEnd()` + 新增 `pinToBottom()` | 消息控件刚建好时布局未定稿，`singleShot(0)` 读到 `maximum()==0`，载入历史后停在顶部 | `pinToBottom(3)`：立刻贴底 + 随后 3 轮事件循环补跳；每轮先查 `m_followBottom`，用户上滚即停手 |
| 3 | `composer.cpp` `layoutChoiceOverlay()`、`choiceoverlay.cpp` | ①卡片高度用「旧宽度」量出的 `sizeHint` 定稿，偏小；②正文滚动区竖直策略 `Ignored`，QBoxLayout 把富余高度分给标题行/作答区（实测标题行 93、正文只剩 94）；③QSS 无 `line-height`，选项行盒偏矮；④自定义 `sizeHint()` 未计卡片 1px 边框 | ①落位后按定稿宽度**再量一次**，只增不减地向上生长；②滚动区改 `QSizePolicy::Preferred`（`minimumHeight=0` 保留可压缩性）；③`choiceItemName` 最小高 18、`choiceItemDesc` 16；④`sizeHint()` 补边框 2px |
| 4 | `chartwidget.cpp` `createEmptyState()` | 只设 `maximumWidth(270)` 时 QLabel 的 `sizeHint` 收成单行宽（实测 120px），两行文本被裁 | 改 `setFixedWidth(270)`，并按 `QFontMetrics::boundingRect(…, TextWordWrap)` 给最小高 |
| 5 | `settingsdialog.cpp` `buildUi()` | `.eyebrow`（行内 span）占的是**所在行盒** 18px、`.settings-head h2` 行盒 24px，Qt 按字形高只给 11/22 | `eyebrow->setMinimumHeight(18)`、`title->setMinimumHeight(24)`（表头 62 → 76） |
| 6 | `theme.cpp` `appStyleSheet()` | **共同根因**：QSS 里只写 `font-size` 的规则会让 Qt 按「应用字体」重建字体族——中文 Windows 上是**宋体 SimSun**（webui 是 `html{font-family:"Bahnschrift","Microsoft YaHei UI"}`） | 给每条「有 `font-size` 且无 `font-family`」的规则补上 `font-family: %UI%`，等价于 webui 的字体继承 |
| 6b | `usagepanel.cpp` `preferredContentWidth()` | 量宽时选项控件还没被 QSS polish，布局 `sizeHint` 用的是未套样式表的字体 → 同一下拉在 **196 / 169** 之间跳（webui 恒为 167.8） | 量宽前 `ensurePolished()` + `layout()->activate()`，定稿后再量 |

## 7.2 第 6 条的根因量化（`dumps/fonts.tsv` → `dumps/fonts2.tsv`）

| 观测 | 修前 | 修后 |
|---|---|---|
| 仍解析为 `SimSun` 的控件 | 301 / 671（其中 88 个 11px、75 个 9px…即所有经 QSS 设过字号的文本控件） | 301 / 671，但**「带像素字号且仍为宋体」= 0 个**（其余是无字号规则的容器/无文本控件） |
| 拉丁串 ink 宽（用量下拉模型名） | 167px（SimSun） | 与 webui 同族同号 → 下拉宽 169 vs webui 167 |
| 行盒（13px / 11px 文本） | 16 / 13（1.23em） | 16 / 13（Qt 取字体 ascent+descent+leading）→ 与 Chromium 的 18 / 16（1.38em）**仍差**，故第 5、6a、第 3③ 项按 webui 实测行盒补最小高 |

## 7.3 最终二进制复验（`dumps/final-*.tsv`，同会话同视口）

| 项 | 修前 | 修后（final） | webui 参考 |
|---|---|---|---|
| 1 粘性询问 | `choiceCard` 可见 442×282（弹已作答的第 1 轮询问） | **vis=0**（不弹） | 不弹（`choiceOpen:false`） |
| 2 滚动 | `messageList.y=126`（scrollTop 0） | **y=−354 → scrollTop 480 = max**（贴底） | scrollTop 673 = max（贴底） |
| 3 选择浮层 | 442×282 @ (9,526) | **442×314 @ (9,494)** | 442×316.3 @ (9,491.7) |
| ├ 标题行 | 440×93 | **440×40** | 40 |
| ├ 列表视口 | 432×94（4 个候选只露 3 个 + 滚动条） | **440×215**（4 个全部可见，无滚动条） | 内容 187.8 + 问题 28.5 |
| ├ 选项行 | 420×43 | **428×50** | 49.9 |
| └ 提示行 | 418×39 | **418×11** | 12 |
| 4 空状态提示 | 120×26（两行被裁） | **270×28**（单行 + margin，未裁） | `.empty-state p` 250.5×15 + margin 7px 0（盒 29） |
| 5 设置表头 | 920×62（eyebrow 11 / title 22） | **920×76（eyebrow 18 / title 24）** | 75（920 宽视口） |
| 6a 会话面板 | 444×100，行 424×44，标题 13 / 元信息 11 | **444×108，行 424×52，标题 18 / 元信息 16** | 112 / 53 / 18 / 16 |
| └ 列表 | 434×54 | **434×62** | 442×63 |
| 6b 用量模型下拉 | 196×72（偶发 169，量宽时机不确定） | **169×72（截图路径与导出一致）** | 167×71（`webui_capture` 实测） |
| （校准项）用量日历浮层 | 558×250 | 558×252 | 560×252 |

## 7.4 回归验证

* **Qt 测试套件：52 passed, 0 failed, 0 skipped**（`QtChartWidget/build/tests/verify_final.txt`，16834ms）
  * 其中 **2 条断言按修复后的（对齐 webui 的）行为重写**，如实说明：
    * `expandKeepsScrollPosition`：原首断言要求「载入历史后**不在**底部」，与新行为（webui 载入后贴底）直接冲突 → 改为先 `QCOMPARE(value, maximum)` 断言贴底，再手动 `setValue(0)` 制造「用户离开底部」的前提，后半段（展开工具项 + 追加文本不得抢滚动条）原样保留。
    * `choiceOverlayFitsNarrowWindow`：原断言要求窄窗下**必须有滚动条**（当时卡片被量矮、正文溢出）；卡片修好后 420×460 窗口里内容正好放得下 → 改为「正文高于视口时才必须有滚动条」，并**新增**更矮宿主（420×360）的断言：卡片被宿主上限截断、正文转为滚动、选项仍不被裁。
* **字体族回填 A/B**（同会话、同视口，Qt 两次渲染 vs 同一张 webui 截图）：
  `chat` 回填 7.677% vs 不回填 7.319%；`traj` 6.084% vs 6.175%；两次 Qt 渲染互差 3.6% / 3.1%。
  → 全屏像素指标上中性（±0.4pp），但第 3③/5/6a/6b 四项的**盒子几何**只能靠它对齐（不回填时用量下拉 196、表头 62、会话行 44）。

## 7.5 复验对照图（左 Qt 修后 / 右 webui，`report/`）

`round3-聊天-全量会话-对照.png`、`round3-选择浮层-对照.png`、`round3-轮次轨面板-对照.png`、`round3-子窗口-对照.png`（皮肤浮层 / 会话面板 / Toast / 用量模型下拉 / 用量日历浮层，自上而下五行）。
生成脚本：`_qt_webui_diff\compose_round3.py`（尺寸自检 + 复用 `compose_round2.py` 的排版函数）。

## 7.6 仍未解决（均不在本轮 6 项内，下一轮候选）

| 现象 | Qt | webui | 说明 |
|---|---|---|---|
| 消息区内容高（16 条会话） | 997 | 1189 | **−16%**；全屏 chat 像素差（7.677%）主要由它主导（内容错位），逐条消息卡的行盒/内边距累计差，与 6a 同类根因（QSS 无 `line-height`） |
| 设置对话框宽 | 920 | 890 | 模态遮罩 15px padding 未复刻；连带内容区 551 vs 516、操作栏 60 vs 56 |
| 会话行 / 面板 | 52 / 108 | 53 / 112 | 余 1–4px（≈1%） |

## 7.7 本轮复现命令

```powershell
# 构建（库 + demo + 测试）
QtChartWidget\build.bat

# 沙箱后端（数据隔离；GRIDSTAR_DATA_DIR_PINNED 为提权钉目录用的内部变量）
$env:GRIDSTAR_DATA_DIR_PINNED='...\_qt_webui_diff\sandbox\data'     ; python app.py --port 1232  # 16 条，含已作答询问
$env:GRIDSTAR_DATA_DIR_PINNED='...\_qt_webui_diff\sandbox\data-ask' ; python app.py --port 1233  # 末轮停在未作答询问

# 字体族 / 几何 / 子窗口（探针支持 --fonts、--dump、--open session|settings|usage|usagecal|rail|choice|toast）
$env:QT_QPA_PLATFORM='windows'; $env:QT_QPA_FONTDIR='C:\Windows\Fonts'
probe2.exe --live --api http://127.0.0.1:1232 --fonts dumps\fonts2.tsv
probe2.exe --live --api http://127.0.0.1:1233 --dump  dumps\final-ask.tsv
probe2.exe --live --api http://127.0.0.1:1232 --open session  --dump dumps\final-session.tsv
probe2.exe --live --api http://127.0.0.1:1232 --open settings --dump dumps\final-models.tsv
$env:PROBE_DUMP_DELAY=3400; probe2.exe --live --api http://127.0.0.1:1232 --open usage --dump dumps\final-usage.tsv

# 测试套件
$env:QT_QPA_PLATFORM='offscreen'; QtChartWidget\bin\qtchartwidget_tests.exe

# 子窗口截图（--tab 决定抓取目标；弹出型浮层是独立 Qt::Popup 窗口，必须用对应 --tab 才抓得到）
$env:PROBE_SHOT_DELAY=1800
probe2.exe --live --api http://127.0.0.1:1232 --open theme --tab theme      --shot qt_fix\theme-popup.png
$env:PROBE_SHOT_DELAY=3600
probe2.exe --live --api http://127.0.0.1:1232 --open usage --tab usage-list --shot qt_fix\usage-popup.png
probe2.exe --live --api http://127.0.0.1:1232 --open usagecal --tab usage-cal --shot qt_fix\usage-cal.png

# webui 侧对照（Playwright + 本机 Edge，无需 Tabbit）
python _qt_webui_diff\webui_capture.py _qt_webui_diff\webui_new   # 含 usageListPopup 167x71 / usageRangePopup 560x252
python _qt_webui_diff\webui_parts_fix.py                          # 设置表头 + 空状态盒模型（460 视口）
python _qt_webui_diff\compose_round3.py                           # 生成第 7.5 节的对照图
```

> 设置表头在 **920 宽**视口的 webui 参考值（`#settings-modal` 内 `.settings-head` 75、`.eyebrow` 18 行盒、`#settings-title` 24）由一次性 Playwright 查询得到（脚本见 `webui_parts_fix.py` 的 `JS` 片段，换 `viewport={'width':920,'height':720}` 即可复现）。
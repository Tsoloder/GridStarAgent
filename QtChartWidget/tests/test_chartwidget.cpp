// QtChartWidget 公开 API 回归测试
//
// 只通过 include/chartwidget.h 的公开接口驱动，用 QObject 树（objectName / class 动态属性）
// 观察渲染结果——内部类不导出，所以测试同时也在守着"公开契约"这条边界。
//
// 运行：bin\qtchartwidget_tests.exe（无参数跑全部；-functions 列出用例；
//       <用例名> 只跑一个；-v2 打印每条断言）

#include <chartwidget.h>

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScopedPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSharedPointer>
#include <QShortcut>
#include <QStyle>
#include <QTableWidget>
#include <QTextEdit>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <thread>

namespace {

// QSS 的 .class 选择器对应动态属性 "class"（空格分隔可多个）
bool hasClass(const QObject *object, const QString &cls)
{
    const QWidget *widget = qobject_cast<const QWidget *>(object);
    if (!widget)
        return false;
    return widget->property("class").toString().split(QLatin1Char(' ')).contains(cls);
}

QList<QWidget *> widgetsByClass(QWidget *root, const QString &cls)
{
    QList<QWidget *> out;
    const QList<QWidget *> all = root->findChildren<QWidget *>();
    for (QWidget *widget : all) {
        if (hasClass(widget, cls))
            out.append(widget);
    }
    return out;
}

QWidget *widgetByClass(QWidget *root, const QString &cls)
{
    const QList<QWidget *> found = widgetsByClass(root, cls);
    return found.isEmpty() ? nullptr : found.first();
}

QString labelText(QWidget *root, const QString &cls)
{
    for (QWidget *widget : widgetsByClass(root, cls)) {
        if (auto *label = qobject_cast<QLabel *>(widget))
            return label->text();
    }
    return QString();
}

QStringList labelTexts(QWidget *root, const QString &cls)
{
    QStringList out;
    for (QWidget *widget : widgetsByClass(root, cls)) {
        if (auto *label = qobject_cast<QLabel *>(widget))
            out << label->text();
    }
    return out;
}

QStringList fullLabelTexts(QWidget *root, const QString &cls)
{
    QStringList out;
    for (QWidget *widget : widgetsByClass(root, cls)) {
        if (auto *label = qobject_cast<QLabel *>(widget))
            out << (label->toolTip().isEmpty() ? label->text() : label->toolTip());
    }
    return out;
}

// 不依赖鼠标几何：直接派发 press+release（仍会经过控件上装的 event filter）
void clickWidget(QWidget *widget)
{
    if (!widget)
        return;
    const QPointF pos(2, 2);
    QMouseEvent press(QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::LeftButton,
                        Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

// 浮层离场是 240ms 淡出（webui app.js:842），「隐藏 + 清空」发生在动画结束回调里：
// 等待时留够余量，免得在离屏平台的定时器抖动下误判
void waitOverlayClosed() { QTest::qWait(350); }

// 浮层里按顺序找文本匹配的选项行（.choiceItem）
QWidget *choiceItemByText(QWidget *card, const QString &text)
{
    for (QWidget *item : widgetsByClass(card, QStringLiteral("choiceItem"))) {
        if (labelTexts(item, QStringLiteral("choiceItemName")).contains(text))
            return item;
    }
    return nullptr;
}

// 账本结构快照：按布局顺序记录「分组头 / 数据行 / 摘要行」及其文本（用 toolTip 取全文）。
// 用来验证「增量追加的结果」与「把同样的 events 重新全量加载」完全一致
QStringList ledgerShape(QWidget *traj)
{
    QStringList out;
    auto *area = traj->findChild<QScrollArea *>(QStringLiteral("trajLedger"));
    QLayout *layout = (area && area->widget()) ? area->widget()->layout() : nullptr;
    if (!layout)
        return out;
    for (int i = 0; i < layout->count(); ++i) {
        QWidget *widget = layout->itemAt(i)->widget();
        if (!widget)
            continue;
        QString kind;
        if (hasClass(widget, QStringLiteral("trajGroup"))) {
            kind = QStringLiteral("G:");
        } else if (hasClass(widget, QStringLiteral("trajRow"))) {
            kind = QStringLiteral("R:");
        } else if (hasClass(widget, QStringLiteral("trajSummary"))) {
            kind = QStringLiteral("S:");
        } else {
            continue;
        }
        QString text;
        for (QLabel *label : widget->findChildren<QLabel *>()) {
            if (!label->toolTip().isEmpty()) {
                text = label->toolTip();
                break;
            }
        }
        out << kind + text;
    }
    return out;
}

// 账本行的「签名」：把行内所有标签的文本拼起来（ElidedLabel 的全文在 toolTip 里）。
// 用来判断「同一行」以及「可见窗口换了一批行」
QString rowSignature(QWidget *row)
{
    QStringList parts;
    for (QLabel *label : row->findChildren<QLabel *>()) {
        const QString text = label->toolTip().isEmpty() ? label->text() : label->toolTip();
        if (!text.isEmpty())
            parts << text;
    }
    return parts.join(QStringLiteral("|"));
}

QList<QWidget *> visibleRows(QWidget *traj)
{
    return widgetsByClass(traj, QStringLiteral("trajRow"));
}

// 每轮 3 条记录（用户提问 / 模型请求开始 / 请求结束）的轨迹事件
QVariantList trajectoryFixture(int turns)
{
    QVariantList events;
    for (int i = 0; i < turns; ++i) {
        events << QVariantMap{ { QStringLiteral("type"), QStringLiteral("user") },
                               { QStringLiteral("turn"), i + 1 },
                               { QStringLiteral("content"),
                                 QStringLiteral("第 %1 轮：城南线跳闸，帮我出处置方案").arg(i + 1) },
                               { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:04:00") } }
               << QVariantMap{ { QStringLiteral("type"), QStringLiteral("traj_request_start") },
                               { QStringLiteral("turn"), i + 1 },
                               { QStringLiteral("request"), 1 },
                               { QStringLiteral("start"), QStringLiteral("2026-09-06T09:04:00") } }
               << QVariantMap{ { QStringLiteral("type"), QStringLiteral("traj_request_end") },
                               { QStringLiteral("turn"), i + 1 },
                               { QStringLiteral("request"), 1 },
                               { QStringLiteral("status"), QStringLiteral("completed") },
                               { QStringLiteral("content"), QStringLiteral("建议先合 lk07 转供。") },
                               { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:04:21") } };
    }
    return events;
}

bool fuzzy(qreal a, qreal b) { return qAbs(a - b) < 0.0001; }

// 一轮历史：user + assistant(含工具调用/用量/耗时) + tool 结果
QVariantList historyFixture()
{
    QVariantMap user;
    user.insert(QStringLiteral("role"), QStringLiteral("user"));
    user.insert(QStringLiteral("content"), QStringLiteral("城南线跳闸，生成处置方案"));
    user.insert(QStringLiteral("display_content"), QStringLiteral("城南线跳闸，生成处置方案"));
    user.insert(QStringLiteral("ts"), QStringLiteral("2026-09-06T09:04:00"));

    QVariantMap call;
    call.insert(QStringLiteral("id"), QStringLiteral("call_91"));
    QVariantMap function;
    function.insert(QStringLiteral("name"), QStringLiteral("scada_snapshot"));
    function.insert(QStringLiteral("arguments"),
                    QStringLiteral("{\"feeder\":\"城南线\"}"));
    call.insert(QStringLiteral("function"), function);

    QVariantMap usage;
    usage.insert(QStringLiteral("total"), 100);
    usage.insert(QStringLiteral("input"), 80);
    usage.insert(QStringLiteral("output"), 20);
    usage.insert(QStringLiteral("estimated"), false);

    QVariantMap assistant;
    assistant.insert(QStringLiteral("role"), QStringLiteral("assistant"));
    assistant.insert(QStringLiteral("content"), QStringLiteral("先取断面，再校核转供潮流。"));
    assistant.insert(QStringLiteral("reasoning_content"), QStringLiteral("先确认开关位置。"));
    assistant.insert(QStringLiteral("tool_calls"), QVariant(QVariantList{ call }));
    assistant.insert(QStringLiteral("usage"), usage);
    assistant.insert(QStringLiteral("ts"), QStringLiteral("2026-09-06T09:04:06"));
    assistant.insert(QStringLiteral("elapsed_ms"), 1500);
    assistant.insert(QStringLiteral("think_ms"), 400);
    assistant.insert(QStringLiteral("ttft_ms"), 300);
    assistant.insert(QStringLiteral("tps"), 24);

    QVariantMap tool;
    tool.insert(QStringLiteral("role"), QStringLiteral("tool"));
    tool.insert(QStringLiteral("tool_call_id"), QStringLiteral("call_91"));
    tool.insert(QStringLiteral("tool_name"), QStringLiteral("scada_snapshot"));
    tool.insert(QStringLiteral("content"),
                QStringLiteral("{\"breaker\":\"open\",\"I_A\":1820}"));

    return QVariantList{ user, assistant, tool };
}

// GET /usage/stats 的响应形状（两个模型、两个供应商；实测与估算分开记账）
QVariantMap usageFixture(const QString &start, const QString &end)
{
    QVariantMap proModel;
    proModel.insert(QStringLiteral("model"), QStringLiteral("gridstar/gs-pro-32k"));
    proModel.insert(QStringLiteral("provider"), QStringLiteral("gridstar"));
    proModel.insert(QStringLiteral("label"), QStringLiteral("gs-pro-32k"));
    proModel.insert(QStringLiteral("total"), 1428571);
    proModel.insert(QStringLiteral("input"), 1200000);
    proModel.insert(QStringLiteral("output"), 228571);
    proModel.insert(QStringLiteral("measured"), 1300000);
    proModel.insert(QStringLiteral("estimated"), 128571);
    proModel.insert(QStringLiteral("cache_read"), 466667);
    proModel.insert(QStringLiteral("cache_write"), 1000);
    proModel.insert(QStringLiteral("turns"), 33);
    proModel.insert(QStringLiteral("sessions"), 1);

    QVariantMap chatModel;
    chatModel.insert(QStringLiteral("model"), QStringLiteral("deepseek/deepseek-chat"));
    chatModel.insert(QStringLiteral("provider"), QStringLiteral("deepseek"));
    chatModel.insert(QStringLiteral("label"), QStringLiteral("deepseek-chat"));
    chatModel.insert(QStringLiteral("total"), 200000);
    chatModel.insert(QStringLiteral("input"), 170000);
    chatModel.insert(QStringLiteral("output"), 30000);
    chatModel.insert(QStringLiteral("measured"), 180000);
    chatModel.insert(QStringLiteral("estimated"), 20000);
    chatModel.insert(QStringLiteral("cache_read"), 40000);
    chatModel.insert(QStringLiteral("cache_write"), 0);
    chatModel.insert(QStringLiteral("turns"), 5);
    chatModel.insert(QStringLiteral("sessions"), 1);

    QVariantList providers;
    providers << QVariantMap{ { QStringLiteral("provider"), QStringLiteral("gridstar") },
                              { QStringLiteral("label"), QStringLiteral("GridStar 网关") },
                              { QStringLiteral("total"), 1428571 },
                              { QStringLiteral("input"), 1200000 },
                              { QStringLiteral("output"), 228571 },
                              { QStringLiteral("measured"), 1300000 },
                              { QStringLiteral("estimated"), 128571 },
                              { QStringLiteral("turns"), 33 } }
              << QVariantMap{ { QStringLiteral("provider"), QStringLiteral("deepseek") },
                              { QStringLiteral("label"), QStringLiteral("deepseek") },
                              { QStringLiteral("total"), 200000 },
                              { QStringLiteral("input"), 170000 },
                              { QStringLiteral("output"), 30000 },
                              { QStringLiteral("measured"), 180000 },
                              { QStringLiteral("estimated"), 20000 },
                              { QStringLiteral("turns"), 5 } };

    QVariantList buckets;
    const QStringList stamps{ QStringLiteral("2026-09-20T09"), QStringLiteral("2026-09-20T10"),
                              QStringLiteral("2026-09-20T11") };
    for (const QString &stamp : stamps) {
        buckets << QVariantMap{ { QStringLiteral("t"), stamp },
                                { QStringLiteral("input"), 400000 },
                                { QStringLiteral("output"), 80000 },
                                { QStringLiteral("total"), 480000 },
                                { QStringLiteral("measured"), 460000 },
                                { QStringLiteral("estimated"), 20000 },
                                { QStringLiteral("turns"), 3 } };
    }

    QVariantMap out;
    out.insert(QStringLiteral("range"),
               QVariantMap{ { QStringLiteral("start"), start },
                            { QStringLiteral("end"), end },
                            { QStringLiteral("resolution"), QStringLiteral("hour") } });
    out.insert(QStringLiteral("totals"),
               QVariantMap{ { QStringLiteral("total"), 1628571 },
                            { QStringLiteral("input"), 1370000 },
                            { QStringLiteral("output"), 258571 },
                            { QStringLiteral("measured"), 1480000 },
                            { QStringLiteral("estimated"), 148571 },
                            { QStringLiteral("cache_read"), 506667 },
                            { QStringLiteral("cache_write"), 1000 },
                            { QStringLiteral("reasoning"), 5000 },
                            { QStringLiteral("turns"), 38 },
                            { QStringLiteral("sessions"), 2 } });
    out.insert(QStringLiteral("buckets"), buckets);
    out.insert(QStringLiteral("providers"), providers);
    out.insert(QStringLiteral("models"), QVariantList{ proModel, chatModel });
    out.insert(QStringLiteral("candidates"),
               QVariantMap{ { QStringLiteral("providers"), providers },
                            { QStringLiteral("models"), QVariantList{ proModel, chatModel } } });
    return out;
}

// 设置中心用例共用的最小草稿：一个 provider + 一个 model（进模型页需要至少一份配置）
QVariantMap settingsDraftFixture()
{
    QVariantMap capabilities;
    const char *const capKeys[5] = { "tools", "parallel_tools", "reasoning", "vision",
                                     "stream_usage" };
    for (const char *key : capKeys)
        capabilities.insert(QLatin1String(key), false);

    const QVariantMap model{
        { QStringLiteral("id"), QStringLiteral("gs-pro-32k") },
        { QStringLiteral("provider"), QStringLiteral("gridstar") },
        { QStringLiteral("name"), QStringLiteral("GS-Pro 32K") },
        { QStringLiteral("enabled"), true },
        { QStringLiteral("context_window"), 32768 },
        { QStringLiteral("max_output_tokens"), 4096 },
        { QStringLiteral("capabilities"), capabilities },
    };
    const QVariantMap provider{
        { QStringLiteral("id"), QStringLiteral("gridstar") },
        { QStringLiteral("name"), QStringLiteral("GridStar 网关") },
        { QStringLiteral("type"), QStringLiteral("openai") },
        { QStringLiteral("base_url"), QStringLiteral("https://llm.gridstar.local/v1") },
        { QStringLiteral("api_key"), QStringLiteral("") },
        { QStringLiteral("enabled"), true },
    };
    QVariantMap config;
    config.insert(QStringLiteral("providers"), QVariantList{ provider });
    config.insert(QStringLiteral("models"), QVariantList{ model });
    config.insert(QStringLiteral("default_model"), QStringLiteral("gridstar/gs-pro-32k"));
    return config;
}

} // namespace

class TestChartWidget : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void themeSwitchAndSignal();
    void zoomSteps();
    void modeModelSkillSignals();
    void inputSendRoundTrip();
    void attachmentChipPreview();
    void attachChipFitsNarrowWindow();
    void bubbleAttachmentThumbFitsNarrowWindow();

    void historyMergesTurnWithUsageAndTiming();
    void historyToolResultBackfill();
    void processAndToolHoverAccent();
    void bubbleDetailPopupAnchoring();
    void bubbleDetailPopupFitsNarrowHost();

    void optionsOverlayChooseAndEsc();
    void optionsOverlayFreeText();
    void choiceOverlayShowsAboveComposer();
    void choiceOverlayFitsNarrowWindow();
    void toolParamsOverlaySubmit();
    void approvalOverlayRoundTrip();

    void workflowProposalRun();
    void phasePlanCollapsesWhenComplete();
    void phasePlanSurvivesViewTabSwitch();

    void trajectoryViewTabSwitch();
    void trajectoryInspectorResponsive();
    void sessionBadgeStatus();
    void toolArgsTableAndResultFormat();
    void askUserToolCallIsNotRendered();
    void usageModelLabelMapping();
    void letterSpacingApplied();
    void trajectoryRowElides();
    void hoverRevealsCopyButton();
    void overlayAndCardTransitions();
    void inputAutoGrowOnResize();
    void expandKeepsScrollPosition();
    void escScopedToOwnWidget();
    void zoomShortcutScopedToWidget();
    void keyboardReachability();
    void turnRailPanelHover();
    void overlayGeometryMatchesWebui();
    void sessionPanelReflowsWhileOpen();
    void toastFitsWrappedText();
    void usagePanelTab();
    void usageStaleResponseIgnored();
    void usagePopupsFitNarrowHost();
    void settingsDialogCompactLayout();
    void confirmDialogFitsNarrowSettings();
    void markdownTableSizesToContent();
    // 回归：整张样式表必须能被 Qt 完整解析（不支持的选择器会丢弃其后所有规则）
    void appStyleSheetParsesFully();
    void guiThreadGuard();
    void renderPerformance();
    void trajectoryLedgerVirtualization();

private:
    QWidget *choiceCard() const;
    void openOptionsOverlay(const QString &extra = QString());
    QScopedPointer<gs::ChartWidget> m_chart;
};

void TestChartWidget::init()
{
    // 公开 API 是全局单例式的皮肤/缩放状态：每个用例前复位，避免互相污染
    m_chart.reset(new gs::ChartWidget);
    m_chart->setTheme(QStringLiteral("dark"));
    m_chart->zoomReset();
    m_chart->resize(460, 820);
    m_chart->show();
    QTest::qWait(20); // 让布局与浮层几何落定
}

void TestChartWidget::cleanup()
{
    m_chart.reset();
    QTest::qWait(5);
}

QWidget *TestChartWidget::choiceCard() const
{
    return m_chart->findChild<QWidget *>(QStringLiteral("choiceCard"));
}

// 结构化块里的 options 由 finishAssistant / appendAssistantMessage 自动弹成浮层
void TestChartWidget::openOptionsOverlay(const QString &extra)
{
    const QString content = QStringLiteral(
                                "请选择处置方案\n\n```json\n"
                                "{\"options\":[{\"label\":\"方案A\",\"value\":\"a\"},"
                                "{\"label\":\"方案B\",\"value\":\"b\",\"description\":\"备选\"}]}\n"
                                "```\n")
                            + extra;
    m_chart->appendAssistantMessage(content);
    QTest::qWait(20);
}

// ---------------------------------------------------------------- 皮肤 / 缩放

void TestChartWidget::themeSwitchAndSignal()
{
    QCOMPARE(m_chart->theme(), QStringLiteral("dark"));

    QSignalSpy spy(m_chart.data(), &gs::ChartWidget::themeChanged);
    m_chart->setTheme(QStringLiteral("silver"));
    QCOMPARE(m_chart->theme(), QStringLiteral("silver"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("silver"));
    // 换肤必须真正重设 QSS（--bg-grad-a 三套皮肤各不相同），只有 id 变了不算数
    QVERIFY(m_chart->styleSheet().contains(QStringLiteral("#f7f8f9")));

    m_chart->setTheme(QStringLiteral("blue"));
    QCOMPARE(m_chart->theme(), QStringLiteral("blue"));
    QVERIFY(m_chart->styleSheet().contains(QStringLiteral("#eaf2fa")));

    // 未知皮肤回退深色
    m_chart->setTheme(QStringLiteral("nope"));
    QCOMPARE(m_chart->theme(), QStringLiteral("dark"));
    QVERIFY(m_chart->styleSheet().contains(QStringLiteral("#15212a")));
}

void TestChartWidget::zoomSteps()
{
    QVERIFY(fuzzy(m_chart->zoomFactor(), 1.0));

    m_chart->zoomIn();
    QVERIFY(fuzzy(m_chart->zoomFactor(), 1.1));
    m_chart->zoomOut();
    QVERIFY(fuzzy(m_chart->zoomFactor(), 1.0));

    // 最大档 1.6 之后不再变化
    for (int i = 0; i < 10; ++i)
        m_chart->zoomIn();
    QVERIFY(fuzzy(m_chart->zoomFactor(), 1.6));

    m_chart->zoomReset();
    QVERIFY(fuzzy(m_chart->zoomFactor(), 1.0));
}

// ---------------------------------------------------------------- 输入区

void TestChartWidget::modeModelSkillSignals()
{
    QSignalSpy modeSpy(m_chart.data(), &gs::ChartWidget::modeChanged);
    m_chart->setMode(QStringLiteral("auto"));
    QCOMPARE(m_chart->mode(), QStringLiteral("auto"));
    QCOMPARE(modeSpy.count(), 1);

    const QVariantList models{
        QVariantMap{ { QStringLiteral("key"), QStringLiteral("gridstar/gs-pro-32k") },
                     { QStringLiteral("provider"), QStringLiteral("gridstar") },
                     { QStringLiteral("id"), QStringLiteral("gs-pro-32k") },
                     { QStringLiteral("name"), QStringLiteral("GS-Pro 32K") },
                     { QStringLiteral("enabled"), true },
                     { QStringLiteral("provider_enabled"), true } },
        QVariantMap{ { QStringLiteral("key"), QStringLiteral("local/gs-mini") },
                     { QStringLiteral("provider"), QStringLiteral("local") },
                     { QStringLiteral("id"), QStringLiteral("gs-mini") },
                     { QStringLiteral("name"), QStringLiteral("GS-Mini") },
                     { QStringLiteral("enabled"), true },
                     { QStringLiteral("provider_enabled"), true } },
    };
    m_chart->setModels(models);
    m_chart->setCurrentModel(QStringLiteral("local/gs-mini"));
    QCOMPARE(m_chart->currentModel(), QStringLiteral("local/gs-mini"));

    m_chart->setSkills(QVariantList{ QVariantMap{ { QStringLiteral("id"),
                                                   QStringLiteral("cfd-meshing") },
                                                 { QStringLiteral("name"),
                                                   QStringLiteral("CFD 网格") } } });
    m_chart->setCurrentSkill(QStringLiteral("cfd-meshing"));
    QCOMPARE(m_chart->currentSkill(), QStringLiteral("cfd-meshing"));
}

void TestChartWidget::inputSendRoundTrip()
{
    m_chart->setConfigLoaded(true);
    QPushButton *send = m_chart->findChild<QPushButton *>(QStringLiteral("sendButton"));
    QVERIFY(send);

    // 无内容且非 busy 时发送键不可用
    QVERIFY(!send->isEnabled());

    QSignalSpy spy(m_chart.data(), &gs::ChartWidget::sendMessage);
    m_chart->setInputText(QStringLiteral("生成城南线处置方案"));
    QCOMPARE(m_chart->inputText(), QStringLiteral("生成城南线处置方案"));
    QVERIFY(send->isEnabled());

    send->click();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("生成城南线处置方案"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("生成城南线处置方案"));
    QCOMPARE(spy.at(0).at(2).toList().size(), 0);
    // 发送后输入框清空
    QCOMPARE(m_chart->inputText(), QString());
}

void TestChartWidget::attachmentChipPreview()
{
    // 造一张真实图片，让图片条目走缩略图分支（webui .attach-chip img）
    const QString imagePath = QDir::temp().filePath(QStringLiteral("gs_attach_test.png"));
    QPixmap pixmap(8, 8);
    pixmap.fill(Qt::red);
    QVERIFY(pixmap.save(imagePath));

    m_chart->addAttachments(QVariantList{
        QVariantMap{ { QStringLiteral("id"), 1 },
                     { QStringLiteral("name"), QStringLiteral("shot.png") },
                     { QStringLiteral("size"), 2048 },
                     { QStringLiteral("ext"), QStringLiteral("png") },
                     // 故意不给 kind：库按扩展名推断（webui attachKind 的口径）
                     { QStringLiteral("path"), imagePath } },
        QVariantMap{ { QStringLiteral("id"), 2 },
                     { QStringLiteral("name"), QStringLiteral("report.pdf") },
                     { QStringLiteral("size"), 40960 },
                     { QStringLiteral("ext"), QStringLiteral("pdf") },
                     { QStringLiteral("kind"), QStringLiteral("text") },
                     { QStringLiteral("uploading"), true } } });
    QTest::qWait(20);

    QCOMPARE(widgetsByClass(m_chart.data(), QStringLiteral("attachChip")).size(), 2);
    // 图片给 22×22 缩略图，文档给扩展名标签
    const QList<QWidget *> thumbs =
        widgetsByClass(m_chart.data(), QStringLiteral("attachChipThumb"));
    QCOMPARE(thumbs.size(), 1);
    QCOMPARE(thumbs.first()->size(), QSize(22, 22));
    QVERIFY(labelTexts(m_chart.data(), QStringLiteral("attachExt")).contains(QStringLiteral("PDF")));
    // 上传中：大小位显示「上传中…」
    QVERIFY(labelTexts(m_chart.data(), QStringLiteral("attachSize"))
                .contains(QStringLiteral("上传中…")));

    QFile::remove(imagePath);
}

// style.css：.attach-chip{max-width:230px}；@media(max-width:640px){max-width:150px}
void TestChartWidget::attachChipFitsNarrowWindow()
{
    const QString longName = QStringLiteral(
        "gridstar-load-flow-analysis-report-2026-09-final-version.pdf");
    m_chart->resize(420, 820);
    m_chart->addAttachments(QVariantList{ QVariantMap{ { QStringLiteral("id"), 1 },
                                                       { QStringLiteral("name"), longName },
                                                       { QStringLiteral("size"), 10240 },
                                                       { QStringLiteral("ext"),
                                                         QStringLiteral("pdf") } } });
    QTest::qWait(20);

    QWidget *chip = widgetByClass(m_chart.data(), QStringLiteral("attachChip"));
    QVERIFY(chip);
    QWidget *nameWidget = widgetByClass(chip, QStringLiteral("attachName"));
    QVERIFY(nameWidget);
    auto *name = qobject_cast<QLabel *>(nameWidget);
    QVERIFY(name);
    QCOMPARE(name->toolTip(), longName);

    // 窄屏：名称省略收紧到 64（宽屏 140 的 150/230 近似）
    QCOMPARE(name->maximumWidth(), 64);
    const QString narrowText = name->text();
    QVERIFY(narrowText.length() < longName.length());
    QVERIFY(narrowText.endsWith(QChar(0x2026)));

    m_chart->resize(900, 820);
    QTest::qWait(20);
    QCOMPARE(name->maximumWidth(), 140);
    QVERIFY(name->text().length() > narrowText.length());
}

// style.css：.bubble-attachments .attach-thumb{84px}；@media(max-width:640px){64px}
void TestChartWidget::bubbleAttachmentThumbFitsNarrowWindow()
{
    const QString imagePath = QDir::temp().filePath(QStringLiteral("gs_bubble_attach_test.png"));
    QPixmap pixmap(120, 120);
    pixmap.fill(Qt::blue);
    QVERIFY(pixmap.save(imagePath));

    m_chart->resize(900, 820);
    m_chart->appendUserMessage(QStringLiteral("看一下这张图"),
                               QVariantList{ QVariantMap{ { QStringLiteral("name"),
                                                            QStringLiteral("shot.png") },
                                                          { QStringLiteral("kind"),
                                                            QStringLiteral("image") },
                                                          { QStringLiteral("url"), imagePath } } });
    QTest::qWait(30);

    auto *thumb = qobject_cast<QLabel *>(
        widgetByClass(m_chart.data(), QStringLiteral("attachThumb")));
    QVERIFY(thumb);
    QCOMPARE(thumb->size(), QSize(84, 84));

    m_chart->resize(560, 820);
    QTest::qWait(30);
    QCOMPARE(thumb->size(), QSize(64, 64));

    m_chart->resize(900, 820);
    QTest::qWait(30);
    QCOMPARE(thumb->size(), QSize(84, 84));

    QFile::remove(imagePath);
}

// ---------------------------------------------------------------- 历史渲染

void TestChartWidget::historyMergesTurnWithUsageAndTiming()
{
    m_chart->setHistory(historyFixture());
    QTest::qWait(20);

    // 一轮一张卡：user 一张 + assistant 一张（tool 结果回填进 assistant 卡，不另起卡）
    QList<QWidget *> cards;
    for (QWidget *frame : m_chart->findChildren<QFrame *>(QStringLiteral("turnCard")))
        cards.append(frame);
    QCOMPARE(cards.size(), 2);

    // 助手卡底部信息行：用量 pill + 用时 pill + 时刻
    QPushButton *usagePill = nullptr;
    QPushButton *timingPill = nullptr;
    for (QWidget *widget : m_chart->findChildren<QPushButton *>()) {
        if (!hasClass(widget, QStringLiteral("bubblePill")))
            continue;
        auto *button = qobject_cast<QPushButton *>(widget);
        if (button->text().contains(QStringLiteral("tokens")))
            usagePill = button;
        else if (button->text().contains(QStringLiteral("用时")))
            timingPill = button;
    }
    QVERIFY(usagePill);
    QCOMPARE(usagePill->text(), QStringLiteral("100 tokens"));
    QVERIFY(timingPill);
    QCOMPARE(timingPill->text(), QStringLiteral("用时 1.5秒"));

    const QString clock = labelText(m_chart.data(), QStringLiteral("bubbleMeta"));
    QCOMPARE(clock.size(), 8); // HH:mm:ss
    QVERIFY(clock.contains(QLatin1Char(':')));

    // 思考过程收在过程行里，摘要给正文片段
    QVERIFY(widgetByClass(m_chart.data(), QStringLiteral("procRow")));
}

void TestChartWidget::historyToolResultBackfill()
{
    m_chart->setHistory(historyFixture());
    QTest::qWait(20);

    const QList<QWidget *> items = widgetsByClass(m_chart.data(), QStringLiteral("toolItem"));
    QCOMPARE(items.size(), 1);
    QCOMPARE(labelText(items.first(), QStringLiteral("statusLabel")), QStringLiteral("完成"));

    // 工具行摘要：N 个 · 全部完成
    QVERIFY(labelTexts(m_chart.data(), QStringLiteral("procSum"))
                .filter(QStringLiteral("全部完成"))
                .size()
            == 1);
    // 结果按 JSON 缩进美化写入
    QVERIFY(labelText(items.first(), QStringLiteral("toolPre"))
                .contains(QStringLiteral("1820")));
    // 参数表（.tool-args-table 的容器）
    QVERIFY(m_chart->findChild<QWidget *>(QStringLiteral("toolArgsBox")));
}

void TestChartWidget::processAndToolHoverAccent()
{
    m_chart->setHistory(historyFixture());
    QTest::qWait(30);

    QWidget *thinkRow = nullptr;
    for (QWidget *candidate : widgetsByClass(m_chart.data(), QStringLiteral("procRow"))) {
        if (candidate->property("proc").toString() == QStringLiteral("think")) {
            thinkRow = candidate;
            break;
        }
    }
    QVERIFY(thinkRow);
    QWidget *thinkHead = widgetByClass(thinkRow, QStringLiteral("procRowHead"));
    auto *thinkSum = qobject_cast<QLabel *>(widgetByClass(thinkRow, QStringLiteral("procSum")));
    auto *thinkLabel = qobject_cast<QLabel *>(widgetByClass(thinkRow, QStringLiteral("procLabel")));
    QVERIFY(thinkHead && thinkSum && thinkLabel);

    // webui .proc-row:hover：摘要与箭头转青，.proc-label 仍保持 muted
    QCOMPARE(thinkSum->palette().color(QPalette::WindowText), QColor(QStringLiteral("#78909d")));
    QEvent enter(QEvent::HoverEnter);
    QApplication::sendEvent(thinkHead, &enter);
    QVERIFY(thinkRow->property("hovered").toBool());
    QCOMPARE(thinkSum->palette().color(QPalette::WindowText), QColor(QStringLiteral("#50badf")));
    QCOMPARE(thinkLabel->palette().color(QPalette::WindowText), QColor(QStringLiteral("#8499a6")));

    QEvent leave(QEvent::HoverLeave);
    QApplication::sendEvent(thinkHead, &leave);
    QVERIFY(!thinkRow->property("hovered").toBool());
    QCOMPARE(thinkSum->palette().color(QPalette::WindowText), QColor(QStringLiteral("#78909d")));

    // 工具项：名称随 summary 悬停转青，状态标签仍保留自己的绿色
    QWidget *toolItem = widgetByClass(m_chart.data(), QStringLiteral("toolItem"));
    QVERIFY(toolItem);
    QWidget *toolSummary = widgetByClass(toolItem, QStringLiteral("toolItemSummary"));
    auto *toolName = qobject_cast<QLabel *>(widgetByClass(toolItem, QStringLiteral("toolItemName")));
    auto *toolStatus = qobject_cast<QLabel *>(widgetByClass(toolItem, QStringLiteral("statusLabel")));
    QVERIFY(toolSummary && toolName && toolStatus);
    const QColor statusColor = toolStatus->palette().color(QPalette::WindowText);
    QCOMPARE(statusColor, QColor(QStringLiteral("#50ce91")));

    QApplication::sendEvent(toolSummary, &enter);
    QVERIFY(toolSummary->property("hovered").toBool());
    QCOMPARE(toolName->palette().color(QPalette::WindowText), QColor(QStringLiteral("#50badf")));
    QCOMPARE(toolStatus->palette().color(QPalette::WindowText), statusColor);
    QApplication::sendEvent(toolSummary, &leave);
    QVERIFY(!toolSummary->property("hovered").toBool());
    QCOMPARE(toolName->palette().color(QPalette::WindowText), QColor(QStringLiteral("#eaf6fa")));

    // 运行中的工具行优先级高于 hover：摘要保持橙色
    m_chart->appendToolCall(QStringLiteral("hover_running"),
                            QStringLiteral("power_flow"), QVariantMap());
    QTest::qWait(20);
    QWidget *toolsRow = nullptr;
    for (QWidget *candidate : widgetsByClass(m_chart.data(), QStringLiteral("procRow"))) {
        if (candidate->property("proc").toString() == QStringLiteral("tools")
            && candidate->property("running").toBool()) {
            toolsRow = candidate;
            break;
        }
    }
    QVERIFY(toolsRow);
    QVERIFY(toolsRow->property("running").toBool());
    QWidget *toolsHead = widgetByClass(toolsRow, QStringLiteral("procRowHead"));
    auto *toolsSum = qobject_cast<QLabel *>(widgetByClass(toolsRow, QStringLiteral("procSum")));
    QVERIFY(toolsHead && toolsSum);
    QCOMPARE(toolsSum->palette().color(QPalette::WindowText), QColor(QStringLiteral("#e7a84d")));
    QApplication::sendEvent(toolsHead, &enter);
    QCOMPARE(toolsSum->palette().color(QPalette::WindowText), QColor(QStringLiteral("#e7a84d")));
}

// ---------------------------------------------------------------- 气泡明细浮层

// .bubble-timing-pop/.bubble-usage-pop：left:0;bottom:calc(100% + 7px)，
// 贴按钮左缘并在其上方 7px 落底、整体向上生长
void TestChartWidget::bubbleDetailPopupAnchoring()
{
    m_chart->setHistory(historyFixture());
    QTest::qWait(30);

    QPushButton *usagePill = nullptr;
    QPushButton *timingPill = nullptr;
    for (QWidget *widget : m_chart->findChildren<QPushButton *>()) {
        if (!hasClass(widget, QStringLiteral("bubblePill")))
            continue;
        auto *button = qobject_cast<QPushButton *>(widget);
        if (button->text().contains(QStringLiteral("tokens")))
            usagePill = button;
        else if (button->text().contains(QStringLiteral("用时")))
            timingPill = button;
    }
    QVERIFY(usagePill && timingPill);

    auto openPop = [this](QWidget *pill) -> QWidget * {
        clickWidget(pill);
        QTest::qWait(30);
        for (QWidget *candidate : widgetsByClass(m_chart.data(), QStringLiteral("bubblePop"))) {
            if (candidate->isVisible())
                return candidate;
        }
        return nullptr;
    };
    auto checkAbove = [](QWidget *pop, QWidget *pill) {
        const QPoint anchor = pill->mapToGlobal(QPoint(0, 0));
        const QPoint popTop = pop->mapToGlobal(QPoint(0, 0));
        // 高度必须在 show() 之后定稿：隐藏状态量到的高度偏小会把浮层压到按钮上
        QCOMPARE(anchor.y() - (popTop.y() + pop->height()), 7);
        QVERIFY(pop->height() > 0);
    };

    QWidget *pop = openPop(usagePill);
    QVERIFY(pop);
    checkAbove(pop, usagePill);
    // 明细行的值按 .pop-row 右对齐，说明行已排布
    QVERIFY(!labelTexts(pop, QStringLiteral("popRowValue")).isEmpty());
    pop->hide();
    QTest::qWait(20);

    pop = openPop(timingPill);
    QVERIFY(pop);
    checkAbove(pop, timingPill);
    pop->hide();
    QTest::qWait(20);
}

void TestChartWidget::bubbleDetailPopupFitsNarrowHost()
{
    m_chart->resize(420, 820);
    m_chart->appendAssistantText(QStringLiteral("用量明细的窄屏回归测试。"));
    QTest::qWait(20);

    const QString longModel =
        QStringLiteral("Very Long Provider With A Model Name That Must Be Elided / "
                       "gridstar-extended-reasoning-model-with-an-extremely-long-version-name");
    QVariantMap usage;
    usage.insert(QStringLiteral("total"), 123456);
    usage.insert(QStringLiteral("input"), 100000);
    usage.insert(QStringLiteral("output"), 23456);
    usage.insert(QStringLiteral("estimated"), false);
    usage.insert(QStringLiteral("label"), QStringLiteral("123.5k tokens"));
    usage.insert(QStringLiteral("model_label"), longModel);
    m_chart->setTokenUsageDetail(usage);
    QTest::qWait(20);

    QPushButton *usagePill = nullptr;
    for (QWidget *widget : m_chart->findChildren<QPushButton *>()) {
        if (!hasClass(widget, QStringLiteral("bubblePill")))
            continue;
        auto *button = qobject_cast<QPushButton *>(widget);
        if (button && button->text().contains(QStringLiteral("tokens"))) {
            usagePill = button;
            break;
        }
    }
    QVERIFY(usagePill);

    clickWidget(usagePill);
    QTest::qWait(30);

    QWidget *pop = nullptr;
    for (QWidget *candidate : widgetsByClass(m_chart.data(), QStringLiteral("bubblePop"))) {
        if (candidate->isVisible()) {
            pop = candidate;
            break;
        }
    }
    QVERIFY(pop);

    QWidget *host = usagePill->window();
    const QRect hostRect(host->mapToGlobal(QPoint(0, 0)), host->size());
    const QRect popupRect = pop->geometry();
    QVERIFY2(hostRect.contains(popupRect),
             qPrintable(QStringLiteral("popup %1,%2 %3x%4 outside host %5,%6 %7x%8")
                            .arg(popupRect.x())
                            .arg(popupRect.y())
                            .arg(popupRect.width())
                            .arg(popupRect.height())
                            .arg(hostRect.x())
                            .arg(hostRect.y())
                            .arg(hostRect.width())
                            .arg(hostRect.height())));
    QVERIFY(popupRect.left() >= hostRect.left() + 6);
    QVERIFY(popupRect.right() <= hostRect.right() - 6);

    QLabel *modelValue = nullptr;
    for (QLabel *label : pop->findChildren<QLabel *>()) {
        if (label->toolTip() == longModel) {
            modelValue = label;
            break;
        }
    }
    QVERIFY(modelValue);
    QVERIFY(modelValue->width() > 0);
    QVERIFY(modelValue->text() != modelValue->toolTip());
    QVERIFY(modelValue->text().endsWith(QChar(0x2026)));

    QLabel *total = nullptr;
    for (QLabel *label : pop->findChildren<QLabel *>()) {
        if (!hasClass(label, QStringLiteral("popTotal")))
            continue;
        total = label;
        if (label->isVisible())
            QVERIFY(label->width() > 0);
    }
    QVERIFY(total && total->isVisible());

    for (QLabel *label : pop->findChildren<QLabel *>()) {
        if (hasClass(label, QStringLiteral("popRowValue")) && label->isVisible()) {
            QVERIFY(label->width() > 0);
            QVERIFY(!label->text().isEmpty());
        }
    }

    pop->hide();
}

// ---------------------------------------------------------------- 选择浮层

void TestChartWidget::optionsOverlayChooseAndEsc()
{
    openOptionsOverlay();

    QWidget *card = choiceCard();
    QVERIFY(card);
    QVERIFY(card->isVisible());

    // 两个模型给的选项 + 自动补的「其他」
    const QStringList names = labelTexts(card, QStringLiteral("choiceItemName"));
    QCOMPARE(names.size(), 3);
    QVERIFY(names.contains(QStringLiteral("方案A")));
    QVERIFY(names.contains(QStringLiteral("方案B")));
    QVERIFY(names.contains(QStringLiteral("其他")));

    // 点选项即确认：发出 optionChosen 并收卡
    QSignalSpy spy(m_chart.data(), &gs::ChartWidget::optionChosen);
    clickWidget(choiceItemByText(card, QStringLiteral("方案A")));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("a"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("方案A"));
    waitOverlayClosed();
    QVERIFY(!card->isVisible());
}

void TestChartWidget::optionsOverlayFreeText()
{
    openOptionsOverlay();
    QWidget *card = choiceCard();
    QVERIFY(card && card->isVisible());

    // 只有「其他」放开输入框
    QLineEdit *input = card->findChild<QLineEdit *>(QStringLiteral("choiceInput"));
    QVERIFY(input);
    QVERIFY(!input->isEnabled());

    clickWidget(choiceItemByText(card, QStringLiteral("其他")));
    QVERIFY(input->isEnabled());

    // 空文本提交被拦下（标记 invalid，不收卡）
    QSignalSpy spy(m_chart.data(), &gs::ChartWidget::optionChosen);
    QPushButton *submit = card->findChild<QPushButton *>(QStringLiteral("choiceSubmit"));
    QVERIFY(submit);
    QVERIFY(submit->isEnabled());
    submit->click();
    QCOMPARE(spy.count(), 0);
    QVERIFY(card->isVisible());
    QVERIFY(input->property("invalid").toBool());

    input->setText(QStringLiteral("改为按甩负荷方案"));
    submit->click();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("改为按甩负荷方案"));

    // Esc 收起浮层：审批卡不放行，普通询问卡放行
    openOptionsOverlay();
    QVERIFY(choiceCard()->isVisible());
    QTest::keyClick(m_chart.data(), Qt::Key_Escape);
    waitOverlayClosed();
    QVERIFY(!choiceCard()->isVisible());
}

void TestChartWidget::choiceOverlayShowsAboveComposer()
{
    QVariantMap running;
    running.insert(QStringLiteral("id"), QStringLiteral("p1"));
    running.insert(QStringLiteral("title"), QStringLiteral("running phase"));
    running.insert(QStringLiteral("status"), QStringLiteral("running"));
    QVariantMap plan;
    plan.insert(QStringLiteral("title"), QStringLiteral("phase plan"));
    plan.insert(QStringLiteral("phases"), QVariant(QVariantList{ running }));
    m_chart->setPhasePlan(plan);
    QTest::qWait(20);
    QVERIFY(m_chart->findChild<QWidget *>(QStringLiteral("phasePanel"))->isVisible());

    QWidget *composer = m_chart->findChild<QWidget *>(QStringLiteral("composer"));
    QWidget *inputWrap = m_chart->findChild<QWidget *>(QStringLiteral("inputWrap"));
    QVERIFY(composer && inputWrap);
    const int normalHeight = composer->height();
    QVERIFY(normalHeight > 80);

    openOptionsOverlay();
    QWidget *card = choiceCard();
    QVERIFY(card && card->isVisible());
    QVERIFY(!inputWrap->isVisible());
    QTest::qWait(20);

    QCOMPARE(composer->height(), normalHeight);
    QVERIFY(card->parentWidget() != composer);
    QCOMPARE(card->height(), card->sizeHint().height());

    const QRect cardRect(card->mapTo(m_chart.data(), QPoint(0, 0)), card->size());
    QVERIFY(m_chart->rect().contains(cardRect));
    QVERIFY(cardRect.top()
            < composer->mapTo(m_chart.data(), QPoint(0, 0)).y());

    const int composerBottom =
        composer->mapTo(m_chart.data(), QPoint(0, composer->height())).y();
    QCOMPARE(cardRect.y() + cardRect.height(), composerBottom - 12);

    const QList<QWidget *> items = widgetsByClass(card, QStringLiteral("choiceItem"));
    QCOMPARE(items.size(), 3);
    for (QWidget *item : items) {
        QVERIFY(item->isVisible());
        QVERIFY(cardRect.contains(
            QRect(item->mapTo(m_chart.data(), QPoint(0, 0)), item->size())));
    }
    QWidget *hint = widgetByClass(card, QStringLiteral("choiceHint"));
    QVERIFY(hint && hint->isVisible());
    QVERIFY(cardRect.contains(
        QRect(hint->mapTo(m_chart.data(), QPoint(0, 0)), hint->size())));

    QWidget *phasePanel = m_chart->findChild<QWidget *>(QStringLiteral("phasePanel"));
    QVERIFY(phasePanel && phasePanel->isVisible());
    const QRect phaseRect(phasePanel->mapTo(m_chart.data(), QPoint(0, 0)),
                          phasePanel->size());
    QVERIFY(!phaseRect.intersects(cardRect));
    QCOMPARE(cardRect.top() - (phaseRect.y() + phaseRect.height()), 8);

    // 折叠时卡片必须跟随 sizeHint 收缩；它由 Composer 绝对定位，不会自动变矮
    QWidget *head = card->findChild<QWidget *>(QStringLiteral("choiceHead"));
    QVERIFY(head && head->isVisible());
    const int expandedHeight = card->height();
    clickWidget(head);
    QTest::qWait(20);

    const int collapsedHeight = card->height();
    QVERIFY(collapsedHeight < expandedHeight);
    QCOMPARE(collapsedHeight, card->sizeHint().height());
    QCOMPARE(card->mapTo(m_chart.data(), QPoint(0, collapsedHeight)).y(),
             composerBottom - 12);
    QCOMPARE(labelText(card, QStringLiteral("choiceCaret")), QString::fromUtf8("⌃"));
    QWidget *list = widgetByClass(card, QStringLiteral("choiceList"));
    QVERIFY(list && !list->isVisible());
    QVERIFY(!hint->isVisible());
    for (QWidget *item : items)
        QVERIFY(!item->isVisible());

    const QRect collapsedRect(card->mapTo(m_chart.data(), QPoint(0, 0)), card->size());
    const QRect collapsedPhaseRect(phasePanel->mapTo(m_chart.data(), QPoint(0, 0)),
                                   phasePanel->size());
    QVERIFY(!collapsedPhaseRect.intersects(collapsedRect));
    QCOMPARE(collapsedRect.top()
                 - (collapsedPhaseRect.y() + collapsedPhaseRect.height()),
             8);

    // 再展开，内容与高度恢复
    clickWidget(head);
    QTest::qWait(20);
    QVERIFY(card->height() > collapsedHeight);
    QCOMPARE(card->height(), card->sizeHint().height());
    QVERIFY(list->isVisible());
    QVERIFY(hint->isVisible());
}

// 窄窗口 + 长问题 + 多条长描述：卡片高度必须按定稿宽度重算一次。
// 若沿用隐藏状态或旧宽度下的 sizeHint，描述文字和底部提示会被卡片底边裁掉。
void TestChartWidget::choiceOverlayFitsNarrowWindow()
{
    m_chart->resize(420, 460);
    QTest::qWait(30);

    const QString paragraph = QStringLiteral(
        "分区内多条馈线同时重载，新能源出力仍在快速上升；需要先校验联络开关与保护定值，"
        "确认转供后的电压和电流不过限，再决定下一阶段的处置顺序。");
    QVariantList options;
    for (int i = 0; i < 3; ++i) {
        options << QVariantMap{
            { QStringLiteral("label"), QStringLiteral("方案%1：调整联络开关并转移负荷").arg(i + 1) },
            { QStringLiteral("value"), QStringLiteral("plan-%1").arg(i + 1) },
            { QStringLiteral("description"), paragraph },
        };
    }
    QVariantMap payload;
    payload.insert(QStringLiteral("question"), paragraph + paragraph);
    payload.insert(QStringLiteral("options"), options);

    m_chart->showChoice(payload);
    QTest::qWait(40);

    QWidget *card = choiceCard();
    QVERIFY(card && card->isVisible());
    QWidget *composer = m_chart->findChild<QWidget *>(QStringLiteral("composer"));
    QVERIFY(composer);

    const QRect cardRect(card->mapTo(m_chart.data(), QPoint(0, 0)), card->size());
    const QRect hostRect(m_chart->rect());
    QVERIFY2(hostRect.contains(cardRect),
             qPrintable(QStringLiteral("choice card %1,%2 %3x%4 outside host %5x%6")
                            .arg(cardRect.x()).arg(cardRect.y())
                            .arg(cardRect.width()).arg(cardRect.height())
                            .arg(hostRect.width()).arg(hostRect.height())));

    const int composerBottom =
        composer->mapTo(m_chart.data(), QPoint(0, composer->height())).y();
    QCOMPARE(cardRect.y() + cardRect.height(), composerBottom - 12);

    // 内容比可视区高时正文自己滚动：选项必须完整躺在滚动内容里（不被压扁/裁掉），
    // 并且能滚到最后一项，而不是被卡片底边吃掉一半。
    QScrollArea *scroll = card->findChild<QScrollArea *>(QStringLiteral("choiceScroll"));
    QVERIFY(scroll);
    QWidget *body = scroll->widget();
    QVERIFY(body);
    // 卡片按正文自然高排布：放得下就不该有滚动条，放不下（宿主太矮）必须能滚动，
    // 两种情况下选项都不能被卡片底边裁掉（对齐 webui：内容高时 .choice-list 也不滚动，整卡长到宿主上限）
    if (body->height() > scroll->viewport()->height()) {
        QVERIFY2(scroll->verticalScrollBar()->maximum() > 0,
                 qPrintable(QStringLiteral("choice body %1 overflows viewport %2, scrollbar missing")
                                .arg(body->height()).arg(scroll->viewport()->height())));
    } else {
        QCOMPARE(scroll->verticalScrollBar()->maximum(), 0);
    }

    // 宿主再矮一截：卡片被宿主上限截断，正文必须转成滚动而不是把选项裁掉
    m_chart->resize(420, 360);
    QTest::qWait(40);
    {
        const QRect narrowRect(card->mapTo(m_chart.data(), QPoint(0, 0)), card->size());
        QVERIFY2(m_chart->rect().contains(narrowRect),
                 qPrintable(QStringLiteral("narrow choice card %1,%2 %3x%4 outside host %5x%6")
                                .arg(narrowRect.x()).arg(narrowRect.y())
                                .arg(narrowRect.width()).arg(narrowRect.height())
                                .arg(m_chart->width()).arg(m_chart->height())));
        const int narrowComposerBottom =
            composer->mapTo(m_chart.data(), QPoint(0, composer->height())).y();
        QCOMPARE(narrowRect.y() + narrowRect.height(), narrowComposerBottom - 12);
        if (body->height() > scroll->viewport()->height()) {
            QVERIFY2(scroll->verticalScrollBar()->maximum() > 0,
                     qPrintable(QStringLiteral("capped card: body %1 over viewport %2, scrollbar missing")
                                    .arg(body->height()).arg(scroll->viewport()->height())));
        }
        const QList<QWidget *> narrowItems = widgetsByClass(card, QStringLiteral("choiceItem"));
        QVERIFY(!narrowItems.isEmpty());
        const QWidget *narrowLast = narrowItems.last();
        const int narrowLastBottom =
            narrowLast->mapTo(body, QPoint(0, narrowLast->height())).y();
        QVERIFY(body->height() >= narrowLastBottom);
    }

    const QList<QWidget *> items = widgetsByClass(card, QStringLiteral("choiceItem"));
    QCOMPARE(items.size(), options.size() + 1); // 末尾自动补「其他」
    for (QWidget *item : items) {
        QVERIFY(item->isVisible());
        const QRect itemRect(item->mapTo(body, QPoint(0, 0)), item->size());
        QVERIFY2(body->rect().contains(itemRect),
                 qPrintable(QStringLiteral("choice item %1,%2 %3x%4 outside body %5x%6")
                                .arg(itemRect.x()).arg(itemRect.y())
                                .arg(itemRect.width()).arg(itemRect.height())
                                .arg(body->width()).arg(body->height())));
        for (QLabel *label : item->findChildren<QLabel *>()) {
            const int needed = label->heightForWidth(label->width());
            if (label->text().isEmpty() || needed < 0)
                continue;
            QVERIFY2(label->height() >= needed,
                     qPrintable(QStringLiteral("clipped label '%1': h=%2 need=%3 w=%4")
                                    .arg(label->text().left(12)).arg(label->height())
                                    .arg(needed).arg(label->width())));
        }
    }
    QWidget *lastItem = items.last();
    const int lastBottom = lastItem->mapTo(body, QPoint(0, lastItem->height())).y();
    QVERIFY2(body->height() >= lastBottom,
             qPrintable(QStringLiteral("scrollable body %1 shorter than last item bottom %2")
                            .arg(body->height()).arg(lastBottom)));

    QWidget *hint = widgetByClass(card, QStringLiteral("choiceHint"));
    QVERIFY(hint && hint->isVisible());
    const QRect hintRect(hint->mapTo(card, QPoint(0, 0)), hint->size());
    QVERIFY(card->rect().contains(hintRect));

    QWidget *submit = card->findChild<QWidget *>(QStringLiteral("choiceSubmit"));
    QVERIFY(submit && submit->isVisible());
    const QRect submitRect(submit->mapTo(card, QPoint(0, 0)), submit->size());
    QVERIFY(card->rect().contains(submitRect));
}

void TestChartWidget::toolParamsOverlaySubmit()
{
    const QString content = QStringLiteral(
        "```json\n{\"tool_params\":{\"tool\":\"power_flow\","
        "\"params\":[{\"name\":\"case\",\"value\":\"城南线\",\"description\":\"算例\"},"
        "{\"name\":\"topology\",\"value\":{\"lk07\":\"closed\"},\"description\":\"拓扑\"}]}}\n```");
    m_chart->appendAssistantMessage(content);
    QTest::qWait(20);

    QWidget *card = choiceCard();
    QVERIFY(card && card->isVisible());

    // 参数表：字符串用单行、对象用多行
    QCOMPARE(card->findChildren<QPlainTextEdit *>().size(), 1);
    bool hasCaseField = false;
    for (QLineEdit *edit : card->findChildren<QLineEdit *>())
        if (edit->text() == QStringLiteral("城南线"))
            hasCaseField = true;
    QVERIFY(hasCaseField);

    // 参数块没有 options 时补「确认执行 / 取消」
    const QStringList names = labelTexts(card, QStringLiteral("choiceItemName"));
    QVERIFY(names.contains(QStringLiteral("确认执行")));
    QVERIFY(names.contains(QStringLiteral("取消")));

    // 提交：按 webui 口径打包成一轮结构化消息发出
    QSignalSpy spy(m_chart.data(), &gs::ChartWidget::sendMessage);
    clickWidget(choiceItemByText(card, QStringLiteral("确认执行")));
    QCOMPARE(spy.count(), 1);
    const QString message = spy.at(0).at(0).toString();
    QVERIFY(message.startsWith(QStringLiteral("<structured_interaction>")));
    QVERIFY(message.contains(QStringLiteral("\"tool_params_confirmed\"")));
    QVERIFY(message.contains(QStringLiteral("\"confirmed\":true")));
    QVERIFY(message.contains(QStringLiteral("\"lk07\":\"closed\"")));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("确认执行"));
    waitOverlayClosed();
    QVERIFY(!card->isVisible());
}

void TestChartWidget::approvalOverlayRoundTrip()
{
    QVariantMap schema;
    QVariantMap properties;
    properties.insert(QStringLiteral("line"),
                      QVariantMap{ { QStringLiteral("type"), QStringLiteral("string") },
                                   { QStringLiteral("description"), QStringLiteral("线路名") } });
    schema.insert(QStringLiteral("properties"), properties);
    schema.insert(QStringLiteral("required"), QVariant(QVariantList{ QStringLiteral("line") }));

    QVariantMap event;
    event.insert(QStringLiteral("name"), QStringLiteral("switch_order"));
    event.insert(QStringLiteral("call_id"), QStringLiteral("ap1"));
    event.insert(QStringLiteral("args"), QVariantMap{ { QStringLiteral("line"),
                                                       QStringLiteral("城南线") } });
    event.insert(QStringLiteral("schema"), schema);
    m_chart->appendApproval(event);
    QTest::qWait(20);

    QWidget *card = choiceCard();
    QVERIFY(card && card->isVisible());
    // 审批只有批准/拒绝，没有「其他」也没有关闭入口
    const QStringList names = labelTexts(card, QStringLiteral("choiceItemName"));
    QCOMPARE(names.size(), 2);
    QVERIFY(names.contains(QStringLiteral("批准")));
    QVERIFY(names.contains(QStringLiteral("拒绝")));

    // 批准：连同（可编辑的）参数一起回执
    QSignalSpy spy(m_chart.data(), &gs::ChartWidget::approvalDecided);
    clickWidget(choiceItemByText(card, QStringLiteral("批准")));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("ap1"));
    QCOMPARE(spy.at(0).at(1).toBool(), true);
    QCOMPARE(spy.at(0).at(2).toMap().value(QStringLiteral("line")).toString(),
             QStringLiteral("城南线"));

    // 宿主 POST 成功后回收审批卡
    m_chart->resolveApproval(QStringLiteral("ap1"), true);
    waitOverlayClosed();
    QVERIFY(!card->isVisible());
}

// ---------------------------------------------------------------- 卡片

void TestChartWidget::workflowProposalRun()
{
    const QString content = QStringLiteral(
        "```json\n{\"workflow\":{\"steps\":["
        "{\"tool\":\"scada_snapshot\",\"desc\":\"读取断面\",\"params\":{\"feeder\":\"城南线\"}},"
        "{\"tool\":\"power_flow\",\"desc\":\"潮流校核\",\"params\":{}}]}}\n```");
    m_chart->appendAssistantMessage(content);
    QTest::qWait(20);

    QPushButton *run = nullptr;
    for (QWidget *widget : m_chart->findChildren<QPushButton *>()) {
        if (auto *button = qobject_cast<QPushButton *>(widget))
            if (button->text() == QStringLiteral("执行工作流"))
                run = button;
    }
    QVERIFY(run);
    QVERIFY(run->isEnabled());

    QSignalSpy spy(m_chart.data(), &gs::ChartWidget::workflowRunRequested);
    run->click();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toList().size(), 2);
    // 触发后按钮禁用，避免重复下发
    QVERIFY(!run->isEnabled());
}

void TestChartWidget::phasePlanCollapsesWhenComplete()
{
    QVariantMap done;
    done.insert(QStringLiteral("id"), QStringLiteral("p1"));
    done.insert(QStringLiteral("title"), QStringLiteral("读取断面"));
    done.insert(QStringLiteral("status"), QStringLiteral("done"));
    QVariantMap skipped;
    skipped.insert(QStringLiteral("id"), QStringLiteral("p2"));
    skipped.insert(QStringLiteral("title"), QStringLiteral("潮流校核"));
    skipped.insert(QStringLiteral("status"), QStringLiteral("skipped"));
    QVariantMap plan;
    plan.insert(QStringLiteral("title"), QStringLiteral("故障处置阶段"));
    plan.insert(QStringLiteral("phases"), QVariant(QVariantList{ done, skipped }));

    m_chart->setPhasePlan(plan);
    QTest::qWait(20);
    QWidget *panel = m_chart->findChild<QWidget *>(QStringLiteral("phasePanel"));
    QVERIFY(panel);
    QVERIFY(panel->isVisible());

    // 本轮结束时计划已全部进入终态 → 计划窗口收起
    m_chart->appendAssistantText(QStringLiteral("处置完成。"));
    m_chart->finishAssistant();
    QTest::qWait(20);
    QVERIFY(!panel->isVisible());

    // 仍在执行中的计划不会被收起
    QVariantMap running;
    running.insert(QStringLiteral("id"), QStringLiteral("p3"));
    running.insert(QStringLiteral("status"), QStringLiteral("running"));
    QVariantMap plan2;
    plan2.insert(QStringLiteral("title"), QStringLiteral("处置阶段"));
    plan2.insert(QStringLiteral("phases"), QVariant(QVariantList{ done, running }));
    m_chart->setPhasePlan(plan2);
    QTest::qWait(20);
    QVERIFY(panel->isVisible());
    m_chart->appendAssistantText(QStringLiteral("继续。"));
    m_chart->finishAssistant();
    QTest::qWait(20);
    QVERIFY(panel->isVisible());
}

// ---------------------------------------------------------------- 轨迹 / 会话

void TestChartWidget::trajectoryViewTabSwitch()
{
    QVariantList events;
    events << QVariantMap{ { QStringLiteral("type"), QStringLiteral("user") },
                           { QStringLiteral("turn"), 1 },
                           { QStringLiteral("content"), QStringLiteral("城南线跳闸") },
                           { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:04:00") } }
           << QVariantMap{ { QStringLiteral("type"), QStringLiteral("traj_request_start") },
                           { QStringLiteral("turn"), 1 },
                           { QStringLiteral("request"), 1 },
                           { QStringLiteral("model"), QStringLiteral("gridstar/gs-pro-32k") },
                           { QStringLiteral("start"), QStringLiteral("2026-09-06T09:04:00") } }
           << QVariantMap{ { QStringLiteral("type"), QStringLiteral("traj_request_end") },
                           { QStringLiteral("turn"), 1 },
                           { QStringLiteral("request"), 1 },
                           { QStringLiteral("status"), QStringLiteral("completed") },
                           { QStringLiteral("content"), QStringLiteral("建议先合 lk07 转供。") },
                           { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:04:21") } };
    m_chart->setTrajectoryEvents(events);

    QWidget *traj = m_chart->findChild<QWidget *>(QStringLiteral("trajectoryView"));
    QWidget *messages = m_chart->findChild<QWidget *>(QStringLiteral("messages"));
    QWidget *composer = m_chart->findChild<QWidget *>(QStringLiteral("composer"));
    QVERIFY(traj && messages && composer);
    QVERIFY(messages->isVisible());
    QVERIFY(!traj->isVisible());

    QSignalSpy spy(m_chart.data(), &gs::ChartWidget::viewTabChanged);
    m_chart->setViewTab(QStringLiteral("traj"));
    QTest::qWait(20);
    QCOMPARE(m_chart->viewTab(), QStringLiteral("traj"));
    QCOMPARE(spy.count(), 1);
    QVERIFY(traj->isVisible());
    QVERIFY(!messages->isVisible());
    QVERIFY(!composer->isVisible());

    // 轨迹账本里应有一行用户消息 + 一行助手请求
    QVERIFY(widgetsByClass(traj, QStringLiteral("trajRow")).size() >= 2);

    m_chart->setViewTab(QStringLiteral("chat"));
    QTest::qWait(20);
    QVERIFY(!traj->isVisible());
    QVERIFY(messages->isVisible());
    QVERIFY(composer->isVisible());

    // 轮次分组下的追加（含乱序）：增量追加的结果必须与「把同样的 events 全量重新加载」一致。
    // 乱序记录会落在已有的分组里（分组按「连续段」归并），这条路径上增量追加会主动退回全量重建
    m_chart->setViewTab(QStringLiteral("traj"));
    QPushButton *turnButton = nullptr;
    for (QPushButton *button : traj->findChildren<QPushButton *>()) {
        if (hasClass(button, QStringLiteral("trajViewButton"))
            && button->text() == QStringLiteral("轮次")) {
            turnButton = button;
        }
    }
    QVERIFY(turnButton);
    turnButton->click();
    QTest::qWait(50);

    const QVariantMap turnTwo{ { QStringLiteral("type"), QStringLiteral("user") },
                               { QStringLiteral("turn"), 2 },
                               { QStringLiteral("content"), QStringLiteral("第 2 轮") },
                               { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:30:00") } };
    // 乱序：第 1 轮的记录出现在第 2 轮之后
    const QVariantMap lateFirst{ { QStringLiteral("type"), QStringLiteral("user") },
                                 { QStringLiteral("turn"), 1 },
                                 { QStringLiteral("content"), QStringLiteral("补一条第 1 轮的记录") },
                                 { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:31:00") } };
    m_chart->appendTrajectoryEvents(QVariantList{ turnTwo });
    QTest::qWait(30);
    m_chart->appendTrajectoryEvents(QVariantList{ lateFirst });
    QTest::qWait(30);
    const QStringList incremental = ledgerShape(traj);

    events << turnTwo << lateFirst; // 全量重建同样的 events
    m_chart->setTrajectoryEvents(events);
    QTest::qWait(30);
    QCOMPARE(ledgerShape(traj), incremental);
    QVERIFY(incremental.size() >= 5); // 两个分组头 + 数据行都在
}

void TestChartWidget::sessionBadgeStatus()
{
    m_chart->setSessions(QVariantList{
        QVariantMap{ { QStringLiteral("id"), QStringLiteral("s1") },
                     { QStringLiteral("title"), QStringLiteral("城南线处置") },
                     { QStringLiteral("status"), QStringLiteral("running") },
                     { QStringLiteral("updated_at"), QStringLiteral("2026-09-06T09:04:00") } },
        QVariantMap{ { QStringLiteral("id"), QStringLiteral("s2") },
                     { QStringLiteral("title"), QStringLiteral("已完成的会话") },
                     { QStringLiteral("status"), QStringLiteral("done") },
                     { QStringLiteral("updated_at"), QStringLiteral("2026-09-06T08:00:00") } },
        // 缺 status 时回退到服务端布尔字段
        QVariantMap{ { QStringLiteral("id"), QStringLiteral("s3") },
                     { QStringLiteral("title"), QStringLiteral("待确认的会话") },
                     { QStringLiteral("waiting"), true } },
    });
    QTest::qWait(20);

    QStringList badges;
    for (QLabel *badge : m_chart->findChildren<QLabel *>()) {
        if (!hasClass(badge, QStringLiteral("sessionBadge")))
            continue;
        badges << QStringLiteral("%1:%2").arg(badge->property("status").toString(), badge->text());
    }
    QVERIFY(badges.contains(QStringLiteral("running:进行中")));
    QVERIFY(badges.contains(QStringLiteral("done:已完成")));
    QVERIFY(badges.contains(QStringLiteral("waiting:待确认")));
}

void TestChartWidget::toolArgsTableAndResultFormat()
{
    m_chart->appendToolCall(QStringLiteral("c9"), QStringLiteral("power_flow"),
                            QVariantMap{ { QStringLiteral("case"), QStringLiteral("城南线") },
                                         { QStringLiteral("topology"),
                                           QVariantMap{ { QStringLiteral("lk07"),
                                                          QStringLiteral("closed") } } } });
    QTest::qWait(10);

    QWidget *item = widgetByClass(m_chart.data(), QStringLiteral("toolItem"));
    QVERIFY(item);
    QCOMPARE(labelText(item, QStringLiteral("statusLabel")), QStringLiteral("执行中"));
    // 参数表：参数名 + 结构化值缩进展开
    QVERIFY(item->findChild<QWidget *>(QStringLiteral("toolArgsBox")));
    QVERIFY(labelTexts(item, QStringLiteral("toolArgsKey")).contains(QStringLiteral("case")));
    QVERIFY(labelTexts(item, QStringLiteral("toolArgsValue"))
                .filter(QStringLiteral("lk07"))
                .size()
            == 1);

    // 结果：JSON 字符串缩进美化
    m_chart->appendToolResult(QStringLiteral("c9"), QStringLiteral("power_flow"),
                              QStringLiteral("{\"ok\":true,\"load\":1.07}"));
    QTest::qWait(10);
    QCOMPARE(labelText(item, QStringLiteral("statusLabel")), QStringLiteral("完成"));
    QVERIFY(labelText(item, QStringLiteral("toolPre")).contains(QStringLiteral("\"ok\": true")));

    // 超长内容不撑爆卡片：参数表与结果都在限高滚动容器里（CSS max-height 180/160）
    bool argsInScroll = false;
    bool resultInScroll = false;
    for (QScrollArea *scroll : item->findChildren<QScrollArea *>()) {
        QWidget *content = scroll->widget();
        if (!content)
            continue;
        if (content->objectName() == QStringLiteral("toolArgsBox"))
            argsInScroll = true;
        else if (hasClass(content, QStringLiteral("toolPre")))
            resultInScroll = true;
        QVERIFY(scroll->maximumHeight() > 0);
    }
    QVERIFY(argsInScroll);
    QVERIFY(resultInScroll);

    // 结果文本带 error / denied 时判为失败
    m_chart->appendToolCall(QStringLiteral("c10"), QStringLiteral("switch_order"), QVariantMap());
    m_chart->appendToolResult(QStringLiteral("c10"), QStringLiteral("switch_order"),
                              QStringLiteral("Tool execution denied by operator"));
    QTest::qWait(10);
    QWidget *failed = nullptr;
    for (QWidget *candidate : widgetsByClass(m_chart.data(), QStringLiteral("toolItem")))
        if (labelText(candidate, QStringLiteral("statusLabel")) == QStringLiteral("失败"))
            failed = candidate;
    QVERIFY(failed);
}

void TestChartWidget::phasePlanSurvivesViewTabSwitch()
{
    QVariantMap done;
    done.insert(QStringLiteral("id"), QStringLiteral("p1"));
    done.insert(QStringLiteral("status"), QStringLiteral("done"));
    QVariantMap running;
    running.insert(QStringLiteral("id"), QStringLiteral("p2"));
    running.insert(QStringLiteral("status"), QStringLiteral("running"));
    QVariantMap plan;
    plan.insert(QStringLiteral("title"), QStringLiteral("处置阶段"));
    plan.insert(QStringLiteral("phases"), QVariant(QVariantList{ done, running }));
    m_chart->setPhasePlan(plan);
    QTest::qWait(20);

    QWidget *panel = m_chart->findChild<QWidget *>(QStringLiteral("phasePanel"));
    QVERIFY(panel);
    QVERIFY(panel->isVisible());

    // 切到轨迹：计划窗口收起（它只服务对话里的执行过程）
    m_chart->setViewTab(QStringLiteral("traj"));
    QTest::qWait(20);
    QVERIFY(!panel->isVisible());

    // 切回对话：未跑完的计划必须恢复出来（不能被"父级隐藏"误判成没计划）
    m_chart->setViewTab(QStringLiteral("chat"));
    QTest::qWait(20);
    QVERIFY(panel->isVisible());
}

void TestChartWidget::askUserToolCallIsNotRendered()
{
    m_chart->appendAssistantText(QStringLiteral("先确认一下口径。"));
    // 询问类工具调用不落成工具条目（app.js ASK_USER_TOOL），改由输入框上方的浮层承担
    m_chart->appendToolCall(QStringLiteral("ask1"), QStringLiteral("ask_user_question"),
                            QVariantMap{
                                { QStringLiteral("question"), QStringLiteral("选一个") },
                                { QStringLiteral("options"),
                                  QVariant(QVariantList{ QVariantMap{ { QStringLiteral("label"),
                                                                        QStringLiteral("A") } } }) } });
    m_chart->appendToolResult(QStringLiteral("ask1"), QStringLiteral("ask_user_question"),
                              QStringLiteral("{}"));
    QTest::qWait(20);

    QVERIFY(widgetsByClass(m_chart.data(), QStringLiteral("toolItem")).isEmpty());
    QVERIFY(widgetsByClass(m_chart.data(), QStringLiteral("procRow")).isEmpty());
    // 正文照常保留（turnCard 是 objectName，不是 class 属性）
    QVERIFY(!m_chart->findChildren<QFrame *>(QStringLiteral("turnCard")).isEmpty());
}

void TestChartWidget::usageModelLabelMapping()
{
    m_chart->setModels(QVariantList{ QVariantMap{
        { QStringLiteral("key"), QStringLiteral("custom/gs-mini") },
        { QStringLiteral("provider"), QStringLiteral("custom") },
        { QStringLiteral("provider_name"), QStringLiteral("我的供应商") },
        { QStringLiteral("id"), QStringLiteral("gs-mini") },
        { QStringLiteral("name"), QStringLiteral("GS-Mini") },
        { QStringLiteral("enabled"), true },
        { QStringLiteral("provider_enabled"), true } } });

    m_chart->appendAssistantText(QStringLiteral("已生成处置方案。"));
    QVariantMap usage;
    usage.insert(QStringLiteral("total"), 1200);
    usage.insert(QStringLiteral("input"), 900);
    usage.insert(QStringLiteral("output"), 300);
    usage.insert(QStringLiteral("estimated"), false);
    usage.insert(QStringLiteral("model"), QStringLiteral("custom/gs-mini")); // 后端回传 provider ID
    m_chart->setTokenUsageDetail(usage);
    QTest::qWait(20);

    QPushButton *pill = nullptr;
    for (QWidget *widget : m_chart->findChildren<QPushButton *>()) {
        if (!hasClass(widget, QStringLiteral("bubblePill")))
            continue;
        if (auto *button = qobject_cast<QPushButton *>(widget))
            if (button->text().contains(QStringLiteral("tokens")))
                pill = button;
    }
    QVERIFY(pill);
    QCOMPARE(pill->text(), QStringLiteral("1,200 tokens"));

    clickWidget(pill); // 打开用量明细弹层
    QTest::qWait(20);
    // 「提供方 / 模型」把 provider ID 自动换成供应商名称（app.js usageModelLabel）
    const QStringList values = fullLabelTexts(m_chart.data(), QStringLiteral("popRowValue"));
    QVERIFY(values.contains(QStringLiteral("我的供应商 / gs-mini")));
    QVERIFY(values.contains(QStringLiteral("300"))); // 输出行：无推理 token 时只给数字
}

void TestChartWidget::letterSpacingApplied()
{
    // QSS 不支持 letter-spacing：由 ChartWidget 在控件 polish 时补上（.bubble-meta = .4px）
    m_chart->appendAssistantText(QStringLiteral("已生成处置方案。"));
    QTest::qWait(50);

    auto *meta = qobject_cast<QLabel *>(widgetByClass(m_chart.data(), QStringLiteral("bubbleMeta")));
    QVERIFY(meta);
    QCOMPARE(meta->font().letterSpacingType(), QFont::AbsoluteSpacing);
    // Qt 会把字距量化（.4px 回读成 0.390625），所以用容差而不是精确比较
    QVERIFY(qAbs(meta->font().letterSpacing() - 0.4) < 0.02);

    // 未在表里的控件不受影响
    QWidget *body = widgetByClass(m_chart.data(), QStringLiteral("markdownView"));
    QVERIFY(body);
    QVERIFY(body->font().letterSpacingType() != QFont::AbsoluteSpacing
            || qAbs(body->font().letterSpacing()) < 0.001);
}

void TestChartWidget::trajectoryRowElides()
{
    // 300 字的长文本：账本行只能占一行，必须省略而不是硬切（webui .traj-row-preview）
    const QString longText(300, QChar(0x5feb)); // 「快」×300
    QVariantList events;
    events << QVariantMap{ { QStringLiteral("type"), QStringLiteral("user") },
                           { QStringLiteral("turn"), 1 },
                           { QStringLiteral("content"), longText },
                           { QStringLiteral("display_content"), longText },
                           { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:04:00") } };
    m_chart->setTrajectoryEvents(events);
    m_chart->setViewTab(QStringLiteral("traj"));
    QTest::qWait(50);

    QWidget *traj = m_chart->findChild<QWidget *>(QStringLiteral("trajectoryView"));
    QVERIFY(traj);
    auto *preview = qobject_cast<QLabel *>(widgetByClass(traj, QStringLiteral("trajRowPreview")));
    QVERIFY(preview);
    // 省略标签保留全文（toolTip），显示文本被省略并带省略号
    QVERIFY(!preview->toolTip().isEmpty());
    QVERIFY(preview->text().length() < preview->toolTip().length());
    QVERIFY(preview->text().endsWith(QChar(0x2026)));
}

void TestChartWidget::trajectoryInspectorResponsive()
{
    QVariantList events;
    events << QVariantMap{ { QStringLiteral("type"), QStringLiteral("user") },
                           { QStringLiteral("turn"), 1 },
                           { QStringLiteral("content"), QStringLiteral("城南线跳闸") },
                           { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:04:00") } }
           << QVariantMap{ { QStringLiteral("type"), QStringLiteral("traj_request_start") },
                           { QStringLiteral("turn"), 1 },
                           { QStringLiteral("request"), 1 },
                           { QStringLiteral("start"), QStringLiteral("2026-09-06T09:04:00") } }
           << QVariantMap{ { QStringLiteral("type"), QStringLiteral("traj_request_end") },
                           { QStringLiteral("turn"), 1 },
                           { QStringLiteral("request"), 1 },
                           { QStringLiteral("status"), QStringLiteral("completed") },
                           { QStringLiteral("content"), QStringLiteral("建议先合 lk07。") },
                           { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:04:21") } };
    m_chart->setTrajectoryEvents(events);
    m_chart->setViewTab(QStringLiteral("traj"));
    QTest::qWait(50);

    QWidget *traj = m_chart->findChild<QWidget *>(QStringLiteral("trajectoryView"));
    QWidget *inspector = m_chart->findChild<QWidget *>(QStringLiteral("trajInspector"));
    QLineEdit *search = m_chart->findChild<QLineEdit *>(QStringLiteral("trajSearch"));
    QVERIFY(traj && inspector && search);

    // 选中一行 → 详情面板出现
    QWidget *row = widgetByClass(traj, QStringLiteral("trajRow"));
    QVERIFY(row);
    clickWidget(row);
    QTest::qWait(50);
    QVERIFY(inspector->isVisible());

    // webui @media(max-width:760px)：宽屏 380 / 中屏 300
    m_chart->resize(900, 820);
    QTest::qWait(50);
    QCOMPARE(inspector->width(), 380);
    QCOMPARE(search->width(), 220);
    m_chart->resize(761, 820);
    QTest::qWait(50);
    QCOMPARE(inspector->width(), 380);
    QCOMPARE(search->width(), 220);
    m_chart->resize(760, 820);
    QTest::qWait(50);
    QCOMPARE(inspector->width(), 300);
    QCOMPARE(search->width(), 140);

    // 560 是覆盖式的边界：561 仍走 760 档的内嵌布局
    QWidget *body = inspector->parentWidget();
    QVERIFY(body);
    m_chart->resize(561, 820);
    QTest::qWait(70);
    QCOMPARE(inspector->width(), 300);
    QCOMPARE(search->width(), 140);
    QVERIFY(body->layout() && body->layout()->indexOf(inspector) >= 0);

    // webui @media(max-width:560px)：88vw 覆盖式浮层（脱离布局）
    m_chart->resize(560, 820);
    QTest::qWait(70);
    QCOMPARE(inspector->width(), 493); // 560 * 0.88
    QVERIFY(inspector->isVisible());
    QVERIFY(!body->layout() || body->layout()->indexOf(inspector) < 0);

    // 回到宽屏：放回布局并恢复宽度
    m_chart->resize(900, 820);
    QTest::qWait(70);
    QCOMPARE(inspector->width(), 380);
    QVERIFY(inspector->parentWidget()->layout()->indexOf(inspector) >= 0);
}

// style.css @media(max-width:640px)：设置中心侧栏 210→118、.form-grid 双列→单列
void TestChartWidget::settingsDialogCompactLayout()
{
    const QVariantMap config = settingsDraftFixture();
    m_chart->setConfigLoaded(true);
    m_chart->setSettingsDraft(config, 1);
    m_chart->openSettings(QStringLiteral("models"));
    QTest::qWait(50);

    auto *dialog = qobject_cast<QDialog *>(m_chart->findChild<QWidget *>(QStringLiteral("settingsDialog")));
    QVERIFY(dialog);
    QWidget *sidebar = dialog->findChild<QWidget *>(QStringLiteral("providerSidebar"));
    auto *providerGrid = dialog->findChild<QGridLayout *>(QStringLiteral("providerFormGrid"));
    auto *modelGrid = dialog->findChild<QGridLayout *>(QStringLiteral("modelFormGrid"));
    QVERIFY(sidebar);
    QVERIFY(providerGrid);
    QVERIFY(modelGrid);

    int row = -1;
    int col = -1;
    int rowSpan = 0;
    int colSpan = 0;

    // 宽屏：侧栏 210，第二个字段落在第一行第二列
    dialog->resize(900, 700);
    QTest::qWait(50);
    QCOMPARE(sidebar->width(), 210);
    QVERIFY(!dialog->property("compact").toBool());
    providerGrid->getItemPosition(1, &row, &col, &rowSpan, &colSpan);
    QCOMPARE(row, 0);
    QCOMPARE(col, 1);
    modelGrid->getItemPosition(1, &row, &col, &rowSpan, &colSpan);
    QCOMPARE(row, 0);
    QCOMPARE(col, 1);

    // 紧凑：侧栏 118，第二个字段换到第二行第一列
    dialog->resize(620, 700);
    QTest::qWait(50);
    QCOMPARE(sidebar->width(), 118);
    // style.css @media(max-width:640px)：.settings-dialog{border:0;border-radius:0}
    QVERIFY(dialog->property("compact").toBool());
    QVERIFY(m_chart->styleSheet().contains(
        QStringLiteral("QDialog#settingsDialog[compact=\"true\"]")));
    providerGrid->getItemPosition(1, &row, &col, &rowSpan, &colSpan);
    QCOMPARE(row, 1);
    QCOMPARE(col, 0);
    modelGrid->getItemPosition(1, &row, &col, &rowSpan, &colSpan);
    QCOMPARE(row, 1);
    QCOMPARE(col, 0);
}

// .confirm-dialog{width:92vw;max-width:380px}：设置中心最小宽 360（<380），
// 固定 380 的确认框会横向溢出父窗口
void TestChartWidget::confirmDialogFitsNarrowSettings()
{
    m_chart->setConfigLoaded(true);
    m_chart->setSettingsDraft(settingsDraftFixture(), 1);
    m_chart->openSettings(QStringLiteral("models"));
    QTest::qWait(50);

    auto *settings =
        qobject_cast<QDialog *>(m_chart->findChild<QWidget *>(QStringLiteral("settingsDialog")));
    QVERIFY(settings);
    // 最小宽必须能到 360，否则这个回归在 ChartWidget（最小宽 420）下不可达
    settings->resize(360, 700);
    QTest::qWait(50);
    QCOMPARE(settings->width(), 360);

    // 制造脏状态：改 provider 名字（textChanged -> markDirty）
    QLineEdit *nameEdit = nullptr;
    for (QLineEdit *edit : settings->findChildren<QLineEdit *>()) {
        if (edit->text() == QStringLiteral("GridStar 网关")) {
            nameEdit = edit;
            break;
        }
    }
    QVERIFY(nameEdit);
    nameEdit->setText(QStringLiteral("GridStar 网关 2"));

    // 确认框走模态 exec()：用 0 延时定时器在嵌套事件循环里量宽并收掉
    QPointer<QWidget> host(settings);
    auto confirmWidth = QSharedPointer<int>::create(0);
    QTimer::singleShot(0, m_chart.data(), [host, confirmWidth] {
        if (!host)
            return;
        QWidget *confirm = host->findChild<QWidget *>(QStringLiteral("confirmDialog"));
        if (!confirm)
            return;
        *confirmWidth = confirm->width();
        confirm->close();
    });
    m_chart->closeSettings();

    QVERIFY2(*confirmWidth > 0, "未捕获到二次确认框");
    QVERIFY(*confirmWidth <= 380);
    QVERIFY(*confirmWidth <= qRound(360 * 0.92));
}

void TestChartWidget::hoverRevealsCopyButton()
{
    m_chart->appendUserMessage(QStringLiteral("城南线跳闸"));
    QTest::qWait(50);

    QWidget *message = widgetByClass(m_chart.data(), QStringLiteral("messageWidget"));
    QVERIFY(message);
    QWidget *copy = widgetByClass(message, QStringLiteral("bubbleCopy"));
    QVERIFY(copy);

    // .bubble-copy { opacity:.5 }；卡片悬浮时才提亮到 1（QSS 做不到父 :hover 驱动子选择器）
    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(copy->graphicsEffect());
    QVERIFY(effect);
    QVERIFY(qAbs(effect->opacity() - 0.5) < 0.001);

    QEvent enter(QEvent::Enter);
    QApplication::sendEvent(message, &enter);
    QVERIFY(qAbs(effect->opacity() - 1.0) < 0.001);

    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(message, &leave);
    QVERIFY(qAbs(effect->opacity() - 0.5) < 0.001);
}

void TestChartWidget::overlayAndCardTransitions()
{
    // 浮层入场：挂 opacity 效果，动画结束后完全不透明
    openOptionsOverlay();
    QWidget *card = choiceCard();
    QVERIFY(card && card->isVisible());
    auto *cardEffect = qobject_cast<QGraphicsOpacityEffect *>(card->graphicsEffect());
    QVERIFY(cardEffect);
    QTest::qWait(250); // .18s 入场
    QVERIFY(cardEffect->opacity() > 0.99);
    // 入场跑完就把 effect 关掉：常驻会让这张卡每帧多一次离屏合成
    QVERIFY(!cardEffect->isEnabled());

    // 离场：先淡出、动画结束才隐藏并清空（240ms，对齐 webui app.js:842）
    m_chart->closeChoice();
    QTest::qWait(60);
    QVERIFY(cardEffect->isEnabled());
    QVERIFY(cardEffect->opacity() < 0.99);
    waitOverlayClosed();
    QVERIFY(!card->isVisible());
    QVERIFY(!cardEffect->isEnabled());

    // 卡片入场用一次性 opacity 效果，动画跑完就卸掉（避免长期离屏渲染）
    m_chart->appendUserMessage(QStringLiteral("城南线跳闸"));
    const QList<QWidget *> messages =
        widgetsByClass(m_chart.data(), QStringLiteral("messageWidget"));
    QVERIFY(!messages.isEmpty());
    QWidget *message = messages.last(); // 刚创建的那张卡
    QVERIFY(qobject_cast<QGraphicsOpacityEffect *>(message->graphicsEffect()));
    QTest::qWait(250);
    QVERIFY(!message->graphicsEffect());
}

void TestChartWidget::inputAutoGrowOnResize()
{
    m_chart->setConfigLoaded(true);
    auto *input = m_chart->findChild<QTextEdit *>(QStringLiteral("messageInput"));
    QVERIFY(input);

    // 长度要落在 autoGrow 的下限(3 行)与上限(200px)之间，否则两头都封顶看不出差异
    m_chart->setInputText(QString(260, QLatin1Char('w')));
    m_chart->resize(900, 820);
    QTest::qWait(60);
    const int wideHeight = input->height();

    // 变窄 → 换行更多 → 输入框必须跟着长高（webui 里有 window resize 监听）
    m_chart->resize(520, 820);
    QTest::qWait(80);
    const int narrowHeight = input->height();
    QVERIFY2(narrowHeight > wideHeight,
             qPrintable(QStringLiteral("wide=%1 narrow=%2 width=%3")
                            .arg(wideHeight)
                            .arg(narrowHeight)
                            .arg(input->width())));

    m_chart->resize(900, 820);
}

void TestChartWidget::expandKeepsScrollPosition()
{
    // 造足够长的历史（可滚动），并带一个工具项用于展开
    QVariantList messages;
    for (int i = 0; i < 6; ++i) {
        messages << QVariantMap{ { QStringLiteral("role"), QStringLiteral("user") },
                                 { QStringLiteral("content"),
                                   QString(200, QLatin1Char('x')) + QString::number(i) } }
                 << QVariantMap{ { QStringLiteral("role"), QStringLiteral("assistant") },
                                 { QStringLiteral("content"), QString(300, QLatin1Char('y')) } };
    }
    QVariantMap assistant;
    assistant.insert(QStringLiteral("role"), QStringLiteral("assistant"));
    assistant.insert(QStringLiteral("content"), QStringLiteral("处理中"));
    assistant.insert(QStringLiteral("tool_calls"),
                     QVariant(QVariantList{ QVariantMap{
                         { QStringLiteral("id"), QStringLiteral("t1") },
                         { QStringLiteral("function"),
                           QVariantMap{ { QStringLiteral("name"), QStringLiteral("power_flow") },
                                        { QStringLiteral("arguments"),
                                          QStringLiteral("{\"case\":\"x\"}") } } } } }));
    messages << assistant;
    m_chart->setHistory(messages);
    QTest::qWait(80);

    auto *bar = m_chart->findChild<QScrollArea *>(QStringLiteral("messages"))
                    ->verticalScrollBar();
    QVERIFY(bar);
    // 载入历史后停在最新一条（webui renderHistory 末尾贴底，对应 scrollToEnd/pinToBottom）
    QCOMPARE(bar->value(), bar->maximum());
    // 制造「用户不在底部」的前提：手动上滚
    bar->setValue(0);
    QTest::qWait(30);
    QVERIFY(bar->maximum() > bar->value()); // 内容可滚动，且用户已离开底部

    // 展开工具项会把内容顶高：应重判为「不贴底」（webui 在下一帧重判）
    QWidget *summary = widgetByClass(m_chart.data(), QStringLiteral("toolItemSummary"));
    QVERIFY(summary);
    clickWidget(summary);
    QTest::qWait(80);

    const int before = bar->value();
    m_chart->appendAssistantText(QStringLiteral("继续输出一段内容"));
    QTest::qWait(80);
    // 未贴底 → 流式分片不会把用户刚展开的位置拽走
    QVERIFY(qAbs(bar->value() - before) <= 2);
}

void TestChartWidget::escScopedToOwnWidget()
{
    openOptionsOverlay();
    QWidget *card = choiceCard();
    QVERIFY(card && card->isVisible());

    // 宿主自己的窗口按 Esc：既不该被吞掉，也不该收起库里的浮层
    QWidget host;
    host.resize(300, 200);
    host.show();
    QTest::qWait(30);
    QTest::keyClick(&host, Qt::Key_Escape);
    QTest::qWait(60);
    QVERIFY(card->isVisible());

    // 事件落在库内部时，Esc 仍然收起浮层
    QTest::keyClick(m_chart.data(), Qt::Key_Escape);
    waitOverlayClosed();
    QVERIFY(!card->isVisible());
}

void TestChartWidget::zoomShortcutScopedToWidget()
{
    // 缩放快捷键必须是 WidgetWithChildrenShortcut：否则在宿主窗口里任何位置
    // 按 Ctrl+= 都会改库的字号（WindowShortcut 是窗口级的）
    const QList<QShortcut *> shortcuts = m_chart->findChildren<QShortcut *>();
    QVERIFY(!shortcuts.isEmpty());
    for (QShortcut *shortcut : shortcuts)
        QCOMPARE(shortcut->context(), Qt::WidgetWithChildrenShortcut);
}

void TestChartWidget::keyboardReachability()
{
    // webui 里这些交互元素都是原生 button / tabindex="0"：Tab 必须能到达，
    // 且图标按钮要有无障碍名称（对应 aria-label）
    m_chart->setConfigLoaded(true);

    for (const QString &name : { QStringLiteral("sendButton"), QStringLiteral("attachButton"),
                                 QStringLiteral("voiceButton"), QStringLiteral("settingsButton") }) {
        auto *button = m_chart->findChild<QPushButton *>(name);
        QVERIFY2(button, qPrintable(name));
        QVERIFY2(button->focusPolicy() & Qt::TabFocus, qPrintable(name));
    }

    // 全树按钮都要可 Tab（设置对话框 / 会话面板 / 皮肤弹层的按钮也在内）
    for (QAbstractButton *button : m_chart->findChildren<QAbstractButton *>()) {
        QVERIFY2(button->focusPolicy() & Qt::TabFocus,
                 qPrintable(button->objectName() + QLatin1Char(' ') + button->text()));
    }

    // 无文本按钮的 accessibleName 取 toolTip
    int iconButtons = 0;
    for (QPushButton *button : m_chart->findChildren<QPushButton *>()) {
        if (!button->text().isEmpty() || button->toolTip().isEmpty())
            continue;
        ++iconButtons;
        QCOMPARE(button->accessibleName(), button->toolTip());
    }
    QVERIFY(iconButtons > 0);

    // .proc-row 是 role="button"：Enter 与点击等价
    m_chart->setHistory(historyFixture());
    QTest::qWait(30);
    QWidget *procRow = widgetByClass(m_chart.data(), QStringLiteral("procRow"));
    QVERIFY(procRow);
    QWidget *procHead = widgetByClass(procRow, QStringLiteral("procRowHead"));
    QWidget *procBody = widgetByClass(procRow, QStringLiteral("procBody"));
    QVERIFY(procHead && procBody);
    QVERIFY(procHead->focusPolicy() & Qt::TabFocus);
    const bool procOpen = procBody->isVisible();
    QTest::keyClick(procHead, Qt::Key_Return);
    QTest::qWait(30);
    QCOMPARE(procBody->isVisible(), !procOpen);

    // .session-select 是 tabindex="0"：键盘选中会话
    m_chart->setSessions(QVariantList{ QVariantMap{
        { QStringLiteral("id"), QStringLiteral("s1") },
        { QStringLiteral("title"), QStringLiteral("城南线处置") },
        { QStringLiteral("updated_at"), QStringLiteral("2026-09-06T09:04:00") } } });
    QTest::qWait(30);
    QWidget *select = widgetByClass(m_chart.data(), QStringLiteral("sessionSelect"));
    QVERIFY(select);
    QVERIFY(select->focusPolicy() & Qt::TabFocus);

    QSignalSpy sessionSpy(m_chart.data(), &gs::ChartWidget::sessionSelected);
    QTest::keyClick(select, Qt::Key_Return);
    QTest::qWait(30);
    QCOMPARE(sessionSpy.count(), 1);
    QCOMPARE(sessionSpy.at(0).at(0).toString(), QStringLiteral("s1"));

    // 设置对话框是独立顶级窗口：里面的控件事后要能看到字距也已补齐
    m_chart->openSettings();
    QTest::qWait(30);
    QWidget *dialog = m_chart->findChild<QWidget *>(QStringLiteral("settingsDialog"));
    QVERIFY(dialog);
    auto *eyebrow = qobject_cast<QLabel *>(widgetByClass(dialog, QStringLiteral("sectionLabel")));
    QVERIFY(eyebrow);
    QCOMPARE(eyebrow->font().letterSpacingType(), QFont::AbsoluteSpacing);
    QVERIFY(qAbs(eyebrow->font().letterSpacing() - 1.4) < 0.05);
}

// 轮次导航轨：鼠标移入轨道展开整轮列表（一轮一行，序号 + 提问摘要），
// 点行跳转并收起；离开留 180ms 延迟再收起（app.js renderTurnRail / showTurnRailPanel）
void TestChartWidget::turnRailPanelHover()
{
    QVariantList history = historyFixture();
    history << QVariantMap{ { QStringLiteral("role"), QStringLiteral("user") },
                            { QStringLiteral("content"), QStringLiteral("改为甩负荷方案处理") },
                            { QStringLiteral("display_content"),
                              QStringLiteral("改为甩负荷方案处理") } }
            << QVariantMap{ { QStringLiteral("role"), QStringLiteral("assistant") },
                            { QStringLiteral("content"), QStringLiteral("已改为甩负荷方案。") } };
    m_chart->setHistory(history);
    QTest::qWait(30);

    QWidget *rail = m_chart->findChild<QWidget *>(QStringLiteral("turnRail"));
    QVERIFY(rail);
    QVERIFY(rail->isVisible());
    QCOMPARE(widgetsByClass(rail, QStringLiteral("turnRailDot")).size(), 2);

    QWidget *panel = m_chart->findChild<QWidget *>(QStringLiteral("turnRailPanel"));
    QVERIFY(panel);
    QVERIFY(!panel->isVisible()); // 未悬停时不展开

    QEvent enter(QEvent::Enter);
    QApplication::sendEvent(rail, &enter);
    QTest::qWait(20);
    QVERIFY(panel->isVisible());

    const QList<QWidget *> rows = widgetsByClass(panel, QStringLiteral("turnRailRow"));
    QCOMPARE(rows.size(), 2);
    QVERIFY(rowSignature(rows.first()).contains(QStringLiteral("城南线")));
    QVERIFY2(rowSignature(rows.at(1)).contains(QStringLiteral("甩负荷")),
             qPrintable(rowSignature(rows.at(1))));
    // 行的先后与轮次一致，且都落在面板内（不会被裁到面板之外）
    QVERIFY(rowSignature(rows.at(1)).startsWith(QStringLiteral("2|")));
    for (QWidget *row : rows) {
        const QPoint topLeft = row->mapTo(panel, QPoint(0, 0));
        QVERIFY(panel->rect().contains(topLeft));
        QVERIFY(panel->rect().contains(topLeft + QPoint(0, row->height() - 1)));
    }

    // 当前轮次在列表行上高亮（与轨道白点同步）
    int activeRows = 0;
    for (QWidget *row : rows) {
        if (row->property("active").toBool())
            ++activeRows;
    }
    QCOMPARE(activeRows, 1);

    // 点击行即跳转并收起面板
    clickWidget(rows.at(1));
    QTest::qWait(20);
    QVERIFY(!panel->isVisible());

    // 行是键盘可达的：Enter / Space 与点击等价（webui 里这是一枚 <button>）
    for (Qt::Key key : { Qt::Key_Return, Qt::Key_Space }) {
        QApplication::sendEvent(rail, &enter);
        QTest::qWait(20);
        QVERIFY(panel->isVisible());
        QWidget *row = widgetsByClass(panel, QStringLiteral("turnRailRow")).at(1);
        row->setFocus();
        QTest::keyClick(row, key);
        QTest::qWait(20);
        QVERIFY(!panel->isVisible());
    }

    // 离开轨道后在 180ms 延迟窗口内保持，之后收起
    QApplication::sendEvent(rail, &enter);
    QTest::qWait(20);
    QVERIFY(panel->isVisible());
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(rail, &leave);
    QVERIFY(panel->isVisible());
    QTest::qWait(260);
    QVERIFY(!panel->isVisible());
}

// 浮层几何与 webui 的 CSS 口径一致：
//   .session-panel      top:126px;left/right:8px;max-height:calc(100% - 154px)（无固定高度，内容少时收缩）
//   .model-listbox      left:0;bottom:calc(100% + 7px);width:calc(100vw - 18px);max-width:330px（Skill 250px）
//   .turn-rail-panel    width:300px;max-width:min(300px,calc(100vw - 26px));max-height:min(62vh,420px)
void TestChartWidget::overlayGeometryMatchesWebui()
{
    const int hostWidth = m_chart->width();
    const int hostHeight = m_chart->height();

    // --- 会话面板 ---
    QVariantList sessions;
    for (int i = 0; i < 20; ++i) {
        sessions << QVariantMap{ { QStringLiteral("id"), QStringLiteral("s%1").arg(i) },
                                 { QStringLiteral("title"), QStringLiteral("会话 %1").arg(i) },
                                 { QStringLiteral("updated_at"),
                                   QStringLiteral("2026-09-28T10:00:00") } };
    }
    m_chart->setSessions(sessions);
    QWidget *sessionTrigger = m_chart->findChild<QWidget *>(QStringLiteral("sessionTrigger"));
    QVERIFY(sessionTrigger);
    clickWidget(sessionTrigger);
    QTest::qWait(20);

    QWidget *sessionPanel = m_chart->findChild<QWidget *>(QStringLiteral("sessionPanel"));
    QVERIFY(sessionPanel && sessionPanel->isVisible());
    QCOMPARE(sessionPanel->mapTo(m_chart.data(), QPoint(0, 0)).y(), 126);
    QCOMPARE(sessionPanel->width(), hostWidth - 16);
    // 内容远超上限 → 高度正好是 max-height，底边距宿主 28px
    QCOMPARE(sessionPanel->height(), hostHeight - 154);

    clickWidget(sessionTrigger); // 收起
    m_chart->setSessions(QVariantList{ sessions.first() });
    clickWidget(sessionTrigger);
    QTest::qWait(20);
    // 只有一条会话时按内容收缩（webui 两条 max-height 里后写的生效，430px 那条已被覆盖）
    QVERIFY(sessionPanel->isVisible());
    QVERIFY(sessionPanel->height() < hostHeight - 154);
    QVERIFY(sessionPanel->height() > 0);
    // 单条会话时面板按内容收缩：这一行必须完整落在面板内（首次展开时面板还是隐藏的，
    // 尺寸只能来自 sizeHint，量少了就会把最后一行截掉）
    QWidget *sessionHead = sessionPanel->findChild<QWidget *>(QStringLiteral("panelHead"));
    QWidget *sessionRow = widgetByClass(sessionPanel, QStringLiteral("sessionRow"));
    QVERIFY(sessionHead && sessionRow);
    QVERIFY(sessionPanel->height() >= sessionHead->height() + sessionRow->height() + 10);
    QVERIFY(sessionRow->mapTo(sessionPanel, QPoint(0, 0)).y() + sessionRow->height()
            <= sessionPanel->height());
    clickWidget(sessionTrigger);
    QTest::qWait(20);
    QVERIFY(!sessionPanel->isVisible());

    // --- 模型 / Skill 下拉 ---
    const QList<QWidget *> controls = widgetsByClass(m_chart.data(), QStringLiteral("modelControl"));
    const QList<QWidget *> triggers = widgetsByClass(m_chart.data(), QStringLiteral("comboTrigger"));
    QCOMPARE(controls.size(), 2);
    QCOMPARE(triggers.size(), 2);

    // 先在宽窗口验证下拉左缘对齐；窄窗口下另有“限制在宿主内”的边界测试。
    m_chart->resize(800, hostHeight);
    QTest::qWait(20);

    auto openPopup = [this](QWidget *trigger) -> QWidget * {
        clickWidget(trigger);
        QTest::qWait(20);
        const QList<QWidget *> popups =
            m_chart->findChildren<QWidget *>(QStringLiteral("listbox"));
        for (QWidget *popup : popups) {
            if (popup->isVisible())
                return popup;
        }
        return nullptr;
    };

    QWidget *modelPopup = openPopup(triggers.at(0));
    QVERIFY(modelPopup);
    QCOMPARE(modelPopup->width(), qMin(330, hostWidth - 18));
    const QWidget *modelControl = controls.at(0);
    // left:0 贴外层 .model-control 左缘（不是内部按钮）；绝对定位的包含块是它的 padding box
    const QPoint modelAnchor =
        modelControl->mapToGlobal(modelControl->contentsRect().topLeft());
    QCOMPARE(modelPopup->mapToGlobal(QPoint(0, 0)).x(),
             modelAnchor.x());
    // bottom:calc(100% + 7px)：弹层底边在控件上方 7px（弹层整体向上生长）
    QCOMPARE(modelAnchor.y() - modelPopup->mapToGlobal(QPoint(0, 0)).y(),
             modelPopup->height() + 7);
    modelPopup->hide();
    QTest::qWait(20);

    QWidget *skillPopup = openPopup(triggers.at(1));
    QVERIFY(skillPopup);
    QCOMPARE(skillPopup->width(), qMin(250, hostWidth - 18));
    const QWidget *skillControl = controls.at(1);
    const QPoint skillAnchor =
        skillControl->mapToGlobal(skillControl->contentsRect().topLeft());
    QCOMPARE(skillPopup->mapToGlobal(QPoint(0, 0)).x(), skillAnchor.x());
    QCOMPARE(skillAnchor.y() - skillPopup->mapToGlobal(QPoint(0, 0)).y(),
             skillPopup->height() + 7);
    skillPopup->hide();
    QTest::qWait(20);

    // 窄窗口：宽度按 calc(100vw - 18px) 收敛（仍不超过 max-width），且整体不越出宿主窗口
    m_chart->setMinimumSize(0, 0); // 控件自带 420x460 下限，否则 resize 到不了 300
    m_chart->resize(300, hostHeight);
    QTest::qWait(20);
    QCOMPARE(m_chart->width(), 300);
    modelPopup = openPopup(triggers.at(0));
    QVERIFY(modelPopup);
    // 开浮层时内部控件会让位，宿主随后被布局下限撑回一点，所以这里量的是“不超过”而不是等值
    QVERIFY(modelPopup->width() <= qMin(330, m_chart->width() - 18));
    const QPoint hostOrigin = m_chart->mapToGlobal(QPoint(0, 0));
    const int popupX = modelPopup->pos().x();
    QVERIFY(popupX >= hostOrigin.x());
    QVERIFY(popupX + modelPopup->width() <= hostOrigin.x() + m_chart->width());
    modelPopup->hide();
    QTest::qWait(20);
    m_chart->setMinimumSize(420, 460);
    m_chart->resize(hostWidth, hostHeight);
    QTest::qWait(20);

    // --- 轮次导航面板 ---
    QWidget *rail = m_chart->findChild<QWidget *>(QStringLiteral("turnRail"));
    QWidget *railPanel = m_chart->findChild<QWidget *>(QStringLiteral("turnRailPanel"));
    QVERIFY(rail && railPanel);
    QEvent enter(QEvent::Enter);

    m_chart->setHistory(historyFixture()); // 1 轮
    QTest::qWait(30);
    QVERIFY(rail->isVisible());
    QApplication::sendEvent(rail, &enter);
    QTest::qWait(20);
    QVERIFY(railPanel->isVisible());
    QCOMPARE(railPanel->width(), 300);
    // 高度按内容：head + (1 行 × 30 + 列表内边距 8)，没有旧的 60px 下限
    QWidget *railHead = widgetByClass(railPanel, QStringLiteral("turnRailHead"));
    QVERIFY(railHead);
    QVERIFY(qAbs(railPanel->height() - (railHead->height() + 38)) <= 2);

    // 窄窗口：max-width:min(300px,calc(100vw - 26px))，展开状态跟随重排
    // （控件自带 420x460 下限，resize(260) 会被顶回 420，所以这里验的是“不超过”）
    m_chart->resize(260, hostHeight);
    QTest::qWait(20);
    QVERIFY(railPanel->isVisible());
    QCOMPARE(railPanel->width(), qMin(300, m_chart->width() - 26));

    // 多轮：高度被 max-height:min(62vh,420px) 截断，列表内部滚动
    QVariantList manyTurns;
    for (int i = 0; i < 20; ++i) {
        manyTurns << QVariantMap{ { QStringLiteral("role"), QStringLiteral("user") },
                                  { QStringLiteral("content"),
                                    QStringLiteral("第 %1 轮提问").arg(i + 1) },
                                  { QStringLiteral("display_content"),
                                    QStringLiteral("第 %1 轮提问").arg(i + 1) } };
    }
    m_chart->setHistory(manyTurns);
    QTest::qWait(30);
    QApplication::sendEvent(rail, &enter);
    QTest::qWait(20);
    QVERIFY(railPanel->isVisible());
    QCOMPARE(railPanel->height(), qMin(qRound(m_chart->height() * 0.62), 420));

    // --- 皮肤下拉 ---
    // .theme-listbox{top:32px;right:0;width:132px}：28px 触发器 + 4px 间距，右缘与触发器对齐
    m_chart->resize(hostWidth, hostHeight);
    QTest::qWait(20);
    QWidget *themeTrigger = m_chart->findChild<QWidget *>(QStringLiteral("themeTrigger"));
    QVERIFY(themeTrigger);
    clickWidget(themeTrigger);
    QTest::qWait(20);
    QWidget *themePopup = m_chart->findChild<QWidget *>(QStringLiteral("themeListbox"));
    QVERIFY(themePopup && themePopup->isVisible());
    QCOMPARE(themePopup->width(), 132);
    const QPoint triggerBottom =
        themeTrigger->mapToGlobal(QPoint(0, themeTrigger->height()));
    const QPoint triggerRight =
        themeTrigger->mapToGlobal(QPoint(themeTrigger->width(), 0));
    const QPoint themeOrigin = themePopup->mapToGlobal(QPoint(0, 0));
    QCOMPARE(themeOrigin.y() - triggerBottom.y(), 4);
    QCOMPARE(themeOrigin.x() + themePopup->width(), triggerRight.x());
    const QPoint themeHostOrigin = m_chart->mapToGlobal(QPoint(0, 0));
    QVERIFY(themeOrigin.x() >= themeHostOrigin.x());
    QVERIFY(themeOrigin.x() + themePopup->width()
            <= themeHostOrigin.x() + m_chart->width());
    // 三个主题选项都必须落在定稿高度内（隐藏状态量到的尺寸会把最后一行截掉）
    const QList<QWidget *> themeOptions =
        widgetsByClass(themePopup, QStringLiteral("themeOption"));
    QCOMPARE(themeOptions.size(), 3);
    QVERIFY(themeOptions.last()->mapTo(themePopup, QPoint(0, 0)).y()
            + themeOptions.last()->height() <= themePopup->height());
    themePopup->hide();
    QTest::qWait(20);
}

// 会话面板打开后搜索过滤：webui 的 max-height 只是上限，内容变少时面板要立即收缩。
// Qt 的 render() 只重建列表；若不给宿主重排信号，面板会一直保持过滤前的最大高度。
void TestChartWidget::sessionPanelReflowsWhileOpen()
{
    QVariantList sessions;
    for (int i = 0; i < 20; ++i) {
        sessions << QVariantMap{ { QStringLiteral("id"), QStringLiteral("s%1").arg(i) },
                                 { QStringLiteral("title"), QStringLiteral("会话 %1").arg(i) },
                                 { QStringLiteral("updated_at"),
                                   QStringLiteral("2026-09-28T10:00:00") } };
    }
    m_chart->setSessions(sessions);

    QWidget *trigger = m_chart->findChild<QWidget *>(QStringLiteral("sessionTrigger"));
    QVERIFY(trigger);
    clickWidget(trigger);
    QTest::qWait(30);

    QWidget *panel = m_chart->findChild<QWidget *>(QStringLiteral("sessionPanel"));
    QVERIFY(panel && panel->isVisible());
    const int maxHeight = m_chart->height() - 154;
    QCOMPARE(panel->height(), maxHeight);
    QCOMPARE(widgetsByClass(panel, QStringLiteral("sessionRow")).size(), sessions.size());

    // 唯一标题过滤到一行：面板应收缩到内容高度，行不能被底边截断。
    QLineEdit *search = panel->findChild<QLineEdit *>(QStringLiteral("sessionSearch"));
    QVERIFY(search);
    search->setText(QStringLiteral("会话 17"));
    QTest::qWait(30);

    QCOMPARE(widgetsByClass(panel, QStringLiteral("sessionRow")).size(), 1);
    QVERIFY2(panel->height() < maxHeight,
             qPrintable(QStringLiteral("session panel kept stale max height %1")
                            .arg(panel->height())));
    QWidget *row = widgetByClass(panel, QStringLiteral("sessionRow"));
    QVERIFY(row);
    const QRect rowRect(row->mapTo(panel, QPoint(0, 0)), row->size());
    QVERIFY2(panel->rect().contains(rowRect),
             qPrintable(QStringLiteral("session row %1,%2 %3x%4 outside panel %5x%6")
                            .arg(rowRect.x()).arg(rowRect.y())
                            .arg(rowRect.width()).arg(rowRect.height())
                            .arg(panel->width()).arg(panel->height())));

    // 无匹配结果时也要按空状态收缩。
    search->setText(QStringLiteral("不存在的会话"));
    QTest::qWait(30);
    QCOMPARE(widgetsByClass(panel, QStringLiteral("sessionRow")).size(), 0);
    QLabel *empty = qobject_cast<QLabel *>(
        widgetByClass(panel, QStringLiteral("listboxEmpty")));
    QVERIFY(empty && empty->isVisible());
    QVERIFY(panel->height() < maxHeight);
    const QRect emptyRect(empty->mapTo(panel, QPoint(0, 0)), empty->size());
    QVERIFY(panel->rect().contains(emptyRect));
}

// Toast（#toast）：宽固定 = 宿主宽 - 20px、高随文字换行。
// 高度来自 heightForWidth()，而样式表的 padding/border 要等 show() 之后才生效，
// 隐藏状态下先算出来的高度偏小 → 多行提示的末行会被裁掉。
void TestChartWidget::toastFitsWrappedText()
{
    const QString text =
        QStringLiteral("附件上传失败：后台拒绝了这次上传，请检查文件大小、类型与网关配额后重试。");
    m_chart->showToast(text);
    QTest::qWait(30);

    QWidget *toast = m_chart->findChild<QWidget *>(QStringLiteral("toast"));
    QVERIFY(toast && toast->isVisible());
    QCOMPARE(toast->width(), m_chart->width() - 20);
    QCOMPARE(toast->mapTo(m_chart.data(), QPoint(0, 0)).x(), 10);

    // 独立核算：QSS padding 8px 10px + 1px 边框，文本可用宽度就是窗口宽减掉这些
    const QFontMetrics metrics(toast->font());
    const int textWidth = toast->width() - 2 * 10 - 2;
    const int textHeight = metrics
                               .boundingRect(QRect(0, 0, textWidth, 10000),
                                             Qt::TextWordWrap, text)
                               .height();
    const int needed = textHeight + 2 * 8 + 2;
    QVERIFY2(toast->height() >= needed,
             qPrintable(QStringLiteral("toast 高度 %1 装不下文本 %2（需要 %3）")
                            .arg(toast->height())
                            .arg(text)
                            .arg(needed)));

    // 同一份文本再走一次 heightForWidth：定稿后必须不比已分配的矮
    QVERIFY2(toast->height() >= toast->heightForWidth(toast->width()),
             qPrintable(QStringLiteral("height=%1 heightForWidth=%2")
                            .arg(toast->height())
                            .arg(toast->heightForWidth(toast->width()))));

    // 提示条整体浮在输入区上方，且不越出宿主
    QWidget *composer = m_chart->findChild<QWidget *>(QStringLiteral("composer"));
    QVERIFY(composer);
    const QRect toastRect(toast->mapTo(m_chart.data(), QPoint(0, 0)), toast->size());
    QVERIFY(m_chart->rect().contains(toastRect));
    QVERIFY(toastRect.bottom() <= composer->mapTo(m_chart.data(), QPoint(0, 0)).y() - 12 + 1);

    // 短文本只占一行：高度应明显小于多行版本
    m_chart->showToast(QStringLiteral("已复制到剪贴板"));
    QTest::qWait(30);
    QVERIFY(toast->height() < needed);
}

// 设置中心「用量」页：懒加载、默认筛选、M 单位概览、三图、可排序明细表
void TestChartWidget::usagePanelTab()
{
    m_chart->setConfigLoaded(true);
    m_chart->setModels(QVariantList{
        QVariantMap{ { QStringLiteral("provider"), QStringLiteral("gridstar") },
                     { QStringLiteral("provider_name"), QStringLiteral("GridStar 网关") },
                     { QStringLiteral("id"), QStringLiteral("gs-pro-32k") },
                     { QStringLiteral("name"), QStringLiteral("GS-Pro 32K") },
                     { QStringLiteral("enabled"), true } },
        QVariantMap{ { QStringLiteral("provider"), QStringLiteral("deepseek") },
                     { QStringLiteral("provider_name"), QStringLiteral("DeepSeek") },
                     { QStringLiteral("id"), QStringLiteral("deepseek-chat") },
                     { QStringLiteral("name"), QStringLiteral("DeepSeek Chat") },
                     { QStringLiteral("enabled"), true } } });

    QSignalSpy spy(m_chart.data(), &gs::ChartWidget::usageStatsRequested);
    m_chart->openSettings(QStringLiteral("usage"));
    QTest::qWait(30);
    // 切到用量 Tab 才发起首次请求
    QCOMPARE(spy.count(), 1);
    const QString requestId = spy.at(0).at(0).toString();
    QVERIFY(!requestId.isEmpty());                 // 请求 ID 由组件生成，宿主须原样带回
    QVERIFY(!spy.at(0).at(1).toString().isEmpty()); // start
    QVERIFY(!spy.at(0).at(2).toString().isEmpty()); // end
    QVERIFY(spy.at(0).at(3).toString().isEmpty());  // 默认全部供应商
    QVERIFY(spy.at(0).at(4).toString().isEmpty());  // 默认全部模型

    QWidget *dialog = m_chart->findChild<QWidget *>(QStringLiteral("settingsDialog"));
    QVERIFY(dialog);

    // 默认筛选：模型用量 / 全部供应商 / 全部模型 / 按天 / 最近 7 天
    QCOMPARE(labelText(dialog->findChild<QWidget *>(QStringLiteral("usageGroup")),
                       QStringLiteral("usageSelectText")),
             QStringLiteral("模型用量"));
    QCOMPARE(labelText(dialog->findChild<QWidget *>(QStringLiteral("usageProvider")),
                       QStringLiteral("usageSelectText")),
             QStringLiteral("全部供应商"));
    QCOMPARE(labelText(dialog->findChild<QWidget *>(QStringLiteral("usageModel")),
                       QStringLiteral("usageSelectText")),
             QStringLiteral("全部模型"));
    QCOMPARE(labelText(dialog->findChild<QWidget *>(QStringLiteral("usageGranularity")),
                       QStringLiteral("usageSelectText")),
             QStringLiteral("按天"));
    QCOMPARE(labelText(dialog->findChild<QWidget *>(QStringLiteral("usageRange")),
                       QStringLiteral("usageSelectText")),
             QStringLiteral("最近 7 天"));

    m_chart->setUsageStats(requestId,
                           usageFixture(spy.at(0).at(1).toString(), spy.at(0).at(2).toString()));
    QTest::qWait(30);

    // 概览卡一律以百万（M）为单位；缓存命中率按 cache_read / (cache_read + input)
    QWidget *overview = dialog->findChild<QWidget *>(QStringLiteral("usageOverview"));
    QVERIFY(overview);
    QCOMPARE(labelTexts(overview, QStringLiteral("usageStatValue")),
             QStringList({ QStringLiteral("1.63M"), QStringLiteral("1.37M"),
                           QStringLiteral("0.26M"), QStringLiteral("1.48M / 0.15M"),
                           QStringLiteral("27%"), QStringLiteral("2") }));

    // 三个图表容器都已渲染
    QVERIFY(dialog->findChild<QWidget *>(QStringLiteral("usageLine")));
    QVERIFY(dialog->findChild<QWidget *>(QStringLiteral("usageBar")));
    QVERIFY(dialog->findChild<QWidget *>(QStringLiteral("usagePie")));

    // 明细表：显示名取配置、token 列按 M、轮次按原值
    auto *table = dialog->findChild<QTableWidget *>(QStringLiteral("usageTable"));
    QVERIFY(table);
    QCOMPARE(table->columnCount(), 8);
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("GS-Pro 32K"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("GridStar 网关"));
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("1.43M"));
    QCOMPARE(table->item(0, 7)->text(), QStringLiteral("33")); // 轮次是计数，不换算成 M

    // 点表头「总量」切换为升序：小用量排到前面
    QHeaderView *header = table->horizontalHeader();
    const int x = header->sectionViewportPosition(2) + header->sectionSize(2) / 2;
    QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x, 8));
    QTest::qWait(20);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("DeepSeek Chat"));

    // 日期浮层：日历在左、预设在右，确定钮压到预设列底部（.usage-range-panel 的 DOM 顺序）
    clickWidget(dialog->findChild<QWidget *>(QStringLiteral("usageRange")));
    QTest::qWait(30);
    QWidget *rangePanel = widgetByClass(m_chart.data(), QStringLiteral("usageRangePanel"));
    QVERIFY(rangePanel);
    QWidget *calGrid = widgetByClass(rangePanel, QStringLiteral("usageCalGrid"));
    const QList<QWidget *> presets = widgetsByClass(rangePanel, QStringLiteral("usageRangePreset"));
    QVERIFY(calGrid);
    QCOMPARE(presets.size(), 5);
    QVERIFY(calGrid->mapTo(rangePanel, QPoint(0, 0)).x()
            < presets.first()->mapTo(rangePanel, QPoint(0, 0)).x());
    QWidget *confirm = widgetByClass(rangePanel, QStringLiteral("usageCalConfirm"));
    QVERIFY(confirm);
    QVERIFY(confirm->mapTo(rangePanel, QPoint(0, 0)).y()
            > presets.last()->mapTo(rangePanel, QPoint(0, 0)).y());
    // 面板挂在 .usage-filters 上：right:0 贴筛选行右缘、top:100% + 5px。
    // 宽高必须取 show() 之后的定稿值，否则底部预设/日历会被截掉。
    QWidget *filtersRow = widgetByClass(dialog, QStringLiteral("usageFilters"));
    QVERIFY(filtersRow);
    const QPoint filtersOrigin = filtersRow->mapToGlobal(QPoint(0, 0));
    const QPoint filtersBelow = filtersRow->mapToGlobal(QPoint(0, filtersRow->height()));
    const QPoint rangeOrigin = rangePanel->mapToGlobal(QPoint(0, 0));
    QCOMPARE(rangeOrigin.y() - filtersBelow.y(), 5);
    QCOMPARE(rangeOrigin.x() + rangePanel->width(), filtersOrigin.x() + filtersRow->width());
    QVERIFY(confirm->mapTo(rangePanel, QPoint(0, confirm->height())).y()
            <= rangePanel->height());
    rangePanel->hide();

    // 下拉挂在 .usage-filter 容器上（含标签）：left:0 贴容器左缘，不是按钮左缘
    QWidget *modelSelect = dialog->findChild<QWidget *>(QStringLiteral("usageModel"));
    QVERIFY(modelSelect);
    QWidget *modelBox = modelSelect->parentWidget();
    QVERIFY(modelBox && hasClass(modelBox, QStringLiteral("usageFilter")));
    clickWidget(modelSelect);
    QTest::qWait(30);
    QWidget *listPopup = nullptr;
    for (QWidget *candidate : widgetsByClass(m_chart.data(), QStringLiteral("usageListbox"))) {
        if (candidate->isVisible())
            listPopup = candidate;
    }
    QVERIFY(listPopup);
    const QPoint boxOrigin = modelBox->mapToGlobal(QPoint(0, 0));
    const QPoint boxBelow = modelBox->mapToGlobal(QPoint(0, modelBox->height()));
    const QPoint listOrigin = listPopup->mapToGlobal(QPoint(0, 0));
    QCOMPARE(listOrigin.x(), boxOrigin.x());
    QCOMPARE(listOrigin.y() - boxBelow.y(), 5);
    const QList<QWidget *> listOptions = widgetsByClass(listPopup, QStringLiteral("usageOption"));
    QVERIFY(listOptions.size() >= 2);
    QVERIFY(listOptions.last()->mapTo(listPopup, QPoint(0, 0)).y()
            < listPopup->height());
    // 选项行不能被压塌：ElidedLabel 的文本要等首次 resize 才回填，
    // 隐藏状态下量高度时每个选项的最小高度必须是 webui 的 30px 行高
    for (QWidget *option : listOptions) {
        QVERIFY(option->height() >= 30);
        auto *optionLabel = option->findChild<QLabel *>();
        QVERIFY(optionLabel);
        QVERIFY(optionLabel->height() > 0);
    }
    listPopup->hide();
    QTest::qWait(20);

    // 窗口 ≤900：概览收到 3 列、饼图与柱状改上下堆叠（style.css 的媒体查询）
    dialog->resize(780, dialog->height());
    QTest::qWait(40);
    const QList<QWidget *> cards = widgetsByClass(overview, QStringLiteral("usageStat"));
    QCOMPARE(cards.size(), 6);
    const QPoint first = cards.first()->mapTo(overview, QPoint(0, 0));
    const QPoint fourth = cards.at(3)->mapTo(overview, QPoint(0, 0));
    QCOMPARE(fourth.x(), first.x());
    QVERIFY(fourth.y() > first.y());
    QWidget *pieChart = dialog->findChild<QWidget *>(QStringLiteral("usagePie"));
    QWidget *barChart = dialog->findChild<QWidget *>(QStringLiteral("usageBar"));
    QVERIFY(barChart->mapTo(dialog, QPoint(0, 0)).y() > pieChart->mapTo(dialog, QPoint(0, 0)).y());
}

// 用量请求竞态：晚到的旧响应（requestId 对不上）既不能覆盖当前视图，也不能把当前视图打成错误态
void TestChartWidget::usageStaleResponseIgnored()
{
    m_chart->setConfigLoaded(true);
    QSignalSpy spy(m_chart.data(), &gs::ChartWidget::usageStatsRequested);
    m_chart->openSettings(QStringLiteral("usage"));
    QTest::qWait(30);
    QCOMPARE(spy.count(), 1);
    const QString firstId = spy.at(0).at(0).toString();
    const QString start = spy.at(0).at(1).toString();
    const QString end = spy.at(0).at(2).toString();
    QVERIFY(!firstId.isEmpty());

    QWidget *dialog = m_chart->findChild<QWidget *>(QStringLiteral("settingsDialog"));
    QVERIFY(dialog);
    QWidget *refresh = widgetByClass(dialog, QStringLiteral("usageRefresh"));
    QVERIFY(refresh);

    // 刷新即同一区间再发一次：requestId 必须换新（区间相同也要能分辨先后）
    clickWidget(refresh);
    QTest::qWait(20);
    QCOMPARE(spy.count(), 2);
    const QString secondId = spy.at(1).at(0).toString();
    QVERIFY(!secondId.isEmpty());
    QVERIFY(secondId != firstId);

    // 当前请求先回：概览按新数据渲染
    m_chart->setUsageStats(secondId, usageFixture(start, end));
    QTest::qWait(30);
    QWidget *overview = dialog->findChild<QWidget *>(QStringLiteral("usageOverview"));
    QVERIFY(overview);
    QCOMPARE(labelTexts(overview, QStringLiteral("usageStatValue")).value(0),
             QStringLiteral("1.63M"));
    QVERIFY(!labelText(dialog, QStringLiteral("usageStatus")).contains(QStringLiteral("失败")));

    // 旧请求晚到：整包数据必须被丢弃（否则概览会被旧值覆盖）
    QVariantMap stale = usageFixture(start, end);
    QVariantMap staleTotals = stale.value(QStringLiteral("totals")).toMap();
    staleTotals.insert(QStringLiteral("total"), 999);
    stale.insert(QStringLiteral("totals"), staleTotals);
    m_chart->setUsageStats(firstId, stale);
    QTest::qWait(30);
    QCOMPARE(labelTexts(overview, QStringLiteral("usageStatValue")).value(0),
             QStringLiteral("1.63M"));

    // 旧请求的失败回调同样被丢弃：不能把已渲染好的视图打成错误态
    m_chart->setUsageLoadFailed(firstId, QStringLiteral("连接超时"));
    QTest::qWait(30);
    QVERIFY(!labelText(dialog, QStringLiteral("usageStatus")).contains(QStringLiteral("失败")));
    QCOMPARE(labelTexts(overview, QStringLiteral("usageStatValue")).value(0),
             QStringLiteral("1.63M"));
}

// 回归：用量浮层必须跟 style.css 的 640px 媒体查询一致。
// 窄宿主下双月改为纵向并被宿主约束；长下拉选项不能让浮层越出设置窗口。
void TestChartWidget::usagePopupsFitNarrowHost()
{
    m_chart->setConfigLoaded(true);
    m_chart->setModels(QVariantList{
        QVariantMap{ { QStringLiteral("provider"), QStringLiteral("gridstar") },
                     { QStringLiteral("provider_name"), QStringLiteral("很长的供应商显示名称") },
                     { QStringLiteral("id"),
                       QStringLiteral("gridstar-reasoning-model-with-a-very-long-id") },
                     { QStringLiteral("name"),
                       QStringLiteral("非常长的模型名称用于触发浮层宽度收敛") },
                     { QStringLiteral("enabled"), true } } });
    m_chart->openSettings(QStringLiteral("usage"));
    QTest::qWait(50);
    QWidget *dialog = m_chart->findChild<QWidget *>(QStringLiteral("settingsDialog"));
    QVERIFY(dialog);

    dialog->setFixedSize(620, 460);
    QTest::qWait(80);
    const QRect hostRect(dialog->mapToGlobal(QPoint(0, 0)), dialog->size());

    QWidget *rangeAnchor = dialog->findChild<QWidget *>(QStringLiteral("usageRange"));
    QVERIFY(rangeAnchor);
    clickWidget(rangeAnchor);
    QTest::qWait(60);
    QWidget *range = widgetByClass(m_chart.data(), QStringLiteral("usageRangePanel"));
    QVERIFY(range);
    const QList<QWidget *> grids = widgetsByClass(range, QStringLiteral("usageCalGrid"));
    QCOMPARE(grids.size(), 2);
    const QPoint first = grids.at(0)->mapTo(range, QPoint(0, 0));
    const QPoint second = grids.at(1)->mapTo(range, QPoint(0, 0));
    QVERIFY2(second.y() > first.y(), "窄宿主下双月日历仍横向排列");
    const QRect rangeRect(range->mapToGlobal(QPoint(0, 0)), range->size());
    QVERIFY2(hostRect.contains(rangeRect),
             qPrintable(QStringLiteral("range popup %1,%2 %3x%4 outside host %5x%6")
                            .arg(rangeRect.x())
                            .arg(rangeRect.y())
                            .arg(rangeRect.width())
                            .arg(rangeRect.height())
                            .arg(hostRect.width())
                            .arg(hostRect.height())));
    auto *rangeScroll = range->findChild<QScrollArea *>(QStringLiteral("usageRangeScroll"));
    QVERIFY(rangeScroll);
    QVERIFY2(rangeScroll->verticalScrollBar()->maximum() > 0,
             "纵向双月超出 460px 宿主时没有可滚动区域");
    range->hide();
    QTest::qWait(30);

    // 340px 宿主可用宽 328px：长选项自然宽 >340，也必须收敛到宿主内。
    dialog->setFixedSize(340, 460);
    QTest::qWait(80);
    const QRect narrowHost(dialog->mapToGlobal(QPoint(0, 0)), dialog->size());
    QWidget *modelSelect = dialog->findChild<QWidget *>(QStringLiteral("usageModel"));
    QVERIFY(modelSelect);
    clickWidget(modelSelect);
    QTest::qWait(60);
    QWidget *listPopup = nullptr;
    for (QWidget *candidate : widgetsByClass(m_chart.data(), QStringLiteral("usageListbox"))) {
        if (candidate->isVisible())
            listPopup = candidate;
    }
    QVERIFY(listPopup);
    QVERIFY2(listPopup->width() <= narrowHost.width() - 12,
             qPrintable(QStringLiteral("list popup width %1 exceeds narrow host %2")
                            .arg(listPopup->width())
                            .arg(narrowHost.width())));
    const QRect listRect(listPopup->mapToGlobal(QPoint(0, 0)), listPopup->size());
    QVERIFY2(narrowHost.contains(listRect),
             qPrintable(QStringLiteral("list popup %1,%2 %3x%4 outside host %5x%6")
                            .arg(listRect.x())
                            .arg(listRect.y())
                            .arg(listRect.width())
                            .arg(listRect.height())
                            .arg(narrowHost.width())
                            .arg(narrowHost.height())));
    listPopup->hide();
}

// 回归：.md-table-wrap 的高度应为「表格自然高度 + 实际可见的横向滚动条」，
// 不能在没有横向滚动条时仍预留滚动条空白（webui 没有这行空白）。
void TestChartWidget::markdownTableSizesToContent()
{
    m_chart->appendAssistantMessage(QStringLiteral(
        "| 项目 | 数值 |\n| --- | --- |\n| 城南线跳闸 | 12.3 |\n| 城北线过载 | 4.5 |\n"));
    QTest::qWait(80);
    QWidget *shortTable = widgetByClass(m_chart.data(), QStringLiteral("mdTable"));
    QVERIFY(shortTable);
    auto *shortScroll = shortTable->findChild<QScrollArea *>();
    QVERIFY(shortScroll);
    QWidget *shortGrid = shortScroll->widget();
    QVERIFY(shortGrid);
    QVERIFY2(!shortScroll->horizontalScrollBar()->isVisible(),
             "短表格不应出现横向滚动条");
    QCOMPARE(shortScroll->height(), shortGrid->sizeHint().height());

    m_chart->appendAssistantMessage(QStringLiteral(
        "| 很长的项目名称列 | 很长的结果列 | 很长的备注列 | 很长的附加列 |\n"
        "| --- | --- | --- | --- |\n"
        "| 城南线跳闸导致的大面积停电事故 | 1234567890.123 | 需要立即处理的详细说明文本 | 额外信息 |\n"
        "| 城北线过载保护动作 | 9876543210.987 | 复核后确认的详细处理结论 | 附加备注 |\n"));
    QTest::qWait(80);
    QWidget *wideTable = nullptr;
    for (QWidget *candidate : widgetsByClass(m_chart.data(), QStringLiteral("mdTable"))) {
        if (candidate != shortTable)
            wideTable = candidate;
    }
    QVERIFY(wideTable);
    auto *wideScroll = wideTable->findChild<QScrollArea *>();
    QVERIFY(wideScroll);
    QWidget *wideGrid = wideScroll->widget();
    QVERIFY(wideGrid);
    QVERIFY2(wideScroll->horizontalScrollBar()->isVisible(),
             "宽表格应出现横向滚动条");
    QCOMPARE(wideScroll->height(),
             wideGrid->sizeHint().height() + wideScroll->horizontalScrollBar()->height());
}

// 回归防护：Qt QSS 遇到它不认识的选择器（CSS3 的 :not(...)、:!state）时，不只是跳过那一条，
// 而是会丢弃其后所有规则 —— 曾让 #choiceSubmit 之后的半张样式表（设置中心、用量页、各浮层…）
// 全部失效。这里既做静态检查，又把探针规则追加到真实样式表末尾做动态复核。
void TestChartWidget::appStyleSheetParsesFully()
{
    const QString sheet = m_chart->styleSheet();
    QVERIFY2(!sheet.contains(QStringLiteral(":not(")), "样式表含 Qt 不支持的 :not(...) 选择器");
    QVERIFY2(!sheet.contains(QStringLiteral(":!")), "样式表含 Qt 不支持的 :!state 选择器");

    // 追加到末尾的探针规则必须命中（命中→ max-width=24）；中段一旦再混入坏选择器即失效。
    const QString probe = QStringLiteral("QPushButton.qssParseProbe { min-width: 24px; max-width: 24px; }");
    QScopedPointer<QWidget> holder(new QWidget);
    holder->setStyleSheet(sheet + probe);
    auto *button = new QPushButton(QString::fromUtf8("x"), holder.data());
    button->setProperty("class", QStringLiteral("qssParseProbe"));
    button->style()->unpolish(button);
    button->style()->polish(button);
    button->ensurePolished();
    button->adjustSize();
    QCOMPARE(button->maximumWidth(), 26);
}

void TestChartWidget::guiThreadGuard()
{
    // 公开 API 的线程契约：非 GUI 线程调用会被丢弃（见 README「线程模型」）。
    // 开发构建里是 Q_ASSERT，跑不到这里的断言，所以只在发布构建下验证早退行为。
#ifdef QT_NO_DEBUG
    m_chart->appendUserMessage(QStringLiteral("城南线跳闸"));
    QTest::qWait(30);
    const int before = widgetsByClass(m_chart.data(), QStringLiteral("messageWidget")).size();
    QVERIFY(before > 0);

    std::thread worker([this] { m_chart->appendAssistantText(QStringLiteral("越界写入")); });
    worker.join();
    QTest::qWait(30);
    QCOMPARE(widgetsByClass(m_chart.data(), QStringLiteral("messageWidget")).size(), before);
#else
    QSKIP("debug 构建下非 GUI 线程调用会触发断言");
#endif
}

// 长会话下的三个热点：流式分片、轨迹追加、搜索输入。断言只放「数量级」级别的上限
// （机器快慢差几倍很正常），真实数字用 qInfo 打出来看趋势。
void TestChartWidget::renderPerformance()
{
    m_chart->setConfigLoaded(true);

    // 1) 流式：400 个分片（约 16KB 正文），每个分片都会重排整篇 Markdown
    const QString chunk = QStringLiteral("**城南线**跳闸后建议先合 lk07 转供。")
                          + QString(12, QChar(0x3002));
    QElapsedTimer streaming;
    streaming.start();
    for (int i = 0; i < 400; ++i)
        m_chart->appendAssistantText(chunk);
    m_chart->finishAssistant();
    const qint64 streamingMs = streaming.elapsed();
    qInfo("bench 流式 400 分片: %lld ms", streamingMs);

    // 2) 轨迹：1500 条记录建账本 + 60 次单条追加（宿主每收到一个 SSE 事件就会这么推）
    QVariantList events;
    for (int i = 0; i < 500; ++i) {
        events << QVariantMap{ { QStringLiteral("type"), QStringLiteral("user") },
                               { QStringLiteral("turn"), i + 1 },
                               { QStringLiteral("content"),
                                 QStringLiteral("第 %1 轮：城南线跳闸，帮我出处置方案").arg(i + 1) },
                               { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:04:00") } }
               << QVariantMap{ { QStringLiteral("type"), QStringLiteral("traj_request_start") },
                               { QStringLiteral("turn"), i + 1 },
                               { QStringLiteral("request"), 1 },
                               { QStringLiteral("start"), QStringLiteral("2026-09-06T09:04:00") } }
               << QVariantMap{ { QStringLiteral("type"), QStringLiteral("traj_request_end") },
                               { QStringLiteral("turn"), i + 1 },
                               { QStringLiteral("request"), 1 },
                               { QStringLiteral("status"), QStringLiteral("completed") },
                               { QStringLiteral("content"), QStringLiteral("建议先合 lk07 转供。") },
                               { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:04:21") } };
    }
    m_chart->setViewTab(QStringLiteral("traj"));
    QTest::qWait(50);

    QElapsedTimer firstBuild;
    firstBuild.start();
    m_chart->setTrajectoryEvents(events);
    const qint64 firstBuildMs = firstBuild.elapsed();
    qInfo("bench 轨迹首次建账本 1500 条: %lld ms", firstBuildMs);

    QElapsedTimer appends;
    appends.start();
    qint64 appendCallMs = 0;
    qint64 appendSettleMs = 0;
    // 预热：setTrajectoryEvents 刚建完账本，第一次插入会连带把整账本布局一次（一次性成本），
    // 和下面「连续追加」的量级不同，先把它吃掉再计时
    m_chart->appendTrajectoryEvents(QVariantList{ QVariantMap{
        { QStringLiteral("type"), QStringLiteral("user") },
        { QStringLiteral("turn"), 590 },
        { QStringLiteral("content"), QStringLiteral("预热追加") },
        { QStringLiteral("ts"), QStringLiteral("2026-09-06T09:59:00") } } });
    QTest::qWait(0);
    for (int i = 0; i < 10; ++i) {
        QElapsedTimer call;
        call.start();
        m_chart->appendTrajectoryEvents(QVariantList{ QVariantMap{
            { QStringLiteral("type"), QStringLiteral("user") },
            { QStringLiteral("turn"), 600 + i },
            { QStringLiteral("content"), QStringLiteral("追加事件 %1").arg(i) },
            { QStringLiteral("ts"), QStringLiteral("2026-09-06T10:00:00") } } });
        appendCallMs += call.elapsed();
        // 真机上事件是一条一条经事件循环进来的：让布局/重绘在这里落地，
        // 分开统计「我们自己的代码」与「Qt 的布局重绘」
        QElapsedTimer settle;
        settle.start();
        QTest::qWait(0);
        appendSettleMs += settle.elapsed();
    }
    const qint64 appendsMs = appends.elapsed();
    qInfo("bench 轨迹追加 10 次（平铺）: %lld ms（调用 %lld / 事件循环 %lld）", appendsMs,
          appendCallMs, appendSettleMs);

    // 2b) 轮次分组视图 · 同一轮内追加（真实运行里最常见：一轮中不断产生记录）
    QWidget *traj = m_chart->findChild<QWidget *>(QStringLiteral("trajectoryView"));
    QVERIFY(traj);
    QPushButton *turnButton = nullptr;
    for (QPushButton *button : traj->findChildren<QPushButton *>()) {
        if (hasClass(button, QStringLiteral("trajViewButton"))
            && button->text() == QStringLiteral("轮次")) {
            turnButton = button;
        }
    }
    QVERIFY(turnButton);
    turnButton->click();
    QTest::qWait(50);

    QElapsedTimer sameTurn;
    sameTurn.start();
    qint64 sameTurnCallMs = 0;
    qint64 sameTurnSettleMs = 0;
    // 先起一轮新的（模拟「当前正在跑的那一轮」），后面的记录都续在它后面
    QElapsedTimer warmUp;
    warmUp.start();
    m_chart->appendTrajectoryEvents(QVariantList{ QVariantMap{
        { QStringLiteral("type"), QStringLiteral("user") },
        { QStringLiteral("turn"), 610 },
        { QStringLiteral("content"), QStringLiteral("第 610 轮开始") },
        { QStringLiteral("ts"), QStringLiteral("2026-09-06T10:00:00") } } });
    QTest::qWait(0);
    const qint64 warmUpMs = warmUp.elapsed();
    // 这一段包含「切到轮次视图」后的第一次追加：账本刚被整体重建过，会顺带做一次完整布局
    qInfo("bench 轨迹追加 1 次（新起一轮，含视图切换后的首次布局）: %lld ms", warmUpMs);
    for (int i = 0; i < 5; ++i) {
        QElapsedTimer call;
        call.start();
        m_chart->appendTrajectoryEvents(QVariantList{ QVariantMap{
            { QStringLiteral("type"), QStringLiteral("user") },
            { QStringLiteral("turn"), 610 }, // 续在最后一组后面：真实运行里最常见
            { QStringLiteral("content"), QStringLiteral("同轮追加 %1").arg(i) },
            { QStringLiteral("ts"), QStringLiteral("2026-09-06T10:00:00") } } });
        sameTurnCallMs += call.elapsed();
        QElapsedTimer settle;
        settle.start();
        QTest::qWait(0);
        sameTurnSettleMs += settle.elapsed();
    }
    const qint64 sameTurnMs = sameTurn.elapsed();
    // 这里只报「我们自己的调用」与「事件循环」两段之和：块的墙钟时间会含上面那次首次布局
    qInfo("bench 轨迹追加 5 次（轮次分组·同轮）: 调用 %lld / 事件循环 %lld（块内合计 %lld）",
          sameTurnCallMs, sameTurnSettleMs, sameTurnMs);

    // 2c) 轮次分组视图 · 每次开新一轮（每追加一条都要新建一个分组头）
    QElapsedTimer groupedAppends;
    groupedAppends.start();
    qint64 groupedCallMs = 0;
    qint64 groupedSettleMs = 0;
    for (int i = 0; i < 5; ++i) {
        QElapsedTimer call;
        call.start();
        m_chart->appendTrajectoryEvents(QVariantList{ QVariantMap{
            { QStringLiteral("type"), QStringLiteral("user") },
            { QStringLiteral("turn"), 700 + i },
            { QStringLiteral("content"), QStringLiteral("分组追加 %1").arg(i) },
            { QStringLiteral("ts"), QStringLiteral("2026-09-06T11:00:00") } } });
        groupedCallMs += call.elapsed();
        QElapsedTimer settle;
        settle.start();
        QTest::qWait(0);
        groupedSettleMs += settle.elapsed();
    }
    const qint64 groupedAppendsMs = groupedAppends.elapsed();
    qInfo("bench 轨迹追加 5 次（轮次分组）: %lld ms（调用 %lld / 事件循环 %lld）", groupedAppendsMs,
          groupedCallMs, groupedSettleMs);

    // 3) 搜索：在 1560 条记录上逐字输入 8 个字符（去抖后只重建一次账本）
    auto *search = m_chart->findChild<QLineEdit *>(QStringLiteral("trajSearch"));
    QVERIFY(search);
    QElapsedTimer typing;
    typing.start();
    for (int i = 1; i <= 8; ++i)
        search->setText(QStringLiteral("lk07") + QString::number(i));
    const qint64 typingMs = typing.elapsed();
    qInfo("bench 搜索逐字输入 8 次（同步部分）: %lld ms", typingMs);
    QTest::qWait(300); // 等去抖到期（200ms）完成那一次重建

    // 4) 清空搜索后点一行 + 滚一屏：以前两者都会重建整个账本
    search->clear();
    QTest::qWait(300);

    // 视窗化后滚动只换掉「进入视口的那几十行」
    auto *ledgerArea = m_chart->findChild<QScrollArea *>(QStringLiteral("trajLedger"));
    QVERIFY(ledgerArea);
    QScrollBar *ledgerBar = ledgerArea->verticalScrollBar();
    QVERIFY(ledgerBar);
    ledgerBar->setValue(0);
    QTest::qWait(0);
    QElapsedTimer scrolling;
    scrolling.start();
    qint64 scrollSettleMs = 0;
    for (int i = 0; i < 10; ++i) {
        ledgerBar->setValue(qMin(ledgerBar->maximum(),
                                 ledgerBar->value() + ledgerArea->viewport()->height()));
        QElapsedTimer settle;
        settle.start();
        QTest::qWait(0);
        scrollSettleMs += settle.elapsed();
    }
    const qint64 scrollingMs = scrolling.elapsed();
    qInfo("bench 账本滚一屏 ×10: %lld ms（其中事件循环 %lld）", scrollingMs, scrollSettleMs);
    const QList<QWidget *> rows = widgetsByClass(traj, QStringLiteral("trajRow"));
    QVERIFY(!rows.isEmpty());
    QElapsedTimer selecting;
    selecting.start();
    clickWidget(rows.at(rows.size() / 2));
    const qint64 selectingMs = selecting.elapsed();
    qInfo("bench 选中一行: %lld ms", selectingMs);

    // 断言只压在**库自己的代码**上（调用耗时）：全量重建账本一次约 230ms，
    // 所以一旦退回「每个事件重建一次」，单次调用就会从个位数毫秒涨到 200ms+，立刻被拦住。
    // 事件循环那部分（Qt 对账本做一次全量布局/重绘）与行数成正比，是 Qt 侧成本，
    // 只打日志看趋势，不做断言，免得在慢机器上误报
    QVERIFY(streamingMs < 5000);
    QVERIFY(firstBuildMs < 5000);
    QVERIFY(appendCallMs < 400);         // 10 次 × 40ms（实测个位数毫秒）
    QVERIFY(sameTurnCallMs < 300);       // 5 次 × 60ms
    QVERIFY(groupedCallMs < 300);        // 5 次 × 60ms
    QVERIFY(typingMs < 100);             // 逐字输入不得同步重建账本
    QVERIFY(selectingMs < 150);          // 选中一行不得重建账本（全量重建约 230ms）
    QVERIFY(scrollingMs < 3000);         // 滚 10 屏：只换可视区那几十行
}

// 账本视窗化：只有可视区那几十行是真实控件，但内容高度、滚动、选中态都要跟全量一致
void TestChartWidget::trajectoryLedgerVirtualization()
{
    // 400 轮 = 1200 条记录，条目数远超「一屏能放下」的行数
    m_chart->setTrajectoryEvents(trajectoryFixture(400));
    m_chart->setViewTab(QStringLiteral("traj"));
    QTest::qWait(50);

    QWidget *traj = m_chart->findChild<QWidget *>(QStringLiteral("trajectoryView"));
    QVERIFY(traj);
    auto *ledger = traj->findChild<QScrollArea *>(QStringLiteral("trajLedger"));
    QVERIFY(ledger);
    QScrollBar *bar = ledger->verticalScrollBar();
    QVERIFY(bar);

    // 1) 控件数与记录数解耦：只实例化可视区（含少量余量）那几十行
    const int rowsAtTop = visibleRows(traj).size();
    QVERIFY(rowsAtTop > 0);
    QVERIFY2(rowsAtTop < 80, qPrintable(QStringLiteral("实例化行数 %1").arg(rowsAtTop)));
    // 但内容高度仍是全量的：能一路滚到最后一轮
    QVERIFY2(bar->maximum() > 1000, qPrintable(QString::number(bar->maximum())));
    // 且窗口起点对得上：视口第一行就是第一条记录
    QVERIFY(rowSignature(visibleRows(traj).first()).contains(QStringLiteral("第 1 轮")));

    // 2) 滚到底：可见窗口换了一批行，且仍在「几十行」这个量级
    const QString topBefore = rowSignature(visibleRows(traj).first());
    bar->setValue(bar->maximum());
    QTest::qWait(30);
    const QList<QWidget *> rowsAtBottom = visibleRows(traj);
    QVERIFY(!rowsAtBottom.isEmpty());
    QVERIFY(rowsAtBottom.size() < 80);
    QVERIFY(rowSignature(rowsAtBottom.first()) != topBefore);
    // 末尾可见行里能看到最后一轮的内容
    QStringList bottomSignatures;
    for (QWidget *row : rowsAtBottom)
        bottomSignatures << rowSignature(row);
    QVERIFY(bottomSignatures.filter(QStringLiteral("400")).size() > 0);

    // 3) 选中态跨窗口保留：选中一行 → 滚走（控件被回收）→ 滚回：仍带选中描边
    bar->setValue(0);
    QTest::qWait(30);
    QWidget *target = visibleRows(traj).first();
    QVERIFY(target);
    const QString targetSignature = rowSignature(target);
    clickWidget(target);
    QTest::qWait(30);
    QVERIFY(target->property("selected").toBool());

    bar->setValue(bar->maximum());
    QTest::qWait(30);
    QWidget *restored = nullptr;
    bar->setValue(0);
    QTest::qWait(30);
    for (QWidget *row : visibleRows(traj)) {
        if (rowSignature(row) == targetSignature) {
            restored = row;
            break;
        }
    }
    QVERIFY2(restored, "滚回来应重新实例化同一行");
    QVERIFY(restored->property("selected").toBool());

    // 4) 命中测试不依赖「整账本都存在」：点可视区最后一行同样能选中
    // （点完可能会为了把它滚进视口而换一次窗口，所以按「行签名」而不是控件指针断言）
    QWidget *tail = visibleRows(traj).last();
    QVERIFY(tail);
    const QString tailSignature = rowSignature(tail);
    clickWidget(tail);
    QTest::qWait(30);
    bool tailSelected = false;
    for (QWidget *row : visibleRows(traj)) {
        if (rowSignature(row) == tailSignature && row->property("selected").toBool())
            tailSelected = true;
    }
    QVERIFY2(tailSelected, "点击的那一行应处于选中态");
}

int main(int argc, char *argv[])
{
    // 无头运行：不弹真实窗口，行为与渲染逻辑不变
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // offscreen 平台默认不加载系统字体，会刷屏 "Cannot find font directory" 警告
    if (qgetenv("QT_QPA_FONTDIR").isEmpty()) {
        const QByteArray windir = qgetenv("WINDIR");
        if (!windir.isEmpty() && QFile::exists(QString::fromLocal8Bit(windir) + QStringLiteral("/Fonts")))
            qputenv("QT_QPA_FONTDIR", windir + "/Fonts");
    }
    QApplication app(argc, argv);
    TestChartWidget test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_chartwidget.moc"

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
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QScopedPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QTextEdit>
#include <QSignalSpy>
#include <QTest>

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

    void historyMergesTurnWithUsageAndTiming();
    void historyToolResultBackfill();

    void optionsOverlayChooseAndEsc();
    void optionsOverlayFreeText();
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

    m_chart->setTheme(QStringLiteral("blue"));
    QCOMPARE(m_chart->theme(), QStringLiteral("blue"));

    // 未知皮肤回退深色
    m_chart->setTheme(QStringLiteral("nope"));
    QCOMPARE(m_chart->theme(), QStringLiteral("dark"));
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
    const QStringList values = labelTexts(m_chart.data(), QStringLiteral("popRowValue"));
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
    QVERIFY(traj && inspector);

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
    m_chart->resize(700, 820);
    QTest::qWait(50);
    QCOMPARE(inspector->width(), 300);

    // webui @media(max-width:560px)：88vw 覆盖式浮层（脱离布局）
    m_chart->resize(500, 820);
    QTest::qWait(70);
    QCOMPARE(inspector->width(), 440); // 500 * 0.88
    QVERIFY(inspector->isVisible());
    QWidget *body = inspector->parentWidget();
    QVERIFY(body);
    QVERIFY(!body->layout() || body->layout()->indexOf(inspector) < 0);

    // 回到宽屏：放回布局并恢复宽度
    m_chart->resize(900, 820);
    QTest::qWait(70);
    QCOMPARE(inspector->width(), 380);
    QVERIFY(inspector->parentWidget()->layout()->indexOf(inspector) >= 0);
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
    QVERIFY(bar->maximum() > bar->value()); // 内容可滚动，且当前贴底

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
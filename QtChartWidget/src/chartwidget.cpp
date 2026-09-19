#include "chartwidget.h"

#include "commonwidgets.h"
#include "composer.h"
#include "markdownview.h"
#include "messagewidgets.h"
#include "phasepanel.h"
#include "popups.h"
#include "settingsdialog.h"
#include "theme.h"
#include "trajectoryview.h"

#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace gs {
namespace {

// app.js ASK_USER_TOOL：询问类工具调用不落成工具条目，改由输入框上方的浮层承担
const char kAskUserTool[] = "ask_user_question";

// app.js usageModelLabel：后端回传的是 provider ID（如 custom），这里换成用户配置的供应商名称
QString usageModelLabel(const QVariantList &models, const QString &raw)
{
    const QString key = raw.trimmed();
    if (key.isEmpty())
        return QString();
    const int slash = key.indexOf(QLatin1Char('/'));
    const QString providerId = slash > 0 ? key.left(slash) : QString();
    const QString modelId = slash > 0 ? key.mid(slash + 1) : key;
    if (providerId.isEmpty())
        return modelId;
    for (const QVariant &item : models) {
        const QVariantMap map = item.toMap();
        if (map.value(QStringLiteral("provider")).toString() != providerId)
            continue;
        const QString name = map.value(QStringLiteral("provider_name")).toString();
        return (name.isEmpty() ? providerId : name) + QStringLiteral(" / ") + modelId;
    }
    return providerId + QStringLiteral(" / ") + modelId;
}

// app.js renderStructured：结构化块里的询问载荷（取最后一个候选块）
QVariantMap askPayloadOf(const QVariantMap &data)
{
    QVariantMap toolParams = data.value(QStringLiteral("tool_params")).toMap();
    if (toolParams.isEmpty())
        toolParams = data.value(QStringLiteral("toolparams")).toMap();
    const QVariantList options = data.value(QStringLiteral("options")).toList();
    if (toolParams.isEmpty() && options.isEmpty())
        return QVariantMap();
    QVariantMap payload;
    if (!toolParams.isEmpty())
        payload.insert(QStringLiteral("tool_params"), toolParams);
    if (!options.isEmpty())
        payload.insert(QStringLiteral("options"), options);
    return payload;
}

bool isSelfOrChildOf(QWidget *widget, QWidget *ancestor)
{
    for (QWidget *p = widget; p; p = p->parentWidget())
        if (p == ancestor)
            return true;
    return false;
}

// 控件自身或其祖先是否带某个 class（QSS 的 .class 动态属性）
bool hasClassOrAncestor(QWidget *widget, const char *cls)
{
    const QString target = QLatin1String(cls);
    for (QWidget *w = widget; w; w = w->parentWidget()) {
        if (w->property("class").toString().split(QLatin1Char(' ')).contains(target))
            return true;
    }
    return false;
}

// app.js extractPhase
QVariantMap extractPhase(const QVariant &value)
{
    const QVariantMap direct = value.toMap();
    if (direct.contains(QStringLiteral("phases")))
        return direct;
    QString text = value.type() == QVariant::String ? value.toString()
                                                    : direct.value(QStringLiteral("text")).toString();
    if (text.isEmpty())
        return QVariantMap();
    const QVariantList blocks = structuredBlocks(text).found;
    for (const QVariant &item : blocks) {
        const QVariantMap map = item.toMap();
        if (map.contains(QStringLiteral("phase_plan")))
            return map.value(QStringLiteral("phase_plan")).toMap();
    }
    return QVariantMap();
}

// 气泡工具结果的失败判定与工具组摘要刷新（app.js renderToolResult / updateToolGroup）
void applyToolResult(ToolItemWidget *item, const QString &result)
{
    const bool failed = result.toLower().contains(QLatin1String("error"))
                        || result.contains(QLatin1String("denied"));
    item->setState(failed ? QStringLiteral("failed") : QStringLiteral("succeeded"));
    item->setResult(result);
    for (QWidget *w = item->parentWidget(); w; w = w->parentWidget()) {
        if (auto *message = qobject_cast<MessageWidget *>(w)) {
            message->updateToolSummary();
            break;
        }
    }
}

// 从 ToolItemWidget 反查它所属的消息气泡
MessageWidget *ownerMessage(QWidget *widget)
{
    for (QWidget *w = widget->parentWidget(); w; w = w->parentWidget())
        if (auto *message = qobject_cast<MessageWidget *>(w))
            return message;
    return nullptr;
}

bool hasToolItems(MessageWidget *message)
{
    return message && !message->findChildren<ToolItemWidget *>().isEmpty();
}

bool hasProcRows(MessageWidget *message)
{
    return message && !message->findChildren<ProcRow *>().isEmpty();
}

// 设置对话框是独立顶级窗口，里面不少容器是「先无父创建、之后才挂进来」的
// （providerSidebar / providerEditor / skillsPanel …）：它们作为临时顶级窗口时就已经被
// polish 过了，之后再也等不到我们的事件过滤器，字距与无障碍属性会整块漏掉。
// 打开对话框时补一次整棵子树。
void applyDeferredStyleDetails(QWidget *root)
{
    applyLetterSpacing(root);
    applyAccessibility(root);
    const QList<QWidget *> all = root->findChildren<QWidget *>();
    for (QWidget *widget : all) {
        applyLetterSpacing(widget);
        applyAccessibility(widget);
    }
}

// sessions 数组里"当前会话"的可选 status 字段由 popups.cpp 的会话面板读取
} // namespace

// ------------------------------------------------------------ 线程契约

// 公开 API 必须在 GUI 线程调用：内部直接操作 QWidget / QSS，Qt 的部件体系没有跨线程保护。
// 契约全文见 README「线程模型」；下面这些「宿主收到网络 / 流式回调后推数据」的入口最容易
// 被误放到工作线程上，所以在运行期兜一层：开发期断言直接暴露，发布期 qWarning + 忽略本次
// 调用（好过静默的内存与绘制损坏）。
bool ChartWidget::assertGuiThread(const char *entry) const
{
    if (QThread::currentThread() == thread())
        return true;
    qWarning("ChartWidget::%s() 被非 GUI 线程调用（当前线程 %p，对象线程 %p）：本次调用被忽略，"
             "请改用信号槽或 QMetaObject::invokeMethod(Qt::QueuedConnection) 切回 GUI 线程",
             entry, QThread::currentThread(), thread());
    Q_ASSERT_X(false, "ChartWidget", "公开 API 被非 GUI 线程调用");
    return false;
}

// ------------------------------------------------------------ 构造

ChartWidget::ChartWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("appShell"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAcceptDrops(true);
    setMinimumSize(420, 460);

    // QSS 里没有全局字体规则，基础字号/字体族要在根部件上显式设置
    QFont font(uiFont());
    font.setPixelSize(scaledPx(basePixelSize()));
    setFont(font);
    setStyleSheet(appStyleSheet());

    // VSCode 式界面缩放快捷键：Ctrl+= / Ctrl++ 放大，Ctrl+- 缩小，Ctrl+0 重置
    const QList<QKeySequence> zoomInKeys{QKeySequence(QStringLiteral("Ctrl+=")),
                                         QKeySequence(QStringLiteral("Ctrl++"))};
    for (const QKeySequence &seq : zoomInKeys) {
        auto *shortcut = new QShortcut(seq, this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, &ChartWidget::zoomIn);
    }
    auto *zoomOutShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+-")), this);
    zoomOutShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(zoomOutShortcut, &QShortcut::activated, this, &ChartWidget::zoomOut);
    auto *zoomResetShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+0")), this);
    zoomResetShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(zoomResetShortcut, &QShortcut::activated, this, &ChartWidget::zoomReset);

    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(0, 0, 0, 0);
    m_root->setSpacing(0);

    buildTopbar();
    buildSessionbar();
    buildViewTabs();
    buildMessages();
    buildTurnRail();

    // 轨迹视图与消息区互斥（.trajectory-view / #messages）
    m_trajView = new TrajectoryView(this);
    m_trajView->setVisible(false);
    m_root->addWidget(m_trajView, 1);

    // .phase-panel { margin: 0 9px 8px }
    m_phaseWrap = new QWidget(this);
    auto *phaseLayout = new QVBoxLayout(m_phaseWrap);
    phaseLayout->setContentsMargins(9, 0, 9, 8);
    phaseLayout->setSpacing(0);
    m_phasePanel = new PhasePanel(m_phaseWrap);
    phaseLayout->addWidget(m_phasePanel);
    m_phaseWrap->setVisible(false);
    m_root->addWidget(m_phaseWrap);

    m_composer = new Composer(this);
    m_root->addWidget(m_composer);

    // 叠加层：不入布局，靠 layoutOverlays() 定位（.app-shell { position: relative }）
    m_sessionPanel = new SessionPanel(this);
    m_toast = new Toast(this);
    m_dropOverlay = new DropOverlay(this);
    m_themePopup = new ThemeListPopup(this);

    m_settings = new SettingsDialog(this);
    m_settings->setModal(true);
    m_settings->setWindowModality(Qt::WindowModal);

    connect(m_connection, &ConnectionButton::clicked, this, &ChartWidget::connectionCheckRequested);
    connect(m_newSession, &QPushButton::clicked, this, &ChartWidget::newSessionRequested);

    connect(m_sessionPanel, &SessionPanel::sessionSelected, this, [this](const QString &id) {
        closeSessionPanel();
        emit sessionSelected(id);
    });
    connect(m_sessionPanel, &SessionPanel::sessionRenamed, this, &ChartWidget::sessionRenamed);
    connect(m_sessionPanel, &SessionPanel::sessionCleared, this, &ChartWidget::sessionCleared);
    connect(m_sessionPanel, &SessionPanel::sessionDeleted, this, &ChartWidget::sessionDeleted);
    connect(m_sessionPanel, &SessionPanel::closeRequested, this, &ChartWidget::closeSessionPanel);

    connect(m_themePopup, &ThemeListPopup::themeChosen, this, [this](const QString &id) {
        applyTheme(id);
        emit themeChanged(id);
    });

    connect(m_tabChat, &QPushButton::clicked, this, [this] { setViewTab(QStringLiteral("chat")); });
    connect(m_tabTraj, &QPushButton::clicked, this, [this] { setViewTab(QStringLiteral("traj")); });

    connect(m_composer, &Composer::sendMessage, this, &ChartWidget::sendMessage);
    connect(m_composer, &Composer::stopRequested, this, &ChartWidget::stopRequested);
    connect(m_composer, &Composer::modeChanged, this, &ChartWidget::modeChanged);
    connect(m_composer, &Composer::modelSelected, this, &ChartWidget::modelSelected);
    connect(m_composer, &Composer::skillSelected, this, &ChartWidget::skillSelected);
    connect(m_composer, &Composer::settingsRequested, this, [this] { openSettings(); });
    connect(m_composer, &Composer::attachRequested, this, &ChartWidget::attachRequested);
    connect(m_composer, &Composer::voiceRequested, this, &ChartWidget::voiceRequested);
    connect(m_composer, &Composer::attachmentRemoved, this, &ChartWidget::attachmentRemoved);
    connect(m_composer, &Composer::optionChosen, this, &ChartWidget::optionChosen);
    connect(m_composer, &Composer::approvalDecided, this, &ChartWidget::approvalDecided);
    connect(m_composer, &Composer::choiceOpenChanged, this, [this](bool open) {
        syncPhaseLift();
        emit choiceOpenChanged(open);
    });
    connect(m_composer, &Composer::choiceResized, this, &ChartWidget::syncPhaseLift);

    connect(m_trajView, &TrajectoryView::reloadRequested, this,
            &ChartWidget::trajectoryReloadRequested);

    connect(m_settings, &SettingsDialog::saveRequested, this, &ChartWidget::settingsSaveRequested);
    connect(m_settings, &SettingsDialog::testProviderRequested, this,
            &ChartWidget::testProviderRequested);
    connect(m_settings, &SettingsDialog::readModelsRequested, this,
            &ChartWidget::readModelsRequested);
    connect(m_settings, &SettingsDialog::refreshSkillsRequested, this,
            &ChartWidget::refreshSkillsRequested);
    connect(m_settings, &SettingsDialog::refreshMcpRequested, this,
            &ChartWidget::refreshMcpRequested);

    // 消息区滚动：贴底才跟随；离开底部即交出滚动控制权，同时刷新导航轨高亮
    connect(m_messages->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        m_followBottom = atBottom();
        updateTurnRailActive();
    });

    // 会话触发器/皮肤触发器都不是 QPushButton（要放省略号标题 + 图标），点击由 app 级
    // 过滤器统一接管：app filter 先于目标部件执行，且鼠标事件会沿 parentWidget() 链每层
    // 重走一次过滤器，所以在触发器子树上命中即 return true，避免冒泡导致重复开合。
    m_currentTitle->installEventFilter(this);
    m_composer->installEventFilter(this); // 输入区高度变化时重排 Toast / 导航轨
    qApp->installEventFilter(this);
}

ChartWidget::~ChartWidget()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

// ------------------------------------------------------------ 骨架

void ChartWidget::buildTopbar()
{
    // .topbar { height:48px; padding:0 12px; display:flex; align-items:center }
    auto *topbar = new QWidget(this);
    topbar->setObjectName(QStringLiteral("topbar"));
    topbar->setAttribute(Qt::WA_StyledBackground, true);
    topbar->setFixedHeight(48);
    auto *layout = new QHBoxLayout(topbar);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(8);

    auto *mark = new QLabel(topbar);
    mark->setObjectName(QStringLiteral("brandMark"));
    mark->setAttribute(Qt::WA_StyledBackground, true);
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(24, 24);
    const QPixmap logo(QStringLiteral(":/icons/Logo.ico"));
    if (!logo.isNull())
        mark->setPixmap(logo.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    auto *brand = new QLabel(QStringLiteral("GridStar Agent"), topbar);
    brand->setObjectName(QStringLiteral("brandText"));

    m_connection = new ConnectionButton(topbar);

    // 皮肤切换（.theme-switch）：幽灵触发器 + 下拉
    m_themeTrigger = new QWidget(topbar);
    m_themeTrigger->setObjectName(QStringLiteral("themeTrigger"));
    m_themeTrigger->setAttribute(Qt::WA_StyledBackground, true);
    m_themeTrigger->setFixedHeight(28);
    m_themeTrigger->setCursor(Qt::PointingHandCursor);
    m_themeTrigger->setToolTip(QStringLiteral("切换皮肤"));
    auto *themeLayout = new QHBoxLayout(m_themeTrigger);
    themeLayout->setContentsMargins(8, 0, 8, 0);
    themeLayout->setSpacing(6);
    m_themeSwatch = new ThemeSwatch(themeId(), 12, true, m_themeTrigger);
    m_themeLabel = new QLabel(themeName(), m_themeTrigger);
    m_themeLabel->setObjectName(QStringLiteral("themeLabel"));
    m_themeLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    auto *themeChevron = new QLabel(m_themeTrigger);
    setClass(themeChevron, QStringLiteral("chevronGlyph"));
    themeChevron->setPixmap(iconPixmap(QStringLiteral("chevron-down"), gs::palette().muted, 12));
    themeLayout->addWidget(m_themeSwatch, 0);
    themeLayout->addWidget(m_themeLabel, 0);
    themeLayout->addWidget(themeChevron, 0);

    layout->addWidget(mark);
    layout->addWidget(brand);
    layout->addStretch(1); // .connection { margin-left:auto }
    layout->addWidget(m_connection);
    layout->addWidget(m_themeTrigger);
    m_root->addWidget(topbar);
}

void ChartWidget::buildSessionbar()
{
    // .sessionbar { height:44px; padding:6px 9px; display:flex; align-items:center }
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("sessionbar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(44);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(9, 6, 9, 6);
    layout->setSpacing(8); // .sessionbar>*+* { margin-left:8px }

    auto *newSession = new IconPushButton(bar);
    newSession->setObjectName(QStringLiteral("newSession"));
    newSession->setFixedHeight(30); // .compact { height:30px }
    newSession->setCursor(Qt::PointingHandCursor);
    newSession->setText(QStringLiteral("新对话"));
    newSession->setIconColors(gs::palette().muted, gs::palette().cyan);
    newSession->setIconName(QStringLiteral("plus"), 14);
    m_newSession = newSession;
    layout->addWidget(m_newSession);

    // .session-trigger { flex:1; height:30px; justify-content:flex-end; padding:0 5px }
    m_sessionTrigger = new QWidget(bar);
    m_sessionTrigger->setObjectName(QStringLiteral("sessionTrigger"));
    m_sessionTrigger->setAttribute(Qt::WA_StyledBackground, true);
    m_sessionTrigger->setFixedHeight(30);
    m_sessionTrigger->setCursor(Qt::PointingHandCursor);
    auto *triggerLayout = new QHBoxLayout(m_sessionTrigger);
    triggerLayout->setContentsMargins(5, 0, 5, 0);
    triggerLayout->setSpacing(7); // .session-trigger>*+* { margin-left:7px }

    // 不用 makeLabel：它带 TextSelectableByMouse，会吞掉鼠标事件导致点击不冒泡到触发器
    m_currentTitle = new QLabel(QStringLiteral("选择会话"), m_sessionTrigger);
    m_currentTitle->setObjectName(QStringLiteral("currentTitle"));
    m_currentTitle->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_currentTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_currentTitle->setProperty("full", QStringLiteral("选择会话"));

    m_sessionChevron = new QLabel(m_sessionTrigger);
    setClass(m_sessionChevron, QStringLiteral("chevronGlyph"));
    m_sessionChevron->setPixmap(iconPixmap(QStringLiteral("chevron-down"), gs::palette().muted, 12));

    triggerLayout->addWidget(m_currentTitle, 1);
    triggerLayout->addWidget(m_sessionChevron);
    layout->addWidget(m_sessionTrigger, 1);
    m_root->addWidget(bar);
}

void ChartWidget::buildViewTabs()
{
    // .view-tabs { height:34px; display:flex; border-bottom:1px solid line }
    m_viewTabs = new QWidget(this);
    m_viewTabs->setObjectName(QStringLiteral("viewTabs"));
    m_viewTabs->setAttribute(Qt::WA_StyledBackground, true);
    m_viewTabs->setFixedHeight(34);
    auto *layout = new QHBoxLayout(m_viewTabs);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tabChat = new QPushButton(QStringLiteral("对话"), m_viewTabs);
    setClass(m_tabChat, QStringLiteral("viewTab"));
    m_tabChat->setProperty("active", true);
    m_tabChat->setCursor(Qt::PointingHandCursor);
    m_tabChat->setFocusPolicy(Qt::TabFocus); // webui 的 .view-tab 是 button：可 Tab、不留点击焦点环

    m_tabTraj = new QPushButton(QStringLiteral("轨迹"), m_viewTabs);
    setClass(m_tabTraj, QStringLiteral("viewTab"));
    m_tabTraj->setProperty("active", false);
    m_tabTraj->setCursor(Qt::PointingHandCursor);
    m_tabTraj->setFocusPolicy(Qt::TabFocus);

    layout->addWidget(m_tabChat);
    layout->addWidget(m_tabTraj);
    layout->addStretch(1);
    m_root->addWidget(m_viewTabs);
}

void ChartWidget::buildMessages()
{
    // .messages { flex:1; overflow-y:auto; padding:13px 11px }
    m_messages = new QScrollArea(this);
    m_messages->setObjectName(QStringLiteral("messages"));
    m_messages->setFrameShape(QFrame::NoFrame);
    m_messages->setWidgetResizable(true);
    m_messages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_messages->viewport()->setAutoFillBackground(false);

    m_messageList = new QWidget(m_messages);
    m_messageList->setObjectName(QStringLiteral("messageList"));
    m_messageLayout = new QVBoxLayout(m_messageList);
    // .message { margin:0 0 13px } → 间距 13，底部留白由最后一条消息的 margin 提供
    m_messageLayout->setContentsMargins(11, 13, 11, 0);
    m_messageLayout->setSpacing(13);

    m_emptyState = createEmptyState();
    m_messageLayout->addWidget(m_emptyState, 1);
    m_messageLayout->addStretch(0);
    m_messages->setWidget(m_messageList);
    m_root->addWidget(m_messages, 1);
}

void ChartWidget::buildTurnRail()
{
    // .turn-rail：固定在视口左侧，竖排白点，一轮一个（不足一轮时隐藏）
    m_turnRail = new QWidget(this);
    m_turnRail->setObjectName(QStringLiteral("turnRail"));
    m_turnRail->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(m_turnRail);
    layout->setContentsMargins(0, 6, 0, 6);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignHCenter);
    m_turnRail->setVisible(false);

    m_turnRailTip = new QFrame(this);
    m_turnRailTip->setObjectName(QStringLiteral("turnRailTip"));
    m_turnRailTip->setAttribute(Qt::WA_StyledBackground, true);
    m_turnRailTip->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto *tipLayout = new QVBoxLayout(m_turnRailTip);
    tipLayout->setContentsMargins(9, 7, 9, 7);
    m_turnRailTipText = new QLabel(m_turnRailTip);
    m_turnRailTipText->setObjectName(QStringLiteral("turnRailTipText"));
    m_turnRailTipText->setWordWrap(true);
    m_turnRailTipText->setTextInteractionFlags(Qt::NoTextInteraction);
    tipLayout->addWidget(m_turnRailTipText);
    m_turnRailTip->setVisible(false);
}

QWidget *ChartWidget::createEmptyState()
{
    // .empty-state { flex:1; column; center; text-align:center; padding:30px }
    auto *box = new QWidget(m_messageList);
    box->setObjectName(QStringLiteral("emptyState"));
    box->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(30, 30, 30, 30);
    layout->setSpacing(0);

    auto *symbol = new QLabel(box);
    symbol->setObjectName(QStringLiteral("emptySymbol"));
    symbol->setAttribute(Qt::WA_StyledBackground, true);
    symbol->setAlignment(Qt::AlignCenter);
    symbol->setFixedSize(40, 40);
    symbol->setPixmap(iconPixmap(QStringLiteral("zap"), gs::palette().cyan, 22));
    symbol->setContentsMargins(0, 0, 0, 12); // .empty-symbol { margin-bottom:12px }

    auto *title = new QLabel(QStringLiteral("对话已就绪"), box);
    title->setObjectName(QStringLiteral("emptyTitle"));
    title->setAlignment(Qt::AlignCenter);

    auto *hint = new QLabel(QStringLiteral("描述你的工程目标，Agent 将按当前模式执行。"), box);
    hint->setObjectName(QStringLiteral("emptyHint"));
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    hint->setMaximumWidth(270); // .empty-state p { max-width:270px; margin:7px 0 }
    hint->setContentsMargins(0, 7, 0, 7);

    layout->addStretch(1);
    layout->addWidget(symbol, 0, Qt::AlignHCenter);
    layout->addWidget(title, 0, Qt::AlignHCenter);
    layout->addWidget(hint, 0, Qt::AlignHCenter);
    layout->addStretch(1);
    return box;
}

void ChartWidget::removeWelcome()
{
    if (!m_emptyState)
        return;
    m_messageLayout->removeWidget(m_emptyState);
    m_emptyState->deleteLater();
    m_emptyState = nullptr;
    const int last = m_messageLayout->count() - 1;
    if (last >= 0)
        m_messageLayout->setStretch(last, 1); // 有消息时尾部弹簧撑开，消息贴顶
}

void ChartWidget::updateEmptyState()
{
    bool hasMessages = false;
    for (int i = 0; i < m_messageLayout->count(); ++i) {
        QWidget *w = m_messageLayout->itemAt(i)->widget();
        if (w && w != m_emptyState) {
            hasMessages = true;
            break;
        }
    }
    if (hasMessages) {
        removeWelcome();
        return;
    }
    if (m_emptyState)
        return;
    m_emptyState = createEmptyState();
    m_messageLayout->insertWidget(0, m_emptyState, 1);
    const int last = m_messageLayout->count() - 1;
    if (last >= 0)
        m_messageLayout->setStretch(last, 0);
}

void ChartWidget::updateTitleElide()
{
    // #current-title { overflow:hidden; text-overflow:ellipsis; white-space:nowrap }
    const QString full = m_currentTitle->property("full").toString();
    const QString source = full.isEmpty() ? QStringLiteral("选择会话") : full;
    const QString elided = elidedText(source, m_currentTitle->fontMetrics(), m_currentTitle->width());
    if (m_currentTitle->text() == elided)
        return;
    m_currentTitle->setText(elided);
}

void ChartWidget::layoutOverlays()
{
    const QSize host = size();
    m_sessionPanel->layoutIn(host);
    // Toast 浮在输入区之上（输入区高度随控件行换行变化，不能用固定底距）
    m_toast->layoutIn(host, m_composer ? m_composer->height() + 12 : 125);
    m_dropOverlay->layoutIn(host);
    updateTitleElide();

    // 轮次导航轨：左边缘、垂直居中，最高占视口 62%
    if (m_turnRail) {
        const int dotsHeight = m_railDots.size() * 18 + 12;
        const int railHeight = qMin(dotsHeight, qRound(host.height() * 0.62));
        const int top = qMax(0, (host.height() - railHeight) / 2);
        m_turnRail->setGeometry(0, top, 12, railHeight);
    }
    if (m_turnRailTip && m_turnRailTip->isVisible())
        showTurnRailTip(m_railActive);
}

void ChartWidget::scrollToEnd(bool force)
{
    if (m_scrollLocked)
        return;
    if (force)
        m_followBottom = true;
    else if (!m_followBottom)
        return; // 贴底才跟随
    QScrollBar *bar = m_messages->verticalScrollBar();
    QTimer::singleShot(0, this, [bar] { bar->setValue(bar->maximum()); });
}

bool ChartWidget::atBottom() const
{
    // app.js BOTTOM_SLACK = 24
    QScrollBar *bar = m_messages->verticalScrollBar();
    return bar->maximum() - bar->value() <= 24;
}

// ------------------------------------------------------------ 轮次导航轨

void ChartWidget::rebuildTurnRail()
{
    if (!m_turnRail)
        return;
    QList<MessageWidget *> turns;
    if (m_viewTab == QLatin1String("chat")) {
        for (int i = 0; i < m_messageLayout->count(); ++i) {
            auto *message = qobject_cast<MessageWidget *>(m_messageLayout->itemAt(i)->widget());
            if (message && message->role() == QLatin1String("user"))
                turns.append(message);
        }
    }

    bool same = turns.size() == m_railTurns.size();
    if (same) {
        for (int i = 0; i < turns.size(); ++i) {
            if (m_railTurns.at(i).data() != turns.at(i)) {
                same = false;
                break;
            }
        }
    }
    if (!same) {
        hideTurnRailTip();
        m_railTurns.clear();
        m_railDots.clear();
        // 清空轨道上的旧点
        if (QLayout *layout = m_turnRail->layout()) {
            while (QLayoutItem *item = layout->takeAt(0)) {
                if (QWidget *w = item->widget())
                    w->deleteLater();
                delete item;
            }
        }
        for (int i = 0; i < turns.size(); ++i) {
            auto *dot = new TurnRailDot(i, m_turnRail);
            connect(dot, &TurnRailDot::activated, this, &ChartWidget::focusTurn);
            connect(dot, &TurnRailDot::hovered, this, &ChartWidget::showTurnRailTip);
            connect(dot, &TurnRailDot::unhovered, this, &ChartWidget::hideTurnRailTip);
            if (auto *layout = qobject_cast<QVBoxLayout *>(m_turnRail->layout()))
                layout->addWidget(dot, 0, Qt::AlignHCenter);
            m_railTurns.append(turns.at(i));
            m_railDots.append(dot);
        }
    }
    m_turnRail->setVisible(!turns.isEmpty());
    if (m_turnRail->isVisible())
        layoutOverlays();
    updateTurnRailActive();
}

void ChartWidget::updateTurnRailActive()
{
    if (m_railTurns.isEmpty())
        return;
    const int mid = m_messages->viewport()->height() / 2;
    int active = 0;
    for (int i = 0; i < m_railTurns.size() && i < m_railDots.size(); ++i) {
        MessageWidget *message = m_railTurns.at(i).data();
        if (!message)
            continue;
        const int top = message->mapTo(m_messages->viewport(), QPoint(0, 0)).y();
        if (top <= mid)
            active = i;
    }
    m_railActive = active;
    for (int i = 0; i < m_railDots.size(); ++i)
        m_railDots.at(i)->setActive(i == active);
}

void ChartWidget::showTurnRailTip(int turn)
{
    if (!m_turnRailTip || turn < 0 || turn >= m_railTurns.size())
        return;
    MessageWidget *message = m_railTurns.at(turn).data();
    if (!message)
        return;
    const QString text = message->property("copyText").toString().simplified();
    m_turnRailTipText->setText(text.isEmpty() ? QStringLiteral("（本轮无文本内容）") : text);
    m_turnRailTip->adjustSize();
    const int maxWidth = qMax(120, width() / 3);
    if (m_turnRailTip->width() > maxWidth)
        m_turnRailTip->setFixedWidth(maxWidth);
    const int dotY = m_turnRail->y() + 6 + turn * 18;
    int top = dotY + 6 - m_turnRailTip->height() / 2;
    top = qBound(8, top, qMax(8, height() - m_turnRailTip->height() - 8));
    m_turnRailTip->move(22, top);
    m_turnRailTip->raise();
    m_turnRailTip->setVisible(true);
}

void ChartWidget::hideTurnRailTip()
{
    if (m_turnRailTip)
        m_turnRailTip->setVisible(false);
}

void ChartWidget::focusTurn(int turn)
{
    if (turn < 0 || turn >= m_railTurns.size())
        return;
    MessageWidget *message = m_railTurns.at(turn).data();
    if (!message)
        return;
    // 定位到该轮顶部（block:"start"）
    const int top = message->mapTo(m_messageList, QPoint(0, 0)).y();
    m_messages->verticalScrollBar()->setValue(qMax(0, top - 13));
}

// ------------------------------------------------------------ 视图页签

void ChartWidget::setViewTab(const QString &tab)
{
    if (m_viewTab == tab)
        return;
    m_viewTab = tab;
    const bool traj = tab == QLatin1String("traj");
    m_tabChat->setProperty("active", !traj);
    m_tabTraj->setProperty("active", traj);
    restyle(m_tabChat);
    restyle(m_tabTraj);
    m_trajView->setVisible(traj);
    m_messages->setVisible(!traj);
    m_composer->setVisible(!traj);
    if (traj) {
        m_phaseWrap->setVisible(false);
    } else if (!m_phasePanel->isHidden()) {
        // 用 isHidden 而不是 isVisible：切到轨迹时计划窗口是被父级隐藏的，
        // isVisible() 此时为假，会导致切回对话后再也恢复不出来
        m_phaseWrap->setVisible(true);
        scrollToEnd(true);
    }
    rebuildTurnRail();
    layoutOverlays();
    emit viewTabChanged(tab);
}

// ------------------------------------------------------------ 皮肤

void ChartWidget::setTheme(const QString &id)
{
    applyTheme(id);
    emit themeChanged(gs::themeId());
}

QString ChartWidget::theme() const
{
    return gs::themeId();
}

void ChartWidget::applyTheme(const QString &id)
{
    gs::setTheme(id);
    setStyleSheet(appStyleSheet());
    // 皮肤切换后已按旧色着色的图标要重建
    m_sessionChevron->setPixmap(iconPixmap(QStringLiteral("chevron-down"), gs::palette().muted, 12));
    m_connection->update();

    const QList<ThemeSwatch *> swatches = findChildren<ThemeSwatch *>();
    for (ThemeSwatch *swatch : swatches)
        swatch->update();

    // 图标按钮的着色需显式重设（QSS 的 color 不作用于 QIcon）
    for (IconPushButton *button : findChildren<IconPushButton *>()) {
        const QString cls = button->property("class").toString();
        const QString name = button->objectName();
        if (name == QLatin1String("sendButton"))
            continue;
        if (name == QLatin1String("newSession"))
            button->setIconColors(gs::palette().muted, gs::palette().cyan);
        else if (cls.contains(QLatin1String("sessionAction")))
            button->setIconColors(gs::palette().muted,
                                  button->property("danger").toBool() ? gs::palette().red
                                                                      : gs::palette().cyan);
        else
            button->setIconColors(gs::palette().muted2, gs::palette().cyan);
    }

    if (m_themeSwatch)
        m_themeSwatch->update();
    if (m_themeLabel)
        m_themeLabel->setText(themeName());
    if (m_themePopup)
        m_themePopup->setCurrent(gs::themeId());

    // 自绘控件（连接状态点、阶段项、进度条、导航轨…）重绘即可
    for (QWidget *w : findChildren<QWidget *>())
        w->update();
}

// ------------------------------------------------------------ 顶栏 / 会话栏

void ChartWidget::setConnectionState(const QString &state, const QString &label)
{
    if (!assertGuiThread(__func__))
        return;
    m_connection->setState(state, label);
}

void ChartWidget::setSessions(const QVariantList &sessions)
{
    if (!assertGuiThread(__func__))
        return;
    m_sessions = sessions;
    m_sessionPanel->setSessions(sessions);
}

void ChartWidget::setCurrentSessionTitle(const QString &title)
{
    if (!assertGuiThread(__func__))
        return;
    m_currentTitle->setProperty("full", title);
    updateTitleElide();
}

QString ChartWidget::currentSessionTitle() const
{
    return m_currentTitle->property("full").toString();
}

void ChartWidget::toggleSessionPanel()
{
    if (m_sessionPanel->isOpen()) {
        closeSessionPanel();
        return;
    }
    m_sessionPanel->setSessions(m_sessions);
    m_sessionPanel->layoutIn(size());
    m_sessionPanel->open();
}

void ChartWidget::closeSessionPanel()
{
    m_sessionPanel->closePanel();
}

// ------------------------------------------------------------ 输入区

void ChartWidget::setModels(const QVariantList &models)
{
    m_models = models; // 用量弹层的「提供方 / 模型」按 provider_name 映射（app.js usageModelLabel）
    m_composer->setModels(models);
}
void ChartWidget::setCurrentModel(const QString &key) { m_composer->setCurrentModel(key); }
QString ChartWidget::currentModel() const { return m_composer->currentModel(); }

void ChartWidget::setSkills(const QVariantList &skills)
{
    m_skills = skills; // 供宿主查询；气泡不再显示 Skill 名（b12146e）
    m_composer->setSkills(skills);
}
void ChartWidget::setCurrentSkill(const QString &id) { m_composer->setCurrentSkill(id); }
QString ChartWidget::currentSkill() const { return m_composer->currentSkill(); }
void ChartWidget::setMode(const QString &mode) { m_composer->setMode(mode); }
QString ChartWidget::mode() const { return m_composer->mode(); }

void ChartWidget::setBusy(bool busy)
{
    m_composer->setBusy(busy);
    if (busy)
        return;
    // app.js renderFailure：sendMessage().finally(() => button.disabled = false)
    const QList<QPushButton *> buttons = m_messageList->findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
        if (button->property("retry").toBool())
            button->setEnabled(true);
}
bool ChartWidget::isBusy() const { return m_composer->isBusy(); }
void ChartWidget::setConfigLoaded(bool loaded) { m_composer->setConfigLoaded(loaded); }
void ChartWidget::setConfigWarning(const QString &text) { m_composer->setConfigWarning(text); }

QString ChartWidget::inputText() const { return m_composer->text(); }
void ChartWidget::setInputText(const QString &text) { m_composer->setText(text); }
void ChartWidget::focusInput() { m_composer->focusInput(); }

void ChartWidget::addAttachments(const QVariantList &items) { m_composer->addAttachments(items); }
void ChartWidget::clearAttachments() { m_composer->clearAttachments(); }
QVariantList ChartWidget::attachments() const { return m_composer->attachments(); }
void ChartWidget::setVoiceEnabled(bool enabled) { m_composer->setVoiceEnabled(enabled); }
void ChartWidget::setVoiceRecording(bool recording) { m_composer->setVoiceRecording(recording); }

// ------------------------------------------------------------ 选择 / 审批

void ChartWidget::showChoice(const QVariantMap &payload)
{
    if (!assertGuiThread(__func__))
        return;
    m_composer->showChoice(payload);
}

void ChartWidget::closeChoice()
{
    m_composer->closeChoice();
}

void ChartWidget::appendApproval(const QVariantMap &event)
{
    if (!assertGuiThread(__func__))
        return;
    QVariantMap approval;
    approval.insert(QStringLiteral("event"), event);
    QVariantMap payload;
    payload.insert(QStringLiteral("approval"), approval);
    showChoice(payload);
}

void ChartWidget::resolveApproval(const QString &callId, bool approved)
{
    m_composer->setApprovalResolved(callId, approved);
}

void ChartWidget::reEnableApproval(const QString &callId)
{
    m_composer->reEnableApproval(callId);
}

// ------------------------------------------------------------ 消息流

MessageWidget *ChartWidget::createMessage(const QString &role, const QString &content,
                                          const QString &label, const QVariantList &attachments)
{
    removeWelcome();
    auto *message = new MessageWidget(role, label, m_messageList);
    m_messageLayout->insertWidget(m_messageLayout->count() - 1, message); // 插在尾部弹簧之前
    message->setAttachments(attachments);
    message->body()->setText(content);
    message->setBodyVisible(message->body()->hasVisibleContent());
    connect(message, &MessageWidget::copyRequested, this, [this](const QString &text) {
        // app.js 卡片拷贝：无内容时提示，成功/失败各给一条 toast
        const QString payload = text.isEmpty() ? QString() : text;
        if (!payload.trimmed().isEmpty()) {
            QApplication::clipboard()->setText(payload);
            showToast(QStringLiteral("已复制到剪贴板"));
        } else {
            showToast(QStringLiteral("没有可复制的内容"));
        }
    });
    scrollToEnd();
    rebuildTurnRail();
    return message;
}

MessageWidget *ChartWidget::ensureAssistant()
{
    if (m_current && !m_currentFinished)
        return m_current;
    // 气泡不再显示 Skill 名（b12146e），只保留失败/停止等状态标签
    m_current = createMessage(QStringLiteral("assistant"), QString());
    m_currentText.clear();
    m_currentFinished = false;
    return m_current;
}

void ChartWidget::appendUserMessage(const QString &content, const QVariantList &attachments)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *message = createMessage(QStringLiteral("user"), content, QString(), attachments);
    message->setProperty("copyText", content);
    // app.js sendMessage：新一轮提问必须落底并恢复自动跟随，即使上一轮用户上滚停留在历史里
    scrollToEnd(true);
}

void ChartWidget::appendAssistantMessage(const QString &content, const QString &label,
                                         const QVariantList &attachments)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *message = createMessage(QStringLiteral("assistant"), content, label, attachments);
    // 一次性追加：正文里的结构化块立即渲染成卡片（等价于 createMessage + finishAssistant）
    const StructuredBlocks parsed = structuredBlocks(content);
    message->body()->setText(parsed.visible);
    message->setBodyVisible(message->body()->hasVisibleContent());
    message->setCopyText(parsed.visible);
    QVariantMap ask;
    for (const QVariant &item : parsed.found) {
        const QVariantMap data = item.toMap();
        renderStructured(data, message);
        const QVariantMap candidate = askPayloadOf(data);
        if (!candidate.isEmpty())
            ask = candidate;
    }
    if (!ask.isEmpty())
        m_composer->showChoice(ask);
    scrollToEnd();
}

void ChartWidget::appendAssistantText(const QString &delta)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *message = ensureAssistant();
    m_currentText += delta;
    message->body()->setText(m_currentText);
    message->setBodyVisible(message->body()->hasVisibleContent());
    message->setCopyText(m_currentText);
    message->settleThink();
    scrollToEnd();
}

void ChartWidget::appendReasoning(const QString &delta)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *message = ensureAssistant();
    message->appendReasoning(delta);
    scrollToEnd();
}

void ChartWidget::finishAssistant()
{
    if (!assertGuiThread(__func__))
        return;
    finishAssistantInternal(false);
}

void ChartWidget::finishAssistantInternal(bool deferred)
{
    MessageWidget *message = m_current;
    if (!message || m_currentFinished)
        return;
    m_currentFinished = true;

    message->stopLiveTiming();
    message->settleProcess();

    const StructuredBlocks parsed = structuredBlocks(m_currentText);
    message->body()->setText(parsed.visible);
    message->setBodyVisible(message->body()->hasVisibleContent());
    message->setCopyText(parsed.visible);

    // 文本兜底的 options / tool_params 块也算一次待作答询问，取最后一个候选块
    QVariantMap ask;
    for (const QVariant &item : parsed.found) {
        const QVariantMap data = item.toMap();
        renderStructured(data, message);
        const QVariantMap candidate = askPayloadOf(data);
        if (!candidate.isEmpty())
            ask = candidate;
    }
    m_lastTurnAwaiting = !ask.isEmpty();

    // 只有思考过程/工具调用、没有正文时也要留住这一轮
    if (parsed.visible.trimmed().isEmpty() && parsed.found.isEmpty()
        && !hasToolItems(message) && !hasProcRows(message)) {
        if (m_current.data() == message)
            m_current = nullptr;
        message->deleteLater();
        rebuildTurnRail();
        return;
    }

    // 历史重放要等整轮重放完再弹，否则中途那些旧询问会闪一下
    if (!deferred && !ask.isEmpty())
        m_composer->showChoice(ask);

    // 计划窗口只服务执行过程：本轮结束时计划已全部完成就收起
    if (planComplete(m_phasePlanData))
        m_phaseWrap->setVisible(false);

    m_currentText.clear();
    updateEmptyState();
    rebuildTurnRail();
    syncPhaseLift();
    scrollToEnd();
}

bool ChartWidget::planComplete(const QVariantMap &plan)
{
    const QVariantList phases = plan.value(QStringLiteral("phases")).toList();
    if (phases.isEmpty())
        return false;
    static const QStringList doneStatuses{ QStringLiteral("done"), QStringLiteral("succeeded"),
                                           QStringLiteral("completed"), QStringLiteral("skipped") };
    for (const QVariant &item : phases) {
        if (!doneStatuses.contains(item.toMap().value(QStringLiteral("status")).toString()))
            return false;
    }
    return true;
}

void ChartWidget::renderStructured(const QVariantMap &data, MessageWidget *message)
{
    if (!message)
        return;

    if (data.contains(QStringLiteral("phase_plan")))
        setPhasePlan(data.value(QStringLiteral("phase_plan")));

    // tool_params / options 合并到输入框上方的浮层（见 ChoiceOverlay），对话流里不再单独成卡
    if (data.contains(QStringLiteral("workflow"))) {
        const QVariantList steps =
            data.value(QStringLiteral("workflow")).toMap().value(QStringLiteral("steps")).toList();
        auto *card = new WorkflowProposalCard(steps, message);
        connect(card, &WorkflowProposalCard::runRequested, this,
                &ChartWidget::workflowRunRequested);
        message->stack()->addWidget(card);
    }
}

void ChartWidget::appendToolCall(const QString &callId, const QString &name, const QVariant &args)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *message = ensureAssistant();
    if (!message)
        return;
    message->settleThink();
    // app.js handleStreamEvent：询问类调用先落定思考，但不渲染工具条目
    if (name == QLatin1String(kAskUserTool))
        return;
    ToolItemWidget *item = message->addToolCall(callId, name, args);
    if (item && !callId.isEmpty())
        m_toolItems.insert(callId, item);
    scrollToEnd();
}

void ChartWidget::appendToolResult(const QString &callId, const QString &name,
                                   const QString &result)
{
    if (!assertGuiThread(__func__))
        return;
    // app.js handleStreamEvent：询问类调用的结果同样不落成工具条目
    if (name == QLatin1String(kAskUserTool))
        return;
    ToolItemWidget *item = m_toolItems.value(callId, nullptr);
    if (!item) {
        // app.js renderToolResult：调用项不存在时先补一条 running 的调用
        MessageWidget *message = ensureAssistant();
        if (!message)
            return;
        item = message->addToolCall(callId, name, QVariantMap());
        if (item && !callId.isEmpty())
            m_toolItems.insert(callId, item);
    }
    if (!item)
        return;
    applyToolResult(item, result);
    scrollToEnd();
}

void ChartWidget::appendWorkflowEvent(const QVariantMap &event)
{
    if (!assertGuiThread(__func__))
        return;
    const QString type = event.value(QStringLiteral("type")).toString();
    if (!m_workflow || type == QLatin1String("workflow_started")) {
        MessageWidget *message =
            createMessage(QStringLiteral("workflow"), QString(), QStringLiteral("WORKFLOW"));
        message->setBodyVisible(false); // app.js: message.body.remove()
        auto *card = new WorkflowRunCard(message);
        message->stack()->addWidget(card);
        m_workflow = card;
        m_workflowMessage = message;
        m_workflowSteps.clear();
    }
    if (!m_workflow)
        return;
    if (type == QLatin1String("workflow_step")) {
        const int index = event.value(QStringLiteral("index")).toInt();
        if (index >= 0) {
            // app.js steps[event.index] = event：稀疏数组用无效 QVariant 占位
            while (m_workflowSteps.size() <= index)
                m_workflowSteps.append(QVariant());
            m_workflowSteps[index] = event;
        }
    }
    m_workflow->setSteps(m_workflowSteps);
    if (type == QLatin1String("workflow_done"))
        m_workflow->setStatus(event.value(QStringLiteral("status")).toString());
    scrollToEnd();
}

void ChartWidget::setTokenUsage(qint64 total, qint64 input, qint64 output, bool estimated)
{
    QVariantMap usage;
    usage.insert(QStringLiteral("total"), total);
    usage.insert(QStringLiteral("input"), input);
    usage.insert(QStringLiteral("output"), output);
    usage.insert(QStringLiteral("estimated"), estimated);
    setTokenUsageDetail(usage);
}

void ChartWidget::setTokenUsageDetail(const QVariantMap &usage)
{
    if (!assertGuiThread(__func__))
        return;
    if (!m_current)
        return;
    QVariantMap detail = usage;
    // 宿主没给 model_label 时按 usage.model 自动映射供应商名称（用量弹层的「提供方 / 模型」）
    if (!detail.contains(QStringLiteral("model_label"))) {
        const QString label =
            usageModelLabel(m_models, detail.value(QStringLiteral("model")).toString());
        if (!label.isEmpty())
            detail.insert(QStringLiteral("model_label"), label);
    }
    m_current->setUsage(detail);
}

void ChartWidget::setTurnTiming(const QVariantMap &timing)
{
    if (!assertGuiThread(__func__))
        return;
    if (!m_current)
        return;
    m_current->setTiming(timing);
}

void ChartWidget::startLiveTiming(qint64 startMs)
{
    if (!assertGuiThread(__func__))
        return;
    if (!m_current)
        return;
    m_current->startLiveTiming(startMs);
}

void ChartWidget::appendToolResultMessage(const QString &content)
{
    // 工具结果找不到对应调用时的退化形态
    MessageWidget *message =
        createMessage(QStringLiteral("tool"), QString(), QStringLiteral("TOOL RESULT"));
    message->setBodyVisible(false);
    if (ToolItemWidget *item = message->addToolCall(QString(), QString(), QVariantMap()))
        applyToolResult(item, content);
    scrollToEnd();
}

void ChartWidget::appendFailure(const QString &text, bool retryable, const QString &retryMessage,
                                const QString &retryDisplay, const QVariantList &retryAttachments)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *notice = createMessage(QStringLiteral("assistant"), text,
                                          retryable ? QStringLiteral("RETRY")
                                                    : QStringLiteral("ERROR"));
    notice->setCardError(true); // app.js renderFailure：错误卡整圈红边
    if (!retryable)
        return;

    auto *button = new QPushButton(QStringLiteral("重发这条消息"), notice);
    setClass(button, QStringLiteral("actionButton"));
    button->setProperty("retry", true);
    button->setCursor(Qt::PointingHandCursor);
    QVBoxLayout *bubbleLayout =
        qobject_cast<QVBoxLayout *>(notice->findChild<QFrame *>(QStringLiteral("bubble"))->layout());
    if (bubbleLayout) {
        bubbleLayout->addSpacing(10); // button.style.marginTop = "10px"
        bubbleLayout->addWidget(button);
    }
    connect(button, &QPushButton::clicked, this,
            [this, button, retryMessage, retryDisplay, retryAttachments] {
                if (m_composer->isBusy()) {
                    showToast(QStringLiteral("当前还有请求在处理中"));
                    return;
                }
                button->setEnabled(false);
                emit retryRequested(retryMessage, retryDisplay, retryAttachments);
            });
}

void ChartWidget::markCurrentStopped()
{
    if (m_current)
        m_current->markStopped();
}

void ChartWidget::setPhasePlan(const QVariant &value)
{
    if (!assertGuiThread(__func__))
        return;
    QVariantMap phase = extractPhase(value);
    if (phase.isEmpty())
        phase = value.toMap(); // app.js renderPhase: extractPhase(value) || value
    const QVariantList phases = phase.value(QStringLiteral("phases")).toList();
    if (phases.isEmpty())
        return;
    m_phasePlanData = phase;
    m_phaseWrap->setVisible(true);
    m_phasePanel->setPlan(phase);
    syncPhaseLift();
}

void ChartWidget::showToast(const QString &text)
{
    if (!assertGuiThread(__func__))
        return;
    m_toast->showMessage(text);
}

void ChartWidget::syncPhaseLift()
{
    // 选择浮层浮在输入框上方，会盖住紧贴其上的计划窗口：浮层出现/变高时把计划窗口顶开
    if (!m_phaseWrap)
        return;
    auto *layout = qobject_cast<QVBoxLayout *>(m_phaseWrap->layout());
    if (!layout)
        return;
    const int lift = m_composer->choiceOpen()
                         ? qMax(0, m_composer->choiceHeight() + 8 + 12 - m_composer->height())
                         : 0;
    layout->setContentsMargins(9, 0, 9, 8 + lift);
}

void ChartWidget::setTrajectoryEvents(const QVariantList &events)
{
    if (!assertGuiThread(__func__))
        return;
    m_trajView->setEvents(events);
}

void ChartWidget::appendTrajectoryEvents(const QVariantList &events)
{
    if (!assertGuiThread(__func__))
        return;
    m_trajView->appendEvents(events);
}

void ChartWidget::clearMessages()
{
    if (!assertGuiThread(__func__))
        return;
    for (int i = m_messageLayout->count() - 1; i >= 0; --i) {
        QWidget *w = m_messageLayout->itemAt(i)->widget();
        if (!w || w == m_emptyState)
            continue;
        m_messageLayout->removeWidget(w);
        w->deleteLater();
    }
    m_current = nullptr;
    m_currentText.clear();
    m_currentFinished = true;
    m_toolItems.clear();
    m_workflow = nullptr;
    m_workflowMessage = nullptr;
    m_workflowSteps.clear();
    m_lastTurnAwaiting = false;
    m_phasePlanData.clear();
    m_phaseWrap->setVisible(false); // loadSession: el.phasePanel.classList.add("hidden")
    m_composer->closeChoice();
    updateEmptyState();
    rebuildTurnRail();
}

void ChartWidget::setHistory(const QVariantList &messages)
{
    if (!assertGuiThread(__func__))
        return;
    clearMessages();

    // 一条 user 消息之后、下一条 user/workflow 消息之前的 assistant/tool 消息属于同一轮
    QPointer<MessageWidget> turn;
    QString turnText;
    QString turnTs;
    QVariantMap turnUsage;
    QVariantMap turnTiming;
    QVariantMap lastAsk;

    auto finishTurn = [&]() {
        if (turn) {
            m_current = turn.data();
            m_currentText = turnText;
            m_currentFinished = false;
            finishAssistantInternal(true);
            if (turn) {
                if (!turnUsage.isEmpty())
                    turn->setUsage(turnUsage);
                turn->setTimeStamp(turnTs);
                if (!turnTiming.isEmpty())
                    turn->setTiming(turnTiming);
            }
            if (turn && turn->property("awaiting").toBool())
                lastAsk = turn->property("pendingAsk").toMap();
        }
        turn = nullptr;
        turnText.clear();
        turnTs.clear();
        turnUsage.clear();
        turnTiming.clear();
        m_current = nullptr;
        m_currentText.clear();
        m_currentFinished = true;
    };

    for (const QVariant &entry : messages) {
        const QVariantMap message = entry.toMap();
        const QString role = message.value(QStringLiteral("role")).toString();

        if (role == QLatin1String("assistant")) {
            if (!turn) {
                turn = createMessage(QStringLiteral("assistant"), QString());
                m_current = turn.data();
                m_currentText.clear();
                m_currentFinished = false;
            }
            if (message.value(QStringLiteral("interrupted")).toBool())
                turn->markStopped();
            const QString content = message.value(QStringLiteral("content")).toString();
            if (!content.isEmpty()) {
                if (!turnText.isEmpty())
                    turnText += QStringLiteral("\n\n");
                turnText += content;
                turn->body()->setText(turnText);
                turn->setBodyVisible(turn->body()->hasVisibleContent());
                turn->setCopyText(turnText);
            }
            const QString reasoning = message.value(QStringLiteral("reasoning_content")).toString();
            if (!reasoning.isEmpty())
                turn->appendReasoning(reasoning);
            const QVariantList calls = message.value(QStringLiteral("tool_calls")).toList();
            for (const QVariant &callVar : calls) {
                const QVariantMap call = callVar.toMap();
                const QVariantMap function = call.value(QStringLiteral("function")).toMap();
                const QString callId = call.value(QStringLiteral("id")).toString();
                const QString name = function.value(QStringLiteral("name")).toString();
                const QString raw = function.value(QStringLiteral("arguments")).toString();
                const QJsonDocument doc =
                    QJsonDocument::fromJson(raw.isEmpty() ? QByteArray("{}") : raw.toUtf8());
                const QVariant args = doc.isObject() ? QVariant(doc.object().toVariantMap())
                                                     : QVariant(QVariantMap());
                // 询问类调用不落成工具条目：末尾待作答时由输入框上方的浮层弹出
                if (name == QLatin1String(kAskUserTool)) {
                    turn->setProperty("awaiting", true);
                    turn->setProperty("pendingAsk", args);
                    continue;
                }
                ToolItemWidget *item = turn->addToolCall(callId, name, args);
                if (item && !callId.isEmpty())
                    m_toolItems.insert(callId, item);
            }
            if (message.contains(QStringLiteral("usage")))
                turnUsage = message.value(QStringLiteral("usage")).toMap();
            const QString ts = message.value(QStringLiteral("ts")).toString();
            if (!ts.isEmpty())
                turnTs = ts;
            // 本轮可能由多条 assistant 消息合并，取最后一条的耗时字段
            if (message.contains(QStringLiteral("elapsed_ms")))
                turnTiming.insert(QStringLiteral("elapsed"),
                                  message.value(QStringLiteral("elapsed_ms")));
            if (message.contains(QStringLiteral("think_ms")))
                turnTiming.insert(QStringLiteral("think"), message.value(QStringLiteral("think_ms")));
            if (message.contains(QStringLiteral("ttft_ms")))
                turnTiming.insert(QStringLiteral("ttft"), message.value(QStringLiteral("ttft_ms")));
            if (message.contains(QStringLiteral("tps")))
                turnTiming.insert(QStringLiteral("tps"), message.value(QStringLiteral("tps")));
            continue;
        }

        if (role == QLatin1String("tool")) {
            if (message.value(QStringLiteral("tool_name")).toString()
                == QLatin1String(kAskUserTool))
                continue;
            const QString callId = message.value(QStringLiteral("tool_call_id")).toString();
            ToolItemWidget *item = m_toolItems.value(callId, nullptr);
            MessageWidget *parent = item ? ownerMessage(item) : nullptr;
            if (!parent) {
                // 调用没落盘时才退化为独立 TOOL RESULT 气泡
                parent = createMessage(QStringLiteral("tool"), QString(),
                                       QStringLiteral("TOOL RESULT"));
                parent->setBodyVisible(false);
                item = parent->addToolCall(callId,
                                           message.value(QStringLiteral("tool_name")).toString(),
                                           QVariantMap());
                if (item && !callId.isEmpty())
                    m_toolItems.insert(callId, item);
            }
            if (item)
                applyToolResult(item, message.value(QStringLiteral("content")).toString());
            continue; // turn 保持不变
        }

        finishTurn();

        if (role == QLatin1String("user")) {
            QString shown = message.value(QStringLiteral("display_content")).toString();
            if (shown.isEmpty())
                shown = message.value(QStringLiteral("content")).toString();
            MessageWidget *item = createMessage(QStringLiteral("user"), shown, QString(),
                                               message.value(QStringLiteral("attachments")).toList());
            item->setProperty("copyText", shown);
            item->setTimeStamp(message.value(QStringLiteral("ts")).toString());
        } else if (role == QLatin1String("workflow")) {
            QVariantMap started;
            started.insert(QStringLiteral("type"), QStringLiteral("workflow_started"));
            appendWorkflowEvent(started);
            const QVariantList steps = message.value(QStringLiteral("steps")).toList();
            for (int i = 0; i < steps.size(); ++i) {
                QVariantMap step = steps.at(i).toMap();
                step.insert(QStringLiteral("type"), QStringLiteral("workflow_step"));
                step.insert(QStringLiteral("index"), i);
                appendWorkflowEvent(step);
            }
            QVariantMap done;
            done.insert(QStringLiteral("type"), QStringLiteral("workflow_done"));
            done.insert(QStringLiteral("status"), message.value(QStringLiteral("status")).toString());
            done.insert(QStringLiteral("message"),
                        message.value(QStringLiteral("message")).toString());
            appendWorkflowEvent(done);
        }
    }
    finishTurn();

    // 历史最后一轮停在未作答的询问上才弹浮层（供宿主恢复「待确认」徽标）
    m_lastTurnAwaiting = !lastAsk.isEmpty();
    if (m_lastTurnAwaiting)
        m_composer->showChoice(lastAsk);
    scrollToEnd(true);
    rebuildTurnRail();
}

// ------------------------------------------------------------ 设置中心

void ChartWidget::setSettingsDraft(const QVariantMap &config, const QVariant &revision)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->loadConfig(config, revision);
}
void ChartWidget::setDiscoveredModels(const QString &providerId, const QVariantList &models)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setDiscoveredModels(providerId, models);
}
void ChartWidget::setProviderBusy(bool testing, bool reading)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setProviderBusy(testing, reading);
}
void ChartWidget::setSettingsSkills(const QVariantList &skills, bool loading, const QString &error)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setSkills(skills, loading, error);
}
void ChartWidget::setMcpTools(const QVariantList &tools, bool connected, bool loading,
                              const QString &error)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setMcpTools(tools, connected, loading, error);
}
void ChartWidget::setSettingsStatus(const QString &text)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setStatus(text);
}

void ChartWidget::openSettings(const QString &tab)
{
    if (!tab.isEmpty())
        m_settings->switchTab(tab);
    m_settings->show();
    // show() 之后补：QSS 在展示时才生效，字距必须在这之后设才不会被样式覆盖
    applyDeferredStyleDetails(m_settings);
    m_settings->raise();
    m_settings->activateWindow();
}

void ChartWidget::closeSettings()
{
    m_settings->close(); // 必须 close()：hide() 不会触发 closeEvent 的脏检查
}

void ChartWidget::settingsSaved()
{
    m_settings->finishSaved();
}

// ------------------------------------------------------------ 拖拽

void ChartWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    event->acceptProposedAction();
    ++m_dragDepth;
    m_dropOverlay->setActive(true);
}

void ChartWidget::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    event->acceptProposedAction(); // 默认 ignore 会让后续 drop 被拒
}

void ChartWidget::dragLeaveEvent(QDragLeaveEvent *event)
{
    Q_UNUSED(event);
    m_dragDepth = qMax(0, m_dragDepth - 1);
    if (m_dragDepth == 0)
        m_dropOverlay->setActive(false);
}

void ChartWidget::dropEvent(QDropEvent *event)
{
    m_dragDepth = 0;
    m_dropOverlay->setActive(false);
    if (!event->mimeData()->hasUrls())
        return;
    // 类型/大小/数量校验与上传都由宿主完成，这里只回报落下的本地文件
    QVariantList items;
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        const QFileInfo info(path);
        if (!info.isFile())
            continue;
        QVariantMap item;
        item.insert(QStringLiteral("name"), info.fileName());
        item.insert(QStringLiteral("path"), path);
        item.insert(QStringLiteral("size"), info.size());
        item.insert(QStringLiteral("ext"), attachExt(info.fileName()));
        items.append(item);
    }
    if (items.isEmpty())
        return;
    event->acceptProposedAction();
    emit attachmentsAdded(items);
}

// ------------------------------------------------------------ 事件

void ChartWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutOverlays();
}

bool ChartWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_currentTitle && event->type() == QEvent::Resize) {
        updateTitleElide();
        return false;
    }
    if (watched == m_composer && event->type() == QEvent::Resize) {
        layoutOverlays();
        return false;
    }

    auto *target = qobject_cast<QWidget *>(watched);
    switch (event->type()) {
    case QEvent::MouseButtonRelease: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton || !target)
            break;
        if (isSelfOrChildOf(target, m_sessionTrigger)) {
            toggleSessionPanel();
            return true; // 拦住，避免事件沿 parentWidget() 链再触发一次
        }
        if (isSelfOrChildOf(target, m_themeTrigger)) {
            m_themePopup->setCurrent(gs::themeId());
            m_themePopup->openBelow(m_themeTrigger);
            return true;
        }
        // 展开/收起过程行或工具项会把内容顶高：下一帧重判是否仍贴底（webui 同），
        // 免得紧接着到来的流式分片把用户刚展开的位置拽走
        if (hasClassOrAncestor(target, "procRowHead")
            || hasClassOrAncestor(target, "toolItemSummary")) {
            QTimer::singleShot(0, this, [this] { m_followBottom = atBottom(); });
        }
        break;
    }
    case QEvent::Polish:
        // qApp 过滤器会收到全应用的事件：只处理自己的后代，
        // 否则会给宿主控件（恰好同名 class）也套上字距 / 放开焦点
        if (target && isSelfOrChildOf(target, this)) {
            applyLetterSpacing(target);
            applyAccessibility(target);
        }
        break;
    case QEvent::MouseButtonPress:
        if (m_sessionPanel->isOpen() && target
            && !isSelfOrChildOf(target, m_sessionTrigger)
            && !isSelfOrChildOf(target, m_sessionPanel)
            && target->window() == window()) // 别的窗口的点击不算「点面板外」
            closeSessionPanel();             // 点面板外关闭，但不拦截事件本身
        break;
    case QEvent::KeyPress:
        // 只处理发生在自己身上的 Esc：否则会把宿主窗口/宿主对话框的 Esc 一起吞掉
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape && target
            && isSelfOrChildOf(target, this) && !(m_settings && m_settings->isVisible())) {
            if (m_sessionPanel->isOpen()) {
                closeSessionPanel();
                return true;
            }
            // Esc 收起询问浮层，露出被盖住的输入框；审批卡要等后端回执，不放行
            if (m_composer->choiceOpen() && m_composer->approvalCallId().isEmpty()) {
                closeChoice();
                return true;
            }
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

// ------------------------------------------------------------ 界面缩放

// 缩放档位（VSCode 风格：离散步进，Ctrl+0 回到 100%）
static const qreal kZoomSteps[] = {0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.4, 1.6};
static const int kZoomStepCount = int(sizeof(kZoomSteps) / sizeof(kZoomSteps[0]));

void ChartWidget::zoomIn()
{
    for (int i = 0; i < kZoomStepCount; ++i) {
        if (kZoomSteps[i] > gs::zoomFactor() + 0.001) {
            gs::setZoomFactor(kZoomSteps[i]);
            applyZoom();
            return;
        }
    }
}

void ChartWidget::zoomOut()
{
    for (int i = kZoomStepCount - 1; i >= 0; --i) {
        if (kZoomSteps[i] < gs::zoomFactor() - 0.001) {
            gs::setZoomFactor(kZoomSteps[i]);
            applyZoom();
            return;
        }
    }
}

void ChartWidget::zoomReset()
{
    if (qFuzzyCompare(gs::zoomFactor(), 1.0))
        return;
    gs::setZoomFactor(1.0);
    applyZoom();
}

qreal ChartWidget::zoomFactor() const { return gs::zoomFactor(); }

void ChartWidget::applyZoom()
{
    // 1) 根字体 + 全局 QSS 重设：子控件与设置中心对话框（子窗口）继承生效
    QFont f(uiFont());
    f.setPixelSize(scaledPx(basePixelSize()));
    setFont(f);
    setStyleSheet(appStyleSheet());

    // 2) 创建时烘焙的内联 HTML 参数标签：按存好的原文用新字号重建
    const QList<QLabel *> labels = findChildren<QLabel *>();
    for (QLabel *label : labels) {
        const QVariant bold = label->property("paramBold");
        if (!bold.isValid())
            continue;
        const QString descPart = label->property("paramDesc").toString();
        QString html = QStringLiteral("<span style=\"color:%1;font-size:%2px;font-weight:600;\">%3</span>")
                           .arg(cssColor(gs::palette().text))
                           .arg(scaledPx(12))
                           .arg(escapeHtml(bold.toString()));
        if (!descPart.isEmpty())
            html += QStringLiteral("<br><span style=\"color:%1;font-size:%2px;\">%3</span>")
                        .arg(cssColor(gs::palette().muted))
                        .arg(scaledPx(11))
                        .arg(escapeHtml(descPart));
        label->setText(html);
    }

    // 3) 烘焙等宽字体的 QPlainTextEdit（审批卡参数/原始 JSON 区）
    const QList<QPlainTextEdit *> areas = findChildren<QPlainTextEdit *>();
    for (QPlainTextEdit *area : areas) {
        const QVariant base = area->property("monoBasePx");
        if (!base.isValid())
            continue;
        QFont mono = area->font();
        mono.setFamily(monoFont());
        mono.setPixelSize(scaledPx(base.toInt()));
        area->setFont(mono);
        const QVariant baseH = area->property("monoBaseHeight");
        if (baseH.isValid())
            area->setFixedHeight(qRound(baseH.toInt() * gs::zoomFactor()));
    }

    // 4) 下拉触发器：chevron 图标与省略文本按新字号重算
    const QList<ComboTrigger *> combos = findChildren<ComboTrigger *>();
    for (ComboTrigger *combo : combos)
        combo->refreshZoom();

    // 5) 自绘控件（连接状态点、阶段项、导航轨…）paint 时取 scaledPx，重绘即可
    const QList<QWidget *> all = findChildren<QWidget *>();
    for (QWidget *w : all)
        w->update();
}

} // namespace gs

// C 风格工厂：宿主程序（甚至非 Qt 语言绑定）可直接创建一个 UI 面板
extern "C" QTCHARTWIDGET_EXPORT QWidget *qtchartwidget_create()
{
    return new gs::ChartWidget();
}

extern "C" QTCHARTWIDGET_EXPORT const char *qtchartwidget_version()
{
    // 2.0.0：公开头有破坏性改动（见 CHANGELOG.md），改这里时同步改 CHANGELOG 与 README
    return "2.0.0";
}
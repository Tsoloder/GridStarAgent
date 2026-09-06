#include "chartwidget.h"

#include "commonwidgets.h"
#include "composer.h"
#include "markdownview.h"
#include "messagewidgets.h"
#include "phasepanel.h"
#include "popups.h"
#include "settingsdialog.h"
#include "theme.h"

#include <QApplication>
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
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace gs {
namespace {

// app.js skillLabel：技能名列表用「 · 」连接
QString joinSkills(const QVariantList &skills)
{
    QStringList names;
    for (const QVariant &item : skills)
        names << item.toString();
    return names.join(QStringLiteral(" · "));
}

// app.js sendMessage 里 assistant 气泡的标签：skill.name || skill.id
QString skillLabelOf(const QVariantList &skills, const QString &id)
{
    if (id.isEmpty())
        return QString();
    for (const QVariant &item : skills) {
        const QVariantMap skill = item.toMap();
        if (skill.value(QStringLiteral("id")).toString() != id)
            continue;
        const QString name = skill.value(QStringLiteral("name")).toString();
        return name.isEmpty() ? id : name;
    }
    return id;
}

bool isSelfOrChildOf(QWidget *widget, QWidget *ancestor)
{
    for (QWidget *p = widget; p; p = p->parentWidget())
        if (p == ancestor)
            return true;
    return false;
}

// app.js finishAssistant：正文与结构化块都为空、又没有工具组/审批卡时移除空气泡
bool hasToolOrApproval(MessageWidget *message)
{
    return message->findChild<ToolGroupWidget *>() != nullptr
           || message->findChild<ApprovalCard *>() != nullptr;
}

// app.js renderToolResult 的状态判定与汇总刷新
void applyToolResult(ToolItemWidget *item, const QString &result)
{
    const bool failed = result.toLower().contains(QLatin1String("error"))
                        || result.contains(QLatin1String("denied"));
    item->setState(failed ? QStringLiteral("failed") : QStringLiteral("succeeded"));
    item->setResult(result);
    // addToolCall 把 item 挂在组内的 tool-list 上，需沿 parentWidget() 链找 ToolGroupWidget
    for (QWidget *w = item->parentWidget(); w; w = w->parentWidget()) {
        if (auto *group = qobject_cast<ToolGroupWidget *>(w)) {
            group->updateSummary();
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

} // namespace

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
        connect(shortcut, &QShortcut::activated, this, &ChartWidget::zoomIn);
    }
    auto *zoomOutShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+-")), this);
    connect(zoomOutShortcut, &QShortcut::activated, this, &ChartWidget::zoomOut);
    auto *zoomResetShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+0")), this);
    connect(zoomResetShortcut, &QShortcut::activated, this, &ChartWidget::zoomReset);

    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(0, 0, 0, 0);
    m_root->setSpacing(0);

    buildTopbar();
    buildSessionbar();
    buildMessages();

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

    connect(m_composer, &Composer::sendMessage, this,
            [this](const QString &text, const QVariantList &attachments) {
                emit sendMessage(text, text, attachments);
            });
    connect(m_composer, &Composer::stopRequested, this, &ChartWidget::stopRequested);
    connect(m_composer, &Composer::modeChanged, this, &ChartWidget::modeChanged);
    connect(m_composer, &Composer::modelSelected, this, &ChartWidget::modelSelected);
    connect(m_composer, &Composer::skillSelected, this, &ChartWidget::skillSelected);
    connect(m_composer, &Composer::settingsRequested, this, [this] { openSettings(); });
    connect(m_composer, &Composer::attachRequested, this, &ChartWidget::attachRequested);
    connect(m_composer, &Composer::voiceRequested, this, &ChartWidget::voiceRequested);
    connect(m_composer, &Composer::attachmentRemoved, this, &ChartWidget::attachmentRemoved);

    connect(m_settings, &SettingsDialog::saveRequested, this, &ChartWidget::settingsSaveRequested);
    connect(m_settings, &SettingsDialog::testProviderRequested, this,
            &ChartWidget::testProviderRequested);
    connect(m_settings, &SettingsDialog::readModelsRequested, this,
            &ChartWidget::readModelsRequested);
    connect(m_settings, &SettingsDialog::refreshSkillsRequested, this,
            &ChartWidget::refreshSkillsRequested);
    connect(m_settings, &SettingsDialog::refreshMcpRequested, this,
            &ChartWidget::refreshMcpRequested);

    // 会话触发器不是 QPushButton（要放省略号标题 + ⌄），点击由 app 级过滤器统一接管：
    // app filter 先于目标部件执行，且鼠标事件会沿 parentWidget() 链每层重走一次过滤器，
    // 所以在触发器子树上命中即 return true，避免冒泡导致重复开合。
    m_currentTitle->installEventFilter(this);
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
    layout->setSpacing(8); // .brand>*+* { margin-left:8px }

    auto *mark = new QLabel(QStringLiteral("GS"), topbar);
    mark->setObjectName(QStringLiteral("brandMark"));
    mark->setAttribute(Qt::WA_StyledBackground, true);
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(24, 24);

    auto *brand = new QLabel(QStringLiteral("GRIDSTAR AI"), topbar);
    brand->setObjectName(QStringLiteral("brandText"));

    m_connection = new ConnectionButton(topbar);

    layout->addWidget(mark);
    layout->addWidget(brand);
    layout->addStretch(1); // .connection { margin-left:auto }
    layout->addWidget(m_connection);
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
    newSession->setProperty("variant", QStringLiteral("primary"));
    newSession->setFixedHeight(30); // .compact { height:30px }
    newSession->setCursor(Qt::PointingHandCursor);
    newSession->setText(QStringLiteral("新对话"));
    newSession->setIconColors(QColor(QStringLiteral("#f4fbfe")),
                              QColor(QStringLiteral("#f4fbfe")));
    newSession->setIconName(QStringLiteral("plus"), 12);
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
    m_sessionChevron->setPixmap(iconPixmap(QStringLiteral("chevron-down"),
                                           QColor(QStringLiteral("#a8bbc6")), 12));

    triggerLayout->addWidget(m_currentTitle, 1);
    triggerLayout->addWidget(m_sessionChevron);
    layout->addWidget(m_sessionTrigger, 1);
    m_root->addWidget(bar);
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
    symbol->setPixmap(iconPixmap(QStringLiteral("zap"),
                                 QColor(QStringLiteral("#50badf")), 22));
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
    m_toast->layoutIn(host);
    m_dropOverlay->layoutIn(host);
    updateTitleElide();
}

void ChartWidget::scrollToEnd()
{
    QScrollBar *bar = m_messages->verticalScrollBar();
    QTimer::singleShot(0, this, [bar] { bar->setValue(bar->maximum()); });
}

// ------------------------------------------------------------ 顶栏 / 会话栏

void ChartWidget::setConnectionState(const QString &state, const QString &label)
{
    m_connection->setState(state, label);
}

void ChartWidget::setSessions(const QVariantList &sessions)
{
    m_sessions = sessions;
    m_sessionPanel->setSessions(sessions);
}

void ChartWidget::setCurrentSessionTitle(const QString &title)
{
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

void ChartWidget::setModels(const QVariantList &models) { m_composer->setModels(models); }
void ChartWidget::setCurrentModel(const QString &key) { m_composer->setCurrentModel(key); }
QString ChartWidget::currentModel() const { return m_composer->currentModel(); }

void ChartWidget::setSkills(const QVariantList &skills)
{
    m_skills = skills; // ensureAssistant 按 currentSkill 查技能名做气泡标签
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
    scrollToEnd();
    return message;
}

MessageWidget *ChartWidget::ensureAssistant()
{
    if (m_current && !m_currentFinished)
        return m_current;
    // app.js sendMessage：assistant 气泡标签取当前 Skill 名
    const QString label = skillLabelOf(m_skills, m_composer->currentSkill());
    m_current = createMessage(QStringLiteral("assistant"), QString(), label);
    m_currentText.clear();
    m_currentGroup = nullptr;
    m_currentFinished = false;
    return m_current;
}

void ChartWidget::appendUserMessage(const QString &content, const QVariantList &attachments)
{
    createMessage(QStringLiteral("user"), content, QString(), attachments);
}

void ChartWidget::appendAssistantMessage(const QString &content, const QString &label,
                                         const QVariantList &attachments)
{
    MessageWidget *message = createMessage(QStringLiteral("assistant"), content, label, attachments);
    // 一次性追加：正文里的结构化块立即渲染成卡片（等价于 createMessage + finishAssistant）
    const StructuredBlocks parsed = structuredBlocks(content);
    message->body()->setText(parsed.visible);
    message->setBodyVisible(message->body()->hasVisibleContent());
    for (const QVariant &item : parsed.found)
        renderStructured(item.toMap(), message);
    scrollToEnd();
}

void ChartWidget::appendAssistantText(const QString &delta)
{
    MessageWidget *message = ensureAssistant();
    m_currentText += delta;
    message->body()->setText(m_currentText);
    message->setBodyVisible(message->body()->hasVisibleContent());
    scrollToEnd();
}

void ChartWidget::appendReasoning(const QString &delta)
{
    MessageWidget *message = ensureAssistant();
    message->appendReasoning(delta);
    scrollToEnd();
}

void ChartWidget::finishAssistant()
{
    MessageWidget *message = m_current;
    if (!message || m_currentFinished)
        return;
    m_currentFinished = true;

    const StructuredBlocks parsed = structuredBlocks(m_currentText);
    message->body()->setText(parsed.visible);
    message->setBodyVisible(message->body()->hasVisibleContent());
    for (const QVariant &item : parsed.found)
        renderStructured(item.toMap(), message);

    if (parsed.visible.trimmed().isEmpty() && parsed.found.isEmpty()
        && !hasToolOrApproval(message)) {
        if (m_current.data() == message)
            m_current = nullptr;
        message->deleteLater();
    }
    m_currentText.clear();
    m_currentGroup = nullptr;
    updateEmptyState();
    scrollToEnd();
}

void ChartWidget::renderStructured(const QVariantMap &data, MessageWidget *message)
{
    if (!message)
        return;

    if (data.contains(QStringLiteral("phase_plan")))
        setPhasePlan(data.value(QStringLiteral("phase_plan")));

    QVariantMap toolParams = data.value(QStringLiteral("tool_params")).toMap();
    if (toolParams.isEmpty())
        toolParams = data.value(QStringLiteral("toolparams")).toMap();
    const QVariantList options = data.value(QStringLiteral("options")).toList();

    if (!toolParams.isEmpty()) {
        const QString tool = toolParams.value(QStringLiteral("tool")).toString();
        auto *card = new ToolParamsCard(toolParams, options, message);
        connect(card, &ToolParamsCard::decided, this,
                [this, tool](bool confirmed, const QVariantMap &params, const QString &label) {
                    emit toolParamsConfirmed(tool, confirmed, params, label);
                });
        message->stack()->addWidget(card);
    } else if (!options.isEmpty()) {
        auto *card = new OptionsCard(options, message);
        connect(card, &OptionsCard::optionChosen, this, &ChartWidget::optionChosen);
        message->stack()->addWidget(card);
    }

    if (data.contains(QStringLiteral("workflow"))) {
        const QVariantList steps =
            data.value(QStringLiteral("workflow")).toMap().value(QStringLiteral("steps")).toList();
        auto *card = new WorkflowProposalCard(steps, message);
        connect(card, &WorkflowProposalCard::runRequested, this,
                &ChartWidget::workflowRunRequested);
        message->stack()->addWidget(card);
    }
}

ToolGroupWidget *ChartWidget::toolGroup(MessageWidget *message)
{
    if (!message)
        return nullptr;
    if (m_currentGroup && m_currentGroup->parentWidget() == message)
        return m_currentGroup;
    QVBoxLayout *stack = message->stack();
    for (int i = 0; i < stack->count(); ++i)
        if (auto *group = qobject_cast<ToolGroupWidget *>(stack->itemAt(i)->widget()))
            return group;
    auto *group = new ToolGroupWidget(message);
    stack->addWidget(group);
    if (m_current.data() == message)
        m_currentGroup = group;
    return group;
}

void ChartWidget::appendToolCall(const QString &callId, const QString &name, const QVariant &args)
{
    ToolGroupWidget *group = toolGroup(ensureAssistant());
    if (!group)
        return;
    ToolItemWidget *item = group->addToolCall(callId, name, prettyJson(args));
    if (item && !callId.isEmpty())
        m_toolItems.insert(callId, item);
    scrollToEnd();
}

void ChartWidget::appendToolResult(const QString &callId, const QString &name,
                                   const QString &result)
{
    ToolItemWidget *item = m_toolItems.value(callId, nullptr);
    if (!item) {
        // app.js renderToolResult：调用项不存在时先补一条 running 的调用
        ToolGroupWidget *group = toolGroup(ensureAssistant());
        if (!group)
            return;
        item = group->addToolCall(callId, name, QStringLiteral("{}"));
        if (item && !callId.isEmpty())
            m_toolItems.insert(callId, item);
    }
    if (!item)
        return;
    applyToolResult(item, result);
    scrollToEnd();
}

void ChartWidget::appendApproval(const QVariantMap &event)
{
    MessageWidget *message = ensureAssistant();
    const QString callId = event.value(QStringLiteral("call_id")).toString();
    auto *card = new ApprovalCard(event, message);
    if (!callId.isEmpty())
        m_approvals.insert(callId, card);
    // app.js resolve()：POST 成功后才改状态，这里只把决定交给宿主，
    // 宿主成功后调 resolveApproval（失败时 reEnableApproval）
    connect(card, &ApprovalCard::decided, this,
            [this, callId](bool approved, const QVariantMap &args) {
                emit approvalDecided(callId, approved, args);
            });
    connect(card, &ApprovalCard::jsonInvalid, this, [this] {
        showToast(QStringLiteral("工具参数不是有效 JSON"));
        emit approvalJsonInvalid();
    });
    message->stack()->addWidget(card);
    scrollToEnd();
}

void ChartWidget::resolveApproval(const QString &callId, bool approved)
{
    if (ApprovalCard *card = m_approvals.value(callId))
        card->setResolved(approved);
}

void ChartWidget::reEnableApproval(const QString &callId)
{
    if (ApprovalCard *card = m_approvals.value(callId))
        card->reEnable();
}

void ChartWidget::appendWorkflowEvent(const QVariantMap &event)
{
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
    if (m_current)
        m_current->setTokenUsage(total, input, output, estimated);
}

void ChartWidget::appendToolResultMessage(const QString &content)
{
    // 工具结果找不到对应调用时的退化形态（renderHistoryMessage 的 tool 分支）
    MessageWidget *message =
        createMessage(QStringLiteral("tool"), QString(), QStringLiteral("TOOL RESULT"));
    message->setBodyVisible(false);
    auto *group = new ToolGroupWidget(message);
    message->stack()->addWidget(group);
    if (ToolItemWidget *item = group->addToolCall(QString(), QString(), QStringLiteral("{}")))
        applyToolResult(item, content);
    scrollToEnd();
}

void ChartWidget::appendFailure(const QString &text, bool retryable, const QString &retryMessage,
                                const QString &retryDisplay, const QVariantList &retryAttachments)
{
    MessageWidget *notice = createMessage(QStringLiteral("assistant"), text,
                                          retryable ? QStringLiteral("RETRY")
                                                    : QStringLiteral("ERROR"));
    QFrame *bubble = notice->findChild<QFrame *>(QStringLiteral("bubble"));
    if (bubble) {
        bubble->setProperty("error", true); // QFrame#bubble[error="true"] { border-color: red }
        restyle(bubble);
    }
    if (!retryable || !bubble)
        return;

    auto *button = new QPushButton(QStringLiteral("重发这条消息"), bubble);
    setClass(button, QStringLiteral("actionButton"));
    button->setProperty("retry", true);
    button->setCursor(Qt::PointingHandCursor);
    if (auto *bubbleLayout = qobject_cast<QVBoxLayout *>(bubble->layout())) {
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

void ChartWidget::setPhasePlan(const QVariant &value)
{
    QVariantMap phase = extractPhase(value);
    if (phase.isEmpty())
        phase = value.toMap(); // app.js renderPhase: extractPhase(value) || value
    const QVariantList phases = phase.value(QStringLiteral("phases")).toList();
    if (phases.isEmpty())
        return;
    m_phaseWrap->setVisible(true);
    m_phasePanel->setPlan(phase);
}

void ChartWidget::showToast(const QString &text)
{
    m_toast->showMessage(text);
}

void ChartWidget::clearMessages()
{
    for (int i = m_messageLayout->count() - 1; i >= 0; --i) {
        QWidget *w = m_messageLayout->itemAt(i)->widget();
        if (!w || w == m_emptyState)
            continue;
        m_messageLayout->removeWidget(w);
        w->deleteLater();
    }
    m_current = nullptr;
    m_currentText.clear();
    m_currentGroup = nullptr;
    m_currentFinished = true;
    m_toolItems.clear();
    m_approvals.clear();
    m_workflow = nullptr;
    m_workflowMessage = nullptr;
    m_workflowSteps.clear();
    m_phaseWrap->setVisible(false); // loadSession: el.phasePanel.classList.add("hidden")
    updateEmptyState();
}

void ChartWidget::setHistory(const QVariantList &messages)
{
    clearMessages();

    // 一条 user 消息之后、下一条 user/workflow 消息之前的 assistant/tool 消息属于同一轮
    QPointer<MessageWidget> turn;
    QString turnText;
    QVariant turnUsage;

    auto finishTurn = [&]() {
        if (turn) {
            m_current = turn.data();
            m_currentText = turnText;
            m_currentFinished = false;
            finishAssistant();
            if (turn && turnUsage.isValid()) {
                const QVariantMap usage = turnUsage.toMap();
                turn->setTokenUsage(usage.value(QStringLiteral("total")).toLongLong(),
                                    usage.value(QStringLiteral("input")).toLongLong(),
                                    usage.value(QStringLiteral("output")).toLongLong(),
                                    usage.value(QStringLiteral("estimated")).toBool());
            }
        }
        turn = nullptr;
        turnText.clear();
        turnUsage = QVariant();
        m_current = nullptr;
        m_currentText.clear();
        m_currentGroup = nullptr;
        m_currentFinished = true;
    };

    for (const QVariant &entry : messages) {
        const QVariantMap message = entry.toMap();
        const QString role = message.value(QStringLiteral("role")).toString();

        if (role == QLatin1String("assistant")) {
            if (!turn) {
                turn = createMessage(QStringLiteral("assistant"), QString(), QString());
                m_current = turn.data();
                m_currentText.clear();
                m_currentGroup = nullptr;
                m_currentFinished = false;
            }
            // 带工具调用的消息不存 active_skills，Skill 标签要等本轮后续消息补上
            turn->setLabel(joinSkills(message.value(QStringLiteral("active_skills")).toList()));
            const QString content = message.value(QStringLiteral("content")).toString();
            if (!content.isEmpty()) {
                if (!turnText.isEmpty())
                    turnText += QStringLiteral("\n\n");
                turnText += content;
                turn->body()->setText(turnText);
                turn->setBodyVisible(turn->body()->hasVisibleContent());
            }
            const QString reasoning = message.value(QStringLiteral("reasoning_content")).toString();
            if (!reasoning.isEmpty())
                turn->appendReasoning(reasoning);
            const QVariantList calls = message.value(QStringLiteral("tool_calls")).toList();
            for (const QVariant &callVar : calls) {
                const QVariantMap call = callVar.toMap();
                const QVariantMap function = call.value(QStringLiteral("function")).toMap();
                const QString callId = call.value(QStringLiteral("id")).toString();
                const QString raw = function.value(QStringLiteral("arguments")).toString();
                const QJsonDocument doc =
                    QJsonDocument::fromJson(raw.isEmpty() ? QByteArray("{}") : raw.toUtf8());
                const QVariant args =
                    doc.isObject() ? QVariant(doc.object().toVariantMap()) : QVariant(QVariantMap());
                ToolGroupWidget *group = toolGroup(turn.data());
                if (!group)
                    continue;
                ToolItemWidget *item =
                    group->addToolCall(callId, function.value(QStringLiteral("name")).toString(),
                                       prettyJson(args));
                if (item && !callId.isEmpty())
                    m_toolItems.insert(callId, item);
            }
            if (message.contains(QStringLiteral("usage")))
                turnUsage = message.value(QStringLiteral("usage"));
            continue;
        }

        if (role == QLatin1String("tool")) {
            const QString callId = message.value(QStringLiteral("tool_call_id")).toString();
            ToolItemWidget *item = m_toolItems.value(callId, nullptr);
            MessageWidget *parent = item ? ownerMessage(item) : nullptr;
            if (!parent) {
                // 调用没落盘时才退化为独立 TOOL RESULT 气泡
                parent = createMessage(QStringLiteral("tool"), QString(),
                                       QStringLiteral("TOOL RESULT"));
                parent->setBodyVisible(false);
                auto *group = new ToolGroupWidget(parent);
                parent->stack()->addWidget(group);
                item = group->addToolCall(callId,
                                          message.value(QStringLiteral("tool_name")).toString(),
                                          QStringLiteral("{}"));
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
            createMessage(QStringLiteral("user"), shown, QString(),
                          message.value(QStringLiteral("attachments")).toList());
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
    scrollToEnd();
}

// ------------------------------------------------------------ 设置中心

void ChartWidget::setSettingsDraft(const QVariantMap &config, const QVariant &revision)
{
    m_settings->loadConfig(config, revision);
}
void ChartWidget::setDiscoveredModels(const QString &providerId, const QVariantList &models)
{
    m_settings->setDiscoveredModels(providerId, models);
}
void ChartWidget::setProviderBusy(bool testing, bool reading)
{
    m_settings->setProviderBusy(testing, reading);
}
void ChartWidget::setSettingsSkills(const QVariantList &skills, bool loading, const QString &error)
{
    m_settings->setSkills(skills, loading, error);
}
void ChartWidget::setMcpTools(const QVariantList &tools, bool connected, bool loading,
                              const QString &error)
{
    m_settings->setMcpTools(tools, connected, loading, error);
}
void ChartWidget::setSettingsStatus(const QString &text) { m_settings->setStatus(text); }

void ChartWidget::openSettings(const QString &tab)
{
    if (!tab.isEmpty())
        m_settings->switchTab(tab);
    m_settings->show();
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
        break;
    }
    case QEvent::MouseButtonPress:
        if (m_sessionPanel->isOpen() && target
            && !isSelfOrChildOf(target, m_sessionTrigger)
            && !isSelfOrChildOf(target, m_sessionPanel))
            closeSessionPanel(); // 点面板外关闭，但不拦截事件本身
        break;
    case QEvent::KeyPress:
        if (m_sessionPanel->isOpen()
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            closeSessionPanel();
            return true;
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
        QString html = QStringLiteral("<span style=\"color:#d2e0e6;font-size:%1px;font-weight:600;\">%2</span>")
                           .arg(scaledPx(12))
                           .arg(escapeHtml(bold.toString()));
        if (!descPart.isEmpty())
            html += QStringLiteral("<br><span style=\"color:#9fb2bd;font-size:%1px;\">%2</span>")
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

    // 5) 自绘控件（ConnectionButton / 阶段项）paint 时取 scaledPx，重绘即可
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
    return "1.0.0";
}

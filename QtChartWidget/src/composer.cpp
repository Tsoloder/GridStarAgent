#include "composer.h"

#include "choiceoverlay.h"
#include "commonwidgets.h"
#include "messagewidgets.h"
#include "popups.h"
#include "theme.h"

#include <QEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

namespace gs {
namespace {

// 输入框高度：默认三行（75px），随内容增高，到上限后锁定并改为可滚动
const int kInputMinHeight = 75;
const int kInputMaxHeight = 200;
const int kInputChrome = 16; // padding 10px + 6px

} // namespace

Composer::Composer(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("composer"));
    setAttribute(Qt::WA_StyledBackground, true);

    // .composer { padding:8px 9px 12px }
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(9, 8, 9, 12);
    outer->setSpacing(0);

    // 配置警告（.config-warning）
    m_warning = new QLabel(QStringLiteral("后端尚未配置模型，聊天暂不可用。"), this);
    m_warning->setObjectName(QStringLiteral("configWarning"));
    m_warning->setAttribute(Qt::WA_StyledBackground, true);
    m_warning->setWordWrap(true);
    m_warning->setVisible(false);
    m_warningGap = new QWidget(this);
    m_warningGap->setFixedHeight(6);
    m_warningGap->setVisible(false);
    outer->addWidget(m_warning);
    outer->addWidget(m_warningGap);

    // 输入容器（.input-wrap）：附件条 + 输入框 + 控件行，整体是一张浮起的卡片
    m_inputWrap = new QFrame(this);
    m_inputWrap->setObjectName(QStringLiteral("inputWrap"));
    m_inputWrap->setAttribute(Qt::WA_StyledBackground, true);
    auto *wl = new QVBoxLayout(m_inputWrap);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->setSpacing(0);

    m_attachBar = new QWidget(m_inputWrap);
    m_attachBar->setObjectName(QStringLiteral("attachBar"));
    m_attachLayout = new FlowLayout(m_attachBar, 0, 5, 5);
    m_attachLayout->setContentsMargins(10, 10, 10, 0); // CSS .attach-bar padding 10px 10px 0
    m_attachBar->setVisible(false);
    wl->addWidget(m_attachBar);

    m_input = new QTextEdit(m_inputWrap);
    m_input->setObjectName(QStringLiteral("messageInput"));
    m_input->setPlaceholderText(QString::fromUtf8("输入任务...按Enter发送，Shift+Enter换行"));
    m_input->setFrameShape(QFrame::NoFrame);
    m_input->setFixedHeight(kInputMinHeight);
    m_input->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->setTabChangesFocus(true);
    // QTextEdit 的视口默认用调色板 Base 填充，会盖掉 #inputWrap 的主题底色
    m_input->viewport()->setAutoFillBackground(false);
    m_input->installEventFilter(this);
    wl->addWidget(m_input);

    // 控件行（.controls）：左侧流式布局（窄屏自动换行），右侧固定操作组
    m_controls = new QWidget(m_inputWrap);
    m_controls->setObjectName(QStringLiteral("controls"));
    auto *hl = new QHBoxLayout(m_controls);
    hl->setContentsMargins(8, 0, 8, 8);
    hl->setSpacing(5);

    m_leftControls = new QWidget(m_controls);
    auto *leftFlow = new FlowLayout(m_leftControls, 0, 5, 5);
    leftFlow->setContentsMargins(0, 0, 0, 0);

    m_attach = new IconPushButton(m_leftControls);
    m_attach->setObjectName(QStringLiteral("attachButton"));
    m_attach->setFixedSize(30, 30);
    m_attach->setToolTip(QStringLiteral("添加附件"));
    m_attach->setCursor(Qt::PointingHandCursor);
    m_attach->setFocusPolicy(Qt::TabFocus); // webui 的 button 可聚焦；TabFocus 不留点击焦点环
    m_attach->setIconColors(gs::palette().muted2, gs::palette().cyan);
    m_attach->setIconName(QStringLiteral("paperclip"), 16);
    leftFlow->addWidget(m_attach);

    auto *modeSwitch = new QWidget(m_leftControls);
    modeSwitch->setObjectName(QStringLiteral("modeSwitch"));
    auto *modeLayout = new QHBoxLayout(modeSwitch);
    modeLayout->setContentsMargins(0, 0, 0, 0);
    modeLayout->setSpacing(0);
    m_manual = new QPushButton(QStringLiteral("手动"), modeSwitch);
    m_auto = new QPushButton(QStringLiteral("自动"), modeSwitch);
    for (QPushButton *button : { m_manual, m_auto }) {
        setClass(button, QStringLiteral("modeButton"));
        button->setFixedHeight(28);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::TabFocus);
        modeLayout->addWidget(button);
    }
    m_manual->setProperty("active", true);
    leftFlow->addWidget(modeSwitch);

    struct ControlDef { ComboTrigger **trigger; const char *tip; };
    const ControlDef defs[2] = { { &m_modelTrigger, "模型" }, { &m_skillTrigger, "Skill" } };
    for (const ControlDef &def : defs) {
        auto *control = new QFrame(m_leftControls);
        setClass(control, QStringLiteral("modelControl"));
        control->setAttribute(Qt::WA_StyledBackground, true);
        control->setFixedHeight(30);
        control->setMaximumWidth(240);
        control->setToolTip(QString::fromUtf8(def.tip));
        auto *layout = new QHBoxLayout(control);
        layout->setContentsMargins(8, 0, 2, 0);
        layout->setSpacing(0);
        ComboTrigger *trigger = new ComboTrigger(control);
        trigger->setFocusPolicy(Qt::TabFocus);
        layout->addWidget(trigger, 1);
        *def.trigger = trigger;
        leftFlow->addWidget(control);
    }

    m_busyLabel = makeLabel(QStringLiteral("busyLabel"), QString(), m_controls);
    m_busyLabel->setTextInteractionFlags(Qt::NoTextInteraction);

    m_settings = new IconPushButton(m_controls);
    m_settings->setObjectName(QStringLiteral("settingsButton"));
    m_settings->setFixedSize(30, 30);
    m_settings->setToolTip(QStringLiteral("打开设置"));
    m_settings->setCursor(Qt::PointingHandCursor);
    m_settings->setFocusPolicy(Qt::TabFocus);
    m_settings->setIconColors(gs::palette().muted2, gs::palette().cyan);
    m_settings->setIconName(QStringLiteral("settings"), 16);

    m_voice = new IconPushButton(m_controls);
    m_voice->setObjectName(QStringLiteral("voiceButton"));
    m_voice->setFixedSize(30, 30);
    m_voice->setToolTip(QStringLiteral("语音输入"));
    m_voice->setCursor(Qt::PointingHandCursor);
    m_voice->setFocusPolicy(Qt::TabFocus);
    m_voice->setIconColors(gs::palette().muted2, gs::palette().cyan);
    m_voice->setIconName(QStringLiteral("mic"), 16);
    m_voice->setEnabled(false);

    m_send = new IconPushButton(m_controls);
    m_send->setObjectName(QStringLiteral("sendButton"));
    m_send->setFixedSize(30, 30);
    m_send->setToolTip(QStringLiteral("发送"));
    m_send->setCursor(Qt::PointingHandCursor);
    m_send->setFocusPolicy(Qt::TabFocus);
    m_send->setIconColors(QColor(QStringLiteral("#ffffff")),
                          QColor(QStringLiteral("#ffffff")),
                          QColor(QStringLiteral("#cfe3ec")));
    m_send->setIconName(QStringLiteral("arrow-up"), 16);
    m_send->setEnabled(false);

    hl->addWidget(m_leftControls, 1);
    hl->addWidget(m_busyLabel, 0);
    hl->addWidget(m_settings, 0);
    hl->addWidget(m_voice, 0);
    hl->addWidget(m_send, 0);
    wl->addWidget(m_controls);
    outer->addWidget(m_inputWrap);

    // 选择浮层（.choice-overlay）：绝对定位在输入框上方，不入布局
    m_choice = new ChoiceOverlay(this);
    m_choice->setVisible(false);

    m_modelList = new ListBoxPopup(this);
    m_skillList = new ListBoxPopup(this);

    connect(m_manual, &QPushButton::clicked, this, [this] { setMode(QStringLiteral("manual")); });
    connect(m_auto, &QPushButton::clicked, this, [this] { setMode(QStringLiteral("auto")); });
    connect(m_settings, &QPushButton::clicked, this, &Composer::settingsRequested);
    connect(m_attach, &QPushButton::clicked, this, &Composer::attachRequested);
    connect(m_voice, &QPushButton::clicked, this, &Composer::voiceRequested);
    connect(m_send, &QPushButton::clicked, this, [this] {
        if (m_busy) {
            emit stopRequested();
            return;
        }
        const QString content = m_input->toPlainText().trimmed();
        if (content.isEmpty() && m_attachments.isEmpty())
            return;
        const QVariantList attachments = m_attachments;
        m_input->clear();
        autoGrowInput();
        clearAttachments();
        emit sendMessage(content, content, attachments);
    });
    connect(m_modelTrigger, &ComboTrigger::clicked, this, &Composer::openModelList);
    connect(m_skillTrigger, &ComboTrigger::clicked, this, &Composer::openSkillList);
    connect(m_modelList, &ListBoxPopup::chosen, this, [this](const QString &key) {
        setCurrentModel(key);
        emit modelSelected(key);
    });
    connect(m_skillList, &ListBoxPopup::chosen, this, [this](const QString &id) {
        setCurrentSkill(id);
        emit skillSelected(id);
    });
    m_modelList->installEventFilter(this);
    m_skillList->installEventFilter(this);
    connect(m_input, &QTextEdit::textChanged, this, [this] {
        autoGrowInput();
        updateSendState();
    });

    connect(m_choice, &ChoiceOverlay::optionChosen, this, &Composer::optionChosen);
    connect(m_choice, &ChoiceOverlay::approvalDecided, this, &Composer::approvalDecided);
    connect(m_choice, &ChoiceOverlay::sendRequested, this,
            [this](const QString &payload, const QString &display) {
                emit sendMessage(payload, display, QVariantList());
            });
    connect(m_choice, &ChoiceOverlay::openStateChanged, this, [this](bool open) {
        m_choiceOpen = open;
        // 浮层展示期间彻底藏掉输入框：卡片圆角更大，不藏会露出来
        m_inputWrap->setVisible(!open);
        layoutChoiceOverlay();
        emit choiceOpenChanged(open);
    });
    // webui 用 ResizeObserver 观察浮层高度；这里由卡片自己在尺寸变化时上报
    connect(m_choice, &ChoiceOverlay::sizeChanged, this, [this] {
        layoutChoiceOverlay();
        emit choiceResized();
    });
}

void Composer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 宽度变化会改变输入框的换行数，高度必须跟着重算（webui 里是 window resize 监听）。
    // 要等 QTextEdit 自己收到 resize、文档宽度更新之后再算，否则拿到的还是旧行数
    QTimer::singleShot(0, this, [this] { autoGrowInput(); });
    layoutChoiceOverlay();
}

void Composer::layoutChoiceOverlay()
{
    if (!m_choice)
        return;
    if (!m_choiceOpen || !m_choice->isVisible()) {
        return;
    }
    const int available = qMax(80, height() - 24);
    const int wanted = m_choice->sizeHint().height();
    const int cardHeight = qMin(wanted, available);
    m_choice->setGeometry(9, height() - 12 - cardHeight, qMax(0, width() - 18), cardHeight);
    m_choice->raise();
}

void Composer::autoGrowInput()
{
    if (!m_input)
        return;
    const qreal docHeight = m_input->document()->size().height();
    const int full = qCeil(docHeight) + kInputChrome;
    const int target = qBound(kInputMinHeight, full, kInputMaxHeight);
    if (m_input->height() != target)
        m_input->setFixedHeight(target);
    m_input->setVerticalScrollBarPolicy(full > kInputMaxHeight ? Qt::ScrollBarAsNeeded
                                                               : Qt::ScrollBarAlwaysOff);
}

bool Composer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_input) {
        switch (event->type()) {
        case QEvent::FocusIn:
            m_inputWrap->setProperty("focus", true);
            restyle(m_inputWrap);
            break;
        case QEvent::FocusOut:
            m_inputWrap->setProperty("focus", false);
            restyle(m_inputWrap);
            break;
        case QEvent::KeyPress: {
            auto *key = static_cast<QKeyEvent *>(event);
            const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
            if (enter && !(key->modifiers() & Qt::ShiftModifier)) {
                m_send->click();
                return true;
            }
            break;
        }
        default:
            break;
        }
    } else if (watched == m_modelList && event->type() == QEvent::Hide) {
        m_modelTrigger->setOpen(false);
    } else if (watched == m_skillList && event->type() == QEvent::Hide) {
        m_skillTrigger->setOpen(false);
    }
    return QWidget::eventFilter(watched, event);
}

void Composer::setMode(const QString &mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    m_manual->setProperty("active", mode == QLatin1String("manual"));
    m_auto->setProperty("active", mode == QLatin1String("auto"));
    restyle(m_manual);
    restyle(m_auto);
    emit modeChanged(m_mode);
}

void Composer::setBusy(bool busy)
{
    m_busy = busy;
    m_send->setIconName(busy ? QStringLiteral("stop") : QStringLiteral("arrow-up"));
    m_send->setProperty("stop", busy);
    restyle(m_send);
    m_send->setToolTip(busy ? QStringLiteral("停止接收") : QStringLiteral("发送"));
    m_input->setEnabled(!busy);
    m_attach->setEnabled(!busy);
    m_busyLabel->setText(busy ? QStringLiteral("Agent 正在处理…") : QString());
    updateSendState();
}

void Composer::setConfigLoaded(bool loaded)
{
    m_configLoaded = loaded;
    if (loaded)
        setConfigWarning(QString());
    updateSendState();
}

void Composer::setConfigWarning(const QString &text)
{
    m_warning->setText(text);
    m_warning->setVisible(!text.isEmpty());
    m_warningGap->setVisible(!text.isEmpty());
}

void Composer::setModels(const QVariantList &models)
{
    m_models = models;
}

void Composer::setCurrentModel(const QString &key)
{
    m_model = key;
    QString label = QStringLiteral("未配置");
    const QVariantList visible = visibleModels(m_models);
    for (const QVariant &v : visible) {
        const QVariantMap item = v.toMap();
        if (modelKey(item) == key) {
            label = modelName(item);
            break;
        }
    }
    m_modelTrigger->setText(label);
    updateSendState();
}

void Composer::setSkills(const QVariantList &skills)
{
    m_skills = skills;
}

void Composer::setCurrentSkill(const QString &id)
{
    m_skill = id;
    QString label = QStringLiteral("无 Skill");
    for (const QVariant &v : m_skills) {
        const QVariantMap item = v.toMap();
        if (item.value(QStringLiteral("id")).toString() == id) {
            const QString name = item.value(QStringLiteral("name")).toString();
            label = name.isEmpty() ? id : name;
            break;
        }
    }
    m_skillTrigger->setText(label);
}

void Composer::openModelList()
{
    m_modelList->setModelOptions(m_models, m_model);
    m_modelTrigger->setOpen(true);
    m_modelList->openAbove(m_modelTrigger);
}

void Composer::openSkillList()
{
    m_skillList->setSkillOptions(m_skills, m_skill);
    m_skillTrigger->setOpen(true);
    m_skillList->openAbove(m_skillTrigger);
}

void Composer::addAttachments(const QVariantList &items)
{
    for (const QVariant &v : items) {
        QVariantMap item = v.toMap();
        if (!item.contains(QStringLiteral("id")))
            item.insert(QStringLiteral("id"), m_attachments.size() + 1);
        if (item.value(QStringLiteral("ext")).toString().isEmpty())
            item.insert(QStringLiteral("ext"),
                        attachExt(item.value(QStringLiteral("name")).toString()));
        m_attachments.append(item);
    }
    renderAttachments();
    updateSendState();
}

void Composer::clearAttachments()
{
    m_attachments.clear();
    renderAttachments();
    updateSendState();
}

void Composer::renderAttachments()
{
    while (QLayoutItem *item = m_attachLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    m_uploading = 0;
    for (const QVariant &v : m_attachments) {
        const QVariantMap item = v.toMap();
        const bool uploading = item.value(QStringLiteral("uploading")).toBool();
        if (uploading)
            ++m_uploading;
        auto *chip = new AttachChip(item, m_attachBar);
        chip->setProperty("attachId", item.value(QStringLiteral("id")));
        chip->setToolTip(item.value(QStringLiteral("name")).toString());
        connect(chip, &AttachChip::removeClicked, this, [this, id = item.value(QStringLiteral("id"))] {
            for (int i = 0; i < m_attachments.size(); ++i) {
                if (m_attachments.at(i).toMap().value(QStringLiteral("id")) == id) {
                    m_attachments.removeAt(i);
                    break;
                }
            }
            renderAttachments();
            updateSendState();
            emit attachmentRemoved(id.toString());
        });
        m_attachLayout->addWidget(chip);
    }
    m_attachBar->setVisible(!m_attachments.isEmpty());
}

void Composer::setVoiceEnabled(bool enabled)
{
    m_voice->setEnabled(enabled);
}

void Composer::setVoiceRecording(bool recording)
{
    m_voice->setProperty("recording", recording);
    restyle(m_voice);
    m_voice->update();
}

QString Composer::text() const
{
    return m_input->toPlainText();
}

void Composer::setText(const QString &text)
{
    m_input->setPlainText(text);
    autoGrowInput();
    updateSendState();
}

void Composer::focusInput()
{
    m_input->setFocus();
}

void Composer::updateSendState()
{
    const bool hasPayload = !m_input->toPlainText().trimmed().isEmpty() || !m_attachments.isEmpty();
    m_send->setEnabled(m_busy || (hasPayload && m_configLoaded && m_uploading == 0));
}

// ------------------------------------------------------------ 选择浮层

void Composer::showChoice(const QVariantMap &payload)
{
    m_choice->showChoice(payload);
    layoutChoiceOverlay();
}

void Composer::closeChoice()
{
    m_choice->closeOverlay();
}

bool Composer::choiceOpen() const
{
    return m_choiceOpen && m_choice->isVisible();
}

int Composer::choiceHeight() const
{
    return choiceOpen() ? m_choice->height() : 0;
}

QString Composer::approvalCallId() const
{
    return m_choice->approvalCallId();
}

void Composer::setApprovalResolved(const QString &callId, bool approved)
{
    m_choice->setApprovalResolved(callId, approved);
}

void Composer::reEnableApproval(const QString &callId)
{
    m_choice->reEnableApproval(callId);
}

} // namespace gs
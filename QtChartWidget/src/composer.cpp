#include "composer.h"

#include "choiceoverlay.h"
#include "commonwidgets.h"
#include "messagewidgets.h"
#include "popups.h"
#include "theme.h"

#include <QEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
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
    // webui 在浮层打开时用 visibility:hidden 隐藏输入区，仍保留其布局占位。
    QSizePolicy inputWrapPolicy = m_inputWrap->sizePolicy();
    inputWrapPolicy.setRetainSizeWhenHidden(true);
    m_inputWrap->setSizePolicy(inputWrapPolicy);
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
    m_input->setPlaceholderText(QString::fromUtf8("输入任务...按Enter发送，Shift+Enter换行，输入 / 选择技能"));
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

    // 交互模式下拉（.model-control.mode-control）：原来是一对分段按钮，webui 已改成下拉
    auto buildControl = [&](const QString &tip, ComboTrigger **trigger, QWidget **control) {
        auto *frame = new QFrame(m_leftControls);
        setClass(frame, QStringLiteral("modelControl"));
        frame->setAttribute(Qt::WA_StyledBackground, true);
        frame->setFixedHeight(30);
        frame->setMaximumWidth(240);
        frame->setToolTip(tip);
        auto *layout = new QHBoxLayout(frame);
        // webui 撤掉箭头后 padding 改成左右对称的 0 6px
        layout->setContentsMargins(6, 0, 6, 0);
        layout->setSpacing(0);
        ComboTrigger *combo = new ComboTrigger(frame);
        combo->setFocusPolicy(Qt::TabFocus);
        layout->addWidget(combo, 1);
        *trigger = combo;
        *control = frame;
        leftFlow->addWidget(frame);
    };
    buildControl(QString::fromUtf8("交互模式"), &m_modeTrigger, &m_modeControl);
    buildControl(QString::fromUtf8("模型"), &m_modelTrigger, &m_modelControl);
    m_modeTrigger->setText(QString::fromUtf8("手动"));

    // webui：Skill 触发器与下拉整块删除，改成输入框左侧的小标签（#skill-chip）
    m_skillChip = new QPushButton(m_leftControls);
    m_skillChip->setObjectName(QStringLiteral("skillChip"));
    setClass(m_skillChip, QStringLiteral("skillChip"));
    m_skillChip->setAttribute(Qt::WA_StyledBackground, true);
    m_skillChip->setFixedHeight(28);
    m_skillChip->setCursor(Qt::PointingHandCursor);
    m_skillChip->setFocusPolicy(Qt::TabFocus);
    m_skillChip->setToolTip(QString::fromUtf8("清除技能"));
    {
        auto *chipLayout = new QHBoxLayout(m_skillChip);
        chipLayout->setContentsMargins(7, 0, 7, 0);
        chipLayout->setSpacing(5);
        m_skillChipName = makeLabel(QStringLiteral("skillChipName"), QString(), m_skillChip);
        m_skillChipName->setTextInteractionFlags(Qt::NoTextInteraction);
        m_skillChipClose = makeLabel(QStringLiteral("skillChipClose"), QString::fromUtf8("×"),
                                     m_skillChip);
        m_skillChipClose->setTextInteractionFlags(Qt::NoTextInteraction);
        chipLayout->addWidget(m_skillChipName);
        chipLayout->addWidget(m_skillChipClose);
        // QPushButton 的最小宽来自自己的 text（这里是空的），不吃子控件的最小宽，
        // 不补的话布局会把芯片压到只剩左右内边距的 22px（webui 侧是 flex:0 0 auto）。
        // 试过 chipLayout->setSizeConstraint(SetMinimumSize)，它连芯片高一起压成 15，
        // 所以改成按名字实测宽度写最小宽，字号缩放变化由 refreshZoom 兜
        updateSkillChipWidth();
    }
    m_skillChip->setVisible(false);
    leftFlow->addWidget(m_skillChip);

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

    // 选择浮层（.choice-overlay）：绝对定位在输入框上方，不入布局。
    // Qt 会裁剪子控件，因此挂到 Composer 的顶层宿主上；布局时再映射回输入区坐标。
    QWidget *overlayParent = parentWidget() ? parentWidget() : this;
    m_choice = new ChoiceOverlay(overlayParent);
    m_choice->setVisible(false);
    connect(m_choice, &QObject::destroyed, this, [this] { m_choice = nullptr; });

    m_modelList = new ListBoxPopup(this);
    // objectName 保持 "listbox"（QSS 的 #listbox 认它），区分靠 role 动态属性
    m_modelList->setProperty("role", QStringLiteral("model"));
    m_modeList = new ListBoxPopup(this);
    m_modeList->setProperty("role", QStringLiteral("mode"));
    // 斜杠面板与选择浮层同一套挂载方式：浮在输入框上方，不入布局
    m_slash = new SlashPanel(overlayParent);
    m_slash->setVisible(false);
    connect(m_slash, &QObject::destroyed, this, [this] { m_slash = nullptr; });

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
    m_modelTrigger->setObjectName(QStringLiteral("modelTrigger"));
    m_modeTrigger->setObjectName(QStringLiteral("modeTrigger"));
    connect(m_modeTrigger, &ComboTrigger::clicked, this, [this] {
        if (modeListOpen())
            closeModeList();
        else
            openModeList();
    });
    connect(m_modelList, &ListBoxPopup::chosen, this, [this](const QString &key) {
        setCurrentModel(key);
        emit modelSelected(key);
    });
    connect(m_modeList, &ListBoxPopup::chosen, this, [this](const QString &value) {
        selectMode(value);
    });
    connect(m_skillChip, &QPushButton::clicked, this, [this] {
        // 点标签即清除技能，焦点还给输入框（webui：selectSkill("") 后 input.focus()）
        selectSkill(QString());
        m_input->setFocus(Qt::OtherFocusReason);
    });
    connect(m_slash, &SlashPanel::itemChosen, this, &Composer::runSlashItem);
    m_modelList->installEventFilter(this);
    m_modeList->installEventFilter(this);
    connect(m_input, &QTextEdit::textChanged, this, [this] {
        autoGrowInput();
        // 正文变化时同步斜杠面板：不以 "/" 开头就收起，否则刷新候选并保持展开
        syncSlashMenu();
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

Composer::~Composer()
{
    // 浮层通常挂在宿主窗口下，Composer 被单独销毁时不能把它遗留在界面上。
    delete m_choice;
    delete m_slash;
}

void Composer::hideEvent(QHideEvent *event)
{
    if (m_choice)
        m_choice->hide();
    closeSlashMenu();
    QWidget::hideEvent(event);
}

void Composer::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    layoutChoiceOverlay();
    layoutSlashPanel();
}

void Composer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 宽度变化会改变输入框的换行数，高度必须跟着重算（webui 里是 window resize 监听）。
    // 要等 QTextEdit 自己收到 resize、文档宽度更新之后再算，否则拿到的还是旧行数
    QTimer::singleShot(0, this, [this] { autoGrowInput(); });
    applyAttachmentCompact();
    layoutChoiceOverlay();
    layoutSlashPanel();
}

void Composer::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_choice && m_choiceOpen)
        m_choice->show();
    layoutChoiceOverlay();
    layoutSlashPanel();
    if (m_choiceOpen)
        emit choiceResized();
}

void Composer::layoutChoiceOverlay()
{
    if (!m_choice || m_choiceLayingOut)
        return;
    if (!m_choiceOpen || !m_choice->isVisible()) {
        return;
    }
    QWidget *host = m_choice->parentWidget();
    if (!host)
        return;
    // 下面按宽度量高时会再触发 resizeEvent -> sizeChanged -> 本函数，重的这层直接跳过
    m_choiceLayingOut = true;

    const int cardWidth = qMax(0, width() - 18);
    // webui .choice-overlay{bottom:12px}：卡片底边贴在输入区上方 12px、向上生长
    const int cardBottom = mapTo(host, QPoint(0, height() - 12)).y();
    int cardHeight = 0;
    {
        // 隐藏态/旧宽度量出来的 sizeHint 偏小，窄窗口上底部的作答区会被卡片底边裁掉，
        // 所以先按定稿宽度把内容重排一次再取高度（量高期间的 sizeChanged 不要外泄）
        QSignalBlocker blocker(m_choice);
        cardHeight = m_choice->heightForCardWidth(cardWidth);
    }
    // 内容比可视区还高时贴住宿主顶边，正文自己滚动；不能顶出宿主被裁掉
    cardHeight = qBound(0, cardHeight, qMax(0, cardBottom - host->rect().top()));
    const QPoint topLeft = mapTo(host, QPoint(9, height() - 12 - cardHeight));
    m_choice->setGeometry(QRect(topLeft, QSize(cardWidth, cardHeight)));
    m_choice->raise();

    // 第一遍量高时正文还没按最终宽度定稿（sizeHint 偏小），卡片会被压矮、候选列表挤出滚动条。
    // 落位后再按定稿宽度量一次：需要更高就保持底边不动、向上生长（只增不减，避免来回震荡）。
    int settledHeight = 0;
    {
        QSignalBlocker blocker(m_choice);
        settledHeight = m_choice->heightForCardWidth(cardWidth);
    }
    settledHeight = qBound(0, settledHeight, qMax(0, cardBottom - host->rect().top()));
    if (settledHeight > cardHeight) {
        m_choice->setGeometry(QRect(mapTo(host, QPoint(9, height() - 12 - settledHeight)),
                                    QSize(cardWidth, settledHeight)));
    }

    m_choiceLayingOut = false;
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
            if (m_slashOpen && m_slash && m_slash->mode() == QLatin1String("root")) {
                // 斜杠面板开着时正文里的按键先给面板：上下走候选、回车选中、Esc 收起。
                // 模型模式下正文还是 "/"，面板的过滤词在它自己的搜索框里，这里不拦。
                switch (key->key()) {
                case Qt::Key_Escape:
                    closeSlashMenu();
                    return true;
                case Qt::Key_Down:
                case Qt::Key_Up:
                    m_slash->moveFocus(key->key() == Qt::Key_Down ? 1 : -1);
                    return true;
                case Qt::Key_Return:
                case Qt::Key_Enter:
                    // 不能落到下面的发送分支：先关面板再说，否则会发出一条空消息
                    if (!(key->modifiers() & Qt::ShiftModifier)) {
                        if (m_slash->chooseFocused()) {
                            closeSlashMenu();
                            return true;
                        }
                    }
                    break;
                default:
                    break;
                }
            }
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
    } else if (watched == m_modeList && event->type() == QEvent::Hide) {
        m_modeTrigger->setOpen(false);
    }
    return QWidget::eventFilter(watched, event);
}

void Composer::setMode(const QString &mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    m_modeTrigger->setText(mode == QLatin1String("auto") ? QString::fromUtf8("自动")
                                                         : QString::fromUtf8("手动"));
    emit modeChanged(m_mode);
}

void Composer::selectMode(const QString &value)
{
    closeModeList();
    setMode(value);
}

void Composer::openModeList()
{
    closeModelList();
    closeSlashMenu();
    m_modeList->setModeOptions(m_mode);
    m_modeTrigger->setOpen(true);
    m_modeList->openAbove(m_modeControl ? m_modeControl : m_modeTrigger);
}

void Composer::closeModeList()
{
    if (m_modeList)
        m_modeList->hide();
    m_modeTrigger->setOpen(false);
}

bool Composer::modeListOpen() const
{
    return m_modeList && m_modeList->isVisible();
}

// 名字宽度按字体实测（QSS 的 padding 不参与 sizeHint 计算，只能自己量）
void Composer::refreshZoom()
{
    updateSkillChipWidth();
}

void Composer::updateSkillChipWidth()
{
    if (!m_skillChip || !m_skillChipName)
        return;
    const QFontMetrics fm(m_skillChipName->font());
    const int inner = fm.horizontalAdvance(m_skillChipName->text()) + 5 + 7;
    m_skillChip->setMinimumWidth(2 * 7 + inner);
}

void Composer::renderSkillChip()
{
    QString label = QStringLiteral("无 Skill");
    for (const QVariant &v : m_skills) {
        const QVariantMap item = v.toMap();
        if (item.value(QStringLiteral("id")).toString() == m_skill) {
            const QString name = item.value(QStringLiteral("name")).toString();
            label = name.isEmpty() ? m_skill : name;
            break;
        }
    }
    m_skillChipName->setText(label);
    updateSkillChipWidth();
    // webui：没有技能时整块 .skill-chip 不显示
    m_skillChip->setVisible(!m_skill.isEmpty());
}

void Composer::selectSkill(const QString &id)
{
    const bool fromMenu = m_slashOpen;
    setCurrentSkill(id);
    emit skillSelected(id);
    // "/xx" 只是唤起面板的输入，选定后清掉，避免混进消息正文
    if (m_input->toPlainText().startsWith(QLatin1Char('/'))) {
        m_input->clear();
        autoGrowInput();
    }
    closeSlashMenu();
    updateSendState();
    if (fromMenu)
        m_input->setFocus(Qt::OtherFocusReason);
}

QString Composer::slashQuery() const
{
    const QString text = m_input->toPlainText();
    if (!text.startsWith(QLatin1Char('/')))
        return QString();
    return text.mid(1).trimmed().toLower();
}

void Composer::openSlashRoot()
{
    m_slash->setRootOptions(m_skills, m_skill, m_model);
    m_slash->setQuery(slashQuery());
    m_slash->setVisible(true);
    m_slashOpen = true;
    layoutSlashPanel();
    m_slash->raise();
}

void Composer::openSlashModelPicker()
{
    m_slash->setModelOptions(m_models, m_model);
    // 模型面板的过滤词与正文无关，面板自带搜索框
    m_slash->setQuery(QString());
    m_slashOpen = true;
    m_slash->setVisible(true);
    layoutSlashPanel();
    m_slash->raise();
    m_slash->focusSearch();
}

void Composer::closeSlashMenu()
{
    if (m_slash) {
        m_slash->hide();
        // 收起时回到根面板：下次敲 "/" 应该看到技能与指令，而不是上次的模型列表
        m_slash->resetToRoot();
    }
    m_slashOpen = false;
}

void Composer::runSlashItem(const QString &kind, const QString &id)
{
    if (kind == QLatin1String("skill")) {
        selectSkill(id);
        return;
    }
    if (kind == QLatin1String("model")) {
        setCurrentModel(id);
        emit modelSelected(id);
        finishSlashPick();
        return;
    }
    if (id == QLatin1String("model")) {
        openSlashModelPicker();
        return;
    }
    if (id == QLatin1String("export")) {
        closeSlashMenu();
        emit exportRequested();
    }
}

void Composer::finishSlashPick()
{
    // 面板收起后正文里的 "/" 已经没有意义，一并清掉并还回焦点
    if (m_input->toPlainText().startsWith(QLatin1Char('/'))) {
        m_input->clear();
        autoGrowInput();
    }
    closeSlashMenu();
    updateSendState();
    m_input->setFocus(Qt::OtherFocusReason);
}

void Composer::syncSlashMenu()
{
    if (m_slashOpen && m_slash && m_slash->mode() == QLatin1String("model"))
        return; // 模型面板开着时不跟正文联动
    const QString text = m_input->toPlainText();
    if (!text.startsWith(QLatin1Char('/'))) {
        if (m_slashOpen)
            closeSlashMenu();
        return;
    }
    if (m_slashOpen)
        m_slash->setQuery(slashQuery());
    else
        openSlashRoot();
}

void Composer::layoutSlashPanel()
{
    if (!m_slash || !m_slashOpen)
        return;
    QWidget *host = m_slash->parentWidget();
    if (!host)
        return;
    // webui .slash-menu{left:0;right:0;bottom:calc(100% + 6px)}：宽度贴输入框、底边在其上方 6px
    const int panelWidth = qMax(0, m_inputWrap->width());
    const int panelHeight = m_slash->heightForContent(panelWidth);
    const QPoint bottomCenter = mapTo(host, QPoint(m_inputWrap->x() + panelWidth / 2,
                                                   m_inputWrap->y() - 6));
    QRect geometry(QPoint(bottomCenter.x() - panelWidth / 2, bottomCenter.y() - panelHeight),
                   QSize(panelWidth, panelHeight));
    // 上方塞不下时贴着宿主顶边，内容自己滚动
    geometry.moveTop(qMax(host->rect().top(), geometry.top()));
    m_slash->setGeometry(geometry);
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
    renderSkillChip();
}

void Composer::openModelList()
{
    closeModeList();
    closeSlashMenu();
    m_modelList->setModelOptions(m_models, m_model);
    m_modelTrigger->setOpen(true);
    // webui 下拉贴在外层 .model-control 左缘，而不是内部按钮
    m_modelList->openAbove(m_modelControl ? m_modelControl : m_modelTrigger);
}

void Composer::closeModelList()
{
    if (m_modelList)
        m_modelList->hide();
    m_modelTrigger->setOpen(false);
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
        chip->setCompact(compactAttachments());
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

bool Composer::compactAttachments() const
{
    // 媒体查询落在窗口宽度上，与 webui 的 @media(max-width:640px) 同口径
    const QWidget *host = window();
    return host && host->width() <= 640;
}

void Composer::applyAttachmentCompact()
{
    if (!m_attachBar)
        return;
    const bool compact = compactAttachments();
    const QList<AttachChip *> chips =
        m_attachBar->findChildren<AttachChip *>(QString(), Qt::FindDirectChildrenOnly);
    for (AttachChip *chip : chips)
        chip->setCompact(compact);
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

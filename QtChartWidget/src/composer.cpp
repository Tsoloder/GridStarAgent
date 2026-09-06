#include "composer.h"

#include "commonwidgets.h"
#include "messagewidgets.h"
#include "popups.h"
#include "theme.h"

#include <QEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

namespace gs {
namespace {

// 回形针 / 麦克风图标按钮（.iconSquare）：SVG 资源着色绘制，颜色跟随 CSS 状态
class IconButton : public QPushButton
{
public:
    enum Glyph { Paperclip, Microphone };
    IconButton(Glyph glyph, int size, QWidget *parent = nullptr)
        : QPushButton(parent), m_glyph(glyph)
    {
        setFixedSize(29, 29);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        m_target = size;
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPushButton::paintEvent(event); // 先让 QSS 画边框/背景
        QColor color = QColor(QStringLiteral("#8499a6"));
        if (!isEnabled())
            color = QColor(QStringLiteral("#55686f"));
        else if (property("recording").toBool())
            color = QColor(QStringLiteral("#ffffff"));
        else if (underMouse())
            color = QColor(QStringLiteral("#50badf"));

        const QString name = m_glyph == Paperclip ? QStringLiteral("paperclip")
                                                  : QStringLiteral("mic");
        QPainter painter(this);
        painter.drawPixmap((width() - m_target) / 2, (height() - m_target) / 2,
                           iconPixmap(name, color, m_target));
    }

private:
    Glyph m_glyph;
    int m_target = 17;
};

} // namespace

// ------------------------------------------------------------ Composer

Composer::Composer(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("composer"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(9, 8, 9, 7);
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

    // 控制行（.controls）
    auto *controls = new QWidget(this);
    controls->setObjectName(QStringLiteral("controls"));
    auto *controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 6);
    controlsLayout->setSpacing(5); // .controls>*+* { margin-left:5px }

    auto *modeSwitch = new QWidget(controls);
    modeSwitch->setObjectName(QStringLiteral("modeSwitch"));
    auto *modeLayout = new QHBoxLayout(modeSwitch);
    modeLayout->setContentsMargins(0, 0, 0, 0);
    modeLayout->setSpacing(0);
    m_manual = new QPushButton(QStringLiteral("MANUAL"), modeSwitch);
    m_auto = new QPushButton(QStringLiteral("AUTO"), modeSwitch);
    for (QPushButton *button : { m_manual, m_auto }) {
        setClass(button, QStringLiteral("modeButton"));
        button->setFixedHeight(27);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        modeLayout->addWidget(button);
    }
    m_manual->setProperty("active", true);
    controlsLayout->addWidget(modeSwitch);

    // 模型 / Skill 下拉（.model-control > span + .model-combobox > button）
    struct ControlDef { const char *caption; ComboTrigger **trigger; };
    const ControlDef defs[2] = { { "模型", &m_modelTrigger }, { "Skill", &m_skillTrigger } };
    for (const ControlDef &def : defs) {
        auto *control = new QFrame(controls);
        setClass(control, QStringLiteral("modelControl"));
        control->setAttribute(Qt::WA_StyledBackground, true);
        control->setFixedHeight(27);
        control->setMaximumWidth(240);
        control->setToolTip(QString::fromUtf8(def.caption));
        auto *layout = new QHBoxLayout(control);
        layout->setContentsMargins(5, 0, 0, 0);
        layout->setSpacing(4);
        QLabel *caption = makeLabel(QStringLiteral("modelControlCaption"),
                                    QString::fromUtf8(def.caption), control);
        ComboTrigger *trigger = new ComboTrigger(control);
        trigger->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(caption);
        layout->addWidget(trigger, 1);
        *def.trigger = trigger;
        controlsLayout->addWidget(control, 1);
    }
    controlsLayout->addStretch(1);

    m_settings = new IconPushButton(controls);
    m_settings->setObjectName(QStringLiteral("settingsButton"));
    m_settings->setFixedSize(28, 27);
    m_settings->setToolTip(QStringLiteral("打开设置"));
    m_settings->setCursor(Qt::PointingHandCursor);
    m_settings->setFocusPolicy(Qt::NoFocus);
    m_settings->setIconColors(QColor(QStringLiteral("#90a7b3")),
                              QColor(QStringLiteral("#50badf")));
    m_settings->setIconName(QStringLiteral("settings"), 14);
    controlsLayout->addWidget(m_settings);
    outer->addWidget(controls);

    // 附件条（.attach-bar）
    m_attachBar = new QWidget(this);
    m_attachBar->setObjectName(QStringLiteral("attachBar"));
    m_attachLayout = new FlowLayout(m_attachBar, 0, 5, 5);
    m_attachBar->setVisible(false);
    outer->addWidget(m_attachBar);

    // 输入区（.input-wrap）：文本框 + 三个绝对定位按钮
    m_inputWrap = new QFrame(this);
    m_inputWrap->setObjectName(QStringLiteral("inputWrap"));
    m_inputWrap->setAttribute(Qt::WA_StyledBackground, true);
    m_inputWrap->setFixedHeight(72);
    m_input = new QTextEdit(m_inputWrap);
    m_input->setObjectName(QStringLiteral("messageInput"));
    m_input->setPlaceholderText(QStringLiteral("输入网格生成或质量检查任务，可拖拽文件到此…"));
    m_input->setFrameShape(QFrame::NoFrame);
    m_input->setFixedHeight(70);
    m_input->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_input->setTabChangesFocus(true);
    m_input->installEventFilter(this);

    m_attach = new IconButton(IconButton::Paperclip, 17, m_inputWrap);
    setClass(m_attach, QStringLiteral("iconSquare"));
    m_attach->setToolTip(QStringLiteral("添加附件"));
    m_voice = new IconButton(IconButton::Microphone, 18, m_inputWrap);
    setClass(m_voice, QStringLiteral("iconSquare"));
    m_voice->setToolTip(QStringLiteral("语音输入"));
    m_voice->setEnabled(false);
    m_send = new IconPushButton(m_inputWrap);
    m_send->setObjectName(QStringLiteral("sendButton"));
    m_send->setFixedSize(29, 29);
    m_send->setToolTip(QStringLiteral("发送"));
    m_send->setCursor(Qt::PointingHandCursor);
    m_send->setFocusPolicy(Qt::NoFocus);
    m_send->setIconColors(QColor(QStringLiteral("#ffffff")),
                          QColor(QStringLiteral("#ffffff")),
                          QColor(QStringLiteral("#cfe3ec")));
    m_send->setIconName(QStringLiteral("arrow-up"), 14);
    m_send->setEnabled(false);
    outer->addWidget(m_inputWrap);

    // 底注（.composer-foot）
    auto *foot = new QWidget(this);
    foot->setFixedHeight(14);
    auto *footLayout = new QHBoxLayout(foot);
    footLayout->setContentsMargins(0, 3, 0, 0);
    footLayout->setSpacing(0);
    m_busyLabel = new QLabel(QStringLiteral("Enter 发送 · Shift+Enter 换行"), foot);
    m_busyLabel->setObjectName(QStringLiteral("busyLabel"));
    m_busyLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    footLayout->addStretch(1);
    footLayout->addWidget(m_busyLabel);
    outer->addWidget(foot);

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
        clearAttachments();
        emit sendMessage(content, attachments);
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
    connect(m_input, &QTextEdit::textChanged, this, [this] { updateSendState(); });
}

void Composer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutInputButtons();
}

void Composer::layoutInputButtons()
{
    if (!m_inputWrap)
        return;
    m_input->setGeometry(1, 1, qMax(0, m_inputWrap->width() - 2), 70);
    // right:79 / right:43 / right:7，bottom:7
    const int y = m_inputWrap->height() - 7 - 29;
    m_attach->move(m_inputWrap->width() - 79 - 29, y);
    m_voice->move(m_inputWrap->width() - 43 - 29, y);
    m_send->move(m_inputWrap->width() - 7 - 29, y);
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
    m_send->setIconName(busy ? QStringLiteral("square") : QStringLiteral("arrow-up"));
    m_send->setProperty("stop", busy);
    restyle(m_send);
    m_send->setToolTip(busy ? QStringLiteral("停止接收") : QStringLiteral("发送"));
    m_input->setEnabled(!busy);
    m_attach->setEnabled(!busy);
    m_busyLabel->setText(busy ? QStringLiteral("Agent 正在处理…")
                              : QStringLiteral("Enter 发送 · Shift+Enter 换行"));
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
        const QString name = item.value(QStringLiteral("name")).toString();
        auto *chip = new AttachChip(name, item.value(QStringLiteral("size")).toLongLong(),
                                    item.value(QStringLiteral("ext")).toString(), uploading,
                                    m_attachBar);
        chip->setProperty("attachId", item.value(QStringLiteral("id")));
        chip->setToolTip(name);
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

} // namespace gs

#include "popups.h"

#include "commonwidgets.h"
#include "theme.h"

#include <QApplication>
#include <QDesktopWidget>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMap>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

namespace gs {

// ------------------------------------------------------------ 模型工具函数

QString modelKey(const QVariantMap &item)
{
    const QString key = item.value(QStringLiteral("key")).toString();
    if (!key.isEmpty())
        return key;
    const QString id = item.contains(QStringLiteral("model_id"))
                           ? item.value(QStringLiteral("model_id")).toString()
                           : item.value(QStringLiteral("id")).toString();
    if (id.contains(QLatin1Char('/')))
        return id;
    return item.value(QStringLiteral("provider")).toString() + QLatin1Char('/') + id;
}

QString modelName(const QVariantMap &item)
{
    for (const char *k : { "name", "display_name", "model_id", "id" }) {
        const QString v = item.value(QLatin1String(k)).toString();
        if (!v.isEmpty())
            return v;
    }
    return modelKey(item);
}

QVariantList visibleModels(const QVariantList &models)
{
    QVariantList out;
    for (const QVariant &v : models) {
        const QVariantMap item = v.toMap();
        // enabled !== false && provider_enabled !== false
        const QVariant enabled = item.value(QStringLiteral("enabled"));
        const QVariant providerEnabled = item.value(QStringLiteral("provider_enabled"));
        if ((enabled.isValid() && !enabled.toBool())
            || (providerEnabled.isValid() && !providerEnabled.toBool()))
            continue;
        out.append(item);
    }
    return out;
}

// ------------------------------------------------------------ ListOptionRow

ListOptionRow::ListOptionRow(const QString &value, const QString &name, const QString &sub,
                             QWidget *parent)
    : QWidget(parent), m_value(value), m_full(name)
{
    setClass(this, QStringLiteral("modelOption"));
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    m_search = (name + QLatin1Char(' ') + sub).toLower();

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(7);

    // grid-template-columns: 18px minmax(0,1fr) auto
    // 行内标签关掉文本交互：makeLabel 默认允许选中文本，会吃掉点击导致 mouseReleaseEvent 收不到
    m_check = makeLabel(QStringLiteral("modelCheck"), QString(), this);
    m_check->setTextInteractionFlags(Qt::NoTextInteraction);
    m_check->setFixedWidth(18);
    m_check->setAlignment(Qt::AlignCenter);
    m_name = makeLabel(QStringLiteral("modelName"), name, this);
    m_name->setTextInteractionFlags(Qt::NoTextInteraction);
    m_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_id = makeLabel(QStringLiteral("modelId"), sub, this);
    m_id->setTextInteractionFlags(Qt::NoTextInteraction);

    layout->addWidget(m_check);
    layout->addWidget(m_name, 1);
    layout->addWidget(m_id);
}

void ListOptionRow::setSelected(bool selected)
{
    if (selected)
        m_check->setPixmap(iconPixmap(QStringLiteral("check"),
                                      QColor(QStringLiteral("#50badf")), 11));
    else
        m_check->clear();
    setProperty("selected", selected);
    restyle(this);
}

void ListOptionRow::setFocusedRow(bool focused)
{
    setProperty("focused", focused);
    restyle(this);
}

void ListOptionRow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos()))
        emit activated(m_value);
    QWidget::mouseReleaseEvent(event);
}

void ListOptionRow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // strong { text-overflow: ellipsis }
    m_name->setText(elidedText(m_full, m_name->fontMetrics(), m_name->width()));
}

// ------------------------------------------------------------ ListBoxPopup

ListBoxPopup::ListBoxPopup(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("listbox"));
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setFixedWidth(250);
    setMaximumHeight(280);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(5, 5, 5, 5);
    outer->setSpacing(0);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("listboxScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);

    m_inner = new QWidget(m_scroll);
    m_inner->setObjectName(QStringLiteral("listboxInner"));
    m_innerLayout = new QVBoxLayout(m_inner);
    m_innerLayout->setContentsMargins(0, 0, 0, 0);
    m_innerLayout->setSpacing(0);
    m_scroll->setWidget(m_inner);
    outer->addWidget(m_scroll);

    m_typeTimer = new QTimer(this);
    m_typeTimer->setSingleShot(true);
    m_typeTimer->setInterval(700);
    connect(m_typeTimer, &QTimer::timeout, this, [this] { m_typeAhead.clear(); });
}

void ListBoxPopup::reset()
{
    m_rows.clear();
    m_focusIndex = 0;
    m_typeAhead.clear();
    while (QLayoutItem *item = m_innerLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
}

void ListBoxPopup::addGroupLabel(const QString &text)
{
    QLabel *label = makeLabel(QStringLiteral("modelGroupLabel"), text.toUpper(), m_inner);
    m_innerLayout->addWidget(label);
}

void ListBoxPopup::addGroupSeparator()
{
    // .model-group + .model-group { margin-top:5px; border-top:1px solid #293d48 }
    m_innerLayout->addSpacing(5);
    auto *line = new QFrame(m_inner);
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background:#293d48;"));
    m_innerLayout->addWidget(line);
}

ListOptionRow *ListBoxPopup::addRow(const QString &value, const QString &name, const QString &sub,
                                    bool selected, const QString &tooltip)
{
    auto *row = new ListOptionRow(value, name, sub, m_inner);
    row->setSelected(selected);
    if (!tooltip.isEmpty())
        row->setToolTip(tooltip);
    connect(row, &ListOptionRow::activated, this, [this](const QString &v) {
        hide();
        emit chosen(v);
    });
    m_innerLayout->addWidget(row);
    m_rows.append(row);
    return row;
}

void ListBoxPopup::setModelOptions(const QVariantList &models, const QString &selectedKey)
{
    reset();
    const QVariantList visible = visibleModels(models);
    // groups: provider -> items（保持出现顺序）
    QStringList providers;
    QMap<QString, QVariantList> groups;
    QMap<QString, QString> groupLabels;
    for (const QVariant &v : visible) {
        const QVariantMap item = v.toMap();
        const QString provider = item.value(QStringLiteral("provider")).toString();
        if (!groups.contains(provider)) {
            providers.append(provider);
            groupLabels.insert(provider,
                               item.value(QStringLiteral("provider_name")).toString().isEmpty()
                                   ? provider
                                   : item.value(QStringLiteral("provider_name")).toString());
        }
        groups[provider].append(item);
    }
    for (int i = 0; i < providers.size(); ++i) {
        if (i > 0)
            addGroupSeparator();
        const QString provider = providers.at(i);
        addGroupLabel(groupLabels.value(provider));
        const QVariantList items = groups.value(provider);
        for (const QVariant &v : items) {
            const QVariantMap item = v.toMap();
            const QString key = modelKey(item);
            addRow(key, modelName(item),
                   item.value(QStringLiteral("model_id")).toString().isEmpty()
                       ? item.value(QStringLiteral("id")).toString()
                       : item.value(QStringLiteral("model_id")).toString(),
                   key == selectedKey);
        }
    }
    if (providers.isEmpty()) {
        QLabel *empty = makeLabel(QStringLiteral("listboxEmpty"),
                                  QStringLiteral("未配置可用模型"), m_inner);
        empty->setAlignment(Qt::AlignCenter);
        m_innerLayout->addWidget(empty);
    }
    m_focusIndex = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i)->value() == selectedKey) {
            m_focusIndex = i;
            break;
        }
    }
    applyFocus();
}

void ListBoxPopup::setSkillOptions(const QVariantList &skills, const QString &selectedId)
{
    reset();
    QVariantList all;
    QVariantMap none;
    none.insert(QStringLiteral("id"), QString());
    none.insert(QStringLiteral("name"), QStringLiteral("无 Skill"));
    all.append(none);
    all += skills;
    for (const QVariant &v : all) {
        const QVariantMap item = v.toMap();
        const QString id = item.value(QStringLiteral("id")).toString();
        const QString name = item.value(QStringLiteral("name")).toString().isEmpty() ? id
                                                                                     : item.value(QStringLiteral("name")).toString();
        addRow(id, name, QString(), id == selectedId,
               item.value(QStringLiteral("description")).toString());
    }
    m_focusIndex = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i)->value() == selectedId) {
            m_focusIndex = i;
            break;
        }
    }
    applyFocus();
}

void ListBoxPopup::applyFocus()
{
    for (int i = 0; i < m_rows.size(); ++i)
        m_rows.at(i)->setFocusedRow(i == m_focusIndex);
    if (m_rows.isEmpty())
        return;
    ListOptionRow *active = m_rows.at(qBound(0, m_focusIndex, m_rows.size() - 1));
    // scrollIntoView({block:"nearest"})
    QRect visible = m_scroll->viewport()->rect();
    QRect row(active->mapTo(m_inner, QPoint(0, 0)), active->size());
    if (row.top() < visible.top())
        m_scroll->verticalScrollBar()->setValue(row.top());
    else if (row.bottom() > visible.bottom())
        m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->value()
                                                + row.bottom() - visible.bottom());
}

void ListBoxPopup::moveFocus(int delta)
{
    if (m_rows.isEmpty())
        return;
    m_focusIndex = (m_focusIndex + delta + m_rows.size()) % m_rows.size();
    applyFocus();
}

void ListBoxPopup::openAbove(QWidget *anchor)
{
    if (!anchor)
        return;
    m_inner->layout()->activate();
    const int height = qMin(m_inner->sizeHint().height() + 12, 280);
    const QRect screen = QApplication::desktop()->availableGeometry(anchor);
    QPoint topLeft = anchor->mapToGlobal(QPoint(0, 0));
    int x = qBound(screen.left(), topLeft.x(), screen.right() - width());
    int y = topLeft.y() - height - 4;
    if (y < screen.top())
        y = qMin(topLeft.y() + anchor->height() + 4, screen.bottom() - height);
    setGeometry(x, y, width(), height);
    show();
    raise();
    setFocus(Qt::PopupFocusReason);
    applyFocus();
}

void ListBoxPopup::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        hide();
        return;
    case Qt::Key_Down:
        moveFocus(1);
        return;
    case Qt::Key_Up:
        moveFocus(-1);
        return;
    case Qt::Key_Enter:
    case Qt::Key_Return:
        if (!m_rows.isEmpty()) {
            const QString value = m_rows.at(qBound(0, m_focusIndex, m_rows.size() - 1))->value();
            hide();
            emit chosen(value);
        }
        return;
    default:
        break;
    }
    // 输入即搜索（handleModelKeys 的 type-ahead）
    const QString text = event->text();
    if (text.size() == 1 && !text.at(0).isSpace() && text.at(0).unicode() >= 0x20) {
        m_typeAhead += text.toLower();
        m_typeTimer->start();
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows.at(i)->searchText().contains(m_typeAhead)) {
                m_focusIndex = i;
                applyFocus();
                break;
            }
        }
        return;
    }
    QFrame::keyPressEvent(event);
}

// ------------------------------------------------------------ SessionPanel

QRect SessionPanel::areaFor(const QSize &host)
{
    const int width = qMax(0, host.width() - 16);
    const int height = qMax(0, qMin(430, host.height() - 120));
    return QRect(8, 92, width, height);
}

SessionPanel::SessionPanel(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("sessionPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setVisible(false);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *head = new QWidget(this);
    head->setObjectName(QStringLiteral("panelHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(8, 8, 8, 8);
    headLayout->setSpacing(6);
    m_search = new QLineEdit(head);
    m_search->setObjectName(QStringLiteral("sessionSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索会话"));
    m_search->setFixedHeight(30);
    auto *close = new IconPushButton(head);
    close->setProperty("variant", QStringLiteral("icon"));
    close->setFixedSize(30, 30);
    close->setCursor(Qt::PointingHandCursor);
    close->setToolTip(QStringLiteral("关闭"));
    close->setIconColors(QColor(QStringLiteral("#d7e2e8")),
                         QColor(QStringLiteral("#50badf")));
    close->setIconName(QStringLiteral("x"), 14);
    headLayout->addWidget(m_search, 1);
    headLayout->addWidget(close);
    outer->addWidget(head);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("sessionScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setStyleSheet(QStringLiteral("QScrollArea#sessionScroll{border:0;background:transparent;}"));
    auto *list = new QWidget(scroll);
    list->setObjectName(QStringLiteral("sessionList"));
    list->setStyleSheet(QStringLiteral("QWidget#sessionList{background:transparent;}"));
    m_listLayout = new QVBoxLayout(list);
    m_listLayout->setContentsMargins(5, 5, 5, 5);
    m_listLayout->setSpacing(0);
    m_listLayout->addStretch(1);
    scroll->setWidget(list);
    outer->addWidget(scroll, 1);

    connect(close, &QPushButton::clicked, this, [this] {
        closePanel();
        emit closeRequested();
    });
    connect(m_search, &QLineEdit::textChanged, this, [this] { render(); });
}

void SessionPanel::setSessions(const QVariantList &sessions)
{
    m_sessions = sessions;
    render();
}

void SessionPanel::render()
{
    const QString query = m_search->text().trimmed().toLower();
    while (QLayoutItem *item = m_listLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    QVariantList shown;
    for (const QVariant &v : m_sessions) {
        const QVariantMap session = v.toMap();
        if (query.isEmpty() || session.value(QStringLiteral("title")).toString().toLower().contains(query))
            shown.append(session);
    }

    if (shown.isEmpty()) {
        QLabel *empty = makeLabel(QStringLiteral("listboxEmpty"), QStringLiteral("没有会话"), this);
        empty->setAlignment(Qt::AlignCenter);
        m_listLayout->addWidget(empty);
        m_listLayout->addStretch(1);
        return;
    }

    for (const QVariant &v : shown) {
        const QVariantMap session = v.toMap();
        const QString id = session.value(QStringLiteral("id")).toString();
        const QString title = session.value(QStringLiteral("title")).toString();

        auto *row = new QWidget(this);
        setClass(row, QStringLiteral("sessionRow"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(0);

        // .session-select：标题 + 更新时间，整块可点
        auto *select = new QWidget(row);
        select->setCursor(Qt::PointingHandCursor);
        auto *selectLayout = new QVBoxLayout(select);
        selectLayout->setContentsMargins(7, 9, 7, 9);
        selectLayout->setSpacing(2);
        QLabel *titleLabel = makeLabel(QStringLiteral("sessionTitle"),
                                       title.isEmpty() ? QStringLiteral("未命名会话") : title, select);
        titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
        titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        QString stamp = session.value(QStringLiteral("updated_at")).toString();
        if (stamp.isEmpty())
            stamp = session.value(QStringLiteral("created_at")).toString();
        stamp = stamp.left(16).replace(QLatin1Char('T'), QLatin1Char(' '));
        QLabel *meta = makeLabel(QStringLiteral("sessionMeta"), stamp, select);
        meta->setTextInteractionFlags(Qt::NoTextInteraction);
        selectLayout->addWidget(titleLabel);
        selectLayout->addWidget(meta);

        auto *actions = new QWidget(row);
        auto *actionsLayout = new QHBoxLayout(actions);
        actionsLayout->setContentsMargins(0, 0, 4, 0);
        actionsLayout->setSpacing(0);
        struct Action { const char *icon; const char *tip; bool danger; };
        const Action defs[3] = { { "pencil", "重命名", false },
                                 { "trash-2", "清空", false },
                                 { "x", "删除", true } };
        QList<QPushButton *> buttons;
        for (const Action &def : defs) {
            auto *button = new IconPushButton(actions);
            setClass(button, QStringLiteral("sessionAction"));
            if (def.danger)
                button->setProperty("danger", true);
            button->setToolTip(QString::fromUtf8(def.tip));
            button->setCursor(Qt::PointingHandCursor);
            button->setFixedSize(28, 28);
            button->setIconColors(QColor(QStringLiteral("#8499a6")),
                                  def.danger ? QColor(QStringLiteral("#e36c6c"))
                                             : QColor(QStringLiteral("#50badf")));
            button->setIconName(QString::fromUtf8(def.icon), 13);
            actionsLayout->addWidget(button);
            buttons.append(button);
        }

        rowLayout->addWidget(select, 1);
        rowLayout->addWidget(actions);
        m_listLayout->addWidget(row);

        select->installEventFilter(this);
        select->setProperty("sessionId", id);
        buttons.at(0)->setProperty("sessionId", id);
        buttons.at(1)->setProperty("sessionId", id);
        buttons.at(2)->setProperty("sessionId", id);
        connect(buttons.at(0), &QPushButton::clicked, this, [this, id] { emit sessionRenamed(id); });
        connect(buttons.at(1), &QPushButton::clicked, this, [this, id] { emit sessionCleared(id); });
        connect(buttons.at(2), &QPushButton::clicked, this, [this, id] { emit sessionDeleted(id); });
    }
    m_listLayout->addStretch(1);
}

bool SessionPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            const QString id = watched->property("sessionId").toString();
            if (!id.isEmpty()) {
                emit sessionSelected(id);
                return true;
            }
        }
    }
    return QFrame::eventFilter(watched, event);
}

void SessionPanel::open()
{
    m_open = true;
    show();
    raise();
    m_search->setFocus();
}

void SessionPanel::closePanel()
{
    m_open = false;
    hide();
}

bool SessionPanel::isOpen() const
{
    return m_open;
}

void SessionPanel::layoutIn(const QSize &host)
{
    setGeometry(areaFor(host));
}

// ------------------------------------------------------------ Toast

Toast::Toast(QWidget *parent)
    : QLabel(parent)
{
    setObjectName(QStringLiteral("toast"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setWordWrap(true);
    setVisible(false);
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(5000);
    connect(timer, &QTimer::timeout, this, [this] { hide(); });
    m_timer = timer;
}

void Toast::showMessage(const QString &text)
{
    setText(text);
    if (parentWidget())
        layoutIn(parentWidget()->size());
    show();
    raise();
    m_timer->start();
}

void Toast::layoutIn(const QSize &host)
{
    const int width = qMax(0, host.width() - 20);
    const int height = heightForWidth(width);
    setGeometry(10, qMax(0, host.height() - 125 - height), width, height);
}

// ------------------------------------------------------------ DropOverlay

DropOverlay::DropOverlay(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("dropOverlay"));
    setAttribute(Qt::WA_StyledBackground, true);
    setVisible(false);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QLabel *text = new QLabel(QStringLiteral("松开以添加附件"), this);
    text->setObjectName(QStringLiteral("dropOverlayText"));
    text->setAlignment(Qt::AlignCenter);
    layout->addWidget(text);
}

void DropOverlay::setActive(bool active)
{
    setVisible(active);
    if (active) {
        if (parentWidget())
            layoutIn(parentWidget()->size());
        raise();
    }
}

void DropOverlay::layoutIn(const QSize &host)
{
    setGeometry(0, 0, host.width(), host.height());
}

// ------------------------------------------------------------ ConfirmDialog

ConfirmDialog::ConfirmDialog(const QString &title, const QString &message, const QString *input,
                             const QString &confirmText, const QString &cancelText, bool danger,
                             QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("confirmDialog"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setModal(true);
    setFixedWidth(380);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(0);

    QLabel *titleLabel = new QLabel(title, this);
    titleLabel->setObjectName(QStringLiteral("confirmTitle"));
    layout->addWidget(titleLabel);
    layout->addSpacing(8);

    if (!message.isEmpty()) {
        QLabel *messageLabel = new QLabel(message, this);
        messageLabel->setObjectName(QStringLiteral("confirmMessage"));
        messageLabel->setWordWrap(true);
        layout->addWidget(messageLabel);
        layout->addSpacing(12);
    }

    if (input) {
        m_input = new QLineEdit(*input, this);
        setClass(m_input, QStringLiteral("settingsInput"));
        m_input->setFixedHeight(32);
        layout->addWidget(m_input);
        layout->addSpacing(12);
    }

    auto *actions = new QHBoxLayout();
    actions->setSpacing(8);
    actions->addStretch(1);
    auto *cancel = new QPushButton(cancelText, this);
    cancel->setProperty("variant", QStringLiteral("option"));
    cancel->setMinimumWidth(72);
    cancel->setMinimumHeight(30);
    cancel->setCursor(Qt::PointingHandCursor);
    auto *ok = new QPushButton(confirmText, this);
    ok->setProperty("variant", danger ? QStringLiteral("optionDanger") : QStringLiteral("optionPrimary"));
    ok->setMinimumWidth(72);
    ok->setMinimumHeight(30);
    ok->setCursor(Qt::PointingHandCursor);
    ok->setDefault(true);
    actions->addWidget(cancel);
    actions->addWidget(ok);
    layout->addLayout(actions);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    if (m_input) {
        m_input->setFocus();
        m_input->selectAll();
    } else {
        ok->setFocus();
    }
}

QString ConfirmDialog::textValue() const
{
    return m_input ? m_input->text().trimmed() : QString();
}

bool ConfirmDialog::ask(QWidget *parent, const QString &title, const QString &message,
                        const QString &confirmText, bool danger)
{
    ConfirmDialog dialog(title, message, nullptr, confirmText, QStringLiteral("取消"), danger, parent);
    return dialog.exec() == QDialog::Accepted;
}

QString ConfirmDialog::prompt(QWidget *parent, const QString &title, const QString &initial,
                              const QString &confirmText)
{
    ConfirmDialog dialog(title, QString(), &initial, confirmText, QStringLiteral("取消"), false, parent);
    if (dialog.exec() != QDialog::Accepted)
        return QString();
    return dialog.textValue();
}

} // namespace gs

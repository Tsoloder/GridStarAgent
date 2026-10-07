#include "popups.h"

#include "commonwidgets.h"
#include "theme.h"

#include <QApplication>
#include <QDesktopWidget>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMap>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
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

QVariantList configuredModels(const QVariantList &models)
{
    // app.js configuredModels()：下拉只列配置里写过的模型，
    // 供应商发现来的整份目录（status === "discovered"）不进选择范围
    QVariantList out;
    for (const QVariant &v : visibleModels(models)) {
        if (v.toMap().value(QStringLiteral("status")).toString() == QLatin1String("discovered"))
            continue;
        out.append(v);
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

    // grid-template-columns: minmax(0,1fr) auto auto（勾已挪到最右）
    // 行内标签关掉文本交互：makeLabel 默认允许选中文本，会吃掉点击导致 mouseReleaseEvent 收不到
    m_name = makeLabel(QStringLiteral("modelName"), name, this);
    m_name->setTextInteractionFlags(Qt::NoTextInteraction);
    m_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_id = makeLabel(QStringLiteral("modelId"), sub, this);
    m_id->setTextInteractionFlags(Qt::NoTextInteraction);
    m_check = makeLabel(QStringLiteral("modelCheck"), QString(), this);
    m_check->setTextInteractionFlags(Qt::NoTextInteraction);
    // .model-check{min-width:12px;justify-self:end}：勾不占位时必须留出 12px，行宽才不会跳
    m_check->setFixedWidth(12);
    m_check->setAlignment(Qt::AlignCenter);

    layout->addWidget(m_name, 1);
    layout->addWidget(m_id);
    layout->addWidget(m_check);
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

void ListOptionRow::setCompact(bool compact)
{
    if (m_compact == compact)
        return;
    m_compact = compact;
    m_id->setVisible(!compact);
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

// ------------------------------------------------------------ ModeOptionRow

ModeOptionRow::ModeOptionRow(const QString &value, const QString &title, const QString &hint,
                            QWidget *parent)
    : QWidget(parent), m_value(value)
{
    setClass(this, QStringLiteral("modelOption modeOption"));
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(6);

    // .mode-option-text：标题 + 说明两行，说明不参与省略（webui 是 white-space:normal）
    m_text = new QWidget(this);
    setClass(m_text, QStringLiteral("modeOptionText"));
    auto *textLayout = new QVBoxLayout(m_text);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);
    QLabel *titleLabel = makeLabel(QStringLiteral("modeOptionTitle"), title, m_text);
    titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    QLabel *hintLabel = makeLabel(QStringLiteral("modeOptionHint"), hint, m_text);
    hintLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    hintLabel->setWordWrap(true);
    textLayout->addWidget(titleLabel);
    textLayout->addWidget(hintLabel);

    m_check = makeLabel(QStringLiteral("modeCheck"), QStringLiteral("✓"), this);
    m_check->setTextInteractionFlags(Qt::NoTextInteraction);
    m_check->setAlignment(Qt::AlignTop | Qt::AlignRight);
    m_check->setFixedWidth(12);
    // .mode-check{visibility:hidden} + 选中才可见：未选中不画勾但仍占位
    m_check->setVisible(false);

    layout->addWidget(m_text, 1);
    layout->addWidget(m_check, 0);
}

void ModeOptionRow::setSelected(bool selected)
{
    if (m_check)
        m_check->setVisible(selected);
    setProperty("selected", selected);
    restyle(this);
}

void ModeOptionRow::setFocusedRow(bool focused)
{
    setProperty("focused", focused);
    restyle(this);
}

void ModeOptionRow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos()))
        emit activated(m_value);
    QWidget::mouseReleaseEvent(event);
}

// ------------------------------------------------------------ SlashRow

SlashRow::SlashRow(const QString &kind, const QString &id, const QString &name, const QString &code,
                   const QString &desc, bool withCheck, QWidget *parent)
    : QWidget(parent), m_kind(kind), m_id(id), m_name(name), m_code(code), m_desc(desc)
{
    setClass(this, QStringLiteral("slashItem"));
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    if (!desc.isEmpty())
        setToolTip(desc);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(8);
    // .slash-item{align-items:baseline}：三行文本同基线，说明右对齐并单行省略
    layout->setAlignment(Qt::AlignVCenter);

    QLabel *nameLabel = makeLabel(QStringLiteral("slashName"), name, this);
    nameLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    layout->addWidget(nameLabel, 0);
    QLabel *codeLabel = makeLabel(QStringLiteral("slashCode"), code, this);
    codeLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    layout->addWidget(codeLabel, 0);
    // QLabel 没有 text-overflow：说明过长时按当前宽度省略（.slash-desc 的 ellipsis）
    m_descLabel = makeLabel(QStringLiteral("slashDesc"), desc, this);
    m_descLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    m_descLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_descLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(m_descLabel, 1);
    if (withCheck) {
        m_check = makeLabel(QStringLiteral("modelCheck"), QString(), this);
        m_check->setTextInteractionFlags(Qt::NoTextInteraction);
        m_check->setFixedWidth(12);
        m_check->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_check, 0);
    }
}

void SlashRow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_descLabel)
        m_descLabel->setText(elidedText(m_desc, m_descLabel->fontMetrics(), m_descLabel->width()));
}

bool SlashRow::matches(const QString &query) const
{
    if (query.isEmpty())
        return true;
    return m_name.toLower().contains(query) || m_code.toLower().contains(query)
           || m_id.toLower().contains(query);
}

void SlashRow::setSelected(bool selected)
{
    if (m_check) {
        if (selected)
            m_check->setPixmap(iconPixmap(QStringLiteral("check"),
                                          QColor(QStringLiteral("#50badf")), 11));
        else
            m_check->clear();
    }
    setProperty("selected", selected);
    restyle(this);
}

void SlashRow::setFocusedRow(bool focused)
{
    setProperty("focused", focused);
    restyle(this);
}

void SlashRow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos()))
        emit activated(m_kind, m_id);
    QWidget::mouseReleaseEvent(event);
}

// ------------------------------------------------------------ ListBoxPopup

ListBoxPopup::ListBoxPopup(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("listbox"));
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
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
    m_modeRows.clear();
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
    m_preferredWidth = 330;
    setClass(this, QStringLiteral("listbox"));
    reset();
    // 只列配置里写过的模型：供应商发现来的整份目录不进选择范围
    const QVariantList visible = configuredModels(models);
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
                                  QStringLiteral("没有已配置的模型"), m_inner);
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
    m_preferredWidth = 250;
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

void ListBoxPopup::setModeOptions(const QString &selectedMode)
{
    // .mode-listbox{width:240px}：同一个 ListBoxPopup 换脸，样式靠类属性区分
    m_preferredWidth = 240;
    setClass(this, QStringLiteral("listbox modeListbox"));
    reset();
    // app.js MODE_OPTIONS：手动 / 自动两项，每项一行标题加一行说明
    const struct {
        const char *value;
        const char *title;
        const char *hint;
    } options[] = {
        {"manual", "手动", "每步操作先确认参数，由审批面板把关"},
        {"auto", "自动", "按默认参数连续执行，不逐步确认"},
    };
    int selectedIndex = 0;
    for (int i = 0; i < 2; ++i) {
        auto *row = new ModeOptionRow(QString::fromLatin1(options[i].value),
                                      QString::fromUtf8(options[i].title),
                                      QString::fromUtf8(options[i].hint), m_inner);
        const bool selected = QString::fromLatin1(options[i].value) == selectedMode;
        row->setSelected(selected);
        if (selected)
            selectedIndex = i;
        connect(row, &ModeOptionRow::activated, this, [this](const QString &v) {
            hide();
            emit chosen(v);
        });
        m_innerLayout->addWidget(row);
        m_modeRows.append(row);
    }
    m_focusIndex = selectedIndex;
    applyFocus();
}

void ListBoxPopup::applyFocus()
{
    for (int i = 0; i < m_modeRows.size(); ++i)
        m_modeRows.at(i)->setFocusedRow(i == m_focusIndex);
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
    const int count = m_modeRows.isEmpty() ? m_rows.size() : m_modeRows.size();
    if (count <= 0)
        return;
    m_focusIndex = (m_focusIndex + delta + count) % count;
    applyFocus();
}

void ListBoxPopup::openAbove(QWidget *anchor)
{
    if (!anchor)
        return;
    // webui：下拉 left:0 贴外层 .model-control 左缘、bottom:calc(100% + 7px)，
    // 宽度 calc(100vw - 18px) 再被 max-width 截断（模型 330px / Skill 250px）
    QWidget *host = anchor->window();
    const QRect hostRect = host ? QRect(host->mapToGlobal(QPoint(0, 0)), host->size())
                                : QApplication::desktop()->availableGeometry(anchor);
    const bool compact = hostRect.width() <= 640;
    for (ListOptionRow *row : m_rows)
        row->setCompact(compact);
    m_inner->layout()->activate();
    const int width = qMax(0, qMin(m_preferredWidth, hostRect.width() - 18));
    // 绝对定位的包含块是 .model-control 的 padding box（border-box 内缩 1px 边框）
    const QPoint topLeft = anchor->mapToGlobal(anchor->contentsRect().topLeft());
    const int minX = hostRect.left();
    const int maxX = qMax(minX, hostRect.right() - width);
    const int x = qBound(minX, topLeft.x(), maxX);

    const auto anchoredY = [&](int popupHeight) {
        // bottom:calc(100% + 7px)：底边贴着控件上方 7px
        int y = topLeft.y() - popupHeight - 7;
        // 上方空间不够时翻到下面（webui 没有这一支，属于兜底）
        if (y < hostRect.top())
            y = qMin(anchor->mapToGlobal(QPoint(0, 0)).y() + anchor->height() + 7,
                     hostRect.bottom() - popupHeight);
        return y;
    };

    const int estimated = qMin(m_inner->sizeHint().height() + 12, 280);
    setGeometry(x, anchoredY(estimated), width, estimated);
    show();
    // 样式表要等 show() 之后才 polish，隐藏状态下量到的 sizeHint 偏小，窗口会被布局的最小高度顶大；
    // 按定稿高度重贴一次，保持底边贴着锚点（webui 的弹层底边固定、向上生长）。
    const int settled = height();
    if (settled != estimated)
        setGeometry(x, anchoredY(settled), width, settled);
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
        if (!m_modeRows.isEmpty()) {
            const QString value =
                m_modeRows.at(qBound(0, m_focusIndex, m_modeRows.size() - 1))->value();
            hide();
            emit chosen(value);
        } else if (!m_rows.isEmpty()) {
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

// ------------------------------------------------------------ SlashPanel

SlashPanel::SlashPanel(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("slashMenu"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::NoFocus);

    auto *outer = new QVBoxLayout(this);
    // .slash-menu{padding:5px;max-height:264px;overflow:auto}
    outer->setContentsMargins(5, 5, 5, 5);
    outer->setSpacing(0);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("slashScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);

    m_inner = new QWidget(m_scroll);
    setClass(m_inner, QStringLiteral("slashInner"));
    m_innerLayout = new QVBoxLayout(m_inner);
    m_innerLayout->setContentsMargins(0, 0, 0, 0);
    m_innerLayout->setSpacing(0);
    m_scroll->setWidget(m_inner);
    outer->addWidget(m_scroll);

    m_empty = makeLabel(QStringLiteral("listboxEmpty"), QStringLiteral("没有匹配的技能或指令"),
                        m_inner);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setVisible(false);
    m_innerLayout->addWidget(m_empty);
}

void SlashPanel::clearRows()
{
    m_rows.clear();
    m_entries.clear();
    m_focusIndex = -1;
    // m_empty 是常驻子控件（全空才显示），重建时不能跟着删
    while (QLayoutItem *item = m_innerLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            if (w == m_empty)
                continue;
            w->deleteLater();
        }
        delete item;
    }
    m_innerLayout->addWidget(m_empty);
    if (m_search) {
        m_search->deleteLater();
        m_search = nullptr;
    }
}

SlashRow *SlashPanel::addRow(const QString &groupLabel, const QString &kind, const QString &id,
                             const QString &name, const QString &code, const QString &desc,
                             bool withCheck)
{
    // .slash-group：整组没命中时连标题一起藏
    QWidget *group = nullptr;
    if (!groupLabel.isEmpty()) {
        // 同一组的行共享一个 .slash-group 容器
        if (!m_entries.isEmpty() && m_entries.last().group
            && m_entries.last().group->property("groupLabel").toString() == groupLabel) {
            group = m_entries.last().group;
        } else {
            group = new QWidget(m_inner);
            setClass(group, QStringLiteral("slashGroup"));
            group->setProperty("groupLabel", groupLabel);
            auto *groupLayout = new QVBoxLayout(group);
            groupLayout->setContentsMargins(0, 0, 0, 0);
            groupLayout->setSpacing(0);
            QLabel *label = makeLabel(QStringLiteral("slashGroupLabel"), groupLabel, group);
            label->setTextInteractionFlags(Qt::NoTextInteraction);
            groupLayout->addWidget(label);
            m_innerLayout->addWidget(group);
        }
    }
    auto *row = new SlashRow(kind, id, name, code, desc, withCheck, group ? group : m_inner);
    if (group)
        group->layout()->addWidget(row);
    else
        m_innerLayout->addWidget(row);
    if (withCheck) {
        const QString current = kind == QLatin1String("model")
                                    ? property("selectedModel").toString()
                                    : property("selectedSkill").toString();
        row->setSelected(id == current);
    }
    connect(row, &SlashRow::activated, this, [this](const QString &k, const QString &rowId) {
        emit itemChosen(k, rowId);
    });
    m_rows.append(row);
    RowEntry entry;
    entry.row = row;
    entry.group = group;
    entry.kind = kind;
    entry.id = id;
    m_entries.append(entry);
    return row;
}

void SlashPanel::setRootOptions(const QVariantList &skills, const QString &selectedSkill,
                                const QString &selectedModel)
{
    m_mode = QStringLiteral("root");
    setProperty("selectedSkill", selectedSkill);
    setProperty("selectedModel", selectedModel);
    clearRows();
    // 指令在前：模型、导出这类动作比技能更常用
    addRow(QStringLiteral("指令"), QStringLiteral("command"), QStringLiteral("model"),
           QStringLiteral("模型"), QStringLiteral("model"),
           QStringLiteral("选择本次会话使用的模型"), false);
    addRow(QStringLiteral("指令"), QStringLiteral("command"), QStringLiteral("export"),
           QStringLiteral("导出对话"), QStringLiteral("export"),
           QStringLiteral("把当前会话导出为 Markdown 文件"), false);
    addRow(QStringLiteral("技能"), QStringLiteral("skill"), QString(), QStringLiteral("不启用技能"),
           QStringLiteral("—"), QStringLiteral("默认：按消息内容自动匹配技能"), false);
    for (const QVariant &v : skills) {
        const QVariantMap item = v.toMap();
        const QString id = item.value(QStringLiteral("id")).toString();
        addRow(QStringLiteral("技能"), QStringLiteral("skill"), id,
               item.value(QStringLiteral("name")).toString().isEmpty()
                   ? id
                   : item.value(QStringLiteral("name")).toString(),
               id, item.value(QStringLiteral("description")).toString(), false);
    }
    m_inner->layout()->activate();
}

void SlashPanel::setModelOptions(const QVariantList &models, const QString &selectedModel)
{
    m_mode = QStringLiteral("model");
    setProperty("selectedModel", selectedModel);
    setProperty("selectedSkill", QString());
    clearRows();
    // 模型面板顶部多一个过滤输入框，过滤词与正文无关（模型面板里正文还是 "/"）
    m_search = new QLineEdit(m_inner);
    m_search->setObjectName(QStringLiteral("slashSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索模型..."));
    m_search->setAttribute(Qt::WA_MacShowFocusRect, false);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        setQuery(text);
    });
    m_search->installEventFilter(this);
    m_innerLayout->addWidget(m_search);

    for (const QVariant &v : configuredModels(models)) {
        const QVariantMap item = v.toMap();
        const QString key = modelKey(item);
        const QString code = item.value(QStringLiteral("model_id")).toString().isEmpty()
                                 ? item.value(QStringLiteral("id")).toString()
                                 : item.value(QStringLiteral("model_id")).toString();
        const QString provider = item.value(QStringLiteral("provider_name")).toString().isEmpty()
                                     ? item.value(QStringLiteral("provider")).toString()
                                     : item.value(QStringLiteral("provider_name")).toString();
        addRow(QStringLiteral("模型"), QStringLiteral("model"), key, modelName(item),
               code.isEmpty() ? key : code, provider, true);
    }
    m_inner->layout()->activate();
}

void SlashPanel::setQuery(const QString &query)
{
    m_query = query.trimmed().toLower();
    applyFilter();
}

void SlashPanel::focusSearch()
{
    if (m_search)
        m_search->setFocus(Qt::OtherFocusReason);
}

void SlashPanel::resetToRoot()
{
    m_mode = QStringLiteral("root");
    m_query.clear();
}

void SlashPanel::applyFilter()
{
    if (m_entries.isEmpty())
        return;
    const QString previous = focusedRow() ? focusedRow()->id() : QString();
    int shown = 0;
    QSet<QWidget *> groupHits;
    for (RowEntry &entry : m_entries) {
        const bool hit = entry.row->matches(m_query);
        // 过滤后只是隐藏，不重建：删字变宽时能重新出现，也不会每敲一个字丢掉一次按键
        entry.shown = hit;
        entry.row->setVisible(hit);
        if (hit) {
            ++shown;
            if (entry.group)
                groupHits.insert(entry.group);
        }
    }
    for (const RowEntry &entry : m_entries) {
        if (entry.group)
            entry.group->setVisible(groupHits.contains(entry.group));
    }
    m_empty->setText(m_mode == QLatin1String("model") ? QStringLiteral("没有匹配的已配置模型")
                                                      : QStringLiteral("没有匹配的技能或指令"));
    m_empty->setVisible(shown == 0);
    m_inner->layout()->activate();

    const QList<SlashRow *> rows = visibleRows();
    int index = 0;
    for (int i = 0; i < rows.size(); ++i) {
        if (rows.at(i)->id() == previous) {
            index = i;
            break;
        }
    }
    focusRow(index);
}

void SlashPanel::focusRow(int index)
{
    for (int i = 0; i < m_rows.size(); ++i)
        m_rows.at(i)->setFocusedRow(false);
    m_focusIndex = index;
    SlashRow *row = focusedRow();
    if (row)
        row->setFocusedRow(true);
}

void SlashPanel::moveFocus(int delta)
{
    const QList<SlashRow *> rows = visibleRows();
    if (rows.isEmpty())
        return;
    const int current = qMax(0, rows.indexOf(focusedRow()));
    focusRow((current + delta + rows.size()) % rows.size());
}

bool SlashPanel::chooseFocused()
{
    SlashRow *row = focusedRow();
    if (!row)
        return false;
    emit itemChosen(row->kind(), row->id());
    return true;
}

SlashRow *SlashPanel::focusedRow() const
{
    for (SlashRow *row : m_rows) {
        if (row->property("focused").toBool())
            return row;
    }
    return nullptr;
}

QList<SlashRow *> SlashPanel::visibleRows() const
{
    QList<SlashRow *> out;
    for (const RowEntry &entry : m_entries) {
        if (entry.shown)
            out.append(entry.row);
    }
    return out;
}

int SlashPanel::heightForContent(int width)
{
    // 先按定稿宽度把内容重排一次：说明文字要按最终宽度省略，量到的才是真高度
    const int contentWidth = qMax(0, width - 10);
    m_inner->setFixedWidth(contentWidth);
    m_inner->layout()->activate();
    const int content = m_inner->sizeHint().height() + 10;
    // .slash-menu{max-height:264px}
    return qBound(0, content, 264);
}

bool SlashPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Down:
            moveFocus(1);
            return true;
        case Qt::Key_Up:
            moveFocus(-1);
            return true;
        case Qt::Key_Enter:
        case Qt::Key_Return:
            return chooseFocused();
        case Qt::Key_Escape:
            hide();
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void SlashPanel::mouseReleaseEvent(QMouseEvent *event)
{
    // 空白处点击不关面板（webui 同理：只有选项或外部点击才收起）
    QFrame::mouseReleaseEvent(event);
}

QRect SessionPanel::areaFor(const QSize &host)
{
    // webui .session-panel：top:126px;left/right:8px;max-height:calc(100% - 154px)
    const int width = qMax(0, host.width() - 16);
    const int maxHeight = qMax(0, host.height() - 154);
    return QRect(8, 126, width, maxHeight);
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
    m_head = head;
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
    close->setIconColors(gs::palette().text, gs::palette().cyan);
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
    m_list = list;
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
        empty->ensurePolished(); // QSS 的边距要先落地，量出的空状态高度才是最终值
        m_listLayout->addWidget(empty);
        empty->show();
        m_listLayout->addStretch(1);
        syncHostLayout();
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
        setClass(select, QStringLiteral("sessionSelect"));
        // webui 的 .session-select 带 tabindex="0"：整块可点也要能 Tab 到 + 回车触发
        select->setFocusPolicy(Qt::TabFocus);
        select->setCursor(Qt::PointingHandCursor);
        auto *selectLayout = new QVBoxLayout(select);
        selectLayout->setContentsMargins(7, 9, 7, 9);
        // webui 的 .session-text 里 strong/small 是紧邻的块，没有间隙（实测 18+16=34）
        selectLayout->setSpacing(0);
        QLabel *titleLabel = makeLabel(QStringLiteral("sessionTitle"),
                                       title.isEmpty() ? QStringLiteral("未命名会话") : title, select);
        titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
        titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        // QSS 没有 line-height：Chromium 的 normal 行盒是 1.38em（13px→18、11px→16），
        // Qt 只有 1.23em（13px→16、11px→13），这里按 webui 实测行盒补足最小值（会话行 44→53）
        titleLabel->setMinimumHeight(18);
        QString stamp = session.value(QStringLiteral("updated_at")).toString();
        if (stamp.isEmpty())
            stamp = session.value(QStringLiteral("created_at")).toString();
        stamp = stamp.left(16).replace(QLatin1Char('T'), QLatin1Char(' '));
        QLabel *meta = makeLabel(QStringLiteral("sessionMeta"), stamp, select);
        meta->setTextInteractionFlags(Qt::NoTextInteraction);
        meta->setMinimumHeight(16); // .session-select small：11px / 行盒 16

        // 状态徽标（.session-badge）：前端实时状态优先，刷新后靠服务端 active/waiting 兜底
        QString status = session.value(QStringLiteral("status")).toString();
        if (status.isEmpty()) {
            if (session.value(QStringLiteral("waiting")).toBool())
                status = QStringLiteral("waiting");
            else if (session.value(QStringLiteral("active")).toBool())
                status = QStringLiteral("running");
        }
        static const QHash<QString, QString> statusText{
            { QStringLiteral("running"), QStringLiteral("进行中") },
            { QStringLiteral("done"), QStringLiteral("已完成") },
            { QStringLiteral("stopped"), QStringLiteral("已停止") },
            { QStringLiteral("error"), QStringLiteral("异常") },
            { QStringLiteral("waiting"), QStringLiteral("待确认") },
        };
        auto *titleRow = new QWidget(select);
        auto *titleLayout = new QHBoxLayout(titleRow);
        titleLayout->setContentsMargins(0, 0, 0, 0);
        titleLayout->setSpacing(6);
        QLabel *badge = makeLabel(QStringLiteral("sessionBadge"), statusText.value(status), titleRow);
        badge->setProperty("status", status);
        badge->setTextInteractionFlags(Qt::NoTextInteraction);
        badge->setVisible(!status.isEmpty() && statusText.contains(status));
        titleLayout->addWidget(titleLabel, 1);
        titleLayout->addWidget(badge, 0);
        selectLayout->addWidget(titleRow);
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
            button->setIconColors(gs::palette().muted,
                                  def.danger ? gs::palette().red
                                             : gs::palette().cyan);
            button->setIconName(QString::fromUtf8(def.icon), 13);
            actionsLayout->addWidget(button);
            buttons.append(button);
        }

        rowLayout->addWidget(select, 1);
        rowLayout->addWidget(actions);
        // QSS（.session-badge 的 padding 等）要先落地：行刚建出来还没显示过时
        // sizeHint 偏小，面板按它收缩会把最后一行截掉
        row->ensurePolished();
        m_listLayout->addWidget(row);
        // 隐藏控件在布局里算「空项」（QWidgetItem::isEmpty），sizeHint/heightForWidth 会把它当 0：
        // 面板打开期间过滤出的新行、面板打开前 setSessions() 建的行，都要显式 show 才量得到高度
        row->show();

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
    syncHostLayout();
}

// 面板打开期间内容变换（搜索过滤、会话刷新）后要立刻跟着宿主重排：
// webui 的 max-height 只是上限，内容变少时面板必须收缩，否则行会被旧高度留在半空/被底边截断。
void SessionPanel::syncHostLayout()
{
    if (!m_open)
        return;
    if (QWidget *host = parentWidget())
        layoutIn(host->size());
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
    } else if (event->type() == QEvent::KeyPress) {
        // 键盘等价于点击（原生 button 的行为）：仅 .session-select 可聚焦
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
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
    const QRect area = areaFor(host);
    // 行刚增删完时列表布局还没跑过，先定稿再量自然高度
    if (m_listLayout)
        m_listLayout->activate();
    // webui 面板没有固定高度：内容少时收到内容高，内容多时被 max-height 截断并滚动
    const int naturalHeight = (m_head ? m_head->sizeHint().height() : 0)
                              + (m_list ? m_list->sizeHint().height() : 0);
    const int height = qMin(area.height(), qMax(sizeHint().height(), naturalHeight));
    setGeometry(area.x(), area.y(), area.width(), height);
}

// ------------------------------------------------------- ThemeListPopup

ThemeListPopup::ThemeListPopup(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("themeListbox"));
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(132);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(0);

    for (const QString &id : themeIds()) {
        auto *button = new QPushButton(this);
        setClass(button, QStringLiteral("themeOption"));
        button->setFixedHeight(30);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::TabFocus);
        auto *bl = new QHBoxLayout(button);
        bl->setContentsMargins(8, 0, 8, 0);
        bl->setSpacing(8);
        bl->addWidget(new ThemeSwatch(id, 14, true, button), 0);
        QLabel *name = makeLabel(QStringLiteral("themeOptionText"), themeName(id), button);
        name->setTextInteractionFlags(Qt::NoTextInteraction);
        bl->addWidget(name, 1);
        connect(button, &QPushButton::clicked, this, [this, id] {
            hide();
            emit themeChosen(id);
        });
        layout->addWidget(button);
        m_entries.append({ id, button });
    }
}

void ThemeListPopup::setCurrent(const QString &id)
{
    for (const Entry &entry : qAsConst(m_entries)) {
        const bool active = entry.id == id;
        entry.button->setProperty("active", active);
        restyle(entry.button);
    }
}

void ThemeListPopup::openBelow(QWidget *anchor)
{
    if (!anchor)
        return;
    // .theme-listbox{top:32px;right:0;width:132px}：28px 触发器 + 4px 间距，右缘与触发器对齐
    QWidget *host = anchor->window();
    const QRect hostRect = host ? QRect(host->mapToGlobal(QPoint(0, 0)), host->size())
                                : QRect(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
    const QPoint below = anchor->mapToGlobal(QPoint(0, anchor->height() + 4));
    const auto anchoredPos = [&](int popupHeight) {
        const int minX = hostRect.left() + 6;
        const int x = qBound(minX, below.x() + anchor->width() - width(),
                             qMax(minX, hostRect.right() - width() - 6));
        const int y = qMin(below.y(),
                           qMax(hostRect.top() + 6, hostRect.bottom() - popupHeight - 6));
        return QPoint(x, y);
    };

    adjustSize();
    // 隐藏状态下量到的高度偏小，先落位，show() 之后按定稿高度重贴一次
    const int estimated = height();
    move(anchoredPos(estimated));
    show();
    const int settled = height();
    if (settled != estimated)
        move(anchoredPos(settled));
    raise();
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
        layoutIn(parentWidget()->size(), m_bottomInset);
    show();
    raise();
    m_timer->start();
}

void Toast::layoutIn(const QSize &host, int bottomInset)
{
    m_bottomInset = bottomInset;
    const int width = qMax(0, host.width() - 20);
    const int height = heightForWidth(width);
    setGeometry(10, qMax(0, host.height() - bottomInset - height), width, height);
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
    // CSS .confirm-dialog{width:92vw;max-width:380px}：设置中心最小宽 360（<380），
    // 固定 380 会在窄宿主下溢出，这里按父窗口宽度收窄
    const int hostWidth = parent ? parent->window()->width() : 0;
    setFixedWidth(hostWidth > 0 ? qMin(380, qRound(hostWidth * 0.92)) : 380);

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

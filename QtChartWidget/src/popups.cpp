#include "popups.h"

#include "commonwidgets.h"
#include "theme.h"

#include <QApplication>
#include <QDesktopWidget>
#include <QEvent>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMap>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QFile>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSizePolicy>
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

void ListBoxPopup::setPlainOptions(const QVariantList &rows, const QString &selectedValue,
                                   int preferredWidth)
{
    // 与 setModelOptions 同一个 ListBoxPopup 换脸：.model-listbox 默认 330px 上限
    m_preferredWidth = preferredWidth;
    setClass(this, QStringLiteral("listbox"));
    reset();
    for (const QVariant &v : rows) {
        const QVariantMap row = v.toMap();
        const QString value = row.value(QStringLiteral("value")).toString();
        const QString name = row.value(QStringLiteral("name")).toString();
        addRow(value, name.isEmpty() ? value : name, row.value(QStringLiteral("sub")).toString(),
               value == selectedValue, row.value(QStringLiteral("tooltip")).toString());
    }
    if (m_rows.isEmpty()) {
        QLabel *empty = makeLabel(QStringLiteral("listboxEmpty"), QStringLiteral("没有可选项"),
                                  m_inner);
        empty->setAlignment(Qt::AlignCenter);
        m_innerLayout->addWidget(empty);
    }
    m_focusIndex = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i)->value() == selectedValue) {
            m_focusIndex = i;
            break;
        }
    }
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

// ------------------------------------------------------------ 上下文用量圆环 / 浮层

namespace {

// app.js formatTokens：紧凑 token 数，按十进制量级切档（1,000,000 → 1M）。
// 拿 1024 进制去切会把 1M 窗口显示成 976.6K，所以这里跟 webui 一致：
//   value >= 1e6 → Math.round(value/100000)/10 + "M"
//   value >= 1e3 → Math.round(value/100)/10 + "K"
// 也就是「保留一位小数」，小数为 0 时 JS 的字符串化不带 .0，这里同样去掉。
QString formatContextTokens(double value)
{
    const auto plain = [](double v) {
        const int tenths = qRound(v * 10.0);
        if (tenths % 10 == 0)
            return QString::number(tenths / 10);
        return QString::number(tenths / 10.0, 'f', 1);
    };
    if (value >= 1000000.0)
        return plain(value / 1000000.0) + QLatin1Char('M');
    if (value >= 1000.0)
        return plain(value / 1000.0) + QLatin1Char('K');
    return plain(value);
}

// renderContextRing 的取数口径：used 与 window 都拿得到才能算比率，
// percent 只在缺 max 时兜底（宿主算过了就用它）。
struct RingState
{
    bool hasData = false;
    qreal ratio = -1.0;
    int percent = -1;
    double window = 0.0;
    double used = 0.0;
    double left = 0.0;
    bool estimated = false;
};

RingState readRingState(const QVariantMap &usage)
{
    RingState state;
    state.estimated = usage.value(QStringLiteral("estimated")).toBool();
    // max 是宿主的主字段名，window 是 webui 事件里的叫法，两个都认
    double window = usage.value(QStringLiteral("max")).toDouble();
    if (window <= 0.0)
        window = usage.value(QStringLiteral("window")).toDouble();
    const double used = usage.value(QStringLiteral("used")).toDouble();
    const double leftRaw = usage.value(QStringLiteral("left")).toDouble();
    const QVariant pct = usage.value(QStringLiteral("percent"));
    const bool hasPercent = pct.isValid() && !pct.isNull();

    state.window = window > 0.0 ? window : 0.0;
    state.used = used > 0.0 ? used : 0.0;
    if (state.window > 0.0 && state.used > 0.0) {
        state.ratio = qBound(0.0, state.used / state.window, 1.0);
        state.percent = qRound(state.ratio * 100.0);
        state.hasData = true;
    } else if (state.used > 0.0 && hasPercent) {
        // 窗口上限未知但宿主给了百分比：弧按百分比画，上限那行显示「—」
        const qreal p = qBound(0.0, pct.toDouble(), 100.0);
        state.ratio = p / 100.0;
        state.percent = qRound(p);
        state.hasData = true;
    } else if (state.window > 0.0) {
        // 只知道上限：空环 + 上限数值（webui 的空态同样是 dashoffset 满圈）
        state.ratio = 0.0;
        state.percent = 0;
        state.hasData = true;
    }
    if (state.hasData) {
        state.left = leftRaw > 0.0 ? leftRaw : qMax(0.0, state.window - state.used);
    }
    return state;
}

} // namespace

// ------------------------------------------------------------ ContextBar

ContextBar::ContextBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("contextBar"));
    setFixedHeight(qMax(3, gs::scaledPx(4)));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void ContextBar::setPercent(int percent)
{
    const int clamped = qBound(0, percent, 100);
    if (m_percent == clamped)
        return;
    m_percent = clamped;
    update();
}

QSize ContextBar::sizeHint() const
{
    return QSize(gs::scaledPx(80), qMax(3, gs::scaledPx(4)));
}

QSize ContextBar::minimumSizeHint() const
{
    return QSize(gs::scaledPx(40), qMax(3, gs::scaledPx(4)));
}

void ContextBar::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    const gs::Palette &pal = gs::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal radius = height() / 2.0;
    painter.setPen(Qt::NoPen);
    painter.setBrush(pal.surface3);
    painter.drawRoundedRect(QRectF(rect()), radius, radius);
    if (m_percent <= 0)
        return;
    const qreal filled = width() * m_percent / 100.0;
    if (filled <= 0.5)
        return;
    painter.setBrush(pal.cyan);
    painter.drawRoundedRect(QRectF(0, 0, filled, height()), radius, radius);
}

// ------------------------------------------------------------ ContextRing

int ContextRing::ringBoxPx()
{
    // webui：<svg width=22 height=22> 放在 30×30 的 .context-ring 里
    return 22;
}

ContextRing::ContextRing(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("contextRing"));
    // 与控件行里其它控件同高（30px），环面（22px）居中
    setFixedSize(30, 30);
    // webui 是 <div tabindex=0>：点击/悬停都该能看到浮层，所以给 StrongFocus
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setToolTip(QStringLiteral("上下文用量"));
}

void ContextRing::setUsage(const QVariantMap &usage)
{
    m_usage = usage;
    const RingState state = readRingState(usage);
    m_ratio = state.hasData ? state.ratio : -1.0;
    m_percent = state.hasData ? state.percent : -1;
    update();
}

void ContextRing::clearUsage()
{
    m_usage.clear();
    m_ratio = -1.0;
    m_percent = -1;
    update();
}

void ContextRing::enterEvent(QEvent *event)
{
    m_hover = true;
    update();
    QWidget::enterEvent(event);
}

void ContextRing::leaveEvent(QEvent *event)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(event);
}

void ContextRing::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    const gs::Palette &pal = gs::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // .context-ring:hover / :focus-visible { border-color:line-strong; background:surface-2 }
    if (m_hover || hasFocus()) {
        painter.setPen(QPen(pal.lineStrong, 1));
        painter.setBrush(pal.surface2);
        const qreal radius = gs::scaledPx(4);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }

    // SVG viewBox 0 0 36 36 等比缩到 22×22：r=15.5、stroke-width=4
    const qreal box = qMin<qreal>(ringBoxPx(), qMin(width(), height()));
    const qreal scale = box / 36.0;
    const qreal radius = 15.5 * scale;
    const qreal stroke = qMax<qreal>(1.0, 4.0 * scale);
    const QPointF center((width() - box) / 2.0 + box / 2.0, (height() - box) / 2.0 + box / 2.0);
    const QRectF circle(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0);

    // 底环 .context-ring-track{stroke:var(--line)}；空态整体弱化（.unknown 的 opacity:.55）
    QColor trackColor = pal.line;
    if (m_ratio < 0.0) {
        trackColor = pal.muted2;
        trackColor.setAlphaF(0.55);
    }
    QPen trackPen(trackColor);
    trackPen.setWidthF(stroke);
    trackPen.setCapStyle(Qt::FlatCap);
    painter.setPen(trackPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(circle);

    // 空态不画弧（webui 把 stroke-dashoffset 设满一整圈，弧就看不见了）
    if (m_ratio < 0.0)
        return;
    QColor arcColor = pal.cyan;
    if (m_ratio >= 0.9)
        arcColor = pal.red; // .context-ring.danger
    else if (m_ratio >= 0.7)
        arcColor = pal.orange; // .context-ring.warn
    QPen arcPen(arcColor);
    arcPen.setWidthF(stroke);
    arcPen.setCapStyle(Qt::RoundCap);
    painter.setPen(arcPen);
    // rotate(-90 18 18) 让 0° 落在 12 点方向；Qt 的正角是逆时针，所以跨度取负
    const int startAngle = 90 * 16;
    const int spanAngle = -qRound(m_ratio * 360.0 * 16.0);
    if (spanAngle != 0)
        painter.drawArc(circle, startAngle, spanAngle);
}

// ------------------------------------------------------------ ContextPop

ContextPop::ContextPop(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("contextPop"));
    setAttribute(Qt::WA_StyledBackground, true);
    // .context-pop{pointer-events:none}：浮层不吃鼠标，否则一碰就丢 hover
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);

    auto *outer = new QVBoxLayout(this);
    // .context-pop{padding:10px 12px}
    outer->setContentsMargins(gs::scaledPx(12), gs::scaledPx(10), gs::scaledPx(12),
                              gs::scaledPx(10));
    outer->setSpacing(0);

    // 标题行：上下文用量 + 百分比（.pop-title，下边框一条线）
    auto *head = new QWidget(this);
    head->setObjectName(QStringLiteral("contextPopHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, gs::scaledPx(7));
    headLayout->setSpacing(6);
    QLabel *title = makeLabel(QStringLiteral("popTitle"), QString::fromUtf8("上下文用量"), head);
    title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_pct = makeLabel(QStringLiteral("popTotal"), QString::fromUtf8("—"), head);
    m_pct->setTextInteractionFlags(Qt::NoTextInteraction);
    m_pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    headLayout->addWidget(title, 1);
    headLayout->addWidget(m_pct, 0);
    outer->addWidget(head);
    outer->addSpacing(gs::scaledPx(6));

    // 三行「窗口上限 / 已用 / 剩余」（.pop-row）
    const auto addRow = [&](const QString &label, QLabel **valueOut) {
        auto *row = new QWidget(this);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, gs::scaledPx(3), 0, gs::scaledPx(3));
        rowLayout->setSpacing(gs::scaledPx(18));
        QLabel *name = makeLabel(QStringLiteral("popLabel"), label, row);
        name->setTextInteractionFlags(Qt::NoTextInteraction);
        QLabel *value = makeLabel(QStringLiteral("popValue"), QString::fromUtf8("—"), row);
        value->setTextInteractionFlags(Qt::NoTextInteraction);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rowLayout->addWidget(name, 1);
        rowLayout->addWidget(value, 0);
        outer->addWidget(row);
        *valueOut = value;
    };
    addRow(QString::fromUtf8("窗口上限"), &m_max);
    addRow(QString::fromUtf8("已用"), &m_used);
    addRow(QString::fromUtf8("剩余"), &m_left);

    m_bar = new ContextBar(this);
    outer->addSpacing(gs::scaledPx(7));
    outer->addWidget(m_bar);

    // .pop-note{"本地估算"}：只有 estimated 为真才露出来
    m_noteGap = new QWidget(this);
    m_noteGap->setFixedHeight(gs::scaledPx(6));
    m_noteGap->setVisible(false);
    outer->addWidget(m_noteGap);
    m_note = makeLabel(QStringLiteral("popNote"), QString::fromUtf8("本地估算"), this);
    m_note->setTextInteractionFlags(Qt::NoTextInteraction);
    m_note->setVisible(false);
    outer->addWidget(m_note);

    applyTheme();
}

void ContextPop::applyTheme()
{
    const gs::Palette &pal = gs::palette();
    // webui .context-pop{min-width:216px}
    setMinimumWidth(gs::scaledPx(216));

    // 注意：这个浮层挂在 Composer 的**宿主**上（是 Composer 的兄弟，不是后代），
    // 而自己又带一份局部表，会盖住祖先的全局 QSS。早先的写法是把整张局部 QSS 重建，
    // 但只要换肤那次没能调到 applyTheme，浮层就会长期停在旧配色。
    // 这里改成：背景自绘（paintEvent 读 palette），文字直接写进控件调色板与字号，
    // 不依赖任何选择器匹配。
    const QList<QLabel *> labels = findChildren<QLabel *>();
    for (QLabel *label : labels) {
        const QString cls = label->property("class").toString();
        QColor color = pal.text;
        int px = 11;
        bool bold = false;
        bool mono = false;
        if (cls.contains(QLatin1String("popTitle"))) {
            color = pal.text;
            bold = true;
        } else if (cls.contains(QLatin1String("popTotal"))) {
            color = pal.text;
            mono = true;
        } else if (cls.contains(QLatin1String("popLabel"))) {
            color = pal.muted;
        } else if (cls.contains(QLatin1String("popValue"))) {
            color = pal.textBright;
            mono = true;
        } else if (cls.contains(QLatin1String("popNote"))) {
            color = pal.muted2;
            px = 9;
        }
        QFont font = label->font();
        font.setPixelSize(gs::scaledPx(px));
        font.setBold(bold);
        if (mono)
            font.setFamily(gs::monoFont());
        label->setFont(font);
        QPalette p = label->palette();
        p.setColor(QPalette::WindowText, color);
        p.setColor(QPalette::Text, color);
        label->setPalette(p);
    }
    update();
}

void ContextPop::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    const gs::Palette &pal = gs::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(pal.line, 1));
    painter.setBrush(pal.band2);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

void ContextPop::setUsage(const QVariantMap &usage)
{
    const RingState state = readRingState(usage);
    if (!state.hasData) {
        m_pct->setText(QString::fromUtf8("—"));
        m_max->setText(state.window > 0.0 ? formatContextTokens(state.window)
                                          : QString::fromUtf8("—"));
        m_used->setText(QString::fromUtf8("—"));
        m_left->setText(QString::fromUtf8("—"));
        m_bar->setPercent(0);
        m_note->setVisible(false);
        m_noteGap->setVisible(false);
        return;
    }
    m_pct->setText(QStringLiteral("%1%").arg(state.percent));
    m_max->setText(state.window > 0.0 ? formatContextTokens(state.window)
                                      : QString::fromUtf8("—"));
    m_used->setText(formatContextTokens(state.used));
    m_left->setText(formatContextTokens(state.left));
    m_bar->setPercent(state.percent);
    m_note->setVisible(state.estimated);
    m_noteGap->setVisible(state.estimated);
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
            // deleteLater 要等回到事件循环才真删：重建发生在面板开着的时候，
            // 先藏起来，否则旧行会在这一帧里与新行叠在一起
            w->hide();
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

void SlashPanel::setSkillCategory(const QString &category)
{
    m_skillCategory = category;
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

    // webui slashGroups()：分类决定技能怎么摆
    //   全部  → 技能按各自 category 分组，组名首字母大写（缺省「通用」），组名排序；
    //           「不启用技能」这一项不出现
    //   具体分类 → 只留 category == 分类 或 common 的技能，整组标题用分类名，
    //           组内第一项是固定的「不启用技能」
    const auto groupName = [](const QString &raw) {
        const QString trimmed = raw.trimmed();
        const QString value = trimmed.isEmpty() ? QStringLiteral("通用") : trimmed;
        return value.left(1).toUpper() + value.mid(1);
    };
    const auto skillName = [](const QVariantMap &item, const QString &id) {
        const QString name = item.value(QStringLiteral("name")).toString();
        return name.isEmpty() ? id : name;
    };
    const QString category = m_skillCategory.trimmed();
    if (category.isEmpty()) {
        // QMap 按 key 升序，与 webui 的 Object.keys(byCat).sort() 同序
        QMap<QString, QVariantList> groups;
        for (const QVariant &v : skills) {
            const QVariantMap item = v.toMap();
            groups[groupName(item.value(QStringLiteral("category")).toString())].append(item);
        }
        for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
            for (const QVariant &v : it.value()) {
                const QVariantMap item = v.toMap();
                const QString id = item.value(QStringLiteral("id")).toString();
                addRow(it.key(), QStringLiteral("skill"), id, skillName(item, id),
                       id.isEmpty() ? QStringLiteral("—") : id,
                       item.value(QStringLiteral("description")).toString(), false);
            }
        }
    } else {
        const QString label = groupName(category);
        const QString wanted = category.toLower();
        addRow(label, QStringLiteral("skill"), QString(), QStringLiteral("不启用技能"),
               QStringLiteral("—"), QStringLiteral("默认：按消息内容自动匹配技能"), false);
        for (const QVariant &v : skills) {
            const QVariantMap item = v.toMap();
            const QString id = item.value(QStringLiteral("id")).toString();
            // 空 id 就是上面那一行「不启用技能」，不要再来一遍
            if (id.isEmpty())
                continue;
            const QString itemCategory =
                item.value(QStringLiteral("category")).toString().trimmed().toLower();
            if (itemCategory != wanted && itemCategory != QLatin1String("common"))
                continue;
            addRow(label, QStringLiteral("skill"), id, skillName(item, id), id,
                   item.value(QStringLiteral("description")).toString(), false);
        }
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
    // 保留上一焦点：只有真有 id 的行才值得眼（空 id 是「不启用技能」这类项，
    // 不能把它当成「上次选中的行」把初始焦点吸过去）
    if (!previous.isEmpty()) {
        for (int i = 0; i < rows.size(); ++i) {
            if (rows.at(i)->id() == previous) {
                index = i;
                break;
            }
        }
    }
    focusRow(index);
}

void SlashPanel::focusRow(int index)
{
    for (int i = 0; i < m_rows.size(); ++i)
        m_rows.at(i)->setFocusedRow(false);
    // index 是「可见行」的下标（applyFilter / moveFocus 都按 visibleRows 算），
    // 不能直接拿去索引 m_rows，否则叠上过滤词就对不上了
    const QList<SlashRow *> rows = visibleRows();
    if (rows.isEmpty()) {
        m_focusIndex = -1;
        return;
    }
    m_focusIndex = qBound(0, index, rows.size() - 1);
    rows.at(m_focusIndex)->setFocusedRow(true);
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

#include "usagepanel.h"

#include "commonwidgets.h"
#include "popups.h"
#include "theme.h"
#include "usagecharts.h"

#include <QApplication>
#include <QAbstractButton>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QtMath>

#include <algorithm>

namespace gs {

namespace {

struct Preset
{
    const char *id;
    const char *label;
    int hours;
    int days;
};

// 结果缓存的条目上限（与 app.js 的 USAGE_CACHE_LIMIT 同口径）
constexpr int kUsageCacheLimit = 32;

// USAGE_PRESETS
const Preset kPresets[] = {
    { "12h", "最近 12 小时", 12, 0 },
    { "24h", "最近 24 小时", 24, 0 },
    { "3d", "最近 3 天", 0, 3 },
    { "7d", "最近 7 天", 0, 7 },
    { "30d", "最近 30 天", 0, 30 },
};

const Preset &presetById(const QString &id)
{
    for (const Preset &preset : kPresets) {
        if (id == QLatin1String(preset.id))
            return preset;
    }
    return kPresets[3];
}

QString pad2(int value)
{
    return value < 10 ? QStringLiteral("0%1").arg(value) : QString::number(value);
}

// app.js usageIsoLocal
QString isoLocal(const QDateTime &date)
{
    return date.toString(QStringLiteral("yyyy-MM-ddTHH:mm"));
}

QString dayKey(const QDateTime &date)
{
    return date.toString(QStringLiteral("yyyy-MM-dd"));
}

QLabel *plainLabel(const QString &cls, const QString &text, QWidget *parent = nullptr)
{
    QLabel *label = makeLabel(cls, text, parent);
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    return label;
}

// .usage-card 的头部：eyebrow + 标题 + 右侧提示
QWidget *cardHead(const QString &eyebrow, const QString &title, QLabel **titleOut,
                  const QString &hint, QWidget *parent)
{
    auto *head = new QWidget(parent);
    auto *layout = new QHBoxLayout(head);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    auto *box = new QVBoxLayout;
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(2);
    box->addWidget(plainLabel(QStringLiteral("eyebrow"), eyebrow, head));
    QLabel *titleLabel = plainLabel(QStringLiteral("usageCardTitle"), title, head);
    box->addWidget(titleLabel);
    layout->addLayout(box, 1);
    if (!hint.isEmpty())
        layout->addWidget(plainLabel(QStringLiteral("usageCardHint"), hint, head), 0, Qt::AlignVCenter);
    if (titleOut)
        *titleOut = titleLabel;
    return head;
}

QFrame *cardFrame(QWidget *parent)
{
    auto *card = new QFrame(parent);
    setClass(card, QStringLiteral("usageCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setFrameShape(QFrame::NoFrame);
    return card;
}

// 浮层和既有的模型 / 皮肤下拉一样，必须以 ChartWidget 为父窗口创建：
// 以设置对话框为父、事后再重挂，Qt 会把样式重置回原生样式（QSS 就不生效了）。
// 面板在滚动区里，anchor->window() 是设置对话框，再往上一层就是 ChartWidget。
static QWidget *overlayOwner(QWidget *anchor, QWidget *fallback)
{
    QWidget *window = anchor ? anchor->window() : nullptr;
    QWidget *owner = window ? window->parentWidget() : nullptr;
    return owner ? owner : fallback;
}

// 纵轴 / 图例等自绘提示文案（与 webui usage-card-head em 同文）
const char *const kZoomHint = "滚轮缩放 · 拖拽平移 · 双击复位 · Shift + 滚轮缩放纵轴";

} // namespace

// ------------------------------------------------------------ UsageSelect

UsageSelect::UsageSelect(bool withCalendar, QWidget *parent)
    : QWidget(parent), m_withCalendar(withCalendar)
{
    setClass(this, QStringLiteral("usageSelect"));
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setFixedHeight(30);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 0, 10, 0);
    layout->setSpacing(7);
    if (m_withCalendar) {
        m_calendar = new QLabel(this);
        setClass(m_calendar, QStringLiteral("usageSelectIcon"));
        m_calendar->setFixedSize(13, 13);
        layout->addWidget(m_calendar, 0, Qt::AlignVCenter);
    }
    m_label = plainLabel(QStringLiteral("usageSelectText"), QString(), this);
    layout->addWidget(m_label, 0, Qt::AlignVCenter);
    m_chevron = new QLabel(this);
    setClass(m_chevron, QStringLiteral("usageSelectIcon"));
    m_chevron->setFixedSize(12, 12);
    layout->addWidget(m_chevron, 0, Qt::AlignVCenter);
    refreshIcons();
}

void UsageSelect::refreshIcons()
{
    if (m_calendar)
        m_calendar->setPixmap(iconPixmap(QStringLiteral("clock"), gs::palette().muted2, 13));
    if (m_chevron)
        m_chevron->setPixmap(iconPixmap(QStringLiteral("chevron-down"), gs::palette().muted2, 12));
}

void UsageSelect::setText(const QString &text)
{
    m_full = text;
    if (m_label)
        m_label->setText(text);
}

void UsageSelect::setOpen(bool open)
{
    setProperty("open", open);
    restyle(this);
}

void UsageSelect::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos()))
        emit clicked();
    QWidget::mouseReleaseEvent(event);
}

void UsageSelect::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
        || event->key() == Qt::Key_Space) {
        emit clicked();
        return;
    }
    QWidget::keyPressEvent(event);
}

// ------------------------------------------------------------ UsageListPopup

UsageListPopup::UsageListPopup(QWidget *parent) : QFrame(parent)
{
    setClass(this, QStringLiteral("usageListbox"));
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(5, 5, 5, 5);
    outer->setSpacing(0);
    // .usage-listbox{max-height:280px;overflow:auto}：选项多了滚动而不是撑高
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("usageListScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    m_inner = new QWidget(scroll);
    m_inner->setObjectName(QStringLiteral("usageListInner"));
    m_layout = new QVBoxLayout(m_inner);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    scroll->setWidget(m_inner);
    outer->addWidget(scroll);
    // 与既有模型 / 皮肤下拉一致：不自己铺样式表，直接继承父窗口（ChartWidget）的样式表
}

void UsageListPopup::setItems(const QVariantList &items, const QString &current)
{
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    for (const QVariant &v : items) {
        const QVariantMap data = v.toMap();
        const QString value = data.value(QStringLiteral("value")).toString();
        auto *button = new QPushButton(m_inner);
        setClass(button, QStringLiteral("usageOption"));
        button->setProperty("selected", value == current);
        button->setEnabled(!data.value(QStringLiteral("disabled")).toBool());
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::TabFocus);
        auto *layout = new QHBoxLayout(button);
        layout->setContentsMargins(8, 7, 8, 7);
        layout->setSpacing(8);
        // .usage-listbox button>span：单行省略，长模型名不撑破浮层（min-width:140/max-width:340）
        auto *label = new ElidedLabel(button);
        setClass(label, QStringLiteral("usageOptionLabel"));
        label->setFullText(data.value(QStringLiteral("label")).toString());
        layout->addWidget(label, 1);
        const QString note = data.value(QStringLiteral("note")).toString();
        if (!note.isEmpty())
            layout->addWidget(plainLabel(QStringLiteral("usageOptionNote"), note, button), 0);
        connect(button, &QPushButton::clicked, this, [this, value] {
            hide();
            emit chosen(value);
        });
        m_layout->addWidget(button);
    }
    adjustSize();
    setFixedWidth(qBound(140, sizeHint().width(), 340));
    setMaximumHeight(280);
}

void UsageListPopup::openBelow(QWidget *anchor)
{
    if (!anchor)
        return;
    adjustSize();
    const int width = qBound(140, sizeHint().width(), 340);
    setFixedWidth(width);
    const int height = qMin(sizeHint().height(), 280);
    const QPoint below = anchor->mapToGlobal(QPoint(0, anchor->height() + 5));
    int x = below.x();
    int y = below.y();
    if (QWidget *host = anchor->window()) {
        const QRect frame = host->geometry();
        x = qBound(frame.left() + 6, x, frame.right() - width - 6);
        y = qMin(y, frame.bottom() - height - 6);
    }
    setGeometry(x, y, width, height);
    show();
    raise();
}

void UsageListPopup::hideEvent(QHideEvent *event)
{
    emit closed();
    QFrame::hideEvent(event);
}

// ------------------------------------------------------------ UsageRangePopup

UsageRangePopup::UsageRangePopup(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("usageRangePopup"));
    setClass(this, QStringLiteral("usageRangePanel"));
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(14);

    // DOM 顺序是日历在前、预设在右（.usage-range-panel{display:flex;gap:14px}）
    auto *calendar = new QWidget(this);
    auto *calendarLayout = new QVBoxLayout(calendar);
    calendarLayout->setContentsMargins(0, 0, 0, 0);
    calendarLayout->setSpacing(8);
    auto *nav = new QWidget(calendar);
    auto *navLayout = new QHBoxLayout(nav);
    navLayout->setContentsMargins(0, 0, 0, 0);
    navLayout->setSpacing(8);
    m_title = plainLabel(QStringLiteral("usageCalTitle"), QString(), nav);
    navLayout->addWidget(m_title, 1);
    auto *steps = new QWidget(nav);
    auto *stepsLayout = new QHBoxLayout(steps);
    stepsLayout->setContentsMargins(0, 0, 0, 0);
    stepsLayout->setSpacing(4);
    const struct { const char *text; int delta; } stepsDef[] = { { "\xc2\xab", -12 },
                                                                 { "\xe2\x80\xb9", -1 },
                                                                 { "\xe2\x80\xba", 1 },
                                                                 { "\xc2\xbb", 12 } };
    for (const auto &step : stepsDef) {
        auto *button = new QPushButton(QString::fromUtf8(step.text), steps);
        setClass(button, QStringLiteral("usageCalStep"));
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::TabFocus);
        const int delta = step.delta;
        connect(button, &QPushButton::clicked, this, [this, delta] {
            m_anchor = m_anchor.addMonths(delta);
            rebuildCalendar();
        });
        stepsLayout->addWidget(button);
    }
    navLayout->addWidget(steps, 0);
    calendarLayout->addWidget(nav);
    m_monthHost = new QWidget(calendar);
    calendarLayout->addWidget(m_monthHost);
    layout->addWidget(calendar, 1);

    m_presetBox = new QWidget(this);
    m_presetBox->setFixedWidth(112);
    auto *presetLayout = new QVBoxLayout(m_presetBox);
    presetLayout->setContentsMargins(0, 0, 0, 0);
    presetLayout->setSpacing(6);
    layout->addWidget(m_presetBox, 0, Qt::AlignTop);
    // 同下拉浮层：不自己铺样式表，直接继承父窗口（ChartWidget）的样式表
}

void UsageRangePopup::rebuildPresets()
{
    auto *layout = qobject_cast<QVBoxLayout *>(m_presetBox->layout());
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    for (const Preset &preset : kPresets) {
        auto *button = new QPushButton(QString::fromUtf8(preset.label), m_presetBox);
        setClass(button, QStringLiteral("usageRangePreset"));
        button->setProperty("active", !m_start.isValid() && m_preset == QLatin1String(preset.id));
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::TabFocus);
        const QString id = QString::fromLatin1(preset.id);
        connect(button, &QPushButton::clicked, this, [this, id] {
            hide();
            emit presetChosen(id);
        });
        layout->addWidget(button);
    }
    m_confirm = new QPushButton(QStringLiteral("确定"), m_presetBox);
    setClass(m_confirm, QStringLiteral("usageCalConfirm"));
    m_confirm->setCursor(Qt::PointingHandCursor);
    m_confirm->setFocusPolicy(Qt::TabFocus);
    m_confirm->setEnabled(m_pendingStart.isValid());
    connect(m_confirm, &QPushButton::clicked, this, [this] {
        if (!m_pendingStart.isValid())
            return;
        const QDate end = m_pendingEnd.isValid() ? m_pendingEnd : m_pendingStart;
        hide();
        emit customChosen(m_pendingStart, end);
    });
    layout->addStretch(1); // .usage-cal-confirm{margin-top:auto}：确定钮压到预设列底部
    layout->addWidget(m_confirm);
}

void UsageRangePopup::rebuildCalendar()
{
    if (m_confirm)
        m_confirm->setEnabled(m_pendingStart.isValid());
    if (!m_title)
        return;
    const QDate second = m_anchor.addMonths(1);
    m_title->setText(QStringLiteral("%1-%2 ~ %3-%4")
                         .arg(m_anchor.year())
                         .arg(pad2(m_anchor.month()))
                         .arg(second.year())
                         .arg(pad2(second.month())));

    auto *hostLayout = qobject_cast<QHBoxLayout *>(m_monthHost->layout());
    if (!hostLayout) {
        hostLayout = new QHBoxLayout(m_monthHost);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        hostLayout->setSpacing(16);
    }
    while (QLayoutItem *item = hostLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    // 已选区间：优先用日历上的待定期，其次用当前区间
    const QDate start = m_pendingStart.isValid()
                            ? m_pendingStart
                            : (m_start.isValid() ? m_start.date() : QDate());
    const QDate end = m_pendingStart.isValid()
                          ? (m_pendingEnd.isValid() ? m_pendingEnd : m_pendingStart)
                          : (m_end.isValid() ? m_end.date() : QDate());

    for (int offset = 0; offset < 2; ++offset) {
        const QDate month = m_anchor.addMonths(offset);
        auto *grid = new QWidget(m_monthHost);
        setClass(grid, QStringLiteral("usageCalGrid"));
        grid->setFixedWidth(196);
        auto *gridLayout = new QVBoxLayout(grid);
        gridLayout->setContentsMargins(0, 0, 0, 0);
        gridLayout->setSpacing(0);
        QLabel *monthLabel =
            plainLabel(QStringLiteral("usageCalMonth"),
                       QStringLiteral("%1-%2").arg(month.year()).arg(pad2(month.month())), grid);
        monthLabel->setAlignment(Qt::AlignCenter); // .usage-cal-month{text-align:center}
        gridLayout->addWidget(monthLabel);
        auto *week = new QWidget(grid);
        setClass(week, QStringLiteral("usageCalWeek"));
        auto *weekLayout = new QHBoxLayout(week);
        weekLayout->setContentsMargins(0, 0, 0, 0);
        weekLayout->setSpacing(0);
        const QStringList names{ QStringLiteral("一"), QStringLiteral("二"), QStringLiteral("三"),
                                 QStringLiteral("四"), QStringLiteral("五"), QStringLiteral("六"),
                                 QStringLiteral("日") };
        for (const QString &name : names) {
            QLabel *cell = plainLabel(QStringLiteral("usageCalWeekCell"), name, week);
            cell->setAlignment(Qt::AlignCenter);
            cell->setFixedHeight(20); // .usage-cal-week span{height:20px}
            weekLayout->addWidget(cell, 1);
        }
        gridLayout->addWidget(week);
        auto *cells = new QWidget(grid);
        auto *cellsLayout = new QGridLayout(cells);
        cellsLayout->setContentsMargins(0, 0, 0, 0);
        cellsLayout->setSpacing(0);
        const QDate firstOfMonth(month.year(), month.month(), 1);
        const int offsetDays = (firstOfMonth.dayOfWeek() + 6) % 7; // 周一开头
        const QDate cursor = firstOfMonth.addDays(-offsetDays);
        for (int index = 0; index < 42; ++index) {
            const QDate day = cursor.addDays(index);
            auto *button = new QPushButton(QString::number(day.day()), cells);
            setClass(button, QStringLiteral("usageCalDay"));
            button->setProperty("other", day.month() != month.month());
            QString state;
            if (start.isValid() && end.isValid()) {
                if (day == start && day == end)
                    state = QStringLiteral("single");
                else if (day == start)
                    state = QStringLiteral("start");
                else if (day == end)
                    state = QStringLiteral("end");
                else if (day > start && day < end)
                    state = QStringLiteral("in");
            }
            button->setProperty("state", state);
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::TabFocus);
            button->setFixedHeight(26);
            connect(button, &QPushButton::clicked, this, [this, day] { pickDay(day); });
            cellsLayout->addWidget(button, index / 7, index % 7);
        }
        gridLayout->addWidget(cells);
        hostLayout->addWidget(grid, 0, Qt::AlignTop);
    }
    adjustSize();
}

void UsageRangePopup::pickDay(const QDate &day)
{
    if (!m_pendingStart.isValid() || m_pendingEnd.isValid()) {
        m_pendingStart = day;
        m_pendingEnd = QDate();
        rebuildCalendar();
        return;
    }
    m_pendingEnd = day;
    hide();
    emit customChosen(m_pendingStart <= day ? m_pendingStart : day,
                      m_pendingStart <= day ? day : m_pendingStart);
}

void UsageRangePopup::setState(const QString &preset, const QDateTime &start, const QDateTime &end)
{
    m_preset = preset;
    m_start = start;
    m_end = end;
    m_pendingStart = QDate();
    m_pendingEnd = QDate();
    const QDate anchor = end.isValid() ? end.date()
                                       : (start.isValid() ? start.date() : QDate::currentDate());
    m_anchor = QDate(anchor.year(), anchor.month(), 1);
    rebuildPresets();
    rebuildCalendar();
}

void UsageRangePopup::openBelowRight(QWidget *filtersRow)
{
    if (!filtersRow)
        return;
    adjustSize();
    const QPoint below = filtersRow->mapToGlobal(QPoint(0, filtersRow->height() + 5));
    int x = below.x() + filtersRow->width() - width();
    int y = below.y();
    if (QWidget *host = filtersRow->window()) {
        const QRect frame = host->geometry();
        x = qBound(frame.left() + 6, x, frame.right() - width() - 6);
        y = qMin(y, frame.bottom() - height() - 6);
    }
    move(x, y);
    show();
    raise();
}

void UsageRangePopup::hideEvent(QHideEvent *event)
{
    emit closed();
    QFrame::hideEvent(event);
}

// ------------------------------------------------------------ UsageStat

UsageStat::UsageStat(const QString &caption, QWidget *parent) : QFrame(parent)
{
    setClass(this, QStringLiteral("usageStat"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFrameShape(QFrame::NoFrame);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 9, 10, 9);
    layout->setSpacing(4);
    m_caption = plainLabel(QStringLiteral("usageStatLabel"), caption, this);
    m_value = plainLabel(QStringLiteral("usageStatValue"), QString(), this);
    m_value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(m_caption);
    layout->addWidget(m_value);
}

void UsageStat::setValue(const QString &value)
{
    m_value->setText(value);
}

// ------------------------------------------------------------ UsagePanel

UsagePanel::UsagePanel(QWidget *parent) : QWidget(parent)
{
    setClass(this, QStringLiteral("usagePanel"));
    buildUi();
    // 默认筛选文案先落位（对应 index.html 里各 span 的初始文本），数据到达前也不空白
    renderFilters();
}

void UsagePanel::buildUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(18, 16, 18, 16);
    m_rootLayout->setSpacing(12);
    m_rootLayout->addWidget(buildFilters());
    m_status = plainLabel(QStringLiteral("usageStatus"), QString(), this);
    m_rootLayout->addWidget(m_status);
    m_rootLayout->addWidget(buildOverview());
    m_rootLayout->addWidget(buildTrendCard());

    // .usage-grid：宽屏两栏，≤900 收成一栏（饼图在上、柱状在下）
    m_gridRow = new QWidget(this);
    m_gridLayout = new QGridLayout(m_gridRow);
    m_gridLayout->setContentsMargins(0, 0, 0, 0);
    m_gridLayout->setSpacing(12);
    m_gridLayout->addWidget(buildPieCard(), 0, 0);
    m_gridLayout->addWidget(buildBarCard(), 0, 1);
    m_rootLayout->addWidget(m_gridRow);

    m_rootLayout->addWidget(buildTableCard());
    m_rootLayout->addStretch(1);

    m_live = new QLabel(this);
    setClass(m_live, QStringLiteral("usageLive"));
    m_live->setFixedSize(1, 1);
    m_live->setVisible(false);

    applyResponsive();
}

// 浮层在首次展开时才创建：必须以 ChartWidget 为父窗口。若先以设置对话框为父、
// 之后再 setParent 重挂，Qt 会把样式重置回原生样式，QSS 就不再生效了
// （既有模型 / 皮肤下拉都是「创建时就是 ChartWidget 的子窗口」，因此有样式）。
void UsagePanel::ensurePopups(QWidget *anchor)
{
    if (m_listPopup)
        return;
    QWidget *owner = overlayOwner(anchor, this);
    m_listPopup = new UsageListPopup(owner);
    connect(m_listPopup, &UsageListPopup::chosen, this, [this](const QString &value) {
        if (m_listRole == QLatin1String("group")) {
            m_groupBy = value;
            closePopups();
            render();
        } else if (m_listRole == QLatin1String("provider")) {
            pickProvider(value);
        } else if (m_listRole == QLatin1String("model")) {
            pickModel(value);
        } else if (m_listRole == QLatin1String("granularity")) {
            m_granularity = value;
            m_hasView = false;
            closePopups();
            render();
        }
    });
    connect(m_listPopup, &UsageListPopup::closed, this, [this] {
        m_popupClosedAt = QDateTime::currentMSecsSinceEpoch();
        if (m_groupSelect)
            m_groupSelect->setOpen(false);
        if (m_providerSelect)
            m_providerSelect->setOpen(false);
        if (m_modelSelect)
            m_modelSelect->setOpen(false);
        if (m_granularitySelect)
            m_granularitySelect->setOpen(false);
        if (m_rangeSelect)
            m_rangeSelect->setOpen(false);
    });

    m_rangePopup = new UsageRangePopup(owner);
    connect(m_rangePopup, &UsageRangePopup::presetChosen, this, [this](const QString &id) {
        m_preset = id;
        m_customStart = QDateTime();
        m_customEnd = QDateTime();
        // 12/24 小时档在按天下只剩一两个点，强制切到按小时
        if (presetById(id).hours)
            m_granularity = QStringLiteral("hour");
        m_hasView = false;
        closePopups();
        loadStats();
    });
    connect(m_rangePopup, &UsageRangePopup::customChosen, this,
            [this](const QDate &start, const QDate &end) {
                if (start.isValid() && end.isValid())
                    applyCustom(start, end);
            });
    connect(m_rangePopup, &UsageRangePopup::closed, this, [this] {
        m_popupClosedAt = QDateTime::currentMSecsSinceEpoch();
        if (m_rangeSelect)
            m_rangeSelect->setOpen(false);
    });
}

void UsagePanel::hideEvent(QHideEvent *event)
{
    // 浮层挂在 ChartWidget 上，对话框收起时要跟着收
    closePopups();
    QWidget::hideEvent(event);
}

// style.css 的媒体查询落在窗口宽度上（900px / 640px），这里同样看顶层窗口
void UsagePanel::applyResponsive()
{
    const QWidget *host = window();
    const int windowWidth = host ? host->width() : this->width();
    if (windowWidth <= 0)
        return; // 还没进窗口，等首次 resizeEvent
    const int mode = windowWidth <= 640 ? 2 : (windowWidth <= 900 ? 1 : 0);
    if (mode == m_layoutMode)
        return;
    m_layoutMode = mode;
    layoutOverviewCards(mode == 2 ? 2 : (mode == 1 ? 3 : 6));
    layoutChartCards(mode != 0);
    // ≤640：.usage-panel{padding:12px}、.usage-filters{gap:5px}、.usage-filter-label{display:none}
    if (m_rootLayout)
        m_rootLayout->setContentsMargins(mode == 2 ? 12 : 18, mode == 2 ? 12 : 16,
                                        mode == 2 ? 12 : 18, mode == 2 ? 12 : 16);
    if (m_filterFlow)
        m_filterFlow->setSpacings(mode == 2 ? 5 : 8, mode == 2 ? 5 : 8);
    for (QLabel *label : m_filterLabels)
        label->setVisible(mode != 2);
}

void UsagePanel::layoutOverviewCards(int columns)
{
    if (!m_overviewLayout || columns <= 0)
        return;
    while (QLayoutItem *item = m_overviewLayout->takeAt(0))
        delete item; // 只摘布局项，卡片控件留着复排
    for (int index = 0; index < m_stats.size(); ++index)
        m_overviewLayout->addWidget(m_stats.at(index), index / columns, index % columns);
    for (int column = 0; column < 6; ++column)
        m_overviewLayout->setColumnStretch(column, column < columns ? 1 : 0);
}

void UsagePanel::layoutChartCards(bool stacked)
{
    if (!m_gridLayout || !m_pie || !m_bar)
        return;
    QWidget *pieCard = m_pie->parentWidget();
    QWidget *barCard = m_bar->parentWidget();
    if (!pieCard || !barCard)
        return;
    while (QLayoutItem *item = m_gridLayout->takeAt(0))
        delete item;
    m_gridLayout->addWidget(pieCard, 0, 0);
    m_gridLayout->addWidget(barCard, stacked ? 1 : 0, stacked ? 0 : 1);
    m_gridLayout->setColumnStretch(0, 1);
    m_gridLayout->setColumnStretch(1, stacked ? 0 : 1);
}

void UsagePanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyResponsive();
}

QWidget *UsagePanel::buildFilters()
{
    m_filtersRow = new QWidget(this);
    setClass(m_filtersRow, QStringLiteral("usageFilters"));
    m_filterFlow = new FlowLayout(m_filtersRow, 0, 8, 8);
    m_filterFlow->setContentsMargins(0, 0, 0, 0);

    auto addFilter = [this](const QString &caption, UsageSelect *select) {
        auto *box = new QWidget(m_filtersRow);
        setClass(box, QStringLiteral("usageFilter"));
        auto *layout = new QHBoxLayout(box);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        QLabel *label = plainLabel(QStringLiteral("usageFilterLabel"), caption, box);
        m_filterLabels.append(label);
        layout->addWidget(label);
        layout->addWidget(select);
        m_filterFlow->addWidget(box);
    };

    m_groupSelect = new UsageSelect(false, m_filtersRow);
    m_providerSelect = new UsageSelect(false, m_filtersRow);
    m_modelSelect = new UsageSelect(false, m_filtersRow);
    m_granularitySelect = new UsageSelect(false, m_filtersRow);
    m_rangeSelect = new UsageSelect(true, m_filtersRow);
    m_groupSelect->setObjectName(QStringLiteral("usageGroup"));
    m_providerSelect->setObjectName(QStringLiteral("usageProvider"));
    m_modelSelect->setObjectName(QStringLiteral("usageModel"));
    m_granularitySelect->setObjectName(QStringLiteral("usageGranularity"));
    m_rangeSelect->setObjectName(QStringLiteral("usageRange"));
    addFilter(QStringLiteral("分组"), m_groupSelect);
    addFilter(QStringLiteral("供应商"), m_providerSelect);
    addFilter(QStringLiteral("模型"), m_modelSelect);
    addFilter(QStringLiteral("粒度"), m_granularitySelect);
    addFilter(QStringLiteral("日期"), m_rangeSelect);

    auto *refresh = new QPushButton(QStringLiteral("刷新"), m_filtersRow);
    setClass(refresh, QStringLiteral("usageRefresh"));
    refresh->setProperty("variant", QStringLiteral("secondary"));
    refresh->setCursor(Qt::PointingHandCursor);
    refresh->setFocusPolicy(Qt::TabFocus);
    connect(refresh, &QPushButton::clicked, this, [this] { loadStats(true); });
    m_filterFlow->addWidget(refresh);

    // 点击已展开的触发器要能收起（app.js toggleUsagePopover）
    auto toggleOff = [this](QWidget *trigger) {
        if (m_activeTrigger == trigger
            && QDateTime::currentMSecsSinceEpoch() - m_popupClosedAt < 300) {
            closePopups();
            return true;
        }
        return false;
    };

    connect(m_groupSelect, &UsageSelect::clicked, this, [this, toggleOff] {
        if (toggleOff(m_groupSelect))
            return;
        ensurePopups(m_groupSelect);
        m_listRole = QStringLiteral("group");
        QVariantList items;
        items << QVariantMap{ { QStringLiteral("value"), QStringLiteral("model") },
                              { QStringLiteral("label"), QStringLiteral("模型用量") } }
              << QVariantMap{ { QStringLiteral("value"), QStringLiteral("provider") },
                              { QStringLiteral("label"), QStringLiteral("供应商用量") } };
        m_listPopup->setItems(items, m_groupBy);
        m_listPopup->openBelow(m_groupSelect);
        m_groupSelect->setOpen(true);
        m_activeTrigger = m_groupSelect;
    });
    connect(m_providerSelect, &UsageSelect::clicked, this, [this, toggleOff] {
        if (toggleOff(m_providerSelect))
            return;
        ensurePopups(m_providerSelect);
        m_listRole = QStringLiteral("provider");
        QVariantList items;
        items << QVariantMap{ { QStringLiteral("value"), QString() },
                              { QStringLiteral("label"), QStringLiteral("全部供应商") } };
        for (const QVariant &v : m_providerOptions) {
            const QVariantMap row = v.toMap();
            items << QVariantMap{ { QStringLiteral("value"), row.value(QStringLiteral("value")) },
                                  { QStringLiteral("label"), row.value(QStringLiteral("label")) } };
        }
        m_listPopup->setItems(items, m_provider);
        m_listPopup->openBelow(m_providerSelect);
        m_providerSelect->setOpen(true);
        m_activeTrigger = m_providerSelect;
    });
    connect(m_modelSelect, &UsageSelect::clicked, this, [this, toggleOff] {
        if (toggleOff(m_modelSelect))
            return;
        ensurePopups(m_modelSelect);
        m_listRole = QStringLiteral("model");
        QVariantList items;
        items << QVariantMap{ { QStringLiteral("value"), QString() },
                              { QStringLiteral("label"), QStringLiteral("全部模型") } };
        for (const QVariant &v : m_modelOptions) {
            const QVariantMap row = v.toMap();
            items << QVariantMap{ { QStringLiteral("value"), row.value(QStringLiteral("value")) },
                                  { QStringLiteral("label"), row.value(QStringLiteral("label")) },
                                  { QStringLiteral("note"), row.value(QStringLiteral("note")) } };
        }
        m_listPopup->setItems(items, m_model);
        m_listPopup->openBelow(m_modelSelect);
        m_modelSelect->setOpen(true);
        m_activeTrigger = m_modelSelect;
    });
    connect(m_granularitySelect, &UsageSelect::clicked, this, [this, toggleOff] {
        if (toggleOff(m_granularitySelect))
            return;
        ensurePopups(m_granularitySelect);
        m_listRole = QStringLiteral("granularity");
        const bool hourDisabled = m_data.isEmpty()
                                  || m_data.value(QStringLiteral("range")).toMap()
                                         .value(QStringLiteral("resolution")).toString()
                                         == QLatin1String("day");
        QVariantList items;
        items << QVariantMap{ { QStringLiteral("value"), QStringLiteral("day") },
                              { QStringLiteral("label"), QStringLiteral("按天") } };
        QVariantMap hour{ { QStringLiteral("value"), QStringLiteral("hour") },
                          { QStringLiteral("label"), QStringLiteral("按小时") } };
        if (hourDisabled) {
            hour.insert(QStringLiteral("disabled"), true);
            hour.insert(QStringLiteral("note"), QStringLiteral("当前范围仅提供按天"));
        }
        items << hour;
        m_listPopup->setItems(items, m_granularity);
        m_listPopup->openBelow(m_granularitySelect);
        m_granularitySelect->setOpen(true);
        m_activeTrigger = m_granularitySelect;
    });
    connect(m_rangeSelect, &UsageSelect::clicked, this, [this, toggleOff] {
        if (toggleOff(m_rangeSelect))
            return;
        ensurePopups(m_rangeSelect);
        m_listPopup->hide();
        m_rangePopup->setState(m_preset, m_customStart, m_customEnd);
        m_rangePopup->openBelowRight(m_filtersRow);
        m_rangeSelect->setOpen(true);
        m_activeTrigger = m_rangeSelect;
    });

    return m_filtersRow;
}

QWidget *UsagePanel::buildOverview()
{
    m_overview = new QWidget(this);
    m_overview->setObjectName(QStringLiteral("usageOverview"));
    setClass(m_overview, QStringLiteral("usageOverview"));
    m_overviewLayout = new QGridLayout(m_overview);
    m_overviewLayout->setContentsMargins(0, 0, 0, 0);
    m_overviewLayout->setSpacing(8);
    const QStringList captions{ QStringLiteral("总用量"), QStringLiteral("输入"),
                                QStringLiteral("输出"), QStringLiteral("实测 / 估算"),
                                QStringLiteral("缓存命中率"), QStringLiteral("会话数") };
    m_stats.clear();
    for (const QString &caption : captions)
        m_stats.append(new UsageStat(caption, m_overview));
    layoutOverviewCards(6);
    return m_overview;
}

QWidget *UsagePanel::buildTrendCard()
{
    QFrame *card = cardFrame(this);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    layout->addWidget(cardHead(QStringLiteral("TREND"), QStringLiteral("输入 / 输出趋势"), nullptr,
                               QString::fromUtf8(kZoomHint), card));
    m_line = new UsageTimeChart(UsageTimeChart::Line, card);
    m_line->setObjectName(QStringLiteral("usageLine"));
    connect(m_line, &UsageTimeChart::viewRequested, this,
            [this](bool hasView, qint64 start, qint64 end) { setView(hasView, start, end); });
    connect(m_line, &UsageTimeChart::hovered, this, [this](const QVariantMap &row) {
        announceChartHover(row);
    });
    layout->addWidget(m_line);
    return card;
}

QWidget *UsagePanel::buildPieCard()
{
    QFrame *card = cardFrame(this);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    layout->addWidget(cardHead(QStringLiteral("SHARE"), QStringLiteral("模型占比"), &m_pieTitle,
                               QString(), card));
    m_pie = new UsagePieChart(card);
    m_pie->setObjectName(QStringLiteral("usagePie"));
    connect(m_pie, &UsagePieChart::hovered, this, [this](const QVariantMap &group) {
        if (group.isEmpty())
            m_live->setText(QString());
        else
            m_live->setText(QStringLiteral("%1 %2")
                                .arg(group.value(QStringLiteral("label")).toString())
                                .arg(formatMillions(group.value(QStringLiteral("value")).toLongLong())));
    });
    layout->addWidget(m_pie);
    return card;
}

QWidget *UsagePanel::buildBarCard()
{
    QFrame *card = cardFrame(this);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    layout->addWidget(cardHead(QStringLiteral("MEASURED"), QStringLiteral("实测 vs 估算"), nullptr,
                               QString(), card));
    m_bar = new UsageTimeChart(UsageTimeChart::Bar, card);
    m_bar->setObjectName(QStringLiteral("usageBar"));
    connect(m_bar, &UsageTimeChart::viewRequested, this,
            [this](bool hasView, qint64 start, qint64 end) { setView(hasView, start, end); });
    connect(m_bar, &UsageTimeChart::hovered, this, [this](const QVariantMap &row) {
        announceChartHover(row);
    });
    layout->addWidget(m_bar);
    return card;
}

QWidget *UsagePanel::buildTableCard()
{
    QFrame *card = cardFrame(this);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    layout->addWidget(cardHead(QStringLiteral("DETAIL"), QStringLiteral("模型明细"), &m_tableTitle,
                               QString(), card));
    m_table = new QTableWidget(card);
    m_table->setObjectName(QStringLiteral("usageTable"));
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionsClickable(true);
    m_table->horizontalHeader()->setSortIndicatorShown(true);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->viewport()->setAutoFillBackground(false);
    m_table->setCornerButtonEnabled(false);
    // QTableView 自带的角落按钮不是交互元素，但「全树按钮可 Tab」的契约仍要满足
    for (QAbstractButton *button : m_table->findChildren<QAbstractButton *>())
        button->setFocusPolicy(Qt::TabFocus);
    m_table->verticalHeader()->setDefaultSectionSize(28);
    m_table->setMaximumHeight(260);
    connect(m_table->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int section) {
        const QList<Column> cols = columns();
        if (section < 0 || section >= cols.size())
            return;
        const Column &column = cols.at(section);
        if (m_sortKey == column.key)
            m_sortDesc = !m_sortDesc;
        else {
            m_sortKey = column.key;
            m_sortDesc = column.numeric;
        }
        renderTable();
    });
    layout->addWidget(m_table);
    return card;
}

void UsagePanel::announceChartHover(const QVariantMap &row)
{
    if (!m_live)
        return;
    if (row.isEmpty()) {
        m_live->setText(QString());
        return;
    }
    m_live->setText(QStringLiteral("%1 用量：总 %2，输入 %3，输出 %4")
                        .arg(bucketTitle(row.value(QStringLiteral("t")).toString()))
                        .arg(formatMillions(row.value(QStringLiteral("total")).toLongLong()))
                        .arg(formatMillions(row.value(QStringLiteral("input")).toLongLong()))
                        .arg(formatMillions(row.value(QStringLiteral("output")).toLongLong())));
}

// ------------------------------------------------------------ 数据流

void UsagePanel::setCatalogModels(const QVariantList &models)
{
    m_models = models;
    if (m_hasData)
        render();
}

void UsagePanel::enterTab()
{
    if (m_loaded)
        render();
    else
        loadStats();
}

void UsagePanel::setStats(const QString &requestId, const QVariantMap &data)
{
    QString key;
    // 晚到的旧响应不能写进当前视图（app.js token）：requestId 对不上就丢弃
    if (!takePendingResponse(requestId, key))
        return;
    if (!key.isEmpty()) {
        // 与 app.js 一致：条目到上限就整体清空，长时间切换筛选不会无限累积
        if (m_cache.size() >= kUsageCacheLimit)
            m_cache.clear();
        m_cache.insert(key, data);
    }
    adoptData(data);
}

void UsagePanel::setLoadFailed(const QString &requestId, const QString &error)
{
    QString key;
    if (!takePendingResponse(requestId, key))
        return;
    m_loading = false;
    m_error = error;
    render();
}

bool UsagePanel::takePendingResponse(const QString &requestId, QString &cacheKey)
{
    if (requestId.isEmpty() || requestId != m_pendingRequestId)
        return false;
    cacheKey = m_pendingKey;
    invalidatePendingResponse();
    return true;
}

void UsagePanel::invalidatePendingResponse()
{
    m_pendingRequestId.clear();
    m_pendingKey.clear();
}

void UsagePanel::adoptData(const QVariantMap &data)
{
    m_data = data;
    m_hasData = true;
    m_loaded = true;
    m_loading = false;
    m_error.clear();
    // 跨度过大时服务端只给按天桶，粒度档位跟着回落
    const QString resolution = data.value(QStringLiteral("range")).toMap()
                                   .value(QStringLiteral("resolution")).toString();
    if (resolution == QLatin1String("day"))
        m_granularity = QStringLiteral("day");
    render();
}

void UsagePanel::windowRange(QDateTime &start, QDateTime &end) const
{
    if (m_customStart.isValid() && m_customEnd.isValid()) {
        start = m_customStart;
        end = m_customEnd;
        return;
    }
    const Preset &preset = presetById(m_preset);
    const QDateTime now = QDateTime::currentDateTime();
    if (preset.hours > 0) {
        start = now.addSecs(-3600LL * preset.hours);
    } else {
        start = QDateTime(now.date().addDays(-(preset.days - 1)), QTime(0, 0));
    }
    end = now;
}

QString UsagePanel::rangeText() const
{
    if (m_customStart.isValid() && m_customEnd.isValid()) {
        const QString start = dayKey(m_customStart);
        const QString end = dayKey(m_customEnd);
        return start == end ? start : QStringLiteral("%1 ~ %2").arg(start, end);
    }
    return QString::fromUtf8(presetById(m_preset).label);
}

bool UsagePanel::drilled() const
{
    const QVariantMap range = m_data.value(QStringLiteral("range")).toMap();
    if (m_data.isEmpty() || range.value(QStringLiteral("resolution")).toString() != QLatin1String("hour"))
        return false;
    if (m_granularity == QLatin1String("hour"))
        return false;
    if (!m_hasView)
        return false;
    return (m_viewEnd - m_viewStart) < 3 * 86400000LL;
}

QVariantList UsagePanel::rollupDays() const
{
    QVariantList out;
    QHash<QString, int> indexOf;
    const QVariantList buckets = m_data.value(QStringLiteral("buckets")).toList();
    const QStringList fields{ QStringLiteral("input"), QStringLiteral("output"),
                              QStringLiteral("total"), QStringLiteral("measured"),
                              QStringLiteral("estimated"), QStringLiteral("turns") };
    for (const QVariant &v : buckets) {
        const QVariantMap row = v.toMap();
        const QString key = row.value(QStringLiteral("t")).toString().left(10);
        if (!indexOf.contains(key)) {
            indexOf.insert(key, out.size());
            QVariantMap target;
            target.insert(QStringLiteral("t"), key);
            for (const QString &field : fields)
                target.insert(field, qint64(0));
            out.append(target);
        }
        QVariantMap target = out.at(indexOf.value(key)).toMap();
        for (const QString &field : fields)
            target.insert(field, target.value(field).toLongLong() + row.value(field).toLongLong());
        out[indexOf.value(key)] = target;
    }
    return out;
}

QVariantList UsagePanel::chartBuckets() const
{
    if (m_data.isEmpty())
        return QVariantList();
    const QString resolution = m_data.value(QStringLiteral("range")).toMap()
                                   .value(QStringLiteral("resolution")).toString();
    if (resolution == QLatin1String("day"))
        return m_data.value(QStringLiteral("buckets")).toList();
    if (m_granularity == QLatin1String("hour") || drilled())
        return m_data.value(QStringLiteral("buckets")).toList();
    return rollupDays();
}

void UsagePanel::domainAndView(qint64 &domainStart, qint64 &domainEnd, bool &hasView,
                               qint64 &viewStart, qint64 &viewEnd) const
{
    const QVariantMap range = m_data.value(QStringLiteral("range")).toMap();
    // 右端补满当前桶，保证日/小时两种粒度共用同一区间（下钻时视窗才不会跳）
    domainStart = parseBucketMs(range.value(QStringLiteral("start")).toString());
    domainEnd = parseBucketMs(range.value(QStringLiteral("end")).toString()) + 3600000;
    hasView = m_hasView;
    viewStart = m_viewStart;
    viewEnd = m_viewEnd;
}

void UsagePanel::syncCatalog()
{
    // 配置目录只用来把历史记录里的键映射成设置页里显示的同一个名字
    m_catalog.clear();
    m_configuredProviders.clear();
    for (const QVariant &v : m_models) {
        const QVariantMap item = v.toMap();
        const QVariant enabled = item.value(QStringLiteral("enabled"));
        if (enabled.isValid() && !enabled.toBool())
            continue;
        const QString key = modelKey(item);
        const QString provider = item.value(QStringLiteral("provider")).toString();
        const QString providerNameValue = item.value(QStringLiteral("provider_name")).toString().isEmpty()
                                              ? provider
                                              : item.value(QStringLiteral("provider_name")).toString();
        QVariantMap entry;
        // gs::modelName(QVariantMap)：name / display_name / model_id / id / key 依次回退
        entry.insert(QStringLiteral("name"), gs::modelName(item));
        entry.insert(QStringLiteral("provider"), provider);
        entry.insert(QStringLiteral("providerName"), providerNameValue);
        m_catalog.insert(key, entry);
        if (!m_configuredProviders.contains(provider))
            m_configuredProviders.insert(provider, providerNameValue);
    }

    // 下拉选项只列「有调用记录」的模型与供应商：candidates 按时间窗算出，不随筛选缩水
    const QVariantMap candidates = m_data.value(QStringLiteral("candidates")).toMap();
    m_providerOptions.clear();
    for (const QVariant &v : candidates.value(QStringLiteral("providers")).toList()) {
        const QVariantMap row = v.toMap();
        QVariantMap item;
        item.insert(QStringLiteral("value"), row.value(QStringLiteral("provider")));
        item.insert(QStringLiteral("label"),
                    providerName(row.value(QStringLiteral("provider")).toString(),
                                 row.value(QStringLiteral("label")).toString()));
        m_providerOptions.append(item);
    }
    m_modelOptions.clear();
    for (const QVariant &v : candidates.value(QStringLiteral("models")).toList()) {
        const QVariantMap row = v.toMap();
        if (!m_provider.isEmpty() && row.value(QStringLiteral("provider")).toString() != m_provider)
            continue;
        QVariantMap item;
        item.insert(QStringLiteral("value"), row.value(QStringLiteral("model")));
        item.insert(QStringLiteral("label"),
                    modelName(row.value(QStringLiteral("model")).toString(),
                              row.value(QStringLiteral("label")).toString()));
        item.insert(QStringLiteral("note"),
                    m_provider.isEmpty()
                        ? providerName(row.value(QStringLiteral("provider")).toString())
                        : QString());
        m_modelOptions.append(item);
    }

    // 当前时间窗内没有记录的选中项要原样留着，静默清空会让下拉和表格各说各话
    bool hasModel = false;
    for (const QVariant &v : m_modelOptions) {
        if (v.toMap().value(QStringLiteral("value")).toString() == m_model)
            hasModel = true;
    }
    if (!m_model.isEmpty() && !hasModel) {
        QVariantMap item;
        item.insert(QStringLiteral("value"), m_model);
        item.insert(QStringLiteral("label"), modelName(m_model));
        m_modelOptions.append(item);
    }
    bool hasProvider = false;
    for (const QVariant &v : m_providerOptions) {
        if (v.toMap().value(QStringLiteral("value")).toString() == m_provider)
            hasProvider = true;
    }
    if (!m_provider.isEmpty() && !hasProvider) {
        QVariantMap item;
        item.insert(QStringLiteral("value"), m_provider);
        item.insert(QStringLiteral("label"), providerName(m_provider));
        m_providerOptions.append(item);
    }
}

QString UsagePanel::modelName(const QString &key, const QString &fallback) const
{
    const QVariantMap entry = m_catalog.value(key);
    if (!entry.isEmpty())
        return entry.value(QStringLiteral("name")).toString();
    const QString base = fallback.isEmpty() ? key : fallback;
    return QStringLiteral("%1（未在配置中）").arg(base);
}

QString UsagePanel::providerName(const QString &id, const QString &fallback) const
{
    if (m_configuredProviders.contains(id))
        return m_configuredProviders.value(id);
    // 与后端 UNKNOWN_PROVIDER 对应：早期记录里 usage.model 不含供应商前缀
    if (id == QLatin1String("unknown"))
        return QStringLiteral("未知供应商");
    return fallback.isEmpty() ? (id.isEmpty() ? QStringLiteral("未知供应商") : id) : fallback;
}

// ------------------------------------------------------------ 渲染

void UsagePanel::render()
{
    syncCatalog();
    renderFilters();
    if (m_data.isEmpty() && m_error.isEmpty()) {
        m_status->setText(QStringLiteral("正在统计用量…"));
        return;
    }
    if (!m_error.isEmpty()) {
        m_status->setText(QStringLiteral("用量统计失败：%1").arg(m_error));
    } else {
        const QVariantMap range = m_data.value(QStringLiteral("range")).toMap();
        QString text = QStringLiteral("%1 至 %2 · 可用最细粒度 %3")
                           .arg(range.value(QStringLiteral("start")).toString().replace(QLatin1Char('T'), QLatin1Char(' ')),
                                range.value(QStringLiteral("end")).toString().replace(QLatin1Char('T'), QLatin1Char(' ')),
                                range.value(QStringLiteral("resolution")).toString() == QLatin1String("hour")
                                    ? QStringLiteral("按小时")
                                    : QStringLiteral("按天"));
        if (m_loading)
            text += QStringLiteral(" · 正在刷新…");
        m_status->setText(text);
    }
    renderOverview();
    renderCharts();
    renderPie();
    renderTable();
}

void UsagePanel::renderFilters()
{
    m_groupSelect->setText(m_groupBy == QLatin1String("provider")
                               ? QStringLiteral("供应商用量")
                               : QStringLiteral("模型用量"));
    m_providerSelect->setText(m_provider.isEmpty() ? QStringLiteral("全部供应商")
                                                   : providerName(m_provider));
    QString picked;
    for (const QVariant &v : m_modelOptions) {
        if (v.toMap().value(QStringLiteral("value")).toString() == m_model)
            picked = v.toMap().value(QStringLiteral("label")).toString();
    }
    m_modelSelect->setText(m_model.isEmpty()
                               ? QStringLiteral("全部模型")
                               : (picked.isEmpty() ? modelName(m_model) : picked));
    m_granularitySelect->setText(m_granularity == QLatin1String("hour") ? QStringLiteral("按小时")
                                                                        : QStringLiteral("按天"));
    m_rangeSelect->setText(rangeText());
}

void UsagePanel::renderOverview()
{
    const QVariantMap totals = m_data.value(QStringLiteral("totals")).toMap();
    const qint64 cacheRead = totals.value(QStringLiteral("cache_read")).toLongLong();
    const qint64 input = totals.value(QStringLiteral("input")).toLongLong();
    const qint64 base = cacheRead + input;
    const QString hitRate = base > 0
                                ? QStringLiteral("%1%").arg(qRound(double(cacheRead) / double(base) * 100))
                                : QStringLiteral("—");
    const QStringList values{
        formatMillions(totals.value(QStringLiteral("total")).toLongLong()),
        formatMillions(input),
        formatMillions(totals.value(QStringLiteral("output")).toLongLong()),
        QStringLiteral("%1 / %2")
            .arg(formatMillions(totals.value(QStringLiteral("measured")).toLongLong()),
                 formatMillions(totals.value(QStringLiteral("estimated")).toLongLong())),
        hitRate,
        QString::number(totals.value(QStringLiteral("sessions")).toLongLong()),
    };
    for (int i = 0; i < m_stats.size() && i < values.size(); ++i)
        m_stats.at(i)->setValue(values.at(i));
}

void UsagePanel::renderCharts()
{
    if (m_data.isEmpty()) {
        m_line->clearData();
        m_bar->clearData();
        return;
    }
    qint64 domainStart = 0, domainEnd = 0, viewStart = 0, viewEnd = 0;
    bool hasView = false;
    domainAndView(domainStart, domainEnd, hasView, viewStart, viewEnd);
    const QVariantList buckets = chartBuckets();
    m_line->setData(buckets, domainStart, domainEnd, hasView, viewStart, viewEnd);
    m_bar->setData(buckets, domainStart, domainEnd, hasView, viewStart, viewEnd);
}

bool UsagePanel::pieGroups(QVariantList &groups, QString &unit) const
{
    unit = QStringLiteral("总量");
    if (m_groupBy == QLatin1String("provider")) {
        for (const QVariant &v : m_data.value(QStringLiteral("providers")).toList()) {
            const QVariantMap row = v.toMap();
            QVariantMap item;
            item.insert(QStringLiteral("label"),
                        providerName(row.value(QStringLiteral("provider")).toString(),
                                     row.value(QStringLiteral("label")).toString()));
            item.insert(QStringLiteral("value"), row.value(QStringLiteral("total")));
            groups.append(item);
        }
        return !groups.isEmpty();
    }
    if (!m_model.isEmpty()) {
        // 选定单个模型后仍按模型切分只剩一个满圆，改为看这个模型的 token 构成
        QVariantMap found;
        for (const QVariant &v : m_data.value(QStringLiteral("models")).toList()) {
            if (v.toMap().value(QStringLiteral("model")).toString() == m_model) {
                found = v.toMap();
                break;
            }
        }
        if (found.isEmpty())
            return false;
        const QList<QPair<QString, QString>> parts{
            { QStringLiteral("输入"), QStringLiteral("input") },
            { QStringLiteral("输出"), QStringLiteral("output") },
            { QStringLiteral("缓存读"), QStringLiteral("cache_read") },
            { QStringLiteral("缓存写"), QStringLiteral("cache_write") },
        };
        for (const auto &part : parts) {
            const qint64 value = found.value(part.second).toLongLong();
            if (value <= 0)
                continue;
            QVariantMap item;
            item.insert(QStringLiteral("label"), part.first);
            item.insert(QStringLiteral("value"), value);
            groups.append(item);
        }
        return !groups.isEmpty();
    }
    const QVariantList models = m_data.value(QStringLiteral("models")).toList();
    qint64 rest = 0;
    int restCount = 0;
    for (int i = 0; i < models.size(); ++i) {
        const QVariantMap row = models.at(i).toMap();
        if (i < 6) {
            QVariantMap item;
            item.insert(QStringLiteral("label"),
                        modelName(row.value(QStringLiteral("model")).toString(),
                                  row.value(QStringLiteral("label")).toString()));
            item.insert(QStringLiteral("value"), row.value(QStringLiteral("total")));
            groups.append(item);
        } else {
            rest += row.value(QStringLiteral("total")).toLongLong();
            ++restCount;
        }
    }
    if (restCount > 0) {
        QVariantMap item;
        item.insert(QStringLiteral("label"), QStringLiteral("其他 %1 个").arg(restCount));
        item.insert(QStringLiteral("value"), rest);
        groups.append(item);
    }
    return !groups.isEmpty();
}

void UsagePanel::renderPie()
{
    QString picked;
    for (const QVariant &v : m_modelOptions) {
        if (v.toMap().value(QStringLiteral("value")).toString() == m_model)
            picked = v.toMap().value(QStringLiteral("label")).toString();
    }
    if (m_pieTitle) {
        m_pieTitle->setText(m_groupBy == QLatin1String("provider")
                                ? QStringLiteral("供应商占比")
                                : (m_model.isEmpty()
                                       ? QStringLiteral("模型占比")
                                       : QStringLiteral("%1 构成")
                                             .arg(picked.isEmpty() ? modelName(m_model) : picked)));
    }
    QVariantList groups;
    QString unit;
    if (m_data.isEmpty() || !pieGroups(groups, unit))
        m_pie->clearData();
    else
        m_pie->setGroups(groups, unit);
}

QList<UsagePanel::Column> UsagePanel::columns() const
{
    QList<Column> out;
    const bool provider = m_groupBy == QLatin1String("provider");
    out << Column{ QStringLiteral("name"),
                   provider ? QStringLiteral("供应商") : QStringLiteral("模型"), false, false };
    if (!provider)
        out << Column{ QStringLiteral("provider"), QStringLiteral("供应商"), false, false };
    out << Column{ QStringLiteral("total"), QStringLiteral("总量"), true, true }
        << Column{ QStringLiteral("input"), QStringLiteral("输入"), true, true }
        << Column{ QStringLiteral("output"), QStringLiteral("输出"), true, true }
        << Column{ QStringLiteral("measured"), QStringLiteral("实测"), true, true }
        << Column{ QStringLiteral("estimated"), QStringLiteral("估算"), true, true }
        << Column{ QStringLiteral("turns"), QStringLiteral("轮次"), true, false };
    return out;
}

QVariantList UsagePanel::tableRows(bool provider) const
{
    QVariantList out;
    const QStringList fields{ QStringLiteral("total"), QStringLiteral("input"),
                              QStringLiteral("output"), QStringLiteral("measured"),
                              QStringLiteral("estimated"), QStringLiteral("turns") };
    if (provider) {
        for (const QVariant &v : m_data.value(QStringLiteral("providers")).toList()) {
            const QVariantMap row = v.toMap();
            QVariantMap item;
            item.insert(QStringLiteral("name"),
                        providerName(row.value(QStringLiteral("provider")).toString(),
                                     row.value(QStringLiteral("label")).toString()));
            item.insert(QStringLiteral("fullKey"), row.value(QStringLiteral("provider")));
            for (const QString &field : fields)
                item.insert(field, row.value(field));
            out.append(item);
        }
        return out;
    }
    for (const QVariant &v : m_data.value(QStringLiteral("models")).toList()) {
        const QVariantMap row = v.toMap();
        QVariantMap item;
        item.insert(QStringLiteral("name"),
                    modelName(row.value(QStringLiteral("model")).toString(),
                              row.value(QStringLiteral("label")).toString()));
        item.insert(QStringLiteral("fullKey"), row.value(QStringLiteral("model")));
        item.insert(QStringLiteral("provider"),
                    providerName(row.value(QStringLiteral("provider")).toString()));
        for (const QString &field : fields)
            item.insert(field, row.value(field));
        out.append(item);
    }
    return out;
}

void UsagePanel::renderTable()
{
    if (!m_table)
        return;
    const bool provider = m_groupBy == QLatin1String("provider");
    if (m_tableTitle)
        m_tableTitle->setText(provider ? QStringLiteral("供应商明细") : QStringLiteral("模型明细"));

    const QList<Column> cols = columns();
    QVariantList rows = m_data.isEmpty() ? QVariantList() : tableRows(provider);

    // 默认按总量降序；点击表头切换升降序
    for (const Column &column : cols) {
        if (column.key != m_sortKey)
            continue;
        const bool numeric = column.numeric;
        const QString key = column.key;
        const bool desc = m_sortDesc;
        std::stable_sort(rows.begin(), rows.end(), [numeric, key, desc](const QVariant &left,
                                                                        const QVariant &right) {
            const QVariantMap a = left.toMap();
            const QVariantMap b = right.toMap();
            int result;
            if (numeric)
                result = a.value(key).toLongLong() < b.value(key).toLongLong()
                             ? -1
                             : (a.value(key).toLongLong() > b.value(key).toLongLong() ? 1 : 0);
            else
                result = a.value(key).toString().localeAwareCompare(b.value(key).toString());
            return desc ? result > 0 : result < 0;
        });
        break;
    }

    m_table->clear();
    m_table->setColumnCount(cols.size());
    m_table->setRowCount(rows.size());
    int sortSection = 0;
    for (int i = 0; i < cols.size(); ++i) {
        const Column &column = cols.at(i);
        auto *header = new QTableWidgetItem(column.label);
        header->setTextAlignment(column.numeric ? (Qt::AlignRight | Qt::AlignVCenter)
                                                : (Qt::AlignLeft | Qt::AlignVCenter));
        if (column.key == m_sortKey) {
            sortSection = i;
            header->setForeground(gs::palette().cyan); // .usage-table th button.active
        }
        m_table->setHorizontalHeaderItem(i, header);
    }
    m_table->horizontalHeader()->setSortIndicator(sortSection,
                                                  m_sortDesc ? Qt::DescendingOrder
                                                             : Qt::AscendingOrder);

    for (int r = 0; r < rows.size(); ++r) {
        const QVariantMap row = rows.at(r).toMap();
        for (int c = 0; c < cols.size(); ++c) {
            const Column &column = cols.at(c);
            QString text;
            if (column.key == QLatin1String("name"))
                text = row.value(QStringLiteral("name")).toString();
            else if (column.numeric && column.tokens)
                text = formatMillions(row.value(column.key).toLongLong());
            else if (column.numeric)
                text = QString::number(row.value(column.key).toLongLong());
            else
                text = row.value(column.key).toString();
            auto *item = new QTableWidgetItem(text);
            item->setTextAlignment(column.numeric ? (Qt::AlignRight | Qt::AlignVCenter)
                                                  : (Qt::AlignLeft | Qt::AlignVCenter));
            if (column.numeric) {
                // .usage-table td.num{font-family:Consolas,monospace}
                QFont mono(monoFont());
                mono.setPixelSize(scaledPx(11));
                item->setFont(mono);
                // 缩放时按这个基础字号重建（见 ChartWidget::applyZoom）
                item->setData(Qt::UserRole + 1, 11);
            }
            if (column.key == QLatin1String("name"))
                item->setToolTip(row.value(QStringLiteral("fullKey")).toString());
            m_table->setItem(r, c, item);
        }
    }
    m_table->resizeColumnsToContents();
    if (m_table->columnCount() > 0 && m_table->columnWidth(0) > 220)
        m_table->setColumnWidth(0, 220);
}

// ------------------------------------------------------------ 交互

void UsagePanel::setView(bool hasView, qint64 start, qint64 end)
{
    m_hasView = hasView;
    if (hasView) {
        m_viewStart = start;
        m_viewEnd = end;
    }
    renderCharts();
}

void UsagePanel::pickProvider(const QString &value)
{
    m_provider = value;
    // 已选模型若不属于新供应商，两个条件会自相矛盾查出空结果，直接回落到全部模型
    if (!m_model.isEmpty()) {
        const QVariantMap row = candidateModel(m_model);
        const QString owner = !row.isEmpty()
                                  ? row.value(QStringLiteral("provider")).toString()
                                  : m_catalog.value(m_model).value(QStringLiteral("provider")).toString();
        if (!owner.isEmpty() && owner != value)
            m_model.clear();
    }
    m_hasView = false;
    syncCatalog();
    closePopups();
    loadStats();
}

void UsagePanel::pickModel(const QString &value)
{
    m_model = value;
    // 模型键自带供应商，选模型时把供应商一并对齐
    const QVariantMap row = candidateModel(value);
    const QString owner = !row.isEmpty()
                              ? row.value(QStringLiteral("provider")).toString()
                              : m_catalog.value(value).value(QStringLiteral("provider")).toString();
    if (!owner.isEmpty() && owner != m_provider)
        m_provider = owner;
    m_hasView = false;
    syncCatalog();
    closePopups();
    loadStats();
}

QVariantMap UsagePanel::candidateModel(const QString &key) const
{
    const QVariantMap candidates = m_data.value(QStringLiteral("candidates")).toMap();
    for (const QVariant &v : candidates.value(QStringLiteral("models")).toList()) {
        if (v.toMap().value(QStringLiteral("model")).toString() == key)
            return v.toMap();
    }
    return QVariantMap();
}

void UsagePanel::applyCustom(const QDate &startDay, const QDate &endDay)
{
    const QDate low = startDay <= endDay ? startDay : endDay;
    const QDate high = startDay <= endDay ? endDay : startDay;
    m_customStart = QDateTime(low, QTime(0, 0));
    m_customEnd = QDateTime(high, QTime(23, 59));
    m_preset.clear();
    // 自定义区间跨度为 1 天时同样切到按小时
    if (low == high)
        m_granularity = QStringLiteral("hour");
    m_hasView = false;
    closePopups();
    loadStats();
}

void UsagePanel::loadStats(bool force)
{
    QDateTime start, end;
    windowRange(start, end);
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(isoLocal(start), isoLocal(end), m_provider, m_model);
    if (!force && m_cache.contains(key)) {
        // 命中缓存也要作废仍在飞的请求（app.js token）
        invalidatePendingResponse();
        m_loading = false;
        adoptData(m_cache.value(key));
        return;
    }
    // 每次请求换一个 ID：宿主回填时原样带回，晚到的旧响应据此丢弃
    m_pendingRequestId = QStringLiteral("usage-%1").arg(++m_requestSeq);
    m_pendingKey = key;
    m_loading = true;
    m_error.clear();
    if (m_status)
        m_status->setText(QStringLiteral("正在统计用量…"));
    emit statsRequested(m_pendingRequestId, isoLocal(start), isoLocal(end), m_provider, m_model);
}

void UsagePanel::closePopups()
{
    if (m_listPopup && m_listPopup->isVisible())
        m_listPopup->hide();
    if (m_rangePopup && m_rangePopup->isVisible())
        m_rangePopup->hide();
    if (m_groupSelect)
        m_groupSelect->setOpen(false);
    if (m_providerSelect)
        m_providerSelect->setOpen(false);
    if (m_modelSelect)
        m_modelSelect->setOpen(false);
    if (m_granularitySelect)
        m_granularitySelect->setOpen(false);
    if (m_rangeSelect)
        m_rangeSelect->setOpen(false);
    m_popupClosedAt = QDateTime::currentMSecsSinceEpoch();
    m_activeTrigger = nullptr;
}

} // namespace gs

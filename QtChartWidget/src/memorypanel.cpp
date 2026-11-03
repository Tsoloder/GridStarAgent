#include "memorypanel.h"

#include "commonwidgets.h"
#include "popups.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace gs {

namespace {

constexpr int kPageSize = 20;
constexpr int kSearchDebounceMs = 300;
constexpr int kPollIntervalMs = 3000;
// 维护任务刚投递时后端可能还没把 state 翻成 running，这段窗口内自己先轮询，
// 免得「点了没反应」
constexpr int kPollArmMs = 15000;

// ---- 通用小件 -------------------------------------------------------------

QLabel *plainLabel(const QString &cls, const QString &text, QWidget *parent = nullptr)
{
    QLabel *label = makeLabel(cls, text, parent);
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    return label;
}

QPushButton *styledButton(const QString &text, const QString &variant, QWidget *parent = nullptr)
{
    auto *button = new QPushButton(text, parent);
    if (!variant.isEmpty())
        button->setProperty("variant", variant);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::TabFocus);
    return button;
}

// .link-button：无边框、随主题上色的文字按钮（编辑 / 删除 / 清除 / 保存 / 取消）
QPushButton *linkButton(const QString &text, const QString &tone = QString(),
                        QWidget *parent = nullptr)
{
    auto *button = new QPushButton(text, parent);
    setClass(button, QStringLiteral("linkButton"));
    if (!tone.isEmpty())
        button->setProperty("tone", tone);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::TabFocus);
    return button;
}

QFrame *sectionFrame(QWidget *parent, bool danger = false)
{
    auto *frame = new QFrame(parent);
    setClass(frame, QStringLiteral("memorySection"));
    frame->setFrameShape(QFrame::NoFrame);
    frame->setAttribute(Qt::WA_StyledBackground, true);
    if (danger)
        frame->setProperty("danger", true);
    return frame;
}

// .memory-toolbar-head：eyebrow + 标题 + 计数
QWidget *sectionHead(const QString &eyebrowText, const QString &titleText, QLabel **countOut,
                     bool withCount, QWidget *parent)
{
    auto *head = new QWidget(parent);
    auto *layout = new QVBoxLayout(head);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    QLabel *eyebrow = plainLabel(QStringLiteral("eyebrow"), eyebrowText, head);
    // .eyebrow 是行内 span：占的是所在行盒高度，不是 9px 字形高（settingsdialog 同处理）
    eyebrow->setMinimumHeight(18);
    layout->addWidget(eyebrow);
    auto *line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(6);
    // 与技能 / MCP 页同一个标题角色：.memory-toolbar-head h3 + h3 small
    line->addWidget(plainLabel(QStringLiteral("panelTitle"), titleText, head));
    if (withCount) {
        QLabel *count = plainLabel(QStringLiteral("panelCount"), QStringLiteral("0"), head);
        line->addWidget(count, 0, Qt::AlignBottom);
        if (countOut)
            *countOut = count;
    }
    line->addStretch(1);
    layout->addLayout(line);
    return head;
}

void clearLayoutItems(QLayout *layout)
{
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            // 立刻藏起来：deleteLater 要等下一轮事件循环，留在原位会闪一下旧卡
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

// ---- 值 / 时间格式（照 app.js memory* 的口径） -----------------------------

// app.js memoryTime：库里存 UTC，展示一律转本地时区 yyyy-MM-dd HH:mm
QString memoryTimeText(const QVariant &value)
{
    const QString raw = value.toString().trimmed();
    if (raw.isEmpty())
        return QString();
    QString stamp = raw;
    if (stamp.size() > 10 && stamp.at(10) == QLatin1Char(' '))
        stamp[10] = QLatin1Char('T');
    // 带时区（Z / ±hh:mm）就按原样解析，不带就按 UTC 解释（与 webui 补 "Z" 同一个意思）
    const bool hasZone = stamp.endsWith(QLatin1Char('Z'), Qt::CaseInsensitive)
        || stamp.mid(10).contains(QLatin1Char('+'))
        || stamp.mid(10).contains(QLatin1Char('-'));
    QDateTime date = QDateTime::fromString(stamp, Qt::ISODateWithMs);
    if (!date.isValid())
        date = QDateTime::fromString(stamp, Qt::ISODate);
    if (!date.isValid()) {
        QString fallback = raw.left(16);
        fallback.replace(QLatin1Char('T'), QLatin1Char(' '));
        return fallback;
    }
    if (!hasZone)
        date.setTimeSpec(Qt::UTC);
    return date.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

// app.js memoryScopeLabel：skill:<id> 只显示 <id>
QString scopeLabel(const QString &scope)
{
    const QString raw = scope.isEmpty() ? QStringLiteral("global") : scope;
    if (raw.startsWith(QLatin1String("skill:")))
        return raw.mid(6);
    return raw;
}

QString statusLabel(const QString &status)
{
    if (status == QLatin1String("candidate"))
        return QStringLiteral("待审核");
    if (status == QLatin1String("approved"))
        return QStringLiteral("已启用");
    if (status == QLatin1String("rejected"))
        return QStringLiteral("已拒绝");
    if (status == QLatin1String("deprecated"))
        return QStringLiteral("已停用");
    return status;
}

QString kindLabel(const QString &kind)
{
    return kind == QLatin1String("fact") ? QStringLiteral("工程事实") : QStringLiteral("策略经验");
}

bool isNumericVariant(const QVariant &value)
{
    switch (value.type()) {
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
    case QVariant::Double:
        return true;
    default:
        return false;
    }
}

// app.js memoryFieldEditText：编辑框里放能直接改的字面量，字符串不带引号
QString fieldEditText(const QVariant &value)
{
    if (value.type() == QVariant::String)
        return value.toString();
    if (!value.isValid() || value.isNull())
        return QString();
    return value.toString();
}

// app.js memoryFieldType：类型族由原值锁定，界面上改不出来
QString fieldTypeLabel(const QVariant &value)
{
    if (value.type() == QVariant::Bool)
        return QStringLiteral("布尔");
    if (isNumericVariant(value))
        return QStringLiteral("数字");
    if (value.type() == QVariant::String)
        return QStringLiteral("文本");
    return QStringLiteral("未知");
}

QString jsonStringLiteral(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    out.replace(QLatin1Char('"'), QLatin1String("\\\""));
    out.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    out.replace(QLatin1Char('\r'), QLatin1String("\\r"));
    out.replace(QLatin1Char('\t'), QLatin1String("\\t"));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

// app.js memoryFieldValue：字符串带引号、对象走 JSON、空值写 null
QString fieldValueText(const QVariant &value)
{
    switch (value.type()) {
    case QVariant::String:
        return jsonStringLiteral(value.toString());
    case QVariant::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QVariant::Invalid:
        return QStringLiteral("null");
    case QVariant::Map:
        return QString::fromUtf8(
            QJsonDocument(QJsonObject::fromVariantMap(value.toMap())).toJson(QJsonDocument::Compact));
    case QVariant::List:
        return QString::fromUtf8(
            QJsonDocument(QJsonArray::fromVariantList(value.toList())).toJson(QJsonDocument::Compact));
    default:
        return value.toString();
    }
}

// app.js memoryParseFieldValue：按原值类型还原，类型不合法就报错。
// 类型族只认原值：数字格存不下文本，布尔格只认 true/false。
bool parseFieldValue(const QString &text, const QVariant &original, QVariant &out, QString &error)
{
    if (original.type() == QVariant::String) {
        out = text;
        return true;
    }
    const QString raw = text.trimmed();
    if (original.type() == QVariant::Bool) {
        const QString lower = raw.toLower();
        if (lower == QLatin1String("true") || lower == QLatin1String("1")
            || lower == QLatin1String("yes") || lower == QLatin1String("on")) {
            out = true;
            return true;
        }
        if (lower == QLatin1String("false") || lower == QLatin1String("0")
            || lower == QLatin1String("no") || lower == QLatin1String("off")) {
            out = false;
            return true;
        }
        error = QStringLiteral("布尔字段只能填 true 或 false");
        return false;
    }
    if (!isNumericVariant(original)) {
        // 原值是 null / 对象 / 数组这类：文字改不动类型，按文本交回
        out = text;
        return true;
    }
    static const QRegularExpression numberRe(
        QStringLiteral("^[+-]?(\\d+(\\.\\d*)?|\\.\\d+)([eE][+-]?\\d+)?$"));
    if (!numberRe.match(raw).hasMatch()) {
        error = QStringLiteral("数字字段只能填数字");
        return false;
    }
    bool ok = false;
    const double parsed = raw.toDouble(&ok);
    if (!ok) {
        error = QStringLiteral("数字字段只能填数字");
        return false;
    }
    out = parsed;
    return true;
}

bool sameValue(const QVariant &a, const QVariant &b)
{
    if (isNumericVariant(a) && isNumericVariant(b))
        return a.toDouble() == b.toDouble();
    if (a.type() != b.type())
        return false;
    return a == b;
}

bool isNumberVariant(const QVariant &value)
{
    return isNumericVariant(value);
}

// ---- 可点的一行（.memory-switch 卡片 / <summary> 头行） ---------------------
// 不用 Q_OBJECT：这里只需要一个回调，省掉在 .cpp 里塞 moc 的麻烦。
class ClickRow : public QFrame
{
public:
    explicit ClickRow(QWidget *parent = nullptr) : QFrame(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_StyledBackground, true);
        setProperty("clickable", true);
    }

    std::function<void()> activated;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos()))
            trigger();
        QFrame::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
            || event->key() == Qt::Key_Space) {
            trigger();
            return;
        }
        QFrame::keyPressEvent(event);
    }

private:
    void trigger()
    {
        if (!property("clickable").toBool())
            return;
        if (activated)
            activated();
    }
};

// 行内文字 / 提示不接鼠标，点击落到整行上（webui 里它们是 label 里的 span）
void makeMouseTransparent(QWidget *widget)
{
    widget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

} // namespace

// ------------------------------------------------------------ MemoryPanel

MemoryPanel::MemoryPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("memoryPanel"));
    setClass(this, QStringLiteral("memoryPanelHost"));

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(kSearchDebounceMs);
    connect(m_searchTimer, &QTimer::timeout, this, [this] { reloadFromFirstPage(); });

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, [this] {
        emit refreshRequested();
        syncPolling();
    });

    buildUi();
    renderAll();
}

void MemoryPanel::buildUi()
{
    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(0, 0, 0, 0);
    m_root->setSpacing(0);

    // .memory-panel 自己是纵向滚动容器（外层 settingsdialog 不再套 plainScroll）
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("memoryScroll"));
    setClass(m_scroll, QStringLiteral("memoryScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_root->addWidget(m_scroll, 1);

    m_content = new QWidget;
    m_content->setObjectName(QStringLiteral("memoryContent"));
    setClass(m_content, QStringLiteral("memoryContent"));
    m_scroll->setWidget(m_content);

    m_contentLayout = new QVBoxLayout(m_content);
    m_contentLayout->setContentsMargins(18, 16, 18, 16);
    m_contentLayout->setSpacing(12);
    m_contentLayout->addWidget(buildStatusBar());
    m_contentLayout->addWidget(buildSwitchRow());
    m_contentLayout->addWidget(buildExperienceSection());
    m_contentLayout->addWidget(buildToolSection());
    m_contentLayout->addWidget(buildMaintenanceSection());
    m_contentLayout->addStretch(1);
}

QWidget *MemoryPanel::buildStatusBar()
{
    m_status = plainLabel(QStringLiteral("memoryStatus"), QStringLiteral("正在读取记忆…"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::RichText);
    m_status->setAttribute(Qt::WA_StyledBackground, true);
    return m_status;
}

QWidget *MemoryPanel::buildSwitchRow()
{
    // style.css .memory-switches{grid-template-columns:repeat(auto-fit,minmax(220px,1fr))}
    // 设置对话框最窄也有 640，三列等宽就是它的等价物
    auto *host = new QWidget;
    setClass(host, QStringLiteral("memorySwitches"));
    m_switchHost = host;
    m_switchLayout = new QHBoxLayout(host);
    m_switchLayout->setContentsMargins(0, 0, 0, 0);
    m_switchLayout->setSpacing(8);

    struct RowDef {
        const char *title;
        const char *hint;
    };
    static const RowDef rows[3] = {
        { "启用记忆", "关闭后停止提炼与参数记忆写入，已有数据保留。" },
        { "自动提炼", "从已完成的计划里提炼待审核候选，通过审核后才会被使用。" },
        { "注入记忆提示", "当前为影子期：只采集不注入，开启后也要等审核通过的记忆才生效。" },
    };
    static const char *const keys[3] = { "enabled", "extract", "inject" };

    for (int i = 0; i < 3; ++i) {
        auto *card = new ClickRow(host);
        setClass(card, QStringLiteral("memorySwitch"));
        auto *rowLayout = new QHBoxLayout(card);
        rowLayout->setContentsMargins(10, 9, 10, 9);
        rowLayout->setSpacing(8);

        auto *box = new QCheckBox(card);
        setClass(box, QStringLiteral("memoryCheck"));
        box->setFocusPolicy(Qt::TabFocus);
        rowLayout->addWidget(box, 0, Qt::AlignTop);

        auto *body = new QVBoxLayout;
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(3);
        QLabel *title = plainLabel(QStringLiteral("memorySwitchTitle"),
                                   QString::fromUtf8(rows[i].title), card);
        makeMouseTransparent(title);
        QLabel *hint = plainLabel(QStringLiteral("memorySwitchHint"),
                                  QString::fromUtf8(rows[i].hint), card);
        hint->setWordWrap(true);
        makeMouseTransparent(hint);
        body->addWidget(title);
        body->addWidget(hint);
        rowLayout->addLayout(body, 1);

        card->activated = [box] { box->toggle(); };
        const QString key = QString::fromLatin1(keys[i]);
        connect(box, &QCheckBox::toggled, this, [this, key](bool checked) {
            if (m_syncingSwitches)
                return;
            if (m_busy) {
                renderSwitches();
                return;
            }
            QVariantMap updates;
            updates.insert(key, checked);
            emit configPatchRequested(updates);
        });

        m_switchLayout->addWidget(card, 1);
        m_switchCards[i] = card;
        m_switchBoxes[i] = box;
    }
    return host;
}

QWidget *MemoryPanel::buildExperienceSection()
{
    QFrame *section = sectionFrame(nullptr);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(0);
    layout->addWidget(sectionHead(QStringLiteral("经验条目"),
                                  QStringLiteral("策略经验与工程事实"), &m_count, true, section));
    layout->addSpacing(6);
    layout->addWidget(buildFilterRow());
    layout->addSpacing(4);
    layout->addWidget(buildBulkBar());
    layout->addSpacing(6);

    m_listHost = new QWidget(section);
    setClass(m_listHost, QStringLiteral("memoryList"));
    m_listLayout = new QVBoxLayout(m_listHost);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(8);
    layout->addWidget(m_listHost);

    m_moreRow = new QWidget(section);
    auto *moreLayout = new QVBoxLayout(m_moreRow);
    moreLayout->setContentsMargins(0, 10, 0, 0);
    moreLayout->setSpacing(0);
    m_loadMore = styledButton(QStringLiteral("加载更多"), QStringLiteral("secondary"), m_moreRow);
    m_loadMore->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_loadMore, &QPushButton::clicked, this, [this] { requestPage(m_page + 1); });
    moreLayout->addWidget(m_loadMore);
    m_moreRow->setVisible(false);
    layout->addWidget(m_moreRow);
    return section;
}

QWidget *MemoryPanel::buildFilterRow()
{
    auto *row = new QWidget;
    setClass(row, QStringLiteral("memoryFilters"));
    auto *flow = new FlowLayout(row, 0, 8, 8);

    m_statusFilter = new QComboBox(row);
    setClass(m_statusFilter, QStringLiteral("settingsInput"));
    m_statusFilter->setFocusPolicy(Qt::TabFocus);
    m_statusFilter->addItem(QStringLiteral("待审核"), QStringLiteral("candidate"));
    m_statusFilter->addItem(QStringLiteral("已启用"), QStringLiteral("approved"));
    m_statusFilter->addItem(QStringLiteral("已拒绝"), QStringLiteral("rejected"));
    m_statusFilter->addItem(QStringLiteral("已停用"), QStringLiteral("deprecated"));
    m_statusFilter->addItem(QStringLiteral("全部"), QString());
    m_statusFilter->setMinimumWidth(scaledPx(96));

    m_kindFilter = new QComboBox(row);
    setClass(m_kindFilter, QStringLiteral("settingsInput"));
    m_kindFilter->setFocusPolicy(Qt::TabFocus);
    m_kindFilter->addItem(QStringLiteral("全部"), QString());
    m_kindFilter->addItem(QStringLiteral("策略经验"), QStringLiteral("experience"));
    m_kindFilter->addItem(QStringLiteral("工程事实"), QStringLiteral("fact"));
    m_kindFilter->setMinimumWidth(scaledPx(104));

    m_scopeFilter = new QComboBox(row);
    setClass(m_scopeFilter, QStringLiteral("settingsInput"));
    m_scopeFilter->setFocusPolicy(Qt::TabFocus);
    m_scopeFilter->addItem(QStringLiteral("全部作用域"), QString());
    m_scopeFilter->setMinimumWidth(scaledPx(132));

    m_search = new QLineEdit(row);
    setClass(m_search, QStringLiteral("settingsInput"));
    m_search->setFocusPolicy(Qt::TabFocus);
    m_search->setPlaceholderText(QStringLiteral("关键词"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(scaledPx(150));

    m_refresh = styledButton(QStringLiteral("刷新"), QStringLiteral("secondary"), row);

    const auto addCell = [row, flow](const QString &caption, QWidget *control) {
        auto *cell = new QWidget(row);
        setClass(cell, QStringLiteral("memoryFilter"));
        auto *cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(4);
        cellLayout->addWidget(plainLabel(QStringLiteral("memoryFilterLabel"), caption, cell));
        cellLayout->addWidget(control);
        flow->addWidget(cell);
    };
    addCell(QStringLiteral("状态"), m_statusFilter);
    addCell(QStringLiteral("类型"), m_kindFilter);
    addCell(QStringLiteral("作用域"), m_scopeFilter);
    addCell(QStringLiteral("搜索"), m_search);

    // .memory-filters{align-items:flex-end}：刷新按钮底边与筛选控件对齐，
    // 所以上面垫一段与「标题行」等高的空白
    auto *refreshCell = new QWidget(row);
    auto *refreshLayout = new QVBoxLayout(refreshCell);
    refreshLayout->setContentsMargins(0, 0, 0, 0);
    refreshLayout->setSpacing(0);
    refreshLayout->addSpacing(scaledPx(14));
    refreshLayout->addWidget(m_refresh);
    flow->addWidget(refreshCell);

    // 任一筛选或搜索变化都回到 page=1（webui 的三个 onchange + 300ms 去抖）
    connect(m_statusFilter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { if (!m_syncingFilters) reloadFromFirstPage(); });
    connect(m_kindFilter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { if (!m_syncingFilters) reloadFromFirstPage(); });
    connect(m_scopeFilter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { if (!m_syncingFilters) reloadFromFirstPage(); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_searchTimer->start(); });
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        reloadFromFirstPage();
        emit refreshRequested();
    });
    return row;
}

QWidget *MemoryPanel::buildBulkBar()
{
    auto *bar = new QFrame;
    m_bulkBar = bar;
    setClass(bar, QStringLiteral("memoryBulk"));
    bar->setFrameShape(QFrame::NoFrame);
    bar->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(9, 7, 9, 7);
    layout->setSpacing(8);

    m_selectPage = new QCheckBox(QStringLiteral("本页全选"), bar);
    setClass(m_selectPage, QStringLiteral("memoryCheck"));
    m_selectPage->setFocusPolicy(Qt::TabFocus);
    m_selectPage->setTristate(false);
    layout->addWidget(m_selectPage, 0, Qt::AlignVCenter);

    m_bulkCount = plainLabel(QStringLiteral("memoryBulkCount"), QStringLiteral("已选 0 条"), bar);
    layout->addWidget(m_bulkCount, 0, Qt::AlignVCenter);

    m_bulkApprove = styledButton(QStringLiteral("批量通过"), QStringLiteral("secondary"), bar);
    m_bulkReject = styledButton(QStringLiteral("批量拒绝"), QStringLiteral("secondary"), bar);
    layout->addWidget(m_bulkApprove, 0);
    layout->addWidget(m_bulkReject, 0);
    layout->addStretch(1);

    connect(m_selectPage, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_syncingBulk)
            return;
        QStringList pageIds;
        for (const QVariant &entry : m_items) {
            const QVariantMap item = entry.toMap();
            if (item.value(QStringLiteral("status")).toString() == QLatin1String("candidate"))
                pageIds << item.value(QStringLiteral("id")).toString();
        }
        for (const QString &id : pageIds) {
            if (checked)
                m_selected.insert(id);
            else
                m_selected.remove(id);
        }
        // 本页全选之后把逐卡勾选框一起对上，避免界面与集合不一致
        for (auto it = m_cardChecks.constBegin(); it != m_cardChecks.constEnd(); ++it) {
            QSignalBlocker blocker(it.value());
            it.value()->setChecked(m_selected.contains(it.key()));
        }
        renderBulk();
    });
    connect(m_bulkApprove, &QPushButton::clicked, this,
            [this] { runBulk(QStringLiteral("approve")); });
    connect(m_bulkReject, &QPushButton::clicked, this,
            [this] { runBulk(QStringLiteral("reject")); });

    bar->setVisible(false);
    return bar;
}

QWidget *MemoryPanel::buildToolSection()
{
    QFrame *section = sectionFrame(nullptr);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(0);
    layout->addWidget(sectionHead(QStringLiteral("参数记忆"), QStringLiteral("工具参数记忆"),
                                  &m_toolCount, true, section));
    layout->addSpacing(4);

    QLabel *hint = plainLabel(QStringLiteral("memoryHint"),
                              QStringLiteral("你在工具审批中确认过的参数会被记住，作为下次调用的默认建议。"),
                              section);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *host = new QWidget(section);
    setClass(host, QStringLiteral("memoryTools"));
    m_toolsLayout = new QVBoxLayout(host);
    m_toolsLayout->setContentsMargins(0, 0, 0, 0);
    m_toolsLayout->setSpacing(6);
    layout->addWidget(host);
    return section;
}

QWidget *MemoryPanel::buildMaintenanceSection()
{
    QFrame *section = sectionFrame(nullptr, true);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(0);
    layout->addWidget(sectionHead(QStringLiteral("维护任务"), QStringLiteral("维护"), nullptr,
                                  false, section));
    layout->addSpacing(6);

    m_maintResult = plainLabel(QStringLiteral("memoryHint"),
                               QStringLiteral("还没有运行过维护任务。"), section);
    m_maintResult->setWordWrap(true);
    layout->addWidget(m_maintResult);

    auto *actions = new QWidget(section);
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 4, 0, 4);
    actionsLayout->setSpacing(8);
    m_runExtract = styledButton(QStringLiteral("立即提炼"), QStringLiteral("secondary"), actions);
    m_runGovernance = styledButton(QStringLiteral("运行维护"), QStringLiteral("secondary"), actions);
    m_clearTools = styledButton(QStringLiteral("清空全部工具参数记忆"), QStringLiteral("danger"),
                                actions);
    actionsLayout->addWidget(m_runExtract, 0);
    actionsLayout->addWidget(m_runGovernance, 0);
    actionsLayout->addWidget(m_clearTools, 0);
    actionsLayout->addStretch(1);
    layout->addWidget(actions);

    QLabel *hint = plainLabel(QStringLiteral("memoryHint"),
                              QStringLiteral("「立即提炼」只生成待审核候选，不会自动启用；"
                                             "「清空全部工具参数记忆」不可撤销。"),
                              section);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    connect(m_runExtract, &QPushButton::clicked, this,
            [this] { startMaintenance(QStringLiteral("extract")); });
    connect(m_runGovernance, &QPushButton::clicked, this,
            [this] { startMaintenance(QStringLiteral("governance")); });
    connect(m_clearTools, &QPushButton::clicked, this, [this] { clearAllTools(); });
    return section;
}

// ------------------------------------------------------------ 渲染：状态条

void MemoryPanel::renderStatus()
{
    if (!m_status)
        return;

    QString tone;
    QString text;  // 纯文本，最后整体转义
    QString extra; // 已经转义好的弱化段落（notice / 宿主提示）
    QStringList parts;

    const auto weakSpan = [](const QString &raw) {
        return QStringLiteral("<span style=\"color:%1;\">%2</span>")
            .arg(cssColor(gs::palette().muted2), escapeHtml(raw));
    };

    if (m_loading && !m_loaded) {
        text = QStringLiteral("正在读取记忆…");
    } else if (!m_error.isEmpty() && m_summary.isEmpty()) {
        tone = QStringLiteral("error");
        text = QStringLiteral("记忆读取失败：%1").arg(m_error);
    } else if (m_summary.isEmpty()) {
        text = m_statusText.isEmpty() ? QStringLiteral("正在读取记忆…") : m_statusText;
    } else {
        const QVariantMap store = m_summary.value(QStringLiteral("store")).toMap();
        const QVariantMap counts = m_summary.value(QStringLiteral("experience")).toMap();
        const bool enabled = m_summary.value(QStringLiteral("enabled")).toBool();

        parts << (enabled ? QStringLiteral("记忆已启用") : QStringLiteral("记忆已关闭"));
        tone = enabled ? QStringLiteral("ok") : QStringLiteral("off");

        if (store.value(QStringLiteral("status")).toString() == QLatin1String("healthy")) {
            parts << QStringLiteral("经验库正常");
        } else {
            parts << QStringLiteral("经验库降级");
            tone = QStringLiteral("warn");
        }
        // 关闭时不谈库健康，避免误导
        if (enabled) {
            const QVariant fts = store.value(QStringLiteral("fts"));
            if (fts.isValid() && !fts.toBool()) {
                parts << QStringLiteral("关键词检索降级为模糊匹配");
                tone = QStringLiteral("warn");
            } else if (fts.toBool()) {
                parts << QStringLiteral("全文检索可用");
            }
        }
        parts << QStringLiteral("待审核 %1 条").arg(counts.value(QStringLiteral("candidate")).toInt());

        const QVariantMap maint = m_summary.value(QStringLiteral("maintenance")).toMap();
        if (maint.value(QStringLiteral("state")).toString() == QLatin1String("running")) {
            const QString step = maint.value(QStringLiteral("step")).toString();
            parts << QStringLiteral("维护进行中（%1）")
                         .arg(step.isEmpty() ? QStringLiteral("准备") : step);
            tone = QStringLiteral("warn");
        } else {
            const QString finished = memoryTimeText(maint.value(QStringLiteral("finished_at")));
            if (!finished.isEmpty())
                parts << QStringLiteral("上次维护 %1").arg(finished);
        }
        text = parts.join(QStringLiteral(" · "));

        const QString notice = m_summary.value(QStringLiteral("notice")).toString().trimmed();
        // webui 只 append 一个空格，读起来会和上一条粘成一句，这里补个中点分隔
        if (!notice.isEmpty())
            extra = weakSpan(QStringLiteral("· ") + notice);
    }

    // 宿主下发的即时提示：正常串后面追加一段弱化文本（读取失败时它已经在主串里）
    if (!m_summary.isEmpty() && !m_statusText.isEmpty()) {
        if (!extra.isEmpty())
            extra += QLatin1Char(' ');
        extra += weakSpan(m_statusText);
    }

    m_status->setProperty("tone", tone);
    restyle(m_status);
    QString html = escapeHtml(text);
    if (!extra.isEmpty())
        html += QLatin1Char(' ') + extra;
    m_status->setText(html);
}

// ------------------------------------------------------------ 渲染：开关

void MemoryPanel::renderSwitches()
{
    if (!m_switchHost)
        return;
    const bool hasSummary = !m_summary.isEmpty();
    m_switchHost->setVisible(hasSummary);
    if (!hasSummary)
        return;

    const QVariantMap config = m_summary.value(QStringLiteral("config")).toMap();
    const bool on = m_summary.value(QStringLiteral("enabled")).toBool();
    const bool checked[3] = { on, config.value(QStringLiteral("experience_extract")).toBool(),
                              config.value(QStringLiteral("experience_inject")).toBool() };

    m_syncingSwitches = true;
    for (int i = 0; i < 3; ++i) {
        if (m_switchBoxes[i])
            m_switchBoxes[i]->setChecked(checked[i]);
    }
    m_syncingSwitches = false;
    applySwitchEnabled();
}

void MemoryPanel::applySwitchEnabled()
{
    const bool on = m_summary.value(QStringLiteral("enabled")).toBool();
    // 后两个开关在「启用记忆」关闭时置灰
    const bool blocked[3] = { m_busy, m_busy || !on, m_busy || !on };
    for (int i = 0; i < 3; ++i) {
        QCheckBox *box = m_switchBoxes[i];
        QWidget *card = m_switchCards[i];
        if (!box || !card)
            continue;
        box->setEnabled(!blocked[i]);
        card->setProperty("disabled", blocked[i]);
        card->setProperty("clickable", !blocked[i]);
        restyle(card);
    }
}

// ------------------------------------------------------------ 渲染：筛选

void MemoryPanel::renderFilters()
{
    if (!m_scopeFilter)
        return;
    m_syncingFilters = true;
    const QString current = m_scopeFilter->currentData().toString();
    m_scopeFilter->clear();
    m_scopeFilter->addItem(QStringLiteral("全部作用域"), QString());
    const QVariantList scopes = m_facets.value(QStringLiteral("scopes")).toList();
    for (const QVariant &entry : scopes) {
        QString value;
        int count = 0;
        if (entry.canConvert<QVariantMap>()) {
            const QVariantMap map = entry.toMap();
            value = map.value(QStringLiteral("value")).toString();
            count = map.value(QStringLiteral("count")).toInt();
        } else {
            value = entry.toString();
        }
        if (value.isEmpty())
            continue;
        m_scopeFilter->addItem(QStringLiteral("%1（%2）").arg(scopeLabel(value)).arg(count), value);
    }
    const int index = current.isEmpty() ? 0 : m_scopeFilter->findData(current);
    m_scopeFilter->setCurrentIndex(index < 0 ? 0 : index);
    m_syncingFilters = false;
}

// ------------------------------------------------------------ 渲染：列表

void MemoryPanel::renderList()
{
    if (!m_listLayout)
        return;
    clearLayoutItems(m_listLayout);
    m_cardButtons.clear();
    m_cardChecks.clear();

    if (m_count)
        m_count->setText(QString::number(m_total));

    if (m_items.isEmpty()) {
        QString text;
        if (!m_error.isEmpty())
            text = QStringLiteral("读取失败：%1").arg(m_error);
        else if (m_loaded)
            text = QStringLiteral("这个筛选下还没有记忆。完成的计划会被自动提炼成待审核候选。");
        if (!text.isEmpty()) {
            QLabel *empty = plainLabel(QStringLiteral("memoryEmpty"), text, m_listHost);
            empty->setWordWrap(true);
            m_listLayout->addWidget(empty);
        }
    } else {
        for (const QVariant &entry : m_items)
            m_listLayout->addWidget(buildCard(entry.toMap()));
    }

    // .memory-more：只在还有没翻完的条目时出现
    if (m_moreRow) {
        const bool more = m_items.size() < m_total && !m_items.isEmpty();
        m_moreRow->setVisible(more);
        if (m_loadMore)
            m_loadMore->setEnabled(more && !m_busy);
    }
    renderBulk();
}

QWidget *MemoryPanel::buildCard(const QVariantMap &item)
{
    const QString id = item.value(QStringLiteral("id")).toString();
    const QString status = item.value(QStringLiteral("status")).toString();
    const bool selectable = status == QLatin1String("candidate");

    auto *card = new QFrame(m_listHost);
    setClass(card, QStringLiteral("memoryCard"));
    card->setFrameShape(QFrame::NoFrame);
    card->setAttribute(Qt::WA_StyledBackground, true);
    // 列表装不满时 Qt 会把可伸展的子件抳开：卡片按内容高度收住，
    // 否则每张卡下方会多出一大片空白（webui 里 .memory-card 是 fit-content）
    card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(10, 9, 10, 9);
    layout->setSpacing(0);

    // 头行：勾选位 + 状态 chip + 标签串
    auto *head = new QWidget(card);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(8);

    if (selectable) {
        auto *box = new QCheckBox(head);
        setClass(box, QStringLiteral("memoryCheck"));
        box->setFocusPolicy(Qt::TabFocus);
        box->setChecked(m_selected.contains(id));
        box->setEnabled(!m_busy);
        box->setAccessibleName(QStringLiteral("选择这条记忆"));
        connect(box, &QCheckBox::toggled, this, [this, id](bool checked) {
            if (checked)
                m_selected.insert(id);
            else
                m_selected.remove(id);
            renderBulk();
        });
        headLayout->addWidget(box, 0, Qt::AlignVCenter);
        m_cardChecks.insert(id, box);
    } else {
        // 非候选不可选，留等宽占位保持列对齐
        auto *spacer = new QWidget(head);
        setClass(spacer, QStringLiteral("memoryCardSpacer"));
        spacer->setFixedSize(scaledPx(14), scaledPx(14));
        headLayout->addWidget(spacer, 0, Qt::AlignVCenter);
    }

    QLabel *chip = plainLabel(QStringLiteral("memoryChip"), statusLabel(status), head);
    chip->setProperty("tone", status);
    chip->setAttribute(Qt::WA_StyledBackground, true);
    headLayout->addWidget(chip, 0, Qt::AlignVCenter);

    auto *tags = new QWidget(head);
    auto *tagLayout = new QHBoxLayout(tags);
    tagLayout->setContentsMargins(0, 0, 0, 0);
    tagLayout->setSpacing(6);
    QStringList tagTexts;
    tagTexts << scopeLabel(item.value(QStringLiteral("scope")).toString());
    tagTexts << kindLabel(item.value(QStringLiteral("kind")).toString());
    const QString taskType = item.value(QStringLiteral("task_type")).toString();
    if (!taskType.isEmpty())
        tagTexts << taskType;
    for (const QString &tag : tagTexts) {
        QLabel *tagLabel = plainLabel(QStringLiteral("memoryCardTag"), tag, tags);
        tagLayout->addWidget(tagLabel, 0, Qt::AlignVCenter);
    }
    tagLayout->addStretch(1);
    headLayout->addWidget(tags, 1);
    layout->addWidget(head);

    const QString title = item.value(QStringLiteral("title")).toString().trimmed();
    QLabel *titleLabel = plainLabel(QStringLiteral("memoryCardTitle"),
                                    title.isEmpty() ? QStringLiteral("（无标题）") : title, card);
    titleLabel->setWordWrap(true);
    titleLabel->setContentsMargins(0, 6, 0, 4);
    layout->addWidget(titleLabel);

    QStringList bits;
    bits << QStringLiteral("置信度 %1")
                .arg(QString::number(item.value(QStringLiteral("confidence")).toDouble(), 'f', 2));
    bits << QStringLiteral("证据 %1").arg(item.value(QStringLiteral("evidence_count")).toInt());
    bits << memoryTimeText(item.value(selectable ? QStringLiteral("created_at")
                                                : QStringLiteral("updated_at")));
    if (item.value(QStringLiteral("possible_duplicate")).toBool())
        bits << QStringLiteral("疑似重复");
    bits.removeAll(QString());
    QLabel *meta = plainLabel(QStringLiteral("memoryCardMeta"), bits.join(QStringLiteral(" · ")), card);
    meta->setContentsMargins(0, 0, 0, 8);
    layout->addWidget(meta);

    auto *actions = new QWidget(card);
    auto *actionLayout = new QHBoxLayout(actions);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(6);

    QPushButton *detail = styledButton(QStringLiteral("查看详情"), QStringLiteral("secondary"), actions);
    connect(detail, &QPushButton::clicked, this, [this, id] { emit detailRequested(id); });
    actionLayout->addWidget(detail, 0);
    m_cardButtons << detail;

    struct ActionDef {
        const char *action;
        const char *label;
        const char *tone;
    };
    QList<ActionDef> defs;
    if (status == QLatin1String("candidate")) {
        defs << ActionDef{ "approve", "通过", "approve" };
        defs << ActionDef{ "reject", "拒绝", "deny" };
    } else if (status == QLatin1String("approved")) {
        defs << ActionDef{ "deprecate", "停用", "deny" };
    } else if (status == QLatin1String("rejected") || status == QLatin1String("deprecated")) {
        defs << ActionDef{ "restore", "恢复", "approve" };
    }
    defs << ActionDef{ "delete", "删除", "danger" };
    for (const ActionDef &def : defs) {
        auto *button = new QPushButton(QString::fromUtf8(def.label), actions);
        setClass(button, QStringLiteral("actionButton"));
        button->setProperty("tone", QString::fromLatin1(def.tone));
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::TabFocus);
        button->setEnabled(!m_busy);
        const QString action = QString::fromLatin1(def.action);
        connect(button, &QPushButton::clicked, this, [this, id, action] { actOnItem(id, action); });
        actionLayout->addWidget(button, 0);
        m_cardButtons << button;
    }
    actionLayout->addStretch(1);
    layout->addWidget(actions);
    return card;
}

void MemoryPanel::renderBulk()
{
    if (!m_bulkBar)
        return;
    bool hasCandidate = false;
    QStringList pageIds;
    for (const QVariant &entry : m_items) {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("status")).toString() == QLatin1String("candidate")) {
            hasCandidate = true;
            pageIds << item.value(QStringLiteral("id")).toString();
        }
    }
    m_bulkBar->setVisible(hasCandidate);

    const int count = m_selected.size();
    if (m_bulkCount)
        m_bulkCount->setText(QStringLiteral("已选 %1 条").arg(count));
    if (m_bulkApprove)
        m_bulkApprove->setEnabled(count > 0 && !m_busy);
    if (m_bulkReject)
        m_bulkReject->setEnabled(count > 0 && !m_busy);

    if (m_selectPage) {
        bool all = !pageIds.isEmpty();
        for (const QString &id : pageIds)
            all = all && m_selected.contains(id);
        m_syncingBulk = true;
        // 不用三态：QCheckBox 的 PartiallyChecked 要 tristate 才画得出来，而 tristate 下
        // 一次点击会停在中态，跟集合对不上。部分选中就显示为未选中，点一下即全选本页
        m_selectPage->setCheckState(all ? Qt::Checked : Qt::Unchecked);
        m_selectPage->setEnabled(!pageIds.isEmpty() && !m_busy);
        m_syncingBulk = false;
    }
}

// ------------------------------------------------------------ 渲染：参数记忆

void MemoryPanel::renderTools()
{
    if (!m_toolsLayout)
        return;
    clearLayoutItems(m_toolsLayout);
    m_toolButtons.clear();

    const QVariantList tools = m_tools.value(QStringLiteral("tools")).toList();
    if (m_toolCount)
        m_toolCount->setText(QString::number(m_tools.value(QStringLiteral("tool_count")).toInt()));

    // 展开态跟着数据收敛：工具已经不在了就把钥匙也丢掉（边收集边删会把 QSet 迭代器打坏）
    QSet<QString> present;
    for (const QVariant &entry : tools)
        present.insert(entry.toMap().value(QStringLiteral("tool")).toString());
    QList<QString> stale;
    for (const QString &name : m_openTools) {
        if (!present.contains(name))
            stale.append(name);
    }
    for (const QString &name : stale)
        m_openTools.remove(name);
    if (!m_editTool.isEmpty() && !present.contains(m_editTool)) {
        m_editTool.clear();
        m_editField.clear();
    }

    if (tools.isEmpty()) {
        if (m_toolsLoaded) {
            QLabel *empty = plainLabel(QStringLiteral("memoryEmpty"),
                                       QStringLiteral("还没有记住的工具参数。你在工具审批中确认参数后，"
                                                      "这里会出现记录。"),
                                       this);
            empty->setWordWrap(true);
            m_toolsLayout->addWidget(empty);
        }
        return;
    }
    for (const QVariant &entry : tools)
        m_toolsLayout->addWidget(buildToolBlock(entry.toMap()));
}

QWidget *MemoryPanel::buildToolBlock(const QVariantMap &tool)
{
    const QString name = tool.value(QStringLiteral("tool")).toString();
    const QVariantMap fields = tool.value(QStringLiteral("fields")).toMap();
    const bool open = m_openTools.contains(name);

    auto *block = new QFrame(m_content);
    setClass(block, QStringLiteral("memoryTool"));
    block->setFrameShape(QFrame::NoFrame);
    block->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(block);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *head = new ClickRow(block);
    setClass(head, QStringLiteral("memoryToolHead"));
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(10, 8, 10, 8);
    headLayout->setSpacing(10);

    auto *chevron = new Chevron(head);
    chevron->setOpen(open);
    makeMouseTransparent(chevron);
    headLayout->addWidget(chevron, 0, Qt::AlignVCenter);

    QLabel *nameLabel = plainLabel(QStringLiteral("memoryToolName"), name, head);
    nameLabel->setWordWrap(true);
    makeMouseTransparent(nameLabel);
    headLayout->addWidget(nameLabel, 1);

    QLabel *meta = plainLabel(QStringLiteral("memoryToolMeta"),
                              QStringLiteral("%1 个字段 · 更新于 %2")
                                  .arg(fields.size())
                                  .arg(memoryTimeText(tool.value(QStringLiteral("updated_at")))),
                              head);
    makeMouseTransparent(meta);
    headLayout->addWidget(meta, 0, Qt::AlignVCenter);

    QPushButton *clear = linkButton(QStringLiteral("清除"), QStringLiteral("danger"), head);
    clear->setEnabled(!m_busy);
    connect(clear, &QPushButton::clicked, this, [this, name] { clearTool(name); });
    headLayout->addWidget(clear, 0, Qt::AlignVCenter);
    m_toolButtons << clear;

    layout->addWidget(head);

    auto *body = new QWidget(block);
    setClass(body, QStringLiteral("memoryToolBody"));
    body->setAttribute(Qt::WA_StyledBackground, true);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(10, 6, 10, 10);
    bodyLayout->setSpacing(0);
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
        bodyLayout->addWidget(buildFieldRow(name, it.key(), it.value()));
    body->setVisible(open);
    layout->addWidget(body);

    head->activated = [this, name, body, chevron] {
        const bool nowOpen = !m_openTools.contains(name);
        if (nowOpen)
            m_openTools.insert(name);
        else
            m_openTools.remove(name);
        body->setVisible(nowOpen);
        chevron->setOpen(nowOpen);
    };
    return block;
}

QWidget *MemoryPanel::buildFieldRow(const QString &tool, const QString &field,
                                    const QVariant &value)
{
    const bool editing = m_editTool == tool && m_editField == field;

    auto *row = new QWidget;
    setClass(row, QStringLiteral("memoryField"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 6, 0, 6);
    layout->setSpacing(7);

    QLabel *nameLabel = plainLabel(QStringLiteral("memoryFieldName"), field, row);
    layout->addWidget(nameLabel, 0, Qt::AlignVCenter);
    layout->addWidget(plainLabel(QStringLiteral("memoryFieldSep"), QStringLiteral("="), row), 0,
                      Qt::AlignVCenter);

    if (editing) {
        auto *input = new QLineEdit(row);
        setClass(input, QStringLiteral("memoryFieldInput"));
        input->setText(fieldEditText(value));
        input->setAccessibleName(field);
        input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        layout->addWidget(input, 1);

        QLabel *typeLabel = plainLabel(QStringLiteral("memoryFieldType"), fieldTypeLabel(value), row);
        layout->addWidget(typeLabel, 0, Qt::AlignVCenter);

        QPushButton *save = linkButton(QStringLiteral("保存"), QStringLiteral("approve"), row);
        QPushButton *cancel = linkButton(QStringLiteral("取消"), QString(), row);
        layout->addWidget(save, 0);
        layout->addWidget(cancel, 0);
        m_toolButtons << save << cancel;

        connect(save, &QPushButton::clicked, this,
                [this, tool, field, input] { saveField(tool, field, input); });
        connect(input, &QLineEdit::returnPressed, this,
                [this, tool, field, input] { saveField(tool, field, input); });
        auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), input);
        escape->setContext(Qt::WidgetShortcut);
        connect(escape, &QShortcut::activated, this, [this] { cancelFieldEdit(); });

        // 重建之后焦点回到输入框（webui memoryRenderTools 末尾的 focus + select）
        QTimer::singleShot(0, input, [input] {
            input->setFocus();
            input->selectAll();
        });
    } else {
        QLabel *valueLabel = plainLabel(QStringLiteral("memoryFieldValue"), fieldValueText(value), row);
        valueLabel->setWordWrap(true);
        valueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(valueLabel, 1, Qt::AlignVCenter);

        QPushButton *edit = linkButton(QStringLiteral("编辑"), QString(), row);
        QPushButton *remove = linkButton(QStringLiteral("删除"), QStringLiteral("danger"), row);
        edit->setEnabled(!m_busy);
        remove->setEnabled(!m_busy);
        layout->addWidget(edit, 0);
        layout->addWidget(remove, 0);
        m_toolButtons << edit << remove;

        connect(edit, &QPushButton::clicked, this,
                [this, tool, field] { startFieldEdit(tool, field); });
        connect(remove, &QPushButton::clicked, this,
                [this, tool, field] { deleteField(tool, field); });
    }
    return row;
}

// ------------------------------------------------------------ 渲染：维护

void MemoryPanel::renderMaintenance()
{
    const QVariantMap maint = m_summary.value(QStringLiteral("maintenance")).toMap();
    const bool running = maint.value(QStringLiteral("state")).toString() == QLatin1String("running");

    if (m_runExtract)
        m_runExtract->setEnabled(!running && !m_busy);
    if (m_runGovernance)
        m_runGovernance->setEnabled(!running && !m_busy);
    if (m_clearTools)
        m_clearTools->setEnabled(!m_busy);
    if (!m_maintResult)
        return;

    QString text;
    if (running) {
        const QString step = maint.value(QStringLiteral("step")).toString();
        text = QStringLiteral("维护运行中：%1…")
                   .arg(step.isEmpty() ? QStringLiteral("准备") : step);
    } else {
        const QString lastError = maint.value(QStringLiteral("last_error")).toString().trimmed();
        const QVariantMap result = maint.value(QStringLiteral("last_result")).toMap();
        const QString finished = memoryTimeText(maint.value(QStringLiteral("finished_at")));
        if (!lastError.isEmpty()) {
            text = QStringLiteral("上次维护失败（%1）。").arg(lastError);
        } else if (result.isEmpty() || finished.isEmpty()) {
            text = QStringLiteral("还没有运行过维护任务。");
        } else {
            QStringList bits;
            bits << QStringLiteral("上次维护 %1").arg(finished);
            const char *const keys[] = { "merged", "flagged", "archived",
                                         "submitted", "candidates", "facts" };
            for (const char *key : keys) {
                const QVariant value = result.value(QString::fromLatin1(key));
                if (value.isValid() && isNumberVariant(value))
                    bits << QStringLiteral("%1 %2").arg(QString::fromLatin1(key), value.toString());
            }
            text = bits.join(QStringLiteral(" · ")) + QStringLiteral("。");
        }
    }
    m_maintResult->setText(text);
}

void MemoryPanel::renderAll()
{
    renderStatus();
    renderSwitches();
    renderFilters();
    renderList();
    renderTools();
    renderMaintenance();
}

// ------------------------------------------------------------ 请求

QVariantMap MemoryPanel::currentFilters() const
{
    QVariantMap filters;
    const QString status = m_statusFilter ? m_statusFilter->currentData().toString() : QString();
    const QString kind = m_kindFilter ? m_kindFilter->currentData().toString() : QString();
    const QString scope = m_scopeFilter ? m_scopeFilter->currentData().toString() : QString();
    const QString query = m_search ? m_search->text().trimmed() : QString();
    // 空值键一律不下发：状态「全部」的 value 是空串，传过去后端会当成筛选条件
    if (!status.isEmpty())
        filters.insert(QStringLiteral("status"), status);
    if (!kind.isEmpty())
        filters.insert(QStringLiteral("kind"), kind);
    if (!scope.isEmpty())
        filters.insert(QStringLiteral("scope"), scope);
    if (!query.isEmpty())
        filters.insert(QStringLiteral("q"), query);
    filters.insert(QStringLiteral("page_size"), m_pageSize > 0 ? m_pageSize : kPageSize);
    return filters;
}

void MemoryPanel::requestPage(int page)
{
    const int target = page < 1 ? 1 : page;
    // 回到第 1 页时清空选择（webui 口径：加载更多不清，只有重查才清）。
    // 翻到后续页时不清是因为「本页全选」不该因为翻页而失效；
    // 已不在列表里的选中项由 setListing() 里的 pruneSelection() 收掉。
    if (target == 1)
        clearSelection();
    if (!m_loaded) {
        m_loading = true;
        renderStatus();
    }
    emit listRequested(target, currentFilters());
}

void MemoryPanel::reloadFromFirstPage()
{
    requestPage(1);
}

void MemoryPanel::clearSelection()
{
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    for (auto it = m_cardChecks.constBegin(); it != m_cardChecks.constEnd(); ++it) {
        QSignalBlocker blocker(it.value());
        it.value()->setChecked(false);
    }
    renderBulk();
}

void MemoryPanel::pruneSelection()
{
    QSet<QString> visible;
    for (const QVariant &entry : m_items)
        visible.insert(entry.toMap().value(QStringLiteral("id")).toString());
    QSet<QString> kept;
    for (const QString &id : m_selected) {
        if (visible.contains(id))
            kept.insert(id);
    }
    m_selected = kept;
}

QVariant MemoryPanel::storedValue(const QString &tool, const QString &field) const
{
    const QVariantList tools = m_tools.value(QStringLiteral("tools")).toList();
    for (const QVariant &entry : tools) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("tool")).toString() != tool)
            continue;
        const QVariantMap fields = map.value(QStringLiteral("fields")).toMap();
        return fields.value(field);
    }
    return QVariant();
}

// ------------------------------------------------------------ 交互：经验条目

void MemoryPanel::actOnItem(const QString &id, const QString &action)
{
    if (m_busy)
        return;
    struct Spec {
        const char *action;
        const char *label;
        const char *message;
        bool danger;
    };
    static const Spec specs[] = {
        { "approve", "通过", "通过后这条记忆才可能被未来的任务参考。", false },
        { "reject", "拒绝", "拒绝后它不会再出现在待审核列表，相同内容也不会被重复提案。", true },
        { "deprecate", "停用", "停用后它不再被参考，但记录和审计会保留。", true },
        { "restore", "恢复", "恢复后它会重新参与检索。", false },
        { "delete", "删除", "删除不可恢复，也无法再被检索到。", true },
    };
    const Spec *spec = nullptr;
    for (const Spec &entry : specs) {
        if (action == QLatin1String(entry.action)) {
            spec = &entry;
            break;
        }
    }
    const QString label = spec ? QString::fromUtf8(spec->label) : action;
    const QString message = spec ? QString::fromUtf8(spec->message) : QString();
    const bool danger = spec ? spec->danger : false;
    if (!ConfirmDialog::ask(this, QStringLiteral("确定要%1这条记忆吗？").arg(label), message, label,
                            danger))
        return;
    emit actionRequested(id, action);
}

void MemoryPanel::runBulk(const QString &action)
{
    if (m_busy || m_selected.isEmpty())
        return;
    QStringList ids = m_selected.values();
    ids.sort();
    const bool reject = action == QLatin1String("reject");
    const QString label = reject ? QStringLiteral("拒绝") : QStringLiteral("通过");
    if (!ConfirmDialog::ask(this, QStringLiteral("批量%1").arg(label),
                            QStringLiteral("将对 %1 条经验执行该操作，是否继续？").arg(ids.size()),
                            label, reject))
        return;
    emit bulkRequested(ids, action);
}

// ------------------------------------------------------------ 交互：参数记忆

void MemoryPanel::startFieldEdit(const QString &tool, const QString &field)
{
    if (m_busy)
        return;
    m_editTool = tool;
    m_editField = field;
    // webui memoryStartFieldEdit：编辑就一定展开这块
    m_openTools.insert(tool);
    renderTools();
}

void MemoryPanel::cancelFieldEdit()
{
    if (m_editTool.isEmpty() && m_editField.isEmpty())
        return;
    m_editTool.clear();
    m_editField.clear();
    // 取消即放弃这次保存的回执对账，别让晚到的 setTools 再改编辑态
    m_pendingTool.clear();
    m_pendingField.clear();
    m_pendingValue = QVariant();
    renderTools();
}

void MemoryPanel::saveField(const QString &tool, const QString &field, QLineEdit *input)
{
    if (m_busy || !input)
        return;
    const QVariant original = storedValue(tool, field);
    QVariant parsed;
    QString error;
    if (!parseFieldValue(input->text(), original, parsed, error)) {
        // 值不合法就留在编辑态，把错误挂在输入框上（webui 走 showToast + 回焦）
        input->setToolTip(error);
        input->setProperty("invalid", true);
        restyle(input);
        input->setFocus();
        input->selectAll();
        return;
    }
    if (sameValue(parsed, original)) {
        cancelFieldEdit();
        return;
    }
    input->setProperty("invalid", false);
    restyle(input);
    m_pendingTool = tool;
    m_pendingField = field;
    m_pendingValue = parsed;
    emit fieldSaveRequested(tool, field, parsed);
}

void MemoryPanel::deleteField(const QString &tool, const QString &field)
{
    if (m_busy)
        return;
    if (!ConfirmDialog::ask(this, QStringLiteral("删除记住的参数"),
                            QStringLiteral("将删除 %1 记住的 %2，后续调用不再自动建议这个值。")
                                .arg(tool, field),
                            QStringLiteral("删除"), true))
        return;
    emit fieldDeleteRequested(tool, field);
}

void MemoryPanel::clearTool(const QString &tool)
{
    if (m_busy)
        return;
    if (!ConfirmDialog::ask(this, QStringLiteral("清除这个工具的参数记忆"),
                            QStringLiteral("将删除 %1 记住的全部参数，后续调用不再自动建议。")
                                .arg(tool),
                            QStringLiteral("清除"), true))
        return;
    emit toolClearRequested(tool);
}

void MemoryPanel::clearAllTools()
{
    if (m_busy)
        return;
    const int tools = m_tools.value(QStringLiteral("tool_count")).toInt();
    QVariant fieldCount = m_tools.value(QStringLiteral("field_count"));
    int fields = fieldCount.toInt();
    if (!fieldCount.isValid()) {
        fields = 0;
        const QVariantList list = m_tools.value(QStringLiteral("tools")).toList();
        for (const QVariant &entry : list)
            fields += entry.toMap().value(QStringLiteral("fields")).toMap().size();
    }
    if (!ConfirmDialog::ask(this, QStringLiteral("清空全部工具参数记忆"),
                            QStringLiteral("将删除 %1 个工具记住的 %2 个字段，不可撤销。"
                                           "后续调用会退回参数默认值。")
                                .arg(tools)
                                .arg(fields),
                            QStringLiteral("清空"), true))
        return;
    emit toolsClearAllRequested();
}

// ------------------------------------------------------------ 交互：维护

bool MemoryPanel::maintenanceRunning() const
{
    const QVariantMap maint = m_summary.value(QStringLiteral("maintenance")).toMap();
    return maint.value(QStringLiteral("state")).toString() == QLatin1String("running");
}

void MemoryPanel::startMaintenance(const QString &kind)
{
    if (m_busy || maintenanceRunning())
        return;
    emit maintenanceRequested(kind);
    // 投递之后自己先盯一会儿：summary 的 state 翻成 running 之前也能把进度刷出来
    m_pollArmUntil = QDateTime::currentMSecsSinceEpoch() + kPollArmMs;
    syncPolling();
}

void MemoryPanel::syncPolling()
{
    if (!m_pollTimer)
        return;
    const bool armed = QDateTime::currentMSecsSinceEpoch() < m_pollArmUntil;
    const bool want = m_tabActive && (maintenanceRunning() || armed);
    if (want && !m_pollTimer->isActive())
        m_pollTimer->start(kPollIntervalMs);
    else if (!want && m_pollTimer->isActive())
        m_pollTimer->stop();
}

// ------------------------------------------------------------ 宿主回填

void MemoryPanel::enterTab()
{
    m_tabActive = true;
    if (!m_loaded) {
        m_loading = true;
        renderStatus();
        // 首次进入：列表由本类发起（refreshRequested 只约定 summary + tool-parameters）
        emit listRequested(1, currentFilters());
    }
    emit refreshRequested();
    syncPolling();
}

void MemoryPanel::leaveTab()
{
    m_tabActive = false;
    if (m_searchTimer)
        m_searchTimer->stop();
    syncPolling();
}

void MemoryPanel::setSummary(const QVariantMap &summary)
{
    const bool wasRunning = maintenanceRunning();
    m_summary = summary;
    m_loaded = true;
    m_loading = false;
    m_error.clear();
    renderStatus();
    renderSwitches();
    renderMaintenance();
    syncPolling();
    // 维护从 running 回到空闲：候选列表已经变了，按 webui memoryPollTick 重拉第一页
    if (wasRunning && !maintenanceRunning())
        reloadFromFirstPage();
}

void MemoryPanel::setListing(const QVariantMap &listing)
{
    const QVariantList incoming = listing.value(QStringLiteral("items")).toList();
    int page = listing.value(QStringLiteral("page")).toInt();
    if (page <= 0)
        page = 1;
    const bool append = page > 1;

    if (!append) {
        m_items = incoming;
    } else {
        // 附加页：同一页被重复请求时不要重复堆同一批条目
        QSet<QString> known;
        for (const QVariant &entry : m_items)
            known.insert(entry.toMap().value(QStringLiteral("id")).toString());
        for (const QVariant &entry : incoming) {
            const QString id = entry.toMap().value(QStringLiteral("id")).toString();
            if (id.isEmpty() || known.contains(id))
                continue;
            m_items.append(entry);
        }
    }
    m_total = listing.value(QStringLiteral("total")).toInt();
    m_page = page;
    const int pageSize = listing.value(QStringLiteral("page_size")).toInt();
    if (pageSize > 0)
        m_pageSize = pageSize;
    m_facets = listing.value(QStringLiteral("facets")).toMap();
    m_error.clear();
    m_loading = false;
    m_loaded = true;

    pruneSelection();
    renderFilters();
    renderList();
    renderStatus();
}

void MemoryPanel::setListFailed(const QString &error)
{
    m_error = error;
    m_loading = false;
    renderList();
    renderStatus();
}

void MemoryPanel::setTools(const QVariantMap &payload)
{
    m_tools = payload;
    m_toolsLoaded = true;

    // 保存过的那一行：后端真改了才退出编辑态，否则说明这次保存没成，输入框留着让人直接改
    if (!m_pendingField.isEmpty()) {
        const QVariant stored = storedValue(m_pendingTool, m_pendingField);
        if (!stored.isValid() || sameValue(stored, m_pendingValue)) {
            if (m_editTool == m_pendingTool && m_editField == m_pendingField) {
                m_editTool.clear();
                m_editField.clear();
            }
        }
        m_pendingTool.clear();
        m_pendingField.clear();
        m_pendingValue = QVariant();
    }
    renderTools();
    renderStatus();
}

void MemoryPanel::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    // 只切可用性，不重建：重建会把正在编辑的输入框连同没提交的文本一起扔掉
    applySwitchEnabled();
    renderBulk();
    renderMaintenance();
    if (m_refresh)
        m_refresh->setEnabled(!busy);
    if (m_loadMore)
        m_loadMore->setEnabled(!busy && m_items.size() < m_total && !m_items.isEmpty());
    for (QPushButton *button : m_cardButtons) {
        if (button)
            button->setEnabled(!busy);
    }
    for (QCheckBox *box : m_cardChecks) {
        if (box)
            box->setEnabled(!busy);
    }
    for (QPushButton *button : m_toolButtons) {
        if (button)
            button->setEnabled(!busy);
    }
    renderStatus();
}

void MemoryPanel::setStatusText(const QString &text)
{
    if (m_statusText == text)
        return;
    m_statusText = text;
    renderStatus();
}

} // namespace gs
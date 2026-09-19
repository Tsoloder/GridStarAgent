#include "messagewidgets.h"
#include "commonwidgets.h"
#include "markdownview.h"
#include "theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace gs {

// ---------------------------------------------------------------- JSON 工具

static QString compactJson(const QVariant &value)
{
    const QJsonDocument doc = QJsonDocument::fromVariant(value);
    if (doc.isNull())
        return value.toString();
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

QString prettyJson(const QVariant &value)
{
    const QJsonDocument doc = QJsonDocument::fromVariant(value);
    if (doc.isNull())
        return value.toString();
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
}

QString valueForInput(const QVariant &value)
{
    switch (value.type()) {
    case QVariant::Map:
    case QVariant::Hash:
    case QVariant::List:
    case QVariant::StringList:
        return compactJson(value);
    default:
        return value.toString();
    }
}

static QVariant parsedJsonOrText(const QString &text)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || doc.isNull())
        return text;
    return doc.toVariant();
}

QVariant coerceValue(const QString &text, const QVariant &original)
{
    switch (original.type()) {
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
    case QVariant::Double: {
        bool ok = false;
        const double number = text.toDouble(&ok);
        return ok ? QVariant(number) : QVariant(text);
    }
    case QVariant::Bool:
        return QVariant(text == QLatin1String("true"));
    case QVariant::Map:
    case QVariant::Hash:
    case QVariant::List:
    case QVariant::StringList:
        return parsedJsonOrText(text);
    default:
        return QVariant(text);
    }
}

QVariant coerceSchemaValue(const QString &text, const QString &type)
{
    const QString trimmed = text.trimmed();
    if (type == QLatin1String("number") || type == QLatin1String("integer")) {
        bool ok = false;
        const double number = trimmed.toDouble(&ok);
        return ok ? QVariant(number) : QVariant(text);
    }
    if (type == QLatin1String("boolean")) {
        const QString lower = trimmed.toLower();
        return QVariant(lower == QLatin1String("true") || lower == QLatin1String("1")
                        || lower == QLatin1String("yes"));
    }
    if (type == QLatin1String("object") || type == QLatin1String("array"))
        return parsedJsonOrText(trimmed);
    return QVariant(text);
}

QString attachExt(const QString &name)
{
    const int i = name.lastIndexOf(QLatin1Char('.'));
    if (i >= 0 && i < name.size() - 1)
        return name.mid(i + 1).toLower();
    return QString();
}

// app.js formatClock：HH:MM:SS，空/非法值返回空串
QString formatClock(const QString &isoOrEmpty)
{
    if (isoOrEmpty.isEmpty())
        return QString();
    QDateTime dt = QDateTime::fromString(isoOrEmpty, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(isoOrEmpty, Qt::ISODate);
    if (!dt.isValid())
        return QString();
    return dt.toLocalTime().toString(QStringLiteral("HH:mm:ss"));
}

// app.js formatDurationCn：820毫秒 / 1.4秒 / 2分5秒
QString formatDurationCn(qint64 ms)
{
    if (ms < 0)
        return QString();
    if (ms < 1000)
        return QStringLiteral("%1毫秒").arg(ms);
    if (ms < 60000)
        return QStringLiteral("%1秒").arg(QString::number(ms / 1000.0, 'f', 1));
    const qint64 m = ms / 60000;
    const qint64 s = qRound((ms % 60000) / 1000.0);
    return QStringLiteral("%1分%2秒").arg(m).arg(s);
}

// app.js formatInt：千分位
QString formatInt(qint64 value)
{
    const QString text = QString::number(value);
    QString out;
    int count = 0;
    for (int i = text.size() - 1; i >= 0; --i) {
        out.prepend(text.at(i));
        if (++count % 3 == 0 && i > 0)
            out.prepend(QLatin1Char(','));
    }
    return out;
}

// ------------------------------------------------------------ 通用小构件

static QString optionVariant(const QString &style)
{
    if (style == QLatin1String("primary"))
        return QStringLiteral("optionPrimary");
    if (style == QLatin1String("danger"))
        return QStringLiteral("optionDanger");
    return QStringLiteral("option");
}

static QPushButton *makeOptionButton(const QString &label, const QString &style, QWidget *parent)
{
    auto *button = new QPushButton(label, parent);
    button->setProperty("variant", optionVariant(style));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

// 参数行标签：<b>name</b> + desc（对应 .param-row label）。
// 字号与配色取自当前调色板，并按缩放档计算。
static QLabel *makeParamLabel(const QString &boldPart, const QString &descPart, QWidget *parent)
{
    const Palette &pal = palette();
    QString html = QStringLiteral("<span style=\"color:%1;font-size:%2px;font-weight:600;\">%3</span>")
                       .arg(cssColor(pal.text))
                       .arg(scaledPx(12))
                       .arg(escapeHtml(boldPart));
    if (!descPart.isEmpty())
        html += QStringLiteral("<br><span style=\"color:%1;font-size:%2px;\">%3</span>")
                    .arg(cssColor(pal.muted))
                    .arg(scaledPx(11))
                    .arg(escapeHtml(descPart));
    QLabel *label = makeLabel(QStringLiteral("paramName"), html, parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setProperty("paramBold", boldPart);
    label->setProperty("paramDesc", descPart);
    return label;
}

// .param-row：grid-template-columns minmax(85px,1fr) minmax(100px,1.3fr); gap 7px; padding 5px 0
static QWidget *makeParamRow(QWidget *left, QWidget *right, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *grid = new QGridLayout(row);
    grid->setContentsMargins(0, 5, 0, 5);
    grid->setHorizontalSpacing(7);
    grid->setVerticalSpacing(0);
    grid->setColumnStretch(0, 10);
    grid->setColumnStretch(1, 13);
    grid->addWidget(left, 0, 0);
    grid->addWidget(right, 0, 1);
    return row;
}

static QWidget *makeOptionsFlow(QWidget *parent)
{
    auto *flow = new QWidget(parent);
    auto *layout = new FlowLayout(flow, 0, 6, 6);
    layout->setContentsMargins(8, 2, 2, 8); // CSS .options padding 8px 2px 2px 8px
    return flow;
}

// 可点击头部内的标签：makeLabel 默认允许选中文本，会吃掉点击导致头部 eventFilter 收不到事件
static QLabel *headLabel(const QString &className, const QString &text, QWidget *parent)
{
    QLabel *l = makeLabel(className, text, parent);
    l->setTextInteractionFlags(Qt::NoTextInteraction);
    return l;
}

// ------------------------------------------------------- 气泡明细弹层（Popup）

// .bubble-timing-pop / .bubble-usage-pop：挂在信息行按钮上方的小浮层。
// 用 Qt::Popup 独立窗口，避免被消息区 overflow 裁掉；点外部自动收起（Qt 原生行为）。
class DetailPop : public QFrame
{
public:
    explicit DetailPop(QWidget *parent = nullptr) : QFrame(parent)
    {
        setClass(this, QStringLiteral("bubblePop"));
        setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_StyledBackground, true);
        setMinimumWidth(216);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(0);
        m_layout = layout;

        auto *titleRow = new QWidget(this);
        auto *tl = new QHBoxLayout(titleRow);
        tl->setContentsMargins(0, 0, 0, 7);
        tl->setSpacing(6);
        m_title = makeLabel(QStringLiteral("popTitle"), QString(), titleRow);
        m_title->setTextInteractionFlags(Qt::NoTextInteraction);
        m_total = makeLabel(QStringLiteral("popTotal"), QString(), titleRow);
        m_total->setTextInteractionFlags(Qt::NoTextInteraction);
        tl->addWidget(m_title, 1);
        tl->addWidget(m_total, 0);
        m_layout->addWidget(titleRow);
        m_titleRow = titleRow;
    }

    void configure(const QString &title, const QList<QPair<QString, QString>> &rows)
    {
        m_title->setText(title);
        for (auto it = m_rows.constBegin(); it != m_rows.constEnd(); ++it) {
            m_layout->removeWidget(it.value().first);
            it.value().first->deleteLater();
        }
        m_rows.clear();
        for (const QPair<QString, QString> &row : rows) {
            auto *widget = new QWidget(this);
            auto *hl = new QHBoxLayout(widget);
            hl->setContentsMargins(0, 3, 0, 3);
            hl->setSpacing(18);
            QLabel *key = makeLabel(QStringLiteral("popRowLabel"), row.second, widget);
            key->setTextInteractionFlags(Qt::NoTextInteraction);
            QLabel *value = makeLabel(QStringLiteral("popRowValue"), QString(), widget);
            value->setTextInteractionFlags(Qt::NoTextInteraction);
            hl->addWidget(key, 1);
            hl->addWidget(value, 0);
            m_layout->addWidget(widget);
            m_rows.insert(row.first, qMakePair(widget, value));
        }
    }

    // values 中缺失或为空的键对应的行整体隐藏（与 app.js row.hidden 一致）
    void setValues(const QVariantMap &values)
    {
        for (auto it = m_rows.constBegin(); it != m_rows.constEnd(); ++it) {
            const QVariant value = values.value(it.key());
            const QString text = value.toString();
            it.value().first->setVisible(!text.isEmpty());
            it.value().second->setText(text);
        }
        adjustSize();
    }

    void setTitleTotal(const QString &text)
    {
        m_total->setText(text);
        m_total->setVisible(!text.isEmpty());
    }

    void popupAbove(QWidget *anchor)
    {
        adjustSize();
        const QPoint topLeft = anchor->mapToGlobal(QPoint(0, 0));
        int x = topLeft.x();
        int y = topLeft.y() - height() - 7;
        if (QWidget *host = anchor->window()) {
            const QRect hostRect = host->geometry();
            x = qBound(hostRect.left() + 6, x, hostRect.right() - width() - 6);
            y = qMax(hostRect.top() + 6, y);
        }
        move(x, y);
        show();
    }

private:
    QVBoxLayout *m_layout = nullptr;
    QWidget *m_titleRow = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_total = nullptr;
    // key → (行容器, 值标签)
    QHash<QString, QPair<QWidget *, QLabel *>> m_rows;
};

// ------------------------------------------------------------------ ProcRow

ProcRow::ProcRow(const QString &key, const QString &label, QWidget *parent)
    : QWidget(parent), m_key(key)
{
    setClass(this, QStringLiteral("procRow"));
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("proc", key);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_head = new QWidget(this);
    setClass(m_head, QStringLiteral("procRowHead"));
    m_head->setCursor(Qt::PointingHandCursor);
    m_head->setAttribute(Qt::WA_Hover, true);
    auto *hl = new QHBoxLayout(m_head);
    hl->setContentsMargins(0, 3, 0, 3);
    hl->setSpacing(6);

    m_chevron = new Chevron(m_head);
    m_chevron->setFixedSize(8, 8);
    m_dot = new PulseDot(m_head);
    m_dot->setColor(key == QLatin1String("tools") ? gs::palette().orange : gs::palette().cyan);
    m_dot->setVisible(false);
    m_label = headLabel(QStringLiteral("procLabel"), label, m_head);
    m_summary = headLabel(QStringLiteral("procSum"), QString(), m_head);
    hl->addWidget(m_chevron, 0);
    hl->addWidget(m_dot, 0);
    hl->addWidget(m_label, 0);
    hl->addWidget(m_summary, 1);

    // .proc-body { padding:2px 0 6px 12px }，内含 .proc-rail（左侧 1px 竖线 + padding-left 10px）
    m_body = new QWidget(this);
    setClass(m_body, QStringLiteral("procBody"));
    auto *bl = new QVBoxLayout(m_body);
    bl->setContentsMargins(12, 2, 0, 6);
    bl->setSpacing(0);
    m_rail = new QWidget(m_body);
    setClass(m_rail, QStringLiteral("procRail"));
    m_rail->setAttribute(Qt::WA_StyledBackground, true);
    m_railLayout = new QVBoxLayout(m_rail);
    m_railLayout->setContentsMargins(10, 0, 0, 0);
    m_railLayout->setSpacing(0);
    m_railLayout->setAlignment(Qt::AlignTop);
    bl->addWidget(m_rail);
    m_body->setVisible(false);

    layout->addWidget(m_head);
    layout->addWidget(m_body);

    // webui 的 .proc-row 是 role="button" tabindex="0"：键盘也要能展开
    m_head->setFocusPolicy(Qt::TabFocus);
    m_head->installEventFilter(this);
}

void ProcRow::setOpen(bool open)
{
    m_open = open;
    m_chevron->setOpen(open);
    m_body->setVisible(open);
}

void ProcRow::setSummary(const QString &text)
{
    m_summary->setText(text);
    m_summary->setToolTip(text);
}

void ProcRow::setRunning(bool running)
{
    if (property("running").toBool() == running)
        return;
    setProperty("running", running);
    m_chevron->setVisible(!running);
    m_dot->setVisible(running);
    m_dot->setActive(running);
    // 流式思考的摘要在运行态贴右，最新吐出的字始终可见
    if (m_key == QLatin1String("think"))
        m_summary->setAlignment(running ? (Qt::AlignRight | Qt::AlignVCenter)
                                        : (Qt::AlignLeft | Qt::AlignVCenter));
    restyle(this);
}

void ProcRow::setFailed(bool failed)
{
    if (property("failed").toBool() == failed)
        return;
    setProperty("failed", failed);
    restyle(this);
}

bool ProcRow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_head) {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton)
                setOpen(!m_open);
            return true;
        }
        // Enter / Space 与点击等价（webui 的 role=button 也吃这两个键）
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
                setOpen(!m_open);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ------------------------------------------------------------ ToolItemWidget

// 工具返回值多是 JSON 字符串：能解析就缩进美化，嵌套的 JSON 字符串一并展开
static QVariant expandJsonStrings(const QVariant &value, int depth)
{
    if (depth <= 0)
        return value;
    if (value.type() == QVariant::String) {
        const QString text = value.toString().trimmed();
        if (text.isEmpty() || (text.at(0) != QLatin1Char('{') && text.at(0) != QLatin1Char('[')))
            return value;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError)
            return value;
        return expandJsonStrings(doc.toVariant(), depth - 1);
    }
    if (value.type() == QVariant::List) {
        QVariantList out;
        for (const QVariant &item : value.toList())
            out.append(expandJsonStrings(item, depth - 1));
        return out;
    }
    if (value.type() == QVariant::Map) {
        QVariantMap out;
        const QVariantMap map = value.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it)
            out.insert(it.key(), expandJsonStrings(it.value(), depth - 1));
        return out;
    }
    return value;
}

static QString formatToolResult(const QString &raw)
{
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty() || (trimmed.at(0) != QLatin1Char('{') && trimmed.at(0) != QLatin1Char('[')))
        return raw;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError)
        return raw;
    const QJsonDocument expanded = QJsonDocument::fromVariant(expandJsonStrings(doc.toVariant(), 3));
    return QString::fromUtf8(expanded.toJson(QJsonDocument::Indented)).trimmed();
}

// .tool-args-table：两列表格（参数 / 值），结构化值用等宽 JSON 展开
static QWidget *makeArgsTable(const QVariant &args, QWidget *parent)
{
    auto *box = new QFrame(parent);
    box->setObjectName(QStringLiteral("toolArgsBox"));
    box->setAttribute(Qt::WA_StyledBackground, true);

    auto *grid = new QGridLayout(box);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(0);
    grid->setVerticalSpacing(0);
    grid->setColumnStretch(0, 38);
    grid->setColumnStretch(1, 62);

    const QVariantMap map = args.type() == QVariant::Map ? args.toMap() : QVariantMap();
    if (map.isEmpty()) {
        auto *empty = makeLabel(QStringLiteral("toolArgsEmpty"), QStringLiteral("（无参数）"), box);
        grid->addWidget(empty, 0, 0, 1, 2);
        return box;
    }

    QLabel *headKey = headLabel(QStringLiteral("toolArgsHead"), QStringLiteral("参数"), box);
    QLabel *headValue = headLabel(QStringLiteral("toolArgsHead"), QStringLiteral("值"), box);
    headKey->setContentsMargins(8, 5, 8, 5);
    headValue->setContentsMargins(8, 5, 8, 5);
    grid->addWidget(headKey, 0, 0);
    grid->addWidget(headValue, 0, 1);

    int row = 1;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it, ++row) {
        const bool structured = it.value().type() == QVariant::Map
                                || it.value().type() == QVariant::List;
        QLabel *key = makeLabel(QStringLiteral("toolArgsKey"), it.key(), box);
        key->setTextInteractionFlags(Qt::TextSelectableByMouse);
        key->setContentsMargins(8, 5, 10, 5);
        key->setWordWrap(true);
        QLabel *value = makeLabel(QStringLiteral("toolArgsValue"),
                                  structured ? prettyJson(it.value()) : it.value().toString(), box);
        value->setProperty("json", structured);
        value->setContentsMargins(8, 5, 8, 5);
        value->setWordWrap(true);
        grid->addWidget(key, row, 0);
        grid->addWidget(value, row, 1);
    }
    return box;
}

// CSS 的 max-height + overflow:auto → 限高滚动区（工具参数表 180px）
static QWidget *wrapScrollable(QWidget *content, int maxHeight, QWidget *parent)
{
    auto *scroll = new QScrollArea(parent);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMaximumHeight(maxHeight);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(content);
    return scroll;
}

ToolItemWidget::ToolItemWidget(const QString &callId, const QString &name, const QVariant &args,
                               QWidget *parent)
    : QWidget(parent), m_callId(callId)
{
    setClass(this, QStringLiteral("toolItem"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *summary = new QWidget(this);
    setClass(summary, QStringLiteral("toolItemSummary"));
    summary->setAttribute(Qt::WA_StyledBackground, true);
    summary->setCursor(Qt::PointingHandCursor);
    auto *sl = new QHBoxLayout(summary);
    sl->setContentsMargins(0, 3, 0, 3);
    sl->setSpacing(8);
    m_dot = new StatusDot(summary);
    m_dot->setState(QStringLiteral("running"));
    QLabel *nameLabel = headLabel(QStringLiteral("toolItemName"),
                                  name.isEmpty() ? QStringLiteral("工具调用") : name, summary);
    m_status = headLabel(QStringLiteral("statusLabel"), QStringLiteral("执行中"), summary);
    sl->addWidget(m_dot, 0);
    sl->addWidget(nameLabel, 1);
    sl->addWidget(m_status, 0);
    m_summary = summary;
    // 工具项标题行同样要能 Tab 到并用 Enter / Space 展开
    summary->setFocusPolicy(Qt::TabFocus);
    summary->installEventFilter(this);

    m_detail = new QWidget(this);
    auto *dl = new QVBoxLayout(m_detail);
    dl->setContentsMargins(0, 0, 0, 6);
    dl->setSpacing(0);

    dl->addWidget(makeLabel(QStringLiteral("toolDetailLabel"), QStringLiteral("调用参数"), m_detail));
    dl->addWidget(wrapScrollable(makeArgsTable(args, m_detail), scaledPx(180), m_detail));

    m_resultWrap = new QWidget(m_detail);
    auto *rl = new QVBoxLayout(m_resultWrap);
    rl->setContentsMargins(0, 8, 0, 0);
    rl->setSpacing(4);
    auto *separator = new QFrame(m_resultWrap);
    separator->setFixedHeight(1);
    separator->setStyleSheet(QStringLiteral("background:%1;").arg(cssColor(gs::palette().line)));
    rl->addWidget(separator);
    rl->addWidget(makeLabel(QStringLiteral("toolDetailLabel"), QStringLiteral("调用结果"), m_resultWrap));
    // 结果限高滚动（CSS .tool-result pre { max-height:160px; overflow:auto }）
    m_result = makeLabel(QStringLiteral("toolPre"), QString(), m_resultWrap);
    m_result->setTextFormat(Qt::PlainText);
    m_result->setWordWrap(true);
    m_result->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    rl->addWidget(wrapScrollable(m_result, scaledPx(160), m_resultWrap));
    m_resultWrap->setVisible(false);
    dl->addWidget(m_resultWrap);

    m_detail->setVisible(false);

    layout->addWidget(m_summary);
    layout->addWidget(m_detail);
}

void ToolItemWidget::setOpen(bool open)
{
    m_open = open;
    m_detail->setVisible(open);
}

void ToolItemWidget::setState(const QString &state)
{
    m_state = state;
    m_dot->setState(state);
    if (state == QLatin1String("running")) {
        m_status->setText(QStringLiteral("执行中"));
        m_status->setProperty("status", QVariant());
    } else if (state == QLatin1String("failed")) {
        m_status->setText(QStringLiteral("失败"));
        m_status->setProperty("status", QStringLiteral("failed"));
    } else {
        m_status->setText(QStringLiteral("完成"));
        m_status->setProperty("status", QStringLiteral("succeeded"));
    }
    restyle(m_status);
}

void ToolItemWidget::setResult(const QString &result)
{
    m_result->setText(formatToolResult(result));
    m_resultWrap->setVisible(true);
}

bool ToolItemWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_summary) {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton)
                setOpen(!m_open);
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
                setOpen(!m_open);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------- WorkflowProposalCard

WorkflowProposalCard::WorkflowProposalCard(const QVariantList &steps, QWidget *parent)
    : QFrame(parent)
{
    setClass(this, QStringLiteral("structured"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(makeLabel(QStringLiteral("structuredTitle"),
                                QStringLiteral("静态工作流"), this));

    auto *paramsWrap = new QWidget(this);
    auto *paramsLayout = new QVBoxLayout(paramsWrap);
    paramsLayout->setContentsMargins(7, 7, 7, 7);
    paramsLayout->setSpacing(0);
    int index = 0;
    for (const QVariant &stepVar : steps) {
        const QVariantMap step = stepVar.toMap();
        ++index;
        QLabel *label = makeParamLabel(QStringLiteral("%1. %2").arg(index).arg(step.value(QStringLiteral("tool")).toString()),
                                       step.value(QStringLiteral("desc")).toString(), paramsWrap);
        QLabel *code = makeLabel(QStringLiteral("toolPre"),
                                 compactJson(step.value(QStringLiteral("params")).toMap()),
                                 paramsWrap);
        code->setTextFormat(Qt::PlainText);
        code->setWordWrap(true);
        paramsLayout->addWidget(makeParamRow(label, code, paramsWrap));
    }
    layout->addWidget(paramsWrap);

    auto *flow = makeOptionsFlow(this);
    QPushButton *run = makeOptionButton(QStringLiteral("执行工作流"), QStringLiteral("primary"), flow);
    run->setEnabled(!steps.isEmpty());
    connect(run, &QPushButton::clicked, this, [this, run, steps] {
        run->setEnabled(false);
        emit runRequested(steps);
    });
    static_cast<FlowLayout *>(flow->layout())->addWidget(run);
    layout->addWidget(flow);
}

// --------------------------------------------------------- WorkflowRunCard

WorkflowRunCard::WorkflowRunCard(QWidget *parent) : QFrame(parent)
{
    setClass(this, QStringLiteral("workflowCard"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *head = new QWidget(this);
    setClass(head, QStringLiteral("cardHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(8, 7, 8, 7);
    hl->setSpacing(8);
    QLabel *title = makeLabel(QStringLiteral("cardTitle"), QStringLiteral("工作流执行"), head);
    title->setWordWrap(true);
    m_status = makeLabel(QStringLiteral("statusLabel"), QStringLiteral("运行中"), head);
    hl->addWidget(title, 1);
    hl->addWidget(m_status, 0);
    layout->addWidget(head);

    m_body = new QWidget(this);
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(7, 7, 7, 7);
    m_bodyLayout->setSpacing(0);
    m_bodyLayout->setAlignment(Qt::AlignTop);
    layout->addWidget(m_body);
}

void WorkflowRunCard::setSteps(const QVariantList &steps)
{
    while (QLayoutItem *item = m_bodyLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            delete w;
        delete item;
    }
    for (int index = 0; index < steps.size(); ++index) {
        const QVariant stepVar = steps.at(index);
        if (!stepVar.isValid())
            continue; // 稀疏数组洞：app.js 重绘时 if (step) 跳过
        const QVariantMap step = stepVar.toMap();
        QLabel *label = makeParamLabel(QStringLiteral("%1. %2").arg(index + 1).arg(step.value(QStringLiteral("tool")).toString()),
                                       step.value(QStringLiteral("desc")).toString(), m_body);
        const QString status = step.value(QStringLiteral("status")).toString();
        QLabel *statusLabel = makeLabel(QStringLiteral("statusLabel"), status, m_body);
        statusLabel->setProperty("status", status);
        m_bodyLayout->addWidget(makeParamRow(label, statusLabel, m_body));
    }
}

void WorkflowRunCard::setStatus(const QString &status)
{
    m_status->setText(status);
    m_status->setProperty("status", status);
    restyle(m_status);
}

// ----------------------------------------------------------- MessageWidget

// app.js CARD_ROLES：只有这三种角色套 turn-card（工作流等走普通气泡）
static bool isCardRole(const QString &role)
{
    return role == QLatin1String("user") || role == QLatin1String("assistant")
           || role == QLatin1String("tool");
}

MessageWidget::MessageWidget(const QString &role, const QString &label, QWidget *parent)
    : QWidget(parent), m_role(role)
{
    setClass(this, QStringLiteral("messageWidget"));
    setAttribute(Qt::WA_StyledBackground, true);

    m_stack = new QVBoxLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->setSpacing(7);
    m_stack->setAlignment(role == QLatin1String("user") ? (Qt::AlignRight | Qt::AlignTop)
                                                       : (Qt::AlignLeft | Qt::AlignTop));

    m_bubble = new QFrame(this);
    m_bubble->setObjectName(QStringLiteral("bubble"));
    m_bubbleLayout = new QVBoxLayout(m_bubble);
    m_bubbleLayout->setContentsMargins(12, 10, 12, 0);
    m_bubbleLayout->setSpacing(0);

    if (!label.isEmpty())
        setLabel(label);

    m_body = new MarkdownView(m_bubble);
    m_bubbleLayout->addWidget(m_body);

    if (isCardRole(role)) {
        m_card = new QFrame(this);
        m_card->setObjectName(QStringLiteral("turnCard"));
        m_card->setProperty("role", role);
        m_card->setAttribute(Qt::WA_StyledBackground, true);
        m_cardLayout = new QVBoxLayout(m_card);
        m_cardLayout->setContentsMargins(0, 0, 0, 0);
        m_cardLayout->setSpacing(0);
        m_cardLayout->addWidget(m_bubble);

        // 过程区（思考 / 工具调用）：卡内、正文之下、信息行之上
        m_process = new QWidget(m_card);
        m_process->setObjectName(QStringLiteral("turnProcess"));
        m_processLayout = new QVBoxLayout(m_process);
        m_processLayout->setContentsMargins(12, 2, 12, 0);
        m_processLayout->setSpacing(0);
        m_process->setVisible(false);
        m_cardLayout->addWidget(m_process);

        buildFoot();
        m_stack->addWidget(m_card);
    } else {
        m_stack->addWidget(m_bubble);
    }

    // 实时消息默认填当前时间；历史渲染会用消息自带 ts 覆盖（老会话无 ts 则清空）
    if (m_time)
        m_time->setText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));

    // webui @keyframes reveal：卡片入场淡入；结束后移除 effect，避免长期离屏渲染
    auto *fade = new QGraphicsOpacityEffect(this);
    fade->setOpacity(0.2);
    setGraphicsEffect(fade);
    auto *anim = new QPropertyAnimation(fade, "opacity", this);
    anim->setDuration(180);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(0.2);
    anim->setEndValue(1.0);
    connect(anim, &QPropertyAnimation::finished, this, [this] { setGraphicsEffect(nullptr); });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void MessageWidget::buildFoot()
{
    m_foot = new QWidget(m_card);
    m_foot->setObjectName(QStringLiteral("turnFoot"));
    auto *hl = new QHBoxLayout(m_foot);
    hl->setContentsMargins(12, 6, 12, 8);
    hl->setSpacing(8);

    m_copy = new IconPushButton(m_foot);
    setClass(m_copy, QStringLiteral("bubbleCopy"));
    m_copy->setFixedSize(20, 20);
    m_copy->setCursor(Qt::PointingHandCursor);
    m_copy->setToolTip(QStringLiteral("复制内容"));
    m_copy->setIconColors(gs::palette().muted2, gs::palette().cyan);
    m_copy->setIconName(QStringLiteral("copy"), 12);
    // .bubble-copy { opacity:.5 } + .turn-card:hover .bubble-copy { opacity:1 }
    m_copyOpacity = new QGraphicsOpacityEffect(m_copy);
    m_copyOpacity->setOpacity(0.5);
    m_copy->setGraphicsEffect(m_copyOpacity);
    connect(m_copy, &QPushButton::clicked, this, [this] {
        emit copyRequested(m_copyText);
    });

    m_usage = new QPushButton(m_foot);
    setClass(m_usage, QStringLiteral("bubblePill"));
    m_usage->setCursor(Qt::PointingHandCursor);
    m_usage->setToolTip(QStringLiteral("本轮用量明细"));
    m_usage->setVisible(false);

    m_timing = new QPushButton(m_foot);
    setClass(m_timing, QStringLiteral("bubblePill"));
    m_timing->setCursor(Qt::PointingHandCursor);
    m_timing->setToolTip(QStringLiteral("本轮用时明细"));
    m_timing->setVisible(false);

    m_time = makeLabel(QStringLiteral("bubbleMeta"), QString(), m_foot);
    m_time->setTextInteractionFlags(Qt::NoTextInteraction);

    hl->addWidget(m_copy, 0);
    hl->addStretch(1);
    hl->addWidget(m_usage, 0);
    hl->addWidget(m_timing, 0);
    hl->addWidget(m_time, 0);
    m_cardLayout->addWidget(m_foot);

    // 明细弹层：全局唯一，点开新的先收起旧的
    connect(m_usage, &QPushButton::clicked, this, [this] {
        auto *pop = new DetailPop(this);
        pop->configure(QStringLiteral("本轮用量"),
                       { { QStringLiteral("model"), QStringLiteral("提供方 / 模型") },
                         { QStringLiteral("cache_hit"), QStringLiteral("缓存命中") },
                         { QStringLiteral("uncached_input"), QStringLiteral("未缓存输入") },
                         { QStringLiteral("cache_read"), QStringLiteral("缓存读取") },
                         { QStringLiteral("output"), QStringLiteral("输出") } });
        const QVariantMap u = m_usageInfo;
        const qint64 cacheRead = u.value(QStringLiteral("cache_read")).toLongLong();
        const qint64 input = u.value(QStringLiteral("input")).toLongLong();
        const qint64 output = u.value(QStringLiteral("output")).toLongLong();
        QVariantMap rows;
        rows.insert(QStringLiteral("model"), u.value(QStringLiteral("model_label")).toString());
        rows.insert(QStringLiteral("cache_hit"),
                    (cacheRead && input)
                        ? QStringLiteral("%1%").arg(formatInt(qRound(cacheRead * 100.0 / input)))
                        : QString());
        rows.insert(QStringLiteral("uncached_input"), formatInt(qMax<qint64>(input - cacheRead, 0)));
        rows.insert(QStringLiteral("cache_read"), cacheRead ? formatInt(cacheRead) : QString());
        QString outputText = formatInt(output);
        if (u.value(QStringLiteral("reasoning")).toLongLong())
            outputText += QStringLiteral(" tok（其中推理 %1 tok）")
                              .arg(formatInt(u.value(QStringLiteral("reasoning")).toLongLong()));
        rows.insert(QStringLiteral("output"), outputText);
        pop->setValues(rows);
        pop->setTitleTotal(u.value(QStringLiteral("label")).toString());
        pop->popupAbove(m_usage);
    });
    connect(m_timing, &QPushButton::clicked, this, [this] {
        auto *pop = new DetailPop(this);
        pop->configure(QStringLiteral("本轮用时和速度"),
                       { { QStringLiteral("total"), QStringLiteral("本轮总用时") },
                         { QStringLiteral("think"), QStringLiteral("思考用时") },
                         { QStringLiteral("tps"), QStringLiteral("输出速度 (TPS)") },
                         { QStringLiteral("ttft"), QStringLiteral("首 token 用时 (TTFT，累计)") } });
        const QVariantMap t = m_timingInfo;
        QVariantMap rows;
        rows.insert(QStringLiteral("total"), formatDurationCn(t.value(QStringLiteral("elapsed")).toLongLong()));
        rows.insert(QStringLiteral("think"), formatDurationCn(t.value(QStringLiteral("think")).toLongLong()));
        rows.insert(QStringLiteral("tps"),
                    t.contains(QStringLiteral("tps"))
                        ? QStringLiteral("%1 tok/s").arg(QString::number(t.value(QStringLiteral("tps")).toDouble(), 'g', 4))
                        : QString());
        rows.insert(QStringLiteral("ttft"), formatDurationCn(t.value(QStringLiteral("ttft")).toLongLong()));
        pop->setValues(rows);
        pop->setTitleTotal(QString());
        pop->popupAbove(m_timing);
    });

    m_liveTimer = new QTimer(this);
    m_liveTimer->setInterval(1000);
    connect(m_liveTimer, &QTimer::timeout, this, [this] {
        const qint64 ms = qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_liveStart);
        m_timing->setText(QStringLiteral("已用时 %1").arg(formatDurationCn(ms)));
    });
}

void MessageWidget::setBodyVisible(bool visible)
{
    m_body->setVisible(visible);
}

void MessageWidget::setLabel(const QString &label)
{
    if (label.isEmpty())
        return; // app.js skillLabel: if (!label) return —— 不清除已有标签
    if (m_label) {
        m_label->setText(label.toUpper()); // CSS text-transform: uppercase
        return;
    }
    m_label = makeLabel(QStringLiteral("messageLabel"), label.toUpper(), m_bubble);
    m_label->setTextInteractionFlags(Qt::NoTextInteraction);
    m_label->setContentsMargins(0, 0, 0, 5); // CSS margin-bottom 5px
    m_bubbleLayout->insertWidget(0, m_label);
}

void MessageWidget::setAttachments(const QVariantList &attachments)
{
    if (attachments.isEmpty())
        return;
    auto *wrap = new QWidget(m_bubble);
    setClass(wrap, QStringLiteral("bubbleAttachments"));
    auto *flow = new FlowLayout(wrap, 0, 5, 5);
    flow->setContentsMargins(0, 0, 0, 6); // CSS margin-bottom 6px
    for (const QVariant &itemVar : attachments) {
        const QVariantMap item = itemVar.toMap();
        if (item.isEmpty())
            continue;
        const QString name = item.value(QStringLiteral("name")).isValid()
                                 ? item.value(QStringLiteral("name")).toString()
                                 : QStringLiteral("附件");
        const QString kind = item.value(QStringLiteral("kind")).toString();
        const QString url = item.value(QStringLiteral("url")).toString();
        if (kind == QLatin1String("image") && !url.isEmpty() && QFile::exists(url)) {
            auto *thumb = new QLabel(wrap);
            setClass(thumb, QStringLiteral("attachThumb"));
            thumb->setFixedSize(84, 84);
            QPixmap pixmap(url);
            if (!pixmap.isNull())
                thumb->setPixmap(pixmap.scaled(84, 84, Qt::KeepAspectRatioByExpanding,
                                               Qt::SmoothTransformation));
            thumb->setToolTip(name);
            flow->addWidget(thumb);
            continue;
        }
        auto *chip = new QFrame(wrap);
        setClass(chip, QStringLiteral("attachFile"));
        auto *cl = new QHBoxLayout(chip);
        cl->setContentsMargins(6, 2, 6, 2);
        cl->setSpacing(5);
        QString ext = attachExt(name);
        if (ext.isEmpty())
            ext = QStringLiteral("file");
        cl->addWidget(makeLabel(QStringLiteral("attachExt"), ext.toUpper(), chip));
        QLabel *nameLabel = makeLabel(QStringLiteral("attachName"), name, chip);
        nameLabel->setMaximumWidth(190);
        nameLabel->setText(elidedText(name, nameLabel->fontMetrics(), 190));
        nameLabel->setToolTip(name);
        cl->addWidget(nameLabel);
        flow->addWidget(chip);
    }
    m_bubbleLayout->insertWidget(0, wrap);
}

// ---- 过程区

ProcRow *MessageWidget::ensureProcRow(const QString &key)
{
    if (!m_process)
        return nullptr;
    ProcRow *&slot = (key == QLatin1String("think")) ? m_think : m_tools;
    if (slot)
        return slot;
    const QString label = key == QLatin1String("think") ? QStringLiteral("思考过程")
                                                        : QStringLiteral("工具调用");
    slot = new ProcRow(key, label, m_process);
    if (key == QLatin1String("think") && m_tools)
        m_processLayout->insertWidget(0, slot); // 思考始终在工具调用之上
    else
        m_processLayout->addWidget(slot);
    m_process->setVisible(true);
    return slot;
}

void MessageWidget::appendReasoning(const QString &delta)
{
    m_reasoningBuf += delta;
    ProcRow *row = ensureProcRow(QStringLiteral("think"));
    if (!row)
        return;
    row->setRunning(true);
    // 折叠态右侧一直刷新的是思考正文的当前最后一行，展开后看全文
    updateThinkSummary();
    QLayout *railLayout = qobject_cast<QVBoxLayout *>(row->rail()->layout());
    if (railLayout) {
        QLabel *text = nullptr;
        if (railLayout->count() == 0) {
            text = makeLabel(QStringLiteral("procRailText"), QString(), row->rail());
            text->setWordWrap(true);
            railLayout->addWidget(text);
        } else {
            text = qobject_cast<QLabel *>(railLayout->itemAt(0)->widget());
        }
        if (text)
            text->setText(m_reasoningBuf);
    }
}

void MessageWidget::updateThinkSummary()
{
    if (!m_think)
        return;
    // 收起时给正文片段（比耗时/字数更有用）：流式中取最后一行，结束后取第一行
    const bool running = m_think->property("running").toBool();
    const QString source = m_reasoningBuf.trimmed();
    if (source.isEmpty())
        return;
    const int newline = source.indexOf(QLatin1Char('\n'));
    const QString text = running
                             ? source.section(QLatin1Char('\n'), -1)
                             : source.left(newline >= 0 ? newline : source.size());
    m_think->setSummary(text.simplified());
}

void MessageWidget::settleThink()
{
    if (!m_think || !m_think->property("running").toBool())
        return;
    m_think->setRunning(false);
    updateThinkSummary();
}

void MessageWidget::settleProcess()
{
    if (m_think && m_think->property("running").toBool()) {
        m_think->setRunning(false);
        updateThinkSummary();
    }
    if (m_tools && m_tools->property("running").toBool()) {
        m_tools->setRunning(false);
        // app.js settleProcess：中断时把「执行中」改成「已中断」
        QString summary = m_tools->property("summaryText").toString();
        summary.replace(QStringLiteral("执行中"), QStringLiteral("已中断"));
        m_tools->setProperty("summaryText", summary);
        m_tools->setSummary(summary);
    }
}

ToolItemWidget *MessageWidget::addToolCall(const QString &callId, const QString &name,
                                           const QVariant &args)
{
    ProcRow *row = ensureProcRow(QStringLiteral("tools"));
    if (!row)
        return nullptr;
    row->setRunning(true);
    QLayout *railLayout = qobject_cast<QVBoxLayout *>(row->rail()->layout());
    if (!railLayout)
        return nullptr;
    QWidget *list = nullptr;
    for (int i = 0; i < railLayout->count(); ++i) {
        QWidget *w = railLayout->itemAt(i)->widget();
        if (w && w->property("class").toString() == QLatin1String("toolList")) {
            list = w;
            break;
        }
    }
    if (!list) {
        list = new QWidget(row->rail());
        setClass(list, QStringLiteral("toolList"));
        auto *ll = new QVBoxLayout(list);
        ll->setContentsMargins(0, 0, 0, 0);
        ll->setSpacing(0);
        ll->setAlignment(Qt::AlignTop);
        railLayout->addWidget(list);
    }
    auto *item = new ToolItemWidget(callId, name, args, list);
    static_cast<QVBoxLayout *>(list->layout())->addWidget(item);
    updateToolSummary();
    return item;
}

void MessageWidget::updateToolSummary()
{
    if (!m_tools)
        return;
    QWidget *list = nullptr;
    if (QLayout *railLayout = m_tools->rail()->layout()) {
        for (int i = 0; i < railLayout->count(); ++i) {
            QWidget *w = railLayout->itemAt(i)->widget();
            if (w && w->property("class").toString() == QLatin1String("toolList")) {
                list = w;
                break;
            }
        }
    }
    const QList<ToolItemWidget *> items =
        list ? list->findChildren<ToolItemWidget *>(QString(), Qt::FindDirectChildrenOnly)
             : QList<ToolItemWidget *>();
    bool running = false;
    bool failed = false;
    for (ToolItemWidget *item : items) {
        if (item->state() == QLatin1String("running"))
            running = true;
        else if (item->state() == QLatin1String("failed"))
            failed = true;
    }
    const QString summary = QStringLiteral("%1 个 · %2")
                                .arg(items.size())
                                .arg(running ? QStringLiteral("执行中")
                                             : (failed ? QStringLiteral("有失败")
                                                       : QStringLiteral("全部完成")));
    m_tools->setProperty("summaryText", summary);
    m_tools->setSummary(summary);
    m_tools->setRunning(running);
    m_tools->setFailed(!running && failed);
}

// ---- 底部信息行

void MessageWidget::setUsage(const QVariantMap &usage)
{
    if (!m_usage)
        return;
    const qint64 total = usage.value(QStringLiteral("total")).toLongLong();
    const qint64 input = usage.value(QStringLiteral("input")).toLongLong();
    const qint64 output = usage.value(QStringLiteral("output")).toLongLong();
    if (!total && !input && !output) {
        m_usage->setVisible(false);
        return;
    }
    m_usageInfo = usage;
    // 供应商没回传 usage 时后端给本地估算值，用 ≈ 区分实测与估算
    const QString label = (usage.value(QStringLiteral("estimated")).toBool() ? QStringLiteral("≈ ")
                                                                            : QString())
                          + formatInt(total) + QStringLiteral(" tokens");
    m_usageInfo.insert(QStringLiteral("label"), label);
    m_usage->setText(label);
    m_usage->setVisible(true);
    restyle(m_usage);
}

void MessageWidget::setTiming(const QVariantMap &timing)
{
    if (!m_timing)
        return;
    if (!timing.contains(QStringLiteral("elapsed"))) {
        m_timing->setVisible(false);
        return;
    }
    m_timingInfo = timing;
    stopLiveTiming();
    m_timing->setText(QStringLiteral("用时 %1")
                          .arg(formatDurationCn(timing.value(QStringLiteral("elapsed")).toLongLong())));
    m_timing->setVisible(true);
    restyle(m_timing);
}

void MessageWidget::setTimeStamp(const QString &isoOrEmpty)
{
    if (!m_time)
        return;
    m_time->setText(formatClock(isoOrEmpty));
}

void MessageWidget::startLiveTiming(qint64 startMs)
{
    if (!m_timing || !m_liveTimer)
        return;
    stopLiveTiming();
    m_liveStart = startMs > 0 ? startMs : QDateTime::currentMSecsSinceEpoch();
    m_timing->setVisible(true);
    m_timing->setText(QStringLiteral("已用时 %1")
                          .arg(formatDurationCn(QDateTime::currentMSecsSinceEpoch() - m_liveStart)));
    m_liveTimer->start();
}

void MessageWidget::stopLiveTiming()
{
    if (m_liveTimer)
        m_liveTimer->stop();
}

void MessageWidget::markStopped()
{
    // app.js markStopped：给气泡补「已停止」标记，避免半截内容看起来像正常答完
    const QString current = m_label ? m_label->text() : QString();
    if (current.contains(QStringLiteral("已停止")))
        return;
    setLabel(current.isEmpty() ? QStringLiteral("已停止")
                              : current + QStringLiteral(" · 已停止"));
}

void MessageWidget::setCardError(bool error)
{
    if (!m_card)
        return;
    m_card->setProperty("error", error);
    restyle(m_card);
}

void MessageWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyMaxWidths();
}

void MessageWidget::enterEvent(QEvent *event)
{
    if (m_copyOpacity)
        m_copyOpacity->setOpacity(1.0);
    QWidget::enterEvent(event);
}

void MessageWidget::leaveEvent(QEvent *event)
{
    if (m_copyOpacity)
        m_copyOpacity->setOpacity(0.5);
    QWidget::leaveEvent(event);
}

// CSS: .message > * { max-width: 94% }；助手卡片宽度恒定，避免流式时宽度跳动
void MessageWidget::applyMaxWidths()
{
    const int maxWidth = qRound(width() * 0.94);
    for (int i = 0; i < m_stack->count(); ++i) {
        if (QWidget *child = m_stack->itemAt(i)->widget()) {
            child->setMaximumWidth(maxWidth);
            if (m_card && child == m_card && m_role == QLatin1String("assistant"))
                child->setMinimumWidth(maxWidth);
        }
    }
}

} // namespace gs
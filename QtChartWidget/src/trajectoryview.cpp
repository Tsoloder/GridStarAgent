#include "trajectoryview.h"

#include "commonwidgets.h"
#include "markdownview.h"
#include "theme.h"

#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTextDocument>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace gs {
namespace {

// ================= 与 app.js 一一对应的投影工具 =================

// TRAJ_FAILED_RE
const QRegularExpression &failedRe()
{
    static const QRegularExpression re(
        QStringLiteral("^(Tool error:|Tool execution denied|Tool blocked by active Skill policy|\\[deferred\\])"));
    return re;
}

// TRAJ_SOURCE_LABEL
QString sourceLabel(const QString &source)
{
    if (source == QLatin1String("system")) return QStringLiteral("系统");
    if (source == QLatin1String("user")) return QStringLiteral("用户");
    if (source == QLatin1String("context")) return QStringLiteral("上下文");
    if (source == QLatin1String("assistant")) return QStringLiteral("助手");
    if (source == QLatin1String("tool")) return QStringLiteral("工具");
    return source;
}

// TRAJ_SPAN_CLASS：助手记为 model，其余同名，保证同一来源同色
QString spanClass(const QString &source)
{
    if (source == QLatin1String("assistant")) return QStringLiteral("model");
    if (source == QLatin1String("system")) return QStringLiteral("system");
    if (source == QLatin1String("context")) return QStringLiteral("context");
    if (source == QLatin1String("tool")) return QStringLiteral("tool");
    return QStringLiteral("user");
}

// trajEstimateTokens：中日韩 1 token，其余 4 字符 1 token
int estimateTokens(const QString &text)
{
    int cjk = 0;
    const int n = text.size();
    for (int i = 0; i < n; ++i) {
        const ushort code = text.at(i).unicode();
        if ((code >= 0x3040 && code <= 0x30ff) || (code >= 0x4e00 && code <= 0x9fff)
            || (code >= 0xac00 && code <= 0xd7af))
            ++cjk;
    }
    return cjk + (n - cjk) / 4;
}

QString vstr(const QVariantMap &map, const char *key)
{
    return map.value(QString::fromLatin1(key)).toString();
}

int vint(const QVariantMap &map, const char *key)
{
    return map.value(QString::fromLatin1(key)).toInt();
}

// JS 的 typeof x === "number"：只认真正的数字，字符串/布尔一律不算
bool vnum(const QVariantMap &map, const char *key, double *out)
{
    const QVariant value = map.value(QString::fromLatin1(key));
    if (!value.isValid() || value.isNull())
        return false;
    switch (value.type()) {
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
    case QVariant::Double:
        *out = value.toDouble();
        return true;
    default:
        return false;
    }
}

// trajKey：事件 → 基础键（空字符串表示不参与投影）
QString trajKey(const QVariantMap &ev)
{
    const QString type = vstr(ev, "type");
    if (type == QLatin1String("user"))
        return QStringLiteral("u") + QString::number(vint(ev, "turn"));
    if (type == QLatin1String("traj_system_prompt"))
        return QStringLiteral("sp") + QString::number(vint(ev, "turn"));
    if (type == QLatin1String("traj_context"))
        return QStringLiteral("cx") + QString::number(vint(ev, "turn")) + QLatin1Char('.')
               + QString::number(vint(ev, "request")) + QLatin1Char('.') + vstr(ev, "kind");
    if (type == QLatin1String("traj_request_start"))
        return QStringLiteral("rs") + QString::number(vint(ev, "turn")) + QLatin1Char('.')
               + QString::number(vint(ev, "request"));
    if (type == QLatin1String("traj_request_end"))
        return QStringLiteral("re") + QString::number(vint(ev, "turn")) + QLatin1Char('.')
               + QString::number(vint(ev, "request"));
    if (type == QLatin1String("tool_call"))
        return QStringLiteral("tc") + vstr(ev, "id");
    if (type == QLatin1String("tool_result"))
        return QStringLiteral("tr") + vstr(ev, "call_id");
    return QString();
}

// trajFormatMs
QString formatMs(bool has, double ms)
{
    if (!has || std::isnan(ms))
        return QStringLiteral("—");
    if (ms < 1000.0)
        return QString::number(qRound(ms)) + QStringLiteral(" 毫秒");
    return QString::number(ms / 1000.0, 'f', 2) + QStringLiteral(" 秒");
}

// JSON.stringify(v, null, pretty)
QString jsonAny(const QVariant &value, bool pretty)
{
    const QVariant v = value.isNull() ? QVariant(QVariantMap()) : value;
    const QJsonDocument doc = QJsonDocument::fromVariant(v);
    QString out = QString::fromUtf8(doc.toJson(pretty ? QJsonDocument::Indented : QJsonDocument::Compact));
    if (out.endsWith(QLatin1Char('\n')))
        out.chop(1);
    return out;
}

// trajPreview：折叠空白后按 limit 截断
QString preview(const QString &text, int limit)
{
    QString str = text.simplified();
    if (str.size() > limit)
        str = str.left(limit) + QStringLiteral("…");
    return str;
}

// trajArgsText：缩进 JSON，过长截断
QString argsText(const QVariant &args)
{
    QString text = jsonAny(args, true);
    if (text.size() > 1200)
        text = text.left(1200) + QStringLiteral("\n…");
    return text;
}

// trajRecordMs
bool recordMs(const TrajRecord &rec, double *out)
{
    if (rec.kind == QLatin1String("request") && rec.hasTiming && rec.timing.totalSet) {
        *out = rec.timing.totalMs;
        return true;
    }
    if (rec.source == QLatin1String("tool") && rec.hasDurationMs) {
        *out = rec.durationMs;
        return true;
    }
    return false;
}

// 记录里最长的耗时：行内迷你条按它算相对比例。增量追加时会重算，
// 已渲染的旧行保持旧比例，等下一次全量重建对齐（差一个条宽，不值得为它重建整账本）
double maxRecordMs(const QList<TrajRecord> &records)
{
    double maxMs = 0;
    for (const TrajRecord &rec : records) {
        double ms = 0;
        if (recordMs(rec, &ms) && ms > maxMs)
            maxMs = ms;
    }
    return maxMs;
}

// trajRecordText（搜索用）
QString recordText(const TrajRecord &rec)
{
    QStringList parts;
    parts << rec.label << rec.name << rec.model << rec.content << rec.reasoning;
    if (rec.hasArgs)
        parts << jsonAny(rec.args, false);
    for (const QVariant &tc : rec.toolCalls) {
        const QVariantMap map = tc.toMap();
        parts << map.value(QStringLiteral("name")).toString();
        parts << jsonAny(map.value(QStringLiteral("args")), false);
    }
    return parts.join(QLatin1Char('\n'));
}

// trajRowMeta
QString rowMetaText(const TrajRecord &rec)
{
    QStringList bits;
    if (rec.hasTokens && rec.tokens.total)
        bits << QString::number(rec.tokens.total) + QStringLiteral(" tok")
                    + (rec.tokens.estimated ? QStringLiteral("≈") : QString());
    double ms = 0;
    if (recordMs(rec, &ms))
        bits << formatMs(true, ms);
    return bits.join(QStringLiteral(" · "));
}

QString statusText(const QString &status)
{
    if (status == QLatin1String("completed")) return QStringLiteral("已完成");
    if (status == QLatin1String("failed")) return QStringLiteral("失败");
    if (status == QLatin1String("interrupted")) return QStringLiteral("已停止");
    return QStringLiteral("进行中");
}

// trajGroupKey
QString groupKeyOf(const QString &view, const TrajRecord &rec)
{
    if (view.isEmpty())
        return QString();
    if (view == QLatin1String("call"))
        return QStringLiteral("c") + QString::number(rec.turn) + QLatin1Char('.') + QString::number(rec.request);
    return QStringLiteral("t") + QString::number(rec.turn);
}

// Date.parse 的 Qt 等价物：容忍微秒精度与空格分隔的 ISO 时间
double parseIsoMs(const QString &raw)
{
    QString str = raw.trimmed();
    if (str.isEmpty())
        return std::nan("");
    if (str.contains(QLatin1Char(' ')) && !str.contains(QLatin1Char('T')))
        str.replace(QLatin1Char(' '), QLatin1Char('T'));
    static const QRegularExpression frac(QStringLiteral("(\\.\\d{3})\\d+"));
    str.replace(frac, QStringLiteral("\\1")); // Qt 的 ISO 解析只认到毫秒
    QDateTime dt = QDateTime::fromString(str, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(str, Qt::ISODate);
    if (!dt.isValid())
        return std::nan("");
    return double(dt.toMSecsSinceEpoch());
}

// trajTimeMs：优先 timing.start，其次 ts
double timeMsOf(const TrajRecord &rec)
{
    QString raw = rec.hasTiming ? rec.timing.start : QString();
    if (raw.isEmpty())
        raw = rec.ts;
    return parseIsoMs(raw);
}

// 时间轴条颜色（对应 .traj-span.* 的 CSS，全部取自调色板）
QColor spanColor(const QString &cls)
{
    const Palette &pal = gs::palette();
    if (cls.contains(QLatin1String("failed"))) return pal.red;
    if (cls.startsWith(QLatin1String("ttft"))) return pal.assistantText.lighter(150);
    if (cls.startsWith(QLatin1String("system"))) return pal.muted;
    if (cls.startsWith(QLatin1String("context"))) return pal.okText;
    if (cls.startsWith(QLatin1String("model"))) return pal.assistantText;
    if (cls.startsWith(QLatin1String("tool"))) return pal.warnText;
    return pal.cyan;
}

// 行内耗时条填充色（对应 .traj-row.source-* .traj-row-bar > i）
QColor sourceFill(const TrajRecord &rec)
{
    const Palette &pal = gs::palette();
    if (rec.status == QLatin1String("failed")) return pal.red;
    if (rec.source == QLatin1String("assistant")) return pal.assistantText;
    if (rec.source == QLatin1String("tool")) return pal.warnText;
    if (rec.source == QLatin1String("system")) return pal.muted;
    if (rec.source == QLatin1String("context")) return pal.okText;
    return pal.cyan;
}

// QSS 的 .class 选择器匹配动态属性 class；行/分组内文字不可选中，避免抢走点击
QLabel *mkLabel(const QString &cls, const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    if (!cls.isEmpty())
        setClass(label, cls);
    label->setAttribute(Qt::WA_StyledBackground, false);
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    return label;
}

QLabel *mkSelectableLabel(const QString &cls, const QString &text, QWidget *parent)
{
    QLabel *label = mkLabel(cls, text, parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

// 单行省略标签：webui 里 .traj-row-preview / .traj-group-label 都带 text-overflow:ellipsis
ElidedLabel *mkElided(const QString &cls, const QString &text, QWidget *parent)
{
    auto *label = new ElidedLabel(parent);
    if (!cls.isEmpty())
        setClass(label, cls);
    label->setAttribute(Qt::WA_StyledBackground, false);
    label->setFullText(text);
    return label;
}

void clearLayout(QVBoxLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

// 时间轴画布坐标：轨道高 8px、行距 3px，上下各留 4px 供选区画出轨道外
int laneTop(int lane)
{
    return 4 + lane * 15;
}

const int kLaneHeight = 8;
// 账本内容的上边距：视窗化时「内容坐标 ↔ 条目下标」的换算要用到它（见 ledgerEntryIndexAt）
const int kLedgerTopMargin = 4;
// 可视区上下各多建几行，滚动时不至于先看到空白再补上
const int kLedgerOverscan = 240;

} // namespace

// ============================ TrajTimelineCanvas ============================

TrajTimelineCanvas::TrajTimelineCanvas(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void TrajTimelineCanvas::setSpans(const QList<TrajSpan> &spans, bool valid, double dMin, double dMax)
{
    m_spans = spans;
    m_valid = valid && !spans.isEmpty();
    m_dMin = dMin;
    m_dMax = dMax;
    update();
}

void TrajTimelineCanvas::setSelectedId(const QString &id)
{
    if (m_selected == id)
        return;
    m_selected = id;
    update();
}

void TrajTimelineCanvas::clearRange()
{
    if (!m_hasRange)
        return;
    m_hasRange = false;
    update();
}

double TrajTimelineCanvas::displayAt(int x) const
{
    if (!m_valid)
        return std::nan("");
    const double w = width();
    if (w <= 0)
        return std::nan("");
    const double ratio = qBound(0.0, double(x) / w, 1.0);
    return m_dMin + ratio * (m_dMax - m_dMin);
}

// trajSpanHit：优先取包含该点的最窄跨度，否则取投影上最近的跨度
const TrajSpan *TrajTimelineCanvas::spanAt(double d) const
{
    if (std::isnan(d) || m_spans.isEmpty())
        return nullptr;
    const TrajSpan *best = nullptr;
    double bestDist = 0;
    double bestWidth = 0;
    bool first = true;
    for (const TrajSpan &sp : m_spans) {
        const double dist = d < sp.ds ? sp.ds - d : (d > sp.de ? d - sp.de : 0.0);
        const double width = sp.de - sp.ds;
        if (first || dist < bestDist || (dist == bestDist && width < bestWidth)) {
            best = &sp;
            bestDist = dist;
            bestWidth = width;
            first = false;
        }
    }
    return best;
}

void TrajTimelineCanvas::paintEvent(QPaintEvent *)
{
    if (!m_valid || m_spans.isEmpty())
        return;
    const double domain = m_dMax - m_dMin;
    if (domain <= 0)
        return;
    const double w = width();

    QPainter p(this);
    for (const TrajSpan &sp : m_spans) {
        double x0 = (sp.ds - m_dMin) / domain * w;
        double x1 = (sp.de - m_dMin) / domain * w;
        if (x1 < x0)
            std::swap(x0, x1);
        // 相邻条各让出 min(宽度 8%, 1px) 的缝，最短保留 2px 保证可见
        const double sw = x1 - x0;
        const double gap = qMin(sw * 0.08, 1.0);
        const double left = x0 + gap;
        const double barWidth = qMax(2.0, sw - 2.0 * gap);
        const QRectF rect(left, laneTop(sp.lane), barWidth, double(kLaneHeight));

        QColor color = spanColor(sp.cls);
        if (!m_hoverId.isEmpty() && sp.id == m_hoverId)
            color = color.lighter(130);
        p.fillRect(rect, color);
        if (!m_selected.isEmpty() && sp.id == m_selected) {
            p.setPen(gs::palette().cyan);
            p.setBrush(Qt::NoBrush);
            p.drawRect(rect.adjusted(0.0, 0.0, -1.0, -1.0));
        }
    }

    if (m_hasRange) {
        const double left = qMax(0.0, (m_rangeD0 - m_dMin) / domain * w);
        const double right = qMin(w, (m_rangeD1 - m_dMin) / domain * w);
        const QRectF rect(left, 0.0, qMax(right - left, 0.0), double(height()));
        QColor fill = gs::palette().cyan;
        fill.setAlphaF(0.10);
        p.fillRect(rect, fill);
        p.setPen(gs::palette().cyan);
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect.adjusted(0.0, 0.0, -1.0, -1.0));
    }
}

void TrajTimelineCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_valid) {
        QWidget::mousePressEvent(event);
        return;
    }
    const double d = displayAt(event->pos().x());
    if (std::isnan(d)) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_dragMoved = false;
    m_dragStartD = d;
    m_dragStartX = event->pos().x();
    event->accept();
}

void TrajTimelineCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        const TrajSpan *sp = spanAt(displayAt(event->pos().x()));
        const QString id = sp ? sp->id : QString();
        if (id != m_hoverId) {
            m_hoverId = id;
            update();
        }
        setCursor(id.isEmpty() ? Qt::CrossCursor : Qt::PointingHandCursor);
        setToolTip(sp ? sp->title : QString());
        return;
    }
    const double d = displayAt(event->pos().x());
    if (std::isnan(d))
        return;
    if (std::abs(event->pos().x() - m_dragStartX) > 3)
        m_dragMoved = true;
    if (!m_dragMoved)
        return;
    const double d0 = qMin(m_dragStartD, d);
    const double d1 = qMax(m_dragStartD, d);
    // 选区记投影坐标（画选区）+ 命中的记录 id（过滤账本）
    QStringList ids;
    for (const TrajSpan &sp : m_spans)
        if (sp.ds <= d1 && sp.de >= d0)
            ids << sp.id;
    m_hasRange = true;
    m_rangeD0 = d0;
    m_rangeD1 = d1;
    update();
    emit rangeChanged(ids, d0, d1);
}

void TrajTimelineCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    if (!m_dragMoved) {
        // 单击（未拖动）：清选区后定位到该处的账本记录
        const bool had = m_hasRange;
        m_hasRange = false;
        if (had)
            update();
        const TrajSpan *sp = spanAt(displayAt(event->pos().x()));
        emit cleared();
        if (sp)
            emit spanClicked(sp->id);
    }
}

void TrajTimelineCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    m_dragging = false;
    if (m_hasRange) {
        m_hasRange = false;
        update();
    }
    emit cleared();
    QWidget::mouseDoubleClickEvent(event);
}

// =============================== TrajTimeline ===============================

TrajTimeline::TrajTimeline(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(58); // CSS: 上下内边距 7+9，三条 12px 泳道 + 2×3px 间距

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(9, 7, 9, 9);
    root->setSpacing(3);

    const QString names[3] = {QStringLiteral("输入"), QStringLiteral("模型"), QStringLiteral("工具")};
    for (int i = 0; i < 3; ++i) {
        auto *lane = new QWidget(this);
        lane->setFixedHeight(12);
        auto *row = new QHBoxLayout(lane);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);

        auto *label = mkLabel(QStringLiteral("trajLaneLabel"), names[i], lane);
        label->setFixedWidth(32);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        auto *track = new QFrame(lane);
        setClass(track, QStringLiteral("trajLaneTrack"));
        track->setAttribute(Qt::WA_StyledBackground, true);
        track->setFixedHeight(kLaneHeight);

        row->addWidget(label, 0);
        row->addWidget(track, 1);
        root->addWidget(lane, 0);

        m_labels[i] = label;
        m_tracks[i] = track;
    }

    m_canvas = new TrajTimelineCanvas(this);
    m_canvas->raise();
}

void TrajTimeline::setSpans(const QList<TrajSpan> &spans, bool valid, double dMin, double dMax)
{
    m_canvas->setSpans(spans, valid, dMin, dMax);
}

void TrajTimeline::setSelectedId(const QString &id)
{
    m_canvas->setSelectedId(id);
}

void TrajTimeline::clearRange()
{
    m_canvas->clearRange();
}

void TrajTimeline::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 先让布局落定，轨道宽度才是本次 resize 后的真实值
    if (QLayout *root = layout())
        root->activate();
    syncCanvasGeometry();
    // 首帧布局可能还要等一次事件循环，再同步一次兜底
    QTimer::singleShot(0, this, [this] { syncCanvasGeometry(); });
}

void TrajTimeline::syncCanvasGeometry()
{
    if (!m_canvas || !m_tracks[0] || !m_tracks[2])
        return;
    // 画布只覆盖三根轨道的并集（水平与轨道对齐，纵向上下各外扩 4px 供选区）
    const QPoint top0 = m_tracks[0]->mapTo(this, QPoint(0, 0));
    const QPoint top2 = m_tracks[2]->mapTo(this, QPoint(0, 0));
    const int y = top0.y() - 4;
    const int h = (top2.y() + m_tracks[2]->height()) - top0.y() + 8;
    const int w = m_tracks[0]->width();
    if (m_canvas->geometry() == QRect(top0.x(), y, w, h))
        return;
    m_canvas->setGeometry(top0.x(), y, w, h);
    m_canvas->raise();
}

// 点击泳道标签/内边距属于「点时间轴」，按 webui 语义不收起详情：吞掉事件不让其冒泡
void TrajTimeline::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

// ================================ TrajHitBox ================================

TrajHitBox::TrajHitBox(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_Hover, true);
    // 行 / 分组头的 QSS 背景（hover、selected、分组分隔线）需要样式化背景才会绘制
    setAttribute(Qt::WA_StyledBackground, true);
    // 账本行 / 分组头在 webui 里都是 tabindex="0"
    setFocusPolicy(Qt::TabFocus);
}

void TrajHitBox::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_handler) {
        m_handler();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TrajHitBox::keyPressEvent(QKeyEvent *event)
{
    const int key = event->key();
    if ((key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) && m_handler) {
        m_handler();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

// ============================== TrajectoryView ==============================

TrajectoryView::TrajectoryView(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("trajectoryView"));
    buildUi();
}

void TrajectoryView::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 工具栏：三个开关 + 搜索 ----
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("trajToolbar"));
    m_toolbar->setAttribute(Qt::WA_StyledBackground, true);
    m_toolbar->installEventFilter(this);
    auto *toolbar = new QHBoxLayout(m_toolbar);
    toolbar->setContentsMargins(9, 6, 9, 6);
    toolbar->setSpacing(0);

    auto makeToggle = [this](const QString &text, QPushButton **slot) {
        auto *button = new QPushButton(text, m_toolbar);
        setClass(button, QStringLiteral("trajViewButton"));
        button->setProperty("active", false);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::TabFocus);
        *slot = button;
    };
    makeToggle(QStringLiteral("时长"), &m_btnDuration);
    makeToggle(QStringLiteral("轮次"), &m_btnTurn);
    makeToggle(QStringLiteral("调用"), &m_btnCall);
    toolbar->addWidget(m_btnDuration, 0);
    toolbar->addWidget(m_btnTurn, 0);
    toolbar->addWidget(m_btnCall, 0);
    toolbar->addStretch(1);
    toolbar->addSpacing(10);

    m_search = new QLineEdit(m_toolbar);
    m_search->setObjectName(QStringLiteral("trajSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索记录…"));
    m_search->setFixedHeight(26);
    m_search->setFixedWidth(220);
    toolbar->addWidget(m_search, 0);
    root->addWidget(m_toolbar, 0);

    // ---- 时间轴概览 ----
    m_timeline = new TrajTimeline(this);
    m_timeline->setObjectName(QStringLiteral("trajTimeline"));
    root->addWidget(m_timeline, 0);
    connect(m_timeline->canvas(), &TrajTimelineCanvas::spanClicked, this,
            [this](const QString &id) { selectRecord(id); });
    connect(m_timeline->canvas(), &TrajTimelineCanvas::rangeChanged, this,
            [this](const QStringList &ids, double d0, double d1) {
                m_hasRange = true;
                m_rangeD0 = d0;
                m_rangeD1 = d1;
                m_rangeIds = ids;
                postRefresh();
            });
    connect(m_timeline->canvas(), &TrajTimelineCanvas::cleared, this, [this] {
        if (!m_hasRange)
            return;
        m_hasRange = false;
        m_rangeIds.clear();
        postRefresh();
    });

    // ---- 账本 + 详情面板 ----
    auto *body = new QWidget(this);
    m_body = body;
    auto *bodyLayout = new QHBoxLayout(body);
    m_bodyLayout = bodyLayout;
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    m_ledger = new QScrollArea(body);
    m_ledger->setObjectName(QStringLiteral("trajLedger"));
    m_ledger->setFrameShape(QFrame::NoFrame);
    m_ledger->setWidgetResizable(true);
    m_ledger->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_ledger->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));
    m_ledger->viewport()->installEventFilter(this);
    m_ledgerInner = new QWidget;
    // 行内长文本不参与最小宽度：横向滚动条是关的，inner 超过视口就会被裁掉
    m_ledgerInner->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_ledgerInner->installEventFilter(this);
    m_ledgerLayout = new QVBoxLayout(m_ledgerInner);
    m_ledgerLayout->setContentsMargins(0, kLedgerTopMargin, 0, 10);
    m_ledgerLayout->setSpacing(0);
    // 视窗化：账本里只有「可视区那几十行」是真实控件，其余用上下两个 spacer 占位；
    // 末尾的 stretch 负责吸收「内容比视口短」时的多余空间（否则行会被拉高）
    m_ledgerSpacerTop = new QWidget(m_ledgerInner);
    m_ledgerSpacerTop->setFixedHeight(0);
    m_ledgerLayout->addWidget(m_ledgerSpacerTop);
    m_ledgerSpacerBottom = new QWidget(m_ledgerInner);
    m_ledgerSpacerBottom->setFixedHeight(0);
    m_ledgerLayout->addWidget(m_ledgerSpacerBottom);
    m_ledgerLayout->addStretch(1);
    m_ledger->setWidget(m_ledgerInner);
    m_ledger->verticalScrollBar()->setSingleStep(24);
    connect(m_ledger->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        updateLedgerWindow(); // 滚动只重建「新进入视口的那几行」
    });
    bodyLayout->addWidget(m_ledger, 1);

    buildInspector();
    bodyLayout->addWidget(m_inspector, 0);
    root->addWidget(body, 1);

    // ---- 交互 ----
    connect(m_btnDuration, &QPushButton::clicked, this, [this] {
        m_actualDuration = !m_actualDuration;
        m_hasRange = false;
        m_rangeIds.clear();
        m_timeline->clearRange();
        syncViewButtons();
        refreshAll();
    });
    connect(m_btnTurn, &QPushButton::clicked, this, [this] {
        m_view = (m_view == QLatin1String("turn")) ? QString() : QStringLiteral("turn");
        resetCollapsed();
        syncViewButtons();
        refreshAll();
    });
    connect(m_btnCall, &QPushButton::clicked, this, [this] {
        m_view = (m_view == QLatin1String("call")) ? QString() : QStringLiteral("call");
        resetCollapsed();
        syncViewButtons();
        refreshAll();
    });
    // 搜索：本地账本可能有几千行，逐字重建会卡（每次输入都要重建全部行控件），
    // 所以停手 200ms 再刷新一次
    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(200);
    connect(m_searchDebounce, &QTimer::timeout, this, [this] {
        rebuildLedger();
        rebuildInspector();
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_query = text;
        m_searchDebounce->start();
    });

    syncViewButtons();
}

void TrajectoryView::buildInspector()
{
    m_inspector = new QWidget;
    m_inspector->setObjectName(QStringLiteral("trajInspector"));
    m_inspector->setAttribute(Qt::WA_StyledBackground, true);
    m_inspector->setFixedWidth(380);

    auto *root = new QVBoxLayout(m_inspector);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    const QString headBorder = QStringLiteral("background: transparent; border-bottom: 1px solid %1;")
                                   .arg(cssColor(gs::palette().line));

    // 头部：来源徽标 + 位置 + 关闭
    auto *head = new QWidget(m_inspector);
    head->setAttribute(Qt::WA_StyledBackground, true);
    head->setStyleSheet(headBorder);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(10, 8, 10, 8);
    headLayout->setSpacing(8);
    m_inspBadge = mkLabel(QStringLiteral("trajBadge"), QString(), head);
    m_inspBadge->setProperty("source", QString());
    headLayout->addWidget(m_inspBadge, 0);
    m_inspPos = mkLabel(QStringLiteral("trajInspPos"), QString(), head);
    m_inspPos->setMinimumWidth(0);
    headLayout->addWidget(m_inspPos, 1);

    auto *close = new IconPushButton(head);
    close->setProperty("variant", QStringLiteral("icon"));
    close->setCursor(Qt::PointingHandCursor);
    close->setFocusPolicy(Qt::TabFocus);
    close->setFixedSize(24, 24);
    close->setIconColors(gs::palette().muted, gs::palette().textBright);
    close->setIconName(QStringLiteral("x"), scaledPx(14));
    headLayout->addWidget(close, 0);
    root->addWidget(head, 0);
    connect(close, &QPushButton::clicked, this, [this] { clearSelection(); });

    // 三个页签：概述 / 预览 / 原始内容
    auto *tabsRow = new QWidget(m_inspector);
    tabsRow->setAttribute(Qt::WA_StyledBackground, true);
    tabsRow->setStyleSheet(headBorder);
    auto *tabsLayout = new QHBoxLayout(tabsRow);
    tabsLayout->setContentsMargins(8, 0, 8, 0);
    tabsLayout->setSpacing(0);
    const QString tabText[3] = {QStringLiteral("概述"), QStringLiteral("预览"), QStringLiteral("原始内容")};
    const QString tabId[3] = {QStringLiteral("overview"), QStringLiteral("preview"), QStringLiteral("raw")};
    for (int i = 0; i < 3; ++i) {
        auto *button = new QPushButton(tabText[i], tabsRow);
        setClass(button, QStringLiteral("trajInspTab"));
        button->setProperty("active", false);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::TabFocus);
        const QString id = tabId[i];
        connect(button, &QPushButton::clicked, this, [this, id] {
            m_inspectorTab = id;
            rebuildInspector();
        });
        tabsLayout->addWidget(button, 0);
        m_tabs[i] = button;
    }
    tabsLayout->addStretch(1);
    root->addWidget(tabsRow, 0);

    // 正文
    m_inspBody = new QScrollArea(m_inspector);
    m_inspBody->setFrameShape(QFrame::NoFrame);
    m_inspBody->setWidgetResizable(true);
    m_inspBody->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_inspBody->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));
    m_inspBodyInner = new QWidget;
    m_inspBodyLayout = new QVBoxLayout(m_inspBodyInner);
    m_inspBodyLayout->setContentsMargins(12, 10, 12, 10);
    m_inspBodyLayout->setSpacing(0);
    m_inspBodyLayout->setAlignment(Qt::AlignTop);
    m_inspBody->setWidget(m_inspBodyInner);
    root->addWidget(m_inspBody, 1);

    m_inspector->setVisible(false);
}

// ---------- 数据摄入 / 投影 ----------

void TrajectoryView::setEvents(const QVariantList &events)
{
    m_events.clear();
    m_count.clear();
    m_keys.clear();
    ingest(events);
    m_loaded = true;
    refreshAll();
}

void TrajectoryView::appendEvents(const QVariantList &events)
{
    if (events.isEmpty())
        return;
    ingest(events);
    projectRecords();
    // 时间轴是单块自绘画布，重算只是纯计算（不建控件），保持全量
    rebuildTimeline();
    // 账本按行建控件：几千行时全量重建要几百毫秒，而实时轨迹是「一条一条追加」进来的，
    // 所以正常路径只把新增的记录补上去（其余情况见 canAppendLedger）
    if (canAppendLedger(events))
        appendLedgerTail();
    else
        rebuildLedger();
    rebuildInspector();
}

void TrajectoryView::clearEvents()
{
    m_events.clear();
    m_count.clear();
    m_keys.clear();
    m_records.clear();
    m_selected.clear();
    m_hasRange = false;
    m_rangeD0 = 0;
    m_rangeD1 = 0;
    m_rangeIds.clear();
    m_view.clear();
    m_collapsed.clear();
    m_query.clear();
    m_actualDuration = false;
    m_loadRequested = false;
    m_loaded = false;
    if (m_search)
        m_search->clear();
    syncViewButtons();
    refreshAll();
}

// 摄入事件并按「基础键 + 出现序号」去重：落盘回放与实时流顺序一致，序号天然对齐
void TrajectoryView::ingest(const QVariantList &events)
{
    for (const QVariant &value : events) {
        const QVariantMap ev = value.toMap();
        const QString type = vstr(ev, "type");
        if (type.isEmpty())
            continue;
        const QString base = trajKey(ev);
        if (base.isEmpty())
            continue;
        const int n = m_count.value(base, 0);
        m_count.insert(base, n + 1);
        const QString key = base + QLatin1Char('#') + QString::number(n);
        if (m_keys.contains(key))
            continue;
        m_keys.insert(key);
        m_events.append(ev);
    }
}

void TrajectoryView::projectRecords()
{
    m_records.clear();
    QHash<QString, int> openRequests;
    QHash<QString, int> openTools;
    int seq = 0;

    auto push = [this, &seq](const TrajRecord &rec) {
        TrajRecord item = rec;
        item.id = QStringLiteral("r") + QString::number(++seq);
        m_records.append(item);
        return m_records.size() - 1;
    };

    auto newRequest = [&push](int turn, int request, const QString &model,
                              const QString &startIso, const QVariant &raw) {
        TrajRecord rec;
        rec.source = QStringLiteral("assistant");
        rec.kind = QStringLiteral("request");
        rec.turn = turn;
        rec.request = request;
        rec.step = 0;
        rec.status = QStringLiteral("running");
        rec.label = QStringLiteral("请求 #") + QString::number(request);
        rec.model = model;
        rec.hasTiming = true;
        rec.timing.start = startIso;
        rec.raw = raw;
        rec.ts = !startIso.isEmpty() ? startIso
                                     : raw.toMap().value(QStringLiteral("ts")).toString();
        return push(rec);
    };

    auto newTool = [&push](const QVariantMap &ev, bool hasResult, const QVariantMap &resultEv) {
        TrajRecord rec;
        rec.source = QStringLiteral("tool");
        rec.kind = QStringLiteral("tool_call");
        rec.turn = vint(ev, "turn");
        rec.request = vint(ev, "request");
        rec.step = vint(ev, "step");
        rec.status = QStringLiteral("running");
        rec.name = vstr(ev, "name");
        rec.hasArgs = true;
        rec.args = ev.value(QStringLiteral("args")).toMap();
        rec.callId = vstr(ev, "id");
        if (rec.callId.isEmpty() && hasResult)
            rec.callId = vstr(resultEv, "call_id");
        rec.ts = vstr(ev, "ts");
        if (hasResult) {
            QVariantMap raw;
            raw.insert(QStringLiteral("tool_call"), ev);
            raw.insert(QStringLiteral("tool_result"), resultEv);
            rec.raw = raw;
        } else {
            rec.raw = ev;
        }
        return push(rec);
    };

    for (const QVariant &value : m_events) {
        const QVariantMap ev = value.toMap();
        const QString type = vstr(ev, "type");
        const QString ts = vstr(ev, "ts");

        if (type == QLatin1String("user")) {
            QString content = vstr(ev, "display_content");
            if (content.isEmpty())
                content = vstr(ev, "content");
            TrajRecord rec;
            rec.source = QStringLiteral("user");
            rec.kind = QStringLiteral("user");
            rec.turn = vint(ev, "turn");
            rec.request = 0;
            rec.step = 0;
            rec.status = QStringLiteral("completed");
            rec.label = QStringLiteral("用户消息");
            rec.content = content;
            const int tokens = estimateTokens(content);
            rec.hasTokens = true;
            rec.tokens.total = tokens;
            rec.tokens.reasoning = 0;
            rec.tokens.content = tokens;
            rec.tokens.estimated = true;
            rec.raw = ev;
            rec.ts = ts;
            push(rec);
        } else if (type == QLatin1String("traj_system_prompt")) {
            const QString content = vstr(ev, "content");
            TrajRecord rec;
            rec.source = QStringLiteral("system");
            rec.kind = QStringLiteral("system_prompt");
            rec.turn = vint(ev, "turn");
            rec.request = 0;
            rec.step = 0;
            rec.status = QStringLiteral("completed");
            rec.label = QStringLiteral("初始系统提示词");
            rec.content = content;
            const int tokens = estimateTokens(content);
            rec.hasTokens = true;
            rec.tokens.total = tokens;
            rec.tokens.reasoning = 0;
            rec.tokens.content = tokens;
            rec.tokens.estimated = true;
            rec.raw = ev;
            rec.ts = ts;
            push(rec);
        } else if (type == QLatin1String("traj_context")) {
            QString kind = vstr(ev, "kind");
            if (kind.isEmpty())
                kind = QStringLiteral("context");
            const QString content = vstr(ev, "content");
            TrajRecord rec;
            rec.source = QStringLiteral("context");
            rec.kind = kind;
            rec.turn = vint(ev, "turn");
            rec.request = vint(ev, "request");
            rec.step = 0;
            rec.status = QStringLiteral("completed");
            rec.label = (kind == QLatin1String("ledger_snapshot")) ? QStringLiteral("台账快照")
                                                                   : QStringLiteral("上下文注入");
            rec.content = content;
            const int tokens = estimateTokens(content);
            rec.hasTokens = true;
            rec.tokens.total = tokens;
            rec.tokens.reasoning = 0;
            rec.tokens.content = tokens;
            rec.tokens.estimated = true;
            rec.raw = ev;
            rec.ts = ts;
            push(rec);
        } else if (type == QLatin1String("traj_request_start")) {
            const int turn = vint(ev, "turn");
            const int request = vint(ev, "request");
            const QString start = vstr(ev, "start");
            const QString key = QString::number(turn) + QLatin1Char('.') + QString::number(request);
            openRequests.insert(key, newRequest(turn, request, vstr(ev, "model"),
                                                start.isEmpty() ? ts : start, ev));
        } else if (type == QLatin1String("traj_request_end")) {
            const int turn = vint(ev, "turn");
            const int request = vint(ev, "request");
            const QString key = QString::number(turn) + QLatin1Char('.') + QString::number(request);
            int index = openRequests.value(key, -1);
            if (index < 0) {
                const QVariantMap timing = ev.value(QStringLiteral("timing")).toMap();
                QString start = timing.value(QStringLiteral("start")).toString();
                if (start.isEmpty())
                    start = ts;
                index = newRequest(turn, request, vstr(ev, "model"), start, ev);
                openRequests.insert(key, index);
            }
            TrajRecord &rec = m_records[index];
            const QString status = vstr(ev, "status");
            rec.status = status.isEmpty() ? QStringLiteral("completed") : status;
            const QString model = vstr(ev, "model");
            if (!model.isEmpty())
                rec.model = model;
            rec.content = vstr(ev, "content");
            rec.reasoning = vstr(ev, "reasoning");
            rec.toolCalls.clear();
            for (const QVariant &tc : ev.value(QStringLiteral("tool_calls")).toList()) {
                const QVariantMap map = tc.toMap();
                QVariantMap item;
                item.insert(QStringLiteral("id"), map.value(QStringLiteral("id")).toString());
                item.insert(QStringLiteral("name"), map.value(QStringLiteral("name")).toString());
                item.insert(QStringLiteral("args"), map.value(QStringLiteral("args")).toMap());
                rec.toolCalls.append(item);
            }
            const QVariant timingValue = ev.value(QStringLiteral("timing"));
            if (timingValue.isValid() && !timingValue.isNull() && timingValue.canConvert<QVariantMap>()) {
                const QVariantMap timing = timingValue.toMap();
                rec.hasTiming = true;
                const QString start = timing.value(QStringLiteral("start")).toString();
                if (!start.isEmpty())
                    rec.timing.start = start;
                double d = 0;
                rec.timing.totalSet = vnum(timing, "total_ms", &d);
                rec.timing.totalMs = rec.timing.totalSet ? d : 0;
                rec.timing.ttftSet = vnum(timing, "ttft_ms", &d);
                rec.timing.ttftMs = rec.timing.ttftSet ? d : 0;
                rec.timing.genSet = vnum(timing, "gen_ms", &d);
                rec.timing.genMs = rec.timing.genSet ? d : 0;
                rec.timing.tpsSet = vnum(timing, "tok_per_s", &d);
                rec.timing.tokPerS = rec.timing.tpsSet ? d : 0;
            }
            rec.raw = ev;
            const QVariantMap usage = ev.value(QStringLiteral("usage")).toMap();
            double d = 0;
            const int usageTotal = vnum(usage, "total", &d) ? int(d) : 0;
            const int usageOutput = vnum(usage, "output", &d) ? int(d) : 0;
            const int contentTokens = estimateTokens(rec.content);
            const int reasoningTokens = estimateTokens(rec.reasoning);
            rec.hasTokens = true;
            rec.tokens.total = usageTotal ? usageTotal
                                          : (usageOutput ? usageOutput : contentTokens + reasoningTokens);
            rec.tokens.reasoning = reasoningTokens;
            rec.tokens.content = contentTokens;
            rec.tokens.estimated = (usageTotal == 0);
            openRequests.remove(key);
        } else if (type == QLatin1String("tool_call")) {
            openTools.insert(vstr(ev, "id"), newTool(ev, false, QVariantMap()));
        } else if (type == QLatin1String("tool_result")) {
            const QString callId = vstr(ev, "call_id");
            int index = openTools.value(callId, -1);
            if (index < 0) {
                QVariantMap synth;
                synth.insert(QStringLiteral("id"), callId);
                synth.insert(QStringLiteral("name"), vstr(ev, "name"));
                synth.insert(QStringLiteral("turn"), vint(ev, "turn"));
                synth.insert(QStringLiteral("request"), vint(ev, "request"));
                synth.insert(QStringLiteral("step"), vint(ev, "step"));
                synth.insert(QStringLiteral("ts"), vstr(ev, "ts"));
                index = newTool(synth, true, ev);
                openTools.insert(callId, index);
            }
            TrajRecord &rec = m_records[index];
            const QVariant resultValue = ev.value(QStringLiteral("result"));
            const QString result = resultValue.isNull() ? QString() : resultValue.toString();
            rec.content = result;
            rec.status = failedRe().match(result).hasMatch() ? QStringLiteral("failed")
                                                             : QStringLiteral("completed");
            double d = 0;
            rec.hasDurationMs = vnum(ev, "duration_ms", &d);
            rec.durationMs = rec.hasDurationMs ? d : 0;
            const QString resultTs = vstr(ev, "ts");
            if (!resultTs.isEmpty())
                rec.ts = resultTs;
            QVariantMap raw;
            const QVariantMap existing = rec.raw.toMap();
            raw.insert(QStringLiteral("tool_call"),
                       existing.contains(QStringLiteral("tool_call"))
                           ? existing.value(QStringLiteral("tool_call"))
                           : rec.raw);
            raw.insert(QStringLiteral("tool_result"), ev);
            rec.raw = raw;
            openTools.remove(callId);
        }
    }

    // markTurnStarts：轮次边界
    bool hasLast = false;
    int last = 0;
    for (TrajRecord &rec : m_records) {
        rec.turnStart = (!hasLast || rec.turn != last);
        hasLast = true;
        last = rec.turn;
    }
}

// ---------- 渲染调度 ----------

void TrajectoryView::refreshAll()
{
    projectRecords();
    rebuildTimeline();
    rebuildLedger();
    rebuildInspector();
}

// 由子控件（行 / 分组 / 时间轴 / 详情面板）触发的刷新：延迟一帧，
// 避免在事件派发过程中删除正在处理事件的控件
void TrajectoryView::postRefresh()
{
    if (m_rebuildPending)
        return;
    m_rebuildPending = true;
    QTimer::singleShot(0, this, [this] {
        m_rebuildPending = false;
        refreshAll();
    });
}

// ---------- 时间轴 ----------

void TrajectoryView::rebuildTimeline()
{
    QList<TrajSpan> spans;
    if (!m_actualDuration) {
        // 等宽投影：横轴 = 记录序号；初始系统提示词固定排在最前
        int preambleIdx = -1;
        for (int i = 0; i < m_records.size(); ++i) {
            if (m_records[i].kind == QLatin1String("system_prompt")) {
                preambleIdx = i;
                break;
            }
        }
        QList<const TrajRecord *> sequence;
        if (preambleIdx >= 0)
            sequence.append(&m_records[preambleIdx]);
        for (int i = 0; i < m_records.size(); ++i) {
            if (i != preambleIdx)
                sequence.append(&m_records[i]);
        }

        double lastTs = 0;
        for (int index = 0; index < sequence.size(); ++index) {
            const TrajRecord *rec = sequence[index];
            const double ts = timeMsOf(*rec);
            if (!std::isnan(ts))
                lastTs = ts;
            const bool isTool = rec->source == QLatin1String("tool");
            const bool isRequest = rec->kind == QLatin1String("request");
            const QString failed = (rec->status == QLatin1String("failed")) ? QStringLiteral(" failed")
                                                                           : QString();
            double ms = 0;
            const bool hasMs = recordMs(*rec, &ms);
            const QString label = isTool
                ? (rec->name.isEmpty() ? QStringLiteral("工具") : rec->name)
                : (rec->label.isEmpty() ? sourceLabel(rec->source) : rec->label);
            const QString title = label + (hasMs ? QStringLiteral(" · ") + formatMs(true, ms) : QString());

            TrajSpan span;
            span.lane = isTool ? 2 : (isRequest ? 1 : 0);
            span.start = lastTs;
            span.end = lastTs;
            span.ds = index;
            span.de = index + 1;
            span.cls = spanClass(rec->source) + failed;
            span.id = rec->id;
            span.title = title;
            spans.append(span);

            // 请求的首 token 占该请求格子的前一段
            const TrajTiming &timing = rec->timing;
            if (isRequest && rec->hasTiming && timing.totalSet && timing.ttftSet
                && timing.totalMs > 0 && timing.ttftMs > 0 && timing.ttftMs < timing.totalMs) {
                TrajSpan head;
                head.lane = 1;
                head.start = lastTs;
                head.end = lastTs;
                head.ds = index;
                head.de = index + timing.ttftMs / timing.totalMs;
                head.cls = QStringLiteral("ttft") + failed;
                head.id = rec->id;
                head.title = title + QStringLiteral(" · 首 token ") + formatMs(true, timing.ttftMs);
                spans.append(head);
            }
        }
    } else {
        // 实际时长投影：横轴 = 真实耗时，空转间隔不计入宽度
        for (const TrajRecord &rec : m_records) {
            double ms = 0;
            const bool hasMs = recordMs(rec, &ms);
            const QString durTxt = hasMs ? QStringLiteral(" · ") + formatMs(true, ms) : QString();

            if (rec.source == QLatin1String("user")) {
                const double t = timeMsOf(rec);
                if (!std::isnan(t)) {
                    TrajSpan span;
                    span.lane = 0;
                    span.start = t;
                    span.end = t + 1;
                    span.cls = QStringLiteral("user");
                    span.id = rec.id;
                    span.title = (rec.label.isEmpty() ? QStringLiteral("用户消息") : rec.label)
                                 + QStringLiteral(" · ") + rec.ts;
                    spans.append(span);
                }
            } else if (rec.kind == QLatin1String("request") && rec.hasTiming && !rec.timing.start.isEmpty()) {
                const double t = parseIsoMs(rec.timing.start);
                if (std::isnan(t))
                    continue;
                const double total = rec.timing.totalSet ? rec.timing.totalMs : 0.0;
                const QString title = rec.label + durTxt;
                TrajSpan span;
                span.lane = 1;
                span.start = t;
                span.end = t + qMax(total, 1.0);
                span.cls = (rec.status == QLatin1String("failed")) ? QStringLiteral("model failed")
                                                                   : QStringLiteral("model");
                span.id = rec.id;
                span.title = title;
                spans.append(span);
                if (rec.timing.ttftSet && rec.timing.ttftMs > 0) {
                    TrajSpan head;
                    head.lane = 1;
                    head.start = t;
                    head.end = t + rec.timing.ttftMs;
                    head.cls = QStringLiteral("ttft");
                    head.id = rec.id;
                    head.title = title + QStringLiteral(" · 首 token ") + formatMs(true, rec.timing.ttftMs);
                    spans.append(head);
                }
            } else if (rec.source == QLatin1String("tool") && rec.hasDurationMs) {
                const double endT = timeMsOf(rec);
                if (!std::isnan(endT)) {
                    TrajSpan span;
                    span.lane = 2;
                    span.start = endT - rec.durationMs;
                    span.end = endT;
                    span.cls = (rec.status == QLatin1String("failed")) ? QStringLiteral("tool failed")
                                                                       : QStringLiteral("tool");
                    span.id = rec.id;
                    span.title = (rec.name.isEmpty() ? QStringLiteral("工具") : rec.name) + durTxt;
                    spans.append(span);
                }
            }
        }
        // 折叠空转：没有任何 span 覆盖的时段不计入宽度
        QList<int> order;
        for (int i = 0; i < spans.size(); ++i)
            order.append(i);
        std::sort(order.begin(), order.end(), [&spans](int a, int b) {
            if (spans[a].start != spans[b].start)
                return spans[a].start < spans[b].start;
            return spans[a].end < spans[b].end;
        });
        double removed = 0;
        bool hasCovered = false;
        double covered = 0;
        for (int i : order) {
            TrajSpan &span = spans[i];
            if (hasCovered && span.start > covered)
                removed += span.start - covered;
            span.idle = removed;
            covered = hasCovered ? qMax(covered, span.end) : span.end;
            hasCovered = true;
        }
        for (TrajSpan &span : spans) {
            span.ds = span.start - span.idle;
            span.de = span.end - span.idle;
        }
    }

    bool valid = false;
    double dMin = 0;
    double dMax = 1;
    for (const TrajSpan &span : spans) {
        if (!valid) {
            dMin = span.ds;
            dMax = span.de;
            valid = true;
        } else {
            dMin = qMin(dMin, span.ds);
            dMax = qMax(dMax, span.de);
        }
    }
    if (valid && dMax - dMin <= 0)
        dMax = dMin + 1; // 域宽为 0（单条瞬时记录）时补 1 格避免除零

    m_timeline->setSpans(spans, valid, dMin, dMax);
    m_timeline->setSelectedId(m_selected);
}

// ---------- 账本 ----------

void TrajectoryView::rebuildLedger()
{
    if (!m_ledgerLayout)
        return;
    // 过滤（查询 / 框选）后算出「条目列表」：这一步只产生数据、不建控件
    QList<TrajRecord> records = m_records;
    const QString query = m_query.trimmed().toLower();
    if (!query.isEmpty()) {
        QList<TrajRecord> filtered;
        for (const TrajRecord &rec : records) {
            if (recordText(rec).toLower().contains(query))
                filtered.append(rec);
        }
        records = filtered;
    }
    if (m_hasRange) {
        QList<TrajRecord> filtered;
        for (const TrajRecord &rec : records) {
            if (m_rangeIds.contains(rec.id))
                filtered.append(rec);
        }
        records = filtered;
    }

    clearLedgerWindow();
    buildLedgerEntries(records);
    refreshLedgerPrefix();
    updateLedgerWindow(true);

    if (!m_selected.isEmpty()) {
        // 滚动位置要等布局把 scrollbar 的 range 算出来（下一帧）才对得准
        QTimer::singleShot(0, this, [this] {
            if (!m_selected.isEmpty())
                scrollToRecord(m_selected);
        });
    }
}

// ---------- 条目计算（纯数据，不建控件） ----------

QString TrajectoryView::groupLabelText(const TrajRecord &rec) const
{
    if (m_view == QLatin1String("turn"))
        return QStringLiteral("第 %1 轮").arg(rec.turn);
    return rec.request ? QStringLiteral("第 %1 轮 · 请求 #%2").arg(rec.turn).arg(rec.request)
                       : QStringLiteral("第 %1 轮 · 输入与上下文").arg(rec.turn);
}

void TrajectoryView::buildLedgerEntries(const QList<TrajRecord> &records)
{
    m_ledgerEntries.clear();
    m_groupEntryKey.clear();
    m_lastGroupKey.clear();
    m_hasLastGroup = false;
    m_ledgerRecordsShown = 0;
    m_ledgerEmpty = false;
    if (records.isEmpty()) {
        LedgerEntry entry;
        entry.kind = LedgerKind::Empty;
        m_ledgerEntries.append(entry);
        m_ledgerEmpty = true;
        return;
    }
    appendLedgerEntries(records, 0);
}

// 把 records 里 fromRecord 之后的记录换算成条目：全量重建（fromRecord == 0）与增量追加共用
void TrajectoryView::appendLedgerEntries(const QList<TrajRecord> &records, int fromRecord)
{
    const bool grouped = !m_view.isEmpty();
    const QHash<QString, LedgerGroupStats> groupStats = computeGroupStats(records);

    // 初始系统提示词不属于某一轮：任何视图下都固定排到最前
    int preambleIdx = -1;
    for (int i = 0; i < records.size(); ++i) {
        if (records[i].kind == QLatin1String("system_prompt")) {
            preambleIdx = i;
            break;
        }
    }
    if (fromRecord == 0 && preambleIdx >= 0) {
        LedgerEntry entry;
        entry.kind = LedgerKind::Preamble;
        entry.recordIndex = preambleIdx;
        m_ledgerEntries.append(entry);
    }

    QHash<QString, bool> summarized;
    QSet<QString> touchedGroups;
    for (int i = qMax(0, fromRecord); i < records.size(); ++i) {
        if (i == preambleIdx)
            continue;
        const TrajRecord &rec = records[i];
        const QString groupKey = grouped ? groupKeyOf(m_view, rec) : QString();
        if (grouped)
            touchedGroups.insert(groupKey);
        if (grouped && (!m_hasLastGroup || groupKey != m_lastGroupKey)) {
            m_hasLastGroup = true;
            m_lastGroupKey = groupKey;
            LedgerEntry entry;
            entry.kind = LedgerKind::Group;
            entry.groupKey = groupKey;
            entry.label = groupLabelText(rec);
            entry.stats = groupStats.value(groupKey);
            m_ledgerEntries.append(entry);
            m_groupEntryKey.insert(groupKey, entry.label);
        }
        if (grouped && m_collapsed.value(groupKey, false)) {
            // 轮次视图收起时保留本轮用户消息行，其余折成一行摘要
            if (m_view == QLatin1String("turn") && rec.source == QLatin1String("user")) {
                LedgerEntry row;
                row.kind = LedgerKind::Row;
                row.recordIndex = i;
                row.markTurnStart = true;
                m_ledgerEntries.append(row);
                continue;
            }
            if (m_view == QLatin1String("turn") && !summarized.value(groupKey, false)) {
                summarized.insert(groupKey, true);
                LedgerEntry summary;
                summary.kind = LedgerKind::Summary;
                summary.groupKey = groupKey;
                summary.stats = groupStats.value(groupKey);
                m_ledgerEntries.append(summary);
            }
            continue;
        }
        LedgerEntry row;
        row.kind = LedgerKind::Row;
        row.recordIndex = i;
        row.markTurnStart = m_view != QLatin1String("call");
        m_ledgerEntries.append(row);
    }

    m_ledgerRecordsShown = records.size();

    if (!grouped || touchedGroups.isEmpty())
        return;
    // 新记录会让所在分组的统计过期（「3 条」→「5 条」）：条目里的数据必须更新；
    // 恰好实例化在可视区里的分组头顺便就地刷新文案，其余等滚进视口时按新数据重建
    for (LedgerEntry &entry : m_ledgerEntries) {
        if (entry.kind != LedgerKind::Group || !touchedGroups.contains(entry.groupKey))
            continue;
        entry.stats = groupStats.value(entry.groupKey);
        const LedgerGroupHeader header = widgetsOfGroup(entry.groupKey);
        if (header.count)
            header.count->setText(QString::number(entry.stats.count));
        if (header.meta)
            header.meta->setText(groupMetaText(entry.stats));
    }
}

void TrajectoryView::refreshLedgerPrefix()
{
    m_ledgerPrefix.resize(m_ledgerEntries.size() + 1);
    m_ledgerPrefix[0] = 0;
    for (int i = 0; i < m_ledgerEntries.size(); ++i)
        m_ledgerPrefix[i + 1] = m_ledgerPrefix[i] + ledgerEntryHeight(m_ledgerEntries[i]);
}

// ---------- 视窗化：只给可视区实例化行控件 ----------

int TrajectoryView::ledgerEntryHeight(const LedgerEntry &entry) const
{
    const int measured = m_ledgerKindHeights.value(int(entry.kind), 0);
    if (measured > 0)
        return measured;
    // 还没实测过（首次、或缩放刚变过）：先按名义高度占位，
    // 建完窗口会在同一次调用里实测并纠正（见 updateLedgerWindow 末尾）
    return qMax(30, scaledPx(30));
}

int TrajectoryView::ledgerVisibleHeight() const
{
    return (m_ledger && m_ledger->viewport()) ? m_ledger->viewport()->height() : 0;
}

// 内容坐标（inner 部件坐标）→ 条目下标：前缀高度数组上二分
int TrajectoryView::ledgerEntryIndexAt(int y) const
{
    if (m_ledgerEntries.isEmpty() || m_ledgerPrefix.size() != m_ledgerEntries.size() + 1)
        return 0;
    const int contentY = qMax(0, y - kLedgerTopMargin);
    int lo = 0;
    int hi = m_ledgerEntries.size() - 1;
    int found = 0;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (m_ledgerPrefix[mid] <= contentY) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return found;
}

QWidget *TrajectoryView::buildLedgerEntryWidget(const LedgerEntry &entry, double maxMs)
{
    switch (entry.kind) {
    case LedgerKind::Empty: {
        auto *empty = mkLabel(QStringLiteral("trajEmpty"), QStringLiteral("暂无轨迹记录"), m_ledgerInner);
        empty->setAlignment(Qt::AlignCenter);
        empty->setContentsMargins(0, 24, 0, 24);
        return empty;
    }
    case LedgerKind::Preamble:
        return buildRow(m_records[entry.recordIndex], maxMs, false);
    case LedgerKind::Group:
        return buildGroup(entry, m_collapsed.value(entry.groupKey, false));
    case LedgerKind::Summary:
        return buildSummary(entry.groupKey, groupSummaryText(entry.stats));
    case LedgerKind::Row:
        return buildRow(m_records[entry.recordIndex], maxMs, entry.markTurnStart);
    }
    return nullptr;
}

// 只清掉当前实例化的行：不动窗口范围与 spacer——换行过程中内容总高度要保持稳定，
// 否则「先删行」会让内容瞬间变矮、scrollbar 被 clamp，进而触发重入与跳动
void TrajectoryView::clearLedgerWindowRows()
{
    for (QWidget *row : qAsConst(m_ledgerWindowRows)) {
        if (!row)
            continue;
        m_ledgerLayout->removeWidget(row);
        row->hide();
        row->deleteLater();
    }
    m_ledgerWindowRows.clear();
    m_rowWidgets.clear();
    m_selectedRow = nullptr;
}

void TrajectoryView::clearLedgerWindow()
{
    clearLedgerWindowRows();
    m_ledgerWindowFrom = 0;
    m_ledgerWindowTo = 0;
}

void TrajectoryView::invalidateLedgerHeights()
{
    m_ledgerKindHeights.clear();
    m_ledgerHeightZoom = -1.0;
}

void TrajectoryView::updateLedgerSpacers()
{
    if (!m_ledgerSpacerTop || !m_ledgerSpacerBottom)
        return;
    if (m_ledgerPrefix.size() != m_ledgerEntries.size() + 1)
        return;
    const int count = m_ledgerEntries.size();
    const int from = qBound(0, m_ledgerWindowFrom, count);
    const int to = qBound(from, m_ledgerWindowTo, count);
    m_ledgerSpacerTop->setFixedHeight(m_ledgerPrefix[from]);
    m_ledgerSpacerBottom->setFixedHeight(m_ledgerPrefix[count] - m_ledgerPrefix[to]);
}

void TrajectoryView::updateLedgerWindow(bool force)
{
    if (!m_ledgerLayout || !m_ledger || m_ledgerEntries.isEmpty())
        return;
    if (m_ledgerUpdating)
        return; // 换行的副作用又改了 scrollbar：忽略这次重入，外层结束时状态是自洽的
    m_ledgerUpdating = true;
    // 缩放（字号）变了：行高实测作废、重新测
    if (!qFuzzyCompare(m_ledgerHeightZoom + 1.0, gs::zoomFactor() + 1.0))
        invalidateLedgerHeights();
    if (m_ledgerPrefix.size() != m_ledgerEntries.size() + 1)
        refreshLedgerPrefix();

    const int top = m_ledger->verticalScrollBar()->value();
    const int viewH = ledgerVisibleHeight();
    const int count = m_ledgerEntries.size();
    int from = ledgerEntryIndexAt(qMax(0, top - kLedgerOverscan));
    int to = ledgerEntryIndexAt(top + viewH + kLedgerOverscan) + 1;
    from = qBound(0, from, count);
    to = qBound(from, to, count);
    if (!force && from == m_ledgerWindowFrom && to == m_ledgerWindowTo) {
        m_ledgerUpdating = false;
        return; // 视口没变：一个控件都不用动
    }

    // 先按新窗口调好 spacer，再换行控件：内容总高度在换行过程中保持稳定
    m_ledgerWindowFrom = from;
    m_ledgerWindowTo = to;
    updateLedgerSpacers();
    clearLedgerWindowRows();
    const double maxMs = maxRecordMs(m_records);
    for (int i = from; i < to; ++i) {
        QWidget *row = buildLedgerEntryWidget(m_ledgerEntries[i], maxMs);
        if (!row)
            continue;
        // 行夹在两个 spacer 之间：顶部 spacer 固定在 0，新行依次插到它后面
        m_ledgerLayout->insertWidget(1 + m_ledgerWindowRows.size(), row);
        m_ledgerWindowRows.append(row);
    }

    // 首次（或缩放后）实测行高：同类条目行高一致（单行文本 + 同样边距），
    // 实测值与占位值不符时纠正一次，保证「滚动位置 ↔ 内容高度」对得上
    bool corrected = false;
    for (int i = 0; i < m_ledgerWindowRows.size(); ++i) {
        QWidget *row = m_ledgerWindowRows[i];
        row->ensurePolished(); // QSS 的边距/字号要先落地，sizeHint 才是最终行高
        const LedgerEntry &entry = m_ledgerEntries[m_ledgerWindowFrom + i];
        const int height = qMax(row->minimumHeight(), row->sizeHint().height());
        if (m_ledgerKindHeights.value(int(entry.kind), 0) != height) {
            m_ledgerKindHeights.insert(int(entry.kind), height);
            corrected = true;
        }
    }
    m_ledgerHeightZoom = gs::zoomFactor();
    m_ledgerUpdating = false; // 先放开重入锁：下面的「按实测行高重排一次」要能进来
    if (corrected && !m_ledgerMeasuring) {
        m_ledgerMeasuring = true;
        refreshLedgerPrefix();
        updateLedgerWindow(true);
        m_ledgerMeasuring = false;
    }
}

// 把某条记录滚进视口（可选行没实例化也能算出来：位置来自前缀高度）
bool TrajectoryView::scrollToRecord(const QString &id)
{
    if (id.isEmpty() || !m_ledger || m_ledgerPrefix.size() != m_ledgerEntries.size() + 1)
        return false;
    int index = -1;
    for (int i = 0; i < m_ledgerEntries.size(); ++i) {
        const LedgerEntry &entry = m_ledgerEntries[i];
        if (entry.kind != LedgerKind::Row || entry.recordIndex < 0
            || entry.recordIndex >= m_records.size())
            continue;
        if (m_records[entry.recordIndex].id == id) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return false; // 被过滤掉、或收在收起的分组里：没有对应的行

    QScrollBar *bar = m_ledger->verticalScrollBar();
    const int y = m_ledgerPrefix[index];
    const int height = m_ledgerPrefix[index + 1] - y;
    const int viewH = ledgerVisibleHeight();
    const int viewTop = bar->value() - kLedgerTopMargin;
    if (y + height > viewTop && y < viewTop + viewH)
        return true; // 已经在视口里，不动滚动位置
    const int target = y - qMax(0, (viewH - height) / 2) + kLedgerTopMargin;
    bar->setValue(qBound(bar->minimum(), target, bar->maximum()));
    updateLedgerWindow(true);
    return true;
}

QString TrajectoryView::groupMetaText(const LedgerGroupStats &stats) const
{
    QStringList bits;
    bits << QStringLiteral("%1 条").arg(stats.count);
    if (stats.tokens)
        bits << QStringLiteral("%1 tok").arg(stats.tokens);
    if (stats.ms > 0)
        bits << formatMs(true, stats.ms);
    return bits.join(QStringLiteral(" · "));
}

QString TrajectoryView::groupSummaryText(const LedgerGroupStats &stats) const
{
    return QStringLiteral("… %1 个步骤 · %2 个工具调用").arg(stats.steps).arg(stats.tools);
}

QHash<QString, TrajectoryView::LedgerGroupStats>
TrajectoryView::computeGroupStats(const QList<TrajRecord> &list) const
{
    QHash<QString, LedgerGroupStats> stats;
    if (m_view.isEmpty())
        return stats;
    for (const TrajRecord &rec : list) {
        LedgerGroupStats &group = stats[groupKeyOf(m_view, rec)];
        group.count += 1;
        if (rec.hasTokens && rec.tokens.total)
            group.tokens += rec.tokens.total;
        double ms = 0;
        if (recordMs(rec, &ms))
            group.ms += ms;
        if (rec.source == QLatin1String("user"))
            continue;
        if (rec.kind == QLatin1String("request"))
            group.steps += 1;
        if (rec.source == QLatin1String("tool"))
            group.tools += 1;
    }
    return stats;
}

// 增量追加：只把新增记录换算成条目（控件依旧只服务可视区）
void TrajectoryView::appendLedgerTail()
{
    const int before = m_ledgerEntries.size();
    appendLedgerEntries(m_records, m_ledgerRecordsShown);
    if (m_ledgerPrefix.size() < before + 1) {
        refreshLedgerPrefix(); // 理论上不该发生（前缀一直跟着条目走），兜底
    } else {
        // 前缀只需追加新增部分：O(新增) 而不是 O(全量)
        m_ledgerPrefix.resize(m_ledgerEntries.size() + 1);
        for (int i = before; i < m_ledgerEntries.size(); ++i)
            m_ledgerPrefix[i + 1] = m_ledgerPrefix[i] + ledgerEntryHeight(m_ledgerEntries[i]);
    }
    // 内容变高了：底 spacer 跟着长。这一步只动 3 个布局项，与总行数无关
    updateLedgerSpacers();
    // 只有「窗口本来就贴到末尾」时才补建新行：用户滚在上面看历史时一个控件都不用动
    if (m_ledgerWindowTo >= before)
        updateLedgerWindow(true);
}

bool TrajectoryView::canAppendLedger(const QVariantList &events) const
{
    if (m_ledgerEmpty || !m_query.trimmed().isEmpty() || m_hasRange)
        return false;
    if (m_hasLastGroup && m_collapsed.value(m_lastGroupKey, false))
        return false;
    for (const QVariant &value : events) {
        // 新的系统提示词要排到最前，位置不固定：退回全量重建
        if (vstr(value.toMap(), "type") == QLatin1String("system_prompt"))
            return false;
    }
    // 分组视图下，新增记录只能「续在最后一组之后」：同组继续、或开一个新组。
    // 若落在前面某个已有分组里（事件乱序），插到末尾会重复分组头且顺序错乱 → 全量重建
    if (m_hasLastGroup) {
        for (int i = qMax(0, m_ledgerRecordsShown); i < m_records.size(); ++i) {
            const QString key = groupKeyOf(m_view, m_records[i]);
            if (key != m_lastGroupKey && m_groupEntryKey.contains(key))
                return false;
        }
    }
    return true;
}

// 选择态只影响两行的描边与时间轴：整账本重建（几千行）代价太大
void TrajectoryView::applySelectionHighlight(const QString &id)
{
    if (m_selectedRow) {
        m_selectedRow->setProperty("selected", false);
        restyle(m_selectedRow);
    }
    QWidget *row = id.isEmpty() ? nullptr : m_rowWidgets.value(id).data();
    if (row) {
        row->setProperty("selected", true);
        restyle(row);
    }
    m_selectedRow = row;
    m_timeline->setSelectedId(id);
}

QWidget *TrajectoryView::buildRow(const TrajRecord &rec, double maxMs, bool markTurnStart)
{
    auto *row = new TrajHitBox(m_ledgerInner);
    setClass(row, QStringLiteral("trajRow"));
    row->setProperty("selected", rec.id == m_selected);
    row->setMinimumHeight(30);
    m_rowWidgets.insert(rec.id, row); // 移动高亮时按 id 找行，不必重建账本

    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 4, 10, 4);
    layout->setSpacing(8);

    // 轮次边界：左侧竖线（QSS 无该状态，颜色取自调色板）
    auto *indicator = new QFrame(row);
    indicator->setFixedWidth(3);
    indicator->setStyleSheet(markTurnStart
        ? QStringLiteral("background: %1;").arg(cssColor(gs::palette().cyanMid))
        : QStringLiteral("background: transparent;"));
    layout->addWidget(indicator, 0);

    // 徽标列：列宽固定、内容靠右
    auto *badgeCell = new QWidget(row);
    badgeCell->setFixedWidth(46);
    auto *badgeLayout = new QHBoxLayout(badgeCell);
    badgeLayout->setContentsMargins(0, 0, 0, 0);
    badgeLayout->setSpacing(0);
    badgeLayout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *badge = mkLabel(QStringLiteral("trajBadge"), sourceLabel(rec.source), badgeCell);
    badge->setProperty("source", rec.source);
    badgeLayout->addWidget(badge, 0);
    layout->addWidget(badgeCell, 0);

    // 主体
    auto *main = new QWidget(row);
    auto *mainLayout = new QHBoxLayout(main);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(8);

    auto shrinkable = [](QLabel *label) {
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    };

    if (rec.source == QLatin1String("tool")) {
        auto *name = mkLabel(QStringLiteral("trajRowLabel"), rec.name, main);
        name->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        mainLayout->addWidget(name, 0);
        const QString text = preview(rec.status == QLatin1String("running")
                                         ? jsonAny(rec.args, false)
                                         : rec.content, 160);
        auto *comment = mkElided(QStringLiteral("trajRowPreview"), text, main);
        shrinkable(comment);
        mainLayout->addWidget(comment, 1);
    } else {
        auto *label = mkLabel(QStringLiteral("trajRowLabel"), rec.label, main);
        label->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
        mainLayout->addWidget(label, 0);
        if (!rec.model.isEmpty()) {
            auto *model = mkLabel(QStringLiteral("trajRowMeta"), rec.model, main);
            model->setMaximumWidth(140);
            shrinkable(model);
            mainLayout->addWidget(model, 0);
        }
        auto *comment = mkElided(QStringLiteral("trajRowPreview"), preview(rec.content, 160), main);
        shrinkable(comment);
        mainLayout->addWidget(comment, 1);
    }
    layout->addWidget(main, 1);

    // 右侧 meta：token 数 + 耗时
    auto *meta = mkLabel(QStringLiteral("trajRowMeta"), rowMetaText(rec), row);
    layout->addWidget(meta, 0);

    // 行内耗时条：与本视图最长耗时成比例；无耗时只画空轨道
    if (maxMs > 0) {
        auto *bar = new MiniBar(row);
        setClass(bar, QStringLiteral("trajRowBar"));
        bar->setFixedWidth(90);
        double ms = 0;
        if (recordMs(rec, &ms) && ms > 0)
            bar->setRatio(qBound(0.0, ms / maxMs, 1.0));
        else
            bar->setRatio(-1.0);
        bar->setFill(sourceFill(rec));
        layout->addWidget(bar, 0);
    }

    if (rec.id == m_selected)
        m_selectedRow = row;

    const QString id = rec.id;
    row->setHandler([this, id] { selectRecord(id); });
    return row;
}

QWidget *TrajectoryView::buildGroup(const LedgerEntry &entry, bool collapsed, QLabel **countOut,
                                    QLabel **metaOut)
{
    auto *group = new TrajHitBox(m_ledgerInner);
    setClass(group, QStringLiteral("trajGroup"));
    group->setProperty("collapsed", collapsed);
    // 视窗化后分组头只可能「恰好实例化」在可视区里：靠这个属性把它的统计标签找出来就地刷新
    group->setProperty("groupKey", entry.groupKey);
    group->setCursor(Qt::PointingHandCursor);

    auto *layout = new QHBoxLayout(group);
    layout->setContentsMargins(12, 8, 12, 4);
    layout->setSpacing(4);

    auto *caret = mkLabel(QString(), collapsed ? QStringLiteral("▸") : QStringLiteral("▾"), group);
    caret->setFixedWidth(12);
    caret->setStyleSheet(QStringLiteral("color: %1; font-size: %2px; background: transparent;")
                             .arg(cssColor(gs::palette().muted2)).arg(scaledPx(9)));
    layout->addWidget(caret, 0);

    auto *labelWidget = mkElided(QStringLiteral("trajGroupLabel"), entry.label, group);
    labelWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(labelWidget, 1);

    auto *countWidget = mkLabel(QStringLiteral("trajGroupMeta"), QString::number(entry.stats.count),
                                group);
    countWidget->setObjectName(QStringLiteral("trajGroupCount"));
    layout->addWidget(countWidget, 0);

    layout->addStretch(1);

    auto *metaWidget = mkLabel(QStringLiteral("trajGroupMeta"), groupMetaText(entry.stats), group);
    metaWidget->setObjectName(QStringLiteral("trajGroupMetaText"));
    layout->addWidget(metaWidget, 0);

    if (countOut)
        *countOut = countWidget;
    if (metaOut)
        *metaOut = metaWidget;

    const QString key = entry.groupKey;
    group->setHandler([this, key] { toggleGroup(key); });
    return group;
}

// 当前实例化在可视区里的分组头（不在可视区就没有控件，也就无需刷新）
TrajectoryView::LedgerGroupHeader TrajectoryView::widgetsOfGroup(const QString &key) const
{
    LedgerGroupHeader header;
    for (QWidget *widget : m_ledgerWindowRows) {
        if (!widget || widget->property("groupKey").toString() != key)
            continue;
        header.count = widget->findChild<QLabel *>(QStringLiteral("trajGroupCount"));
        header.meta = widget->findChild<QLabel *>(QStringLiteral("trajGroupMetaText"));
        break;
    }
    return header;
}

QWidget *TrajectoryView::buildSummary(const QString &key, const QString &text)
{
    auto *summary = new TrajHitBox(m_ledgerInner);
    setClass(summary, QStringLiteral("trajSummary"));
    summary->setCursor(Qt::PointingHandCursor);

    auto *layout = new QHBoxLayout(summary);
    layout->setContentsMargins(22, 6, 10, 8);
    auto *label = mkLabel(QStringLiteral("trajSummary"), text, summary);
    label->setToolTip(QStringLiteral("展开本轮全部记录"));
    layout->addWidget(label, 1);

    summary->setHandler([this, key] { toggleGroup(key); });
    return summary;
}

// ---------- 详情面板 ----------

void TrajectoryView::rebuildInspector()
{
    const TrajRecord *rec = findRecord(m_selected);
    if (!rec) {
        m_inspector->setVisible(false);
        return;
    }
    m_inspector->setVisible(true);

    m_inspBadge->setText(sourceLabel(rec->source));
    m_inspBadge->setProperty("source", rec->source);
    restyle(m_inspBadge);

    QStringList pos;
    if (rec->turn)
        pos << QStringLiteral("第 %1 轮").arg(rec->turn);
    if (rec->request)
        pos << QStringLiteral("请求 #%1").arg(rec->request);
    if (rec->step)
        pos << QStringLiteral("步骤 %1").arg(rec->step);
    m_inspPos->setText(pos.isEmpty() ? QStringLiteral("—") : pos.join(QStringLiteral(" · ")));

    const QString tabIds[3] = {QStringLiteral("overview"), QStringLiteral("preview"), QStringLiteral("raw")};
    bool known = false;
    for (const QString &id : tabIds) {
        if (id == m_inspectorTab)
            known = true;
    }
    if (!known)
        m_inspectorTab = QStringLiteral("overview");
    for (int i = 0; i < 3; ++i) {
        m_tabs[i]->setProperty("active", tabIds[i] == m_inspectorTab);
        restyle(m_tabs[i]);
    }

    rebuildInspectorBody();
}

void TrajectoryView::rebuildInspectorBody()
{
    if (!m_inspBodyLayout)
        return;
    clearLayout(m_inspBodyLayout);
    const TrajRecord *rec = findRecord(m_selected);
    if (!rec)
        return;

    // 等宽代码块：保留 JSON 缩进（QSS 用 trajInspRaw 的容器外观）
    auto monoBlock = [this](const QString &text) {
        auto *area = new QPlainTextEdit(m_inspBodyInner);
        setClass(area, QStringLiteral("trajInspRaw"));
        area->setFrameShape(QFrame::NoFrame);
        area->setReadOnly(true);
        area->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        area->setWordWrapMode(QTextOption::WrapAnywhere);
        area->setPlainText(text);
        const int height = int(area->document()->size().height()) + 18;
        area->setFixedHeight(qBound(scaledPx(64), height, scaledPx(320)));
        return area;
    };

    if (m_inspectorTab == QLatin1String("preview")) {
        if (!rec->content.isEmpty()) {
            auto *view = new MarkdownView(m_inspBodyInner);
            view->setText(rec->content);
            m_inspBodyLayout->addWidget(view, 0);
        } else {
            m_inspBodyLayout->addWidget(
                mkLabel(QStringLiteral("trajEmpty"), QStringLiteral("（无内容）"), m_inspBodyInner), 0);
        }
        m_inspBodyLayout->addStretch(1);
        return;
    }

    if (m_inspectorTab == QLatin1String("raw")) {
        m_inspBodyLayout->addWidget(monoBlock(jsonAny(rec->raw, true)), 0);
        m_inspBodyLayout->addStretch(1);
        return;
    }

    // ---- 概述 ----
    auto addRow = [this](const QString &name, const QString &value, bool rich) {
        auto *row = new QWidget(m_inspBodyInner);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 3, 0, 3);
        layout->setSpacing(10);
        auto *nameLabel = mkLabel(QStringLiteral("trajInspRowLabel"), name, row);
        nameLabel->setFixedWidth(76);
        nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        layout->addWidget(nameLabel, 0);
        auto *valueLabel = mkSelectableLabel(QStringLiteral("trajInspRowValue"), value, row);
        valueLabel->setTextFormat(rich ? Qt::RichText : Qt::PlainText);
        valueLabel->setWordWrap(true);
        valueLabel->setMinimumWidth(1);
        valueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(valueLabel, 1);
        m_inspBodyLayout->addWidget(row, 0);
    };
    auto addSection = [this](const QString &text) {
        auto *label = mkLabel(QStringLiteral("trajInspSection"), text, m_inspBodyInner);
        label->setContentsMargins(0, 12, 0, 6);
        m_inspBodyLayout->addWidget(label, 0);
    };

    QStringList pos;
    if (rec->turn)
        pos << QStringLiteral("第 %1 轮").arg(rec->turn);
    if (rec->request)
        pos << QStringLiteral("请求 #%1").arg(rec->request);
    if (rec->step)
        pos << QStringLiteral("步骤 %1").arg(rec->step);

    const QString sourceText = (rec->source == QLatin1String("assistant"))
        ? (QStringLiteral("请求 #%1 ›").arg(rec->request))
        : (rec->source == QLatin1String("tool")
               ? (rec->name.isEmpty() ? QStringLiteral("工具") : rec->name)
               : sourceLabel(rec->source));
    QString sourceValue = escapeHtml(sourceText);
    if (!pos.isEmpty())
        sourceValue += QStringLiteral(" <small>") + escapeHtml(pos.join(QStringLiteral(" · ")))
                       + QStringLiteral("</small>");
    addRow(QStringLiteral("来源"), sourceValue, true);
    addRow(QStringLiteral("状态"), statusText(rec->status), false);
    if (rec->hasTokens) {
        addRow(QStringLiteral("Token"),
               QString::number(rec->tokens.total) + QStringLiteral(" tok")
                   + (rec->tokens.estimated ? QStringLiteral("（估算）") : QString()), false);
        addRow(QStringLiteral("推理"), QString::number(rec->tokens.reasoning) + QStringLiteral(" tok"), false);
        addRow(QStringLiteral("内容"), QString::number(rec->tokens.content) + QStringLiteral(" tok"), false);
    }
    if (rec->source == QLatin1String("tool"))
        addRow(QStringLiteral("耗时"), formatMs(rec->hasDurationMs, rec->durationMs), false);

    if (rec->source == QLatin1String("tool")) {
        addSection(QStringLiteral("参数"));
        m_inspBodyLayout->addWidget(monoBlock(argsText(rec->args)), 0);
    }

    if (rec->kind == QLatin1String("request") && rec->hasTiming) {
        addSection(QStringLiteral("请求计时"));
        const TrajTiming &timing = rec->timing;
        addRow(QStringLiteral("开始时间"),
               timing.start.isEmpty() ? QStringLiteral("—") : timing.start, false);
        addRow(QStringLiteral("总时长"), formatMs(timing.totalSet, timing.totalMs), false);
        addRow(QStringLiteral("首 token 延迟"), formatMs(timing.ttftSet, timing.ttftMs), false);
        addRow(QStringLiteral("生成"), formatMs(timing.genSet, timing.genMs), false);
        addRow(QStringLiteral("吞吐量"),
               timing.tpsSet ? QString::number(timing.tokPerS) + QStringLiteral(" tok/s")
                             : QStringLiteral("—"), false);
    }

    if (rec->kind == QLatin1String("request") && !rec->toolCalls.isEmpty()) {
        addSection(QStringLiteral("工具调用"));
        int index = 1;
        for (const QVariant &tc : rec->toolCalls)
            addRow(QString::number(index++), tc.toMap().value(QStringLiteral("name")).toString(), false);
    }

    m_inspBodyLayout->addStretch(1);
}

// ---------- 交互 ----------

const TrajRecord *TrajectoryView::findRecord(const QString &id) const
{
    if (id.isEmpty())
        return nullptr;
    for (const TrajRecord &rec : m_records) {
        if (rec.id == id)
            return &rec;
    }
    return nullptr;
}

void TrajectoryView::selectRecord(const QString &id)
{
    if (id.isEmpty() || id == m_selected)
        return;
    m_selected = id;
    // 行数多时全量重建要几百毫秒：这里只把描边从旧行挪到新行，再刷新详情面板
    applySelectionHighlight(id);
    // 从时间轴点过来时那一行可能还没实例化（在视口外）：按条目位置滚过去
    scrollToRecord(id);
    rebuildInspector();
}

void TrajectoryView::clearSelection()
{
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    applySelectionHighlight(QString());
    rebuildInspector();
}

void TrajectoryView::toggleGroup(const QString &key)
{
    m_collapsed.insert(key, !m_collapsed.value(key, false));
    postRefresh();
}

void TrajectoryView::resetCollapsed()
{
    m_collapsed.clear();
    if (m_view.isEmpty())
        return;
    for (const TrajRecord &rec : m_records)
        m_collapsed.insert(groupKeyOf(m_view, rec), true);
}

void TrajectoryView::syncViewButtons()
{
    if (!m_btnDuration)
        return;
    const bool turnOn = (m_view == QLatin1String("turn"));
    const bool callOn = (m_view == QLatin1String("call"));
    m_btnTurn->setProperty("active", turnOn);
    restyle(m_btnTurn);
    m_btnCall->setProperty("active", callOn);
    restyle(m_btnCall);
    m_btnDuration->setProperty("active", m_actualDuration);
    restyle(m_btnDuration);
    m_btnTurn->setToolTip(turnOn ? QStringLiteral("正在按轮次分组显示；点击切回平铺")
                                 : QStringLiteral("点击按轮次分组（每轮带用户消息）"));
    m_btnCall->setToolTip(callOn ? QStringLiteral("正在按模型调用分组显示；点击切回平铺")
                                 : QStringLiteral("点击按模型调用分组"));
    m_btnDuration->setToolTip(m_actualDuration
        ? QStringLiteral("正在按实际时长展示（条长=各步耗时，空转已折叠）；点击切到等宽操作")
        : QStringLiteral("正在按等宽操作展示；点击切到实际时长"));
}

void TrajectoryView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    applyResponsiveLayout();
    // 首次显示时视口尺寸才有效：按真实视口决定要实例化哪几行
    updateLedgerWindow(true);
    // 只在从未请求过加载时发一次，避免每次 show 都拉一遍
    if (m_loadRequested)
        return;
    m_loadRequested = true;
    emit reloadRequested();
}

void TrajectoryView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyResponsiveLayout();
    // 视口变高/变矮会改变「该实例化哪几行」；范围没变时内部会直接返回
    updateLedgerWindow();
}

// webui：@media(max-width:760px){flex-basis:300px}、@media(max-width:560px){绝对定位 88vw}
void TrajectoryView::applyResponsiveLayout()
{
    if (!m_body || !m_bodyLayout || !m_inspector)
        return;
    const int w = width();
    const bool overlay = w < 560;
    if (overlay != m_overlayInspector) {
        m_overlayInspector = overlay;
        const bool wasVisible = m_inspector->isVisible();
        if (overlay)
            m_bodyLayout->removeWidget(m_inspector); // 脱离布局，改成覆盖式浮层
        else
            m_bodyLayout->addWidget(m_inspector, 0); // 放回布局（账本之后）
        m_inspector->setParent(m_body);              // setParent 会隐藏控件，下面恢复
        m_inspector->setVisible(wasVisible);
    }
    if (overlay) {
        const int width = qMax(160, qRound(w * 0.88));
        m_inspector->setFixedWidth(width);
        m_inspector->setGeometry(m_body->width() - width, 0, width, m_body->height());
        m_inspector->raise();
        // 布局可能还没落定，再补一次
        QTimer::singleShot(0, this, [this, width] {
            if (!m_overlayInspector || !m_body || !m_inspector)
                return;
            m_inspector->setGeometry(m_body->width() - width, 0, width, m_body->height());
        });
    } else {
        m_inspector->setFixedWidth(w < 760 ? 300 : 380);
    }
}

// 点击本控件空白区域（工具栏/账本底、时间轴之外）收起详情
void TrajectoryView::mousePressEvent(QMouseEvent *event)
{
    clearSelection();
    QWidget::mousePressEvent(event);
}

bool TrajectoryView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress
        && (watched == m_toolbar || watched == m_ledgerInner
            || (m_ledger && watched == m_ledger->viewport()))) {
        clearSelection();
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace gs
#include "usagecharts.h"

#include "theme.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QtMath>

#include <limits>

namespace gs {

namespace {

const int kPadLeft = 62;
const int kPadRight = 16;
const int kPadTop = 14;
const int kPadBottom = 30;
const int kLegendHeight = 20;

qint64 clampInt(qint64 value, qint64 low, qint64 high)
{
    return value < low ? low : (value > high ? high : value);
}

double clampDouble(double value, double low, double high)
{
    return value < low ? low : (value > high ? high : value);
}

// charts.js niceStep：把任意刻度步长吸附到 1 / 2 / 5 × 10^n
double niceStep(double raw)
{
    if (!(raw > 0.0))
        return 1.0;
    const double magnitude = qPow(10.0, qFloor(std::log10(raw)));
    const double normalized = raw / magnitude;
    const double factor = normalized <= 1.0 ? 1.0 : (normalized <= 2.0 ? 2.0 : (normalized <= 5.0 ? 5.0 : 10.0));
    return factor * magnitude;
}

// charts.js xLabel：跨度大只看日，中等加小时，窄跨度只看小时
QString xLabelText(qint64 ms, qint64 span)
{
    const QDateTime date = QDateTime::fromMSecsSinceEpoch(ms);
    const QString monthDay = date.toString(QStringLiteral("MM-dd"));
    if (span > 3 * 86400000LL)
        return monthDay;
    if (span > 6 * 3600000LL)
        return monthDay + QLatin1Char(' ') + date.toString(QStringLiteral("HH")) + QStringLiteral(":00");
    return date.toString(QStringLiteral("HH")) + QStringLiteral(":00");
}

int scaledFontPx(int basePx)
{
    return scaledPx(basePx);
}

QFont chartFont(int basePx, bool bold = false, bool mono = false)
{
    QFont font;
    font.setFamily(mono ? monoFont() : uiFont());
    font.setPixelSize(scaledFontPx(basePx));
    font.setBold(bold);
    return font;
}

QList<QColor> pieColors()
{
    const Palette &p = gs::palette();
    return { p.cyan, p.green, p.orange, p.assistantText, p.cyanMid, p.red, p.muted2 };
}

} // namespace

// ------------------------------------------------------------ 数值格式

QString formatMillions(qint64 tokens)
{
    if (tokens == 0)
        return QStringLiteral("0M");
    const double millions = double(tokens) / 1000000.0;
    const double abs = qAbs(millions);
    // 1M 以上留两位小数；不足 1M 时按数量级补足小数位，否则固定两位会把
    // 几千的用量压成 0.00M，看起来像没有消耗
    const int digits = abs >= 1.0 ? 2 : qMin(8, 1 - int(qFloor(std::log10(abs))));
    QString text = QString::number(millions, 'f', digits < 0 ? 0 : digits);
    if (text.contains(QLatin1Char('.'))) {
        while (text.endsWith(QLatin1Char('0')))
            text.chop(1);
        if (text.endsWith(QLatin1Char('.')))
            text.chop(1);
    }
    return text + QLatin1Char('M');
}

QString usageAxisLabel(qint64 tokens)
{
    return tokens == 0 ? QStringLiteral("0") : formatMillions(tokens);
}

qint64 parseBucketMs(const QString &label)
{
    if (label.size() < 10)
        return 0;
    const int year = label.mid(0, 4).toInt();
    const int month = label.mid(5, 2).toInt();
    const int day = label.mid(8, 2).toInt();
    const int hour = label.size() > 10 ? label.mid(11, 2).toInt() : 0;
    const QDateTime date(QDate(year, month, day), QTime(hour, 0), Qt::LocalTime);
    return date.isValid() ? date.toMSecsSinceEpoch() : 0;
}

QString bucketTitle(const QString &label)
{
    const qint64 ms = parseBucketMs(label);
    if (ms == 0)
        return label;
    const QDateTime date = QDateTime::fromMSecsSinceEpoch(ms);
    return date.toString(QStringLiteral("yyyy-MM-dd HH")) + QStringLiteral(":00");
}

// ------------------------------------------------------------ UsageTimeChart

UsageTimeChart::UsageTimeChart(Mode mode, QWidget *parent)
    : QWidget(parent), m_mode(mode)
{
    if (mode == Line) {
        m_series = { { QStringLiteral("input"), QStringLiteral("输入"), gs::palette().cyan },
                     { QStringLiteral("output"), QStringLiteral("输出"), gs::palette().green } };
    } else {
        m_series = { { QStringLiteral("measured"), QStringLiteral("实测"), gs::palette().cyan },
                     { QStringLiteral("estimated"), QStringLiteral("估算"), gs::palette().orange } };
    }
    setFixedHeight(normalHeight());
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
}

void UsageTimeChart::setData(const QVariantList &buckets, qint64 domainStart, qint64 domainEnd,
                             bool hasView, qint64 viewStart, qint64 viewEnd)
{
    m_buckets = buckets;
    m_domainStart = domainStart;
    m_domainEnd = domainEnd > domainStart ? domainEnd : domainStart + 3600000;
    m_hasView = hasView;
    m_viewStart = hasView ? viewStart : m_domainStart;
    m_viewEnd = hasView ? viewEnd : m_domainEnd;
    m_hovering = false;
    m_hoverRow.clear();
    m_emptyText.clear();
    bool hasData = false;
    for (const QVariant &v : m_buckets) {
        if (v.toMap().value(QStringLiteral("total")).toLongLong() > 0) {
            hasData = true;
            break;
        }
    }
    if (m_buckets.isEmpty() || !hasData)
        setEmptyNote(QStringLiteral("所选范围内没有用量记录"));
    update();
}

void UsageTimeChart::clearData()
{
    m_buckets.clear();
    m_hovering = false;
    m_hoverRow.clear();
    setEmptyNote(QStringLiteral("所选范围内没有用量记录"));
    update();
}

void UsageTimeChart::setEmptyNote(const QString &text)
{
    m_emptyText = text;
    // .usage-chart{min-height:60} + .chart-empty{min-height:120}：空态不占满图高
    setFixedHeight(text.isEmpty() ? normalHeight() : 120);
}

int UsageTimeChart::normalHeight() const
{
    return (m_mode == Line ? 230 : 190) + kLegendHeight;
}

int UsageTimeChart::legendHeight() const
{
    return m_series.isEmpty() ? 0 : kLegendHeight;
}

QRect UsageTimeChart::plotRect() const
{
    const int top = legendHeight() + kPadTop;
    const int height = qMax(40, this->height() - legendHeight() - kPadTop - kPadBottom);
    const int width = qMax(40, this->width() - kPadLeft - kPadRight);
    return QRect(kPadLeft, top, width, height);
}

UsageTimeChart::Scale UsageTimeChart::scaleFor(qint64 peak) const
{
    Scale scale;
    const double raw = double(peak) * m_yZoom;
    const double step = niceStep(raw / 4.0);
    const int count = qMax(1, int(qCeil(raw / step)));
    scale.top = qint64(qRound(count * step));
    for (int i = 0; i <= count; ++i)
        scale.ticks << qint64(qRound(i * step));
    return scale;
}

qint64 UsageTimeChart::yFor(const Scale &scale, qint64 value, const QRect &plot) const
{
    if (scale.top <= 0)
        return plot.top() + plot.height();
    const double ratio = clampDouble(double(value) / double(scale.top), 0.0, 1.0);
    return qint64(qRound(plot.top() + plot.height() - ratio * plot.height()));
}

int UsageTimeChart::xFor(qint64 ms, qint64 viewStart, qint64 viewEnd, const QRect &plot) const
{
    const qint64 span = qMax<qint64>(1, viewEnd - viewStart);
    const double ratio = double(ms - viewStart) / double(span);
    return plot.left() + int(qRound(ratio * plot.width()));
}

QVariantMap UsageTimeChart::nearestBucket(qint64 target) const
{
    QVariantMap best;
    qint64 bestGap = std::numeric_limits<qint64>::max();
    for (const QVariant &v : m_buckets) {
        const QVariantMap row = v.toMap();
        const qint64 gap = qAbs(parseBucketMs(row.value(QStringLiteral("t")).toString()) - target);
        if (gap < bestGap) {
            bestGap = gap;
            best = row;
        }
    }
    return best;
}

qint64 UsageTimeChart::anchorRatioMs(QPoint pos, qint64 viewStart, qint64 viewEnd) const
{
    const QRect plot = plotRect();
    const double ratio = clampDouble(double(pos.x() - plot.left()) / qMax(1, plot.width()), 0.0, 1.0);
    return viewStart + qint64(double(viewEnd - viewStart) * ratio);
}

void UsageTimeChart::paintLegend(QPainter &painter, int &heightOut) const
{
    heightOut = legendHeight();
    m_legendHits.clear();
    if (heightOut == 0)
        return;
    QFont font = chartFont(10);
    painter.setFont(font);
    const QFontMetrics fm(font);
    const int swatch = scaledPx(9);
    int x = kPadLeft;
    for (const Series &item : m_series) {
        const bool off = m_hidden.contains(item.key);
        QColor color = item.color;
        QColor text = gs::palette().muted;
        if (off) {
            color.setAlphaF(0.45);
            text.setAlphaF(0.45);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(x, (heightOut - swatch) / 2.0, swatch, swatch), 2, 2);
        painter.setPen(text);
        QFont labelFont = font;
        labelFont.setStrikeOut(off);
        painter.setFont(labelFont);
        const int textW = fm.horizontalAdvance(item.label);
        painter.drawText(QRect(x + swatch + 5, 0, textW, heightOut), Qt::AlignLeft | Qt::AlignVCenter,
                         item.label);
        painter.setFont(font);
        m_legendHits.append({ QRect(x - 2, 1, swatch + 5 + textW + 6, heightOut - 2), item.key });
        x += swatch + 5 + textW + 10;
    }
}

void UsageTimeChart::paintLine(QPainter &painter, const QRect &plot, const Scale &scale,
                               qint64 viewStart, qint64 viewEnd) const
{
    // 视窗内的桶首尾各多带一个邻居，让线段延伸到视窗外由裁剪收边
    QList<int> inside;
    for (int i = 0; i < m_buckets.size(); ++i) {
        const qint64 ms = parseBucketMs(m_buckets.at(i).toMap().value(QStringLiteral("t")).toString());
        if (ms >= viewStart && ms <= viewEnd)
            inside.append(i);
    }
    if (inside.isEmpty())
        return;
    const int from = qMax(0, inside.first() - 1);
    const int to = qMin(m_buckets.size() - 1, inside.last() + 1);

    painter.save();
    painter.setClipRect(plot.adjusted(0, -4, 0, 8));
    for (const Series &item : m_series) {
        if (m_hidden.contains(item.key))
            continue;
        QPolygonF points;
        for (int i = from; i <= to; ++i) {
            const QVariantMap row = m_buckets.at(i).toMap();
            const qint64 ms = parseBucketMs(row.value(QStringLiteral("t")).toString());
            points << QPointF(xFor(ms, viewStart, viewEnd, plot),
                              yFor(scale, row.value(item.key).toLongLong(), plot));
        }
        QPen pen(item.color);
        pen.setWidthF(2.0);
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        if (points.size() > 1)
            painter.drawPolyline(points);
        painter.setPen(Qt::NoPen);
        QColor dot = item.color;
        dot.setAlphaF(0.9);
        painter.setBrush(dot);
        for (const QPointF &point : points)
            painter.drawEllipse(point, 2.5, 2.5);
    }
    painter.restore();
}

void UsageTimeChart::paintBar(QPainter &painter, const QRect &plot, const Scale &scale,
                              qint64 viewStart, qint64 viewEnd) const
{
    const qreal slot = double(plot.width()) / double(qMax(1, m_buckets.size()));
    const qreal barWidth = qMax(1.0, qMin(26.0, slot * 0.66));
    painter.save();
    painter.setClipRect(plot.adjusted(0, -4, 0, 8));
    for (const QVariant &v : m_buckets) {
        const QVariantMap row = v.toMap();
        const qint64 ms = parseBucketMs(row.value(QStringLiteral("t")).toString());
        if (ms < viewStart || ms > viewEnd)
            continue;
        const qreal left = qBound<qreal>(plot.left(),
                                         xFor(ms, viewStart, viewEnd, plot) - barWidth / 2.0,
                                         plot.left() + plot.width() - barWidth);
        qreal base = plot.top() + plot.height();
        const QString keys[2] = { QStringLiteral("measured"), QStringLiteral("estimated") };
        for (int k = 0; k < 2; ++k) {
            if (m_hidden.contains(keys[k]))
                continue;
            const qint64 value = row.value(keys[k]).toLongLong();
            if (value <= 0)
                continue;
            const qreal top = yFor(scale, value, plot);
            const qreal heightPx = qMax(1.0, base - top);
            painter.setPen(Qt::NoPen);
            if (k == 0) {
                painter.setBrush(gs::palette().cyan);
            } else {
                // 估算段用斜纹区分，不靠颜色单一通道传达实测与估算的差别
                QColor fill = gs::palette().orange;
                fill.setAlphaF(0.16);
                painter.setBrush(fill);
            }
            painter.drawRect(QRectF(left, base - heightPx, barWidth, heightPx));
            if (k == 1) {
                QPen pen(gs::palette().orange);
                pen.setWidthF(0.6);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(QRectF(left, base - heightPx, barWidth, heightPx));
            }
            base -= heightPx;
        }
    }
    painter.restore();
}

void UsageTimeChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 空状态：不画空坐标系（charts.js .chart-empty）
    if (!m_emptyText.isEmpty()) {
        painter.setPen(gs::palette().muted);
        painter.setFont(chartFont(11));
        painter.drawText(rect(), Qt::AlignCenter, m_emptyText);
        return;
    }

    int legendH = 0;
    paintLegend(painter, legendH);
    const QRect plot = plotRect();

    // 纵轴量程：被隐藏的序列退出量程计算
    qint64 peak = 0;
    for (const QVariant &v : m_buckets) {
        const QVariantMap row = v.toMap();
        if (m_mode == Line) {
            for (const Series &item : m_series) {
                if (m_hidden.contains(item.key))
                    continue;
                peak = qMax(peak, row.value(item.key).toLongLong());
            }
        } else {
            qint64 sum = 0;
            if (!m_hidden.contains(QStringLiteral("measured")))
                sum += row.value(QStringLiteral("measured")).toLongLong();
            if (!m_hidden.contains(QStringLiteral("estimated")))
                sum += row.value(QStringLiteral("estimated")).toLongLong();
            peak = qMax(peak, sum);
        }
    }
    const Scale scale = scaleFor(peak);

    // 网格 + 纵轴刻度
    painter.setPen(QPen(gs::palette().line, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(plot);
    painter.setFont(chartFont(9, false, true));
    for (qint64 value : scale.ticks) {
        const int y = int(yFor(scale, value, plot));
        QPen gridPen(gs::palette().line, 1);
        if (value != 0) {
            gridPen.setStyle(Qt::CustomDashLine);
            gridPen.setDashPattern(QVector<qreal>{ 3, 4 });
        }
        painter.setPen(gridPen);
        painter.drawLine(plot.left(), y, plot.left() + plot.width(), y);
        painter.setPen(gs::palette().muted2);
        painter.drawText(QRect(kPadLeft - 58, y - 8, 50, 16), Qt::AlignRight | Qt::AlignVCenter,
                         usageAxisLabel(value));
    }

    const qint64 span = qMax<qint64>(1, m_viewEnd - m_viewStart);
    const int tickCount = qMax(2, qMin(7, plot.width() / 110));
    QString previous;
    painter.setFont(chartFont(9));
    for (int index = 0; index <= tickCount; ++index) {
        const qint64 ms = m_viewStart + qint64(double(span) * index / tickCount);
        const QString text = xLabelText(ms, span);
        if (text == previous)
            continue;
        previous = text;
        const int x = qBound(plot.left() + 14, xFor(ms, m_viewStart, m_viewEnd, plot),
                             plot.left() + plot.width() - 14);
        painter.setPen(gs::palette().muted2);
        painter.drawText(QRect(x - 30, plot.top() + plot.height() + 6, 60, 16),
                         Qt::AlignHCenter | Qt::AlignTop, text);
    }

    if (m_mode == Line) {
        bool anyVisible = false;
        for (const Series &item : m_series) {
            if (!m_hidden.contains(item.key))
                anyVisible = true;
        }
        if (anyVisible)
            paintLine(painter, plot, scale, m_viewStart, m_viewEnd);
        else {
            painter.setPen(gs::palette().muted);
            painter.setFont(chartFont(11));
            painter.drawText(plot, Qt::AlignCenter, QStringLiteral("所有序列均已隐藏，点击图例恢复"));
        }
    } else {
        paintBar(painter, plot, scale, m_viewStart, m_viewEnd);
    }

    // 悬浮辅助线 + 浮层（charts.js guide / .chart-tip）
    if (m_hovering && !m_hoverRow.isEmpty()) {
        QPen guide(gs::palette().cyan, 1);
        guide.setStyle(Qt::CustomDashLine);
        guide.setDashPattern({ 3, 3 });
        painter.setPen(guide);
        painter.drawLine(m_hoverX, plot.top(), m_hoverX, plot.top() + plot.height());

        QStringList rows;
        if (m_mode == Line) {
            rows << QStringLiteral("总用量|%1").arg(formatMillions(m_hoverRow.value(QStringLiteral("total")).toLongLong()))
                 << QStringLiteral("输入|%1").arg(formatMillions(m_hoverRow.value(QStringLiteral("input")).toLongLong()))
                 << QStringLiteral("输出|%1").arg(formatMillions(m_hoverRow.value(QStringLiteral("output")).toLongLong()));
        } else {
            rows << QStringLiteral("实测|%1").arg(formatMillions(m_hoverRow.value(QStringLiteral("measured")).toLongLong()))
                 << QStringLiteral("估算|%1").arg(formatMillions(m_hoverRow.value(QStringLiteral("estimated")).toLongLong()))
                 << QStringLiteral("合计|%1").arg(formatMillions(m_hoverRow.value(QStringLiteral("total")).toLongLong()));
        }
        const int tipW = 184;
        const QFont titleFont = chartFont(10, true);
        const QFont rowFont = chartFont(10, false, true);
        const QFontMetrics rowFm(rowFont);
        const int tipH = 7 + QFontMetrics(titleFont).height() + 4 + rows.size() * rowFm.height() + 7;
        const int tipX = qBound(4, m_hoverX - 90, qMax(4, width() - tipW - 4));
        const int tipY = legendH + 30;
        const QRect box(tipX, tipY, tipW, tipH);
        painter.setPen(QPen(gs::palette().lineStrong, 1));
        painter.setBrush(gs::palette().panel);
        painter.drawRoundedRect(box, 4, 4);
        painter.setFont(titleFont);
        painter.setPen(gs::palette().textBright);
        painter.drawText(QRect(box.left() + 9, box.top() + 7, box.width() - 18, QFontMetrics(titleFont).height()),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         bucketTitle(m_hoverRow.value(QStringLiteral("t")).toString()));
        painter.setFont(rowFont);
        int y = box.top() + 7 + QFontMetrics(titleFont).height() + 4;
        for (const QString &row : rows) {
            const QStringList parts = row.split(QLatin1Char('|'));
            painter.setPen(gs::palette().muted);
            painter.drawText(QRect(box.left() + 9, y, box.width() - 18, rowFm.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, parts.value(0));
            painter.setPen(gs::palette().text);
            painter.drawText(QRect(box.left() + 9, y, box.width() - 18, rowFm.height()),
                             Qt::AlignRight | Qt::AlignVCenter, parts.value(1));
            y += rowFm.height();
        }
    }
}

void UsageTimeChart::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const QRect plot = plotRect();
        const qint64 span = m_dragOriginEnd - m_dragOriginStart;
        const double shift = double(event->pos().x() - m_dragOriginX) / qMax(1, plot.width()) * span;
        // 平移钳制的是偏移量而不是两端：独立钳制两端会在贴边时压扁视窗
        const qint64 start = clampInt(m_dragOriginStart - qint64(shift), m_domainStart,
                                      qMax(m_domainStart, m_domainEnd - span));
        m_hasView = true;
        m_viewStart = start;
        m_viewEnd = start + span;
        emit viewRequested(true, m_viewStart, m_viewEnd);
        update();
        return;
    }
    const QRect plot = plotRect();
    if (!m_emptyText.isEmpty() || !plot.contains(event->pos())) {
        if (m_hovering) {
            m_hovering = false;
            m_hoverRow.clear();
            emit hovered(QVariantMap());
            update();
        }
        return;
    }
    const QVariantMap row = nearestBucket(anchorRatioMs(event->pos(), m_viewStart, m_viewEnd));
    if (row.isEmpty())
        return;
    m_hovering = true;
    m_hoverRow = row;
    m_hoverX = xFor(parseBucketMs(row.value(QStringLiteral("t")).toString()), m_viewStart, m_viewEnd, plot);
    emit hovered(row);
    update();
}

void UsageTimeChart::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    // 图例可点击隐藏序列，被隐藏的序列同时退出纵轴量程计算（charts.js legendRow）
    for (const LegendHit &hit : m_legendHits) {
        if (hit.rect.contains(event->pos())) {
            if (m_hidden.contains(hit.key))
                m_hidden.remove(hit.key);
            else
                m_hidden.insert(hit.key);
            update();
            event->accept();
            return;
        }
    }
    if (m_emptyText.isEmpty() && plotRect().contains(event->pos())) {
        m_dragging = true;
        m_dragOriginX = event->pos().x();
        m_dragOriginStart = m_viewStart;
        m_dragOriginEnd = m_viewEnd;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }
}

void UsageTimeChart::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging) {
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        event->accept();
    }
}

void UsageTimeChart::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_emptyText.isEmpty() && plotRect().contains(event->pos())) {
        m_hasView = false;
        m_viewStart = m_domainStart;
        m_viewEnd = m_domainEnd;
        emit viewRequested(false, 0, 0);
        update();
    }
}

void UsageTimeChart::wheelEvent(QWheelEvent *event)
{
    if (!m_emptyText.isEmpty() || !plotRect().contains(event->pos())) {
        event->ignore();
        return;
    }
    // 浏览器滚轮向上 deltaY<0；Qt 的 angleDelta().y() 向上为正，取反对齐
    const qreal magnitude = -event->angleDelta().y();
    if (event->modifiers() & Qt::ShiftModifier) {
        m_yZoom = qBound(0.05, m_yZoom * qExp(magnitude * 0.002), 20.0);
        update();
        event->accept();
        return;
    }
    const qint64 anchor = anchorRatioMs(event->pos(), m_viewStart, m_viewEnd);
    const qreal factor = qExp(magnitude * 0.002);
    qint64 start = qint64(anchor - (anchor - m_viewStart) * factor);
    qint64 end = qint64(anchor + (m_viewEnd - anchor) * factor);
    // 视窗只能落在给定区间内部，且不允许缩到比最小跨度还窄
    const qint64 minSpan = qMax<qint64>(qint64((m_domainEnd - m_domainStart) * 0.01), 1000);
    start = clampInt(start, m_domainStart, m_domainEnd);
    end = clampInt(end, m_domainStart, m_domainEnd);
    if (end - start < minSpan) {
        const qint64 middle = (start + end) / 2;
        start = clampInt(middle - minSpan / 2, m_domainStart, qMax(m_domainStart, m_domainEnd - minSpan));
        end = start + minSpan;
    }
    m_hasView = true;
    m_viewStart = start;
    m_viewEnd = end;
    emit viewRequested(true, m_viewStart, m_viewEnd);
    update();
    event->accept();
}

void UsageTimeChart::leaveEvent(QEvent *event)
{
    if (m_hovering) {
        m_hovering = false;
        m_hoverRow.clear();
        emit hovered(QVariantMap());
        update();
    }
    QWidget::leaveEvent(event);
}

// ------------------------------------------------------------ UsagePieChart

UsagePieChart::UsagePieChart(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(190);
    setMouseTracking(true);
}

void UsagePieChart::setGroups(const QVariantList &groups, const QString &unit)
{
    m_groups.clear();
    m_total = 0;
    for (const QVariant &v : groups) {
        const QVariantMap item = v.toMap();
        const qint64 value = item.value(QStringLiteral("value")).toLongLong();
        if (value <= 0)
            continue;
        m_groups.append(item);
        m_total += value;
    }
    m_unit = unit.isEmpty() ? QStringLiteral("总量") : unit;
    m_focus = -1;
    setEmptyNote(m_total > 0 ? QString() : QStringLiteral("所选范围内没有用量记录"));
    update();
}

void UsagePieChart::clearData()
{
    m_groups.clear();
    m_total = 0;
    m_focus = -1;
    setEmptyNote(QStringLiteral("所选范围内没有用量记录"));
    update();
}

void UsagePieChart::setEmptyNote(const QString &text)
{
    m_emptyText = text;
    // 环形空态同样是 .chart-empty 的 120px，而不是满高 190
    setFixedHeight(text.isEmpty() ? 190 : 120);
}

void UsagePieChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!m_emptyText.isEmpty()) {
        painter.setPen(gs::palette().muted);
        painter.setFont(chartFont(11));
        painter.drawText(rect(), Qt::AlignCenter, m_emptyText);
        return;
    }

    // 图例固定占左侧 46%（.pie-legend），环形画布占其余
    const int legendW = qMax(120, int(width() * 0.46));
    const QRect legendRect(0, 0, legendW, height());
    const QRect canvas(legendW + 12, 0, qMax(40, width() - legendW - 12), height());
    const QPointF center(canvas.left() + canvas.width() / 2.0, canvas.height() / 2.0);
    const qreal outer = qMax(26.0, qMin(canvas.height() / 2.0, canvas.width() * 0.44));
    const qreal inner = outer * 0.6;
    const QList<QColor> colors = pieColors();

    // 扇区
    qreal angle = -M_PI / 2.0;
    for (int i = 0; i < m_groups.size(); ++i) {
        const qint64 value = m_groups.at(i).toMap().value(QStringLiteral("value")).toLongLong();
        const qreal sweep = double(value) / double(m_total) * M_PI * 2.0;
        const qreal start = angle;
        const qreal end = angle + qMax(sweep - 0.004, 0.0001);
        QColor color = colors.at(i % colors.size());
        if (m_focus >= 0 && m_focus != i)
            color.setAlphaF(0.35);
        painter.setBrush(color);
        painter.setPen(QPen(gs::palette().panel, 1));
        const QRectF box(center.x() - outer, center.y() - outer, outer * 2, outer * 2);
        const int startDeg = int(qRound(-start * 180.0 / M_PI * 16));
        const int spanDeg = int(qRound(-(end - start) * 180.0 / M_PI * 16));
        painter.drawPie(box, startDeg, spanDeg);
        angle += sweep;
    }
    // 掏空成环形
    painter.setPen(Qt::NoPen);
    painter.setBrush(gs::palette().band2);
    painter.drawEllipse(center, inner, inner);

    // 中心：当前范围总量 / 命中扇区的数值与占比
    const QVariantMap focusItem = (m_focus >= 0 && m_focus < m_groups.size())
                                      ? m_groups.at(m_focus).toMap()
                                      : QVariantMap();
    const qint64 centerValue = focusItem.isEmpty()
                                   ? m_total
                                   : focusItem.value(QStringLiteral("value")).toLongLong();
    painter.setPen(gs::palette().textBright);
    painter.setFont(chartFont(15, true));
    painter.drawText(QRectF(center.x() - outer, center.y() - outer, outer * 2, outer),
                     Qt::AlignHCenter | Qt::AlignBottom, formatMillions(centerValue));
    painter.setPen(gs::palette().muted2);
    painter.setFont(chartFont(9));
    const QString centerLabel = focusItem.isEmpty()
                                    ? m_unit
                                    : QStringLiteral("%1 · %2%")
                                          .arg(focusItem.value(QStringLiteral("label")).toString())
                                          .arg(qRound(double(centerValue) / double(m_total) * 100));
    painter.drawText(QRectF(center.x() - outer, center.y(), outer * 2, outer),
                     Qt::AlignHCenter | Qt::AlignTop, centerLabel);

    // 左侧图例
    painter.setFont(chartFont(10));
    const QFontMetrics fm(chartFont(10));
    const QFont mono = chartFont(9, false, true);
    const int swatch = scaledPx(9);
    int y = 4;
    for (int i = 0; i < m_groups.size(); ++i) {
        const QVariantMap item = m_groups.at(i).toMap();
        const int rowH = fm.height() + 4;
        if (y + rowH > legendRect.bottom())
            break;
        const bool active = m_focus == i;
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.at(i % colors.size()));
        painter.drawRoundedRect(QRectF(2, y + (rowH - swatch) / 2.0, swatch, swatch), 2, 2);
        const QString label = item.value(QStringLiteral("label")).toString();
        const QString pct = QStringLiteral("%1%").arg(
            QString::number(double(item.value(QStringLiteral("value")).toLongLong()) / double(m_total) * 100, 'f', 1));
        const int valueW = 54;
        const int pctW = 40;
        const int nameX = 2 + swatch + 7;
        const int nameW = qMax(10, legendRect.right() - nameX - pctW - valueW - 6);
        painter.setPen(active ? gs::palette().textBright : gs::palette().muted);
        painter.setFont(chartFont(10));
        painter.drawText(QRect(nameX, y, nameW, rowH), Qt::AlignLeft | Qt::AlignVCenter,
                         fm.elidedText(label, Qt::ElideRight, nameW));
        painter.setFont(mono);
        painter.setPen(gs::palette().muted2);
        painter.drawText(QRect(nameX + nameW, y, pctW, rowH), Qt::AlignRight | Qt::AlignVCenter, pct);
        painter.setPen(gs::palette().text);
        painter.drawText(QRect(nameX + nameW + pctW, y, valueW, rowH),
                         Qt::AlignRight | Qt::AlignVCenter,
                         formatMillions(item.value(QStringLiteral("value")).toLongLong()));
        y += rowH;
    }
}

void UsagePieChart::mouseMoveEvent(QMouseEvent *event)
{
    if (m_total <= 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const int legendW = qMax(120, int(width() * 0.46));
    const QRect canvas(legendW + 12, 0, qMax(40, width() - legendW - 12), height());
    const QPointF center(canvas.left() + canvas.width() / 2.0, canvas.height() / 2.0);
    const qreal outer = qMax(26.0, qMin(canvas.height() / 2.0, canvas.width() * 0.44));
    const qreal inner = outer * 0.6;
    const QPointF delta = QPointF(event->pos()) - center;
    const qreal distance = qSqrt(delta.x() * delta.x() + delta.y() * delta.y());

    int focus = -1;
    if (distance >= inner && distance <= outer) {
        qreal angle = qAtan2(delta.y(), delta.x());
        if (angle < -M_PI / 2.0)
            angle += M_PI * 2.0;
        qreal cursor = -M_PI / 2.0;
        for (int i = 0; i < m_groups.size(); ++i) {
            const qint64 value = m_groups.at(i).toMap().value(QStringLiteral("value")).toLongLong();
            const qreal sweep = double(value) / double(m_total) * M_PI * 2.0;
            if (angle >= cursor && angle < cursor + sweep) {
                focus = i;
                break;
            }
            cursor += sweep;
        }
    }
    if (focus != m_focus) {
        m_focus = focus;
        emit hovered(focus >= 0 ? m_groups.at(focus).toMap() : QVariantMap());
        update();
    }
}

void UsagePieChart::leaveEvent(QEvent *event)
{
    if (m_focus != -1) {
        m_focus = -1;
        emit hovered(QVariantMap());
        update();
    }
    QWidget::leaveEvent(event);
}

} // namespace gs
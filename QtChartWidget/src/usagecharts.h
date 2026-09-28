#ifndef GS_USAGECHARTS_H
#define GS_USAGECHARTS_H

#include <QColor>
#include <QHash>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

namespace gs {

// app.js Charts.formatMillions：用量一律以百万（M）为单位，不足 1M 也写小数
// （0.5M / 0.075M），不退回 K；小数位按数量级补足，0 显示为 0M。
QString formatMillions(qint64 tokens);
// 坐标轴刻度同单位，但 0 只写 "0"，不写 "0M"
QString usageAxisLabel(qint64 tokens);

// 桶标签（"YYYY-MM-DDTHH" 或 "YYYY-MM-DD"）解析成本地时间的毫秒
qint64 parseBucketMs(const QString &label);
// 桶标签 → "YYYY-MM-DD HH:00"（浮层标题）
QString bucketTitle(const QString &label);

// 自绘时间轴图表：折线（输入 / 输出）或堆叠柱（实测 / 估算）。
// 折线与柱状共享同一时间视窗（由调用方持有并下发），滚轮以鼠标位置为锚点缩放、
// 拖拽平移、双击复位；Shift + 滚轮缩放纵轴。对应 webui/charts.js 的 line / bar。
class UsageTimeChart : public QWidget
{
    Q_OBJECT
public:
    enum Mode { Line, Bar };
    explicit UsageTimeChart(Mode mode, QWidget *parent = nullptr);

    // buckets: [{t, input, output, total, measured, estimated, turns}]
    // hasView 为假时按完整区间渲染；视窗由 UsagePanel 持有
    void setData(const QVariantList &buckets, qint64 domainStart, qint64 domainEnd,
                 bool hasView, qint64 viewStart, qint64 viewEnd);
    void clearData();

signals:
    // 缩放 / 平移后的新视窗；hasView 为假表示双击复位到完整区间
    void viewRequested(bool hasView, qint64 start, qint64 end);
    // 悬浮命中的桶（空 map 表示离开），UsagePanel 用它更新 aria-live 文本
    void hovered(const QVariantMap &row);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Series { QString key; QString label; QColor color; };
    struct Scale { qint64 top = 0; QList<qint64> ticks; };
    struct LegendHit { QRect rect; QString key; };

    void setEmptyNote(const QString &text);
    int normalHeight() const;
    int legendHeight() const;
    QRect plotRect() const;
    Scale scaleFor(qint64 peak) const;
    qint64 yFor(const Scale &scale, qint64 value, const QRect &plot) const;
    int xFor(qint64 ms, qint64 viewStart, qint64 viewEnd, const QRect &plot) const;
    QVariantMap nearestBucket(qint64 target) const;
    void paintLegend(QPainter &painter, int &heightOut) const;
    void paintLine(QPainter &painter, const QRect &plot, const Scale &scale, qint64 viewStart,
                   qint64 viewEnd) const;
    void paintBar(QPainter &painter, const QRect &plot, const Scale &scale, qint64 viewStart,
                  qint64 viewEnd) const;
    qint64 anchorRatioMs(QPoint pos, qint64 viewStart, qint64 viewEnd) const;

    Mode m_mode = Line;
    QVariantList m_buckets;
    qint64 m_domainStart = 0;
    qint64 m_domainEnd = 0;
    bool m_hasView = false;
    qint64 m_viewStart = 0;
    qint64 m_viewEnd = 0;
    QString m_emptyText;
    QList<Series> m_series;
    QSet<QString> m_hidden;
    qreal m_yZoom = 1.0;
    // 图例命中区随绘制填充（paintLegend），鼠标点击据此切换序列显隐
    mutable QList<LegendHit> m_legendHits;

    bool m_hovering = false;
    QVariantMap m_hoverRow;
    int m_hoverX = 0;
    bool m_dragging = false;
    int m_dragOriginX = 0;
    qint64 m_dragOriginStart = 0;
    qint64 m_dragOriginEnd = 0;
};

// 自绘环形图（对应 charts.js 的 pie）：扇区 + 中心总量 + 右侧图例，悬浮高亮。
class UsagePieChart : public QWidget
{
    Q_OBJECT
public:
    explicit UsagePieChart(QWidget *parent = nullptr);

    // groups: [{label, value}]，unit 为中心标签（默认「总量」）
    void setGroups(const QVariantList &groups, const QString &unit = QString());
    void clearData();

signals:
    void hovered(const QVariantMap &group);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void setEmptyNote(const QString &text);

    QVariantList m_groups;
    QString m_unit;
    QString m_emptyText;
    qint64 m_total = 0;
    int m_focus = -1;
};

} // namespace gs

#endif // GS_USAGECHARTS_H
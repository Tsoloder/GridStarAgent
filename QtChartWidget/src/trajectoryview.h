#ifndef GS_TRAJECTORYVIEW_H
#define GS_TRAJECTORYVIEW_H

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QWidget>

#include <functional>

QT_BEGIN_NAMESPACE
class QEvent;
class QFrame;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QScrollArea;
class QShowEvent;
class QTimer;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

// token 计数（对应 app.js trajEstimateTokens：中日韩 1 token，其余 4 字符 1 token）
struct TrajTokens
{
    int total = 0;
    int reasoning = 0;
    int content = 0;
    bool estimated = false;
};

// 请求计时（*Set 为 false 表示 JSON 里是 null，对应 JS 的 != null 判定）
struct TrajTiming
{
    QString start;
    bool totalSet = false;  double totalMs = 0;
    bool ttftSet = false;   double ttftMs = 0;
    bool genSet = false;    double genMs = 0;
    bool tpsSet = false;    double tokPerS = 0;
};

// 一条业务记录（app.js trajProject 的输出）
struct TrajRecord
{
    QString id;
    QString source;   // system / user / context / assistant / tool
    QString kind;     // user / system_prompt / ledger_snapshot / context / request / tool_call
    int turn = 0;
    int request = 0;
    int step = 0;
    QString status;   // running / completed / failed / interrupted
    QString label;
    QString name;
    bool hasArgs = false;
    QVariantMap args;
    QString callId;
    QString model;
    QString content;
    QString reasoning;
    QVariantList toolCalls;   // [{id,name,args}]
    bool hasTokens = false;
    TrajTokens tokens;
    bool hasTiming = false;
    TrajTiming timing;
    bool hasDurationMs = false;
    double durationMs = 0;
    QVariant raw;
    QString ts;
    bool turnStart = false;   // 轮次边界（app.js markTurnStarts）
};

// 时间轴上的一个跨度（app.js renderTrajTimeline 的 spans）
struct TrajSpan
{
    int lane = 0;        // 0 输入 / 1 模型 / 2 工具
    double start = 0;    // 真实时刻（毫秒）
    double end = 0;
    double ds = 0;       // 投影坐标（等宽=记录序号；时长=折叠空转后的毫秒）
    double de = 0;
    double idle = 0;
    QString cls;         // user / system / context / model / ttft / tool（可带 failed）
    QString id;
    QString title;
};

// 时间轴轨道画布：自绘全部 spans 与选区，处理点击定位与拖拽选区
class TrajTimelineCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit TrajTimelineCanvas(QWidget *parent = nullptr);

    void setSpans(const QList<TrajSpan> &spans, bool valid, double dMin, double dMax);
    void setSelectedId(const QString &id);
    void clearRange();
    bool hasRange() const { return m_hasRange; }

signals:
    void spanClicked(const QString &id);
    void rangeChanged(const QStringList &ids, double d0, double d1);
    void cleared();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    double displayAt(int x) const;
    const TrajSpan *spanAt(double d) const;

    QList<TrajSpan> m_spans;
    bool m_valid = false;
    double m_dMin = 0;
    double m_dMax = 1;
    QString m_selected;
    bool m_hasRange = false;
    double m_rangeD0 = 0;
    double m_rangeD1 = 0;
    bool m_dragging = false;
    bool m_dragMoved = false;
    double m_dragStartD = 0;
    int m_dragStartX = 0;
    QString m_hoverId;
};

// 时间轴概览：三条泳道（输入/模型/工具），标签 + 轨道 + 覆盖画布
class TrajTimeline : public QWidget
{
    Q_OBJECT
public:
    explicit TrajTimeline(QWidget *parent = nullptr);

    void setSpans(const QList<TrajSpan> &spans, bool valid, double dMin, double dMax);
    void setSelectedId(const QString &id);
    void clearRange();
    TrajTimelineCanvas *canvas() const { return m_canvas; }

protected:
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    // 画布只覆盖三根轨道的并集：几何要在布局落定后同步（否则首帧会用旧的轨道宽度）
    void syncCanvasGeometry();

    QLabel *m_labels[3] = {nullptr, nullptr, nullptr};
    QFrame *m_tracks[3] = {nullptr, nullptr, nullptr};
    TrajTimelineCanvas *m_canvas = nullptr;
};

// 可点击容器（账本行 / 分组头 / 摘要行共用；沿用动态属性 class，不新增 QSS）
class TrajHitBox : public QWidget
{
public:
    using Handler = std::function<void()>;

    explicit TrajHitBox(QWidget *parent = nullptr);
    void setHandler(Handler handler) { m_handler = std::move(handler); }

protected:
    void mousePressEvent(QMouseEvent *event) override;
    // webui 的 .traj-row / .traj-group / 摘要行都带 tabindex="0"：Enter / Space 等价点击
    void keyPressEvent(QKeyEvent *event) override;

private:
    Handler m_handler;
};

// 轨迹视图（webui #trajectory-view）：工具栏 + 时间轴概览 + 账本 + 详情面板
class TrajectoryView : public QWidget
{
    Q_OBJECT
public:
    explicit TrajectoryView(QWidget *parent = nullptr);

    // 全量重置：events 为后端 /sessions/{id}/trajectory 的 events 数组
    void setEvents(const QVariantList &events);
    // 实时追加：与 setEvents 的数据同构，内部按「基础键 + 出现序号」去重
    void appendEvents(const QVariantList &events);
    void clearEvents();

signals:
    // 宿主收到后去拉 /sessions/{id}/trajectory 并回填 setEvents
    void reloadRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void buildInspector();
    // webui 的 @media(max-width:760px)/(max-width:560px)：窄屏收窄详情面板，
    // 更窄时把详情面板改成覆盖式浮层
    void applyResponsiveLayout();

    void ingest(const QVariantList &events);
    void projectRecords();
    void refreshAll();
    void postRefresh();

    void rebuildTimeline();
    void rebuildLedger();
    void rebuildInspector();
    void rebuildInspectorBody();

    // 账本分组头的统计：条目计算与增量追加都要用
    struct LedgerGroupStats
    {
        int count = 0;
        int tokens = 0;
        int steps = 0;
        int tools = 0;
        double ms = 0;
    };
    // 账本里的一项（分组头 / 数据行 / 收起摘要 / 空占位）：只描述「该渲染什么」，
    // 不持有控件——控件只为可视区实例化（见 updateLedgerWindow）
    enum class LedgerKind { Empty, Preamble, Group, Summary, Row };
    struct LedgerEntry
    {
        LedgerKind kind = LedgerKind::Row;
        int recordIndex = -1;        // Row：m_records 下标
        bool markTurnStart = false;  // Row：左侧轮次边界竖线
        QString groupKey;            // Group / Summary：展开收起用
        QString label;               // Group：「第 N 轮」等
        LedgerGroupStats stats;      // Group / Summary：条数 / token / 耗时 / 步骤 / 工具
    };
    // 已实例化分组头的两枚统计标签（增量追加时就地刷新；没实例化就不用刷）
    struct LedgerGroupHeader
    {
        QLabel *count = nullptr;
        QLabel *meta = nullptr;
    };
    QHash<QString, LedgerGroupStats> computeGroupStats(const QList<TrajRecord> &list) const;
    QString groupMetaText(const LedgerGroupStats &stats) const;
    QString groupSummaryText(const LedgerGroupStats &stats) const;
    QString groupLabelText(const TrajRecord &rec) const;
    // 条目计算：buildLedgerEntries 全量（filters 之后），appendLedgerEntries 只补新增记录
    void buildLedgerEntries(const QList<TrajRecord> &records);
    void appendLedgerEntries(const QList<TrajRecord> &records, int fromRecord);
    void refreshLedgerPrefix();          // 重算前缀高度（条目高度变了才需要）
    void appendLedgerTail();
    // 只有「账本非空 + 无查询/框选过滤 + 末尾分组未收起 + 本批不含系统提示词 + 不落在旧分组里」
    // 才能增量追加；否则退回全量重建
    bool canAppendLedger(const QVariantList &events) const;

    // ---- 视窗化：只给可视区（含少量上下余量）实例化行控件 ----
    int ledgerEntryHeight(const LedgerEntry &entry) const;
    QWidget *buildLedgerEntryWidget(const LedgerEntry &entry, double maxMs);
    void clearLedgerWindow();
    void clearLedgerWindowRows();        // 只删行：换行时先调 spacer 再删，内容高度不抖
    void updateLedgerSpacers();          // 上下占位 spacer = 可视区之外的内容高度
    void updateLedgerWindow(bool force = false);
    void invalidateLedgerHeights();      // 缩放/字体变了：行高实测作废
    int ledgerVisibleHeight() const;
    int ledgerEntryIndexAt(int y) const; // 内容坐标 → 条目下标（前缀高度二分）
    bool scrollToRecord(const QString &id);

    // 只移动高亮（选中行 + 时间轴），不动账本结构
    void applySelectionHighlight(const QString &id);

    QWidget *buildRow(const TrajRecord &rec, double maxMs, bool markTurnStart);
    QWidget *buildGroup(const LedgerEntry &entry, bool collapsed, QLabel **countOut = nullptr,
                        QLabel **metaOut = nullptr);
    QWidget *buildSummary(const QString &key, const QString &text);
    LedgerGroupHeader widgetsOfGroup(const QString &key) const;

    const TrajRecord *findRecord(const QString &id) const;
    void selectRecord(const QString &id);
    void clearSelection();
    void toggleGroup(const QString &key);
    void syncViewButtons();
    void resetCollapsed();

    // 数据（与 app.js state.traj 对齐）
    QVariantList m_events;
    QHash<QString, int> m_count;      // 基础键 → 已出现次数
    QSet<QString> m_keys;             // 去重后的完整键
    QList<TrajRecord> m_records;
    QString m_query;
    QString m_view;                   // "" 平铺 / "turn" 轮次 / "call" 调用
    bool m_actualDuration = false;    // 「时长」开关
    QHash<QString, bool> m_collapsed;
    QString m_selected;
    QString m_inspectorTab = QStringLiteral("overview");
    bool m_hasRange = false;
    double m_rangeD0 = 0;
    double m_rangeD1 = 0;
    QStringList m_rangeIds;
    bool m_rebuildPending = false;
    bool m_loadRequested = false;
    bool m_loaded = false;

    // 账本视窗状态：条目列表是「全量」的（只有数据、没有控件），控件只为可视区实例化
    QVector<LedgerEntry> m_ledgerEntries;
    QVector<int> m_ledgerPrefix;               // 前缀高度和，size = entries + 1，便于二分定位
    QHash<int, int> m_ledgerKindHeights;       // LedgerKind → 实测行高（int 存枚举）
    qreal m_ledgerHeightZoom = -1.0;           // 行高实测时的缩放系数（缩放变了要重测）
    QWidget *m_ledgerSpacerTop = nullptr;
    QWidget *m_ledgerSpacerBottom = nullptr;
    QVector<QWidget *> m_ledgerWindowRows;     // 当前实例化的可见行（按顺序）
    int m_ledgerWindowFrom = 0;                // 可视窗口在 m_ledgerEntries 里的范围 [from, to)
    int m_ledgerWindowTo = 0;
    bool m_ledgerMeasuring = false;            // 正在为实测行高做一次性重建
    bool m_ledgerUpdating = false;             // 换行重入锁（scrollbar 变化是换行的副作用）
    int m_ledgerRecordsShown = 0;              // 已换算成条目的记录数（m_records 下标空间）
    bool m_ledgerEmpty = false;                // 当前只有「暂无轨迹记录」占位
    QString m_lastGroupKey;
    bool m_hasLastGroup = false;
    QHash<QString, QPointer<QWidget>> m_rowWidgets;   // 记录 id → 行控件（仅可视区内的）
    QHash<QString, QString> m_groupEntryKey;          // 分组键 → 已算好的分组标题（增量追加复用）
    QTimer *m_searchDebounce = nullptr;

    // 控件
    QWidget *m_toolbar = nullptr;
    QPushButton *m_btnDuration = nullptr;
    QPushButton *m_btnTurn = nullptr;
    QPushButton *m_btnCall = nullptr;
    QLineEdit *m_search = nullptr;
    TrajTimeline *m_timeline = nullptr;
    QWidget *m_body = nullptr;
    QHBoxLayout *m_bodyLayout = nullptr;
    QScrollArea *m_ledger = nullptr;
    QWidget *m_ledgerInner = nullptr;
    QVBoxLayout *m_ledgerLayout = nullptr;
    QPointer<QWidget> m_selectedRow;
    QWidget *m_inspector = nullptr;
    // 窄屏时详情面板脱离布局、绝对定位成浮层
    bool m_overlayInspector = false;
    QLabel *m_inspBadge = nullptr;
    QLabel *m_inspPos = nullptr;
    QPushButton *m_tabs[3] = {nullptr, nullptr, nullptr};
    QScrollArea *m_inspBody = nullptr;
    QWidget *m_inspBodyInner = nullptr;
    QVBoxLayout *m_inspBodyLayout = nullptr;
};

} // namespace gs

#endif // GS_TRAJECTORYVIEW_H
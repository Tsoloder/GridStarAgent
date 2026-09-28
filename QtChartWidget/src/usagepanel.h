#ifndef GS_USAGEPANEL_H
#define GS_USAGEPANEL_H

#include <QDateTime>
#include <QFrame>
#include <QHash>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QBoxLayout;
class QGridLayout;
class QHideEvent;
class QLabel;
class QPushButton;
class QResizeEvent;
class QScrollArea;
class QStackedWidget;
class QTableWidget;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

class FlowLayout;
class UsageTimeChart;
class UsagePieChart;

// 用量面板的胶囊下拉触发器（.usage-select）：文本 + ⌄，展开时描边提亮
class UsageSelect : public QWidget
{
    Q_OBJECT
public:
    // withCalendar 为真时左侧带一枚日历图标（日期筛选）
    UsageSelect(bool withCalendar = false, QWidget *parent = nullptr);
    void setText(const QString &text);
    QString text() const { return m_full; }
    void setOpen(bool open);
    void refreshIcons();

signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QLabel *m_label = nullptr;
    QLabel *m_calendar = nullptr;
    QLabel *m_chevron = nullptr;
    QString m_full;
    bool m_withCalendar = false;
};

// 用量下拉的选项浮层（.usage-listbox）
class UsageListPopup : public QFrame
{
    Q_OBJECT
public:
    explicit UsageListPopup(QWidget *parent = nullptr);
    // items: [{value, label, note, disabled}]
    void setItems(const QVariantList &items, const QString &current);
    void openBelow(QWidget *anchor);

signals:
    void chosen(const QString &value);
    void closed();

protected:
    void hideEvent(QHideEvent *event) override;

private:
    // 选项内容自然宽（label 全文 + note）：QPushButton::sizeHint 走样式、看不到内部布局，
    // 直接问 sizeHint 会把长模型名量窄，浮层被压在 min-width 140 上
    int preferredContentWidth() const;

    QVBoxLayout *m_layout = nullptr;
    QWidget *m_inner = nullptr;
};

// 日期区间浮层（.usage-range-panel）：预设 + 双月日历 + 确定
class UsageRangePopup : public QFrame
{
    Q_OBJECT
public:
    explicit UsageRangePopup(QWidget *parent = nullptr);
    // 打开前设置当前区间（用于高亮与日历锚点）
    void setState(const QString &preset, const QDateTime &start, const QDateTime &end);
    void openBelowRight(QWidget *filtersRow);

signals:
    void presetChosen(const QString &id);
    // 日历上点选的起止日（自然日）
    void customChosen(const QDate &start, const QDate &end);
    void closed();

protected:
    void hideEvent(QHideEvent *event) override;

private:
    void rebuildPresets();
    void rebuildCalendar();
    void pickDay(const QDate &day);
    void applyLayoutMode(bool compact);

    QString m_preset;
    QDateTime m_start;
    QDateTime m_end;
    QDate m_pendingStart;
    QDate m_pendingEnd;
    QDate m_anchor;
    QBoxLayout *m_rootLayout = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QWidget *m_calendar = nullptr;
    QWidget *m_presetBox = nullptr;
    QWidget *m_monthHost = nullptr;
    QBoxLayout *m_monthLayout = nullptr;
    QLabel *m_title = nullptr;
    QPushButton *m_confirm = nullptr;
    bool m_compact = false;
};

// 概览卡（.usage-stat）
class UsageStat : public QFrame
{
    Q_OBJECT
public:
    UsageStat(const QString &caption, QWidget *parent = nullptr);
    void setValue(const QString &value);

private:
    QLabel *m_caption = nullptr;
    QLabel *m_value = nullptr;
};

// 设置中心「用量」页（#panel-usage）：筛选 + 概览 + 三图 + 明细表。
// 数据来自后端 GET /usage/stats，由宿主回填 setStats()。
class UsagePanel : public QWidget
{
    Q_OBJECT
public:
    explicit UsagePanel(QWidget *parent = nullptr);

    // 显示名映射的来源（/config/models 的 models）
    void setCatalogModels(const QVariantList &models);
    // 宿主回填：requestId 必须原样带回 statsRequested 下发的值；
    // data 与 GET /usage/stats 的响应同构
    void setStats(const QString &requestId, const QVariantMap &data);
    void setLoadFailed(const QString &requestId, const QString &error);
    // 进入该页：首次懒加载
    void enterTab();

signals:
    void statsRequested(const QString &requestId, const QString &start, const QString &end,
                        const QString &provider, const QString &model);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct Column { QString key; QString label; bool numeric; bool tokens; };

    void buildUi();
    // 概览列数与图表并排/堆叠跟随窗口宽度（对应 style.css 的 900px / 640px 媒体查询）
    void applyResponsive();
    void layoutOverviewCards(int columns);
    void layoutChartCards(bool stacked);
    // 首次展开时才建浮层，并以 ChartWidget 为父窗口（否则 QSS 不生效）
    void ensurePopups(QWidget *anchor);
    QWidget *buildFilters();
    QWidget *buildOverview();
    QWidget *buildTrendCard();
    QWidget *buildPieCard();
    QWidget *buildBarCard();
    QWidget *buildTableCard();

    void windowRange(QDateTime &start, QDateTime &end) const;
    QString rangeText() const;
    bool drilled() const;
    QVariantList rollupDays() const;
    QVariantList chartBuckets() const;
    void domainAndView(qint64 &domainStart, qint64 &domainEnd, bool &hasView, qint64 &viewStart,
                       qint64 &viewEnd) const;

    void syncCatalog();
    QString modelName(const QString &key, const QString &fallback = QString()) const;
    QString providerName(const QString &id, const QString &fallback = QString()) const;

    void render();
    void renderFilters();
    void renderOverview();
    void renderCharts();
    void renderPie();
    void renderTable();
    void renderListboxes();

    void loadStats(bool force = false);
    void adoptData(const QVariantMap &data);
    // 请求对账：仅当 requestId 与在飞请求一致时返回 true，并带出缓存键
    bool takePendingResponse(const QString &requestId, QString &cacheKey);
    // 作废在飞请求（重新发起或命中缓存都会换 ID）
    void invalidatePendingResponse();
    void pickProvider(const QString &value);
    void pickModel(const QString &value);
    void applyCustom(const QDate &startDay, const QDate &endDay);
    QVariantMap candidateModel(const QString &key) const;

    QList<Column> columns() const;
    QVariantList tableRows(bool provider) const;
    bool pieGroups(QVariantList &groups, QString &unit) const;

    void closePopups();
    void setView(bool hasView, qint64 start, qint64 end);
    void announceChartHover(const QVariantMap &row);

    // 状态（对应 app.js state.usage）
    bool m_loaded = false;
    bool m_loading = false;
    bool m_hasData = false;
    QString m_error;
    QString m_preset = QStringLiteral("7d");
    QDateTime m_customStart;
    QDateTime m_customEnd;
    QString m_granularity = QStringLiteral("day");
    QString m_groupBy = QStringLiteral("model");
    QString m_provider;
    QString m_model;
    QHash<QString, QVariantMap> m_catalog;
    QHash<QString, QString> m_configuredProviders;
    QVariantList m_providerOptions;
    QVariantList m_modelOptions;
    QVariantMap m_data;
    bool m_hasView = false;
    qint64 m_viewStart = 0;
    qint64 m_viewEnd = 0;
    QString m_sortKey = QStringLiteral("total");
    bool m_sortDesc = true;
    // 请求去重：晚到的旧响应不能写进当前视图（app.js token）。
    // 每次发起请求都会换一个 requestId，只有带当前 ID 的响应才被接受
    quint64 m_requestSeq = 0;
    QString m_pendingRequestId;
    QString m_pendingKey;
    QHash<QString, QVariantMap> m_cache;
    QVariantList m_models;

    UsageSelect *m_groupSelect = nullptr;
    UsageSelect *m_providerSelect = nullptr;
    UsageSelect *m_modelSelect = nullptr;
    UsageSelect *m_granularitySelect = nullptr;
    UsageSelect *m_rangeSelect = nullptr;
    QWidget *m_filtersRow = nullptr;
    FlowLayout *m_filterFlow = nullptr;
    QList<QLabel *> m_filterLabels;
    QVBoxLayout *m_rootLayout = nullptr;
    QGridLayout *m_overviewLayout = nullptr;
    QWidget *m_gridRow = nullptr;
    QGridLayout *m_gridLayout = nullptr;
    int m_layoutMode = -1; // 0: >900 六列并排 / 1: ≤900 三列堆叠 / 2: ≤640 两列
    QLabel *m_status = nullptr;
    QWidget *m_overview = nullptr;
    QList<UsageStat *> m_stats;
    QLabel *m_pieTitle = nullptr;
    QLabel *m_tableTitle = nullptr;
    UsageTimeChart *m_line = nullptr;
    UsageTimeChart *m_bar = nullptr;
    UsagePieChart *m_pie = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_live = nullptr;
    UsageListPopup *m_listPopup = nullptr;
    UsageRangePopup *m_rangePopup = nullptr;
    // 当前展开的触发器与最近一次收起时刻：点击已展开的触发器要能收起（app.js toggleUsagePopover）
    QWidget *m_activeTrigger = nullptr;
    qint64 m_popupClosedAt = 0;
    QString m_listRole;
};

} // namespace gs

#endif // GS_USAGEPANEL_H

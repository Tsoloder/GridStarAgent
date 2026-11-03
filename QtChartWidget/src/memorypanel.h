#ifndef GS_MEMORYPANEL_H
#define GS_MEMORYPANEL_H

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QEvent;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

// 设置中心「记忆」页（webui #panel-memory / app.js memory* 一组函数）。
//
// 三段结构（与 webui 同序）：
//   1. 经验条目 —— 状态条 + 三个开关 + 筛选行 + 批量条 + 卡片列表 + 加载更多
//   2. 参数记忆 —— 按工具分组的字段列表，字段值可编辑（类型族由原值锁定）
//   3. 维护    —— 立即提炼 / 运行维护 / 清空全部工具参数记忆
//
// 库不做网络：本类只渲染数据、发意图信号，宿主负责 /memory/* 的调用与回填。
//
// 样式：本页所有 .memory-* / .linkButton / .actionButton 规则集中在 theme.cpp
// 「设置中心 · 记忆页」段，面板自身不再拼局部样式表。
class MemoryPanel : public QWidget
{
    Q_OBJECT
public:
    explicit MemoryPanel(QWidget *parent = nullptr);

    // 切到本页时才懒加载（与 UsagePanel::enterTab 同一套做法）；离开时停轮询
    void enterTab();
    void leaveTab();

    // ---- 宿主回填 ----
    // GET /memory/summary：{enabled, config, store, experience, maintenance, notice}
    void setSummary(const QVariantMap &summary);
    // GET /memory/experiences：{items, total, page, page_size, facets}
    // page > 1 视为「加载更多」，新结果追加到现有列表
    void setListing(const QVariantMap &listing);
    void setListFailed(const QString &error);
    // GET /memory/tool-parameters：{tools:[{tool, fields, updated_at}], tool_count}
    void setTools(const QVariantMap &payload);
    // PATCH /memory/config 之类的操作进行中，禁掉会打架的按钮
    void setBusy(bool busy);
    // 宿主下发的即时提示（保存失败、已保存之类），追加在状态条同级提示位
    void setStatusText(const QString &text);

signals:
    // 宿主收到后并发拉 /memory/summary + /memory/tool-parameters（或只补失败的那一份）
    void refreshRequested();
    // filters: {status, kind, scope, q, page_size}（空值键不下发）
    void listRequested(int page, const QVariantMap &filters);
    void configPatchRequested(const QVariantMap &updates);
    // action = approve | reject | deprecate | restore | delete
    void actionRequested(const QString &id, const QString &action);
    void bulkRequested(const QStringList &ids, const QString &action);
    // 查看详情：宿主拉单条后自己弹框，或直接用列表里已有的 payload
    void detailRequested(const QString &id);
    void fieldSaveRequested(const QString &tool, const QString &field, const QVariant &value);
    void fieldDeleteRequested(const QString &tool, const QString &field);
    void toolClearRequested(const QString &tool);
    void toolsClearAllRequested();
    // kind = extract | governance
    void maintenanceRequested(const QString &kind);

private:
    // ---- 搭骨架 ----
    void buildUi();
    QWidget *buildStatusBar();
    QWidget *buildSwitchRow();
    QWidget *buildExperienceSection();
    QWidget *buildFilterRow();
    QWidget *buildBulkBar();
    QWidget *buildToolSection();
    QWidget *buildMaintenanceSection();
    QWidget *buildCard(const QVariantMap &item);
    QWidget *buildToolBlock(const QVariantMap &tool);
    QWidget *buildFieldRow(const QString &tool, const QString &field, const QVariant &value);

    // ---- 渲染（对应 app.js 的 memoryRender*） ----
    void renderAll();
    void renderStatus();
    void renderSwitches();
    // 只切开关的可用性（不动勾选值）：保存进行中不该把用户刚点下的勾退回旧值
    void applySwitchEnabled();
    void renderFilters();
    void renderList();
    void renderBulk();
    void renderTools();
    void renderMaintenance();

    // ---- 请求与选择 ----
    QVariantMap currentFilters() const;
    void requestPage(int page);
    void reloadFromFirstPage();
    void clearSelection();
    void pruneSelection();
    QVariant storedValue(const QString &tool, const QString &field) const;

    // ---- 交互 ----
    void actOnItem(const QString &id, const QString &action);
    void runBulk(const QString &action);
    void startFieldEdit(const QString &tool, const QString &field);
    void cancelFieldEdit();
    void saveField(const QString &tool, const QString &field, QLineEdit *input);
    void deleteField(const QString &tool, const QString &field);
    void clearTool(const QString &tool);
    void clearAllTools();
    void startMaintenance(const QString &kind);

    // ---- 维护轮询 ----
    bool maintenanceRunning() const;
    void syncPolling();

    // 状态（对应 app.js state.memory）
    bool m_loading = false;
    bool m_loaded = false;
    bool m_toolsLoaded = false;
    bool m_busy = false;
    bool m_tabActive = false;
    bool m_syncingSwitches = false;
    bool m_syncingFilters = false;
    bool m_syncingBulk = false;
    QString m_error;
    QString m_statusText;
    QVariantMap m_summary;
    QVariantList m_items;
    int m_total = 0;
    int m_page = 1;
    int m_pageSize = 20;
    QVariantMap m_facets;
    QVariantMap m_tools;
    QSet<QString> m_selected;
    QSet<QString> m_openTools;
    // 正在编辑的字段（webui mem.editing）
    QString m_editTool;
    QString m_editField;
    // 已提交、等宿主回填确认的字段保存（成功才退出编辑态）
    QString m_pendingTool;
    QString m_pendingField;
    QVariant m_pendingValue;
    // 维护任务刚投递时先自己轮询一会儿，等 summary 把 state 翻成 running
    qint64 m_pollArmUntil = 0;

    // 控件
    QVBoxLayout *m_root = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QVBoxLayout *m_contentLayout = nullptr;

    QLabel *m_status = nullptr;

    QWidget *m_switchHost = nullptr;
    QHBoxLayout *m_switchLayout = nullptr;
    QWidget *m_switchCards[3] = {nullptr, nullptr, nullptr};
    QCheckBox *m_switchBoxes[3] = {nullptr, nullptr, nullptr};

    QLabel *m_count = nullptr;
    QComboBox *m_statusFilter = nullptr;
    QComboBox *m_kindFilter = nullptr;
    QComboBox *m_scopeFilter = nullptr;
    QLineEdit *m_search = nullptr;
    QPushButton *m_refresh = nullptr;
    QTimer *m_searchTimer = nullptr;

    QWidget *m_bulkBar = nullptr;
    QCheckBox *m_selectPage = nullptr;
    QLabel *m_bulkCount = nullptr;
    QPushButton *m_bulkApprove = nullptr;
    QPushButton *m_bulkReject = nullptr;

    QWidget *m_listHost = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QWidget *m_moreRow = nullptr;
    QPushButton *m_loadMore = nullptr;
    QList<QPushButton *> m_cardButtons;
    QHash<QString, QCheckBox *> m_cardChecks;

    QLabel *m_toolCount = nullptr;
    QVBoxLayout *m_toolsLayout = nullptr;
    QList<QPushButton *> m_toolButtons;

    QLabel *m_maintResult = nullptr;
    QPushButton *m_runExtract = nullptr;
    QPushButton *m_runGovernance = nullptr;
    QPushButton *m_clearTools = nullptr;
    QTimer *m_pollTimer = nullptr;
};

} // namespace gs

#endif // GS_MEMORYPANEL_H
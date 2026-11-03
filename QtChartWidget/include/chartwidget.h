#ifndef CHARTWIDGET_H
#define CHARTWIDGET_H

#include "qtchartwidget_global.h"

#include <QHash>
#include <QPointer>
#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QFrame;
class QLabel;
class QPushButton;
class QScrollArea;
class QSpacerItem;
class QTimer;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

class Composer;
class ConnectionButton;
class DropOverlay;
class MessageWidget;
class PhasePanel;
class SessionPanel;
class SettingsDialog;
class ThemeListPopup;
class ThemeSwatch;
class Toast;
class ToolItemWidget;
class TrajectoryView;
class TurnRailDot;
class TurnRailRow;
class WorkflowRunCard;

// GridStar AI 主界面（webui/index.html 的 main.app-shell）：
// 顶栏（品牌 / 连接 / 皮肤切换）+ 会话栏 + 视图页签（对话 / 轨迹）+ 消息区 +
// 轮次导航轨 + 阶段面板 + 输入区，叠加会话面板 / 选择浮层 / Toast / 拖拽遮罩。
// 部件本身不做任何网络请求：宿主通过 setter 推数据、通过信号收交互。
//
// 线程模型：所有公开 API 都必须在 GUI 线程调用（内部直接操作 QWidget 与 QSS，
// Qt 的部件体系没有跨线程保护）。宿主在 HTTP / 流式回调里拿到数据时，请用信号槽或
// QMetaObject::invokeMethod(..., Qt::QueuedConnection) 切回 GUI 线程再推。
// 数据推送类入口（消息流 / 轨迹 / 会话 / 设置）带运行期校验：非 GUI 线程调用会被忽略并告警。
class QTCHARTWIDGET_EXPORT ChartWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ChartWidget(QWidget *parent = nullptr);
    ~ChartWidget() override;

    // ---- 皮肤（深色 / 银白 / 蔚蓝，对应 webui 的 data-theme） ----
    void setTheme(const QString &id);
    QString theme() const;

    // ---- 顶栏 / 会话栏 ----
    void setConnectionState(const QString &state, const QString &label);
    // sessions 支持可选 status 字段：running|done|stopped|error|waiting
    // （缺省时回退到服务端的 waiting / active 布尔字段）
    void setSessions(const QVariantList &sessions);
    void setCurrentSessionTitle(const QString &title);
    QString currentSessionTitle() const;

    // ---- 视图页签 ----
    void setViewTab(const QString &tab); // "chat" | "traj"
    QString viewTab() const { return m_viewTab; }

    // ---- 输入区状态 ----
    void setModels(const QVariantList &models);
    void setCurrentModel(const QString &key);
    QString currentModel() const;
    void setSkills(const QVariantList &skills);
    void setCurrentSkill(const QString &id);
    QString currentSkill() const;
    void setMode(const QString &mode);
    QString mode() const;
    void setBusy(bool busy);
    bool isBusy() const;
    void setConfigLoaded(bool loaded);
    void setConfigWarning(const QString &text);

    // ---- 专业分类（webui #category-select）：categories: [{id, name}]，
    // 首项「全部」由库自己补（id 为空串） ----
    void setCategories(const QVariantList &categories);
    void setCurrentCategory(const QString &id);
    QString currentCategory() const;
    // 会话 meta 里的 category 回填（app.js syncCategoryFromMeta）
    void syncCategoryFromSession(const QString &category);

    // ---- 知识库（webui #kb-select）：libraries: [{id, name, documents, chunks}]，
    // 库自己补「不使用知识库」（id = "__none__"）与「全部知识库」（id = "*"）两项 ----
    void setKnowledgeBases(const QVariantList &libraries);
    void setCurrentKnowledgeBase(const QString &id);
    QString currentKnowledgeBase() const;

    // ---- 上下文用量圆环（webui #context-ring）：
    // usage: {max, used, left, percent, estimated, model}；空 map 收进圆环 ----
    void setContextUsage(const QVariantMap &usage);
    void clearContextUsage();

    QString inputText() const;
    void setInputText(const QString &text);
    void focusInput();

    // ---- 附件（拖拽 / 选择文件后由宿主上传，再回填芯片） ----
    void addAttachments(const QVariantList &items);
    void clearAttachments();
    QVariantList attachments() const;
    void setVoiceEnabled(bool enabled);
    void setVoiceRecording(bool recording);

    // ---- 选择 / 审批浮层（模型提问、工具参数确认、审批共用输入框上方那张卡） ----
    // payload：{ question, title, options[], tool_params|toolparams }
    void showChoice(const QVariantMap &payload);
    void closeChoice();
    // 宿主把审批 POST 成功后调用：卡片收卡；失败时用 reEnableApproval 恢复按钮
    void resolveApproval(const QString &callId, bool approved);
    void reEnableApproval(const QString &callId);

    // ---- 消息流（对应 app.js createMessage / renderHistory / finishAssistant） ----
    void clearMessages();
    // resume = true：这批历史后面还要接上正在跑的新一轮（重连场景）。
    // 此时末尾那轮的询问已经被本轮 user 消息回答过了，不能再当待答弹出来，
    // 否则重连后会把已审批的旧参数卡重新弹回输入框上方（webui c4a6d84）
    void setHistory(const QVariantList &messages, bool resume = false);
    void appendUserMessage(const QString &content, const QVariantList &attachments = QVariantList());
    // 一次性追加完整的助手消息（正文里的结构化块会立即渲染成卡片）
    void appendAssistantMessage(const QString &content, const QString &label = QString(),
                                const QVariantList &attachments = QVariantList());
    // 流式：appendAssistantText* → appendReasoning / appendToolCall / appendToolResult
    //       → finishAssistant
    void appendAssistantText(const QString &delta);
    void appendReasoning(const QString &delta);
    void finishAssistant();
    void appendToolCall(const QString &callId, const QString &name, const QVariant &args);
    void appendToolResult(const QString &callId, const QString &name, const QString &result);
    // 审批到达（event: { name, args, schema, call_id }）：弹出输入框上方的审批浮层
    void appendApproval(const QVariantMap &event);
    // event: { type: workflow_started|workflow_step|workflow_done, index, tool, desc, status }
    void appendWorkflowEvent(const QVariantMap &event);
    // 本轮用量明细（total/input/output/estimated/cache_read/reasoning/model）
    void setTokenUsage(qint64 total, qint64 input, qint64 output, bool estimated);
    void setTokenUsageDetail(const QVariantMap &usage);
    // 本轮用时明细（elapsed/think/ttft/tps）
    void setTurnTiming(const QVariantMap &timing);
    // 实时「已用时」计时（startMs 为本轮开始时刻，0 表示从当前起算）
    void startLiveTiming(qint64 startMs = 0);
    // 独立的 TOOL RESULT 气泡（工具结果找不到对应调用时的退化形态）
    void appendToolResultMessage(const QString &content);
    // 失败提示气泡（renderFailure）：retryMessage 非空时带「重发这条消息」按钮
    void appendFailure(const QString &text, bool retryable, const QString &retryMessage,
                       const QString &retryDisplay, const QVariantList &retryAttachments);
    // 「已停止」标记（markStopped）
    void markCurrentStopped();
    void setPhasePlan(const QVariant &value);
    void showToast(const QString &text);

    // ---- 轨迹视图 ----
    // events 与后端 /sessions/{id}/trajectory 的 events 同构；宿主收到
    // trajectoryReloadRequested 后拉取并回填
    void setTrajectoryEvents(const QVariantList &events);
    void appendTrajectoryEvents(const QVariantList &events);

    // ---- 设置中心 ----
    void setSettingsDraft(const QVariantMap &config, const QVariant &revision);
    void setDiscoveredModels(const QString &providerId, const QVariantList &models);
    // 模型信息快照（GET /config/model-metadata 的状态 + 可选的 limits）：
    // 状态行始终能显示；要填模型上限就额外塞 "limits": {modelId: {...}}
    // 或 "models": [{id, context_window, max_output_tokens, capabilities}] 数组
    void setModelMetadata(const QVariantMap &snapshot);
    // kind = "load" | "refresh"；httpStatus 非 0 时状态行会带上 HTTP 码
    void setModelMetadataFailed(const QString &kind, const QString &error, int httpStatus = 0);
    void setProviderBusy(bool testing, bool reading);
    void setSettingsSkills(const QVariantList &skills, bool loading, const QString &error);
    void setMcpTools(const QVariantList &tools, bool connected, bool loading,
                     const QString &error);
    void openSettings(const QString &tab = QString());
    void closeSettings();
    void setSettingsStatus(const QString &text);
    // 宿主保存成功后调用：清除脏标记并关闭对话框
    void settingsSaved();

    // ---- 设置中心「记忆」页（GET /memory/*） ----
    // summary: {enabled, config, store, experience, maintenance, notice}
    void setMemorySummary(const QVariantMap &summary);
    // listing: {items, total, page, page_size, facets}
    void setMemoryExperiences(const QVariantMap &listing);
    void setMemoryListFailed(const QString &error);
    // payload: {tools:[{tool, fields, updated_at}], tool_count}
    void setMemoryTools(const QVariantMap &payload);
    void setMemoryStatusText(const QString &text);

    // ---- 设置中心「知识库」页（GET /kb/*） ----
    void setKbLibraries(const QVariantList &libraries);
    void setKbStatus(const QVariantMap &status);
    void setKbFolders(const QVariantList &folders, const QVariantMap &tree);
    void setKbDocuments(const QVariantList &documents, int total);
    void setKbSearchResult(const QVariantMap &result);
    void setKbStatusText(const QString &text);

    // ---- 用量统计（设置中心「用量」页） ----
    // 宿主收到 usageStatsRequested 后拉 GET /usage/stats 并回填；
    // data 与后端响应同构：{range, totals, buckets, providers, models, candidates}
    // 回填时必须原样带回信号下发的 requestId，不匹配的在飞响应会被丢弃
    void setUsageStats(const QString &requestId, const QVariantMap &data);
    void setUsageLoadFailed(const QString &requestId, const QString &error);

    // ---- 界面缩放（VSCode 式快捷键：Ctrl+= 放大 / Ctrl+- 缩小 / Ctrl+0 重置） ----
    void zoomIn();
    void zoomOut();
    void zoomReset();
    qreal zoomFactor() const;

signals:
    void sendMessage(const QString &message, const QString &display,
                     const QVariantList &attachments);
    void stopRequested();
    void modeChanged(const QString &mode);
    void modelSelected(const QString &key);
    void skillSelected(const QString &id);

    void newSessionRequested();
    void sessionSelected(const QString &id);
    void sessionRenamed(const QString &id);
    void sessionCleared(const QString &id);
    void sessionDeleted(const QString &id);
    // 输入框斜杠面板里的「/ 导出对话」：宿主 POST /sessions/{id}/export 并保存返回的文件名
    void sessionExportRequested();
    void connectionCheckRequested();
    // 顶栏品牌图标被点击：宿主确认后启动 GridStar 主程序（webui launchGridStar）
    void launchAppRequested();

    // 输入区两个新下拉
    void categorySelected(const QString &id);
    void knowledgeBaseSelected(const QString &id);

    void optionChosen(const QString &value, const QString &label);
    // 「结束」类选项（webui 5948605）：点它就是本轮收尾。宿主收到后 POST
    // /sessions/{id}/end-turn 并落一条用户消息，不开新一轮——否则 auto 模式会接着
    // 干、再弹一张同样的卡。识别只做保守判断，见 chartwidget.cpp 的 isEndTurnOption
    void endTurnRequested(const QString &label);
    // 工具参数确认不发独立信号：按 webui 口径打包成 <structured_interaction> 一轮消息，
    // 由 sendMessage 带出（display 为所选按钮文案）
    void approvalDecided(const QString &callId, bool approved, const QVariantMap &args);
    void workflowRunRequested(const QVariantList &steps);
    void retryRequested(const QString &message, const QString &display,
                        const QVariantList &attachments);

    // 宿主收到后拉 /sessions/{id}/trajectory 并回填 setTrajectoryEvents
    void trajectoryReloadRequested();
    void viewTabChanged(const QString &tab);
    void themeChanged(const QString &id);
    void choiceOpenChanged(bool open);

    void settingsSaveRequested(const QVariantMap &config, const QVariant &revision);
    // 设置中心被打开（tab = models | skills | mcp | memory | kb | usage）：
    // 宿主据此拉对应数据（如 model-metadata、memory summary、kb libraries）。
    // 宿主导航或用户点设置按钮都会触发
    void settingsOpened(const QString &tab);
    void testProviderRequested(const QString &providerId);
    void readModelsRequested(const QString &providerId);
    // 宿主收到后 POST /config/model-metadata/refresh，再回填 setModelMetadata
    void modelMetadataRefreshRequested();
    // 新增一个模型后，webui 会立刻拉它的上限（app.js fillModelLimits →
    // POST /config/model-metadata/resolve）；库不联网，只把时机发出来，
    // 宿主拿到后把 resolve 结果放进 setModelMetadata 的 limits 键即可
    void modelLimitsResolveRequested(const QString &providerId, const QString &modelId);
    void refreshSkillsRequested();
    void refreshMcpRequested();
    // 宿主收到后拉 GET /usage/stats（start/end 为本地时间 "yyyy-MM-ddTHH:mm"）并回填 setUsageStats
    void usageStatsRequested(const QString &requestId, const QString &start, const QString &end,
                             const QString &provider, const QString &model);

    // ---- 记忆页的意图（宿主调 /memory/*） ----
    void memoryRefreshRequested();
    void memoryListRequested(int page, const QVariantMap &filters);
    void memoryConfigPatchRequested(const QVariantMap &updates);
    void memoryActionRequested(const QString &id, const QString &action);
    void memoryBulkRequested(const QStringList &ids, const QString &action);
    void memoryDetailRequested(const QString &id);
    void memoryFieldSaveRequested(const QString &tool, const QString &field, const QVariant &value);
    void memoryFieldDeleteRequested(const QString &tool, const QString &field);
    void memoryToolClearRequested(const QString &tool);
    void memoryToolsClearAllRequested();
    void memoryMaintenanceRequested(const QString &kind);

    // ---- 知识库页的意图（宿主调 /kb/*） ----
    void kbRefreshRequested(const QString &libraryId);
    void kbLibraryCreateRequested(const QString &name);
    void kbLibraryRenameRequested(const QString &libraryId, const QString &name);
    void kbLibraryDeleteRequested(const QString &libraryId);
    void kbFilesImportRequested(const QStringList &paths, const QString &libraryId,
                                const QString &folderId);
    void kbDirImportRequested(const QString &libraryId, const QString &dirPath);
    // 目录导入带上目标文件夹（空串 = 库根）：KbPanel 现在只发这个，旧的 dirImportRequested
    // 路径不再 emit（宿主改接本信号）
    void kbDirImportToFolderRequested(const QString &libraryId, const QString &dirPath,
                                      const QString &folderId);
    void kbDocumentDeleteRequested(const QString &docId);
    void kbDocumentReindexRequested(const QString &docId);
    void kbDocumentMoveRequested(const QString &docId, const QString &folderId);
    void kbFolderCreateRequested(const QString &libraryId, const QString &parentId,
                                 const QString &name);
    void kbFolderRenameRequested(const QString &folderId, const QString &name);
    void kbFolderDeleteRequested(const QString &folderId);
    void kbSearchRequested(const QString &libraryId, const QString &query);
    // 归档树切目录（libraryId, folderId；folderId 为 all / root / 真实 id）
    void kbFolderChanged(const QString &libraryId, const QString &folderId);

    void attachRequested();
    void voiceRequested();
    void attachmentsAdded(const QVariantList &items);
    void attachmentRemoved(const QString &id);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    // Windows OLE 拖拽下 dragMoveEvent 默认 ignore 会导致 drop 被拒，必须显式 accept
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // 线程契约校验（见类注释）：非 GUI 线程返回 false，调用方应直接 return
    bool assertGuiThread(const char *entry) const;

    void buildTopbar();
    void buildSessionbar();
    void buildViewTabs();
    void buildMessages();
    void buildTurnRail();
    QWidget *createEmptyState();

    MessageWidget *createMessage(const QString &role, const QString &content,
                                 const QString &label = QString(),
                                 const QVariantList &attachments = QVariantList());
    MessageWidget *ensureAssistant();
    // deferred：历史重放时不立即弹询问浮层（等整轮重放完再弹，避免旧询问闪动）
    void finishAssistantInternal(bool deferred);
    void renderStructured(const QVariantMap &data, MessageWidget *message);
    void removeWelcome();
    void updateEmptyState();
    void updateTitleElide();
    void scrollToEnd(bool force = false);
    void pinToBottom(int retries);
    bool atBottom() const;
    void toggleSessionPanel();
    void closeSessionPanel();
    void layoutOverlays();
    void applyZoom();
    void applyTheme(const QString &id);

    // 轮次导航轨（鼠标移入轨道即向右展开整轮列表，见 .turn-rail-panel）
    void rebuildTurnRail();
    void updateTurnRailActive();
    void showTurnRailPanel();
    void scheduleHideTurnRailPanel();
    void hideTurnRailPanel();
    void layoutTurnRailPanel();
    void focusTurn(int turn);

    // 计划窗口：跑完即收起；选择浮层展开时上移让位
    void hidePhasePanel();
    void settlePhasePlan();
    void syncPhaseLift();
    static bool planComplete(const QVariantMap &plan);

    // 框架
    QVBoxLayout *m_root = nullptr;
    ConnectionButton *m_connection = nullptr;
    QPushButton *m_newSession = nullptr;
    QWidget *m_sessionTrigger = nullptr;
    QLabel *m_currentTitle = nullptr;
    QLabel *m_sessionChevron = nullptr;
    QWidget *m_themeTrigger = nullptr;
    ThemeSwatch *m_themeSwatch = nullptr;
    QLabel *m_themeLabel = nullptr;
    ThemeListPopup *m_themePopup = nullptr;
    QWidget *m_viewTabs = nullptr;
    QPushButton *m_tabChat = nullptr;
    QPushButton *m_tabTraj = nullptr;
    QScrollArea *m_messages = nullptr;
    QWidget *m_messageList = nullptr;
    QVBoxLayout *m_messageLayout = nullptr;
    QWidget *m_emptyState = nullptr;
    QWidget *m_phaseWrap = nullptr;
    PhasePanel *m_phasePanel = nullptr;
    QSpacerItem *m_phaseLiftSpacer = nullptr;
    TrajectoryView *m_trajView = nullptr;
    Composer *m_composer = nullptr;
    QString m_viewTab = QStringLiteral("chat");

    // 轮次导航轨（.turn-rail）：页面最左边缘竖排白点，一轮一个；
    // 鼠标移入轨道区域后向右展开 .turn-rail-panel 列出整轮
    QWidget *m_turnRail = nullptr;
    QFrame *m_turnRailPanel = nullptr;
    QLabel *m_turnRailCount = nullptr;
    QScrollArea *m_turnRailScroll = nullptr;
    QWidget *m_turnRailList = nullptr;
    QVBoxLayout *m_turnRailListLayout = nullptr;
    QTimer *m_turnRailHideTimer = nullptr;
    QList<TurnRailDot *> m_railDots;
    QList<TurnRailRow *> m_railRows;
    QList<QPointer<MessageWidget>> m_railTurns;
    int m_railActive = 0;

    // 叠加层
    SessionPanel *m_sessionPanel = nullptr;
    Toast *m_toast = nullptr;
    DropOverlay *m_dropOverlay = nullptr;
    SettingsDialog *m_settings = nullptr;

    // 当前流式轮次（app.js 的 state.assistant）
    QPointer<MessageWidget> m_current;
    QString m_currentText;
    bool m_currentFinished = true;
    QHash<QString, QPointer<ToolItemWidget>> m_toolItems;

    // 工作流卡（state.workflow）
    QPointer<WorkflowRunCard> m_workflow;
    QPointer<MessageWidget> m_workflowMessage;
    QVariantList m_workflowSteps;

    // 历史重放：最后一轮是否停在未作答的询问上（恢复「待确认」徽标）
    bool m_lastTurnAwaiting = false;
    // 最近一次收到的阶段计划（判断是否跑完以决定是否收起计划窗口）
    QVariantMap m_phasePlanData;

    QVariantList m_sessions;
    QVariantList m_skills;
    // models 缓存：用量弹层的「提供方 / 模型」要按 provider_name 映射
    QVariantList m_models;
    int m_dragDepth = 0;
    // 流式期间贴底才跟随：用户上滚查看历史时不被下一个分片拽回底部
    bool m_followBottom = true;
    bool m_scrollLocked = false;
};

} // namespace gs

#endif // CHARTWIDGET_H

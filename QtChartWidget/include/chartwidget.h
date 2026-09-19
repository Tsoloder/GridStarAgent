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
    void setHistory(const QVariantList &messages);
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
    void setProviderBusy(bool testing, bool reading);
    void setSettingsSkills(const QVariantList &skills, bool loading, const QString &error);
    void setMcpTools(const QVariantList &tools, bool connected, bool loading,
                     const QString &error);
    void openSettings(const QString &tab = QString());
    void closeSettings();
    void setSettingsStatus(const QString &text);
    // 宿主保存成功后调用：清除脏标记并关闭对话框
    void settingsSaved();

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
    void connectionCheckRequested();

    void optionChosen(const QString &value, const QString &label);
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
    void testProviderRequested(const QString &providerId);
    void readModelsRequested(const QString &providerId);
    void refreshSkillsRequested();
    void refreshMcpRequested();

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
    bool atBottom() const;
    void toggleSessionPanel();
    void closeSessionPanel();
    void layoutOverlays();
    void applyZoom();
    void applyTheme(const QString &id);

    // 轮次导航轨
    void rebuildTurnRail();
    void updateTurnRailActive();
    void showTurnRailTip(int turn);
    void hideTurnRailTip();
    void focusTurn(int turn);

    // 计划窗口：跑完即收起；选择浮层展开时上移让位
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
    TrajectoryView *m_trajView = nullptr;
    Composer *m_composer = nullptr;
    QString m_viewTab = QStringLiteral("chat");

    // 轮次导航轨（.turn-rail）：页面最左边缘竖排白点，一轮一个
    QWidget *m_turnRail = nullptr;
    QFrame *m_turnRailTip = nullptr;
    QLabel *m_turnRailTipText = nullptr;
    QList<TurnRailDot *> m_railDots;
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
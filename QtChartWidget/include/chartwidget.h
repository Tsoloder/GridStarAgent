#ifndef CHARTWIDGET_H
#define CHARTWIDGET_H

#include "qtchartwidget_global.h"

#include <QHash>
#include <QPointer>
#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

class ApprovalCard;
class Composer;
class ConnectionButton;
class DropOverlay;
class MessageWidget;
class PhasePanel;
class SessionPanel;
class SettingsDialog;
class Toast;
class ToolGroupWidget;
class ToolItemWidget;
class WorkflowRunCard;

// GridStar AI 主界面（webui/index.html 的 main.app-shell）：
// 顶栏 + 会话栏 + 消息区 + 阶段面板 + 输入区，叠加会话面板 / Toast / 拖拽遮罩。
// 部件本身不做任何网络请求：宿主通过 setter 推数据、通过信号收交互。
class QTCHARTWIDGET_EXPORT ChartWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ChartWidget(QWidget *parent = nullptr);
    ~ChartWidget() override;

    // ---- 顶栏 / 会话栏 ----
    void setConnectionState(const QString &state, const QString &label);
    void setSessions(const QVariantList &sessions);
    void setCurrentSessionTitle(const QString &title);
    QString currentSessionTitle() const;

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

    // ---- 消息流（对应 app.js createMessage / renderHistory / finishAssistant） ----
    void clearMessages();
    void setHistory(const QVariantList &messages);
    void appendUserMessage(const QString &content, const QVariantList &attachments = QVariantList());
    // 一次性追加完整的助手消息（正文里的结构化块会立即渲染成卡片）
    void appendAssistantMessage(const QString &content, const QString &label = QString(),
                                const QVariantList &attachments = QVariantList());
    // 流式：appendAssistantText* → appendReasoning / appendToolCall / appendToolResult
    //       / appendApproval → finishAssistant
    void appendAssistantText(const QString &delta);
    void appendReasoning(const QString &delta);
    void finishAssistant();
    void appendToolCall(const QString &callId, const QString &name, const QVariant &args);
    void appendToolResult(const QString &callId, const QString &name, const QString &result);
    // event: { name, args, schema, call_id }
    void appendApproval(const QVariantMap &event);
    // 宿主把审批 POST 成功后调用：卡片状态变「已批准/已拒绝」；失败时用 reEnableApproval 恢复按钮
    void resolveApproval(const QString &callId, bool approved);
    void reEnableApproval(const QString &callId);
    // event: { type: workflow_started|workflow_step|workflow_done, index, tool, desc, status }
    void appendWorkflowEvent(const QVariantMap &event);
    void setTokenUsage(qint64 total, qint64 input, qint64 output, bool estimated);
    // 独立的 TOOL RESULT 气泡（工具结果找不到对应调用时的退化形态）
    void appendToolResultMessage(const QString &content);
    // 失败提示气泡（renderFailure）：retry 非空时带「重发这条消息」按钮
    void appendFailure(const QString &text, bool retryable, const QString &retryMessage,
                       const QString &retryDisplay, const QVariantList &retryAttachments);
    void setPhasePlan(const QVariant &value);
    void showToast(const QString &text);

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
    void toolParamsConfirmed(const QString &tool, bool confirmed, const QVariantMap &params,
                             const QString &label);
    void approvalDecided(const QString &callId, bool approved, const QVariantMap &args);
    void approvalJsonInvalid();
    void workflowRunRequested(const QVariantList &steps);
    void retryRequested(const QString &message, const QString &display,
                        const QVariantList &attachments);

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
    void buildTopbar();
    void buildSessionbar();
    void buildMessages();
    QWidget *createEmptyState();

    MessageWidget *createMessage(const QString &role, const QString &content,
                                 const QString &label = QString(),
                                 const QVariantList &attachments = QVariantList());
    MessageWidget *ensureAssistant();
    void renderStructured(const QVariantMap &data, MessageWidget *message);
    ToolGroupWidget *toolGroup(MessageWidget *message);
    void removeWelcome();
    void updateEmptyState();
    void updateTitleElide();
    void scrollToEnd();
    void toggleSessionPanel();
    void closeSessionPanel();
    void layoutOverlays();
    void applyZoom();

    // 框架
    QVBoxLayout *m_root = nullptr;
    ConnectionButton *m_connection = nullptr;
    QPushButton *m_newSession = nullptr;
    QWidget *m_sessionTrigger = nullptr;
    QLabel *m_currentTitle = nullptr;
    QLabel *m_sessionChevron = nullptr;
    QScrollArea *m_messages = nullptr;
    QWidget *m_messageList = nullptr;
    QVBoxLayout *m_messageLayout = nullptr;
    QWidget *m_emptyState = nullptr;
    QWidget *m_phaseWrap = nullptr;
    PhasePanel *m_phasePanel = nullptr;
    Composer *m_composer = nullptr;

    // 叠加层
    SessionPanel *m_sessionPanel = nullptr;
    Toast *m_toast = nullptr;
    DropOverlay *m_dropOverlay = nullptr;
    SettingsDialog *m_settings = nullptr;

    // 当前流式轮次（app.js 的 state.assistant）
    QPointer<MessageWidget> m_current;
    QString m_currentText;
    QPointer<ToolGroupWidget> m_currentGroup;
    bool m_currentFinished = true;
    QHash<QString, ToolItemWidget *> m_toolItems;
    QHash<QString, QPointer<ApprovalCard>> m_approvals;

    // 工作流卡（state.workflow）
    QPointer<WorkflowRunCard> m_workflow;
    QPointer<MessageWidget> m_workflowMessage;
    QVariantList m_workflowSteps;

    QVariantList m_sessions;
    QVariantList m_skills; // setSkills 缓存，ensureAssistant 按 currentSkill 查技能名做气泡标签
    int m_dragDepth = 0;
};

} // namespace gs

#endif // CHARTWIDGET_H

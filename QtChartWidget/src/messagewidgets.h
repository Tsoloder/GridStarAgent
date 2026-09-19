#ifndef GS_MESSAGEWIDGETS_H
#define GS_MESSAGEWIDGETS_H

#include <QFrame>
#include <QPointer>
#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QGraphicsOpacityEffect;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

class MarkdownView;
class Chevron;
class StatusDot;
class PulseDot;
class IconPushButton;

// 过程行（.proc-row）：一行淡字 chip，可就地展开；运行态把箭头换成呼吸点，位置与高度不变
class ProcRow : public QWidget
{
    Q_OBJECT
public:
    // key 为 "think" / "tools"
    ProcRow(const QString &key, const QString &label, QWidget *parent = nullptr);

    QString key() const { return m_key; }
    bool isOpen() const { return m_open; }
    void setOpen(bool open);
    // 收起态右侧摘要（思考进程显示正文片段，工具显示「N 个 · 执行中」）
    void setSummary(const QString &text);
    void setRunning(bool running);
    void setFailed(bool failed);
    // 展开体里的竖线轨道容器（.proc-rail）
    QWidget *rail() const { return m_rail; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QString m_key;
    QWidget *m_head = nullptr;
    Chevron *m_chevron = nullptr;
    PulseDot *m_dot = nullptr;
    QLabel *m_label = nullptr;
    QLabel *m_summary = nullptr;
    QWidget *m_body = nullptr;
    QWidget *m_rail = nullptr;
    QVBoxLayout *m_railLayout = nullptr;
    bool m_open = false;
};

// 单个工具调用项（details.tool-item）：状态点 + 名称 + 状态，可展开参数表/结果
class ToolItemWidget : public QWidget
{
    Q_OBJECT
public:
    ToolItemWidget(const QString &callId, const QString &name, const QVariant &args,
                   QWidget *parent = nullptr);
    QString callId() const { return m_callId; }
    QString state() const { return m_state; }
    void setState(const QString &state);
    void setResult(const QString &result);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setOpen(bool open);

    QString m_callId;
    QString m_state = QStringLiteral("running");
    QWidget *m_summary = nullptr;
    StatusDot *m_dot = nullptr;
    QLabel *m_status = nullptr;
    QWidget *m_detail = nullptr;
    QWidget *m_resultWrap = nullptr;
    QLabel *m_result = nullptr;
    bool m_open = false;
};

// 静态工作流提案卡（.structured + "静态工作流"）
class WorkflowProposalCard : public QFrame
{
    Q_OBJECT
public:
    explicit WorkflowProposalCard(const QVariantList &steps, QWidget *parent = nullptr);

signals:
    void runRequested(const QVariantList &steps);
};

// 工作流执行卡（.workflow-card）
class WorkflowRunCard : public QFrame
{
    Q_OBJECT
public:
    explicit WorkflowRunCard(QWidget *parent = nullptr);
    void setSteps(const QVariantList &steps);
    void setStatus(const QString &status);

private:
    QLabel *m_status = nullptr;
    QWidget *m_body = nullptr;
    QVBoxLayout *m_bodyLayout = nullptr;
};

// 一条消息（article.message）：一轮一张卡 —— 正文 + 过程区（思考/工具）+ 底部信息行。
// 卡片不带身份行，靠左右对齐与渐变浮层区分角色（app.js createMessage）
class MessageWidget : public QWidget
{
    Q_OBJECT
public:
    MessageWidget(const QString &role, const QString &label = QString(),
                  QWidget *parent = nullptr);

    QString role() const { return m_role; }
    MarkdownView *body() const { return m_body; }
    // 文章级竖排布局：turn-card 之后追加工作流提案等结构化卡
    QVBoxLayout *stack() const { return m_stack; }
    void setBodyVisible(bool visible);
    // 事后设置/更新卡片顶部的 message-label（RETRY / ERROR / STOPPED / TOOL RESULT）
    void setLabel(const QString &label);
    void setAttachments(const QVariantList &attachments);

    // ---- 过程区 ----
    void appendReasoning(const QString &delta);
    // 思考段落结束（开始产出正文或开始调工具）立即停掉呼吸态
    void settleThink();
    // 流结束（正常/停止/出错）统一落定：停掉所有呼吸点
    void settleProcess();
    ToolItemWidget *addToolCall(const QString &callId, const QString &name, const QVariant &args);
    void updateToolSummary();

    // ---- 底部信息行 ----
    // usage: { total, input, output, estimated, cache_read, reasoning, model }
    void setUsage(const QVariantMap &usage);
    // timing: { elapsed, think, ttft, tps }
    void setTiming(const QVariantMap &timing);
    void setTimeStamp(const QString &isoOrEmpty);
    // 运行中实时「已用时」计时（从本轮开始墙钟起算），finishAssistant 时停掉
    void startLiveTiming(qint64 startMs);
    void stopLiveTiming();
    // 「已停止」标记（app.js markStopped）
    void markStopped();
    // 失败卡整圈红边（app.js renderFailure）
    void setCardError(bool error);
    // 复制按钮可用文本
    void setCopyText(const QString &text) { m_copyText = text; }

signals:
    void copyRequested(const QString &text);

protected:
    void resizeEvent(QResizeEvent *event) override;
    // 卡片悬浮才让复制按钮显形（webui .turn-card:hover .bubble-copy{opacity:1}；
    // QSS 的父 :hover 不会驱动子选择器，只能用事件驱动）
    void enterEvent(QEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void buildFoot();
    void applyMaxWidths();
    void updateThinkSummary();
    ProcRow *ensureProcRow(const QString &key);

    QString m_role;
    QVBoxLayout *m_stack = nullptr;
    QFrame *m_card = nullptr;
    QVBoxLayout *m_cardLayout = nullptr;
    QFrame *m_bubble = nullptr;
    QVBoxLayout *m_bubbleLayout = nullptr;
    MarkdownView *m_body = nullptr;
    QLabel *m_label = nullptr;

    QWidget *m_process = nullptr;
    QVBoxLayout *m_processLayout = nullptr;
    ProcRow *m_think = nullptr;
    ProcRow *m_tools = nullptr;
    QString m_reasoningBuf;

    QWidget *m_foot = nullptr;
    IconPushButton *m_copy = nullptr;
    QGraphicsOpacityEffect *m_copyOpacity = nullptr;
    QPushButton *m_usage = nullptr;
    QPushButton *m_timing = nullptr;
    QLabel *m_time = nullptr;
    QVariantMap m_usageInfo;
    QVariantMap m_timingInfo;
    QTimer *m_liveTimer = nullptr;
    qint64 m_liveStart = 0;
    QString m_copyText;
};

// JSON 工具（与 app.js valueForInput / coerceValue / coerceSchemaValue 对应）
QString valueForInput(const QVariant &value);
QVariant coerceValue(const QString &text, const QVariant &original);
QVariant coerceSchemaValue(const QString &text, const QString &type);
QString prettyJson(const QVariant &value);
QString attachExt(const QString &name);
// app.js formatClock / formatDurationCn / formatInt
QString formatClock(const QString &isoOrEmpty);
QString formatDurationCn(qint64 ms);
QString formatInt(qint64 value);

} // namespace gs

#endif // GS_MESSAGEWIDGETS_H
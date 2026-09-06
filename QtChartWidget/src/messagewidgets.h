#ifndef GS_MESSAGEWIDGETS_H
#define GS_MESSAGEWIDGETS_H

#include <QFrame>
#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

class MarkdownView;
class Chevron;
class StatusDot;

// 单个工具调用项（details.tool-item）：状态点 + 名称 + 状态，可展开参数/结果
class ToolItemWidget : public QWidget
{
    Q_OBJECT
public:
    ToolItemWidget(const QString &callId, const QString &name, const QString &argsJson,
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
    QLabel *m_args = nullptr;
    QWidget *m_resultWrap = nullptr;
    QLabel *m_result = nullptr;
    bool m_open = false;
};

// 工具调用组（details.tool-group）：标题 + N 个 + 汇总状态，可展开
class ToolGroupWidget : public QFrame
{
    Q_OBJECT
public:
    explicit ToolGroupWidget(QWidget *parent = nullptr);
    ToolItemWidget *addToolCall(const QString &callId, const QString &name,
                                const QString &argsJson);
    ToolItemWidget *findTool(const QString &callId) const;
    void updateSummary();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *m_head = nullptr;
    Chevron *m_chevron = nullptr;
    QLabel *m_count = nullptr;
    QLabel *m_status = nullptr;
    QWidget *m_list = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QList<ToolItemWidget *> m_items;
    bool m_open = false;
};

// 选项卡（.structured + "请选择下一步"）
class OptionsCard : public QFrame
{
    Q_OBJECT
public:
    OptionsCard(const QVariantList &options, QWidget *parent = nullptr);

signals:
    void optionChosen(const QString &value, const QString &label);

private:
    QList<QPushButton *> m_buttons;
};

// 确认工具参数卡（.structured.params-card）
class ToolParamsCard : public QFrame
{
    Q_OBJECT
public:
    ToolParamsCard(const QVariantMap &toolParams, const QVariantList &options,
                   QWidget *parent = nullptr);

signals:
    void decided(bool confirmed, const QVariantMap &params, const QString &label);

private:
    struct Param { QString name; QVariant original; QLineEdit *input = nullptr; };
    QList<Param> m_params;
    QList<QPushButton *> m_buttons;
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

// 审批卡（.approval-card）：审批工具 · name + 参数编辑 + 批准/拒绝
class ApprovalCard : public QFrame
{
    Q_OBJECT
public:
    // event: { name, args(QVariantMap), schema(QVariantMap), call_id }
    explicit ApprovalCard(const QVariantMap &event, QWidget *parent = nullptr);
    void setResolved(bool approved);
    void reEnable();

signals:
    void decided(bool approved, const QVariantMap &args);
    void jsonInvalid();

private:
    struct Entry { QString name; QString type; QLineEdit *input = nullptr; QPlainTextEdit *area = nullptr; };
    void setInputsEnabled(bool enabled);

    QList<Entry> m_entries;
    QPlainTextEdit *m_rawArgs = nullptr;
    QLabel *m_status = nullptr;
    QList<QPushButton *> m_buttons;
};

// 一条消息（article.message）：可选思考过程 + 气泡（label/附件/Markdown/token）+ 下方卡片
class MessageWidget : public QWidget
{
    Q_OBJECT
public:
    MessageWidget(const QString &role, const QString &label = QString(),
                  QWidget *parent = nullptr);

    QString role() const { return m_role; }
    MarkdownView *body() const { return m_body; }
    void setBodyVisible(bool visible);
    // 事后设置/更新气泡顶部的 message-label（history 的 active_skills 可能晚于首条消息到达）
    void setLabel(const QString &label);
    void setAttachments(const QVariantList &attachments);
    void setTokenUsage(qint64 total, qint64 input, qint64 output, bool estimated);
    void appendReasoning(const QString &delta);
    QVBoxLayout *stack() const { return m_stack; }

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void applyMaxWidths();

    QString m_role;
    QVBoxLayout *m_stack = nullptr;
    QFrame *m_bubble = nullptr;
    QVBoxLayout *m_bubbleLayout = nullptr;
    MarkdownView *m_body = nullptr;
    QFrame *m_reasoning = nullptr;
    QLabel *m_reasoningText = nullptr;
    QString m_reasoningBuf;
    QLabel *m_tokenUsage = nullptr;
};

// JSON 工具（与 app.js valueForInput / coerceValue / coerceSchemaValue 对应）
QString valueForInput(const QVariant &value);
QVariant coerceValue(const QString &text, const QVariant &original);
QVariant coerceSchemaValue(const QString &text, const QString &type);
QString prettyJson(const QVariant &value);
QString attachExt(const QString &name);

} // namespace gs

#endif // GS_MESSAGEWIDGETS_H

#ifndef GS_CHOICEOVERLAY_H
#define GS_CHOICEOVERLAY_H

#include <QFrame>
#include <QVariant>

QT_BEGIN_NAMESPACE
class QGraphicsOpacityEffect;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPropertyAnimation;
class QPushButton;
class QResizeEvent;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

// 选择浮层（.choice-overlay）：浮在输入框上方、把输入框盖住的一张卡。
// 三种载荷共用同一张卡（app.js renderChoiceCard）：
//   options      —— 模型提问的单选项
//   tool_params  —— 工具参数确认（参数表 + 选项，一次提交同时回填）
//   approval     —— 后端阻塞等待的审批（只有批准/拒绝，无关闭入口）
class ChoiceOverlay : public QFrame
{
    Q_OBJECT
public:
    explicit ChoiceOverlay(QWidget *parent = nullptr);

    // payload：{ question, title, options[], tool_params|toolparams, approval:{event, sessionId} }
    void showChoice(const QVariantMap &payload);
    void closeOverlay();
    bool isOpen() const { return m_open; }
    // 关闭动画结束后是否已彻底隐藏（供计划窗口避让判断）
    bool isVisibleCard() const;

    // 审批回执：由宿主在 POST 成功后调用；失败用 reEnableApproval 恢复按钮
    QString approvalCallId() const { return m_approvalCallId; }
    void setApprovalResolved(const QString &callId, bool approved);
    void reEnableApproval(const QString &callId);

    // 折叠/展开（点标题行）
    void toggleCollapsed();

signals:
    // 普通选项被点选（点一下即确认）
    void optionChosen(const QString &value, const QString &label);
    // 工具参数确认 / 参数 + 「其他」自由文本：都按 webui 口径打包成一轮消息发出
    void sendRequested(const QString &payload, const QString &display);
    void approvalDecided(const QString &callId, bool approved, const QVariantMap &args);
    // 开合状态变化（输入框需要隐藏/恢复，计划窗口需要避让）
    void openStateChanged(bool open);
    // 卡片高度变化（折叠/展开、换提问）：宿主重算计划窗口的避让距离
    void sizeChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct ParamEntry {
        QString name;
        QString type;
        QLineEdit *input = nullptr;
        QPlainTextEdit *area = nullptr;
    };
    struct OptionEntry {
        QWidget *item = nullptr;
        QVariantMap option;
        QString label;
        bool other = false;
    };

    void rebuild();
    void paintSelection(int index);
    void submit();
    void activate(int index);
    void setSubmitted(bool submitted);
    QVariantMap paramValues() const;
    // webui .choice-overlay 的入场/离场过渡（opacity .18s / 收起 .16s）
    void fadeIn();
    void fadeOut();

    QWidget *m_head = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_caret = nullptr;
    QPushButton *m_close = nullptr;
    QLabel *m_question = nullptr;
    QWidget *m_params = nullptr;
    QVBoxLayout *m_paramsLayout = nullptr;
    QWidget *m_list = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QWidget *m_foot = nullptr;
    QLineEdit *m_input = nullptr;
    QPushButton *m_submit = nullptr;

    QList<ParamEntry> m_paramEntries;
    QList<OptionEntry> m_optionEntries;
    bool m_isApproval = false;
    bool m_isToolParams = false;
    QString m_toolName;
    QString m_approvalCallId;
    QString m_sessionId;
    QVariantMap m_approvalEvent;
    int m_selected = -1;
    bool m_submitted = false;
    bool m_collapsed = false;
    bool m_open = false;
    QGraphicsOpacityEffect *m_opacity = nullptr;
    QPropertyAnimation *m_fade = nullptr;
};

} // namespace gs

#endif // GS_CHOICEOVERLAY_H
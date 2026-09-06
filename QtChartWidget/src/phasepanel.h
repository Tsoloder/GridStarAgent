#ifndef GS_PHASEPANEL_H
#define GS_PHASEPANEL_H

#include <QFrame>
#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QScrollArea;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

class ProgressLine;

// 单个阶段步骤（.phase-step）：全自绘 —— 状态圆圈字形、连接线、标题/备注省略
class PhaseStep : public QWidget
{
public:
    PhaseStep(const QString &title, const QString &note, const QString &status,
              QWidget *parent = nullptr);

    void setStatus(const QString &status);
    QString status() const { return m_status; }
    // CSS .phase-step:not(:last-child):after 跨行连接线：上一行画下半段，下一行画上半段
    void setConnectors(bool top, bool bottom);
    QSize sizeHint() const override { return QSize(220, 30); }
    QSize minimumSizeHint() const override { return QSize(60, 30); }

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;

private:
    QString m_title;
    QString m_note;
    QString m_status = QStringLiteral("pending");
    bool m_connectorTop = false;
    bool m_connectorBottom = false;
    bool m_hover = false;
};

// 阶段计划面板（.phase-panel）：标题 + completed/total 徽章 + 折叠箭头 + 底部渐变进度条
class PhasePanel : public QFrame
{
    Q_OBJECT
public:
    explicit PhasePanel(QWidget *parent = nullptr);

    // plan: { title, phases: [ { id, title, status, note, desc } ] }
    void setPlan(const QVariantMap &plan);
    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void layoutProgress();

    QWidget *m_head = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_count = nullptr;
    QLabel *m_chevron = nullptr;
    ProgressLine *m_progress = nullptr;
    QScrollArea *m_steps = nullptr;
    QWidget *m_inner = nullptr;
    QVBoxLayout *m_innerLayout = nullptr;
    bool m_expanded = false;
};

} // namespace gs

#endif // GS_PHASEPANEL_H

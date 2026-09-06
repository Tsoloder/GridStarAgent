#ifndef GS_POPUPS_H
#define GS_POPUPS_H

#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QScrollArea;
class QTimer;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

// app.js: modelKey / modelName / visibleModels
QString modelKey(const QVariantMap &item);
QString modelName(const QVariantMap &item);
QVariantList visibleModels(const QVariantList &models);

// 下拉列表中的一行（.model-option）：勾选位 + 名称 + ID
class ListOptionRow : public QWidget
{
    Q_OBJECT
public:
    ListOptionRow(const QString &value, const QString &name, const QString &sub,
                  QWidget *parent = nullptr);
    QString value() const { return m_value; }
    QString searchText() const { return m_search; }
    void setSelected(bool selected);
    void setFocusedRow(bool focused);

signals:
    void activated(const QString &value);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QString m_value;
    QString m_search;
    QString m_full;
    QLabel *m_check = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_id = nullptr;
};

// 模型 / Skill 下拉（.model-listbox）：Qt::Popup，在触发器上方弹出
class ListBoxPopup : public QFrame
{
    Q_OBJECT
public:
    explicit ListBoxPopup(QWidget *parent = nullptr);

    // 按 provider 分组渲染（renderModelList）
    void setModelOptions(const QVariantList &models, const QString &selectedKey);
    // 平铺渲染，首项固定为「无 Skill」（renderSkillList）
    void setSkillOptions(const QVariantList &skills, const QString &selectedId);
    void openAbove(QWidget *anchor);

signals:
    void chosen(const QString &value);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void reset();
    ListOptionRow *addRow(const QString &value, const QString &name, const QString &sub,
                          bool selected, const QString &tooltip = QString());
    void addGroupLabel(const QString &text);
    void addGroupSeparator();
    void moveFocus(int delta);
    void applyFocus();

    QScrollArea *m_scroll = nullptr;
    QWidget *m_inner = nullptr;
    QVBoxLayout *m_innerLayout = nullptr;
    QList<ListOptionRow *> m_rows;
    int m_focusIndex = 0;
    QString m_typeAhead;
    QTimer *m_typeTimer = nullptr;
};

// 会话历史面板（#session-panel）：宿主负责定位（areaFor）
class SessionPanel : public QFrame
{
    Q_OBJECT
public:
    explicit SessionPanel(QWidget *parent = nullptr);

    void setSessions(const QVariantList &sessions);
    QVariantList sessions() const { return m_sessions; }
    void open();
    void closePanel();
    bool isOpen() const;
    void layoutIn(const QSize &host);
    static QRect areaFor(const QSize &host);

signals:
    void sessionSelected(const QString &id);
    void sessionRenamed(const QString &id);
    void sessionCleared(const QString &id);
    void sessionDeleted(const QString &id);
    void closeRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void render();

    QLineEdit *m_search = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QVariantList m_sessions;
    bool m_open = false;
};

// Toast（#toast）：底部提示，5 秒自动隐藏
class Toast : public QLabel
{
    Q_OBJECT
public:
    explicit Toast(QWidget *parent = nullptr);
    void showMessage(const QString &text);
    void layoutIn(const QSize &host);

private:
    QTimer *m_timer = nullptr;
};

// 拖拽遮罩（#drop-overlay）
class DropOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit DropOverlay(QWidget *parent = nullptr);
    void setActive(bool active);
    void layoutIn(const QSize &host);
};

// 确认 / 输入对话框（app.js showDialog）
class ConfirmDialog : public QDialog
{
    Q_OBJECT
public:
    ConfirmDialog(const QString &title, const QString &message, const QString *input,
                  const QString &confirmText, const QString &cancelText, bool danger,
                  QWidget *parent = nullptr);
    QString textValue() const;

    static bool ask(QWidget *parent, const QString &title, const QString &message,
                    const QString &confirmText = QStringLiteral("确定"), bool danger = false);
    static QString prompt(QWidget *parent, const QString &title, const QString &initial,
                          const QString &confirmText = QStringLiteral("确定"));

private:
    QLineEdit *m_input = nullptr;
};

} // namespace gs

#endif // GS_POPUPS_H

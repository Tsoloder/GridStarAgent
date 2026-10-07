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

// app.js: modelKey / modelName / visibleModels / configuredModels
QString modelKey(const QVariantMap &item);
QString modelName(const QVariantMap &item);
QVariantList visibleModels(const QVariantList &models);
// 只列配置里写过的模型：供应商发现来的整份目录不进选择范围
QVariantList configuredModels(const QVariantList &models);

// 下拉列表中的一行（.model-option）：名称 + ID + 勾选位（webui 把勾从最左挪到了最右）
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
    // style.css @media(max-width:640px)：.model-option small{display:none}
    void setCompact(bool compact);

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
    bool m_compact = false;
};

// 模式下拉中的一行（.mode-option）：两行文本（标题 + 说明）+ 最右勾选位
class ModeOptionRow : public QWidget
{
    Q_OBJECT
public:
    ModeOptionRow(const QString &value, const QString &title, const QString &hint,
                  QWidget *parent = nullptr);
    QString value() const { return m_value; }
    void setSelected(bool selected);
    void setFocusedRow(bool focused);

signals:
    void activated(const QString &value);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QString m_value;
    QWidget *m_text = nullptr;
    QLabel *m_check = nullptr;
};

// 斜杠面板中的一行（.slash-item）：名称 + 代号 + 右侧说明（+ 模型行的勾选位）
class SlashRow : public QWidget
{
    Q_OBJECT
public:
    SlashRow(const QString &kind, const QString &id, const QString &name, const QString &code,
             const QString &desc, bool withCheck, QWidget *parent = nullptr);
    QString kind() const { return m_kind; }
    QString id() const { return m_id; }
    // 过滤命中判定：名称 / 代号 / id 任一包含
    bool matches(const QString &query) const;
    void setSelected(bool selected);
    void setFocusedRow(bool focused);

signals:
    void activated(const QString &kind, const QString &id);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QString m_kind;
    QString m_id;
    QString m_name;
    QString m_code;
    QString m_desc;
    QLabel *m_check = nullptr;
    QLabel *m_descLabel = nullptr;
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
    // 交互模式（renderModeList）：手动 / 自动两项，勾在选中项右侧
    void setModeOptions(const QString &selectedMode);
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
    QList<ModeOptionRow *> m_modeRows;
    // webui：.model-listbox 默认 max-width:330px，.skill-control .model-listbox 固定 250px
    int m_preferredWidth = 250;
    int m_focusIndex = 0;
    QString m_typeAhead;
    QTimer *m_typeTimer = nullptr;
};

// 斜杠面板（#slash-menu）：输入框里敲 "/" 唤起，浮在输入框上方。
// 根面板是指令 + 技能，选「模型」指令后切到可搜索的模型列表。
// 面板不入宿主布局，由 Composer 定位（与 ChoiceOverlay 同一套挂载方式）。
class SlashPanel : public QFrame
{
    Q_OBJECT
public:
    explicit SlashPanel(QWidget *parent = nullptr);

    // 根面板：指令在前（模型 / 导出对话），技能在后
    void setRootOptions(const QVariantList &skills, const QString &selectedSkill,
                        const QString &selectedModel);
    // 模型面板：顶部多一个过滤输入框
    void setModelOptions(const QVariantList &models, const QString &selectedModel);
    // 过滤词来自正文（根面板）或顶部输入框（模型面板）
    void setQuery(const QString &query);
    void focusSearch();
    // 退回根面板（收起时调用：下次敲 "/" 该看到技能与指令，而不是上次的模型列表）
    void resetToRoot();
    QString mode() const { return m_mode; }

    // 内容定稿高度（未含阴影），由 Composer 按可用空间截断
    int heightForContent(int width);

    // 键盘导航由 Composer 从输入框的 eventFilter 驱动（面板自身不抢焦点），
    // 所以这几个必须在公开面上：过滤后只走"可见行"，回车选中的也就是看得见的那条
    void moveFocus(int delta);
    bool chooseFocused();

signals:
    // 命中一行：kind = command | skill | model
    void itemChosen(const QString &kind, const QString &id);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct RowEntry {
        SlashRow *row = nullptr;
        QWidget *group = nullptr;
        QString kind;
        QString id;
        // setVisible 会因祖先未显示而为假，可见性自己记
        bool shown = true;
    };

    void clearRows();
    SlashRow *addRow(const QString &groupLabel, const QString &kind, const QString &id,
                     const QString &name, const QString &code, const QString &desc,
                     bool withCheck);
    QList<SlashRow *> visibleRows() const;
    void applyFilter();
    void focusRow(int index);
    SlashRow *focusedRow() const;

    QString m_mode = QStringLiteral("root");
    QString m_query;
    QLineEdit *m_search = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_inner = nullptr;
    QVBoxLayout *m_innerLayout = nullptr;
    QLabel *m_empty = nullptr;
    QList<SlashRow *> m_rows;
    QList<RowEntry> m_entries;
    int m_focusIndex = -1;
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
    // 打开状态下内容变化后按宿主尺寸重排（webui max-height 只是上限）
    void syncHostLayout();

    QWidget *m_head = nullptr;
    QWidget *m_list = nullptr;
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
    // bottomInset 为距底部的距离：宿主传输入区高度，让 Toast 始终浮在输入区之上
    void layoutIn(const QSize &host, int bottomInset = 125);
    int bottomInset() const { return m_bottomInset; }

private:
    QTimer *m_timer = nullptr;
    int m_bottomInset = 125;
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

// 皮肤下拉（.theme-listbox）：三种皮肤各一枚色片 + 名称，选中项高亮
class ThemeListPopup : public QFrame
{
    Q_OBJECT
public:
    explicit ThemeListPopup(QWidget *parent = nullptr);
    void setCurrent(const QString &id);
    void openBelow(QWidget *anchor);

signals:
    void themeChosen(const QString &id);

private:
    struct Entry { QString id; QPushButton *button = nullptr; };
    QList<Entry> m_entries;
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

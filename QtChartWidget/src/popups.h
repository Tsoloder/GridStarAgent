#ifndef GS_POPUPS_H
#define GS_POPUPS_H

#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QPaintEvent;
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
    // 平铺选项（专业分类 / 知识库）：rows 每项 {value, name, sub}，勾在选中项右侧。
    // preferredWidth 对应 webui .model-listbox 的 330px 上限，实际还会被宿主窗口宽度削
    void setPlainOptions(const QVariantList &rows, const QString &selectedValue,
                         int preferredWidth = 330);
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

// 上下文用量浮层里的进度条（.pop-bar）：轨道 + 百分比填充
class ContextBar : public QWidget
{
public:
    explicit ContextBar(QWidget *parent = nullptr);
    void setPercent(int percent);
    int percent() const { return m_percent; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int m_percent = 0;
};

// 上下文用量圆环（#context-ring）：webui 里是 22×22 的 SVG（viewBox 36、r=15.5、
// stroke 4、rotate(-90)），这里用 QPainter 等比复刻：底环 .context-ring-track +
// 进度弧 .context-ring-arc（圆头线帽、从 12 点方向顺时针）。
// 控件本体是 30×30 的悬停区（.context-ring 有 hover 底色与描边），环面按 22×22 居中。
class ContextRing : public QWidget
{
    Q_OBJECT
public:
    explicit ContextRing(QWidget *parent = nullptr);
    // usage: {max, used, left, percent, estimated, model}
    void setUsage(const QVariantMap &usage);
    void clearUsage();
    bool hasData() const { return m_ratio >= 0.0; }
    // 无数据时返回 -1（与 webui 的空态一致，浮层显示「—」）
    int percent() const { return m_percent; }
    // 环面边长（22px）：浮层与测试按它对齐
    static int ringBoxPx();

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QVariantMap m_usage;
    // < 0 表示无数据（空态只留底环）
    qreal m_ratio = -1.0;
    int m_percent = -1;
    bool m_hover = false;
};

// 上下文用量浮层（#context-pop）：标题「上下文用量 百分比」+ 三行数值 + 一条进度条 +
// 估算小注。由 Composer 定位（与 ChoiceOverlay 同一套挂载方式），
// 并且鼠标穿透（webui .context-pop{pointer-events:none}）。
class ContextPop : public QFrame
{
    Q_OBJECT
public:
    explicit ContextPop(QWidget *parent = nullptr);
    void setUsage(const QVariantMap &usage);
    // 按当前皮肤与界面缩放重设文字色与字号（配色取 gs::palette()，字号取 gs::scaledPx()）
    void applyTheme();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QLabel *m_pct = nullptr;
    QLabel *m_max = nullptr;
    QLabel *m_used = nullptr;
    QLabel *m_left = nullptr;
    QLabel *m_note = nullptr;
    ContextBar *m_bar = nullptr;
    QWidget *m_noteGap = nullptr;
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
    // 专业分类（webui slashGroups）：空串 = 「全部」，技能按各自 category 分组；
    // 非空时只留该分类 + common 的技能，并带上固定的「不启用技能」。
    // 需在 setRootOptions 之前设置；面板开着时改了要重新 setRootOptions。
    void setSkillCategory(const QString &category);
    QString skillCategory() const { return m_skillCategory; }
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
    // 当前专业分类（空串 = 全部）：决定技能行的分组
    QString m_skillCategory;
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

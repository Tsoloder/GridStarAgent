#ifndef GS_COMPOSER_H
#define GS_COMPOSER_H

#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QEvent;
class QLabel;
class QPushButton;
class QTextEdit;
class QHideEvent;
class QMoveEvent;
class QShowEvent;
class QTimer;
QT_END_NAMESPACE

namespace gs {

class AttachChip;
class ChoiceOverlay;
class ComboTrigger;
class ContextPop;
class ContextRing;
class FlowLayout;
class IconPushButton;
class ListBoxPopup;
class SlashPanel;

// 底部输入区（footer.composer）：输入框 + 附件条 + 控件行 + 选择浮层。
// 控件行用流式布局，窄屏自动换行、随内容增高。
class Composer : public QWidget
{
    Q_OBJECT
public:
    explicit Composer(QWidget *parent = nullptr);
    ~Composer() override;

    void setMode(const QString &mode);
    QString mode() const { return m_mode; }

    void setBusy(bool busy);
    bool isBusy() const { return m_busy; }
    void setConfigLoaded(bool loaded);
    void setConfigWarning(const QString &text);

    void setModels(const QVariantList &models);
    void setCurrentModel(const QString &key);
    QString currentModel() const { return m_model; }
    void setSkills(const QVariantList &skills);
    void setCurrentSkill(const QString &id);
    QString currentSkill() const { return m_skill; }

    // ---- 专业分类（webui #category-select）：首项「全部」由库自补（id 为空串） ----
    void setCategories(const QVariantList &categories);
    void setCurrentCategory(const QString &id);
    QString currentCategory() const { return m_category; }
    // 会话 meta 里的 category 回填（syncCategoryFromMeta）
    void syncCategoryFromSession(const QString &category);

    // ---- 知识库（webui #kb-select）：库自补「不使用知识库」/「全部知识库」两项 ----
    void setKnowledgeBases(const QVariantList &libraries);
    void setCurrentKnowledgeBase(const QString &id);
    QString currentKnowledgeBase() const { return m_kb; }

    // ---- 上下文用量圆环（webui #context-ring） ----
    // usage: {max, used, left, percent, estimated, model}
    void setContextUsage(const QVariantMap &usage);
    void clearContextUsage();

    void addAttachments(const QVariantList &items);
    void clearAttachments();
    QVariantList attachments() const { return m_attachments; }

    void setVoiceEnabled(bool enabled);
    void setVoiceRecording(bool recording);

    QString text() const;
    void setText(const QString &text);
    void focusInput();

    // ---- 选择浮层（模型提问 / 工具参数确认 / 审批） ----
    void showChoice(const QVariantMap &payload);
    void closeChoice();
    bool choiceOpen() const;
    // 浮层当前高度（0 表示未开），供计划窗口避让计算
    int choiceHeight() const;
    QString approvalCallId() const;
    void setApprovalResolved(const QString &callId, bool approved);
    void reEnableApproval(const QString &callId);
    // 主题缩放变了：芯片的文字宽度是实测出来的，得按新字号重算
    void refreshZoom();
    // 皮肤切换后：下拉浮层与上下文浮层挂在 Composer 的宿主上（是它的兄弟而非后代），
    // 全局 QSS 重建带不到它们，由 ChartWidget::applyTheme 显式调这里重烤
    void refreshTheme();
    // 斜杠面板是否展开；点面板外收起（由 ChartWidget 的全局事件过滤在按下时调用）
    bool slashOpen() const { return m_slashOpen; }
    void dismissSlashForClick(QObject *target);

signals:
    void sendMessage(const QString &text, const QString &display, const QVariantList &attachments);
    void stopRequested();
    void modeChanged(const QString &mode);
    void modelSelected(const QString &key);
    void skillSelected(const QString &id);
    void categorySelected(const QString &id);
    void knowledgeBaseSelected(const QString &id);
    void settingsRequested();
    void attachRequested();
    void voiceRequested();
    void attachmentRemoved(const QString &id);
    // 斜杠面板里的「导出对话」指令：宿主负责 POST /sessions/{id}/export
    void exportRequested();

    void optionChosen(const QString &value, const QString &label);
    void approvalDecided(const QString &callId, bool approved, const QVariantMap &args);
    // 浮层开合（输入框需要隐藏/恢复，计划窗口需要避让）
    void choiceOpenChanged(bool open);
    // 输入区想弹一条 Toast（如“上一轮还在收尾，略等片刻再选”）
    void toastRequested(const QString &text);
    // 浮层高度变化（折叠/展开、换提问）
    void choiceResized();

protected:
    void changeEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateSendState();
    void renderAttachments();
    // style.css @media(max-width:640px)：附件 chip 收窄（见 AttachChip::setCompact）
    bool compactAttachments() const;
    void applyAttachmentCompact();
    void layoutChoiceOverlay();
    void autoGrowInput();

    // ---- 控件行里的四个下拉（模式 / 模型 / 分类 / 知识库）共用一套开合与互斥 ----
    // webui 的四个下拉 + 斜杠面板同一时间只能有一个展开：open 前先关掉其余全部。
    enum class Dropdown { None = 0, Mode, Model, Category, KnowledgeBase };
    void openDropdown(Dropdown which);
    void closeDropdown(Dropdown which);
    bool dropdownOpen(Dropdown which) const;
    void closeAllDropdowns();
    ListBoxPopup *popupFor(Dropdown which) const;
    ComboTrigger *triggerFor(Dropdown which) const;
    QWidget *controlFor(Dropdown which) const;
    // 事件来自哪个下拉的控件/触发器（供悬停与键盘分支复用）
    Dropdown dropdownFor(const QObject *watched) const;

    void openModelList();
    void closeModelList();
    void openModeList();
    void closeModeList();
    bool modeListOpen() const;
    void openCategoryList();
    void closeCategoryList();
    bool categoryListOpen() const;
    void openKbList();
    void closeKbList();
    bool kbListOpen() const;
    // webui bindHoverDropdown：指针移入四个 .model-control（模式 / 模型 / 分类 / 知识库）
    // 任一控件即展开，移开「控件 ∪ 浮层」合计 150ms 后收起（HOVER_CLOSE_DELAY）。
    void hoverEnterControl(Dropdown which);
    void updateHoverPoll();
    void hoverPollTick();
    bool cursorIn(QWidget *widget) const;

    // 分类 / 知识库的目录与触发器文案（webui renderKbListbox / selectCategory）
    QVariantList categoryRows() const;
    QVariantList kbRows() const;
    QString categoryLabel(const QString &id) const;
    QString kbLabel(const QString &id) const;
    void selectCategory(const QString &id);
    void selectKnowledgeBase(const QString &id);
    // 分类变了且斜杠面板正开着：技能分组跟着重排（webui selectCategory → renderSlashMenu）
    void refreshSlashSkills();

    // ---- 上下文用量浮层（#context-pop）：悬停或焦点进入圆环时弹出 ----
    void showContextPop();
    void hideContextPop();
    void layoutContextPop();
    // 局部 QSS（分类/知识库浮层没有 #listbox 这个 objectName，得自己给样式）
    void applyLocalStyles();

    void selectMode(const QString &value);
    void renderSkillChip();
    // 技能芯片的最小宽要按名字实测，QPushButton 不吃子控件的宽度
    void updateSkillChipWidth();

    // ---- 斜杠面板（输入框敲 "/" 唤起，浮在输入框上方） ----
    void layoutSlashPanel();
    void syncSlashMenu();
    // 正文不以 "/" 开头时返回空串，否则返回去掉 "/" 的过滤词
    QString slashQuery() const;
    void openSlashRoot();
    void openSlashModelPicker();
    void closeSlashMenu();
    void runSlashItem(const QString &kind, const QString &id);
    void selectSkill(const QString &id);
    void finishSlashPick();

    bool inputBusy() const { return m_busy; }

    QString m_mode = QStringLiteral("manual");
    bool m_busy = false;
    bool m_configLoaded = false;
    int m_uploading = 0;
    QString m_model;
    QString m_skill;
    QVariantList m_models;
    QVariantList m_skills;
    QVariantList m_attachments;
    // 分类 / 知识库当前值与目录
    QString m_category;
    // webui kbState.selected 的默认值是 "*"（全部知识库）
    QString m_kb = QStringLiteral("*");
    QVariantList m_categories;
    QVariantList m_kbs;
    bool m_categoryTouched = false;
    QVariantMap m_contextUsage;

    QLabel *m_warning = nullptr;
    QWidget *m_warningGap = nullptr;
    QWidget *m_inputWrap = nullptr;
    QWidget *m_attachBar = nullptr;
    FlowLayout *m_attachLayout = nullptr;
    QTextEdit *m_input = nullptr;
    QWidget *m_controls = nullptr;
    QWidget *m_leftControls = nullptr;
    // webui：模式由分段按钮改成下拉（.model-control.mode-control）
    ComboTrigger *m_modeTrigger = nullptr;
    QWidget *m_modeControl = nullptr;
    ListBoxPopup *m_modeList = nullptr;
    ComboTrigger *m_modelTrigger = nullptr;
    QWidget *m_modelControl = nullptr;
    ListBoxPopup *m_modelList = nullptr;
    // webui #category-select / .model-control：专业分类（首项「全部」由库自补）
    ComboTrigger *m_categoryTrigger = nullptr;
    QWidget *m_categoryControl = nullptr;
    ListBoxPopup *m_categoryList = nullptr;
    // webui #kb-select / .kb-control：知识库（不使用 / 全部 + 各库）
    ComboTrigger *m_kbTrigger = nullptr;
    QWidget *m_kbControl = nullptr;
    ListBoxPopup *m_kbList = nullptr;
    // webui #context-ring / .context-pop：上下文用量圆环 + 悬浮浮层
    ContextRing *m_ring = nullptr;
    ContextPop *m_contextPop = nullptr;
    bool m_contextPopOpen = false;
    // webui：技能改成输入框左侧的小标签（#skill-chip），触发器与下拉已删除
    QPushButton *m_skillChip = nullptr;
    QLabel *m_skillChipName = nullptr;
    QLabel *m_skillChipClose = nullptr;
    SlashPanel *m_slash = nullptr;
    bool m_slashOpen = false;
    IconPushButton *m_attach = nullptr;
    IconPushButton *m_settings = nullptr;
    IconPushButton *m_voice = nullptr;
    IconPushButton *m_send = nullptr;
    QLabel *m_busyLabel = nullptr;
    ChoiceOverlay *m_choice = nullptr;
    bool m_choiceOpen = false;
    // 悬停开合：列表在光标下打开时才接管（键盘/程序化打开的不会被悬停逻辑收走），
    // 光标离开控件与浮层合计 150ms 就收起
    QTimer *m_hoverPoll = nullptr;
    int m_hoverOutside = 0;
    // 当前受悬停管辖的下拉（None = 轮询停着）：由下拉自己的打开方式决定
    Dropdown m_hoverManaged = Dropdown::None;
    // layoutChoiceOverlay() 量高时会改卡片几何 -> resizeEvent -> sizeChanged 回头调用本函数
    bool m_choiceLayingOut = false;
};

} // namespace gs

#endif // GS_COMPOSER_H

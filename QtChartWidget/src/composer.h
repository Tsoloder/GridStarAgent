#ifndef GS_COMPOSER_H
#define GS_COMPOSER_H

#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QTextEdit;
class QHideEvent;
class QMoveEvent;
class QShowEvent;
QT_END_NAMESPACE

namespace gs {

class AttachChip;
class ChoiceOverlay;
class ComboTrigger;
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

signals:
    void sendMessage(const QString &text, const QString &display, const QVariantList &attachments);
    void stopRequested();
    void modeChanged(const QString &mode);
    void modelSelected(const QString &key);
    void skillSelected(const QString &id);
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
    // 浮层高度变化（折叠/展开、换提问）
    void choiceResized();

protected:
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
    void openModelList();
    void closeModelList();
    void openModeList();
    void closeModeList();
    bool modeListOpen() const;
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
    // layoutChoiceOverlay() 量高时会改卡片几何 -> resizeEvent -> sizeChanged 回头调用本函数
    bool m_choiceLayingOut = false;
};

} // namespace gs

#endif // GS_COMPOSER_H

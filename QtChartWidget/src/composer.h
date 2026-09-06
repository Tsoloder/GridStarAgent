#ifndef GS_COMPOSER_H
#define GS_COMPOSER_H

#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QTextEdit;
QT_END_NAMESPACE

namespace gs {

class AttachChip;
class ComboTrigger;
class FlowLayout;
class IconPushButton;
class ListBoxPopup;

// 底部输入区（footer.composer）：模式切换、模型/Skill 下拉、附件条、输入框与发送按钮
class Composer : public QWidget
{
    Q_OBJECT
public:
    explicit Composer(QWidget *parent = nullptr);

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

signals:
    void sendMessage(const QString &text, const QVariantList &attachments);
    void stopRequested();
    void modeChanged(const QString &mode);
    void modelSelected(const QString &key);
    void skillSelected(const QString &id);
    void settingsRequested();
    void attachRequested();
    void voiceRequested();
    void attachmentRemoved(const QString &id);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateSendState();
    void renderAttachments();
    void layoutInputButtons();
    void openModelList();
    void openSkillList();

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
    QPushButton *m_manual = nullptr;
    QPushButton *m_auto = nullptr;
    ComboTrigger *m_modelTrigger = nullptr;
    ComboTrigger *m_skillTrigger = nullptr;
    ListBoxPopup *m_modelList = nullptr;
    ListBoxPopup *m_skillList = nullptr;
    IconPushButton *m_settings = nullptr;
    QWidget *m_attachBar = nullptr;
    FlowLayout *m_attachLayout = nullptr;
    QWidget *m_inputWrap = nullptr;
    QTextEdit *m_input = nullptr;
    QPushButton *m_attach = nullptr;
    QPushButton *m_voice = nullptr;
    IconPushButton *m_send = nullptr;
    QLabel *m_busyLabel = nullptr;
};

} // namespace gs

#endif // GS_COMPOSER_H

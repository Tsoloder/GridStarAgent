#ifndef GS_SETTINGSDIALOG_H
#define GS_SETTINGSDIALOG_H

#include <QDialog>
#include <QFrame>
#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

// HTML <details> 的等价物：点击 summary 展开 / 收起 body
class ExpandCard : public QFrame
{
    Q_OBJECT
public:
    ExpandCard(const QString &frameClass, const QString &summaryClass,
               const QString &bodyClass, QWidget *parent = nullptr);

    QWidget *summary() const { return m_summary; }
    QWidget *body() const { return m_body; }
    QVBoxLayout *summaryLayout() const { return m_summaryLayout; }
    QVBoxLayout *bodyLayout() const { return m_bodyLayout; }
    void setOpen(bool open);
    bool isOpen() const { return m_open; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *m_summary = nullptr;
    QVBoxLayout *m_summaryLayout = nullptr;
    QWidget *m_body = nullptr;
    QVBoxLayout *m_bodyLayout = nullptr;
    bool m_open = false;
};

// 设置中心（#settings-modal）：模型 / 技能 / MCP 工具 三个 Tab
// 对应 app.js 的 renderSettings / renderProviderEditor / renderModelCard /
// renderCandidates / renderSkills / renderMcpTools / validateSettings / saveSettings
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    // openSettings()：把配置副本作为草稿载入
    void loadConfig(const QVariantMap &config, const QVariant &revision);
    QVariantMap draft() const;
    QVariant revision() const { return m_revision; }
    bool isDirty() const { return m_dirty; }

    // readProviderModels() 的结果：providerId -> 候选模型列表
    void setDiscoveredModels(const QString &providerId, const QVariantList &models);
    // testProvider() / readProviderModels() 进行中的按钮文案
    void setProviderBusy(bool testing, bool reading);

    void setSkills(const QVariantList &skills, bool loading, const QString &error);
    void setMcpTools(const QVariantList &tools, bool connected, bool loading,
                     const QString &error);

    void switchTab(const QString &tab);
    QString activeTab() const { return m_tab; }
    void setStatus(const QString &text);
    // saveSettings() 成功后由宿主调用
    void finishSaved();

signals:
    void testProviderRequested(const QString &providerId);
    void readModelsRequested(const QString &providerId);
    void refreshSkillsRequested();
    void refreshMcpRequested();
    void saveRequested(const QVariantMap &config, const QVariant &revision);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    QWidget *buildModelsPage();
    QWidget *buildProviderSidebar();
    QWidget *buildProviderEditor();
    QWidget *buildSkillsPage();
    QWidget *buildMcpPage();

    void renderSettings();
    void renderProviderList();
    void renderProviderEditor();
    void renderModelCard(const QVariantMap &model);
    void renderCandidates();
    void renderSkills();
    void renderMcpTools();
    void markDirty();

    int providerIndex(const QString &id) const;
    int modelIndex(const QString &key) const;
    QVariantList providerModels(const QString &id) const;
    void addProvider();
    void deleteProvider();
    void clearApiKey();
    void addModel(const QString &rawId, const QString &name);
    bool validateSettings();
    void attemptClose();

    // 草稿状态（state.settings）
    QVariantMap m_extra;             // version 等其余字段
    QVariantList m_providers;
    QVariantList m_models;
    QString m_defaultModel;
    QVariant m_revision;
    QString m_activeProviderId;
    bool m_dirty = false;
    QString m_tab = QStringLiteral("models");

    QVariantMap m_discovered;
    bool m_testing = false;
    bool m_reading = false;

    QVariantList m_skills;
    bool m_skillsLoading = false;
    QString m_skillsError;
    QVariantList m_tools;
    bool m_toolsConnected = false;
    bool m_toolsLoading = false;
    QString m_toolsError;

    bool m_updating = false;         // 回填控件时抑制信号

    // 框架
    QStackedWidget *m_stack = nullptr;
    QPushButton *m_tabModels = nullptr;
    QPushButton *m_tabSkills = nullptr;
    QPushButton *m_tabMcp = nullptr;
    QPushButton *m_saveButton = nullptr;
    QLabel *m_status = nullptr;

    // 供应商侧栏
    QVBoxLayout *m_providerNavLayout = nullptr;

    // 供应商编辑器
    QWidget *m_placeholder = nullptr;
    QFrame *m_sectionProvider = nullptr;
    QFrame *m_sectionModels = nullptr;
    QLabel *m_editorEyebrow = nullptr;
    QLabel *m_editorTitle = nullptr;
    QCheckBox *m_enabledCheck = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_idEdit = nullptr;
    QComboBox *m_discoveryCombo = nullptr;
    QLineEdit *m_baseUrlEdit = nullptr;
    QLineEdit *m_keyEnvEdit = nullptr;
    QLineEdit *m_apiKeyEdit = nullptr;
    QComboBox *m_defaultApiCombo = nullptr;
    QPushButton *m_clearKeyButton = nullptr;
    QPushButton *m_testButton = nullptr;
    QLabel *m_inlineResult = nullptr;
    QPushButton *m_deleteProviderButton = nullptr;
    QLabel *m_modelsTitle = nullptr;
    QLabel *m_modelsCount = nullptr;
    QPushButton *m_readButton = nullptr;
    QVBoxLayout *m_modelListLayout = nullptr;
    QLineEdit *m_candidateSearch = nullptr;
    QVBoxLayout *m_candidateLayout = nullptr;
    QLineEdit *m_manualId = nullptr;
    QPushButton *m_addManualButton = nullptr;

    // 技能 / MCP
    QLabel *m_skillCount = nullptr;
    QLabel *m_skillsStatus = nullptr;
    QVBoxLayout *m_skillsLayout = nullptr;
    QPushButton *m_refreshSkills = nullptr;
    QLabel *m_mcpCount = nullptr;
    QLabel *m_mcpStatus = nullptr;
    QVBoxLayout *m_mcpLayout = nullptr;
    QPushButton *m_refreshMcp = nullptr;
};

} // namespace gs

#endif // GS_SETTINGSDIALOG_H

#ifndef GS_SETTINGSDIALOG_H
#define GS_SETTINGSDIALOG_H

#include <QDialog>
#include <QFrame>
#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QBoxLayout;
class QCheckBox;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QResizeEvent;
class QSpacerItem;
class QStackedWidget;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

class UsagePanel;

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

// 设置中心（#settings-modal）：模型 / 技能 / MCP 工具 / 用量 四个 Tab
// 对应 app.js 的 renderSettings / renderProviderEditor / renderModelCard /
// renderCandidates / renderSkills / renderMcpTools / validateSettings / saveSettings /
// renderUsagePanel
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

    // 用量页：配置目录（显示名映射）与后端回填
    void setUsageCatalog(const QVariantList &models);
    void setUsageStats(const QString &requestId, const QVariantMap &data);
    void setUsageLoadFailed(const QString &requestId, const QString &error);

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
    void usageStatsRequested(const QString &requestId, const QString &start, const QString &end,
                             const QString &provider, const QString &model);

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // Esc 走 reject()，脏检查要在两条路径上都生效
    void reject() override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    // style.css @media(max-width:640px)：设置窗口转紧凑布局
    void applyCompactMode(bool compact);
    QWidget *buildModelsPage();
    QWidget *buildProviderSidebar();
    QWidget *buildProviderEditor();
    QWidget *buildSkillsPage();
    QWidget *buildMcpPage();
    QWidget *buildUsagePage();

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
    // 未保存修改的二次确认（关闭按钮 / 窗口关闭 / Esc 共用）
    bool confirmDiscard();

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
    QHBoxLayout *m_headLayout = nullptr;
    QHBoxLayout *m_tabsLayout = nullptr;
    QSpacerItem *m_tabsSpacer = nullptr;
    QHBoxLayout *m_actionsLayout = nullptr;
    QPushButton *m_tabModels = nullptr;
    QPushButton *m_tabSkills = nullptr;
    QPushButton *m_tabMcp = nullptr;
    QPushButton *m_tabUsage = nullptr;
    QPushButton *m_saveButton = nullptr;
    QLabel *m_status = nullptr;

    // 供应商侧栏
    QWidget *m_providerSidebar = nullptr;
    QVBoxLayout *m_providerSidebarLayout = nullptr;
    QVBoxLayout *m_providerNavLayout = nullptr;

    // 供应商编辑器
    QWidget *m_providerEditor = nullptr;
    QVBoxLayout *m_providerEditorLayout = nullptr;
    QGridLayout *m_providerGrid = nullptr;
    QList<QWidget *> m_providerFields;
    // .form-grid：每个模型卡片一套，切紧凑模式时重排为单列
    QList<QGridLayout *> m_modelGrids;
    QList<QList<QWidget *>> m_modelGridFields;
    QBoxLayout *m_providerActions = nullptr;
    QSpacerItem *m_providerActionsSpacer = nullptr;
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
    // SSL 证书验证：默认（系统证书）/ 跳过验证 / 自定义 CA 证书路径。
    // 自定义时那个路径框才现身，值直接写进 provider 的 ssl_verify（字符串即路径）
    QComboBox *m_sslCombo = nullptr;
    QWidget *m_sslPathRow = nullptr;
    QLineEdit *m_sslPathEdit = nullptr;
    // 下拉切回「自定义」时要还回上次填的路径，provider 里此刻只存着 true/false
    QString m_sslPathMemory;
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
    QVBoxLayout *m_skillsPageLayout = nullptr;
    QVBoxLayout *m_mcpPageLayout = nullptr;
    QLabel *m_skillCount = nullptr;
    QLabel *m_skillsStatus = nullptr;
    QVBoxLayout *m_skillsLayout = nullptr;
    QPushButton *m_refreshSkills = nullptr;
    QLabel *m_mcpCount = nullptr;
    QLabel *m_mcpStatus = nullptr;
    QVBoxLayout *m_mcpLayout = nullptr;
    QPushButton *m_refreshMcp = nullptr;

    // 用量页
    UsagePanel *m_usagePanel = nullptr;

    bool m_compact = false;
};

} // namespace gs

#endif // GS_SETTINGSDIALOG_H

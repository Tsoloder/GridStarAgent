#include "settingsdialog.h"

#include "commonwidgets.h"
#include "popups.h"
#include "theme.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace gs {

namespace {

const char *const kCapabilities[5][2] = {
    {"tools", "Tools"},
    {"parallel_tools", "Parallel tools"},
    {"reasoning", "Reasoning"},
    {"vision", "Vision"},
    {"stream_usage", "Stream usage"},
};

QWidget *styledWidget(const QString &cls, QWidget *parent = nullptr)
{
    auto *w = new QWidget(parent);
    if (!cls.isEmpty())
        setClass(w, cls);
    w->setAttribute(Qt::WA_StyledBackground, true);
    return w;
}

QFrame *styledFrame(const QString &cls, QWidget *parent = nullptr)
{
    auto *f = new QFrame(parent);
    if (!cls.isEmpty())
        setClass(f, cls);
    f->setAttribute(Qt::WA_StyledBackground, true);
    f->setFrameShape(QFrame::NoFrame);
    return f;
}

QPushButton *styledButton(const QString &text, const QString &variant, QWidget *parent = nullptr)
{
    auto *b = new QPushButton(text, parent);
    if (!variant.isEmpty())
        b->setProperty("variant", variant);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

// makeLabel 默认允许选中文本，会吃掉点击；可点击行内的标签要关掉
QLabel *plainLabel(const QString &cls, const QString &text, QWidget *parent = nullptr)
{
    QLabel *l = makeLabel(cls, text, parent);
    l->setTextInteractionFlags(Qt::NoTextInteraction);
    return l;
}

QLineEdit *settingsInput(const QString &placeholder)
{
    auto *e = new QLineEdit;
    setClass(e, QStringLiteral("settingsInput"));
    e->setPlaceholderText(placeholder);
    e->setFixedHeight(32);
    return e;
}

QComboBox *settingsCombo()
{
    auto *c = new QComboBox;
    setClass(c, QStringLiteral("settingsInput"));
    c->setFixedHeight(32);
    c->setFocusPolicy(Qt::NoFocus);
    return c;
}

void selectComboData(QComboBox *combo, const QString &value)
{
    const int index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

QString comboData(QComboBox *combo)
{
    return combo->currentData().toString();
}

// .settings-field：小标题在上、输入控件在下
QWidget *fieldRow(const QString &caption, QWidget *input)
{
    auto *row = new QWidget;
    auto *l = new QVBoxLayout(row);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(5);
    l->addWidget(plainLabel(QStringLiteral("fieldCaption"), caption));
    l->addWidget(input);
    return row;
}

QScrollArea *plainScroll(QWidget *content, const QString &objectName = QString(),
                         const QString &cls = QString())
{
    auto *area = new QScrollArea;
    if (!objectName.isEmpty())
        area->setObjectName(objectName);
    if (!cls.isEmpty())
        setClass(area, cls);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidgetResizable(true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->viewport()->setAutoFillBackground(false);
    area->setWidget(content);
    return area;
}

void clearLayout(QLayout *layout)
{
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        else if (QLayout *child = item->layout()) {
            clearLayout(child);
            delete child;
        }
        delete item;
    }
}

// app.js schemaParams()：从 JSON Schema 抽取入参摘要
QVariantList schemaParams(const QVariantMap &schema)
{
    QVariantList out;
    const QVariantMap props = schema.value(QStringLiteral("properties")).toMap();
    const QVariantList required = schema.value(QStringLiteral("required")).toList();
    const QStringList names = props.keys();
    for (const QString &name : names) {
        const QVariantMap p = props.value(name).toMap();
        QString type = p.value(QStringLiteral("type")).toString();
        if (type.isEmpty())
            type = QStringLiteral("any");
        QVariantMap item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("required"), required.contains(QVariant(name)));
        item.insert(QStringLiteral("description"),
                    p.value(QStringLiteral("description")).toString());
        out.append(item);
    }
    return out;
}

QLabel *emptyHint(const QString &text)
{
    QLabel *label = makeLabel(QStringLiteral("listboxEmpty"), text);
    label->setAlignment(Qt::AlignCenter);
    return label;
}

} // namespace

// ------------------------------------------------------------ ExpandCard

ExpandCard::ExpandCard(const QString &frameClass, const QString &summaryClass,
                       const QString &bodyClass, QWidget *parent)
    : QFrame(parent)
{
    setClass(this, frameClass);
    setAttribute(Qt::WA_StyledBackground, true);
    setFrameShape(QFrame::NoFrame);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_summary = new QWidget(this);
    setClass(m_summary, summaryClass);
    m_summary->setAttribute(Qt::WA_StyledBackground, true);
    m_summary->setCursor(Qt::PointingHandCursor);
    m_summaryLayout = new QVBoxLayout(m_summary);
    m_summaryLayout->setContentsMargins(0, 0, 0, 0);
    m_summaryLayout->setSpacing(0);
    m_summary->installEventFilter(this);
    layout->addWidget(m_summary);

    m_body = new QWidget(this);
    if (!bodyClass.isEmpty()) {
        setClass(m_body, bodyClass);
        m_body->setAttribute(Qt::WA_StyledBackground, true);
    }
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(0, 0, 0, 0);
    m_bodyLayout->setSpacing(0);
    m_body->setVisible(false);
    layout->addWidget(m_body);
}

void ExpandCard::setOpen(bool open)
{
    if (m_open == open)
        return;
    m_open = open;
    m_body->setVisible(open);
}

bool ExpandCard::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_summary && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (m_summary->rect().contains(me->pos()))
            setOpen(!m_open);
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

// -------------------------------------------------------- SettingsDialog

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("settingsDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    resize(920, 720);
    setMinimumSize(680, 460);

    buildUi();
    switchTab(QStringLiteral("models"));
    renderSettings();
}

void SettingsDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- .settings-head ----
    auto *head = new QWidget(this);
    head->setObjectName(QStringLiteral("settingsHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(18, 16, 18, 16);
    hl->setSpacing(0);
    auto *titleBox = new QVBoxLayout;
    titleBox->setContentsMargins(0, 0, 0, 0);
    titleBox->setSpacing(2);
    titleBox->addWidget(plainLabel(QStringLiteral("eyebrow"), QStringLiteral("CONFIGURATION")));
    auto *title = plainLabel(QString(), QStringLiteral("设置中心"));
    title->setObjectName(QStringLiteral("settingsTitle"));
    titleBox->addWidget(title);
    hl->addLayout(titleBox, 1);
    auto *closeButton = new IconPushButton(this);
    closeButton->setProperty("variant", QStringLiteral("icon"));
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setFocusPolicy(Qt::NoFocus);
    closeButton->setFixedSize(30, 30);
    closeButton->setIconColors(QColor(QStringLiteral("#d7e2e8")),
                               QColor(QStringLiteral("#50badf")));
    closeButton->setIconName(QStringLiteral("x"), 14);
    hl->addWidget(closeButton, 0, Qt::AlignVCenter);
    root->addWidget(head);
    connect(closeButton, &QPushButton::clicked, this, &SettingsDialog::attemptClose);

    // ---- .settings-tabs ----
    auto *tabs = new QWidget(this);
    tabs->setObjectName(QStringLiteral("settingsTabs"));
    tabs->setAttribute(Qt::WA_StyledBackground, true);
    tabs->setFixedHeight(42);
    auto *tl = new QHBoxLayout(tabs);
    tl->setContentsMargins(18, 0, 18, 0);
    tl->setSpacing(0);
    const QString tabNames[3] = {QStringLiteral("模型"), QStringLiteral("技能"),
                                 QStringLiteral("MCP 工具")};
    QPushButton **tabPtrs[3] = {&m_tabModels, &m_tabSkills, &m_tabMcp};
    for (int i = 0; i < 3; ++i) {
        auto *button = new QPushButton(tabNames[i], tabs);
        setClass(button, QStringLiteral("settingsTab"));
        button->setFixedHeight(42);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        *tabPtrs[i] = button;
        tl->addWidget(button);
    }
    tl->addStretch(1);
    root->addWidget(tabs);
    connect(m_tabModels, &QPushButton::clicked, this,
            [this] { switchTab(QStringLiteral("models")); });
    connect(m_tabSkills, &QPushButton::clicked, this,
            [this] { switchTab(QStringLiteral("skills")); });
    connect(m_tabMcp, &QPushButton::clicked, this,
            [this] { switchTab(QStringLiteral("mcp")); });

    // ---- .settings-content ----
    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("settingsContent"));
    m_stack->addWidget(buildModelsPage());
    m_stack->addWidget(buildSkillsPage());
    m_stack->addWidget(buildMcpPage());
    root->addWidget(m_stack, 1);

    // ---- .settings-actions ----
    auto *actions = new QWidget(this);
    actions->setObjectName(QStringLiteral("settingsActions"));
    actions->setAttribute(Qt::WA_StyledBackground, true);
    actions->setMinimumHeight(56);
    auto *al = new QHBoxLayout(actions);
    al->setContentsMargins(18, 10, 18, 10);
    al->setSpacing(8);
    m_status = plainLabel(QString(), QString());
    m_status->setObjectName(QStringLiteral("settingsStatus"));
    al->addWidget(m_status, 1);
    auto *cancel = styledButton(QStringLiteral("取消"), QStringLiteral("secondary"));
    cancel->setMinimumWidth(88);
    cancel->setFixedHeight(32);
    m_saveButton = styledButton(QStringLiteral("保存设置"), QStringLiteral("primary"));
    m_saveButton->setMinimumWidth(88);
    m_saveButton->setFixedHeight(32);
    al->addWidget(cancel);
    al->addWidget(m_saveButton);
    root->addWidget(actions);
    connect(cancel, &QPushButton::clicked, this, &SettingsDialog::attemptClose);
    connect(m_saveButton, &QPushButton::clicked, this, [this] {
        if (!validateSettings())
            return;
        m_saveButton->setEnabled(false);
        emit saveRequested(draft(), m_revision);
    });
}

QWidget *SettingsDialog::buildModelsPage()
{
    auto *page = new QWidget;
    auto *l = new QHBoxLayout(page);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    l->addWidget(buildProviderSidebar());
    l->addWidget(buildProviderEditor(), 1);
    return page;
}

QWidget *SettingsDialog::buildProviderSidebar()
{
    auto *side = new QWidget;
    side->setObjectName(QStringLiteral("providerSidebar"));
    side->setAttribute(Qt::WA_StyledBackground, true);
    side->setFixedWidth(210);
    auto *l = new QVBoxLayout(side);
    l->setContentsMargins(10, 15, 10, 15);
    l->setSpacing(0);
    l->addWidget(plainLabel(QStringLiteral("sectionLabel"), QStringLiteral("供应商")));

    auto *nav = new QWidget;
    nav->setObjectName(QStringLiteral("providerNav"));
    nav->setAttribute(Qt::WA_StyledBackground, true);
    m_providerNavLayout = new QVBoxLayout(nav);
    m_providerNavLayout->setContentsMargins(0, 0, 0, 0);
    m_providerNavLayout->setSpacing(0);
    m_providerNavLayout->addStretch(1);
    l->addWidget(plainScroll(nav, QStringLiteral("providerNavScroll")), 1);

    auto *add = new IconPushButton(side);
    add->setObjectName(QStringLiteral("addProvider"));
    add->setMinimumHeight(28);
    add->setCursor(Qt::PointingHandCursor);
    add->setFocusPolicy(Qt::NoFocus);
    add->setText(QStringLiteral("添加供应商"));
    add->setIconColors(QColor(QStringLiteral("#d7e2e8")),
                       QColor(QStringLiteral("#d7e2e8")));
    add->setIconName(QStringLiteral("plus"), 12);
    l->addSpacing(10);
    l->addWidget(add);
    connect(add, &QPushButton::clicked, this, &SettingsDialog::addProvider);
    return side;
}

QWidget *SettingsDialog::buildProviderEditor()
{
    auto *editor = new QWidget;
    editor->setObjectName(QStringLiteral("providerEditor"));
    editor->setAttribute(Qt::WA_StyledBackground, true);
    auto *el = new QVBoxLayout(editor);
    el->setContentsMargins(18, 18, 18, 18);
    el->setSpacing(14);

    // .placeholder-panel
    m_placeholder = new QWidget(editor);
    auto *pl = new QVBoxLayout(m_placeholder);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(0);
    pl->addStretch(1);
    pl->addWidget(plainLabel(QStringLiteral("placeholderTitle"),
                             QStringLiteral("添加供应商以开始配置")),
                  0, Qt::AlignHCenter);
    pl->addStretch(1);
    el->addWidget(m_placeholder, 1);

    // ===== provider-section：PROVIDER =====
    m_sectionProvider = styledFrame(QStringLiteral("providerSection"));
    auto *sl = new QVBoxLayout(m_sectionProvider);
    sl->setContentsMargins(15, 15, 15, 15);
    sl->setSpacing(0);

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(12);
    auto *titleBox = new QVBoxLayout;
    titleBox->setContentsMargins(0, 0, 0, 0);
    titleBox->setSpacing(2);
    m_editorEyebrow = plainLabel(QStringLiteral("eyebrow"), QStringLiteral("PROVIDER"));
    m_editorTitle = plainLabel(QStringLiteral("sectionTitle"), QString());
    titleBox->addWidget(m_editorEyebrow);
    titleBox->addWidget(m_editorTitle);
    titleRow->addLayout(titleBox, 1);
    m_enabledCheck = new QCheckBox(QStringLiteral("启用"), m_sectionProvider);
    setClass(m_enabledCheck, QStringLiteral("settingsCheck"));
    m_enabledCheck->setCursor(Qt::PointingHandCursor);
    titleRow->addWidget(m_enabledCheck, 0, Qt::AlignVCenter);
    sl->addLayout(titleRow);
    sl->addSpacing(14);

    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    m_nameEdit = settingsInput(QString());
    m_idEdit = settingsInput(QString());
    m_idEdit->setEnabled(false);
    m_discoveryCombo = settingsCombo();
    m_discoveryCombo->addItem(QStringLiteral("OpenAI / Compatible / Ollama"),
                              QStringLiteral("openai"));
    m_discoveryCombo->addItem(QStringLiteral("Anthropic"), QStringLiteral("anthropic"));
    m_discoveryCombo->addItem(QStringLiteral("不支持模型发现"), QStringLiteral("none"));
    m_baseUrlEdit = settingsInput(QString());
    m_keyEnvEdit = settingsInput(QString());
    m_apiKeyEdit = settingsInput(QStringLiteral("输入 API Key"));
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_defaultApiCombo = settingsCombo();
    m_defaultApiCombo->addItem(QStringLiteral("OpenAI Chat"), QStringLiteral("openai-chat"));
    m_defaultApiCombo->addItem(QStringLiteral("OpenAI Responses"),
                               QStringLiteral("openai-responses"));
    m_defaultApiCombo->addItem(QStringLiteral("Anthropic Messages"),
                               QStringLiteral("anthropic-messages"));
    grid->addWidget(fieldRow(QStringLiteral("供应商名称"), m_nameEdit), 0, 0);
    grid->addWidget(fieldRow(QStringLiteral("供应商 ID"), m_idEdit), 0, 1);
    grid->addWidget(fieldRow(QStringLiteral("供应商类型"), m_discoveryCombo), 1, 0);
    grid->addWidget(fieldRow(QStringLiteral("API 地址"), m_baseUrlEdit), 1, 1);
    grid->addWidget(fieldRow(QStringLiteral("API Key 环境变量"), m_keyEnvEdit), 2, 0);
    grid->addWidget(fieldRow(QStringLiteral("API Key"), m_apiKeyEdit), 2, 1);
    grid->addWidget(fieldRow(QStringLiteral("默认 API 协议"), m_defaultApiCombo), 3, 0);
    sl->addLayout(grid);

    auto *acts = new QHBoxLayout;
    acts->setContentsMargins(0, 0, 0, 0);
    acts->setSpacing(7);
    m_clearKeyButton = styledButton(QStringLiteral("清除 Key"), QStringLiteral("secondary"));
    m_testButton = styledButton(QStringLiteral("测试连接"), QStringLiteral("secondary"));
    m_inlineResult = plainLabel(QStringLiteral("inlineResult"), QString());
    m_deleteProviderButton =
        styledButton(QStringLiteral("删除供应商"), QStringLiteral("danger"));
    acts->addWidget(m_clearKeyButton);
    acts->addWidget(m_testButton);
    acts->addWidget(m_inlineResult, 0, Qt::AlignVCenter);
    acts->addStretch(1);
    acts->addWidget(m_deleteProviderButton);
    sl->addSpacing(14);
    sl->addLayout(acts);
    el->addWidget(m_sectionProvider);

    connect(m_enabledCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("enabled"), on);
        m_providers[index] = provider;
        markDirty();
    });
    connect(m_nameEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("name"), text);
        m_providers[index] = provider;
        markDirty();
        m_editorTitle->setText(text);
        renderProviderList();
    });
    connect(m_discoveryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("discovery_api"), comboData(m_discoveryCombo));
        m_providers[index] = provider;
        markDirty();
    });
    connect(m_baseUrlEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("base_url"), text);
        m_providers[index] = provider;
        markDirty();
    });
    connect(m_keyEnvEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("api_key_env"), text);
        m_providers[index] = provider;
        markDirty();
        m_apiKeyEdit->setEnabled(text.trimmed().isEmpty());
    });
    connect(m_apiKeyEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        // app.js：只有输入了内容、或原本不是掩码值时才回写
        if (text.isEmpty() && provider.value(QStringLiteral("api_key")).toString()
                == QLatin1String("********"))
            return;
        provider.insert(QStringLiteral("api_key"), text);
        m_providers[index] = provider;
        markDirty();
    });
    connect(m_defaultApiCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("default_api"), comboData(m_defaultApiCombo));
        m_providers[index] = provider;
        markDirty();
    });
    connect(m_clearKeyButton, &QPushButton::clicked, this, &SettingsDialog::clearApiKey);
    connect(m_deleteProviderButton, &QPushButton::clicked, this,
            &SettingsDialog::deleteProvider);
    connect(m_testButton, &QPushButton::clicked, this, [this] {
        if (m_activeProviderId.isEmpty())
            return;
        m_testing = true;
        renderProviderEditor();
        emit testProviderRequested(m_activeProviderId);
    });

    // ===== provider-section：MODELS =====
    m_sectionModels = styledFrame(QStringLiteral("providerSection"));
    auto *ml = new QVBoxLayout(m_sectionModels);
    ml->setContentsMargins(15, 15, 15, 15);
    ml->setSpacing(0);

    auto *headRow = new QHBoxLayout;
    headRow->setContentsMargins(0, 0, 0, 0);
    headRow->setSpacing(12);
    auto *headBox = new QVBoxLayout;
    headBox->setContentsMargins(0, 0, 0, 0);
    headBox->setSpacing(2);
    headBox->addWidget(plainLabel(QStringLiteral("eyebrow"), QStringLiteral("MODELS")));
    auto *titleLine = new QHBoxLayout;
    titleLine->setContentsMargins(0, 0, 0, 0);
    titleLine->setSpacing(6);
    m_modelsTitle = plainLabel(QStringLiteral("sectionTitle"), QStringLiteral("已添加模型"));
    m_modelsCount = plainLabel(QStringLiteral("sectionTitleSmall"), QStringLiteral("0"));
    titleLine->addWidget(m_modelsTitle);
    titleLine->addWidget(m_modelsCount, 0, Qt::AlignBottom);
    titleLine->addStretch(1);
    headBox->addLayout(titleLine);
    headRow->addLayout(headBox, 1);
    m_readButton = styledButton(QStringLiteral("读取模型"), QStringLiteral("secondary"));
    headRow->addWidget(m_readButton, 0, Qt::AlignVCenter);
    ml->addLayout(headRow);
    ml->addSpacing(14);
    connect(m_readButton, &QPushButton::clicked, this, [this] {
        if (m_activeProviderId.isEmpty())
            return;
        m_reading = true;
        renderProviderEditor();
        emit readModelsRequested(m_activeProviderId);
    });

    auto *listHost = new QWidget(m_sectionModels);
    m_modelListLayout = new QVBoxLayout(listHost);
    m_modelListLayout->setContentsMargins(0, 0, 0, 0);
    m_modelListLayout->setSpacing(7);
    ml->addWidget(listHost);

    // .add-model
    auto *addBox = styledFrame(QString());
    addBox->setObjectName(QStringLiteral("addModelBox"));
    auto *abl = new QVBoxLayout(addBox);
    abl->setContentsMargins(12, 12, 12, 12);
    abl->setSpacing(0);
    abl->addWidget(plainLabel(QStringLiteral("fieldCaption"),
                              QStringLiteral("添加模型 · 搜索候选")));
    abl->addSpacing(5);
    m_candidateSearch = settingsInput(QStringLiteral("搜索模型 ID"));
    abl->addWidget(m_candidateSearch);
    auto *candidates = new QWidget;
    candidates->setObjectName(QStringLiteral("candidateList"));
    candidates->setAttribute(Qt::WA_StyledBackground, true);
    m_candidateLayout = new QVBoxLayout(candidates);
    m_candidateLayout->setContentsMargins(0, 0, 0, 0);
    m_candidateLayout->setSpacing(0);
    m_candidateLayout->addStretch(1);
    auto *candidateScroll = plainScroll(candidates, QStringLiteral("candidateScroll"));
    candidateScroll->setMaximumHeight(150);
    abl->addSpacing(7);
    abl->addWidget(candidateScroll);
    abl->addSpacing(7);
    auto *manual = new QHBoxLayout;
    manual->setContentsMargins(0, 0, 0, 0);
    manual->setSpacing(7);
    m_manualId = settingsInput(QStringLiteral("手动输入模型 ID"));
    m_addManualButton = styledButton(QStringLiteral("添加"), QStringLiteral("primary"));
    m_addManualButton->setFixedHeight(32);
    manual->addWidget(m_manualId, 1);
    manual->addWidget(m_addManualButton);
    abl->addLayout(manual);
    ml->addSpacing(12);
    ml->addWidget(addBox);
    el->addWidget(m_sectionModels);
    el->addStretch(1);

    connect(m_candidateSearch, &QLineEdit::textChanged, this,
            [this](const QString &) { renderCandidates(); });
    connect(m_addManualButton, &QPushButton::clicked, this, [this] {
        addModel(m_manualId->text(), QString());
        m_manualId->clear();
    });
    connect(m_manualId, &QLineEdit::returnPressed, this, [this] {
        addModel(m_manualId->text(), QString());
        m_manualId->clear();
    });

    return plainScroll(editor, QStringLiteral("providerEditorScroll"));
}

QWidget *SettingsDialog::buildSkillsPage()
{
    auto *page = new QWidget;
    auto *l = new QVBoxLayout(page);
    l->setContentsMargins(18, 18, 18, 18);
    l->setSpacing(0);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(0);
    auto *box = new QVBoxLayout;
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(2);
    box->addWidget(plainLabel(QStringLiteral("eyebrow"), QStringLiteral("SKILLS")));
    auto *line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(6);
    line->addWidget(plainLabel(QStringLiteral("panelTitle"), QStringLiteral("技能")));
    m_skillCount = plainLabel(QStringLiteral("panelCount"), QStringLiteral("0"));
    line->addWidget(m_skillCount, 0, Qt::AlignBottom);
    line->addStretch(1);
    box->addLayout(line);
    toolbar->addLayout(box, 1);
    m_refreshSkills = styledButton(QStringLiteral("刷新"), QStringLiteral("secondary"));
    toolbar->addWidget(m_refreshSkills, 0, Qt::AlignVCenter);
    l->addLayout(toolbar);
    l->addSpacing(12);

    m_skillsStatus = plainLabel(QStringLiteral("panelStatus"), QString());
    l->addWidget(m_skillsStatus);
    l->addSpacing(10);

    auto *panel = new QWidget;
    panel->setObjectName(QStringLiteral("skillsPanel"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    m_skillsLayout = new QVBoxLayout(panel);
    m_skillsLayout->setContentsMargins(0, 0, 0, 0);
    m_skillsLayout->setSpacing(7);
    m_skillsLayout->addStretch(1);
    l->addWidget(plainScroll(panel, QString(), QStringLiteral("panelScroll")), 1);

    connect(m_refreshSkills, &QPushButton::clicked, this, [this] {
        m_refreshSkills->setEnabled(false);
        emit refreshSkillsRequested();
    });
    return page;
}

QWidget *SettingsDialog::buildMcpPage()
{
    auto *page = new QWidget;
    auto *l = new QVBoxLayout(page);
    l->setContentsMargins(18, 18, 18, 18);
    l->setSpacing(0);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(0);
    auto *box = new QVBoxLayout;
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(2);
    box->addWidget(plainLabel(QStringLiteral("eyebrow"), QStringLiteral("MCP TOOLS")));
    auto *line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(6);
    line->addWidget(plainLabel(QStringLiteral("panelTitle"), QStringLiteral("MCP 工具")));
    m_mcpCount = plainLabel(QStringLiteral("panelCount"), QStringLiteral("0"));
    line->addWidget(m_mcpCount, 0, Qt::AlignBottom);
    line->addStretch(1);
    box->addLayout(line);
    toolbar->addLayout(box, 1);
    m_refreshMcp = styledButton(QStringLiteral("刷新"), QStringLiteral("secondary"));
    toolbar->addWidget(m_refreshMcp, 0, Qt::AlignVCenter);
    l->addLayout(toolbar);
    l->addSpacing(12);

    m_mcpStatus = plainLabel(QStringLiteral("panelStatus"), QString());
    l->addWidget(m_mcpStatus);
    l->addSpacing(10);

    auto *panel = new QWidget;
    panel->setObjectName(QStringLiteral("mcpPanel"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    m_mcpLayout = new QVBoxLayout(panel);
    m_mcpLayout->setContentsMargins(0, 0, 0, 0);
    m_mcpLayout->setSpacing(7);
    m_mcpLayout->addStretch(1);
    l->addWidget(plainScroll(panel, QString(), QStringLiteral("panelScroll")), 1);

    connect(m_refreshMcp, &QPushButton::clicked, this, [this] {
        m_refreshMcp->setEnabled(false);
        emit refreshMcpRequested();
    });
    return page;
}

// ------------------------------------------------------------ 数据接口

void SettingsDialog::loadConfig(const QVariantMap &config, const QVariant &revision)
{
    m_extra = config;
    m_extra.remove(QStringLiteral("providers"));
    m_extra.remove(QStringLiteral("models"));
    m_extra.remove(QStringLiteral("default_model"));
    m_extra.remove(QStringLiteral("revision"));

    m_providers = config.value(QStringLiteral("providers")).toList();
    m_models = config.value(QStringLiteral("models")).toList();
    m_defaultModel = config.value(QStringLiteral("default_model")).toString();
    m_revision = revision.isValid() ? revision : config.value(QStringLiteral("revision"));
    m_activeProviderId =
        m_providers.isEmpty() ? QString() : m_providers.first().toMap().value(QStringLiteral("id")).toString();
    m_dirty = false;
    m_discovered.clear();
    m_testing = false;
    m_reading = false;
    m_candidateSearch->clear();
    m_manualId->clear();
    m_saveButton->setEnabled(true);
    setStatus(QString());
    switchTab(QStringLiteral("models"));
    renderSettings();
}

QVariantMap SettingsDialog::draft() const
{
    QVariantMap out = m_extra;
    out.insert(QStringLiteral("providers"), m_providers);
    out.insert(QStringLiteral("models"), m_models);
    out.insert(QStringLiteral("default_model"), m_defaultModel);
    return out;
}

void SettingsDialog::setDiscoveredModels(const QString &providerId, const QVariantList &models)
{
    m_discovered.insert(providerId, models);
    if (providerId == m_activeProviderId)
        renderProviderEditor();
}

void SettingsDialog::setProviderBusy(bool testing, bool reading)
{
    m_testing = testing;
    m_reading = reading;
    m_readButton->setEnabled(!reading);
    m_testButton->setEnabled(!testing);
    renderProviderEditor();
}

void SettingsDialog::setSkills(const QVariantList &skills, bool loading, const QString &error)
{
    m_skills = skills;
    m_skillsLoading = loading;
    m_skillsError = error;
    m_refreshSkills->setEnabled(!loading);
    renderSkills();
}

void SettingsDialog::setMcpTools(const QVariantList &tools, bool connected, bool loading,
                                 const QString &error)
{
    m_tools = tools;
    m_toolsConnected = connected;
    m_toolsLoading = loading;
    m_toolsError = error;
    m_refreshMcp->setEnabled(!loading);
    renderMcpTools();
}

void SettingsDialog::switchTab(const QString &tab)
{
    m_tab = tab;
    m_tabModels->setProperty("active", tab == QLatin1String("models"));
    m_tabSkills->setProperty("active", tab == QLatin1String("skills"));
    m_tabMcp->setProperty("active", tab == QLatin1String("mcp"));
    restyle(m_tabModels);
    restyle(m_tabSkills);
    restyle(m_tabMcp);
    m_stack->setCurrentIndex(tab == QLatin1String("skills")
                                 ? 1
                                 : (tab == QLatin1String("mcp") ? 2 : 0));
    m_saveButton->setVisible(tab == QLatin1String("models"));
    if (tab == QLatin1String("mcp"))
        emit refreshMcpRequested();
    else if (tab == QLatin1String("skills"))
        renderSkills();
}

void SettingsDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}

void SettingsDialog::finishSaved()
{
    m_dirty = false;
    m_saveButton->setEnabled(true);
    done(QDialog::Accepted);
}

void SettingsDialog::markDirty()
{
    m_dirty = true;
    setStatus(QStringLiteral("有未保存的修改"));
}

// ------------------------------------------------------------ 渲染

void SettingsDialog::renderSettings()
{
    renderProviderList();
    renderProviderEditor();
}

void SettingsDialog::renderProviderList()
{
    clearLayout(m_providerNavLayout);
    for (int i = 0; i < m_providers.size(); ++i) {
        const QVariantMap provider = m_providers.at(i).toMap();
        const QString id = provider.value(QStringLiteral("id")).toString();
        auto *item = new QWidget;
        setClass(item, QStringLiteral("providerNavItem"));
        item->setAttribute(Qt::WA_StyledBackground, true);
        item->setCursor(Qt::PointingHandCursor);
        item->setProperty("providerId", id);
        item->installEventFilter(this);
        auto *il = new QHBoxLayout(item);
        il->setContentsMargins(8, 9, 8, 9);
        il->setSpacing(8);
        auto *textBox = new QVBoxLayout;
        textBox->setContentsMargins(0, 0, 0, 0);
        textBox->setSpacing(2);
        textBox->addWidget(plainLabel(QStringLiteral("providerName"),
                                      provider.value(QStringLiteral("name")).toString()));
        textBox->addWidget(plainLabel(QStringLiteral("providerId"), id));
        il->addLayout(textBox, 1);
        auto *count = plainLabel(QStringLiteral("providerCount"),
                                 QString::number(providerModels(id).size()));
        count->setMinimumWidth(22);
        count->setAlignment(Qt::AlignCenter);
        il->addWidget(count, 0, Qt::AlignVCenter);
        if (id == m_activeProviderId)
            item->setProperty("active", true);
        m_providerNavLayout->addWidget(item);
    }
    m_providerNavLayout->addStretch(1);
}

void SettingsDialog::renderProviderEditor()
{
    const int index = providerIndex(m_activeProviderId);
    const bool hasProvider = index >= 0;
    m_placeholder->setVisible(!hasProvider);
    m_sectionProvider->setVisible(hasProvider);
    m_sectionModels->setVisible(hasProvider);
    if (!hasProvider)
        return;

    const QVariantMap provider = m_providers.at(index).toMap();
    const QVariantList models = providerModels(m_activeProviderId);
    const QString apiKey = provider.value(QStringLiteral("api_key")).toString();
    const bool masked = apiKey == QLatin1String("********");
    const QString keyEnv = provider.value(QStringLiteral("api_key_env")).toString();

    m_updating = true;
    m_editorTitle->setText(provider.value(QStringLiteral("name")).toString());
    const QVariant enabled = provider.value(QStringLiteral("enabled"));
    m_enabledCheck->setChecked(!enabled.isValid() || enabled.toBool());
    m_nameEdit->setText(provider.value(QStringLiteral("name")).toString());
    m_idEdit->setText(provider.value(QStringLiteral("id")).toString());
    selectComboData(m_discoveryCombo,
                    provider.value(QStringLiteral("discovery_api")).toString().isEmpty()
                        ? QStringLiteral("openai")
                        : provider.value(QStringLiteral("discovery_api")).toString());
    m_discoveryCombo->setEnabled(models.isEmpty());
    m_baseUrlEdit->setText(provider.value(QStringLiteral("base_url")).toString());
    m_keyEnvEdit->setText(keyEnv);
    m_apiKeyEdit->setText(masked ? QString() : apiKey);
    m_apiKeyEdit->setPlaceholderText(masked ? QStringLiteral("已安全保存")
                                            : QStringLiteral("输入 API Key"));
    m_apiKeyEdit->setEnabled(keyEnv.trimmed().isEmpty());
    selectComboData(m_defaultApiCombo,
                    provider.value(QStringLiteral("default_api")).toString());
    m_modelsCount->setText(QString::number(models.size()));
    m_testButton->setText(m_testing ? QStringLiteral("测试中…") : QStringLiteral("测试连接"));
    m_readButton->setText(m_reading ? QStringLiteral("读取中…") : QStringLiteral("读取模型"));
    m_updating = false;

    clearLayout(m_modelListLayout);
    for (int i = 0; i < models.size(); ++i)
        renderModelCard(models.at(i).toMap());
    renderCandidates();
}

void SettingsDialog::renderModelCard(const QVariantMap &model)
{
    const QString provider = model.value(QStringLiteral("provider")).toString();
    const QString id = model.value(QStringLiteral("id")).toString();
    const QString key = provider + QLatin1Char('/') + id;
    const QString displayName = model.value(QStringLiteral("name")).toString().isEmpty()
                                    ? id
                                    : model.value(QStringLiteral("name")).toString();

    auto *card = new ExpandCard(QStringLiteral("settingsModel"),
                                QStringLiteral("settingsModelSummary"),
                                QStringLiteral("modelAdvanced"));

    auto *summary = new QHBoxLayout;
    summary->setContentsMargins(10, 9, 10, 9);
    summary->setSpacing(10);
    auto *textBox = new QVBoxLayout;
    textBox->setContentsMargins(0, 0, 0, 0);
    textBox->setSpacing(0);
    textBox->addWidget(plainLabel(QStringLiteral("settingsModelName"), displayName));
    textBox->addWidget(plainLabel(QStringLiteral("settingsModelId"), id));
    summary->addLayout(textBox, 1);
    const QString api = model.value(QStringLiteral("api")).toString();
    summary->addWidget(plainLabel(QStringLiteral("settingsModelApi"),
                                  api.isEmpty() ? QStringLiteral("继承供应商") : api),
                       0, Qt::AlignVCenter);
    auto *remove = new IconPushButton(card->summary());
    setClass(remove, QStringLiteral("modelDelete"));
    remove->setFixedSize(22, 22);
    remove->setCursor(Qt::PointingHandCursor);
    remove->setFocusPolicy(Qt::NoFocus);
    remove->setIconColors(QColor(QStringLiteral("#8da0a9")),
                          QColor(QStringLiteral("#e36c6c")));
    remove->setIconName(QStringLiteral("x"), 12);
    summary->addWidget(remove, 0, Qt::AlignVCenter);
    card->summaryLayout()->addLayout(summary);

    auto *body = card->bodyLayout();
    body->setContentsMargins(12, 12, 12, 12);
    body->setSpacing(0);
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    auto *nameEdit = settingsInput(QString());
    nameEdit->setText(model.value(QStringLiteral("name")).toString());
    auto *contextEdit = settingsInput(QString());
    contextEdit->setValidator(new QIntValidator(1, 100000000, contextEdit));
    contextEdit->setText(QString::number(model.value(QStringLiteral("context_window")).toInt()
                                             ? model.value(QStringLiteral("context_window")).toInt()
                                             : 32768));
    auto *outputEdit = settingsInput(QString());
    outputEdit->setValidator(new QIntValidator(1, 100000000, outputEdit));
    outputEdit->setText(QString::number(model.value(QStringLiteral("max_output_tokens")).toInt()
                                            ? model.value(QStringLiteral("max_output_tokens")).toInt()
                                            : 4096));
    auto *apiCombo = settingsCombo();
    apiCombo->addItem(QStringLiteral("继承供应商"), QString());
    apiCombo->addItem(QStringLiteral("OpenAI Chat"), QStringLiteral("openai-chat"));
    apiCombo->addItem(QStringLiteral("OpenAI Responses"), QStringLiteral("openai-responses"));
    apiCombo->addItem(QStringLiteral("Anthropic Messages"),
                      QStringLiteral("anthropic-messages"));
    selectComboData(apiCombo, model.value(QStringLiteral("api")).toString());

    grid->addWidget(fieldRow(QStringLiteral("显示名称"), nameEdit), 0, 0);
    grid->addWidget(fieldRow(QStringLiteral("Context window"), contextEdit), 0, 1);
    grid->addWidget(fieldRow(QStringLiteral("Max output tokens"), outputEdit), 1, 0);
    grid->addWidget(fieldRow(QStringLiteral("API 协议覆盖"), apiCombo), 1, 1);
    body->addLayout(grid);

    auto *caps = new QWidget(card->body());
    auto *flow = new FlowLayout(caps, 0, 18, 8);
    flow->setContentsMargins(0, 12, 0, 0);
    const QVariantMap capabilities = model.value(QStringLiteral("capabilities")).toMap();
    for (int i = 0; i < 5; ++i) {
        const QString capKey = QLatin1String(kCapabilities[i][0]);
        auto *check = new QCheckBox(QLatin1String(kCapabilities[i][1]), caps);
        setClass(check, QStringLiteral("settingsCheck"));
        check->setCursor(Qt::PointingHandCursor);
        check->setChecked(capabilities.value(capKey).toBool());
        connect(check, &QCheckBox::toggled, this, [this, key, capKey](bool on) {
            const int index = modelIndex(key);
            if (index < 0)
                return;
            QVariantMap item = m_models.at(index).toMap();
            QVariantMap caps = item.value(QStringLiteral("capabilities")).toMap();
            caps.insert(capKey, on);
            item.insert(QStringLiteral("capabilities"), caps);
            m_models[index] = item;
            markDirty();
        });
        flow->addWidget(check);
    }
    body->addWidget(caps);

    auto writeField = [this, key](const QString &field, const QVariant &value) {
        const int index = modelIndex(key);
        if (index < 0)
            return;
        QVariantMap item = m_models.at(index).toMap();
        item.insert(field, value);
        m_models[index] = item;
        markDirty();
    };
    connect(nameEdit, &QLineEdit::textChanged, this, [writeField](const QString &text) {
        writeField(QStringLiteral("name"), text);
    });
    connect(contextEdit, &QLineEdit::textChanged, this, [writeField](const QString &text) {
        writeField(QStringLiteral("context_window"), text.toInt());
    });
    connect(outputEdit, &QLineEdit::textChanged, this, [writeField](const QString &text) {
        writeField(QStringLiteral("max_output_tokens"), text.toInt());
    });
    connect(apiCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [writeField, apiCombo](int) {
        const QString value = comboData(apiCombo);
        writeField(QStringLiteral("api"), value.isEmpty() ? QVariant() : QVariant(value));
    });
    connect(remove, &QPushButton::clicked, this, [this, key] {
        const int index = modelIndex(key);
        if (index < 0)
            return;
        m_models.removeAt(index);
        if (m_defaultModel == key) {
            m_defaultModel.clear();
            for (int i = 0; i < m_models.size(); ++i) {
                const QVariantMap item = m_models.at(i).toMap();
                const QVariant on = item.value(QStringLiteral("enabled"));
                if (!on.isValid() || on.toBool()) {
                    m_defaultModel = item.value(QStringLiteral("provider")).toString()
                        + QLatin1Char('/') + item.value(QStringLiteral("id")).toString();
                    break;
                }
            }
        }
        markDirty();
        renderSettings();
    });

    m_modelListLayout->addWidget(card);
}

void SettingsDialog::renderCandidates()
{
    clearLayout(m_candidateLayout);
    const QVariantList candidates = m_discovered.value(m_activeProviderId).toList();
    QSet<QString> added;
    const QVariantList models = providerModels(m_activeProviderId);
    for (int i = 0; i < models.size(); ++i)
        added.insert(models.at(i).toMap().value(QStringLiteral("id")).toString());
    const QString query = m_candidateSearch->text().trimmed().toLower();

    int shown = 0;
    for (int i = 0; i < candidates.size(); ++i) {
        const QVariantMap item = candidates.at(i).toMap();
        QString id = item.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            id = item.value(QStringLiteral("model_id")).toString();
        if (!query.isEmpty() && !id.toLower().contains(query))
            continue;
        const bool exists = added.contains(id);
        QString name = item.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            name = id;

        auto *button = new QPushButton;
        setClass(button, QStringLiteral("candidateItem"));
        button->setEnabled(!exists);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        auto *bl = new QHBoxLayout(button);
        bl->setContentsMargins(6, 6, 6, 6);
        bl->setSpacing(6);
        auto *mark = plainLabel(QStringLiteral("candidateName"), QString());
        mark->setFixedWidth(18);
        mark->setAlignment(Qt::AlignCenter);
        mark->setPixmap(iconPixmap(exists ? QStringLiteral("check")
                                          : QStringLiteral("plus"),
                                   QColor(QStringLiteral("#d7e2e8")), 11));
        bl->addWidget(mark);
        bl->addWidget(plainLabel(QStringLiteral("candidateName"), name), 1);
        bl->addWidget(plainLabel(QStringLiteral("candidateId"), id));
        connect(button, &QPushButton::clicked, this, [this, id, name] { addModel(id, name); });
        m_candidateLayout->addWidget(button);
        ++shown;
    }
    if (!shown)
        m_candidateLayout->addWidget(emptyHint(QStringLiteral("暂无候选，可手动添加")));
    m_candidateLayout->addStretch(1);
}

void SettingsDialog::renderSkills()
{
    m_skillCount->setText(QString::number(m_skills.size()));
    if (m_skillsLoading)
        m_skillsStatus->setText(QStringLiteral("正在读取技能…"));
    else if (!m_skillsError.isEmpty())
        m_skillsStatus->setText(QStringLiteral("技能读取失败：%1").arg(m_skillsError));
    else
        m_skillsStatus->setText(QStringLiteral("共 %1 个技能").arg(m_skills.size()));

    clearLayout(m_skillsLayout);
    if (m_skillsLoading && m_skills.isEmpty()) {
        m_skillsLayout->addStretch(1);
        return;
    }
    if (m_skills.isEmpty()) {
        m_skillsLayout->addWidget(emptyHint(QStringLiteral("暂无可用技能")));
        m_skillsLayout->addStretch(1);
        return;
    }

    for (int i = 0; i < m_skills.size(); ++i) {
        const QVariantMap skill = m_skills.at(i).toMap();
        const QString id = skill.value(QStringLiteral("id")).toString();
        QString name = skill.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            name = id;
        const QVariantList tools = skill.value(QStringLiteral("allowed_tools")).toList();
        const QVariantList shadowed = skill.value(QStringLiteral("shadowed")).toList();

        auto *card = new ExpandCard(QStringLiteral("skillCard"),
                                    QStringLiteral("skillCardSummary"), QString());
        auto *summary = new QHBoxLayout;
        summary->setContentsMargins(10, 9, 10, 9);
        summary->setSpacing(10);
        auto *nameLabel = plainLabel(QStringLiteral("skillName"), name);
        nameLabel->setWordWrap(true);
        summary->addWidget(nameLabel, 1);
        const QString version = skill.value(QStringLiteral("version")).toString();
        if (!version.isEmpty())
            summary->addWidget(plainLabel(QStringLiteral("skillVersion"),
                                          QStringLiteral("v%1").arg(version)),
                               0, Qt::AlignVCenter);
        summary->addWidget(plainLabel(QStringLiteral("skillSource"),
                                      skill.value(QStringLiteral("source")).toString().toUpper()),
                           0, Qt::AlignVCenter);
        summary->addWidget(plainLabel(QStringLiteral("skillSource"),
                                      QStringLiteral("%1 工具").arg(tools.size())),
                           0, Qt::AlignVCenter);
        card->summaryLayout()->addLayout(summary);

        auto *body = card->bodyLayout();
        const QString description = skill.value(QStringLiteral("description")).toString();
        if (!description.isEmpty()) {
            auto *desc = makeLabel(QStringLiteral("skillDesc"), description);
            desc->setWordWrap(true);
            body->addWidget(desc);
        }
        auto *meta = new QWidget(card->body());
        auto *ml = new QHBoxLayout(meta);
        ml->setContentsMargins(10, 0, 10, 9);
        ml->setSpacing(4);
        ml->addWidget(plainLabel(QStringLiteral("skillMetaText"), QStringLiteral("ID")));
        ml->addWidget(plainLabel(QStringLiteral("paramCode"), id));
        ml->addStretch(1);
        body->addWidget(meta);

        if (!tools.isEmpty()) {
            auto *strip = styledWidget(QStringLiteral("codeChipStrip"), card->body());
            auto *flow = new FlowLayout(strip, 0, 8, 7);
            flow->setContentsMargins(10, 2, 10, 9);
            for (int t = 0; t < tools.size(); ++t)
                flow->addWidget(plainLabel(QStringLiteral("codeChip"),
                                           tools.at(t).toString()));
            body->addWidget(strip);
        } else {
            body->addWidget(makeLabel(QStringLiteral("skillDesc"),
                                      QStringLiteral("未限定可用工具")));
        }

        if (!shadowed.isEmpty()) {
            QStringList parts;
            for (int s = 0; s < shadowed.size(); ++s) {
                const QVariantMap item = shadowed.at(s).toMap();
                QString text = item.value(QStringLiteral("source")).toString();
                const QString version = item.value(QStringLiteral("version")).toString();
                if (!version.isEmpty())
                    text += QStringLiteral(" v%1").arg(version);
                parts << text;
            }
            auto *note = makeLabel(QStringLiteral("skillShadowed"),
                                   QStringLiteral("覆盖了 %1 个同名来源：%2")
                                       .arg(shadowed.size())
                                       .arg(parts.join(QStringLiteral("、"))));
            note->setWordWrap(true);
            body->addWidget(note);
        }
        m_skillsLayout->addWidget(card);
    }
    m_skillsLayout->addStretch(1);
}

void SettingsDialog::renderMcpTools()
{
    m_mcpCount->setText(QString::number(m_tools.size()));
    if (m_toolsLoading && m_tools.isEmpty())
        m_mcpStatus->setText(QStringLiteral("正在读取 MCP 工具…"));
    else if (!m_toolsError.isEmpty())
        m_mcpStatus->setText(QStringLiteral("MCP 读取失败：%1").arg(m_toolsError));
    else if (m_toolsConnected)
        m_mcpStatus->setText(QStringLiteral("已连接 · 共 %1 个工具").arg(m_tools.size()));
    else
        m_mcpStatus->setText(QStringLiteral("MCP 服务未连接"));

    clearLayout(m_mcpLayout);
    if (m_toolsLoading && m_tools.isEmpty()) {
        m_mcpLayout->addStretch(1);
        return;
    }
    if (m_tools.isEmpty()) {
        m_mcpLayout->addWidget(emptyHint(QStringLiteral("暂无可用工具")));
        m_mcpLayout->addStretch(1);
        return;
    }

    for (int i = 0; i < m_tools.size(); ++i) {
        const QVariantMap tool = m_tools.at(i).toMap();
        const QVariantList params = schemaParams(tool.value(QStringLiteral("input_schema")).toMap());
        auto *card = new ExpandCard(QStringLiteral("mcpTool"),
                                    QStringLiteral("mcpToolSummary"), QString());
        auto *summary = new QHBoxLayout;
        summary->setContentsMargins(10, 9, 10, 9);
        summary->setSpacing(10);
        auto *nameLabel = plainLabel(QStringLiteral("skillName"),
                                     tool.value(QStringLiteral("name")).toString());
        nameLabel->setWordWrap(true);
        summary->addWidget(nameLabel, 1);
        summary->addWidget(plainLabel(QStringLiteral("skillSource"),
                                      QStringLiteral("%1 参数").arg(params.size())),
                           0, Qt::AlignVCenter);
        card->summaryLayout()->addLayout(summary);

        auto *body = card->bodyLayout();
        const QString description = tool.value(QStringLiteral("description")).toString();
        if (!description.isEmpty()) {
            auto *desc = makeLabel(QStringLiteral("skillDesc"), description);
            desc->setWordWrap(true);
            body->addWidget(desc);
        }
        if (params.isEmpty()) {
            body->addWidget(makeLabel(QStringLiteral("skillDesc"), QStringLiteral("无参数")));
        } else {
            auto *list = styledWidget(QStringLiteral("mcpParamList"), card->body());
            auto *ll = new QVBoxLayout(list);
            ll->setContentsMargins(10, 2, 10, 9);
            ll->setSpacing(0);
            for (int p = 0; p < params.size(); ++p) {
                const QVariantMap param = params.at(p).toMap();
                auto *row = styledWidget(p + 1 < params.size()
                                             ? QStringLiteral("mcpParamRow")
                                             : QString(),
                                         list);
                auto *rl = new QHBoxLayout(row);
                rl->setContentsMargins(0, 7, 0, 7);
                rl->setSpacing(8);
                rl->addWidget(plainLabel(QStringLiteral("paramCode"),
                                         param.value(QStringLiteral("name")).toString()));
                rl->addWidget(plainLabel(QStringLiteral("mcpParamType"),
                                         param.value(QStringLiteral("type")).toString()));
                if (param.value(QStringLiteral("required")).toBool())
                    rl->addWidget(plainLabel(QStringLiteral("mcpParamRequired"),
                                             QStringLiteral("必填")));
                const QString desc = param.value(QStringLiteral("description")).toString();
                if (!desc.isEmpty()) {
                    auto *descLabel = plainLabel(QStringLiteral("mcpParamDesc"), desc);
                    descLabel->setWordWrap(true);
                    rl->addWidget(descLabel, 1);
                } else {
                    rl->addStretch(1);
                }
                ll->addWidget(row);
            }
            body->addWidget(list);
        }
        m_mcpLayout->addWidget(card);
    }
    m_mcpLayout->addStretch(1);
}

// ------------------------------------------------------------ 操作

int SettingsDialog::providerIndex(const QString &id) const
{
    for (int i = 0; i < m_providers.size(); ++i) {
        if (m_providers.at(i).toMap().value(QStringLiteral("id")).toString() == id)
            return i;
    }
    return -1;
}

int SettingsDialog::modelIndex(const QString &key) const
{
    for (int i = 0; i < m_models.size(); ++i) {
        const QVariantMap item = m_models.at(i).toMap();
        const QString itemKey = item.value(QStringLiteral("provider")).toString()
            + QLatin1Char('/') + item.value(QStringLiteral("id")).toString();
        if (itemKey == key)
            return i;
    }
    return -1;
}

QVariantList SettingsDialog::providerModels(const QString &id) const
{
    QVariantList out;
    for (int i = 0; i < m_models.size(); ++i) {
        const QVariantMap item = m_models.at(i).toMap();
        if (item.value(QStringLiteral("provider")).toString() == id)
            out.append(item);
    }
    return out;
}

void SettingsDialog::addProvider()
{
    QString id = QStringLiteral("custom");
    int suffix = 2;
    while (providerIndex(id) >= 0)
        id = QStringLiteral("custom-%1").arg(suffix++);

    QVariantMap provider;
    provider.insert(QStringLiteral("id"), id);
    // PROVIDER_PRESETS.compatible
    provider.insert(QStringLiteral("name"), QStringLiteral("新供应商"));
    provider.insert(QStringLiteral("base_url"), QString());
    provider.insert(QStringLiteral("default_api"), QStringLiteral("openai-chat"));
    provider.insert(QStringLiteral("discovery_api"), QStringLiteral("openai"));
    provider.insert(QStringLiteral("api_key"), QString());
    provider.insert(QStringLiteral("api_key_env"), QString());
    provider.insert(QStringLiteral("headers"), QVariantMap());
    provider.insert(QStringLiteral("discover_models"), true);
    provider.insert(QStringLiteral("enabled"), true);
    m_providers.append(provider);
    m_activeProviderId = id;
    markDirty();
    renderSettings();
    m_nameEdit->setFocus();
    m_nameEdit->selectAll();
}

void SettingsDialog::deleteProvider()
{
    const int index = providerIndex(m_activeProviderId);
    if (index < 0)
        return;
    if (!providerModels(m_activeProviderId).isEmpty()) {
        setStatus(QStringLiteral("请先删除该供应商下的模型"));
        return;
    }
    if (m_defaultModel.startsWith(m_activeProviderId + QLatin1Char('/'))) {
        setStatus(QStringLiteral("默认模型属于该供应商，无法删除"));
        return;
    }
    m_providers.removeAt(index);
    m_activeProviderId = m_providers.isEmpty()
                             ? QString()
                             : m_providers.first().toMap().value(QStringLiteral("id")).toString();
    markDirty();
    renderSettings();
}

void SettingsDialog::clearApiKey()
{
    const int index = providerIndex(m_activeProviderId);
    if (index < 0)
        return;
    QVariantMap provider = m_providers.at(index).toMap();
    provider.insert(QStringLiteral("api_key"), QString());
    m_providers[index] = provider;
    markDirty();
    renderProviderEditor();
}

void SettingsDialog::addModel(const QString &rawId, const QString &name)
{
    const QString id = rawId.trimmed();
    if (id.isEmpty()) {
        setStatus(QStringLiteral("模型 ID 不得为空"));
        return;
    }
    if (providerIndex(m_activeProviderId) < 0)
        return;
    const QVariantList existing = providerModels(m_activeProviderId);
    for (int i = 0; i < existing.size(); ++i) {
        if (existing.at(i).toMap().value(QStringLiteral("id")).toString() == id) {
            setStatus(QStringLiteral("该模型已添加"));
            return;
        }
    }
    QVariantMap capabilities;
    for (int i = 0; i < 5; ++i)
        capabilities.insert(QLatin1String(kCapabilities[i][0]), false);

    QVariantMap model;
    model.insert(QStringLiteral("id"), id);
    model.insert(QStringLiteral("provider"), m_activeProviderId);
    model.insert(QStringLiteral("api"), QVariant());
    model.insert(QStringLiteral("name"), name.isEmpty() ? id : name);
    model.insert(QStringLiteral("enabled"), true);
    model.insert(QStringLiteral("context_window"), 32768);
    model.insert(QStringLiteral("max_output_tokens"), 4096);
    model.insert(QStringLiteral("capabilities"), capabilities);
    model.insert(QStringLiteral("compat"), QVariantMap());
    m_models.append(model);
    if (m_defaultModel.isEmpty())
        m_defaultModel = m_activeProviderId + QLatin1Char('/') + id;
    markDirty();
    renderSettings();
}

bool SettingsDialog::validateSettings()
{
    QStringList errors;
    for (int i = 0; i < m_providers.size(); ++i) {
        const QVariantMap provider = m_providers.at(i).toMap();
        const QUrl url(provider.value(QStringLiteral("base_url")).toString());
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
            errors << QStringLiteral("%1 的 API 地址无效")
                          .arg(provider.value(QStringLiteral("name")).toString());
    }
    for (int i = 0; i < m_models.size(); ++i) {
        const QVariantMap model = m_models.at(i).toMap();
        if (model.value(QStringLiteral("context_window")).toInt()
            < model.value(QStringLiteral("max_output_tokens")).toInt())
            errors << QStringLiteral("%1 的 context window 不能小于最大输出")
                          .arg(model.value(QStringLiteral("id")).toString());
    }
    bool hasDefault = false;
    QString fallback;
    for (int i = 0; i < m_models.size(); ++i) {
        const QVariantMap model = m_models.at(i).toMap();
        const QVariant on = model.value(QStringLiteral("enabled"));
        if (on.isValid() && !on.toBool())
            continue;
        const QString key = model.value(QStringLiteral("provider")).toString()
            + QLatin1Char('/') + model.value(QStringLiteral("id")).toString();
        if (key == m_defaultModel)
            hasDefault = true;
        if (fallback.isEmpty())
            fallback = key;
    }
    if (!hasDefault) {
        if (fallback.isEmpty())
            errors << QStringLiteral("请至少添加一个已启用模型");
        else
            m_defaultModel = fallback;
    }
    setStatus(errors.isEmpty() ? QString() : errors.first());
    return errors.isEmpty();
}

void SettingsDialog::attemptClose()
{
    if (m_dirty) {
        const bool ok = ConfirmDialog::ask(this, QStringLiteral("放弃未保存的设置"),
                                           QStringLiteral("模型设置尚未保存，确定要关闭吗？"),
                                           QStringLiteral("放弃更改"), true);
        if (!ok)
            return;
    }
    m_saveButton->setEnabled(true);
    done(QDialog::Rejected);
}

void SettingsDialog::closeEvent(QCloseEvent *event)
{
    if (m_dirty) {
        const bool ok = ConfirmDialog::ask(this, QStringLiteral("放弃未保存的设置"),
                                           QStringLiteral("模型设置尚未保存，确定要关闭吗？"),
                                           QStringLiteral("放弃更改"), true);
        if (!ok) {
            event->ignore();
            return;
        }
    }
    m_saveButton->setEnabled(true);
    event->accept();
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *widget = qobject_cast<QWidget *>(watched);
        const QVariant id = widget ? widget->property("providerId") : QVariant();
        if (id.isValid()) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (widget->rect().contains(me->pos())
                && id.toString() != m_activeProviderId) {
                m_activeProviderId = id.toString();
                m_candidateSearch->clear();
                renderSettings();
            }
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace gs

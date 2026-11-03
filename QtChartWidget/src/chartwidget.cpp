#include "chartwidget.h"

#include "commonwidgets.h"
#include "composer.h"
#include "markdownview.h"
#include "messagewidgets.h"
#include "phasepanel.h"
#include "popups.h"
#include "settingsdialog.h"
#include "theme.h"
#include "trajectoryview.h"
#include "usagepanel.h"

#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QFontMetrics>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSpacerItem>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace gs {
namespace {

// app.js ASK_USER_TOOL：询问类工具调用不落成工具条目，改由输入框上方的浮层承担
const char kAskUserTool[] = "ask_user_question";

// 「结束」类选项（webui 5948605）：点了就是本轮收尾。把它当普通消息发回去，auto 模式的
// 模型会接着干（再探测、再总结、再弹一张同样的卡），用户点多少次都出不来。
// 识别只做保守判断：label 命中收尾措辞、或 value 是白名单里的结束值；含「继续、下一步、
// 再试」这类明确要继续的措辞一票否决——宁可当普通选择发回去，也别误吞一条真指令。
bool isEndTurnOption(const QString &label, const QString &value)
{
    static const QRegularExpression labelRe(QStringLiteral(
        "本轮到此结束|结束本轮|结束任务|就此结束|到此为止|先到这里|先到这步|先这样吧|先这样|"
        "就这样吧|不用了|不必了|收尾结束|停止本轮|(^|[，,、\\s])结束$"));
    static const QRegularExpression valueRe(QStringLiteral(
        "^(stop|stop_here|stop_turn|end|end_turn|exit|quit|finish|done|cancel)$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression continueRe(QStringLiteral("继续|接着|下一步|再试|然后|之后"));

    const QString text = label.trimmed();
    const QString raw = value.trimmed();
    if (continueRe.match(text).hasMatch())
        return false;
    if (!text.isEmpty() && text.size() <= 24 && labelRe.match(text).hasMatch())
        return true;
    return !raw.isEmpty() && valueRe.match(raw).hasMatch();
}

// QLayout treats negative margins as "unset". Keep the normal flow size and
// overlap the phase dock with the composer when WebUI's lift is negative.
class PhaseLiftLayout : public QVBoxLayout
{
public:
    explicit PhaseLiftLayout(QWidget *parent) : QVBoxLayout(parent) {}

    void setPhaseWidget(QWidget *widget) { m_phaseWidget = widget; }

    void setPhaseOverlap(int overlap)
    {
        if (m_overlap == overlap)
            return;
        m_overlap = qMax(0, overlap);
        invalidate();
    }

    void setGeometry(const QRect &rect) override
    {
        QVBoxLayout::setGeometry(rect);
        if (!m_phaseWidget || m_overlap <= 0)
            return;
        const QRect geometry = m_phaseWidget->geometry();
        m_phaseWidget->move(geometry.x(), geometry.y() + m_overlap);
        m_phaseWidget->raise();
    }

private:
    QWidget *m_phaseWidget = nullptr;
    int m_overlap = 0;
};

// app.js usageModelLabel：后端回传的是 provider ID（如 custom），这里换成用户配置的供应商名称
QString usageModelLabel(const QVariantList &models, const QString &raw)
{
    const QString key = raw.trimmed();
    if (key.isEmpty())
        return QString();
    const int slash = key.indexOf(QLatin1Char('/'));
    const QString providerId = slash > 0 ? key.left(slash) : QString();
    const QString modelId = slash > 0 ? key.mid(slash + 1) : key;
    if (providerId.isEmpty())
        return modelId;
    for (const QVariant &item : models) {
        const QVariantMap map = item.toMap();
        if (map.value(QStringLiteral("provider")).toString() != providerId)
            continue;
        const QString name = map.value(QStringLiteral("provider_name")).toString();
        return (name.isEmpty() ? providerId : name) + QStringLiteral(" / ") + modelId;
    }
    return providerId + QStringLiteral(" / ") + modelId;
}

// app.js renderStructured：结构化块里的询问载荷（取最后一个候选块）
QVariantMap askPayloadOf(const QVariantMap &data)
{
    QVariantMap toolParams = data.value(QStringLiteral("tool_params")).toMap();
    if (toolParams.isEmpty())
        toolParams = data.value(QStringLiteral("toolparams")).toMap();
    const QVariantList options = data.value(QStringLiteral("options")).toList();
    if (toolParams.isEmpty() && options.isEmpty())
        return QVariantMap();
    QVariantMap payload;
    if (!toolParams.isEmpty())
        payload.insert(QStringLiteral("tool_params"), toolParams);
    if (!options.isEmpty())
        payload.insert(QStringLiteral("options"), options);
    return payload;
}

bool isSelfOrChildOf(QWidget *widget, QWidget *ancestor)
{
    for (QWidget *p = widget; p; p = p->parentWidget())
        if (p == ancestor)
            return true;
    return false;
}

// 控件自身或其祖先是否带某个 class（QSS 的 .class 动态属性）
bool hasClassOrAncestor(QWidget *widget, const char *cls)
{
    const QString target = QLatin1String(cls);
    for (QWidget *w = widget; w; w = w->parentWidget()) {
        if (w->property("class").toString().split(QLatin1Char(' ')).contains(target))
            return true;
    }
    return false;
}

// app.js railTurnText：轮次摘要取该轮用户消息的正文，空则给占位。
// copyText 由调用方在 createMessage 之后回填，所以再退到渲染用的正文
QString turnText(MessageWidget *message)
{
    QString text = message->property("copyText").toString().simplified();
    if (text.isEmpty() && message->body())
        text = message->body()->text().simplified();
    return text.isEmpty() ? QStringLiteral("（本轮无文本内容）") : text;
}

// 清空布局里的全部条目（含嵌套布局）
void clearLayoutItems(QLayout *layout)
{
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        else if (QLayout *child = item->layout()) {
            clearLayoutItems(child);
            delete child;
        }
        delete item;
    }
}

// app.js extractPhase
QVariantMap extractPhase(const QVariant &value)
{
    const QVariantMap direct = value.toMap();
    if (direct.contains(QStringLiteral("phases")))
        return direct;
    QString text = value.type() == QVariant::String ? value.toString()
                                                    : direct.value(QStringLiteral("text")).toString();
    if (text.isEmpty())
        return QVariantMap();
    const QVariantList blocks = structuredBlocks(text).found;
    for (const QVariant &item : blocks) {
        const QVariantMap map = item.toMap();
        if (map.contains(QStringLiteral("phase_plan")))
            return map.value(QStringLiteral("phase_plan")).toMap();
    }
    return QVariantMap();
}

// 气泡工具结果的失败判定与工具组摘要刷新（app.js renderToolResult / updateToolGroup）
void applyToolResult(ToolItemWidget *item, const QString &result)
{
    const bool failed = result.toLower().contains(QLatin1String("error"))
                        || result.contains(QLatin1String("denied"));
    item->setState(failed ? QStringLiteral("failed") : QStringLiteral("succeeded"));
    item->setResult(result);
    for (QWidget *w = item->parentWidget(); w; w = w->parentWidget()) {
        if (auto *message = qobject_cast<MessageWidget *>(w)) {
            message->updateToolSummary();
            break;
        }
    }
}

// 从 ToolItemWidget 反查它所属的消息气泡
MessageWidget *ownerMessage(QWidget *widget)
{
    for (QWidget *w = widget->parentWidget(); w; w = w->parentWidget())
        if (auto *message = qobject_cast<MessageWidget *>(w))
            return message;
    return nullptr;
}

bool hasToolItems(MessageWidget *message)
{
    return message && !message->findChildren<ToolItemWidget *>().isEmpty();
}

bool hasProcRows(MessageWidget *message)
{
    return message && !message->findChildren<ProcRow *>().isEmpty();
}

// 设置对话框是独立顶级窗口，里面不少容器是「先无父创建、之后才挂进来」的
// （providerSidebar / providerEditor / skillsPanel …）：它们作为临时顶级窗口时就已经被
// polish 过了，之后再也等不到我们的事件过滤器，字距与无障碍属性会整块漏掉。
// 打开对话框时补一次整棵子树。
void applyDeferredStyleDetails(QWidget *root)
{
    applyLetterSpacing(root);
    applyAccessibility(root);
    const QList<QWidget *> all = root->findChildren<QWidget *>();
    for (QWidget *widget : all) {
        applyLetterSpacing(widget);
        applyAccessibility(widget);
    }
}

// sessions 数组里"当前会话"的可选 status 字段由 popups.cpp 的会话面板读取
} // namespace

// ------------------------------------------------------------ 线程契约

// 公开 API 必须在 GUI 线程调用：内部直接操作 QWidget / QSS，Qt 的部件体系没有跨线程保护。
// 契约全文见 README「线程模型」；下面这些「宿主收到网络 / 流式回调后推数据」的入口最容易
// 被误放到工作线程上，所以在运行期兜一层：开发期断言直接暴露，发布期 qWarning + 忽略本次
// 调用（好过静默的内存与绘制损坏）。
bool ChartWidget::assertGuiThread(const char *entry) const
{
    if (QThread::currentThread() == thread())
        return true;
    qWarning("ChartWidget::%s() 被非 GUI 线程调用（当前线程 %p，对象线程 %p）：本次调用被忽略，"
             "请改用信号槽或 QMetaObject::invokeMethod(Qt::QueuedConnection) 切回 GUI 线程",
             entry, QThread::currentThread(), thread());
    Q_ASSERT_X(false, "ChartWidget", "公开 API 被非 GUI 线程调用");
    return false;
}

// ------------------------------------------------------------ 构造

ChartWidget::ChartWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("appShell"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAcceptDrops(true);
    setMinimumSize(420, 460);

    // QSS 里没有全局字体规则，基础字号/字体族要在根部件上显式设置
    QFont font(uiFont());
    font.setPixelSize(scaledPx(basePixelSize()));
    setFont(font);
    setStyleSheet(appStyleSheet());

    // VSCode 式界面缩放快捷键：Ctrl+= / Ctrl++ 放大，Ctrl+- 缩小，Ctrl+0 重置
    const QList<QKeySequence> zoomInKeys{QKeySequence(QStringLiteral("Ctrl+=")),
                                         QKeySequence(QStringLiteral("Ctrl++"))};
    for (const QKeySequence &seq : zoomInKeys) {
        auto *shortcut = new QShortcut(seq, this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, &ChartWidget::zoomIn);
    }
    auto *zoomOutShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+-")), this);
    zoomOutShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(zoomOutShortcut, &QShortcut::activated, this, &ChartWidget::zoomOut);
    auto *zoomResetShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+0")), this);
    zoomResetShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(zoomResetShortcut, &QShortcut::activated, this, &ChartWidget::zoomReset);

    auto *rootLayout = new PhaseLiftLayout(this);
    m_root = rootLayout;
    m_root->setContentsMargins(0, 0, 0, 0);
    m_root->setSpacing(0);

    buildTopbar();
    buildSessionbar(); // 内部会把视图切换（对话 / 轨迹）旋到第二行右侧
    buildMessages();
    buildTurnRail();

    // 轨迹视图与消息区互斥（.trajectory-view / #messages）
    m_trajView = new TrajectoryView(this);
    m_trajView->setVisible(false);
    m_root->addWidget(m_trajView, 1);

    // .phase-panel { margin: 0 auto 10px }：与消息区同一条主栏竖线
    m_phaseWrap = new QWidget(this);
    m_phaseLayout = new QVBoxLayout(m_phaseWrap);
    m_phaseLayout->setContentsMargins(0, 0, 0, 8);
    m_phaseLayout->setSpacing(0);
    m_phasePanel = new PhasePanel(m_phaseWrap);
    m_phaseLayout->addWidget(m_phasePanel);
    m_phaseLiftSpacer = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_phaseLayout->addSpacerItem(m_phaseLiftSpacer);
    m_phaseWrap->setVisible(false);
    m_root->addWidget(m_phaseWrap);
    rootLayout->setPhaseWidget(m_phaseWrap);

    m_composer = new Composer(this);
    m_root->addWidget(m_composer);

    // 叠加层：不入布局，靠 layoutOverlays() 定位（.app-shell { position: relative }）
    m_sessionPanel = new SessionPanel(this);
    m_toast = new Toast(this);
    m_dropOverlay = new DropOverlay(this);
    m_themePopup = new ThemeListPopup(this);

    m_settings = new SettingsDialog(this);
    m_settings->setModal(true);
    m_settings->setWindowModality(Qt::WindowModal);

    connect(m_connection, &ConnectionButton::clicked, this, &ChartWidget::connectionCheckRequested);
    connect(m_newSession, &QPushButton::clicked, this, &ChartWidget::newSessionRequested);

    connect(m_sessionPanel, &SessionPanel::sessionSelected, this, [this](const QString &id) {
        closeSessionPanel();
        emit sessionSelected(id);
    });
    connect(m_sessionPanel, &SessionPanel::sessionRenamed, this, &ChartWidget::sessionRenamed);
    connect(m_sessionPanel, &SessionPanel::sessionCleared, this, &ChartWidget::sessionCleared);
    connect(m_sessionPanel, &SessionPanel::sessionDeleted, this, &ChartWidget::sessionDeleted);
    connect(m_sessionPanel, &SessionPanel::closeRequested, this, &ChartWidget::closeSessionPanel);

    connect(m_themePopup, &ThemeListPopup::themeChosen, this, [this](const QString &id) {
        applyTheme(id);
        emit themeChanged(id);
    });

    connect(m_tabChat, &QPushButton::clicked, this, [this] { setViewTab(QStringLiteral("chat")); });
    connect(m_tabTraj, &QPushButton::clicked, this, [this] { setViewTab(QStringLiteral("traj")); });

    // 新一轮提问让上一轮遗留的确认浮层立即收起：流程已经往下走了，那张卡里的参数
    // 再提交一次就是错数据；本轮结束时自然会有新的卡顶上（webui c4a6d84）。
    // 这个连接必须排在 sendMessage 转发之前，保证宿主收到时浮层已经收起
    connect(m_composer, &Composer::sendMessage, this, [this] { m_composer->closeChoice(); },
            Qt::DirectConnection);
    connect(m_composer, &Composer::sendMessage, this, &ChartWidget::sendMessage);
    connect(m_composer, &Composer::stopRequested, this, &ChartWidget::stopRequested);
    connect(m_composer, &Composer::modeChanged, this, &ChartWidget::modeChanged);
    connect(m_composer, &Composer::modelSelected, this, &ChartWidget::modelSelected);
    connect(m_composer, &Composer::skillSelected, this, &ChartWidget::skillSelected);
    connect(m_composer, &Composer::categorySelected, this, &ChartWidget::categorySelected);
    connect(m_composer, &Composer::knowledgeBaseSelected,
            this, &ChartWidget::knowledgeBaseSelected);
    connect(m_composer, &Composer::exportRequested, this, &ChartWidget::sessionExportRequested);
    connect(m_composer, &Composer::settingsRequested, this, [this] { openSettings(); });
    connect(m_composer, &Composer::attachRequested, this, &ChartWidget::attachRequested);
    connect(m_composer, &Composer::voiceRequested, this, &ChartWidget::voiceRequested);
    connect(m_composer, &Composer::attachmentRemoved, this, &ChartWidget::attachmentRemoved);
    // 「结束」类选项不走普通发送：只发 endTurnRequested 并在本地收尾，不开新一轮
    connect(m_composer, &Composer::optionChosen, this,
            [this](const QString &value, const QString &label) {
                if (!isEndTurnOption(label, value)) {
                    emit optionChosen(value, label);
                    return;
                }
                m_composer->closeChoice();
                markCurrentStopped();
                hidePhasePanel();
                appendUserMessage(label, QVariantList());
                emit endTurnRequested(label);
            });
    connect(m_composer, &Composer::approvalDecided, this, &ChartWidget::approvalDecided);
    connect(m_composer, &Composer::choiceOpenChanged, this, [this](bool open) {
        syncPhaseLift();
        emit choiceOpenChanged(open);
    });
    connect(m_composer, &Composer::choiceResized, this, &ChartWidget::syncPhaseLift);
    connect(m_composer, &Composer::toastRequested, this, &ChartWidget::showToast);

    connect(m_trajView, &TrajectoryView::reloadRequested, this,
            &ChartWidget::trajectoryReloadRequested);

    connect(m_settings, &SettingsDialog::saveRequested, this, &ChartWidget::settingsSaveRequested);
    connect(m_settings, &SettingsDialog::testProviderRequested, this,
            &ChartWidget::testProviderRequested);
    connect(m_settings, &SettingsDialog::readModelsRequested, this,
            &ChartWidget::readModelsRequested);
    connect(m_settings, &SettingsDialog::modelMetadataRefreshRequested, this,
            &ChartWidget::modelMetadataRefreshRequested);
    connect(m_settings, &SettingsDialog::modelLimitsResolveRequested, this,
            &ChartWidget::modelLimitsResolveRequested);
    connect(m_settings, &SettingsDialog::kbFolderChanged, this, &ChartWidget::kbFolderChanged);

    // 记忆页 / 知识库页的意图信号：SettingsDialog 发出来之后必须继续往上传，
    // 否则宿主一个都收不到（先前只连了 kbFolderChanged，其余 24 条断在对话框里）
    connect(m_settings, &SettingsDialog::memoryRefreshRequested,
            this, &ChartWidget::memoryRefreshRequested);
    connect(m_settings, &SettingsDialog::memoryListRequested,
            this, &ChartWidget::memoryListRequested);
    connect(m_settings, &SettingsDialog::memoryConfigPatchRequested,
            this, &ChartWidget::memoryConfigPatchRequested);
    connect(m_settings, &SettingsDialog::memoryActionRequested,
            this, &ChartWidget::memoryActionRequested);
    connect(m_settings, &SettingsDialog::memoryBulkRequested,
            this, &ChartWidget::memoryBulkRequested);
    connect(m_settings, &SettingsDialog::memoryDetailRequested,
            this, &ChartWidget::memoryDetailRequested);
    connect(m_settings, &SettingsDialog::memoryFieldSaveRequested,
            this, &ChartWidget::memoryFieldSaveRequested);
    connect(m_settings, &SettingsDialog::memoryFieldDeleteRequested,
            this, &ChartWidget::memoryFieldDeleteRequested);
    connect(m_settings, &SettingsDialog::memoryToolClearRequested,
            this, &ChartWidget::memoryToolClearRequested);
    connect(m_settings, &SettingsDialog::memoryToolsClearAllRequested,
            this, &ChartWidget::memoryToolsClearAllRequested);
    connect(m_settings, &SettingsDialog::memoryMaintenanceRequested,
            this, &ChartWidget::memoryMaintenanceRequested);

    connect(m_settings, &SettingsDialog::kbRefreshRequested,
            this, &ChartWidget::kbRefreshRequested);
    connect(m_settings, &SettingsDialog::kbLibraryCreateRequested,
            this, &ChartWidget::kbLibraryCreateRequested);
    connect(m_settings, &SettingsDialog::kbLibraryRenameRequested,
            this, &ChartWidget::kbLibraryRenameRequested);
    connect(m_settings, &SettingsDialog::kbLibraryDeleteRequested,
            this, &ChartWidget::kbLibraryDeleteRequested);
    connect(m_settings, &SettingsDialog::kbFilesImportRequested,
            this, &ChartWidget::kbFilesImportRequested);
    connect(m_settings, &SettingsDialog::kbDirImportRequested,
            this, &ChartWidget::kbDirImportRequested);
    connect(m_settings, &SettingsDialog::kbDirImportToFolderRequested,
            this, &ChartWidget::kbDirImportToFolderRequested);
    connect(m_settings, &SettingsDialog::kbDocumentDeleteRequested,
            this, &ChartWidget::kbDocumentDeleteRequested);
    connect(m_settings, &SettingsDialog::kbDocumentReindexRequested,
            this, &ChartWidget::kbDocumentReindexRequested);
    connect(m_settings, &SettingsDialog::kbDocumentMoveRequested,
            this, &ChartWidget::kbDocumentMoveRequested);
    connect(m_settings, &SettingsDialog::kbFolderCreateRequested,
            this, &ChartWidget::kbFolderCreateRequested);
    connect(m_settings, &SettingsDialog::kbFolderRenameRequested,
            this, &ChartWidget::kbFolderRenameRequested);
    connect(m_settings, &SettingsDialog::kbFolderDeleteRequested,
            this, &ChartWidget::kbFolderDeleteRequested);
    connect(m_settings, &SettingsDialog::kbSearchRequested,
            this, &ChartWidget::kbSearchRequested);
    connect(m_settings, &SettingsDialog::refreshSkillsRequested, this,
            &ChartWidget::refreshSkillsRequested);
    connect(m_settings, &SettingsDialog::refreshMcpRequested, this,
            &ChartWidget::refreshMcpRequested);
    connect(m_settings, &SettingsDialog::usageStatsRequested, this,
            &ChartWidget::usageStatsRequested);

    // 消息区滚动：贴底才跟随；离开底部即交出滚动控制权，同时刷新导航轨高亮
    connect(m_messages->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        m_followBottom = atBottom();
        updateTurnRailActive();
    });

    // 会话触发器/皮肤触发器都不是 QPushButton（要放省略号标题 + 图标），点击由 app 级
    // 过滤器统一接管：app filter 先于目标部件执行，且鼠标事件会沿 parentWidget() 链每层
    // 重走一次过滤器，所以在触发器子树上命中即 return true，避免冒泡导致重复开合。
    m_currentTitle->installEventFilter(this);
    m_composer->installEventFilter(this); // 输入区高度变化时重排 Toast / 导航轨
    qApp->installEventFilter(this);
}

ChartWidget::~ChartWidget()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

// ------------------------------------------------------------ 骨架

void ChartWidget::buildTopbar()
{
    // .topbar { height:52px; padding:0 16px; display:flex; align-items:center }
    auto *topbar = new QWidget(this);
    topbar->setObjectName(QStringLiteral("topbar"));
    topbar->setAttribute(Qt::WA_StyledBackground, true);
    topbar->setFixedHeight(52);
    auto *layout = new QHBoxLayout(topbar);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(10);

    // 品牌图标：webui 6213b58 把它改成 button（#brand-mark.brand-mark-btn），
    // 点击即 POST /gridstar/ensure 拉起 GridStar 主程序；库侧只发信号，请求与 Toast 由宿主做
    auto *mark = new QPushButton(topbar);
    mark->setObjectName(QStringLiteral("brandMark"));
    setClass(mark, QStringLiteral("brandMarkBtn"));
    mark->setFixedSize(22, 22); // .brand-mark { width:22px; height:22px }
    mark->setCursor(Qt::PointingHandCursor);
    mark->setFocusPolicy(Qt::TabFocus);
    mark->setToolTip(QStringLiteral("打开 GridStar"));
    mark->setAccessibleName(QStringLiteral("打开 GridStar"));
    const QPixmap logo(QStringLiteral(":/icons/Logo.ico"));
    if (!logo.isNull()) {
        mark->setIcon(QIcon(logo));
        mark->setIconSize(QSize(22, 22));
    }
    connect(mark, &QPushButton::clicked, this, &ChartWidget::launchAppRequested);

    auto *brand = new QLabel(QStringLiteral("GridStar Agent"), topbar);
    brand->setObjectName(QStringLiteral("brandText"));
    // 窄窗口下先压标题，不要让顶栏把宿主的最小宽度顶到 300 以上
    brand->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    brand->setMinimumWidth(0);

    m_connection = new ConnectionButton(topbar);

    // 皮肤切换（.theme-switch）：幽灵触发器 + 下拉
    m_themeTrigger = new QWidget(topbar);
    m_themeTrigger->setObjectName(QStringLiteral("themeTrigger"));
    m_themeTrigger->setAttribute(Qt::WA_StyledBackground, true);
    m_themeTrigger->setFixedHeight(28);
    m_themeTrigger->setCursor(Qt::PointingHandCursor);
    m_themeTrigger->setToolTip(QStringLiteral("切换皮肤"));
    auto *themeLayout = new QHBoxLayout(m_themeTrigger);
    themeLayout->setContentsMargins(7, 0, 9, 0);
    themeLayout->setSpacing(6);
    m_themeSwatch = new ThemeSwatch(themeId(), 12, true, m_themeTrigger);
    m_themeLabel = new QLabel(themeName(), m_themeTrigger);
    m_themeLabel->setObjectName(QStringLiteral("themeLabel"));
    m_themeLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    auto *themeChevron = new QLabel(m_themeTrigger);
    setClass(themeChevron, QStringLiteral("chevronGlyph"));
    themeChevron->setPixmap(iconPixmap(QStringLiteral("chevron-down"), gs::palette().muted, 12));
    themeLayout->addWidget(m_themeSwatch, 0);
    themeLayout->addWidget(m_themeLabel, 0);
    themeLayout->addWidget(themeChevron, 0);

    layout->addWidget(mark);
    layout->addWidget(brand);
    layout->addStretch(1); // .connection { margin-left:auto }
    layout->addWidget(m_connection);
    layout->addWidget(m_themeTrigger);
    m_root->addWidget(topbar);
}

void ChartWidget::buildSessionbar()
{
    // .sessionbar { height:44px; gap:10px; padding:0 12px; background:var(--band-1) }
    // 第二行是一个整体：新建 / 会话名 / 右侧视图切换
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("sessionbar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(44);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(10);

    auto *newSession = new IconPushButton(bar);
    newSession->setObjectName(QStringLiteral("newSession"));
    newSession->setFixedHeight(28); // .compact { height:28px }
    newSession->setCursor(Qt::PointingHandCursor);
    newSession->setText(QStringLiteral("新对话"));
    newSession->setIconColors(gs::palette().textBright, gs::palette().cyan);
    newSession->setIconName(QStringLiteral("plus"), 13);
    m_newSession = newSession;
    layout->addWidget(m_newSession);

    // .session-trigger { height:28px; padding:0 8px; justify-content:flex-start }
    m_sessionTrigger = new QWidget(bar);
    m_sessionTrigger->setObjectName(QStringLiteral("sessionTrigger"));
    m_sessionTrigger->setAttribute(Qt::WA_StyledBackground, true);
    m_sessionTrigger->setFixedHeight(28);
    m_sessionTrigger->setCursor(Qt::PointingHandCursor);
    auto *triggerLayout = new QHBoxLayout(m_sessionTrigger);
    triggerLayout->setContentsMargins(8, 0, 8, 0);
    triggerLayout->setSpacing(7); // .session-trigger>*+* { margin-left:7px }

    // 不用 makeLabel：它带 TextSelectableByMouse，会吞掉鼠标事件导致点击不冒泡到触发器
    m_currentTitle = new QLabel(QStringLiteral("选择会话"), m_sessionTrigger);
    m_currentTitle->setObjectName(QStringLiteral("currentTitle"));
    m_currentTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_currentTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_currentTitle->setProperty("full", QStringLiteral("选择会话"));

    m_sessionChevron = new QLabel(m_sessionTrigger);
    setClass(m_sessionChevron, QStringLiteral("chevronGlyph"));
    m_sessionChevron->setPixmap(iconPixmap(QStringLiteral("chevron-down"), gs::palette().muted, 12));

    triggerLayout->addWidget(m_currentTitle, 1);
    triggerLayout->addWidget(m_sessionChevron);
    layout->addWidget(m_sessionTrigger, 1);

    // 视图切换并入本行右侧（refined/toolbar.css：.view-tabs { margin-left:auto }）
    buildViewTabs();
    layout->addWidget(m_viewTabs, 0, Qt::AlignVCenter);
    m_root->addWidget(bar);
}

void ChartWidget::buildViewTabs()
{
    // .view-tabs { margin-left:auto; gap:2px; border:0; background:transparent }
    // .view-tab { height:26px; padding:0 11px; border-radius:7px; font-size:11.5px }
    m_viewTabs = new QWidget(this);
    m_viewTabs->setObjectName(QStringLiteral("viewTabs"));
    m_viewTabs->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QHBoxLayout(m_viewTabs);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_tabChat = new QPushButton(QStringLiteral("对话"), m_viewTabs);
    setClass(m_tabChat, QStringLiteral("viewTab"));
    m_tabChat->setProperty("active", true);
    m_tabChat->setCursor(Qt::PointingHandCursor);
    m_tabChat->setFocusPolicy(Qt::TabFocus); // webui 的 .view-tab 是 button：可 Tab、不留点击焦点环
    m_tabChat->setFixedHeight(26);

    m_tabTraj = new QPushButton(QStringLiteral("轨迹"), m_viewTabs);
    setClass(m_tabTraj, QStringLiteral("viewTab"));
    m_tabTraj->setProperty("active", false);
    m_tabTraj->setCursor(Qt::PointingHandCursor);
    m_tabTraj->setFocusPolicy(Qt::TabFocus);
    m_tabTraj->setFixedHeight(26);

    layout->addWidget(m_tabChat);
    layout->addWidget(m_tabTraj);
}

void ChartWidget::buildMessages()
{
    // .messages { flex:1; overflow-y:auto; padding:20px 20px 8px }
    m_messages = new QScrollArea(this);
    m_messages->setObjectName(QStringLiteral("messages"));
    m_messages->setFrameShape(QFrame::NoFrame);
    m_messages->setWidgetResizable(true);
    m_messages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_messages->viewport()->setAutoFillBackground(false);

    m_messageList = new QWidget(m_messages);
    m_messageList->setObjectName(QStringLiteral("messageList"));
    m_messageLayout = new QVBoxLayout(m_messageList);
    // .message { margin:0 auto 18px }；左右外距由 syncContentMargins() 按主栏宽度写入
    m_messageLayout->setContentsMargins(20, 20, 20, 0);
    m_messageLayout->setSpacing(18);

    m_emptyState = createEmptyState();
    m_messageLayout->addWidget(m_emptyState, 1);
    m_messageLayout->addStretch(0);
    m_messages->setWidget(m_messageList);
    m_root->addWidget(m_messages, 1);
}

// 主栏宽度：消息区 / 阶段条 / 输入区共用一条竖线（refined 的 --content-max）
void ChartWidget::syncContentMargins()
{
    // 用宿主自身宽度算：resizeEvent 里 viewport 的宽度还没跟上，读到旧值会把 side 定死为 0
    const int side = gs::contentSidePadding(width());
    if (side == m_contentSide)
        return;
    m_contentSide = side;
    if (m_messageLayout)
        m_messageLayout->setContentsMargins(side, 20, side, 0);
    if (m_phaseLayout)
        m_phaseLayout->setContentsMargins(side, 0, side, 10);
    if (m_composer)
        m_composer->setContentSidePadding(side);
}

void ChartWidget::buildTurnRail()
{
    // .turn-rail：固定在视口左侧，竖排白点，一轮一个（不足一轮时隐藏）
    m_turnRail = new QWidget(this);
    m_turnRail->setObjectName(QStringLiteral("turnRail"));
    m_turnRail->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(m_turnRail);
    layout->setContentsMargins(0, 6, 0, 6);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignHCenter);
    m_turnRail->setVisible(false);
    // 鼠标进入轨道区域即展开右侧整轮列表（对应 app.js 的 mouseenter / mouseleave）
    m_turnRail->installEventFilter(this);

    // .turn-rail-panel：贴在轨道右侧展开，一轮一行（序号 + 提问摘要），点击行跳转
    m_turnRailPanel = new QFrame(this);
    m_turnRailPanel->setObjectName(QStringLiteral("turnRailPanel"));
    setClass(m_turnRailPanel, QStringLiteral("turnRailPanel"));
    m_turnRailPanel->setAttribute(Qt::WA_StyledBackground, true);
    // webui：width:300px;max-width:min(300px,calc(100vw - 26px))，宽度随窗口收缩
    m_turnRailPanel->setMaximumWidth(300);
    auto *panelLayout = new QVBoxLayout(m_turnRailPanel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    auto *head = new QWidget(m_turnRailPanel);
    setClass(head, QStringLiteral("turnRailHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(10, 7, 10, 7);
    headLayout->setSpacing(8);
    QLabel *headTitle = makeLabel(QStringLiteral("turnRailHeadTitle"), QStringLiteral("对话轮次"), head);
    headTitle->setTextInteractionFlags(Qt::NoTextInteraction);
    m_turnRailCount = makeLabel(QStringLiteral("turnRailHeadCount"), QString(), head);
    m_turnRailCount->setTextInteractionFlags(Qt::NoTextInteraction);
    headLayout->addWidget(headTitle);
    headLayout->addStretch(1);
    headLayout->addWidget(m_turnRailCount);
    panelLayout->addWidget(head);

    m_turnRailScroll = new QScrollArea(m_turnRailPanel);
    m_turnRailScroll->setObjectName(QStringLiteral("turnRailScroll"));
    m_turnRailScroll->setFrameShape(QFrame::NoFrame);
    m_turnRailScroll->setWidgetResizable(true);
    m_turnRailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_turnRailScroll->viewport()->setAutoFillBackground(false);
    m_turnRailList = new QWidget(m_turnRailScroll);
    m_turnRailList->setObjectName(QStringLiteral("turnRailList"));
    m_turnRailListLayout = new QVBoxLayout(m_turnRailList);
    m_turnRailListLayout->setContentsMargins(4, 4, 4, 4);
    m_turnRailListLayout->setSpacing(0);
    m_turnRailListLayout->addStretch(1);
    m_turnRailScroll->setWidget(m_turnRailList);
    panelLayout->addWidget(m_turnRailScroll, 1);

    m_turnRailPanel->setVisible(false);
    m_turnRailPanel->installEventFilter(this);
    // 离开轨道 / 列表留一点延迟再收起：从白点移到右侧列表要跨过间隙，不能让面板闪退
    m_turnRailHideTimer = new QTimer(this);
    m_turnRailHideTimer->setSingleShot(true);
    m_turnRailHideTimer->setInterval(180);
    connect(m_turnRailHideTimer, &QTimer::timeout, this, &ChartWidget::hideTurnRailPanel);
}

QWidget *ChartWidget::createEmptyState()
{
    // .empty-state { flex:1; column; center; text-align:center; padding:30px }
    auto *box = new QWidget(m_messageList);
    box->setObjectName(QStringLiteral("emptyState"));
    box->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(30, 30, 30, 30);
    layout->setSpacing(0);

    auto *symbol = new QLabel(box);
    symbol->setObjectName(QStringLiteral("emptySymbol"));
    symbol->setAttribute(Qt::WA_StyledBackground, true);
    symbol->setAlignment(Qt::AlignCenter);
    symbol->setFixedSize(58, 58);
    symbol->setPixmap(iconPixmap(QStringLiteral("zap"), gs::palette().cyan, 24));
    symbol->setContentsMargins(0, 0, 0, 20); // .empty-symbol { margin-bottom:20px }

    auto *title = new QLabel(QStringLiteral("对话已就绪"), box);
    title->setObjectName(QStringLiteral("emptyTitle"));
    title->setAlignment(Qt::AlignCenter);

    auto *hint = new QLabel(QStringLiteral("描述你的工程目标，Agent 将按当前模式执行。"), box);
    hint->setObjectName(QStringLiteral("emptyHint"));
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    // .empty-state p { max-width:270px; margin:7px 0 }：只给 max-width 时 QLabel 的 sizeHint
    // 会收到单行宽（实测 120px），两行文本被裁成一行且右侧截断；这里定宽再按字体度量给最小高
    hint->setFixedWidth(300);
    hint->setContentsMargins(0, 11, 0, 0);
    {
        const QFontMetrics metrics(hint->font());
        const QRect needed = metrics.boundingRect(QRect(0, 0, 300, 0), Qt::TextWordWrap, hint->text());
        hint->setMinimumHeight(needed.height() + 11); // + margin-top:11px
    }

    layout->addStretch(1);
    layout->addWidget(symbol, 0, Qt::AlignHCenter);
    layout->addWidget(title, 0, Qt::AlignHCenter);
    layout->addWidget(hint, 0, Qt::AlignHCenter);
    layout->addStretch(1);
    return box;
}

void ChartWidget::removeWelcome()
{
    if (!m_emptyState)
        return;
    m_messageLayout->removeWidget(m_emptyState);
    m_emptyState->deleteLater();
    m_emptyState = nullptr;
    const int last = m_messageLayout->count() - 1;
    if (last >= 0)
        m_messageLayout->setStretch(last, 1); // 有消息时尾部弹簧撑开，消息贴顶
}

void ChartWidget::updateEmptyState()
{
    bool hasMessages = false;
    for (int i = 0; i < m_messageLayout->count(); ++i) {
        QWidget *w = m_messageLayout->itemAt(i)->widget();
        if (w && w != m_emptyState) {
            hasMessages = true;
            break;
        }
    }
    if (hasMessages) {
        removeWelcome();
        return;
    }
    if (m_emptyState)
        return;
    m_emptyState = createEmptyState();
    m_messageLayout->insertWidget(0, m_emptyState, 1);
    const int last = m_messageLayout->count() - 1;
    if (last >= 0)
        m_messageLayout->setStretch(last, 0);
}

void ChartWidget::updateTitleElide()
{
    // #current-title { overflow:hidden; text-overflow:ellipsis; white-space:nowrap }
    const QString full = m_currentTitle->property("full").toString();
    const QString source = full.isEmpty() ? QStringLiteral("选择会话") : full;
    const QString elided = elidedText(source, m_currentTitle->fontMetrics(), m_currentTitle->width());
    if (m_currentTitle->text() == elided)
        return;
    m_currentTitle->setText(elided);
}

void ChartWidget::layoutOverlays()
{
    syncContentMargins();
    const QSize host = size();
    m_sessionPanel->layoutIn(host);
    // Toast 浮在输入区之上（输入区高度随控件行换行变化，不能用固定底距）；
    // 审批卡很局时会被压在卡片底下，所以再把卡片高度加上去（webui syncToastLift）
    const int composerBottom = m_composer ? m_composer->height() + gs::scaledPx(14) : 125;
    m_toast->layoutIn(host, composerBottom + (m_composer ? m_composer->choiceHeight() : 0));
    m_dropOverlay->layoutIn(host);
    updateTitleElide();

    // 轮次导航轨：左边缘、垂直居中，最高占视口 62%
    if (m_turnRail) {
        const int dotsHeight = m_railDots.size() * 18 + 12;
        const int railHeight = qMin(dotsHeight, qRound(host.height() * 0.62));
        const int top = qMax(0, (host.height() - railHeight) / 2);
        m_turnRail->setGeometry(0, top, 12, railHeight);
    }
    if (m_turnRailPanel && m_turnRailPanel->isVisible())
        layoutTurnRailPanel();
}

void ChartWidget::scrollToEnd(bool force)
{
    if (m_scrollLocked)
        return;
    if (force)
        m_followBottom = true;
    else if (!m_followBottom)
        return; // 贴底才跟随
    QScrollBar *bar = m_messages->verticalScrollBar();
    pinToBottom(3);
}

// 立刻跳到当前最大值，并在随后几轮事件循环里补跳：实时载入历史时消息控件是刚建出来的，
// 布局/滚动条范围要等 LayoutRequest 处理完才生效，单次 singleShot(0) 会读到 maximum()==0
// 而停在顶部（webui 载入历史后停在最新消息）。retries 次链式回调保证在范围定稿后贴底。
void ChartWidget::pinToBottom(int retries)
{
    QScrollBar *bar = m_messages->verticalScrollBar();
    bar->setValue(bar->maximum());
    if (retries <= 0)
        return;
    QTimer::singleShot(0, this, [this, retries] {
        if (!m_followBottom)
            return; // 用户已经往上滚了，别再抢滚动条
        pinToBottom(retries - 1);
    });
}

bool ChartWidget::atBottom() const
{
    // app.js BOTTOM_SLACK = 24
    QScrollBar *bar = m_messages->verticalScrollBar();
    return bar->maximum() - bar->value() <= 24;
}

// ------------------------------------------------------------ 轮次导航轨

void ChartWidget::rebuildTurnRail()
{
    if (!m_turnRail)
        return;
    QList<MessageWidget *> turns;
    if (m_viewTab == QLatin1String("chat")) {
        for (int i = 0; i < m_messageLayout->count(); ++i) {
            auto *message = qobject_cast<MessageWidget *>(m_messageLayout->itemAt(i)->widget());
            if (message && message->role() == QLatin1String("user"))
                turns.append(message);
        }
    }

    bool same = turns.size() == m_railTurns.size();
    if (same) {
        for (int i = 0; i < turns.size(); ++i) {
            if (m_railTurns.at(i).data() != turns.at(i)) {
                same = false;
                break;
            }
        }
    }
    if (!same) {
        hideTurnRailPanel();
        m_railTurns.clear();
        m_railDots.clear();
        m_railRows.clear();
        // 清空轨道上的旧点与列表面板里的旧行
        clearLayoutItems(m_turnRail->layout());
        if (m_turnRailListLayout) {
            clearLayoutItems(m_turnRailListLayout);
            m_turnRailListLayout->addStretch(1); // 行按顺序插到弹簧之前
        }
        m_turnRailCount->setText(turns.isEmpty() ? QString()
                                                 : QStringLiteral("%1 轮").arg(turns.size()));
        for (int i = 0; i < turns.size(); ++i) {
            const QString text = turnText(turns.at(i));
            auto *dot = new TurnRailDot(i, m_turnRail);
            connect(dot, &TurnRailDot::activated, this, &ChartWidget::focusTurn);
            if (auto *layout = qobject_cast<QVBoxLayout *>(m_turnRail->layout()))
                layout->addWidget(dot, 0, Qt::AlignHCenter);

            auto *row = new TurnRailRow(i, text, m_turnRailList);
            connect(row, &TurnRailRow::activated, this, [this](int turn) {
                focusTurn(turn);
                // 点击行即跳转并收起面板（与 app.js 一致；点白点不收起）
                hideTurnRailPanel();
            });
            m_turnRailListLayout->insertWidget(m_turnRailListLayout->count() - 1, row);

            m_railTurns.append(turns.at(i));
            m_railDots.append(dot);
            m_railRows.append(row);
        }
    }
    m_turnRail->setVisible(!turns.isEmpty());
    if (turns.isEmpty())
        hideTurnRailPanel();
    // 正文可能晚于行创建才回填（createMessage 收尾就会触发一次重建），就地刷新行文案
    for (int i = 0; i < m_railRows.size() && i < m_railTurns.size(); ++i) {
        if (MessageWidget *message = m_railTurns.at(i).data())
            m_railRows.at(i)->setTurnText(turnText(message));
    }
    if (m_turnRail->isVisible())
        layoutOverlays();
    updateTurnRailActive();
}

void ChartWidget::updateTurnRailActive()
{
    if (m_railTurns.isEmpty())
        return;
    const int mid = m_messages->viewport()->height() / 2;
    int active = 0;
    for (int i = 0; i < m_railTurns.size() && i < m_railDots.size(); ++i) {
        MessageWidget *message = m_railTurns.at(i).data();
        if (!message)
            continue;
        const int top = message->mapTo(m_messages->viewport(), QPoint(0, 0)).y();
        if (top <= mid)
            active = i;
    }
    m_railActive = active;
    for (int i = 0; i < m_railDots.size(); ++i)
        m_railDots.at(i)->setActive(i == active);
    // 轨道白点与列表行同步高亮（app.js updateTurnRailActive）
    for (int i = 0; i < m_railRows.size(); ++i)
        m_railRows.at(i)->setRowActive(i == active);
}

void ChartWidget::showTurnRailPanel()
{
    if (!m_turnRailPanel || m_railTurns.isEmpty())
        return;
    if (m_turnRailHideTimer)
        m_turnRailHideTimer->stop();
    layoutTurnRailPanel();
    m_turnRailPanel->setVisible(true);
    m_turnRailPanel->raise();
    updateTurnRailActive();
}

void ChartWidget::scheduleHideTurnRailPanel()
{
    if (!m_turnRailHideTimer)
        return;
    m_turnRailHideTimer->stop();
    m_turnRailHideTimer->start();
}

void ChartWidget::hideTurnRailPanel()
{
    if (m_turnRailHideTimer)
        m_turnRailHideTimer->stop();
    if (m_turnRailPanel)
        m_turnRailPanel->setVisible(false);
}

void ChartWidget::layoutTurnRailPanel()
{
    if (!m_turnRailPanel || !m_turnRail || !m_turnRailListLayout)
        return;
    // 面板贴在轨道右侧展开、纵向与轨道居中；越界时整体回推，避免被裁掉。
    // 高度按行数直接算，不依赖 sizeHint（首次展开时列表还没布局过，量不到高度）
    int headHeight = 30;
    if (QLayout *layout = m_turnRailPanel->layout()) {
        if (QLayoutItem *item = layout->itemAt(0)) {
            if (QWidget *head = item->widget()) {
                const int hint = head->sizeHint().height();
                if (hint > 0)
                    headHeight = hint;
            }
        }
    }
    const int listHeight = m_railRows.size() * 30 + 8;
    const int maxHeight = qMin(qRound(height() * 0.62), 420);
    // webui 只给 max-height：行数少时面板按内容收缩，行数多时截断并滚动
    const int panelHeight = qMax(0, qMin(headHeight + listHeight, maxHeight));
    const int panelWidth = qMin(300, qMax(0, width() - 26));
    const int x = m_turnRail->x() + m_turnRail->width() + 10;
    const int center = m_turnRail->y() + m_turnRail->height() / 2;
    int y = center - panelHeight / 2;
    y = qBound(8, y, qMax(8, height() - panelHeight - 8));
    m_turnRailPanel->setGeometry(x, y, panelWidth, panelHeight);
}

void ChartWidget::focusTurn(int turn)
{
    if (turn < 0 || turn >= m_railTurns.size())
        return;
    MessageWidget *message = m_railTurns.at(turn).data();
    if (!message)
        return;
    // 定位到该轮顶部（block:"start"）
    const int top = message->mapTo(m_messageList, QPoint(0, 0)).y();
    m_messages->verticalScrollBar()->setValue(qMax(0, top - 13));
}

// ------------------------------------------------------------ 视图页签

void ChartWidget::setViewTab(const QString &tab)
{
    if (m_viewTab == tab)
        return;
    m_viewTab = tab;
    const bool traj = tab == QLatin1String("traj");
    m_tabChat->setProperty("active", !traj);
    m_tabTraj->setProperty("active", traj);
    restyle(m_tabChat);
    restyle(m_tabTraj);
    m_trajView->setVisible(traj);
    m_messages->setVisible(!traj);
    m_composer->setVisible(!traj);
    if (traj) {
        m_phaseWrap->setVisible(false);
    } else if (!m_phasePanel->isHidden()) {
        // 用 isHidden 而不是 isVisible：切到轨迹时计划窗口是被父级隐藏的，
        // isVisible() 此时为假，会导致切回对话后再也恢复不出来
        m_phaseWrap->setVisible(true);
        scrollToEnd(true);
    }
    rebuildTurnRail();
    layoutOverlays();
    emit viewTabChanged(tab);
}

// ------------------------------------------------------------ 皮肤

void ChartWidget::setTheme(const QString &id)
{
    applyTheme(id);
    emit themeChanged(gs::themeId());
}

QString ChartWidget::theme() const
{
    return gs::themeId();
}

void ChartWidget::applyTheme(const QString &id)
{
    gs::setTheme(id);
    setStyleSheet(appStyleSheet());
    // Composer 的浮层（选择浮层 / 斜杠面板 / 上下文浮层）挂在它的宿主上，
    // 是 Composer 的兄弟而非后代：换肤重建全局 QSS 带不到它们，显式重烤一次
    if (m_composer)
        m_composer->refreshTheme();
    // 皮肤切换后已按旧色着色的图标要重建
    m_sessionChevron->setPixmap(iconPixmap(QStringLiteral("chevron-down"), gs::palette().muted, 12));
    m_connection->update();

    const QList<ThemeSwatch *> swatches = findChildren<ThemeSwatch *>();
    for (ThemeSwatch *swatch : swatches)
        swatch->update();

    // 图标按钮的着色需显式重设（QSS 的 color 不作用于 QIcon）
    for (IconPushButton *button : findChildren<IconPushButton *>()) {
        const QString cls = button->property("class").toString();
        const QString name = button->objectName();
        if (name == QLatin1String("sendButton"))
            continue;
        if (name == QLatin1String("newSession"))
            button->setIconColors(gs::palette().muted, gs::palette().cyan);
        else if (cls.contains(QLatin1String("sessionAction")))
            button->setIconColors(gs::palette().muted,
                                  button->property("danger").toBool() ? gs::palette().red
                                                                      : gs::palette().cyan);
        else
            button->setIconColors(gs::palette().muted2, gs::palette().cyan);
    }

    if (m_themeSwatch)
        m_themeSwatch->update();
    if (m_themeLabel)
        m_themeLabel->setText(themeName());
    if (m_themePopup)
        m_themePopup->setCurrent(gs::themeId());

    // 自绘控件（连接状态点、阶段项、进度条、导航轨…）重绘即可
    for (QWidget *w : findChildren<QWidget *>())
        w->update();
}

// ------------------------------------------------------------ 顶栏 / 会话栏

void ChartWidget::setConnectionState(const QString &state, const QString &label)
{
    if (!assertGuiThread(__func__))
        return;
    m_connection->setState(state, label);
}

void ChartWidget::setSessions(const QVariantList &sessions)
{
    if (!assertGuiThread(__func__))
        return;
    m_sessions = sessions;
    m_sessionPanel->setSessions(sessions);
}

void ChartWidget::setCurrentSessionTitle(const QString &title)
{
    if (!assertGuiThread(__func__))
        return;
    m_currentTitle->setProperty("full", title);
    updateTitleElide();
}

QString ChartWidget::currentSessionTitle() const
{
    return m_currentTitle->property("full").toString();
}

void ChartWidget::toggleSessionPanel()
{
    if (m_sessionPanel->isOpen()) {
        closeSessionPanel();
        return;
    }
    m_sessionPanel->setSessions(m_sessions);
    m_sessionPanel->layoutIn(size());
    m_sessionPanel->open();
}

void ChartWidget::closeSessionPanel()
{
    m_sessionPanel->closePanel();
}

// ------------------------------------------------------------ 输入区

void ChartWidget::setModels(const QVariantList &models)
{
    m_models = models; // 用量弹层的「提供方 / 模型」按 provider_name 映射（app.js usageModelLabel）
    m_composer->setModels(models);
    // 设置中心「用量」页的显示名映射也依赖这份目录（app.js usageSyncCatalog）
    m_settings->setUsageCatalog(models);
}
void ChartWidget::setCurrentModel(const QString &key) { m_composer->setCurrentModel(key); }
QString ChartWidget::currentModel() const { return m_composer->currentModel(); }

void ChartWidget::setSkills(const QVariantList &skills)
{
    m_skills = skills; // 供宿主查询；气泡不再显示 Skill 名（b12146e）
    m_composer->setSkills(skills);
}
void ChartWidget::setCurrentSkill(const QString &id) { m_composer->setCurrentSkill(id); }
QString ChartWidget::currentSkill() const { return m_composer->currentSkill(); }
void ChartWidget::setMode(const QString &mode) { m_composer->setMode(mode); }
QString ChartWidget::mode() const { return m_composer->mode(); }

// ------------------------------------------------------------ 分类 / 知识库 / 上下文

void ChartWidget::setCategories(const QVariantList &categories)
{
    m_composer->setCategories(categories);
}
void ChartWidget::setCurrentCategory(const QString &id) { m_composer->setCurrentCategory(id); }
QString ChartWidget::currentCategory() const { return m_composer->currentCategory(); }
void ChartWidget::syncCategoryFromSession(const QString &category)
{
    m_composer->syncCategoryFromSession(category);
}

void ChartWidget::setKnowledgeBases(const QVariantList &libraries)
{
    m_composer->setKnowledgeBases(libraries);
}
void ChartWidget::setCurrentKnowledgeBase(const QString &id)
{
    m_composer->setCurrentKnowledgeBase(id);
}
QString ChartWidget::currentKnowledgeBase() const { return m_composer->currentKnowledgeBase(); }

void ChartWidget::setContextUsage(const QVariantMap &usage) { m_composer->setContextUsage(usage); }
void ChartWidget::clearContextUsage() { m_composer->clearContextUsage(); }

void ChartWidget::setBusy(bool busy)
{
    m_composer->setBusy(busy);
    if (busy)
        return;
    // app.js renderFailure：sendMessage().finally(() => button.disabled = false)
    const QList<QPushButton *> buttons = m_messageList->findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
        if (button->property("retry").toBool())
            button->setEnabled(true);
}
bool ChartWidget::isBusy() const { return m_composer->isBusy(); }
void ChartWidget::setConfigLoaded(bool loaded) { m_composer->setConfigLoaded(loaded); }
void ChartWidget::setConfigWarning(const QString &text) { m_composer->setConfigWarning(text); }

QString ChartWidget::inputText() const { return m_composer->text(); }
void ChartWidget::setInputText(const QString &text) { m_composer->setText(text); }
void ChartWidget::focusInput() { m_composer->focusInput(); }

void ChartWidget::addAttachments(const QVariantList &items) { m_composer->addAttachments(items); }
void ChartWidget::clearAttachments() { m_composer->clearAttachments(); }
QVariantList ChartWidget::attachments() const { return m_composer->attachments(); }
void ChartWidget::setVoiceEnabled(bool enabled) { m_composer->setVoiceEnabled(enabled); }
void ChartWidget::setVoiceRecording(bool recording) { m_composer->setVoiceRecording(recording); }

// ------------------------------------------------------------ 选择 / 审批

void ChartWidget::showChoice(const QVariantMap &payload)
{
    if (!assertGuiThread(__func__))
        return;
    m_composer->showChoice(payload);
}

void ChartWidget::closeChoice()
{
    m_composer->closeChoice();
}

void ChartWidget::appendApproval(const QVariantMap &event)
{
    if (!assertGuiThread(__func__))
        return;
    QVariantMap approval;
    approval.insert(QStringLiteral("event"), event);
    QVariantMap payload;
    payload.insert(QStringLiteral("approval"), approval);
    showChoice(payload);
}

void ChartWidget::resolveApproval(const QString &callId, bool approved)
{
    m_composer->setApprovalResolved(callId, approved);
}

void ChartWidget::reEnableApproval(const QString &callId)
{
    m_composer->reEnableApproval(callId);
}

// ------------------------------------------------------------ 消息流

MessageWidget *ChartWidget::createMessage(const QString &role, const QString &content,
                                          const QString &label, const QVariantList &attachments)
{
    removeWelcome();
    auto *message = new MessageWidget(role, label, m_messageList);
    m_messageLayout->insertWidget(m_messageLayout->count() - 1, message); // 插在尾部弹簧之前
    message->setAttachments(attachments);
    message->body()->setText(content);
    message->setBodyVisible(message->body()->hasVisibleContent());
    // 兜底：先按传入正文当复制源；流式 / 结构化块路径会在收尾时用 setCopyText 覆盖成可见正文
    message->setCopyText(content);
    connect(message, &MessageWidget::copyRequested, this, [this](const QString &text) {
        // app.js 卡片拷贝：无内容时提示，成功/失败各给一条 toast
        const QString payload = text.isEmpty() ? QString() : text;
        if (!payload.trimmed().isEmpty()) {
            QApplication::clipboard()->setText(payload);
            showToast(QStringLiteral("已复制到剪贴板"));
        } else {
            showToast(QStringLiteral("没有可复制的内容"));
        }
    });
    scrollToEnd();
    rebuildTurnRail();
    return message;
}

MessageWidget *ChartWidget::ensureAssistant()
{
    if (m_current && !m_currentFinished)
        return m_current;
    // 气泡不再显示 Skill 名（b12146e），只保留失败/停止等状态标签
    m_current = createMessage(QStringLiteral("assistant"), QString());
    m_currentText.clear();
    m_currentFinished = false;
    return m_current;
}

void ChartWidget::appendUserMessage(const QString &content, const QVariantList &attachments)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *message = createMessage(QStringLiteral("user"), content, QString(), attachments);
    // property 供轮次轨摘要（turnText）读；复制按钮读的是 m_copyText，两个都要写，
    // 否则用户消息的复制按钮就只会弹「没有可复制的内容」
    message->setProperty("copyText", content);
    message->setCopyText(content);
    // app.js sendMessage：新一轮提问必须落底并恢复自动跟随，即使上一轮用户上滚停留在历史里
    scrollToEnd(true);
}

void ChartWidget::appendAssistantMessage(const QString &content, const QString &label,
                                         const QVariantList &attachments)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *message = createMessage(QStringLiteral("assistant"), content, label, attachments);
    // 一次性追加：正文里的结构化块立即渲染成卡片（等价于 createMessage + finishAssistant）
    const StructuredBlocks parsed = structuredBlocks(content);
    message->body()->setText(parsed.visible);
    message->setBodyVisible(message->body()->hasVisibleContent());
    message->setCopyText(parsed.visible);
    QVariantMap ask;
    for (const QVariant &item : parsed.found) {
        const QVariantMap data = item.toMap();
        renderStructured(data, message);
        const QVariantMap candidate = askPayloadOf(data);
        if (!candidate.isEmpty())
            ask = candidate;
    }
    if (!ask.isEmpty())
        m_composer->showChoice(ask);
    scrollToEnd();
}

void ChartWidget::appendAssistantText(const QString &delta)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *message = ensureAssistant();
    m_currentText += delta;
    message->body()->setText(m_currentText);
    message->setBodyVisible(message->body()->hasVisibleContent());
    message->setCopyText(m_currentText);
    message->settleThink();
    scrollToEnd();
}

void ChartWidget::appendReasoning(const QString &delta)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *message = ensureAssistant();
    message->appendReasoning(delta);
    scrollToEnd();
}

void ChartWidget::finishAssistant()
{
    if (!assertGuiThread(__func__))
        return;
    finishAssistantInternal(false);
}

// 每轮收尾都要过一遍：计划窗口只服务执行过程，本轮结束时计划已全部完成就收起。
// 放在空轮兜底之前——那一支会提前 return，否则空正文的那一轮会把计划窗口留在屏上
void ChartWidget::settlePhasePlan()
{
    if (planComplete(m_phasePlanData))
        hidePhasePanel();
}

void ChartWidget::finishAssistantInternal(bool deferred)
{
    MessageWidget *message = m_current;
    if (!message || m_currentFinished)
        return;
    m_currentFinished = true;

    message->stopLiveTiming();
    message->settleProcess();

    // 先结算计划窗口，后面的空轮兜底可能直接返回
    settlePhasePlan();

    const StructuredBlocks parsed = structuredBlocks(m_currentText);
    message->body()->setText(parsed.visible);
    message->setBodyVisible(message->body()->hasVisibleContent());
    message->setCopyText(parsed.visible);

    // 文本兜底的 options / tool_params 块也算一次待作答询问，取最后一个候选块
    QVariantMap ask;
    for (const QVariant &item : parsed.found) {
        const QVariantMap data = item.toMap();
        renderStructured(data, message);
        const QVariantMap candidate = askPayloadOf(data);
        if (!candidate.isEmpty())
            ask = candidate;
    }
    m_lastTurnAwaiting = !ask.isEmpty();

    // 只有思考过程/工具调用、没有正文时也要留住这一轮
    if (parsed.visible.trimmed().isEmpty() && parsed.found.isEmpty()
        && !hasToolItems(message) && !hasProcRows(message)) {
        if (m_current.data() == message)
            m_current = nullptr;
        message->deleteLater();
        rebuildTurnRail();
        return;
    }

    // 历史重放要等整轮重放完再弹，否则中途那些旧询问会闪一下
    if (!deferred && !ask.isEmpty())
        m_composer->showChoice(ask);

    m_currentText.clear();
    updateEmptyState();
    rebuildTurnRail();
    syncPhaseLift();
    scrollToEnd();
}

bool ChartWidget::planComplete(const QVariantMap &plan)
{
    const QVariantList phases = plan.value(QStringLiteral("phases")).toList();
    if (phases.isEmpty())
        return false;
    static const QStringList doneStatuses{ QStringLiteral("done"), QStringLiteral("succeeded"),
                                           QStringLiteral("completed"), QStringLiteral("skipped") };
    for (const QVariant &item : phases) {
        if (!doneStatuses.contains(item.toMap().value(QStringLiteral("status")).toString()))
            return false;
    }
    return true;
}

void ChartWidget::renderStructured(const QVariantMap &data, MessageWidget *message)
{
    if (!message)
        return;

    if (data.contains(QStringLiteral("phase_plan")))
        setPhasePlan(data.value(QStringLiteral("phase_plan")));

    // tool_params / options 合并到输入框上方的浮层（见 ChoiceOverlay），对话流里不再单独成卡
    if (data.contains(QStringLiteral("workflow"))) {
        const QVariantList steps =
            data.value(QStringLiteral("workflow")).toMap().value(QStringLiteral("steps")).toList();
        auto *card = new WorkflowProposalCard(steps, message);
        connect(card, &WorkflowProposalCard::runRequested, this,
                &ChartWidget::workflowRunRequested);
        message->stack()->addWidget(card);
    }
}

void ChartWidget::appendToolCall(const QString &callId, const QString &name, const QVariant &args)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *message = ensureAssistant();
    if (!message)
        return;
    message->settleThink();
    // app.js handleStreamEvent：询问类调用先落定思考，但不渲染工具条目
    if (name == QLatin1String(kAskUserTool))
        return;
    ToolItemWidget *item = message->addToolCall(callId, name, args);
    if (item && !callId.isEmpty())
        m_toolItems.insert(callId, item);
    scrollToEnd();
}

void ChartWidget::appendToolResult(const QString &callId, const QString &name,
                                   const QString &result)
{
    if (!assertGuiThread(__func__))
        return;
    // app.js handleStreamEvent：询问类调用的结果同样不落成工具条目
    if (name == QLatin1String(kAskUserTool))
        return;
    ToolItemWidget *item = m_toolItems.value(callId, nullptr);
    if (!item) {
        // app.js renderToolResult：调用项不存在时先补一条 running 的调用
        MessageWidget *message = ensureAssistant();
        if (!message)
            return;
        item = message->addToolCall(callId, name, QVariantMap());
        if (item && !callId.isEmpty())
            m_toolItems.insert(callId, item);
    }
    if (!item)
        return;
    applyToolResult(item, result);
    scrollToEnd();
}

void ChartWidget::appendWorkflowEvent(const QVariantMap &event)
{
    if (!assertGuiThread(__func__))
        return;
    const QString type = event.value(QStringLiteral("type")).toString();
    if (!m_workflow || type == QLatin1String("workflow_started")) {
        MessageWidget *message =
            createMessage(QStringLiteral("workflow"), QString(), QStringLiteral("WORKFLOW"));
        message->setBodyVisible(false); // app.js: message.body.remove()
        auto *card = new WorkflowRunCard(message);
        message->stack()->addWidget(card);
        m_workflow = card;
        m_workflowMessage = message;
        m_workflowSteps.clear();
    }
    if (!m_workflow)
        return;
    if (type == QLatin1String("workflow_step")) {
        const int index = event.value(QStringLiteral("index")).toInt();
        if (index >= 0) {
            // app.js steps[event.index] = event：稀疏数组用无效 QVariant 占位
            while (m_workflowSteps.size() <= index)
                m_workflowSteps.append(QVariant());
            m_workflowSteps[index] = event;
        }
    }
    m_workflow->setSteps(m_workflowSteps);
    if (type == QLatin1String("workflow_done"))
        m_workflow->setStatus(event.value(QStringLiteral("status")).toString());
    scrollToEnd();
}

void ChartWidget::setTokenUsage(qint64 total, qint64 input, qint64 output, bool estimated)
{
    QVariantMap usage;
    usage.insert(QStringLiteral("total"), total);
    usage.insert(QStringLiteral("input"), input);
    usage.insert(QStringLiteral("output"), output);
    usage.insert(QStringLiteral("estimated"), estimated);
    setTokenUsageDetail(usage);
}

void ChartWidget::setTokenUsageDetail(const QVariantMap &usage)
{
    if (!assertGuiThread(__func__))
        return;
    if (!m_current)
        return;
    QVariantMap detail = usage;
    // 宿主没给 model_label 时按 usage.model 自动映射供应商名称（用量弹层的「提供方 / 模型」）
    if (!detail.contains(QStringLiteral("model_label"))) {
        const QString label =
            usageModelLabel(m_models, detail.value(QStringLiteral("model")).toString());
        if (!label.isEmpty())
            detail.insert(QStringLiteral("model_label"), label);
    }
    m_current->setUsage(detail);
}

void ChartWidget::setTurnTiming(const QVariantMap &timing)
{
    if (!assertGuiThread(__func__))
        return;
    if (!m_current)
        return;
    m_current->setTiming(timing);
}

void ChartWidget::startLiveTiming(qint64 startMs)
{
    if (!assertGuiThread(__func__))
        return;
    if (!m_current)
        return;
    m_current->startLiveTiming(startMs);
}

void ChartWidget::appendToolResultMessage(const QString &content)
{
    // 工具结果找不到对应调用时的退化形态
    MessageWidget *message =
        createMessage(QStringLiteral("tool"), QString(), QStringLiteral("TOOL RESULT"));
    message->setBodyVisible(false);
    if (ToolItemWidget *item = message->addToolCall(QString(), QString(), QVariantMap()))
        applyToolResult(item, content);
    scrollToEnd();
}

void ChartWidget::appendFailure(const QString &text, bool retryable, const QString &retryMessage,
                                const QString &retryDisplay, const QVariantList &retryAttachments)
{
    if (!assertGuiThread(__func__))
        return;
    MessageWidget *notice = createMessage(QStringLiteral("assistant"), text,
                                          retryable ? QStringLiteral("RETRY")
                                                    : QStringLiteral("ERROR"));
    notice->setCardError(true); // app.js renderFailure：错误卡整圈红边
    if (!retryable)
        return;

    auto *button = new QPushButton(QStringLiteral("重发这条消息"), notice);
    setClass(button, QStringLiteral("actionButton"));
    button->setProperty("retry", true);
    button->setCursor(Qt::PointingHandCursor);
    QVBoxLayout *bubbleLayout =
        qobject_cast<QVBoxLayout *>(notice->findChild<QFrame *>(QStringLiteral("bubble"))->layout());
    if (bubbleLayout) {
        bubbleLayout->addSpacing(10); // button.style.marginTop = "10px"
        bubbleLayout->addWidget(button);
    }
    connect(button, &QPushButton::clicked, this,
            [this, button, retryMessage, retryDisplay, retryAttachments] {
                if (m_composer->isBusy()) {
                    showToast(QStringLiteral("当前还有请求在处理中"));
                    return;
                }
                button->setEnabled(false);
                emit retryRequested(retryMessage, retryDisplay, retryAttachments);
            });
}

void ChartWidget::markCurrentStopped()
{
    if (m_current)
        m_current->markStopped();
}

void ChartWidget::setPhasePlan(const QVariant &value)
{
    if (!assertGuiThread(__func__))
        return;
    QVariantMap phase = extractPhase(value);
    if (phase.isEmpty())
        phase = value.toMap(); // app.js renderPhase: extractPhase(value) || value
    const QVariantList phases = phase.value(QStringLiteral("phases")).toList();
    if (phases.isEmpty())
        return;
    // 计划窗口只服务执行过程：全部阶段进入终态就收起，下一条 plan_updated 会再次出现。
    // 规则放在渲染口（app.js renderPhase 开头），实时 plan_updated、切会话加载、历史回放三条路都绕不过去
    if (planComplete(phase)) {
        hidePhasePanel();
        return;
    }
    m_phasePlanData = phase;
    m_phaseWrap->setVisible(true);
    m_phasePanel->setPlan(phase);
    syncPhaseLift();
}

// 收起计划窗口：面板内容与 state 一起清掉，避免切会话后残留旧计划（app.js hidePhasePanel）
void ChartWidget::hidePhasePanel()
{
    m_phasePlanData.clear();
    m_phaseWrap->setVisible(false);
    m_phasePanel->clearPlan();
}

void ChartWidget::showToast(const QString &text)
{
    if (!assertGuiThread(__func__))
        return;
    m_toast->showMessage(text);
}

void ChartWidget::syncPhaseLift()
{
    // 选择浮层浮在输入框上方，会盖住紧贴其上的计划窗口：浮层出现/变高时把计划窗口顶开
    if (!m_phaseWrap || !m_phaseLiftSpacer)
        return;
    auto *layout = qobject_cast<QVBoxLayout *>(m_phaseWrap->layout());
    if (!layout)
        return;
    const int lift = m_composer->choiceOpen()
                         ? m_composer->choiceHeight() + 12 - m_composer->height()
                         : 0;
    m_phaseLiftSpacer->changeSize(0, qMax(0, lift), QSizePolicy::Fixed,
                                  QSizePolicy::Fixed);
    layout->invalidate();
    static_cast<PhaseLiftLayout *>(m_root)->setPhaseOverlap(qMax(0, -lift));
}

void ChartWidget::setTrajectoryEvents(const QVariantList &events)
{
    if (!assertGuiThread(__func__))
        return;
    m_trajView->setEvents(events);
}

void ChartWidget::appendTrajectoryEvents(const QVariantList &events)
{
    if (!assertGuiThread(__func__))
        return;
    m_trajView->appendEvents(events);
}

void ChartWidget::clearMessages()
{
    if (!assertGuiThread(__func__))
        return;
    for (int i = m_messageLayout->count() - 1; i >= 0; --i) {
        QWidget *w = m_messageLayout->itemAt(i)->widget();
        if (!w || w == m_emptyState)
            continue;
        m_messageLayout->removeWidget(w);
        w->deleteLater();
    }
    m_current = nullptr;
    m_currentText.clear();
    m_currentFinished = true;
    m_toolItems.clear();
    m_workflow = nullptr;
    m_workflowMessage = nullptr;
    m_workflowSteps.clear();
    m_lastTurnAwaiting = false;
    m_phasePlanData.clear();
    m_phaseWrap->setVisible(false); // loadSession 走 hidePhasePanel()：收起要连面板内容一起清
    m_phasePanel->clearPlan();
    m_composer->closeChoice();
    updateEmptyState();
    rebuildTurnRail();
}

void ChartWidget::setHistory(const QVariantList &messages, bool resume)
{
    if (!assertGuiThread(__func__))
        return;
    clearMessages();

    // 一条 user 消息之后、下一条 user/workflow 消息之前的 assistant/tool 消息属于同一轮
    QPointer<MessageWidget> turn;
    QString turnText;
    QString turnTs;
    QVariantMap turnUsage;
    QVariantMap turnTiming;
    QVariantMap lastAsk;

    auto finishTurn = [&]() {
        if (turn) {
            m_current = turn.data();
            m_currentText = turnText;
            m_currentFinished = false;
            finishAssistantInternal(true);
            if (turn) {
                if (!turnUsage.isEmpty())
                    turn->setUsage(turnUsage);
                turn->setTimeStamp(turnTs);
                if (!turnTiming.isEmpty())
                    turn->setTiming(turnTiming);
            }
            // 只有「最后一轮」停在未作答询问上才恢复浮层：后一轮结束时必须清掉，
            // 否则历史里早期已作答的询问会一直粘着（对齐 app.js:1418 的 last.awaitingInput 判断）
            lastAsk = (turn && turn->property("awaiting").toBool())
                          ? turn->property("pendingAsk").toMap()
                          : QVariantMap();
        }
        turn = nullptr;
        turnText.clear();
        turnTs.clear();
        turnUsage.clear();
        turnTiming.clear();
        m_current = nullptr;
        m_currentText.clear();
        m_currentFinished = true;
    };

    for (const QVariant &entry : messages) {
        const QVariantMap message = entry.toMap();
        const QString role = message.value(QStringLiteral("role")).toString();

        if (role == QLatin1String("assistant")) {
            if (!turn) {
                turn = createMessage(QStringLiteral("assistant"), QString());
                m_current = turn.data();
                m_currentText.clear();
                m_currentFinished = false;
            }
            if (message.value(QStringLiteral("interrupted")).toBool())
                turn->markStopped();
            const QString content = message.value(QStringLiteral("content")).toString();
            if (!content.isEmpty()) {
                if (!turnText.isEmpty())
                    turnText += QStringLiteral("\n\n");
                turnText += content;
                turn->body()->setText(turnText);
                turn->setBodyVisible(turn->body()->hasVisibleContent());
                turn->setCopyText(turnText);
            }
            const QString reasoning = message.value(QStringLiteral("reasoning_content")).toString();
            if (!reasoning.isEmpty())
                turn->appendReasoning(reasoning);
            const QVariantList calls = message.value(QStringLiteral("tool_calls")).toList();
            for (const QVariant &callVar : calls) {
                const QVariantMap call = callVar.toMap();
                const QVariantMap function = call.value(QStringLiteral("function")).toMap();
                const QString callId = call.value(QStringLiteral("id")).toString();
                const QString name = function.value(QStringLiteral("name")).toString();
                const QString raw = function.value(QStringLiteral("arguments")).toString();
                const QJsonDocument doc =
                    QJsonDocument::fromJson(raw.isEmpty() ? QByteArray("{}") : raw.toUtf8());
                const QVariant args = doc.isObject() ? QVariant(doc.object().toVariantMap())
                                                     : QVariant(QVariantMap());
                // 询问类调用不落成工具条目：末尾待作答时由输入框上方的浮层弹出
                if (name == QLatin1String(kAskUserTool)) {
                    turn->setProperty("awaiting", true);
                    turn->setProperty("pendingAsk", args);
                    continue;
                }
                ToolItemWidget *item = turn->addToolCall(callId, name, args);
                if (item && !callId.isEmpty())
                    m_toolItems.insert(callId, item);
            }
            if (message.contains(QStringLiteral("usage")))
                turnUsage = message.value(QStringLiteral("usage")).toMap();
            const QString ts = message.value(QStringLiteral("ts")).toString();
            if (!ts.isEmpty())
                turnTs = ts;
            // 本轮可能由多条 assistant 消息合并，取最后一条的耗时字段
            if (message.contains(QStringLiteral("elapsed_ms")))
                turnTiming.insert(QStringLiteral("elapsed"),
                                  message.value(QStringLiteral("elapsed_ms")));
            if (message.contains(QStringLiteral("think_ms")))
                turnTiming.insert(QStringLiteral("think"), message.value(QStringLiteral("think_ms")));
            if (message.contains(QStringLiteral("ttft_ms")))
                turnTiming.insert(QStringLiteral("ttft"), message.value(QStringLiteral("ttft_ms")));
            if (message.contains(QStringLiteral("tps")))
                turnTiming.insert(QStringLiteral("tps"), message.value(QStringLiteral("tps")));
            continue;
        }

        if (role == QLatin1String("tool")) {
            if (message.value(QStringLiteral("tool_name")).toString()
                == QLatin1String(kAskUserTool))
                continue;
            const QString callId = message.value(QStringLiteral("tool_call_id")).toString();
            ToolItemWidget *item = m_toolItems.value(callId, nullptr);
            MessageWidget *parent = item ? ownerMessage(item) : nullptr;
            if (!parent) {
                // 调用没落盘时才退化为独立 TOOL RESULT 气泡
                parent = createMessage(QStringLiteral("tool"), QString(),
                                       QStringLiteral("TOOL RESULT"));
                parent->setBodyVisible(false);
                item = parent->addToolCall(callId,
                                           message.value(QStringLiteral("tool_name")).toString(),
                                           QVariantMap());
                if (item && !callId.isEmpty())
                    m_toolItems.insert(callId, item);
            }
            if (item)
                applyToolResult(item, message.value(QStringLiteral("content")).toString());
            continue; // turn 保持不变
        }

        finishTurn();

        if (role == QLatin1String("user")) {
            QString shown = message.value(QStringLiteral("display_content")).toString();
            if (shown.isEmpty())
                shown = message.value(QStringLiteral("content")).toString();
            MessageWidget *item = createMessage(QStringLiteral("user"), shown, QString(),
                                               message.value(QStringLiteral("attachments")).toList());
            item->setProperty("copyText", shown);
            item->setCopyText(shown);
            item->setTimeStamp(message.value(QStringLiteral("ts")).toString());
        } else if (role == QLatin1String("workflow")) {
            QVariantMap started;
            started.insert(QStringLiteral("type"), QStringLiteral("workflow_started"));
            appendWorkflowEvent(started);
            const QVariantList steps = message.value(QStringLiteral("steps")).toList();
            for (int i = 0; i < steps.size(); ++i) {
                QVariantMap step = steps.at(i).toMap();
                step.insert(QStringLiteral("type"), QStringLiteral("workflow_step"));
                step.insert(QStringLiteral("index"), i);
                appendWorkflowEvent(step);
            }
            QVariantMap done;
            done.insert(QStringLiteral("type"), QStringLiteral("workflow_done"));
            done.insert(QStringLiteral("status"), message.value(QStringLiteral("status")).toString());
            done.insert(QStringLiteral("message"),
                        message.value(QStringLiteral("message")).toString());
            appendWorkflowEvent(done);
        }
    }
    finishTurn();

    // 历史最后一轮停在未作答的询问上才弹浮层（供宿主恢复「待确认」徽标）。
    // resume（重连/续跑）：末尾那轮的询问已经被本轮 user 消息回答过了，不能再弹，
    // 否则刷新回来后会把已审批的旧参数卡重新弹回输入框上方（webui c4a6d84）
    m_lastTurnAwaiting = !resume && !lastAsk.isEmpty();
    if (resume)
        m_composer->closeChoice();
    else if (m_lastTurnAwaiting)
        m_composer->showChoice(lastAsk);
    scrollToEnd(true);
    rebuildTurnRail();
}

// ------------------------------------------------------------ 设置中心

void ChartWidget::setSettingsDraft(const QVariantMap &config, const QVariant &revision)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->loadConfig(config, revision);
}
void ChartWidget::setDiscoveredModels(const QString &providerId, const QVariantList &models)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setDiscoveredModels(providerId, models);
}

void ChartWidget::setModelMetadata(const QVariantMap &snapshot)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setModelMetadata(snapshot);
}

void ChartWidget::setModelMetadataFailed(const QString &kind, const QString &error, int httpStatus)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setModelMetadataFailed(kind, error, httpStatus);
}
void ChartWidget::setProviderBusy(bool testing, bool reading)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setProviderBusy(testing, reading);
}
void ChartWidget::setSettingsSkills(const QVariantList &skills, bool loading, const QString &error)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setSkills(skills, loading, error);
}
void ChartWidget::setMcpTools(const QVariantList &tools, bool connected, bool loading,
                              const QString &error)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setMcpTools(tools, connected, loading, error);
}
void ChartWidget::setSettingsStatus(const QString &text)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setStatus(text);
}

// ------------------------------------------------------------ 设置中心：记忆 / 知识库

void ChartWidget::setMemorySummary(const QVariantMap &summary)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setMemorySummary(summary);
}

void ChartWidget::setMemoryExperiences(const QVariantMap &listing)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setMemoryExperiences(listing);
}

void ChartWidget::setMemoryListFailed(const QString &error)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setMemoryListFailed(error);
}

void ChartWidget::setMemoryTools(const QVariantMap &payload)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setMemoryTools(payload);
}

void ChartWidget::setMemoryStatusText(const QString &text)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setMemoryStatusText(text);
}

void ChartWidget::setKbLibraries(const QVariantList &libraries)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setKbLibraries(libraries);
}

void ChartWidget::setKbStatus(const QVariantMap &status)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setKbStatus(status);
}

void ChartWidget::setKbFolders(const QVariantList &folders, const QVariantMap &tree)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setKbFolders(folders, tree);
}

void ChartWidget::setKbDocuments(const QVariantList &documents, int total)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setKbDocuments(documents, total);
}

void ChartWidget::setKbSearchResult(const QVariantMap &result)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setKbSearchResult(result);
}

void ChartWidget::setKbStatusText(const QString &text)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setKbStatusText(text);
}

void ChartWidget::setUsageStats(const QString &requestId, const QVariantMap &data)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setUsageStats(requestId, data);
}

void ChartWidget::setUsageLoadFailed(const QString &requestId, const QString &error)
{
    if (!assertGuiThread(__func__))
        return;
    m_settings->setUsageLoadFailed(requestId, error);
}

void ChartWidget::openSettings(const QString &tab)
{
    // 显示名映射依赖这份目录，进入用量页前先备好（app.js usageEnsureCatalog）
    m_settings->setUsageCatalog(m_models);
    if (!tab.isEmpty())
        m_settings->switchTab(tab);
    m_settings->show();
    // show() 之后补：QSS 在展示时才生效，字距必须在这之后设才不会被样式覆盖
    applyDeferredStyleDetails(m_settings);
    m_settings->raise();
    m_settings->activateWindow();
    // 告诉宿主设置中心被打开了（哪个 Tab）：它据此拉 model-metadata / memory summary /
    // kb libraries 这类“打开才需要”的数据。以前控件自带的设置按钮那一路宿主无感
    emit settingsOpened(!tab.isEmpty() ? tab : m_settings->activeTab());
}

void ChartWidget::closeSettings()
{
    m_settings->close(); // 必须 close()：hide() 不会触发 closeEvent 的脏检查
}

void ChartWidget::settingsSaved()
{
    m_settings->finishSaved();
}

// ------------------------------------------------------------ 拖拽

void ChartWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    event->acceptProposedAction();
    ++m_dragDepth;
    m_dropOverlay->setActive(true);
}

void ChartWidget::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    event->acceptProposedAction(); // 默认 ignore 会让后续 drop 被拒
}

void ChartWidget::dragLeaveEvent(QDragLeaveEvent *event)
{
    Q_UNUSED(event);
    m_dragDepth = qMax(0, m_dragDepth - 1);
    if (m_dragDepth == 0)
        m_dropOverlay->setActive(false);
}

void ChartWidget::dropEvent(QDropEvent *event)
{
    m_dragDepth = 0;
    m_dropOverlay->setActive(false);
    if (!event->mimeData()->hasUrls())
        return;
    // 类型/大小/数量校验与上传都由宿主完成，这里只回报落下的本地文件
    QVariantList items;
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        const QFileInfo info(path);
        if (!info.isFile())
            continue;
        QVariantMap item;
        item.insert(QStringLiteral("name"), info.fileName());
        item.insert(QStringLiteral("path"), path);
        item.insert(QStringLiteral("size"), info.size());
        item.insert(QStringLiteral("ext"), attachExt(info.fileName()));
        items.append(item);
    }
    if (items.isEmpty())
        return;
    event->acceptProposedAction();
    emit attachmentsAdded(items);
}

// ------------------------------------------------------------ 事件

void ChartWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    syncContentMargins();
    layoutOverlays();
}

bool ChartWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_currentTitle && event->type() == QEvent::Resize) {
        updateTitleElide();
        return false;
    }
    if (watched == m_composer && event->type() == QEvent::Resize) {
        layoutOverlays();
        return false;
    }
    // 轮次面板：轨道与面板各自算悬停区，跨间隙留 180ms 延迟收起（app.js 同）
    if (watched == m_turnRail) {
        if (event->type() == QEvent::Enter)
            showTurnRailPanel();
        else if (event->type() == QEvent::Leave)
            scheduleHideTurnRailPanel();
        return false;
    }
    if (watched == m_turnRailPanel) {
        if (event->type() == QEvent::Enter) {
            if (m_turnRailHideTimer)
                m_turnRailHideTimer->stop();
        } else if (event->type() == QEvent::Leave) {
            scheduleHideTurnRailPanel();
        }
        return false;
    }

    auto *target = qobject_cast<QWidget *>(watched);
    switch (event->type()) {
    case QEvent::MouseButtonRelease: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton || !target)
            break;
        if (isSelfOrChildOf(target, m_sessionTrigger)) {
            toggleSessionPanel();
            return true; // 拦住，避免事件沿 parentWidget() 链再触发一次
        }
        if (isSelfOrChildOf(target, m_themeTrigger)) {
            m_themePopup->setCurrent(gs::themeId());
            m_themePopup->openBelow(m_themeTrigger);
            return true;
        }
        // 展开/收起过程行或工具项会把内容顶高：下一帧重判是否仍贴底（webui 同），
        // 免得紧接着到来的流式分片把用户刚展开的位置拽走
        if (hasClassOrAncestor(target, "procRowHead")
            || hasClassOrAncestor(target, "toolItemSummary")) {
            QTimer::singleShot(0, this, [this] { m_followBottom = atBottom(); });
        }
        break;
    }
    case QEvent::Polish:
        // qApp 过滤器会收到全应用的事件：只处理自己的后代，
        // 否则会给宿主控件（恰好同名 class）也套上字距 / 放开焦点
        if (target && isSelfOrChildOf(target, this)) {
            applyLetterSpacing(target);
            applyAccessibility(target);
        }
        break;
    case QEvent::MouseButtonPress:
        // QWidget 默认会忽略按下事件并冒泡到父级；若不拦住，点击会话触发器时
        // 父级会被误判为“面板外”，先关闭、松开又重开，表现为无法再次点击收起。
        if (target && isSelfOrChildOf(target, m_sessionTrigger))
            return true;
        // 斜杠面板不是 Qt::Popup，点面板外得自己收（webui 的 document 点击处理）
        if (target && m_composer && m_composer->slashOpen())
            m_composer->dismissSlashForClick(target);
        if (m_sessionPanel->isOpen() && target
            && !isSelfOrChildOf(target, m_sessionTrigger)
            && !isSelfOrChildOf(target, m_sessionPanel)
            && target->window() == window()) // 别的窗口的点击不算「点面板外」
            closeSessionPanel();             // 点面板外关闭，但不拦截事件本身
        break;
    case QEvent::KeyPress:
        // 只处理发生在自己身上的 Esc：否则会把宿主窗口/宿主对话框的 Esc 一起吞掉
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape && target
            && isSelfOrChildOf(target, this) && !(m_settings && m_settings->isVisible())) {
            if (m_sessionPanel->isOpen()) {
                closeSessionPanel();
                return true;
            }
            // Esc 收起询问浮层，露出被盖住的输入框；审批卡要等后端回执，不放行
            if (m_composer->choiceOpen() && m_composer->approvalCallId().isEmpty()) {
                closeChoice();
                return true;
            }
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

// ------------------------------------------------------------ 界面缩放

// 缩放档位（VSCode 风格：离散步进，Ctrl+0 回到 100%）
static const qreal kZoomSteps[] = {0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.4, 1.6};
static const int kZoomStepCount = int(sizeof(kZoomSteps) / sizeof(kZoomSteps[0]));

void ChartWidget::zoomIn()
{
    for (int i = 0; i < kZoomStepCount; ++i) {
        if (kZoomSteps[i] > gs::zoomFactor() + 0.001) {
            gs::setZoomFactor(kZoomSteps[i]);
            applyZoom();
            return;
        }
    }
}

void ChartWidget::zoomOut()
{
    for (int i = kZoomStepCount - 1; i >= 0; --i) {
        if (kZoomSteps[i] < gs::zoomFactor() - 0.001) {
            gs::setZoomFactor(kZoomSteps[i]);
            applyZoom();
            return;
        }
    }
}

void ChartWidget::zoomReset()
{
    if (qFuzzyCompare(gs::zoomFactor(), 1.0))
        return;
    gs::setZoomFactor(1.0);
    applyZoom();
}

qreal ChartWidget::zoomFactor() const { return gs::zoomFactor(); }

void ChartWidget::applyZoom()
{
    // 1) 根字体 + 全局 QSS 重设：子控件与设置中心对话框（子窗口）继承生效
    QFont f(uiFont());
    f.setPixelSize(scaledPx(basePixelSize()));
    setFont(f);
    setStyleSheet(appStyleSheet());

    // 2) 创建时烘焙的内联 HTML 参数标签：按存好的原文用新字号重建
    const QList<QLabel *> labels = findChildren<QLabel *>();
    for (QLabel *label : labels) {
        const QVariant bold = label->property("paramBold");
        if (!bold.isValid())
            continue;
        const QString descPart = label->property("paramDesc").toString();
        QString html = QStringLiteral("<span style=\"color:%1;font-size:%2px;font-weight:600;\">%3</span>")
                           .arg(cssColor(gs::palette().text))
                           .arg(scaledPx(12))
                           .arg(escapeHtml(bold.toString()));
        if (!descPart.isEmpty())
            html += QStringLiteral("<br><span style=\"color:%1;font-size:%2px;\">%3</span>")
                        .arg(cssColor(gs::palette().muted))
                        .arg(scaledPx(11))
                        .arg(escapeHtml(descPart));
        label->setText(html);
    }

    // 3) 烘焙等宽字体的 QPlainTextEdit（审批卡参数/原始 JSON 区）
    const QList<QPlainTextEdit *> areas = findChildren<QPlainTextEdit *>();
    for (QPlainTextEdit *area : areas) {
        const QVariant base = area->property("monoBasePx");
        if (!base.isValid())
            continue;
        QFont mono = area->font();
        mono.setFamily(monoFont());
        mono.setPixelSize(scaledPx(base.toInt()));
        area->setFont(mono);
        const QVariant baseH = area->property("monoBaseHeight");
        if (baseH.isValid())
            area->setFixedHeight(qRound(baseH.toInt() * gs::zoomFactor()));
    }

    // 4) 下拉触发器：chevron 图标与省略文本按新字号重算
    const QList<ComboTrigger *> combos = findChildren<ComboTrigger *>();
    for (ComboTrigger *combo : combos)
        combo->refreshZoom();
    // 输入框里的技能芯片：宽度是按名字字号实测的
    if (m_composer)
        m_composer->refreshZoom();
    // 用量页的筛选触发器同理（chevron 与日历的小图标）
    const QList<UsageSelect *> usageSelects = findChildren<UsageSelect *>();
    for (UsageSelect *select : usageSelects)
        select->refreshIcons();

    // 5) 烘焙了等宽的明细表单元格（用量页 token / 轮次列）：按存好的基础字号重建
    const QList<QTableWidget *> tables = findChildren<QTableWidget *>();
    for (QTableWidget *table : tables) {
        for (int row = 0; row < table->rowCount(); ++row) {
            for (int column = 0; column < table->columnCount(); ++column) {
                QTableWidgetItem *item = table->item(row, column);
                if (!item)
                    continue;
                const QVariant base = item->data(Qt::UserRole + 1);
                if (!base.isValid())
                    continue;
                QFont mono(monoFont());
                mono.setPixelSize(scaledPx(base.toInt()));
                item->setFont(mono);
            }
        }
    }

    // 6) 自绘控件（连接状态点、阶段项、导航轨…）paint 时取 scaledPx，重绘即可
    const QList<QWidget *> all = findChildren<QWidget *>();
    for (QWidget *w : all)
        w->update();
}

} // namespace gs

// C 风格工厂：宿主程序（甚至非 Qt 语言绑定）可直接创建一个 UI 面板
extern "C" QTCHARTWIDGET_EXPORT QWidget *qtchartwidget_create()
{
    return new gs::ChartWidget();
}

extern "C" QTCHARTWIDGET_EXPORT const char *qtchartwidget_version()
{
    // 2.4.0：对齐 webui 的精修视觉层（中性冷黑调色板 / 正文 15px / 主栏 clamp(680,86vw,1320)）
    // 与审批卡限高 + 参数分级折叠；公开头只做加法。改这里时同步改 CHANGELOG 与 README
    return "2.4.0";
}

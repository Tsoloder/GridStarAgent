// QtChartWidget.dll 最小「接线」示例。
//
// 宿主只做三件事：
//   1) 创建 ChartWidget（跨语言场景可用 C 工厂 qtchartwidget_create()）
//   2) 用 setter 推数据：会话 / 模型 / 技能 / 历史 / 连接状态…
//   3) connect 信号，把界面交互翻译成自己的业务（HTTP / SSE / WebSocket / 本地服务）
//
// 本示例没有真实后端：用 QTimer 假流式代替 LLM 响应。真实工程中把
// startTurn()/streamAnswer() 的函数体换成你自己的请求与回调即可，
// 一轮流式对话的固定套路是：
//   setBusy(true) → appendReasoning* / appendAssistantText* / appendToolCall
//   → appendToolResult →（可选 appendApproval）→ finishAssistant()
//   → setTokenUsage() → setBusy(false)
#include <chartwidget.h>

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QVariant>

#include <functional>
#include <initializer_list>
#include <memory>
#include <utility>

using gs::ChartWidget;

namespace {

// 构造 QVariantMap 的简写，仅示例数据使用
QVariantMap obj(std::initializer_list<std::pair<const char *, QVariant>> items)
{
    QVariantMap out;
    for (const auto &item : items)
        out.insert(QString::fromUtf8(item.first), item.second);
    return out;
}

// ------------------------------------------------- 示例数据（字段形状见 chartwidget.h 注释）

QVariantList demoSessions()
{
    return { obj({ { "id", "s-1024" },
                   { "title", "10kV 城南线故障研判" },
                   { "updated_at", "2026-09-06T09:12" },
                   { "created_at", "2026-09-06T08:40" } }),
             obj({ { "id", "s-1023" },
                   { "title", "110kV 主变重过载分析" },
                   { "updated_at", "2026-09-05T18:03" },
                   { "created_at", "2026-09-05T17:20" } }) };
}

// 模型唯一键 = "<provider>/<id>"，与 setCurrentModel / modelSelected 一致
QVariantList demoModels()
{
    return { obj({ { "provider", "gridstar" },
                   { "provider_name", "GridStar 网关" },
                   { "id", "gs-pro-32k" },
                   { "name", "GS-Pro 32K" },
                   { "enabled", true },
                   { "provider_enabled", true } }),
             obj({ { "provider", "deepseek" },
                   { "id", "deepseek-chat" },
                   { "name", "DeepSeek Chat" },
                   { "enabled", true },
                   { "provider_enabled", true } }) };
}

QVariantList demoSkills()
{
    return { obj({ { "id", "grid-analysis" },
                   { "name", "电网分析" },
                   { "description", "潮流计算与故障研判" } }),
             obj({ { "id", "dispatch-report" },
                   { "name", "调度报告" },
                   { "description", "生成处置记录" } }) };
}

QVariantList demoHistory()
{
    return { obj({ { "role", "user" },
                   { "content", "城南线跳闸，帮我做故障研判。" },
                   { "display_content", "城南线跳闸，帮我做故障研判。" } }),
             obj({ { "role", "assistant" },
                   { "content", "已隔离 #12 分段，等待转供电指令。" },
                   { "usage", obj({ { "total", 320 }, { "input", 260 },
                                   { "output", 60 }, { "estimated", false } }) } }) };
}

// 假回答正文：```json 围栏里的 options 块会被渲染成选项卡片，
// 用户点击后触发 optionChosen(value, label) 信号
QString answerText()
{
    return QStringLiteral(
        "## 研判结论\n\n"
        "**城南线 #12 分段** 判定为永久性故障，分段开关已隔离。\n\n"
        "1. 转供路径：城南线 → 环城线，经联络开关 `LK-07` 合环\n"
        "2. 转供后主变 T2 负载率 *78%*，在安全限值内\n\n"
        "```json\n"
        "{\"options\":[{\"value\":\"继续执行转供电\",\"label\":\"继续转供电\",\"style\":\"primary\"},"
        "{\"value\":\"生成处置报告\",\"label\":\"生成报告\"}]}\n"
        "```\n");
}

// ------------------------------------------------- 假流式响应（真实工程替换为网络回调）

// 递归分块推送正文。lambda 通过 shared_ptr<std::function> 持有自身，
// 按值捕获避免自引用悬垂；QTimer 以 chart 为 context，界面销毁后自动停止。
void streamAnswer(ChartWidget *chart, const QString &text)
{
    auto pos = std::make_shared<int>(0);
    auto step = std::make_shared<std::function<void()>>();
    *step = [chart, text, pos, step] {
        if (*pos < text.size()) {
            chart->appendAssistantText(text.mid(*pos, 12)); // SSE delta → 直接透传
            *pos += 12;
            QTimer::singleShot(40, chart, [step] { (*step)(); });
            return;
        }
        // 正文发完：追加一次工具调用，700ms 后回填结果并收尾本轮
        chart->appendToolCall(QStringLiteral("call_1"), QStringLiteral("scada_snapshot"),
                              obj({ { "feeder", "城南线" } }));
        QTimer::singleShot(700, chart, [chart] {
            chart->appendToolResult(QStringLiteral("call_1"), QStringLiteral("scada_snapshot"),
                                    QStringLiteral("{\"breaker\":\"open\",\"I_A\":1820}"));
            // 高风险操作推审批卡：用户点击 → approvalDecided(callId, approved, args)
            chart->appendApproval(obj({ { "name", "switch_order" },
                                        { "call_id", "call_2" },
                                        { "args", obj({ { "device", "LK-07" },
                                                       { "action", "close" } }) },
                                        { "schema",
                                          obj({ { "type", "object" },
                                                { "properties",
                                                  obj({ { "device",
                                                          obj({ { "type", "string" },
                                                                { "description", "开关编号" } }) },
                                                        { "action",
                                                          obj({ { "type", "string" },
                                                                { "description", "open / close" } }) } }) },
                                                { "required",
                                                  QVariant(QVariantList{
                                                      QStringLiteral("device"),
                                                      QStringLiteral("action") }) } }) } }));
            chart->finishAssistant();                  // 结束本轮流式（结构化块定稿）
            chart->setTokenUsage(1842, 1510, 332, false);
            chart->setBusy(false);                     // 停止按钮恢复为发送按钮
        });
    };
    (*step)();
}

// 一轮对话的入口：sendMessage / optionChosen / retryRequested 都汇聚到这里
void startTurn(ChartWidget *chart, const QString &message, const QString &display,
               const QVariantList &attachments)
{
    if (chart->isBusy()) { // 上一轮还没结束，拒绝并提示
        chart->showToast(QStringLiteral("当前还有请求在处理中"));
        return;
    }
    chart->appendUserMessage(display.isEmpty() ? message : display, attachments);
    chart->clearAttachments(); // 附件已随消息发出，清空输入区芯片
    chart->setBusy(true);
    // 真实工程：在这里发起 POST /chat（携带 message + attachments），
    // 收到 reasoning delta 调 appendReasoning()，正文 delta 调 appendAssistantText()。
    chart->appendReasoning(QStringLiteral("跳闸电流超过速断定值，先隔离故障段，再校核转供能力。"));
    streamAnswer(chart, answerText());
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 1) 创建部件。C 宿主 / 插件场景可改用：
    //    QWidget *w = qtchartwidget_create();  // extern "C"，见 chartwidget.cpp
    auto *chart = new ChartWidget;
    chart->setWindowTitle(QStringLiteral("GridStar AI · host_example"));
    chart->resize(960, 720);

    // 2) 推初始数据（全部是 setter，部件自身不发任何请求）
    chart->setConnectionState(QStringLiteral("online"), QStringLiteral("服务在线"));
    chart->setSessions(demoSessions());
    chart->setCurrentSessionTitle(QStringLiteral("10kV 城南线故障研判"));
    chart->setModels(demoModels());
    chart->setCurrentModel(QStringLiteral("gridstar/gs-pro-32k"));
    chart->setSkills(demoSkills());
    chart->setCurrentSkill(QStringLiteral("grid-analysis"));
    chart->setMode(QStringLiteral("auto"));
    chart->setConfigLoaded(true);
    chart->setHistory(demoHistory());

    // 3) 接线：信号 → 宿主业务。以下覆盖 ChartWidget 的全部交互信号。

    // ---- 对话主链路 ----
    QObject::connect(chart, &ChartWidget::sendMessage, chart,
                     [chart](const QString &message, const QString &display,
                             const QVariantList &attachments) {
                         chart->setInputText(QString()); // 输入框由宿主负责清空
                         startTurn(chart, message, display, attachments);
                     });
    QObject::connect(chart, &ChartWidget::stopRequested, chart, [chart] {
        // 真实工程：先 abort 网络请求，再收尾 UI
        chart->finishAssistant();
        chart->setBusy(false);
    });
    QObject::connect(chart, &ChartWidget::retryRequested, chart,
                     [chart](const QString &message, const QString &display,
                             const QVariantList &attachments) {
                         startTurn(chart, message, display, attachments);
                     });
    QObject::connect(chart, &ChartWidget::optionChosen, chart,
                     [chart](const QString &value, const QString &label) {
                         startTurn(chart, value, label, QVariantList());
                     });

    // ---- 输入区状态 ----
    QObject::connect(chart, &ChartWidget::modeChanged, chart, [chart](const QString &mode) {
        chart->showToast(mode == QLatin1String("auto") ? QStringLiteral("自动模式")
                                                       : QStringLiteral("手动模式"));
    });
    QObject::connect(chart, &ChartWidget::modelSelected, chart, [chart](const QString &key) {
        chart->setCurrentModel(key); // 宿主持久化选择后回写
    });
    QObject::connect(chart, &ChartWidget::skillSelected, chart, [chart](const QString &id) {
        chart->setCurrentSkill(id);
    });

    // ---- 会话管理（真实工程对应后端 /sessions CRUD） ----
    QObject::connect(chart, &ChartWidget::newSessionRequested, chart, [chart] {
        chart->clearMessages();
        chart->setCurrentSessionTitle(QStringLiteral("新对话"));
        chart->focusInput();
    });
    QObject::connect(chart, &ChartWidget::sessionSelected, chart, [chart](const QString &id) {
        Q_UNUSED(id);
        chart->setHistory(demoHistory()); // 拉取该会话历史后回填
    });
    QObject::connect(chart, &ChartWidget::sessionRenamed, chart, [](const QString &id) {
        Q_UNUSED(id); // 宿主弹重命名对话框 → PUT /sessions/{id} → setSessions() 刷新
    });
    QObject::connect(chart, &ChartWidget::sessionCleared, chart, [chart](const QString &id) {
        Q_UNUSED(id);
        chart->clearMessages();
    });
    QObject::connect(chart, &ChartWidget::sessionDeleted, chart, [chart](const QString &id) {
        Q_UNUSED(id); // DELETE /sessions/{id} 成功后 setSessions() 刷新列表
    });
    QObject::connect(chart, &ChartWidget::connectionCheckRequested, chart, [chart] {
        chart->setConnectionState(QStringLiteral("checking"), QStringLiteral("检测中"));
        QTimer::singleShot(700, chart, [chart] { // 真实工程：GET /health 后回填
            chart->setConnectionState(QStringLiteral("online"), QStringLiteral("服务在线"));
        });
    });

    // ---- 结构化卡片交互 ----
    QObject::connect(chart, &ChartWidget::toolParamsConfirmed, chart,
                     [chart](const QString &tool, bool confirmed, const QVariantMap &params,
                             const QString &label) {
                         Q_UNUSED(label);
                         if (confirmed) {
                             // 参数确认 → 宿主带 params 调用工具
                             const QByteArray json = QJsonDocument(
                                 QJsonObject::fromVariantMap(params)).toJson(QJsonDocument::Compact);
                             chart->appendAssistantMessage(QStringLiteral("已确认 **%1** 参数：%2")
                                                               .arg(tool, QString::fromUtf8(json)));
                         } else {
                             chart->appendAssistantMessage(QStringLiteral("已取消 **%1**。").arg(tool));
                         }
                     });
    QObject::connect(chart, &ChartWidget::approvalDecided, chart,
                     [chart](const QString &callId, bool approved, const QVariantMap &args) {
                         Q_UNUSED(args);
                         // 真实工程：POST 审批结果，成功后才 resolveApproval；失败用 reEnableApproval
                         QTimer::singleShot(400, chart, [chart, callId, approved] {
                             chart->resolveApproval(callId, approved);
                             chart->showToast(approved ? QStringLiteral("已批准，指令下发中")
                                                       : QStringLiteral("已拒绝该操作"));
                         });
                     });
    QObject::connect(chart, &ChartWidget::approvalJsonInvalid, chart, [chart] {
        chart->showToast(QStringLiteral("审批参数不是合法 JSON"));
    });
    QObject::connect(chart, &ChartWidget::workflowRunRequested, chart,
                     [chart](const QVariantList &steps) {
                         chart->setBusy(true);
                         chart->appendWorkflowEvent(obj({ { "type", "workflow_started" } }));
                         for (int i = 0; i < steps.size(); ++i) {
                             QVariantMap step = steps.at(i).toMap();
                             step.insert(QStringLiteral("type"), QStringLiteral("workflow_step"));
                             step.insert(QStringLiteral("index"), i);
                             // 真实工程：每步执行成功后再推 workflow_step 事件
                             QTimer::singleShot(500 * (i + 1), chart,
                                                [chart, step] { chart->appendWorkflowEvent(step); });
                         }
                         QTimer::singleShot(500 * (steps.size() + 1), chart, [chart] {
                             chart->appendWorkflowEvent(obj({ { "type", "workflow_done" },
                                                              { "status", "done" } }));
                             chart->setBusy(false);
                         });
                     });

    // ---- 设置中心 ----
    QObject::connect(chart, &ChartWidget::settingsSaveRequested, chart,
                     [chart](const QVariantMap &config, const QVariant &revision) {
                         Q_UNUSED(revision); // 乐观锁版本号：后端冲突时提示刷新
                         chart->setSettingsStatus(QStringLiteral("正在保存…"));
                         QTimer::singleShot(500, chart, [chart, config] {
                             // 保存成功后：回推模型列表 + 新 draft + settingsSaved() 关闭对话框
                             chart->setModels(config.value(QStringLiteral("models")).toList());
                             chart->setCurrentModel(
                                 config.value(QStringLiteral("default_model")).toString());
                             chart->setSettingsDraft(config, 8);
                             chart->settingsSaved();
                         });
                     });
    QObject::connect(chart, &ChartWidget::testProviderRequested, chart,
                     [chart](const QString &providerId) {
                         chart->setProviderBusy(true, false);
                         QTimer::singleShot(900, chart, [chart, providerId] {
                             chart->setProviderBusy(false, false);
                             chart->setSettingsStatus(
                                 QStringLiteral("%1 连通正常").arg(providerId));
                         });
                     });
    QObject::connect(chart, &ChartWidget::readModelsRequested, chart,
                     [chart](const QString &providerId) {
                         chart->setProviderBusy(false, true);
                         QTimer::singleShot(900, chart, [chart, providerId] {
                             chart->setDiscoveredModels(providerId, demoModels());
                             chart->setProviderBusy(false, false);
                         });
                     });
    QObject::connect(chart, &ChartWidget::refreshSkillsRequested, chart, [chart] {
        chart->setSettingsSkills(QVariantList(), true, QString()); // loading 态
        QTimer::singleShot(600, chart, [chart] {
            chart->setSettingsSkills(demoSkills(), false, QString());
        });
    });
    QObject::connect(chart, &ChartWidget::refreshMcpRequested, chart, [chart] {
        chart->setMcpTools(QVariantList(), false, true, QString()); // loading 态
        QTimer::singleShot(600, chart, [] { /* 重连成功后 setMcpTools(tools, true, false, "") */ });
    });

    // ---- 附件与语音 ----
    QObject::connect(chart, &ChartWidget::attachRequested, chart, [chart] {
        const QStringList paths = QFileDialog::getOpenFileNames(
            chart, QStringLiteral("选择附件"), QString(), QStringLiteral("所有文件 (*.*)"));
        QVariantList items;
        for (const QString &path : paths) {
            const QFileInfo info(path);
            if (info.isFile())
                items.append(obj({ { "id", info.absoluteFilePath() },
                                   { "name", info.fileName() },
                                   { "path", info.absoluteFilePath() },
                                   { "size", info.size() },
                                   { "ext", info.suffix().toLower() } }));
        }
        if (!items.isEmpty())
            chart->addAttachments(items); // 拖拽进来的文件走 attachmentsAdded，同一形状
    });
    QObject::connect(chart, &ChartWidget::attachmentsAdded, chart,
                     [chart](const QVariantList &items) { chart->addAttachments(items); });
    QObject::connect(chart, &ChartWidget::attachmentRemoved, chart, [](const QString &id) {
        Q_UNUSED(id); // 芯片已由部件自行移除，宿主只需清理自己的上传队列
    });
    QObject::connect(chart, &ChartWidget::voiceRequested, chart, [chart] {
        static bool recording = false; // 真实工程：接录音设备，PCM 送 ASR
        recording = !recording;
        chart->setVoiceRecording(recording);
        if (!recording)
            chart->setInputText(chart->inputText() + QStringLiteral("（语音转写文本）"));
    });

    chart->show();
    chart->focusInput();
    return app.exec();
}

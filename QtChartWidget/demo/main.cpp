// QtChartWidget 示例宿主。
//
// ChartWidget 是纯 UI 库：宿主用 setter 推数据、用信号收交互。本文件演示一轮完整的
// 流式对话（思考 → 正文 → 工具调用 → 结果 → token 统计）、审批卡、结构化卡片、
// 工作流卡、失败重试与设置中心，并支持 `--shot <file>` 离屏截图与 webui 对照。
//
// 输入框里可用的演示指令：
//   /history 载入示例历史   /clear 清空        /fail 制造失败气泡
//   /approve 审批卡         /workflow 工作流卡  /options 选项卡
//   /params 工具参数确认卡  /toast <文本>      /settings [tab]
#include <chartwidget.h>

#include <QApplication>
#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QPixmap>
#include <QStringList>
#include <QTimer>
#include <QVariant>

#include <initializer_list>
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

QStringList chunkText(const QString &text, int size)
{
    QStringList out;
    for (int i = 0; i < text.size(); i += size)
        out << text.mid(i, size);
    return out;
}

// ------------------------------------------------------------------ 示例数据

QVariantList demoSessions()
{
    return { obj({ { "id", "s-1024" },
                   { "title", "10kV 城南线故障研判与转供电方案" },
                   { "updated_at", "2026-09-06T09:12" },
                   { "created_at", "2026-09-06T08:40" } }),
             obj({ { "id", "s-1023" },
                   { "title", "110kV 主变重过载分析与负荷转移" },
                   { "updated_at", "2026-09-05T18:03" },
                   { "created_at", "2026-09-05T17:20" } }),
             obj({ { "id", "s-1022" },
                   { "title", "配电自动化终端离线排查" },
                   { "updated_at", "2026-09-04T11:47" },
                   { "created_at", "2026-09-04T10:02" } }) };
}

QVariantList demoModels()
{
    return { obj({ { "provider", "gridstar" },
                   { "provider_name", "GridStar 网关" },
                   { "id", "gs-pro-32k" },
                   { "name", "GS-Pro 32K" },
                   { "enabled", true },
                   { "provider_enabled", true } }),
             obj({ { "provider", "gridstar" },
                   { "id", "gs-lite-8k" },
                   { "name", "GS-Lite 8K" },
                   { "enabled", true },
                   { "provider_enabled", true } }),
             obj({ { "provider", "deepseek" },
                   { "id", "deepseek-chat" },
                   { "name", "DeepSeek Chat" },
                   { "enabled", true },
                   { "provider_enabled", true } }),
             obj({ { "provider", "deepseek" },
                   { "id", "deepseek-reasoner" },
                   { "name", "DeepSeek Reasoner（停用）" },
                   { "enabled", false },
                   { "provider_enabled", true } }) };
}

// 输入区 Skill 下拉只需要 id / name
QVariantList demoSkills()
{
    return { obj({ { "id", "grid-analysis" },
                   { "name", "电网分析" },
                   { "description", "潮流计算、N-1 校核与故障研判" } }),
             obj({ { "id", "dispatch-report" },
                   { "name", "调度报告" },
                   { "description", "生成处置记录与调度日报" } }),
             obj({ { "id", "device-ops" },
                   { "name", "设备运维" },
                   { "description", "缺陷归档与检修计划" } }) };
}

// 设置中心「技能」页需要 version / source / allowed_tools
QVariantList demoSettingsSkills()
{
    QVariantList out;
    const QVariantList skills = demoSkills();
    const QStringList versions{ QStringLiteral("1.4.0"), QStringLiteral("1.0.2"),
                                QStringLiteral("0.9.1") };
    const QStringList sources{ QStringLiteral("builtin"), QStringLiteral("builtin"),
                               QStringLiteral("local") };
    const QVariantList tools{ QVariantList{ QStringLiteral("scada_snapshot"),
                                            QStringLiteral("power_flow") },
                              QVariantList{ QStringLiteral("report_export") },
                              QVariantList{ QStringLiteral("device_query") } };
    for (int i = 0; i < skills.size(); ++i) {
        QVariantMap skill = skills.at(i).toMap();
        skill.insert(QStringLiteral("version"), versions.at(i));
        skill.insert(QStringLiteral("source"), sources.at(i));
        skill.insert(QStringLiteral("allowed_tools"), tools.at(i));
        out.append(skill);
    }
    return out;
}

QVariantList demoMcpTools()
{
    return { obj({ { "name", "scada_snapshot" },
                   { "description", "读取指定馈线的 SCADA 断面数据" },
                   { "input_schema",
                     obj({ { "type", "object" },
                           { "properties",
                             obj({ { "feeder",
                                     obj({ { "type", "string" },
                                           { "description", "馈线名称" } }) },
                                   { "ts",
                                     obj({ { "type", "string" },
                                           { "description", "断面时刻，ISO8601" } }) } }) },
                           { "required", QVariant(QVariantList{ QStringLiteral("feeder") }) } }) } }),
             obj({ { "name", "switch_order" },
                   { "description", "下发开关遥控令（需要审批）" },
                   { "input_schema",
                     obj({ { "type", "object" },
                           { "properties",
                             obj({ { "device",
                                     obj({ { "type", "string" },
                                           { "description", "开关编号" } }) },
                                   { "action",
                                     obj({ { "type", "string" },
                                           { "description", "open / close" } }) },
                                   { "delay_s",
                                     obj({ { "type", "integer" },
                                           { "description", "延时秒数" } }) } }) },
                           { "required",
                             QVariant(QVariantList{ QStringLiteral("device"),
                                                    QStringLiteral("action") }) } }) } }) };
}

QVariantMap demoConfig()
{
    return obj({ { "revision", 7 },
                 { "default_model", "gridstar/gs-pro-32k" },
                 { "providers",
                   QVariant(QVariantList{
                       obj({ { "id", "gridstar" },
                             { "name", "GridStar 网关" },
                             { "type", "openai" },
                             { "base_url", "https://llm.gridstar.local/v1" },
                             { "api_key", "gs-****-****" },
                             { "enabled", true },
                             { "description", "内网统一网关" } }),
                       obj({ { "id", "deepseek" },
                             { "name", "DeepSeek" },
                             { "type", "openai" },
                             { "base_url", "https://api.deepseek.com/v1" },
                             { "api_key", "" },
                             { "enabled", true },
                             { "description", "公网备用" } }) }) },
                 { "models", QVariant(demoModels()) },
                 { "mcp_servers",
                   QVariant(QVariantList{ obj({ { "name", "grid-mcp" },
                                                { "url", "http://127.0.0.1:8123/mcp" },
                                                { "enabled", true } }) }) } });
}

QString answerText()
{
    return QStringLiteral(
        "## 研判结论\n\n"
        "**城南线 #12 分段** 判定为永久性故障，故障电流 `1.82 kA`，分段开关已隔离。\n\n"
        "1. 转供路径：城南线 → 环城线，经联络开关 `LK-07` 合环\n"
        "2. 转供后主变 T2 负载率 *78%*，在安全限值内\n"
        "3. 预计复电时间 12 分钟，涉及用户 1 842 户\n\n"
        "```cpp\n"
        "// 转供后网损校核\n"
        "double loss_kW = powerFlow(case_transfer_LK07);\n"
        "if (loss_kW > 15.0)\n"
        "    reroute(\"环城线 #3\");\n"
        "```\n");
}

QString phasePlanText()
{
    return QStringLiteral(
        "\n```json\n"
        "{\"phase_plan\":{\"title\":\"故障处置阶段\",\"phases\":["
        "{\"title\":\"故障定位\",\"status\":\"done\",\"note\":\"行波测距 + SCADA 变位\"},"
        "{\"title\":\"隔离故障段\",\"status\":\"done\",\"note\":\"#12 分段开关已分闸\"},"
        "{\"title\":\"转供电\",\"status\":\"active\",\"note\":\"经 LK-07 合环转供\"},"
        "{\"title\":\"复电确认\",\"status\":\"pending\",\"note\":\"等待现场回报\"}]}}\n"
        "```\n");
}

QVariantList demoHistory()
{
    QVariantList out;
    out.append(obj({ { "role", "user" },
                     { "content", "城南线跳闸，帮我做故障研判并给出转供电方案。" },
                     { "display_content", "城南线跳闸，帮我做故障研判并给出转供电方案。" },
                     { "attachments",
                       QVariant(QVariantList{ obj({ { "name", "城南线单线图.pdf" },
                                                    { "kind", "file" } }),
                                              obj({ { "name", "SCADA_0904.csv" },
                                                    { "kind", "file" } }) }) } }));

    out.append(obj({ { "role", "assistant" },
                     { "content", "先读取跳闸时刻的 SCADA 断面，再做转供潮流校核。" },
                     { "reasoning_content",
                       "跳闸电流 1.82kA 远超速断定值，优先判断为永久故障；"
                       "需要确认联络开关 LK-07 可用以及转供后主变负载率。" },
                     { "active_skills", QVariant(QVariantList{ QStringLiteral("电网分析") }) },
                     { "tool_calls",
                       QVariant(QVariantList{
                           obj({ { "id", "call_91" },
                                 { "function",
                                   obj({ { "name", "scada_snapshot" },
                                         { "arguments",
                                           "{\"feeder\":\"城南线\","
                                           "\"ts\":\"2026-09-06T09:04:00\"}" } }) } }),
                           obj({ { "id", "call_92" },
                                 { "function",
                                   obj({ { "name", "power_flow" },
                                         { "arguments", "{\"case\":\"transfer_LK07\"}" } }) } }) }) } }));

    out.append(obj({ { "role", "tool" },
                     { "tool_call_id", "call_91" },
                     { "tool_name", "scada_snapshot" },
                     { "content",
                       "{\"breaker\":\"open\",\"I_A\":1820,\"U_pu\":0.94,\"lk07\":\"available\"}" } }));

    out.append(obj({ { "role", "tool" },
                     { "tool_call_id", "call_92" },
                     { "tool_name", "power_flow" },
                     { "content", "error: 潮流不收敛，请检查联络线阻抗参数" } }));

    out.append(obj({ { "role", "assistant" },
                     { "content", answerText() + phasePlanText() },
                     { "active_skills", QVariant(QVariantList{ QStringLiteral("电网分析") }) },
                     { "usage",
                       obj({ { "total", 1842 },
                             { "input", 1510 },
                             { "output", 332 },
                             { "estimated", false } }) } }));

    out.append(obj({ { "role", "user" },
                     { "content", "按静态工作流执行处置。" },
                     { "display_content", "按静态工作流执行处置。" } }));

    out.append(obj({ { "role", "workflow" },
                     { "status", "partial" },
                     { "message", "第 3 步被拒绝" },
                     { "steps",
                       QVariant(QVariantList{
                           obj({ { "tool", "scada_snapshot" },
                                 { "desc", "读取断面" },
                                 { "status", "done" } }),
                           obj({ { "tool", "power_flow" },
                                 { "desc", "转供潮流校核" },
                                 { "status", "done" } }),
                           obj({ { "tool", "switch_order" },
                                 { "desc", "下发联络开关令" },
                                 { "status", "failed" } }) }) } }));
    return out;
}

QVariantMap approvalEvent(const QString &callId)
{
    return obj({ { "name", "switch_order" },
                 { "call_id", callId },
                 { "args",
                   obj({ { "device", "LK-07" },
                         { "action", "close" },
                         { "delay_s", 5 } }) },
                 { "schema",
                   obj({ { "type", "object" },
                         { "properties",
                           obj({ { "device",
                                   obj({ { "type", "string" },
                                         { "description", "开关编号" } }) },
                                 { "action",
                                   obj({ { "type", "string" },
                                         { "description", "open / close" } }) },
                                 { "delay_s",
                                   obj({ { "type", "integer" },
                                         { "description", "延时秒数" } }) } }) },
                         { "required",
                           QVariant(QVariantList{ QStringLiteral("device"),
                                                  QStringLiteral("action") }) } }) } });
}

QVariantList workflowSteps()
{
    return { obj({ { "tool", "scada_snapshot" },
                   { "desc", "读取断面" },
                   { "params", obj({ { "feeder", "城南线" } }) },
                   { "status", "done" } }),
             obj({ { "tool", "power_flow" },
                   { "desc", "转供潮流校核" },
                   { "params", obj({ { "case", "transfer_LK07" } }) },
                   { "status", "done" } }),
             obj({ { "tool", "switch_order" },
                   { "desc", "下发联络开关令" },
                   { "params", obj({ { "device", "LK-07" }, { "action", "close" } }) },
                   { "status", "done" } }) };
}

} // namespace

// ------------------------------------------------------------ DemoHost

// 假后端：把 ChartWidget 的信号翻译成 setter 调用，模拟真实宿主的行为
class DemoHost
{
public:
    explicit DemoHost(ChartWidget *chart) : m_chart(chart) { wire(); }

    void populate();
    void loadHistory();

private:
    void wire();
    void startTurn(const QString &message, const QString &display,
                   const QVariantList &attachments);
    bool runCommand(const QString &text, const QVariantList &attachments);
    void streamChunk(int index);
    void runWorkflow(const QVariantList &steps);
    void attachFiles(const QStringList &paths);

    ChartWidget *m_chart;
    QStringList m_chunks;
    int m_callSeq = 0;
    int m_attachSeq = 0;
    bool m_recording = false;
    bool m_stopped = false;
    bool m_workflowActive = false;
};

void DemoHost::populate()
{
    m_chart->setConnectionState(QStringLiteral("online"), QStringLiteral("服务在线"));
    m_chart->setSessions(demoSessions());
    m_chart->setCurrentSessionTitle(QStringLiteral("10kV 城南线故障研判与转供电方案"));
    m_chart->setModels(demoModels());
    m_chart->setCurrentModel(QStringLiteral("gridstar/gs-pro-32k"));
    m_chart->setSkills(demoSkills());
    m_chart->setCurrentSkill(QStringLiteral("grid-analysis"));
    m_chart->setMode(QStringLiteral("auto"));
    m_chart->setConfigLoaded(true);
    m_chart->setVoiceEnabled(true);
    m_chart->setSettingsDraft(demoConfig(), 7);
    m_chart->setSettingsSkills(demoSettingsSkills(), false, QString());
    m_chart->setMcpTools(demoMcpTools(), true, false, QString());
}

void DemoHost::loadHistory()
{
    m_chart->setHistory(demoHistory());
    m_chart->setCurrentSessionTitle(QStringLiteral("10kV 城南线故障研判与转供电方案"));
}

void DemoHost::wire()
{
    ChartWidget *c = m_chart;

    QObject::connect(c, &ChartWidget::sendMessage, c,
                     [this](const QString &message, const QString &display,
                            const QVariantList &attachments) {
                         m_chart->setInputText(QString());
                         startTurn(message, display, attachments);
                     });
    QObject::connect(c, &ChartWidget::retryRequested, c,
                     [this](const QString &message, const QString &display,
                            const QVariantList &attachments) {
                         startTurn(message, display, attachments);
                     });
    QObject::connect(c, &ChartWidget::stopRequested, c, [this] {
        m_stopped = true;
        if (m_workflowActive) {
            m_workflowActive = false;
            m_chart->appendWorkflowEvent(
                obj({ { "type", "workflow_done" }, { "status", "partial" } }));
        }
        m_chart->finishAssistant();
        m_chart->setBusy(false);
        m_chart->showToast(QStringLiteral("已停止接收"));
    });
    QObject::connect(c, &ChartWidget::modeChanged, c, [this](const QString &mode) {
        m_chart->showToast(mode == QLatin1String("auto")
                               ? QStringLiteral("自动模式：Agent 可连续调用工具")
                               : QStringLiteral("手动模式：每步都需确认"));
    });
    QObject::connect(c, &ChartWidget::modelSelected, c, [this](const QString &key) {
        m_chart->setCurrentModel(key);
        m_chart->showToast(QStringLiteral("模型已切换：%1").arg(key));
    });
    QObject::connect(c, &ChartWidget::skillSelected, c, [this](const QString &id) {
        m_chart->setCurrentSkill(id);
        m_chart->showToast(id.isEmpty() ? QStringLiteral("已取消 Skill")
                                        : QStringLiteral("Skill：%1").arg(id));
    });
    QObject::connect(c, &ChartWidget::connectionCheckRequested, c, [this] {
        m_chart->setConnectionState(QStringLiteral("checking"), QStringLiteral("检测中"));
        QTimer::singleShot(700, m_chart, [this] {
            m_chart->setConnectionState(QStringLiteral("online"), QStringLiteral("服务在线"));
            m_chart->showToast(QStringLiteral("网关连通性正常"));
        });
    });

    QObject::connect(c, &ChartWidget::newSessionRequested, c, [this] {
        m_chart->clearMessages();
        m_chart->setCurrentSessionTitle(QStringLiteral("新对话"));
        m_chart->focusInput();
        m_chart->showToast(QStringLiteral("已新建会话"));
    });
    QObject::connect(c, &ChartWidget::sessionSelected, c, [this](const QString &id) {
        loadHistory();
        m_chart->showToast(QStringLiteral("已载入会话 %1").arg(id));
    });
    QObject::connect(c, &ChartWidget::sessionRenamed, c, [this](const QString &id) {
        m_chart->showToast(QStringLiteral("重命名会话 %1（宿主实现）").arg(id));
    });
    QObject::connect(c, &ChartWidget::sessionCleared, c, [this](const QString &id) {
        m_chart->clearMessages();
        m_chart->showToast(QStringLiteral("已清空会话 %1").arg(id));
    });
    QObject::connect(c, &ChartWidget::sessionDeleted, c, [this](const QString &id) {
        m_chart->showToast(QStringLiteral("已删除会话 %1").arg(id));
    });

    QObject::connect(c, &ChartWidget::optionChosen, c,
                     [this](const QString &value, const QString &label) {
                         startTurn(value, label, QVariantList());
                     });
    QObject::connect(c, &ChartWidget::toolParamsConfirmed, c,
                     [this](const QString &tool, bool confirmed, const QVariantMap &params,
                            const QString &label) {
                         qDebug() << "[demo] tool_params" << tool << confirmed << params << label;
                         m_chart->appendAssistantMessage(
                             confirmed ? QStringLiteral("已确认 **%1** 参数，开始执行。").arg(tool)
                                       : QStringLiteral("已取消 **%1**。").arg(tool));
                     });
    QObject::connect(c, &ChartWidget::approvalDecided, c,
                     [this](const QString &callId, bool approved, const QVariantMap &args) {
                         qDebug() << "[demo] approval" << callId << approved << args;
                         // 真实宿主在这里 POST 审批结果，成功后才改卡片状态
                         QTimer::singleShot(400, m_chart, [this, callId, approved] {
                             m_chart->resolveApproval(callId, approved);
                             m_chart->showToast(approved ? QStringLiteral("已批准，指令下发中")
                                                         : QStringLiteral("已拒绝该操作"));
                         });
                     });
    QObject::connect(c, &ChartWidget::approvalJsonInvalid, c, [this] {
        qDebug() << "[demo] approval json invalid";
    });
    QObject::connect(c, &ChartWidget::workflowRunRequested, c,
                     [this](const QVariantList &steps) {
                         m_chart->setBusy(true);
                         runWorkflow(steps);
                     });

    QObject::connect(c, &ChartWidget::settingsSaveRequested, c,
                     [this](const QVariantMap &config, const QVariant &revision) {
                         qDebug() << "[demo] save config, revision =" << revision
                                  << "providers =" << config.value("providers").toList().size();
                         m_chart->setSettingsStatus(QStringLiteral("正在写入 config.json…"));
                         QTimer::singleShot(500, m_chart, [this, config] {
                             // 回推模型列表，输入区下拉与设置中心保持一致
                             m_chart->setModels(config.value(QStringLiteral("models")).toList());
                             m_chart->setCurrentModel(
                                 config.value(QStringLiteral("default_model")).toString());
                             m_chart->setSettingsDraft(config, 8);
                             m_chart->settingsSaved();
                             m_chart->showToast(QStringLiteral("配置已保存"));
                         });
                     });
    QObject::connect(c, &ChartWidget::testProviderRequested, c, [this](const QString &providerId) {
        m_chart->setProviderBusy(true, false);
        m_chart->setSettingsStatus(QStringLiteral("正在测试 %1…").arg(providerId));
        QTimer::singleShot(900, m_chart, [this, providerId] {
            m_chart->setProviderBusy(false, false);
            m_chart->setSettingsStatus(QStringLiteral("%1 连通正常（128 ms）").arg(providerId));
        });
    });
    QObject::connect(c, &ChartWidget::readModelsRequested, c, [this](const QString &providerId) {
        m_chart->setProviderBusy(false, true);
        m_chart->setSettingsStatus(QStringLiteral("正在读取 %1 模型列表…").arg(providerId));
        QTimer::singleShot(900, m_chart, [this, providerId] {
            m_chart->setDiscoveredModels(providerId, demoModels());
            m_chart->setProviderBusy(false, false);
            m_chart->setSettingsStatus(QStringLiteral("读取到 %1 个模型").arg(demoModels().size()));
        });
    });
    QObject::connect(c, &ChartWidget::refreshSkillsRequested, c, [this] {
        m_chart->setSettingsSkills(QVariantList(), true, QString());
        QTimer::singleShot(600, m_chart, [this] {
            m_chart->setSettingsSkills(demoSettingsSkills(), false, QString());
            m_chart->showToast(QStringLiteral("技能已刷新"));
        });
    });
    QObject::connect(c, &ChartWidget::refreshMcpRequested, c, [this] {
        m_chart->setMcpTools(QVariantList(), false, true, QString());
        QTimer::singleShot(600, m_chart, [this] {
            m_chart->setMcpTools(demoMcpTools(), true, false, QString());
            m_chart->showToast(QStringLiteral("MCP 已重连"));
        });
    });

    QObject::connect(c, &ChartWidget::attachRequested, c, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(
            m_chart, QStringLiteral("选择附件"), QString(),
            QStringLiteral("所有文件 (*.*)"));
        attachFiles(paths);
    });
    QObject::connect(c, &ChartWidget::attachmentsAdded, c, [this](const QVariantList &items) {
        QStringList paths;
        for (const QVariant &item : items)
            paths << item.toMap().value(QStringLiteral("path")).toString();
        attachFiles(paths);
    });
    QObject::connect(c, &ChartWidget::attachmentRemoved, c, [](const QString &id) {
        qDebug() << "[demo] attachment removed" << id;
    });
    QObject::connect(c, &ChartWidget::voiceRequested, c, [this] {
        m_recording = !m_recording;
        m_chart->setVoiceRecording(m_recording);
        m_chart->showToast(m_recording ? QStringLiteral("录音中…（演示）")
                                       : QStringLiteral("已停止录音（演示）"));
    });
}

// 上传是宿主的活：先塞「上传中」芯片，再回填完成状态
void DemoHost::attachFiles(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    QVariantList pending;
    QVariantList done;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.isFile())
            continue;
        const QString id = QStringLiteral("local-%1").arg(++m_attachSeq);
        const QVariantMap base = obj({ { "id", id },
                                       { "name", info.fileName() },
                                       { "path", info.absoluteFilePath() },
                                       { "size", info.size() },
                                       { "ext", info.suffix().toLower() } });
        QVariantMap uploading = base;
        uploading.insert(QStringLiteral("uploading"), true);
        pending.append(uploading);
        done.append(base);
    }
    if (pending.isEmpty())
        return;
    const QVariantList current = m_chart->attachments();
    QVariantList merged = current;
    for (const QVariant &item : pending)
        merged.append(item);
    m_chart->clearAttachments();
    m_chart->addAttachments(merged);

    QVariantList settled = current;
    for (const QVariant &item : done)
        settled.append(item);
    QTimer::singleShot(900, m_chart, [this, settled] {
        m_chart->clearAttachments();
        m_chart->addAttachments(settled);
    });
}

void DemoHost::startTurn(const QString &message, const QString &display,
                         const QVariantList &attachments)
{
    if (m_chart->isBusy()) {
        m_chart->showToast(QStringLiteral("当前还有请求在处理中"));
        return;
    }
    m_stopped = false;
    const QString text = message.trimmed();
    if (runCommand(text, attachments))
        return;

    m_chart->appendUserMessage(display.isEmpty() ? message : display, attachments);
    m_chart->clearAttachments();
    m_chart->setBusy(true);
    m_chart->appendReasoning(
        QStringLiteral("跳闸电流 1.82kA 超过速断定值，先隔离故障段，再校核转供后的主变负载率。"));
    m_chunks = chunkText(answerText(), 9);
    streamChunk(0);
}

void DemoHost::streamChunk(int index)
{
    if (m_stopped)
        return;
    if (index < m_chunks.size()) {
        m_chart->appendAssistantText(m_chunks.at(index));
        QTimer::singleShot(45, m_chart, [this, index] { streamChunk(index + 1); });
        return;
    }
    const QString callId = QStringLiteral("call_%1").arg(++m_callSeq);
    m_chart->appendToolCall(callId, QStringLiteral("power_flow"),
                            QVariant(obj({ { "case", "transfer_LK07" },
                                           { "feeder", "城南线" } })));
    QTimer::singleShot(700, m_chart, [this, callId] {
        if (m_stopped)
            return;
        m_chart->appendToolResult(callId, QStringLiteral("power_flow"),
                                  QStringLiteral("{\"max_load_rate\":0.78,\"loss_kW\":12.4}"));
        m_chart->finishAssistant();
        m_chart->setTokenUsage(1842, 1510, 332, false);
        m_chart->setBusy(false);
    });
}

void DemoHost::runWorkflow(const QVariantList &steps)
{
    m_chart->appendWorkflowEvent(obj({ { "type", "workflow_started" } }));
    m_workflowActive = true;
    // 步骤间隔放慢到 4s，保证 busy/停止态有足够时间被观察与点击
    const int interval = 4000;
    for (int i = 0; i < steps.size(); ++i) {
        QVariantMap step = steps.at(i).toMap();
        step.insert(QStringLiteral("type"), QStringLiteral("workflow_step"));
        step.insert(QStringLiteral("index"), i);
        QTimer::singleShot(interval * (i + 1), m_chart, [this, step] {
            if (m_stopped)
                return;
            m_chart->appendWorkflowEvent(step);
        });
    }
    QTimer::singleShot(interval * (steps.size() + 1), m_chart, [this] {
        if (m_stopped)
            return;
        m_workflowActive = false;
        m_chart->appendWorkflowEvent(obj({ { "type", "workflow_done" },
                                           { "status", "done" } }));
        m_chart->setBusy(false);
        m_chart->showToast(QStringLiteral("工作流执行完成"));
    });
}

bool DemoHost::runCommand(const QString &text, const QVariantList &attachments)
{
    Q_UNUSED(attachments);
    if (!text.startsWith(QLatin1Char('/')))
        return false;

    const int space = text.indexOf(QLatin1Char(' '));
    const QString name = (space < 0 ? text : text.left(space)).toLower();
    const QString rest = space < 0 ? QString() : text.mid(space + 1).trimmed();

    if (name == QLatin1String("/history")) {
        loadHistory();
        return true;
    }
    if (name == QLatin1String("/clear")) {
        m_chart->clearMessages();
        return true;
    }
    if (name == QLatin1String("/toast")) {
        m_chart->showToast(rest.isEmpty() ? QStringLiteral("这是一条提示") : rest);
        return true;
    }
    if (name == QLatin1String("/settings")) {
        m_chart->openSettings(rest);
        return true;
    }

    m_chart->appendUserMessage(text);
    m_chart->clearAttachments();
    m_chart->setBusy(true);

    if (name == QLatin1String("/fail")) {
        QTimer::singleShot(300, m_chart, [this, text] {
            m_chart->setBusy(false);
            m_chart->appendFailure(QStringLiteral("请求失败：upstream connect error（503）"), true,
                                   text, text, QVariantList());
        });
        return true;
    }
    if (name == QLatin1String("/approve")) {
        QTimer::singleShot(300, m_chart, [this] {
            m_chart->appendApproval(approvalEvent(QStringLiteral("call_ap_%1").arg(++m_callSeq)));
            m_chart->setBusy(false);
        });
        return true;
    }
    if (name == QLatin1String("/workflow")) {
        QTimer::singleShot(300, m_chart, [this] { runWorkflow(workflowSteps()); });
        return true;
    }
    if (name == QLatin1String("/options")) {
        QTimer::singleShot(300, m_chart, [this] {
            m_chart->appendAssistantMessage(QStringLiteral(
                "故障段已隔离，请选择下一步：\n\n"
                "```json\n"
                "{\"options\":[{\"value\":\"继续执行转供电\",\"label\":\"继续转供电\",\"style\":\"primary\"},"
                "{\"value\":\"生成处置报告\",\"label\":\"生成处置报告\"},"
                "{\"value\":\"取消操作\",\"label\":\"取消\",\"style\":\"danger\"}]}\n"
                "```\n"));
            m_chart->finishAssistant();
            m_chart->setBusy(false);
        });
        return true;
    }
    if (name == QLatin1String("/params")) {
        QTimer::singleShot(300, m_chart, [this] {
            m_chart->appendAssistantMessage(QStringLiteral(
                "请确认遥控参数：\n\n"
                "```json\n"
                "{\"tool_params\":{\"tool\":\"switch_order\",\"params\":["
                "{\"name\":\"device\",\"value\":\"LK-07\",\"description\":\"联络开关编号\"},"
                "{\"name\":\"action\",\"value\":\"close\",\"description\":\"合闸\"},"
                "{\"name\":\"delay_s\",\"value\":5,\"description\":\"延时秒数\"}]},"
                "\"options\":[{\"value\":\"确认下发\",\"label\":\"确认下发\",\"style\":\"primary\"},"
                "{\"value\":\"取消\",\"label\":\"取消\",\"style\":\"danger\"}]}\n"
                "```\n"));
            m_chart->finishAssistant();
            m_chart->setBusy(false);
        });
        return true;
    }

    m_chart->setBusy(false);
    m_chart->showToast(QStringLiteral("未知指令：%1").arg(name));
    return true;
}

// ------------------------------------------------------------ main

int main(int argc, char *argv[])
{
    QString shotPath;
    bool emptyShot = false;
    QStringList rest;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--shot") && i + 1 < argc) {
            shotPath = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == QLatin1String("--empty")) {
            emptyShot = true;
        } else {
            rest << arg;
        }
    }
    // 截图模式用 offscreen 平台插件，避免弹窗口
    if (!shotPath.isEmpty())
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("QtChartWidgetDemo"));

    auto *chart = new ChartWidget;
    chart->setWindowTitle(QStringLiteral("GridStar AI · QtChartWidget"));
    chart->resize(460, 820);

    // 宿主对象与界面同生命周期，进程退出即释放
    auto *host = new DemoHost(chart);
    host->populate();
    if (!emptyShot)
        host->loadHistory();

    chart->show();
    chart->focusInput();

    if (!shotPath.isEmpty()) {
        QTimer::singleShot(500, &app, [chart, shotPath] {
            const QPixmap shot = chart->grab();
            if (shot.isNull() || !shot.save(shotPath)) {
                qCritical("截图保存失败: %s", qPrintable(shotPath));
                qApp->exit(1);
                return;
            }
            qInfo("已保存截图 %s (%dx%d)", qPrintable(shotPath), shot.width(), shot.height());
            qApp->exit(0);
        });
    }
    return app.exec();
}

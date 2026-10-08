// QtChartWidget 示例宿主。
//
// ChartWidget 是纯 UI 库：宿主用 setter 推数据、用信号收交互。本文件演示一轮完整的
// 流式对话（思考 → 正文 → 工具调用 → 结果 → token 统计）、审批卡、结构化卡片、
// 工作流卡、失败重试与设置中心，并支持 `--shot <file>` 离屏截图与 webui 对照。
//
// 默认使用内置假后端；传 `--live --api http://127.0.0.1:1231` 可连接真实
// agent.app，对健康检查、会话、模型、MCP、SSE 对话与审批做端到端验证。
//
// 输入框里可用的演示指令：
//   /history 载入示例历史   /clear 清空        /fail 制造失败气泡
//   /approve 审批卡         /workflow 工作流卡  /options 选项卡
//   /params 工具参数确认卡  /toast <文本>      /settings [tab]
#include <chartwidget.h>

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLineEdit>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <QtMath>

#include <functional>
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

// GET /usage/stats 的演示响应：按小时切桶，供应商/模型两套聚合，candidates 不随筛选缩水
QVariantMap demoUsageStats(const QString &start, const QString &end, const QString &provider,
                           const QString &model)
{
    // 三个模型固定占比，便于演示分组 / 供应商筛选与占比图
    struct Share { const char *model; const char *provider; const char *label; double weight; };
    const Share shares[] = {
        { "gs-pro-32k", "gridstar", "GS-Pro 32K", 0.5 },
        { "gs-lite-8k", "gridstar", "GS-Lite 8K", 0.2 },
        { "deepseek-chat", "deepseek", "DeepSeek Chat", 0.3 },
    };

    const QDateTime startDt = QDateTime::fromString(start, QStringLiteral("yyyy-MM-ddTHH:mm"));
    const QDateTime endDt = QDateTime::fromString(end, QStringLiteral("yyyy-MM-ddTHH:mm"));
    QVariantList buckets;
    qint64 tInput = 0, tOutput = 0, tMeasured = 0, tEstimated = 0, tTurns = 0;
    QDateTime cursor = startDt;
    for (int index = 0; cursor <= endDt && index < 3000; ++index) {
        const double wave = 0.55 + 0.45 * qSin(index * 0.7);
        const qint64 inTok = qint64(80000 * wave) + 12000;
        const qint64 outTok = qint64(inTok * 0.22) + 400;
        const qint64 total = inTok + outTok;
        // 每 11 桶造一条估算兜底记录（实测与估算分开记账）
        const bool estimated = (index % 11 == 0);
        QVariantMap bucket;
        bucket.insert(QStringLiteral("t"), cursor.toString(QStringLiteral("yyyy-MM-ddTHH")));
        bucket.insert(QStringLiteral("input"), inTok);
        bucket.insert(QStringLiteral("output"), outTok);
        bucket.insert(QStringLiteral("total"), total);
        bucket.insert(QStringLiteral("measured"), estimated ? 0 : total);
        bucket.insert(QStringLiteral("estimated"), estimated ? total : 0);
        bucket.insert(QStringLiteral("turns"), 2 + index % 3);
        buckets.append(bucket);
        tInput += inTok;
        tOutput += outTok;
        tMeasured += estimated ? 0 : total;
        tEstimated += estimated ? total : 0;
        tTurns += 2 + index % 3;
        cursor = cursor.addSecs(3600);
    }

    const qint64 windowTotal = tInput + tOutput;
    QVariantList providers;
    QVariantList models;
    QVariantList candidateProviders;
    QVariantList candidateModels;
    for (const Share &share : shares) {
        const QString providerId = QString::fromUtf8(share.provider);
        const qint64 total = qint64(windowTotal * share.weight);
        QVariantMap row = obj({ { "provider", providerId },
                                { "label", QString::fromUtf8(share.label) },
                                { "model", providerId + QLatin1Char('/') + share.model },
                                { "total", total },
                                { "input", qint64(total * 0.8) },
                                { "output", total - qint64(total * 0.8) },
                                { "measured", qint64(total * 0.9) },
                                { "estimated", total - qint64(total * 0.9) },
                                { "cache_read", qint64(total * 0.35) },
                                { "cache_write", qint64(total * 0.05) },
                                { "turns", tTurns / 3 },
                                { "sessions", 3 } });
        candidateModels.append(row);
        if (!candidateProviders.isEmpty()) {
            QVariantMap existing = candidateProviders.last().toMap();
            if (existing.value(QStringLiteral("provider")).toString() == providerId)
                continue;
        }
        QVariantMap providerRow = row;
        providerRow.remove(QStringLiteral("model"));
        candidateProviders.append(providerRow);
        providers.append(providerRow);
    }
    for (const QVariant &v : candidateModels) {
        const QVariantMap row = v.toMap();
        const QString rowProvider = row.value(QStringLiteral("provider")).toString();
        const QString rowModel = row.value(QStringLiteral("model")).toString();
        if (!provider.isEmpty() && rowProvider != provider)
            continue;
        if (!model.isEmpty() && rowModel != model)
            continue;
        models.append(row);
    }

    QVariantMap out;
    out.insert(QStringLiteral("range"),
               obj({ { "start", start }, { "end", end }, { "resolution", "hour" } }));
    out.insert(QStringLiteral("totals"),
               obj({ { "total", tInput + tOutput },
                     { "input", tInput },
                     { "output", tOutput },
                     { "measured", tMeasured },
                     { "estimated", tEstimated },
                     { "cache_read", qint64(tInput * 0.35) },
                     { "cache_write", qint64(tInput * 0.05) },
                     { "reasoning", qint64(tOutput * 0.15) },
                     { "turns", tTurns },
                     { "sessions", 4 } }));
    out.insert(QStringLiteral("buckets"), buckets);
    out.insert(QStringLiteral("providers"), providers);
    out.insert(QStringLiteral("models"), models);
    out.insert(QStringLiteral("candidates"),
               obj({ { "providers", candidateProviders }, { "models", candidateModels } }));
    return out;
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
    explicit DemoHost(ChartWidget *chart, bool live = false,
                      const QString &apiBase = QStringLiteral("http://127.0.0.1:1231"));

    void populate();
    void loadHistory();
    // 轨迹视图示例数据（宿主从 /sessions/{id}/trajectory 拉到的 events）
    void loadTrajectory();
    // 初始数据推完之后再接信号：否则 populate 的 setter 会触发下面这些处理器里
    // 的 showToast，启动即弹一条提示压住阶段计划面板，干扰离屏对照图
    void wire();
    // live 模式启动后异步拉取服务状态与首屏数据；fake 模式不调用。
    void bootstrap();

private:
    using JsonCallback = std::function<void(const QJsonObject &, int)>;

    void startTurn(const QString &message, const QString &display,
                   const QVariantList &attachments);
    bool runCommand(const QString &text, const QVariantList &attachments);
    void streamChunk(int index);
    void runWorkflow(const QVariantList &steps);
    void attachFiles(const QStringList &paths);
    void uploadFiles(const QStringList &paths);

    // ---- 真实后台（agent.app）适配层 ----
    QString apiUrl(const QString &path) const;
    QString encodedId(const QString &id) const;
    void requestJson(const QString &method, const QString &path, const QJsonObject &payload,
                     const JsonCallback &callback);
    void processSseFrame(const QByteArray &frame, const QString &message, const QString &display,
                         const QVariantList &attachments);
    void failLiveTurn(const QString &error, const QString &message, const QString &display,
                      const QVariantList &attachments, bool retryable = true);
    void startLiveTurn(const QString &message, const QString &display,
                       const QVariantList &attachments);
    void checkConnection(bool notify = false);
    void loadModels();
    void loadSkills();
    void loadMcp(bool refresh);
    void loadConfig();
    void refreshSessions(bool loadFirst);
    void loadSession(const QString &id);
    void createSession();
    void loadTrajectoryRemote();
    void saveConfig(const QVariantMap &config, const QVariant &revision);
    void testProvider(const QString &providerId);
    void readProviderModels(const QString &providerId);
    void requestUsageStats(const QString &requestId, const QString &start, const QString &end,
                           const QString &provider, const QString &model);
    void resolveLiveApproval(const QString &callId, bool approved, const QVariantMap &args);
    void renameSession(const QString &id);

    ChartWidget *m_chart;
    QStringList m_chunks;
    int m_callSeq = 0;
    int m_attachSeq = 0;
    bool m_recording = false;
    bool m_stopped = false;
    bool m_workflowActive = false;

    bool m_live = false;
    QString m_apiBase;
    QNetworkAccessManager *m_network = nullptr;
    QString m_sessionId;
    QNetworkReply *m_chatReply = nullptr;
    QByteArray m_sseBuffer;
    bool m_streamDone = false;
    bool m_streamFailed = false;
    QVariantMap m_config;
    QVariant m_configRevision;
    QVariantList m_liveModels;
    QVariantList m_liveSkills;
    QVariantList m_liveMcpTools;
    bool m_healthReachable = false;
    bool m_configReady = false;
    bool m_runtimeReady = false;
    bool m_mcpConnected = false;
    bool m_bootstrapStarted = false;
    // 会话存储异常每次启动只提示一次（对应 webui warnStorageIssues 的 warnedStorageIssues 集合）
    QSet<QString> m_warnedStorageIssues;
};

DemoHost::DemoHost(ChartWidget *chart, bool live, const QString &apiBase)
    : m_chart(chart), m_live(live), m_apiBase(apiBase.trimmed())
{
    while (m_apiBase.endsWith(QLatin1Char('/')))
        m_apiBase.chop(1);
    if (m_apiBase.isEmpty())
        m_apiBase = QStringLiteral("http://127.0.0.1:1231");
    if (m_live)
        m_network = new QNetworkAccessManager(m_chart);
}

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

QString DemoHost::apiUrl(const QString &path) const
{
    return m_apiBase + (path.startsWith(QLatin1Char('/')) ? path
                                                          : QStringLiteral("/") + path);
}

QString DemoHost::encodedId(const QString &id) const
{
    return QString::fromLatin1(QUrl::toPercentEncoding(id));
}

void DemoHost::requestJson(const QString &method, const QString &path,
                           const QJsonObject &payload, const JsonCallback &callback)
{
    if (!m_network) {
        if (callback)
            callback(QJsonObject{ { QStringLiteral("error"), QStringLiteral("网络未初始化") } }, 0);
        return;
    }

    QNetworkRequest request(QUrl(apiUrl(path)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = nullptr;
    if (method == QLatin1String("GET"))
        reply = m_network->get(request);
    else if (method == QLatin1String("POST"))
        reply = m_network->post(request, body);
    else if (method == QLatin1String("PUT"))
        reply = m_network->sendCustomRequest(request, "PUT", body);
    else if (method == QLatin1String("DELETE"))
        reply = m_network->deleteResource(request);
    else
        reply = m_network->sendCustomRequest(request, method.toUtf8(), body);

    QObject::connect(reply, &QNetworkReply::finished, m_chart, [reply, callback] {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
        QJsonObject object;
        if (document.isObject()) {
            object = document.object();
        } else {
            object.insert(QStringLiteral("error"),
                          parseError.error == QJsonParseError::NoError
                              ? QString::fromUtf8(raw).trimmed()
                              : QStringLiteral("响应不是 JSON：%1").arg(parseError.errorString()));
        }
        if (status == 0 && !object.contains(QStringLiteral("error")))
            object.insert(QStringLiteral("error"), reply->errorString());
        reply->deleteLater();
        if (callback)
            callback(object, status);
    });
}

void DemoHost::bootstrap()
{
    if (!m_live || m_bootstrapStarted)
        return;
    m_bootstrapStarted = true;
    m_chart->setConnectionState(QStringLiteral("checking"), QStringLiteral("连接后台…"));
    m_chart->setConfigLoaded(false);
    checkConnection(false);
    loadConfig();
    loadModels();
    loadSkills();
    refreshSessions(true);
}

void DemoHost::checkConnection(bool notify)
{
    if (!m_live)
        return;
    m_chart->setConnectionState(QStringLiteral("checking"),
                                notify ? QStringLiteral("检测中") : QStringLiteral("连接后台…"));
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/health"), QJsonObject(),
        [this, notify](const QJsonObject &health, int status) {
            if (status < 200 || status >= 300) {
                m_healthReachable = false;
                m_configReady = false;
                m_runtimeReady = false;
                m_mcpConnected = false;
                const QString error = health.value(QStringLiteral("error")).toString();
                const QString label =
                    status == 0 ? QStringLiteral("服务不可用")
                                : QStringLiteral("服务异常 · HTTP %1").arg(status);
                m_chart->setConnectionState(QStringLiteral("offline"), label);
                m_chart->setConfigLoaded(false);
                m_chart->setMcpTools(QVariantList(), false, false,
                                     error.isEmpty()
                                         ? QStringLiteral("后台不可达")
                                         : error);
                if (notify)
                    m_chart->showToast(
                        error.isEmpty() ? QStringLiteral("无法连接后台：%1").arg(m_apiBase)
                                        : QStringLiteral("连接失败：%1").arg(error));
                qWarning() << "[demo:live] health failed" << status << error;
                return;
            }

            m_healthReachable = true;
            m_configReady = health.value(QStringLiteral("config_loaded")).toBool(false);
            m_runtimeReady = health.value(QStringLiteral("runtime_ready")).toBool(false);
            requestJson(
                QStringLiteral("GET"), QStringLiteral("/mcp/tools"), QJsonObject(),
                [this, notify](const QJsonObject &mcp, int mcpStatus) {
                    m_mcpConnected = (mcpStatus >= 200 && mcpStatus < 300)
                                     && mcp.value(QStringLiteral("connected")).toBool(false);
                    const QString mcpError =
                        mcp.value(QStringLiteral("error")).toString();
                    const QVariantList tools =
                        mcp.value(QStringLiteral("tools")).toArray().toVariantList();
                    m_liveMcpTools = tools;
                    m_chart->setMcpTools(tools, m_mcpConnected, false, mcpError);

                    if (!m_configReady || !m_runtimeReady) {
                        m_chart->setConnectionState(
                            QStringLiteral("checking"),
                            m_configReady ? QStringLiteral("模型运行时就绪中")
                                          : QStringLiteral("服务未就绪"));
                    } else if (!m_mcpConnected) {
                        m_chart->setConnectionState(QStringLiteral("checking"),
                                                    QStringLiteral("MCP 未连接"));
                    } else {
                        m_chart->setConnectionState(QStringLiteral("online"),
                                                    QStringLiteral("Agent 已连接"));
                    }

                    if (notify) {
                        if (!m_configReady)
                            m_chart->showToast(QStringLiteral("后台已响应，但尚未加载配置"));
                        else if (!m_runtimeReady)
                            m_chart->showToast(QStringLiteral("后台已响应，但模型运行时未就绪"));
                        else if (!m_mcpConnected)
                            m_chart->showToast(QStringLiteral("后台已响应，但 MCP 未连接"));
                        else
                            m_chart->showToast(QStringLiteral("Agent 后台连接正常"));
                    }
                    qInfo() << "[demo:live] health ok; config=" << m_configReady
                            << "runtime=" << m_runtimeReady << "mcp=" << m_mcpConnected;
                });
        });
}

void DemoHost::loadModels()
{
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/config/models"), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                qWarning() << "[demo:live] models failed" << status
                           << object.value(QStringLiteral("error")).toString();
                return;
            }
            m_liveModels = object.value(QStringLiteral("models")).toArray().toVariantList();
            m_chart->setModels(m_liveModels);
            const QString defaultModel =
                object.value(QStringLiteral("default_model")).toString();
            const QString current = m_chart->currentModel();
            bool currentAvailable = false;
            for (const QVariant &item : m_liveModels) {
                if (item.toMap().value(QStringLiteral("key")).toString() == current) {
                    currentAvailable = true;
                    break;
                }
            }
            if (!defaultModel.isEmpty() && (!currentAvailable || current.isEmpty()))
                m_chart->setCurrentModel(defaultModel);
            if (!m_liveModels.isEmpty())
                m_chart->setConfigLoaded(true);
        });
}

void DemoHost::loadSkills()
{
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/skills"), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                const QString error = object.value(QStringLiteral("error")).toString();
                m_chart->setSettingsSkills(QVariantList(), false, error);
                return;
            }
            m_liveSkills = object.value(QStringLiteral("skills")).toArray().toVariantList();
            m_chart->setSkills(m_liveSkills);
            m_chart->setSettingsSkills(m_liveSkills, false, QString());
            qInfo() << "[demo:live] skills loaded:" << m_liveSkills.size();
        });
}

void DemoHost::loadMcp(bool refresh)
{
    const QString path = refresh ? QStringLiteral("/mcp/tools?refresh=1")
                                 : QStringLiteral("/mcp/tools");
    m_chart->setMcpTools(m_liveMcpTools, m_mcpConnected, true, QString());
    requestJson(QStringLiteral("GET"), path, QJsonObject(),
                [this](const QJsonObject &object, int status) {
                    const bool connected = (status >= 200 && status < 300)
                                           && object.value(QStringLiteral("connected")).toBool(false);
                    const QString error = object.value(QStringLiteral("error")).toString();
                    m_liveMcpTools =
                        object.value(QStringLiteral("tools")).toArray().toVariantList();
                    m_mcpConnected = connected;
                    m_chart->setMcpTools(m_liveMcpTools, connected, false, error);
                    if (m_healthReachable && m_configReady && m_runtimeReady) {
                        m_chart->setConnectionState(
                            connected ? QStringLiteral("online")
                                      : QStringLiteral("checking"),
                            connected ? QStringLiteral("Agent 已连接")
                                      : QStringLiteral("MCP 未连接"));
                    }
                });
}

void DemoHost::loadConfig()
{
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/config"), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                qWarning() << "[demo:live] config failed" << status
                           << object.value(QStringLiteral("error")).toString();
                return;
            }
            const QJsonValue configValue = object.value(QStringLiteral("config"));
            if (!configValue.isObject()) {
                m_config.clear();
                m_configRevision = QVariant();
                m_chart->setConfigLoaded(false);
                m_chart->setConfigWarning(QStringLiteral("后台尚未配置模型，请先在设置中心保存配置"));
                return;
            }
            m_config = configValue.toObject().toVariantMap();
            m_configRevision = object.value(QStringLiteral("revision")).toVariant();
            m_chart->setSettingsDraft(m_config, m_configRevision);
            m_chart->setConfigLoaded(true);
            qInfo() << "[demo:live] config loaded, revision =" << m_configRevision
                    << "providers =" << m_config.value(QStringLiteral("providers")).toList().size();
        });
}

void DemoHost::refreshSessions(bool loadFirst)
{
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/sessions"), QJsonObject(),
        [this, loadFirst](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                qWarning() << "[demo:live] sessions failed" << status
                           << object.value(QStringLiteral("error")).toString();
                return;
            }
            const QVariantList sessions =
                object.value(QStringLiteral("sessions")).toArray().toVariantList();
            m_chart->setSessions(sessions);
            // app.js warnStorageIssues：同名异常只弹一次，否则每次刷新都盖掉别的提示
            const QVariantList issues =
                object.value(QStringLiteral("storage_issues")).toArray().toVariantList();
            for (const QVariant &issue : issues) {
                const QString text = issue.toString();
                if (text.isEmpty() || m_warnedStorageIssues.contains(text))
                    continue;
                m_warnedStorageIssues.insert(text);
                m_chart->showToast(QStringLiteral("会话存储异常：") + text);
            }
            if (!loadFirst)
                return;
            if (sessions.isEmpty()) {
                createSession();
                return;
            }
            QString wanted = m_sessionId;
            bool found = false;
            for (const QVariant &item : sessions) {
                if (item.toMap().value(QStringLiteral("id")).toString() == wanted) {
                    found = true;
                    break;
                }
            }
            if (!found)
                wanted = sessions.first().toMap().value(QStringLiteral("id")).toString();
            if (!wanted.isEmpty())
                loadSession(wanted);
        });
}

void DemoHost::createSession()
{
    QJsonObject body;
    body.insert(QStringLiteral("title"), QStringLiteral("新对话"));
    body.insert(QStringLiteral("model_id"), m_chart->currentModel());
    requestJson(
        QStringLiteral("POST"), QStringLiteral("/sessions"), body,
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->showToast(
                    QStringLiteral("新建会话失败：%1")
                        .arg(object.value(QStringLiteral("error")).toString()));
                return;
            }
            m_sessionId = object.value(QStringLiteral("id")).toString();
            m_chart->clearMessages();
            m_chart->setCurrentSessionTitle(
                object.value(QStringLiteral("title")).toString(QStringLiteral("新对话")));
            m_chart->setPhasePlan(QVariant());
            m_chart->setTrajectoryEvents(QVariantList());
            m_chart->focusInput();
            refreshSessions(false);
            qInfo() << "[demo:live] session created" << m_sessionId;
        });
}

void DemoHost::loadSession(const QString &id)
{
    if (id.isEmpty())
        return;
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/sessions/") + encodedId(id), QJsonObject(),
        [this, id](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->showToast(
                    QStringLiteral("载入会话失败：%1")
                        .arg(object.value(QStringLiteral("error")).toString()));
                return;
            }
            const QJsonObject meta = object.value(QStringLiteral("meta")).toObject();
            m_sessionId = meta.value(QStringLiteral("id")).toString(id);
            m_chart->setCurrentSessionTitle(
                meta.value(QStringLiteral("title")).toString(QStringLiteral("新对话")));
            const QString modelId = meta.value(QStringLiteral("model_id")).toString();
            if (!modelId.isEmpty())
                m_chart->setCurrentModel(modelId);
            m_chart->setHistory(object.value(QStringLiteral("messages")).toArray().toVariantList());
            const QJsonValue plan = object.value(QStringLiteral("plan"));
            m_chart->setPhasePlan(plan.isNull() || plan.isUndefined() ? QVariant()
                                                                      : plan.toVariant());
            loadTrajectoryRemote();
            qInfo() << "[demo:live] session loaded" << m_sessionId
                    << "messages ="
                    << object.value(QStringLiteral("messages")).toArray().size();
        });
}

void DemoHost::loadTrajectoryRemote()
{
    if (m_sessionId.isEmpty()) {
        m_chart->setTrajectoryEvents(QVariantList());
        return;
    }
    requestJson(
        QStringLiteral("GET"),
        QStringLiteral("/sessions/") + encodedId(m_sessionId)
            + QStringLiteral("/trajectory"),
        QJsonObject(),
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                qWarning() << "[demo:live] trajectory failed" << status;
                return;
            }
            m_chart->setTrajectoryEvents(
                object.value(QStringLiteral("events")).toArray().toVariantList());
        });
}

void DemoHost::saveConfig(const QVariantMap &config, const QVariant &revision)
{
    m_chart->setSettingsStatus(QStringLiteral("正在写入后台 config.json…"));
    QJsonObject body;
    body.insert(QStringLiteral("revision"), QJsonValue::fromVariant(revision));
    body.insert(QStringLiteral("config"), QJsonObject::fromVariantMap(config));
    requestJson(
        QStringLiteral("POST"), QStringLiteral("/config"), body,
        [this](const QJsonObject &object, int status) {
            if (status == 409) {
                m_chart->setSettingsStatus(
                    QStringLiteral("保存冲突：配置已被其他客户端修改，请刷新后重试"));
                return;
            }
            if (status < 200 || status >= 300) {
                m_chart->setSettingsStatus(
                    QStringLiteral("保存失败：%1")
                        .arg(object.value(QStringLiteral("error")).toString()));
                return;
            }
            const QJsonValue saved = object.value(QStringLiteral("config"));
            if (saved.isObject())
                m_config = saved.toObject().toVariantMap();
            m_configRevision = object.value(QStringLiteral("revision")).toVariant();
            m_chart->setSettingsDraft(m_config, m_configRevision);
            m_chart->settingsSaved();
            m_chart->showToast(QStringLiteral("配置已保存到后台"));
            loadModels();
            checkConnection(false);
        });
}

void DemoHost::testProvider(const QString &providerId)
{
    QVariantMap provider;
    const QVariantList providers =
        m_config.value(QStringLiteral("providers")).toList();
    for (const QVariant &item : providers) {
        if (item.toMap().value(QStringLiteral("id")).toString() == providerId) {
            provider = item.toMap();
            break;
        }
    }
    if (provider.isEmpty()) {
        m_chart->setSettingsStatus(
            QStringLiteral("未找到 Provider：%1").arg(providerId));
        return;
    }
    m_chart->setProviderBusy(true, false);
    m_chart->setSettingsStatus(QStringLiteral("正在测试 %1…").arg(providerId));
    QJsonObject body;
    body.insert(QStringLiteral("provider"), QJsonObject::fromVariantMap(provider));
    requestJson(
        QStringLiteral("POST"), QStringLiteral("/config/providers/test"), body,
        [this, providerId](const QJsonObject &object, int status) {
            m_chart->setProviderBusy(false, false);
            if (status < 200 || status >= 300) {
                m_chart->setSettingsStatus(
                    QStringLiteral("%1 测试失败：%2")
                        .arg(providerId,
                             object.value(QStringLiteral("error")).toString()));
                return;
            }
            m_chart->setSettingsStatus(QStringLiteral("%1 连通正常").arg(providerId));
        });
}

void DemoHost::readProviderModels(const QString &providerId)
{
    QVariantMap provider;
    const QVariantList providers =
        m_config.value(QStringLiteral("providers")).toList();
    for (const QVariant &item : providers) {
        if (item.toMap().value(QStringLiteral("id")).toString() == providerId) {
            provider = item.toMap();
            break;
        }
    }
    if (provider.isEmpty()) {
        m_chart->setSettingsStatus(
            QStringLiteral("未找到 Provider：%1").arg(providerId));
        return;
    }
    m_chart->setProviderBusy(false, true);
    m_chart->setSettingsStatus(QStringLiteral("正在读取 %1 模型列表…").arg(providerId));
    QJsonObject body;
    body.insert(QStringLiteral("provider"), QJsonObject::fromVariantMap(provider));
    requestJson(
        QStringLiteral("POST"), QStringLiteral("/config/providers/models"), body,
        [this, providerId](const QJsonObject &object, int status) {
            m_chart->setProviderBusy(false, false);
            if (status < 200 || status >= 300) {
                m_chart->setSettingsStatus(
                    QStringLiteral("%1 模型读取失败：%2")
                        .arg(providerId,
                             object.value(QStringLiteral("error")).toString()));
                return;
            }
            const QVariantList models =
                object.value(QStringLiteral("models")).toArray().toVariantList();
            m_chart->setDiscoveredModels(providerId, models);
            m_chart->setSettingsStatus(
                QStringLiteral("读取到 %1 个模型").arg(models.size()));
        });
}

void DemoHost::requestUsageStats(const QString &requestId, const QString &start,
                                 const QString &end, const QString &provider,
                                 const QString &model)
{
    QUrlQuery query;
    if (!start.isEmpty())
        query.addQueryItem(QStringLiteral("start"), start);
    if (!end.isEmpty())
        query.addQueryItem(QStringLiteral("end"), end);
    if (!provider.isEmpty())
        query.addQueryItem(QStringLiteral("provider"), provider);
    if (!model.isEmpty())
        query.addQueryItem(QStringLiteral("model"), model);
    const QString path =
        query.isEmpty()
            ? QStringLiteral("/usage/stats")
            : QStringLiteral("/usage/stats?") + query.query(QUrl::FullyEncoded);
    requestJson(
        QStringLiteral("GET"), path, QJsonObject(),
        [this, requestId](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->setUsageLoadFailed(
                    requestId, object.value(QStringLiteral("error")).toString(
                                   QStringLiteral("用量统计读取失败")));
                return;
            }
            m_chart->setUsageStats(requestId, object.toVariantMap());
        });
}

void DemoHost::resolveLiveApproval(const QString &callId, bool approved,
                                   const QVariantMap &args)
{
    if (m_sessionId.isEmpty()) {
        m_chart->reEnableApproval(callId);
        m_chart->showToast(QStringLiteral("当前没有可提交审批的会话"));
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("approved"), approved);
    body.insert(QStringLiteral("args"), QJsonObject::fromVariantMap(args));
    const QString path =
        QStringLiteral("/sessions/") + encodedId(m_sessionId)
        + QStringLiteral("/tool-approvals/") + encodedId(callId);
    requestJson(
        QStringLiteral("POST"), path, body,
        [this, callId, approved](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->reEnableApproval(callId);
                m_chart->showToast(
                    QStringLiteral("审批提交失败：%1")
                        .arg(object.value(QStringLiteral("error")).toString()));
                return;
            }
            m_chart->resolveApproval(callId, approved);
            m_chart->showToast(approved ? QStringLiteral("已批准，后台继续执行")
                                        : QStringLiteral("已拒绝该操作"));
            refreshSessions(false);
        });
}

void DemoHost::renameSession(const QString &id)
{
    QString initial = m_chart->currentSessionTitle();
    if (id != m_sessionId || initial.isEmpty()) {
        requestJson(
            QStringLiteral("GET"), QStringLiteral("/sessions/") + encodedId(id),
            QJsonObject(),
            [this, id](const QJsonObject &object, int status) {
                QString title;
                if (status >= 200 && status < 300)
                    title = object.value(QStringLiteral("meta"))
                                .toObject()
                                .value(QStringLiteral("title"))
                                .toString();
                bool ok = false;
                const QString next =
                    QInputDialog::getText(m_chart, QStringLiteral("重命名会话"),
                                          QStringLiteral("新标题"), QLineEdit::Normal,
                                          title, &ok)
                        .trimmed();
                if (!ok || next.isEmpty())
                    return;
                QJsonObject body;
                body.insert(QStringLiteral("title"), next);
                requestJson(
                    QStringLiteral("PUT"),
                    QStringLiteral("/sessions/") + encodedId(id)
                        + QStringLiteral("/rename"),
                    body,
                    [this, id, next](const QJsonObject &, int renameStatus) {
                        if (renameStatus >= 200 && renameStatus < 300) {
                            if (id == m_sessionId)
                                m_chart->setCurrentSessionTitle(next);
                            refreshSessions(false);
                            m_chart->showToast(QStringLiteral("会话已重命名"));
                        } else {
                            m_chart->showToast(QStringLiteral("重命名失败"));
                        }
                    });
            });
        return;
    }

    bool ok = false;
    const QString next =
        QInputDialog::getText(m_chart, QStringLiteral("重命名会话"),
                              QStringLiteral("新标题"), QLineEdit::Normal, initial, &ok)
            .trimmed();
    if (!ok || next.isEmpty())
        return;
    QJsonObject body;
    body.insert(QStringLiteral("title"), next);
    requestJson(
        QStringLiteral("PUT"),
        QStringLiteral("/sessions/") + encodedId(id) + QStringLiteral("/rename"), body,
        [this, id, next](const QJsonObject &, int status) {
            if (status < 200 || status >= 300) {
                m_chart->showToast(QStringLiteral("重命名失败"));
                return;
            }
            if (id == m_sessionId)
                m_chart->setCurrentSessionTitle(next);
            refreshSessions(false);
            m_chart->showToast(QStringLiteral("会话已重命名"));
        });
}

void DemoHost::wire()
{
    ChartWidget *c = m_chart;

    QObject::connect(c, &ChartWidget::sendMessage, c,
                     [this](const QString &message, const QString &display,
                            const QVariantList &attachments) {
                         m_chart->setInputText(QString());
                         // 工具参数确认按 webui 口径以 <structured_interaction> 消息回传（真实后端
                         // 识别该标记并回填参数）；demo 没有后端，这里就地给一条回执，
                         // 免得把标记当成普通提问走一遍假流式
                         if (!m_live
                             && message.startsWith(
                                 QStringLiteral("<structured_interaction>"))) {
                             m_chart->appendUserMessage(display.isEmpty()
                                                            ? QStringLiteral("已确认参数")
                                                            : display);
                             m_chart->appendAssistantMessage(
                                 QStringLiteral("已按确认后的参数下发指令（演示）。"));
                             return;
                         }
                         if (m_live)
                             startLiveTurn(message, display, attachments);
                         else
                             startTurn(message, display, attachments);
                     });
    QObject::connect(c, &ChartWidget::retryRequested, c,
                     [this](const QString &message, const QString &display,
                            const QVariantList &attachments) {
                         if (m_live)
                             startLiveTurn(message, display, attachments);
                         else
                             startTurn(message, display, attachments);
                     });
    QObject::connect(c, &ChartWidget::stopRequested, c, [this] {
        m_stopped = true;
        if (m_live) {
            if (!m_sessionId.isEmpty()) {
                requestJson(QStringLiteral("POST"),
                            QStringLiteral("/sessions/") + encodedId(m_sessionId)
                                + QStringLiteral("/cancel"),
                            QJsonObject(), JsonCallback());
            }
            if (m_chatReply)
                m_chatReply->abort();
            m_chart->markCurrentStopped();
            m_chart->finishAssistant();
            m_chart->setBusy(false);
            m_chart->showToast(QStringLiteral("已请求停止后台任务"));
            refreshSessions(false);
            return;
        }
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
        if (m_live) {
            checkConnection(true);
            return;
        }
        m_chart->setConnectionState(QStringLiteral("checking"), QStringLiteral("检测中"));
        QTimer::singleShot(700, m_chart, [this] {
            m_chart->setConnectionState(QStringLiteral("online"), QStringLiteral("服务在线"));
            m_chart->showToast(QStringLiteral("网关连通性正常"));
        });
    });

    QObject::connect(c, &ChartWidget::newSessionRequested, c, [this] {
        if (m_live) {
            createSession();
            return;
        }
        m_chart->clearMessages();
        m_chart->setCurrentSessionTitle(QStringLiteral("新对话"));
        m_chart->focusInput();
        m_chart->showToast(QStringLiteral("已新建会话"));
    });
    QObject::connect(c, &ChartWidget::sessionSelected, c, [this](const QString &id) {
        if (m_live) {
            loadSession(id);
        } else {
            loadHistory();
            m_chart->showToast(QStringLiteral("已载入会话 %1").arg(id));
        }
    });
    QObject::connect(c, &ChartWidget::sessionRenamed, c, [this](const QString &id) {
        if (m_live)
            renameSession(id);
        else
            m_chart->showToast(QStringLiteral("重命名会话 %1（宿主实现）").arg(id));
    });
    QObject::connect(c, &ChartWidget::sessionCleared, c, [this](const QString &id) {
        if (m_live) {
            requestJson(
                QStringLiteral("POST"),
                QStringLiteral("/sessions/") + encodedId(id) + QStringLiteral("/clear"),
                QJsonObject(),
                [this, id](const QJsonObject &object, int status) {
                    if (status < 200 || status >= 300) {
                        m_chart->showToast(
                            QStringLiteral("清空失败：%1")
                                .arg(object.value(QStringLiteral("error")).toString()));
                        return;
                    }
                    if (id == m_sessionId) {
                        m_chart->clearMessages();
                        m_chart->setPhasePlan(QVariant());
                    }
                    m_chart->showToast(QStringLiteral("已清空会话"));
                    refreshSessions(false);
                });
            return;
        }
        m_chart->clearMessages();
        m_chart->showToast(QStringLiteral("已清空会话 %1").arg(id));
    });
    QObject::connect(c, &ChartWidget::sessionDeleted, c, [this](const QString &id) {
        if (m_live) {
            requestJson(
                QStringLiteral("DELETE"),
                QStringLiteral("/sessions/") + encodedId(id), QJsonObject(),
                [this, id](const QJsonObject &object, int status) {
                    if (status < 200 || status >= 300) {
                        m_chart->showToast(
                            QStringLiteral("删除失败：%1")
                                .arg(object.value(QStringLiteral("error")).toString()));
                        return;
                    }
                    if (id == m_sessionId)
                        m_sessionId.clear();
                    m_chart->showToast(QStringLiteral("已删除会话"));
                    refreshSessions(true);
                });
            return;
        }
        m_chart->showToast(QStringLiteral("已删除会话 %1").arg(id));
    });
    QObject::connect(c, &ChartWidget::trajectoryReloadRequested, c, [this] {
        if (m_live)
            loadTrajectoryRemote();
        else
            loadTrajectory();
    });
    // 输入框斜杠面板的「/ 导出对话」：直接打后端导出接口，不必跑一整轮对话
    QObject::connect(c, &ChartWidget::sessionExportRequested, c, [this] {
        if (m_sessionId.isEmpty()) {
            m_chart->showToast(QStringLiteral("当前没有可导出的会话"));
            return;
        }
        if (!m_live) {
            m_chart->showToast(QStringLiteral("已导出会话 %1（宿主实现）").arg(m_sessionId));
            return;
        }
        requestJson(
            QStringLiteral("POST"),
            QStringLiteral("/sessions/") + encodedId(m_sessionId) + QStringLiteral("/export"),
            QJsonObject(),
            [this](const QJsonObject &object, int status) {
                if (status < 200 || status >= 300) {
                    m_chart->showToast(
                        QStringLiteral("导出失败：%1")
                            .arg(object.value(QStringLiteral("error")).toString()));
                    return;
                }
                const QString path = object.value(QStringLiteral("path")).toString();
                m_chart->showToast(QStringLiteral("已导出：%1")
                                       .arg(path.isEmpty() ? QStringLiteral("exports/") : path));
            });
    });

    QObject::connect(c, &ChartWidget::optionChosen, c,
                     [this](const QString &value, const QString &label) {
                         if (m_live)
                             startLiveTurn(value, label, QVariantList());
                         else
                             startTurn(value, label, QVariantList());
                     });
    QObject::connect(c, &ChartWidget::approvalDecided, c,
                     [this](const QString &callId, bool approved, const QVariantMap &args) {
                         qDebug() << "[demo] approval" << callId << approved << args;
                         if (m_live) {
                             resolveLiveApproval(callId, approved, args);
                             return;
                         }
                         // 真实宿主在这里 POST 审批结果，成功后才改卡片状态
                         QTimer::singleShot(400, m_chart, [this, callId, approved] {
                             m_chart->resolveApproval(callId, approved);
                             m_chart->showToast(approved ? QStringLiteral("已批准，指令下发中")
                                                         : QStringLiteral("已拒绝该操作"));
                         });
                     });
    QObject::connect(c, &ChartWidget::workflowRunRequested, c,
                     [this](const QVariantList &steps) {
                         m_chart->setBusy(true);
                         runWorkflow(steps);
                     });

    QObject::connect(c, &ChartWidget::settingsSaveRequested, c,
                     [this](const QVariantMap &config, const QVariant &revision) {
                         if (m_live) {
                             saveConfig(config, revision);
                             return;
                         }
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
        if (m_live) {
            testProvider(providerId);
            return;
        }
        m_chart->setProviderBusy(true, false);
        m_chart->setSettingsStatus(QStringLiteral("正在测试 %1…").arg(providerId));
        QTimer::singleShot(900, m_chart, [this, providerId] {
            m_chart->setProviderBusy(false, false);
            m_chart->setSettingsStatus(QStringLiteral("%1 连通正常（128 ms）").arg(providerId));
        });
    });
    QObject::connect(c, &ChartWidget::readModelsRequested, c, [this](const QString &providerId) {
        if (m_live) {
            readProviderModels(providerId);
            return;
        }
        m_chart->setProviderBusy(false, true);
        m_chart->setSettingsStatus(QStringLiteral("正在读取 %1 模型列表…").arg(providerId));
        QTimer::singleShot(900, m_chart, [this, providerId] {
            m_chart->setDiscoveredModels(providerId, demoModels());
            m_chart->setProviderBusy(false, false);
            m_chart->setSettingsStatus(QStringLiteral("读取到 %1 个模型").arg(demoModels().size()));
        });
    });
    QObject::connect(c, &ChartWidget::refreshSkillsRequested, c, [this] {
        if (m_live) {
            m_chart->setSettingsSkills(m_liveSkills, true, QString());
            loadSkills();
            return;
        }
        m_chart->setSettingsSkills(QVariantList(), true, QString());
        QTimer::singleShot(600, m_chart, [this] {
            m_chart->setSettingsSkills(demoSettingsSkills(), false, QString());
            m_chart->showToast(QStringLiteral("技能已刷新"));
        });
    });
    QObject::connect(c, &ChartWidget::refreshMcpRequested, c, [this] {
        if (m_live) {
            loadMcp(true);
            return;
        }
        m_chart->setMcpTools(QVariantList(), false, true, QString());
        QTimer::singleShot(600, m_chart, [this] {
            m_chart->setMcpTools(demoMcpTools(), true, false, QString());
            m_chart->showToast(QStringLiteral("MCP 已重连"));
        });
    });
    // 设置中心「用量」页：宿主拉 GET /usage/stats 后回填
    QObject::connect(c, &ChartWidget::usageStatsRequested, c,
                     [this](const QString &requestId, const QString &start, const QString &end,
                            const QString &provider, const QString &model) {
                         if (m_live) {
                             requestUsageStats(requestId, start, end, provider, model);
                             return;
                         }
                         QTimer::singleShot(150, m_chart,
                                            [this, requestId, start, end, provider, model] {
                             m_chart->setUsageStats(requestId,
                                                    demoUsageStats(start, end, provider, model));
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
    if (m_live) {
        uploadFiles(paths);
        return;
    }
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

void DemoHost::uploadFiles(const QStringList &paths)
{
    if (!m_network || paths.isEmpty())
        return;

    QVariantList merged = m_chart->attachments();
    QVariantList localItems;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.isFile())
            continue;
        QVariantMap item;
        item.insert(QStringLiteral("id"),
                    QStringLiteral("upload-%1").arg(++m_attachSeq));
        item.insert(QStringLiteral("name"), info.fileName());
        item.insert(QStringLiteral("path"), info.absoluteFilePath());
        item.insert(QStringLiteral("size"), info.size());
        item.insert(QStringLiteral("ext"), info.suffix().toLower());
        item.insert(QStringLiteral("uploading"), true);
        localItems.append(item);
        merged.append(item);
    }
    if (localItems.isEmpty())
        return;
    m_chart->clearAttachments();
    m_chart->addAttachments(merged);

    for (const QVariant &entry : localItems) {
        const QVariantMap item = entry.toMap();
        const QString id = item.value(QStringLiteral("id")).toString();
        const QString path = item.value(QStringLiteral("path")).toString();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QVariantMap failed = item;
            failed.insert(QStringLiteral("uploading"), false);
            failed.insert(QStringLiteral("error"), QStringLiteral("无法读取文件"));
            QVariantList current = m_chart->attachments();
            for (int i = 0; i < current.size(); ++i) {
                QVariantMap existing = current.at(i).toMap();
                if (existing.value(QStringLiteral("id")).toString() == id) {
                    current[i] = failed;
                    break;
                }
            }
            m_chart->clearAttachments();
            m_chart->addAttachments(current);
            continue;
        }

        QUrl url(apiUrl(QStringLiteral("/upload")));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("name"),
                           item.value(QStringLiteral("name")).toString());
        url.setQuery(query);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/octet-stream"));
        QNetworkReply *reply = m_network->post(request, file.readAll());
        QObject::connect(reply, &QNetworkReply::finished, m_chart, [this, reply, item, id] {
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            const QJsonObject object = QJsonDocument::fromJson(raw).object();
            QVariantMap settled = item;
            settled.insert(QStringLiteral("uploading"), false);
            if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300) {
                const QVariantMap uploaded = object.toVariantMap();
                for (auto it = uploaded.constBegin(); it != uploaded.constEnd(); ++it)
                    settled.insert(it.key(), it.value());
                settled.insert(QStringLiteral("id"), id);
            } else {
                const QString error = object.value(QStringLiteral("error")).toString(
                    reply->errorString());
                settled.insert(QStringLiteral("error"), error);
                m_chart->showToast(QStringLiteral("附件上传失败：%1").arg(error));
            }
            QVariantList current = m_chart->attachments();
            for (int i = 0; i < current.size(); ++i) {
                QVariantMap existing = current.at(i).toMap();
                if (existing.value(QStringLiteral("id")).toString() == id) {
                    current[i] = settled;
                    break;
                }
            }
            m_chart->clearAttachments();
            m_chart->addAttachments(current);
            reply->deleteLater();
        });
    }
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

void DemoHost::startLiveTurn(const QString &message, const QString &display,
                             const QVariantList &attachments)
{
    if (!m_live || !m_network)
        return;
    if (m_chart->isBusy()) {
        m_chart->showToast(QStringLiteral("当前还有请求在处理中"));
        return;
    }

    const QString text = message.trimmed();
    if (runCommand(text, attachments))
        return;

    const QString shown = display.isEmpty() ? message : display;
    if (text.isEmpty() && attachments.isEmpty())
        return;

    if (m_sessionId.isEmpty()) {
        QJsonObject body;
        body.insert(QStringLiteral("title"), QStringLiteral("新对话"));
        body.insert(QStringLiteral("model_id"), m_chart->currentModel());
        requestJson(
            QStringLiteral("POST"), QStringLiteral("/sessions"), body,
            [this, message, display, attachments](const QJsonObject &object, int status) {
                if (status < 200 || status >= 300) {
                    m_chart->showToast(
                        QStringLiteral("创建会话失败，消息未发送：%1")
                            .arg(object.value(QStringLiteral("error")).toString()));
                    return;
                }
                m_sessionId = object.value(QStringLiteral("id")).toString();
                m_chart->setCurrentSessionTitle(
                    object.value(QStringLiteral("title")).toString(QStringLiteral("新对话")));
                refreshSessions(false);
                startLiveTurn(message, display, attachments);
            });
        return;
    }

    m_stopped = false;
    m_streamDone = false;
    m_streamFailed = false;
    m_chart->appendUserMessage(shown, attachments);
    m_chart->clearAttachments();
    m_chart->setBusy(true);
    m_chart->startLiveTiming();

    QJsonObject body;
    body.insert(QStringLiteral("session_id"), m_sessionId);
    body.insert(QStringLiteral("message"), message);
    body.insert(QStringLiteral("display_content"),
                shown == message ? QString() : shown);
    body.insert(QStringLiteral("interaction_mode"), m_chart->mode());
    body.insert(QStringLiteral("model_id"), m_chart->currentModel());
    const QString skill = m_chart->currentSkill();
    if (!skill.isEmpty()) {
        QJsonObject selected;
        selected.insert(QStringLiteral("id"), skill);
        selected.insert(QStringLiteral("params"), QJsonObject());
        body.insert(QStringLiteral("selected_skills"), QJsonArray{ selected });
    } else {
        body.insert(QStringLiteral("selected_skills"), QJsonArray());
    }
    body.insert(QStringLiteral("attachments"), QJsonArray::fromVariantList(attachments));

    QNetworkRequest request(QUrl(apiUrl(QStringLiteral("/chat/stream"))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "text/event-stream");
    QNetworkReply *reply =
        m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_chatReply = reply;
    m_sseBuffer.clear();

    QObject::connect(reply, &QNetworkReply::readyRead, m_chart,
                     [this, reply, message, display, attachments] {
                         m_sseBuffer += reply->readAll();
                         m_sseBuffer.replace("\r\n", "\n");
                         int boundary;
                         while ((boundary = m_sseBuffer.indexOf("\n\n")) >= 0) {
                             const QByteArray frame = m_sseBuffer.left(boundary);
                             m_sseBuffer.remove(0, boundary + 2);
                             processSseFrame(frame, message, display, attachments);
                         }
                     });

    QObject::connect(
        reply, &QNetworkReply::finished, m_chart,
        [this, reply, message, display, attachments] {
            m_sseBuffer += reply->readAll();
            m_sseBuffer.replace("\r\n", "\n");
            int boundary;
            while ((boundary = m_sseBuffer.indexOf("\n\n")) >= 0) {
                const QByteArray frame = m_sseBuffer.left(boundary);
                m_sseBuffer.remove(0, boundary + 2);
                processSseFrame(frame, message, display, attachments);
            }
            const QByteArray tail = m_sseBuffer.trimmed();
            m_sseBuffer.clear();
            if (!tail.isEmpty())
                processSseFrame(tail, message, display, attachments);

            if (!m_streamDone && !m_streamFailed) {
                if (m_stopped) {
                    m_chart->markCurrentStopped();
                    m_chart->finishAssistant();
                    m_chart->setBusy(false);
                    refreshSessions(false);
                } else {
                    const int status =
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QJsonObject errorObject =
                        QJsonDocument::fromJson(tail).object();
                    QString error =
                        errorObject.value(QStringLiteral("error")).toString();
                    if (error.isEmpty())
                        error = reply->errorString();
                    if (error.isEmpty())
                        error = QStringLiteral("流式连接已结束（HTTP %1）").arg(status);
                    failLiveTurn(error, message, display, attachments, status != 400
                                                                       && status != 404);
                }
            }
            if (m_chatReply == reply)
                m_chatReply = nullptr;
            reply->deleteLater();
        });
}

void DemoHost::processSseFrame(const QByteArray &frame, const QString &message,
                               const QString &display, const QVariantList &attachments)
{
    if (frame.trimmed().isEmpty())
        return;
    QString type = QStringLiteral("message");
    QList<QByteArray> dataLines;
    const QList<QByteArray> lines = frame.split('\n');
    for (const QByteArray &rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.startsWith("event:"))
            type = QString::fromUtf8(line.mid(6)).trimmed();
        else if (line.startsWith("data:"))
            dataLines.append(line.mid(5).trimmed());
    }
    if (dataLines.isEmpty())
        return;
    QByteArray payload;
    for (int i = 0; i < dataLines.size(); ++i) {
        if (i > 0)
            payload.append('\n');
        payload.append(dataLines.at(i));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (!document.isObject()) {
        qWarning() << "[demo:live] invalid SSE payload" << type << parseError.errorString();
        return;
    }
    const QVariantMap event = document.object().toVariantMap();

    if (type == QLatin1String("text_chunk")) {
        m_chart->appendAssistantText(
            event.value(QStringLiteral("delta")).toString());
    } else if (type == QLatin1String("reasoning_chunk")) {
        m_chart->appendReasoning(
            event.value(QStringLiteral("delta")).toString());
    } else if (type == QLatin1String("plan_updated")) {
        m_chart->setPhasePlan(event.value(QStringLiteral("plan")));
    } else if (type == QLatin1String("tool_call")) {
        const QString name = event.value(QStringLiteral("name")).toString();
        if (name != QLatin1String("ask_user_question")) {
            m_chart->appendToolCall(
                event.value(QStringLiteral("id")).toString(), name,
                event.value(QStringLiteral("args")));
        }
    } else if (type == QLatin1String("tool_result")) {
        const QString name = event.value(QStringLiteral("name")).toString();
        if (name != QLatin1String("ask_user_question")) {
            m_chart->appendToolResult(
                event.value(QStringLiteral("call_id")).toString(), name,
                event.value(QStringLiteral("result")).toString());
        }
    } else if (type == QLatin1String("options_offered")) {
        m_chart->showChoice(event);
    } else if (type == QLatin1String("tool_approval_required")) {
        m_chart->appendApproval(event);
    } else if (type == QLatin1String("notice")) {
        QString notice = event.value(QStringLiteral("message")).toString();
        if (notice.isEmpty())
            notice = QStringLiteral("附件处理提示");
        m_chart->showToast(notice);
    } else if (type == QLatin1String("token_usage")) {
        m_chart->setTokenUsage(
            event.value(QStringLiteral("tokens")).toLongLong(),
            event.value(QStringLiteral("tokens_input")).toLongLong(),
            event.value(QStringLiteral("tokens_output")).toLongLong(),
            event.value(QStringLiteral("tokens_estimated")).toBool());
    } else if (type == QLatin1String("workflow_started")
               || type == QLatin1String("workflow_step")
               || type == QLatin1String("workflow_done")) {
        QVariantMap workflow = event;
        workflow.insert(QStringLiteral("type"), type);
        m_chart->appendWorkflowEvent(workflow);
    } else if (type == QLatin1String("error")) {
        QString error = event.value(QStringLiteral("message")).toString();
        if (error.isEmpty())
            error = QStringLiteral("后台返回错误");
        const bool retryable =
            event.contains(QStringLiteral("retryable"))
                ? event.value(QStringLiteral("retryable")).toBool()
                : true;
        failLiveTurn(error, message, display, attachments,
                     retryable);
    } else if (type == QLatin1String("done")) {
        if (m_streamDone || m_streamFailed)
            return;
        m_streamDone = true;
        m_chart->finishAssistant();

        QVariantMap usage;
        usage.insert(QStringLiteral("total"), event.value(QStringLiteral("tokens")));
        usage.insert(QStringLiteral("input"), event.value(QStringLiteral("tokens_input")));
        usage.insert(QStringLiteral("output"), event.value(QStringLiteral("tokens_output")));
        usage.insert(QStringLiteral("estimated"),
                     event.value(QStringLiteral("tokens_estimated")));
        usage.insert(QStringLiteral("cache_read"),
                     event.value(QStringLiteral("cache_read_tokens")));
        usage.insert(QStringLiteral("reasoning"),
                     event.value(QStringLiteral("reasoning_tokens")));
        usage.insert(QStringLiteral("model"), event.value(QStringLiteral("model")));
        m_chart->setTokenUsageDetail(usage);

        QVariantMap timing;
        timing.insert(QStringLiteral("elapsed"), event.value(QStringLiteral("elapsed_ms")));
        timing.insert(QStringLiteral("think"), event.value(QStringLiteral("think_ms")));
        timing.insert(QStringLiteral("ttft"), event.value(QStringLiteral("ttft_ms")));
        timing.insert(QStringLiteral("tps"), event.value(QStringLiteral("tps")));
        m_chart->setTurnTiming(timing);
        m_chart->setBusy(false);
        refreshSessions(false);
    }
}

void DemoHost::failLiveTurn(const QString &error, const QString &message,
                            const QString &display, const QVariantList &attachments,
                            bool retryable)
{
    if (m_streamDone || m_streamFailed)
        return;
    m_streamFailed = true;
    m_chart->finishAssistant();
    m_chart->setBusy(false);
    m_chart->appendFailure(error, retryable, message, display, attachments);
    if (m_chatReply)
        m_chatReply->abort();
    refreshSessions(false);
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
        if (m_live) {
            if (!m_sessionId.isEmpty())
                loadSession(m_sessionId);
        } else {
            loadHistory();
        }
        return true;
    }
    if (name == QLatin1String("/clear")) {
        if (m_live && !m_sessionId.isEmpty()) {
            requestJson(
                QStringLiteral("POST"),
                QStringLiteral("/sessions/") + encodedId(m_sessionId)
                    + QStringLiteral("/clear"),
                QJsonObject(),
                [this](const QJsonObject &object, int status) {
                    if (status < 200 || status >= 300) {
                        m_chart->showToast(
                            QStringLiteral("清空失败：%1")
                                .arg(object.value(QStringLiteral("error")).toString()));
                        return;
                    }
                    m_chart->clearMessages();
                    m_chart->setPhasePlan(QVariant());
                    refreshSessions(false);
                });
        } else {
            m_chart->clearMessages();
        }
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

    if (m_live) {
        if (name == QLatin1String("/fail") || name == QLatin1String("/approve")
            || name == QLatin1String("/workflow") || name == QLatin1String("/options")
            || name == QLatin1String("/params")) {
            m_chart->showToast(
                QStringLiteral("live 模式不执行假数据指令 %1").arg(name));
        } else {
            m_chart->showToast(QStringLiteral("未知指令：%1").arg(name));
        }
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

// 轨迹视图示例数据：与后端 /sessions/{id}/trajectory 的 events 同构
void DemoHost::loadTrajectory()
{
    const QString t1 = QStringLiteral("2026-09-06T09:04:00");
    const QString t2 = QStringLiteral("2026-09-06T09:04:06");
    const QString t3 = QStringLiteral("2026-09-06T09:04:21");
    const QString t4 = QStringLiteral("2026-09-06T09:04:38");

    QVariantList events;
    events << obj({ { "type", "user" }, { "turn", 1 }, { "ts", t1 },
                    { "content", "城南线 10kV 馈线跳闸，生成处置方案" },
                    { "display_content", "城南线 10kV 馈线跳闸，生成处置方案" } })
           << obj({ { "type", "traj_system_prompt" }, { "turn", 1 }, { "ts", t1 },
                    { "content", "你是电网调度 Agent，按台账与潮流校核结果给出处置建议。" } })
           << obj({ { "type", "traj_context" }, { "turn", 1 }, { "request", 1 },
                    { "kind", "ledger_snapshot" }, { "ts", t1 },
                    { "content", "台账：城南线（lk07）联络开关可用；#3 主变负载 78%。" } })
           << obj({ { "type", "traj_request_start" }, { "turn", 1 }, { "request", 1 },
                    { "model", "gridstar/gs-pro-32k" }, { "start", t1 }, { "ts", t1 } })
           << obj({ { "type", "tool_call" }, { "id", "call_91" }, { "name", "scada_snapshot" },
                    { "args", obj({ { "feeder", "城南线" }, { "ts", t2 } }) },
                    { "turn", 1 }, { "request", 1 }, { "step", 1 }, { "ts", t2 } })
           << obj({ { "type", "tool_result" }, { "call_id", "call_91" }, { "name", "scada_snapshot" },
                    { "result", "{\"breaker\":\"open\",\"I_A\":1820,\"U_pu\":0.94,\"lk07\":\"available\"}" },
                    { "duration_ms", 420 }, { "turn", 1 }, { "request", 1 }, { "step", 1 }, { "ts", t2 } })
           << obj({ { "type", "tool_call" }, { "id", "call_92" }, { "name", "power_flow" },
                    { "args", obj({ { "case", "城南线转供" }, { "topology", "lk07_closed" } }) },
                    { "turn", 1 }, { "request", 1 }, { "step", 2 }, { "ts", t3 } })
           << obj({ { "type", "tool_result" }, { "call_id", "call_92" }, { "name", "power_flow" },
                    { "result", "Tool error: 转供后 #3 主变负载 107% 越限" },
                    { "duration_ms", 2680 }, { "turn", 1 }, { "request", 1 }, { "step", 2 }, { "ts", t3 } })
           << obj({ { "type", "traj_request_end" }, { "turn", 1 }, { "request", 1 },
                    { "model", "gridstar/gs-pro-32k" }, { "status", "completed" }, { "ts", t3 },
                    { "content", "城南线跳闸后建议先合上 lk07 由邻线转供，但需先降 #3 主变负载。" },
                    { "reasoning", "先取断面确认开关位置，再做转供潮流校核；越限则先降载。" },
                    { "tool_calls", QVariant(QVariantList{
                          obj({ { "id", "call_91" }, { "name", "scada_snapshot" },
                                { "args", obj({ { "feeder", "城南线" } }) } }),
                          obj({ { "id", "call_92" }, { "name", "power_flow" },
                                { "args", obj({ { "case", "城南线转供" } }) } }) }) },
                    { "timing", obj({ { "start", t1 }, { "total_ms", 21400 },
                                      { "ttft_ms", 1300 }, { "gen_ms", 20100 },
                                      { "tok_per_s", 22.4 } }) },
                    { "usage", obj({ { "input", 3120 }, { "output", 456 }, { "total", 3576 } }) } })
           << obj({ { "type", "user" }, { "turn", 2 }, { "ts", t4 },
                    { "content", "改按甩负荷方案处理" },
                    { "display_content", "改按甩负荷方案处理" } })
           << obj({ { "type", "traj_request_start" }, { "turn", 2 }, { "request", 1 },
                    { "model", "gridstar/gs-pro-32k" }, { "start", t4 }, { "ts", t4 } })
           << obj({ { "type", "traj_request_end" }, { "turn", 2 }, { "request", 1 },
                    { "model", "gridstar/gs-pro-32k" }, { "status", "completed" },
                    { "ts", QStringLiteral("2026-09-06T09:04:52") },
                    { "content", "改为按甩负荷方案：先切 #3 主变可中断负荷 12MW，再合 lk07。" },
                    { "reasoning", "甩负荷后重算潮流，越限消除。" },
                    { "timing", obj({ { "start", t4 }, { "total_ms", 14200 },
                                      { "ttft_ms", 980 }, { "gen_ms", 13220 },
                                      { "tok_per_s", 26.1 } }) },
                    { "usage", obj({ { "input", 3980 }, { "output", 288 }, { "total", 4268 } }) } });

    m_chart->setTrajectoryEvents(events);
}

int main(int argc, char *argv[])
{
    QString shotPath;
    bool emptyShot = false;
    bool live = false;
    QString apiBase = QStringLiteral("http://127.0.0.1:1231");
    QString theme;
    QString tab;
    QStringList rest;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--shot") && i + 1 < argc) {
            shotPath = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == QLatin1String("--live")) {
            live = true;
        } else if (arg == QLatin1String("--api") && i + 1 < argc) {
            apiBase = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == QLatin1String("--theme") && i + 1 < argc) {
            theme = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == QLatin1String("--tab") && i + 1 < argc) {
            tab = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == QLatin1String("--empty")) {
            emptyShot = true;
        } else {
            rest << arg;
        }
    }
    // 截图模式默认用 offscreen 平台插件；外部显式指定平台时沿用（Windows 平台才能渲染文字）
    if (!shotPath.isEmpty() && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("QtChartWidgetDemo"));

    auto *chart = new ChartWidget;
    chart->setWindowTitle(QStringLiteral("GridStar AI · QtChartWidget"));
    chart->resize(460, 820);
    if (!theme.isEmpty())
        chart->setTheme(theme); // dark / silver / blue

    // 宿主对象与界面同生命周期，进程退出即释放
    auto *host = new DemoHost(chart, live, apiBase);
    if (live) {
        chart->setConnectionState(QStringLiteral("checking"), QStringLiteral("连接后台…"));
        chart->setConfigLoaded(false);
        chart->setMode(QStringLiteral("auto"));
        chart->setVoiceEnabled(true);
    } else {
        host->populate();
        if (!emptyShot)
            host->loadHistory();
    }
    // 初始数据推完之后才接信号：否则 populate 的 setter（如 setMode）会触发处理器里的
    // showToast，启动即弹一条提示压住阶段计划面板，干扰离屏对照图。
    // 必须早于下面的 Tab 块：openSettings("usage") 会同步发 usageStatsRequested。
    host->wire();
    if (tab == QLatin1String("traj")) {
        if (!live)
            host->loadTrajectory();
        chart->setViewTab(QStringLiteral("traj"));
    } else if (tab == QLatin1String("usage")) {
        // 打开设置中心的「用量」页（与 webui 的第四个 Tab 对照）
        chart->openSettings(QStringLiteral("usage"));
    } else if (tab == QLatin1String("models") || tab == QLatin1String("skills")
               || tab == QLatin1String("mcp")) {
        // 设置中心其余三个 Tab（与 webui 的 tab-models / tab-skills / tab-mcp 对照）
        chart->openSettings(tab);
    } else if (tab == QLatin1String("rail")) {
        // 离屏无法悬停：等价地派发 Enter 展开轮次列表面板，便于出对照图
        QTimer::singleShot(200, chart, [chart] {
            if (QWidget *rail = chart->findChild<QWidget *>(QStringLiteral("turnRail"))) {
                QEvent enter(QEvent::Enter);
                QApplication::sendEvent(rail, &enter);
            }
        });
    } else if (tab == QLatin1String("theme")) {
        // 皮肤下拉（既有浮层）也能出对照图，用来核对浮层是否继承主题 QSS
        QTimer::singleShot(200, chart, [chart] {
            if (QWidget *trigger = chart->findChild<QWidget *>(QStringLiteral("themeTrigger"))) {
                const QPoint center(trigger->width() / 2, trigger->height() / 2);
                QMouseEvent press(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(trigger, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QApplication::sendEvent(trigger, &release);
            }
        });
    } else if (tab == QLatin1String("usage-cal")) {
        // 用量页 + 日期浮层：离屏没有真实点击，等价地给日期触发器派发一次点击
        chart->openSettings(QStringLiteral("usage"));
        QTimer::singleShot(500, chart, [chart] {
            if (QWidget *range = chart->findChild<QWidget *>(QStringLiteral("usageRange"))) {
                const QPoint center(range->width() / 2, range->height() / 2);
                QMouseEvent press(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(range, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QApplication::sendEvent(range, &release);
            }
        });
    } else if (tab == QLatin1String("usage-list")) {
        // 用量页 + 下拉：核对 .usage-listbox 是否相对 .usage-filter 容器（含标签）左对齐
        chart->openSettings(QStringLiteral("usage"));
        QTimer::singleShot(500, chart, [chart] {
            if (QWidget *select = chart->findChild<QWidget *>(QStringLiteral("usageModel"))) {
                const QPoint center(select->width() / 2, select->height() / 2);
                QMouseEvent press(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(select, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QApplication::sendEvent(select, &release);
            }
        });
    }

    chart->show();
    chart->focusInput();
    if (live)
        QTimer::singleShot(0, chart, [host] { host->bootstrap(); });

    if (!shotPath.isEmpty()) {
        QTimer::singleShot(700, &app, [chart, shotPath, tab, theme] {
            if (!theme.isEmpty())
                chart->setTheme(theme); // 显示之后再套一次，确保离屏截图用的是目标皮肤
            // 用量页在设置对话框里，截图目标跟着切换
            QWidget *target = chart;
            if (tab == QLatin1String("usage") || tab == QLatin1String("usage-cal")
                || tab == QLatin1String("usage-list")) {
                if (QWidget *dialog =
                        chart->findChild<QWidget *>(QStringLiteral("settingsDialog"))) {
                    // 面板比对话框高，截全图时先拉高，把三图与明细表一起拍进来
                    dialog->resize(dialog->width(), 1240);
                    target = dialog;
                }
            } else if (tab == QLatin1String("models") || tab == QLatin1String("skills")
                       || tab == QLatin1String("mcp")) {
                if (QWidget *dialog =
                        chart->findChild<QWidget *>(QStringLiteral("settingsDialog")))
                    target = dialog;
            }
            if (tab == QLatin1String("usage-cal")) {
                // 日期浮层是独立控件，单独抓它自己
                if (QWidget *popup =
                        chart->findChild<QWidget *>(QStringLiteral("usageRangePopup")))
                    target = popup;
            }
            if (tab == QLatin1String("usage-list")) {
                // 用量下拉是独立 Qt::Popup 窗口，单独抓它自己
                if (QWidget *popup =
                        chart->findChild<QWidget *>(QStringLiteral("usageListPopup")))
                    target = popup;
            }
            if (tab == QLatin1String("theme")) {
                // 皮肤下拉同理
                if (QWidget *popup =
                        chart->findChild<QWidget *>(QStringLiteral("themeListbox")))
                    target = popup;
            }
            const QPixmap shot = target->grab();
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

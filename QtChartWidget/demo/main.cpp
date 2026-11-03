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

#include <QAbstractButton>
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
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <QtMath>

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

// 模型设置页要用的能力位：界面只摆四项（视觉 / 视频 / 音频 / 推理）
QVariant demoCapabilities(bool vision, bool video, bool audio, bool reasoning)
{
    return QVariant(obj({ { "vision", vision }, { "video", video },
                          { "audio", audio }, { "reasoning", reasoning } }));
}

QVariantList demoModels()
{
    return { obj({ { "provider", "gridstar" },
                   { "provider_name", "GridStar 网关" },
                   { "id", "gs-pro-32k" },
                   { "name", "GS-Pro 32K" },
                   { "enabled", true },
                   { "provider_enabled", true },
                   { "context_window", 32768 },
                   { "max_output_tokens", 8192 },
                   { "capabilities", demoCapabilities(true, false, false, true) } }),
             obj({ { "provider", "gridstar" },
                   { "id", "gs-lite-8k" },
                   { "name", "GS-Lite 8K" },
                   { "enabled", true },
                   { "provider_enabled", true },
                   { "context_window", 8192 },
                   { "max_output_tokens", 2048 },
                   { "capabilities", demoCapabilities(false, false, false, false) } }),
             obj({ { "provider", "deepseek" },
                   { "id", "deepseek-chat" },
                   { "name", "DeepSeek Chat" },
                   { "enabled", true },
                   { "provider_enabled", true },
                   { "context_window", 65536 },
                   { "max_output_tokens", 8192 },
                   { "capabilities", demoCapabilities(false, false, false, false) } }),
             obj({ { "provider", "deepseek" },
                   { "id", "deepseek-reasoner" },
                   { "name", "DeepSeek Reasoner（停用）" },
                   { "enabled", false },
                   { "provider_enabled", true },
                   { "context_window", 65536 },
                   { "max_output_tokens", 8192 },
                   { "capabilities", demoCapabilities(false, false, false, true) } }) };
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
// ---------------------------------------------------------------- 记忆 / 知识库演示数据
// 只用于演示宿主：形状与后端 /memory/* 、/kb/* 的响应同构，库本身不做任何校验

QVariantMap demoMemorySummary()
{
    QVariantMap maintenance;
    maintenance.insert(QStringLiteral("state"), QStringLiteral("idle"));
    maintenance.insert(QStringLiteral("finished_at"), QStringLiteral("2026-11-02T08:12:00Z"));
    QVariantMap last;
    last.insert(QStringLiteral("merged"), 2);
    last.insert(QStringLiteral("flagged"), 1);
    last.insert(QStringLiteral("archived"), 3);
    last.insert(QStringLiteral("submitted"), 4);
    last.insert(QStringLiteral("candidates"), 4);
    last.insert(QStringLiteral("facts"), 2);
    maintenance.insert(QStringLiteral("last_result"), last);

    QVariantMap store;
    store.insert(QStringLiteral("status"), QStringLiteral("healthy"));
    store.insert(QStringLiteral("fts"), true);

    QVariantMap counts;
    counts.insert(QStringLiteral("candidate"), 4);
    counts.insert(QStringLiteral("approved"), 6);
    counts.insert(QStringLiteral("rejected"), 1);
    counts.insert(QStringLiteral("deprecated"), 1);

    QVariantMap config;
    config.insert(QStringLiteral("experience_extract"), true);
    config.insert(QStringLiteral("experience_inject"), false);

    QVariantMap summary;
    summary.insert(QStringLiteral("enabled"), true);
    summary.insert(QStringLiteral("store"), store);
    summary.insert(QStringLiteral("experience"), counts);
    summary.insert(QStringLiteral("config"), config);
    summary.insert(QStringLiteral("maintenance"), maintenance);
    summary.insert(QStringLiteral("notice"), QStringLiteral("当前为影子期：只采集不注入。"));
    return summary;
}

QVariantMap demoMemoryItem(const QString &id, const QString &status, const QString &kind,
                           const QString &scope, const QString &title, double confidence,
                           int evidence, const QString &when)
{
    QVariantMap item;
    item.insert(QStringLiteral("id"), id);
    item.insert(QStringLiteral("status"), status);
    item.insert(QStringLiteral("kind"), kind);
    item.insert(QStringLiteral("scope"), scope);
    item.insert(QStringLiteral("task_type"), QStringLiteral("modeling"));
    item.insert(QStringLiteral("title"), title);
    item.insert(QStringLiteral("confidence"), confidence);
    item.insert(QStringLiteral("evidence_count"), evidence);
    item.insert(QStringLiteral("created_at"), when);
    item.insert(QStringLiteral("updated_at"), when);
    return item;
}

QVariantMap demoMemoryExperiences()
{
    QVariantList items;
    items << demoMemoryItem(QStringLiteral("m-1001"), QStringLiteral("candidate"),
                            QStringLiteral("experience"), QStringLiteral("aircraft"),
                            QStringLiteral("附面层首层高度取 y+ 不超过 1e-5 时收敛更稳"),
                            0.86, 3, QStringLiteral("2026-11-02T07:40:00Z"));
    items << demoMemoryItem(QStringLiteral("m-1002"), QStringLiteral("candidate"),
                            QStringLiteral("fact"), QStringLiteral("common"),
                            QStringLiteral("本机 GridStar 求解器版本为 4.2.1"),
                            0.94, 5, QStringLiteral("2026-11-01T15:02:00Z"));
    items << demoMemoryItem(QStringLiteral("m-1003"), QStringLiteral("approved"),
                            QStringLiteral("experience"), QStringLiteral("missile"),
                            QStringLiteral("网格量超过 800 万后先粗算再加密，避免一次提交超时"),
                            0.91, 8, QStringLiteral("2026-10-30T09:20:00Z"));
    items << demoMemoryItem(QStringLiteral("m-1004"), QStringLiteral("rejected"),
                            QStringLiteral("experience"), QStringLiteral("aircraft"),
                            QStringLiteral("把边界层厚度直接写成 0.1（证据不足）"),
                            0.31, 1, QStringLiteral("2026-10-29T11:05:00Z"));

    QVariantMap listing;
    listing.insert(QStringLiteral("items"), items);
    listing.insert(QStringLiteral("total"), 12);
    listing.insert(QStringLiteral("page"), 1);
    listing.insert(QStringLiteral("page_size"), 20);
    QVariantMap facets;
    facets.insert(QStringLiteral("scopes"), QVariantList{ QStringLiteral("aircraft"),
                                                          QStringLiteral("missile"),
                                                          QStringLiteral("common") });
    facets.insert(QStringLiteral("task_types"), QVariantList{ QStringLiteral("modeling") });
    listing.insert(QStringLiteral("facets"), facets);
    return listing;
}

QVariantMap demoMemoryTools()
{
    QVariantMap fields;
    fields.insert(QStringLiteral("max_iterations"), 500);
    fields.insert(QStringLiteral("tolerance"), 1e-5);
    fields.insert(QStringLiteral("report_format"), QStringLiteral("pdf"));
    fields.insert(QStringLiteral("verbose"), false);
    QVariantMap tool;
    tool.insert(QStringLiteral("tool"), QStringLiteral("run_cfd_solver"));
    tool.insert(QStringLiteral("fields"), fields);
    tool.insert(QStringLiteral("updated_at"), QStringLiteral("2026-11-02T06:31:00Z"));

    QVariantMap fields2;
    fields2.insert(QStringLiteral("unit"), QStringLiteral("m/s"));
    fields2.insert(QStringLiteral("altitude"), 10000);
    QVariantMap tool2;
    tool2.insert(QStringLiteral("tool"), QStringLiteral("set_flight_condition"));
    tool2.insert(QStringLiteral("fields"), fields2);
    tool2.insert(QStringLiteral("updated_at"), QStringLiteral("2026-11-01T19:12:00Z"));

    QVariantMap payload;
    payload.insert(QStringLiteral("tools"), QVariantList{ tool, tool2 });
    payload.insert(QStringLiteral("tool_count"), 2);
    return payload;
}

QVariantList demoCategories()
{
    QVariantList list;
    const struct { const char *id; const char *name; } rows[] = {
        { "aircraft", "Aircraft" }, { "missile", "Missile" }, { "common", "Common" }
    };
    for (const auto &row : rows) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), QString::fromLatin1(row.id));
        item.insert(QStringLiteral("name"), QString::fromLatin1(row.name));
        list << item;
    }
    return list;
}

QVariantList demoKnowledgeBases()
{
    QVariantList list;
    const struct { const char *id; const char *name; int docs; int chunks; } rows[] = {
        { "kb-aero", "气动手册", 42, 1860 },
        { "kb-struct", "结构强度规范", 18, 740 },
        { "kb-general", "通用工程知识", 96, 4120 }
    };
    for (const auto &row : rows) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), QString::fromLatin1(row.id));
        item.insert(QStringLiteral("name"), QString::fromUtf8(row.name));
        item.insert(QStringLiteral("documents"), row.docs);
        item.insert(QStringLiteral("chunks"), row.chunks);
        list << item;
    }
    return list;
}

QVariantMap demoContextUsage()
{
    QVariantMap usage;
    usage.insert(QStringLiteral("max"), 32768);
    usage.insert(QStringLiteral("used"), 12480);
    usage.insert(QStringLiteral("left"), 20288);
    usage.insert(QStringLiteral("percent"), 38);
    usage.insert(QStringLiteral("estimated"), true);
    usage.insert(QStringLiteral("model"), QStringLiteral("gs-pro-32k"));
    return usage;
}

QVariantMap demoKbStatus()
{
    QVariantMap status;
    status.insert(QStringLiteral("available"), true);
    status.insert(QStringLiteral("documents"), 42);
    status.insert(QStringLiteral("chunks"), 1860);
    status.insert(QStringLiteral("params"), 318);
    status.insert(QStringLiteral("db_bytes"), 22 * 1024 * 1024LL);
    return status;
}

QVariantList demoKbFolders()
{
    QVariantList folders;
    const struct { const char *id; const char *name; const char *parent; int depth; } rows[] = {
        { "fd-manual", "型号手册", "", 1 },
        { "fd-flight", "飞行性能", "fd-manual", 2 },
        { "fd-struct", "结构强度", "", 1 }
    };
    for (const auto &row : rows) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), QString::fromLatin1(row.id));
        item.insert(QStringLiteral("name"), QString::fromUtf8(row.name));
        item.insert(QStringLiteral("parent_id"), QString::fromLatin1(row.parent));
        item.insert(QStringLiteral("depth"), row.depth);
        folders << item;
    }
    return folders;
}

QVariantMap demoKbTree()
{
    QVariantMap flight;
    flight.insert(QStringLiteral("id"), QStringLiteral("fd-flight"));
    flight.insert(QStringLiteral("name"), QStringLiteral("飞行性能"));
    flight.insert(QStringLiteral("depth"), 2);
    flight.insert(QStringLiteral("total"), 8);
    flight.insert(QStringLiteral("children"), QVariantList());

    QVariantMap manual;
    manual.insert(QStringLiteral("id"), QStringLiteral("fd-manual"));
    manual.insert(QStringLiteral("name"), QStringLiteral("型号手册"));
    manual.insert(QStringLiteral("depth"), 1);
    manual.insert(QStringLiteral("total"), 12);
    manual.insert(QStringLiteral("children"), QVariantList{ flight });

    QVariantMap structure;
    structure.insert(QStringLiteral("id"), QStringLiteral("fd-struct"));
    structure.insert(QStringLiteral("name"), QStringLiteral("结构强度"));
    structure.insert(QStringLiteral("depth"), 1);
    structure.insert(QStringLiteral("total"), 6);
    structure.insert(QStringLiteral("children"), QVariantList());

    QVariantMap tree;
    tree.insert(QStringLiteral("documents"), 3);
    tree.insert(QStringLiteral("children"), QVariantList{ manual, structure });
    return tree;
}

QVariantList demoKbDocuments()
{
    QVariantList docs;
    const struct { const char *id; const char *name; const char *path; const char *folder;
                   int pages; int chunks; int params; qint64 size; const char *when; } rows[] = {
        { "d-01", "A320 飞行性能手册.pdf", "D:\\资料\\A320\\性能.pdf", "fd-flight",
          486, 1240, 96, 18 * 1024 * 1024LL, "2026-11-02 09:14" },
        { "d-02", "附面层计算规范.docx", "D:\\资料\\规范\\附面层.docx", "fd-manual",
          64, 210, 24, 3 * 1024 * 1024LL, "2026-11-01 16:40" },
        { "d-03", "结构强度校核条目.xlsx", "D:\\资料\\结构\\校核.xlsx", "fd-struct",
          12, 88, 41, 820 * 1024LL, "2026-10-31 11:02" },
        { "d-04", "旧版扫描件.pdf", "D:\\资料\\旧版.pdf", "",
          0, 0, 0, 0, "2026-10-30 08:00" }
    };
    for (const auto &row : rows) {
        QVariantMap doc;
        doc.insert(QStringLiteral("doc_id"), QString::fromLatin1(row.id));
        doc.insert(QStringLiteral("name"), QString::fromUtf8(row.name));
        doc.insert(QStringLiteral("path"), QString::fromUtf8(row.path));
        doc.insert(QStringLiteral("folder_id"), QString::fromLatin1(row.folder));
        doc.insert(QStringLiteral("pages"), row.pages);
        doc.insert(QStringLiteral("chunks"), row.chunks);
        doc.insert(QStringLiteral("params"), row.params);
        doc.insert(QStringLiteral("size"), row.size);
        doc.insert(QStringLiteral("indexed_at"), QString::fromLatin1(row.when));
        const bool failed = row.id == QLatin1String("d-04");
        doc.insert(QStringLiteral("status"), failed ? QStringLiteral("failed")
                                                     : QStringLiteral("ok"));
        if (failed)
            doc.insert(QStringLiteral("error"), QStringLiteral("扫描件无文本层，OCR 未识别到内容"));
        docs << doc;
    }
    return docs;
}

QVariantMap demoKbSearch()
{
    QVariantMap param;
    param.insert(QStringLiteral("name"), QStringLiteral("boundary_layer_height"));
    param.insert(QStringLiteral("domain"), QStringLiteral("aircraft"));
    param.insert(QStringLiteral("default"), QStringLiteral("1e-5"));
    param.insert(QStringLiteral("min"), QStringLiteral("1e-6"));
    param.insert(QStringLiteral("max"), QStringLiteral("1e-4"));
    param.insert(QStringLiteral("description"),
                 QStringLiteral("附面层首层网格高度，影响 y+ 与摩阻计算精度。"));
    param.insert(QStringLiteral("doc"), QStringLiteral("附面层计算规范.docx"));
    param.insert(QStringLiteral("heading_path"), QStringLiteral("第 3 章 > 3.2 首层高度"));
    param.insert(QStringLiteral("page"), 12);

    QVariantMap hit;
    hit.insert(QStringLiteral("title"), QStringLiteral("附面层首层高度确定方法"));
    hit.insert(QStringLiteral("doc"), QStringLiteral("附面层计算规范.docx"));
    hit.insert(QStringLiteral("page"), 12);
    hit.insert(QStringLiteral("score"), 0.82);
    hit.insert(QStringLiteral("heading_path"), QStringLiteral("第 3 章 / 3.2 首层高度"));
    hit.insert(QStringLiteral("chunk_id"), QStringLiteral("c-3128"));
    hit.insert(QStringLiteral("snippet"),
               QStringLiteral("首层高度应使 y+ 落在 1~5 区间，推荐取 1e-5 m……"));

    QVariantMap result;
    result.insert(QStringLiteral("params"), QVariantList{ param });
    result.insert(QStringLiteral("hits"), QVariantList{ hit });
    return result;
}

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

// 后端错误体优先取 error / detail，取不到退回宿主给的话术；带 HTTP 码便于排障
QString errorText(const QJsonObject &object, const QString &fallback, int status = 0)
{
    QString message = object.value(QStringLiteral("error")).toString();
    if (message.isEmpty())
        message = object.value(QStringLiteral("detail")).toString();
    if (message.isEmpty())
        message = fallback;
    return status > 0 ? QStringLiteral("%1（HTTP %2）").arg(message).arg(status) : message;
}

// 专业分类（webui #category-select）不是接口给的表：webui 把 skills 的 category 去重后
// 拼成下拉，首项「全部」由库自己补（id 为空串）。这里照同一口径从 /skills 响应派生。
QVariantList categoriesFromSkills(const QVariantList &skills)
{
    QStringList seen;
    QVariantList out;
    for (const QVariant &value : skills) {
        QString id = value.toMap().value(QStringLiteral("category")).toString().trimmed().toLower();
        if (id.isEmpty() || seen.contains(id))
            continue;
        seen << id;
        QString name = id;
        name[0] = name.at(0).toUpper();
        out << obj({ { "id", id }, { "name", name } });
    }
    return out;
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
    // 只给 --tab approve 的离屏截图用：弹一张审批卡
    void approveForShot();
    // 轨迹视图示例数据（宿主从 /sessions/{id}/trajectory 拉到的 events）
    void loadTrajectory();
    // 初始数据推完之后再接信号：否则 populate 的 setter 会触发下面这些处理器里
    // 的 showToast，启动即弹一条提示压住阶段计划面板，干扰离屏对照图
    void wire();
    // live 模式启动后异步拉取服务状态与首屏数据；fake 模式不调用。
    void bootstrap();
    // 打开设置中心：宿主自己拉一次模型信息快照再交给库（「打开」这个动作库不发信号）
    void openSettings(const QString &tab);

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
                       const QVariantList &attachments, bool appendUser = true);
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

    // ---- 记忆页（/memory/*，只在 m_live 下联网）----
    void loadMemorySummary();
    void loadMemoryTools();
    void loadMemoryList(int page, const QVariantMap &filters);
    void memoryPatchConfig(const QVariantMap &updates);
    void memoryTransition(const QString &id, const QString &action);
    void memoryBulk(const QStringList &ids, const QString &action);
    void memorySaveField(const QString &tool, const QString &field, const QVariant &value);
    void memoryDeleteField(const QString &tool, const QString &field);
    void memoryClearTool(const QString &tool);
    void memoryClearAllTools();
    void memoryMaintenance(const QString &kind);
    void memoryShowDetail(const QString &id);

    // ---- 知识库页（/kb/*）+ 输入区知识库下拉 ----
    void loadKnowledgeBases();
    void kbRefresh(const QString &libraryId);
    // 把新拿到的库列表推给面板，并按 wanted 定下当前库，随后统一重拉 status/folders/documents
    void kbAdoptLibraries(const QVariantList &libraries, const QString &wanted);
    void kbLoadPanel(const QString &libraryId, const QString &folderKey);
    void kbImportFiles(const QStringList &paths, const QString &libraryId,
                       const QString &folderId);
    void kbImportDir(const QString &libraryId, const QString &dirPath,
                     const QString &folderId);

    // ---- 模型信息快照 / 顶栏 / 收尾 ----
    void loadModelMetadata();
    void refreshModelMetadata();
    void resolveModelLimits(const QString &providerId, const QString &modelId);
    void launchGridStar();
    void endTurn(const QString &label);

    // ---- 上下文用量圆环（webui renderContextRing / restoreContextRing）----
    void applyContextUsage(const QVariantMap &event);
    void restoreContextRing(const QVariantList &messages);
    // 统一的回填入口：window <= 0 且 used <= 0 时清空圆环
    void pushContextRing(double used, qint64 window, bool estimated);
    qint64 contextWindowFor(const QString &modelKey) const;

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

    // 输入区两件状态：宿主侧镜像（webui state.selectedCategory / kbState.selected）。
    // 分类与知识库都没有独立的 meta 写接口，靠 /chat/stream 的 body 生效
    QString m_category;
    bool m_categoryTouched = false;
    QString m_kbLibrary = QStringLiteral("*");

    // 设置页知识库面板：当前库 + 当前目录（all / root / 真实 id）
    QString m_kbPanelLibrary;
    QString m_kbFolder = QStringLiteral("all");
    // 正在把库列表推给面板：面板回发的 libraryChanged / folderChanged 是回声，不重复拉取
    bool m_kbAdopting = false;

    // 记忆列表的最近一次结果与筛选（详情从这里取，不另开接口）
    QVariantList m_memoryItems;
    QVariantMap m_memoryFilters;
    int m_memoryPage = 1;
    bool m_memorySummaryBusy = false;
    bool m_memoryToolsBusy = false;

    // 模型信息快照：resolve 只补 limits 键，不能把状态行覆盖没了
    QVariantMap m_modelMeta;
    // 圆环的当前读数：窗口分母随模型切换 / 模型目录晚到都要重算，所以留一份最近值
    double m_contextUsed = 0.0;
    bool m_contextEstimated = false;
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

    // 输入区新件（webui #category-select / #kb-select / #context-ring）
    m_chart->setCategories(demoCategories());
    m_chart->setCurrentCategory(QStringLiteral("aircraft"));
    m_chart->setKnowledgeBases(demoKnowledgeBases());
    m_chart->setCurrentKnowledgeBase(QStringLiteral("*"));
    m_chart->setContextUsage(demoContextUsage());

    // 设置中心两个新页
    m_chart->setMemorySummary(demoMemorySummary());
    m_chart->setMemoryExperiences(demoMemoryExperiences());
    m_chart->setMemoryTools(demoMemoryTools());
    // 详情浮层由宿主弹：假数据路径也把这份列表留住，点「查看详情」才有东西可摊
    m_memoryItems = demoMemoryExperiences()
                        .value(QStringLiteral("items"))
                        .toList();
    m_chart->setKbLibraries(demoKnowledgeBases());
    m_chart->setKbStatus(demoKbStatus());
    m_chart->setKbFolders(demoKbFolders(), demoKbTree());
    m_chart->setKbDocuments(demoKbDocuments(), 42);
    m_chart->setKbSearchResult(demoKbSearch());

    // 模型信息快照（webui 模型页的状态行）：只给状态，不填上限
    QVariantMap snapshot;
    snapshot.insert(QStringLiteral("source"), QStringLiteral("内置快照 + 供应商目录"));
    snapshot.insert(QStringLiteral("fetched_at"), QStringLiteral("2026-11-02T12:00:00Z"));
    snapshot.insert(QStringLiteral("models"), 4);
    snapshot.insert(QStringLiteral("providers"), 2);
    snapshot.insert(QStringLiteral("hosts"), 2);
    snapshot.insert(QStringLiteral("available"), true);
    m_chart->setModelMetadata(snapshot);
}

void DemoHost::loadHistory()
{
    m_chart->setHistory(demoHistory());
    m_chart->setCurrentSessionTitle(QStringLiteral("10kV 城南线故障研判与转供电方案"));
}

void DemoHost::approveForShot()
{
    // 比常规审批卡多两个参数，用来压测浮层高度（参数多时应该在可视区内滚，不能顶出窗口）
    const QVariantMap schema = obj({ { "type", "object" },
                                     { "properties",
                                       obj({ { "device",
                                               obj({ { "type", "string" },
                                                     { "description", "开关编号" } }) },
                                             { "action",
                                               obj({ { "type", "string" },
                                                     { "description", "open / close" } }) },
                                             { "delay_s",
                                               obj({ { "type", "integer" },
                                                     { "description", "延时秒数" } }) },
                                             { "note",
                                               obj({ { "type", "string" },
                                                     { "description",
                                                       "操作备注：这一行故意写长一点，测试参数行换行与卡片整体高度" } }) },
                                             { "confirm_text",
                                               obj({ { "type", "string" },
                                                     { "description", "二次确认文案" } }) } }) },
                                     { "required",
                                       QVariant(QVariantList{ QStringLiteral("device"),
                                                              QStringLiteral("action") }) } });
    const QVariantMap event = obj({ { "name", "switch_order" },
                                    { "call_id", "call_shot_1" },
                                    { "args",
                                      obj({ { "device", "LK-07" },
                                            { "action", "close" },
                                            { "delay_s", 5 },
                                            { "note", "" },
                                            { "confirm_text", "" } }) },
                                    { "schema", schema } });
    m_chart->appendApproval(event);
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
    // 输入区知识库下拉与设置页共用一份库列表；模型信息快照先拉一次，等用户打开设置就已有状态行
    loadKnowledgeBases();
    loadModelMetadata();
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
            // 模型目录晚于会话详情到达时圆环可能只拿到了 used：这里用真实 context_window 补算一次
            if (m_contextUsed > 0.0)
                pushContextRing(m_contextUsed, contextWindowFor(m_chart->currentModel()),
                                m_contextEstimated);
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
            // 专业分类下拉从技能目录派生（与 webui 一致），别让 live 模式一直空着
            m_chart->setCategories(categoriesFromSkills(m_liveSkills));
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
            // 新会话没有 meta：分类回「全部」、知识库回「全部知识库」、上下文圆环清掉
            m_category.clear();
            m_categoryTouched = false;
            m_chart->syncCategoryFromSession(QString());
            m_kbLibrary = QStringLiteral("*");
            m_chart->setCurrentKnowledgeBase(m_kbLibrary);
            m_chart->clearContextUsage();
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

            // 切会话必须回填的三样（webui loadSession）：
            // 1) meta.category —— 服务端权威值，走 syncCategoryFromSession（会无条件覆盖）；
            //    这不是用户的选择，所以把 categoryTouched 清掉
            m_category = meta.value(QStringLiteral("category")).toString();
            m_categoryTouched = false;
            m_chart->syncCategoryFromSession(m_category);
            // 2) meta.kb_library —— 后端用空串表示「不使用」、"*" 表示全部，
            //    输入区下拉里「不使用」的 id 是 __none__，两边做一次映射
            const QJsonValue kbValue = meta.value(QStringLiteral("kb_library"));
            const QString kbRaw = (kbValue.isNull() || kbValue.isUndefined())
                                      ? QStringLiteral("*")
                                      : kbValue.toString();
            m_kbLibrary = kbRaw.isEmpty() ? QStringLiteral("__none__") : kbRaw;
            m_chart->setCurrentKnowledgeBase(m_kbLibrary);

            const QVariantList messages = object.value(QStringLiteral("messages")).toArray().toVariantList();
            m_chart->setHistory(messages);
            // 3) 上下文圆环 —— 从历史里最近一条带 context_used 的消息恢复
            restoreContextRing(messages);
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
                // 重新拉一份配置与 revision，用户不必重启宿主就能再存一次
                loadConfig();
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

// ------------------------------------------------------------ 记忆页（/memory/*）
//
// 信号 → 请求的对应关系见 README「/memory/*、/kb/* 接口对照」。
// 回填全部发生在 QNetworkReply::finished 的 lambda 里（以 m_chart 为 context，
// 本来就投递在 GUI 线程），不会踩库的 assertGuiThread。

void DemoHost::loadMemorySummary()
{
    if (!m_live || m_memorySummaryBusy)
        return;
    m_memorySummaryBusy = true;
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/memory/summary"), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            m_memorySummaryBusy = false;
            if (status < 200 || status >= 300) {
                m_chart->setMemoryStatusText(
                    QStringLiteral("记忆状态读取失败：%1")
                        .arg(errorText(object, QStringLiteral("后台不可达"), status)));
                return;
            }
            m_chart->setMemorySummary(object.toVariantMap());
        });
}

void DemoHost::loadMemoryTools()
{
    if (!m_live || m_memoryToolsBusy)
        return;
    m_memoryToolsBusy = true;
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/memory/tool-parameters"), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            m_memoryToolsBusy = false;
            if (status < 200 || status >= 300) {
                m_chart->setMemoryStatusText(
                    QStringLiteral("工具参数记忆读取失败：%1")
                        .arg(errorText(object, QStringLiteral("后台不可达"), status)));
                return;
            }
            m_chart->setMemoryTools(object.toVariantMap());
        });
}

void DemoHost::loadMemoryList(int page, const QVariantMap &filters)
{
    if (!m_live)
        return;
    m_memoryPage = page < 1 ? 1 : page;
    m_memoryFilters = filters;
    // 筛选键与后端 /memory/experiences 的 query 同名；空值一律不下发
    // （状态「全部」的 value 是空串，传过去会被当成筛选条件）
    QUrlQuery query;
    const auto put = [&query, &filters](const char *key) {
        const QString value = filters.value(QString::fromLatin1(key)).toString();
        if (!value.isEmpty())
            query.addQueryItem(QString::fromLatin1(key), value);
    };
    put("status");
    put("kind");
    put("scope");
    put("q");
    query.addQueryItem(QStringLiteral("page"), QString::number(m_memoryPage));
    const int pageSize = filters.value(QStringLiteral("page_size")).toInt();
    query.addQueryItem(QStringLiteral("page_size"), QString::number(pageSize > 0 ? pageSize : 20));
    requestJson(
        QStringLiteral("GET"),
        QStringLiteral("/memory/experiences?") + query.query(QUrl::FullyEncoded), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->setMemoryListFailed(
                    errorText(object, QStringLiteral("记忆列表读取失败"), status));
                return;
            }
            m_memoryItems = object.value(QStringLiteral("items")).toArray().toVariantList();
            m_chart->setMemoryExperiences(object.toVariantMap());
        });
}

void DemoHost::memoryPatchConfig(const QVariantMap &updates)
{
    if (!m_live || updates.isEmpty())
        return;
    requestJson(
        QStringLiteral("PATCH"), QStringLiteral("/memory/config"),
        QJsonObject::fromVariantMap(updates),
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->showToast(
                    QStringLiteral("记忆设置保存失败：%1")
                        .arg(errorText(object, QStringLiteral("未知错误"), status)));
                loadMemorySummary();
                return;
            }
            m_chart->setMemorySummary(object.toVariantMap());
            m_chart->showToast(QStringLiteral("记忆设置已保存"));
            // 记忆开关与模型设置共用同一份 config.json / revision（webui memorySyncSettingsConfig）。
            // 这里只把宿主侧副本与 revision 更新，不 setSettingsDraft：那个入口会把设置对话框
            // 整份重载并切回「模型」页，用户正站在「记忆」页上会被踢走。
            requestJson(
                QStringLiteral("GET"), QStringLiteral("/config"), QJsonObject(),
                [this](const QJsonObject &config, int configStatus) {
                    if (configStatus < 200 || configStatus >= 300)
                        return;
                    const QJsonValue body = config.value(QStringLiteral("config"));
                    if (!body.isObject())
                        return;
                    m_config = body.toObject().toVariantMap();
                    m_configRevision = config.value(QStringLiteral("revision")).toVariant();
                });
        });
}

void DemoHost::memoryTransition(const QString &id, const QString &action)
{
    if (!m_live || id.isEmpty() || action.isEmpty())
        return;
    // 单条状态流转是 PATCH /memory/experiences/{id}，action 放 body（后端不接受 POST）
    QJsonObject body;
    body.insert(QStringLiteral("action"), action);
    requestJson(
        QStringLiteral("PATCH"),
        QStringLiteral("/memory/experiences/") + encodedId(id), body,
        [this, action](const QJsonObject &object, int status) {
            if (status == 409) {
                // 乐观锁：这条在别处已经变过状态了
                m_chart->showToast(QStringLiteral("该条状态已变化，已刷新"));
                loadMemoryList(1, m_memoryFilters);
                return;
            }
            if (status < 200 || status >= 300) {
                m_chart->showToast(errorText(object, QStringLiteral("操作失败"), status));
                return;
            }
            m_chart->showToast(QStringLiteral("已执行 %1").arg(action));
            loadMemoryList(1, m_memoryFilters); // webui memoryAct：回到第一页重拉
            loadMemorySummary();
        });
}

void DemoHost::memoryBulk(const QStringList &ids, const QString &action)
{
    if (!m_live || ids.isEmpty() || action.isEmpty())
        return;
    QJsonObject body;
    body.insert(QStringLiteral("ids"), QJsonArray::fromStringList(ids));
    body.insert(QStringLiteral("action"), action);
    requestJson(
        QStringLiteral("POST"), QStringLiteral("/memory/experiences/bulk"), body,
        [this, action](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->showToast(
                    errorText(object, QStringLiteral("批量操作失败"), status));
                return;
            }
            const int succeeded = object.value(QStringLiteral("succeeded")).toInt();
            const int failed = object.value(QStringLiteral("failed")).toInt();
            m_chart->showToast(failed > 0
                                   ? QStringLiteral("成功 %1 条，失败 %2 条")
                                         .arg(succeeded)
                                         .arg(failed)
                                   : QStringLiteral("已%1 %2 条").arg(action).arg(succeeded));
            loadMemoryList(1, m_memoryFilters);
            loadMemorySummary();
        });
}

void DemoHost::memorySaveField(const QString &tool, const QString &field, const QVariant &value)
{
    if (!m_live)
        return;
    // 只改值、不新增字段（后端口径）；成功后用 tool-parameters 整体回填
    QJsonObject body;
    body.insert(QStringLiteral("value"), QJsonValue::fromVariant(value));
    requestJson(
        QStringLiteral("PATCH"),
        QStringLiteral("/memory/tool-parameters/") + encodedId(tool) + QStringLiteral("/")
            + encodedId(field),
        body,
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->showToast(
                    errorText(object, QStringLiteral("参数保存失败"), status));
                return;
            }
            m_chart->showToast(QStringLiteral("已更新"));
            loadMemoryTools();
        });
}

void DemoHost::memoryDeleteField(const QString &tool, const QString &field)
{
    if (!m_live)
        return;
    requestJson(
        QStringLiteral("DELETE"),
        QStringLiteral("/memory/tool-parameters/") + encodedId(tool) + QStringLiteral("/")
            + encodedId(field),
        QJsonObject(),
        [this](const QJsonObject &object, int status) {
            // 204 是正常路径；404 说明别处已经删过了，照样重拉
            if (status != 404 && (status < 200 || status >= 300)) {
                m_chart->showToast(
                    errorText(object, QStringLiteral("删除失败"), status));
                return;
            }
            m_chart->showToast(status == 404 ? QStringLiteral("该字段已不存在，已刷新")
                                             : QStringLiteral("已删除"));
            loadMemoryTools();
        });
}

void DemoHost::memoryClearTool(const QString &tool)
{
    if (!m_live)
        return;
    requestJson(
        QStringLiteral("DELETE"),
        QStringLiteral("/memory/tool-parameters/") + encodedId(tool), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            if (status != 404 && (status < 200 || status >= 300)) {
                m_chart->showToast(
                    errorText(object, QStringLiteral("清除失败"), status));
                return;
            }
            m_chart->showToast(status == 404 ? QStringLiteral("该工具已不存在，已刷新")
                                             : QStringLiteral("已清除"));
            loadMemoryTools();
        });
}

void DemoHost::memoryClearAllTools()
{
    if (!m_live)
        return;
    requestJson(
        QStringLiteral("DELETE"), QStringLiteral("/memory/tool-parameters"), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->showToast(
                    errorText(object, QStringLiteral("清空失败"), status));
                return;
            }
            m_chart->showToast(
                QStringLiteral("已清空 %1 个字段")
                    .arg(object.value(QStringLiteral("fields")).toInt()));
            loadMemoryTools();
        });
}

void DemoHost::memoryMaintenance(const QString &kind)
{
    if (!m_live)
        return;
    QJsonObject body;
    body.insert(QStringLiteral("kind"), kind.isEmpty() ? QStringLiteral("governance") : kind);
    requestJson(
        QStringLiteral("POST"), QStringLiteral("/memory/maintenance/runs"), body,
        [this, kind](const QJsonObject &object, int status) {
            if (status == 409) {
                m_chart->showToast(QStringLiteral("已有维护任务在运行"));
                loadMemorySummary();
                return;
            }
            // 202 = 已投递，进度靠面板自己的轮询（memoryRefreshRequested）拉 summary
            if (status < 200 || status >= 300) {
                m_chart->showToast(
                    errorText(object, QStringLiteral("维护任务投递失败"), status));
                return;
            }
            m_chart->showToast(kind == QLatin1String("extract")
                                   ? QStringLiteral("已投递提炼任务，完成后候选会出现在待审核列表")
                                   : QStringLiteral("已开始维护"));
            loadMemorySummary();
        });
}

void DemoHost::memoryShowDetail(const QString &id)
{
    // 详情不另开接口：列表里已经带着 payload，直接摊开给用户看
    // （真要做服务端详情可以打 GET /memory/experiences/{id}，demo 从简）
    QVariantMap item;
    for (const QVariant &entry : m_memoryItems) {
        const QVariantMap candidate = entry.toMap();
        if (candidate.value(QStringLiteral("id")).toString() == id) {
            item = candidate;
            break;
        }
    }
    if (item.isEmpty()) {
        m_chart->showToast(QStringLiteral("该条记忆已不在当前列表，已刷新"));
        loadMemoryList(m_memoryPage, m_memoryFilters);
        return;
    }
    QStringList lines;
    QString title = item.value(QStringLiteral("title")).toString();
    if (title.isEmpty())
        title = QStringLiteral("（无标题）");
    lines << QStringLiteral("标题：%1").arg(title);
    lines << QStringLiteral("状态：%1").arg(item.value(QStringLiteral("status")).toString());
    lines << QStringLiteral("类型：%1").arg(item.value(QStringLiteral("kind")).toString());
    lines << QStringLiteral("作用域：%1").arg(item.value(QStringLiteral("scope")).toString());
    lines << QStringLiteral("置信度：%1    证据：%2")
                 .arg(item.value(QStringLiteral("confidence")).toDouble())
                 .arg(item.value(QStringLiteral("evidence_count")).toInt());
    const QVariant payload = item.value(QStringLiteral("payload"));
    if (payload.isValid() && !payload.toMap().isEmpty()) {
        lines << QString();
        lines << QStringLiteral("payload：");
        lines << QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(payload.toMap()))
                                      .toJson(QJsonDocument::Indented));
    }
    QMessageBox::information(m_chart, QStringLiteral("记忆详情 · %1").arg(id),
                             lines.join(QLatin1Char('\n')));
}

// ------------------------------------------------------------ 知识库页（/kb/*）

void DemoHost::loadKnowledgeBases()
{
    if (!m_live)
        return;
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/kb/libraries"), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300)
                return;
            const QVariantList libraries =
                object.value(QStringLiteral("libraries")).toArray().toVariantList();
            m_chart->setKnowledgeBases(libraries);
            // 库被删了就把输入区的选择收回「全部知识库」（webui loadKbLibraries），
            // 否则下一轮会带着一个不存在的库 id 发出去
            if (m_kbLibrary == QLatin1String("*") || m_kbLibrary == QLatin1String("__none__"))
                return;
            bool found = false;
            for (const QVariant &entry : libraries) {
                if (entry.toMap().value(QStringLiteral("id")).toString() == m_kbLibrary) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                m_kbLibrary = QStringLiteral("*");
                m_chart->setCurrentKnowledgeBase(m_kbLibrary);
            }
        });
}

void DemoHost::kbRefresh(const QString &libraryId)
{
    if (!m_live || m_kbAdopting)
        return;
    m_chart->setKbStatusText(QStringLiteral("正在读取知识库…"));
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/kb/libraries"), QJsonObject(),
        [this, libraryId](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->setKbStatusText(
                    QStringLiteral("知识库读取失败：%1")
                        .arg(errorText(object, QStringLiteral("后台不可达"), status)));
                return;
            }
            kbAdoptLibraries(object.value(QStringLiteral("libraries")).toArray().toVariantList(),
                             libraryId);
        });
}

void DemoHost::kbAdoptLibraries(const QVariantList &libraries, const QString &wanted)
{
    m_chart->setKnowledgeBases(libraries);
    QString target = wanted;
    bool found = false;
    for (const QVariant &entry : libraries) {
        if (entry.toMap().value(QStringLiteral("id")).toString() == target) {
            found = true;
            break;
        }
    }
    if (!found) {
        target = libraries.isEmpty()
                     ? QString()
                     : libraries.first().toMap().value(QStringLiteral("id")).toString();
    }
    if (target != m_kbPanelLibrary)
        m_kbFolder = QStringLiteral("all"); // 换库就把归档位置收回「全部资料」
    m_kbPanelLibrary = target;

    // 推库列表：面板若因此换库会同步回发 libraryChanged / folderChanged，
    // 那两发是回声（settingsdialog 把它们接到同一条链上），用 m_kbAdopting 吃掉
    m_kbAdopting = true;
    m_chart->setKbLibraries(libraries);
    m_kbAdopting = false;

    if (m_live)
        kbLoadPanel(m_kbPanelLibrary, m_kbFolder);
}

void DemoHost::kbLoadPanel(const QString &libraryId, const QString &folderKey)
{
    const QString libQuery = libraryId.isEmpty()
                                 ? QString()
                                 : QStringLiteral("?library_id=") + encodedId(libraryId);
    // status 先拉：它给出库级总量，「全部资料」那行与 docsTotal 都靠它（webui kbState.docsTotal）
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/kb/status") + libQuery, QJsonObject(),
        [this, libraryId, folderKey](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->setKbStatusText(
                    QStringLiteral("知识库状态读取失败：%1")
                        .arg(errorText(object, QStringLiteral("后台不可达"), status)));
                return;
            }
            m_chart->setKbStatus(object.toVariantMap());
            const int total = object.value(QStringLiteral("documents")).toInt();
            const QString lib = libraryId;

            requestJson(
                QStringLiteral("GET"),
                QStringLiteral("/kb/folders")
                    + (lib.isEmpty() ? QString() : QStringLiteral("?library_id=") + encodedId(lib)),
                QJsonObject(),
                [this](const QJsonObject &folders, int folderStatus) {
                    if (folderStatus < 200 || folderStatus >= 300)
                        return;
                    m_chart->setKbFolders(
                        folders.value(QStringLiteral("folders")).toArray().toVariantList(),
                        folders.value(QStringLiteral("tree")).toObject().toVariantMap());
                });

            // folder 参数：all 表示不传（后端列全库），root 传空串，其余传真文件夹 id
            QStringList parts;
            if (!lib.isEmpty())
                parts << QStringLiteral("library_id=") + encodedId(lib);
            if (folderKey == QLatin1String("root"))
                parts << QStringLiteral("folder=");
            else if (!folderKey.isEmpty() && folderKey != QLatin1String("all"))
                parts << QStringLiteral("folder=") + encodedId(folderKey);
            const QString path = parts.isEmpty()
                                     ? QStringLiteral("/kb/documents")
                                     : QStringLiteral("/kb/documents?")
                                           + parts.join(QLatin1Char('&'));
            requestJson(
                QStringLiteral("GET"), path, QJsonObject(),
                [this, total](const QJsonObject &docs, int docsStatus) {
                    if (docsStatus < 200 || docsStatus >= 300)
                        return;
                    m_chart->setKbDocuments(
                        docs.value(QStringLiteral("documents")).toArray().toVariantList(), total);
                });
        });
}

void DemoHost::kbImportFiles(const QStringList &paths, const QString &libraryId,
                             const QString &folderId)
{
    if (!m_live || !m_network)
        return;
    QStringList files;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isFile())
            files << info.absoluteFilePath(); // 目录请走 kbDirImportRequested（/kb/import）
    }
    if (files.isEmpty()) {
        m_chart->setKbStatusText(QStringLiteral("没有可导入的文件"));
        return;
    }

    // 逐份串行上传：后端每份都会当场建索引，几十份并发只会把 sqlite 写锁打满。
    // 进度只报到状态行（没做进度条，见 README「宿主职责」）
    auto cursor = std::make_shared<int>(0);
    auto failures = std::make_shared<QStringList>();
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, files, libraryId, folderId, cursor, failures, step] {
        if (*cursor >= files.size()) {
            const int okCount = files.size() - failures->size();
            if (failures->isEmpty()) {
                m_chart->setKbStatusText(
                    QStringLiteral("导入完成：成功 %1 份").arg(okCount));
            } else {
                m_chart->setKbStatusText(QStringLiteral("导入完成：成功 %1 份，失败 %2 份（%3）")
                                             .arg(okCount)
                                             .arg(failures->size())
                                             .arg(failures->first()));
            }
            loadKnowledgeBases(); // 份数 / 分块数变了，输入区下拉的副文本跟着刷
            kbLoadPanel(m_kbPanelLibrary, m_kbFolder);
            return;
        }
        const QString path = files.at(*cursor);
        const QFileInfo info(path);
        const int position = *cursor + 1;
        m_chart->setKbStatusText(QStringLiteral("正在索引进「%1」%2/%3：%4…")
                                     .arg(m_kbFolder.isEmpty() || m_kbFolder == QLatin1String("all")
                                              ? QStringLiteral("全部资料")
                                              : m_kbFolder)
                                     .arg(position)
                                     .arg(files.size())
                                     .arg(info.fileName()));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            failures->append(QStringLiteral("%1：无法读取文件").arg(info.fileName()));
            ++*cursor;
            (*step)();
            return;
        }
        const QByteArray bytes = file.readAll();
        file.close();

        // 字节上传走 /kb/upload：正文即文件字节（与 webui kbUploadOne 的 xhr.send(file) 同形），
        // 目标库 / 目录走 query。multipart 后端不支持，别按表单发
        QUrl url(apiUrl(QStringLiteral("/kb/upload")));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("name"), info.fileName());
        if (!libraryId.isEmpty())
            query.addQueryItem(QStringLiteral("library_id"), libraryId);
        if (!folderId.isEmpty())
            query.addQueryItem(QStringLiteral("folder_id"), folderId);
        url.setQuery(query);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/octet-stream"));
        QNetworkReply *reply = m_network->post(request, bytes);
        QObject::connect(reply, &QNetworkReply::finished, m_chart,
                         [this, reply, info, failures, cursor, step] {
                             const int status =
                                 reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                             const QJsonObject object =
                                 QJsonDocument::fromJson(reply->readAll()).object();
                             if (reply->error() != QNetworkReply::NoError || status < 200
                                 || status >= 300) {
                                 failures->append(
                                     QStringLiteral("%1：%2")
                                         .arg(info.fileName(),
                                              errorText(object, reply->errorString(), status)));
                             }
                             reply->deleteLater();
                             ++*cursor;
                             (*step)();
                         });
    };
    (*step)();
}

void DemoHost::kbImportDir(const QString &libraryId, const QString &dirPath,
                           const QString &folderId)
{
    if (!m_live)
        return;
    const QString dir = dirPath.trimmed();
    if (dir.isEmpty()) {
        m_chart->setKbStatusText(QStringLiteral("请先填一个本机目录路径"));
        return;
    }
    // 目录形态走后端自己的扫描：JSON 传本机路径（与 webui kbImportDir 同一份 body）
    QJsonObject body;
    body.insert(QStringLiteral("paths"), QJsonArray{ dir });
    body.insert(QStringLiteral("recursive"), true);
    body.insert(QStringLiteral("mirror_tree"), true); // 按原目录结构在库里建文件夹
    if (!libraryId.isEmpty())
        body.insert(QStringLiteral("library_id"), libraryId);
    if (!folderId.isEmpty())
        body.insert(QStringLiteral("folder_id"), folderId); // 落当前选中的归档目录
    m_chart->setKbStatusText(QStringLiteral("正在扫描并索引进「库根」…（文件多时请等一会儿）"));
    requestJson(
        QStringLiteral("POST"), QStringLiteral("/kb/import"), body,
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->setKbStatusText(
                    QStringLiteral("导入失败：%1")
                        .arg(errorText(object, QStringLiteral("未知错误"), status)));
                return;
            }
            const int ok = object.value(QStringLiteral("indexed")).toArray().size();
            const int bad = object.value(QStringLiteral("failed")).toArray().size();
            const int skipped = object.value(QStringLiteral("skipped_count")).toInt();
            QString text = QStringLiteral("导入完成：成功 %1 份").arg(ok);
            if (bad > 0)
                text += QStringLiteral("，失败 %1 份").arg(bad);
            if (skipped > 0)
                text += QStringLiteral("，跳过 %1 个不支持的文件").arg(skipped);
            const QJsonArray missing = object.value(QStringLiteral("missing")).toArray();
            if (ok == 0 && !missing.isEmpty())
                text = QStringLiteral("路径不存在：%1").arg(missing.first().toString());
            m_chart->setKbStatusText(text);
            loadKnowledgeBases();
            kbLoadPanel(m_kbPanelLibrary, m_kbFolder);
        });
}

// ------------------------------------------------------------ 模型信息 / 顶栏 / 收尾

void DemoHost::loadModelMetadata()
{
    if (!m_live)
        return;
    requestJson(
        QStringLiteral("GET"), QStringLiteral("/config/model-metadata"), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->setModelMetadataFailed(
                    QStringLiteral("load"),
                    errorText(object, QStringLiteral("模型信息读取失败"), status), status);
                return;
            }
            m_modelMeta = object.toVariantMap();
            m_chart->setModelMetadata(m_modelMeta);
        });
}

void DemoHost::refreshModelMetadata()
{
    if (!m_live)
        return;
    m_chart->setSettingsStatus(QStringLiteral("正在更新模型信息…"));
    requestJson(
        QStringLiteral("POST"), QStringLiteral("/config/model-metadata/refresh"), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->setModelMetadataFailed(
                    QStringLiteral("refresh"),
                    errorText(object, QStringLiteral("更新模型信息失败"), status), status);
                return;
            }
            m_modelMeta = object.toVariantMap();
            m_modelMeta.remove(QStringLiteral("ok")); // 刷新响应比查询多带一个 ok 标记
            m_chart->setModelMetadata(m_modelMeta);
            m_chart->setSettingsStatus(QStringLiteral("模型信息已更新"));
        });
}

void DemoHost::resolveModelLimits(const QString &providerId, const QString &modelId)
{
    if (!m_live || modelId.isEmpty())
        return;
    // 快照只按模型 ID + API 地址查表，base_url 从当前配置的 provider 里取
    QString baseUrl;
    const QVariantList providers = m_config.value(QStringLiteral("providers")).toList();
    for (const QVariant &value : providers) {
        const QVariantMap provider = value.toMap();
        if (provider.value(QStringLiteral("id")).toString() == providerId) {
            baseUrl = provider.value(QStringLiteral("base_url")).toString();
            break;
        }
    }
    QJsonObject item;
    item.insert(QStringLiteral("id"), modelId);
    if (!baseUrl.isEmpty())
        item.insert(QStringLiteral("base_url"), baseUrl);
    QJsonObject body;
    body.insert(QStringLiteral("models"), QJsonArray{ item });
    requestJson(
        QStringLiteral("POST"), QStringLiteral("/config/model-metadata/resolve"), body,
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->setModelMetadataFailed(
                    QStringLiteral("load"),
                    errorText(object, QStringLiteral("模型上限查询失败"), status), status);
                return;
            }
            const QJsonObject limits = object.value(QStringLiteral("limits")).toObject();
            if (limits.isEmpty())
                return;
            // resolve 只给 {limits: {modelId: {...}|null}}：并进上一次的快照再整体回填，
            // 别让状态行（来源 / 抓取日期 / 收录规模）被一个只有 limits 的快照顶掉
            QVariantMap merged = m_modelMeta;
            QVariantMap known = merged.value(QStringLiteral("limits")).toMap();
            for (auto it = limits.constBegin(); it != limits.constEnd(); ++it)
                known.insert(it.key(), it.value().toVariant());
            merged.insert(QStringLiteral("limits"), known);
            m_modelMeta = merged;
            m_chart->setModelMetadata(m_modelMeta);
        });
}

void DemoHost::launchGridStar()
{
    if (!m_live)
        return;
    m_chart->showToast(QStringLiteral("正在确认 GridStar 进程…"));
    requestJson(
        QStringLiteral("POST"), QStringLiteral("/gridstar/ensure"), QJsonObject(),
        [this](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                m_chart->showToast(
                    QStringLiteral("无法启动 GridStar：%1")
                        .arg(errorText(object, QStringLiteral("未知错误"), status)));
                return;
            }
            m_chart->showToast(object.value(QStringLiteral("launched")).toBool()
                                   ? QStringLiteral("正在启动 GridStar…")
                                   : QStringLiteral("GridStar 已在运行"));
        });
}

void DemoHost::endTurn(const QString &label)
{
    const QString text = label.trimmed();
    if (text.isEmpty())
        return;
    if (m_sessionId.isEmpty()) {
        // 库已在本地追过这条用户消息，这里直发就行，不要再追一次
        startLiveTurn(text, text, QVariantList(), false);
        return;
    }
    // 「结束」类选项：只把这个选择落成一条用户消息，不开新一轮（否则 auto 模式会接着干）
    QJsonObject body;
    body.insert(QStringLiteral("label"), text);
    requestJson(
        QStringLiteral("POST"),
        QStringLiteral("/sessions/") + encodedId(m_sessionId) + QStringLiteral("/end-turn"),
        body,
        [this, text](const QJsonObject &object, int status) {
            if (status < 200 || status >= 300) {
                // 后端不认这条收尾（旧版本 / 会话已换）：退回普通选择发送，别让点击落空。
                // 用户消息库里已经追过了（endTurn 的本地收尾），appendUser=false 避开重复气泡
                qWarning() << "[demo:live] end-turn failed" << status
                           << object.value(QStringLiteral("error")).toString();
                m_chart->showToast(QStringLiteral("收尾失败，已按普通选择发送"));
                startLiveTurn(text, text, QVariantList(), false);
                return;
            }
            m_chart->showToast(QStringLiteral("本轮已结束"));
            refreshSessions(false);
        });
}

// ------------------------------------------------------------ 上下文圆环

qint64 DemoHost::contextWindowFor(const QString &modelKey) const
{
    const QVariantList models = m_liveModels.isEmpty() ? demoModels() : m_liveModels;
    for (const QVariant &value : models) {
        const QVariantMap item = value.toMap();
        QString key = item.value(QStringLiteral("key")).toString();
        if (key.isEmpty()) {
            const QString id = item.value(QStringLiteral("id")).toString();
            const QString provider = item.value(QStringLiteral("provider")).toString();
            key = provider.isEmpty() ? id : provider + QLatin1Char('/') + id;
        }
        if (key != modelKey)
            continue;
        const qint64 window = item.value(QStringLiteral("context_window")).toLongLong();
        return window > 0 ? window : 0;
    }
    return 0;
}

void DemoHost::applyContextUsage(const QVariantMap &event)
{
    // 用量口径（webui applyContextUsage）：used 取最近一次请求实际送出的 input（含缓存读），
    // 上限优先用事件里的 context_window，取不到就回落到当前模型的配置
    const double used = event.value(QStringLiteral("context_used")).toDouble();
    const double window = event.value(QStringLiteral("context_window")).toDouble();
    if (used <= 0.0 && window <= 0.0)
        return; // 事件里没带（老后端）：保持圆环现状，别把已有读数清掉

    const QString model = m_chart->currentModel();
    const qint64 max = window > 0.0 ? qint64(window) : contextWindowFor(model);
    pushContextRing(used, max, event.value(QStringLiteral("context_estimated")).toBool());
}

void DemoHost::pushContextRing(double used, qint64 window, bool estimated)
{
    if (used <= 0.0 && window <= 0)
        m_chart->clearContextUsage();
    m_contextUsed = used > 0.0 ? used : 0.0;
    m_contextEstimated = estimated;

    const QString model = m_chart->currentModel();
    QVariantMap usage;
    usage.insert(QStringLiteral("model"), model);
    usage.insert(QStringLiteral("estimated"), estimated);
    if (window > 0)
        usage.insert(QStringLiteral("max"), window);
    if (used > 0.0) {
        usage.insert(QStringLiteral("used"), qint64(used));
        if (window > 0) {
            usage.insert(QStringLiteral("left"), qMax<qint64>(0, window - qint64(used)));
            usage.insert(QStringLiteral("percent"), int(qRound(used / double(window) * 100.0)));
        }
    }
    m_chart->setContextUsage(usage);
}

void DemoHost::restoreContextRing(const QVariantList &messages)
{
    // 切会话 / 刷新后从历史恢复（webui restoreContextRing）：取最近一条带
    // context_used 的消息；一条都没有就清空圆环，不沿用上一个会话的读数
    double used = 0.0;
    for (int i = messages.size() - 1; i >= 0; --i) {
        const QVariantMap usage =
            messages.at(i).toMap().value(QStringLiteral("usage")).toMap();
        const double value = usage.value(QStringLiteral("context_used")).toDouble();
        if (value > 0.0) {
            used = value;
            break;
        }
    }
    if (used <= 0.0) {
        pushContextRing(0.0, 0, false);
        return;
    }
    pushContextRing(used, contextWindowFor(m_chart->currentModel()), false);
}

void DemoHost::openSettings(const QString &tab)
{
    // 现在库会把「设置中心被打开」发成 settingsOpened(tab)，宿主在那里统一拉快照；
    // 这里只保留一个兑底：不经库入口的路径（如书签直开）也能拿到数据
    if (m_live)
        loadModelMetadata();
    m_chart->openSettings(tab);
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
        // 换了模型，圆环的分母跟着换（webui renderContextRing 每次都取当前模型的窗口）
        if (m_contextUsed > 0.0)
            pushContextRing(m_contextUsed, contextWindowFor(key), m_contextEstimated);
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

    // 输入区与顶栏的新件（webui #category-select / #kb-select / #brand-mark）
    QObject::connect(c, &ChartWidget::categorySelected, c, [this](const QString &id) {
        // 用户自己点过下拉框才算「显式选择」（webui categoryTouched）：只有显式选择才允许
        // 覆盖服务端 meta 里的分类，「全部」那个空值也一样
        m_category = id;
        m_categoryTouched = true;
        m_chart->showToast(id.isEmpty() ? QStringLiteral("专业分类：全部")
                                        : QStringLiteral("专业分类：") + id);
    });
    QObject::connect(c, &ChartWidget::knowledgeBaseSelected, c, [this](const QString &id) {
        // __none__ / * / 具体库 id 都存在宿主侧，发消息时作为 kb_library 带出
        m_kbLibrary = id;
        m_chart->showToast(QStringLiteral("知识库：") + id);
    });
    QObject::connect(c, &ChartWidget::launchAppRequested, c, [this] {
        // webui launchGridStar：POST /gridstar/ensure
        if (m_live) {
            launchGridStar();
            return;
        }
        m_chart->showToast(QStringLiteral("演示模式：不会真的启动 GridStar"));
    });
    QObject::connect(c, &ChartWidget::modelMetadataRefreshRequested, c, [this] {
        if (m_live) {
            refreshModelMetadata();
            return;
        }
        m_chart->showToast(QStringLiteral("演示模式：模型信息不会变"));
    });
    // 新增模型后补上限（webui fillModelLimits）：resolve 只给 limits，宿主并回快照里回填
    QObject::connect(c, &ChartWidget::modelLimitsResolveRequested, c,
                     [this](const QString &providerId, const QString &modelId) {
                         if (m_live) {
                             resolveModelLimits(providerId, modelId);
                             return;
                         }
                         qDebug() << "[demo] resolve limits" << providerId << modelId;
                     });

    // 「结束」类选项：宿主 POST /sessions/{id}/end-turn
    QObject::connect(c, &ChartWidget::endTurnRequested, c, [this](const QString &label) {
        if (m_live) {
            endTurn(label);
            return;
        }
        qDebug() << "[demo] end-turn" << label;
    });

    // ---- 设置中心「记忆」页（/memory/*） ----
    QObject::connect(c, &ChartWidget::memoryRefreshRequested, c, [this] {
        // 进页、维护中轮询（3s）都走这里：summary 与 tool-parameters 两路并发拉
        if (!m_live)
            return; // 假数据路径：populate() 已把两份数据推进去
        loadMemorySummary();
        loadMemoryTools();
    });
    QObject::connect(c, &ChartWidget::memoryListRequested, c,
                     [this](int page, const QVariantMap &filters) {
                         if (m_live)
                             loadMemoryList(page, filters);
                     });
    QObject::connect(c, &ChartWidget::memoryConfigPatchRequested, c,
                     [this](const QVariantMap &updates) {
                         if (m_live) {
                             memoryPatchConfig(updates);
                             return;
                         }
                         m_chart->showToast(QStringLiteral("演示模式：记忆开关不会落盘"));
                     });
    QObject::connect(c, &ChartWidget::memoryActionRequested, c,
                     [this](const QString &id, const QString &action) {
                         if (m_live)
                             memoryTransition(id, action);
                     });
    QObject::connect(c, &ChartWidget::memoryBulkRequested, c,
                     [this](const QStringList &ids, const QString &action) {
                         if (m_live)
                             memoryBulk(ids, action);
                     });
    QObject::connect(c, &ChartWidget::memoryDetailRequested, c, [this](const QString &id) {
        // 详情不另开接口：从当前列表里取那条，把 payload 摊开给用户看
        memoryShowDetail(id);
    });
    QObject::connect(c, &ChartWidget::memoryFieldSaveRequested, c,
                     [this](const QString &tool, const QString &field, const QVariant &value) {
                         if (m_live)
                             memorySaveField(tool, field, value);
                     });
    QObject::connect(c, &ChartWidget::memoryFieldDeleteRequested, c,
                     [this](const QString &tool, const QString &field) {
                         if (m_live)
                             memoryDeleteField(tool, field);
                     });
    QObject::connect(c, &ChartWidget::memoryToolClearRequested, c, [this](const QString &tool) {
        if (m_live)
            memoryClearTool(tool);
    });
    QObject::connect(c, &ChartWidget::memoryToolsClearAllRequested, c, [this] {
        if (m_live)
            memoryClearAllTools();
    });
    QObject::connect(c, &ChartWidget::memoryMaintenanceRequested, c, [this](const QString &kind) {
        if (m_live)
            memoryMaintenance(kind);
    });

    // ---- 设置中心「知识库」页（/kb/*） ----
    QObject::connect(c, &ChartWidget::kbRefreshRequested, c, [this](const QString &libraryId) {
        // 进页 / 换库 / 点刷新：库列表 → status → folders + documents
        if (m_live)
            kbRefresh(libraryId);
    });
    QObject::connect(c, &ChartWidget::kbFolderChanged, c,
                     [this](const QString &libraryId, const QString &folderId) {
                         m_kbPanelLibrary = libraryId;
                         m_kbFolder = folderId.isEmpty() ? QStringLiteral("all") : folderId;
                         if (!m_live || m_kbAdopting)
                             return; // 回声：库列表刚由本地推进面板，kbAdoptLibraries 里统一填
                         kbLoadPanel(libraryId, m_kbFolder);
                     });
    QObject::connect(c, &ChartWidget::kbLibraryCreateRequested, c, [this](const QString &name) {
        if (!m_live || name.isEmpty())
            return;
        QJsonObject body;
        body.insert(QStringLiteral("name"), name);
        requestJson(QStringLiteral("POST"), QStringLiteral("/kb/libraries"), body,
                    [this, name](const QJsonObject &object, int status) {
                        if (status < 200 || status >= 300) {
                            m_chart->setKbStatusText(
                                QStringLiteral("新建知识库失败：%1")
                                    .arg(errorText(object, QStringLiteral("未知错误"), status)));
                            return;
                        }
                        const QString created =
                            object.value(QStringLiteral("library"))
                                .toObject()
                                .value(QStringLiteral("id"))
                                .toString();
                        m_chart->setKbStatusText(
                            QStringLiteral("已新建知识库「%1」，现在可以往里加文件了").arg(name));
                        kbAdoptLibraries(
                            object.value(QStringLiteral("libraries")).toArray().toVariantList(),
                            created);
                    });
    });
    QObject::connect(c, &ChartWidget::kbLibraryRenameRequested, c,
                     [this](const QString &libraryId, const QString &name) {
                         if (!m_live || libraryId.isEmpty() || name.isEmpty())
                             return;
                         QJsonObject body;
                         body.insert(QStringLiteral("name"), name);
                         requestJson(QStringLiteral("PATCH"),
                                     QStringLiteral("/kb/libraries/") + encodedId(libraryId), body,
                                     [this, libraryId](const QJsonObject &object, int status) {
                                         if (status < 200 || status >= 300) {
                                             m_chart->setKbStatusText(
                                                 QStringLiteral("重命名知识库失败：%1")
                                                     .arg(errorText(object, QStringLiteral("未知错误"),
                                                                    status)));
                                             return;
                                         }
                                         kbAdoptLibraries(
                                             object.value(QStringLiteral("libraries"))
                                                 .toArray()
                                                 .toVariantList(),
                                             libraryId);
                                     });
                     });
    QObject::connect(c, &ChartWidget::kbLibraryDeleteRequested, c, [this](const QString &libraryId) {
        if (!m_live || libraryId.isEmpty())
            return;
        requestJson(QStringLiteral("DELETE"),
                    QStringLiteral("/kb/libraries/") + encodedId(libraryId), QJsonObject(),
                    [this](const QJsonObject &object, int status) {
                        if (status < 200 || status >= 300) {
                            m_chart->setKbStatusText(
                                QStringLiteral("删除知识库失败：%1")
                                    .arg(errorText(object, QStringLiteral("未知错误"), status)));
                            return;
                        }
                        // 删掉的正好是当前库：kbAdoptLibraries 会落到第一个库（或空）
                        kbAdoptLibraries(
                            object.value(QStringLiteral("libraries")).toArray().toVariantList(),
                            QString());
                    });
    });
    QObject::connect(c, &ChartWidget::kbFilesImportRequested, c,
                     [this](const QStringList &paths, const QString &libraryId,
                            const QString &folderId) {
                         if (m_live)
                             kbImportFiles(paths, libraryId, folderId);
                     });
    QObject::connect(c, &ChartWidget::kbDirImportToFolderRequested, c,
                     [this](const QString &libraryId, const QString &dirPath,
                            const QString &folderId) {
                         if (m_live)
                             kbImportDir(libraryId, dirPath, folderId);
                     });
    QObject::connect(c, &ChartWidget::kbDocumentDeleteRequested, c, [this](const QString &docId) {
        if (!m_live || docId.isEmpty())
            return;
        requestJson(QStringLiteral("DELETE"),
                    QStringLiteral("/kb/documents/") + encodedId(docId), QJsonObject(),
                    [this](const QJsonObject &object, int status) {
                        if (status < 200 || status >= 300) {
                            m_chart->setKbStatusText(
                                QStringLiteral("删除资料失败：%1")
                                    .arg(errorText(object, QStringLiteral("未知错误"), status)));
                            return;
                        }
                        loadKnowledgeBases();
                        kbLoadPanel(m_kbPanelLibrary, m_kbFolder);
                    });
    });
    QObject::connect(c, &ChartWidget::kbDocumentReindexRequested, c, [this](const QString &docId) {
        if (!m_live || docId.isEmpty())
            return;
        m_chart->setKbStatusText(QStringLiteral("正在重建索引…"));
        requestJson(QStringLiteral("POST"),
                    QStringLiteral("/kb/documents/") + encodedId(docId)
                        + QStringLiteral("/reindex"),
                    QJsonObject(),
                    [this](const QJsonObject &object, int status) {
                        if (status < 200 || status >= 300) {
                            m_chart->setKbStatusText(
                                QStringLiteral("重建索引失败：%1")
                                    .arg(errorText(object, QStringLiteral("未知错误"), status)));
                            return;
                        }
                        kbLoadPanel(m_kbPanelLibrary, m_kbFolder);
                    });
    });
    QObject::connect(c, &ChartWidget::kbDocumentMoveRequested, c,
                     [this](const QString &docId, const QString &folderId) {
                         if (!m_live || docId.isEmpty())
                             return;
                         // folder_id 为空串 = 移回库根（后端口径，与 webui kbMoveDoc 一致）
                         QJsonObject body;
                         body.insert(QStringLiteral("folder_id"), folderId);
                         requestJson(QStringLiteral("PATCH"),
                                     QStringLiteral("/kb/documents/") + encodedId(docId), body,
                                     [this](const QJsonObject &object, int status) {
                                         if (status < 200 || status >= 300) {
                                             m_chart->setKbStatusText(
                                                 QStringLiteral("归档失败：%1")
                                                     .arg(errorText(object,
                                                                    QStringLiteral("未知错误"),
                                                                    status)));
                                             return;
                                         }
                                         kbLoadPanel(m_kbPanelLibrary, m_kbFolder);
                                     });
                     });
    QObject::connect(c, &ChartWidget::kbFolderCreateRequested, c,
                     [this](const QString &libraryId, const QString &parentId,
                            const QString &name) {
                         if (!m_live || name.isEmpty())
                             return;
                         QJsonObject body;
                         body.insert(QStringLiteral("library_id"), libraryId);
                         body.insert(QStringLiteral("name"), name);
                         body.insert(QStringLiteral("parent_id"), parentId);
                         requestJson(QStringLiteral("POST"), QStringLiteral("/kb/folders"), body,
                                     [this](const QJsonObject &object, int status) {
                                         if (status < 200 || status >= 300) {
                                             m_chart->setKbStatusText(
                                                 QStringLiteral("新建文件夹失败：%1")
                                                     .arg(errorText(object,
                                                                    QStringLiteral("未知错误"),
                                                                    status)));
                                             return;
                                         }
                                         kbLoadPanel(m_kbPanelLibrary, m_kbFolder);
                                     });
                     });
    QObject::connect(c, &ChartWidget::kbFolderRenameRequested, c,
                     [this](const QString &folderId, const QString &name) {
                         if (!m_live || folderId.isEmpty() || name.isEmpty())
                             return;
                         QJsonObject body;
                         body.insert(QStringLiteral("name"), name);
                         requestJson(QStringLiteral("PATCH"),
                                     QStringLiteral("/kb/folders/") + encodedId(folderId), body,
                                     [this](const QJsonObject &object, int status) {
                                         if (status < 200 || status >= 300) {
                                             m_chart->setKbStatusText(
                                                 QStringLiteral("重命名文件夹失败：%1")
                                                     .arg(errorText(object,
                                                                    QStringLiteral("未知错误"),
                                                                    status)));
                                             return;
                                         }
                                         kbLoadPanel(m_kbPanelLibrary, m_kbFolder);
                                     });
                     });
    QObject::connect(c, &ChartWidget::kbFolderDeleteRequested, c, [this](const QString &folderId) {
        if (!m_live || folderId.isEmpty())
            return;
        requestJson(QStringLiteral("DELETE"),
                    QStringLiteral("/kb/folders/") + encodedId(folderId), QJsonObject(),
                    [this](const QJsonObject &object, int status) {
                        if (status < 200 || status >= 300) {
                            m_chart->setKbStatusText(
                                QStringLiteral("删除文件夹失败：%1")
                                    .arg(errorText(object, QStringLiteral("未知错误"), status)));
                            return;
                        }
                        // 删掉的可能正是当前目录：收回「全部资料」再拉，否则会拉一个不存在的目录
                        m_kbFolder = QStringLiteral("all");
                        kbLoadPanel(m_kbPanelLibrary, m_kbFolder);
                    });
    });
    // 设置中心被打开（哪个 Tab），也包括用户点控件自带的设置按钮那一路：
    // 以前这条路宿主无感，模型信息行会停在「正在读取模型信息…」
    QObject::connect(c, &ChartWidget::settingsOpened, c, [this](const QString &tab) {
        Q_UNUSED(tab)
        if (m_live)
            loadModelMetadata();
    });

    QObject::connect(c, &ChartWidget::kbSearchRequested, c,
                     [this](const QString &libraryId, const QString &query) {
                         if (!m_live)
                             return; // 假数据路径：populate() 已给了示例检索结果
                         QUrlQuery params;
                         params.addQueryItem(QStringLiteral("q"), query);
                         params.addQueryItem(QStringLiteral("top_k"), QStringLiteral("6"));
                         if (!libraryId.isEmpty())
                             params.addQueryItem(QStringLiteral("library_id"), libraryId);
                         requestJson(
                             QStringLiteral("GET"),
                             QStringLiteral("/kb/search?") + params.query(QUrl::FullyEncoded),
                             QJsonObject(),
                             [this](const QJsonObject &object, int status) {
                                 if (status < 200 || status >= 300) {
                                     m_chart->setKbStatusText(
                                         QStringLiteral("检索失败：%1")
                                             .arg(errorText(object, QStringLiteral("未知错误"),
                                                            status)));
                                     return;
                                 }
                                 m_chart->setKbSearchResult(object.toVariantMap());
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
                             const QVariantList &attachments, bool appendUser)
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
            [this, message, display, attachments, appendUser](const QJsonObject &object,
                                                              int status) {
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
                startLiveTurn(message, display, attachments, appendUser);
            });
        return;
    }

    m_stopped = false;
    m_streamDone = false;
    m_streamFailed = false;
    // 「结束类选项收尾失败后改走普通发送」时这条用户消息库已经追加过了（endTurn 的本地收尾），
    // appendUser=false 避开两个一模一样的气泡
    if (appendUser)
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
    // 输入区两个下拉（webui sendMessage 同名字段）：
    //   selected_category + category_explicit —— 只有显式选过才允许覆盖会话 meta 里的分类，
    //     所以「全部」这种空值必须配 category_explicit=true，否则后端会回落到 meta
    //   kb_library —— 后端用空串表示「不使用知识库」，输入区下拉里那个值叫 __none__
    body.insert(QStringLiteral("selected_category"), m_category);
    body.insert(QStringLiteral("category_explicit"), m_categoryTouched);
    body.insert(QStringLiteral("kb_library"),
                m_kbLibrary == QLatin1String("__none__") ? QString() : m_kbLibrary);

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
        // 同一个事件里带着本轮请求实际送出的 input（含缓存读）与模型窗口
        applyContextUsage(event);
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
        // done 也带一份用量字段：token_usage 可能早退（本轮无工具调用），圆环不能停在旧值
        applyContextUsage(event);

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
        openSettings(rest);
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
    // live 模式下数据是异步拉回来的：立刻 openSettings 会被后续会话/配置加载覆盖掉
    // （看到的总是模型页），所以延一下再切 Tab
    auto openTab = [host, chart, live](const QString &name) {
        if (!live) {
            host->openSettings(name);
            return;
        }
        // live 下配置是异步拉回来的，早切会被 loadConfig 盖掉：
        // 连切几次，确保最终停在目标页（幂等，重复切无副作用）
        for (int delay : {900, 1800, 2800})
            QTimer::singleShot(delay, chart, [host, name] { host->openSettings(name); });
    };
    if (tab == QLatin1String("traj")) {
        if (!live)
            host->loadTrajectory();
        chart->setViewTab(QStringLiteral("traj"));
    } else if (tab == QLatin1String("traj-detail")) {
        // 轨迹页 + 选中第一条记录，用来核对右侧详情（尤其「原始内容」的高度）
        if (!live)
            host->loadTrajectory();
        chart->setViewTab(QStringLiteral("traj"));
        QTimer::singleShot(320, chart, [chart] {
            for (QWidget *w : chart->findChildren<QWidget *>()) {
                if (!w->property("class").toString().contains(QStringLiteral("trajRow")))
                    continue;
                const QPoint center(w->width() / 2, w->height() / 2);
                QMouseEvent press(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(w, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, center, Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(w, &release);
                break;
            }
        });
        // 再切到「原始内容」页签：这一页才是代码块高度按内容算的地方
        QTimer::singleShot(520, chart, [chart] {
            for (QAbstractButton *button : chart->findChildren<QAbstractButton *>()) {
                if (button->text() != QStringLiteral("原始内容"))
                    continue;
                button->click();
                break;
            }
        });
    } else if (tab == QLatin1String("approve")) {
        // 审批卡：核对浮层高度（参数多时应该在可视区内出滚动，而不是顶出窗口）
        host->loadHistory();
        QTimer::singleShot(260, chart, [host] {
            host->approveForShot();
        });
    } else if (tab == QLatin1String("usage")) {
        // 打开设置中心的「用量」页（与 webui 的第四个 Tab 对照）
        openTab(QStringLiteral("usage"));
    } else if (tab == QLatin1String("models") || tab == QLatin1String("skills")
               || tab == QLatin1String("mcp") || tab == QLatin1String("memory")
               || tab == QLatin1String("kb")) {
        // 设置中心其余 Tab（与 webui 的 tab-models / -skills / -mcp / -memory / -kb 对照）
        openTab(tab);
    } else if (tab == QLatin1String("model-form")) {
        // 模型行式表单：展开第一张模型卡片（离屏没有真实点击）
        openTab(QStringLiteral("models"));
        QTimer::singleShot(200, chart, [chart] {
            auto *dialog = chart->findChild<QWidget *>(QStringLiteral("settingsDialog"));
            if (!dialog)
                return;
            for (QWidget *w : dialog->findChildren<QWidget *>()) {
                if (!w->property("class").toString().contains(QStringLiteral("settingsModelSummary")))
                    continue;
                const QPoint center(w->width() / 2, w->height() / 2);
                QMouseEvent press(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(w, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, center, Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(w, &release);
                break;
            }
        });
    } else if (tab == QLatin1String("ring")) {
        // 上下文圆环浮层（离屏无法悬停：等价派发 Enter）
        QTimer::singleShot(200, chart, [chart] {
            if (QWidget *ring = chart->findChild<QWidget *>(QStringLiteral("contextRing"))) {
                QEvent enter(QEvent::Enter);
                QApplication::sendEvent(ring, &enter);
            }
        });
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
        host->openSettings(QStringLiteral("usage"));
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
        host->openSettings(QStringLiteral("usage"));
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
            if (!theme.isEmpty()) {
                chart->setTheme(theme); // 显示之后再套一次，确保离屏截图用的是目标皮肤
                // setStyleSheet 引发的 StyleChange 是异步投递的：不先跑一轮事件循环，
                // 子控件的「随皮肤重建局部样式」还没发生，截图会拍到旧配色
                QCoreApplication::processEvents();
                QThread::msleep(150);
                QCoreApplication::processEvents();
            }
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
            } else if (tab == QLatin1String("models") || tab == QLatin1String("model-form")
                       || tab == QLatin1String("skills") || tab == QLatin1String("mcp")) {
                if (QWidget *dialog =
                        chart->findChild<QWidget *>(QStringLiteral("settingsDialog"))) {
                    // 模型页的卡片在编辑区下方，截全图时拉高才能看到行式表单
                    if (tab == QLatin1String("models") || tab == QLatin1String("model-form"))
                        dialog->resize(dialog->width(), 1180);
                    target = dialog;
                }
            } else if (tab == QLatin1String("memory") || tab == QLatin1String("kb")) {
                // 记忆 / 知识库页比对话框高，拉高后再拍，把整个面板装进去
                if (QWidget *dialog =
                        chart->findChild<QWidget *>(QStringLiteral("settingsDialog"))) {
                    dialog->resize(dialog->width(), tab == QLatin1String("kb") ? 1180 : 1300);
                    target = dialog;
                }
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

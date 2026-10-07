// 一次性外观核对：把这轮同步的四处界面（模式下拉 / 斜杠面板 / 技能芯片 /
// 阶段面板圆点+统计串）抓成图，肉眼看一眼排版有没有塌陷。
// 用法：qt_visual.exe <输出目录>
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QTest>
#include <QTextEdit>
#include <QVariantList>
#include <QVariantMap>

#include "chartwidget.h"

using namespace gs;

// 下拉浮层是独立的 Qt::Popup 顶层窗口，chart.grab() 抓不到，
// 得把可见的顶层弹层贴回主图对应位置
static void shot(ChartWidget &chart, const QString &path)
{
    QWidget *popup = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w != &chart && w->isVisible() && w->windowFlags().testFlag(Qt::Popup)) {
            popup = w;
            break;
        }
    }
    if (popup) {
        const QPixmap popupShot = popup->grab();
        QPixmap base = chart.grab();
        QPainter painter(&base);
        const QPoint at = popup->mapToGlobal(QPoint(0, 0)) - chart.mapToGlobal(QPoint(0, 0));
        painter.drawPixmap(at, popupShot);
        painter.end();
        base.save(path);
        return;
    }
    chart.grab().save(path);
}

static QVariantList modelFixture()
{
    QVariantList models;
    models << QVariantMap{ { QStringLiteral("id"), QStringLiteral("gs-pro-32k") },
                           { QStringLiteral("provider"), QStringLiteral("gridstar") },
                           { QStringLiteral("provider_name"), QStringLiteral("GridStar 网关") },
                           { QStringLiteral("name"), QStringLiteral("GS-Pro 32K") },
                           { QStringLiteral("enabled"), true },
                           { QStringLiteral("status"), QStringLiteral("configured") } }
           << QVariantMap{ { QStringLiteral("id"), QStringLiteral("gs-flash-8k") },
                           { QStringLiteral("provider"), QStringLiteral("gridstar") },
                           { QStringLiteral("provider_name"), QStringLiteral("GridStar 网关") },
                           { QStringLiteral("name"), QStringLiteral("GS-Flash 8K") },
                           { QStringLiteral("enabled"), true },
                           { QStringLiteral("status"), QStringLiteral("configured") } }
           << QVariantMap{ { QStringLiteral("id"), QStringLiteral("qwen2.5-72b") },
                           { QStringLiteral("provider"), QStringLiteral("ollama") },
                           { QStringLiteral("provider_name"), QStringLiteral("Ollama") },
                           { QStringLiteral("name"), QStringLiteral("Qwen2.5 72B") },
                           { QStringLiteral("enabled"), true },
                           { QStringLiteral("status"), QStringLiteral("discovered") } };
    return models;
}

static QVariantList skillFixture()
{
    QVariantList skills;
    skills << QVariantMap{ { QStringLiteral("id"), QStringLiteral("cfd-workflow") },
                           { QStringLiteral("name"), QStringLiteral("CFD 工作流") },
                           { QStringLiteral("description"),
                             QStringLiteral("按网格生成→求解→后处理的顺序推进，每步先确认参数") } }
           << QVariantMap{ { QStringLiteral("id"), QStringLiteral("mesh-check") },
                           { QStringLiteral("name"), QStringLiteral("网格质量校核") },
                           { QStringLiteral("description"),
                             QStringLiteral("检查正交性、长宽比与负体积，给出可执行的加密建议") } };
    return skills;
}

static QVariantMap phase(const QString &title, const QString &status, const QString &note = QString())
{
    QVariantMap step;
    step.insert(QStringLiteral("title"), title);
    step.insert(QStringLiteral("status"), status);
    if (!note.isEmpty())
        step.insert(QStringLiteral("note"), note);
    return step;
}

static void dumpWidget(QWidget *w, const char *name)
{
    QString text;
    if (auto *label = qobject_cast<QLabel *>(w))
        text = label->text();
    else if (auto *button = qobject_cast<QPushButton *>(w))
        text = button->text();
    qInfo("%s visible=%d geo=%d,%d %dx%d minHint=%dx%d hint=%dx%d text=%s", name, w->isVisible(),
          w->x(), w->y(), w->width(), w->height(), w->minimumSizeHint().width(),
          w->minimumSizeHint().height(), w->sizeHint().width(), w->sizeHint().height(),
          qPrintable(text));
}

int main(int argc, char *argv[])
{
    qInfo("A: entering main");
    QApplication app(argc, argv);
    qInfo("B: QApplication up, qApp=%p", static_cast<void *>(qApp));
    const QString outDir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QDir().mkpath(outDir);

    ChartWidget chart;
    qInfo("C: ChartWidget constructed");
    chart.setConfigLoaded(true);
    qInfo("D: configLoaded");
    chart.setTheme(QStringLiteral("dark"));
    qInfo("E: theme");
    chart.resize(1100, 860);
    chart.setConnectionState(QStringLiteral("connected"), QStringLiteral("已连接"));
    chart.setSessions(QVariantList{
        QVariantMap{ { QStringLiteral("id"), QStringLiteral("s-1") },
                     { QStringLiteral("title"), QStringLiteral("翼型绕流网格收敛性排查") },
                     { QStringLiteral("status"), QStringLiteral("running") } },
        QVariantMap{ { QStringLiteral("id"), QStringLiteral("s-2") },
                     { QStringLiteral("title"), QStringLiteral("圆柱绕流涡街后处理") },
                     { QStringLiteral("status"), QStringLiteral("done") } } });
    chart.setCurrentSessionTitle(QStringLiteral("翼型绕流网格收敛性排查"));
    chart.setModels(modelFixture());
    chart.setSkills(skillFixture());
    chart.setCurrentModel(QStringLiteral("gridstar/gs-pro-32k"));
    chart.setMode(QStringLiteral("manual"));

    chart.setHistory(QVariantList{
        QVariantMap{ { QStringLiteral("role"), QStringLiteral("user") },
                     { QStringLiteral("content"),
                       QStringLiteral("这版网格在 Re=1e6 下残差降到 1e-4 就不再下降，帮我看下") } },
        QVariantMap{ { QStringLiteral("role"), QStringLiteral("assistant") },
                     { QStringLiteral("content"),
                       QStringLiteral("残差平台通常来自网格尺度与湍流模型不匹配。先做一次网格无关性检查：\n\n"
                                      "1. 把 y+ 控制在 1 附近；\n2. 加密前缘 20% 弦长区域；\n"
                                      "3. 对比三套网格的升力系数。") } } });

    // 未收尾的计划：圆点指示器 + 统计串
    QVariantList steps;
    steps << phase(QStringLiteral("读取几何与边界条件"), QStringLiteral("done"))
          << phase(QStringLiteral("生成初始网格"), QStringLiteral("done"))
          << phase(QStringLiteral("加密前缘并重划"), QStringLiteral("running"),
                   QStringLiteral("当前 2.4 万单元"))
          << phase(QStringLiteral("求解至残差 1e-5"), QStringLiteral("pending"))
          << phase(QStringLiteral("整理后处理图表"), QStringLiteral("pending"));
    QVariantMap plan;
    plan.insert(QStringLiteral("title"), QStringLiteral("网格收敛性排查"));
    plan.insert(QStringLiteral("phases"), steps);
    chart.setPhasePlan(plan);

    // 技能芯片：先选技能，让左控件行一次成型
    chart.setCurrentSkill(QStringLiteral("cfd-workflow"));
    qInfo("after setCurrentSkill: currentSkill=%s", qPrintable(chart.currentSkill()));
    if (auto *w = chart.findChild<QLabel *>(QStringLiteral("skillChipName")))
        qInfo("skillChipName text=%s visible=%d", qPrintable(w->text()), w->isVisible());

    chart.show();
    QTest::qWait(300);
    for (const char *name : { "skillChip", "skillChipName", "skillChipClose", "modeTrigger",
                              "modelTrigger", "phaseCount" }) {
        if (auto *w = chart.findChild<QWidget *>(QString::fromLatin1(name)))
            dumpWidget(w, name);
        else
            qInfo("%s MISSING", name);
    }
    if (auto *name = chart.findChild<QLabel *>(QStringLiteral("skillChipName")))
        qInfo("skillChipName text=[%s] len=%d", qPrintable(name->text()), name->text().size());
    shot(chart, outDir + QStringLiteral("/01_chat_phase.png"));

    // 展开态：圆点指示器 + 连接线
    if (auto *head = chart.findChild<QWidget *>(QStringLiteral("phaseHead"))) {
        QTest::mouseClick(head, Qt::LeftButton);
        QTest::qWait(250);
        shot(chart, outDir + QStringLiteral("/06_phase_expanded.png"));
        if (auto *panel = chart.findChild<QWidget *>(QStringLiteral("phasePanel")))
            qInfo("expanded phasePanel %dx%d", panel->width(), panel->height());
        QTest::mouseClick(head, Qt::LeftButton);
        QTest::qWait(200);
    }

    auto *modeTrigger = chart.findChild<QWidget *>(QStringLiteral("modeTrigger"));
    Q_ASSERT(modeTrigger);
    QTest::mouseClick(modeTrigger, Qt::LeftButton);
    QTest::qWait(250);
    shot(chart, outDir + QStringLiteral("/02_mode_list.png"));
    QTest::keyClick(&chart, Qt::Key_Escape);
    QTest::qWait(150);

    // 斜杠面板（根：指令 + 技能）
    chart.setInputText(QStringLiteral("/"));
    QTest::qWait(250);
    shot(chart, outDir + QStringLiteral("/03_slash_root.png"));
    QTest::qWait(10);

    // 技能芯片单独来一张
    chart.setInputText(QString());
    QTest::qWait(250);
    shot(chart, outDir + QStringLiteral("/05_skill_chip.png"));

    return 0;
}
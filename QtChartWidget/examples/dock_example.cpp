#include <chartwidget.h>

#include <QApplication>
#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// 最小 QDockWidget 宿主：把 gs::ChartWidget 放进一个可停靠面板里，
// 旁边放一个普通工作区，用来核对「嵌入已有主窗口」这条路径。
//
// 运行：
//   dock_example.exe             交互窗口
//   dock_example.exe --shot a.png  离屏截图（不弹窗）
//   dock_example.exe --float       启动后把 Dock 浮起来（核对浮动后的浮层定位）
int main(int argc, char *argv[])
{
    QString shotPath;
    bool floatDock = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--shot") && i + 1 < argc)
            shotPath = QString::fromLocal8Bit(argv[++i]);
        else if (arg == QLatin1String("--float"))
            floatDock = true;
    }
    if (!shotPath.isEmpty() && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);

    QMainWindow window;
    window.resize(1180, 820);
    window.setWindowTitle(QStringLiteral("GridStar Agent · Dock 嵌入验证"));

    auto *workspace = new QWidget(&window);
    auto *workspaceLayout = new QVBoxLayout(workspace);
    auto *hint = new QLabel(QStringLiteral("宿主工作区（这里放你自己的画布 / 表格 / 编辑器）"), workspace);
    hint->setAlignment(Qt::AlignCenter);
    workspaceLayout->addWidget(hint);
    auto *editor = new QTextEdit(workspace);
    editor->setPlainText(QStringLiteral("宿主的普通控件放在这里。\n\n"
                                        "验证点：\n"
                                        "1. Dock 宽度小于 420 时会不会把布局顶开\n"
                                        "2. Dock 浮动后下拉 / 审批卡的定位是否仍然正确\n"
                                        "3. 设置中心弹窗是否盖在 Dock 上而不是跑到屏幕别处\n"
                                        "4. Ctrl+= 缩放是否只在 Dock 内生效"));
    workspaceLayout->addWidget(editor, 1);
    window.setCentralWidget(workspace);

    auto *dock = new QDockWidget(QStringLiteral("GridStar Agent"), &window);
    dock->setObjectName(QStringLiteral("agentDock"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    auto *chart = new gs::ChartWidget(dock);
    dock->setWidget(chart);
    window.addDockWidget(Qt::LeftDockWidgetArea, dock);
    dock->resize(470, 780);

    // 一点演示数据，保证嵌入后能看到真实排版而不是空态
    QVariantMap model;
    model.insert(QStringLiteral("provider"), QStringLiteral("gridstar"));
    model.insert(QStringLiteral("id"), QStringLiteral("gs-pro-32k"));
    model.insert(QStringLiteral("name"), QStringLiteral("GS-Pro 32K"));
    model.insert(QStringLiteral("enabled"), true);
    chart->setModels(QVariantList{ model });
    chart->setCurrentModel(QStringLiteral("gridstar/gs-pro-32k"));
    chart->setConnectionState(QStringLiteral("online"), QStringLiteral("已连接"));
    chart->setConfigLoaded(true);
    chart->setCurrentSessionTitle(QStringLiteral("Dock 嵌入 · 会话标题"));

    window.show();
    if (floatDock)
        dock->setFloating(true);

    if (!shotPath.isEmpty()) {
        QTimer::singleShot(900, &app, [&window, dock, shotPath, floatDock] {
            // 浮动时 Dock 是独立窗口，抓主窗口拍不到，改抓 Dock 本身
            QWidget *target = floatDock ? static_cast<QWidget *>(dock) : static_cast<QWidget *>(&window);
            const QPixmap shot = target->grab();
            if (shot.isNull() || !shot.save(shotPath))
                qCritical("截图保存失败");
            qApp->quit();
        });
        return app.exec();
    }

    // 交互模式下给个提示，避免用户以为卡住了
    QTimer::singleShot(300, chart, [chart] {
        chart->showToast(QStringLiteral("Dock 嵌入验证：拖一下分隔条看看布局"));
    });
    return app.exec();
}
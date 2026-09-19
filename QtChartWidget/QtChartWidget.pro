TEMPLATE = lib
TARGET   = QtChartWidget
CONFIG  += c++17
CONFIG  -= app_bundle
QT      += core gui widgets svg

# 版本号：写进 DLL 的版本资源（资源管理器可见）。
# 改这里时同步 src/chartwidget.cpp 的 qtchartwidget_version() 与 CHANGELOG.md
VERSION = 2.0.0
# Windows 下 qmake 默认会把主版本号拼进产物名（QtChartWidget2.dll），
# 但 demo / examples / tests 与部署脚本都按 QtChartWidget.lib 引用，所以关掉这个后缀
CONFIG += skip_target_version_ext

DEFINES += QTCHARTWIDGET_LIBRARY
msvc: QMAKE_CXXFLAGS += /utf-8

RESOURCES += resources/icons.qrc

DESTDIR     = $$PWD/bin
OBJECTS_DIR = $$PWD/build/obj
MOC_DIR     = $$PWD/build/moc
RCC_DIR     = $$PWD/build/rcc
UI_DIR      = $$PWD/build/ui

INCLUDEPATH += $$PWD/include $$PWD/src

HEADERS += \
    include/qtchartwidget_global.h \
    include/chartwidget.h \
    src/theme.h \
    src/commonwidgets.h \
    src/markdownview.h \
    src/messagewidgets.h \
    src/choiceoverlay.h \
    src/trajectoryview.h \
    src/phasepanel.h \
    src/popups.h \
    src/composer.h \
    src/settingsdialog.h

SOURCES += \
    src/theme.cpp \
    src/commonwidgets.cpp \
    src/markdownview.cpp \
    src/messagewidgets.cpp \
    src/choiceoverlay.cpp \
    src/trajectoryview.cpp \
    src/phasepanel.cpp \
    src/popups.cpp \
    src/composer.cpp \
    src/settingsdialog.cpp \
    src/chartwidget.cpp

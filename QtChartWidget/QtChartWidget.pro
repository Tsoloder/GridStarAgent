TEMPLATE = lib
TARGET   = QtChartWidget
CONFIG  += c++17
CONFIG  -= app_bundle
QT      += core gui widgets svg

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
    src/phasepanel.h \
    src/popups.h \
    src/composer.h \
    src/settingsdialog.h

SOURCES += \
    src/theme.cpp \
    src/commonwidgets.cpp \
    src/markdownview.cpp \
    src/messagewidgets.cpp \
    src/phasepanel.cpp \
    src/popups.cpp \
    src/composer.cpp \
    src/settingsdialog.cpp \
    src/chartwidget.cpp

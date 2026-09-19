TEMPLATE = app
TARGET   = qtchartwidget_tests
CONFIG  += c++17 console
CONFIG  -= app_bundle
QT      += core gui widgets testlib svg

msvc: QMAKE_CXXFLAGS += /utf-8

DESTDIR     = $$PWD/../bin
OBJECTS_DIR = $$PWD/../build/tests/obj
MOC_DIR     = $$PWD/../build/tests/moc

INCLUDEPATH += $$PWD/../include
LIBS        += -L$$PWD/../bin -lQtChartWidget

SOURCES += test_chartwidget.cpp
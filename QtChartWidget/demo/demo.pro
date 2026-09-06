TEMPLATE = app
TARGET   = demo
CONFIG  += c++17 console
CONFIG  -= app_bundle
QT      += core gui widgets

msvc: QMAKE_CXXFLAGS += /utf-8

DESTDIR     = $$PWD/../bin
OBJECTS_DIR = $$PWD/../build/demo-obj
MOC_DIR     = $$PWD/../build/demo-moc
RCC_DIR     = $$PWD/../build/demo-rcc

INCLUDEPATH += $$PWD/../include
DEPENDPATH  += $$PWD/../include
LIBS        += -L$$PWD/../bin -lQtChartWidget

SOURCES += main.cpp

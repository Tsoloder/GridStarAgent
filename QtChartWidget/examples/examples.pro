TEMPLATE = app
TARGET   = host_example
CONFIG  += c++17 console
QT      += core gui widgets

msvc: QMAKE_CXXFLAGS += /utf-8

DESTDIR     = $$PWD/../bin
OBJECTS_DIR = $$PWD/../build/example-obj
MOC_DIR     = $$PWD/../build/example-moc

INCLUDEPATH += $$PWD/../include
DEPENDPATH  += $$PWD/../include

LIBS += -L$$PWD/../bin -lQtChartWidget

SOURCES += host_example.cpp

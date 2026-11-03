QT += core gui widgets svg
CONFIG += c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = dock_example

msvc: QMAKE_CXXFLAGS += /utf-8

INCLUDEPATH += $$PWD/../include
LIBS += -L$$PWD/../bin -lQtChartWidget
DESTDIR = $$PWD/../bin
OBJECTS_DIR = $$PWD/../build/dock
MOC_DIR = $$PWD/../build/dock

SOURCES += dock_example.cpp
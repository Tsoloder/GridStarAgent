#ifndef QTCHARTWIDGET_GLOBAL_H
#define QTCHARTWIDGET_GLOBAL_H

#include <QtCore/qglobal.h>

#if defined(QTCHARTWIDGET_LIBRARY)
#  define QTCHARTWIDGET_EXPORT Q_DECL_EXPORT
#else
#  define QTCHARTWIDGET_EXPORT Q_DECL_IMPORT
#endif

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

// C 风格工厂：宿主程序（甚至非 Qt 语言绑定）可直接创建一个 UI 面板
extern "C" QTCHARTWIDGET_EXPORT QWidget *qtchartwidget_create();
extern "C" QTCHARTWIDGET_EXPORT const char *qtchartwidget_version();

#endif // QTCHARTWIDGET_GLOBAL_H

#ifndef GS_THEME_H
#define GS_THEME_H

#include <QColor>
#include <QString>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

// 主题：与 webui/style.css 的 :root 变量一一对应
namespace gs {

extern const QColor Bg;          // --bg
extern const QColor Surface;     // --surface
extern const QColor Surface2;    // --surface-2
extern const QColor Surface3;    // --surface-3
extern const QColor Line;        // --line
extern const QColor LineStrong;  // --line-strong
extern const QColor Text;        // --text
extern const QColor Muted;       // --muted
extern const QColor Cyan;        // --cyan
extern const QColor CyanDark;    // --cyan-dark
extern const QColor Green;       // --green
extern const QColor Orange;      // --orange
extern const QColor Red;         // --red

// 字体族：CSS 中是 "Bahnschrift","Microsoft YaHei UI",sans-serif
QString uiFont();
QString monoFont();
int basePixelSize();

// 界面缩放（VSCode 式快捷键 Ctrl+= / Ctrl+- / Ctrl+0）。
// 所有字号（QSS 里的 font-size 与代码中的硬编码 px）统一经 zoomFactor 缩放。
qreal zoomFactor();
void setZoomFactor(qreal factor); // 内部会夹取到 [0.5, 2.5]
int scaledPx(int basePx);

// 全局样式表（由 style.css 转写）
QString appStyleSheet();

// 动态属性（state/role/active…）变更后必须 unpolish+polish 才会重新匹配 QSS
void restyle(QWidget *w);

QString escapeHtml(const QString &text);

} // namespace gs

#endif // GS_THEME_H

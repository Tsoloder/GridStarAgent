#ifndef GS_THEME_H
#define GS_THEME_H

#include <QColor>
#include <QString>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

// 主题：与 webui/style.css 的三套 :root 变量一一对应（dark / silver / blue）
namespace gs {

// CSS 自定义属性的 Qt 侧映射；字段名与 style.css 的 --kebab-case 一一对应
struct Palette
{
    QColor bg;                 // --bg
    QColor surface;            // --surface
    QColor surface2;           // --surface-2
    QColor surface3;           // --surface-3
    QColor line;               // --line
    QColor lineStrong;         // --line-strong
    QColor text;               // --text
    QColor muted;              // --muted
    QColor muted2;             // --muted-2
    QColor textBright;         // --text-bright
    QColor cyan;               // --cyan
    QColor cyanDark;           // --cyan-dark
    QColor cyanMid;            // --cyan-mid
    QColor cyanGlow;           // --cyan-glow
    QColor green;              // --green
    QColor orange;             // --orange
    QColor red;                // --red
    QColor bgGradA;            // --bg-grad-a
    QColor band1;              // --band-1
    QColor band2;              // --band-2
    QColor band3;              // --band-3
    QColor panel;              // --panel
    QColor inset;              // --inset
    QColor bubble;             // --bubble
    QColor bubbleUser;         // --bubble-user
    QColor accentTint;         // --accent-tint
    QColor okLine;             // --ok-line
    QColor okTint;             // --ok-tint
    QColor okText;             // --ok-text
    QColor errLine;            // --err-line
    QColor errTint;            // --err-tint
    QColor errStrong;          // --err-strong
    QColor errText;            // --err-text
    QColor warnLine;           // --warn-line
    QColor warnTint;           // --warn-tint
    QColor warnText;           // --warn-text
    QColor warnGlow;           // --warn-glow
    QColor scrollThumb;        // --scroll-thumb
    QColor scrollThumbHover;   // --scroll-thumb-hover
    QColor scrollTrack;        // --scroll-track
    QColor scrim;              // --scrim
    QColor scrim2;             // --scrim-2
    QColor glassBg;            // --glass-bg
    QColor glassBg2;           // --glass-bg-2
    QColor cardGrad1;          // --card-grad-1
    QColor cardGrad2;          // --card-grad-2
    QColor cardUserGrad1;      // --card-user-grad-1
    QColor cardUserGrad2;      // --card-user-grad-2
    QColor cardShadow;         // --card-shadow
    QColor assistantLine;      // --assistant-line
    QColor assistantTint;      // --assistant-tint
    QColor assistantText;      // --assistant-text
    bool dark = true;          // color-scheme: dark
};

// "dark" / "silver" / "blue"
const Palette &palette();
QString themeId();
// 深色 / 银白 / 蔚蓝
QString themeName(const QString &id = QString());

// 切换皮肤（未知 id 回退 dark）。切换后需重设 QSS 并刷新自绘控件。
void setTheme(const QString &id);
// 全部皮肤 id（下拉列表用）
QStringList themeIds();

// 字体族：CSS 中是 "Bahnschrift","Microsoft YaHei UI",sans-serif
QString uiFont();
QString monoFont();
int basePixelSize();

// 界面缩放（VSCode 式快捷键 Ctrl+= / Ctrl+- / Ctrl+0）。
// 所有字号（QSS 里的 font-size 与代码中的硬编码 px）统一经 zoomFactor 缩放。
qreal zoomFactor();
void setZoomFactor(qreal factor); // 内部会夹取到 [0.5, 2.5]
int scaledPx(int basePx);

// 全局样式表（由 style.css 转写，按当前调色板展开）
QString appStyleSheet();

// 动态属性（state/role/active…）变更后必须 unpolish+polish 才会重新匹配 QSS
void restyle(QWidget *w);

// QSS 不支持 letter-spacing：按 webui style.css 里写了 letter-spacing 的选择器，
// 在控件 polish 时用 QFont::setLetterSpacing 补上（幂等；随缩放系数重算）。
void applyLetterSpacing(QWidget *widget);

// webui 里的交互控件都是原生 button / 带 tabindex 的元素：这里在控件 polish 时把按钮
// 收敛成 TabFocus（键盘可达，且不像 StrongFocus 那样点完留焦点环），
// 并把 toolTip 兜底成无障碍名称（图标按钮没有文本）
void applyAccessibility(QWidget *widget);

QString escapeHtml(const QString &text);
// QColor → QSS 颜色字面量（带透明度时输出 rgba()）
QString cssColor(const QColor &color);

} // namespace gs

#endif // GS_THEME_H
#include "theme.h"

#include <QAbstractButton>
#include <QFontDatabase>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QStyle>
#include <QWidget>

namespace gs {

namespace {

// 三套皮肤：逐项对应 webui/refined/tokens.css 的 :root[data-theme=...] 变量表
// （精修层把原来的蓝灰堆叠换成了中性冷黑，边界一律用低透明描边，不再画实色线）
Palette makeDark()
{
    Palette p;
    p.dark = true;
    p.bg = QColor("#0d1014");
    p.surface = QColor("#14181e");
    p.surface2 = QColor("#181d24");
    p.surface3 = QColor("#1e242c");
    p.line = QColor(255, 255, 255, 14);        // rgba(255,255,255,.055)
    p.lineStrong = QColor(255, 255, 255, 26);  // rgba(255,255,255,.1)
    p.text = QColor("#cfd7de");
    p.muted = QColor("#8a95a1");
    p.muted2 = QColor("#67717d");
    p.textBright = QColor("#f1f5f8");
    p.cyan = QColor("#6fb0cc");
    p.cyanDark = QColor("#1c4a5d");
    p.cyanMid = QColor("#4a8aa6");
    p.cyanGlow = QColor(111, 176, 204, 56);    // rgba(111,176,204,.22)
    p.green = QColor("#5fbf94");
    p.orange = QColor("#d9a75f");
    p.red = QColor("#d97b7b");
    p.bgGradA = QColor("#101419");
    p.band1 = QColor("#10141a");
    p.band2 = QColor("#0e1217");
    p.band3 = QColor("#0f1318");
    p.panel = QColor("#11151b");
    p.inset = QColor("#0a0d11");
    p.bubble = QColor("#15191f");
    p.bubbleUser = QColor("#17242c");
    p.accentTint = QColor(111, 176, 204, 26);  // rgba(111,176,204,.1)
    p.okLine = QColor(95, 191, 148, 77);       // rgba(95,191,148,.3)
    p.okTint = QColor(95, 191, 148, 26);
    p.okText = QColor("#7fd0ac");
    p.errLine = QColor(217, 123, 123, 77);     // rgba(217,123,123,.3)
    p.errTint = QColor(217, 123, 123, 26);
    p.errStrong = QColor("#b85c5c");
    p.errText = QColor("#f0b4b4");
    p.warnLine = QColor(217, 167, 95, 77);     // rgba(217,167,95,.3)
    p.warnTint = QColor(217, 167, 95, 26);
    p.warnText = QColor("#e6c68f");
    p.warnGlow = QColor(217, 167, 95, 77);
    p.scrollThumb = QColor("#2a333c");
    p.scrollThumbHover = QColor("#3a4650");
    p.scrollTrack = QColor(0, 0, 0, 0);        // --scroll-track: transparent
    p.scrim = QColor(6, 9, 12, 209);           // rgba(6,9,12,.82)
    p.scrim2 = QColor(13, 16, 20, 230);        // rgba(13,16,20,.9)
    p.glassBg = QColor(16, 20, 26, 184);       // rgba(16,20,26,.72)
    p.glassBg2 = QColor(13, 16, 20, 214);      // rgba(13,16,20,.84)
    p.cardGrad1 = QColor(28, 36, 45, 179);     // rgba(28,36,45,.7)
    p.cardGrad2 = QColor(18, 23, 29, 209);     // rgba(18,23,29,.82)
    p.cardUserGrad1 = QColor(29, 47, 57, 173); // rgba(29,47,57,.68)
    p.cardUserGrad2 = QColor(20, 33, 40, 209); // rgba(20,33,40,.82)
    p.cardShadow = QColor(0, 0, 0, 97);        // rgba(0,0,0,.38)
    p.assistantLine = QColor(255, 255, 255, 18);// rgba(255,255,255,.07)
    p.assistantTint = QColor(255, 255, 255, 8);// rgba(255,255,255,.03)
    p.assistantText = QColor("#8a95a1");
    p.bodyText = QColor("#d9e0e7");
    p.cardSolid = QColor("#12161b");
    p.cardSolid2 = QColor("#14181e");
    p.inputBg = QColor(16, 20, 26, 240);       // rgba(16,20,26,.94)
    p.glassSolid = QColor(19, 23, 29, 240);    // rgba(19,23,29,.94)
    p.codeText = QColor("#a8d4e6");
    p.hairline = QColor(255, 255, 255, 11);    // rgba(255,255,255,.045)
    p.hoverTint = QColor(255, 255, 255, 13);   // rgba(255,255,255,.05)
    return p;
}

Palette makeSilver()
{
    Palette p;
    p.dark = false;
    p.bg = QColor("#f6f7f9");
    p.surface = QColor("#ffffff");
    p.surface2 = QColor("#f2f4f6");
    p.surface3 = QColor("#e9ecef");
    p.line = QColor(16, 24, 32, 18);           // rgba(16,24,32,.07)
    p.lineStrong = QColor(16, 24, 32, 33);     // rgba(16,24,32,.13)
    p.text = QColor("#333c45");
    p.muted = QColor("#667180");
    p.muted2 = QColor("#88929e");
    p.textBright = QColor("#0c1116");
    p.cyan = QColor("#22789b");
    p.cyanDark = QColor("#14607d");
    p.cyanMid = QColor("#3590b3");
    p.cyanGlow = QColor(34, 120, 155, 46);     // rgba(34,120,155,.18)
    p.green = QColor("#177a4c");
    p.orange = QColor("#96660f");
    p.red = QColor("#b23a3a");
    p.bgGradA = QColor("#fbfcfd");
    p.band1 = QColor("#ffffff");
    p.band2 = QColor("#f7f8fa");
    p.band3 = QColor("#f2f4f6");
    p.panel = QColor("#ffffff");
    p.inset = QColor("#ffffff");
    p.bubble = QColor("#ffffff");
    p.bubbleUser = QColor("#eef5f9");
    p.accentTint = QColor(34, 120, 155, 20);   // rgba(34,120,155,.08)
    p.okLine = QColor("#9ccdb4");
    p.okTint = QColor("#e1f2e8");
    p.okText = QColor("#177a4c");
    p.errLine = QColor("#d8a9a9");
    p.errTint = QColor("#f8e7e7");
    p.errStrong = QColor("#c05555");
    p.errText = QColor("#a03440");
    p.warnLine = QColor("#d3b578");
    p.warnTint = QColor("#f7eeda");
    p.warnText = QColor("#8a5c14");
    p.warnGlow = QColor(150, 102, 15, 85);
    p.scrollThumb = QColor("#ccd3da");
    p.scrollThumbHover = QColor("#b3bcc6");
    p.scrollTrack = QColor(0, 0, 0, 0);
    p.scrim = QColor(24, 30, 36, 97);          // rgba(24,30,36,.38)
    p.scrim2 = QColor(246, 247, 249, 237);     // rgba(246,247,249,.93)
    p.glassBg = QColor(252, 253, 254, 194);    // rgba(252,253,254,.76)
    p.glassBg2 = QColor(255, 255, 255, 219);   // rgba(255,255,255,.86)
    p.cardGrad1 = QColor(255, 255, 255, 242);  // rgba(255,255,255,.95)
    p.cardGrad2 = QColor(240, 244, 248, 230);  // rgba(240,244,248,.9)
    p.cardUserGrad1 = QColor(233, 244, 250, 242);
    p.cardUserGrad2 = QColor(219, 235, 243, 230);
    p.cardShadow = QColor(20, 32, 45, 26);     // rgba(20,32,45,.1)
    p.assistantLine = QColor(16, 24, 32, 20);  // rgba(16,24,32,.08)
    p.assistantTint = QColor(16, 24, 32, 5);   // rgba(16,24,32,.02)
    p.assistantText = QColor("#667180");
    p.bodyText = QColor("#2f3740");
    p.cardSolid = QColor("#ffffff");
    p.cardSolid2 = QColor("#ffffff");
    p.inputBg = QColor("#ffffff");
    p.glassSolid = QColor(255, 255, 255, 247); // rgba(255,255,255,.97)
    p.codeText = QColor("#1a6b8a");
    p.hairline = QColor(16, 24, 32, 13);       // rgba(16,24,32,.05)
    p.hoverTint = QColor(16, 24, 32, 13);
    return p;
}

Palette makeBlue()
{
    Palette p;
    p.dark = false;
    p.bg = QColor("#eef3f8");
    p.surface = QColor("#fbfdff");
    p.surface2 = QColor("#eaf1f7");
    p.surface3 = QColor("#dfe9f2");
    p.line = QColor(18, 46, 74, 20);           // rgba(18,46,74,.08)
    p.lineStrong = QColor(18, 46, 74, 38);     // rgba(18,46,74,.15)
    p.text = QColor("#2d4058");
    p.muted = QColor("#5c7189");
    p.muted2 = QColor("#7f92a6");
    p.textBright = QColor("#0d2136");
    p.cyan = QColor("#22789b");
    p.cyanDark = QColor("#14607d");
    p.cyanMid = QColor("#3590b3");
    p.cyanGlow = QColor(34, 120, 155, 46);
    p.green = QColor("#1c7a50");
    p.orange = QColor("#96660f");
    p.red = QColor("#b23a3a");
    p.bgGradA = QColor("#f4f9fd");
    p.band1 = QColor("#f8fbfe");
    p.band2 = QColor("#eef4fa");
    p.band3 = QColor("#e7eff7");
    p.panel = QColor("#fbfdff");
    p.inset = QColor("#fdfeff");
    p.bubble = QColor("#fbfdff");
    p.bubbleUser = QColor("#e3eefa");
    p.accentTint = QColor(34, 120, 155, 20);
    p.okLine = QColor("#9ccdb4");
    p.okTint = QColor("#dff0e6");
    p.okText = QColor("#1c7a50");
    p.errLine = QColor("#d5a9ae");
    p.errTint = QColor("#f6e6e8");
    p.errStrong = QColor("#bf5560");
    p.errText = QColor("#a03440");
    p.warnLine = QColor("#d0b57e");
    p.warnTint = QColor("#f5edda");
    p.warnText = QColor("#8a5c14");
    p.warnGlow = QColor(150, 102, 15, 85);
    p.scrollThumb = QColor("#b9c8d8");
    p.scrollThumbHover = QColor("#9fb2c6");
    p.scrollTrack = QColor(0, 0, 0, 0);
    p.scrim = QColor(20, 40, 62, 97);          // rgba(20,40,62,.38)
    p.scrim2 = QColor(238, 243, 248, 237);     // rgba(238,243,248,.93)
    p.glassBg = QColor(244, 249, 253, 194);    // rgba(244,249,253,.76)
    p.glassBg2 = QColor(251, 253, 255, 219);   // rgba(251,253,255,.86)
    p.cardGrad1 = QColor(252, 254, 255, 242);
    p.cardGrad2 = QColor(230, 240, 249, 230);
    p.cardUserGrad1 = QColor(219, 234, 248, 242);
    p.cardUserGrad2 = QColor(203, 224, 242, 230);
    p.cardShadow = QColor(20, 40, 62, 31);     // rgba(20,40,62,.12)
    p.assistantLine = QColor(18, 46, 74, 20);
    p.assistantTint = QColor(18, 46, 74, 5);
    p.assistantText = QColor("#5c7189");
    p.bodyText = QColor("#2b3a4c");
    p.cardSolid = QColor("#ffffff");
    p.cardSolid2 = QColor("#ffffff");
    p.inputBg = QColor("#ffffff");
    p.glassSolid = QColor(251, 253, 255, 247);
    p.codeText = QColor("#1a6b8a");
    p.hairline = QColor(18, 46, 74, 13);
    p.hoverTint = QColor(18, 46, 74, 13);
    return p;
}

struct ThemeEntry
{
    const char *id;
    const char *name;
    Palette (*make)();
};

const ThemeEntry kThemes[] = {
    { "dark", "深色", &makeDark },
    { "silver", "银白", &makeSilver },
    { "blue", "蔚蓝", &makeBlue },
};
const int kThemeCount = int(sizeof(kThemes) / sizeof(kThemes[0]));

qreal g_zoomFactor = 1.0;
QString g_themeId = QStringLiteral("dark");
Palette g_palette = makeDark();

} // namespace

const Palette &palette() { return g_palette; }

QString themeId() { return g_themeId; }

QString themeName(const QString &id)
{
    const QString key = id.isEmpty() ? g_themeId : id;
    for (int i = 0; i < kThemeCount; ++i) {
        if (key == QLatin1String(kThemes[i].id))
            return QString::fromUtf8(kThemes[i].name);
    }
    return QString::fromUtf8(kThemes[0].name);
}

QStringList themeIds()
{
    QStringList ids;
    for (int i = 0; i < kThemeCount; ++i)
        ids << QString::fromLatin1(kThemes[i].id);
    return ids;
}

void setTheme(const QString &id)
{
    g_themeId = QStringLiteral("dark");
    for (int i = 0; i < kThemeCount; ++i) {
        if (id == QLatin1String(kThemes[i].id)) {
            g_themeId = id;
            g_palette = kThemes[i].make();
            return;
        }
    }
    g_palette = makeDark();
}

int basePixelSize() { return 13; }

qreal zoomFactor() { return g_zoomFactor; }

void setZoomFactor(qreal factor)
{
    g_zoomFactor = qBound(0.5, factor, 2.5);
}

int scaledPx(int basePx)
{
    return qMax(1, qRound(basePx * g_zoomFactor));
}

QString uiFont()
{
    static QString cached;
    if (cached.isEmpty()) {
        // 与 webui/style.css 的 :root 保持一致（那里也是 "Microsoft YaHei" 优先）。
        // 为什么不能用 Bahnschrift 当主族：中文会回退到雅黑渲染，但 QLabel 的高度是按
        // 主字体的 fontMetrics() 算的。Bahnschrift 的 ascent/em 只有 0.794，雅黑是 1.058，
        // 12px 下相差 3.2px：标签框按 9.5px 留上方，中文却要 12.7px，于是字顶被裁。
        // webui 在浏览器里没这个问题，因为 CSS 靠 line-height 撑行盒，与字体 metrics 无关。
        const QStringList want{QStringLiteral("Microsoft YaHei"),
                               QStringLiteral("Microsoft YaHei UI"),
                               QStringLiteral("Segoe UI")};
        const QFontDatabase db;
        const QStringList have = db.families();
        for (const QString &family : want) {
            if (have.contains(family)) {
                cached = family;
                break;
            }
        }
        if (cached.isEmpty())
            cached = QStringLiteral("Microsoft YaHei");
    }
    return cached;
}

QString monoFont()
{
    static QString cached;
    if (cached.isEmpty()) {
        const QStringList want{QStringLiteral("Consolas"),
                               QStringLiteral("Cascadia Mono"),
                               QStringLiteral("Courier New")};
        const QFontDatabase db;
        const QStringList have = db.families();
        for (const QString &family : want) {
            if (have.contains(family)) {
                cached = family;
                break;
            }
        }
        if (cached.isEmpty())
            cached = QStringLiteral("Consolas");
    }
    return cached;
}

// --content-max: clamp(680px, 86vw, 1320px)
int contentMaxWidth(int viewportWidth)
{
    const int minWidth = scaledPx(680);
    const int maxWidth = scaledPx(1320);
    const int preferred = qRound(viewportWidth * 0.86);
    return qBound(minWidth, preferred, maxWidth);
}

int contentSidePadding(int viewportWidth)
{
    const int inner = contentMaxWidth(viewportWidth);
    return qMax(0, (viewportWidth - inner) / 2);
}

void restyle(QWidget *w)
{
    if (!w)
        return;
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

void applyAccessibility(QWidget *widget)
{
    if (!widget)
        return;
    // webui 的焦点环是 :focus-visible（只有键盘驱动才显形），Qt 没有这个伪态：
    // 统一用 TabFocus 近似——可 Tab 到达，但鼠标点完不留一圈焦点环
    // （disabled 的控件本来就聚焦不了，无需特判）
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
        if (button->focusPolicy() != Qt::TabFocus)
            button->setFocusPolicy(Qt::TabFocus);
    }
    // 图标按钮没有文本，无障碍名称取 toolTip（webui 用 aria-label 表达同一信息）
    if (widget->accessibleName().isEmpty() && !widget->toolTip().isEmpty())
        widget->setAccessibleName(widget->toolTip());
}

QString escapeHtml(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    out.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    out.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    out.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return out;
}

// style.css 里显式写了 letter-spacing 的选择器 → 基准字距（px）。
// QSS 没有 letter-spacing，只能在控件 polish 时设 QFont，键按 objectName / class 匹配。
static qreal letterSpacingBase(const QWidget *widget)
{
    struct Entry { const char *key; qreal px; };
    static const Entry byObjectName[] = {
        { "brandText", 0.8 },        // .brand { letter-spacing:.8px }
    };
    static const Entry byClass[] = {
        { "messageLabel", 0.6 },     // .message-label
        { "eyebrow", 1.4 },          // .eyebrow
        { "sectionLabel", 1.4 },     // .section-label
        { "phaseTitle", 0.6 },       // .phase-head strong
        { "phaseCount", 0.3 },       // .phase-head small（统计串）
        { "modelGroupLabel", 1.2 },  // .model-group-label
        { "slashGroupLabel", 1.2 },  // .slash-group-label（QSS 写 letter-spacing 会被 Qt 忽略）
        { "viewTab", 0.5 },          // .view-tab
        { "bubbleMeta", 0.4 },       // .bubble-meta
        { "bubblePill", 0.4 },       // .bubble-timing / .bubble-usage
        { "toolDetailLabel", 0.4 },  // .tool-detail-label
        { "toolArgsEmpty", 0.3 },    // .tool-args-empty
        { "procSum", 0.3 },          // .proc-sum
        { "choiceTitle", 0.6 },      // .choice-title
        { "choiceHint", 0.3 },       // .choice-hint
        { "trajLaneLabel", 1.0 },    // .traj-lane-label
        { "trajGroupLabel", 1.2 },   // .traj-group
        { "trajBadge", 0.5 },        // .traj-badge
        { "trajInspSection", 1.2 },  // .traj-insp-section
        { "skillSource", 0.4 },      // .skill-source
        { "turnRailHeadTitle", 1.0 },// .turn-rail-panel-head
        { "usageFilterLabel", 0.4 }, // .usage-filter-label
        { "usageStatLabel", 0.6 },   // .usage-stat span
        { "usageCalMonth", 0.4 },    // .usage-cal-month
    };

    const QString objectName = widget->objectName();
    for (const Entry &entry : byObjectName) {
        if (objectName == QLatin1String(entry.key))
            return entry.px;
    }
    const QStringList classes = widget->property("class").toString().split(QLatin1Char(' '));
    for (const QString &cls : classes) {
        for (const Entry &entry : byClass) {
            if (cls == QLatin1String(entry.key))
                return entry.px;
        }
    }
    return 0.0;
}

void applyLetterSpacing(QWidget *widget)
{
    if (!widget)
        return;
    const qreal base = letterSpacingBase(widget);
    if (base <= 0.0)
        return;
    const qreal px = base * g_zoomFactor; // 界面缩放时字距同步缩放
    QFont f = widget->font();
    if (f.letterSpacingType() == QFont::AbsoluteSpacing && qFuzzyCompare(f.letterSpacing(), px))
        return; // 幂等：setFont 会再触发 polish，提前返回避免来回
    f.setLetterSpacing(QFont::AbsoluteSpacing, px);
    widget->setFont(f);
}

QString cssColor(const QColor &color)
{
    if (color.alpha() >= 255)
        return color.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

// style.css → Qt Style Sheet。Qt 不支持 letter-spacing / box-shadow / text-transform /
// backdrop-filter，这类效果由自绘控件与渐变近似承担。
static const char *kSheetTemplate = R"QSS(
/* ===== 基础 ===== */
QToolTip {
    color: %TEXT%; background: %SURFACE%; border: 1px solid %LINESTRONG%; padding: 3px 6px;
}
QWidget#appShell {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %BGGRADA%, stop:1 %INSET%);
}

/* ===== 滚动条 ===== */
QScrollBar:vertical { background: %SCROLLTRACK%; width: 8px; margin: 0; border: 0; }
QScrollBar::handle:vertical { background: %SCROLLTHUMB%; border-radius: 4px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: %SCROLLTHUMBHOVER%; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; background: none; border: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: %SCROLLTRACK%; }
QScrollBar:horizontal { background: %SCROLLTRACK%; height: 8px; margin: 0; border: 0; }
QScrollBar::handle:horizontal { background: %SCROLLTHUMB%; border-radius: 4px; min-width: 24px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; background: none; border: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: %SCROLLTRACK%; }

/* ===== 顶栏（玻璃底，refined/chrome.css：两行一个整体，视图切换并入第二行） ===== */
QWidget#topbar { background: %GLASSBG%; border-bottom: 1px solid %HAIRLINE%; }
QLabel#brandText { color: %TEXTBRIGHT%; font-size: 13px; font-weight: 700; background: transparent; }
QLabel#brandMark { background: transparent; }
QPushButton#connection { border: 0; background: transparent; padding: 6px; }
QPushButton#connection:hover { background: %HOVERTINT%; }

/* 皮肤切换（.theme-switch）：幽灵触发器 + 下拉 */
QWidget#themeSwitch { background: transparent; }
QWidget#themeTrigger {
    border: 1px solid transparent; background: transparent; border-radius: 8px;
}
QWidget#themeTrigger:hover { border: 1px solid transparent; background: %HOVERTINT%; }
QLabel#themeLabel { color: %MUTED%; font-size: 11px; background: transparent; }
QWidget#themeTrigger:hover QLabel#themeLabel { color: %TEXTBRIGHT%; }
QFrame#themeListbox { border: 1px solid %LINESTRONG%; background: %GLASSBG2%; border-radius: 12px; }
QPushButton.themeOption {
    border: 1px solid transparent; background: transparent; color: %TEXT%;
    font-size: 11px; text-align: left; padding: 7px 8px; border-radius: 5px;
}
QPushButton.themeOption:hover { border-color: transparent; background: %HOVERTINT%; }
QPushButton.themeOption[active="true"] { color: %CYAN%; }

/* ===== 会话栏（第二行：新建 / 会话名 / 视图切换） ===== */
QWidget#sessionbar { background: %BAND1%; border-bottom: 1px solid %HAIRLINE%; }
QPushButton[variant="primary"] {
    border: 1px solid transparent; background: %CYANDARK%; color: %TEXTBRIGHT%; border-radius: 8px;
}
QPushButton[variant="primary"]:hover { background: %CYANMID%; }
QPushButton[variant="primary"]:disabled { color: %MUTED%; background: %CYANDARK%; }
/* 新建对话：中性底色 + 亮字，不再靠描边 */
QWidget#newSession {
    border: 1px solid %HAIRLINE%; background: %HOVERTINT%; color: %TEXTBRIGHT%; border-radius: 8px;
    padding: 0 11px;
}
QWidget#newSession:hover { border-color: %LINESTRONG%; background: %SURFACE3%; color: %TEXTBRIGHT%; }
QWidget#sessionTrigger { border: 1px solid transparent; background: transparent; border-radius: 8px; }
QWidget#sessionTrigger:hover { border: 1px solid transparent; background: %HOVERTINT%; }
QLabel#currentTitle { color: %MUTED%; font-size: 11px; background: transparent; }
QLabel.chevronGlyph { color: %MUTED%; background: transparent; }

/* 视图切换（对话 / 轨迹）：并入会话栏右侧，胶囊底而非下划线 */
QWidget#viewTabs { background: transparent; border: 0; }
QPushButton.viewTab {
    border: 0; border-radius: 7px; background: transparent;
    color: %MUTED2%; padding: 0 11px; font-size: 11px;
}
QPushButton.viewTab:hover { color: %TEXT%; background: %HOVERTINT%; }
QPushButton.viewTab[active="true"] { background: %HOVERTINT%; color: %TEXTBRIGHT%; font-weight: 500; }


/* ===== 会话面板（毛玻璃浮层） ===== */
QWidget#sessionPanel { border: 1px solid %LINESTRONG%; background: %GLASSBG2%; border-radius: 12px; }
QWidget#panelHead { background: transparent; border-bottom: 1px solid %LINE%; }
QLineEdit#sessionSearch {
    border: 1px solid %LINE%; background: %INSET%; color: %TEXT%; padding: 0 8px; border-radius: 4px;
    selection-background-color: %CYANDARK%;
}
QLineEdit#sessionSearch:focus { border-color: %CYAN%; }
QPushButton[variant="icon"] {
    border: 1px solid %LINE%; background: %SURFACE2%; color: %TEXT%; font-size: 16px; border-radius: 4px;
}
QPushButton[variant="icon"]:hover { border-color: %LINESTRONG%; color: %CYAN%; }
QWidget.sessionRow { background: transparent; border-bottom: 1px solid %LINE%; }
QWidget.sessionRow:hover { background: %SURFACE2%; }
QLabel.sessionTitle { color: %TEXT%; font-size: 13px; background: transparent; }
QLabel.sessionMeta { color: %MUTED%; font-size: 11px; background: transparent; }
/* 会话状态徽标（.session-badge.*） */
QLabel.sessionBadge { font-size: 10px; padding: 3px 6px; border-radius: 8px; background: transparent; }
QLabel.sessionBadge[status="running"] { color: %CYAN%; }
QLabel.sessionBadge[status="done"] { color: %GREEN%; }
QLabel.sessionBadge[status="waiting"] { color: %ORANGE%; }
QLabel.sessionBadge[status="stopped"] { color: %MUTED%; }
QLabel.sessionBadge[status="error"] { color: %RED%; }
QPushButton.sessionAction { border: 0; background: transparent; color: %MUTED%; font-size: 12px; }
QPushButton.sessionAction:hover { color: %CYAN%; }
QPushButton.sessionAction[danger="true"]:hover { color: %RED%; }

/* ===== 消息区（refined/chat.css：留白、阅读宽度、气泡质感） ===== */
QScrollArea#messages { border: 0; background: transparent; }
QWidget#messageList { background: transparent; }
QWidget#emptyState { background: transparent; }
QLabel#emptySymbol {
    border: 1px solid %ASSISTANTLINE%; color: %CYAN%; font-size: 20px; background: transparent;
    border-radius: 18px;
}
QLabel#emptyTitle { color: %TEXTBRIGHT%; font-size: 15px; font-weight: 700; background: transparent; }
QLabel#emptyHint { color: %MUTED2%; font-size: 11px; background: transparent; }

QWidget.messageWidget { background: transparent; }
/* 一轮一张卡：实底 + 极淡描边；用户侧右下角切一个角，形成有来有回 */
QFrame#turnCard {
    border: 1px solid %LINE%; border-radius: 14px;
    background: %CARDSOLID%;
}
QFrame#turnCard[role="assistant"] { background: %CARDSOLID2%; }
QFrame#turnCard[role="user"] {
    border: 1px solid %CYANGLOW%; background: %BUBBLEUSER%;
    border-bottom-right-radius: 5px;
}
QFrame#turnCard[role="tool"] { background: transparent; border-color: transparent; }
QFrame#turnCard[error="true"] { border-color: %RED%; }
QFrame#bubble { border: 0; background: transparent; }
QLabel.messageLabel { color: %MUTED2%; font-size: 10px; font-weight: 500; background: transparent; }

/* 底部信息行（.turn-foot）：左下复制，右下用量/用时/时间 */
QWidget#turnFoot { background: transparent; }
QPushButton.bubbleCopy {
    border: 1px solid transparent; background: transparent; color: %MUTED2%; border-radius: 5px;
}
QPushButton.bubbleCopy:hover { border-color: transparent; background: %HOVERTINT%; color: %TEXTBRIGHT%; }
QLabel.bubbleMeta { color: %MUTED2%; font-size: 10px; background: transparent; }
QPushButton.bubblePill {
    border: 1px solid transparent; border-radius: 8px; background: %HOVERTINT%; color: %MUTED%;
    font-size: 10px; padding: 1px 7px;
}
QPushButton.bubblePill:hover { border-color: transparent; background: %SURFACE3%; color: %TEXTBRIGHT%; }
QPushButton.bubblePill:disabled { color: %MUTED2%; }

/* 明细弹层（.bubble-*-pop） */
QFrame.bubblePop {
    border: 1px solid %LINESTRONG%; background: %GLASSBG2%; border-radius: 12px;
}
QLabel.popTitle { color: %TEXT%; font-size: 11px; font-weight: 600; background: transparent; }
QLabel.popTotal { color: %TEXT%; font-size: 11px; background: transparent; font-family: %UI%; }
QLabel.popRowLabel { color: %MUTED%; font-size: 11px; background: transparent; }
QLabel.popRowValue { color: %TEXTBRIGHT%; font-size: 11px; background: transparent; font-family: %UI%; }

/* ===== 过程区（思考 / 工具调用）：降到背景层，无底色无分隔线 ===== */
QWidget#turnProcess { background: transparent; }
QWidget.procRow { background: transparent; }
QWidget.procRow:hover { background: transparent; }
QLabel.procLabel { color: %MUTED%; font-size: 11px; font-weight: 500; background: transparent; }
QWidget.procRow[running="true"] QLabel.procLabel { color: %TEXTBRIGHT%; }
QLabel.procSum { color: %MUTED2%; font-size: 10px; background: transparent; font-family: %UI%; }
QWidget.procRow[hovered="true"] QLabel.procSum { color: %CYAN%; }
QWidget.procRow[running="true"] QLabel.procSum { color: %CYAN%; }
QWidget.procRow[proc="tools"][running="true"] QLabel.procSum { color: %ORANGE%; }
QWidget.procRow[failed="true"] QLabel.procSum { color: %RED%; }
QWidget.procBody { background: transparent; }
QWidget.procRail {
    border-left: 1px solid %LINE%; background: transparent;
}
QLabel.procRailText { color: %MUTED%; font-size: 11px; background: transparent; }

/* ===== Markdown（refined/chat.css + detail.css：正文 15px，表格去竖线做报告体） ===== */
QWidget.markdownView { background: transparent; }
QLabel.mdParagraph { color: %BODYTEXT%; font-size: 15px; background: transparent; font-family: "Microsoft YaHei", "Microsoft YaHei UI"; }
QLabel.mdHeading { color: %TEXTBRIGHT%; font-size: 15px; font-weight: 700; background: transparent; font-family: "Microsoft YaHei", "Microsoft YaHei UI"; }
QLabel.mdList { color: %BODYTEXT%; font-size: 15px; background: transparent; font-family: "Microsoft YaHei", "Microsoft YaHei UI"; }
QFrame.codeBlock { border: 1px solid %LINE%; background: %INSET%; border-radius: 10px; }
QLabel.codeBlockHeader {
    color: %MUTED2%; font-size: 10px; background: transparent;
    border-bottom: 1px solid %LINE%; padding: 4px 8px;
}
QLabel.codeBlockBody {
    color: %TEXT%; background: transparent; padding: 9px;
    font-family: %MONO%; font-size: 12px;
}
QLabel.mdQuote {
    color: %MUTED%; background: transparent; border-left: 2px solid %CYANMID%; padding: 2px 0 2px 10px;
}
/* 表格：去外框、去竖线、表头无底色，只留极淡行线（detail.css 的最终形态） */
QFrame.mdTable { border: 0; background: transparent; border-radius: 0; }
QLabel.mdTableHead { color: %MUTED%; font-size: 11px; font-weight: 500; background: transparent; }
QLabel.mdTableCell { color: %TEXT%; font-size: 13px; background: transparent; }
QLabel.mdRule { background: %LINE%; }
QLabel.mdImage { border: 1px solid %LINE%; border-radius: 10px; background: transparent; }

/* ===== 工具调用 ===== */
QWidget.toolList { background: transparent; }
QWidget.toolItem { background: transparent; }
QWidget.toolItemSummary { background: transparent; }
QWidget.toolItemSummary:hover { background: transparent; }
QLabel.toolItemName { color: %MUTED%; font-size: 11px; font-weight: 500; background: transparent; }
QWidget.toolItem[state="succeeded"] QLabel.toolItemName,
QWidget.toolItem[state="failed"] QLabel.toolItemName { color: %TEXT%; }
QWidget.toolItemSummary[hovered="true"] QLabel.toolItemName { color: %CYAN%; }
QLabel.statusLabel { color: %ORANGE%; font-size: 10px; background: transparent; }
QLabel.statusLabel[status="succeeded"] { color: %GREEN%; }
QLabel.statusLabel[status="failed"] { color: %RED%; }
QLabel.statusLabel[status="cancelled"] { color: %RED%; }
QLabel.statusLabel[status="done"] { color: %GREEN%; }
QFrame.toolDetail { background: transparent; border: 0; }
QLabel.toolDetailLabel {
    color: %MUTED2%; font-size: 10px; font-weight: 700; background: transparent;
}
QPlainTextEdit.toolPre, QLabel.toolPre {
    color: %MUTED2%; background: transparent; border: 0;
    font-family: %UI%; font-size: 11px;
}
/* 工具参数表（.tool-args-table） */
QFrame#toolArgsBox { border: 1px solid %LINE%; background: %INSET%; border-radius: 8px; }
QLabel.toolArgsHead { color: %MUTED2%; font-size: 10px; font-weight: 600; background: %BAND2%; }
QLabel.toolArgsKey { color: %CYAN%; font-size: 10px; background: transparent; font-family: %UI%; }
QLabel.toolArgsValue { color: %TEXT%; font-size: 10px; background: transparent; font-family: %UI%; }
QLabel.toolArgsValue[json="true"] { color: %CYAN%; }
QLabel.toolArgsEmpty { color: %MUTED2%; font-size: 10px; background: transparent; }

/* ===== 结构化卡片 / 工作流 / 审批（与消息卡同一层实底） ===== */
QFrame.structured {
    border: 1px solid %LINE%; background: %CARDSOLID%; border-radius: 14px;
}
QLabel.structuredTitle {
    color: %TEXTBRIGHT%; font-weight: 700; background: transparent;
    border-bottom: 1px solid %LINE%; padding: 7px 8px;
}
QPushButton[variant="option"] {
    border: 1px solid %LINE%; border-radius: 9px; background: %ASSISTANTTINT%; color: %TEXT%;
    min-height: 28px; padding: 5px 9px; font-size: 12px;
}
QPushButton[variant="option"]:hover { border-color: %LINESTRONG%; background: %HOVERTINT%; }
QPushButton[variant="option"]:disabled { color: %MUTED2%; }
QPushButton[variant="optionPrimary"] {
    border: 1px solid transparent; border-radius: 9px; background: %CYANDARK%; color: #eaf6fa;
    min-height: 28px; padding: 5px 9px; font-size: 12px; font-weight: 500;
}
QPushButton[variant="optionPrimary"]:hover { background: %CYANMID%; }
QPushButton[variant="optionPrimary"]:disabled { color: %MUTED%; }
QPushButton[variant="optionDanger"] {
    border: 1px solid %ERRLINE%; border-radius: 9px; background: %ERRTINT%; color: %ERRTEXT%;
    min-height: 28px; padding: 5px 9px; font-size: 12px;
}
QPushButton[variant="optionDanger"]:hover { background: %ERRTINT%; border-color: %ERRSTRONG%; }
QPushButton.actionButton {
    border: 1px solid %LINE%; border-radius: 9px; background: %ASSISTANTTINT%; color: %TEXT%;
    min-height: 30px; min-width: 72px; padding: 5px 9px; font-size: 12px;
}
QPushButton.actionButton:hover { border-color: %LINESTRONG%; background: %HOVERTINT%; }
QPushButton.actionButton:disabled { color: %MUTED2%; }
QPushButton.actionButton[tone="approve"] {
    border-color: transparent; background: %CYANDARK%; color: #eaf6fa; font-weight: 500;
}
QPushButton.actionButton[tone="approve"]:hover { background: %CYANMID%; }
QLabel.paramName { color: %TEXT%; font-size: 12px; background: transparent; }
QLabel.paramDesc { color: %MUTED%; font-size: 11px; background: transparent; }
QLineEdit.paramInput, QPlainTextEdit.paramInput {
    border: 1px solid %LINE%; background: %INSET%; color: %TEXT%; padding: 5px 7px; border-radius: 8px;
    selection-background-color: %CYANDARK%;
}
QLineEdit.paramInput:focus, QPlainTextEdit.paramInput:focus { border-color: %CYANMID%; }
QLineEdit.paramInput:disabled, QPlainTextEdit.paramInput:disabled { color: %MUTED2%; }
QTextEdit.paramInput {
    border: 1px solid %LINE%; background: %INSET%; color: %TEXT%; padding: 5px 7px; border-radius: 8px;
    selection-background-color: %CYANDARK%;
}
QTextEdit.paramInput:focus { border-color: %CYANMID%; }

QFrame.workflowCard, QFrame.approvalCard {
    border: 1px solid %LINE%; background: %CARDSOLID%; border-radius: 14px;
}
QWidget.cardHead { background: transparent; border-bottom: 1px solid %LINE%; }
QLabel.cardTitle { color: %TEXT%; font-weight: 700; background: transparent; }
QLabel.toolBody {
    color: %MUTED2%; background: transparent; padding: 7px 8px;
    font-family: %UI%; font-size: 11px;
}

/* ===== 选择 / 审批浮层（refined/cards.css：抬成一张玻璃面板） ===== */
QFrame#choiceCard {
    border: 1px solid %LINESTRONG%; border-radius: 16px; background: %GLASSSOLID%;
}
/* 正文滚动区不许画自己的底：卡片底色 + 圆角要保持一整块 */
QScrollArea#choiceScroll { border: 0; background: transparent; }
QWidget#choiceBody { background: transparent; }
QWidget#choiceHead { background: transparent; }
QWidget#choiceHead:hover { background: %HOVERTINT%; }
QLabel.choiceTitle { color: %MUTED%; font-size: 10px; font-weight: 500; background: transparent; }
QPushButton#choiceClose {
    border: 0; background: transparent; color: %MUTED%; font-size: 15px; border-radius: 6px;
}
QPushButton#choiceClose:hover { background: %HOVERTINT%; color: %TEXTBRIGHT%; }
QLabel.choiceQuestion { color: %TEXTBRIGHT%; font-size: 14px; font-weight: 500; background: transparent; }
QWidget.choiceItem { background: transparent; border: 1px solid transparent; border-radius: 10px; }
QWidget.choiceItem:hover { background: %HOVERTINT%; }
QWidget.choiceItem[selected="true"] { border: 1px solid %CYANMID%; background: %ACCENTTINT%; }
QLabel.choiceItemName { color: %TEXTBRIGHT%; font-size: 13px; font-weight: 700; background: transparent; }
QLabel.choiceItemDesc { color: %MUTED2%; font-size: 11px; background: transparent; }
/* 选项圆点（refined/cards.css 的 .choice-dot）：14px 圆环，选中时填青 */
QLabel.choiceDot {
    border: 1px solid %LINESTRONG%; border-radius: 7px; background: transparent;
}
QWidget.choiceItem:hover QLabel.choiceDot { border-color: %MUTED2%; }
QWidget.choiceItem[selected="true"] QLabel.choiceDot {
    border-color: %CYAN%; background: %CYAN%;
}
QLineEdit#choiceInput {
    border: 1px solid %LINE%; border-radius: 10px; background: %INSET%; color: %TEXT%; padding: 0 9px;
    selection-background-color: %CYANDARK%;
}
QLineEdit#choiceInput:focus { border-color: %CYANMID%; }
QLineEdit#choiceInput[invalid="true"] { border-color: %ERRLINE%; background: %ERRTINT%; }
QPushButton#choiceSubmit {
    border: 1px solid transparent; border-radius: 10px; background: %CYANDARK%; color: #eaf6fa;
    font-size: 12px; padding: 0 12px; font-weight: 500;
}
QPushButton#choiceSubmit:hover:enabled { background: %CYANMID%; }
QPushButton#choiceSubmit:disabled { color: %MUTED%; background: %SURFACE3%; }
QLabel.choiceHint { color: %MUTED2%; font-size: 10px; background: transparent; }
QLabel.choiceCaret { color: %MUTED2%; font-size: 11px; background: transparent; }
QLabel.paramMore, QPushButton.paramMore {
    border: 1px solid %LINE%; border-radius: 8px; background: transparent; color: %MUTED%;
    font-size: 11px; padding: 4px 8px;
}
QPushButton.paramMore:hover { border-color: %LINESTRONG%; color: %TEXT%; }

/* ===== 阶段计划面板 ===== */
QFrame#phasePanel {
    border: 1px solid %LINE%; border-radius: 12px;
    background: %ASSISTANTTINT%;
}
QWidget#phaseHead { background: transparent; border-bottom: 1px solid transparent; }
QWidget#phaseHead:hover { background: %HOVERTINT%; }
QWidget#phaseHead[expanded="true"] { background: transparent; border-bottom: 1px solid %LINE%; }
QLabel#phaseTitle { color: %TEXT%; font-size: 12px; font-weight: 500; background: transparent; }
/* .phase-head small：统计串本体是 muted-2，各段颜色由 PhaseCountLabel 内联给出 */
QLabel#phaseCount {
    background: transparent; border: 0; color: %MUTED2%;
    font-size: 10px; font-weight: 500;
}
QLabel.phaseChevron { color: %MUTED2%; font-size: 10px; background: transparent; }
QLabel.phaseChevron[expanded="true"] { color: %CYAN%; }
QScrollArea#phaseSteps { background: %BAND3%; border: 0; }
QWidget#phaseStepsInner { background: %BAND3%; }

/* ===== 输入区（refined/toolbar.css：控件降噪，发送键是唯一亮色点） ===== */
QWidget#composer { background: transparent; }
QLabel#configWarning {
    background: %WARNTINT%; color: %WARNTEXT%; font-size: 11px;
    border-left: 2px solid %ORANGE%; padding: 6px 7px; border-radius: 8px;
}
QWidget#controls { background: transparent; }
QFrame.modelControl { border: 1px solid transparent; background: transparent; border-radius: 8px; }
QFrame.modelControl:hover { border: 1px solid transparent; background: %HOVERTINT%; }
QWidget.comboTrigger { background: transparent; border: 0; }
QLabel.comboText { color: %MUTED%; font-size: 11px; background: transparent; }
QPushButton#settingsButton {
    border: 1px solid transparent; background: transparent; color: %MUTED2%; border-radius: 8px; font-size: 13px;
}
QPushButton#settingsButton:hover { border-color: transparent; background: %HOVERTINT%; color: %TEXTBRIGHT%; }
QPushButton#attachButton, QPushButton#voiceButton {
    border: 1px solid transparent; background: transparent; color: %MUTED2%; border-radius: 8px;
}
QPushButton#attachButton:hover, QPushButton#voiceButton:hover:enabled {
    border-color: transparent; background: %HOVERTINT%; color: %TEXTBRIGHT%;
}
QPushButton#voiceButton:disabled { color: %MUTED2%; }
QPushButton#voiceButton[recording="true"] {
    background: %ERRSTRONG%; border-color: transparent; color: #ffffff;
}
QWidget#attachBar { background: transparent; }
QFrame.attachChip {
    border: 1px solid %LINE%; border-radius: 8px; background: %ASSISTANTTINT%;
}
QLabel.attachExt {
    background: %SURFACE3%; color: %CYAN%; font-size: 10px; font-weight: 700;
    border-radius: 5px; padding: 1px 4px;
}
QLabel.attachName { color: %TEXT%; font-size: 11px; background: transparent; }
QLabel.attachChipThumb { border-radius: 5px; background: %INSET%; }
QLabel.attachSize { color: %MUTED2%; font-size: 10px; background: transparent; }
QPushButton.attachRemove {
    border: 0; background: transparent; color: %MUTED%; font-size: 12px; border-radius: 5px;
}
QPushButton.attachRemove:hover { background: %ERRSTRONG%; color: #ffffff; }

/* 气泡内已发送附件 */
QWidget.bubbleAttachments { background: transparent; }
QFrame.attachFile {
    border: 1px solid %LINE%; border-radius: 8px; background: %INSET%;
}
QLabel.attachThumb {
    border: 1px solid %LINE%; border-radius: 8px; background: %INSET%;
}

/* 输入框：抬升为一层浮起的书写面（.input-wrap 16px 圆角 + 实底 + 深投影） */
QFrame#inputWrap { border: 1px solid %LINE%; background: %INPUTBG%; border-radius: 16px; }
QFrame#inputWrap[focus="true"] { border-color: %CYANMID%; }
QTextEdit#messageInput {
    border: 0; background: transparent; color: %TEXT%; padding: 14px 15px 6px 15px;
    selection-background-color: %CYANDARK%;
}
QTextEdit#messageInput > QWidget { background: transparent; }
QPushButton#sendButton {
    border: 0; border-radius: 9px; background: %CYAN%; color: #08191f; font-size: 15px;
}
QPushButton#sendButton:hover { background: #85c2da; }
QPushButton#sendButton:disabled { color: %MUTED2%; background: %HOVERTINT%; }
QPushButton#sendButton[stop="true"] { border: 0; background: %ERRSTRONG%; color: #ffffff; font-size: 11px; }
QLabel#busyLabel { color: %MUTED2%; font-size: 10px; background: transparent; }

/* ===== 下拉列表（模型 / Skill）：玻璃底 + 柔和投影 ===== */
QWidget#listbox { border: 1px solid %LINESTRONG%; background: %GLASSBG2%; border-radius: 12px; }
QScrollArea#listboxScroll { border: 0; background: transparent; }
QWidget#listboxInner { background: transparent; }
/* 输入区的专业分类 / 知识库下拉：同一个 .model-listbox 家族，
   但 objectName 不同，#listbox 那条匹配不上，这里补齐（成在 theme 里，
   切皮肤跟着全局 QSS 一起重建） */
QFrame#categoryList, QFrame#kbList {
    border: 1px solid %LINESTRONG%; background: %GLASSBG2%; border-radius: 12px;
}
QFrame#kbList QWidget.modelOption QLabel.modelName { color: %TEXT%; }
QFrame#kbList QWidget.modelOption[selected="true"] QLabel.modelName { color: %CYAN%; }
QFrame#kbList QWidget.modelOption QLabel.modelId { color: %MUTED2%; }

/* ===== 上下文用量浮层（#context-pop：min-width:216px，贴在圆环上方） ===== */
QFrame#contextPop { border: 1px solid %LINESTRONG%; border-radius: 12px; background: %GLASSBG2%; }
QWidget#contextPopHead { background: transparent; border: 0; border-bottom: 1px solid %LINE%; }
QLabel.popTitle { color: %TEXT%; font-size: 11px; font-weight: 600; background: transparent; }
QLabel.popTotal { color: %TEXT%; font-size: 11px; font-family: %UI%; background: transparent; }
QLabel.popLabel { color: %MUTED%; font-size: 11px; background: transparent; }
QLabel.popValue { color: %TEXTBRIGHT%; font-size: 11px; font-family: %UI%; background: transparent; }
QLabel.popNote { color: %MUTED2%; font-size: 10px; background: transparent; }
QLabel.modelGroupLabel {
    color: %MUTED%; font-size: 10px; font-weight: 700; background: transparent; padding: 7px 8px 4px 8px;
}
QWidget.modelOption { background: transparent; border: 1px solid transparent; border-radius: 8px; }
QWidget.modelOption:hover, QWidget.modelOption[focused="true"] {
    border: 1px solid transparent; background: %HOVERTINT%;
}
QWidget.modelOption[selected="true"] { background: %ACCENTTINT%; }
QLabel.modelCheck { color: %CYAN%; font-size: 11px; background: transparent; }
QLabel.modelName { color: %TEXT%; font-size: 12px; background: transparent; }
QLabel.modelId { color: %MUTED%; font-size: 10px; background: transparent; font-family: %UI%; }
QLabel.listboxEmpty { color: %MUTED%; font-size: 12px; background: transparent; padding: 16px; }

/* ===== 交互模式下拉（.mode-listbox：固定 240 宽，两行标题 + 说明） ===== */
QFrame#listbox.modeListbox { background: %GLASSBG2%; }
QWidget.modeOptionText { background: transparent; }
QLabel.modeOptionTitle { color: %TEXT%; font-size: 12px; font-weight: 600; background: transparent; }
QLabel.modeOptionHint { color: %MUTED2%; font-size: 10px; background: transparent; }
QLabel.modeCheck { color: %CYAN%; font-size: 12px; background: transparent; }
QWidget.modeOption[selected="true"] QLabel.modeOptionTitle { color: %CYAN%; }

/* ===== Skill chip（取代原来的第三个下拉） ===== */
QPushButton.skillChip {
    border: 1px solid %CYANMID%; border-radius: 8px; background: %ACCENTTINT%;
    color: %CYAN%; font-size: 11px; text-align: left;
}
QLabel.skillChipName { color: %CYAN%; font-size: 11px; font-weight: 600; background: transparent; }
QLabel.skillChipClose { color: %MUTED2%; font-size: 12px; background: transparent; }
QPushButton.skillChip:hover QLabel.skillChipClose { color: %ERRTEXT%; }

/* ===== 斜杠面板（输入框敲 "/" 唤起，浮在输入框上方） ===== */
QFrame#slashMenu {
    border: 1px solid %LINESTRONG%; background: %GLASSBG2%; border-radius: 12px;
}
QScrollArea#slashScroll { border: 0; background: transparent; }
QWidget.slashInner { background: transparent; }
QLabel.slashGroupLabel {
    color: %MUTED%; font-size: 10px; font-weight: 700; background: transparent;
    padding: 7px 8px 4px 8px;
}
QLineEdit.slashSearch {
    border: 1px solid %LINESTRONG%; background: %BG%; color: %TEXT%;
    font-size: 11px; padding: 0 8px; border-radius: 4px; min-height: 30px;
}
QLineEdit.slashSearch:focus { border-color: %CYANMID%; }
QWidget.slashItem { background: transparent; border: 1px solid transparent; border-radius: 8px; }
QWidget.slashItem:hover, QWidget.slashItem[focused="true"] {
    border: 1px solid transparent; background: %HOVERTINT%;
}
QLabel.slashName { color: %TEXT%; font-size: 12px; font-weight: 600; background: transparent; }
QLabel.slashCode { color: %MUTED%; font-size: 10px; background: transparent; font-family: %UI%; }
QLabel.slashDesc { color: %MUTED2%; font-size: 10px; background: transparent; }
QWidget.slashItem[selected="true"] QLabel.slashName,
QWidget.slashItem[selected="true"] QLabel.slashCode { color: %CYAN%; }

/* ===== Toast / 拖拽遮罩 ===== */
QLabel#toast {
    border: 1px solid %ERRLINE%; background: %ERRTINT%; color: %ERRTEXT%; padding: 8px 10px;
    border-radius: 12px;
}
QWidget#dropOverlay {
    border: 2px dashed %CYAN%; background: %SCRIM2%;
}
QLabel#dropOverlayText {
    color: %CYAN%; font-size: 14px; background: transparent;
}

/* ===== 轮次导航轨（.turn-rail）与轮次列表面板（.turn-rail-panel） ===== */
QWidget#turnRail {
    background: transparent; border: 0;
}
QWidget.turnRailDot { background: transparent; border: 0; }
QFrame.turnRailPanel {
    border: 1px solid %LINESTRONG%; border-radius: 8px; background: %PANEL%;
}
QWidget.turnRailHead { background: %BAND3%; border-bottom: 1px solid %LINE%; }
QLabel.turnRailHeadTitle { color: %MUTED%; font-size: 10px; font-weight: 700; background: transparent; }
QLabel.turnRailHeadCount { color: %MUTED2%; font-size: 10px; background: transparent; }
QScrollArea#turnRailScroll { border: 0; background: transparent; }
QWidget#turnRailList { background: transparent; }
QWidget.turnRailRow { border: 0; border-radius: 4px; background: transparent; }
QWidget.turnRailRow:hover, QWidget.turnRailRow[active="true"] { background: %ACCENTTINT%; }
QLabel.turnRailIndex {
    border: 1px solid %LINE%; border-radius: 9px; background: %INSET%; color: %MUTED%;
    font-size: 10px; font-weight: 600;
}
QWidget.turnRailRow:hover QLabel.turnRailIndex,
QWidget.turnRailRow[active="true"] QLabel.turnRailIndex {
    border-color: %CYANMID%; background: %CYANDARK%; color: %TEXTBRIGHT%;
}
QLabel.turnRailText { color: %TEXT%; font-size: 11px; background: transparent; }
QWidget.turnRailRow:hover QLabel.turnRailText,
QWidget.turnRailRow[active="true"] QLabel.turnRailText { color: %TEXTBRIGHT%; }

/* ===== 轨迹视图 ===== */
QWidget#trajectoryView { background: transparent; }
QWidget#trajToolbar { background: %BAND2%; border-bottom: 1px solid %LINE%; }
QPushButton.trajViewButton {
    border: 1px solid transparent; background: transparent; color: %MUTED%; font-size: 11px;
    padding: 0 10px; min-height: 26px;
}
QPushButton.trajViewButton:hover { background: %SURFACE2%; color: %TEXT%; }
QPushButton.trajViewButton[active="true"],
QPushButton.trajViewButton[active="true"]:hover {
    border: 1px solid %CYANMID%; background: %ACCENTTINT%; color: %CYAN%;
}
QLineEdit#trajSearch {
    border: 1px solid %LINE%; background: %INSET%; color: %TEXT%; padding: 0 8px;
    border-radius: 4px; font-size: 11px;
}
QLineEdit#trajSearch:focus { border-color: %CYAN%; }
QWidget#trajTimeline { background: %BAND3%; border-bottom: 1px solid %LINE%; }
QLabel.trajLaneLabel { color: %MUTED2%; font-size: 9px; background: transparent; }
QFrame.trajLaneTrack { background: %INSET%; border: 1px solid %LINE%; border-radius: 3px; }
QWidget#trajLedger { background: %BAND3%; }
QWidget#trajInspector { background: %BAND3%; border-left: 1px solid %LINE%; }
QWidget.trajGroup { background: transparent; border-top: 1px solid %LINE%; }
QWidget.trajGroup:hover { background: %BAND2%; }
QLabel.trajGroupLabel { color: %CYAN%; font-size: 9px; font-weight: 700; background: transparent; }
QLabel.trajGroupMeta { color: %MUTED2%; font-size: 9px; background: transparent; font-family: %UI%; }
QLabel.trajSummary { color: %MUTED2%; font-size: 11px; background: transparent; }
QWidget.trajRow { background: transparent; }
QWidget.trajRow:hover { background: %BAND2%; }
QWidget.trajRow[selected="true"] { background: %ACCENTTINT%; }
QLabel.trajBadge { background: %SURFACE2%; color: %MUTED%; font-size: 9px; border-radius: 3px; padding: 2px 6px; }
QLabel.trajBadge[source="user"] { background: %ACCENTTINT%; color: %CYAN%; }
QLabel.trajBadge[source="context"] { background: %OKTINT%; color: %OKTEXT%; }
QLabel.trajBadge[source="assistant"] { background: %ASSISTANTTINT%; color: %ASSISTANTTEXT%; }
QLabel.trajBadge[source="tool"] { background: %WARNTINT%; color: %WARNTEXT%; }
QLabel.trajRowLabel { color: %TEXTBRIGHT%; font-size: 11px; font-weight: 500; background: transparent; }
QLabel.trajRowPreview { color: %MUTED%; font-size: 11px; background: transparent; }
QLabel.trajRowMeta { color: %MUTED2%; font-size: 9px; background: transparent; font-family: %UI%; }
QWidget.trajRowBar { background: %INSET%; border: 1px solid %LINE%; border-radius: 8px; }
QLabel.trajEmpty { color: %MUTED2%; font-size: 11px; background: transparent; }
QLabel.trajInspPos { color: %MUTED%; font-size: 10px; background: transparent; font-family: %UI%; }
QPushButton.trajInspTab {
    border: 0; border-bottom: 2px solid transparent; background: transparent; color: %MUTED%;
    padding: 0 12px; font-size: 11px; min-height: 30px;
}
QPushButton.trajInspTab:hover { background: %BAND2%; color: %TEXT%; }
QPushButton.trajInspTab[active="true"] { border-bottom: 2px solid %CYAN%; color: %TEXTBRIGHT%; }
QLabel.trajInspRowLabel { color: %MUTED%; font-size: 11px; background: transparent; }
QLabel.trajInspRowValue { color: %TEXT%; font-size: 11px; background: transparent; }
QLabel.trajInspSection { color: %CYAN%; font-size: 9px; font-weight: 700; background: transparent; }
QPlainTextEdit.trajInspRaw, QLabel.trajInspRaw {
    border: 1px solid %LINE%; background: %INSET%; color: %TEXT%;
    font-family: %UI%; font-size: 10px; border-radius: 4px;
}

/* ===== 设置中心 ===== */
QDialog#settingsDialog { background: %GLASSBG2%; border: 1px solid %LINESTRONG%; border-radius: 12px; }
/* style.css @media(max-width:640px)：.settings-dialog{border:0;border-radius:0} */
QDialog#settingsDialog[compact="true"] { border: 0; border-radius: 0; }
QWidget#settingsHead { background: %BAND2%; border-bottom: 1px solid %LINE%; }
QLabel.eyebrow {
    color: %CYAN%; font-size: 9px; font-weight: 700; background: transparent;
}
QLabel#settingsTitle { color: %TEXTBRIGHT%; font-size: 18px; font-weight: 700; background: transparent; }
QWidget#settingsTabs { background: %BAND3%; border-bottom: 1px solid %LINE%; }
QPushButton.settingsTab {
    border: 0; border-bottom: 2px solid transparent; background: transparent;
    color: %MUTED%; padding: 0 18px; font-size: 13px;
}
QPushButton.settingsTab[active="true"] { border-bottom: 2px solid %CYAN%; color: %TEXTBRIGHT%; }
QWidget#settingsContent { background: transparent; }
QPushButton[variant="secondary"] {
    border: 1px solid %LINESTRONG%; border-radius: 4px; background: %SURFACE2%; color: %TEXT%;
    min-height: 28px; padding: 5px 10px;
}
QPushButton[variant="secondary"]:hover { border-color: %CYANMID%; }
QPushButton[variant="secondary"]:disabled { color: %MUTED2%; }
QPushButton[variant="danger"] {
    border: 1px solid %ERRLINE%; border-radius: 4px; background: %ERRTINT%; color: %ERRTEXT%;
    min-height: 28px; padding: 5px 10px;
}
QPushButton[variant="danger"]:hover { background: %ERRTINT%; border-color: %ERRSTRONG%; }

QWidget#providerSidebar { background: %BAND3%; border-right: 1px solid %LINE%; }
QLabel.sectionLabel {
    color: %CYAN%; font-size: 9px; font-weight: 700; background: transparent; padding: 0 7px 9px 7px;
}
QScrollArea#providerNavScroll { border: 0; background: transparent; }
QWidget#providerNav { background: transparent; }
QWidget.providerNavItem { border: 1px solid transparent; background: transparent; border-radius: 4px; }
QWidget.providerNavItem:hover { background: %SURFACE2%; }
QWidget.providerNavItem[active="true"] { border: 1px solid %CYANMID%; background: %ACCENTTINT%; }
QLabel.providerName { color: %TEXT%; font-size: 12px; font-weight: 700; background: transparent; }
QWidget.providerNavItem[active="true"] QLabel.providerName { color: %CYAN%; }
QLabel.providerId { color: %MUTED2%; font-size: 9px; background: transparent; font-family: %UI%; }
QLabel.providerCount {
    background: %SURFACE3%; color: %MUTED%; font-size: 9px; border-radius: 10px; padding: 2px 6px;
}
QPushButton#addProvider {
    border: 1px solid %LINESTRONG%; border-radius: 4px; background: %SURFACE2%; color: %TEXT%; min-height: 28px;
}
QPushButton#addProvider:hover { border-color: %CYANMID%; }

QScrollArea#providerEditorScroll { border: 0; background: transparent; }
QWidget#providerEditor { background: transparent; }
QFrame.providerSection { border: 1px solid %LINE%; background: %BAND2%; border-radius: 8px; }
QLabel.sectionTitle { color: %TEXTBRIGHT%; font-size: 15px; font-weight: 700; background: transparent; }
QLabel.sectionTitleSmall { color: %MUTED2%; font-size: 11px; background: transparent; }
QLabel.fieldCaption { color: %MUTED%; font-size: 10px; background: transparent; }
QLineEdit.settingsInput, QComboBox.settingsInput {
    border: 1px solid %LINE%; background: %INSET%; color: %TEXT%;
    padding: 0 8px; min-height: 30px; border-radius: 4px;
    selection-background-color: %CYANDARK%;
}
QLineEdit.settingsInput:focus, QComboBox.settingsInput:focus { border-color: %CYAN%; }

/* 键盘焦点可见：QSS 没有浏览器那样的默认 focus 环，按钮与自绘可点区域都要显式给 */
QPushButton:focus, QToolButton:focus { outline: 1px solid %CYAN%; outline-offset: 1px; }
QWidget.trajRow:focus, QWidget.trajGroup:focus, QWidget.trajSummary:focus,
QWidget.procRowHead:focus, QWidget.toolItemSummary:focus, QWidget.turnRailRow:focus,
QWidget.choiceItem:focus, QWidget.sessionSelect:focus {
    outline: 1px solid %CYAN%; outline-offset: -1px;
}
QWidget#choiceHead:focus, QWidget#phaseHead:focus { outline: 1px solid %CYAN%; outline-offset: -1px; }
QLineEdit.settingsInput:disabled { color: %MUTED2%; background: %BAND3%; }
QComboBox.settingsInput::drop-down { border: 0; width: 20px; }
/* Qt 的 ::down-arrow 只认 image，CSS 那套"border 三角"会画成一个实心方块，
   所以用一张已着色的小 chevron（三套皮肤下都能看清的中性灰）。 */
QComboBox.settingsInput::down-arrow {
    image: url(:/icons/chevron-down-muted.svg); width: 10px; height: 10px; margin-right: 6px;
}
QComboBox.settingsInput QAbstractItemView::item { padding: 4px 6px; }
QComboBox QAbstractItemView {
    border: 1px solid %LINESTRONG%; background: %INSET%; color: %TEXT%;
    selection-background-color: %ACCENTTINT%; outline: 0;
}
QCheckBox.settingsCheck { color: %MUTED%; font-size: 11px; background: transparent; spacing: 6px; }
QCheckBox.settingsCheck::indicator {
    width: 13px; height: 13px; border: 1px solid %LINESTRONG%; background: %INSET%; border-radius: 4px;
}
QCheckBox.settingsCheck::indicator:checked { background: %CYANDARK%; border-color: %CYANMID%; }
QLabel.inlineResult { color: %MUTED%; font-size: 11px; background: transparent; }

QFrame.settingsModel { border: 1px solid %LINE%; background: %INSET%; border-radius: 8px; }
QWidget.settingsModelSummary { background: transparent; }
QWidget.settingsModelSummary:hover { background: %BAND2%; }
QLabel.settingsModelName { color: %TEXT%; font-size: 12px; font-weight: 700; background: transparent; }
QLabel.settingsModelId { color: %MUTED2%; font-size: 9px; background: transparent; font-family: %UI%; }
QLabel.settingsModelApi { color: %CYAN%; font-size: 9px; background: transparent; font-family: %UI%; }
QPushButton.modelDelete { border: 0; background: transparent; color: %MUTED%; font-size: 16px; }
QPushButton.modelDelete:hover { color: %RED%; }
QWidget.modelAdvanced { background: transparent; border-top: 1px solid %LINE%; }
QFrame#addModelBox { border: 1px dashed %LINESTRONG%; background: %INSET%; border-radius: 8px; }
QScrollArea#candidateScroll { border: 0; background: transparent; }
QWidget#candidateList { background: transparent; }
QPushButton.candidateItem {
    border: 0; background: transparent; color: %TEXT%; text-align: left; padding: 6px; font-size: 11px;
}
QPushButton.candidateItem:hover { background: %ACCENTTINT%; }
QPushButton.candidateItem:disabled { color: %MUTED2%; }
QLabel.candidateName { color: %TEXT%; font-size: 11px; background: transparent; }
QLabel.candidateId { color: %MUTED2%; font-size: 9px; background: transparent; font-family: %UI%; }
QLabel.placeholderTitle { color: %TEXTBRIGHT%; font-size: 13px; font-weight: 700; background: transparent; }
QLabel.placeholderText { color: %MUTED%; font-size: 12px; background: transparent; }

QWidget#skillsPanel, QWidget#mcpPanel { background: transparent; }
QLabel.panelTitle { color: %TEXTBRIGHT%; font-size: 15px; font-weight: 700; background: transparent; }
QLabel.panelCount { color: %MUTED2%; font-size: 11px; background: transparent; }
QLabel.panelStatus { color: %MUTED%; font-size: 11px; background: transparent; }
QScrollArea.panelScroll { border: 0; background: transparent; }
QFrame.skillCard, QFrame.mcpTool { border: 1px solid %LINE%; background: %INSET%; border-radius: 8px; }
QWidget.skillCardSummary, QWidget.mcpToolSummary { background: transparent; }
QWidget.skillCardSummary:hover, QWidget.mcpToolSummary:hover { background: %BAND2%; }
QLabel.skillName { color: %TEXT%; font-size: 12px; font-weight: 500; background: transparent; }
QLabel.skillVersion {
    background: %ACCENTTINT%; color: %CYAN%; font-size: 9px; border-radius: 8px; padding: 2px 6px;
}
QLabel.skillSource {
    background: %SURFACE3%; color: %MUTED%; font-size: 9px; border-radius: 8px; padding: 2px 6px;
}
QLabel.skillDesc { color: %MUTED%; font-size: 11px; background: transparent; padding: 0 10px 9px 10px; }
QLabel.skillMeta { color: %MUTED2%; font-size: 10px; background: transparent; padding: 0 10px 9px 10px; }
QLabel.skillShadowed {
    color: %ORANGE%; font-size: 10px; background: transparent; padding: 8px 10px 9px 10px;
    border-top: 1px solid %LINE%;
}
QWidget.codeChipStrip { background: transparent; border-top: 1px solid %LINE%; padding: 2px 10px 9px 10px; }
QLabel.codeChip {
    border: 1px solid %LINE%; border-radius: 4px; background: %INSET%; color: %CYAN%;
    font-size: 10px; font-family: %UI%; padding: 2px 6px;
}
QLabel.mcpParamDesc { color: %MUTED2%; font-size: 10px; background: transparent; }
QLabel.mcpParamType { color: %MUTED2%; font-size: 10px; background: transparent; }
QLabel.mcpParamRequired {
    border: 1px solid %WARNLINE%; border-radius: 8px; color: %ORANGE%; font-size: 9px; padding: 1px 5px;
}
QLabel.paramCode {
    border-radius: 4px; background: %INSET%; color: %CYAN%;
    font-size: 10px; font-family: %UI%; padding: 1px 5px;
}
QLabel.skillMetaText { color: %MUTED2%; font-size: 10px; background: transparent; }
QWidget.mcpParamRow { background: transparent; border-bottom: 1px solid %LINE%; }
QWidget.mcpParamList { background: transparent; border-top: 1px solid %LINE%; padding: 2px 10px 9px 10px; }

QWidget#settingsActions { background: %BAND3%; border-top: 1px solid %LINE%; }
QLabel#settingsStatus { color: %WARNTEXT%; font-size: 11px; background: transparent; }

/* ===== 设置中心 · 用量（.usage-*，对应 app.js renderUsagePanel + charts.js） ===== */
QScrollArea.usagePanelScroll { border: 0; background: transparent; }
QWidget.usagePanel, QWidget.usageFilters, QWidget.usageFilter,
QWidget.usageOverview, QWidget.usageCalGrid, QWidget.usageCalWeek { background: transparent; }
QLabel.usageFilterLabel { color: %MUTED2%; font-size: 10px; background: transparent; }
QWidget.usageSelect { border: 1px solid %LINE%; border-radius: 15px; background: %SURFACE2%; }
QWidget.usageSelect:hover { border-color: %CYANMID%; }
QWidget.usageSelect[open="true"] { border-color: %CYAN%; }
QWidget.usageSelect[open="true"] QLabel.usageSelectText { color: %TEXTBRIGHT%; }
QLabel.usageSelectText { color: %TEXT%; font-size: 11px; background: transparent; }
QLabel.usageSelectIcon { background: transparent; }
QPushButton.usageRefresh { min-height: 30px; }

QFrame.usageListbox { border: 1px solid %LINESTRONG%; border-radius: 8px; background: %INSET%; }
QScrollArea#usageListScroll { border: 0; background: transparent; }
QWidget#usageListInner { background: transparent; }
QPushButton.usageOption { border: 1px solid transparent; border-radius: 4px; background: transparent; text-align: left; }
QPushButton.usageOption:hover:enabled { border-color: %CYANMID%; background: %ACCENTTINT%; }
QPushButton.usageOption[selected="true"] { color: %CYAN%; }
QPushButton.usageOption:disabled { color: %MUTED2%; }
QLabel.usageOptionLabel { color: %TEXT%; font-size: 11px; background: transparent; }
QLabel.usageOptionNote { color: %MUTED2%; font-size: 9px; background: transparent; font-family: %UI%; }

QFrame.usageRangePanel { border: 1px solid %LINESTRONG%; border-radius: 8px; background: %INSET%; }
QPushButton.usageRangePreset {
    border: 1px solid %LINE%; border-radius: 15px; background: %SURFACE2%; color: %MUTED%;
    font-size: 11px; min-height: 30px; padding: 0 12px;
}
QPushButton.usageRangePreset:hover { border-color: %CYANMID%; color: %TEXT%; }
QPushButton.usageRangePreset[active="true"] { border-color: %CYANMID%; background: %ACCENTTINT%; color: %CYAN%; }
QPushButton.usageCalConfirm {
    border: 1px solid %CYANMID%; border-radius: 15px; background: %CYANDARK%; color: %TEXTBRIGHT%;
    font-size: 11px; min-height: 30px; padding: 0 12px;
}
QPushButton.usageCalConfirm:disabled { border-color: %LINE%; background: %SURFACE2%; color: %MUTED2%; }
QLabel.usageCalTitle { color: %TEXTBRIGHT%; font-size: 12px; background: transparent; }
QPushButton.usageCalStep {
    border: 1px solid %LINE%; border-radius: 4px; background: %SURFACE2%; color: %MUTED%;
    min-width: 24px; max-width: 24px; min-height: 24px; max-height: 24px;
}
QPushButton.usageCalStep:hover { border-color: %CYANMID%; color: %CYAN%; }
QLabel.usageCalMonth { color: %MUTED%; font-size: 10px; background: transparent; padding-bottom: 6px; }
QLabel.usageCalWeekCell { color: %MUTED2%; font-size: 9px; background: transparent; }
QPushButton.usageCalDay { border: 0; border-radius: 4px; background: transparent; color: %TEXT%; font-size: 11px; }
QPushButton.usageCalDay:hover { background: %ACCENTTINT%; }
QPushButton.usageCalDay[other="true"] { color: %MUTED2%; }
QPushButton.usageCalDay[state="in"] { background: %ACCENTTINT%; color: %CYAN%; }
QPushButton.usageCalDay[state="start"], QPushButton.usageCalDay[state="end"],
QPushButton.usageCalDay[state="single"] { background: %CYANDARK%; color: %TEXTBRIGHT%; font-weight: 600; }

QLabel.usageStatus { color: %MUTED%; font-size: 11px; background: transparent; }
QFrame.usageStat { border: 1px solid %LINE%; border-radius: 8px; background: %BAND2%; }
QLabel.usageStatLabel { color: %MUTED2%; font-size: 9px; background: transparent; }
QLabel.usageStatValue { color: %TEXTBRIGHT%; font-size: 14px; background: transparent; }
QFrame.usageCard { border: 1px solid %LINE%; border-radius: 8px; background: %BAND2%; }
QLabel.usageCardTitle { color: %TEXTBRIGHT%; font-size: 13px; background: transparent; }
QLabel.usageCardHint { color: %MUTED2%; font-size: 9px; background: transparent; }

QTableWidget#usageTable { border: 0; background: transparent; gridline-color: %LINE%; }
QTableWidget#usageTable::item { border-bottom: 1px solid %LINE%; padding: 6px 8px; font-size: 11px; }
QTableWidget#usageTable::item:hover { background: %ACCENTTINT%; }
QTableWidget#usageTable QHeaderView { background: transparent; }
QTableWidget#usageTable QHeaderView::section {
    background: %BAND3%; border: 0; border-bottom: 1px solid %LINE%; padding: 6px 8px;
    color: %MUTED%; font-size: 10px;
}
QTableWidget#usageTable QTableCornerButton::section { background: %BAND3%; border: 0; }

/* ===== 设置中心 · 记忆页（.memory-*，对应 webui #panel-memory / app.js memory*） ===== */
QWidget#memoryPanel, QWidget.memoryContent { background: transparent; }
QScrollArea.memoryScroll { border: 0; background: transparent; }

QLabel.memoryStatus {
    border: 1px solid %LINESTRONG%; border-radius: 6px; background: %INSET%;
    color: %MUTED%; font-size: 11px; padding: 8px 10px;
}
QLabel.memoryStatus[tone="ok"] { border-color: %OKLINE%; background: %OKTINT%; color: %OKTEXT%; }
QLabel.memoryStatus[tone="warn"] { border-color: %WARNLINE%; background: %WARNTINT%; color: %WARNTEXT%; }
QLabel.memoryStatus[tone="error"] { border-color: %ERRLINE%; background: %ERRTINT%; color: %ERRTEXT%; }
QLabel.memoryStatus[tone="off"] { color: %MUTED%; }

QWidget.memorySwitches { background: transparent; }
QFrame.memorySwitch { border: 1px solid %LINE%; border-radius: 6px; background: %BAND2%; }
QFrame.memorySwitch:hover { border-color: %LINESTRONG%; }
QFrame.memorySwitch:focus { outline: 1px solid %CYAN%; outline-offset: -1px; }
QFrame.memorySwitch[disabled="true"] { background: %SURFACE2%; border-color: %LINE%; }
QLabel.memorySwitchTitle { color: %TEXTBRIGHT%; font-size: 12px; background: transparent; }
QLabel.memorySwitchHint { color: %MUTED%; font-size: 10px; background: transparent; }
QFrame.memorySwitch[disabled="true"] QLabel.memorySwitchTitle { color: %MUTED%; }

QCheckBox.memoryCheck { color: %MUTED%; font-size: 11px; background: transparent; spacing: 6px; }
QCheckBox.memoryCheck::indicator {
    width: 13px; height: 13px; border: 1px solid %LINESTRONG%; background: %INSET%; border-radius: 4px;
}
QCheckBox.memoryCheck::indicator:checked { background: %CYANDARK%; border-color: %CYANMID%; }
QCheckBox.memoryCheck::indicator:indeterminate { background: %SURFACE3%; border-color: %CYANMID%; }
QCheckBox.memoryCheck::indicator:disabled { background: %SURFACE2%; border-color: %LINE%; }
QCheckBox.memoryCheck:focus { outline: 1px solid %CYAN%; outline-offset: 1px; }

QFrame.memorySection { border: 1px solid %LINE%; border-radius: 6px; background: %BAND2%; }
QFrame.memorySection[danger="true"] { border-color: %ERRLINE%; }
QLabel.memoryHint { color: %MUTED%; font-size: 10px; background: transparent; }
QLabel.memoryFilterLabel { color: %MUTED%; font-size: 10px; background: transparent; }
QWidget.memoryFilters, QWidget.memoryFilter { background: transparent; }

QFrame.memoryBulk { border: 1px dashed %LINESTRONG%; border-radius: 6px; background: %INSET%; }
QLabel.memoryBulkCount { color: %MUTED%; font-size: 11px; background: transparent; }

QWidget.memoryList, QWidget.memoryTools { background: transparent; }
QLabel.memoryEmpty { color: %MUTED%; font-size: 11px; background: transparent; padding: 4px 0; }

QFrame.memoryCard { border: 1px solid %LINE%; border-radius: 6px; background: %INSET%; }
QWidget.memoryCardSpacer { background: transparent; }
QLabel.memoryChip {
    border: 1px solid %LINESTRONG%; border-radius: 10px; color: %MUTED%; font-size: 10px;
    padding: 2px 7px;
}
QLabel.memoryChip[tone="candidate"] { border-color: %WARNLINE%; background: %WARNTINT%; color: %WARNTEXT%; }
QLabel.memoryChip[tone="approved"] { border-color: %OKLINE%; background: %OKTINT%; color: %OKTEXT%; }
QLabel.memoryChip[tone="rejected"] { border-color: %ERRLINE%; background: %ERRTINT%; color: %ERRTEXT%; }
QLabel.memoryChip[tone="deprecated"] { border-color: %LINESTRONG%; background: %SURFACE3%; color: %MUTED%; }
QLabel.memoryCardTag { color: %MUTED2%; font-size: 10px; background: transparent; }
QLabel.memoryCardTitle { color: %TEXTBRIGHT%; font-size: 13px; background: transparent; }
QLabel.memoryCardMeta { color: %MUTED%; font-size: 10px; background: transparent; }
QPushButton.actionButton[tone="approve"] { color: %OKTEXT%; }
QPushButton.actionButton[tone="deny"] { color: %ERRTEXT%; }
QPushButton.actionButton[tone="danger"] { border-color: %ERRLINE%; color: %ERRTEXT%; }

QFrame.memoryTool { border: 1px solid %LINE%; border-radius: 6px; background: %INSET%; }
QWidget.memoryToolHead { background: transparent; }
QWidget.memoryToolHead:hover { background: %SURFACE2%; }
QWidget.memoryToolHead:focus { outline: 1px solid %CYAN%; outline-offset: -1px; }
QLabel.memoryToolName { color: %CYAN%; font-family: %UI%; font-size: 11px; background: transparent; }
QLabel.memoryToolMeta { color: %MUTED2%; font-size: 10px; background: transparent; }
QWidget.memoryToolBody { background: transparent; border-top: 1px solid %LINE%; }
QWidget.memoryField { background: transparent; border-bottom: 1px solid %LINE%; }
QLabel.memoryFieldName { color: %TEXTBRIGHT%; font-family: %UI%; font-size: 11px; background: transparent; }
QLabel.memoryFieldSep { color: %MUTED2%; font-size: 11px; background: transparent; }
QLabel.memoryFieldValue { color: %CYAN%; font-family: %UI%; font-size: 11px; background: transparent; }
QLabel.memoryFieldType { color: %MUTED2%; font-size: 10px; background: transparent; }
QLineEdit.memoryFieldInput {
    border: 1px solid %LINESTRONG%; border-radius: 4px; background: %INSET%; color: %TEXTBRIGHT%;
    font-family: %UI%; font-size: 11px; padding: 3px 7px;
    selection-background-color: %CYANDARK%;
}
QLineEdit.memoryFieldInput:focus { border-color: %CYAN%; }
QLineEdit.memoryFieldInput[invalid="true"] { border-color: %ERRLINE%; background: %ERRTINT%; }

QPushButton.linkButton {
    border: 0; background: transparent; color: %CYAN%; font-size: 11px; padding: 2px 4px;
}
QPushButton.linkButton:hover { color: %CYANMID%; }
QPushButton.linkButton[tone="approve"] { color: %OKTEXT%; }
QPushButton.linkButton[tone="danger"] { color: %ERRTEXT%; }
QPushButton.linkButton:disabled { color: %MUTED2%; }
QPushButton.linkButton:focus { outline: 1px solid %CYAN%; outline-offset: -1px; }

/* ===== 设置中心 · 知识库页（.kb-*，对应 webui #panel-kb / app.js kb*） ===== */
QScrollArea#kbPanelScroll { border: 0; background: transparent; }
QWidget#kbPanelContent { background: transparent; }
/* 滚动区的 viewport 与内容层：QAbstractScrollArea 的 viewport 默认按 Base 填充，
   不押回透明就会在深色皮肤下露出一块白底（原来的局部样式表靠一条无选择器的
   background: transparent 顺带盖住了所有子孙件，搬进全局表后得逐层写清）。 */
QWidget#kbPanelViewport, QWidget#kbTreeViewport, QWidget#kbDocsViewport { background: transparent; }
QWidget.kbTreeInner, QWidget.kbDocsInner, QWidget.kbActions { background: transparent; }

QLabel.kbStatus {
    border: 1px solid %LINE%; border-radius: 4px; background: %INSET%;
    color: %MUTED%; font-size: 11px; padding: 8px 10px;
}
QLabel.kbStatus[tone="error"] { border-color: %ERRLINE%; background: %ERRTINT%; color: %ERRTEXT%; }
QLabel.kbStatus[tone="warn"] { border-color: %WARNLINE%; background: %WARNTINT%; color: %WARNTEXT%; }

QWidget.kbLibBar { border: 1px solid %LINE%; border-radius: 4px; background: %BAND3%; }
QLabel.kbLibBarLabel { color: %MUTED%; font-size: 10px; background: transparent; }
QComboBox.kbLibSelect {
    border: 1px solid %LINE%; border-radius: 4px; background: %INSET%; color: %TEXT%;
    font-size: 12px; padding: 0 8px; min-height: 28px;
}
QComboBox.kbLibSelect:focus { border-color: %CYAN%; }
QComboBox.kbLibSelect:disabled { color: %MUTED2%; }
QComboBox.kbLibSelect::drop-down { border: 0; width: 20px; }
/* Qt 的 ::down-arrow 只认 image：CSS 那套 border 三角会画成一个实心方块 */
QComboBox.kbLibSelect::down-arrow {
    image: url(:/icons/chevron-down-muted.svg); width: 10px; height: 10px; margin-right: 6px;
}

QLineEdit.kbInput {
    border: 1px solid %LINE%; border-radius: 4px; background: %INSET%; color: %TEXT%;
    font-size: 12px; padding: 0 9px; min-height: 28px;
    selection-background-color: %CYANDARK%;
}
QLineEdit.kbInput:focus { border-color: %CYAN%; }
QLineEdit.kbInput:disabled { color: %MUTED2%; }

QFrame.kbCard { border: 1px solid %LINE%; border-radius: 4px; background: %BAND2%; }
QFrame.kbCard[warn="true"] { border-color: %WARNLINE%; background: %WARNTINT%; }
QLabel.kbCardLabel { color: %MUTED%; font-size: 10px; background: transparent; }
QLabel.kbCardValue { color: %TEXTBRIGHT%; font-size: 20px; font-weight: 600; background: transparent; }
QLabel.kbCardNote { color: %MUTED2%; font-size: 10px; background: transparent; }

QFrame.kbSection { border: 1px solid %LINE%; border-radius: 4px; background: %BAND2%; }
QLabel.kbSectionTitle { color: %TEXTBRIGHT%; font-size: 15px; font-weight: 600; background: transparent; }
QLabel.kbCount { color: %MUTED2%; font-size: 11px; background: transparent; }
QLabel.kbHint { color: %MUTED2%; font-size: 11px; background: transparent; }
QLabel.kbEmpty { color: %MUTED2%; font-size: 11px; background: transparent; }

QFrame.kbTree { border: 1px solid %LINE%; border-radius: 4px; background: %BAND2%; }
QWidget.kbTreeHead { background: %BAND3%; border-bottom: 1px solid %LINE%; }
QWidget.kbTreeFoot { background: %BAND3%; border-top: 1px solid %LINE%; }
QScrollArea#kbTreeScroll, QScrollArea#kbDocsScroll { border: 0; background: transparent; }

QPushButton.kbTreeBtn {
    border: 1px solid %LINE%; border-radius: 4px; background: %SURFACE2%; color: %TEXT%;
    font-size: 11px; min-height: 22px; padding: 0 7px;
}
QPushButton.kbTreeBtn:hover:enabled { border-color: %CYAN%; color: %CYAN%; }
QPushButton.kbTreeBtn:disabled { color: %MUTED2%; background: %BAND2%; border-color: %LINE%; }
QPushButton.kbTreeBtn[danger="true"]:hover:enabled {
    border-color: %ERRLINE%; color: %ERRTEXT%; background: %ERRTINT%;
}

QWidget.kbTreeRow { background: transparent; border: 1px solid transparent; border-radius: 4px; }
QWidget.kbTreeRow:hover { background: %ACCENTTINT%; }
QWidget.kbTreeRow[active="true"] { border-color: %CYANMID%; background: %ACCENTTINT%; }
QWidget.kbTreeRow[dropTarget="true"] { border: 1px dashed %CYAN%; background: %ACCENTTINT%; }
QWidget.kbTreeRow:focus { outline: 1px solid %CYAN%; outline-offset: -1px; }
QLabel.kbTreeTwisty { color: %MUTED2%; font-size: 9px; background: transparent; }
QLabel.kbTreeName { color: %TEXT%; font-size: 11px; background: transparent; }
QWidget.kbTreeRow[active="true"] QLabel.kbTreeName { color: %CYAN%; }
QLabel.kbTreeCount {
    background: %SURFACE3%; color: %MUTED%; font-size: 9px; border-radius: 8px; padding: 0 5px;
}
QWidget.kbTreeRow[dropTarget="true"] QLabel.kbTreeCount { background: %CYANDARK%; color: #ffffff; }
QLabel.kbTreeEmpty { color: %MUTED2%; font-size: 10px; background: transparent; }

QWidget.kbDoc { border: 1px solid %LINE%; border-radius: 4px; background: %INSET%; }
QWidget.kbDoc:hover { border-color: %LINESTRONG%; }
QWidget.kbDoc[failed="true"] { border-color: %ERRLINE%; background: %ERRTINT%; }
QWidget.kbDoc[dragging="true"] { border-color: %LINESTRONG%; background: %BAND3%; }
QLabel.kbDocGrip { color: %MUTED2%; font-size: 10px; background: transparent; }
QLabel.kbDocName { color: %TEXTBRIGHT%; font-size: 12px; font-weight: 600; background: transparent; }
QWidget.kbDoc[failed="true"] QLabel.kbDocName { color: %ERRTEXT%; }
QLabel.kbDocMeta { color: %MUTED%; font-size: 10px; background: transparent; }
QWidget.kbDoc[failed="true"] QLabel.kbDocMeta { color: %ERRTEXT%; }

QPushButton.kbOp {
    border: 1px solid %LINE%; border-radius: 4px; background: %SURFACE2%; color: %TEXT%;
    font-size: 11px; min-height: 24px; padding: 0 10px;
}
QPushButton.kbOp:hover:enabled { border-color: %CYAN%; color: %CYAN%; }
QPushButton.kbOp:disabled { color: %MUTED2%; }
QPushButton.kbOp[danger="true"] { color: %ERRTEXT%; border-color: %ERRLINE%; background: %ERRTINT%; }
QPushButton.kbOp[danger="true"]:hover:enabled { border-color: %RED%; }

QLabel.kbResultHead { color: %MUTED%; font-size: 10px; font-weight: 700; background: transparent; }
QFrame.kbResultCard { border: 1px solid %LINE%; border-radius: 4px; background: %INSET%; }
QLabel.kbResultTitle { color: %TEXTBRIGHT%; font-size: 12px; font-weight: 600; background: transparent; }
QLabel.kbResultTag {
    border: 1px solid %LINE%; border-radius: 8px; color: %MUTED%; font-size: 10px;
    padding: 0 6px; background: transparent;
}
QLabel.kbResultEm { color: %CYAN%; font-size: 11px; background: transparent; }
QLabel.kbResultScore { color: %CYAN%; font-size: 11px; font-weight: 500; background: transparent; }
QLabel.kbResultBody { color: %TEXT%; font-size: 11px; background: transparent; }
QLabel.kbResultSrc { color: %MUTED2%; font-size: 10px; background: transparent; }

/* ===== 确认对话框 ===== */
QDialog#confirmDialog { background: %PANEL%; border: 1px solid %LINESTRONG%; border-radius: 12px; }
QLabel#confirmTitle { color: %TEXTBRIGHT%; font-size: 15px; font-weight: 700; background: transparent; }
QLabel#confirmMessage { color: %MUTED%; font-size: 12px; background: transparent; }
)QSS";

QString appStyleSheet()
{
    const Palette &p = g_palette;
    QString out = QLatin1String(kSheetTemplate);

    struct Token { const char *name; QString value; };
    const Token tokens[] = {
        { "BG", cssColor(p.bg) },
        { "SURFACE", cssColor(p.surface) },
        { "SURFACE2", cssColor(p.surface2) },
        { "SURFACE3", cssColor(p.surface3) },
        { "LINE", cssColor(p.line) },
        { "LINESTRONG", cssColor(p.lineStrong) },
        { "TEXT", cssColor(p.text) },
        { "MUTED2", cssColor(p.muted2) },
        { "MUTED", cssColor(p.muted) },
        { "TEXTBRIGHT", cssColor(p.textBright) },
        { "CYANGLOW", cssColor(p.cyanGlow) },
        { "CYANDARK", cssColor(p.cyanDark) },
        { "CYANMID", cssColor(p.cyanMid) },
        { "CYAN", cssColor(p.cyan) },
        { "GREEN", cssColor(p.green) },
        { "ORANGE", cssColor(p.orange) },
        { "RED", cssColor(p.red) },
        { "BGGRADA", cssColor(p.bgGradA) },
        { "BAND1", cssColor(p.band1) },
        { "BAND2", cssColor(p.band2) },
        { "BAND3", cssColor(p.band3) },
        { "PANEL", cssColor(p.panel) },
        { "INSET", cssColor(p.inset) },
        { "BUBBLEUSER", cssColor(p.bubbleUser) },
        { "BUBBLE", cssColor(p.bubble) },
        { "ACCENTTINT", cssColor(p.accentTint) },
        { "OKTINT", cssColor(p.okTint) },
        { "OKTEXT", cssColor(p.okText) },
        { "OKLINE", cssColor(p.okLine) },
        { "ERRLINE", cssColor(p.errLine) },
        { "ERRTINT", cssColor(p.errTint) },
        { "ERRSTRONG", cssColor(p.errStrong) },
        { "ERRTEXT", cssColor(p.errText) },
        { "WARNLINE", cssColor(p.warnLine) },
        { "WARNTINT", cssColor(p.warnTint) },
        { "WARNTEXT", cssColor(p.warnText) },
        { "SCROLLTRACK", cssColor(p.scrollTrack) },
        { "SCROLLTHUMBHOVER", cssColor(p.scrollThumbHover) },
        { "SCROLLTHUMB", cssColor(p.scrollThumb) },
        { "SCRIM2", cssColor(p.scrim2) },
        { "SCRIM", cssColor(p.scrim) },
        { "GLASSBG2", cssColor(p.glassBg2) },
        { "GLASSBG", cssColor(p.glassBg) },
        { "CARDUSERGRAD2", cssColor(p.cardUserGrad2) },
        { "CARDUSERGRAD1", cssColor(p.cardUserGrad1) },
        { "CARDGRAD2", cssColor(p.cardGrad2) },
        { "CARDGRAD1", cssColor(p.cardGrad1) },
        { "ASSISTANTLINE", cssColor(p.assistantLine) },
        { "ASSISTANTTINT", cssColor(p.assistantTint) },
        { "ASSISTANTTEXT", cssColor(p.assistantText) },
        { "BODYTEXT", cssColor(p.bodyText) },
        { "CARDSOLID2", cssColor(p.cardSolid2) },
        { "CARDSOLID", cssColor(p.cardSolid) },
        { "INPUTBG", cssColor(p.inputBg) },
        { "GLASSSOLID", cssColor(p.glassSolid) },
        { "CODETEXT", cssColor(p.codeText) },
        { "HAIRLINE", cssColor(p.hairline) },
        { "HOVERTINT", cssColor(p.hoverTint) },
        { "MONO", monoFont() },
        { "UI", uiFont() },
    };
    for (const Token &token : tokens) {
        out.replace(QLatin1Char('%') + QLatin1String(token.name) + QLatin1Char('%'), token.value);
    }

    // 只写 font-size 的 QSS 规则会让 Qt 按「应用字体」重建字体族——中文 Windows 上是宋体
    // （SimSun），与 webui 的 :root{font-family:"Microsoft YaHei",…} 不一致：
    // 拉丁字形变宽（实测同一串模型名宽 30px）、行盒变矮（会话行 13/11px vs webui 18/16px）。
    // 这里给每条声明了字号却没有字体族的规则补上 %UI%，等价于 webui 的字体继承。
    {
        static const QRegularExpression blockRe(QStringLiteral("\\{[^{}]*\\}"));
        QString patched;
        int pos = 0;
        QRegularExpressionMatch m;
        while ((m = blockRe.match(out, pos)).hasMatch()) {
            patched += out.mid(pos, m.capturedStart() - pos);
            const QString block = m.captured(0);
            if (block.contains(QLatin1String("font-size:"))
                && !block.contains(QLatin1String("font-family:"))) {
                QString body = block;
                body.chop(1); // 去掉 '}'
                if (!body.endsWith(QLatin1Char(';')) && !body.endsWith(QLatin1Char(' ')))
                    body += QLatin1Char(';');
                patched += body + QLatin1String(" font-family: ") + uiFont() + QLatin1String("; }");
            } else {
                patched += block;
            }
            pos = m.capturedEnd();
        }
        patched += out.mid(pos);
        out = patched;
    }

    // 缩放系数 ≠ 1 时把所有 font-size: Npx 按比例放大（按「皮肤 + 缩放档」缓存，两者都是离散值）
    const qreal factor = zoomFactor();
    if (!qFuzzyCompare(factor, 1.0)) {
        static QHash<QString, QString> cache;
        const QString cacheKey = g_themeId + QLatin1Char('|') + QString::number(factor);
        auto it = cache.constFind(cacheKey);
        if (it == cache.constEnd()) {
            static const QRegularExpression re(QStringLiteral("font-size:\\s*(\\d+)px"));
            QString scaled;
            int pos = 0;
            QRegularExpressionMatch m;
            while ((m = re.match(out, pos)).hasMatch()) {
                scaled += out.mid(pos, m.capturedStart() - pos);
                scaled += QStringLiteral("font-size: %1px").arg(scaledPx(m.captured(1).toInt()));
                pos = m.capturedEnd();
            }
            scaled += out.mid(pos);
            it = cache.insert(cacheKey, scaled);
        }
        return it.value();
    }
    return out;
}

} // namespace gs

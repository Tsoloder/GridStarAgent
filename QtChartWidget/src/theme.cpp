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

// 三套皮肤：逐项对应 webui/style.css 的 :root[data-theme=...] 变量表
Palette makeDark()
{
    Palette p;
    p.dark = true;
    p.bg = QColor("#10171e");
    p.surface = QColor("#17222c");
    p.surface2 = QColor("#1b2934");
    p.surface3 = QColor("#223541");
    p.line = QColor("#314653");
    p.lineStrong = QColor("#456171");
    p.text = QColor("#d7e2e8");
    p.muted = QColor("#8499a6");
    p.muted2 = QColor("#78909d");
    p.textBright = QColor("#eaf6fa");
    p.cyan = QColor("#50badf");
    p.cyanDark = QColor("#126c91");
    p.cyanMid = QColor("#238fb9");
    p.cyanGlow = QColor(0x50, 0xba, 0xdf, 0x66);
    p.green = QColor("#50ce91");
    p.orange = QColor("#e7a84d");
    p.red = QColor("#e36c6c");
    p.bgGradA = QColor("#15212a");
    p.band1 = QColor("#18242e");
    p.band2 = QColor("#151f28");
    p.band3 = QColor("#121c24");
    p.panel = QColor("#121d24");
    p.inset = QColor("#0f181f");
    p.bubble = QColor("#1a2933");
    p.bubbleUser = QColor("#233744");
    p.accentTint = QColor("#173b4b");
    p.okLine = QColor("#2f604b");
    p.okTint = QColor("#14281f");
    p.okText = QColor("#8fd0af");
    p.errLine = QColor("#89484b");
    p.errTint = QColor("#3a2528");
    p.errStrong = QColor("#7d353a");
    p.errText = QColor("#ffb5b5");
    p.warnLine = QColor("#7a5a2e");
    p.warnTint = QColor("#332b20");
    p.warnText = QColor("#eac887");
    p.warnGlow = QColor(0xe7, 0xa8, 0x4d, 0x66);
    p.scrollThumb = QColor("#405866");
    p.scrollThumbHover = QColor("#4d6a7a");
    p.scrollTrack = QColor("#101a20");
    p.scrim = QColor(0x07, 0x10, 0x17, 0xdc);
    p.scrim2 = QColor(0x10, 0x17, 0x1e, 0xdb);
    p.glassBg = QColor(0x18, 0x24, 0x2e, 0xbd);
    p.glassBg2 = QColor(0x12, 0x1d, 0x24, 0xd1);
    p.cardGrad1 = QColor(0x26, 0x3e, 0x4e, 0x9e);
    p.cardGrad2 = QColor(0x14, 0x22, 0x2d, 0xc7);
    p.cardUserGrad1 = QColor(0x2e, 0x5c, 0x74, 0x99);
    p.cardUserGrad2 = QColor(0x18, 0x30, 0x3f, 0xc7);
    p.cardShadow = QColor(0x00, 0x00, 0x00, 0x66);
    p.assistantLine = QColor("#7a4a63");
    p.assistantTint = QColor("#33202c");
    p.assistantText = QColor("#e8a6c8");
    return p;
}

Palette makeSilver()
{
    Palette p;
    p.dark = false;
    p.bg = QColor("#eceef0");
    p.surface = QColor("#fdfdfe");
    p.surface2 = QColor("#f1f3f5");
    p.surface3 = QColor("#e4e7ea");
    p.line = QColor("#c7ccd2");
    p.lineStrong = QColor("#a7b0b9");
    p.text = QColor("#333a41");
    p.muted = QColor("#5b656e");
    p.muted2 = QColor("#79838c");
    p.textBright = QColor("#101418");
    p.cyan = QColor("#0e7ca6");
    p.cyanDark = QColor("#0b6d92");
    p.cyanMid = QColor("#2b93bd");
    p.cyanGlow = QColor(0x0e, 0x7c, 0xa6, 0x59);
    p.green = QColor("#177a4c");
    p.orange = QColor("#96660f");
    p.red = QColor("#b23a3a");
    p.bgGradA = QColor("#f7f8f9");
    p.band1 = QColor("#fafbfc");
    p.band2 = QColor("#f1f3f5");
    p.band3 = QColor("#e9ecef");
    p.panel = QColor("#ffffff");
    p.inset = QColor("#ffffff");
    p.bubble = QColor("#ffffff");
    p.bubbleUser = QColor("#e4f1f8");
    p.accentTint = QColor("#dcecf4");
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
    p.warnGlow = QColor(0x96, 0x66, 0x0f, 0x55);
    p.scrollThumb = QColor("#b6bdc4");
    p.scrollThumbHover = QColor("#99a2ab");
    p.scrollTrack = QColor("#eef0f2");
    p.scrim = QColor(0x23, 0x28, 0x2d, 0x73);
    p.scrim2 = QColor(0xf0, 0xf2, 0xf4, 0xeb);
    p.glassBg = QColor(0xfa, 0xfb, 0xfc, 0xbd);
    p.glassBg2 = QColor(0xfd, 0xfd, 0xfe, 0xd1);
    p.cardGrad1 = QColor(0xff, 0xff, 0xff, 0xeb);
    p.cardGrad2 = QColor(0xf0, 0xf5, 0xf9, 0xdb);
    p.cardUserGrad1 = QColor(0xe4, 0xf1, 0xf8, 0xf0);
    p.cardUserGrad2 = QColor(0xd3, 0xe7, 0xf4, 0xe0);
    p.cardShadow = QColor(0x1e, 0x2a, 0x36, 0x21);
    p.assistantLine = QColor("#d3b0c8");
    p.assistantTint = QColor("#f9eff5");
    p.assistantText = QColor("#a75486");
    return p;
}

Palette makeBlue()
{
    Palette p;
    p.dark = false;
    p.bg = QColor("#dce7f3");
    p.surface = QColor("#f3f8fc");
    p.surface2 = QColor("#e3edf7");
    p.surface3 = QColor("#d4e1ee");
    p.line = QColor("#a9c1d9");
    p.lineStrong = QColor("#86a6c6");
    p.text = QColor("#2a415c");
    p.muted = QColor("#546c86");
    p.muted2 = QColor("#6d8399");
    p.textBright = QColor("#0f2740");
    p.cyan = QColor("#1b84a8");
    p.cyanDark = QColor("#166f92");
    p.cyanMid = QColor("#3d9dc0");
    p.cyanGlow = QColor(0x1b, 0x84, 0xa8, 0x59);
    p.green = QColor("#1c7a50");
    p.orange = QColor("#96660f");
    p.red = QColor("#b23a3a");
    p.bgGradA = QColor("#eaf2fa");
    p.band1 = QColor("#ecf3fb");
    p.band2 = QColor("#e0eaf5");
    p.band3 = QColor("#d6e2ef");
    p.panel = QColor("#f4f8fc");
    p.inset = QColor("#fbfdfe");
    p.bubble = QColor("#f5fafd");
    p.bubbleUser = QColor("#ddeaf5");
    p.accentTint = QColor("#d3e7f0");
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
    p.warnGlow = QColor(0x96, 0x66, 0x0f, 0x55);
    p.scrollThumb = QColor("#a9bccd");
    p.scrollThumbHover = QColor("#8ba2b8");
    p.scrollTrack = QColor("#e7eef6");
    p.scrim = QColor(0x1c, 0x30, 0x44, 0x73);
    p.scrim2 = QColor(0xdc, 0xe7, 0xf3, 0xeb);
    p.glassBg = QColor(0xec, 0xf3, 0xfb, 0xbd);
    p.glassBg2 = QColor(0xf4, 0xf8, 0xfc, 0xd1);
    p.cardGrad1 = QColor(0xfa, 0xfd, 0xff, 0xeb);
    p.cardGrad2 = QColor(0xe8, 0xf3, 0xfb, 0xdb);
    p.cardUserGrad1 = QColor(0xd8, 0xec, 0xfa, 0xf0);
    p.cardUserGrad2 = QColor(0xc7, 0xe2, 0xf4, 0xe0);
    p.cardShadow = QColor(0x1c, 0x30, 0x44, 0x26);
    p.assistantLine = QColor("#d5b3cb");
    p.assistantTint = QColor("#f8eff5");
    p.assistantText = QColor("#9d5483");
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
        const QStringList want{QStringLiteral("Bahnschrift"),
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
            cached = QStringLiteral("Microsoft YaHei UI");
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
        { "phaseCount", 0.5 },       // .phase-head small
        { "modelGroupLabel", 1.2 },  // .model-group-label
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

/* ===== 顶栏（玻璃底） ===== */
QWidget#topbar { background: %GLASSBG%; border-bottom: 1px solid %LINE%; }
QLabel#brandText { color: %TEXTBRIGHT%; font-size: 14px; font-weight: 700; background: transparent; }
QLabel#brandMark { background: transparent; }
QPushButton#connection { border: 0; background: transparent; padding: 6px; }
QPushButton#connection:hover { background: %SURFACE2%; }

/* 皮肤切换（.theme-switch）：幽灵触发器 + 下拉 */
QWidget#themeSwitch { background: transparent; }
QWidget#themeTrigger {
    border: 1px solid transparent; background: transparent; border-radius: 4px;
}
QWidget#themeTrigger:hover { border: 1px solid %LINESTRONG%; background: %SURFACE2%; }
QLabel#themeLabel { color: %MUTED%; font-size: 11px; background: transparent; }
QWidget#themeTrigger:hover QLabel#themeLabel { color: %CYAN%; }
QFrame#themeListbox { border: 1px solid %LINESTRONG%; background: %PANEL%; border-radius: 12px; }
QPushButton.themeOption {
    border: 1px solid transparent; background: transparent; color: %TEXT%;
    font-size: 11px; text-align: left; padding: 7px 8px; border-radius: 4px;
}
QPushButton.themeOption:hover { border-color: %CYANMID%; background: %ACCENTTINT%; }
QPushButton.themeOption[active="true"] { color: %CYAN%; }

/* ===== 会话栏 ===== */
QWidget#sessionbar { background: %BAND2%; border-bottom: 1px solid %LINE%; }
QPushButton[variant="primary"] {
    border: 1px solid %CYANMID%; background: %CYANDARK%; color: %TEXTBRIGHT%; border-radius: 4px;
}
QPushButton[variant="primary"]:hover { background: %CYANMID%; }
QPushButton[variant="primary"]:disabled { color: %MUTED%; background: %CYANDARK%; }
/* 新建对话：幽灵按钮（与附件/设置同族） */
QWidget#newSession {
    border: 1px solid transparent; background: transparent; color: %MUTED%; border-radius: 4px;
    padding: 0 10px;
}
QWidget#newSession:hover { border: 1px solid %LINESTRONG%; background: %SURFACE2%; color: %CYAN%; }
QWidget#sessionTrigger { border: 1px solid transparent; background: transparent; border-radius: 4px; }
QWidget#sessionTrigger:hover { border: 1px solid %LINE%; background: %SURFACE%; }
QLabel#currentTitle { color: %MUTED%; background: transparent; }
QLabel.chevronGlyph { color: %MUTED%; background: transparent; }

/* 视图切换（对话 / 轨迹） */
QWidget#viewTabs { background: %BAND3%; border-bottom: 1px solid %LINE%; }
QPushButton.viewTab {
    border: 0; border-bottom: 2px solid transparent; background: transparent;
    color: %MUTED%; padding: 0 18px; font-size: 12px;
}
QPushButton.viewTab:hover { color: %TEXT%; background: %BAND2%; }
QPushButton.viewTab[active="true"] { border-bottom: 2px solid %CYAN%; color: %TEXTBRIGHT%; }

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

/* ===== 消息区 ===== */
QScrollArea#messages { border: 0; background: transparent; }
QWidget#messageList { background: transparent; }
QWidget#emptyState { background: transparent; }
QLabel#emptySymbol {
    border: 1px solid %LINE%; color: %CYAN%; font-size: 23px; background: transparent;
    border-radius: 12px;
}
QLabel#emptyTitle { color: %TEXTBRIGHT%; font-size: 14px; font-weight: 700; background: transparent; }
QLabel#emptyHint { color: %MUTED%; font-size: 12px; background: transparent; }

QWidget.messageWidget { background: transparent; }
/* 一轮一张卡：正文 + 过程区 + 底部信息行；靠渐变浮层与一侧色条区分角色 */
QFrame#turnCard {
    border: 1px solid %LINE%; border-radius: 12px;
    background: qlineargradient(x1:0, y1:0, x2:0.55, y2:1,
                                stop:0 %CARDGRAD1%, stop:1 %CARDGRAD2%);
}
QFrame#turnCard[role="user"] {
    background: qlineargradient(x1:0, y1:0, x2:0.55, y2:1,
                                stop:0 %CARDUSERGRAD1%, stop:1 %CARDUSERGRAD2%);
}
QFrame#turnCard[error="true"] { border-color: %RED%; }
QFrame#bubble { border: 0; background: transparent; }
QLabel.messageLabel { color: %CYAN%; font-size: 10px; font-weight: 700; background: transparent; }

/* 底部信息行（.turn-foot）：左下复制，右下用量/用时/时间 */
QWidget#turnFoot { background: transparent; }
QPushButton.bubbleCopy {
    border: 1px solid transparent; background: transparent; color: %MUTED2%; border-radius: 4px;
}
QPushButton.bubbleCopy:hover { border-color: %LINESTRONG%; background: %SURFACE2%; color: %CYAN%; }
QLabel.bubbleMeta { color: %MUTED2%; font-size: 9px; background: transparent; }
QPushButton.bubblePill {
    border: 1px solid transparent; border-radius: 8px; background: %BAND3%; color: %MUTED%;
    font-size: 9px; padding: 1px 7px;
}
QPushButton.bubblePill:hover { border-color: %CYAN%; color: %CYAN%; }
QPushButton.bubblePill:disabled { color: %MUTED2%; }

/* 明细弹层（.bubble-*-pop） */
QFrame.bubblePop { border: 1px solid %LINE%; background: %GLASSBG2%; border-radius: 8px; }
QLabel.popTitle { color: %TEXT%; font-size: 11px; font-weight: 600; background: transparent; }
QLabel.popTotal { color: %TEXT%; font-size: 11px; background: transparent; font-family: %MONO%; }
QLabel.popRowLabel { color: %MUTED%; font-size: 11px; background: transparent; }
QLabel.popRowValue { color: %TEXTBRIGHT%; font-size: 11px; background: transparent; font-family: %MONO%; }

/* ===== 过程区（思考 / 工具调用）：扁平化，无底色无分隔线 ===== */
QWidget#turnProcess { background: transparent; }
QWidget.procRow { background: transparent; }
QWidget.procRow:hover { background: transparent; }
QLabel.procLabel { color: %MUTED%; font-size: 11px; font-weight: 500; background: transparent; }
QWidget.procRow[running="true"] QLabel.procLabel { color: %TEXTBRIGHT%; }
QLabel.procSum { color: %MUTED2%; font-size: 10px; background: transparent; font-family: %MONO%; }
QWidget.procRow[running="true"] QLabel.procSum { color: %CYAN%; }
QWidget.procRow[proc="tools"][running="true"] QLabel.procSum { color: %ORANGE%; }
QWidget.procRow[failed="true"] QLabel.procSum { color: %RED%; }
QWidget.procBody { background: transparent; }
QWidget.procRail {
    border-left: 1px solid %LINE%; background: transparent;
}
QLabel.procRailText { color: %MUTED%; font-size: 11px; background: transparent; }

/* ===== Markdown ===== */
QWidget.markdownView { background: transparent; }
QLabel.mdParagraph { color: %TEXT%; background: transparent; }
QLabel.mdHeading { color: %TEXTBRIGHT%; font-size: 14px; font-weight: 700; background: transparent; }
QLabel.mdList { color: %TEXT%; background: transparent; }
QFrame.codeBlock { border: 1px solid %LINE%; background: %INSET%; border-radius: 8px; }
QLabel.codeBlockHeader {
    color: %MUTED2%; font-size: 10px; background: transparent;
    border-bottom: 1px solid %LINE%; padding: 4px 8px;
}
QLabel.codeBlockBody {
    color: %TEXT%; background: transparent; padding: 9px;
    font-family: %MONO%; font-size: 11px;
}
QLabel.mdQuote {
    color: %MUTED%; background: transparent; border-left: 2px solid %CYANMID%; padding: 2px 0 2px 10px;
}
QFrame.mdTable { border: 1px solid %LINE%; background: %INSET%; border-radius: 4px; }
QLabel.mdTableHead { color: %TEXTBRIGHT%; font-size: 11px; font-weight: 600; background: %BAND3%; }
QLabel.mdTableCell { color: %TEXT%; font-size: 11px; background: transparent; }
QLabel.mdRule { background: %LINE%; }
QLabel.mdImage { border: 1px solid %LINE%; border-radius: 4px; background: transparent; }

/* ===== 工具调用 ===== */
QWidget.toolList { background: transparent; }
QWidget.toolItem { background: transparent; }
QWidget.toolItemSummary { background: transparent; }
QWidget.toolItemSummary:hover { background: transparent; }
QLabel.toolItemName { color: %TEXTBRIGHT%; font-size: 11px; background: transparent; }
QLabel.statusLabel { color: %ORANGE%; font-size: 10px; background: transparent; }
QLabel.statusLabel[status="succeeded"] { color: %GREEN%; }
QLabel.statusLabel[status="failed"] { color: %RED%; }
QLabel.statusLabel[status="cancelled"] { color: %RED%; }
QLabel.statusLabel[status="done"] { color: %GREEN%; }
QFrame.toolDetail { background: transparent; border: 0; }
QLabel.toolDetailLabel {
    color: %MUTED2%; font-size: 9px; font-weight: 700; background: transparent;
}
QPlainTextEdit.toolPre, QLabel.toolPre {
    color: %TEXT%; background: transparent; border: 0;
    font-family: %MONO%; font-size: 10px;
}
/* 工具参数表（.tool-args-table） */
QFrame#toolArgsBox { border: 1px solid %LINE%; background: %INSET%; border-radius: 6px; }
QLabel.toolArgsHead { color: %MUTED2%; font-size: 9px; font-weight: 600; background: %BAND2%; }
QLabel.toolArgsKey { color: %CYAN%; font-size: 10px; background: transparent; font-family: %MONO%; }
QLabel.toolArgsValue { color: %TEXT%; font-size: 10px; background: transparent; font-family: %MONO%; }
QLabel.toolArgsValue[json="true"] { color: %CYAN%; }
QLabel.toolArgsEmpty { color: %MUTED2%; font-size: 10px; background: transparent; }

/* ===== 结构化卡片 / 工作流 / 审批 ===== */
QFrame.structured { border: 1px solid %LINESTRONG%; background: %BAND2%; border-radius: 8px; }
QLabel.structuredTitle {
    color: %TEXTBRIGHT%; font-weight: 700; background: transparent;
    border-bottom: 1px solid %LINE%; padding: 7px 8px;
}
QPushButton[variant="option"] {
    border: 1px solid %LINESTRONG%; border-radius: 4px; background: %SURFACE2%; color: %TEXT%;
    min-height: 28px; padding: 5px 9px;
}
QPushButton[variant="option"]:hover { border-color: %CYANMID%; }
QPushButton[variant="option"]:disabled { color: %MUTED2%; }
QPushButton[variant="optionPrimary"] {
    border: 1px solid %CYANMID%; border-radius: 4px; background: %CYANDARK%; color: %TEXTBRIGHT%;
    min-height: 28px; padding: 5px 9px;
}
QPushButton[variant="optionPrimary"]:hover { background: %CYANMID%; }
QPushButton[variant="optionPrimary"]:disabled { color: %MUTED%; }
QPushButton[variant="optionDanger"] {
    border: 1px solid %ERRLINE%; border-radius: 4px; background: %ERRTINT%; color: %ERRTEXT%;
    min-height: 28px; padding: 5px 9px;
}
QPushButton[variant="optionDanger"]:hover { background: %ERRTINT%; border-color: %ERRSTRONG%; }
QPushButton.actionButton {
    border: 1px solid %LINESTRONG%; border-radius: 4px; background: %SURFACE2%; color: %TEXT%;
    min-height: 30px; min-width: 72px; padding: 5px 9px;
}
QPushButton.actionButton:hover { border-color: %CYANMID%; }
QPushButton.actionButton:disabled { color: %MUTED2%; }
QLabel.paramName { color: %TEXT%; font-size: 12px; background: transparent; }
QLabel.paramDesc { color: %MUTED%; font-size: 11px; background: transparent; }
QLineEdit.paramInput, QPlainTextEdit.paramInput {
    border: 1px solid %LINE%; background: %INSET%; color: %TEXT%; padding: 5px 7px; border-radius: 4px;
    selection-background-color: %CYANDARK%;
}
QLineEdit.paramInput:focus, QPlainTextEdit.paramInput:focus { border-color: %CYAN%; }
QLineEdit.paramInput:disabled, QPlainTextEdit.paramInput:disabled { color: %MUTED2%; }
QTextEdit.paramInput {
    border: 1px solid %LINE%; background: %INSET%; color: %TEXT%; padding: 5px 7px; border-radius: 4px;
    selection-background-color: %CYANDARK%;
}
QTextEdit.paramInput:focus { border-color: %CYAN%; }

QFrame.workflowCard, QFrame.approvalCard { border: 1px solid %LINE%; background: %BAND2%; border-radius: 8px; }
QWidget.cardHead { background: transparent; border-bottom: 1px solid %LINE%; }
QLabel.cardTitle { color: %TEXT%; font-weight: 700; background: transparent; }
QLabel.toolBody {
    color: %MUTED%; background: transparent; padding: 7px 8px;
    font-family: %MONO%; font-size: 11px;
}

/* ===== 选择 / 审批浮层（.choice-overlay，盖住输入框） ===== */
QFrame#choiceCard {
    border: 1px solid %LINESTRONG%; border-radius: 12px; background: %BAND2%;
}
QWidget#choiceHead { background: transparent; }
QWidget#choiceHead:hover { background: %ACCENTTINT%; }
QLabel.choiceTitle { color: %MUTED2%; font-size: 10px; background: transparent; }
QPushButton#choiceClose {
    border: 0; background: transparent; color: %MUTED%; font-size: 15px; border-radius: 4px;
}
QPushButton#choiceClose:hover { background: %SURFACE3%; color: %TEXTBRIGHT%; }
QLabel.choiceQuestion { color: %TEXTBRIGHT%; font-size: 13px; background: transparent; }
QWidget.choiceItem { background: transparent; border: 1px solid transparent; border-radius: 8px; }
QWidget.choiceItem:hover { background: %BAND3%; }
QWidget.choiceItem[selected="true"] { border: 1px solid %CYANMID%; background: %ACCENTTINT%; }
QLabel.choiceItemName { color: %TEXTBRIGHT%; font-size: 12px; font-weight: 600; background: transparent; }
QLabel.choiceItemDesc { color: %MUTED2%; font-size: 11px; background: transparent; }
QLineEdit#choiceInput {
    border: 1px solid %LINE%; border-radius: 8px; background: %INSET%; color: %TEXT%; padding: 0 9px;
    selection-background-color: %CYANDARK%;
}
QLineEdit#choiceInput:focus { border-color: %CYANMID%; }
QLineEdit#choiceInput[invalid="true"] { border-color: %ERRLINE%; background: %ERRTINT%; }
QPushButton#choiceSubmit {
    border: 1px solid %LINESTRONG%; border-radius: 8px; background: %SURFACE3%; color: %TEXTBRIGHT%;
    font-size: 12px; padding: 0 12px;
}
QPushButton#choiceSubmit:hover:not(:disabled) { border-color: %CYANMID%; background: %ACCENTTINT%; }
QPushButton#choiceSubmit:disabled { color: %MUTED2%; }
QLabel.choiceHint { color: %MUTED2%; font-size: 9px; background: transparent; }
QLabel.choiceCaret { color: %MUTED2%; font-size: 11px; background: transparent; }

/* ===== 阶段计划面板 ===== */
QFrame#phasePanel {
    border: 1px solid %LINESTRONG%; border-radius: 12px;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %SURFACE%, stop:1 %BAND2%);
}
QWidget#phaseHead { background: transparent; border-bottom: 1px solid transparent; }
QWidget#phaseHead:hover { background: %ACCENTTINT%; }
QWidget#phaseHead[expanded="true"] { background: %ACCENTTINT%; border-bottom: 1px solid %LINE%; }
QLabel#phaseTitle { color: %TEXT%; font-size: 12px; font-weight: 700; background: transparent; }
QLabel#phaseCount {
    border: 1px solid %CYANMID%; border-radius: 9px; background: %INSET%; color: %CYAN%;
    font-size: 10px; font-weight: 700; padding: 1px 8px;
}
QLabel.phaseChevron { color: %MUTED2%; font-size: 10px; background: transparent; }
QLabel.phaseChevron[expanded="true"] { color: %CYAN%; }
QScrollArea#phaseSteps { background: %BAND3%; border: 0; }
QWidget#phaseStepsInner { background: %BAND3%; }

/* ===== 输入区 ===== */
QWidget#composer { background: transparent; }
QLabel#configWarning {
    background: %WARNTINT%; color: %WARNTEXT%; font-size: 11px;
    border-left: 2px solid %ORANGE%; padding: 6px 7px;
}
QWidget#controls { background: transparent; }
QWidget#modeSwitch { background: transparent; }
QPushButton.modeButton {
    border: 1px solid transparent; background: transparent; color: %MUTED%; font-size: 11px;
    padding: 0 7px; border-radius: 4px; min-height: 26px;
}
QPushButton.modeButton:hover { background: %SURFACE2%; color: %TEXT%; }
QPushButton.modeButton[active="true"] { border: 1px solid %CYANMID%; background: %ACCENTTINT%; color: %CYAN%; }
QFrame.modelControl { border: 1px solid transparent; background: transparent; border-radius: 4px; }
QFrame.modelControl:hover { border: 1px solid %LINESTRONG%; background: %SURFACE2%; }
QWidget.comboTrigger { background: transparent; border: 0; }
QLabel.comboText { color: %MUTED%; font-size: 11px; background: transparent; }
QPushButton#settingsButton {
    border: 1px solid transparent; background: transparent; color: %MUTED2%; border-radius: 4px; font-size: 13px;
}
QPushButton#settingsButton:hover { border-color: %LINESTRONG%; background: %SURFACE2%; color: %CYAN%; }
QPushButton#attachButton, QPushButton#voiceButton {
    border: 1px solid transparent; background: transparent; color: %MUTED2%; border-radius: 4px;
}
QPushButton#attachButton:hover, QPushButton#voiceButton:hover:!disabled {
    border-color: %LINESTRONG%; background: %SURFACE2%; color: %CYAN%;
}
QPushButton#voiceButton:disabled { color: %MUTED2%; }
QPushButton#voiceButton[recording="true"] {
    background: %ERRSTRONG%; border-color: %ERRLINE%; color: #ffffff;
}
QWidget#attachBar { background: transparent; }
QFrame.attachChip {
    border: 1px solid %LINESTRONG%; border-radius: 4px; background: %SURFACE2%;
}
QLabel.attachExt {
    background: %SURFACE3%; color: %CYAN%; font-size: 9px; font-weight: 700;
    border-radius: 4px; padding: 1px 4px;
}
QLabel.attachName { color: %TEXT%; font-size: 10px; background: transparent; }
QLabel.attachChipThumb { border-radius: 4px; background: %INSET%; }
QLabel.attachSize { color: %MUTED2%; font-size: 10px; background: transparent; }
QPushButton.attachRemove {
    border: 0; background: transparent; color: %MUTED%; font-size: 12px; border-radius: 4px;
}
QPushButton.attachRemove:hover { background: %ERRSTRONG%; color: #ffffff; }

/* 气泡内已发送附件 */
QWidget.bubbleAttachments { background: transparent; }
QFrame.attachFile {
    border: 1px solid %LINE%; border-radius: 4px; background: %INSET%;
}
QLabel.attachThumb {
    border: 1px solid %LINE%; border-radius: 4px; background: %INSET%;
}

QFrame#inputWrap { border: 1px solid %LINESTRONG%; background: %INSET%; border-radius: 8px; }
QFrame#inputWrap[focus="true"] { border-color: %CYANMID%; }
QTextEdit#messageInput {
    border: 0; background: transparent; color: %TEXT%; padding: 10px 12px 6px 12px;
    selection-background-color: %CYANDARK%;
}
QTextEdit#messageInput > QWidget { background: transparent; }
QPushButton#sendButton {
    border: 1px solid %CYANMID%; border-radius: 4px; background: %CYANDARK%; color: #ffffff; font-size: 16px;
}
QPushButton#sendButton:hover { background: %CYANMID%; }
QPushButton#sendButton:disabled { color: %MUTED%; background: %CYANDARK%; }
QPushButton#sendButton[stop="true"] { border-color: %ERRLINE%; background: %ERRSTRONG%; font-size: 11px; }
QLabel#busyLabel { color: %MUTED2%; font-size: 10px; background: transparent; }

/* ===== 下拉列表（模型 / Skill） ===== */
QWidget#listbox { border: 1px solid %LINESTRONG%; background: %INSET%; border-radius: 12px; }
QScrollArea#listboxScroll { border: 0; background: transparent; }
QWidget#listboxInner { background: transparent; }
QLabel.modelGroupLabel {
    color: %MUTED%; font-size: 10px; font-weight: 700; background: transparent; padding: 7px 8px 4px 8px;
}
QWidget.modelOption { background: transparent; border: 1px solid transparent; border-radius: 4px; }
QWidget.modelOption:hover, QWidget.modelOption[focused="true"] {
    border: 1px solid %CYANMID%; background: %ACCENTTINT%;
}
QWidget.modelOption[selected="true"] { background: %ACCENTTINT%; }
QLabel.modelCheck { color: %CYAN%; font-size: 11px; background: transparent; }
QLabel.modelName { color: %TEXT%; font-size: 12px; background: transparent; }
QLabel.modelId { color: %MUTED%; font-size: 10px; background: transparent; font-family: %MONO%; }
QLabel.listboxEmpty { color: %MUTED%; font-size: 12px; background: transparent; padding: 16px; }

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

/* ===== 轮次导航轨（.turn-rail） ===== */
QWidget#turnRail {
    background: transparent; border: 0;
}
QWidget.turnRailDot { background: transparent; border: 0; }
QFrame#turnRailTip {
    border: 1px solid %LINESTRONG%; border-radius: 4px; background: %PANEL%;
}
QLabel#turnRailTipText { color: %TEXT%; font-size: 11px; background: transparent; }

/* ===== 轨迹视图 ===== */
QWidget#trajectoryView { background: transparent; }
QWidget#trajToolbar { background: %BAND2%; border-bottom: 1px solid %LINE%; }
QPushButton.trajViewButton {
    border: 1px solid transparent; background: transparent; color: %MUTED%; font-size: 11px;
    padding: 0 10px; min-height: 26px;
}
QPushButton.trajViewButton:hover:not([active="true"]) { background: %SURFACE2%; color: %TEXT%; }
QPushButton.trajViewButton[active="true"] { border: 1px solid %CYANMID%; background: %ACCENTTINT%; color: %CYAN%; }
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
QLabel.trajGroupMeta { color: %MUTED2%; font-size: 9px; background: transparent; font-family: %MONO%; }
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
QLabel.trajRowMeta { color: %MUTED2%; font-size: 9px; background: transparent; font-family: %MONO%; }
QWidget.trajRowBar { background: %INSET%; border: 1px solid %LINE%; border-radius: 8px; }
QLabel.trajEmpty { color: %MUTED2%; font-size: 11px; background: transparent; }
QLabel.trajInspPos { color: %MUTED%; font-size: 10px; background: transparent; font-family: %MONO%; }
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
    font-family: %MONO%; font-size: 10px; border-radius: 4px;
}

/* ===== 设置中心 ===== */
QDialog#settingsDialog { background: %GLASSBG2%; border: 1px solid %LINESTRONG%; border-radius: 12px; }
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
QLabel.providerId { color: %MUTED2%; font-size: 9px; background: transparent; font-family: %MONO%; }
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
QWidget.procRowHead:focus, QWidget.toolItemSummary:focus,
QWidget.choiceItem:focus, QWidget.sessionSelect:focus {
    outline: 1px solid %CYAN%; outline-offset: -1px;
}
QWidget#choiceHead:focus, QWidget#phaseHead:focus { outline: 1px solid %CYAN%; outline-offset: -1px; }
QLineEdit.settingsInput:disabled { color: %MUTED2%; background: %BAND3%; }
QComboBox.settingsInput::drop-down { border: 0; width: 20px; }
QComboBox.settingsInput::down-arrow { image: none; border-left: 4px solid transparent;
    border-right: 4px solid transparent; border-top: 5px solid %MUTED%; margin-right: 6px; }
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
QLabel.settingsModelId { color: %MUTED2%; font-size: 9px; background: transparent; font-family: %MONO%; }
QLabel.settingsModelApi { color: %CYAN%; font-size: 9px; background: transparent; font-family: %MONO%; }
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
QLabel.candidateId { color: %MUTED2%; font-size: 9px; background: transparent; font-family: %MONO%; }
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
    font-size: 10px; font-family: %MONO%; padding: 2px 6px;
}
QLabel.mcpParamDesc { color: %MUTED2%; font-size: 10px; background: transparent; }
QLabel.mcpParamType { color: %MUTED2%; font-size: 10px; background: transparent; }
QLabel.mcpParamRequired {
    border: 1px solid %WARNLINE%; border-radius: 8px; color: %ORANGE%; font-size: 9px; padding: 1px 5px;
}
QLabel.paramCode {
    border-radius: 4px; background: %INSET%; color: %CYAN%;
    font-size: 10px; font-family: %MONO%; padding: 1px 5px;
}
QLabel.skillMetaText { color: %MUTED2%; font-size: 10px; background: transparent; }
QWidget.mcpParamRow { background: transparent; border-bottom: 1px solid %LINE%; }
QWidget.mcpParamList { background: transparent; border-top: 1px solid %LINE%; padding: 2px 10px 9px 10px; }

QWidget#settingsActions { background: %BAND3%; border-top: 1px solid %LINE%; }
QLabel#settingsStatus { color: %WARNTEXT%; font-size: 11px; background: transparent; }

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
        { "ASSISTANTTINT", cssColor(p.assistantTint) },
        { "ASSISTANTTEXT", cssColor(p.assistantText) },
        { "MONO", monoFont() },
        { "UI", uiFont() },
    };
    for (const Token &token : tokens) {
        out.replace(QLatin1Char('%') + QLatin1String(token.name) + QLatin1Char('%'), token.value);
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
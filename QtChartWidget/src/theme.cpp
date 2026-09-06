#include "theme.h"

#include <QFontDatabase>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QStyle>
#include <QWidget>

namespace gs {

const QColor Bg("#10171e");
const QColor Surface("#17222c");
const QColor Surface2("#1b2934");
const QColor Surface3("#223541");
const QColor Line("#314653");
const QColor LineStrong("#456171");
const QColor Text("#d7e2e8");
const QColor Muted("#8499a6");
const QColor Cyan("#50badf");
const QColor CyanDark("#126c91");
const QColor Green("#50ce91");
const QColor Orange("#e7a84d");
const QColor Red("#e36c6c");

int basePixelSize() { return 13; }

namespace {
qreal g_zoomFactor = 1.0;
}

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

QString escapeHtml(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    out.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    out.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    out.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return out;
}

// style.css → Qt Style Sheet。Qt 不支持 letter-spacing / box-shadow / text-transform，
// 这类效果由自绘控件（commonwidgets.cpp）承担。
QString appStyleSheet()
{
    static const char *sheet = R"QSS(
/* ===== 基础 ===== */
QToolTip {
    color: #d7e2e8; background: #17222c; border: 1px solid #456171; padding: 3px 6px;
}
QWidget#appShell {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #15212a, stop:1 #101820);
}

/* ===== 滚动条（对应 scrollbar-color:#405866 #121b22） ===== */
QScrollBar:vertical { background: #101a20; width: 8px; margin: 0; border: 0; }
QScrollBar::handle:vertical { background: #405866; border-radius: 4px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: #4d6a7a; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; background: none; border: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: #101a20; }
QScrollBar:horizontal { background: #101a20; height: 8px; margin: 0; border: 0; }
QScrollBar::handle:horizontal { background: #405866; border-radius: 4px; min-width: 24px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; background: none; border: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: #101a20; }

/* ===== 顶栏 ===== */
QWidget#topbar { background: #18242e; border-bottom: 1px solid #314653; }
QLabel#brandText { color: #dff6ff; font-size: 14px; font-weight: 700; background: transparent; }
QLabel#brandMark {
    border: 1px solid #298aad; background: #123c50; color: #76d6f5; font-size: 10px; font-weight: 700;
}
QPushButton#connection { border: 0; background: transparent; padding: 6px; }
QPushButton#connection:hover { background: #1d2b35; }

/* ===== 会话栏 ===== */
QWidget#sessionbar { background: #151f28; border-bottom: 1px solid #314653; }
QPushButton[variant="primary"] {
    border: 1px solid #238fb9; background: #126c91; color: #f4fbfe; border-radius: 4px;
}
QPushButton[variant="primary"]:hover { background: #167da5; }
QPushButton[variant="primary"]:disabled { color: #9fc3d2; background: #14536e; }
QWidget#newSession {
    border: 1px solid #238fb9; background: #126c91; color: #f4fbfe; border-radius: 4px;
    padding: 0 10px;
}
QWidget#sessionTrigger { border: 1px solid transparent; background: transparent; }
QWidget#sessionTrigger:hover { border: 1px solid #314653; background: #17222c; }
QLabel#currentTitle { color: #a8bbc6; background: transparent; }
QLabel.chevronGlyph { color: #a8bbc6; background: transparent; }

/* ===== 会话面板 ===== */
QWidget#sessionPanel { border: 1px solid #456171; background: #17232c; }
QWidget#panelHead { background: transparent; border-bottom: 1px solid #314653; }
QLineEdit#sessionSearch {
    border: 1px solid #314653; background: #101820; color: #d7e2e8; padding: 0 8px; border-radius: 0;
    selection-background-color: #126c91;
}
QLineEdit#sessionSearch:focus { border-color: #50badf; }
QPushButton[variant="icon"] {
    border: 1px solid #314653; background: #1b2934; color: #d7e2e8; font-size: 16px; border-radius: 0;
}
QPushButton[variant="icon"]:hover { border-color: #456171; color: #50badf; }
QWidget.sessionRow { background: transparent; border-bottom: 1px solid #273945; }
QWidget.sessionRow:hover { background: #1b2934; }
QLabel.sessionTitle { color: #d7e2e8; font-size: 13px; background: transparent; }
QLabel.sessionMeta { color: #8499a6; font-size: 11px; background: transparent; }
QPushButton.sessionAction { border: 0; background: transparent; color: #8499a6; font-size: 12px; }
QPushButton.sessionAction:hover { color: #50badf; }
QPushButton.sessionAction[danger="true"]:hover { color: #e36c6c; }

/* ===== 消息区 ===== */
QScrollArea#messages { border: 0; background: transparent; }
QWidget#messageList { background: transparent; }
QWidget#emptyState { background: transparent; }
QLabel#emptySymbol {
    border: 1px solid #314653; color: #50badf; font-size: 23px; background: transparent;
}
QLabel#emptyTitle { color: #bfd0d9; font-size: 14px; font-weight: 700; background: transparent; }
QLabel#emptyHint { color: #8499a6; font-size: 12px; background: transparent; }

QWidget.messageWidget { background: transparent; }
QFrame#bubble { border: 1px solid #314653; border-radius: 4px; background: #1a2933; }
QFrame#bubble[role="user"] { background: #233744; border-right: 2px solid #50badf; }
QFrame#bubble[role="assistant"] { border-left: 2px solid #45a9ca; }
QFrame#bubble[error="true"] { border-color: #e36c6c; }
QLabel.messageLabel {
    color: #50badf; font-size: 10px; font-weight: 700; background: transparent;
}
QLabel.tokenUsage {
    color: #50badf; font-size: 10px; background: transparent;
    border-top: 1px solid rgba(122, 240, 204, 40); padding-top: 6px; margin-top: 8px;
}
QFrame#reasoning { border: 1px solid #334753; background: #141f27; }
QPushButton#reasoningSummary {
    border: 0; background: transparent; color: #91a7b3; font-size: 11px; text-align: left; padding: 6px 8px;
}
QPushButton#reasoningSummary:hover { color: #50badf; }
QLabel#reasoningText { color: #9aadb7; font-size: 11px; background: transparent; padding: 0 8px 8px 8px; }

/* Markdown 视图 */
QWidget.markdownView { background: transparent; }
QLabel.mdParagraph { color: #d7e2e8; background: transparent; }
QLabel.mdHeading { color: #eef8fb; font-size: 14px; font-weight: 700; background: transparent; }
QLabel.mdList { color: #d7e2e8; background: transparent; }
QFrame.codeBlock { border: 1px solid #344b59; background: #0c141a; }
QLabel.codeBlockHeader {
    color: #778f9d; font-size: 10px; background: transparent;
    border-bottom: 1px solid #2a3c47; padding: 4px 8px;
}
QLabel.codeBlockBody {
    color: #c5d7df; background: transparent; padding: 9px;
    font-family: %MONO%; font-size: 11px;
}

/* ===== 工具调用组 ===== */
QFrame.toolGroup { border: 1px solid #314653; background: #17242d; }
QWidget.toolGroupHead { background: transparent; }
QWidget.toolGroupHead:hover { background: #1b2b35; }
QLabel.toolGroupTitle { color: #d7e2e8; font-size: 12px; font-weight: 700; background: transparent; }
QLabel.pill {
    background: #243843; color: #91a8b4; font-size: 9px; border-radius: 8px; padding: 2px 6px;
}
QLabel.statusLabel { color: #e7a84d; font-size: 10px; background: transparent; }
QLabel.statusLabel[status="succeeded"] { color: #50ce91; }
QLabel.statusLabel[status="failed"] { color: #e36c6c; }
QLabel.statusLabel[status="cancelled"] { color: #e36c6c; }
QLabel.statusLabel[status="done"] { color: #50ce91; }
QWidget.toolList { background: transparent; border-top: 1px solid #314653; }
QWidget.toolItem { background: transparent; border-bottom: 1px solid #293d48; }
QWidget.toolItemSummary { background: transparent; }
QWidget.toolItemSummary:hover { background: #1b2b35; }
QLabel.toolItemName { color: #d7e2e8; font-size: 11px; background: transparent; }
QFrame.toolDetail { background: #111b22; border-left: 2px solid #314653; }
QLabel.toolDetailLabel {
    color: #78909d; font-size: 9px; font-weight: 700; background: transparent;
}
QPlainTextEdit.toolPre, QLabel.toolPre {
    color: #a9bbc4; background: transparent; border: 0;
    font-family: %MONO%; font-size: 10px;
}

/* ===== 结构化卡片 / 审批卡 ===== */
QFrame.structured { border: 1px solid #456171; background: #17252e; }
QLabel.structuredTitle {
    color: #bfe8f6; font-weight: 700; background: transparent;
    border-bottom: 1px solid #314653; padding: 7px 8px;
}
QPushButton[variant="option"] {
    border: 1px solid #456171; border-radius: 4px; background: #22343f; color: #d7e2e8;
    min-height: 28px; padding: 5px 9px;
}
QPushButton[variant="option"]:hover { border-color: #5c7d8e; }
QPushButton[variant="option"]:disabled { color: #7e939e; }
QPushButton[variant="optionPrimary"] {
    border: 1px solid #258db3; border-radius: 4px; background: #126889; color: #f4fbfe;
    min-height: 28px; padding: 5px 9px;
}
QPushButton[variant="optionPrimary"]:hover { background: #167da5; }
QPushButton[variant="optionPrimary"]:disabled { color: #a8c6d2; }
QPushButton[variant="optionDanger"] {
    border: 1px solid #89484b; border-radius: 4px; background: #46292c; color: #ffb5b5;
    min-height: 28px; padding: 5px 9px;
}
QPushButton[variant="optionDanger"]:hover { background: #543034; }
QPushButton.actionButton {
    border: 1px solid #456171; border-radius: 4px; background: #22343f; color: #d7e2e8;
    min-height: 30px; min-width: 72px; padding: 5px 9px;
}
QPushButton.actionButton:hover { border-color: #5c7d8e; }
QPushButton.actionButton:disabled { color: #7e939e; }
QLabel.paramName { color: #d2e0e6; font-size: 12px; background: transparent; }
QLabel.paramDesc { color: #9fb2bd; font-size: 11px; background: transparent; }
QLineEdit.paramInput, QPlainTextEdit.paramInput {
    border: 1px solid #314653; background: #0f181f; color: #d7e2e8; padding: 5px 7px; border-radius: 0;
    selection-background-color: #126c91;
}
QLineEdit.paramInput:focus, QPlainTextEdit.paramInput:focus { border-color: #50badf; }
QLineEdit.paramInput:disabled, QPlainTextEdit.paramInput:disabled { color: #627985; }

QFrame.workflowCard, QFrame.approvalCard { border: 1px solid #314653; background: #17242d; }
QWidget.cardHead { background: transparent; border-bottom: 1px solid #314653; }
QLabel.cardTitle { color: #d7e2e8; font-weight: 700; background: transparent; }
QLabel.toolBody {
    color: #9fb1bb; background: transparent; padding: 7px 8px;
    font-family: %MONO%; font-size: 11px;
}

/* ===== 阶段计划面板 ===== */
QFrame#phasePanel {
    border: 1px solid #3c5665; border-radius: 4px;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1b2c38, stop:1 #152129);
}
QWidget#phaseHead { background: transparent; border-bottom: 1px solid transparent; }
QWidget#phaseHead:hover { background: #1f3342; }
QWidget#phaseHead[expanded="true"] { background: #1d2f3c; border-bottom: 1px solid #314653; }
QLabel#phaseTitle { color: #d7e7f0; font-size: 12px; font-weight: 700; background: transparent; }
QLabel#phaseCount {
    border: 1px solid #2e4a5c; border-radius: 9px; background: #12222d; color: #50badf;
    font-size: 10px; font-weight: 700; padding: 1px 8px;
}
QLabel.phaseChevron { color: #718894; font-size: 10px; background: transparent; }
QLabel.phaseChevron[expanded="true"] { color: #50badf; }
QScrollArea#phaseSteps { background: #141f28; border: 0; border-top: 1px solid #314653; }
QWidget#phaseStepsInner { background: #141f28; }

/* ===== 输入区 ===== */
QWidget#composer { background: #17232c; border-top: 1px solid #314653; }
QLabel#configWarning {
    background: #332b20; color: #eac887; font-size: 11px;
    border-left: 2px solid #e7a84d; padding: 6px 7px;
}
QWidget#controls { background: transparent; }
QWidget#modeSwitch { background: transparent; }
QPushButton.modeButton {
    border: 1px solid #314653; background: #1d2b35; color: #8298a4; font-size: 9px;
    font-weight: 700; padding: 0 6px; border-radius: 0; min-height: 25px;
}
QPushButton.modeButton[active="true"] { border: 1px solid #267fa0; background: #173b4b; color: #64caed; }
QFrame.modelControl {
    border: 1px solid #314653; background: #1d2b35;
}
QLabel.modelControlCaption { color: #7f96a2; font-size: 9px; font-weight: 700; background: transparent; }
QWidget.comboTrigger { background: transparent; border: 0; }
QWidget.comboTrigger:hover { background: #22333e; }
QLabel.comboText { color: #afc0c9; font-size: 10px; background: transparent; }
QPushButton#settingsButton {
    border: 1px solid #314653; background: #1d2b35; color: #90a7b3; border-radius: 0; font-size: 13px;
}
QPushButton#settingsButton:hover { border-color: #50badf; color: #50badf; }
QWidget#attachBar { background: transparent; }
QFrame.attachChip {
    border: 1px solid #456171; border-radius: 4px; background: #1b2934;
}
QLabel.attachExt {
    background: #223541; color: #50badf; font-size: 9px; font-weight: 700;
    border-radius: 2px; padding: 1px 4px;
}
QLabel.attachName { color: #c3d4dd; font-size: 10px; background: transparent; }
QLabel.attachSize { color: #6d8492; font-size: 10px; background: transparent; }
QPushButton.attachRemove {
    border: 0; background: transparent; color: #8ba0ac; font-size: 12px; border-radius: 2px;
}
QPushButton.attachRemove:hover { background: #7d353a; color: #ffffff; }

/* 气泡内已发送附件（.bubble-attachments / .attach-file / .attach-thumb） */
QWidget.bubbleAttachments { background: transparent; }
QFrame.attachFile {
    border: 1px solid #314653; border-radius: 4px; background: #0f181f;
}
QLabel.attachThumb {
    border: 1px solid #314653; border-radius: 4px; background: #0f181f;
}

QFrame#inputWrap { border: 1px solid #456171; background: #0f181f; }
QFrame#inputWrap[focus="true"] { border-color: #3d94b3; }
QTextEdit#messageInput {
    border: 0; background: transparent; color: #d7e2e8; padding: 8px 114px 8px 8px;
    selection-background-color: #126c91;
}
QPushButton#sendButton {
    border: 1px solid #238eb8; border-radius: 4px; background: #14759b; color: #ffffff; font-size: 16px;
}
QPushButton#sendButton:hover { background: #1788b3; }
QPushButton#sendButton:disabled { color: #cfe3ec; background: #14759b; }
QPushButton#sendButton[stop="true"] { border-color: #a94c52; background: #7d353a; font-size: 11px; }
QPushButton.iconSquare {
    border: 1px solid #456171; border-radius: 4px; background: #1b2934; color: #8499a6;
}
QPushButton.iconSquare:hover:!disabled { border-color: #126c91; color: #50badf; }
QPushButton.iconSquare:disabled { color: #55686f; }
QPushButton.iconSquare[recording="true"] {
    background: #7d353a; border-color: #a94c52; color: #ffffff;
}
QLabel#busyLabel { color: #617783; font-size: 9px; background: transparent; }

/* ===== 下拉列表（模型 / Skill） ===== */
QWidget#listbox { border: 1px solid #456171; background: #111b22; }
QScrollArea#listboxScroll { border: 0; background: transparent; }
QWidget#listboxInner { background: transparent; }
QLabel.modelGroupLabel {
    color: #6d8998; font-size: 9px; font-weight: 700; background: transparent; padding: 7px 8px 4px 8px;
}
QWidget.modelOption { background: transparent; border: 1px solid transparent; }
QWidget.modelOption:hover, QWidget.modelOption[focused="true"] {
    border: 1px solid #31586a; background: #1b303b;
}
QWidget.modelOption[selected="true"] { background: #16272f; }
QLabel.modelCheck { color: #50badf; font-size: 11px; background: transparent; }
QLabel.modelName { color: #d7e2e8; font-size: 11px; background: transparent; }
QLabel.modelId { color: #6f8794; font-size: 9px; background: transparent; font-family: %MONO%; }
QLabel.listboxEmpty { color: #8499a6; font-size: 11px; background: transparent; padding: 16px; }

/* ===== Toast / 拖拽遮罩 ===== */
QLabel#toast {
    border: 1px solid #85484b; background: #3c2528; color: #ffd0d0; padding: 8px 10px;
}
QWidget#dropOverlay {
    border: 2px dashed #50badf; background: rgba(16, 23, 30, 220);
}
QLabel#dropOverlayText {
    color: #50badf; font-size: 14px; background: transparent;
}

/* ===== 设置中心 ===== */
QDialog#settingsDialog { background: #121d24; border: 1px solid #426171; }
QWidget#settingsHead { background: #17242d; border-bottom: 1px solid #314653; }
QLabel.eyebrow {
    color: #5fc5e8; font-size: 9px; font-weight: 700; background: transparent;
}
QLabel#settingsTitle { color: #edf7fa; font-size: 18px; font-weight: 700; background: transparent; }
QWidget#settingsTabs { background: #101920; border-bottom: 1px solid #314653; }
QPushButton.settingsTab {
    border: 0; border-bottom: 2px solid transparent; background: transparent;
    color: #7e949f; padding: 0 18px; font-size: 13px;
}
QPushButton.settingsTab[active="true"] { border-bottom: 2px solid #50badf; color: #d9f2fa; }
QWidget#settingsContent { background: transparent; }
QPushButton[variant="secondary"] {
    border: 1px solid #456171; border-radius: 4px; background: #1e303a; color: #d7e2e8;
    min-height: 28px; padding: 5px 10px;
}
QPushButton[variant="secondary"]:hover { border-color: #488096; }
QPushButton[variant="secondary"]:disabled { color: #7c8f99; }
QPushButton[variant="danger"] {
    border: 1px solid #724246; border-radius: 4px; background: #38262a; color: #f5a6a6;
    min-height: 28px; padding: 5px 10px;
}
QPushButton[variant="danger"]:hover { background: #452d32; }

QWidget#providerSidebar { background: #101a21; border-right: 1px solid #314653; }
QLabel.sectionLabel {
    color: #5fc5e8; font-size: 9px; font-weight: 700; background: transparent; padding: 0 7px 9px 7px;
}
QScrollArea#providerNavScroll { border: 0; background: transparent; }
QWidget#providerNav { background: transparent; }
QWidget.providerNavItem { border: 1px solid transparent; background: transparent; }
QWidget.providerNavItem:hover { background: #16252e; }
QWidget.providerNavItem[active="true"] { border: 1px solid #28566a; background: #18313d; }
QLabel.providerName { color: #d7e2e8; font-size: 12px; font-weight: 700; background: transparent; }
QWidget.providerNavItem[active="true"] QLabel.providerName { color: #8cdcf6; }
QLabel.providerId { color: #647b87; font-size: 9px; background: transparent; font-family: %MONO%; }
QLabel.providerCount {
    background: #223641; color: #8298a3; font-size: 9px; border-radius: 10px; padding: 2px 6px;
}
QPushButton#addProvider {
    border: 1px solid #456171; border-radius: 4px; background: #1e303a; color: #d7e2e8; min-height: 28px;
}
QPushButton#addProvider:hover { border-color: #488096; }

QScrollArea#providerEditorScroll { border: 0; background: transparent; }
QWidget#providerEditor { background: transparent; }
QFrame.providerSection { border: 1px solid #314653; background: #16242d; }
QLabel.sectionTitle { color: #edf7fa; font-size: 15px; font-weight: 700; background: transparent; }
QLabel.sectionTitleSmall { color: #6d8794; font-size: 11px; background: transparent; }
QLabel.fieldCaption { color: #849ba7; font-size: 10px; background: transparent; }
QLineEdit.settingsInput, QComboBox.settingsInput {
    border: 1px solid #314653; background: #0d171d; color: #d7e2e8;
    padding: 0 8px; min-height: 30px; border-radius: 0;
    selection-background-color: #126c91;
}
QLineEdit.settingsInput:focus, QComboBox.settingsInput:focus { border-color: #50badf; }
QLineEdit.settingsInput:disabled { color: #627985; background: #142129; }
QComboBox.settingsInput::drop-down { border: 0; width: 20px; }
QComboBox.settingsInput::down-arrow { image: none; border-left: 4px solid transparent;
    border-right: 4px solid transparent; border-top: 5px solid #8499a6; margin-right: 6px; }
QComboBox QAbstractItemView {
    border: 1px solid #456171; background: #111b22; color: #d7e2e8;
    selection-background-color: #1b303b; outline: 0;
}
QCheckBox.settingsCheck { color: #91a8b3; font-size: 11px; background: transparent; spacing: 6px; }
QCheckBox.settingsCheck::indicator {
    width: 13px; height: 13px; border: 1px solid #456171; background: #0d171d; border-radius: 2px;
}
QCheckBox.settingsCheck::indicator:checked { background: #126c91; border-color: #238fb9; }
QLabel.inlineResult { color: #8499a6; font-size: 11px; background: transparent; }

QFrame.settingsModel { border: 1px solid #304652; background: #111c23; }
QWidget.settingsModelSummary { background: transparent; }
QWidget.settingsModelSummary:hover { background: #16232b; }
QLabel.settingsModelName { color: #d7e2e8; font-size: 12px; font-weight: 700; background: transparent; }
QLabel.settingsModelId { color: #6e8490; font-size: 9px; background: transparent; font-family: %MONO%; }
QLabel.settingsModelApi { color: #79bbd2; font-size: 9px; background: transparent; font-family: %MONO%; }
QPushButton.modelDelete { border: 0; background: transparent; color: #8da0a9; font-size: 16px; }
QPushButton.modelDelete:hover { color: #e36c6c; }
QWidget.modelAdvanced { background: transparent; border-top: 1px solid #314653; }
QFrame#addModelBox { border: 1px dashed #3a5360; background: #111b22; }
QScrollArea#candidateScroll { border: 0; background: transparent; }
QWidget#candidateList { background: transparent; }
QPushButton.candidateItem {
    border: 0; background: transparent; color: #d7e2e8; text-align: left; padding: 6px; font-size: 11px;
}
QPushButton.candidateItem:hover { background: #1b303a; }
QPushButton.candidateItem:disabled { color: #6d8492; }
QLabel.candidateName { color: #d7e2e8; font-size: 11px; background: transparent; }
QLabel.candidateId { color: #718793; font-size: 9px; background: transparent; font-family: %MONO%; }
QLabel.placeholderTitle { color: #c6d6dd; font-size: 13px; font-weight: 700; background: transparent; }
QLabel.placeholderText { color: #8499a6; font-size: 12px; background: transparent; }

QWidget#skillsPanel, QWidget#mcpPanel { background: transparent; }
QLabel.panelTitle { color: #edf7fa; font-size: 15px; font-weight: 700; background: transparent; }
QLabel.panelCount { color: #6d8794; font-size: 11px; background: transparent; }
QLabel.panelStatus { color: #849ba7; font-size: 11px; background: transparent; }
QScrollArea.panelScroll { border: 0; background: transparent; }
QFrame.skillCard, QFrame.mcpTool { border: 1px solid #304652; background: #111c23; }
QWidget.skillCardSummary, QWidget.mcpToolSummary { background: transparent; }
QWidget.skillCardSummary:hover, QWidget.mcpToolSummary:hover { background: #16232b; }
QLabel.skillName { color: #d7e2e8; font-size: 12px; font-weight: 500; background: transparent; }
QLabel.skillVersion {
    background: #1d3a4a; color: #7fc6e6; font-size: 9px; border-radius: 8px; padding: 2px 6px;
}
QLabel.skillSource {
    background: #243843; color: #91a8b4; font-size: 9px; border-radius: 8px; padding: 2px 6px;
}
QLabel.skillDesc { color: #8fa5b1; font-size: 11px; background: transparent; padding: 0 10px 9px 10px; }
QLabel.skillMeta { color: #7b919d; font-size: 10px; background: transparent; padding: 0 10px 9px 10px; }
QLabel.skillShadowed {
    color: #e7a84d; font-size: 10px; background: transparent; padding: 8px 10px 9px 10px;
    border-top: 1px solid #314653;
}
QWidget.codeChipStrip { background: transparent; border-top: 1px solid #314653; padding: 2px 10px 9px 10px; }
QLabel.codeChip {
    border: 1px solid #2b414e; border-radius: 3px; background: #0e171e; color: #9bdcf2;
    font-size: 10px; font-family: %MONO%; padding: 2px 6px;
}
QLabel.mcpParamDesc { color: #7b919d; font-size: 10px; background: transparent; }
QLabel.mcpParamType { color: #7f96a2; font-size: 10px; background: transparent; }
QLabel.mcpParamRequired {
    border: 1px solid #7a5a2e; border-radius: 8px; color: #e7a84d; font-size: 9px; padding: 1px 5px;
}
QLabel.paramCode {
    border-radius: 2px; background: #0e171e; color: #9bdcf2;
    font-size: 10px; font-family: %MONO%; padding: 1px 5px;
}
QLabel.skillMetaText { color: #7b919d; font-size: 10px; background: transparent; }
QWidget.mcpParamRow { background: transparent; border-bottom: 1px solid #263a45; }
QWidget.mcpParamList { background: transparent; border-top: 1px solid #314653; padding: 2px 10px 9px 10px; }

QWidget#settingsActions { background: #101920; border-top: 1px solid #314653; }
QLabel#settingsStatus { color: #e1a867; font-size: 11px; background: transparent; }

/* ===== 确认对话框 ===== */
QDialog#confirmDialog { background: #121d24; border: 1px solid #456171; }
QLabel#confirmTitle { color: #edf7fa; font-size: 15px; font-weight: 700; background: transparent; }
QLabel#confirmMessage { color: #9fb2bd; font-size: 12px; background: transparent; }
)QSS";

    QString out = QLatin1String(sheet);
    out.replace(QLatin1String("%MONO%"), monoFont());
    out.replace(QLatin1String("%UI%"), uiFont());

    // 缩放系数 ≠ 1 时把所有 font-size: Npx 按比例放大（结果按系数缓存，缩放档位是离散的）
    const qreal factor = zoomFactor();
    if (!qFuzzyCompare(factor, 1.0)) {
        static QHash<qreal, QString> cache;
        auto it = cache.constFind(factor);
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
            it = cache.insert(factor, scaled);
        }
        return it.value();
    }
    return out;
}

} // namespace gs

#include "choiceoverlay.h"

#include "commonwidgets.h"
#include "jsoneditor.h"
#include "messagewidgets.h"
#include "theme.h"

#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

namespace gs {
namespace {

// 参数行：<b>name *</b> + desc
QLabel *paramLabel(const QString &name, bool required, const QString &desc, QWidget *parent)
{
    const Palette &pal = palette();
    QString html = QStringLiteral("<span style=\"color:%1;font-size:%2px;font-weight:600;\">%3</span>")
                       .arg(cssColor(pal.text))
                       .arg(scaledPx(12))
                       .arg(escapeHtml(name + (required ? QStringLiteral(" *") : QString())));
    if (!desc.isEmpty())
        html += QStringLiteral("<br><span style=\"color:%1;font-size:%2px;\">%3</span>")
                    .arg(cssColor(pal.muted))
                    .arg(scaledPx(11))
                    .arg(escapeHtml(desc));
    QLabel *label = makeLabel(QStringLiteral("paramName"), html, parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    label->setProperty("paramBold", name);
    label->setProperty("paramDesc", desc);
    return label;
}

QWidget *paramRow(QWidget *left, QWidget *right, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *grid = new QGridLayout(row);
    grid->setContentsMargins(0, 3, 0, 3);
    grid->setHorizontalSpacing(7);
    grid->setVerticalSpacing(0);
    grid->setColumnStretch(0, 10);
    grid->setColumnStretch(1, 13);
    grid->addWidget(left, 0, 0);
    grid->addWidget(right, 0, 1);
    return row;
}

// webui .param-json-btn：JSON 形态的参数在输入框左边贴一个「表格」入口。
// 配色走 gs::palette() 的局部样式表（theme.cpp 未接管这两个 class 前先自洽）。
QPushButton *jsonTableButton(QWidget *parent)
{
    const Palette &pal = palette();
    auto *button = new QPushButton(QStringLiteral("表格"), parent);
    setClass(button, QStringLiteral("paramJsonBtn"));
    button->setToolTip(QStringLiteral("用表格编辑这个 JSON 参数"));
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedWidth(scaledPx(42));
    // webui 的 holder 是 align-items:stretch：按钮和多行输入框一样高
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    button->setFocusPolicy(Qt::TabFocus);
    button->setStyleSheet(
        QStringLiteral("QPushButton.paramJsonBtn {"
                       " border: 1px solid %1; border-radius: 4px; background: %2; color: %3;"
                       " font-size: %4px; font-weight: 600; }"
                       "QPushButton.paramJsonBtn:hover {"
                       " border-color: %5; color: %6; background: %7; }")
            .arg(cssColor(pal.lineStrong), cssColor(pal.surface2), cssColor(pal.muted))
            .arg(scaledPx(11))
            .arg(cssColor(pal.cyanMid), cssColor(pal.cyan), cssColor(pal.accentTint)));
    return button;
}

QString inferType(const QVariant &value)
{
    switch (value.type()) {
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
    case QVariant::Double:
        return QStringLiteral("number");
    case QVariant::Bool:
        return QStringLiteral("boolean");
    case QVariant::Map:
    case QVariant::Hash:
    case QVariant::List:
    case QVariant::StringList:
        return QStringLiteral("object");
    default:
        return QStringLiteral("string");
    }
}

// app.js approvalEntries：按工具 schema 展开参数（必填打星、object/array 用多行输入）
struct ApprovalSpec
{
    QString name;
    QString desc;
    QVariant value;
    QString type;
    bool required = false;
};

QList<ApprovalSpec> approvalEntries(const QVariantMap &event)
{
    const QVariantMap args = event.value(QStringLiteral("args")).toMap();
    const QVariantMap schema = event.value(QStringLiteral("schema")).toMap();
    const QVariantMap props = schema.value(QStringLiteral("properties")).toMap();
    QStringList required;
    for (const QVariant &item : schema.value(QStringLiteral("required")).toList())
        required << item.toString();

    QList<ApprovalSpec> entries;
    if (!props.isEmpty()) {
        for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
            const QVariantMap def = it.value().toMap();
            ApprovalSpec spec;
            spec.name = it.key();
            spec.desc = def.value(QStringLiteral("description")).toString();
            spec.value = args.value(it.key());
            spec.type = def.value(QStringLiteral("type")).isValid()
                            ? def.value(QStringLiteral("type")).toString()
                            : inferType(args.value(it.key()));
            spec.required = required.contains(it.key());
            entries.append(spec);
        }
        for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
            if (props.contains(it.key()))
                continue;
            ApprovalSpec spec;
            spec.name = it.key();
            spec.value = it.value();
            spec.type = inferType(it.value());
            entries.append(spec);
        }
    } else {
        for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
            ApprovalSpec spec;
            spec.name = it.key();
            spec.value = it.value();
            spec.type = inferType(it.value());
            entries.append(spec);
        }
    }
    return entries;
}

// app.js toolParamEntries：{name, desc, value, type}
QList<ApprovalSpec> toolParamEntries(const QVariantMap &toolParams)
{
    QList<ApprovalSpec> entries;
    for (const QVariant &item : toolParams.value(QStringLiteral("params")).toList()) {
        const QVariantMap param = item.toMap();
        ApprovalSpec spec;
        spec.name = param.value(QStringLiteral("name")).toString();
        spec.desc = param.value(QStringLiteral("description")).toString();
        spec.value = param.value(QStringLiteral("value"));
        spec.type = inferType(spec.value);
        entries.append(spec);
    }
    return entries;
}

} // namespace

ChoiceOverlay::ChoiceOverlay(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("choiceCard"));
    setAttribute(Qt::WA_StyledBackground, true);
    setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_head = new QWidget(this);
    m_head->setObjectName(QStringLiteral("choiceHead"));
    // 标题行与底部作答区高度固定：卡片比内容高时，多余高度只该给正文滚动区，
    // 否则 QVBoxLayout 会把可伸展的 head 拉高，文字垂直居中，看起来标题上方凭空多一块空白。
    m_head->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_head->setCursor(Qt::PointingHandCursor);
    m_head->setAttribute(Qt::WA_Hover, true);
    auto *hl = new QHBoxLayout(m_head);
    hl->setContentsMargins(11, 9, 11, 9);
    hl->setSpacing(8);
    m_title = makeLabel(QStringLiteral("choiceTitle"), QString(), m_head);
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_caret = makeLabel(QStringLiteral("choiceCaret"), QString::fromUtf8("⌄"), m_head);
    m_caret->setTextInteractionFlags(Qt::NoTextInteraction);
    m_close = new QPushButton(QString::fromUtf8("×"), m_head);
    m_close->setObjectName(QStringLiteral("choiceClose"));
    m_close->setFixedSize(22, 22);
    m_close->setCursor(Qt::PointingHandCursor);
    m_close->setToolTip(QStringLiteral("收起"));
    hl->addWidget(m_title, 1);
    hl->addWidget(m_caret, 0);
    hl->addWidget(m_close, 0);
    layout->addWidget(m_head);

    // 正文（问题/参数/选项）单独放进可滚动区：webui 的卡片会一路长到顶出容器，
    // 窗口矮、内容长时上半张卡会被裁掉（连标题和关闭按钮都看不到）。
    // 这里让标题行与底部作答区固定，中间的正文放不下就滚动。
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("choiceScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    // 竖直方向用 Preferred：sizeHint 跟着正文自然高走，卡片按自然高排布时标题行/底部作答区
    // 才能各就各位（Ignored 会让它们吃掉正文的高度，实测标题行 93→正文只剩 94）；
    // minimumHeight 仍为 0，宿主太矮时第一步先压正文，标题行和作答区不至于被挤掉
    m_scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_scroll->setMinimumHeight(0);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_body = new QWidget(m_scroll);
    m_body->setObjectName(QStringLiteral("choiceBody"));
    auto *bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    m_scroll->setWidget(m_body);
    layout->addWidget(m_scroll);

    m_question = makeLabel(QStringLiteral("choiceQuestion"), QString(), m_body);
    m_question->setWordWrap(true);
    m_question->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_question->setContentsMargins(11, 0, 11, 9);
    bodyLayout->addWidget(m_question);

    m_params = new QWidget(m_body);
    setClass(m_params, QStringLiteral("params"));
    m_paramsLayout = new QVBoxLayout(m_params);
    m_paramsLayout->setContentsMargins(11, 0, 11, 9);
    m_paramsLayout->setSpacing(0);
    bodyLayout->addWidget(m_params);

    m_list = new QWidget(m_body);
    setClass(m_list, QStringLiteral("choiceList"));
    m_listLayout = new QVBoxLayout(m_list);
    m_listLayout->setContentsMargins(6, 0, 6, 6);
    m_listLayout->setSpacing(0);
    bodyLayout->addWidget(m_list);
    // 卡片比内容高时，多出的高度不该被选项均摊（审批卡的批准/拒绝会被拉成两条空行）
    bodyLayout->addStretch(1);

    m_foot = new QWidget(this);
    m_foot->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *fl = new QVBoxLayout(m_foot);
    fl->setContentsMargins(11, 0, 11, 10);
    fl->setSpacing(4);
    auto *answer = new QWidget(m_foot);
    auto *al = new QHBoxLayout(answer);
    al->setContentsMargins(0, 0, 0, 0);
    al->setSpacing(6);
    m_input = new QLineEdit(answer);
    m_input->setObjectName(QStringLiteral("choiceInput"));
    m_input->setPlaceholderText(QString::fromUtf8("点击「其他」后在此输入答案"));
    m_input->setFixedHeight(32);
    m_input->setEnabled(false);
    m_submit = new QPushButton(QStringLiteral("提交答案"), answer);
    m_submit->setObjectName(QStringLiteral("choiceSubmit"));
    m_submit->setFixedHeight(32);
    m_submit->setCursor(Qt::PointingHandCursor);
    m_submit->setEnabled(false);
    al->addWidget(m_input, 1);
    al->addWidget(m_submit, 0);
    auto *hint = makeLabel(QStringLiteral("choiceHint"), QStringLiteral("按 Esc 取消"), m_foot);
    hint->setTextInteractionFlags(Qt::NoTextInteraction);
    fl->addWidget(answer);
    fl->addWidget(hint);
    layout->addWidget(m_foot);

    // webui 的 .choice-head 是 role="button" tabindex="0"、.choice-close 是 button，都要能 Tab 到
    m_head->setFocusPolicy(Qt::TabFocus);
    m_close->setFocusPolicy(Qt::TabFocus);
    m_head->installEventFilter(this);
    m_close->installEventFilter(this);
    m_input->installEventFilter(this);
    connect(m_submit, &QPushButton::clicked, this, &ChoiceOverlay::submit);
    connect(m_input, &QLineEdit::textChanged, this, [this] {
        if (!m_submitted)
            m_input->setProperty("invalid", false), restyle(m_input);
    });

    // 入场/离场过渡（webui .choice-overlay 的 opacity + translateY）。
    // effect 只在动画期间 setEnabled(true)：跑完就关掉，否则这张卡会一直走离屏合成
    // （不带胶水层的部件挂着 QGraphicsOpacityEffect 时每帧都要额外一次 pixmap 合成）
    m_opacity = new QGraphicsOpacityEffect(this);
    m_opacity->setOpacity(1.0);
    setGraphicsEffect(m_opacity);
    m_opacity->setEnabled(false);
    m_fade = new QPropertyAnimation(m_opacity, "opacity", this);
    m_fade->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_fade, &QPropertyAnimation::finished, this, [this] {
        m_opacity->setEnabled(false);
        if (m_open)
            return; // 离场动画期间又被打开
        setVisible(false);
        rebuild();
    });
}

void ChoiceOverlay::fadeIn()
{
    m_fade->stop();
    m_opacity->setEnabled(true);
    m_fade->setDuration(180);
    m_fade->setStartValue(m_opacity->opacity());
    m_fade->setEndValue(1.0);
    m_fade->start();
}

void ChoiceOverlay::fadeOut()
{
    if (!isVisible()) {
        setVisible(false);
        rebuild();
        return;
    }
    m_fade->stop();
    m_opacity->setEnabled(true);
    // 240ms 对齐 webui app.js:842（收起过渡结束才隐藏并清空内容）
    m_fade->setDuration(240);
    m_fade->setStartValue(m_opacity->opacity());
    m_fade->setEndValue(0.0);
    m_fade->start();
}

bool ChoiceOverlay::isVisibleCard() const
{
    return m_open && isVisible();
}

void ChoiceOverlay::showChoice(const QVariantMap &payload)
{
    rebuild();

    // 换载荷时先清掉上一张卡的审批身份，否则 Esc 会被误判成"审批卡不可关闭"
    m_approvalCallId.clear();
    m_sessionId.clear();
    m_approvalEvent.clear();
    m_isApproval = false;

    const QVariantMap approval = payload.value(QStringLiteral("approval")).toMap();
    m_isApproval = !approval.isEmpty();
    m_approvalEvent = approval.value(QStringLiteral("event")).toMap();
    m_sessionId = approval.value(QStringLiteral("sessionId")).toString();
    m_approvalCallId = m_approvalEvent.value(QStringLiteral("call_id")).toString();

    QVariantMap toolParams = payload.value(QStringLiteral("tool_params")).toMap();
    if (toolParams.isEmpty())
        toolParams = payload.value(QStringLiteral("toolparams")).toMap();
    m_isToolParams = !m_isApproval && !toolParams.isEmpty();
    m_toolName = toolParams.value(QStringLiteral("tool")).toString();

    // 参数条目：审批按 schema 展开，工具参数按 params 列表
    const QList<ApprovalSpec> specs = m_isApproval ? approvalEntries(m_approvalEvent)
                                                   : toolParamEntries(toolParams);

    // webui .param-json-field：文本能表格化成 JSON 时，在参数框左边贴一个「表格」按钮，
    // 改完把新 JSON 写回参数框（走与手工输入同一条 textChanged 途径），提交仍然读参数框文本。
    // 多行输入（object/array）之外，单行参数也要能进表格：groupProperty 的 schema 类型是 string，
    // 值却是一段 JSON 文本，webui 对这种情况同样给按钮。
    auto jsonParamField = [this](QWidget *field, const QString &name) -> QWidget * {
        QString text;
        if (auto *area = qobject_cast<QPlainTextEdit *>(field))
            text = area->toPlainText();
        else if (auto *input = qobject_cast<QLineEdit *>(field))
            text = input->text();
        if (!JsonTableEditor::canEdit(text))
            return field;

        auto *holder = new QWidget(m_params);
        setClass(holder, QStringLiteral("paramJsonField"));
        auto *layout = new QHBoxLayout(holder);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto *button = jsonTableButton(holder);
        connect(button, &QPushButton::clicked, this, [this, field, name, button] {
            QString current;
            if (auto *area = qobject_cast<QPlainTextEdit *>(field))
                current = area->toPlainText();
            else if (auto *input = qobject_cast<QLineEdit *>(field))
                current = input->text();
            QString next;
            if (!JsonTableEditor::edit(window(), name, current, &next)) {
                // 输入框里的内容已经被改坏：照 webui 的 toast 口径提示一句
                if (!JsonTableEditor::canEdit(current))
                    QToolTip::showText(button->mapToGlobal(QPoint(0, -button->height())),
                                       QStringLiteral("这个参数不是可表格化的 JSON（对象或数组），请直接在输入框里改"),
                                       button);
                return;
            }
            if (auto *area = qobject_cast<QPlainTextEdit *>(field))
                area->setPlainText(next);
            else if (auto *input = qobject_cast<QLineEdit *>(field))
                input->setText(next);
        });
        layout->addWidget(button, 0);
        layout->addWidget(field, 1);
        return holder;
    };

    for (const ApprovalSpec &spec : specs) {
        const bool structured = spec.type == QLatin1String("object")
                                || spec.type == QLatin1String("array");
        ParamEntry entry;
        entry.name = spec.name;
        entry.type = spec.type;
        if (structured) {
            auto *area = new QPlainTextEdit(valueForInput(spec.value), m_params);
            setClass(area, QStringLiteral("paramInput"));
            area->setFixedHeight(scaledPx(66));
            area->setProperty("monoBasePx", 11);
            area->setProperty("monoBaseHeight", 66);
            QFont mono = area->font();
            mono.setFamily(monoFont());
            mono.setPixelSize(scaledPx(11));
            area->setFont(mono);
            entry.area = area;
            QWidget *row = paramRow(paramLabel(spec.name, spec.required, spec.desc, m_params),
                                    jsonParamField(area, spec.name), m_params);
            row->ensurePolished(); // QSS 的边距/字号要先落地，量出的行高才是最终值
            m_paramsLayout->addWidget(row);
            m_paramRows.append(row);
            m_paramRequired.append(spec.required);
        } else {
            auto *input = new QLineEdit(valueForInput(spec.value), m_params);
            setClass(input, QStringLiteral("paramInput"));
            entry.input = input;
            QWidget *right = jsonParamField(input, spec.name);
            if (right != input)
                input->setCursorPosition(0); // JSON 参数从开头显示：不需要只看到尾部那截
            QWidget *row = paramRow(paramLabel(spec.name, spec.required, spec.desc, m_params),
                                    right, m_params);
            row->ensurePolished();
            m_paramsLayout->addWidget(row);
            m_paramRows.append(row);
            m_paramRequired.append(spec.required);
        }
        m_paramEntries.append(entry);
    }
    m_params->setVisible(!m_paramEntries.isEmpty());
    buildParamFold();

    // 选项：审批固定批准/拒绝；参数块无选项时补确认/取消
    QVariantList options = payload.value(QStringLiteral("options")).toList();
    QVariantList actions;
    for (const QVariant &item : options) {
        const QVariantMap option = item.toMap();
        if (option.value(QStringLiteral("label")).isValid()
            || option.value(QStringLiteral("value")).isValid())
            actions.append(option);
    }
    if (m_isApproval) {
        actions.clear();
        actions << QVariantMap{ { QStringLiteral("label"), QStringLiteral("批准") },
                                { QStringLiteral("value"), QStringLiteral("approve") },
                                { QStringLiteral("style"), QStringLiteral("primary") } }
                << QVariantMap{ { QStringLiteral("label"), QStringLiteral("拒绝") },
                                { QStringLiteral("value"), QStringLiteral("deny") },
                                { QStringLiteral("style"), QStringLiteral("danger") } };
    } else if (actions.isEmpty() && !m_paramEntries.isEmpty()) {
        actions << QVariantMap{ { QStringLiteral("label"), QStringLiteral("确认执行") },
                                { QStringLiteral("value"), QStringLiteral("confirm") },
                                { QStringLiteral("style"), QStringLiteral("primary") } }
                << QVariantMap{ { QStringLiteral("label"), QStringLiteral("取消") },
                                { QStringLiteral("value"), QStringLiteral("cancel") },
                                { QStringLiteral("style"), QStringLiteral("danger") } };
    }
    if (actions.isEmpty() && m_paramEntries.isEmpty())
        return;

    for (const QVariant &item : actions) {
        const QVariantMap option = item.toMap();
        const QString label = option.value(QStringLiteral("label")).isValid()
                                  ? option.value(QStringLiteral("label")).toString()
                                  : option.value(QStringLiteral("value")).toString();
        OptionEntry entry;
        entry.option = option;
        entry.label = label;
        auto *widget = new QWidget(m_list);
        setClass(widget, QStringLiteral("choiceItem"));
        widget->setAttribute(Qt::WA_StyledBackground, true);
        widget->setCursor(Qt::PointingHandCursor);
        auto *il = new QHBoxLayout(widget);
        il->setContentsMargins(8, 7, 8, 7);
        il->setSpacing(9);
        auto *dot = new QLabel(widget);
        setClass(dot, QStringLiteral("choiceDot"));
        dot->setFixedSize(14, 14);
        dot->setAttribute(Qt::WA_StyledBackground, true);
        auto *text = new QWidget(widget);
        auto *tl = new QVBoxLayout(text);
        tl->setContentsMargins(0, 0, 0, 0);
        tl->setSpacing(2);
        QLabel *name = makeLabel(QStringLiteral("choiceItemName"), label, text);
        name->setWordWrap(true);
        name->setTextInteractionFlags(Qt::NoTextInteraction);
        name->setMinimumHeight(18); // .choice-text strong：12px / 行盒 18（QSS 无 line-height）
        tl->addWidget(name);
        const QString desc = option.value(QStringLiteral("description")).toString();
        if (!desc.isEmpty()) {
            QLabel *small = makeLabel(QStringLiteral("choiceItemDesc"), desc, text);
            small->setWordWrap(true);
            small->setTextInteractionFlags(Qt::NoTextInteraction);
            small->setMinimumHeight(16); // .choice-text small：11px / line-height 1.45 → 行盒 16
            tl->addWidget(small);
        }
        il->addWidget(dot, 0);
        il->addWidget(text, 1);
        entry.item = widget;
        widget->ensurePolished();
        // .choice-item 在 webui 里也是 tabindex="0"
        widget->setFocusPolicy(Qt::TabFocus);
        widget->installEventFilter(this);
        m_listLayout->addWidget(widget);
        m_optionEntries.append(entry);
    }

    // 末尾固定补「其他」：模型没给自由项时也能自己写答案；审批是二选一，不补
    bool hasOther = false;
    for (const OptionEntry &entry : qAsConst(m_optionEntries)) {
        if (entry.label.trimmed().compare(QStringLiteral("其他"), Qt::CaseInsensitive) == 0
            || entry.label.trimmed().compare(QStringLiteral("other"), Qt::CaseInsensitive) == 0)
            hasOther = true;
    }
    if (!m_isApproval && !hasOther) {
        OptionEntry entry;
        entry.label = QStringLiteral("其他");
        entry.other = true;
        auto *widget = new QWidget(m_list);
        setClass(widget, QStringLiteral("choiceItem"));
        widget->setAttribute(Qt::WA_StyledBackground, true);
        widget->setCursor(Qt::PointingHandCursor);
        auto *il = new QHBoxLayout(widget);
        il->setContentsMargins(8, 7, 8, 7);
        il->setSpacing(9);
        auto *dot = new QLabel(widget);
        setClass(dot, QStringLiteral("choiceDot"));
        dot->setFixedSize(14, 14);
        dot->setAttribute(Qt::WA_StyledBackground, true);
        QLabel *name = makeLabel(QStringLiteral("choiceItemName"), entry.label, widget);
        name->setTextInteractionFlags(Qt::NoTextInteraction);
        name->setMinimumHeight(18); // 与带说明的选项一致：.choice-item 单行行盒 18（webui 32）
        il->addWidget(dot, 0);
        il->addWidget(name, 1);
        entry.item = widget;
        widget->ensurePolished();
        widget->setFocusPolicy(Qt::TabFocus);
        widget->installEventFilter(this);
        m_listLayout->addWidget(widget);
        m_optionEntries.append(entry);
    }

    // 标题：approval / tool_params / 默认「请选择下一步」
    QString title = payload.value(QStringLiteral("title")).toString();
    if (title.isEmpty()) {
        if (m_isApproval)
            title = QStringLiteral("审批工具 · %1")
                        .arg(m_approvalEvent.value(QStringLiteral("name")).toString());
        else if (m_isToolParams)
            title = QStringLiteral("确认工具参数 · %1").arg(m_toolName);
        else
            title = QStringLiteral("请选择下一步");
    }
    m_title->setText(title);

    const QString question = payload.value(QStringLiteral("question")).toString();
    m_question->setText(question);
    m_question->setVisible(!question.isEmpty());

    // 审批卡沿用同一张浮层，但不给关闭入口、不给自由作答
    setClass(this, m_isApproval ? QStringLiteral("choiceCard approval")
                                : QStringLiteral("choiceCard"));
    m_close->setVisible(!m_isApproval);
    m_foot->setVisible(!m_isApproval);

    m_collapsed = false;
    // 上一张卡如果处于折叠态，选项区会被隐藏；新卡必须恢复成展开态。
    m_list->setVisible(true);
    m_caret->setText(QString::fromUtf8("⌄"));
    m_selected = -1;
    m_submitted = false;
    paintSelection(-1);

    setVisible(true);
    if (!m_open) {
        m_open = true;
        emit openStateChanged(true);
    }
    fadeIn();
}

// 宿主（Composer）只知道卡片当前几何，量高前必须先把内容按目标宽度重排一次：
// 隐藏态、旧宽度下量出来的 sizeHint 都偏小，窄窗口上底部作答区会被卡片底边裁掉。
int ChoiceOverlay::heightForCardWidth(int width)
{
    const int w = qMax(0, width);
    // 样式表的 padding/border 要 polish 之后才生效
    ensurePolished();
    if (m_head)
        m_head->ensurePolished();
    if (m_scroll)
        m_scroll->ensurePolished();
    if (m_foot)
        m_foot->ensurePolished();

    resize(w, height());
    if (QLayout *cardLayout = layout())
        cardLayout->activate();
    syncBodyHeight(qMax(gs::scaledPx(120), w - margins().left() - margins().right()));

    // 不用 sizeHint()：它会去问滚动区的 sizeHint，而滚动区的 sizeHint 在
    // widgetResizable 下并不跟着刚设好的 body 最小高度走，结果卡片被算矮，
    // 底部作答区被挤出可视区（参数多的审批卡就是这个症状）。
    // 改成头 + 正文自然高 + 脚三项直接相加，其中正文高由 syncBodyHeight() 标定。
    // 头与脚只算可见高度：折叠（m_foot 隐藏）或审批卡（无作答区）时不能再把它们的自然高算进去，
    // 否则卡片会比 sizeHint 高出一整块，折叠动作看起来没生效。
    const int headH = (m_head && m_head->isVisible()) ? m_head->sizeHint().height() : 0;
    const int footH = (m_foot && m_foot->isVisible()) ? m_foot->sizeHint().height() : 0;
    const QMargins m = margins();
    int bodyNatural = m_collapsed ? 0 : m_bodyNatural;
    if (!m_collapsed && m_body && m_body->layout()) {
        // 两个口径取大：sizeHint 在窄宽下偏小、heightForWidth 在宽未定稿时偏大，
        // 取大至少保证底部作答区不被顶出去
        bodyNatural = qMax(bodyNatural, m_body->layout()->sizeHint().height());
    }
    const int total = headH + (m_collapsed ? 0 : bodyNatural) + footH + m.top() + m.bottom();
    return qMax(total, sizeHint().height());
}

QMargins ChoiceOverlay::margins() const
{
    return layout() ? layout()->contentsMargins() : QMargins();
}

// 正文（问题/参数/选项）在滚动区里：先按当前宽度标定它的自然高度，
// 然后把这个高度设为最小高度——卡片放得下就长满，放不下就出滚动条。
void ChoiceOverlay::syncBodyHeight(int knownWidth)
{
    if (!m_scroll || !m_body)
        return;
    QLayout *bodyLayout = m_body->layout();
    if (!bodyLayout)
        return;

    bodyLayout->activate();
    // 优先用调用方给的目标宽度：重布局瞬间 viewport()->width() 还是旧值（或 100 的默认值），
    // 拿它算 heightForWidth 会把每行文字当成极窄宽度来换行，算出翻倍的高度，
    // 表现就是卡片顶部留一大块空白、底部作答区被顶出可视区。
    const int bodyWidth = knownWidth > 0 ? knownWidth : m_scroll->viewport()->width();
    int natural = bodyLayout->sizeHint().height();
    if (bodyWidth > 0 && bodyLayout->hasHeightForWidth())
        natural = bodyLayout->heightForWidth(bodyWidth);

    m_body->setMinimumHeight(natural);
    if (natural != m_bodyNatural) {
        m_bodyNatural = natural;
        updateGeometry();
    }
}

// 卡片高度 = 标题行 + 正文自然高（折叠时为 0）+ 底部作答区；
// 三个子块由 layout() 排布，所以不叠加间距/边距。
QSize ChoiceOverlay::sizeHint() const
{
    int height = 0;
    if (m_head && m_head->isVisible())
        height += m_head->sizeHint().height();
    if (m_scroll && m_scroll->isVisible())
        height += m_collapsed ? 0 : m_bodyNatural;
    if (m_foot && m_foot->isVisible())
        height += m_foot->sizeHint().height();
    // .choiceCard 的边框（theme.cpp：border:1px）会吃掉内容区高度，不补上的话
    // 正文比自然高少 2px，候选列表底部被切掉并冒出滚动条
    height += 2;

    QLayout *cardLayout = layout();
    const int hintedWidth = width() > 0 ? width()
                                        : (cardLayout ? cardLayout->sizeHint().width() : 0);
    return QSize(hintedWidth, height);
}

void ChoiceOverlay::rebuild()
{
    for (const ParamEntry &entry : qAsConst(m_paramEntries)) {
        if (entry.input)
            entry.input->deleteLater();
        if (entry.area)
            entry.area->deleteLater();
    }
    m_paramEntries.clear();
    m_paramRows.clear();
    m_paramRequired.clear();
    m_paramMore = nullptr;
    m_paramFoldCount = 0;
    m_paramsExpanded = false;
    while (QLayoutItem *item = m_paramsLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    m_optionEntries.clear();
    while (QLayoutItem *item = m_listLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    m_input->clear();
    m_input->setEnabled(false);
    m_input->setProperty("invalid", false);
    restyle(m_input);
    m_submit->setEnabled(false);
}

void ChoiceOverlay::closeOverlay()
{
    if (!m_open)
        return;
    m_open = false;
    // 输入框与计划窗口避让立刻恢复（webui 也是这样），卡片本身淡出后再隐藏
    emit openStateChanged(false);
    fadeOut();
}

// webui 64bf303：参数多到要滚动时，平铺本身就是噪音。审批真正要看的是必填项与值动过的项，
// 所以先按「必填 + 前几项」展开（总数封顶 6），其余收成一行，展开后再由滚动兜底。
void ChoiceOverlay::buildParamFold()
{
    const int foldAt = 6;
    m_paramAlwaysShow.clear();
    for (int i = 0; i < m_paramRequired.size(); ++i) {
        if (m_paramRequired.at(i))
            m_paramAlwaysShow.insert(i);
    }
    for (int i = 0; i < m_paramRows.size() && m_paramAlwaysShow.size() < foldAt; ++i)
        m_paramAlwaysShow.insert(i);
    m_paramFoldCount = m_paramRows.size() - m_paramAlwaysShow.size();
    m_paramsExpanded = false;

    if (m_paramFoldCount > 0) {
        m_paramMore = new QPushButton(m_params);
        setClass(m_paramMore, QStringLiteral("paramMore"));
        m_paramMore->setCursor(Qt::PointingHandCursor);
        m_paramMore->setFocusPolicy(Qt::TabFocus);
        m_paramMore->setFlat(true);
        connect(m_paramMore, &QPushButton::clicked, this, [this] {
            m_paramsExpanded = !m_paramsExpanded;
            applyParamFold(m_paramsExpanded);
            // 收起后列表变短，滚动位置得回顶，否则看着像卡在中间
            if (!m_paramsExpanded && m_scroll)
                m_scroll->verticalScrollBar()->setValue(0);
            updateGeometry();
            emit sizeChanged();
        });
        m_paramsLayout->addWidget(m_paramMore);
    }
    applyParamFold(false);
}

void ChoiceOverlay::applyParamFold(bool expanded)
{
    for (int i = 0; i < m_paramRows.size(); ++i) {
        QWidget *row = m_paramRows.at(i);
        if (!row)
            continue;
        row->setVisible(expanded || m_paramAlwaysShow.contains(i));
    }
    if (m_paramMore) {
        m_paramMore->setText(expanded
                                 ? QStringLiteral("收起可选参数")
                                 : QStringLiteral("展开其余 %1 个参数").arg(m_paramFoldCount));
        m_paramMore->setProperty("expanded", expanded);
        gs::restyle(m_paramMore);
    }
}

void ChoiceOverlay::toggleCollapsed()
{    m_collapsed = !m_collapsed;
    m_caret->setText(m_collapsed ? QString::fromUtf8("⌃") : QString::fromUtf8("⌄"));
    m_question->setVisible(!m_collapsed && !m_question->text().isEmpty());
    m_params->setVisible(!m_collapsed && !m_paramEntries.isEmpty());
    m_list->setVisible(!m_collapsed);
    m_foot->setVisible(!m_collapsed && !m_isApproval);
    // 卡片由 Composer 绝对定位，隐藏子控件不会自动改变它的几何尺寸；
    // 主动上报 sizeChanged，让宿主按新的 sizeHint 重新贴到输入框上方。
    updateGeometry();
    emit sizeChanged();
}

void ChoiceOverlay::paintSelection(int index)
{
    m_selected = index;
    for (int i = 0; i < m_optionEntries.size(); ++i) {
        QWidget *item = m_optionEntries.at(i).item;
        item->setProperty("selected", i == index);
        restyle(item);
    }
}

QVariantMap ChoiceOverlay::paramValues() const
{
    QVariantMap values;
    for (const ParamEntry &entry : m_paramEntries) {
        const QString text = entry.area ? entry.area->toPlainText() : entry.input->text();
        values.insert(entry.name, coerceSchemaValue(text, entry.type));
    }
    return values;
}

void ChoiceOverlay::resetSubmitted()
{
    if (!m_submitted)
        return;
    setSubmitted(false);
    // 提交时被禁用的输入区要按当前选择重新放开（选的是「其他」就把输入框与提交按钮都还回来）
    if (m_selected >= 0 && m_selected < m_optionEntries.size()) {
        const OptionEntry &entry = m_optionEntries.at(m_selected);
        if (entry.other && !m_isApproval) {
            m_input->setEnabled(true);
            m_submit->setEnabled(true);
        }
    }
}

void ChoiceOverlay::setSubmitted(bool submitted)
{
    m_submitted = submitted;
    for (const ParamEntry &entry : m_paramEntries) {
        if (entry.input)
            entry.input->setEnabled(!submitted);
        if (entry.area)
            entry.area->setEnabled(!submitted);
    }
    if (submitted) {
        m_input->setEnabled(false);
        m_submit->setEnabled(false);
    }
    for (const OptionEntry &entry : qAsConst(m_optionEntries)) {
        entry.item->setCursor(submitted ? Qt::ArrowCursor : Qt::PointingHandCursor);
        restyle(entry.item);
    }
    setClass(this, m_isApproval ? QStringLiteral("choiceCard approval")
                                : (submitted ? QStringLiteral("choiceCard submitted")
                                             : QStringLiteral("choiceCard")));
    restyle(this);
}

void ChoiceOverlay::activate(int index)
{
    if (index < 0 || index >= m_optionEntries.size() || m_submitted)
        return;
    const OptionEntry entry = m_optionEntries.at(index);
    paintSelection(index);
    if (entry.other) {
        // 选中「其他」才放开输入框：先写清楚要什么，再点提交答案
        m_input->setEnabled(true);
        m_submit->setEnabled(true);
        m_input->setProperty("invalid", false);
        restyle(m_input);
        m_input->setFocus();
        return;
    }
    submit();
}

void ChoiceOverlay::submit()
{
    if (m_submitted || m_selected < 0)
        return;
    const OptionEntry chosen = m_optionEntries.at(m_selected);
    QString value = chosen.label;
    if (chosen.other) {
        const QString text = m_input->text().trimmed();
        if (text.isEmpty()) {
            m_input->setProperty("invalid", true);
            restyle(m_input);
            m_input->setFocus();
            return;
        }
        value = text;
    } else if (chosen.option.value(QStringLiteral("value")).isValid()) {
        value = valueForInput(chosen.option.value(QStringLiteral("value")));
    }

    if (m_isApproval) {
        const bool approved = value == QLatin1String("approve");
        setSubmitted(true);
        emit approvalDecided(m_approvalCallId, approved,
                             approved ? paramValues()
                                      : m_approvalEvent.value(QStringLiteral("args")).toMap());
        return;
    }

    setSubmitted(true);
    if (m_isToolParams) {
        QJsonObject payload;
        payload.insert(QStringLiteral("type"), QStringLiteral("tool_params_confirmed"));
        payload.insert(QStringLiteral("tool"), m_toolName);
        payload.insert(QStringLiteral("confirmed"), value == QLatin1String("confirm"));
        payload.insert(QStringLiteral("params"), QJsonObject::fromVariantMap(paramValues()));
        const QString text = QStringLiteral("<structured_interaction>%1</structured_interaction>")
                                 .arg(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
        const QString display = chosen.other ? value : chosen.label;
        emit sendRequested(chosen.other ? text + QStringLiteral("\n\n") + value : text, display);
        closeOverlay();
        return;
    }

    emit optionChosen(value, chosen.label);
    closeOverlay();
}

void ChoiceOverlay::setApprovalResolved(const QString &callId, bool approved)
{
    if (!m_isApproval || callId != m_approvalCallId)
        return;
    m_title->setText(QStringLiteral("审批工具 · %1 · %2")
                         .arg(m_approvalEvent.value(QStringLiteral("name")).toString(),
                              approved ? QStringLiteral("已批准") : QStringLiteral("已拒绝")));
    setSubmitted(true);
    closeOverlay();
}

void ChoiceOverlay::reEnableApproval(const QString &callId)
{
    if (!m_isApproval || callId != m_approvalCallId)
        return;
    setSubmitted(false);
    m_input->setEnabled(false);
    m_submit->setEnabled(false);
    if (m_isApproval)
        m_title->setText(QStringLiteral("审批工具 · %1")
                             .arg(m_approvalEvent.value(QStringLiteral("name")).toString()));
}

void ChoiceOverlay::mousePressEvent(QMouseEvent *event)
{
    QFrame::mousePressEvent(event);
}

void ChoiceOverlay::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    // 折叠/展开与换提问题都会改变卡片高度：计划窗口的避让距离要跟着重算
    emit sizeChanged();
}

bool ChoiceOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_head) {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton)
                toggleCollapsed();
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
                toggleCollapsed();
                return true;
            }
        }
    }
    if (watched == m_close) {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton)
                closeOverlay();
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
                closeOverlay();
                return true;
            }
        }
    }
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            submit();
            return true;
        }
    }
    for (int i = 0; i < m_optionEntries.size(); ++i) {
        if (watched != m_optionEntries.at(i).item)
            continue;
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton)
                activate(i);
            return true;
        }
        // webui 的 .choice-item 是 role="radio" tabindex="0"
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
                activate(i);
                return true;
            }
        }
        break;
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace gs

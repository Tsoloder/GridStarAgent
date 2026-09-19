#include "choiceoverlay.h"

#include "commonwidgets.h"
#include "messagewidgets.h"
#include "theme.h"

#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
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
#include <QTimer>
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

    m_question = makeLabel(QStringLiteral("choiceQuestion"), QString(), this);
    m_question->setWordWrap(true);
    m_question->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_question->setContentsMargins(11, 0, 11, 9);
    layout->addWidget(m_question);

    m_params = new QWidget(this);
    setClass(m_params, QStringLiteral("params"));
    m_paramsLayout = new QVBoxLayout(m_params);
    m_paramsLayout->setContentsMargins(11, 0, 11, 9);
    m_paramsLayout->setSpacing(0);
    layout->addWidget(m_params);

    m_list = new QWidget(this);
    setClass(m_list, QStringLiteral("choiceList"));
    m_listLayout = new QVBoxLayout(m_list);
    m_listLayout->setContentsMargins(6, 0, 6, 6);
    m_listLayout->setSpacing(0);
    layout->addWidget(m_list);

    m_foot = new QWidget(this);
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
            m_paramsLayout->addWidget(paramRow(paramLabel(spec.name, spec.required, spec.desc, m_params),
                                               area, m_params));
        } else {
            auto *input = new QLineEdit(valueForInput(spec.value), m_params);
            setClass(input, QStringLiteral("paramInput"));
            entry.input = input;
            m_paramsLayout->addWidget(paramRow(paramLabel(spec.name, spec.required, spec.desc, m_params),
                                               input, m_params));
        }
        m_paramEntries.append(entry);
    }
    m_params->setVisible(!m_paramEntries.isEmpty());

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
        tl->addWidget(name);
        const QString desc = option.value(QStringLiteral("description")).toString();
        if (!desc.isEmpty()) {
            QLabel *small = makeLabel(QStringLiteral("choiceItemDesc"), desc, text);
            small->setWordWrap(true);
            small->setTextInteractionFlags(Qt::NoTextInteraction);
            tl->addWidget(small);
        }
        il->addWidget(dot, 0);
        il->addWidget(text, 1);
        entry.item = widget;
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
        il->addWidget(dot, 0);
        il->addWidget(name, 1);
        entry.item = widget;
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

void ChoiceOverlay::rebuild()
{
    for (const ParamEntry &entry : qAsConst(m_paramEntries)) {
        if (entry.input)
            entry.input->deleteLater();
        if (entry.area)
            entry.area->deleteLater();
    }
    m_paramEntries.clear();
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

void ChoiceOverlay::toggleCollapsed()
{
    m_collapsed = !m_collapsed;
    m_caret->setText(m_collapsed ? QString::fromUtf8("⌃") : QString::fromUtf8("⌄"));
    m_question->setVisible(!m_collapsed && !m_question->text().isEmpty());
    m_params->setVisible(!m_collapsed && !m_paramEntries.isEmpty());
    m_list->setVisible(!m_collapsed);
    m_foot->setVisible(!m_collapsed && !m_isApproval);
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
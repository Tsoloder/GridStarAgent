#include "messagewidgets.h"
#include "commonwidgets.h"
#include "markdownview.h"
#include "theme.h"

#include <QEvent>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace gs {

// ---------------------------------------------------------------- JSON 工具

static QString compactJson(const QVariant &value)
{
    const QJsonDocument doc = QJsonDocument::fromVariant(value);
    if (doc.isNull())
        return value.toString();
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

QString prettyJson(const QVariant &value)
{
    const QJsonDocument doc = QJsonDocument::fromVariant(value);
    if (doc.isNull())
        return value.toString();
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
}

QString valueForInput(const QVariant &value)
{
    switch (value.type()) {
    case QVariant::Map:
    case QVariant::Hash:
    case QVariant::List:
    case QVariant::StringList:
        return compactJson(value);
    default:
        return value.toString();
    }
}

static QVariant parsedJsonOrText(const QString &text)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || doc.isNull())
        return text;
    return doc.toVariant();
}

QVariant coerceValue(const QString &text, const QVariant &original)
{
    switch (original.type()) {
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
    case QVariant::Double: {
        bool ok = false;
        const double number = text.toDouble(&ok);
        return ok ? QVariant(number) : QVariant(text);
    }
    case QVariant::Bool:
        return QVariant(text == QLatin1String("true"));
    case QVariant::Map:
    case QVariant::Hash:
    case QVariant::List:
    case QVariant::StringList:
        return parsedJsonOrText(text);
    default:
        return QVariant(text);
    }
}

QVariant coerceSchemaValue(const QString &text, const QString &type)
{
    const QString trimmed = text.trimmed();
    if (type == QLatin1String("number") || type == QLatin1String("integer")) {
        bool ok = false;
        const double number = trimmed.toDouble(&ok);
        return ok ? QVariant(number) : QVariant(text);
    }
    if (type == QLatin1String("boolean")) {
        const QString lower = trimmed.toLower();
        return QVariant(lower == QLatin1String("true") || lower == QLatin1String("1")
                        || lower == QLatin1String("yes"));
    }
    if (type == QLatin1String("object") || type == QLatin1String("array"))
        return parsedJsonOrText(trimmed);
    return QVariant(text);
}

QString attachExt(const QString &name)
{
    const int i = name.lastIndexOf(QLatin1Char('.'));
    if (i >= 0 && i < name.size() - 1)
        return name.mid(i + 1).toLower();
    return QString();
}

// ------------------------------------------------------------ 通用小构件

static QString optionVariant(const QString &style)
{
    if (style == QLatin1String("primary"))
        return QStringLiteral("optionPrimary");
    if (style == QLatin1String("danger"))
        return QStringLiteral("optionDanger");
    return QStringLiteral("option");
}

static QPushButton *makeOptionButton(const QString &label, const QString &style, QWidget *parent)
{
    auto *button = new QPushButton(label, parent);
    button->setProperty("variant", optionVariant(style));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

// 参数行标签：<b>name</b> + desc（对应 .param-row label）。
// 字号烘焙在内联 HTML 里：把原文存进 property，供 ChartWidget::applyZoom 缩放后重建。
static QLabel *makeParamLabel(const QString &boldPart, const QString &descPart, QWidget *parent)
{
    QString html = QStringLiteral("<span style=\"color:#d2e0e6;font-size:%1px;font-weight:600;\">%2</span>")
                       .arg(scaledPx(12))
                       .arg(escapeHtml(boldPart));
    if (!descPart.isEmpty())
        html += QStringLiteral("<br><span style=\"color:#9fb2bd;font-size:%1px;\">%2</span>")
                    .arg(scaledPx(11))
                    .arg(escapeHtml(descPart));
    QLabel *label = makeLabel(QStringLiteral("paramName"), html, parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setProperty("paramBold", boldPart);
    label->setProperty("paramDesc", descPart);
    return label;
}

// .param-row：grid-template-columns minmax(85px,1fr) minmax(100px,1.3fr); gap 7px; padding 5px 0
static QWidget *makeParamRow(QWidget *left, QWidget *right, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *grid = new QGridLayout(row);
    grid->setContentsMargins(0, 5, 0, 5);
    grid->setHorizontalSpacing(7);
    grid->setVerticalSpacing(0);
    grid->setColumnStretch(0, 10);
    grid->setColumnStretch(1, 13);
    grid->addWidget(left, 0, 0);
    grid->addWidget(right, 0, 1);
    return row;
}

static QWidget *makeOptionsFlow(QWidget *parent)
{
    auto *flow = new QWidget(parent);
    auto *layout = new FlowLayout(flow, 0, 6, 6);
    layout->setContentsMargins(8, 2, 2, 8); // CSS .options padding 8px 2px 2px 8px
    return flow;
}

// 可点击头部（toolGroupHead / toolItemSummary）内的标签：makeLabel 默认允许选中文本，
// 会吃掉点击导致头部 eventFilter 收不到 MouseButtonRelease；这里关掉文本交互。
static QLabel *headLabel(const QString &className, const QString &text, QWidget *parent)
{
    QLabel *l = makeLabel(className, text, parent);
    l->setTextInteractionFlags(Qt::NoTextInteraction);
    return l;
}

// ---------------------------------------------------------- ToolItemWidget

ToolItemWidget::ToolItemWidget(const QString &callId, const QString &name,
                               const QString &argsJson, QWidget *parent)
    : QWidget(parent), m_callId(callId)
{
    setClass(this, QStringLiteral("toolItem"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *summary = new QWidget(this);
    setClass(summary, QStringLiteral("toolItemSummary"));
    summary->setAttribute(Qt::WA_StyledBackground, true);
    summary->setCursor(Qt::PointingHandCursor);
    summary->setMinimumHeight(34);
    auto *sl = new QHBoxLayout(summary);
    sl->setContentsMargins(8, 6, 8, 6);
    sl->setSpacing(8);
    m_dot = new StatusDot(summary);
    m_dot->setState(QStringLiteral("running"));
    QLabel *nameLabel = headLabel(QStringLiteral("toolItemName"),
                                  name.isEmpty() ? QStringLiteral("工具调用") : name, summary);
    nameLabel->setWordWrap(true);
    m_status = headLabel(QStringLiteral("statusLabel"), QStringLiteral("执行中"), summary);
    sl->addWidget(m_dot, 0);
    sl->addWidget(nameLabel, 1);
    sl->addWidget(m_status, 0);
    m_summary = summary;
    summary->installEventFilter(this);

    // CSS: .tool-detail margin 0 8px 8px 22px; padding 7px 8px
    auto *detailWrap = new QWidget(this);
    auto *dwl = new QVBoxLayout(detailWrap);
    dwl->setContentsMargins(22, 0, 8, 8);
    dwl->setSpacing(0);
    auto *detail = new QFrame(detailWrap);
    setClass(detail, QStringLiteral("toolDetail"));
    auto *inner = new QVBoxLayout(detail);
    inner->setContentsMargins(8, 7, 8, 7);
    inner->setSpacing(4);
    inner->addWidget(makeLabel(QStringLiteral("toolDetailLabel"), QStringLiteral("调用参数"), detail));
    m_args = makeLabel(QStringLiteral("toolPre"), argsJson, detail);
    m_args->setTextFormat(Qt::PlainText);
    m_args->setWordWrap(true);
    inner->addWidget(m_args);

    m_resultWrap = new QWidget(detail);
    auto *rl = new QVBoxLayout(m_resultWrap);
    rl->setContentsMargins(0, 8, 0, 0);
    rl->setSpacing(4);
    auto *separator = new QFrame(m_resultWrap);
    separator->setFixedHeight(1);
    separator->setStyleSheet(QStringLiteral("background:#283a44;"));
    rl->addWidget(separator);
    rl->addWidget(makeLabel(QStringLiteral("toolDetailLabel"), QStringLiteral("调用结果"), m_resultWrap));
    m_result = makeLabel(QStringLiteral("toolPre"), QString(), m_resultWrap);
    m_result->setTextFormat(Qt::PlainText);
    m_result->setWordWrap(true);
    rl->addWidget(m_result);
    m_resultWrap->setVisible(false);
    inner->addWidget(m_resultWrap);

    dwl->addWidget(detail);
    detailWrap->setVisible(false);
    m_detail = detailWrap;

    layout->addWidget(summary);
    layout->addWidget(detailWrap);
}

void ToolItemWidget::setOpen(bool open)
{
    m_open = open;
    m_detail->setVisible(open);
}

void ToolItemWidget::setState(const QString &state)
{
    m_state = state;
    m_dot->setState(state);
    if (state == QLatin1String("running")) {
        m_status->setText(QStringLiteral("执行中"));
        m_status->setProperty("status", QVariant());
    } else if (state == QLatin1String("failed")) {
        m_status->setText(QStringLiteral("失败"));
        m_status->setProperty("status", QStringLiteral("failed"));
    } else {
        m_status->setText(QStringLiteral("完成"));
        m_status->setProperty("status", QStringLiteral("succeeded"));
    }
    restyle(m_status);
}

void ToolItemWidget::setResult(const QString &result)
{
    m_result->setText(result);
    m_resultWrap->setVisible(true);
}

bool ToolItemWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_summary && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton)
            setOpen(!m_open);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

// --------------------------------------------------------- ToolGroupWidget

ToolGroupWidget::ToolGroupWidget(QWidget *parent) : QFrame(parent)
{
    setClass(this, QStringLiteral("toolGroup"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *head = new QWidget(this);
    setClass(head, QStringLiteral("toolGroupHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    head->setCursor(Qt::PointingHandCursor);
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(8, 8, 8, 8);
    hl->setSpacing(8);
    m_chevron = new Chevron(head);
    QLabel *title = headLabel(QStringLiteral("toolGroupTitle"), QStringLiteral("工具调用"), head);
    m_count = headLabel(QStringLiteral("pill"), QStringLiteral("0 个"), head);
    m_status = headLabel(QStringLiteral("statusLabel"), QStringLiteral("执行中"), head);
    hl->addWidget(m_chevron, 0);
    hl->addWidget(title, 1);
    hl->addWidget(m_count, 0);
    hl->addWidget(m_status, 0);
    m_head = head;
    head->installEventFilter(this);
    layout->addWidget(head);

    m_list = new QWidget(this);
    setClass(m_list, QStringLiteral("toolList"));
    m_list->setAttribute(Qt::WA_StyledBackground, true);
    m_listLayout = new QVBoxLayout(m_list);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(0);
    m_listLayout->setAlignment(Qt::AlignTop);
    m_list->setVisible(false);
    layout->addWidget(m_list);
}

ToolItemWidget *ToolGroupWidget::addToolCall(const QString &callId, const QString &name,
                                             const QString &argsJson)
{
    auto *item = new ToolItemWidget(callId, name, argsJson, m_list);
    m_listLayout->addWidget(item);
    m_items.append(item);
    updateSummary();
    return item;
}

ToolItemWidget *ToolGroupWidget::findTool(const QString &callId) const
{
    for (ToolItemWidget *item : m_items) {
        if (item->callId() == callId)
            return item;
    }
    return nullptr;
}

void ToolGroupWidget::updateSummary()
{
    m_count->setText(QStringLiteral("%1 个").arg(m_items.size()));
    bool running = false;
    bool failed = false;
    for (const ToolItemWidget *item : qAsConst(m_items)) {
        if (item->state() == QLatin1String("running"))
            running = true;
        else if (item->state() == QLatin1String("failed"))
            failed = true;
    }
    if (running) {
        m_status->setText(QStringLiteral("执行中"));
        m_status->setProperty("status", QVariant());
    } else if (failed) {
        m_status->setText(QStringLiteral("失败"));
        m_status->setProperty("status", QStringLiteral("failed"));
    } else {
        m_status->setText(QStringLiteral("完成"));
        m_status->setProperty("status", QStringLiteral("succeeded"));
    }
    restyle(m_status);
}

bool ToolGroupWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_head && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            m_open = !m_open;
            m_chevron->setOpen(m_open);
            m_list->setVisible(m_open);
        }
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

// ------------------------------------------------------------- OptionsCard

OptionsCard::OptionsCard(const QVariantList &options, QWidget *parent) : QFrame(parent)
{
    setClass(this, QStringLiteral("structured"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(makeLabel(QStringLiteral("structuredTitle"),
                                QStringLiteral("请选择下一步"), this));

    auto *flow = makeOptionsFlow(this);
    for (const QVariant &optionVar : options) {
        const QVariantMap option = optionVar.toMap();
        const QString label = option.value(QStringLiteral("label")).isValid()
                                  ? option.value(QStringLiteral("label")).toString()
                                  : (option.value(QStringLiteral("value")).isValid()
                                         ? option.value(QStringLiteral("value")).toString()
                                         : QStringLiteral("选择"));
        QString value;
        if (option.value(QStringLiteral("value")).isValid())
            value = valueForInput(option.value(QStringLiteral("value")));
        else if (option.value(QStringLiteral("label")).isValid())
            value = option.value(QStringLiteral("label")).toString();

        QPushButton *button = makeOptionButton(label, option.value(QStringLiteral("style")).toString(), flow);
        m_buttons.append(button);
        connect(button, &QPushButton::clicked, this, [this, value, label] {
            for (QPushButton *item : qAsConst(m_buttons))
                item->setEnabled(false);
            emit optionChosen(value, label);
        });
        static_cast<FlowLayout *>(flow->layout())->addWidget(button);
    }
    layout->addWidget(flow);
}

// ---------------------------------------------------------- ToolParamsCard

ToolParamsCard::ToolParamsCard(const QVariantMap &toolParams, const QVariantList &options,
                               QWidget *parent)
    : QFrame(parent)
{
    setClass(this, QStringLiteral("structured"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(makeLabel(QStringLiteral("structuredTitle"),
                                QStringLiteral("确认工具参数 · %1")
                                    .arg(toolParams.value(QStringLiteral("tool")).toString()),
                                this));

    auto *paramsWrap = new QWidget(this);
    auto *paramsLayout = new QVBoxLayout(paramsWrap);
    paramsLayout->setContentsMargins(7, 7, 7, 7); // CSS .params padding 7px
    paramsLayout->setSpacing(0);
    const QVariantList params = toolParams.value(QStringLiteral("params")).toList();
    for (const QVariant &paramVar : params) {
        const QVariantMap param = paramVar.toMap();
        const QString name = param.value(QStringLiteral("name")).toString();
        const QVariant original = param.value(QStringLiteral("value"));
        QLabel *label = makeParamLabel(name, param.value(QStringLiteral("description")).toString(),
                                       paramsWrap);
        auto *input = new QLineEdit(valueForInput(original), paramsWrap);
        setClass(input, QStringLiteral("paramInput"));
        paramsLayout->addWidget(makeParamRow(label, input, paramsWrap));
        m_params.append({ name, original, input });
    }
    layout->addWidget(paramsWrap);

    QVariantList actions = options;
    if (actions.isEmpty()) {
        actions << QVariantMap{ { QStringLiteral("label"), QStringLiteral("确认执行") },
                                { QStringLiteral("value"), QStringLiteral("confirm") },
                                { QStringLiteral("style"), QStringLiteral("primary") } }
                << QVariantMap{ { QStringLiteral("label"), QStringLiteral("取消") },
                                { QStringLiteral("value"), QStringLiteral("cancel") },
                                { QStringLiteral("style"), QStringLiteral("danger") } };
    }
    auto *flow = makeOptionsFlow(this);
    for (const QVariant &optionVar : actions) {
        const QVariantMap option = optionVar.toMap();
        const QString label = option.value(QStringLiteral("label")).isValid()
                                  ? option.value(QStringLiteral("label")).toString()
                                  : option.value(QStringLiteral("value")).toString();
        const QString value = valueForInput(option.value(QStringLiteral("value")));
        QPushButton *button = makeOptionButton(label, option.value(QStringLiteral("style")).toString(), flow);
        m_buttons.append(button);
        connect(button, &QPushButton::clicked, this, [this, value, label] {
            for (QPushButton *item : qAsConst(m_buttons))
                item->setEnabled(false);
            for (const Param &param : qAsConst(m_params))
                param.input->setEnabled(false);
            QVariantMap values;
            for (const Param &param : qAsConst(m_params))
                values.insert(param.name, coerceValue(param.input->text(), param.original));
            emit decided(value == QLatin1String("confirm"), values, label);
        });
        static_cast<FlowLayout *>(flow->layout())->addWidget(button);
    }
    layout->addWidget(flow);
}

// ---------------------------------------------------- WorkflowProposalCard

WorkflowProposalCard::WorkflowProposalCard(const QVariantList &steps, QWidget *parent)
    : QFrame(parent)
{
    setClass(this, QStringLiteral("structured"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(makeLabel(QStringLiteral("structuredTitle"),
                                QStringLiteral("静态工作流"), this));

    auto *paramsWrap = new QWidget(this);
    auto *paramsLayout = new QVBoxLayout(paramsWrap);
    paramsLayout->setContentsMargins(7, 7, 7, 7);
    paramsLayout->setSpacing(0);
    int index = 0;
    for (const QVariant &stepVar : steps) {
        const QVariantMap step = stepVar.toMap();
        ++index;
        QLabel *label = makeParamLabel(QStringLiteral("%1. %2").arg(index).arg(step.value(QStringLiteral("tool")).toString()),
                                       step.value(QStringLiteral("desc")).toString(), paramsWrap);
        QLabel *code = makeLabel(QStringLiteral("toolPre"),
                                 compactJson(step.value(QStringLiteral("params")).toMap()),
                                 paramsWrap);
        code->setTextFormat(Qt::PlainText);
        code->setWordWrap(true);
        paramsLayout->addWidget(makeParamRow(label, code, paramsWrap));
    }
    layout->addWidget(paramsWrap);

    auto *flow = makeOptionsFlow(this);
    QPushButton *run = makeOptionButton(QStringLiteral("执行工作流"), QStringLiteral("primary"), flow);
    run->setEnabled(!steps.isEmpty());
    connect(run, &QPushButton::clicked, this, [this, run, steps] {
        run->setEnabled(false);
        emit runRequested(steps);
    });
    static_cast<FlowLayout *>(flow->layout())->addWidget(run);
    layout->addWidget(flow);
}

// --------------------------------------------------------- WorkflowRunCard

WorkflowRunCard::WorkflowRunCard(QWidget *parent) : QFrame(parent)
{
    setClass(this, QStringLiteral("workflowCard"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *head = new QWidget(this);
    setClass(head, QStringLiteral("cardHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(8, 7, 8, 7);
    hl->setSpacing(8);
    QLabel *title = makeLabel(QStringLiteral("cardTitle"), QStringLiteral("工作流执行"), head);
    title->setWordWrap(true);
    m_status = makeLabel(QStringLiteral("statusLabel"), QStringLiteral("运行中"), head);
    hl->addWidget(title, 1);
    hl->addWidget(m_status, 0);
    layout->addWidget(head);

    m_body = new QWidget(this);
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(7, 7, 7, 7);
    m_bodyLayout->setSpacing(0);
    m_bodyLayout->setAlignment(Qt::AlignTop);
    layout->addWidget(m_body);
}

void WorkflowRunCard::setSteps(const QVariantList &steps)
{
    while (QLayoutItem *item = m_bodyLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            delete w;
        delete item;
    }
    for (int index = 0; index < steps.size(); ++index) {
        const QVariant stepVar = steps.at(index);
        if (!stepVar.isValid())
            continue; // 稀疏数组洞：app.js 重绘时 if (step) 跳过
        const QVariantMap step = stepVar.toMap();
        QLabel *label = makeParamLabel(QStringLiteral("%1. %2").arg(index + 1).arg(step.value(QStringLiteral("tool")).toString()),
                                       step.value(QStringLiteral("desc")).toString(), m_body);
        const QString status = step.value(QStringLiteral("status")).toString();
        QLabel *statusLabel = makeLabel(QStringLiteral("statusLabel"), status, m_body);
        statusLabel->setProperty("status", status);
        m_bodyLayout->addWidget(makeParamRow(label, statusLabel, m_body));
    }
}

void WorkflowRunCard::setStatus(const QString &status)
{
    m_status->setText(status);
    m_status->setProperty("status", status);
    restyle(m_status);
}

// ------------------------------------------------------------ ApprovalCard

static QString inferType(const QVariant &value)
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

ApprovalCard::ApprovalCard(const QVariantMap &event, QWidget *parent) : QFrame(parent)
{
    setClass(this, QStringLiteral("approvalCard"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *head = new QWidget(this);
    setClass(head, QStringLiteral("cardHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(8, 7, 8, 7);
    hl->setSpacing(8);
    QLabel *title = makeLabel(QStringLiteral("cardTitle"),
                              QStringLiteral("审批工具 · %1").arg(event.value(QStringLiteral("name")).toString()),
                              head);
    title->setWordWrap(true);
    m_status = makeLabel(QStringLiteral("statusLabel"), QStringLiteral("等待操作"), head);
    hl->addWidget(title, 1);
    hl->addWidget(m_status, 0);
    layout->addWidget(head);

    const QVariantMap args = event.value(QStringLiteral("args")).toMap();
    const QVariantMap schema = event.value(QStringLiteral("schema")).toMap();
    const QVariantMap props = schema.value(QStringLiteral("properties")).toMap();
    QStringList required;
    for (const QVariant &item : schema.value(QStringLiteral("required")).toList())
        required << item.toString();

    struct Spec { QString name; QString type; QString desc; QVariant value; };
    QList<Spec> specs;
    if (!props.isEmpty()) {
        for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
            const QVariantMap def = it.value().toMap();
            Spec spec;
            spec.name = it.key();
            spec.type = def.value(QStringLiteral("type")).isValid()
                            ? def.value(QStringLiteral("type")).toString()
                            : inferType(args.value(it.key()));
            spec.desc = def.value(QStringLiteral("description")).toString();
            spec.value = args.value(it.key());
            specs.append(spec);
        }
        for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
            if (props.contains(it.key()))
                continue;
            Spec spec;
            spec.name = it.key();
            spec.type = inferType(it.value());
            spec.value = it.value();
            specs.append(spec);
        }
    } else {
        for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
            Spec spec;
            spec.name = it.key();
            spec.type = inferType(it.value());
            spec.value = it.value();
            specs.append(spec);
        }
    }

    if (!specs.isEmpty()) {
        auto *paramsWrap = new QWidget(this);
        auto *paramsLayout = new QVBoxLayout(paramsWrap);
        paramsLayout->setContentsMargins(7, 7, 7, 7);
        paramsLayout->setSpacing(0);
        for (const Spec &spec : qAsConst(specs)) {
            const QString mark = required.contains(spec.name) ? QStringLiteral(" *") : QString();
            QLabel *label = makeParamLabel(spec.name + mark, spec.desc, paramsWrap);
            const bool structured = spec.type == QLatin1String("object")
                                    || spec.type == QLatin1String("array");
            Entry entry;
            entry.name = spec.name;
            entry.type = spec.type;
            if (structured) {
                auto *area = new QPlainTextEdit(valueForInput(spec.value), paramsWrap);
                setClass(area, QStringLiteral("paramInput"));
                area->setFixedHeight(qRound(66 * zoomFactor())); // rows=3 · 11px/1.4 等宽
                area->setProperty("monoBasePx", 11);
                area->setProperty("monoBaseHeight", 66);
                QFont mono = area->font();
                mono.setFamily(monoFont());
                mono.setPixelSize(scaledPx(11));
                area->setFont(mono);
                entry.area = area;
                paramsLayout->addWidget(makeParamRow(label, area, paramsWrap));
            } else {
                auto *input = new QLineEdit(valueForInput(spec.value), paramsWrap);
                setClass(input, QStringLiteral("paramInput"));
                entry.input = input;
                paramsLayout->addWidget(makeParamRow(label, input, paramsWrap));
            }
            m_entries.append(entry);
        }
        layout->addWidget(paramsWrap);
    } else {
        auto *argsWrap = new QWidget(this);
        auto *al = new QVBoxLayout(argsWrap);
        al->setContentsMargins(8, 8, 8, 8);
        m_rawArgs = new QPlainTextEdit(prettyJson(event.value(QStringLiteral("args"))), argsWrap);
        setClass(m_rawArgs, QStringLiteral("paramInput"));
        m_rawArgs->setFixedHeight(qRound(90 * zoomFactor()));
        m_rawArgs->setProperty("monoBasePx", 11);
        m_rawArgs->setProperty("monoBaseHeight", 90);
        QFont mono = m_rawArgs->font();
        mono.setFamily(monoFont());
        mono.setPixelSize(scaledPx(11));
        m_rawArgs->setFont(mono);
        al->addWidget(m_rawArgs);
        layout->addWidget(argsWrap);
    }

    auto *actions = new QWidget(this);
    auto *al = new QHBoxLayout(actions);
    al->setContentsMargins(8, 8, 8, 8);
    al->setSpacing(6);
    al->setAlignment(Qt::AlignLeft);
    QPushButton *approve = makeOptionButton(QStringLiteral("批准"), QStringLiteral("primary"), actions);
    QPushButton *deny = makeOptionButton(QStringLiteral("拒绝"), QStringLiteral("danger"), actions);
    m_buttons << approve << deny;
    al->addWidget(approve);
    al->addWidget(deny);
    layout->addWidget(actions);

    connect(approve, &QPushButton::clicked, this, [this] {
        setInputsEnabled(false);
        QVariantMap args;
        if (!m_entries.isEmpty()) {
            for (const Entry &entry : qAsConst(m_entries)) {
                const QString text = entry.area ? entry.area->toPlainText() : entry.input->text();
                args.insert(entry.name, coerceSchemaValue(text, entry.type));
            }
        } else {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(m_rawArgs->toPlainText().toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                emit jsonInvalid();
                setInputsEnabled(true);
                return;
            }
            args = doc.object().toVariantMap();
        }
        emit decided(true, args);
    });
    connect(deny, &QPushButton::clicked, this, [this] {
        setInputsEnabled(false);
        emit decided(false, QVariantMap());
    });
}

void ApprovalCard::setInputsEnabled(bool enabled)
{
    for (const Entry &entry : qAsConst(m_entries)) {
        if (entry.input)
            entry.input->setEnabled(enabled);
        if (entry.area)
            entry.area->setEnabled(enabled);
    }
    if (m_rawArgs)
        m_rawArgs->setEnabled(enabled);
    for (QPushButton *button : qAsConst(m_buttons))
        button->setEnabled(enabled);
}

void ApprovalCard::setResolved(bool approved)
{
    setInputsEnabled(false);
    m_status->setText(approved ? QStringLiteral("已批准") : QStringLiteral("已拒绝"));
    m_status->setProperty("status", approved ? QStringLiteral("succeeded")
                                             : QStringLiteral("cancelled"));
    restyle(m_status);
}

void ApprovalCard::reEnable()
{
    setInputsEnabled(true);
}

// ----------------------------------------------------------- MessageWidget

MessageWidget::MessageWidget(const QString &role, const QString &label, QWidget *parent)
    : QWidget(parent), m_role(role)
{
    setClass(this, QStringLiteral("messageWidget"));
    setAttribute(Qt::WA_StyledBackground, true);

    m_stack = new QVBoxLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->setSpacing(5);
    m_stack->setAlignment(role == QLatin1String("user") ? (Qt::AlignRight | Qt::AlignTop)
                                                        : (Qt::AlignLeft | Qt::AlignTop));

    m_bubble = new QFrame(this);
    m_bubble->setObjectName(QStringLiteral("bubble"));
    m_bubble->setProperty("role", role);
    m_bubbleLayout = new QVBoxLayout(m_bubble);
    m_bubbleLayout->setContentsMargins(10, 9, 10, 9);
    m_bubbleLayout->setSpacing(0);

    if (!label.isEmpty()) {
        QLabel *labelWidget = makeLabel(QStringLiteral("messageLabel"), label.toUpper(), m_bubble);
        labelWidget->setContentsMargins(0, 0, 0, 5); // CSS margin-bottom 5px
        m_bubbleLayout->addWidget(labelWidget);
    }

    m_body = new MarkdownView(m_bubble);
    m_bubbleLayout->addWidget(m_body);

    m_stack->addWidget(m_bubble);
}

void MessageWidget::setBodyVisible(bool visible)
{
    m_body->setVisible(visible);
}

void MessageWidget::setLabel(const QString &label)
{
    if (label.isEmpty())
        return; // app.js skillLabel: if (!label) return —— 不清除已有标签
    QLabel *existing = nullptr;
    for (int i = 0; i < m_bubbleLayout->count(); ++i) {
        auto *w = qobject_cast<QLabel *>(m_bubbleLayout->itemAt(i)->widget());
        if (w && w->property("class").toString() == QLatin1String("messageLabel")) {
            existing = w;
            break;
        }
    }
    if (existing) {
        existing->setText(label.toUpper()); // CSS text-transform: uppercase
        return;
    }
    QLabel *labelWidget = makeLabel(QStringLiteral("messageLabel"), label.toUpper(), m_bubble);
    labelWidget->setContentsMargins(0, 0, 0, 5); // CSS margin-bottom 5px
    m_bubbleLayout->insertWidget(0, labelWidget);
}

void MessageWidget::setAttachments(const QVariantList &attachments)
{
    if (attachments.isEmpty())
        return;
    auto *wrap = new QWidget(m_bubble);
    setClass(wrap, QStringLiteral("bubbleAttachments"));
    auto *flow = new FlowLayout(wrap, 0, 5, 5);
    flow->setContentsMargins(0, 0, 0, 6); // CSS margin-bottom 6px
    for (const QVariant &itemVar : attachments) {
        const QVariantMap item = itemVar.toMap();
        if (item.isEmpty())
            continue;
        const QString name = item.value(QStringLiteral("name")).isValid()
                                 ? item.value(QStringLiteral("name")).toString()
                                 : QStringLiteral("附件");
        const QString kind = item.value(QStringLiteral("kind")).toString();
        const QString url = item.value(QStringLiteral("url")).toString();
        if (kind == QLatin1String("image") && !url.isEmpty() && QFile::exists(url)) {
            auto *thumb = new QLabel(wrap);
            setClass(thumb, QStringLiteral("attachThumb"));
            thumb->setFixedSize(84, 84);
            QPixmap pixmap(url);
            if (!pixmap.isNull())
                thumb->setPixmap(pixmap.scaled(84, 84, Qt::KeepAspectRatioByExpanding,
                                               Qt::SmoothTransformation));
            thumb->setToolTip(name);
            flow->addWidget(thumb);
            continue;
        }
        auto *chip = new QFrame(wrap);
        setClass(chip, QStringLiteral("attachFile"));
        auto *cl = new QHBoxLayout(chip);
        cl->setContentsMargins(6, 2, 6, 2);
        cl->setSpacing(5);
        QString ext = attachExt(name);
        if (ext.isEmpty())
            ext = QStringLiteral("file");
        cl->addWidget(makeLabel(QStringLiteral("attachExt"), ext.toUpper(), chip));
        QLabel *nameLabel = makeLabel(QStringLiteral("attachName"), name, chip);
        nameLabel->setMaximumWidth(190);
        nameLabel->setText(elidedText(name, nameLabel->fontMetrics(), 190));
        nameLabel->setToolTip(name);
        cl->addWidget(nameLabel);
        flow->addWidget(chip);
    }
    m_bubbleLayout->insertWidget(0, wrap);
}

void MessageWidget::setTokenUsage(qint64 total, qint64 input, qint64 output, bool estimated)
{
    if (m_tokenUsage || (!total && !input && !output))
        return;
    m_tokenUsage = makeLabel(QStringLiteral("tokenUsage"),
                             QStringLiteral("%1tokens %2 · input %3 · output %4")
                                 .arg(estimated ? QStringLiteral("≈ ") : QString())
                                 .arg(total)
                                 .arg(input)
                                 .arg(output),
                             m_bubble);
    m_bubbleLayout->addSpacing(8); // CSS margin-top 8px
    m_bubbleLayout->addWidget(m_tokenUsage);
}

void MessageWidget::appendReasoning(const QString &delta)
{
    m_reasoningBuf += delta;
    if (!m_reasoningText) {
        // details.reasoning：默认折叠，summary「思考过程」，正文插在气泡之前
        m_reasoning = new QFrame(this);
        m_reasoning->setObjectName(QStringLiteral("reasoning"));
        m_reasoning->setAttribute(Qt::WA_StyledBackground, true);
        auto *rl = new QVBoxLayout(m_reasoning);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(0);

        auto *summary = new QPushButton(QStringLiteral("思考过程"), m_reasoning);
        summary->setObjectName(QStringLiteral("reasoningSummary"));
        summary->setFlat(true);
        summary->setCursor(Qt::PointingHandCursor);

        m_reasoningText = new QLabel(m_reasoning);
        m_reasoningText->setObjectName(QStringLiteral("reasoningText"));
        m_reasoningText->setWordWrap(true);
        m_reasoningText->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_reasoningText->setVisible(false);

        connect(summary, &QPushButton::clicked, this, [this] {
            m_reasoningText->setVisible(!m_reasoningText->isVisible());
        });

        rl->addWidget(summary);
        rl->addWidget(m_reasoningText);
        m_stack->insertWidget(0, m_reasoning);
    }
    m_reasoningText->setText(m_reasoningBuf);
}

void MessageWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyMaxWidths();
}

// CSS: .message > * { max-width: 94% }
void MessageWidget::applyMaxWidths()
{
    const int maxWidth = qRound(width() * 0.94);
    for (int i = 0; i < m_stack->count(); ++i) {
        if (QWidget *child = m_stack->itemAt(i)->widget())
            child->setMaximumWidth(maxWidth);
    }
}

} // namespace gs

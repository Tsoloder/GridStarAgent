#include "jsoneditor.h"

#include "commonwidgets.h"
#include "theme.h"

#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace gs {
namespace {

// ---------------------------------------------------------------- JSON 值工具
// app.js：Number / Boolean / Object / Array / null / undefined 六种形态，
// 这里用 QVariant + absent 表达。QJson 解析出来的 null 是「无效 QVariant」。

bool isObject(const QVariant &value)
{
    return value.type() == QVariant::Map || value.type() == QVariant::Hash;
}

bool isArray(const QVariant &value)
{
    return value.type() == QVariant::List || value.type() == QVariant::StringList;
}

bool isNumber(const QVariant &value)
{
    switch (value.type()) {
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
    case QVariant::Double:
        return true;
    default:
        return false;
    }
}

// JSON 数字语法（与 app.js jsonCellValue 里那条正则同义）
bool jsonNumberText(const QString &text, double *out)
{
    static const QRegularExpression pattern(
        QStringLiteral("^-?(0|[1-9]\\d*)(\\.\\d+)?([eE][+-]?\\d+)?$"));
    if (!pattern.match(text).hasMatch())
        return false;
    bool ok = false;
    const double number = text.toDouble(&ok);
    if (ok && out)
        *out = number;
    return ok;
}

// app.js parseJsonValue：trim 后空串给 undefined；能解析就返回值，否则 undefined。
// QJsonDocument 只接受对象/数组做顶层，JSON.parse 还认标量，这里补齐后者。
bool parseJsonText(const QString &text, QVariant *out)
{
    const QString raw = text.trimmed();
    if (raw.isEmpty())
        return false;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && !doc.isNull()) {
        if (out)
            *out = doc.toVariant();
        return true;
    }
    if (raw == QLatin1String("null")) {
        if (out)
            *out = QVariant();
        return true;
    }
    if (raw == QLatin1String("true") || raw == QLatin1String("false")) {
        if (out)
            *out = QVariant(raw == QLatin1String("true"));
        return true;
    }
    double number = 0.0;
    if (jsonNumberText(raw, &number)) {
        if (out)
            *out = QVariant(number);
        return true;
    }
    return false;
}

QString compactJson(const QVariant &value)
{
    const QJsonDocument doc = QJsonDocument::fromVariant(value);
    if (doc.isNull())
        return QString();
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

// 数字显示：QVariant::toString 对 double 只给 6 位有效数字（1234567 会变 1.23457e+06），
// 表格里绝不能因此改动数值。这里取「能原样解析回来」的最短写法（等价 JS String(n)）。
QString shortestDouble(double value)
{
    if (!qIsFinite(value))
        return QStringLiteral("null");
    for (int precision = 6; precision <= 17; ++precision) {
        const QString text = QString::number(value, 'g', precision);
        if (text.toDouble() == value)
            return text;
    }
    return QString::number(value, 'g', 17);
}

// app.js jsonCellText
QString cellText(const JsonTableCell &cell)
{
    if (cell.absent)
        return QString();
    const QVariant &value = cell.value;
    if (!value.isValid())
        return QStringLiteral("null");
    if (isObject(value) || isArray(value))
        return compactJson(value);
    if (isNumber(value))
        return shortestDouble(value.toDouble());
    if (value.type() == QVariant::Bool)
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return value.toString();
}

// 单元格原本的类型：输入过程中值会被改掉，类型不能拿新值再判
QString cellHint(const JsonTableCell &cell)
{
    if (cell.absent)
        return QStringLiteral("new");
    const QVariant &value = cell.value;
    if (!value.isValid())
        return QStringLiteral("null");
    if (isObject(value) || isArray(value))
        return QStringLiteral("object");
    if (isNumber(value))
        return QStringLiteral("number");
    if (value.type() == QVariant::Bool)
        return QStringLiteral("boolean");
    return QStringLiteral("string");
}

// app.js jsonCellValue：按 typeHint 把单元格文本还原成原值类型
QVariant cellValue(const QString &text, const QString &hint)
{
    const QString raw = text.trimmed();
    if (hint == QLatin1String("number")) {
        if (raw.isEmpty())
            return QVariant(0.0); // JS: Number("") === 0
        bool ok = false;
        const double number = raw.toDouble(&ok);
        return ok ? QVariant(number) : QVariant(text);
    }
    if (hint == QLatin1String("boolean")) {
        const QString lower = raw.toLower();
        return QVariant(lower == QLatin1String("true") || lower == QLatin1String("1")
                        || lower == QLatin1String("yes"));
    }
    if (hint == QLatin1String("object")) {
        QVariant parsed;
        return parseJsonText(raw, &parsed) ? parsed : QVariant(text);
    }
    if (hint == QLatin1String("null"))
        return raw.isEmpty() ? QVariant() : QVariant(text);
    if (hint == QLatin1String("new")) {
        if (raw.isEmpty())
            return QVariant(QString());
        if (raw == QLatin1String("null"))
            return QVariant();
        if (raw == QLatin1String("true") || raw == QLatin1String("false"))
            return QVariant(raw == QLatin1String("true"));
        double number = 0.0;
        if (jsonNumberText(raw, &number))
            return QVariant(number);
        if (raw.startsWith(QLatin1Char('{')) || raw.startsWith(QLatin1Char('['))) {
            QVariant parsed;
            if (parseJsonText(raw, &parsed))
                return parsed;
        }
        return QVariant(text);
    }
    return QVariant(text);
}

// 单元格里的值已经是字符串、却又以 { [ 开头且解析不开（webui 的标红条件）
bool looksBrokenCell(const JsonTableCell &cell)
{
    if (cell.absent || cell.value.type() != QVariant::String)
        return false;
    const QString raw = cell.value.toString().trimmed();
    if (!raw.startsWith(QLatin1Char('{')) && !raw.startsWith(QLatin1Char('[')))
        return false;
    QVariant parsed;
    return !parseJsonText(raw, &parsed);
}

// app.js jsonGroupCellValue：回写前对齐原值类型（angle 不能由 10 写成 "10"）
QVariant groupCellValue(const QVariant &value, const QVariant &sample)
{
    if (isNumber(sample) && value.type() == QVariant::String) {
        const QString raw = value.toString().trimmed();
        if (raw.isEmpty())
            return QVariant(0.0); // JS: Number("") === 0
        bool ok = false;
        const double number = raw.toDouble(&ok);
        return ok ? QVariant(number) : value;
    }
    if (sample.type() == QVariant::Bool && value.type() == QVariant::String)
        return QVariant(value.toString().trimmed() == QLatin1String("true"));
    return value;
}

// ---------------------------------------------------------------- 形状判定

// 组属性取一份样本：优先 domain，其次 line，再退到任意对象段
bool groupSample(const QVariantMap &body, QVariantMap *sample)
{
    QStringList order{QStringLiteral("domain"), QStringLiteral("line")};
    for (auto it = body.constBegin(); it != body.constEnd(); ++it) {
        if (it.key() != QLatin1String("domain") && it.key() != QLatin1String("line"))
            order.append(it.key());
    }
    for (const QString &section : qAsConst(order)) {
        if (!body.contains(section))
            continue;
        const QVariant list = body.value(section);
        QVariantList items;
        if (isArray(list))
            items = list.toList();
        else if (isObject(list))
            items.append(list);
        for (const QVariant &entry : qAsConst(items)) {
            if (isObject(entry)) {
                *sample = entry.toMap();
                return true;
            }
        }
    }
    return false;
}

// app.js jsonGroupShape：一行一个分部件组。
// 形状不规整（嵌套数组混标量、或根本不是分组结构）就返回 false，退到普通表格。
bool buildGroupShape(const QVariant &value, JsonTableShape *shape)
{
    if (!isArray(value))
        return false;
    const QVariantList items = value.toList();
    if (items.isEmpty())
        return false;

    QVector<JsonTableGroup> groups;
    for (const QVariant &raw : items) {
        if (!isObject(raw))
            return false;
        const QVariantMap item = raw.toMap();
        const QStringList keys = item.keys();
        if (keys.size() == 1) {
            const QVariant body = item.value(keys.first());
            if (!isObject(body))
                return false;
            JsonTableGroup group;
            group.name = keys.first();
            group.body = body.toMap();
            groups.append(group);
            continue;
        }
        const QVariant name = item.value(QStringLiteral("name"));
        if (name.type() == QVariant::String) {
            QVariantMap body;
            for (auto it = item.constBegin(); it != item.constEnd(); ++it) {
                if (it.key() == QLatin1String("name"))
                    continue;
                body.insert(it.key(), it.value());
            }
            JsonTableGroup group;
            group.name = name.toString();
            group.body = body;
            groups.append(group);
            continue;
        }
        return false;
    }

    QVector<QVariantMap> samples;
    samples.reserve(groups.size());
    for (const JsonTableGroup &group : qAsConst(groups)) {
        QVariantMap sample;
        if (!groupSample(group.body, &sample))
            return false;
        samples.append(sample);
    }

    QStringList attrColumns;
    for (const QVariantMap &sample : qAsConst(samples)) {
        for (auto it = sample.constBegin(); it != sample.constEnd(); ++it) {
            if (!attrColumns.contains(it.key()))
                attrColumns.append(it.key());
        }
    }
    if (attrColumns.isEmpty())
        return false;

    // 数组/对象型属性（ids）只展示不回写：domain 的面编号和 line 的线编号不是一回事
    QStringList readonlyAttrs;
    for (const QString &key : qAsConst(attrColumns)) {
        for (const QVariantMap &sample : qAsConst(samples)) {
            const QVariant value = sample.value(key);
            if (isObject(value) || isArray(value)) {
                readonlyAttrs.append(key);
                break;
            }
        }
    }

    shape->mode = QStringLiteral("groupRows");
    shape->columns = QStringList{QStringLiteral("组")};
    shape->columns += attrColumns;
    shape->attrColumns = attrColumns;
    shape->readonlyAttrs = readonlyAttrs;
    shape->readonlyCount = 1;
    shape->groups = groups;
    shape->samples = samples;
    shape->rows.clear();
    for (int index = 0; index < groups.size(); ++index) {
        QVector<JsonTableCell> row;
        JsonTableCell nameCell;
        nameCell.value = QVariant(groups.at(index).name);
        row.append(nameCell);
        for (const QString &key : qAsConst(attrColumns)) {
            JsonTableCell cell;
            if (samples.at(index).contains(key))
                cell.value = samples.at(index).value(key);
            else
                cell.absent = true;
            row.append(cell);
        }
        shape->rows.append(row);
    }
    return true;
}

// app.js jsonTableShape：标量数组 / 对象 / 对象数组；其余返回 false（= raw）
bool buildTableShape(const QVariant &value, JsonTableShape *shape)
{
    if (isArray(value)) {
        const QVariantList items = value.toList();
        if (items.isEmpty()) {
            shape->mode = QStringLiteral("scalarRows");
            shape->columns = QStringList{QStringLiteral("值")};
            shape->rows.clear();
            return true;
        }
        bool allObjects = true;
        bool allScalars = true;
        for (const QVariant &item : qAsConst(items)) {
            if (isObject(item))
                allScalars = false;
            else
                allObjects = false;
        }
        if (allObjects) {
            QStringList columns;
            for (const QVariant &item : qAsConst(items)) {
                const QVariantMap map = item.toMap();
                for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
                    if (!columns.contains(it.key()))
                        columns.append(it.key());
                }
            }
            shape->mode = QStringLiteral("objectRows");
            shape->columns = columns;
            shape->rows.clear();
            for (const QVariant &item : qAsConst(items)) {
                const QVariantMap map = item.toMap();
                QVector<JsonTableCell> row;
                for (const QString &column : qAsConst(columns)) {
                    JsonTableCell cell;
                    if (map.contains(column))
                        cell.value = map.value(column);
                    else
                        cell.absent = true;
                    row.append(cell);
                }
                shape->rows.append(row);
            }
            return true;
        }
        if (allScalars) {
            shape->mode = QStringLiteral("scalarRows");
            shape->columns = QStringList{QStringLiteral("值")};
            shape->rows.clear();
            for (const QVariant &item : qAsConst(items)) {
                QVector<JsonTableCell> row;
                JsonTableCell cell;
                cell.value = item;
                row.append(cell);
                shape->rows.append(row);
            }
            return true;
        }
        return false;
    }
    if (isObject(value)) {
        const QVariantMap map = value.toMap();
        shape->mode = QStringLiteral("pairs");
        shape->columns = QStringList{ QStringLiteral("键"), QStringLiteral("值") };
        shape->rows.clear();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            QVector<JsonTableCell> row;
            JsonTableCell keyCell;
            keyCell.value = QVariant(it.key());
            row.append(keyCell);
            JsonTableCell valueCell;
            valueCell.value = it.value();
            row.append(valueCell);
            shape->rows.append(row);
        }
        return true;
    }
    return false;
}

bool shapeOf(const QString &text, JsonTableShape *shape)
{
    QVariant parsed;
    if (!parseJsonText(text, &parsed))
        return false;
    JsonTableShape group;
    if (buildGroupShape(parsed, &group)) {
        *shape = group;
        return true;
    }
    JsonTableShape table;
    if (buildTableShape(parsed, &table)) {
        *shape = table;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- 局部样式
// 与 style.css 的 .json-editor* 一组对应，配色全部取自 gs::palette()。
// 这些 class 名就是给主会话搬进 theme.cpp 用的清单。
QString jsonEditorStyleSheet()
{
    const Palette &pal = palette();
    QString qss = QStringLiteral(
        "QDialog#jsonEditor { background: $PANEL$; }\n"
        "QWidget#jsonEditorHead { background: $BAND2$; border-bottom: 1px solid $LINE$; }\n"
        "QLabel.jsonEditorTitle { color: $TEXTBRIGHT$; font-size: $F14$px; font-weight: 600; background: transparent; }\n"
        "QLabel.jsonEditorSub { color: $MUTED2$; font-size: $F11$px; background: transparent; }\n"
        "QPushButton.jsonEditorClose { border: 0; border-radius: 4px; background: transparent; color: $MUTED$;"
        " font-size: $F17$px; min-width: $S26$px; max-width: $S26$px; min-height: $S26$px; max-height: $S26$px; }\n"
        "QPushButton.jsonEditorClose:hover { background: $SURFACE3$; color: $TEXTBRIGHT$; }\n"
        "QScrollArea#jsonEditorBody, QWidget#jsonEditorInner { background: transparent; border: 0; }\n"
        "QTableWidget#jsonEditorTable { border: 0; background: transparent; gridline-color: $LINE$;"
        " font-family: \"$MONO$\"; font-size: $F11$px; }\n"
        "QTableWidget#jsonEditorTable::item { padding: 1px 3px; border-bottom: 1px solid $LINE$; }\n"
        "QTableWidget#jsonEditorTable QHeaderView { background: $BAND3$; border: 0; }\n"
        "QTableWidget#jsonEditorTable QHeaderView::section { background: $BAND3$; color: $MUTED2$;"
        " border: 0; border-bottom: 1px solid $LINESTRONG$; padding: 4px 6px;"
        " font-family: \"$MONO$\"; font-size: $F11$px; font-weight: 600; }\n"
        "QTableWidget#jsonEditorTable QLineEdit { border: 1px solid $LINE$; background: $INSET$;"
        " color: $TEXT$; padding: 0 5px; }\n"
        "QTableWidget#jsonEditorTable QLineEdit:focus { border-color: $CYAN$; }\n"
        "QPushButton.jsonEditorDel { border: 0; border-radius: 4px; background: transparent; color: $MUTED2$;"
        " font-size: $F15$px; min-width: $S20$px; max-width: $S20$px; min-height: $S20$px; max-height: $S20$px; }\n"
        "QPushButton.jsonEditorDel:hover { background: $ERRTINT$; color: $RED$; }\n"
        "QLabel.jsonEditorHint { color: $ERRTEXT$; font-size: $F11$px; background: transparent; }\n"
        "QWidget#jsonEditorFoot { background: $BAND3$; border-top: 1px solid $LINE$; }\n"
        "QPushButton.jsonEditorCancel { border: 1px solid $LINESTRONG$; border-radius: 4px;"
        " background: $SURFACE2$; color: $TEXT$; font-size: $F12$px; min-width: $S72$px;"
        " min-height: $S30$px; padding: 4px 9px; }\n"
        "QPushButton.jsonEditorCancel:hover { border-color: $CYANMID$; color: $TEXTBRIGHT$; }\n"
        "QPushButton.jsonEditorApply { border: 1px solid $CYANMID$; border-radius: 4px;"
        " background: $CYANDARK$; color: $TEXTBRIGHT$; font-size: $F12$px; font-weight: 600;"
        " min-width: $S72$px; min-height: $S30$px; padding: 4px 9px; }\n"
        "QPushButton.jsonEditorApply:hover { border-color: $CYAN$; }\n");

    const QList<QPair<QString, QString>> vars{
        { QStringLiteral("$PANEL$"), cssColor(pal.panel) },
        { QStringLiteral("$BAND2$"), cssColor(pal.band2) },
        { QStringLiteral("$BAND3$"), cssColor(pal.band3) },
        { QStringLiteral("$LINE$"), cssColor(pal.line) },
        { QStringLiteral("$LINESTRONG$"), cssColor(pal.lineStrong) },
        { QStringLiteral("$TEXT$"), cssColor(pal.text) },
        { QStringLiteral("$TEXTBRIGHT$"), cssColor(pal.textBright) },
        { QStringLiteral("$MUTED$"), cssColor(pal.muted) },
        { QStringLiteral("$MUTED2$"), cssColor(pal.muted2) },
        { QStringLiteral("$SURFACE2$"), cssColor(pal.surface2) },
        { QStringLiteral("$SURFACE3$"), cssColor(pal.surface3) },
        { QStringLiteral("$INSET$"), cssColor(pal.inset) },
        { QStringLiteral("$CYAN$"), cssColor(pal.cyan) },
        { QStringLiteral("$CYANMID$"), cssColor(pal.cyanMid) },
        { QStringLiteral("$CYANDARK$"), cssColor(pal.cyanDark) },
        { QStringLiteral("$RED$"), cssColor(pal.red) },
        { QStringLiteral("$ERRTINT$"), cssColor(pal.errTint) },
        { QStringLiteral("$ERRTEXT$"), cssColor(pal.errText) },
        { QStringLiteral("$MONO$"), monoFont() },
        { QStringLiteral("$F11$"), QString::number(scaledPx(11)) },
        { QStringLiteral("$F12$"), QString::number(scaledPx(12)) },
        { QStringLiteral("$F14$"), QString::number(scaledPx(14)) },
        { QStringLiteral("$F15$"), QString::number(scaledPx(15)) },
        { QStringLiteral("$F17$"), QString::number(scaledPx(17)) },
        { QStringLiteral("$S20$"), QString::number(scaledPx(20)) },
        { QStringLiteral("$S26$"), QString::number(scaledPx(26)) },
        { QStringLiteral("$S30$"), QString::number(scaledPx(30)) },
        { QStringLiteral("$S72$"), QString::number(scaledPx(72)) },
    };
    for (const QPair<QString, QString> &var : vars)
        qss.replace(var.first, var.second);
    return qss;
}

} // namespace

// ---------------------------------------------------------------- 公开接口

bool JsonTableEditor::canEdit(const QString &text)
{
    JsonTableShape shape;
    return shapeOf(text, &shape);
}

bool JsonTableEditor::edit(QWidget *parent, const QString &name, const QString &text, QString *out)
{
    JsonTableEditor dialog(parent, name, text);
    if (!dialog.hasShape())
        return false;
    if (dialog.exec() != QDialog::Accepted)
        return false;
    if (out)
        *out = dialog.valueText();
    return true;
}

JsonTableEditor::JsonTableEditor(QWidget *parent, const QString &name, const QString &text)
    : QDialog(parent)
{
    shapeOf(text, &m_shape);
    buildUi(name);
    rebuildTable();
}

// ---------------------------------------------------------------- 界面

QString JsonTableEditor::modeHint() const
{
    if (m_shape.mode == QLatin1String("groupRows"))
        return QStringLiteral("分部件组 · 改完的属性会同时应用到该组的 domain 与 line");
    if (m_shape.mode == QLatin1String("pairs"))
        return QStringLiteral("对象 · 每行一个键值对");
    if (m_shape.mode == QLatin1String("scalarRows"))
        return QStringLiteral("值列表 · 每行一个值");
    if (m_shape.mode == QLatin1String("objectRows"))
        return QStringLiteral("对象数组 · 每行一项，列名即字段名");
    return QStringLiteral("这个参数不是可表格化的 JSON（对象或数组），请直接在输入框里改");
}

void JsonTableEditor::buildUi(const QString &name)
{
    setObjectName(QStringLiteral("jsonEditor"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setModal(true);
    setStyleSheet(jsonEditorStyleSheet());
    setWindowTitle(name + QStringLiteral(" · 表格编辑"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // —— 标题行（webui .json-editor-head）
    auto *head = new QWidget(this);
    head->setObjectName(QStringLiteral("jsonEditorHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(scaledPx(14), scaledPx(12), scaledPx(14), scaledPx(12));
    headLayout->setSpacing(scaledPx(10));

    auto *titles = new QWidget(head);
    auto *titlesLayout = new QVBoxLayout(titles);
    titlesLayout->setContentsMargins(0, 0, 0, 0);
    titlesLayout->setSpacing(2);
    auto *title = new QLabel(name + QStringLiteral(" · 表格编辑"), titles);
    setClass(title, QStringLiteral("jsonEditorTitle"));
    title->setWordWrap(true);
    title->setTextInteractionFlags(Qt::NoTextInteraction);
    auto *sub = new QLabel(modeHint(), titles);
    setClass(sub, QStringLiteral("jsonEditorSub"));
    sub->setWordWrap(true);
    sub->setTextInteractionFlags(Qt::NoTextInteraction);
    titlesLayout->addWidget(title);
    titlesLayout->addWidget(sub);

    auto *close = new QPushButton(QString::fromUtf8("×"), head);
    setClass(close, QStringLiteral("jsonEditorClose"));
    close->setCursor(Qt::PointingHandCursor);
    close->setToolTip(QStringLiteral("关闭"));
    close->setAutoDefault(false);
    close->setDefault(false);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);

    headLayout->addWidget(titles, 1);
    headLayout->addWidget(close, 0, Qt::AlignTop);
    root->addWidget(head);

    // —— 表格区（webui .json-editor-body > .json-editor-scroll）
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("jsonEditorBody"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    m_scroll->viewport()->setAutoFillBackground(false);

    auto *body = new QWidget(m_scroll);
    body->setObjectName(QStringLiteral("jsonEditorInner"));
    body->setAttribute(Qt::WA_StyledBackground, true);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(scaledPx(14), scaledPx(10), scaledPx(14), scaledPx(10));
    bodyLayout->setSpacing(scaledPx(6));

    m_table = new QTableWidget(body);
    m_table->setObjectName(QStringLiteral("jsonEditorTable"));
    m_table->setEditTriggers(QAbstractItemView::AllEditTriggers); // webui 的单元格就是常驻输入框
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_table->setWordWrap(false);
    m_table->setShowGrid(false);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionsMovable(false);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setDefaultSectionSize(scaledPx(120));
    m_table->horizontalHeader()->setMinimumSectionSize(scaledPx(44));
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(scaledPx(24));
    m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFont mono(monoFont());
    mono.setPixelSize(scaledPx(11));
    m_table->setFont(mono);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (!item || m_building)
            return;
        commitCell(m_table, item->row(), item->column());
    });

    m_hint = new QLabel(body);
    setClass(m_hint, QStringLiteral("jsonEditorHint"));
    m_hint->setWordWrap(true);
    m_hint->setTextInteractionFlags(Qt::NoTextInteraction);
    m_hint->setMinimumHeight(scaledPx(16));

    bodyLayout->addWidget(m_table);
    bodyLayout->addWidget(m_hint);
    m_scroll->setWidget(body);
    root->addWidget(m_scroll, 1);

    // —— 底部（webui .json-editor-foot）
    auto *foot = new QWidget(this);
    foot->setObjectName(QStringLiteral("jsonEditorFoot"));
    foot->setAttribute(Qt::WA_StyledBackground, true);
    auto *footLayout = new QHBoxLayout(foot);
    footLayout->setContentsMargins(scaledPx(14), scaledPx(10), scaledPx(14), scaledPx(10));
    footLayout->setSpacing(scaledPx(7));
    auto *cancel = new QPushButton(QStringLiteral("取消"), foot);
    setClass(cancel, QStringLiteral("jsonEditorCancel"));
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setAutoDefault(false);
    cancel->setDefault(false);
    auto *apply = new QPushButton(QStringLiteral("应用到参数"), foot);
    setClass(apply, QStringLiteral("jsonEditorApply"));
    apply->setCursor(Qt::PointingHandCursor);
    apply->setAutoDefault(false);
    apply->setDefault(false);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(apply, &QPushButton::clicked, this, &JsonTableEditor::apply);
    footLayout->addStretch(1);
    footLayout->addWidget(cancel, 0);
    footLayout->addWidget(apply, 0);
    root->addWidget(foot);

    // —— 尺寸：宽屏最多 720，窄屏贴边；高不超过宿主窗口的 78%（webui 78vh / 640 上限）
    int width = scaledPx(720);
    int height = scaledPx(460);
    if (const QWidget *host = parentWidget() ? parentWidget()->window() : nullptr) {
        const QSize avail = host->size();
        if (avail.width() > 0) {
            width = qMin(width, qRound(avail.width() * 0.94));
            width = qMin(width, qMax(scaledPx(280), avail.width() - scaledPx(12)));
        }
        if (avail.height() > 0)
            height = qMin(height, qRound(avail.height() * 0.78));
    }
    resize(qMax(width, scaledPx(280)), qMax(height, scaledPx(300)));
}

void JsonTableEditor::rebuildTable()
{
    if (!m_table)
        return;
    m_building = true;

    // 行内的删除按钮是 cell widget：先清空行数把上一批控件删掉，再按新行重建
    m_table->setRowCount(0);
    m_table->setColumnCount(m_shape.columns.size() + 1);
    m_table->setRowCount(m_shape.rows.size());

    QStringList headers = m_shape.columns;
    headers.append(QString());
    m_table->setHorizontalHeaderLabels(headers);

    // 列宽铺满：webui 的 table 是 width:100% 由浏览器分配，列本身不可拖。
    // 固定宽度会让右侧留一条空带，删除列也跟着对不齐。
    for (int column = 0; column < m_shape.columns.size(); ++column)
        m_table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Stretch);
    const int delColumn = m_shape.columns.size();
    m_table->horizontalHeader()->setSectionResizeMode(delColumn, QHeaderView::Fixed);
    m_table->setColumnWidth(delColumn, scaledPx(30));

    const Palette &pal = gs::palette();
    for (int rowIndex = 0; rowIndex < m_shape.rows.size(); ++rowIndex) {
        const QVector<JsonTableCell> row = m_shape.rows.at(rowIndex);
        for (int column = 0; column < m_shape.columns.size(); ++column) {
            const JsonTableCell cell = column < row.size() ? row.at(column) : JsonTableCell();
            const bool locked = column < m_shape.readonlyCount
                                || m_shape.readonlyAttrs.contains(m_shape.columns.at(column));
            auto *item = new QTableWidgetItem(cellText(cell));
            item->setToolTip(cellText(cell));
            if (locked) {
                // 组名与数组型属性（ids）只展示：domain 的面编号和 line 的线编号不是一回事
                item->setFlags(item->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsSelectable);
                item->setForeground(pal.muted);
            } else {
                item->setData(Qt::UserRole, cellHint(cell));
                if (looksBrokenCell(cell)) {
                    item->setBackground(pal.errTint);
                    item->setForeground(pal.errText);
                }
            }
            m_table->setItem(rowIndex, column, item);
        }

        auto *wrap = new QWidget(m_table);
        auto *wrapLayout = new QHBoxLayout(wrap);
        wrapLayout->setContentsMargins(scaledPx(2), 0, scaledPx(2), 0);
        wrapLayout->setSpacing(0);
        auto *del = new QPushButton(QString::fromUtf8("×"), wrap);
        setClass(del, QStringLiteral("jsonEditorDel"));
        del->setCursor(Qt::PointingHandCursor);
        del->setToolTip(QStringLiteral("删除这一行"));
        del->setAutoDefault(false);
        del->setDefault(false);
        connect(del, &QPushButton::clicked, this, [this, rowIndex] {
            // 按钮自己就在要删的行里：延后一拍再重建，别在它的 clicked 里把它删掉
            QTimer::singleShot(0, this, [this, rowIndex] { removeRow(rowIndex); });
        });
        wrapLayout->addWidget(del, 0, Qt::AlignCenter);
        m_table->setCellWidget(rowIndex, delColumn, wrap);
    }

    m_building = false;
    syncTableHeight();
    if (m_hint)
        m_hint->clear();
}

void JsonTableEditor::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // 首次显示后列宽才定稿：确认要不要横向滚动条，再把表格高度收准
    syncTableHeight();
}

void JsonTableEditor::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    // 拉窄会让列压到最小宽、冒出横向滚动条：表格高度得跟着让位
    syncTableHeight();
}

void JsonTableEditor::syncTableHeight()
{
    if (!m_table)
        return;
    m_table->resizeRowsToContents();
    int height = m_table->horizontalHeader()->sizeHint().height() + scaledPx(2);
    for (int row = 0; row < m_table->rowCount(); ++row)
        height += m_table->rowHeight(row);
    // 列多到铺不下时会出现横向滚动条，它会吃掉表格底部，不预留就会裁掉最后一行
    if (m_table->horizontalScrollBar()->isVisible())
        height += m_table->horizontalScrollBar()->height();
    m_table->setFixedHeight(height);
}

void JsonTableEditor::commitCell(QTableWidget *table, int row, int column)
{
    if (m_building || !table)
        return;
    if (row < 0 || row >= m_shape.rows.size())
        return;
    if (column < 0 || column >= m_shape.columns.size())
        return;
    if (column < m_shape.readonlyCount || m_shape.readonlyAttrs.contains(m_shape.columns.at(column)))
        return;
    QTableWidgetItem *item = table->item(row, column);
    if (!item)
        return;

    JsonTableCell cell;
    cell.value = cellValue(item->text(), item->data(Qt::UserRole).toString());
    QVector<JsonTableCell> &cells = m_shape.rows[row];
    if (column < cells.size()) {
        cells[column] = cell;
    } else {
        cells.resize(column + 1);
        cells[column] = cell;
    }

    m_building = true;
    const Palette &pal = gs::palette();
    if (looksBrokenCell(cell)) {
        item->setBackground(pal.errTint);
        item->setForeground(pal.errText);
    } else {
        item->setData(Qt::BackgroundRole, QVariant());
        item->setData(Qt::ForegroundRole, QVariant());
    }
    m_building = false;

    if (m_hint)
        m_hint->setText(anyBrokenCell() ? QStringLiteral("这个单元格里的 JSON 语法不合法")
                                        : QString());
}

void JsonTableEditor::removeRow(int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= m_shape.rows.size())
        return;
    m_shape.rows.remove(rowIndex);
    if (rowIndex < m_shape.groups.size())
        m_shape.groups.remove(rowIndex);
    if (rowIndex < m_shape.samples.size())
        m_shape.samples.remove(rowIndex);
    rebuildTable();
}

bool JsonTableEditor::anyBrokenCell() const
{
    for (const QVector<JsonTableCell> &row : m_shape.rows) {
        for (const JsonTableCell &cell : row) {
            if (looksBrokenCell(cell))
                return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- 回写

QVariant JsonTableEditor::buildGroupValue() const
{
    QStringList writable;
    for (const QString &key : m_shape.attrColumns) {
        if (!m_shape.readonlyAttrs.contains(key))
            writable.append(key);
    }

    QVariantList out;
    for (int index = 0; index < m_shape.rows.size(); ++index) {
        if (index >= m_shape.groups.size())
            break;
        const JsonTableGroup &group = m_shape.groups.at(index);
        const QVariantMap sample = index < m_shape.samples.size() ? m_shape.samples.at(index)
                                                                  : QVariantMap();
        const QVector<JsonTableCell> &row = m_shape.rows.at(index);

        // 可编辑属性打成一份 patch：组里每个段（domain / line / …）的每个条目都同步这一份
        QVariantMap patch;
        for (const QString &key : qAsConst(writable)) {
            const int column = 1 + m_shape.attrColumns.indexOf(key);
            if (column < 0 || column >= row.size())
                continue;
            patch.insert(key, groupCellValue(row.at(column).value, sample.value(key)));
        }

        QVariantMap body = group.body;
        for (auto it = body.begin(); it != body.end(); ++it) {
            const QVariant list = it.value();
            QVariantList items;
            if (isArray(list))
                items = list.toList();
            else if (isObject(list))
                items.append(list);
            else
                continue;
            for (QVariant &entry : items) {
                if (!isObject(entry))
                    continue;
                QVariantMap entryMap = entry.toMap();
                for (auto patchIt = patch.constBegin(); patchIt != patch.constEnd(); ++patchIt)
                    entryMap.insert(patchIt.key(), patchIt.value());
                entry = entryMap;
            }
            if (isArray(list))
                it.value() = items;
            else if (!items.isEmpty())
                it.value() = items.first();
        }

        QVariantMap single;
        single.insert(group.name, body);
        out.append(single);
    }
    return out;
}

QVariant JsonTableEditor::buildValue() const
{
    if (m_shape.mode == QLatin1String("groupRows"))
        return buildGroupValue();

    if (m_shape.mode == QLatin1String("pairs")) {
        QVariantMap out;
        for (const QVector<JsonTableCell> &row : m_shape.rows) {
            const QString key = row.isEmpty() ? QString() : row.at(0).value.toString().trimmed();
            if (key.isEmpty())
                continue;
            out.insert(key, row.size() > 1 ? row.at(1).value : QVariant());
        }
        return out;
    }

    if (m_shape.mode == QLatin1String("scalarRows")) {
        QVariantList out;
        for (const QVector<JsonTableCell> &row : m_shape.rows)
            out.append(row.isEmpty() ? QVariant() : row.at(0).value);
        return out;
    }

    QVariantList out;
    for (const QVector<JsonTableCell> &row : m_shape.rows) {
        QVariantMap item;
        for (int column = 0; column < m_shape.columns.size() && column < row.size(); ++column) {
            if (row.at(column).absent)
                continue;
            item.insert(m_shape.columns.at(column), row.at(column).value);
        }
        out.append(item);
    }
    return out;
}

void JsonTableEditor::apply()
{
    if (anyBrokenCell()) {
        if (m_hint)
            m_hint->setText(QStringLiteral("有单元格的 JSON 语法不合法，修正后再应用"));
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromVariant(buildValue());
    if (doc.isNull()) {
        if (m_hint)
            m_hint->setText(QStringLiteral("这些内容没法写成 JSON，请检查单元格"));
        return;
    }
    m_value = QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
    accept();
}

} // namespace gs
#ifndef GS_JSONEDITOR_H
#define GS_JSONEDITOR_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

QT_BEGIN_NAMESPACE
class QLabel;
class QResizeEvent;
class QScrollArea;
class QShowEvent;
class QTableWidget;
class QWidget;
QT_END_NAMESPACE

namespace gs {

// 表格里的一个单元格。JS 的 undefined 在 QVariant 里没有对应值：
// 对象数组的列是所有行键的并集，某行缺这个键就是 absent（重建 JSON 时直接不写这个字段）。
struct JsonTableCell
{
    QVariant value;
    bool absent = false;
};

// 一个分部件组（app.js jsonGroupShape 里的 {name, body}）
struct JsonTableGroup
{
    QString name;
    QVariantMap body;
};

// 表格化之后的参数形状，字段与 app.js jsonTableShape / jsonGroupShape 的返回对象一一对应。
// mode 为空串即 webui 的 "raw"：这个参数不是可表格化的 JSON。
struct JsonTableShape
{
    QString mode;   // groupRows / pairs / scalarRows / objectRows
    QStringList columns;
    QVector<QVector<JsonTableCell>> rows;

    // 仅 groupRows 使用
    QStringList attrColumns;      // 组属性列（不含行首的「组」）
    QStringList readonlyAttrs;    // 数组/对象型属性（ids）：只展示不回写
    int readonlyCount = 0;        // 行首只读列数（组名那一列）
    QVector<JsonTableGroup> groups;
    QVector<QVariantMap> samples; // 每组的样本条目（优先 domain，其次 line）
};

// 审批卡里 JSON 参数的表格编辑器：
// app.js parseJsonValue / jsonGroupShape / jsonGroupCellValue / jsonGroupApply /
// jsonTableShape / jsonCellText / jsonCellValue / openJsonTableEditor 的 Qt 版。
class JsonTableEditor : public QDialog
{
    Q_OBJECT
public:
    // 这个参数的文本能不能表格化（app.js: jsonTableShape(parseJsonValue(text)).mode !== "raw"）
    static bool canEdit(const QString &text);
    // 打开编辑器。用户点「应用到参数」时把新 JSON 文本写进 out 并返回 true；
    // 取消 / 关闭 / 文本不可表格化都返回 false（false 时 out 不被改动）。
    static bool edit(QWidget *parent, const QString &name, const QString &text, QString *out);

    explicit JsonTableEditor(QWidget *parent, const QString &name, const QString &text);
    bool hasShape() const { return !m_shape.mode.isEmpty(); }
    QString valueText() const { return m_value; }

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildUi(const QString &name);
    QString modeHint() const;
    void rebuildTable();
    void syncTableHeight();
    // 单元格文本落定：还原类型 + 判定嵌套 JSON 是否语法不通（标红）
    void commitCell(QTableWidget *table, int row, int col);
    void removeRow(int row);
    bool anyBrokenCell() const;
    QVariant buildValue() const;
    QVariant buildGroupValue() const;
    void apply();

    JsonTableShape m_shape;
    QTableWidget *m_table = nullptr;
    QScrollArea *m_scroll = nullptr;
    QLabel *m_hint = nullptr;
    QString m_value;
    // 建表 / 写格式时会再发 itemChanged，用它挡住递归
    bool m_building = false;
};

} // namespace gs

#endif // GS_JSONEDITOR_H
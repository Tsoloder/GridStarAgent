#ifndef GS_MARKDOWNVIEW_H
#define GS_MARKDOWNVIEW_H

#include <QString>
#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

// 行内 Markdown → Qt 富文本：`code`、**bold**、*em*、[text](http…)（与 app.js inlineMarkdown 一致）
QString inlineMarkdownToHtml(const QString &text);

// 与 app.js structuredBlocks 一致：摘出 ```json 围栏中含结构化键
// （options / tool_params / toolparams / workflow / phase_plan）的对象，
// visible 为剔除这些块后的可见文本，found 为按出现顺序解析出的对象。
struct StructuredBlocks
{
    QString visible;
    QVariantList found;
};
StructuredBlocks structuredBlocks(const QString &text);

// 块级 Markdown 渲染部件（与 app.js basicMarkdown 一致）：
// ``` 代码块 → QFrame.codeBlock(header+pre)；#~### → .mdHeading；列表 → .mdList；其余 → .mdParagraph
class MarkdownView : public QWidget
{
    Q_OBJECT
public:
    explicit MarkdownView(QWidget *parent = nullptr);

    void setText(const QString &markdown);
    QString text() const { return m_text; }
    bool hasVisibleContent() const;

private:
    void clearBlocks();
    void addCodeBlock(const QString &lang, const QString &code);
    void addHeading(const QString &text);
    void addParagraph(const QString &text);
    void flushList();

    QVBoxLayout *m_layout = nullptr;
    QString m_text;
    // 连续列表项合并为一个 QLabel（对应 <ul>/<ol>）
    QString m_listHtml;
    QString m_listType; // "ul" / "ol" / ""
    int m_listIndex = 0;
    bool m_hasContent = false;
};

} // namespace gs

#endif // GS_MARKDOWNVIEW_H

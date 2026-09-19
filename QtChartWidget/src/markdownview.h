#ifndef GS_MARKDOWNVIEW_H
#define GS_MARKDOWNVIEW_H

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

// 行内 Markdown → Qt 富文本，与 app.js inlineMarkdown 同序：
// 先整体转义 → `code` 换占位符 → 图片 → 链接 → 粗体 → 删除线 → 斜体 → 还原代码段。
// href 走 mdHref 白名单（https? / mailto / #?/ 开头 / 纯路径），其余降级成纯文本。
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

// 块级 Markdown 渲染部件，与 app.js basicMarkdown 同序：
// ``` 代码块 → QFrame.codeBlock(header+body)；#{1,6} → .mdHeading；分隔线 → .mdRule；
// 引用 → QFrame.mdQuote（递归同一套块级规则）；GFM 表格 → QFrame.mdTable（QGridLayout +
// 按需横向滚动）；列表 → .mdList（缩进嵌套 / 任务框 / 续行）；整行图片 → .mdImage；
// 其余 → .mdParagraph。
class MarkdownView : public QWidget
{
    Q_OBJECT
public:
    explicit MarkdownView(QWidget *parent = nullptr);

    void setText(const QString &markdown);
    QString text() const { return m_text; }
    bool hasVisibleContent() const;

private:
    // ``` 围栏代码块：先整体摘出，正文里换成占位行，围栏内文本不再参与块级/行内解析
    struct CodeFence
    {
        QString lang;
        QString code;
    };

    void clearBlocks();
    // 递归渲染块级内容（引用块用同一套规则渲染进自己的容器）；返回真正落地的块数
    int renderBlocks(const QStringList &lines, const QVector<CodeFence> &fences,
                     QVBoxLayout *layout, QWidget *parent);
    void addCodeBlock(QVBoxLayout *layout, QWidget *parent, const QString &lang, const QString &code);
    void addHeading(QVBoxLayout *layout, QWidget *parent, const QString &text, int level);
    void addParagraph(QVBoxLayout *layout, QWidget *parent, const QStringList &lines);
    void addRule(QVBoxLayout *layout, QWidget *parent);
    void addList(QVBoxLayout *layout, QWidget *parent, const QString &html);
    int addQuote(QVBoxLayout *layout, QWidget *parent, const QStringList &lines,
                 const QVector<CodeFence> &fences);
    void addImage(QVBoxLayout *layout, QWidget *parent, const QString &alt, const QString &src);
    void addTable(QVBoxLayout *layout, QWidget *parent, const QStringList &head,
                  const QStringList &aligns, const QVector<QStringList> &rows);

    QVBoxLayout *m_layout = nullptr;
    QString m_text;
    bool m_hasContent = false;
};

} // namespace gs

#endif // GS_MARKDOWNVIEW_H
#include "markdownview.h"
#include "commonwidgets.h"
#include "theme.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QVector>

namespace gs {

StructuredBlocks structuredBlocks(const QString &text)
{
    static const QRegularExpression jsonRe(QStringLiteral("```json\\s*([\\s\\S]*?)```"),
                                           QRegularExpression::CaseInsensitiveOption);
    static const QStringList keys{ QStringLiteral("options"), QStringLiteral("tool_params"),
                                   QStringLiteral("toolparams"), QStringLiteral("workflow"),
                                   QStringLiteral("phase_plan") };

    StructuredBlocks out;
    QString visible;
    int last = 0;
    QRegularExpressionMatchIterator it = jsonRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        visible += text.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();

        const QJsonDocument doc = QJsonDocument::fromJson(m.captured(1).toUtf8());
        const QVariantMap data = doc.isObject() ? doc.object().toVariantMap() : QVariantMap();
        bool structured = false;
        for (const QString &key : keys) {
            if (data.contains(key)) {
                structured = true;
                break;
            }
        }
        if (structured)
            out.found.append(data);
        else
            visible += m.captured(0); // 解析失败或无结构化键：原样保留
    }
    visible += text.mid(last);
    out.visible = visible.trimmed();
    return out;
}

QString inlineMarkdownToHtml(const QString &text)
{
    QString html = escapeHtml(text);
    static const QRegularExpression codeRe(QStringLiteral("`([^`]+)`"));
    static const QRegularExpression boldRe(QStringLiteral("\\*\\*([^*]+)\\*\\*"));
    static const QRegularExpression emRe(QStringLiteral("\\*([^*]+)\\*"));
    static const QRegularExpression linkRe(
        QStringLiteral("\\[([^\\]]+)\\]\\((https?://[^\\s)]+)\\)"));

    html.replace(codeRe,
                 QStringLiteral("<code style=\"background-color:#0e171e;color:#9bdcf2;"
                                "font-family:Consolas,'Courier New',monospace;\">\\1</code>"));
    html.replace(boldRe, QStringLiteral("<strong>\\1</strong>"));
    html.replace(emRe, QStringLiteral("<em>\\1</em>"));
    html.replace(linkRe, QStringLiteral("<a href=\"\\2\">\\1</a>"));
    return html;
}

MarkdownView::MarkdownView(QWidget *parent) : QWidget(parent)
{
    setClass(this, QStringLiteral("markdownView"));
    setAttribute(Qt::WA_StyledBackground, true);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_layout->setAlignment(Qt::AlignTop);
}

void MarkdownView::clearBlocks()
{
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            delete w;
        delete item;
    }
    m_listHtml.clear();
    m_listType.clear();
    m_listIndex = 0;
    m_hasContent = false;
}

void MarkdownView::flushList()
{
    if (m_listType.isEmpty())
        return;
    QLabel *label = makeLabel(QStringLiteral("mdList"), m_listHtml, this);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(true);
    label->setContentsMargins(20, 5, 0, 5); // CSS: ul/ol padding-left 20px; margin 5px 0
    m_layout->addWidget(label);
    m_listHtml.clear();
    m_listType.clear();
    m_listIndex = 0;
    m_hasContent = true;
}

void MarkdownView::addCodeBlock(const QString &lang, const QString &code)
{
    auto *frame = new QFrame(this);
    setClass(frame, QStringLiteral("codeBlock"));
    auto *lay = new QVBoxLayout(frame);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    QLabel *header = makeLabel(QStringLiteral("codeBlockHeader"),
                               lang.isEmpty() ? QStringLiteral("text") : lang, frame);
    QLabel *body = makeLabel(QStringLiteral("codeBlockBody"), code.trimmed(), frame);
    body->setTextFormat(Qt::PlainText);
    body->setWordWrap(true);

    lay->addWidget(header);
    lay->addWidget(body);

    m_layout->addSpacing(8); // CSS: .code-block margin 8px 0
    m_layout->addWidget(frame);
    m_layout->addSpacing(8);
    m_hasContent = true;
}

void MarkdownView::addHeading(const QString &text)
{
    QLabel *label = makeLabel(QStringLiteral("mdHeading"), inlineMarkdownToHtml(text), this);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(true);
    label->setContentsMargins(0, 9, 0, 5); // CSS: h1~h3 margin 9px 0 5px
    m_layout->addWidget(label);
    m_hasContent = true;
}

void MarkdownView::addParagraph(const QString &text)
{
    QLabel *label = makeLabel(QStringLiteral("mdParagraph"), inlineMarkdownToHtml(text), this);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(true);
    label->setContentsMargins(0, 0, 0, 7); // CSS: .markdown p margin 0 0 7px
    m_layout->addWidget(label);
    m_hasContent = true;
}

void MarkdownView::setText(const QString &markdown)
{
    m_text = markdown;
    clearBlocks();
    if (markdown.isEmpty())
        return;

    // 1. 先摘出 ``` 代码块，替换为占位行（与 app.js basicMarkdown 一致）
    struct Block { QString lang; QString code; };
    QVector<Block> blocks;
    QString source = markdown;
    static const QRegularExpression fenceRe(QStringLiteral("```([\\w-]*)\\n?([\\s\\S]*?)```"));
    QRegularExpressionMatchIterator it = fenceRe.globalMatch(source);
    QString rebuilt;
    int last = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        rebuilt += source.mid(last, m.capturedStart() - last);
        rebuilt += QChar('\0') + QString::number(blocks.size()) + QChar('\0');
        blocks.append({ m.captured(1), m.captured(2) });
        last = m.capturedEnd();
    }
    rebuilt += source.mid(last);
    source = rebuilt;

    // 2. 逐行解析
    static const QRegularExpression tokenRe(QStringLiteral("^\\0(\\d+)\\0$"));
    static const QRegularExpression headingRe(QStringLiteral("^(#{1,3})\\s+(.+)"));
    static const QRegularExpression itemRe(QStringLiteral("^\\s*([-*]|\\d+\\.)\\s+(.+)"));

    const QStringList lines = source.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch token = tokenRe.match(line);
        if (token.hasMatch()) {
            flushList();
            const Block &b = blocks.at(token.captured(1).toInt());
            addCodeBlock(b.lang, b.code);
            continue;
        }
        const QRegularExpressionMatch heading = headingRe.match(line);
        if (heading.hasMatch()) {
            flushList();
            addHeading(heading.captured(2));
            continue;
        }
        const QRegularExpressionMatch item = itemRe.match(line);
        if (item.hasMatch()) {
            const QString type = item.captured(1).endsWith(QLatin1Char('.'))
                                     ? QStringLiteral("ol")
                                     : QStringLiteral("ul");
            if (m_listType != type)
                flushList();
            m_listType = type;
            if (type == QLatin1String("ol"))
                m_listHtml += QString::number(++m_listIndex) + QStringLiteral(". ");
            else
                m_listHtml += QStringLiteral("• ");
            m_listHtml += inlineMarkdownToHtml(item.captured(2)) + QStringLiteral("<br>");
            continue;
        }
        flushList();
        if (!line.trimmed().isEmpty())
            addParagraph(line);
    }
    flushList();
}

bool MarkdownView::hasVisibleContent() const
{
    return m_hasContent;
}

} // namespace gs

#include "markdownview.h"
#include "commonwidgets.h"
#include "theme.h"

#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

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

// ------------------------------------------------------------ 行内 Markdown

namespace {

const QChar kCodeToken(0x01); // 代码段占位符：先抽出 `code`，全部行内规则跑完再还原
const QChar kPipeToken(0x02); // 表格单元格里 \| 的占位符，切列前护住、切完还原

// 与 app.js mdHref 一致：只放行 http(s) / mailto / 锚点相对路径 / 纯路径，
// javascript:、data: 等命不中白名单的一律降级成纯文本
QString mdHref(const QString &raw)
{
    const QString href = raw.trimmed();
    if (href.isEmpty())
        return QString();
    static const QRegularExpression schemeRe(QStringLiteral("^(?:https?:|mailto:)"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression anchorRe(QStringLiteral("^[#/?]"));
    static const QRegularExpression pathRe(QStringLiteral("^[\\w.\\-/]+$"));
    if (schemeRe.match(href).hasMatch())
        return href;
    if (anchorRe.match(href).hasMatch() || pathRe.match(href).hasMatch())
        return href;
    return QString();
}

// 逐个匹配做自定义替换（Qt 5.12 的 QString::replace 不接受回调）
template <typename Fn>
QString replaceEach(const QString &input, const QRegularExpression &re, Fn fn)
{
    QString out;
    out.reserve(input.size());
    int last = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(input);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += input.mid(last, m.capturedStart() - last);
        out += fn(m);
        last = m.capturedEnd();
    }
    out += input.mid(last);
    return out;
}

// 本地存在的图片文件按最大宽度（420px）等比缩放后的显示尺寸；
// 远程 URL / 不存在的路径返回空尺寸，由调用方退化成 alt 占位——渲染层不做任何网络访问
QSize localImageSize(const QString &path)
{
    if (path.isEmpty())
        return QSize();
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return QSize();
    QImageReader reader(info.absoluteFilePath());
    QSize size = reader.size();
    if (!size.isValid() || size.isEmpty())
        return QSize();
    const int maxWidth = scaledPx(420);
    if (size.width() > maxWidth)
        size = QSize(maxWidth, qMax(1, qRound(qreal(size.height()) * maxWidth / size.width())));
    return size;
}

// GFM 管道表格切列：`\|` 是单元格内写竖线的转义写法；行内没有 | 时不是表格行
bool tableCells(const QString &line, QStringList *cells)
{
    const QString raw = line.trimmed();
    if (!raw.contains(QLatin1Char('|')))
        return false;
    QString work = raw;
    work.replace(QStringLiteral("\\|"), QString(kPipeToken));
    if (work.startsWith(QLatin1Char('|')))
        work.remove(0, 1);
    if (work.endsWith(QLatin1Char('|')))
        work.chop(1);
    const QStringList parts = work.split(QLatin1Char('|'));
    for (const QString &part : parts) {
        QString cell = part.trimmed();
        cell.replace(kPipeToken, QLatin1Char('|'));
        cells->append(cell);
    }
    return true;
}

// 列对齐：两侧都有 : 居中，只有尾部 : 右对齐，其余左对齐
QString tableAlign(const QString &cell)
{
    const bool left = cell.startsWith(QLatin1Char(':'));
    const bool right = cell.endsWith(QLatin1Char(':'));
    if (left && right)
        return QStringLiteral("center");
    if (right)
        return QStringLiteral("right");
    return QStringLiteral("left");
}

Qt::Alignment cellAlignment(const QString &name)
{
    if (name == QLatin1String("center"))
        return Qt::AlignHCenter;
    if (name == QLatin1String("right"))
        return Qt::AlignRight;
    return Qt::AlignLeft;
}

} // namespace

QString inlineMarkdownToHtml(const QString &text)
{
    static const QRegularExpression codeSpanRe(QStringLiteral("`([^`]+)`"));
    static const QRegularExpression imageRe(QStringLiteral("!\\[([^\\]]*)\\]\\(([^\\s)]+)\\)"));
    static const QRegularExpression linkRe(QStringLiteral("\\[([^\\]]+)\\]\\(([^\\s)]+)\\)"));
    static const QRegularExpression boldStarRe(QStringLiteral("\\*\\*([^*]+)\\*\\*"));
    static const QRegularExpression boldUnderRe(QStringLiteral("__([^_]+)__"));
    static const QRegularExpression delRe(QStringLiteral("~~([^~]+)~~"));
    static const QRegularExpression emStarRe(QStringLiteral("\\*([^*]+)\\*"));
    static const QRegularExpression emUnderRe(QStringLiteral("(^|[^\\w])_([^_]+)_(?=$|[^\\w])"));
    static const QRegularExpression codeTokenRe(
        QString(kCodeToken) + QStringLiteral("(\\d+)") + QString(kCodeToken));

    const Palette &p = gs::palette();
    const QString linkColor = cssColor(p.cyan);
    const QString mutedColor = cssColor(p.muted2);
    // 代码芯片：底色/文字色跟着调色板走，字号不写死（继承 QSS 的基础字号，随缩放一起变）
    const QString chipStyle =
        QStringLiteral("background-color:%1;color:%2;font-family:%3;padding:1px 4px;")
            .arg(cssColor(p.inset), cssColor(p.cyan), monoFont());

    QVector<QString> codes;
    QString html = escapeHtml(text);

    // 1) `code` 换成占位符，避免代码里的 ** / _ / ~~ 被后续规则二次解析
    html = replaceEach(html, codeSpanRe, [&](const QRegularExpressionMatch &m) {
        const QString token = QString(kCodeToken) + QString::number(codes.size()) + QString(kCodeToken);
        codes.append(QStringLiteral("<code style=\"%1\">%2</code>").arg(chipStyle, m.captured(1)));
        return token;
    });

    // 2) 图片与链接：白名单之外原样保留（等于降级成纯文本）
    html = replaceEach(html, imageRe, [&](const QRegularExpressionMatch &m) {
        const QString src = mdHref(m.captured(2));
        if (src.isEmpty())
            return m.captured(0);
        const QSize box = localImageSize(src);
        if (!box.isEmpty())
            return QStringLiteral("<img src=\"%1\" alt=\"%2\" width=\"%3\" height=\"%4\">")
                .arg(src, m.captured(1))
                .arg(box.width())
                .arg(box.height());
        const QString alt = m.captured(1).trimmed();
        return QStringLiteral("<span style=\"color:%1;\">%2</span>")
            .arg(mutedColor, alt.isEmpty() ? escapeHtml(src) : alt);
    });
    html = replaceEach(html, linkRe, [&](const QRegularExpressionMatch &m) {
        const QString href = mdHref(m.captured(2));
        if (href.isEmpty())
            return m.captured(0);
        return QStringLiteral("<a href=\"%1\" style=\"color:%2;\">%3</a>")
            .arg(href, linkColor, m.captured(1));
    });

    // 3) 强调：粗体 → 删除线 → 斜体（_em_ 两侧需非单词字符边界）
    html.replace(boldStarRe, QStringLiteral("<strong>\\1</strong>"));
    html.replace(boldUnderRe, QStringLiteral("<strong>\\1</strong>"));
    html.replace(delRe, QStringLiteral("<del style=\"color:%1;\">\\1</del>").arg(mutedColor));
    html.replace(emStarRe, QStringLiteral("<em>\\1</em>"));
    html.replace(emUnderRe, QStringLiteral("\\1<em>\\2</em>"));

    // 4) 还原代码段
    html = replaceEach(html, codeTokenRe, [&](const QRegularExpressionMatch &m) {
        const int index = m.captured(1).toInt();
        return (index >= 0 && index < codes.size()) ? codes.at(index) : QString();
    });
    return html;
}

// ------------------------------------------------------------ 块级 Markdown

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
    m_hasContent = false;
}

void MarkdownView::addCodeBlock(QVBoxLayout *layout, QWidget *parent, const QString &lang,
                                const QString &code)
{
    auto *frame = new QFrame(parent);
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

    layout->addSpacing(scaledPx(8)); // CSS: .code-block margin 8px 0
    layout->addWidget(frame);
    layout->addSpacing(scaledPx(8));
}

void MarkdownView::addHeading(QVBoxLayout *layout, QWidget *parent, const QString &text, int level)
{
    QString html = inlineMarkdownToHtml(text);
    // h1~h3 直接用 .mdHeading 的字号（QSS 里已随缩放展开）；h4~h6 在 CSS 里小一档
    if (level > 3)
        html = QStringLiteral("<span style=\"font-size:%1px;\">%2</span>")
                   .arg(scaledPx(13))
                   .arg(html);

    QLabel *label = makeLabel(QStringLiteral("mdHeading"), html, parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(true);
    label->setContentsMargins(0, scaledPx(9), 0, scaledPx(5)); // CSS: h1~h6 margin 9px 0 5px
    layout->addWidget(label);
}

void MarkdownView::addParagraph(QVBoxLayout *layout, QWidget *parent, const QStringList &lines)
{
    static const QRegularExpression trailingRe(QStringLiteral("(?: {2,}|\\\\)+$"));
    static const QRegularExpression hardBreakRe(QStringLiteral("(?: {2,}|\\\\)$"));

    // 连续行合并成一个块：软换行按空白连接，行尾 2 空格或反斜杠为硬换行
    QString html;
    for (int i = 0; i < lines.size(); ++i) {
        QString stripped = lines.at(i);
        stripped.remove(trailingRe);
        html += inlineMarkdownToHtml(stripped);
        if (i + 1 < lines.size())
            html += hardBreakRe.match(lines.at(i)).hasMatch() ? QStringLiteral("<br>")
                                                              : QStringLiteral("\n");
    }

    QLabel *label = makeLabel(QStringLiteral("mdParagraph"), html, parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(true);
    label->setContentsMargins(0, 0, 0, scaledPx(7)); // CSS: .markdown p margin 0 0 7px
    layout->addWidget(label);
}

void MarkdownView::addRule(QVBoxLayout *layout, QWidget *parent)
{
    QLabel *label = makeLabel(QStringLiteral("mdRule"), QString(), parent);
    label->setFixedHeight(qMax(1, scaledPx(1)));
    label->setAttribute(Qt::WA_StyledBackground, true);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addSpacing(scaledPx(10)); // CSS: hr margin 10px 0
    layout->addWidget(label);
    layout->addSpacing(scaledPx(10));
}

void MarkdownView::addList(QVBoxLayout *layout, QWidget *parent, const QString &html)
{
    // 嵌套层级以每个列表项的 margin-left 体现，所以这里只留 CSS 的 ul/ol margin 5px 0
    QLabel *label = makeLabel(QStringLiteral("mdList"), html, parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(true);
    label->setContentsMargins(0, scaledPx(5), 0, scaledPx(5));
    layout->addWidget(label);
}

int MarkdownView::addQuote(QVBoxLayout *layout, QWidget *parent, const QStringList &lines,
                           const QVector<CodeFence> &fences)
{
    auto *frame = new QFrame(parent);
    setClass(frame, QStringLiteral("mdQuote"));
    // theme.cpp 里的 .mdQuote 只覆盖 QLabel，而引用块要能放表格/代码块等子控件，
    // 所以左侧 2px 强调竖线在代码里补上（颜色仍取调色板）
    frame->setStyleSheet(
        QStringLiteral("QFrame.mdQuote { border-left: 2px solid %1; background: transparent; }")
            .arg(cssColor(gs::palette().cyanMid)));
    auto *inner = new QVBoxLayout(frame);
    inner->setContentsMargins(scaledPx(10), scaledPx(2), 0, scaledPx(2)); // CSS: padding 2px 0 2px 10px
    inner->setSpacing(0);

    const int count = renderBlocks(lines, fences, inner, frame);
    if (count <= 0) {
        delete frame; // 引用里没有实际内容：不留下空容器
        return 0;
    }
    layout->addSpacing(scaledPx(8)); // CSS: blockquote margin 8px 0
    layout->addWidget(frame);
    layout->addSpacing(scaledPx(8));
    return count;
}

void MarkdownView::addImage(QVBoxLayout *layout, QWidget *parent, const QString &alt,
                            const QString &src)
{
    const QString href = mdHref(src);
    const QSize box = href.isEmpty() ? QSize() : localImageSize(href);

    QLabel *label = nullptr;
    if (!box.isEmpty()) {
        QPixmap pixmap;
        if (pixmap.load(href)) {
            label = makeLabel(QStringLiteral("mdImage"), QString(), parent);
            label->setPixmap(pixmap.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    if (label) {
        label->setContentsMargins(0, scaledPx(6), 0, scaledPx(6)); // CSS: img margin 6px 0
    } else {
        // 远程 URL / 本地不存在：不发起网络请求，显示带 alt 的占位
        const QString placeholder = alt.trimmed().isEmpty() ? src : alt;
        label = makeLabel(QStringLiteral("mdImage"),
                          QStringLiteral("<span style=\"color:%1;\">%2</span>")
                              .arg(cssColor(gs::palette().muted2), escapeHtml(placeholder)),
                          parent);
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        label->setContentsMargins(scaledPx(8), scaledPx(6), scaledPx(8), scaledPx(6));
    }
    layout->addWidget(label);
}

void MarkdownView::addTable(QVBoxLayout *layout, QWidget *parent, const QStringList &head,
                            const QStringList &aligns, const QVector<QStringList> &rows)
{
    const int columns = head.size();
    if (columns <= 0)
        return;

    const Palette &p = gs::palette();
    const int gap = qMax(1, scaledPx(1)); // 网格缝隙 1px：容器底色透出来就是水平/垂直分隔线
    const int padH = scaledPx(9);
    const int padV = scaledPx(6);

    auto *frame = new QFrame(parent);
    setClass(frame, QStringLiteral("mdTable"));
    auto *frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(0, 0, 0, 0);
    frameLayout->setSpacing(0);

    auto *scroll = new QScrollArea(frame);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setStyleSheet(QStringLiteral("background: transparent;"));

    auto *grid = new QWidget;
    grid->setStyleSheet(QStringLiteral("background-color:%1;").arg(cssColor(p.line)));
    auto *gridLayout = new QGridLayout(grid);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setSpacing(gap);

    auto makeCell = [&](const QString &cls, const QString &text, const QString &align,
                        const QColor &background, bool wrap) {
        QLabel *cell = makeLabel(cls, inlineMarkdownToHtml(text), grid);
        cell->setTextFormat(Qt::RichText);
        cell->setWordWrap(wrap);
        cell->setAlignment(cellAlignment(align) | Qt::AlignVCenter);
        cell->setContentsMargins(padH, padV, padH, padV); // CSS: th/td padding 6px 9px
        cell->setStyleSheet(QStringLiteral("background-color:%1;").arg(cssColor(background)));
        return cell;
    };

    for (int c = 0; c < columns; ++c)
        gridLayout->addWidget(makeCell(QStringLiteral("mdTableHead"), head.value(c),
                                       aligns.value(c), p.band3, false),
                              0, c);

    for (int r = 0; r < rows.size(); ++r) {
        // CSS: tbody tr:nth-child(even) 用 --band-2，其余行透出表格底色 --inset
        const QColor background = (r % 2 == 1) ? p.band2 : p.inset;
        for (int c = 0; c < columns; ++c)
            gridLayout->addWidget(makeCell(QStringLiteral("mdTableCell"), rows.at(r).value(c),
                                           aligns.value(c), background, true),
                                  r + 1, c);
    }
    gridLayout->setColumnStretch(columns - 1, 1);
    if (!rows.isEmpty())
        gridLayout->setRowStretch(rows.size(), 1);

    scroll->setWidget(grid);
    scroll->ensurePolished(); // 先把 QSS（表格 11px 字号）烙上，量出来的自然宽度才是最终渲染宽度

    // 列宽取表头与单元格的自然宽度：宽度不够时由 QScrollArea 出横向滚动条，
    // 高度取自然高度并预留横向滚动条的位置，保证 widgetResizable 下内容既不裁切也不多出空白
    const QSize natural = grid->sizeHint();
    grid->setMinimumWidth(natural.width());
    scroll->setFixedHeight(natural.height()
                           + qMax(scaledPx(8), scroll->horizontalScrollBar()->sizeHint().height()));
    frameLayout->addWidget(scroll);

    layout->addSpacing(scaledPx(8)); // CSS: .md-table-wrap margin 8px 0
    layout->addWidget(frame);
    layout->addSpacing(scaledPx(8));
}

int MarkdownView::renderBlocks(const QStringList &lines, const QVector<CodeFence> &fences,
                               QVBoxLayout *layout, QWidget *parent)
{
    static const QRegularExpression tokenRe(QStringLiteral("^") + QString(kCodeToken)
                                            + QStringLiteral("(\\d+)") + QString(kCodeToken)
                                            + QStringLiteral("$"));
    static const QRegularExpression headingRe(QStringLiteral("^\\s*(#{1,6})\\s+(.+?)\\s*#*\\s*$"));
    static const QRegularExpression ruleRe(QStringLiteral("^\\s*(?:[-*_]\\s*){3,}$"));
    static const QRegularExpression quoteRe(QStringLiteral("^\\s*>\\s?"));
    static const QRegularExpression itemRe(QStringLiteral("^(\\s*)([-*+]|\\d+[.)])\\s+(.*)$"));
    static const QRegularExpression taskRe(QStringLiteral("^\\[([ xX])\\]\\s*(.*)$"));
    static const QRegularExpression dividerRe(
        QStringLiteral("^\\s*\\|?\\s*:?-{2,}:?\\s*(?:\\|\\s*:?-{2,}:?\\s*)*\\|?\\s*$"));
    static const QRegularExpression imageOnlyRe(
        QStringLiteral("^\\s*!\\[([^\\]]*)\\]\\(([^\\s)]+)\\)\\s*$"));

    struct Level
    {
        QString type; // "ul" / "ol"
        int indent;
    };

    QVector<Level> levels; // 列表层级栈
    QVector<int> counters; // 与 levels 等长：ol 当前序号
    QStringList para;
    QStringList quote;
    QString listHtml;
    int itemInsertAt = -1; // 最近一个列表项的内容末尾：续行并回这里
    bool blank = false;    // 空行后列表先不急着关，等下一行确认不是列表项再关
    int added = 0;

    auto closeList = [&]() {
        if (!listHtml.isEmpty()) {
            addList(layout, parent, listHtml);
            ++added;
        }
        listHtml.clear();
        itemInsertAt = -1;
        levels.clear();
        counters.clear();
    };
    auto flushPara = [&]() {
        if (para.isEmpty())
            return;
        addParagraph(layout, parent, para);
        para.clear();
        ++added;
    };
    auto flushQuote = [&]() {
        if (quote.isEmpty())
            return;
        const QStringList inner = quote;
        quote.clear();
        added += addQuote(layout, parent, inner, fences);
    };
    auto flushAll = [&]() {
        flushPara();
        flushQuote();
        closeList();
    };

    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);

        const QRegularExpressionMatch token = tokenRe.match(line);
        if (token.hasMatch()) {
            flushAll();
            const int index = token.captured(1).toInt();
            if (index >= 0 && index < fences.size()) {
                addCodeBlock(layout, parent, fences.at(index).lang, fences.at(index).code);
                ++added;
            }
            continue;
        }

        if (line.trimmed().isEmpty()) {
            flushPara();
            flushQuote();
            blank = true;
            continue;
        }

        const QRegularExpressionMatch item = itemRe.match(line);
        if (blank) {
            blank = false;
            if (!item.hasMatch())
                closeList();
        }

        const QRegularExpressionMatch heading = headingRe.match(line);
        if (heading.hasMatch()) {
            flushAll();
            addHeading(layout, parent, heading.captured(2), heading.captured(1).size());
            ++added;
            continue;
        }

        if (ruleRe.match(line).hasMatch()) {
            flushAll();
            addRule(layout, parent);
            ++added;
            continue;
        }

        const QRegularExpressionMatch quoteMark = quoteRe.match(line);
        if (quoteMark.hasMatch()) {
            flushPara();
            closeList();
            quote.append(line.mid(quoteMark.capturedLength()));
            continue;
        }

        QStringList headCells;
        if (tableCells(line, &headCells) && i + 1 < lines.size()) {
            QStringList dividerCells;
            if (tableCells(lines.at(i + 1), &dividerCells) && dividerCells.size() == headCells.size()
                && dividerRe.match(lines.at(i + 1)).hasMatch()) {
                flushAll();
                QStringList aligns;
                aligns.reserve(dividerCells.size());
                for (const QString &cell : dividerCells)
                    aligns.append(tableAlign(cell));
                QVector<QStringList> rows;
                int j = i + 2;
                for (; j < lines.size(); ++j) {
                    QStringList cells;
                    if (!tableCells(lines.at(j), &cells) || lines.at(j).trimmed().isEmpty())
                        break;
                    rows.append(cells);
                }
                addTable(layout, parent, headCells, aligns, rows);
                ++added;
                i = j - 1;
                continue;
            }
        }

        if (item.hasMatch()) {
            flushPara();
            flushQuote();
            QString indentText = item.captured(1);
            indentText.replace(QLatin1Char('\t'), QStringLiteral("  ")); // tab 视作 2 空格
            const int indent = indentText.size();
            const QString marker = item.captured(2);
            const bool ordered = marker.at(0).isDigit();
            const QString type = ordered ? QStringLiteral("ol") : QStringLiteral("ul");
            const int number = ordered ? marker.left(marker.size() - 1).toInt() : 0;

            // 缩进决定嵌套层级：比当前层浅的先收掉，同级换类型也收掉
            while (!levels.isEmpty() && levels.last().indent > indent) {
                levels.removeLast();
                counters.removeLast();
            }
            if (!levels.isEmpty() && levels.last().indent == indent && levels.last().type != type) {
                levels.removeLast();
                counters.removeLast();
            }
            if (levels.isEmpty() || levels.last().indent < indent) {
                levels.append({ type, indent });
                counters.append(ordered && number > 1 ? number : 1);
            }

            const QString content = item.captured(3);
            const QRegularExpressionMatch task = taskRe.match(content);
            QString head;
            QString body = content;
            if (task.hasMatch()) {
                // 任务列表：已勾选 ☑ / 未勾选 ☐，且不显示项目符号
                const QString mark = task.captured(1).toLower() == QLatin1String("x")
                                         ? QStringLiteral("☑")
                                         : QStringLiteral("☐");
                head = QStringLiteral("<span style=\"color:%1;\">%2</span>&nbsp;")
                           .arg(cssColor(gs::palette().cyan), mark);
                body = task.captured(2);
            } else if (ordered) {
                head = QString::number(counters.last()) + QStringLiteral(". ");
                counters.last() += 1;
            } else {
                head = QStringLiteral("• ");
            }

            listHtml += QStringLiteral("<div style=\"margin-left:%1px;\">%2%3")
                            .arg(scaledPx(levels.size() * 20))
                            .arg(head, inlineMarkdownToHtml(body));
            itemInsertAt = listHtml.size();
            listHtml += QStringLiteral("</div>");
            continue;
        }

        // 列表项续行（GFM lazy continuation）：并进上一个列表项，不再另起段落
        if (itemInsertAt >= 0) {
            const QString extra = QStringLiteral(" ") + inlineMarkdownToHtml(line.trimmed());
            listHtml.insert(itemInsertAt, extra);
            itemInsertAt += extra.size();
            continue;
        }

        flushQuote();

        const QRegularExpressionMatch image = imageOnlyRe.match(line);
        if (image.hasMatch()) {
            flushPara();
            addImage(layout, parent, image.captured(1), image.captured(2));
            ++added;
            continue;
        }

        para.append(line);
    }

    flushAll();
    return added;
}

void MarkdownView::setText(const QString &markdown)
{
    m_text = markdown;
    clearBlocks();
    if (markdown.trimmed().isEmpty()) // 空文本 / 纯空白：没有可见内容
        return;

    // 1. 先整体摘出 ``` 围栏，正文里换成占位行（与 app.js basicMarkdown 一致）
    QVector<CodeFence> fences;
    QString source = markdown;
    static const QRegularExpression fenceRe(QStringLiteral("```([\\w-]*)\\n?([\\s\\S]*?)```"));
    {
        QString rebuilt;
        int last = 0;
        QRegularExpressionMatchIterator it = fenceRe.globalMatch(source);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            rebuilt += source.mid(last, m.capturedStart() - last);
            rebuilt += QString(kCodeToken) + QString::number(fences.size()) + QString(kCodeToken);
            fences.append({ m.captured(1), m.captured(2) });
            last = m.capturedEnd();
        }
        rebuilt += source.mid(last);
        source = rebuilt;
    }

    // 2. 逐行解析块级结构
    QStringList lines = source.split(QLatin1Char('\n'));
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
    }

    m_hasContent = renderBlocks(lines, fences, m_layout, this) > 0;
}

bool MarkdownView::hasVisibleContent() const
{
    return m_hasContent;
}

} // namespace gs
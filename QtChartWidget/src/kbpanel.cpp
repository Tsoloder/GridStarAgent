#include "kbpanel.h"

#include "commonwidgets.h"
#include "popups.h"
#include "theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <functional>

namespace gs {

namespace {

// 资料行拖拽的私有 mime：只在本面板内部识别，不存在「把文件拖进窗口」那种系统拖放，
// 所以不会跟 ChartWidget 自己的文件拖放抢事件。
const char *const kDocMime = "application/x-gridstar-kb-document";

QString docIdFromMime(const QMimeData *mime)
{
    if (!mime)
        return QString();
    const QString format = QString::fromLatin1(kDocMime);
    if (!mime->hasFormat(format))
        return QString();
    return QString::fromUtf8(mime->data(format));
}

// app.js formatSize：< 1KB 给字节，其余 KB（无小数）/ MB（一位小数）
QString formatSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
    return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
}

// status.db_bytes / doc.size 从 JSON 过来是 double 或字符串
qint64 toBytes(const QVariant &value)
{
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (ok)
        return static_cast<qint64>(number);
    return static_cast<qint64>(value.toString().toDouble());
}

QString textOf(const QVariantMap &map, const char *key)
{
    return map.value(QString::fromLatin1(key)).toString();
}

int intOf(const QVariantMap &map, const char *key)
{
    return map.value(QString::fromLatin1(key)).toInt();
}

// JS 真值：null / false / 0 / "" 都算没有（app.js 里 `row.default ? ... : ""`）
bool jsTruthy(const QVariant &value)
{
    if (!value.isValid() || value.isNull())
        return false;
    switch (value.userType()) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return value.toLongLong() != 0;
    case QMetaType::Double:
        return value.toDouble() != 0.0;
    default:
        return !value.toString().isEmpty();
    }
}

// `row.min != null`：JSON null 进来是无效 QVariant
bool hasNumber(const QVariant &value)
{
    if (!value.isValid() || value.isNull())
        return false;
    if (value.userType() == QMetaType::QString)
        return !value.toString().isEmpty();
    return true;
}

QVariantList listedList(const QVariantMap &map, const char *key)
{
    return map.value(QString::fromLatin1(key)).toList();
}

QPushButton *makeButton(const QString &text, const QString &variant, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    if (!variant.isEmpty())
        button->setProperty("variant", variant);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::TabFocus);
    return button;
}

QLabel *panelLabel(const QString &cls, const QString &text, QWidget *parent)
{
    QLabel *label = makeLabel(cls, text, parent);
    return label;
}

void clearBoxLayout(QLayout *layout)
{
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            // 先藏再删：deleteLater 要等回到事件循环，期间它已经不在布局里了
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

// 内层滚动区高度跟内容走（style.css .kb-tree max-height:360px / .kb-docs max-height:300px）：
// 内容少时不该留一大片空白，超过上限再滚。
void fitScrollHeight(QScrollArea *scroll, int maxHeight)
{
    if (!scroll || !scroll->widget())
        return;
    QWidget *inner = scroll->widget();
    inner->ensurePolished();
    if (QLayout *layout = inner->layout()) {
        layout->invalidate();
        layout->activate();
    }
    const int floor = scaledPx(24);
    const int wanted = inner->sizeHint().height();
    scroll->setFixedHeight(qBound(floor, wanted, qMax(floor, maxHeight)));
}

} // namespace

// ------------------------------------------------------------ KbTreeRow

KbTreeRow::KbTreeRow(const QString &key, const QString &label, int count, int depth,
                     bool hasChildren, bool expanded, QWidget *parent)
    : QWidget(parent), m_key(key), m_label(label), m_count(count), m_depth(depth),
      m_hasChildren(hasChildren), m_expanded(expanded)
{
    setClass(this, QStringLiteral("kbTreeRow"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAcceptDrops(true);
    setProperty("active", false);
    setProperty("dropTarget", false);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(this);
    // .kb-tree-row 的 paddingLeft = 6 + depth*12（webui 用行内 style 写死）
    layout->setContentsMargins(scaledPx(6 + m_depth * 12), scaledPx(5), scaledPx(6), scaledPx(5));
    layout->setSpacing(scaledPx(5));

    m_twisty = new QLabel(this);
    setClass(m_twisty, QStringLiteral("kbTreeTwisty"));
    m_twisty->setFixedWidth(scaledPx(13));
    m_twisty->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_twisty, 0, Qt::AlignVCenter);

    m_name = new ElidedLabel(this);
    setClass(m_name, QStringLiteral("kbTreeName"));
    m_name->setFullText(label);
    layout->addWidget(m_name, 1, Qt::AlignVCenter);

    m_countLabel = new QLabel(this);
    setClass(m_countLabel, QStringLiteral("kbTreeCount"));
    m_countLabel->setText(QString::number(count));
    m_countLabel->setAlignment(Qt::AlignCenter);
    m_countLabel->setVisible(count > 0);
    layout->addWidget(m_countLabel, 0, Qt::AlignVCenter);

    updateTwisty();
}

void KbTreeRow::updateTwisty()
{
    if (!m_twisty)
        return;
    if (!m_hasChildren) {
        m_twisty->setText(QString());
        return;
    }
    m_twisty->setText(m_expanded ? QStringLiteral("▾") : QStringLiteral("▸"));
}

void KbTreeRow::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    setProperty("active", active);
    restyle(this);
}

void KbTreeRow::setExpanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;
    updateTwisty();
}

void KbTreeRow::setDropTarget(bool on)
{
    if (m_dropTarget == on)
        return;
    m_dropTarget = on;
    setProperty("dropTarget", on);
    restyle(this);
}

void KbTreeRow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        setFocus(Qt::MouseFocusReason);
        if (m_hasChildren && m_twisty && m_twisty->geometry().contains(event->pos())) {
            emit twistyToggled(m_key);
            return;
        }
        emit activated(m_key);
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void KbTreeRow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
        || event->key() == Qt::Key_Space) {
        emit activated(m_key);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void KbTreeRow::dragEnterEvent(QDragEnterEvent *event)
{
    // 「全部资料」不是真实文件夹，不能当落点（app.js kbDropRow）
    if (m_key == QLatin1String("all") || docIdFromMime(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }
    setDropTarget(true);
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void KbTreeRow::dragMoveEvent(QDragMoveEvent *event)
{
    if (m_key == QLatin1String("all") || docIdFromMime(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }
    setDropTarget(true);
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void KbTreeRow::dragLeaveEvent(QDragLeaveEvent *event)
{
    setDropTarget(false);
    QWidget::dragLeaveEvent(event);
}

void KbTreeRow::dropEvent(QDropEvent *event)
{
    const QString docId = docIdFromMime(event->mimeData());
    setDropTarget(false);
    if (docId.isEmpty()) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::MoveAction);
    event->accept();
    emit documentDropped(m_key, docId);
}

// ------------------------------------------------------------ KbDocRow

KbDocRow::KbDocRow(const QVariantMap &doc, QWidget *parent) : QWidget(parent)
{
    m_docId = textOf(doc, "doc_id");
    // app.js：const failed = doc.status !== "ok"
    m_failed = textOf(doc, "status") != QLatin1String("ok");

    setClass(this, QStringLiteral("kbDoc"));
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("failed", m_failed);
    setProperty("dragging", false);
    setCursor(m_failed ? Qt::ArrowCursor : Qt::OpenHandCursor);
    setToolTip(m_failed ? QString() : QStringLiteral("拖到左侧文件夹即可归档"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(scaledPx(10), scaledPx(9), scaledPx(10), scaledPx(9));
    layout->setSpacing(scaledPx(10));

    m_grip = new QLabel(QStringLiteral("≡"), this);
    setClass(m_grip, QStringLiteral("kbDocGrip"));
    m_grip->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_grip, 0, Qt::AlignVCenter);

    auto *main = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(scaledPx(3));

    m_name = new ElidedLabel(main);
    setClass(m_name, QStringLiteral("kbDocName"));
    m_name->setFullText(textOf(doc, "name"));
    const QString path = textOf(doc, "path");
    if (!path.isEmpty())
        m_name->setToolTip(path); // 名称的 title 给完整路径
    mainLayout->addWidget(m_name, 0);

    QString meta;
    if (m_failed) {
        const QString error = textOf(doc, "error");
        meta = QStringLiteral("导入失败：%1")
                   .arg(error.isEmpty() ? QStringLiteral("未知原因") : error);
    } else {
        meta = QStringLiteral("%1 页 · %2 块 · %3 条参数 · %4 · %5")
                   .arg(intOf(doc, "pages"))
                   .arg(intOf(doc, "chunks"))
                   .arg(intOf(doc, "params"))
                   .arg(formatSize(toBytes(doc.value(QStringLiteral("size")))))
                   .arg(textOf(doc, "indexed_at"));
    }
    m_meta = new ElidedLabel(main);
    setClass(m_meta, QStringLiteral("kbDocMeta"));
    m_meta->setFullText(meta);
    mainLayout->addWidget(m_meta, 0);
    layout->addWidget(main, 1);

    auto *ops = new QWidget(this);
    auto *opsLayout = new QHBoxLayout(ops);
    opsLayout->setContentsMargins(0, 0, 0, 0);
    opsLayout->setSpacing(scaledPx(6));

    m_reindex = makeButton(QStringLiteral("重建"), QString(), ops);
    setClass(m_reindex, QStringLiteral("kbOp"));
    m_delete = makeButton(QStringLiteral("删除"), QString(), ops);
    setClass(m_delete, QStringLiteral("kbOp"));
    m_delete->setProperty("danger", true);
    connect(m_reindex, &QPushButton::clicked, this, [this] { emit reindexRequested(m_docId); });
    connect(m_delete, &QPushButton::clicked, this, [this] { emit deleteRequested(m_docId); });
    opsLayout->addWidget(m_reindex, 0);
    opsLayout->addWidget(m_delete, 0);
    layout->addWidget(ops, 0, Qt::AlignVCenter);
}

void KbDocRow::setDragging(bool dragging)
{
    setProperty("dragging", dragging);
    restyle(this);
}

void KbDocRow::setActionsEnabled(bool enabled)
{
    if (m_reindex)
        m_reindex->setEnabled(enabled);
    if (m_delete)
        m_delete->setEnabled(enabled);
}

bool KbDocRow::pressOnButton(const QPoint &pos) const
{
    QWidget *child = childAt(pos);
    while (child && child != this) {
        if (qobject_cast<QAbstractButton *>(child))
            return true;
        child = child->parentWidget();
    }
    return false;
}

void KbDocRow::mousePressEvent(QMouseEvent *event)
{
    m_pressed = false;
    if (event->button() == Qt::LeftButton && !m_failed && !pressOnButton(event->pos())) {
        m_pressed = true;
        m_pressPos = event->pos();
    }
    QWidget::mousePressEvent(event);
}

void KbDocRow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_pressed && !m_failed
        && (event->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        m_pressed = false;
        startDrag();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void KbDocRow::mouseReleaseEvent(QMouseEvent *event)
{
    m_pressed = false;
    QWidget::mouseReleaseEvent(event);
}

void KbDocRow::startDrag()
{
    if (m_docId.isEmpty())
        return;
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(kDocMime), m_docId.toUtf8());
    // 与 webui 的 text/plain 一致：外部（浏览器 / 其它控件）也能读到 doc_id
    mime->setText(m_docId);

    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    const QPixmap shot = grab();
    if (!shot.isNull()) {
        drag->setPixmap(shot);
        drag->setHotSpot(QPoint(shot.width() / 2, shot.height() / 2));
    }
    setDragging(true);
    drag->exec(Qt::MoveAction);
    setDragging(false);
    // 拖动结束（不管有没有落地）：让面板清掉落点高亮
    emit dragFinished();
}

// ------------------------------------------------------------ KbPanel

KbPanel::KbPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("kbPanel"));
    setClass(this, QStringLiteral("kbPanelHost"));

    m_expanded.insert(QStringLiteral("root"));

    buildUi();
    applyStyle();

    rebuildLibraryCombo();
    rebuildCards();
    rebuildTree();
    rebuildDocs();
    rebuildSearchResult();
    renderStatus();
    syncEnabled();
}

void KbPanel::buildUi()
{
    // .kb-panel 自身纵向滚动（外层不再套滚动容器）
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("kbPanelScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_scroll->viewport()->setObjectName(QStringLiteral("kbPanelViewport"));
    outer->addWidget(m_scroll);

    m_content = new QWidget(m_scroll);
    m_content->setObjectName(QStringLiteral("kbPanelContent"));
    m_content->setAttribute(Qt::WA_StyledBackground, false);
    m_scroll->setWidget(m_content);

    m_root = new QVBoxLayout(m_content);
    m_root->setContentsMargins(scaledPx(18), scaledPx(18), scaledPx(18), scaledPx(18));
    m_root->setSpacing(scaledPx(14));

    m_statusLabel = panelLabel(QStringLiteral("kbStatus"), QStringLiteral("正在读取知识库…"), m_content);
    m_statusLabel->setObjectName(QStringLiteral("kbStatus"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setProperty("tone", QString());
    m_root->addWidget(m_statusLabel, 0);

    m_root->addWidget(buildLibBar(), 0);
    m_root->addWidget(buildActions(), 0);
    m_root->addWidget(buildCards(), 0);
    m_root->addWidget(buildBody(), 0);
    m_root->addWidget(buildSearchSection(), 0);
    m_root->addStretch(1);
}

QWidget *KbPanel::buildLibBar()
{
    auto *bar = new QWidget(m_content);
    setClass(bar, QStringLiteral("kbLibBar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(scaledPx(10), scaledPx(9), scaledPx(10), scaledPx(9));
    layout->setSpacing(scaledPx(8));

    layout->addWidget(panelLabel(QStringLiteral("kbLibBarLabel"), QStringLiteral("知识库"), bar), 0);

    m_libCombo = new QComboBox(bar);
    m_libCombo->setObjectName(QStringLiteral("kbLibSelect"));
    setClass(m_libCombo, QStringLiteral("kbLibSelect"));
    m_libCombo->setCursor(Qt::PointingHandCursor);
    m_libCombo->setFocusPolicy(Qt::TabFocus);
    m_libCombo->setMinimumWidth(scaledPx(160));
    m_libCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_libCombo->setMaxVisibleItems(12);
    connect(m_libCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0)
            return;
        const QString id = m_libCombo->itemData(index).toString();
        if (id == m_library)
            return;
        pickLibrary(id);
    });
    layout->addWidget(m_libCombo, 1);

    m_libNew = makeButton(QStringLiteral("＋ 新建"), QStringLiteral("secondary"), bar);
    m_libRename = makeButton(QStringLiteral("重命名"), QStringLiteral("secondary"), bar);
    m_libDelete = makeButton(QStringLiteral("删除"), QStringLiteral("danger"), bar);
    m_libRefresh = makeButton(QStringLiteral("刷新"), QStringLiteral("secondary"), bar);
    layout->addWidget(m_libNew, 0);
    layout->addWidget(m_libRename, 0);
    layout->addWidget(m_libDelete, 0);
    layout->addWidget(m_libRefresh, 0);

    connect(m_libNew, &QPushButton::clicked, this, &KbPanel::createLibrary);
    connect(m_libRename, &QPushButton::clicked, this, &KbPanel::renameLibrary);
    connect(m_libDelete, &QPushButton::clicked, this, &KbPanel::deleteLibrary);
    connect(m_libRefresh, &QPushButton::clicked, this, [this] { emit refreshRequested(m_library); });
    return bar;
}

QWidget *KbPanel::buildActions()
{
    auto *actions = new QWidget(m_content);
    setClass(actions, QStringLiteral("kbActions"));
    auto *layout = new QHBoxLayout(actions);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(scaledPx(8));

    m_pickButton = makeButton(QStringLiteral("＋ 添加文件"), QStringLiteral("secondary"), actions);
    m_pickButton->setToolTip(QStringLiteral("pdf / txt / md / docx / xlsx / doc / xls"));
    layout->addWidget(m_pickButton, 0);

    m_dirInput = new QLineEdit(actions);
    m_dirInput->setObjectName(QStringLiteral("kbDirInput"));
    setClass(m_dirInput, QStringLiteral("kbInput"));
    m_dirInput->setPlaceholderText(QStringLiteral("导入本机目录，如 D:\\资料\\GridStar手册"));
    m_dirInput->setClearButtonEnabled(false);
    m_dirInput->setMinimumWidth(scaledPx(200));
    layout->addWidget(m_dirInput, 1);

    m_importDirButton = makeButton(QStringLiteral("导入目录"), QStringLiteral("secondary"), actions);
    layout->addWidget(m_importDirButton, 0);

    connect(m_pickButton, &QPushButton::clicked, this, &KbPanel::pickFiles);
    connect(m_importDirButton, &QPushButton::clicked, this, &KbPanel::importDirectory);
    connect(m_dirInput, &QLineEdit::returnPressed, this, &KbPanel::importDirectory);
    return actions;
}

QWidget *KbPanel::buildCards()
{
    m_cardsBox = new QWidget(m_content);
    setClass(m_cardsBox, QStringLiteral("kbCards"));
    m_cardsLayout = new QHBoxLayout(m_cardsBox);
    m_cardsLayout->setContentsMargins(0, 0, 0, 0);
    m_cardsLayout->setSpacing(scaledPx(8));
    return m_cardsBox;
}

QWidget *KbPanel::buildBody()
{
    auto *body = new QWidget(m_content);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(scaledPx(12));

    // ---- 左：归档树 ----
    m_treeColumn = new QFrame(body);
    setClass(m_treeColumn, QStringLiteral("kbTree"));
    m_treeColumn->setAttribute(Qt::WA_StyledBackground, true);
    m_treeColumn->setFrameShape(QFrame::NoFrame);
    m_treeColumn->setFixedWidth(scaledPx(200));
    auto *treeLayout = new QVBoxLayout(m_treeColumn);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    treeLayout->setSpacing(0);

    auto *head = new QWidget(m_treeColumn);
    setClass(head, QStringLiteral("kbTreeHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(scaledPx(9), scaledPx(6), scaledPx(9), scaledPx(6));
    headLayout->setSpacing(scaledPx(8));
    headLayout->addWidget(panelLabel(QStringLiteral("eyebrow"), QStringLiteral("归档结构"), head), 1);
    m_folderNew = makeButton(QStringLiteral("＋"), QString(), head);
    setClass(m_folderNew, QStringLiteral("kbTreeBtn"));
    m_folderNew->setToolTip(QStringLiteral("在当前选中项下新建文件夹"));
    headLayout->addWidget(m_folderNew, 0);
    treeLayout->addWidget(head, 0);

    m_treeScroll = new QScrollArea(m_treeColumn);
    m_treeScroll->setObjectName(QStringLiteral("kbTreeScroll"));
    m_treeScroll->setFrameShape(QFrame::NoFrame);
    m_treeScroll->setWidgetResizable(true);
    m_treeScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_treeScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_treeScroll->viewport()->setAutoFillBackground(false);
    m_treeScroll->viewport()->setObjectName(QStringLiteral("kbTreeViewport"));
    m_treeInner = new QWidget(m_treeScroll);
    setClass(m_treeInner, QStringLiteral("kbTreeInner"));
    m_treeLayout = new QVBoxLayout(m_treeInner);
    m_treeLayout->setContentsMargins(scaledPx(4), scaledPx(4), scaledPx(4), scaledPx(4));
    m_treeLayout->setSpacing(scaledPx(2));
    m_treeScroll->setWidget(m_treeInner);
    treeLayout->addWidget(m_treeScroll, 1);

    auto *foot = new QWidget(m_treeColumn);
    setClass(foot, QStringLiteral("kbTreeFoot"));
    foot->setAttribute(Qt::WA_StyledBackground, true);
    auto *footLayout = new QHBoxLayout(foot);
    footLayout->setContentsMargins(scaledPx(6), scaledPx(6), scaledPx(6), scaledPx(6));
    footLayout->setSpacing(scaledPx(5));
    m_folderRename = makeButton(QStringLiteral("重命名"), QString(), foot);
    setClass(m_folderRename, QStringLiteral("kbTreeBtn"));
    m_folderDelete = makeButton(QStringLiteral("删除"), QString(), foot);
    setClass(m_folderDelete, QStringLiteral("kbTreeBtn"));
    m_folderDelete->setProperty("danger", true);
    footLayout->addWidget(m_folderRename, 1);
    footLayout->addWidget(m_folderDelete, 1);
    treeLayout->addWidget(foot, 0);

    connect(m_folderNew, &QPushButton::clicked, this, &KbPanel::createFolder);
    connect(m_folderRename, &QPushButton::clicked, this, &KbPanel::renameFolder);
    connect(m_folderDelete, &QPushButton::clicked, this, &KbPanel::deleteFolder);

    bodyLayout->addWidget(m_treeColumn, 0, Qt::AlignTop);
    bodyLayout->addWidget(buildDocsSection(body), 1);
    return body;
}

QWidget *KbPanel::buildDocsSection(QWidget *parent)
{
    auto *section = new QFrame(parent);
    setClass(section, QStringLiteral("kbSection"));
    section->setAttribute(Qt::WA_StyledBackground, true);
    section->setFrameShape(QFrame::NoFrame);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(scaledPx(12), scaledPx(12), scaledPx(12), scaledPx(12));
    layout->setSpacing(scaledPx(8));

    auto *head = new QWidget(section);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(scaledPx(10));
    headLayout->addWidget(panelLabel(QStringLiteral("eyebrow"), QStringLiteral("资料清单"), head), 0,
                          Qt::AlignBottom);
    m_docTitle = panelLabel(QStringLiteral("kbSectionTitle"), QStringLiteral("全部资料"), head);
    headLayout->addWidget(m_docTitle, 0, Qt::AlignBottom);
    m_docCount = panelLabel(QStringLiteral("kbCount"), QStringLiteral("0"), head);
    headLayout->addWidget(m_docCount, 0, Qt::AlignBottom);
    headLayout->addStretch(1);
    layout->addWidget(head, 0);

    QLabel *hint = panelLabel(QStringLiteral("kbHint"),
                              QStringLiteral("把资料行拖到左侧文件夹即可归档，拖到「库根」就是移出来。"
                                             "导入目录会按原目录层级自动建文件夹。"),
                              section);
    hint->setWordWrap(true);
    layout->addWidget(hint, 0);

    m_docsScroll = new QScrollArea(section);
    m_docsScroll->setObjectName(QStringLiteral("kbDocsScroll"));
    m_docsScroll->setFrameShape(QFrame::NoFrame);
    m_docsScroll->setWidgetResizable(true);
    m_docsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_docsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_docsScroll->viewport()->setAutoFillBackground(false);
    m_docsScroll->viewport()->setObjectName(QStringLiteral("kbDocsViewport"));
    m_docsInner = new QWidget(m_docsScroll);
    setClass(m_docsInner, QStringLiteral("kbDocsInner"));
    m_docsLayout = new QVBoxLayout(m_docsInner);
    m_docsLayout->setContentsMargins(0, 0, 0, 0);
    m_docsLayout->setSpacing(scaledPx(6));
    m_docsScroll->setWidget(m_docsInner);
    layout->addWidget(m_docsScroll, 1);
    return section;
}

QWidget *KbPanel::buildSearchSection()
{
    auto *section = new QFrame(m_content);
    setClass(section, QStringLiteral("kbSection"));
    section->setAttribute(Qt::WA_StyledBackground, true);
    section->setFrameShape(QFrame::NoFrame);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(scaledPx(12), scaledPx(12), scaledPx(12), scaledPx(12));
    layout->setSpacing(scaledPx(8));

    auto *head = new QWidget(section);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(scaledPx(10));
    headLayout->addWidget(panelLabel(QStringLiteral("eyebrow"), QStringLiteral("检索验证"), head), 0,
                          Qt::AlignBottom);
    headLayout->addWidget(panelLabel(QStringLiteral("kbSectionTitle"), QStringLiteral("检索试跑"), head),
                          0, Qt::AlignBottom);
    headLayout->addStretch(1);
    layout->addWidget(head, 0);

    QLabel *hint = panelLabel(QStringLiteral("kbHint"),
                              QStringLiteral("不经过模型，直接看检索会返回什么。检索范围跟着上面的知识库选择走。"),
                              section);
    hint->setWordWrap(true);
    layout->addWidget(hint, 0);

    auto *row = new QWidget(section);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(scaledPx(8));
    m_query = new QLineEdit(row);
    m_query->setObjectName(QStringLiteral("kbQuery"));
    setClass(m_query, QStringLiteral("kbInput"));
    m_query->setPlaceholderText(QStringLiteral("例如：附面层首层高度怎么确定"));
    m_query->setClearButtonEnabled(false);
    rowLayout->addWidget(m_query, 1);
    m_searchButton = makeButton(QStringLiteral("检索"), QStringLiteral("secondary"), row);
    rowLayout->addWidget(m_searchButton, 0);
    layout->addWidget(row, 0);

    auto *results = new QWidget(section);
    m_searchLayout = new QVBoxLayout(results);
    m_searchLayout->setContentsMargins(0, 0, 0, 0);
    m_searchLayout->setSpacing(scaledPx(8));
    m_searchLayout->addStretch(1);
    layout->addWidget(results, 0);

    connect(m_searchButton, &QPushButton::clicked, this, &KbPanel::runSearch);
    // 回车等价点击
    connect(m_query, &QLineEdit::returnPressed, this, &KbPanel::runSearch);
    return section;
}

QString KbPanel::styleKey() const
{
    return themeId() + QLatin1Char('|') + QString::number(zoomFactor());
}

void KbPanel::applyStyle()
{
    // 样式表统一由 theme.cpp 的 appStyleSheet() 维护，这里只剩「按当前缩放重算内联尺寸 + 重排」：
    // QSS 里写的是基准像素，行高 / 内层滚动区高度却依赖 scaledPx()，所以缩放变了得重算。
    m_styleKey = styleKey();

    if (m_treeColumn)
        m_treeColumn->setFixedWidth(scaledPx(200));
    if (m_root)
        m_root->setContentsMargins(scaledPx(18), scaledPx(18), scaledPx(18), scaledPx(18));

    // 行高 / 内层滚动区高度都依赖缩放系数：缩放变了就按新尺寸重排
    if (m_treeLayout)
        rebuildTree();
    if (m_docsLayout)
        rebuildDocs();

    // 切皮肤时上面的重建发生在 StyleChange 处理过程中，子控件还没 polish 完，
    // fitScrollHeight() 量到的 sizeHint 偏小，会把归档树与资料清单压扁只剩一行。
    // 事件循环跑完一轮后子控件尺寸才定稿，这里补一次重算。
    QTimer::singleShot(0, this, [this] {
        if (m_treeScroll)
            fitScrollHeight(m_treeScroll, scaledPx(360));
        if (m_docsScroll)
            fitScrollHeight(m_docsScroll, scaledPx(300));
    });
}

void KbPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (styleKey() != m_styleKey)
        applyStyle();
}

bool KbPanel::event(QEvent *event)
{
    // applyStyle() 会先把 m_styleKey 定稿，重入的 StyleChange 就不会再进来
    if (event->type() == QEvent::StyleChange && styleKey() != m_styleKey)
        applyStyle();
    return QWidget::event(event);
}

// ------------------------------------------------------------ 渲染

void KbPanel::rebuildLibraryCombo()
{
    if (!m_libCombo)
        return;
    const QSignalBlocker blocker(m_libCombo);
    m_libCombo->clear();
    if (m_libraries.isEmpty()) {
        m_libCombo->addItem(QStringLiteral("还没有知识库"), QString());
        return;
    }
    int current = -1;
    for (const QVariant &value : m_libraries) {
        const QVariantMap lib = value.toMap();
        const QString id = lib.value(QStringLiteral("id")).toString();
        m_libCombo->addItem(QStringLiteral("%1（%2）")
                                .arg(lib.value(QStringLiteral("name")).toString())
                                .arg(lib.value(QStringLiteral("documents")).toInt()),
                            id);
        if (id == m_library)
            current = m_libCombo->count() - 1;
    }
    if (current < 0 && m_libCombo->count() > 0)
        current = 0;
    m_libCombo->setCurrentIndex(current);
}

void KbPanel::rebuildCards()
{
    clearLayout(m_cardsLayout);
    if (!m_cardsBox)
        return;
    if (m_status.isEmpty() || !statusReady())
        return;

    addCard(QStringLiteral("文档"), QString::number(intOf(m_status, "documents")),
            QStringLiteral("份"), false);
    addCard(QStringLiteral("分块"), QString::number(intOf(m_status, "chunks")),
            QStringLiteral("个检索单元"), false);
    addCard(QStringLiteral("参数"), QString::number(intOf(m_status, "params")),
            QStringLiteral("条结构化记录"), false);

    // 索引体积是全库的（所有库共一个 db 文件），按库看时不要重复同一个数字
    QString note;
    if (m_status.contains(QStringLiteral("fts")))
        note = statusFts() ? QStringLiteral("FTS5 词法索引") : QStringLiteral("降级模式");
    if (!m_library.isEmpty())
        note = QStringLiteral("各库共用一个文件");
    addCard(QStringLiteral("索引体积"),
            formatSize(toBytes(m_status.value(QStringLiteral("db_bytes")))), note, false);

    const int failed = intOf(m_status, "failed");
    if (failed > 0)
        addCard(QStringLiteral("失败"), QString::number(failed), QStringLiteral("份待处理"), true);
}

QFrame *KbPanel::addCard(const QString &label, const QString &value, const QString &note, bool warn)
{
    auto *card = new QFrame(m_cardsBox);
    setClass(card, QStringLiteral("kbCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setFrameShape(QFrame::NoFrame);
    card->setProperty("warn", warn);
    card->setMinimumWidth(scaledPx(112));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(scaledPx(11), scaledPx(9), scaledPx(11), scaledPx(9));
    layout->setSpacing(scaledPx(2));

    layout->addWidget(panelLabel(QStringLiteral("kbCardLabel"), label, card), 0);
    layout->addWidget(panelLabel(QStringLiteral("kbCardValue"), value, card), 0);
    QLabel *noteLabel = panelLabel(QStringLiteral("kbCardNote"), note, card);
    noteLabel->setWordWrap(true);
    layout->addWidget(noteLabel, 0);

    if (m_cardsLayout)
        m_cardsLayout->addWidget(card, 1);
    return card;
}

KbTreeRow *KbPanel::addTreeRow(const QString &key, const QString &label, int count, int depth,
                               bool hasChildren)
{
    auto *row = new KbTreeRow(key, label, count, depth, hasChildren, m_expanded.contains(key),
                              m_treeInner);
    row->setActive(key == m_currentFolder);
    connect(row, &KbTreeRow::activated, this, &KbPanel::selectFolder);
    connect(row, &KbTreeRow::twistyToggled, this, &KbPanel::toggleFolder);
    connect(row, &KbTreeRow::documentDropped, this,
            [this](const QString &folderKey, const QString &docId) {
                clearDropHints();
                // 落点是「库根」时 folderId 传空串（PATCH 的 folder_id=""）
                const QString folderId = folderKey == QLatin1String("root") ? QString() : folderKey;
                // 已经在目标位置就不发请求（app.js：这份资料已经在这个位置）
                for (const QVariant &value : m_docs) {
                    const QVariantMap doc = value.toMap();
                    if (doc.value(QStringLiteral("doc_id")).toString() != docId)
                        continue;
                    if (doc.value(QStringLiteral("folder_id")).toString() == folderId)
                        return;
                    break;
                }
                emit documentMoveRequested(docId, folderId);
            });
    m_treeLayout->addWidget(row, 0);
    m_treeRows.append(row);
    return row;
}

void KbPanel::rebuildTree()
{
    if (!m_treeLayout || !m_treeInner)
        return;
    clearLayout(m_treeLayout);
    m_treeRows.clear();

    addTreeRow(QStringLiteral("all"), QStringLiteral("全部资料"), m_docsTotal, 0, false);
    const QVariantList children = listedList(m_tree, "children");
    addTreeRow(QStringLiteral("root"), QStringLiteral("库根"), intOf(m_tree, "documents"), 0,
               !children.isEmpty());

    std::function<void(const QVariantList &)> walk = [&](const QVariantList &nodes) {
        for (const QVariant &value : nodes) {
            const QVariantMap node = value.toMap();
            const QString id = node.value(QStringLiteral("id")).toString();
            if (id.isEmpty())
                continue;
            const QVariantList kids = listedList(node, "children");
            // webui 的 walk() 在未展开时整条不渲染（连自己都不见了）；这里保留行本身，
            // 只收起子节点，否则收起的文件夹会连重新展开的入口都丢掉。
            addTreeRow(id, node.value(QStringLiteral("name")).toString(),
                       intOf(node, "total"), intOf(node, "depth") + 1, !kids.isEmpty());
            if (m_expanded.contains(id))
                walk(kids);
        }
    };
    if (m_expanded.contains(QStringLiteral("root")))
        walk(children);

    if (m_folders.isEmpty()) {
        QLabel *empty = panelLabel(QStringLiteral("kbTreeEmpty"),
                                   QStringLiteral("还没有文件夹。导入目录会自动建，或点右上角 ＋ 手动建。"),
                                   m_treeInner);
        empty->setWordWrap(true);
        m_treeLayout->addWidget(empty, 0);
    }
    m_treeLayout->addStretch(1);
    fitScrollHeight(m_treeScroll, scaledPx(360));
    // 同 rebuildDocs：异步数据到达时 inner 还没布局过，sizeHint 偏小会让树被压扁
    QTimer::singleShot(0, this, [this] {
        if (m_treeScroll)
            fitScrollHeight(m_treeScroll, scaledPx(360));
    });
}

void KbPanel::rebuildDocs()
{
    if (!m_docsLayout || !m_docsInner)
        return;
    clearLayout(m_docsLayout);
    m_docRows.clear();

    const QString pathText = folderPathText(m_currentFolder);
    const QList<QVariantMap> docs = visibleDocuments();
    if (m_docTitle)
        m_docTitle->setText(pathText);
    if (m_docCount)
        m_docCount->setText(QString::number(docs.size()));

    if (docs.isEmpty()) {
        QLabel *empty = panelLabel(QStringLiteral("kbEmpty"),
                                   QStringLiteral("「%1」里还没有资料。点「添加文件」，或导入一个目录。")
                                       .arg(pathText),
                                   m_docsInner);
        empty->setWordWrap(true);
        m_docsLayout->addWidget(empty, 0);
    } else {
        for (const QVariantMap &doc : docs) {
            auto *row = new KbDocRow(doc, m_docsInner);
            connect(row, &KbDocRow::deleteRequested, this, &KbPanel::requestDocumentDelete);
            connect(row, &KbDocRow::reindexRequested, this, [this](const QString &docId) {
                emit documentReindexRequested(docId);
            });
            connect(row, &KbDocRow::dragFinished, this, &KbPanel::clearDropHints);
            m_docsLayout->addWidget(row, 0);
            m_docRows.append(row);
        }
    }
    m_docsLayout->addStretch(1);
    fitScrollHeight(m_docsScroll, scaledPx(300));
    // 数据是异步来的：rebuild 发生在网络回调里，此刻 inner 还没参与过布局，
    // sizeHint 量出来接近 0，滚动区就被设成最小高度，行被压成一条线。
    // 事件循环跑完一轮后子控件尺寸才定稿，补一次重算。
    QTimer::singleShot(0, this, [this] {
        if (m_docsScroll)
            fitScrollHeight(m_docsScroll, scaledPx(300));
    });
    syncEnabled();
}

void KbPanel::rebuildSearchResult()
{
    if (!m_searchLayout)
        return;
    clearLayout(m_searchLayout);

    QWidget *host = m_searchLayout->parentWidget();
    const auto addLine = [&](const QString &text) {
        QLabel *label = panelLabel(QStringLiteral("kbEmpty"), text, host);
        label->setWordWrap(true);
        m_searchLayout->addWidget(label, 0);
    };

    if (m_searchPending) {
        addLine(QStringLiteral("检索中…"));
        m_searchLayout->addStretch(1);
        return;
    }
    if (!m_searchNotice.isEmpty()) {
        addLine(m_searchNotice);
        m_searchLayout->addStretch(1);
        return;
    }
    if (m_search.isEmpty()) {
        m_searchLayout->addStretch(1);
        return;
    }

    const QString error = textOf(m_search, "error");
    if (!error.isEmpty()) {
        addLine(QStringLiteral("检索失败：%1").arg(error));
        m_searchLayout->addStretch(1);
        return;
    }

    const QVariantList params = listedList(m_search, "params");
    const QVariantList hits = listedList(m_search, "hits");
    if (params.isEmpty() && hits.isEmpty()) {
        addLine(QStringLiteral("「%1」没有命中。可能这条内容没收录，或换个手册里的说法再试。")
                    .arg(libraryName(m_library)));
        m_searchLayout->addStretch(1);
        return;
    }

    if (!params.isEmpty()) {
        QLabel *head = panelLabel(QStringLiteral("kbResultHead"), QStringLiteral("参数命中"), host);
        m_searchLayout->addWidget(head, 0);
        for (const QVariant &value : params) {
            const QVariantMap row = value.toMap();
            auto *card = new QFrame(host);
            setClass(card, QStringLiteral("kbResultCard"));
            card->setAttribute(Qt::WA_StyledBackground, true);
            card->setFrameShape(QFrame::NoFrame);
            auto *layout = new QVBoxLayout(card);
            layout->setContentsMargins(scaledPx(10), scaledPx(9), scaledPx(10), scaledPx(9));
            layout->setSpacing(scaledPx(6));

            auto *titleRow = new QWidget(card);
            auto *titleLayout = new QHBoxLayout(titleRow);
            titleLayout->setContentsMargins(0, 0, 0, 0);
            titleLayout->setSpacing(scaledPx(8));
            titleLayout->addWidget(panelLabel(QStringLiteral("kbResultTitle"),
                                              textOf(row, "name"), titleRow), 0);
            QString domain = textOf(row, "domain");
            if (domain.isEmpty())
                domain = QStringLiteral("general");
            titleLayout->addWidget(panelLabel(QStringLiteral("kbResultTag"), domain, titleRow), 0);

            const QVariant defaultValue = row.value(QStringLiteral("default"));
            if (jsTruthy(defaultValue))
                titleLayout->addWidget(panelLabel(QStringLiteral("kbResultEm"),
                                                  QStringLiteral("默认 %1").arg(defaultValue.toString()),
                                                  titleRow), 0);
            const QVariant minValue = row.value(QStringLiteral("min"));
            const QVariant maxValue = row.value(QStringLiteral("max"));
            if (hasNumber(minValue) || hasNumber(maxValue)) {
                titleLayout->addWidget(panelLabel(
                                           QStringLiteral("kbResultEm"),
                                           QStringLiteral("范围 %1~%2")
                                               .arg(hasNumber(minValue) ? minValue.toString() : QString())
                                               .arg(hasNumber(maxValue) ? maxValue.toString() : QString()),
                                           titleRow), 0);
            }
            titleLayout->addStretch(1);
            layout->addWidget(titleRow, 0);

            const QString description = textOf(row, "description");
            if (!description.isEmpty()) {
                QLabel *body = panelLabel(QStringLiteral("kbResultBody"), description, card);
                body->setWordWrap(true);
                layout->addWidget(body, 0);
            }

            const QVariant page = row.value(QStringLiteral("page"));
            const QString pageText = hasNumber(page) ? page.toString() : QStringLiteral("?");
            QLabel *src = panelLabel(QStringLiteral("kbResultSrc"),
                                     QStringLiteral("%1 · %2 · P%3")
                                         .arg(textOf(row, "doc"), textOf(row, "heading_path"), pageText),
                                     card);
            src->setWordWrap(true);
            layout->addWidget(src, 0);
            m_searchLayout->addWidget(card, 0);
        }
    }

    if (!hits.isEmpty()) {
        QLabel *head = panelLabel(QStringLiteral("kbResultHead"), QStringLiteral("文本命中"), host);
        m_searchLayout->addWidget(head, 0);
        int index = 0;
        for (const QVariant &value : hits) {
            index += 1;
            const QVariantMap hit = value.toMap();
            auto *card = new QFrame(host);
            setClass(card, QStringLiteral("kbResultCard"));
            card->setAttribute(Qt::WA_StyledBackground, true);
            card->setFrameShape(QFrame::NoFrame);
            auto *layout = new QVBoxLayout(card);
            layout->setContentsMargins(scaledPx(10), scaledPx(9), scaledPx(10), scaledPx(9));
            layout->setSpacing(scaledPx(6));

            auto *titleRow = new QWidget(card);
            auto *titleLayout = new QHBoxLayout(titleRow);
            titleLayout->setContentsMargins(0, 0, 0, 0);
            titleLayout->setSpacing(scaledPx(8));
            titleLayout->addWidget(panelLabel(QStringLiteral("kbResultTitle"),
                                              QStringLiteral("#%1").arg(index), titleRow), 0);
            titleLayout->addWidget(panelLabel(
                                       QStringLiteral("kbResultScore"),
                                       QString::number(hit.value(QStringLiteral("score")).toDouble(),
                                                       'f', 2),
                                       titleRow), 0);
            titleLayout->addWidget(panelLabel(QStringLiteral("kbResultTitle"),
                                              textOf(hit, "doc"), titleRow), 0);
            titleLayout->addStretch(1);
            layout->addWidget(titleRow, 0);

            const QString snippet = textOf(hit, "snippet");
            if (!snippet.isEmpty()) {
                QLabel *body = panelLabel(QStringLiteral("kbResultBody"), snippet, card);
                body->setWordWrap(true);
                layout->addWidget(body, 0);
            }

            QString heading = textOf(hit, "heading_path");
            heading.replace(QStringLiteral(" / "), QStringLiteral(" > "));
            QLabel *src = panelLabel(
                QStringLiteral("kbResultSrc"),
                QStringLiteral("%1 · P%2 · chunk %3")
                    .arg(heading,
                         hit.value(QStringLiteral("page")).toString(),
                         hit.value(QStringLiteral("chunk_id")).toString()),
                card);
            src->setWordWrap(true);
            layout->addWidget(src, 0);
            m_searchLayout->addWidget(card, 0);
        }
    }
    m_searchLayout->addStretch(1);
}

void KbPanel::renderStatus()
{
    if (!m_statusLabel)
        return;

    QString text;
    QString tone;
    if (!m_statusOverride.isEmpty()) {
        text = m_statusOverride;
    } else if (m_status.isEmpty()) {
        text = QStringLiteral("正在读取知识库…");
    } else if (!statusReady()) {
        const QString error = textOf(m_status, "error");
        text = QStringLiteral("知识库不可用：%1")
                   .arg(error.isEmpty() ? QStringLiteral("未知原因") : error);
        tone = QStringLiteral("error");
    } else if (m_status.contains(QStringLiteral("fts")) && !statusFts()) {
        text = QStringLiteral("索引可用，但当前 SQLite 不支持 FTS5，已降级为逐行匹配（会慢很多）");
        tone = QStringLiteral("warn");
    } else {
        const QString scope = m_library.isEmpty()
            ? QString()
            : QStringLiteral("「%1」").arg(libraryName(m_library));
        text = QStringLiteral("索引正常%1 · %2 份文档 / %3 块 / %4 条参数")
                   .arg(scope)
                   .arg(intOf(m_status, "documents"))
                   .arg(intOf(m_status, "chunks"))
                   .arg(intOf(m_status, "params"));
    }

    m_statusLabel->setText(text);
    if (m_statusLabel->property("tone").toString() != tone) {
        m_statusLabel->setProperty("tone", tone);
        restyle(m_statusLabel);
    }
}

void KbPanel::syncEnabled()
{
    const bool busy = m_busy;
    const bool hasLibrary = !m_library.isEmpty();
    const bool realFolder = isRealFolder(m_currentFolder);

    if (m_libCombo)
        m_libCombo->setEnabled(!busy && !m_libraries.isEmpty());
    if (m_libNew)
        m_libNew->setEnabled(!busy);
    if (m_libRename)
        m_libRename->setEnabled(!busy && hasLibrary);
    if (m_libDelete)
        m_libDelete->setEnabled(!busy && hasLibrary);
    if (m_libRefresh)
        m_libRefresh->setEnabled(!busy);
    if (m_pickButton)
        m_pickButton->setEnabled(!busy);
    if (m_dirInput)
        m_dirInput->setEnabled(!busy);
    if (m_importDirButton)
        m_importDirButton->setEnabled(!busy);
    if (m_folderNew)
        m_folderNew->setEnabled(!busy && (hasLibrary || !m_libraries.isEmpty()));
    if (m_folderRename)
        m_folderRename->setEnabled(!busy && realFolder);
    if (m_folderDelete)
        m_folderDelete->setEnabled(!busy && realFolder);
    if (m_searchButton)
        m_searchButton->setEnabled(!busy && !m_searchPending);
    for (KbDocRow *row : m_docRows)
        row->setActionsEnabled(!busy);
}

// ------------------------------------------------------------ 取值

bool KbPanel::statusReady() const
{
    const QVariant available = m_status.value(QStringLiteral("available"));
    if (!available.isValid() || available.isNull())
        return true; // JS：status.available !== false
    return available.toBool();
}

bool KbPanel::statusFts() const
{
    const QVariant fts = m_status.value(QStringLiteral("fts"));
    return fts.isValid() && fts.toBool();
}

QString KbPanel::libraryName(const QString &id) const
{
    if (id == QLatin1String("*"))
        return QStringLiteral("全部知识库");
    if (id.isEmpty())
        return QStringLiteral("当前知识库");
    for (const QVariant &value : m_libraries) {
        const QVariantMap lib = value.toMap();
        if (lib.value(QStringLiteral("id")).toString() == id)
            return lib.value(QStringLiteral("name")).toString();
    }
    return QStringLiteral("未知知识库");
}

bool KbPanel::isRealFolder(const QString &key) const
{
    return !key.isEmpty() && key != QLatin1String("all") && key != QLatin1String("root");
}

QString KbPanel::folderLabel(const QString &key) const
{
    if (key == QLatin1String("all"))
        return QStringLiteral("全部资料");
    if (key == QLatin1String("root") || key.isEmpty())
        return QStringLiteral("库根");
    for (const QVariant &value : m_folders) {
        const QVariantMap folder = value.toMap();
        if (folder.value(QStringLiteral("id")).toString() == key)
            return folder.value(QStringLiteral("name")).toString();
    }
    return QStringLiteral("未知文件夹");
}

QString KbPanel::folderPathText(const QString &key) const
{
    if (key == QLatin1String("all"))
        return QStringLiteral("全部资料");
    if (key == QLatin1String("root") || key.isEmpty())
        return QStringLiteral("库根");

    QHash<QString, QVariantMap> byId;
    for (const QVariant &value : m_folders) {
        const QVariantMap folder = value.toMap();
        byId.insert(folder.value(QStringLiteral("id")).toString(), folder);
    }
    QStringList names;
    QString cursor = key;
    int guard = 0;
    while (!cursor.isEmpty() && byId.contains(cursor) && guard < 40) {
        const QVariantMap folder = byId.value(cursor);
        names.prepend(folder.value(QStringLiteral("name")).toString());
        cursor = folder.value(QStringLiteral("parent_id")).toString();
        guard += 1;
    }
    return names.isEmpty() ? QStringLiteral("库根") : names.join(QStringLiteral(" / "));
}

QString KbPanel::targetFolderId() const
{
    // 「全部资料」「库根」都落到库根
    return isRealFolder(m_currentFolder) ? m_currentFolder : QString();
}

int KbPanel::folderTotal(const QString &folderId) const
{
    std::function<int(const QVariantList &)> walk = [&](const QVariantList &nodes) -> int {
        for (const QVariant &value : nodes) {
            const QVariantMap node = value.toMap();
            if (node.value(QStringLiteral("id")).toString() == folderId)
                return intOf(node, "total");
            const int found = walk(listedList(node, "children"));
            if (found >= 0)
                return found;
        }
        return -1;
    };
    const int total = walk(listedList(m_tree, "children"));
    return total < 0 ? 0 : total;
}

QList<QVariantMap> KbPanel::visibleDocuments() const
{
    QList<QVariantMap> out;
    // 宿主一次性推全量时才需要本地筛；整批文档都没带 folder_id 就原样展示，
    // 不能把整份清单误判成空。宿主按目录推（folder= 已由 folderChanged 请求）时，
    // 这批本来就只有该目录的内容，过滤是幂等的。
    bool anyFolderField = false;
    for (const QVariant &value : m_docs) {
        if (value.toMap().contains(QStringLiteral("folder_id"))) {
            anyFolderField = true;
            break;
        }
    }
    for (const QVariant &value : m_docs) {
        const QVariantMap doc = value.toMap();
        if (m_currentFolder == QLatin1String("all") || !anyFolderField) {
            out.append(doc);
            continue;
        }
        const QString folderId = doc.value(QStringLiteral("folder_id")).toString();
        if (m_currentFolder == QLatin1String("root")) {
            if (folderId.isEmpty())
                out.append(doc);
        } else if (folderId == m_currentFolder) {
            out.append(doc);
        }
    }
    return out;
}

QString KbPanel::documentName(const QString &docId) const
{
    for (const QVariant &value : m_docs) {
        const QVariantMap doc = value.toMap();
        if (doc.value(QStringLiteral("doc_id")).toString() == docId) {
            const QString name = doc.value(QStringLiteral("name")).toString();
            if (!name.isEmpty())
                return name;
        }
    }
    return QStringLiteral("这份文档");
}

// ------------------------------------------------------------ 交互

void KbPanel::selectFolder(const QString &key)
{
    const QString next = key.isEmpty() ? QStringLiteral("all") : key;
    if (next == m_currentFolder)
        return;
    m_currentFolder = next;
    for (KbTreeRow *row : m_treeRows)
        row->setActive(row->folderKey() == m_currentFolder);
    rebuildDocs();
    syncEnabled();
    // 宿主可以按这个目录重拉 /kb/documents?folder=；本地按 folder_id 的筛选作为兜底
    emit folderChanged(m_library, m_currentFolder);
}

void KbPanel::toggleFolder(const QString &key)
{
    if (m_expanded.contains(key))
        m_expanded.remove(key);
    else
        m_expanded.insert(key);
    rebuildTree();
}

void KbPanel::pickLibrary(const QString &id)
{
    if (id == m_library)
        return;
    m_library = id;
    resetFolderState();
    clearLibraryData();
    // 换库后旧检索结果与旧状态文案都作废
    m_search.clear();
    m_searchNotice.clear();
    m_searchPending = false;
    m_statusOverride.clear();

    const QSignalBlocker blocker(m_libCombo);
    if (m_libCombo) {
        for (int i = 0; i < m_libCombo->count(); ++i) {
            if (m_libCombo->itemData(i).toString() == m_library) {
                m_libCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    rebuildTree();
    rebuildDocs();
    rebuildSearchResult();
    rebuildCards();
    renderStatus();
    syncEnabled();
    emit libraryChanged(m_library);
    // 切库后归档位置已收回「全部资料」：统一再报一次目录变更，
    // 宿主不必区分「切库」与「切目录」两条拉取路径
    emit folderChanged(m_library, m_currentFolder);
}

void KbPanel::resetFolderState()
{
    m_currentFolder = QStringLiteral("all");
    m_expanded.clear();
    m_expanded.insert(QStringLiteral("root"));
    m_knownFolders.clear();
}

void KbPanel::clearLibraryData()
{
    m_folders.clear();
    m_tree.clear();
    m_docs.clear();
    m_docsTotal = 0;
    m_status.clear();
}

void KbPanel::createLibrary()
{
    const QString name = ConfirmDialog::prompt(this, QStringLiteral("新建知识库"), QString(),
                                               QStringLiteral("新建"))
                             .trimmed();
    if (name.isEmpty())
        return;
    // 宿主建完会回推库列表：新出现的那个 id 就是刚建的库，选中项跟着过去
    m_pendingCreate = true;
    emit libraryCreateRequested(name);
}

void KbPanel::renameLibrary()
{
    if (m_library.isEmpty())
        return;
    const QString name = ConfirmDialog::prompt(this, QStringLiteral("重命名知识库"),
                                               libraryName(m_library), QStringLiteral("保存"))
                             .trimmed();
    if (name.isEmpty())
        return;
    emit libraryRenameRequested(m_library, name);
}

void KbPanel::deleteLibrary()
{
    if (m_library.isEmpty())
        return;
    int documents = 0;
    for (const QVariant &value : m_libraries) {
        const QVariantMap lib = value.toMap();
        if (lib.value(QStringLiteral("id")).toString() == m_library) {
            documents = lib.value(QStringLiteral("documents")).toInt();
            break;
        }
    }
    const QString message =
        QStringLiteral("确定删除知识库「%1」？库里的 %2 份资料的索引会一起删掉，本机原文件不动。")
            .arg(libraryName(m_library))
            .arg(documents);
    if (!ConfirmDialog::ask(this, QStringLiteral("删除知识库"), message, QStringLiteral("删除"), true))
        return;
    emit libraryDeleteRequested(m_library);
}

void KbPanel::createFolder()
{
    if (m_library.isEmpty() && m_libraries.isEmpty()) {
        setStatusText(QStringLiteral("请先新建或选择一个知识库"));
        return;
    }
    const QString parent = targetFolderId();
    const QString title = parent.isEmpty()
        ? QStringLiteral("在库根新建文件夹")
        : QStringLiteral("在「%1」下新建文件夹").arg(folderLabel(parent));
    const QString name = ConfirmDialog::prompt(this, title, QString(), QStringLiteral("新建")).trimmed();
    if (name.isEmpty())
        return;
    emit folderCreateRequested(m_library, parent, name);
}

void KbPanel::renameFolder()
{
    const QString folderId = m_currentFolder;
    if (!isRealFolder(folderId))
        return;
    const QString name = ConfirmDialog::prompt(this, QStringLiteral("重命名文件夹"),
                                               folderLabel(folderId), QStringLiteral("保存"))
                             .trimmed();
    if (name.isEmpty())
        return;
    emit folderRenameRequested(folderId, name);
}

void KbPanel::deleteFolder()
{
    const QString folderId = m_currentFolder;
    if (!isRealFolder(folderId))
        return;

    QString parentId;
    for (const QVariant &value : m_folders) {
        const QVariantMap folder = value.toMap();
        if (folder.value(QStringLiteral("id")).toString() == folderId) {
            parentId = folder.value(QStringLiteral("parent_id")).toString();
            break;
        }
    }
    const QString message =
        QStringLiteral("确定删除文件夹「%1」？里面的 %2 份资料不会被删，会移到上一层。")
            .arg(folderLabel(folderId))
            .arg(folderTotal(folderId));
    if (!ConfirmDialog::ask(this, QStringLiteral("删除文件夹"), message, QStringLiteral("删除"), true))
        return;

    // 删完选中项回到上一层（app.js：currentFolder = parent_id || "root"）
    m_currentFolder = parentId.isEmpty() ? QStringLiteral("root") : parentId;
    rebuildTree();
    rebuildDocs();
    syncEnabled();
    emit folderDeleteRequested(folderId);
}

void KbPanel::pickFiles()
{
    if (m_library.isEmpty()) {
        setStatusText(QStringLiteral("请先在设置页新建或选一个知识库"));
        return;
    }
    const QString filter =
        QStringLiteral("资料文件 (*.pdf *.txt *.md *.docx *.xlsx *.doc *.xls)");
    const QStringList paths =
        QFileDialog::getOpenFileNames(this, QStringLiteral("添加资料"), QString(), filter);
    if (paths.isEmpty())
        return;
    emit filesImportRequested(paths, m_library, targetFolderId());
}

void KbPanel::importDirectory()
{
    const QString dir = m_dirInput ? m_dirInput->text().trimmed() : QString();
    if (dir.isEmpty()) {
        setStatusText(QStringLiteral("请先填一个本机目录路径"));
        return;
    }
    if (m_library.isEmpty()) {
        setStatusText(QStringLiteral("请先在设置页新建或选一个知识库"));
        return;
    }
    emit dirImportToFolderRequested(m_library, dir, targetFolderId());
}

void KbPanel::runSearch()
{
    const QString query = m_query ? m_query->text().trimmed() : QString();
    m_search.clear();
    if (query.isEmpty()) {
        m_searchNotice = QStringLiteral("先输一个检索词。");
        m_searchPending = false;
        rebuildSearchResult();
        syncEnabled();
        return;
    }
    m_searchNotice.clear();
    m_searchPending = true;
    rebuildSearchResult();
    syncEnabled();
    emit searchRequested(m_library, query);
}

void KbPanel::requestDocumentDelete(const QString &docId)
{
    const QString message =
        QStringLiteral("确定把《%1》从知识库移除？只删索引与内部副本，不动你本地的原文件。")
            .arg(documentName(docId));
    if (!ConfirmDialog::ask(this, QStringLiteral("从知识库移除"), message, QStringLiteral("移除"), true))
        return;
    emit documentDeleteRequested(docId);
}

void KbPanel::clearDropHints()
{
    for (KbTreeRow *row : m_treeRows)
        row->setDropTarget(false);
}

void KbPanel::clearLayout(QVBoxLayout *layout)
{
    clearBoxLayout(layout);
}

void KbPanel::clearLayout(QHBoxLayout *layout)
{
    clearBoxLayout(layout);
}

// ------------------------------------------------------------ 宿主回填

void KbPanel::enterTab()
{
    // 与 app.js kbEnterTab → kbOpenPanel 同义：重拉库列表 / 状态 / 归档结构 / 资料
    emit refreshRequested(m_library);
}

void KbPanel::leaveTab()
{
    // 面板没有需要停的定时器或轮询；检索结果保留，切回来还能看
}

QString KbPanel::currentLibrary() const
{
    return m_library;
}

void KbPanel::setLibraries(const QVariantList &libraries)
{
    // 换列表之前先记下旧 id：新建库时靠「新出现的那个」把选中项跟过去
    QSet<QString> previous;
    for (const QVariant &value : m_libraries)
        previous.insert(value.toMap().value(QStringLiteral("id")).toString());

    m_libraries = libraries;

    QString created;
    if (m_pendingCreate) {
        for (const QVariant &value : m_libraries) {
            const QString id = value.toMap().value(QStringLiteral("id")).toString();
            if (!id.isEmpty() && !previous.contains(id)) {
                created = id;
                break;
            }
        }
    }
    m_pendingCreate = false;

    // 当前库被删掉（或第一次进来还没选）就落到第一个库；没有库则回到空串
    QString wanted = m_library;
    bool found = false;
    for (const QVariant &value : m_libraries) {
        if (value.toMap().value(QStringLiteral("id")).toString() == m_library) {
            found = true;
            break;
        }
    }
    if (!created.isEmpty()) {
        wanted = created;
    } else if (!found) {
        wanted = m_libraries.isEmpty()
            ? QString()
            : m_libraries.first().toMap().value(QStringLiteral("id")).toString();
    }

    const bool changed = wanted != m_library;
    m_library = wanted;
    if (changed) {
        resetFolderState();
        clearLibraryData();
        m_search.clear();
        m_searchNotice.clear();
        m_searchPending = false;
        m_statusOverride.clear();
    }

    rebuildLibraryCombo();
    rebuildCards();
    if (changed) {
        renderStatus();
        rebuildTree();
        rebuildDocs();
        rebuildSearchResult();
    }
    syncEnabled();

    // 选中的库跟宿主刚拉的那份不是同一个：让宿主按新库重拉 folders / documents / status
    if (changed) {
        emit libraryChanged(m_library);
        emit folderChanged(m_library, m_currentFolder); // 此时已回到 all
    }
}

void KbPanel::setStatus(const QVariantMap &status)
{
    m_status = status;
    // 状态是权威数据：渲染时以它为准，之前 setStatusText 的临时文案作废（与 renderKb 同）
    m_statusOverride.clear();
    renderStatus();
    rebuildCards();
}

void KbPanel::setFolders(const QVariantList &folders, const QVariantMap &tree)
{
    m_folders = folders;
    m_tree = tree;

    // 默认全展开：只给新出现的文件夹加进展开集合，用户手动收起的保持收起
    m_expanded.insert(QStringLiteral("root"));
    QSet<QString> known;
    for (const QVariant &value : m_folders) {
        const QVariantMap folder = value.toMap();
        const QString id = folder.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        known.insert(id);
        if (!m_knownFolders.contains(id))
            m_expanded.insert(id);
    }
    m_knownFolders = known;

    // 选中的文件夹没了（被删 / 换了库）就回到「全部资料」
    if (isRealFolder(m_currentFolder) && !known.contains(m_currentFolder))
        m_currentFolder = QStringLiteral("all");

    rebuildTree();
    rebuildDocs();
    syncEnabled();
}

void KbPanel::setDocuments(const QVariantList &documents, int total)
{
    m_docs = documents;
    m_docsTotal = total > 0 ? total : intOf(m_status, "documents");
    rebuildDocs();
    rebuildTree(); // 「全部资料」的计数来自 docsTotal
    syncEnabled();
}

void KbPanel::setSearchResult(const QVariantMap &result)
{
    m_search = result;
    m_searchNotice.clear();
    m_searchPending = false;
    rebuildSearchResult();
    syncEnabled();
}

void KbPanel::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    syncEnabled();
}

void KbPanel::setStatusText(const QString &text)
{
    m_statusOverride = text;
    renderStatus();
}

} // namespace gs
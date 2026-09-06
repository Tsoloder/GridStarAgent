#include "commonwidgets.h"
#include "theme.h"

#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QSvgRenderer>

namespace gs {

// ---------------------------------------------------------------- FlowLayout

FlowLayout::FlowLayout(QWidget *parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing)
{
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem *item) { m_items.append(item); }

int FlowLayout::horizontalSpacing() const
{
    if (m_hSpace >= 0)
        return m_hSpace;
    return smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
}

int FlowLayout::verticalSpacing() const
{
    if (m_vSpace >= 0)
        return m_vSpace;
    return smartSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::count() const { return m_items.size(); }
QLayoutItem *FlowLayout::itemAt(int index) const { return m_items.value(index); }
QLayoutItem *FlowLayout::takeAt(int index)
{
    if (index >= 0 && index < m_items.size())
        return m_items.takeAt(index);
    return nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const { return Qt::Orientations(); }

bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int width) const
{
    return doLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const { return minimumSize(); }

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem *item : qAsConst(m_items))
        size = size.expandedTo(item->minimumSize());
    const QMargins margins = contentsMargins();
    size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    return size;
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
    int left, top, right, bottom;
    getContentsMargins(&left, &top, &right, &bottom);
    const QRect effective = rect.adjusted(+left, +top, -right, -bottom);
    int x = effective.x();
    int y = effective.y();
    int lineHeight = 0;

    for (QLayoutItem *item : qAsConst(m_items)) {
        const int spaceX = horizontalSpacing();
        const int spaceY = verticalSpacing();
        int nextX = x + item->sizeHint().width() + spaceX;
        if (nextX - spaceX > effective.right() && lineHeight > 0) {
            x = effective.x();
            y = y + lineHeight + spaceY;
            nextX = x + item->sizeHint().width() + spaceX;
            lineHeight = 0;
        }
        if (!testOnly)
            item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
        x = nextX;
        lineHeight = qMax(lineHeight, item->sizeHint().height());
    }
    return y + lineHeight - rect.y() + bottom;
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const
{
    QObject *parent = this->parent();
    if (!parent)
        return 6;
    if (parent->isWidgetType()) {
        QWidget *pw = static_cast<QWidget *>(parent);
        return pw->style()->pixelMetric(pm, nullptr, pw);
    }
    return static_cast<QLayout *>(parent)->spacing();
}

// ------------------------------------------------------------------- Chevron

Chevron::Chevron(QWidget *parent) : QWidget(parent)
{
    setFixedSize(10, 10);
}

void Chevron::setOpen(bool open)
{
    if (m_open == open)
        return;
    m_open = open;
    update();
}

void Chevron::paintEvent(QPaintEvent *)
{
    // CSS .tool-chevron：收起为 "›"，展开为 "⌄"，统一由 SVG 渲染并着色
    const int side = qRound(qMin(width(), height()) * 1.4);
    const QPixmap pm = iconPixmap(m_open ? QStringLiteral("chevron-down")
                                         : QStringLiteral("chevron-right"),
                                  QColor("#718894"), side);
    QPainter p(this);
    p.drawPixmap((width() - pm.width()) / 2, (height() - pm.height()) / 2, pm);
}

// ----------------------------------------------------------------- StatusDot

StatusDot::StatusDot(QWidget *parent) : QWidget(parent)
{
    setFixedSize(8, 8);
}

void StatusDot::setState(const QString &state)
{
    if (m_state == state)
        return;
    m_state = state;
    update();
}

void StatusDot::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor core = Orange;
    if (m_state == QLatin1String("succeeded") || m_state == QLatin1String("done"))
        core = Green;
    else if (m_state == QLatin1String("failed"))
        core = Red;

    if (m_state == QLatin1String("running")) {
        QColor glow = core;
        glow.setAlpha(70);
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(QPointF(width() / 2.0, height() / 2.0), 4.0, 4.0);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(core);
    p.drawEllipse(QPointF(width() / 2.0, height() / 2.0), 3.0, 3.0);
}

// ---------------------------------------------------------- ConnectionButton

ConnectionButton::ConnectionButton(QWidget *parent) : QPushButton(parent)
{
    setObjectName(QStringLiteral("connection"));
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("重新检查连接"));
    setState(QStringLiteral("checking"), QStringLiteral("连接中"));
}

void ConnectionButton::setState(const QString &state, const QString &label)
{
    m_state = state;
    setText(label);
    setProperty("state", state);
    restyle(this);
    update();
}

void ConnectionButton::paintEvent(QPaintEvent *)
{
    QColor color = Orange;
    if (m_state == QLatin1String("online"))
        color = Green;
    else if (m_state == QLatin1String("offline"))
        color = Red;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QFont f = font();
    f.setPixelSize(scaledPx(11));
    p.setFont(f);
    const QFontMetrics fm(f);
    const int pad = 6;
    const int dotSize = 7;
    const int gap = 6;
    const int textWidth = fm.horizontalAdvance(text());
    const int total = dotSize + gap + textWidth;
    int x = qMax(pad, (width() - total) / 2);
    const qreal cy = height() / 2.0;

    // box-shadow: 0 0 8px currentColor
    QColor glow = color;
    glow.setAlpha(60);
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(QPointF(x + dotSize / 2.0, cy), dotSize / 2.0 + 3.0, dotSize / 2.0 + 3.0);
    p.setBrush(color);
    p.drawEllipse(QPointF(x + dotSize / 2.0, cy), dotSize / 2.0, dotSize / 2.0);

    p.setPen(color);
    p.drawText(QRect(x + dotSize + gap, 0, textWidth + 2, height()),
               Qt::AlignVCenter | Qt::AlignLeft, text());
}

// -------------------------------------------------------------- ComboTrigger

ComboTrigger::ComboTrigger(QWidget *parent) : QWidget(parent)
{
    setClass(this, QStringLiteral("comboTrigger"));
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(25);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(5, 0, 5, 0);
    layout->setSpacing(5);

    m_text = new QLabel(this);
    setClass(m_text, QStringLiteral("comboText"));
    m_text->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    m_chevron = new QLabel(this);
    setClass(m_chevron, QStringLiteral("comboText"));
    m_chevron->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_chevron->setPixmap(iconPixmap(QStringLiteral("chevron-down"),
                                    QColor(QStringLiteral("#afc0c9")), scaledPx(10)));

    layout->addWidget(m_text, 1);
    layout->addWidget(m_chevron, 0);
}

void ComboTrigger::setRightAligned(bool right)
{
    m_right = right;
    m_text->setAlignment(right ? (Qt::AlignRight | Qt::AlignVCenter)
                               : (Qt::AlignLeft | Qt::AlignVCenter));
    updateElided();
}

void ComboTrigger::setOpen(bool open)
{
    m_open = open;
    m_chevron->setPixmap(iconPixmap(open ? QStringLiteral("chevron-up")
                                         : QStringLiteral("chevron-down"),
                                    QColor(QStringLiteral("#afc0c9")), scaledPx(10)));
}

void ComboTrigger::setText(const QString &text)
{
    m_full = text;
    updateElided();
}

void ComboTrigger::updateElided()
{
    const int available = qMax(10, width() - 10 - m_chevron->sizeHint().width() - 5);
    QFont f = font();
    f.setPixelSize(scaledPx(10));
    m_text->setText(elidedText(m_full, QFontMetrics(f), available));
}

void ComboTrigger::refreshZoom()
{
    setOpen(m_open); // 按当前缩放重建 chevron（保持展开状态）
    updateElided();
}

void ComboTrigger::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateElided();
}

void ComboTrigger::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit clicked();
    QWidget::mousePressEvent(event);
}

bool ComboTrigger::event(QEvent *event)
{
    if (event->type() == QEvent::FontChange)
        updateElided();
    return QWidget::event(event);
}

// -------------------------------------------------------------- ProgressLine

ProgressLine::ProgressLine(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(2);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ProgressLine::setPercent(int percent)
{
    m_percent = qBound(0, percent, 100);
    update();
}

void ProgressLine::paintEvent(QPaintEvent *)
{
    if (m_percent <= 0)
        return;
    QPainter p(this);
    const qreal w = width() * (m_percent / 100.0);

    QColor glow = Cyan;
    glow.setAlpha(70);
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawRect(QRectF(0, 0, w, height() + 1));

    QLinearGradient gradient(0, 0, qMax<qreal>(w, 1), 0);
    gradient.setColorAt(0.0, QColor("#2b86ab"));
    gradient.setColorAt(0.5, Cyan);
    gradient.setColorAt(1.0, QColor("#8fdcf7"));
    p.setBrush(gradient);
    p.drawRect(QRectF(0, 0, w, height()));
}

// ---------------------------------------------------------------- AttachChip

AttachChip::AttachChip(const QString &name, qint64 bytes, const QString &ext, bool uploading,
                       QWidget *parent)
    : QFrame(parent)
{
    setClass(this, QStringLiteral("attachChip"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(24);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(5);

    auto *extLabel = makeLabel(QStringLiteral("attachExt"), ext.toUpper(), this);
    m_name = makeLabel(QStringLiteral("attachName"), name, this);
    m_name->setMaximumWidth(140);
    QFontMetrics fm(m_name->font());
    m_name->setText(elidedText(name, fm, 140));
    m_name->setToolTip(name);
    m_size = makeLabel(QStringLiteral("attachSize"), fileSizeLabel(bytes), this);

    auto *remove = new IconPushButton(this);
    setClass(remove, QStringLiteral("attachRemove"));
    remove->setFixedSize(15, 15);
    remove->setCursor(Qt::PointingHandCursor);
    remove->setToolTip(QStringLiteral("移除附件"));
    remove->setIconColors(QColor(QStringLiteral("#8ba0ac")),
                          QColor(QStringLiteral("#ffffff")));
    remove->setIconName(QStringLiteral("x"), 9);
    connect(remove, &QPushButton::clicked, this, &AttachChip::removeClicked);

    layout->addWidget(extLabel);
    layout->addWidget(m_name);
    layout->addWidget(m_size);
    layout->addWidget(remove);

    setUploading(uploading);
}

void AttachChip::setUploading(bool uploading)
{
    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(graphicsEffect());
    if (uploading) {
        if (!effect) {
            effect = new QGraphicsOpacityEffect(this);
            setGraphicsEffect(effect);
        }
        effect->setOpacity(0.55);
    } else if (effect) {
        effect->setOpacity(1.0);
    }
}

// ------------------------------------------------------------- IconPushButton

IconPushButton::IconPushButton(QWidget *parent)
    : QPushButton(parent)
{
    setAttribute(Qt::WA_Hover, true);
}

void IconPushButton::setIconName(const QString &name, int sizePx)
{
    m_name = name;
    if (sizePx > 0)
        m_sizePx = sizePx;
    refreshIcon();
}

void IconPushButton::setIconColors(const QColor &normal, const QColor &hover,
                                   const QColor &disabled)
{
    m_normal = normal;
    m_hover = hover.isValid() ? hover : normal;
    m_disabled = disabled.isValid() ? disabled : normal;
    refreshIcon();
}

void IconPushButton::refreshIcon()
{
    if (m_name.isEmpty())
        return;
    const QColor color = !isEnabled() ? m_disabled : (m_hovering ? m_hover : m_normal);
    const QPixmap pm = iconPixmap(m_name, color, m_sizePx);
    QIcon icon;
    // 四种模式共用同一张已着色的图，避免 QIcon 对 Disabled 态再做灰度变换
    icon.addPixmap(pm, QIcon::Normal);
    icon.addPixmap(pm, QIcon::Disabled);
    icon.addPixmap(pm, QIcon::Active);
    icon.addPixmap(pm, QIcon::Selected);
    setIcon(icon);
    setIconSize(QSize(m_sizePx, m_sizePx));
}

bool IconPushButton::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::HoverEnter:
        m_hovering = true;
        refreshIcon();
        break;
    case QEvent::HoverLeave:
        m_hovering = false;
        refreshIcon();
        break;
    case QEvent::EnabledChange:
        refreshIcon();
        break;
    default:
        break;
    }
    return QPushButton::event(event);
}

// ------------------------------------------------------------------- helpers

QPixmap iconPixmap(const QString &name, const QColor &color, int sizePx)
{
    if (name.isEmpty() || sizePx <= 0)
        return QPixmap();
    // 64px 母版缓存：同一 SVG 只解析渲染一次，之后按颜色/尺寸复用
    static QHash<QString, QImage> masterCache;
    QImage master = masterCache.value(name);
    if (master.isNull()) {
        QSvgRenderer renderer(QStringLiteral(":/icons/%1.svg").arg(name));
        if (!renderer.isValid())
            return QPixmap();
        master = QImage(64, 64, QImage::Format_ARGB32_Premultiplied);
        master.fill(Qt::transparent);
        QPainter p(&master);
        p.setRenderHint(QPainter::Antialiasing);
        renderer.render(&p, QRectF(0, 0, 64, 64));
        p.end();
        masterCache.insert(name, master);
    }
    // SourceIn：保留母版 alpha，把绘制的部分整体替换成目标颜色
    QImage tinted = master;
    QPainter p(&tinted);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(tinted.rect(), color);
    p.end();
    return QPixmap::fromImage(tinted).scaled(sizePx, sizePx, Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation);
}

QString elidedText(const QString &text, const QFontMetrics &fm, int width)
{
    if (width <= 0 || fm.horizontalAdvance(text) <= width)
        return text;
    return fm.elidedText(text, Qt::ElideRight, width);
}

QString fileSizeLabel(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', bytes < 10240 ? 1 : 0);
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

QLabel *makeLabel(const QString &className, const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    setClass(label, className);
    label->setAttribute(Qt::WA_StyledBackground, false);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

// QSS 的 .class 选择器等价于 [class~="foo"]：匹配动态属性 "class"（空格分隔可多个），
// 而非 objectName；objectName 只服务于 #id 选择器。
void setClass(QWidget *w, const QString &cls)
{
    if (w)
        w->setProperty("class", cls);
}

} // namespace gs

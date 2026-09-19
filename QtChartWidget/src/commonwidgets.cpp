#include "commonwidgets.h"
#include "theme.h"

#include <QEvent>
#include <QFile>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QStyle>
#include <QSvgRenderer>
#include <QTimer>
#include <QtMath>

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
                                  gs::palette().muted2, side);
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
    QColor core = gs::palette().orange;
    if (m_state == QLatin1String("succeeded") || m_state == QLatin1String("done"))
        core = gs::palette().green;
    else if (m_state == QLatin1String("failed"))
        core = gs::palette().red;

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
    QColor color = gs::palette().orange;
    if (m_state == QLatin1String("online"))
        color = gs::palette().green;
    else if (m_state == QLatin1String("offline"))
        color = gs::palette().red;

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
                                    gs::palette().muted, scaledPx(10)));

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
                                    gs::palette().muted, scaledPx(10)));
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
    m_animation = new QPropertyAnimation(this, "animatedPercent", this);
    m_animation->setDuration(450); // CSS transition: width .45s ease
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
}

void ProgressLine::setPercent(int percent)
{
    m_percent = qBound(0, percent, 100);
    update();
}

void ProgressLine::setAnimatedPercent(int percent)
{
    m_percent = qBound(0, percent, 100);
    update();
}

void ProgressLine::animateTo(int percent)
{
    const int target = qBound(0, percent, 100);
    if (m_percent == target)
        return;
    m_animation->stop();
    m_animation->setStartValue(m_percent);
    m_animation->setEndValue(target);
    m_animation->start();
}

void ProgressLine::paintEvent(QPaintEvent *)
{
    if (m_percent <= 0)
        return;
    QPainter p(this);
    const qreal w = width() * (m_percent / 100.0);

    QColor glow = gs::palette().cyan;
    glow.setAlpha(70);
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawRect(QRectF(0, 0, w, height() + 1));

    QLinearGradient gradient(0, 0, qMax<qreal>(w, 1), 0);
    gradient.setColorAt(0.0, gs::palette().cyanMid);
    gradient.setColorAt(0.5, gs::palette().cyan);
    gradient.setColorAt(1.0, gs::palette().cyan.lighter(130));
    p.setBrush(gradient);
    p.drawRect(QRectF(0, 0, w, height()));
}

// ---------------------------------------------------------------- ElidedLabel

ElidedLabel::ElidedLabel(QWidget *parent) : QLabel(parent)
{
    setTextInteractionFlags(Qt::NoTextInteraction);
}

void ElidedLabel::setFullText(const QString &text)
{
    m_full = text;
    setToolTip(text);
    updateElide();
}

QSize ElidedLabel::sizeHint() const
{
    const QFontMetrics fm(font());
    return QSize(fm.horizontalAdvance(m_full), fm.height());
}

QSize ElidedLabel::minimumSizeHint() const
{
    return QSize(0, QLabel::minimumSizeHint().height());
}

void ElidedLabel::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    updateElide();
}

void ElidedLabel::updateElide()
{
    if (width() <= 0)
        return;
    const QString elided = elidedText(m_full, fontMetrics(), width());
    if (text() != elided) // 幂等：setText 可能再触发一次 resize，比较后提前返回避免来回
        setText(elided);
}

// ---------------------------------------------------------------- AttachChip

AttachChip::AttachChip(const QVariantMap &item, QWidget *parent)
    : QFrame(parent)
{
    setClass(this, QStringLiteral("attachChip"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(24);

    const QString name = item.value(QStringLiteral("name")).toString();
    const QString ext = item.value(QStringLiteral("ext")).toString();
    const bool uploading = item.value(QStringLiteral("uploading")).toBool();
    m_bytes = item.value(QStringLiteral("size")).toLongLong();

    // 宿主没给 kind 时按扩展名推断（webui attachKind）：否则拖进一张图片不会出缩略图
    QString kind = item.value(QStringLiteral("kind")).toString();
    if (kind.isEmpty()) {
        static const QStringList imageExts{ QStringLiteral("png"), QStringLiteral("jpg"),
                                            QStringLiteral("jpeg"), QStringLiteral("gif"),
                                            QStringLiteral("webp"), QStringLiteral("bmp") };
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        const QString fromName = dot > 0 ? name.mid(dot + 1).toLower() : QString();
        if (imageExts.contains(fromName))
            kind = QStringLiteral("image");
    }

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(5);

    // 图片给 22×22 缩略图，其余给扩展名标签（webui .attach-chip img / .attach-ext）
    QString preview = item.value(QStringLiteral("path")).toString();
    if (preview.isEmpty())
        preview = item.value(QStringLiteral("url")).toString();
    QPixmap pixmap;
    if (kind == QLatin1String("image") && !preview.isEmpty() && QFile::exists(preview))
        pixmap = QPixmap(preview);
    if (!pixmap.isNull()) {
        auto *thumb = new QLabel(this);
        setClass(thumb, QStringLiteral("attachChipThumb"));
        thumb->setFixedSize(22, 22);
        thumb->setPixmap(pixmap.scaled(22, 22, Qt::KeepAspectRatioByExpanding,
                                       Qt::SmoothTransformation));
        layout->addWidget(thumb);
    } else {
        QString label = ext.isEmpty() ? QStringLiteral("file") : ext;
        layout->addWidget(makeLabel(QStringLiteral("attachExt"), label.toUpper(), this));
    }

    m_name = makeLabel(QStringLiteral("attachName"), name, this);
    m_name->setMaximumWidth(140);
    QFontMetrics fm(m_name->font());
    m_name->setText(elidedText(name, fm, 140));
    m_name->setToolTip(name);
    m_size = makeLabel(QStringLiteral("attachSize"),
                       uploading ? QStringLiteral("上传中…") : fileSizeLabel(m_bytes), this);

    auto *remove = new IconPushButton(this);
    setClass(remove, QStringLiteral("attachRemove"));
    remove->setFixedSize(15, 15);
    remove->setCursor(Qt::PointingHandCursor);
    remove->setToolTip(QStringLiteral("移除附件"));
    remove->setIconColors(QColor(QStringLiteral("#8ba0ac")),
                          QColor(QStringLiteral("#ffffff")));
    remove->setIconName(QStringLiteral("x"), 9);
    connect(remove, &QPushButton::clicked, this, &AttachChip::removeClicked);

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
    // CSS .attach-chip 的上传态文案：大小位改显示「上传中…」
    if (m_size)
        m_size->setText(uploading ? QStringLiteral("上传中…") : fileSizeLabel(m_bytes));
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

// ------------------------------------------------------------------ PulseDot

PulseDot::PulseDot(QWidget *parent) : QWidget(parent)
{
    m_color = gs::palette().cyan;
    setFixedSize(6, 6);
    m_timer = new QTimer(this);
    m_timer->setInterval(60);
    connect(m_timer, &QTimer::timeout, this, [this] {
        m_phase += 0.06;
        if (m_phase > 1.0)
            m_phase -= 1.0;
        update();
    });
}

void PulseDot::setColor(const QColor &color)
{
    m_color = color;
    update();
}

void PulseDot::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (active)
        m_timer->start();
    else
        m_timer->stop();
    update();
}

void PulseDot::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    QColor color = m_color;
    if (m_active) {
        // CSS @keyframes badgePulse：1.4s 内 1 → .45 → 1
        const qreal wave = 0.5 + 0.5 * qCos(m_phase * 6.28318530717958647692);
        color.setAlphaF(0.45 + 0.55 * wave);
    }
    p.setBrush(color);
    p.drawEllipse(QPointF(rect().center()), 3.0, 3.0);
}

// --------------------------------------------------------------- ThemeSwatch

ThemeSwatch::ThemeSwatch(const QString &themeId, int sizePx, bool withBorder, QWidget *parent)
    : QWidget(parent), m_themeId(themeId), m_size(sizePx), m_border(withBorder)
{
    setFixedSize(sizePx, sizePx);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void ThemeSwatch::paintEvent(QPaintEvent *)
{
    // .sw-dark/.sw-silver/.sw-blue：135° 双色各占一半
    QColor first(QStringLiteral("#18242e"));
    QColor second(QStringLiteral("#50badf"));
    if (m_themeId == QLatin1String("silver")) {
        first = QColor(QStringLiteral("#f1f3f5"));
        second = QColor(QStringLiteral("#0e7ca6"));
    } else if (m_themeId == QLatin1String("blue")) {
        first = QColor(QStringLiteral("#dce7f3"));
        second = QColor(QStringLiteral("#1b84a8"));
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient gradient(box.topLeft(), box.bottomRight());
    gradient.setColorAt(0.0, first);
    gradient.setColorAt(0.499, first);
    gradient.setColorAt(0.5, second);
    gradient.setColorAt(1.0, second);
    p.setBrush(gradient);
    if (m_border) {
        p.setPen(gs::palette().lineStrong);
    } else {
        p.setPen(Qt::NoPen);
    }
    p.drawEllipse(box);
}

// -------------------------------------------------------------- TurnRailDot

TurnRailDot::TurnRailDot(int turn, QWidget *parent) : QWidget(parent), m_turn(turn)
{
    setFixedSize(12, 12);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
}

void TurnRailDot::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    update();
}

void TurnRailDot::enterEvent(QEvent *event)
{
    m_hover = true;
    update();
    emit hovered(m_turn);
    QWidget::enterEvent(event);
}

void TurnRailDot::leaveEvent(QEvent *event)
{
    m_hover = false;
    update();
    emit unhovered();
    QWidget::leaveEvent(event);
}

void TurnRailDot::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit activated(m_turn);
    QWidget::mouseReleaseEvent(event);
}

void TurnRailDot::paintEvent(QPaintEvent *)
{
    // .turn-rail-dot:before：5px 圆点，常态 opacity .3，悬浮 .7，当前 1（放大 1.4 + 光晕）
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor color = gs::palette().textBright;
    color.setAlphaF(m_active ? 1.0 : (m_hover ? 0.7 : 0.3));
    p.setPen(Qt::NoPen);
    if (m_active) {
        QColor glow = gs::palette().textBright;
        glow.setAlphaF(0.35);
        p.setBrush(glow);
        p.drawEllipse(QPointF(rect().center()), 6.0, 6.0);
    }
    p.setBrush(color);
    p.drawEllipse(QPointF(rect().center()), m_active ? 3.5 : 2.5, m_active ? 3.5 : 2.5);
}

// ------------------------------------------------------------------- MiniBar

MiniBar::MiniBar(QWidget *parent) : QWidget(parent)
{
    m_fill = gs::palette().cyan;
    setFixedHeight(7);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void MiniBar::setRatio(qreal ratio)
{
    m_ratio = ratio;
    update();
}

void MiniBar::setFill(const QColor &color)
{
    m_fill = color;
    update();
}

void MiniBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(gs::palette().line);
    p.setBrush(gs::palette().inset);
    p.drawRoundedRect(box, height() / 2.0, height() / 2.0);
    if (m_ratio <= 0.0)
        return;
    const qreal w = qMax<qreal>(2.0, box.width() * qMin<qreal>(m_ratio, 1.0));
    p.setPen(Qt::NoPen);
    p.setBrush(m_fill);
    p.drawRoundedRect(QRectF(box.left(), box.top(), w, box.height()),
                      height() / 2.0, height() / 2.0);
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

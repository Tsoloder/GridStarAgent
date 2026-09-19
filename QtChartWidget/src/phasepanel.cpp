#include "phasepanel.h"
#include "commonwidgets.h"
#include "theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QVBoxLayout>

namespace gs {

// ------------------------------------------------------------------ PhaseStep

PhaseStep::PhaseStep(const QString &title, const QString &note, const QString &status,
                     QWidget *parent)
    : QWidget(parent), m_title(title), m_note(note), m_status(status)
{
    setAttribute(Qt::WA_Hover, true);
    setFixedHeight(30); // CSS: padding 6px 8px + 18px 圆圈
    if (!note.isEmpty())
        setToolTip(note);
    else if (!title.isEmpty())
        setToolTip(title);

    // 运行态的呼吸圆点：叠在状态圆圈中心，非运行态隐藏
    m_pulse = new PulseDot(this);
    m_pulse->setColor(gs::palette().cyan);
    m_pulse->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_pulse->move(8 + 9 - 3, 6 + 9 - 3);
    syncPulse();
}

void PhaseStep::syncPulse()
{
    const bool running = m_status == QLatin1String("active") || m_status == QLatin1String("running")
                         || m_status == QLatin1String("in_progress");
    m_pulse->setVisible(running);
    m_pulse->setActive(running);
}

void PhaseStep::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    syncPulse();
    update();
}

void PhaseStep::setConnectors(bool top, bool bottom)
{
    m_connectorTop = top;
    m_connectorBottom = bottom;
    update();
}

bool PhaseStep::event(QEvent *event)
{
    if (event->type() == QEvent::HoverEnter) {
        m_hover = true;
        update();
    } else if (event->type() == QEvent::HoverLeave) {
        m_hover = false;
        update();
    }
    return QWidget::event(event);
}

void PhaseStep::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QString st = m_status;
    const bool done = st == QLatin1String("done") || st == QLatin1String("succeeded")
                      || st == QLatin1String("completed");
    const bool running = st == QLatin1String("active") || st == QLatin1String("running")
                         || st == QLatin1String("in_progress");
    const bool failed = st == QLatin1String("failed");
    const bool skipped = st == QLatin1String("skipped");

    const Palette &pal = gs::palette();
    QColor text = pal.muted;
    QColor glyph = pal.muted2;
    QColor circleBorder = pal.line;
    QColor circleBg = pal.inset;
    QString markIcon = QStringLiteral("circle");
    if (done) {
        text = pal.okText;
        markIcon = QStringLiteral("check");
        circleBorder = pal.okLine;
        circleBg = pal.okTint;
        glyph = pal.green;
    } else if (running) {
        text = pal.textBright;
        markIcon.clear(); // 运行态用呼吸圆点，不再画字形
        circleBorder = pal.cyanMid;
        circleBg = pal.accentTint;
        glyph = pal.cyan;
    } else if (failed) {
        text = pal.errText;
        markIcon = QStringLiteral("x");
        circleBorder = pal.errLine;
        circleBg = pal.errTint;
        glyph = pal.red;
    } else if (skipped) {
        markIcon = QStringLiteral("chevrons-right");
    }

    p.setOpacity(skipped ? 0.55 : 1.0); // CSS .phase-step.skipped { opacity: .55 }

    // 背景：当前执行项整行铺背景色（不再画左侧竖条）；hover 同样给淡底
    QPainterPath bg;
    bg.addRoundedRect(QRectF(0.5, 0.5, width() - 1.0, height() - 1.0), 4, 4);
    if (running)
        p.fillPath(bg, pal.accentTint);
    else if (m_hover)
        p.fillPath(bg, pal.accentTint);

    // 步骤间连接线：left 16.5px（padding 8 + 圆心 8.5），宽 1px
    p.setPen(Qt::NoPen);
    p.setBrush(pal.line);
    if (m_connectorTop)
        p.drawRect(QRectF(16.5, 0, 1, 6));
    if (m_connectorBottom)
        p.drawRect(QRectF(16.5, 26, 1, height() - 26));

    // 状态圆圈 18×18 @ (8,6)
    const QRectF circle(8.5, 6.5, 17, 17);
    QPen circlePen(circleBorder);
    circlePen.setWidthF(1);
    if (skipped)
        circlePen.setStyle(Qt::DashLine); // CSS: border-style dashed
    p.setPen(circlePen);
    p.setBrush(circleBg);
    p.drawEllipse(circle);
    // 状态图标 9×9，居中于圆圈（圆心 17,15）；运行态的呼吸圆点由 PulseDot 子控件承担
    if (!markIcon.isEmpty())
        p.drawPixmap(12.5, 10.5, iconPixmap(markIcon, glyph, 9));

    // 标题（11px, weight 500）+ 备注（10px, 55% 透明度）
    const int textLeft = 8 + 18 + 9; // marker + margin-right 9px
    const int available = qMax(0, width() - textLeft - 8);

    QFont titleFont = p.font();
    titleFont.setPixelSize(scaledPx(11));
    titleFont.setWeight(QFont::Medium);
    const QFontMetrics titleFm(titleFont);
    QFont noteFont = titleFont;
    noteFont.setPixelSize(scaledPx(10));
    noteFont.setWeight(QFont::Normal);
    const QFontMetrics noteFm(noteFont);

    int titleWidth = qMin(titleFm.horizontalAdvance(m_title), available);
    p.setFont(titleFont);
    p.setPen(text);
    p.drawText(QRect(textLeft, 0, titleWidth, height()), Qt::AlignVCenter | Qt::AlignLeft,
               titleFm.elidedText(m_title, Qt::ElideRight, titleWidth));

    if (!m_note.isEmpty()) {
        const int noteLeft = textLeft + titleWidth + 9; // CSS margin-left 9px
        const int noteWidth = width() - 8 - noteLeft;
        if (noteWidth > 0) {
            QColor noteColor = text;
            noteColor.setAlphaF(noteColor.alphaF() * 0.55);
            p.setFont(noteFont);
            p.setPen(noteColor);
            p.drawText(QRect(noteLeft, 0, noteWidth, height()), Qt::AlignVCenter | Qt::AlignLeft,
                       noteFm.elidedText(m_note, Qt::ElideRight, noteWidth));
        }
    }
}

// ----------------------------------------------------------------- PhasePanel

PhasePanel::PhasePanel(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("phasePanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_head = new QWidget(this);
    m_head->setObjectName(QStringLiteral("phaseHead"));
    m_head->setAttribute(Qt::WA_StyledBackground, true);
    m_head->setCursor(Qt::PointingHandCursor);
    m_head->setToolTip(QStringLiteral("点击展开/收起进度"));
    // webui 的 .phase-head 是 role="button" tabindex="0"
    m_head->setFocusPolicy(Qt::TabFocus);
    m_head->installEventFilter(this);

    auto *hl = new QHBoxLayout(m_head);
    hl->setContentsMargins(10, 8, 10, 8); // CSS padding 8px 10px
    hl->setSpacing(0);

    m_title = new QLabel(QStringLiteral("阶段计划"), m_head);
    m_title->setObjectName(QStringLiteral("phaseTitle"));
    m_count = new QLabel(QStringLiteral("0/0"), m_head);
    m_count->setObjectName(QStringLiteral("phaseCount"));
    m_chevron = new QLabel(m_head);
    setClass(m_chevron, QStringLiteral("phaseChevron"));
    m_chevron->setPixmap(iconPixmap(QStringLiteral("chevron-up"),
                                    gs::palette().muted2, 10));
    hl->addWidget(m_title, 1);
    hl->addSpacing(8); // CSS margin-left 8px
    hl->addWidget(m_count, 0);
    hl->addSpacing(8);
    hl->addWidget(m_chevron, 0);

    // CSS .phase-progress：绝对定位于 head 底部（bottom:-1px），高 2px
    m_progress = new ProgressLine(m_head);
    m_progress->setFixedHeight(2);
    m_progress->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    m_steps = new QScrollArea(this);
    m_steps->setObjectName(QStringLiteral("phaseSteps"));
    m_steps->setFrameShape(QFrame::NoFrame);
    m_steps->setWidgetResizable(true);
    m_steps->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_steps->setMaximumHeight(230); // CSS max-height 230px
    m_steps->setVisible(false);

    m_inner = new QWidget;
    m_inner->setObjectName(QStringLiteral("phaseStepsInner"));
    m_inner->setAttribute(Qt::WA_StyledBackground, true);
    m_innerLayout = new QVBoxLayout(m_inner);
    m_innerLayout->setContentsMargins(8, 7, 8, 8); // CSS padding 7px 8px 8px
    m_innerLayout->setSpacing(0);
    m_innerLayout->addStretch(1);
    m_steps->setWidget(m_inner);

    layout->addWidget(m_head);
    layout->addWidget(m_steps);
}

void PhasePanel::setPlan(const QVariantMap &plan)
{
    const QVariantList phases = plan.value(QStringLiteral("phases")).toList();
    if (phases.isEmpty())
        return; // app.js：phases 非数组时直接返回

    setVisible(true);

    QString title = plan.value(QStringLiteral("title")).toString();
    if (title.isEmpty())
        title = QStringLiteral("阶段计划");

    int completed = 0;
    for (const QVariant &item : phases) {
        const QString status = item.toMap().value(QStringLiteral("status")).toString();
        if (status == QLatin1String("done") || status == QLatin1String("succeeded")
            || status == QLatin1String("completed") || status == QLatin1String("skipped"))
            ++completed;
    }

    m_title->setText(title);
    m_count->setText(QStringLiteral("%1/%2").arg(completed).arg(phases.size()));
    m_progress->animateTo(qRound(completed * 100.0 / phases.size()));

    while (QLayoutItem *item = m_innerLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    for (int i = 0; i < phases.size(); ++i) {
        const QVariantMap phase = phases.at(i).toMap();
        QString stepTitle = phase.value(QStringLiteral("title")).toString();
        if (stepTitle.isEmpty())
            stepTitle = phase.value(QStringLiteral("id")).toString();
        if (stepTitle.isEmpty())
            stepTitle = QStringLiteral("阶段");
        QString note = phase.value(QStringLiteral("note")).toString();
        if (note.isEmpty())
            note = phase.value(QStringLiteral("desc")).toString();
        QString status = phase.value(QStringLiteral("status")).toString();
        if (status.isEmpty())
            status = QStringLiteral("pending");

        auto *row = new PhaseStep(stepTitle, note, status, m_inner);
        row->setConnectors(i > 0, i < phases.size() - 1);
        m_innerLayout->addWidget(row);
    }
    m_innerLayout->addStretch(1);
    layoutProgress();
}

void PhasePanel::setExpanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;
    m_head->setProperty("expanded", expanded);
    m_chevron->setProperty("expanded", expanded);
    m_chevron->setPixmap(iconPixmap(expanded ? QStringLiteral("chevron-down")
                                             : QStringLiteral("chevron-up"),
                                    expanded ? gs::palette().cyan
                                             : gs::palette().muted2, 10));
    restyle(m_head);
    restyle(m_chevron);
    m_steps->setVisible(expanded);
    layoutProgress();
}

void PhasePanel::layoutProgress()
{
    m_progress->setGeometry(0, m_head->height() - 1, m_head->width(), 2);
    m_progress->raise();
}

bool PhasePanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_head) {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton)
                setExpanded(!m_expanded);
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
                setExpanded(!m_expanded);
                return true;
            }
        }
        if (event->type() == QEvent::Resize)
            layoutProgress();
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace gs

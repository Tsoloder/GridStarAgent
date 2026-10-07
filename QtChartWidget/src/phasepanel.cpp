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

// ----------------------------------------------------------- PhaseCountLabel

PhaseCountLabel::PhaseCountLabel(QWidget *parent) : QLabel(parent)
{
    setTextFormat(Qt::RichText);
    // 宽可以在 sizeHint 之下压（约束来自外层布局的 max-width 语义），高固定
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumWidth(0);
}

void PhaseCountLabel::setSegments(const QList<QPair<QString, QColor>> &segments)
{
    m_segments = segments;
    if (m_segments.isEmpty())
        m_segments.append(qMakePair(QStringLiteral("0 已完成"), gs::palette().muted2));
    m_rich = QString();
    m_plain = QString();
    for (int i = 0; i < m_segments.size(); ++i) {
        if (i)
            m_plain += QStringLiteral(" · ");
        m_plain += m_segments.at(i).first;
    }
    rebuild();
}

void PhaseCountLabel::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    rebuild();
}

// CSS 里这一段是 flex:0 1 auto + ellipsis：宽度不够就从尾部丢段，至少留第一段
void PhaseCountLabel::rebuild()
{
    if (m_segments.isEmpty())
        return;
    const QFontMetrics fm(font());
    int count = m_segments.size();
    while (count > 1) {
        QString plain;
        for (int i = 0; i < count; ++i) {
            if (i)
                plain += QStringLiteral(" · ");
            plain += m_segments.at(i).first;
        }
        if (fm.horizontalAdvance(plain) <= width())
            break;
        --count;
    }
    QString rich;
    for (int i = 0; i < count; ++i) {
        if (i)
            rich += QStringLiteral(" · ");
        rich += QStringLiteral("<span style=\"color:%1\">%2</span>")
                    .arg(cssColor(m_segments.at(i).second), m_segments.at(i).first);
    }
    if (m_rich == rich)
        return;
    m_rich = rich;
    setText(rich);
}

// ------------------------------------------------------------------ PhaseStep

PhaseStep::PhaseStep(const QString &title, const QString &note, const QString &status,
                     QWidget *parent)
    : QWidget(parent), m_title(title), m_note(note), m_status(status)
{
    setAttribute(Qt::WA_Hover, true);
    setFixedHeight(30); // CSS: padding 6px 8px + 14px 行高
    if (!note.isEmpty())
        setToolTip(note);
    else if (!title.isEmpty())
        setToolTip(title);

    // 运行态指示器：10px 开口环（.phase-step.running:before），非运行态隐藏
    m_pulse = new PulseDot(this);
    m_pulse->setStyle(PulseDot::Spin);
    m_pulse->setRing(10, 1.5);
    m_pulse->setColor(gs::palette().cyan);
    m_pulse->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_pulse->move(8, 6 + (18 - 10) / 2);
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
    QColor dot = pal.muted2; // 待处理：灰点 .55 透明
    qreal dotOpacity = 0.55;
    if (done) {
        text = pal.okText;
        dot = pal.green;
        dotOpacity = 1.0;
    } else if (running) {
        // 运行态：指示器换成 10px 开口环（PulseDot 子控件），本体不画点
        text = pal.textBright;
    } else if (failed) {
        text = pal.errText;
        dot = pal.red;
        dotOpacity = 1.0;
    } else if (skipped) {
        // 跳过：虚线空心点，且整行 55% 透明
        dot = pal.muted2;
        dotOpacity = 1.0;
    }

    p.setOpacity(skipped ? 0.55 : 1.0); // CSS .phase-step.skipped { opacity: .55 }

    // 背景：当前执行项整行铺背景色（不再画左侧竖条）；hover 同样给淡底
    QPainterPath bg;
    bg.addRoundedRect(QRectF(0.5, 0.5, width() - 1.0, height() - 1.0), 4, 4);
    if (running)
        p.fillPath(bg, pal.accentTint);
    else if (m_hover)
        p.fillPath(bg, pal.accentTint);

    // 状态点 8×8 @ (8, 占位行上下居中后 +rowTop)；竖线中心与点中心对齐
    const qreal rowTop = (height() - 18) / 2.0; // CSS: padding 6px，行高 18px
    const QPointF dotCenter(8 + 4, rowTop + 9);
    const int lineX = qRound(dotCenter.x()) - 10; // CSS 里竖线 left:11.5px，比圆心还靠左

    // 步骤间连接线：从点下方 2px 一直连到下一条（CSS left:11.5px; top:17px; bottom:-6px）
    p.setPen(Qt::NoPen);
    p.setBrush(pal.line);
    if (m_connectorTop)
        p.drawRect(QRectF(lineX, 0, 1, qMax(0.0, dotCenter.y() - 1)));
    if (m_connectorBottom)
        p.drawRect(QRectF(lineX, dotCenter.y() + 1, 1,
                          qMax(0.0, height() - dotCenter.y() - 1)));

    if (running) {
        // 环由子控件旋转绘制，这里只留背景
    } else {
        QColor dotColor = dot;
        dotColor.setAlphaF(dotColor.alphaF() * dotOpacity);
        if (skipped) {
            // CSS .phase-step.skipped:before { background:transparent; border:1px dashed var(--muted-2) }
            QPen skipPen(dotColor);
            skipPen.setWidthF(1);
            skipPen.setStyle(Qt::DashLine);
            p.setPen(skipPen);
            p.setBrush(Qt::NoBrush);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(dotColor);
        }
        p.drawEllipse(dotCenter, 4.0, 4.0);
    }

    // 标题（11px, weight 500）+ 备注（10px, 55% 透明度）
    const int textLeft = qRound(dotCenter.x()) + 4 + 9; // 点右缘 + CSS margin-right 9px
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
    m_count = new PhaseCountLabel(m_head);
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

// 标题后的统计串：已完成 / 进行中 / 待处理 / 失败 / 跳过，只列非零项（app.js phaseStats）
static QList<QPair<QString, QColor>> phaseStatsSegments(const QVariantList &phases)
{
    int ok = 0, run = 0, wait = 0, fail = 0, skip = 0;
    for (const QVariant &item : phases) {
        QString st = item.toMap().value(QStringLiteral("status")).toString();
        if (st.isEmpty())
            st = QStringLiteral("pending");
        if (st == QLatin1String("skipped"))
            ++skip;
        else if (st == QLatin1String("done") || st == QLatin1String("succeeded")
                 || st == QLatin1String("completed"))
            ++ok;
        else if (st == QLatin1String("active") || st == QLatin1String("running")
                 || st == QLatin1String("in_progress"))
            ++run;
        else if (st == QLatin1String("failed") || st == QLatin1String("error")
                 || st == QLatin1String("cancelled"))
            ++fail;
        else
            ++wait;
    }

    const Palette &pal = gs::palette();
    QList<QPair<QString, QColor>> segs;
    const auto push = [&segs](int count, const QString &label, const QColor &color) {
        if (count > 0)
            segs.append(qMakePair(QStringLiteral("%1 %2").arg(count).arg(label), color));
    };
    push(ok, QStringLiteral("已完成"), pal.green);   // .st-ok
    push(run, QStringLiteral("进行中"), pal.cyan);   // .st-run
    push(wait, QStringLiteral("待处理"), pal.muted); // .st-wait
    push(fail, QStringLiteral("失败"), pal.red);     // .st-fail
    push(skip, QStringLiteral("跳过"), pal.muted2);  // .st-skip
    return segs;
}

// 清空步骤区：先摘掉父子关系再 deleteLater，否则旧行在事件循环处理前仍挂在
// 面板的对象树上（"面板已空"这种断言会看到残影）
void PhasePanel::clearSteps()
{
    while (QLayoutItem *item = m_innerLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }
    m_innerLayout->addStretch(1);
}

void PhasePanel::clearPlan()
{
    // 计划窗口收起时连内容一起清掉：留着旧行会让下一条无计划的消息仍显示面板
    m_title->setText(QStringLiteral("阶段计划"));
    m_count->setSegments({});
    clearSteps();
    m_progress->setPercent(0);
    setVisible(false);
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
    // 原分数徽标已被统计串取代（dfbb564）：N 已完成 · N 进行中 · N 待处理
    m_count->setSegments(phaseStatsSegments(phases));
    m_progress->animateTo(qRound(completed * 100.0 / phases.size()));

    clearSteps();
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

#ifndef GS_COMMONWIDGETS_H
#define GS_COMMONWIDGETS_H

#include <QColor>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QVariant>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QPropertyAnimation;
class QResizeEvent;
class QTimer;
QT_END_NAMESPACE

namespace gs {

// 流式布局：对应 CSS 的 flex-wrap（附件条、选项按钮、能力勾选、代码芯片）
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent = nullptr, int margin = 0, int hSpacing = 6, int vSpacing = 6);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;
    QLayoutItem *takeAt(int index) override;

private:
    int doLayout(const QRect &rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;

    QList<QLayoutItem *> m_items;
    int m_hSpace;
    int m_vSpace;
};

// CSS 里用旋转方框边框画的折叠箭头（.tool-chevron / .phase-chevron）
class Chevron : public QWidget
{
    Q_OBJECT
public:
    explicit Chevron(QWidget *parent = nullptr);
    void setOpen(bool open);
    bool isOpen() const { return m_open; }
    QSize sizeHint() const override { return QSize(10, 10); }
    QSize minimumSizeHint() const override { return QSize(10, 10); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    bool m_open = false;
};

// 工具项状态圆点（.tool-dot）：执行中橙色带光晕，成功绿色，失败红色
class StatusDot : public QWidget
{
    Q_OBJECT
public:
    explicit StatusDot(QWidget *parent = nullptr);
    void setState(const QString &state);
    QString state() const { return m_state; }
    QSize sizeHint() const override { return QSize(8, 8); }
    QSize minimumSizeHint() const override { return QSize(8, 8); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_state = QStringLiteral("running");
};

// 顶栏连接状态按钮（.connection）：自绘发光圆点 + 文本
class ConnectionButton : public QPushButton
{
    Q_OBJECT
public:
    explicit ConnectionButton(QWidget *parent = nullptr);
    void setState(const QString &state, const QString &label);
    QString state() const { return m_state; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_state = QStringLiteral("checking");
};

// 下拉触发器（.session-trigger / .model-combobox>button）：文本 + ⌄，文本按宽度省略
class ComboTrigger : public QWidget
{
    Q_OBJECT
public:
    explicit ComboTrigger(QWidget *parent = nullptr);
    void setText(const QString &text);
    QString text() const { return m_full; }
    void setOpen(bool open);
    void setRightAligned(bool right);
    // 界面缩放系数变化后：重建 chevron 图标并按新字号重新省略文本
    void refreshZoom();

signals:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override;

private:
    void updateElided();

    QLabel *m_text = nullptr;
    QLabel *m_chevron = nullptr;
    QString m_full;
    bool m_right = false;
    bool m_open = false;
};

// 阶段面板底部 2px 渐变进度条（.phase-progress）
class ProgressLine : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(int animatedPercent READ percent WRITE setAnimatedPercent)
public:
    explicit ProgressLine(QWidget *parent = nullptr);
    void setPercent(int percent);
    // 带过渡地推进（webui .phase-progress { transition: width .45s ease }）
    void animateTo(int percent);
    int percent() const { return m_percent; }
    QSize sizeHint() const override { return QSize(10, 2); }
    QSize minimumSizeHint() const override { return QSize(10, 2); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void setAnimatedPercent(int percent);

    int m_percent = 0;
    QPropertyAnimation *m_animation = nullptr;
};

// 单行省略标签：QLabel 没有 text-overflow，长文本在窄屏下只会硬切。
// sizeHint 按全文宽度给（布局知道"想多宽"），minimumSizeHint 为 0（可压到任意窄）。
class ElidedLabel : public QLabel
{
    Q_OBJECT
public:
    explicit ElidedLabel(QWidget *parent = nullptr);
    // 原文：省略只作用于显示，toolTip 保留完整内容
    void setFullText(const QString &text);
    QString fullText() const { return m_full; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateElide();
    QString m_full;
};

// 附件芯片（.attach-chip）：图片给 22×22 缩略图，其余给扩展名标签；
// 上传中大小位显示「上传中…」（与 webui renderAttachments 同构）
class AttachChip : public QFrame
{
    Q_OBJECT
public:
    // item 与 attachments 的条目同构：{name, size, ext, uploading, kind, path[, url]}
    explicit AttachChip(const QVariantMap &item, QWidget *parent = nullptr);
    void setUploading(bool uploading);

signals:
    void removeClicked();

private:
    QLabel *m_name = nullptr;
    QLabel *m_size = nullptr;
    qint64 m_bytes = 0;
};

// SVG 图标按钮：图标经 iconPixmap 着色后设置，hover/禁用态自动换色。
// QSS 的 color 不作用于图标，颜色需通过 setIconColors 显式指定（与 theme.cpp 的 QSS 保持一致）。
class IconPushButton : public QPushButton
{
    Q_OBJECT
public:
    explicit IconPushButton(QWidget *parent = nullptr);
    // name 为 :/icons/ 下的资源名（不含 .svg），如 "settings"
    void setIconName(const QString &name, int sizePx = 14);
    void setIconColors(const QColor &normal, const QColor &hover,
                       const QColor &disabled = QColor());
    void refreshIcon();

protected:
    bool event(QEvent *event) override;

private:
    QString m_name;
    int m_sizePx = 14;
    QColor m_normal;
    QColor m_hover;
    QColor m_disabled;
    bool m_hovering = false;
};

// 呼吸圆点（.run-dot / 阶段项 active 指示）：运行态下透明度往复动画
class PulseDot : public QWidget
{
    Q_OBJECT
public:
    explicit PulseDot(QWidget *parent = nullptr);
    void setColor(const QColor &color);
    void setActive(bool active);
    bool isActive() const { return m_active; }
    QSize sizeHint() const override { return QSize(6, 6); }
    QSize minimumSizeHint() const override { return QSize(6, 6); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QColor m_color;
    bool m_active = false;
    qreal m_phase = 0.0;
    QTimer *m_timer = nullptr;
};

// 皮肤圆形色片（.theme-swatch / .theme-option .sw）：135° 双色渐变
class ThemeSwatch : public QWidget
{
    Q_OBJECT
public:
    ThemeSwatch(const QString &themeId, int sizePx = 12, bool withBorder = true,
                QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(m_size, m_size); }
    QSize minimumSizeHint() const override { return QSize(m_size, m_size); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_themeId;
    int m_size = 12;
    bool m_border = true;
};

// 轮次导航轨上的一个点（.turn-rail-dot）：12px 命中区 + 5px 圆点，悬浮/当前态提亮放大
class TurnRailDot : public QWidget
{
    Q_OBJECT
public:
    explicit TurnRailDot(int turn, QWidget *parent = nullptr);
    int turn() const { return m_turn; }
    void setActive(bool active);
    bool isActive() const { return m_active; }
    QSize sizeHint() const override { return QSize(12, 12); }
    QSize minimumSizeHint() const override { return QSize(12, 12); }

signals:
    void activated(int turn);
    void hovered(int turn);
    void unhovered();

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    int m_turn = 0;
    bool m_active = false;
    bool m_hover = false;
};

// 轨迹行内耗时条（.traj-row-bar）：带边框的胶囊轨道 + 按比例填充
class MiniBar : public QWidget
{
    Q_OBJECT
public:
    explicit MiniBar(QWidget *parent = nullptr);
    // ratio < 0 表示该记录无耗时：只画空轨道，不臆造时长
    void setRatio(qreal ratio);
    void setFill(const QColor &color);
    QSize sizeHint() const override { return QSize(90, 7); }
    QSize minimumSizeHint() const override { return QSize(40, 7); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    qreal m_ratio = -1.0;
    QColor m_fill;
};

// 工具函数
// 加载 :/icons/<name>.svg，按 color 着色并缩放到 sizePx（内部缓存 64px 母版）
QPixmap iconPixmap(const QString &name, const QColor &color, int sizePx);
QString elidedText(const QString &text, const QFontMetrics &fm, int width);
QString fileSizeLabel(qint64 bytes);
// QSS 的 .class 选择器实际匹配动态属性 "class"（空格分隔可多个），并非 objectName
void setClass(QWidget *w, const QString &cls);
QLabel *makeLabel(const QString &className, const QString &text, QWidget *parent = nullptr);

} // namespace gs

#endif // GS_COMMONWIDGETS_H

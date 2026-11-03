#include "settingsdialog.h"

#include "commonwidgets.h"
#include "popups.h"
#include "theme.h"
#include "usagepanel.h"
#include "memorypanel.h"
#include "kbpanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopWidget>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSet>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <functional>

namespace gs {

namespace {

QWidget *styledWidget(const QString &cls, QWidget *parent = nullptr)
{
    auto *w = new QWidget(parent);
    if (!cls.isEmpty())
        setClass(w, cls);
    w->setAttribute(Qt::WA_StyledBackground, true);
    return w;
}

QFrame *styledFrame(const QString &cls, QWidget *parent = nullptr)
{
    auto *f = new QFrame(parent);
    if (!cls.isEmpty())
        setClass(f, cls);
    f->setAttribute(Qt::WA_StyledBackground, true);
    f->setFrameShape(QFrame::NoFrame);
    return f;
}

QPushButton *styledButton(const QString &text, const QString &variant, QWidget *parent = nullptr)
{
    auto *b = new QPushButton(text, parent);
    if (!variant.isEmpty())
        b->setProperty("variant", variant);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::TabFocus); // webui 的 button 可聚焦；TabFocus 不留点击焦点环
    return b;
}

// makeLabel 默认允许选中文本，会吃掉点击；可点击行内的标签要关掉
QLabel *plainLabel(const QString &cls, const QString &text, QWidget *parent = nullptr)
{
    QLabel *l = makeLabel(cls, text, parent);
    l->setTextInteractionFlags(Qt::NoTextInteraction);
    return l;
}

QLineEdit *settingsInput(const QString &placeholder)
{
    auto *e = new QLineEdit;
    setClass(e, QStringLiteral("settingsInput"));
    e->setPlaceholderText(placeholder);
    e->setFixedHeight(32);
    return e;
}

QComboBox *settingsCombo()
{
    auto *c = new QComboBox;
    setClass(c, QStringLiteral("settingsInput"));
    c->setFixedHeight(32);
    c->setFocusPolicy(Qt::TabFocus); // 下拉框也要能 Tab 到（webui 的 select 是可聚焦元素）
    return c;
}

void selectComboData(QComboBox *combo, const QString &value)
{
    const int index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

QString comboData(QComboBox *combo)
{
    return combo->currentData().toString();
}

// .settings-field：小标题在上、输入控件在下
QWidget *fieldRow(const QString &caption, QWidget *input)
{
    auto *row = new QWidget;
    auto *l = new QVBoxLayout(row);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(5);
    l->addWidget(plainLabel(QStringLiteral("fieldCaption"), caption));
    l->addWidget(input);
    return row;
}

// .form-grid：宽屏每行两个字段，@media(max-width:640px) 时变单列
// 条件字段（如自定义 CA 路径）藏起来时不占位，后面的字段顺势前移。
// 判据是"已经在某个父级里、又被显式藏了"——刚 new 出来还没挂父级的字段
// isHidden() 也是真，只认它会把整张表单都跳过去
static bool formFieldHidden(QWidget *field)
{
    return field->parentWidget() && field->isHidden();
}

void placeFormFields(QGridLayout *grid, const QList<QWidget *> &fields, bool compact)
{
    int slot = 0;
    for (QWidget *field : fields) {
        if (grid->indexOf(field) >= 0)
            grid->removeWidget(field);
        if (formFieldHidden(field))
            continue;
        if (compact) {
            grid->addWidget(field, slot, 0);
            ++slot;
        } else {
            grid->addWidget(field, slot / 2, slot % 2);
            ++slot;
        }
    }
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, compact ? 0 : 1);
}

QScrollArea *plainScroll(QWidget *content, const QString &objectName = QString(),
                         const QString &cls = QString())
{
    auto *area = new QScrollArea;
    if (!objectName.isEmpty())
        area->setObjectName(objectName);
    if (!cls.isEmpty())
        setClass(area, cls);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidgetResizable(true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->viewport()->setAutoFillBackground(false);
    area->setWidget(content);
    return area;
}

void clearLayout(QLayout *layout)
{
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        else if (QLayout *child = item->layout()) {
            clearLayout(child);
            delete child;
        }
        delete item;
    }
}

// app.js schemaParams()：从 JSON Schema 抽取入参摘要
QVariantList schemaParams(const QVariantMap &schema)
{
    QVariantList out;
    const QVariantMap props = schema.value(QStringLiteral("properties")).toMap();
    const QVariantList required = schema.value(QStringLiteral("required")).toList();
    const QStringList names = props.keys();
    for (const QString &name : names) {
        const QVariantMap p = props.value(name).toMap();
        QString type = p.value(QStringLiteral("type")).toString();
        if (type.isEmpty())
            type = QStringLiteral("any");
        QVariantMap item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("required"), required.contains(QVariant(name)));
        item.insert(QStringLiteral("description"),
                    p.value(QStringLiteral("description")).toString());
        out.append(item);
    }
    return out;
}

QLabel *emptyHint(const QString &text)
{
    QLabel *label = makeLabel(QStringLiteral("listboxEmpty"), text);
    label->setAlignment(Qt::AlignCenter);
    return label;
}

// ---- 模型页：能力项 / 数值预设 / 格式化 --------------------------------------
// 规格以 webui/app.js 的 HEAD 为准（CAPABILITIES / formatTokens / applyModelLimits /
// numberField / renderModelCard 一带），不要按旧提交的记忆改。

struct CapabilityDef
{
    const char *key;   // model.capabilities 里的键
    const char *label; // 界面文案
    const char *icon;  // :/icons/<icon>.svg
};

// app.js CAPABILITIES：界面只摆这四项中文
const CapabilityDef kModelCapabilities[4] = {
    {"vision", "视觉", "cap-vision"},
    {"video", "视频", "cap-video"},
    {"audio", "音频", "cap-audio"},
    {"reasoning", "推理", "cap-reasoning"},
};

// 后端 schema 里的旧键：界面不展示，但新模型对象里仍要摆上 false，兼容存量 config.json
const char *const kLegacyCapabilityKeys[] = {"tools", "parallel_tools", "stream_usage"};

// 数值字段的预设档位：(原值, 档位名)
typedef QList<QPair<int, QString> > NumPresets;

// app.js CONTEXT_PRESETS / OUTPUT_PRESETS。档位名是约定叫法（64K 对应 65536，
// 不是 formatTokens 算出来的 65.5K），所以写死配对。
NumPresets contextPresets()
{
    NumPresets list;
    list << qMakePair(65536, QStringLiteral("64K"))
         << qMakePair(131072, QStringLiteral("128K"))
         << qMakePair(200000, QStringLiteral("200K"))
         << qMakePair(262144, QStringLiteral("256K"))
         << qMakePair(1048576, QStringLiteral("1M"));
    return list;
}

NumPresets outputPresets()
{
    NumPresets list;
    list << qMakePair(8192, QStringLiteral("8K"))
         << qMakePair(16384, QStringLiteral("16K"))
         << qMakePair(32768, QStringLiteral("32K"))
         << qMakePair(65536, QStringLiteral("64K"))
         << qMakePair(131072, QStringLiteral("128K"))
         << qMakePair(262144, QStringLiteral("256K"));
    return list;
}

// app.js formatTokens：32K / 1M。按十进制量级切档，1,000,000 就是 1M；
// 拿 1024 进制去切会把 1M 窗口显示成 976.6K。
QString formatTokens(qint64 value)
{
    if (value >= 1000000)
        return QStringLiteral("%1M").arg(QString::number(qRound(value / 100000.0) / 10.0));
    if (value >= 1000)
        return QStringLiteral("%1K").arg(QString::number(qRound(value / 100.0) / 10.0));
    return QString::number(value);
}

// Number(value).toLocaleString("en-US")
QString formatCount(qint64 value)
{
    return QLocale(QLocale::English, QLocale::UnitedStates).toString(value);
}

// app.js formatMetaDate：2026-10-30 → 2026年10月30日
QString formatMetaDate(const QString &value)
{
    const QStringList parts = value.split(QLatin1Char('-'));
    if (parts.size() != 3)
        return value;
    return QStringLiteral("%1年%2月%3日")
        .arg(parts.at(0))
        .arg(parts.at(1).toInt())
        .arg(parts.at(2).toInt());
}

// 「provider/id」：能力项的「用户改过」记忆按它记
QString capKeyOf(const QVariantMap &model)
{
    return model.value(QStringLiteral("provider")).toString() + QLatin1Char('/')
        + model.value(QStringLiteral("id")).toString();
}

// webui 把「用户改过哪些模型的能力项」记在 localStorage：
// 否则用户取消勾选后，下次打开设置又会被元数据自动勾回去。这里进程内记一份。
QSet<QString> &capsTouchedSet()
{
    static QSet<QString> touched;
    return touched;
}

// app.js applyModelCapabilities：只把数据里说支持的勾上，从不反向取消；
// 用户手动改过的模型不碰。
bool applyModelCapabilities(QVariantMap &model, const QVariantMap &caps)
{
    if (caps.isEmpty())
        return false;
    if (capsTouchedSet().contains(capKeyOf(model)))
        return false;
    QVariantMap capabilities = model.value(QStringLiteral("capabilities")).toMap();
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        const QString key = QLatin1String(kModelCapabilities[i].key);
        if (caps.value(key).toBool() && !capabilities.value(key).toBool()) {
            capabilities.insert(key, true);
            changed = true;
        }
    }
    if (changed)
        model.insert(QStringLiteral("capabilities"), capabilities);
    return changed;
}

// app.js applyModelLimits：上限只填空字段；最大输出不得超过上下文窗口。
bool applyModelLimits(QVariantMap &model, const QVariantMap &limits)
{
    if (limits.isEmpty())
        return false;
    bool changed = false;
    const int context = limits.value(QStringLiteral("context_window")).toInt();
    const int output = limits.value(QStringLiteral("max_output_tokens")).toInt();
    if (model.value(QStringLiteral("context_window")).toInt() <= 0 && context > 0) {
        model.insert(QStringLiteral("context_window"), context);
        changed = true;
    }
    if (model.value(QStringLiteral("max_output_tokens")).toInt() <= 0 && output > 0) {
        const int own = model.value(QStringLiteral("context_window")).toInt();
        const int window = own > 0 ? own : context;
        if (window > 0) {
            model.insert(QStringLiteral("max_output_tokens"), qMin(output, window));
            changed = true;
        }
    }
    if (applyModelCapabilities(model, limits.value(QStringLiteral("capabilities")).toMap()))
        changed = true;
    return changed;
}

// app.js autoLimit：数值框的占位文案
QString autoLimitText(const QVariantMap &limits, const QString &field)
{
    const int value = limits.value(field).toInt();
    return value > 0 ? QStringLiteral("自动 · %1").arg(formatTokens(value))
                     : QStringLiteral("保存时自动识别");
}

void unrestrictWidth(QWidget *w)
{
    w->setMinimumWidth(0);
    w->setMaximumWidth(QWIDGETSIZE_MAX);
}

// .settings-field：左列标签（主标题 + 一行小字说明），右列控件；
// @media(max-width:640px) 时改成上下叠（标签在上、控件在下）
class SettingsField : public QWidget
{
public:
    SettingsField(const QString &label, const QString &hint, QWidget *control,
                  QWidget *parent = nullptr)
        : QWidget(parent), m_control(control)
    {
        m_grid = new QGridLayout(this);
        m_grid->setContentsMargins(0, 0, 0, 0);
        m_grid->setHorizontalSpacing(gs::scaledPx(14));
        m_grid->setVerticalSpacing(gs::scaledPx(6));

        m_text = new QWidget(this);
        auto *box = new QVBoxLayout(m_text);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(gs::scaledPx(3));
        if (!label.isEmpty())
            box->addWidget(plainLabel(QStringLiteral("fieldLabel"), label));
        if (!hint.isEmpty()) {
            QLabel *hintLabel = plainLabel(QStringLiteral("fieldHint"), hint);
            hintLabel->setWordWrap(true);
            box->addWidget(hintLabel);
        }

        control->setParent(this);
        control->setMaximumWidth(gs::scaledPx(460)); // .settings-field>input{max-width:460px}
        place(false);
    }

    void setCompact(bool compact)
    {
        if (m_compact == compact)
            return;
        place(compact);
    }

private:
    void place(bool compact)
    {
        m_compact = compact;
        if (m_grid->indexOf(m_text) >= 0)
            m_grid->removeWidget(m_text);
        if (m_grid->indexOf(m_control) >= 0)
            m_grid->removeWidget(m_control);
        if (compact) {
            unrestrictWidth(m_text);
            unrestrictWidth(m_control);
            m_grid->addWidget(m_text, 0, 0, 1, 2);
            m_grid->addWidget(m_control, 1, 0, 1, 2);
            m_grid->setColumnStretch(0, 1);
            m_grid->setColumnStretch(1, 0);
        } else {
            // .settings-field{grid-template-columns:minmax(96px,104px) minmax(0,1fr)}
            m_text->setFixedWidth(gs::scaledPx(104));
            m_control->setMaximumWidth(gs::scaledPx(460));
            m_grid->addWidget(m_text, 0, 0, 1, 1, Qt::AlignVCenter);
            m_grid->addWidget(m_control, 0, 1, 1, 1, Qt::AlignVCenter);
            m_grid->setColumnStretch(0, 0);
            m_grid->setColumnStretch(1, 1);
        }
    }

    QGridLayout *m_grid = nullptr;
    QWidget *m_text = nullptr;
    QWidget *m_control = nullptr;
    bool m_compact = false;
};

// .settings-toggle 里的开关：34×20 胶囊 + 14px 圆钮（checked 时青底白钮）
class SwitchToggle : public QWidget
{
public:
    explicit SwitchToggle(QWidget *parent = nullptr) : QWidget(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setFixedSize(sizeHint());
        m_anim = new QVariantAnimation(this);
        m_anim->setDuration(160); // transition:background .16s ease
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_offset = value.toReal();
            update();
        });
    }

    // 只回填外观，不触发 onToggled
    void setChecked(bool checked)
    {
        m_checked = checked;
        m_offset = checked ? 1.0 : 0.0;
        update();
    }

    bool isChecked() const { return m_checked; }

    std::function<void(bool)> onToggled;

    QSize sizeHint() const override
    {
        return QSize(gs::scaledPx(34), gs::scaledPx(20));
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Palette &p = gs::palette();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal radius = height() / 2.0;
        const QRectF track = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const QColor border = m_checked ? p.cyan : (m_hover ? p.muted : p.lineStrong);
        painter.setPen(QPen(border, 1.0));
        painter.setBrush(m_checked ? p.cyan : p.surface3);
        painter.drawRoundedRect(track, radius, radius);

        const qreal knob = qMax<qreal>(gs::scaledPx(10), track.height() - gs::scaledPx(6));
        const qreal inset = (track.height() - knob) / 2.0;
        const qreal travel = track.width() - knob - inset * 2.0;
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_checked ? QColor(Qt::white) : p.muted);
        painter.drawEllipse(QRectF(track.left() + inset + travel * m_offset,
                                   track.top() + inset, knob, knob));
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (rect().contains(event->pos()))
            toggle();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter) {
            toggle();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void enterEvent(QEvent *event) override
    {
        m_hover = true;
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_hover = false;
        update();
        QWidget::leaveEvent(event);
    }

private:
    void toggle()
    {
        m_checked = !m_checked;
        m_anim->stop();
        m_anim->setStartValue(m_offset);
        m_anim->setEndValue(m_checked ? 1.0 : 0.0);
        m_anim->start();
        if (onToggled)
            onToggled(m_checked);
    }

    QVariantAnimation *m_anim = nullptr;
    bool m_checked = false;
    bool m_hover = false;
    qreal m_offset = 0.0;
};

// .num-listbox：预设档位面板。用 Qt::Popup 弹出：点外面自动收起，也不被滚动区裁掉
class NumPanel : public QFrame
{
public:
    explicit NumPanel(QWidget *parent) : QFrame(parent, Qt::Popup)
    {
        setClass(this, QStringLiteral("numListbox"));
        setAttribute(Qt::WA_StyledBackground, true);
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(5, 5, 5, 5);
        m_layout->setSpacing(0);
    }

    void setPresets(const NumPresets &presets)
    {
        while (QLayoutItem *item = m_layout->takeAt(0)) {
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        m_options.clear();
        for (int i = 0; i < presets.size(); ++i) {
            const int value = presets.at(i).first;
            auto *button = new QPushButton(this);
            setClass(button, QStringLiteral("numOption"));
            button->setProperty("value", value);
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::TabFocus);
            button->setMinimumWidth(gs::scaledPx(180));
            auto *box = new QHBoxLayout(button);
            box->setContentsMargins(gs::scaledPx(8), gs::scaledPx(7), gs::scaledPx(8),
                                    gs::scaledPx(7));
            box->setSpacing(gs::scaledPx(16));
            auto *name = plainLabel(QStringLiteral("numOptionText"), presets.at(i).second);
            auto *count = plainLabel(QStringLiteral("numOptionValue"), formatCount(value));
            count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            box->addWidget(name, 1);
            box->addWidget(count, 0);
            connect(button, &QPushButton::clicked, this, [this, value] {
                hide();
                if (onChosen)
                    onChosen(value);
            });
            m_layout->addWidget(button);
            m_options.append(button);
        }
    }

    // 当前值等于某预设时该项选中
    void setCurrent(int value)
    {
        for (int i = 0; i < m_options.size(); ++i) {
            QPushButton *button = m_options.at(i);
            const bool selected = button->property("value").toInt() == value;
            if (button->property("selected").toBool() == selected)
                continue;
            button->setProperty("selected", selected);
            gs::restyle(button);
            const QList<QLabel *> labels = button->findChildren<QLabel *>();
            for (QLabel *label : labels) {
                const bool isName = label->property("class").toString()
                                        == QLatin1String("numOptionText");
                if (isName)
                    label->setStyleSheet(QStringLiteral("color:%1;")
                                             .arg(gs::cssColor(selected
                                                                   ? gs::palette().cyan
                                                                   : gs::palette().text)));
            }
        }
    }

    std::function<void(int)> onChosen;
    std::function<void()> onClosed;

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            hide();
            return;
        }
        QFrame::keyPressEvent(event);
    }

    void hideEvent(QHideEvent *event) override
    {
        QFrame::hideEvent(event);
        if (onClosed)
            onClosed();
    }

private:
    QVBoxLayout *m_layout = nullptr;
    QList<QPushButton *> m_options;
};

// .num-combobox：可手填的数值框 + 右侧 chevron 预设面板
class NumField : public QWidget
{
public:
    explicit NumField(const NumPresets &presets, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setClass(this, QStringLiteral("numCombo"));
        setAttribute(Qt::WA_StyledBackground, true);
        setFixedHeight(gs::scaledPx(34));

        auto *box = new QHBoxLayout(this);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(0);

        m_input = new QLineEdit(this);
        setClass(m_input, QStringLiteral("numComboInput"));
        m_input->setValidator(new QIntValidator(1, 100000000, m_input));
        m_input->setFrame(false);
        m_input->installEventFilter(this);
        box->addWidget(m_input, 1);

        m_toggle = new IconPushButton(this);
        setClass(m_toggle, QStringLiteral("numToggle"));
        m_toggle->setFixedWidth(gs::scaledPx(34));
        m_toggle->setCursor(Qt::PointingHandCursor);
        m_toggle->setFocusPolicy(Qt::TabFocus);
        m_toggle->setIconColors(gs::palette().muted2, gs::palette().cyan);
        m_toggle->setIconName(QStringLiteral("chevron-down"), 12);
        box->addWidget(m_toggle, 0);

        m_panel = new NumPanel(this);
        m_panel->setPresets(presets);
        // 面板是独立的弹出窗口，不保险能拿到设置窗口那张局部样式表，这里再给它一份
        // （配色仍取 gs::palette()；卡片每次重渲染都会重建，不会留旧皮肤的色）
        const Palette &pal = gs::palette();
        m_panel->setStyleSheet(
            QStringLiteral("QFrame.numListbox { border: 1px solid %1; border-radius: 4px;"
                           " background: %2; }"
                           "QPushButton.numOption { border: 1px solid transparent;"
                           " border-radius: 4px; background: transparent; }"
                           "QPushButton.numOption:hover { border-color: %3; background: %4; }"
                           "QPushButton.numOption[selected=\"true\"] { border-color: %3; }"
                           "QLabel.numOptionText { color: %5; font-size: %6px; background: transparent; }"
                           "QLabel.numOptionValue { color: %7; font-size: %8px; font-family: %9;"
                           " background: transparent; }")
                .arg(gs::cssColor(pal.lineStrong), gs::cssColor(pal.inset),
                     gs::cssColor(pal.cyanMid), gs::cssColor(pal.accentTint),
                     gs::cssColor(pal.text))
                .arg(gs::scaledPx(12))
                .arg(gs::cssColor(pal.muted2))
                .arg(gs::scaledPx(10))
                .arg(gs::monoFont()));
        m_panel->onChosen = [this](int value) { choose(value); };
        m_panel->onClosed = [this] {
            m_closeTimer.restart();
            m_toggle->setProperty("open", false);
            gs::restyle(m_toggle);
        };

        connect(m_toggle, &QPushButton::clicked, this, [this] { togglePanel(); });
        connect(m_input, &QLineEdit::textChanged, this, [this](const QString &) {
            m_panel->setCurrent(value());
        });
    }

    QLineEdit *input() const { return m_input; }
    int value() const { return m_input->text().trimmed().toInt(); }

    void setValue(int value)
    {
        m_input->setText(value > 0 ? QString::number(value) : QString());
        m_panel->setCurrent(value);
    }

    void setPlaceholder(const QString &text) { m_input->setPlaceholderText(text); }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_input) {
            if (event->type() == QEvent::KeyPress) {
                auto *key = static_cast<QKeyEvent *>(event);
                if (key->key() == Qt::Key_Down) {
                    if (!m_panel->isVisible())
                        showPanel();
                    return true;
                }
                if (key->key() == Qt::Key_Escape && m_panel->isVisible()) {
                    m_panel->hide();
                    return true;
                }
            } else if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
                setProperty("focus", event->type() == QEvent::FocusIn);
                gs::restyle(this);
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    // 选档 = 手填：写回输入框，让原有的 textChanged 接线拾到
    void choose(int value)
    {
        m_input->setText(QString::number(value));
        m_input->setFocus();
    }

    void togglePanel()
    {
        if (m_panel->isVisible()) {
            m_panel->hide();
            return;
        }
        // 刚被「点外面」收起来的那一下不再重新展开
        if (m_closeTimer.isValid() && m_closeTimer.elapsed() < 180)
            return;
        showPanel();
    }

    void showPanel()
    {
        m_panel->setCurrent(value());
        const QSize hint = m_panel->sizeHint();
        const int width = qMax(m_input->width() + m_toggle->width(), hint.width());
        m_panel->setFixedWidth(width);

        // 可用范围：最近的滚动区视口（行式表单在 .provider-editor 里滚动）
        QRect area;
        for (QWidget *widget = parentWidget(); widget; widget = widget->parentWidget()) {
            if (auto *scroll = qobject_cast<QScrollArea *>(widget)) {
                area = QRect(scroll->viewport()->mapToGlobal(QPoint(0, 0)),
                             scroll->viewport()->size());
                break;
            }
        }
        if (!area.isValid()) {
            QWidget *host = window();
            area = QRect(host->mapToGlobal(QPoint(0, 0)), host->size());
        }

        const QPoint topLeft = m_input->mapToGlobal(QPoint(0, 0));
        const int panelHeight = m_panel->sizeHint().height();
        // 就在面板底边时朝上展开，否则会被滚动区裁掉
        const bool dropUp = topLeft.y() + height() + 5 + panelHeight > area.bottom()
            && topLeft.y() - 5 - panelHeight > area.top();
        const int y = dropUp ? topLeft.y() - panelHeight - 5 : topLeft.y() + height() + 5;
        m_panel->move(topLeft.x(), y);
        m_panel->show();
        m_panel->raise();
        // 样式表要等 show() 之后才 polish，隐藏状态下量到的高度偏小，按定稿高度再贴一次
        if (m_panel->height() != panelHeight)
            m_panel->move(topLeft.x(), dropUp ? topLeft.y() - m_panel->height() - 5
                                              : topLeft.y() + height() + 5);
        m_closeTimer.invalidate();
        m_toggle->setProperty("open", true);
        gs::restyle(m_toggle);
    }

    QLineEdit *m_input = nullptr;
    IconPushButton *m_toggle = nullptr;
    NumPanel *m_panel = nullptr;
    QElapsedTimer m_closeTimer;
};

} // namespace

// ------------------------------------------------------------ ModelRow

// .srow / .srow-2：行式表单的一行。宽屏时左右分列，紧凑模式下并作单列。
// 行间的分割线由 QSS 的 QFrame.srow{border-bottom} 负责（局部样式表在
// SettingsDialog::applyModelStyle 里按当前调色板生成）
class ModelRow : public QFrame
{
public:
    explicit ModelRow(QWidget *parent = nullptr) : QFrame(parent)
    {
        setClass(this, QStringLiteral("srow"));
        setAttribute(Qt::WA_StyledBackground, true);
        setFrameShape(QFrame::NoFrame);
        setMinimumHeight(gs::scaledPx(52)); // .settings-field{min-height:52px}
        m_grid = new QGridLayout(this);
        m_grid->setContentsMargins(0, 0, 0, 0);
        m_grid->setHorizontalSpacing(gs::scaledPx(24)); // column-gap:24px
        m_grid->setVerticalSpacing(gs::scaledPx(6));
    }

    SettingsField *addField(const QString &label, const QString &hint, QWidget *control)
    {
        auto *field = new SettingsField(label, hint, control, this);
        field->setCompact(m_compact);
        m_fields.append(field);
        place();
        return field;
    }

    void setCompact(bool compact)
    {
        if (m_compact == compact)
            return;
        m_compact = compact;
        setMinimumHeight(compact ? 0 : gs::scaledPx(52));
        for (int i = 0; i < m_fields.size(); ++i)
            m_fields.at(i)->setCompact(compact);
        place();
    }

private:
    void place()
    {
        int slot = 0;
        for (int i = 0; i < m_fields.size(); ++i) {
            SettingsField *field = m_fields.at(i);
            if (m_grid->indexOf(field) >= 0)
                m_grid->removeWidget(field);
            if (m_compact)
                m_grid->addWidget(field, slot, 0, 1, 2);
            else
                m_grid->addWidget(field, 0, slot, 1, 1);
            ++slot;
        }
        m_grid->setColumnStretch(0, 1);
        m_grid->setColumnStretch(1, (!m_compact && m_fields.size() > 1) ? 1 : 0);
    }

    QGridLayout *m_grid = nullptr;
    QList<SettingsField *> m_fields;
    bool m_compact = false;
};

// ------------------------------------------------------------ ExpandCard

ExpandCard::ExpandCard(const QString &frameClass, const QString &summaryClass,
                       const QString &bodyClass, QWidget *parent)
    : QFrame(parent)
{
    setClass(this, frameClass);
    setAttribute(Qt::WA_StyledBackground, true);
    setFrameShape(QFrame::NoFrame);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_summary = new QWidget(this);
    setClass(m_summary, summaryClass);
    m_summary->setAttribute(Qt::WA_StyledBackground, true);
    m_summary->setCursor(Qt::PointingHandCursor);
    m_summaryLayout = new QVBoxLayout(m_summary);
    m_summaryLayout->setContentsMargins(0, 0, 0, 0);
    m_summaryLayout->setSpacing(0);
    m_summary->installEventFilter(this);
    layout->addWidget(m_summary);

    m_body = new QWidget(this);
    if (!bodyClass.isEmpty()) {
        setClass(m_body, bodyClass);
        m_body->setAttribute(Qt::WA_StyledBackground, true);
    }
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(0, 0, 0, 0);
    m_bodyLayout->setSpacing(0);
    m_body->setVisible(false);
    layout->addWidget(m_body);
}

void ExpandCard::setOpen(bool open)
{
    if (m_open == open)
        return;
    m_open = open;
    m_body->setVisible(open);
    // QSS 里的展开态（.settings-model[open="true"]{background:var(--band-3)}）
    setProperty("open", open);
    gs::restyle(this);
    emit openChanged(open);
}

bool ExpandCard::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_summary && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (m_summary->rect().contains(me->pos()))
            setOpen(!m_open);
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

// -------------------------------------------------------- SettingsDialog

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("settingsDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    // style.css：.settings-dialog{max-width:920px;max-height:720px}，高度按视口收敛。
    // 下限放宽到 360，否则 <=640 的紧凑媒体查询永远不可达。
    setMinimumSize(360, 360);

    buildUi();
    const QRect avail = QApplication::desktop()->availableGeometry(parent);
    resize(qMin(920, qMax(minimumWidth(), avail.width() - 28)),
           qMin(720, qMax(minimumHeight(), avail.height() - 28)));
    switchTab(QStringLiteral("models"));
    renderSettings();
}

void SettingsDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- .settings-head ----
    auto *head = new QWidget(this);
    head->setObjectName(QStringLiteral("settingsHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(18, 16, 18, 16);
    hl->setSpacing(0);
    m_headLayout = hl;
    auto *titleBox = new QVBoxLayout;
    titleBox->setContentsMargins(0, 0, 0, 0);
    titleBox->setSpacing(2);
    QLabel *eyebrow = plainLabel(QStringLiteral("eyebrow"), QStringLiteral("配置"));
    // .eyebrow 是行内 span，占的是所在行盒（13px 基础字体的 18px），不是 9px 的字形高；
    // .settings-head h2 是 18px 字体 / 行盒 24。QSS 没有 line-height，按 webui 量到的行盒补足，
    // 表头 67 → 76（webui 920 宽视口实测 75）
    eyebrow->setMinimumHeight(18);
    titleBox->addWidget(eyebrow);
    auto *title = plainLabel(QString(), QStringLiteral("设置中心"));
    title->setObjectName(QStringLiteral("settingsTitle"));
    title->setMinimumHeight(24);
    titleBox->addWidget(title);
    hl->addLayout(titleBox, 1);
    auto *closeButton = new IconPushButton(this);
    closeButton->setProperty("variant", QStringLiteral("icon"));
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setFocusPolicy(Qt::TabFocus);
    closeButton->setFixedSize(30, 30);
    closeButton->setIconColors(QColor(QStringLiteral("#d7e2e8")),
                               QColor(QStringLiteral("#50badf")));
    closeButton->setIconName(QStringLiteral("x"), 14);
    hl->addWidget(closeButton, 0, Qt::AlignVCenter);
    root->addWidget(head);
    connect(closeButton, &QPushButton::clicked, this, &SettingsDialog::attemptClose);

    // ---- .settings-tabs ----
    auto *tabs = new QWidget(this);
    tabs->setObjectName(QStringLiteral("settingsTabs"));
    tabs->setAttribute(Qt::WA_StyledBackground, true);
    tabs->setFixedHeight(42);
    auto *tl = new QHBoxLayout(tabs);
    tl->setContentsMargins(18, 0, 18, 0);
    tl->setSpacing(0);
    m_tabsLayout = tl;
    const QString tabNames[6] = {QStringLiteral("模型"), QStringLiteral("技能"),
                                 QStringLiteral("MCP 工具"), QStringLiteral("记忆"),
                                 QStringLiteral("知识库"), QStringLiteral("用量")};
    QPushButton **tabPtrs[6] = {&m_tabModels, &m_tabSkills, &m_tabMcp,
                                &m_tabMemory, &m_tabKb, &m_tabUsage};
    for (int i = 0; i < 6; ++i) {
        auto *button = new QPushButton(tabNames[i], tabs);
        setClass(button, QStringLiteral("settingsTab"));
        button->setFixedHeight(42);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::TabFocus);
        *tabPtrs[i] = button;
        tl->addWidget(button);
    }
    m_tabsSpacer = new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum);
    tl->addSpacerItem(m_tabsSpacer);
    root->addWidget(tabs);
    connect(m_tabModels, &QPushButton::clicked, this,
            [this] { switchTab(QStringLiteral("models")); });
    connect(m_tabSkills, &QPushButton::clicked, this,
            [this] { switchTab(QStringLiteral("skills")); });
    connect(m_tabMcp, &QPushButton::clicked, this, [this] { switchTab(QStringLiteral("mcp")); });
    connect(m_tabMemory, &QPushButton::clicked, this,
            [this] { switchTab(QStringLiteral("memory")); });
    connect(m_tabKb, &QPushButton::clicked, this, [this] { switchTab(QStringLiteral("kb")); });
    connect(m_tabUsage, &QPushButton::clicked, this, [this] { switchTab(QStringLiteral("usage")); });

    // ---- .settings-content ----
    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("settingsContent"));
    m_stack->addWidget(buildModelsPage());
    m_stack->addWidget(buildSkillsPage());
    m_stack->addWidget(buildMcpPage());
    m_stack->addWidget(buildMemoryPage());
    m_stack->addWidget(buildKbPage());
    m_stack->addWidget(buildUsagePage());
    root->addWidget(m_stack, 1);

    // ---- .settings-actions ----
    auto *actions = new QWidget(this);
    actions->setObjectName(QStringLiteral("settingsActions"));
    actions->setAttribute(Qt::WA_StyledBackground, true);
    actions->setMinimumHeight(56);
    auto *al = new QHBoxLayout(actions);
    al->setContentsMargins(18, 10, 18, 10);
    al->setSpacing(8);
    m_actionsLayout = al;
    m_status = plainLabel(QString(), QString());
    m_status->setObjectName(QStringLiteral("settingsStatus"));
    al->addWidget(m_status, 1);
    auto *cancel = styledButton(QStringLiteral("取消"), QStringLiteral("secondary"));
    cancel->setMinimumWidth(88);
    cancel->setFixedHeight(32);
    m_saveButton = styledButton(QStringLiteral("保存设置"), QStringLiteral("primary"));
    m_saveButton->setMinimumWidth(88);
    m_saveButton->setFixedHeight(32);
    al->addWidget(cancel);
    al->addWidget(m_saveButton);
    root->addWidget(actions);
    connect(cancel, &QPushButton::clicked, this, &SettingsDialog::attemptClose);
    connect(m_saveButton, &QPushButton::clicked, this, [this] {
        if (!validateSettings())
            return;
        m_saveButton->setEnabled(false);
        emit saveRequested(draft(), m_revision);
    });

    // 模型页的局部 QSS（行式表单 / 能力开关 / 数值预设面板 / 模型信息状态行）
    applyModelStyle();
}

// 行式表单与预设面板的样式：theme.cpp 里没有这些 class，先在这里按当前调色板生成；
// 需要主题化时把这张表整体搬进 theme.cpp 即可（class 清单见报告中）。
void SettingsDialog::applyModelStyle()
{
    const Palette &p = gs::palette();
    QString qss = QStringLiteral(
        /* 模型行：webui 把它从圆角卡片改成带下分割线的平铺行 */
        "QFrame.settingsModel { border: 0; border-bottom: 1px solid %LINE%;"
        " border-radius: 0; background: transparent; }\n"
        "QFrame.settingsModel[open=\"true\"] { background: %BAND3%; }\n"
        "QFrame.srow { border: 0; border-bottom: 1px solid %LINE%; background: transparent; }\n"
        "QLabel.fieldLabel { color: %MUTED%; font-size: %F12%px; background: transparent; }\n"
        "QLabel.fieldHint { color: %MUTED2%; font-size: %F10%px; background: transparent; }\n"
        "QLabel.modelId { color: %MUTED2%; font-size: %F11%px; font-family: %MONO%;"
        " background: transparent; }\n"
        "QLabel.modelCtx { color: %MUTED2%; font-size: %F9%px; font-family: %MONO%;"
        " background: %SURFACE2%; border: 0; border-radius: %R9%px; padding: 1px 7px; }\n"
        "QLabel.modelCtxFallback { background: transparent; border: 1px dashed %LINESTRONG%; }\n"
        "QWidget.settingsToggle { border: 0; border-bottom: 1px solid %LINE%;"
        " background: transparent; }\n"
        "QWidget.settingsToggleLast { border: 0; background: transparent; }\n"
        "QLabel.toggleLabel { color: %TEXT%; font-size: %F12%px; background: transparent; }\n"
        /* num-combobox */
        "QWidget.numCombo { border: 1px solid %LINE%; border-radius: %R4%px;"
        " background: %INSET%; }\n"
        "QWidget.numCombo[focus=\"true\"] { border-color: %CYAN%; }\n"
        "QLineEdit.numComboInput { border: 0; background: transparent; color: %TEXT%;"
        " padding: 0 8px; selection-background-color: %CYANDARK%; }\n"
        "QPushButton.numToggle { border: 0; border-left: 1px solid %LINE%;"
        " background: transparent; }\n"
        "QPushButton.numToggle:hover { background: %SURFACE2%; }\n"
        "QFrame.numListbox { border: 1px solid %LINESTRONG%; border-radius: %R4%px;"
        " background: %INSET%; }\n"
        "QPushButton.numOption { border: 1px solid transparent; border-radius: %R4%px;"
        " background: transparent; }\n"
        "QPushButton.numOption:hover { border-color: %CYANMID%; background: %ACCENTTINT%; }\n"
        "QPushButton.numOption[selected=\"true\"] { border-color: %CYANMID%; }\n"
        "QLabel.numOptionText { color: %TEXT%; font-size: %F12%px; background: transparent; }\n"
        "QLabel.numOptionValue { color: %MUTED2%; font-size: %F10%px; font-family: %MONO%;"
        " background: transparent; }\n"
        /* 模型信息状态行 / 链接按钮 / 候选行上限 */
        "QPushButton.linkButton { border: 0; background: transparent; color: %CYAN%;"
        " font-size: %F12%px; padding: 2px 4px; }\n"
        "QPushButton.linkButton:hover { color: %TEXTBRIGHT%; }\n"
        /* 只补一条禁用态：否则读取中 / 更新中的文字链跟可点时长得一模一样 */
        "QPushButton.linkButton:disabled { color: %MUTED2%; }\n"
        "QLabel.modelsMetaTitle { color: %TEXTBRIGHT%; font-size: %F13%px; background: transparent; }\n"
        "QLabel.modelsMetaText { color: %MUTED%; font-size: %F11%px; background: transparent; }\n"
        "QLabel.candidateLimit { color: %CYAN%; font-size: %F9%px; font-family: %MONO%;"
        " background: transparent; }\n");

    qss.replace(QStringLiteral("%LINE%"), cssColor(p.line));
    qss.replace(QStringLiteral("%LINESTRONG%"), cssColor(p.lineStrong));
    qss.replace(QStringLiteral("%MUTED2%"), cssColor(p.muted2));
    qss.replace(QStringLiteral("%MUTED%"), cssColor(p.muted));
    qss.replace(QStringLiteral("%TEXTBRIGHT%"), cssColor(p.textBright));
    qss.replace(QStringLiteral("%TEXT%"), cssColor(p.text));
    qss.replace(QStringLiteral("%CYANDARK%"), cssColor(p.cyanDark));
    qss.replace(QStringLiteral("%CYANMID%"), cssColor(p.cyanMid));
    qss.replace(QStringLiteral("%CYAN%"), cssColor(p.cyan));
    qss.replace(QStringLiteral("%ACCENTTINT%"), cssColor(p.accentTint));
    qss.replace(QStringLiteral("%SURFACE2%"), cssColor(p.surface2));
    qss.replace(QStringLiteral("%BAND3%"), cssColor(p.band3));
    qss.replace(QStringLiteral("%INSET%"), cssColor(p.inset));
    qss.replace(QStringLiteral("%MONO%"), gs::monoFont());
    qss.replace(QStringLiteral("%F9%"), QString::number(gs::scaledPx(9)));
    qss.replace(QStringLiteral("%F10%"), QString::number(gs::scaledPx(10)));
    qss.replace(QStringLiteral("%F11%"), QString::number(gs::scaledPx(11)));
    qss.replace(QStringLiteral("%F12%"), QString::number(gs::scaledPx(12)));
    qss.replace(QStringLiteral("%F13%"), QString::number(gs::scaledPx(13)));
    qss.replace(QStringLiteral("%R4%"), QString::number(gs::scaledPx(4)));
    qss.replace(QStringLiteral("%R9%"), QString::number(gs::scaledPx(9)));
    setStyleSheet(qss);
}

void SettingsDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // 皮肤可能在关闭期间换过：局部样式表按当前调色板重建
    applyModelStyle();
}

// style.css @media(max-width:640px)：
//   .models-panel{grid-template-columns:118px minmax(0,1fr)} / .form-grid{grid-template-columns:1fr}
//   .settings-tabs{padding:0} 且按钮 flex:1 / .provider-actions{flex-wrap:wrap}
//   .settings-head{padding:12px} / .settings-actions{padding:8px 10px}
//   .provider-sidebar{padding:10px 5px} / .provider-editor{padding:10px} / 技能・MCP 面板 padding:10px
void SettingsDialog::applyCompactMode(bool compact)
{
    if (m_compact == compact)
        return;
    m_compact = compact;
    // webui 紧凑档去边框、圆角归零（QSS 靠动态属性切换，见 theme.cpp）
    setProperty("compact", compact);
    restyle(this);

    if (m_headLayout) {
        m_headLayout->setContentsMargins(compact ? 12 : 18, compact ? 12 : 16,
                                         compact ? 12 : 18, compact ? 12 : 16);
    }
    if (m_actionsLayout) {
        m_actionsLayout->setContentsMargins(compact ? 10 : 18, compact ? 8 : 10,
                                            compact ? 10 : 18, compact ? 8 : 10);
    }
    if (m_tabsLayout) {
        m_tabsLayout->setContentsMargins(compact ? 0 : 18, 0, compact ? 0 : 18, 0);
        QPushButton *tabs[6] = {m_tabModels, m_tabSkills, m_tabMcp, m_tabMemory, m_tabKb, m_tabUsage};
        for (int i = 0; i < 6; ++i) {
            if (!tabs[i])
                continue;
            tabs[i]->setSizePolicy(compact ? QSizePolicy::Expanding : QSizePolicy::Minimum,
                                   QSizePolicy::Fixed);
            m_tabsLayout->setStretch(i, compact ? 1 : 0);
        }
        if (m_tabsSpacer) {
            m_tabsSpacer->changeSize(0, 0,
                                     compact ? QSizePolicy::Fixed : QSizePolicy::Expanding,
                                     QSizePolicy::Minimum);
            m_tabsLayout->invalidate();
        }
    }

    if (m_providerSidebar) {
        m_providerSidebar->setFixedWidth(compact ? 118 : 210);
        if (m_providerSidebarLayout) {
            m_providerSidebarLayout->setContentsMargins(compact ? 5 : 10, compact ? 10 : 15,
                                                        compact ? 5 : 10, compact ? 10 : 15);
        }
    }
    if (m_providerEditorLayout) {
        m_providerEditorLayout->setContentsMargins(compact ? 10 : 18, compact ? 10 : 18,
                                                   compact ? 10 : 18, compact ? 10 : 18);
    }
    if (m_providerGrid)
        placeFormFields(m_providerGrid, m_providerFields, compact);
    // 模型卡的行式表单：.srow-2 的两列并作单列；.model-caps 窄屏整段隐藏
    for (int i = 0; i < m_modelRows.size(); ++i) {
        ModelRow *row = m_modelRows.at(i);
        if (row)
            row->setCompact(compact);
    }
    for (int i = 0; i < m_modelCapsRows.size(); ++i) {
        QWidget *caps = m_modelCapsRows.at(i);
        if (caps)
            caps->setVisible(!compact && caps->property("hasCaps").toBool());
    }
    if (m_providerActions) {
        m_providerActions->setDirection(compact ? QBoxLayout::TopToBottom
                                                : QBoxLayout::LeftToRight);
        if (m_providerActionsSpacer) {
            m_providerActionsSpacer->changeSize(0, 0,
                                                compact ? QSizePolicy::Fixed
                                                        : QSizePolicy::Expanding,
                                                QSizePolicy::Minimum);
        }
        m_providerActions->invalidate();
    }

    const int pagePad = compact ? 10 : 18;
    if (m_skillsPageLayout)
        m_skillsPageLayout->setContentsMargins(pagePad, pagePad, pagePad, pagePad);
    if (m_mcpPageLayout)
        m_mcpPageLayout->setContentsMargins(pagePad, pagePad, pagePad, pagePad);

    if (m_stack)
        m_stack->updateGeometry();
}

void SettingsDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    applyCompactMode(width() <= 640);
}

QWidget *SettingsDialog::buildModelsPage()
{
    auto *page = new QWidget;
    auto *l = new QHBoxLayout(page);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    l->addWidget(buildProviderSidebar());
    l->addWidget(buildProviderEditor(), 1);
    return page;
}

QWidget *SettingsDialog::buildProviderSidebar()
{
    auto *side = new QWidget;
    side->setObjectName(QStringLiteral("providerSidebar"));
    side->setAttribute(Qt::WA_StyledBackground, true);
    side->setFixedWidth(210);
    m_providerSidebar = side;
    auto *l = new QVBoxLayout(side);
    l->setContentsMargins(10, 15, 10, 15);
    l->setSpacing(0);
    m_providerSidebarLayout = l;
    l->addWidget(plainLabel(QStringLiteral("sectionLabel"), QStringLiteral("供应商")));

    auto *nav = new QWidget;
    nav->setObjectName(QStringLiteral("providerNav"));
    nav->setAttribute(Qt::WA_StyledBackground, true);
    m_providerNavLayout = new QVBoxLayout(nav);
    m_providerNavLayout->setContentsMargins(0, 0, 0, 0);
    m_providerNavLayout->setSpacing(0);
    m_providerNavLayout->addStretch(1);
    l->addWidget(plainScroll(nav, QStringLiteral("providerNavScroll")), 1);

    auto *add = new IconPushButton(side);
    add->setObjectName(QStringLiteral("addProvider"));
    add->setMinimumHeight(28);
    add->setCursor(Qt::PointingHandCursor);
    add->setFocusPolicy(Qt::TabFocus);
    add->setText(QStringLiteral("添加供应商"));
    add->setIconColors(QColor(QStringLiteral("#d7e2e8")),
                       QColor(QStringLiteral("#d7e2e8")));
    add->setIconName(QStringLiteral("plus"), 12);
    l->addSpacing(10);
    l->addWidget(add);
    connect(add, &QPushButton::clicked, this, &SettingsDialog::addProvider);
    return side;
}

QWidget *SettingsDialog::buildProviderEditor()
{
    auto *editor = new QWidget;
    editor->setObjectName(QStringLiteral("providerEditor"));
    editor->setAttribute(Qt::WA_StyledBackground, true);
    m_providerEditor = editor;
    auto *el = new QVBoxLayout(editor);
    el->setContentsMargins(18, 18, 18, 18);
    el->setSpacing(14);
    m_providerEditorLayout = el;

    // .placeholder-panel
    m_placeholder = new QWidget(editor);
    auto *pl = new QVBoxLayout(m_placeholder);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(0);
    pl->addStretch(1);
    pl->addWidget(plainLabel(QStringLiteral("placeholderTitle"),
                             QStringLiteral("添加供应商以开始配置")),
                  0, Qt::AlignHCenter);
    pl->addStretch(1);
    el->addWidget(m_placeholder, 1);

    // ===== provider-section：PROVIDER =====
    m_sectionProvider = styledFrame(QStringLiteral("providerSection"));
    auto *sl = new QVBoxLayout(m_sectionProvider);
    sl->setContentsMargins(15, 15, 15, 15);
    sl->setSpacing(0);

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(12);
    auto *titleBox = new QVBoxLayout;
    titleBox->setContentsMargins(0, 0, 0, 0);
    titleBox->setSpacing(2);
    m_editorEyebrow = plainLabel(QStringLiteral("eyebrow"), QStringLiteral("PROVIDER"));
    m_editorTitle = plainLabel(QStringLiteral("sectionTitle"), QString());
    titleBox->addWidget(m_editorEyebrow);
    titleBox->addWidget(m_editorTitle);
    titleRow->addLayout(titleBox, 1);
    m_enabledCheck = new QCheckBox(QStringLiteral("启用"), m_sectionProvider);
    setClass(m_enabledCheck, QStringLiteral("settingsCheck"));
    m_enabledCheck->setCursor(Qt::PointingHandCursor);
    titleRow->addWidget(m_enabledCheck, 0, Qt::AlignVCenter);
    sl->addLayout(titleRow);
    sl->addSpacing(14);

    auto *grid = new QGridLayout;
    grid->setObjectName(QStringLiteral("providerFormGrid"));
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);
    m_providerGrid = grid;
    m_nameEdit = settingsInput(QString());
    m_idEdit = settingsInput(QString());
    m_idEdit->setEnabled(false);
    m_discoveryCombo = settingsCombo();
    m_discoveryCombo->addItem(QStringLiteral("OpenAI / Compatible / Ollama"),
                              QStringLiteral("openai"));
    m_discoveryCombo->addItem(QStringLiteral("Anthropic"), QStringLiteral("anthropic"));
    m_discoveryCombo->addItem(QStringLiteral("不支持模型发现"), QStringLiteral("none"));
    m_baseUrlEdit = settingsInput(QString());
    m_keyEnvEdit = settingsInput(QString());
    m_apiKeyEdit = settingsInput(QStringLiteral("输入 API Key"));
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_defaultApiCombo = settingsCombo();
    m_defaultApiCombo->addItem(QStringLiteral("OpenAI Chat"), QStringLiteral("openai-chat"));
    m_defaultApiCombo->addItem(QStringLiteral("OpenAI Responses"),
                               QStringLiteral("openai-responses"));
    m_defaultApiCombo->addItem(QStringLiteral("Anthropic Messages"),
                               QStringLiteral("anthropic-messages"));
    // app.js：SSL 证书验证三选一，「自定义」落到 provider.ssl_verify 的路径字符串上
    m_sslCombo = settingsCombo();
    m_sslCombo->addItem(QStringLiteral("默认（系统证书）"), QStringLiteral("on"));
    m_sslCombo->addItem(QStringLiteral("跳过验证"), QStringLiteral("off"));
    m_sslCombo->addItem(QStringLiteral("自定义 CA 证书路径"), QStringLiteral("custom"));
    m_sslPathEdit = settingsInput(QString());
    m_sslPathEdit->setPlaceholderText(QStringLiteral("/path/to/ca.pem"));
    m_sslPathRow = fieldRow(QStringLiteral("CA 证书路径"), m_sslPathEdit);
    m_sslPathRow->setVisible(false);
    m_providerFields = {
        fieldRow(QStringLiteral("供应商名称"), m_nameEdit),
        fieldRow(QStringLiteral("供应商 ID"), m_idEdit),
        fieldRow(QStringLiteral("供应商类型"), m_discoveryCombo),
        fieldRow(QStringLiteral("API 地址"), m_baseUrlEdit),
        fieldRow(QStringLiteral("API Key 环境变量"), m_keyEnvEdit),
        fieldRow(QStringLiteral("API Key"), m_apiKeyEdit),
        fieldRow(QStringLiteral("默认 API 协议"), m_defaultApiCombo),
        fieldRow(QStringLiteral("SSL 证书验证"), m_sslCombo),
        m_sslPathRow,
    };
    placeFormFields(grid, m_providerFields, m_compact);
    sl->addLayout(grid);

    auto *acts = new QHBoxLayout;
    acts->setContentsMargins(0, 0, 0, 0);
    acts->setSpacing(7);
    m_clearKeyButton = styledButton(QStringLiteral("清除 Key"), QStringLiteral("secondary"));
    m_testButton = styledButton(QStringLiteral("测试连接"), QStringLiteral("secondary"));
    m_inlineResult = plainLabel(QStringLiteral("inlineResult"), QString());
    // .inline-result{min-width:0;flex:1}：长文案不能反过来把窗口最小宽度撑大
    m_inlineResult->setWordWrap(true);
    m_inlineResult->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_deleteProviderButton =
        styledButton(QStringLiteral("删除供应商"), QStringLiteral("danger"));
    acts->addWidget(m_clearKeyButton);
    acts->addWidget(m_testButton);
    acts->addWidget(m_inlineResult, 0, Qt::AlignVCenter);
    m_providerActionsSpacer = new QSpacerItem(0, 0, QSizePolicy::Expanding,
                                              QSizePolicy::Minimum);
    acts->addSpacerItem(m_providerActionsSpacer);
    acts->addWidget(m_deleteProviderButton);
    m_providerActions = acts;
    sl->addSpacing(14);
    sl->addLayout(acts);
    el->addWidget(m_sectionProvider);

    connect(m_enabledCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("enabled"), on);
        m_providers[index] = provider;
        markDirty();
    });
    connect(m_nameEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("name"), text);
        m_providers[index] = provider;
        markDirty();
        m_editorTitle->setText(text);
        renderProviderList();
    });
    connect(m_discoveryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("discovery_api"), comboData(m_discoveryCombo));
        m_providers[index] = provider;
        markDirty();
    });
    connect(m_baseUrlEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("base_url"), text);
        m_providers[index] = provider;
        markDirty();
    });
    connect(m_keyEnvEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("api_key_env"), text);
        m_providers[index] = provider;
        markDirty();
        m_apiKeyEdit->setEnabled(text.trimmed().isEmpty());
    });
    connect(m_apiKeyEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        // app.js：只有输入了内容、或原本不是掩码值时才回写
        if (text.isEmpty() && provider.value(QStringLiteral("api_key")).toString()
                == QLatin1String("********"))
            return;
        provider.insert(QStringLiteral("api_key"), text);
        m_providers[index] = provider;
        markDirty();
    });
    connect(m_defaultApiCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("default_api"), comboData(m_defaultApiCombo));
        m_providers[index] = provider;
        markDirty();
        // 左栅分组看的就是 default_api：改了就重新分一次组（webui 每个 provider
        // 字段变更后都 renderProviderList，只是其它字段不影响分组）
        renderProviderList();
    });
    connect(m_sslCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_updating)
            return;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        const QString mode = comboData(m_sslCombo);
        QString path = m_sslPathEdit->text().trimmed();
        if (mode == QLatin1String("custom")) {
            // 切回「自定义」时还回上次填的路径
            if (path.isEmpty())
                path = m_sslPathMemory;
            m_sslPathEdit->setText(path);
            m_sslPathMemory = path;
        }
        QVariantMap provider = m_providers.at(index).toMap();
        if (mode == QLatin1String("custom"))
            provider.insert(QStringLiteral("ssl_verify"), path);
        else
            provider.insert(QStringLiteral("ssl_verify"), mode == QLatin1String("off") ? false : true);
        m_providers[index] = provider;
        renderProviderEditor(); // 路径框显隐与字段重排都在渲染里
        markDirty();
    });
    connect(m_sslPathEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        if (comboData(m_sslCombo) != QLatin1String("custom"))
            return; // 非自定义档位下这个框是藏着的，不该改写 ssl_verify
        m_sslPathMemory = text;
        const int index = providerIndex(m_activeProviderId);
        if (index < 0)
            return;
        QVariantMap provider = m_providers.at(index).toMap();
        provider.insert(QStringLiteral("ssl_verify"), text);
        m_providers[index] = provider;
        markDirty();
    });
    connect(m_clearKeyButton, &QPushButton::clicked, this, &SettingsDialog::clearApiKey);
    connect(m_deleteProviderButton, &QPushButton::clicked, this,
            &SettingsDialog::deleteProvider);
    connect(m_testButton, &QPushButton::clicked, this, [this] {
        if (m_activeProviderId.isEmpty())
            return;
        m_testing = true;
        renderProviderEditor();
        emit testProviderRequested(m_activeProviderId);
    });

    // ===== provider-section：MODELS =====
    m_sectionModels = styledFrame(QStringLiteral("providerSection"));
    auto *ml = new QVBoxLayout(m_sectionModels);
    ml->setContentsMargins(15, 15, 15, 15);
    ml->setSpacing(0);

    auto *headRow = new QHBoxLayout;
    headRow->setContentsMargins(0, 0, 0, 0);
    headRow->setSpacing(12);
    auto *headBox = new QVBoxLayout;
    headBox->setContentsMargins(0, 0, 0, 0);
    headBox->setSpacing(2);
    headBox->addWidget(plainLabel(QStringLiteral("eyebrow"), QStringLiteral("模型")));
    auto *titleLine = new QHBoxLayout;
    titleLine->setContentsMargins(0, 0, 0, 0);
    titleLine->setSpacing(6);
    m_modelsTitle = plainLabel(QStringLiteral("sectionTitle"), QStringLiteral("已添加模型"));
    m_modelsCount = plainLabel(QStringLiteral("sectionTitleSmall"), QStringLiteral("0"));
    titleLine->addWidget(m_modelsTitle);
    titleLine->addWidget(m_modelsCount, 0, Qt::AlignBottom);
    titleLine->addStretch(1);
    headBox->addLayout(titleLine);
    headRow->addLayout(headBox, 1);
    m_readButton = new QPushButton(QStringLiteral("读取模型"), m_sectionModels);
    // webui HEAD 把它降成文字链（.link-button），与「更新模型信息」同一套
    setClass(m_readButton, QStringLiteral("linkButton"));
    m_readButton->setCursor(Qt::PointingHandCursor);
    m_readButton->setFocusPolicy(Qt::TabFocus);
    headRow->addWidget(m_readButton, 0, Qt::AlignVCenter);
    ml->addLayout(headRow);
    ml->addSpacing(14);
    connect(m_readButton, &QPushButton::clicked, this, [this] {
        if (m_activeProviderId.isEmpty())
            return;
        m_reading = true;
        renderProviderEditor();
        emit readModelsRequested(m_activeProviderId);
    });

    auto *listHost = new QWidget(m_sectionModels);
    m_modelListLayout = new QVBoxLayout(listHost);
    m_modelListLayout->setContentsMargins(0, 0, 0, 0);
    // 模型行现在是平铺行（.settings-model{border-bottom}），行间不再留缝隙
    m_modelListLayout->setSpacing(0);
    ml->addWidget(listHost);

    // .add-model
    auto *addBox = styledFrame(QString());
    addBox->setObjectName(QStringLiteral("addModelBox"));
    auto *abl = new QVBoxLayout(addBox);
    abl->setContentsMargins(12, 12, 12, 12);
    abl->setSpacing(0);
    abl->addWidget(plainLabel(QStringLiteral("fieldCaption"),
                              QStringLiteral("添加模型 · 搜索候选")));
    abl->addSpacing(5);
    m_candidateSearch = settingsInput(QStringLiteral("搜索模型 ID"));
    abl->addWidget(m_candidateSearch);
    auto *candidates = new QWidget;
    candidates->setObjectName(QStringLiteral("candidateList"));
    candidates->setAttribute(Qt::WA_StyledBackground, true);
    m_candidateLayout = new QVBoxLayout(candidates);
    m_candidateLayout->setContentsMargins(0, 0, 0, 0);
    m_candidateLayout->setSpacing(0);
    m_candidateLayout->addStretch(1);
    auto *candidateScroll = plainScroll(candidates, QStringLiteral("candidateScroll"));
    candidateScroll->setMaximumHeight(150);
    abl->addSpacing(7);
    abl->addWidget(candidateScroll);
    abl->addSpacing(7);
    auto *manual = new QHBoxLayout;
    manual->setContentsMargins(0, 0, 0, 0);
    manual->setSpacing(7);
    m_manualId = settingsInput(QStringLiteral("手动输入模型 ID"));
    m_addManualButton = styledButton(QStringLiteral("添加"), QStringLiteral("primary"));
    m_addManualButton->setFixedHeight(32);
    manual->addWidget(m_manualId, 1);
    manual->addWidget(m_addManualButton);
    abl->addLayout(manual);
    ml->addSpacing(12);
    ml->addWidget(addBox);
    el->addWidget(m_sectionModels);

    // ===== provider-section：模型信息（.models-meta） =====
    // /config/model-metadata 的快照状态：缓存时间 / 来源 / 错误 + 手动刷新
    m_sectionMeta = styledFrame(QStringLiteral("providerSection"));
    m_sectionMeta->setObjectName(QStringLiteral("modelsMetaSection"));
    auto *metaLayout = new QHBoxLayout(m_sectionMeta);
    metaLayout->setContentsMargins(15, 14, 15, 14);
    metaLayout->setSpacing(16);
    auto *metaBox = new QVBoxLayout;
    metaBox->setContentsMargins(0, 0, 0, 0);
    metaBox->setSpacing(5);
    m_metaTitle = plainLabel(QStringLiteral("modelsMetaTitle"), QStringLiteral("模型信息"));
    m_metaText = plainLabel(QStringLiteral("modelsMetaText"), QString());
    m_metaText->setWordWrap(true);
    m_metaText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    metaBox->addWidget(m_metaTitle);
    metaBox->addWidget(m_metaText);
    metaLayout->addLayout(metaBox, 1);
    m_metaButton = new QPushButton(QStringLiteral("更新模型信息"), m_sectionMeta);
    setClass(m_metaButton, QStringLiteral("linkButton"));
    m_metaButton->setCursor(Qt::PointingHandCursor);
    m_metaButton->setFocusPolicy(Qt::TabFocus);
    metaLayout->addWidget(m_metaButton, 0, Qt::AlignVCenter);
    el->addWidget(m_sectionMeta);
    connect(m_metaButton, &QPushButton::clicked, this, [this] {
        if (m_metaRefreshing)
            return;
        m_metaRefreshing = true;
        renderModelMeta();
        emit modelMetadataRefreshRequested();
    });
    renderModelMeta();

    el->addStretch(1);

    connect(m_candidateSearch, &QLineEdit::textChanged, this,
            [this](const QString &) { renderCandidates(); });
    connect(m_addManualButton, &QPushButton::clicked, this, [this] {
        addModel(m_manualId->text(), QString());
        m_manualId->clear();
    });
    connect(m_manualId, &QLineEdit::returnPressed, this, [this] {
        addModel(m_manualId->text(), QString());
        m_manualId->clear();
    });

    return plainScroll(editor, QStringLiteral("providerEditorScroll"));
}

QWidget *SettingsDialog::buildSkillsPage()
{
    auto *page = new QWidget;
    auto *l = new QVBoxLayout(page);
    l->setContentsMargins(18, 18, 18, 18);
    l->setSpacing(0);
    m_skillsPageLayout = l;

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(0);
    auto *box = new QVBoxLayout;
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(2);
    box->addWidget(plainLabel(QStringLiteral("eyebrow"), QStringLiteral("技能库")));
    auto *line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(6);
    line->addWidget(plainLabel(QStringLiteral("panelTitle"), QStringLiteral("技能")));
    m_skillCount = plainLabel(QStringLiteral("panelCount"), QStringLiteral("0"));
    line->addWidget(m_skillCount, 0, Qt::AlignBottom);
    line->addStretch(1);
    box->addLayout(line);
    toolbar->addLayout(box, 1);
    m_refreshSkills = styledButton(QStringLiteral("刷新"), QStringLiteral("secondary"));
    toolbar->addWidget(m_refreshSkills, 0, Qt::AlignVCenter);
    l->addLayout(toolbar);
    l->addSpacing(12);

    m_skillsStatus = plainLabel(QStringLiteral("panelStatus"), QString());
    l->addWidget(m_skillsStatus);
    l->addSpacing(10);

    auto *panel = new QWidget;
    panel->setObjectName(QStringLiteral("skillsPanel"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    m_skillsLayout = new QVBoxLayout(panel);
    m_skillsLayout->setContentsMargins(0, 0, 0, 0);
    m_skillsLayout->setSpacing(7);
    m_skillsLayout->addStretch(1);
    l->addWidget(plainScroll(panel, QString(), QStringLiteral("panelScroll")), 1);

    connect(m_refreshSkills, &QPushButton::clicked, this, [this] {
        m_refreshSkills->setEnabled(false);
        emit refreshSkillsRequested();
    });
    return page;
}

QWidget *SettingsDialog::buildMcpPage()
{
    auto *page = new QWidget;
    auto *l = new QVBoxLayout(page);
    l->setContentsMargins(18, 18, 18, 18);
    l->setSpacing(0);
    m_mcpPageLayout = l;

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(0);
    auto *box = new QVBoxLayout;
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(2);
    box->addWidget(plainLabel(QStringLiteral("eyebrow"), QStringLiteral("MCP 工具")));
    auto *line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(6);
    line->addWidget(plainLabel(QStringLiteral("panelTitle"), QStringLiteral("MCP 工具")));
    m_mcpCount = plainLabel(QStringLiteral("panelCount"), QStringLiteral("0"));
    line->addWidget(m_mcpCount, 0, Qt::AlignBottom);
    line->addStretch(1);
    box->addLayout(line);
    toolbar->addLayout(box, 1);
    m_refreshMcp = styledButton(QStringLiteral("刷新"), QStringLiteral("secondary"));
    toolbar->addWidget(m_refreshMcp, 0, Qt::AlignVCenter);
    l->addLayout(toolbar);
    l->addSpacing(12);

    m_mcpStatus = plainLabel(QStringLiteral("panelStatus"), QString());
    l->addWidget(m_mcpStatus);
    l->addSpacing(10);

    auto *panel = new QWidget;
    panel->setObjectName(QStringLiteral("mcpPanel"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    m_mcpLayout = new QVBoxLayout(panel);
    m_mcpLayout->setContentsMargins(0, 0, 0, 0);
    m_mcpLayout->setSpacing(7);
    m_mcpLayout->addStretch(1);
    l->addWidget(plainScroll(panel, QString(), QStringLiteral("panelScroll")), 1);

    connect(m_refreshMcp, &QPushButton::clicked, this, [this] {
        m_refreshMcp->setEnabled(false);
        emit refreshMcpRequested();
    });
    return page;
}

QWidget *SettingsDialog::buildUsagePage()
{
    // #panel-usage：.usage-panel 自身纵向滚动，内容由 UsagePanel 渲染
    m_usagePanel = new UsagePanel;
    connect(m_usagePanel, &UsagePanel::statsRequested, this, &SettingsDialog::usageStatsRequested);
    return plainScroll(m_usagePanel, QString(), QStringLiteral("usagePanelScroll"));
}

QWidget *SettingsDialog::buildMemoryPage()
{
    // #panel-memory：.memory-panel 自身纵向滚动，内容由 MemoryPanel 渲染
    m_memoryPanel = new MemoryPanel;
    connect(m_memoryPanel, &MemoryPanel::refreshRequested,
            this, &SettingsDialog::memoryRefreshRequested);
    connect(m_memoryPanel, &MemoryPanel::listRequested,
            this, &SettingsDialog::memoryListRequested);
    connect(m_memoryPanel, &MemoryPanel::configPatchRequested,
            this, &SettingsDialog::memoryConfigPatchRequested);
    connect(m_memoryPanel, &MemoryPanel::actionRequested,
            this, &SettingsDialog::memoryActionRequested);
    connect(m_memoryPanel, &MemoryPanel::bulkRequested,
            this, &SettingsDialog::memoryBulkRequested);
    connect(m_memoryPanel, &MemoryPanel::detailRequested,
            this, &SettingsDialog::memoryDetailRequested);
    connect(m_memoryPanel, &MemoryPanel::fieldSaveRequested,
            this, &SettingsDialog::memoryFieldSaveRequested);
    connect(m_memoryPanel, &MemoryPanel::fieldDeleteRequested,
            this, &SettingsDialog::memoryFieldDeleteRequested);
    connect(m_memoryPanel, &MemoryPanel::toolClearRequested,
            this, &SettingsDialog::memoryToolClearRequested);
    connect(m_memoryPanel, &MemoryPanel::toolsClearAllRequested,
            this, &SettingsDialog::memoryToolsClearAllRequested);
    connect(m_memoryPanel, &MemoryPanel::maintenanceRequested,
            this, &SettingsDialog::memoryMaintenanceRequested);
    // .memory-panel 自身就是纵向滚动容器，外层不再套一层
    return m_memoryPanel;
}

QWidget *SettingsDialog::buildKbPage()
{
    // #panel-kb：.kb-panel 自身纵向滚动，内容由 KbPanel 渲染
    m_kbPanel = new KbPanel;
    connect(m_kbPanel, &KbPanel::refreshRequested, this, &SettingsDialog::kbRefreshRequested);
    connect(m_kbPanel, &KbPanel::libraryChanged, this, &SettingsDialog::kbRefreshRequested);
    connect(m_kbPanel, &KbPanel::folderChanged, this, &SettingsDialog::kbFolderChanged);
    connect(m_kbPanel, &KbPanel::libraryCreateRequested,
            this, &SettingsDialog::kbLibraryCreateRequested);
    connect(m_kbPanel, &KbPanel::libraryRenameRequested,
            this, &SettingsDialog::kbLibraryRenameRequested);
    connect(m_kbPanel, &KbPanel::libraryDeleteRequested,
            this, &SettingsDialog::kbLibraryDeleteRequested);
    connect(m_kbPanel, &KbPanel::filesImportRequested,
            this, &SettingsDialog::kbFilesImportRequested);
    connect(m_kbPanel, &KbPanel::dirImportRequested,
            this, &SettingsDialog::kbDirImportRequested);
    connect(m_kbPanel, &KbPanel::dirImportToFolderRequested,
            this, &SettingsDialog::kbDirImportToFolderRequested);
    connect(m_kbPanel, &KbPanel::documentDeleteRequested,
            this, &SettingsDialog::kbDocumentDeleteRequested);
    connect(m_kbPanel, &KbPanel::documentReindexRequested,
            this, &SettingsDialog::kbDocumentReindexRequested);
    connect(m_kbPanel, &KbPanel::documentMoveRequested,
            this, &SettingsDialog::kbDocumentMoveRequested);
    connect(m_kbPanel, &KbPanel::folderCreateRequested,
            this, &SettingsDialog::kbFolderCreateRequested);
    connect(m_kbPanel, &KbPanel::folderRenameRequested,
            this, &SettingsDialog::kbFolderRenameRequested);
    connect(m_kbPanel, &KbPanel::folderDeleteRequested,
            this, &SettingsDialog::kbFolderDeleteRequested);
    connect(m_kbPanel, &KbPanel::searchRequested, this, &SettingsDialog::kbSearchRequested);
    // .kb-panel 自身就是纵向滚动容器，外层不再套一层
    return m_kbPanel;
}

// ---- 记忆 / 知识库页的数据转发 ----

void SettingsDialog::setMemorySummary(const QVariantMap &summary)
{
    if (m_memoryPanel)
        m_memoryPanel->setSummary(summary);
}

void SettingsDialog::setMemoryExperiences(const QVariantMap &listing)
{
    if (m_memoryPanel)
        m_memoryPanel->setListing(listing);
}

void SettingsDialog::setMemoryListFailed(const QString &error)
{
    if (m_memoryPanel)
        m_memoryPanel->setListFailed(error);
}

void SettingsDialog::setMemoryTools(const QVariantMap &payload)
{
    if (m_memoryPanel)
        m_memoryPanel->setTools(payload);
}

void SettingsDialog::setMemoryStatusText(const QString &text)
{
    if (m_memoryPanel)
        m_memoryPanel->setStatusText(text);
}

void SettingsDialog::setKbLibraries(const QVariantList &libraries)
{
    if (m_kbPanel)
        m_kbPanel->setLibraries(libraries);
}

void SettingsDialog::setKbStatus(const QVariantMap &status)
{
    if (m_kbPanel)
        m_kbPanel->setStatus(status);
}

void SettingsDialog::setKbFolders(const QVariantList &folders, const QVariantMap &tree)
{
    if (m_kbPanel)
        m_kbPanel->setFolders(folders, tree);
}

void SettingsDialog::setKbDocuments(const QVariantList &documents, int total)
{
    if (m_kbPanel)
        m_kbPanel->setDocuments(documents, total);
}

void SettingsDialog::setKbSearchResult(const QVariantMap &result)
{
    if (m_kbPanel)
        m_kbPanel->setSearchResult(result);
}

void SettingsDialog::setKbStatusText(const QString &text)
{
    if (m_kbPanel)
        m_kbPanel->setStatusText(text);
}

// ------------------------------------------------------------ 数据接口

void SettingsDialog::loadConfig(const QVariantMap &config, const QVariant &revision)
{
    m_extra = config;
    m_extra.remove(QStringLiteral("providers"));
    m_extra.remove(QStringLiteral("models"));
    m_extra.remove(QStringLiteral("default_model"));
    m_extra.remove(QStringLiteral("revision"));

    m_providers = config.value(QStringLiteral("providers")).toList();
    m_models = config.value(QStringLiteral("models")).toList();
    m_defaultModel = config.value(QStringLiteral("default_model")).toString();
    m_revision = revision.isValid() ? revision : config.value(QStringLiteral("revision"));
    m_activeProviderId =
        m_providers.isEmpty() ? QString() : m_providers.first().toMap().value(QStringLiteral("id")).toString();
    m_dirty = false;
    m_discovered.clear();
    m_openModelKeys.clear(); // 新草稿：展开态不跟上一次的同一 provider/id 串上
    m_testing = false;
    m_reading = false;
    m_candidateSearch->clear();
    m_manualId->clear();
    m_saveButton->setEnabled(true);
    setStatus(QString());
    // 快照 / 候选里已有的上限先补上（只填空字段，不标 dirty）
    applyStoredModelLimits();
    // 重载草稿不应该把用户踢回模型页：live 模式下配置比开页晚到，
    // 硬切 "models" 会让 --tab kb / memory / usage 全部失效（看到的总是模型页）
    switchTab(m_tab);
    renderSettings();
}

QVariantMap SettingsDialog::draft() const
{
    QVariantMap out = m_extra;
    out.insert(QStringLiteral("providers"), m_providers);
    out.insert(QStringLiteral("models"), m_models);
    out.insert(QStringLiteral("default_model"), m_defaultModel);
    return out;
}

void SettingsDialog::setDiscoveredModels(const QString &providerId, const QVariantList &models)
{
    m_discovered.insert(providerId, models);
    // webui readProviderModels：候选里带了上限就顺手补进草稿（只填空字段）
    if (applyStoredModelLimits())
        markDirty();
    if (providerId == m_activeProviderId)
        renderProviderEditor();
}

// 快照里的 models / providers / hosts 既可能是计数（后端真实响应），
// 也可能是宿主另行喂进来的数组（模型 ID -> 上限），按类型分别取数
static int metaEntryCount(const QVariantMap &meta, const QString &key)
{
    const QVariant value = meta.value(key);
    if (value.type() == QVariant::List)
        return value.toList().size();
    if (value.type() == QVariant::Map)
        return value.toMap().size();
    return value.toInt();
}

void SettingsDialog::setModelMetadata(const QVariantMap &snapshot)
{
    m_meta = snapshot;
    m_metaError.clear();
    m_metaErrorKind.clear();
    m_metaStatus = 0;
    m_metaRefreshing = false;
    // 上限只填空字段、能力只勾「数据里说支持」的（applyModelLimits 口径）。
    // 刻意不标 dirty：打开设置只看一眼，不该面对「放弃未保存的修改」
    applyStoredModelLimits();
    renderProviderEditor();
}

void SettingsDialog::setModelMetadataFailed(const QString &kind, const QString &error,
                                            int httpStatus)
{
    m_metaError = error.isEmpty() ? QStringLiteral("未知错误") : error;
    m_metaErrorKind = kind;
    m_metaStatus = httpStatus;
    m_metaRefreshing = false;
    renderProviderEditor();
}

void SettingsDialog::renderModelMeta()
{
    if (!m_metaText || !m_metaButton)
        return;
    m_metaText->setText(metaStatusText());
    if (m_metaTitle)
        m_metaTitle->setToolTip(metaTitleText());
    m_metaButton->setText(m_metaRefreshing ? QStringLiteral("更新中…")
                                           : QStringLiteral("更新模型信息"));
    m_metaButton->setEnabled(!m_metaRefreshing);
}

// app.js metaStatusText：缓存时间 / 来源 / 错误（404 时说清是接口未注册）
QString SettingsDialog::metaStatusText() const
{
    if (!m_metaError.isEmpty()) {
        const QString verb = m_metaErrorKind == QLatin1String("refresh")
                                 ? QStringLiteral("更新失败")
                                 : QStringLiteral("读取失败");
        const QString status = m_metaStatus > 0
                                   ? QStringLiteral("（HTTP %1）").arg(m_metaStatus)
                                   : QString();
        const QString hint = m_metaStatus == 404
                                 ? QStringLiteral("，该接口未注册，重启后端服务后再试")
                                 : QString();
        return QStringLiteral("%1：%2%3%4").arg(verb, m_metaError, status, hint);
    }
    if (m_meta.isEmpty())
        return QStringLiteral("正在读取模型信息…");
    const int count = metaEntryCount(m_meta, QStringLiteral("models"));
    const bool available = m_meta.contains(QStringLiteral("available"))
                               ? m_meta.value(QStringLiteral("available")).toBool()
                               : count > 0;
    if (!available)
        return QStringLiteral("还没有模型信息快照，点右侧更新一次即可。");
    const QString fileDay = m_meta.value(QStringLiteral("file_time")).toString().left(10);
    QStringList parts;
    parts << QStringLiteral("模型信息更新于 %1")
                 .arg(fileDay.isEmpty() ? QStringLiteral("未知时间") : formatMetaDate(fileDay));
    // 文件写入时间与数据抓取日期不一致时把后者也带上（快照可能被手工替换或随仓库签出）
    const QString fetchedAt = m_meta.value(QStringLiteral("fetched_at")).toString();
    if (!fetchedAt.isEmpty() && fetchedAt != fileDay)
        parts << QStringLiteral("数据抓取于 %1").arg(formatMetaDate(fetchedAt));
    parts << QStringLiteral("收录 %1 个模型").arg(count);
    return parts.join(QStringLiteral(" · "));
}

// app.js metaTitleText：状态行的 tooltip
QString SettingsDialog::metaTitleText() const
{
    if (m_meta.isEmpty())
        return QString();
    QStringList bits;
    bits << QStringLiteral("快照来源 %1")
                .arg(m_meta.value(QStringLiteral("source")).toString());
    const QString fileTime = m_meta.value(QStringLiteral("file_time")).toString();
    if (!fileTime.isEmpty())
        bits << QStringLiteral("文件写入 %1").arg(fileTime);
    const QString fetchedAt = m_meta.value(QStringLiteral("fetched_at")).toString();
    if (!fetchedAt.isEmpty())
        bits << QStringLiteral("数据抓取于 %1").arg(fetchedAt);
    bits << QStringLiteral("收录 %1 个模型 / %2 家供应商")
                .arg(metaEntryCount(m_meta, QStringLiteral("models")))
                .arg(metaEntryCount(m_meta, QStringLiteral("providers")));
    const int hosts = metaEntryCount(m_meta, QStringLiteral("hosts"));
    if (hosts > 0)
        bits << QStringLiteral("%1 个主机映射").arg(hosts);
    return bits.join(QStringLiteral(" · "));
}

// 模型 ID -> 上限与能力项：先看「读取模型」拿到的候选，再看模型信息快照
QVariantMap SettingsDialog::modelLimits(const QString &providerId, const QString &modelId) const
{
    if (modelId.isEmpty())
        return QVariantMap();
    const QVariantList candidates = m_discovered.value(providerId).toList();
    for (int i = 0; i < candidates.size(); ++i) {
        const QVariantMap item = candidates.at(i).toMap();
        QString id = item.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            id = item.value(QStringLiteral("model_id")).toString();
        if (id == modelId)
            return item;
    }
    return metaLimitsFor(modelId);
}

QVariantMap SettingsDialog::metaLimitsFor(const QString &modelId) const
{
    if (modelId.isEmpty() || m_meta.isEmpty())
        return QVariantMap();
    // 状态接口给的是计数；按模型的上限由宿主另行喂进 "limits"
    // （模型 ID -> {context_window, max_output_tokens, capabilities}），
    // 或直接给一份 "models" 数组
    const QVariantMap limits = m_meta.value(QStringLiteral("limits")).toMap();
    if (limits.contains(modelId))
        return limits.value(modelId).toMap();
    const QVariantList items = m_meta.value(QStringLiteral("models")).toList();
    for (int i = 0; i < items.size(); ++i) {
        const QVariantMap item = items.at(i).toMap();
        if (item.value(QStringLiteral("id")).toString() == modelId)
            return item;
    }
    return QVariantMap();
}

bool SettingsDialog::applyStoredModelLimits()
{
    bool changed = false;
    for (int i = 0; i < m_models.size(); ++i) {
        QVariantMap item = m_models.at(i).toMap();
        const QVariantMap limits = modelLimits(item.value(QStringLiteral("provider")).toString(),
                                               item.value(QStringLiteral("id")).toString());
        if (applyModelLimits(item, limits)) {
            m_models[i] = item;
            changed = true;
        }
    }
    return changed;
}

void SettingsDialog::setProviderBusy(bool testing, bool reading)
{
    m_testing = testing;
    m_reading = reading;
    m_readButton->setEnabled(!reading);
    m_testButton->setEnabled(!testing);
    renderProviderEditor();
}

void SettingsDialog::setSkills(const QVariantList &skills, bool loading, const QString &error)
{
    m_skills = skills;
    m_skillsLoading = loading;
    m_skillsError = error;
    m_refreshSkills->setEnabled(!loading);
    renderSkills();
}

void SettingsDialog::setMcpTools(const QVariantList &tools, bool connected, bool loading,
                                 const QString &error)
{
    m_tools = tools;
    m_toolsConnected = connected;
    m_toolsLoading = loading;
    m_toolsError = error;
    m_refreshMcp->setEnabled(!loading);
    renderMcpTools();
}

void SettingsDialog::switchTab(const QString &tab)
{
    m_tab = tab;
    m_tabModels->setProperty("active", tab == QLatin1String("models"));
    m_tabSkills->setProperty("active", tab == QLatin1String("skills"));
    m_tabMcp->setProperty("active", tab == QLatin1String("mcp"));
    m_tabMemory->setProperty("active", tab == QLatin1String("memory"));
    m_tabKb->setProperty("active", tab == QLatin1String("kb"));
    m_tabUsage->setProperty("active", tab == QLatin1String("usage"));
    restyle(m_tabModels);
    restyle(m_tabSkills);
    restyle(m_tabMcp);
    restyle(m_tabMemory);
    restyle(m_tabKb);
    restyle(m_tabUsage);
    // 页序：0 模型 / 1 技能 / 2 MCP / 3 记忆 / 4 知识库 / 5 用量
    int index = 0;
    if (tab == QLatin1String("skills"))
        index = 1;
    else if (tab == QLatin1String("mcp"))
        index = 2;
    else if (tab == QLatin1String("memory"))
        index = 3;
    else if (tab == QLatin1String("kb"))
        index = 4;
    else if (tab == QLatin1String("usage"))
        index = 5;
    m_stack->setCurrentIndex(index);

    // webui switchSettingsTab：离开记忆页就停轮询
    if (m_memoryPanel && tab != QLatin1String("memory"))
        m_memoryPanel->leaveTab();

    m_saveButton->setVisible(tab == QLatin1String("models"));
    if (tab == QLatin1String("mcp"))
        emit refreshMcpRequested();
    else if (tab == QLatin1String("skills"))
        renderSkills();
    else if (tab == QLatin1String("usage") && m_usagePanel)
        m_usagePanel->enterTab(); // 首次进入懒加载（与 MCP Tab 的既有做法一致）
    else if (tab == QLatin1String("memory") && m_memoryPanel)
        m_memoryPanel->enterTab();
    else if (tab == QLatin1String("kb") && m_kbPanel)
        m_kbPanel->enterTab();
}

void SettingsDialog::setUsageCatalog(const QVariantList &models)
{
    if (m_usagePanel)
        m_usagePanel->setCatalogModels(models);
}

void SettingsDialog::setUsageStats(const QString &requestId, const QVariantMap &data)
{
    if (m_usagePanel)
        m_usagePanel->setStats(requestId, data);
}

void SettingsDialog::setUsageLoadFailed(const QString &requestId, const QString &error)
{
    if (m_usagePanel)
        m_usagePanel->setLoadFailed(requestId, error);
}

void SettingsDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}

void SettingsDialog::finishSaved()
{
    m_dirty = false;
    m_saveButton->setEnabled(true);
    done(QDialog::Accepted);
}

void SettingsDialog::markDirty()
{
    m_dirty = true;
    setStatus(QStringLiteral("有未保存的修改"));
}

// ------------------------------------------------------------ 渲染

void SettingsDialog::renderSettings()
{
    renderProviderList();
    renderProviderEditor();
}

// ------------------------------------------------------------ 供应商侧栏分组
// app.js PROVIDER_GROUPS / providerGroupKey：配置里唯一的分类轴是 default_api，
// 就按它归组，不另造一个供应商类型字段；前缀判定用 startsWith（对应 indexOf(...) === 0）
struct ProviderGroupDef
{
    const char *key;
    const char *label;
};

static const ProviderGroupDef kProviderGroups[] = {
    {"openai", "OpenAI 协议"},
    {"anthropic", "Anthropic 协议"},
    {"other", "其它协议"},
};

static QString providerGroupKey(const QVariantMap &provider)
{
    const QString api = provider.value(QStringLiteral("default_api")).toString();
    if (api.startsWith(QLatin1String("anthropic")))
        return QStringLiteral("anthropic");
    if (api.startsWith(QLatin1String("openai")))
        return QStringLiteral("openai");
    return QStringLiteral("other");
}

void SettingsDialog::renderProviderList()
{
    clearLayout(m_providerNavLayout);
    // .provider-group-label{padding:0 8px 6px;color:var(--muted-2);font-size:9px;font-weight:700}
    // 样式集中化那条线只管模型页，侧栏分组还没进它，先在本地按当前调色板生成
    // （只落在这一批标签上，不碰 applyModelStyle 的模型页局部表；
    // 每次 renderProviderList 重算一遍，换肤后重开设置即跟着变）
    QString labelStyle = QStringLiteral(
        "QLabel.providerGroupLabel { color: %1; font-size: %2px; font-weight: 700;"
        " background: transparent; padding: 0 %3px %4px %5px; }");
    labelStyle = labelStyle.arg(cssColor(gs::palette().muted2))
                     .arg(gs::scaledPx(9))
                     .arg(gs::scaledPx(8))
                     .arg(gs::scaledPx(6))
                     .arg(gs::scaledPx(8));

    // 一项：名称 + ID + 模型数，点一下切当前供应商（与分组前一致）
    auto makeItem = [this](const QString &id, const QVariantMap &provider) {
        auto *item = new QWidget;
        setClass(item, QStringLiteral("providerNavItem"));
        item->setAttribute(Qt::WA_StyledBackground, true);
        item->setCursor(Qt::PointingHandCursor);
        item->setProperty("providerId", id);
        item->installEventFilter(this);
        auto *il = new QHBoxLayout(item);
        il->setContentsMargins(8, 9, 8, 9);
        il->setSpacing(8);
        auto *textBox = new QVBoxLayout;
        textBox->setContentsMargins(0, 0, 0, 0);
        textBox->setSpacing(2);
        textBox->addWidget(plainLabel(QStringLiteral("providerName"),
                                      provider.value(QStringLiteral("name")).toString()));
        textBox->addWidget(plainLabel(QStringLiteral("providerId"), id));
        il->addLayout(textBox, 1);
        auto *count = plainLabel(QStringLiteral("providerCount"),
                                 QString::number(providerModels(id).size()));
        count->setMinimumWidth(22);
        count->setAlignment(Qt::AlignCenter);
        il->addWidget(count, 0, Qt::AlignVCenter);
        if (id == m_activeProviderId)
            item->setProperty("active", true);
        return item;
    };

    int shown = 0;
    for (const ProviderGroupDef &group : kProviderGroups) {
        const QString key = QLatin1String(group.key);
        // 组内顺序 = 配置里的 providers 顺序（webui 的 filter 不改序），
        // 组间顺序 = kProviderGroups，与 default_api 无关
        QStringList ids;
        QList<QVariantMap> items;
        for (int i = 0; i < m_providers.size(); ++i) {
            const QVariantMap provider = m_providers.at(i).toMap();
            if (providerGroupKey(provider) != key)
                continue;
            ids.append(provider.value(QStringLiteral("id")).toString());
            items.append(provider);
        }
        if (ids.isEmpty())
            continue; // 空组不显示

        auto *box = new QWidget;
        setClass(box, QStringLiteral("providerGroup"));
        auto *gl = new QVBoxLayout(box);
        // .provider-group+.provider-group{margin-top:16px}
        gl->setContentsMargins(0, shown == 0 ? 0 : gs::scaledPx(16), 0, 0);
        gl->setSpacing(0);
        QLabel *label = plainLabel(QStringLiteral("providerGroupLabel"),
                                   QString::fromUtf8(group.label));
        label->setStyleSheet(labelStyle);
        gl->addWidget(label);
        for (int i = 0; i < ids.size(); ++i)
            gl->addWidget(makeItem(ids.at(i), items.at(i)));
        m_providerNavLayout->addWidget(box);
        ++shown;
    }
    m_providerNavLayout->addStretch(1);
}

void SettingsDialog::renderProviderEditor()
{
    const int index = providerIndex(m_activeProviderId);
    const bool hasProvider = index >= 0;
    m_placeholder->setVisible(!hasProvider);
    m_sectionProvider->setVisible(hasProvider);
    m_sectionModels->setVisible(hasProvider);
    if (m_sectionMeta)
        m_sectionMeta->setVisible(hasProvider);
    renderModelMeta();
    if (!hasProvider)
        return;

    const QVariantMap provider = m_providers.at(index).toMap();
    const QVariantList models = providerModels(m_activeProviderId);
    const QString apiKey = provider.value(QStringLiteral("api_key")).toString();
    const bool masked = apiKey == QLatin1String("********");
    const QString keyEnv = provider.value(QStringLiteral("api_key_env")).toString();

    m_updating = true;
    m_editorTitle->setText(provider.value(QStringLiteral("name")).toString());
    const QVariant enabled = provider.value(QStringLiteral("enabled"));
    m_enabledCheck->setChecked(!enabled.isValid() || enabled.toBool());
    m_nameEdit->setText(provider.value(QStringLiteral("name")).toString());
    m_idEdit->setText(provider.value(QStringLiteral("id")).toString());
    selectComboData(m_discoveryCombo,
                    provider.value(QStringLiteral("discovery_api")).toString().isEmpty()
                        ? QStringLiteral("openai")
                        : provider.value(QStringLiteral("discovery_api")).toString());
    m_discoveryCombo->setEnabled(models.isEmpty());
    m_baseUrlEdit->setText(provider.value(QStringLiteral("base_url")).toString());
    m_keyEnvEdit->setText(keyEnv);
    m_apiKeyEdit->setText(masked ? QString() : apiKey);
    m_apiKeyEdit->setPlaceholderText(masked ? QStringLiteral("已安全保存")
                                            : QStringLiteral("输入 API Key"));
    m_apiKeyEdit->setEnabled(keyEnv.trimmed().isEmpty());
    selectComboData(m_defaultApiCombo,
                    provider.value(QStringLiteral("default_api")).toString());
    // app.js：false/"false" → 跳过验证；其余非布尔值当路径 → 自定义
    const QVariant sslValue = provider.value(QStringLiteral("ssl_verify"));
    const QString sslText = sslValue.toString();
    QString sslMode = QStringLiteral("on");
    if (sslValue.isValid()) {
        if (sslValue.type() == QVariant::Bool)
            sslMode = sslValue.toBool() ? QStringLiteral("on") : QStringLiteral("off");
        else if (sslText == QLatin1String("false"))
            sslMode = QStringLiteral("off");
        else if (sslText != QLatin1String("true"))
            sslMode = QStringLiteral("custom");
    }
    selectComboData(m_sslCombo, sslMode);
    const bool sslCustom = sslMode == QLatin1String("custom");
    if (sslCustom)
        m_sslPathMemory = sslText;
    m_sslPathEdit->setText(sslCustom ? sslText : QString());
    // 显隐变了就得重排一次，否则藏起来的字段还在网格里占着位
    if (m_sslPathRow->isHidden() == sslCustom) {
        m_sslPathRow->setVisible(sslCustom);
        if (m_providerGrid)
            placeFormFields(m_providerGrid, m_providerFields, m_compact);
    }
    m_modelsCount->setText(QString::number(models.size()));
    m_testButton->setText(m_testing ? QStringLiteral("测试中…") : QStringLiteral("测试连接"));
    m_readButton->setText(m_reading ? QStringLiteral("读取中…") : QStringLiteral("读取模型"));
    m_updating = false;

    clearLayout(m_modelListLayout);
    m_modelRows.clear();
    m_modelCapsRows.clear();
    for (int i = 0; i < models.size(); ++i)
        renderModelCard(models.at(i).toMap());
    renderCandidates();
}

void SettingsDialog::renderModelCard(const QVariantMap &model)
{
    const QString provider = model.value(QStringLiteral("provider")).toString();
    const QString id = model.value(QStringLiteral("id")).toString();
    const QString key = provider + QLatin1Char('/') + id;
    const QString displayName = model.value(QStringLiteral("name")).toString().isEmpty()
                                    ? id
                                    : model.value(QStringLiteral("name")).toString();
    const QVariantMap capabilities = model.value(QStringLiteral("capabilities")).toMap();
    const QVariantMap limits = modelLimits(provider, id);
    const int ownContext = model.value(QStringLiteral("context_window")).toInt();
    const int contextLimit = limits.value(QStringLiteral("context_window")).toInt();
    // 上下文 chip：优先用配置里的 context_window，没有就用快照 / 候选里的值（弱化样式）
    const int chipValue = ownContext > 0 ? ownContext : contextLimit;

    auto *card = new ExpandCard(QStringLiteral("settingsModel"),
                                QStringLiteral("settingsModelSummary"),
                                QStringLiteral("modelAdvanced"));

    // ---- summary：名称/ID → 能力图标 → 上下文 chip → API 协议 → 删除 ----
    auto *summary = new QHBoxLayout;
    summary->setContentsMargins(10, 9, 10, 9);
    summary->setSpacing(10);
    auto *textBox = new QVBoxLayout;
    textBox->setContentsMargins(0, 0, 0, 0);
    textBox->setSpacing(0);
    textBox->addWidget(plainLabel(QStringLiteral("settingsModelName"), displayName));
    textBox->addWidget(plainLabel(QStringLiteral("settingsModelId"), id));
    summary->addLayout(textBox, 1);

    // 能力小图标：只画已开启的，一项都没开就整段不出现。
    // webui 用内联 SVG；Qt 侧走 resources/icons 里的 cap-*，13px 单色描边
    auto *capsRow = new QWidget(card->summary());
    auto *capsLayout = new QHBoxLayout(capsRow);
    capsLayout->setContentsMargins(0, 0, 0, 0);
    capsLayout->setSpacing(gs::scaledPx(7));
    capsRow->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    summary->addWidget(capsRow, 0, Qt::AlignVCenter);
    m_modelCapsRows.append(capsRow);

    QLabel *chipLabel = nullptr;
    if (chipValue > 0) {
        chipLabel = plainLabel(ownContext > 0 ? QStringLiteral("modelCtx")
                                             : QStringLiteral("modelCtx modelCtxFallback"),
                               formatTokens(chipValue));
        if (ownContext <= 0)
            chipLabel->setToolTip(QStringLiteral("来自模型信息，尚未写入配置"));
        summary->addWidget(chipLabel, 0, Qt::AlignVCenter);
    }
    const QPointer<QLabel> chip(chipLabel);

    const QString api = model.value(QStringLiteral("api")).toString();
    summary->addWidget(plainLabel(QStringLiteral("settingsModelApi"),
                                  api.isEmpty() ? QStringLiteral("继承供应商") : api),
                       0, Qt::AlignVCenter);
    // 行尾折叠箭头（webui `.settings-model>summary::after`）：收起朝右、展开朝下。
    // 箭头不自己记状态，只镜像 ExpandCard 的开合，避免两处状态走偏
    auto *chevron = new Chevron(card->summary());
    summary->addWidget(chevron, 0, Qt::AlignVCenter);
    connect(card, &ExpandCard::openChanged, chevron, &Chevron::setOpen);
    auto *remove = new IconPushButton(card->summary());
    setClass(remove, QStringLiteral("modelDelete"));
    remove->setFixedSize(22, 22);
    remove->setCursor(Qt::PointingHandCursor);
    remove->setFocusPolicy(Qt::TabFocus);
    remove->setIconColors(QColor(QStringLiteral("#8da0a9")),
                          QColor(QStringLiteral("#e36c6c")));
    remove->setIconName(QStringLiteral("x"), 12);
    summary->addWidget(remove, 0, Qt::AlignVCenter);
    card->summaryLayout()->addLayout(summary);

    auto *body = card->bodyLayout();
    body->setContentsMargins(12, 0, 12, 12);
    body->setSpacing(0);

    // 能力图标随开关实时重建（webui 只在整块重渲染时刷新，这里做得更跟手）
    auto refreshCaps = [this, capsRow, capsLayout, key] {
        while (QLayoutItem *item = capsLayout->takeAt(0)) {
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        const int index = modelIndex(key);
        const QVariantMap caps = index >= 0
                                     ? m_models.at(index).toMap()
                                           .value(QStringLiteral("capabilities"))
                                           .toMap()
                                     : QVariantMap();
        QStringList labels;
        for (int i = 0; i < 4; ++i) {
            const QString capKey = QLatin1String(kModelCapabilities[i].key);
            if (!caps.value(capKey).toBool())
                continue;
            auto *icon = plainLabel(QString(), QString());
            icon->setPixmap(iconPixmap(QLatin1String(kModelCapabilities[i].icon),
                                       gs::palette().muted, gs::scaledPx(13)));
            icon->setFixedSize(gs::scaledPx(13), gs::scaledPx(13));
            capsLayout->addWidget(icon);
            labels << QString::fromUtf8(kModelCapabilities[i].label);
        }
        capsRow->setProperty("hasCaps", !labels.isEmpty());
        capsRow->setToolTip(labels.isEmpty()
                                ? QString()
                                : QStringLiteral("已启用：%1").arg(labels.join(QStringLiteral(" · "))));
        capsRow->setVisible(!labels.isEmpty() && !m_compact);
    };
    refreshCaps();

    // 行序与 webui 一致：ID → 显示名称 → API 协议 → 上下文/输出（两列）
    //                     → 思考强度 → 四个能力开关
    auto *idRow = new ModelRow(card->body());
    auto *idText = plainLabel(QStringLiteral("modelId"), id);
    idText->setWordWrap(true);
    idText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    idText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    idRow->addField(QStringLiteral("ID"), QString(), idText);
    body->addWidget(idRow);
    m_modelRows.append(idRow);

    auto *nameRow = new ModelRow(card->body());
    auto *nameEdit = settingsInput(QString());
    nameEdit->setText(model.value(QStringLiteral("name")).toString());
    nameRow->addField(QStringLiteral("显示名称"), QString(), nameEdit);
    body->addWidget(nameRow);
    m_modelRows.append(nameRow);

    auto *apiRow = new ModelRow(card->body());
    auto *apiCombo = settingsCombo();
    apiCombo->addItem(QStringLiteral("自动（供应商默认）"), QString());
    apiCombo->addItem(QStringLiteral("OpenAI Chat"), QStringLiteral("openai-chat"));
    apiCombo->addItem(QStringLiteral("OpenAI Responses"), QStringLiteral("openai-responses"));
    apiCombo->addItem(QStringLiteral("Anthropic Messages"),
                      QStringLiteral("anthropic-messages"));
    selectComboData(apiCombo, api);
    apiRow->addField(QStringLiteral("API 协议"),
                     QStringLiteral("仅覆盖此模型的 API 协议；自动模式沿用供应商默认值。"),
                     apiCombo);
    body->addWidget(apiRow);
    m_modelRows.append(apiRow);

    auto *limitRow = new ModelRow(card->body());
    // 双列行（.srow-2）：宽屏两字段并排，窄屏并单列。测试按名字取它验重排
    limitRow->setObjectName(QStringLiteral("modelLimitRow"));
    auto *contextField = new NumField(contextPresets());
    contextField->setValue(ownContext);
    contextField->setPlaceholder(autoLimitText(limits, QStringLiteral("context_window")));
    auto *outputField = new NumField(outputPresets());
    outputField->setValue(model.value(QStringLiteral("max_output_tokens")).toInt());
    outputField->setPlaceholder(autoLimitText(limits, QStringLiteral("max_output_tokens")));
    limitRow->addField(QStringLiteral("上下文长度"), QString(), contextField);
    limitRow->addField(QStringLiteral("最大输出"), QString(), outputField);
    body->addWidget(limitRow);
    m_modelRows.append(limitRow);

    auto *thinkRow = new ModelRow(card->body());
    auto *thinkCombo = settingsCombo();
    thinkCombo->addItem(QStringLiteral("默认（不干预）"), QString());
    thinkCombo->addItem(QStringLiteral("关闭思考"), QStringLiteral("off"));
    thinkCombo->addItem(QStringLiteral("最小"), QStringLiteral("minimal"));
    thinkCombo->addItem(QStringLiteral("低"), QStringLiteral("low"));
    thinkCombo->addItem(QStringLiteral("中"), QStringLiteral("medium"));
    thinkCombo->addItem(QStringLiteral("高"), QStringLiteral("high"));
    // app.js：compat.thinking == "disabled" 显示「关闭思考」，否则用 compat.reasoning_effort
    const QVariantMap compat = model.value(QStringLiteral("compat")).toMap();
    QString thinkingMode = compat.value(QStringLiteral("thinking")).toString()
                                   == QLatin1String("disabled")
                               ? QStringLiteral("off")
                               : compat.value(QStringLiteral("reasoning_effort")).toString();
    const QStringList thinkingValues = {QString(), QStringLiteral("off"), QStringLiteral("minimal"),
                                       QStringLiteral("low"), QStringLiteral("medium"),
                                       QStringLiteral("high")};
    if (!thinkingValues.contains(thinkingMode))
        thinkingMode.clear();
    selectComboData(thinkCombo, thinkingMode);
    thinkRow->addField(QStringLiteral("思考强度"),
                       QStringLiteral("控制该模型的推理档位；默认不干预。"), thinkCombo);
    body->addWidget(thinkRow);
    m_modelRows.append(thinkRow);

    // .settings-toggle：四项能力开关（视觉 / 视频 / 音频 / 推理）
    for (int i = 0; i < 4; ++i) {
        const QString capKey = QLatin1String(kModelCapabilities[i].key);
        auto *row = new QWidget(card->body());
        setClass(row, i == 3 ? QStringLiteral("settingsToggle settingsToggleLast")
                             : QStringLiteral("settingsToggle"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        row->setMinimumHeight(gs::scaledPx(50));
        auto *box = new QHBoxLayout(row);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(gs::scaledPx(14));
        box->addWidget(plainLabel(QStringLiteral("toggleLabel"),
                                  QString::fromUtf8(kModelCapabilities[i].label)),
                       1);
        auto *toggle = new SwitchToggle(row);
        toggle->setChecked(capabilities.value(capKey).toBool());
        toggle->onToggled = [this, key, capKey, refreshCaps](bool on) {
            const int index = modelIndex(key);
            if (index < 0)
                return;
            QVariantMap item = m_models.at(index).toMap();
            QVariantMap caps = item.value(QStringLiteral("capabilities")).toMap();
            caps.insert(capKey, on);
            item.insert(QStringLiteral("capabilities"), caps);
            m_models[index] = item;
            // 用户改过能力项：之后不再让快照自动勾回（webui markCapsTouched）
            capsTouchedSet().insert(key);
            markDirty();
            refreshCaps();
        };
        box->addWidget(toggle, 0, Qt::AlignVCenter);
        body->addWidget(row);
    }

    // 数值框统一走这里：空即删键（webui 的 `delete model[name]`）
    auto writeNumber = [this, key, chip](const QString &field, const QString &text) {
        const int index = modelIndex(key);
        if (index < 0)
            return;
        QVariantMap item = m_models.at(index).toMap();
        const int value = text.trimmed().toInt();
        if (value > 0)
            item.insert(field, value);
        else
            item.remove(field);
        m_models[index] = item;
        markDirty();
        // 折叠行上的 chip 跟着新值走（没有整块重渲染，那一格不会自己更新）
        if (chip && field == QLatin1String("context_window") && value > 0) {
            chip->setText(formatTokens(value));
            setClass(chip.data(), QStringLiteral("modelCtx"));
            chip->setToolTip(QString());
            gs::restyle(chip.data());
        }
    };

    auto writeField = [this, key](const QString &field, const QVariant &value) {
        const int index = modelIndex(key);
        if (index < 0)
            return;
        QVariantMap item = m_models.at(index).toMap();
        item.insert(field, value);
        m_models[index] = item;
        markDirty();
    };
    connect(nameEdit, &QLineEdit::textChanged, this, [writeField](const QString &text) {
        writeField(QStringLiteral("name"), text.isEmpty() ? QVariant() : QVariant(text));
    });
    connect(contextField->input(), &QLineEdit::textChanged, this,
            [writeNumber](const QString &text) {
        writeNumber(QStringLiteral("context_window"), text);
    });
    connect(outputField->input(), &QLineEdit::textChanged, this,
            [writeNumber](const QString &text) {
        writeNumber(QStringLiteral("max_output_tokens"), text);
    });
    connect(apiCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [writeField, apiCombo](int) {
        const QString value = comboData(apiCombo);
        writeField(QStringLiteral("api"), value.isEmpty() ? QVariant() : QVariant(value));
    });
    // 思考强度：off → compat.thinking="disabled"；其余写 compat.reasoning_effort；
    // 空档位两个键都删掉（webui 原样）
    connect(thinkCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, key, thinkCombo](int) {
        const int index = modelIndex(key);
        if (index < 0)
            return;
        QVariantMap item = m_models.at(index).toMap();
        QVariantMap compat = item.value(QStringLiteral("compat")).toMap();
        compat.remove(QStringLiteral("thinking"));
        compat.remove(QStringLiteral("reasoning_effort"));
        const QString value = comboData(thinkCombo);
        if (value == QLatin1String("off"))
            compat.insert(QStringLiteral("thinking"), QStringLiteral("disabled"));
        else if (!value.isEmpty())
            compat.insert(QStringLiteral("reasoning_effort"), value);
        item.insert(QStringLiteral("compat"), compat);
        m_models[index] = item;
        markDirty();
    });
    connect(remove, &QPushButton::clicked, this, [this, key] {
        const int index = modelIndex(key);
        if (index < 0)
            return;
        m_models.removeAt(index);
        if (m_defaultModel == key) {
            m_defaultModel.clear();
            for (int i = 0; i < m_models.size(); ++i) {
                const QVariantMap item = m_models.at(i).toMap();
                const QVariant on = item.value(QStringLiteral("enabled"));
                if (!on.isValid() || on.toBool()) {
                    m_defaultModel = item.value(QStringLiteral("provider")).toString()
                        + QLatin1Char('/') + item.value(QStringLiteral("id")).toString();
                    break;
                }
            }
        }
        markDirty();
        m_openModelKeys.remove(key);
        renderSettings();
    });

    // 重渲染后恢复展开态：宿主回填 limits（setModelMetadata → renderProviderEditor）
    // 走的就是重建这条路，不记着的话用户刚展开的那张会自己收回去，
    // 数值框占位文案（autoLimitText）的新值也就看不到了
    connect(card, &ExpandCard::openChanged, this, [this, key](bool open) {
        if (open)
            m_openModelKeys.insert(key);
        else
            m_openModelKeys.remove(key);
    });
    if (m_openModelKeys.contains(key))
        card->setOpen(true);

    m_modelListLayout->addWidget(card);
}

void SettingsDialog::renderCandidates()
{
    clearLayout(m_candidateLayout);
    const QVariantList candidates = m_discovered.value(m_activeProviderId).toList();
    QSet<QString> added;
    const QVariantList models = providerModels(m_activeProviderId);
    for (int i = 0; i < models.size(); ++i)
        added.insert(models.at(i).toMap().value(QStringLiteral("id")).toString());
    const QString query = m_candidateSearch->text().trimmed().toLower();

    int shown = 0;
    for (int i = 0; i < candidates.size(); ++i) {
        const QVariantMap item = candidates.at(i).toMap();
        QString id = item.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            id = item.value(QStringLiteral("model_id")).toString();
        if (!query.isEmpty() && !id.toLower().contains(query))
            continue;
        const bool exists = added.contains(id);
        QString name = item.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            name = id;
        // 行内「输入/输出上限」（webui renderCandidates 的 limits）
        QStringList limits;
        const int contextWindow = item.value(QStringLiteral("context_window")).toInt();
        const int maxOutput = item.value(QStringLiteral("max_output_tokens")).toInt();
        if (contextWindow > 0)
            limits << QStringLiteral("%1 输入").arg(formatTokens(contextWindow));
        if (maxOutput > 0)
            limits << QStringLiteral("%1 输出").arg(formatTokens(maxOutput));

        auto *button = new QPushButton;
        setClass(button, QStringLiteral("candidateItem"));
        button->setEnabled(!exists);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::TabFocus);
        auto *bl = new QHBoxLayout(button);
        bl->setContentsMargins(6, 6, 6, 6);
        bl->setSpacing(6);
        auto *mark = plainLabel(QStringLiteral("candidateName"), QString());
        mark->setFixedWidth(18);
        mark->setAlignment(Qt::AlignCenter);
        mark->setPixmap(iconPixmap(exists ? QStringLiteral("check") : QStringLiteral("plus"),
                                   gs::palette().cyan, 11));
        bl->addWidget(mark);
        bl->addWidget(plainLabel(QStringLiteral("candidateName"), name), 1);
        auto *right = new QWidget(button);
        auto *rightLayout = new QHBoxLayout(right);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(8);
        rightLayout->addWidget(plainLabel(QStringLiteral("candidateId"), id), 0,
                               Qt::AlignVCenter);
        if (!limits.isEmpty()) {
            rightLayout->addWidget(plainLabel(QStringLiteral("candidateLimit"),
                                             limits.join(QStringLiteral(" · "))),
                                   0, Qt::AlignVCenter);
        }
        bl->addWidget(right, 0, Qt::AlignVCenter);
        connect(button, &QPushButton::clicked, this, [this, id, name] { addModel(id, name); });
        m_candidateLayout->addWidget(button);
        ++shown;
    }
    if (!shown)
        m_candidateLayout->addWidget(emptyHint(QStringLiteral("暂无候选，可手动添加")));
    m_candidateLayout->addStretch(1);
}

void SettingsDialog::renderSkills()
{
    m_skillCount->setText(QString::number(m_skills.size()));
    if (m_skillsLoading)
        m_skillsStatus->setText(QStringLiteral("正在读取技能…"));
    else if (!m_skillsError.isEmpty())
        m_skillsStatus->setText(QStringLiteral("技能读取失败：%1").arg(m_skillsError));
    else
        m_skillsStatus->setText(QStringLiteral("共 %1 个技能").arg(m_skills.size()));

    clearLayout(m_skillsLayout);
    if (m_skillsLoading && m_skills.isEmpty()) {
        m_skillsLayout->addStretch(1);
        return;
    }
    if (m_skills.isEmpty()) {
        m_skillsLayout->addWidget(emptyHint(QStringLiteral("暂无可用技能")));
        m_skillsLayout->addStretch(1);
        return;
    }

    for (int i = 0; i < m_skills.size(); ++i) {
        const QVariantMap skill = m_skills.at(i).toMap();
        const QString id = skill.value(QStringLiteral("id")).toString();
        QString name = skill.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            name = id;
        const QVariantList tools = skill.value(QStringLiteral("allowed_tools")).toList();
        const QVariantList shadowed = skill.value(QStringLiteral("shadowed")).toList();

        auto *card = new ExpandCard(QStringLiteral("skillCard"),
                                    QStringLiteral("skillCardSummary"), QString());
        auto *summary = new QHBoxLayout;
        summary->setContentsMargins(10, 9, 10, 9);
        summary->setSpacing(10);
        auto *nameLabel = plainLabel(QStringLiteral("skillName"), name);
        nameLabel->setWordWrap(true);
        summary->addWidget(nameLabel, 1);
        const QString version = skill.value(QStringLiteral("version")).toString();
        if (!version.isEmpty())
            summary->addWidget(plainLabel(QStringLiteral("skillVersion"),
                                          QStringLiteral("v%1").arg(version)),
                               0, Qt::AlignVCenter);
        summary->addWidget(plainLabel(QStringLiteral("skillSource"),
                                      skill.value(QStringLiteral("source")).toString().toUpper()),
                           0, Qt::AlignVCenter);
        summary->addWidget(plainLabel(QStringLiteral("skillSource"),
                                      QStringLiteral("%1 工具").arg(tools.size())),
                           0, Qt::AlignVCenter);
        card->summaryLayout()->addLayout(summary);

        auto *body = card->bodyLayout();
        const QString description = skill.value(QStringLiteral("description")).toString();
        if (!description.isEmpty()) {
            auto *desc = makeLabel(QStringLiteral("skillDesc"), description);
            desc->setWordWrap(true);
            body->addWidget(desc);
        }
        auto *meta = new QWidget(card->body());
        auto *ml = new QHBoxLayout(meta);
        ml->setContentsMargins(10, 0, 10, 9);
        ml->setSpacing(4);
        ml->addWidget(plainLabel(QStringLiteral("skillMetaText"), QStringLiteral("ID")));
        ml->addWidget(plainLabel(QStringLiteral("paramCode"), id));
        ml->addStretch(1);
        body->addWidget(meta);

        if (!tools.isEmpty()) {
            auto *strip = styledWidget(QStringLiteral("codeChipStrip"), card->body());
            auto *flow = new FlowLayout(strip, 0, 8, 7);
            flow->setContentsMargins(10, 2, 10, 9);
            for (int t = 0; t < tools.size(); ++t)
                flow->addWidget(plainLabel(QStringLiteral("codeChip"),
                                           tools.at(t).toString()));
            body->addWidget(strip);
        } else {
            body->addWidget(makeLabel(QStringLiteral("skillDesc"),
                                      QStringLiteral("未限定可用工具")));
        }

        if (!shadowed.isEmpty()) {
            QStringList parts;
            for (int s = 0; s < shadowed.size(); ++s) {
                const QVariantMap item = shadowed.at(s).toMap();
                QString text = item.value(QStringLiteral("source")).toString();
                const QString version = item.value(QStringLiteral("version")).toString();
                if (!version.isEmpty())
                    text += QStringLiteral(" v%1").arg(version);
                parts << text;
            }
            auto *note = makeLabel(QStringLiteral("skillShadowed"),
                                   QStringLiteral("覆盖了 %1 个同名来源：%2")
                                       .arg(shadowed.size())
                                       .arg(parts.join(QStringLiteral("、"))));
            note->setWordWrap(true);
            body->addWidget(note);
        }
        m_skillsLayout->addWidget(card);
    }
    m_skillsLayout->addStretch(1);
}

void SettingsDialog::renderMcpTools()
{
    m_mcpCount->setText(QString::number(m_tools.size()));
    if (m_toolsLoading && m_tools.isEmpty())
        m_mcpStatus->setText(QStringLiteral("正在读取 MCP 工具…"));
    else if (!m_toolsError.isEmpty())
        m_mcpStatus->setText(QStringLiteral("MCP 读取失败：%1").arg(m_toolsError));
    else if (m_toolsConnected)
        m_mcpStatus->setText(QStringLiteral("已连接 · 共 %1 个工具").arg(m_tools.size()));
    else
        m_mcpStatus->setText(QStringLiteral("MCP 服务未连接"));

    clearLayout(m_mcpLayout);
    if (m_toolsLoading && m_tools.isEmpty()) {
        m_mcpLayout->addStretch(1);
        return;
    }
    if (m_tools.isEmpty()) {
        m_mcpLayout->addWidget(emptyHint(QStringLiteral("暂无可用工具")));
        m_mcpLayout->addStretch(1);
        return;
    }

    for (int i = 0; i < m_tools.size(); ++i) {
        const QVariantMap tool = m_tools.at(i).toMap();
        const QVariantList params = schemaParams(tool.value(QStringLiteral("input_schema")).toMap());
        auto *card = new ExpandCard(QStringLiteral("mcpTool"),
                                    QStringLiteral("mcpToolSummary"), QString());
        auto *summary = new QHBoxLayout;
        summary->setContentsMargins(10, 9, 10, 9);
        summary->setSpacing(10);
        auto *nameLabel = plainLabel(QStringLiteral("skillName"),
                                     tool.value(QStringLiteral("name")).toString());
        nameLabel->setWordWrap(true);
        summary->addWidget(nameLabel, 1);
        summary->addWidget(plainLabel(QStringLiteral("skillSource"),
                                      QStringLiteral("%1 参数").arg(params.size())),
                           0, Qt::AlignVCenter);
        card->summaryLayout()->addLayout(summary);

        auto *body = card->bodyLayout();
        const QString description = tool.value(QStringLiteral("description")).toString();
        if (!description.isEmpty()) {
            auto *desc = makeLabel(QStringLiteral("skillDesc"), description);
            desc->setWordWrap(true);
            body->addWidget(desc);
        }
        if (params.isEmpty()) {
            body->addWidget(makeLabel(QStringLiteral("skillDesc"), QStringLiteral("无参数")));
        } else {
            auto *list = styledWidget(QStringLiteral("mcpParamList"), card->body());
            auto *ll = new QVBoxLayout(list);
            ll->setContentsMargins(10, 2, 10, 9);
            ll->setSpacing(0);
            for (int p = 0; p < params.size(); ++p) {
                const QVariantMap param = params.at(p).toMap();
                auto *row = styledWidget(p + 1 < params.size()
                                             ? QStringLiteral("mcpParamRow")
                                             : QString(),
                                         list);
                auto *rl = new QHBoxLayout(row);
                rl->setContentsMargins(0, 7, 0, 7);
                rl->setSpacing(8);
                rl->addWidget(plainLabel(QStringLiteral("paramCode"),
                                         param.value(QStringLiteral("name")).toString()));
                rl->addWidget(plainLabel(QStringLiteral("mcpParamType"),
                                         param.value(QStringLiteral("type")).toString()));
                if (param.value(QStringLiteral("required")).toBool())
                    rl->addWidget(plainLabel(QStringLiteral("mcpParamRequired"),
                                             QStringLiteral("必填")));
                const QString desc = param.value(QStringLiteral("description")).toString();
                if (!desc.isEmpty()) {
                    auto *descLabel = plainLabel(QStringLiteral("mcpParamDesc"), desc);
                    descLabel->setWordWrap(true);
                    rl->addWidget(descLabel, 1);
                } else {
                    rl->addStretch(1);
                }
                ll->addWidget(row);
            }
            body->addWidget(list);
        }
        m_mcpLayout->addWidget(card);
    }
    m_mcpLayout->addStretch(1);
}

// ------------------------------------------------------------ 操作

int SettingsDialog::providerIndex(const QString &id) const
{
    for (int i = 0; i < m_providers.size(); ++i) {
        if (m_providers.at(i).toMap().value(QStringLiteral("id")).toString() == id)
            return i;
    }
    return -1;
}

int SettingsDialog::modelIndex(const QString &key) const
{
    for (int i = 0; i < m_models.size(); ++i) {
        const QVariantMap item = m_models.at(i).toMap();
        const QString itemKey = item.value(QStringLiteral("provider")).toString()
            + QLatin1Char('/') + item.value(QStringLiteral("id")).toString();
        if (itemKey == key)
            return i;
    }
    return -1;
}

QVariantList SettingsDialog::providerModels(const QString &id) const
{
    QVariantList out;
    for (int i = 0; i < m_models.size(); ++i) {
        const QVariantMap item = m_models.at(i).toMap();
        if (item.value(QStringLiteral("provider")).toString() == id)
            out.append(item);
    }
    return out;
}

void SettingsDialog::addProvider()
{
    QString id = QStringLiteral("custom");
    int suffix = 2;
    while (providerIndex(id) >= 0)
        id = QStringLiteral("custom-%1").arg(suffix++);

    QVariantMap provider;
    provider.insert(QStringLiteral("id"), id);
    // PROVIDER_PRESETS.compatible
    provider.insert(QStringLiteral("name"), QStringLiteral("新供应商"));
    provider.insert(QStringLiteral("base_url"), QString());
    provider.insert(QStringLiteral("default_api"), QStringLiteral("openai-chat"));
    provider.insert(QStringLiteral("discovery_api"), QStringLiteral("openai"));
    provider.insert(QStringLiteral("api_key"), QString());
    provider.insert(QStringLiteral("api_key_env"), QString());
    provider.insert(QStringLiteral("headers"), QVariantMap());
    provider.insert(QStringLiteral("discover_models"), true);
    provider.insert(QStringLiteral("enabled"), true);
    m_providers.append(provider);
    m_activeProviderId = id;
    markDirty();
    renderSettings();
    m_nameEdit->setFocus();
    m_nameEdit->selectAll();
}

void SettingsDialog::deleteProvider()
{
    const int index = providerIndex(m_activeProviderId);
    if (index < 0)
        return;
    if (!providerModels(m_activeProviderId).isEmpty()) {
        setStatus(QStringLiteral("请先删除该供应商下的模型"));
        return;
    }
    if (m_defaultModel.startsWith(m_activeProviderId + QLatin1Char('/'))) {
        setStatus(QStringLiteral("默认模型属于该供应商，无法删除"));
        return;
    }
    m_providers.removeAt(index);
    m_activeProviderId = m_providers.isEmpty()
                             ? QString()
                             : m_providers.first().toMap().value(QStringLiteral("id")).toString();
    markDirty();
    renderSettings();
}

void SettingsDialog::clearApiKey()
{
    const int index = providerIndex(m_activeProviderId);
    if (index < 0)
        return;
    QVariantMap provider = m_providers.at(index).toMap();
    provider.insert(QStringLiteral("api_key"), QString());
    m_providers[index] = provider;
    markDirty();
    renderProviderEditor();
}

void SettingsDialog::addModel(const QString &rawId, const QString &name)
{
    const QString id = rawId.trimmed();
    if (id.isEmpty()) {
        setStatus(QStringLiteral("模型 ID 不得为空"));
        return;
    }
    if (providerIndex(m_activeProviderId) < 0)
        return;
    const QVariantList existing = providerModels(m_activeProviderId);
    for (int i = 0; i < existing.size(); ++i) {
        if (existing.at(i).toMap().value(QStringLiteral("id")).toString() == id) {
            setStatus(QStringLiteral("该模型已添加"));
            return;
        }
    }
    QVariantMap capabilities;
    // webui HEAD 的 addModel：界面四项；后端 schema 里的旧键照样摆上 false，
    // 兼容存量 config.json（tools / parallel_tools / stream_usage）
    for (int i = 0; i < 4; ++i)
        capabilities.insert(QLatin1String(kModelCapabilities[i].key), false);
    for (const char *legacy : kLegacyCapabilityKeys)
        capabilities.insert(QLatin1String(legacy), false);

    QVariantMap model;
    model.insert(QStringLiteral("id"), id);
    model.insert(QStringLiteral("provider"), m_activeProviderId);
    model.insert(QStringLiteral("api"), QVariant());
    model.insert(QStringLiteral("name"), name.isEmpty() ? id : name);
    model.insert(QStringLiteral("enabled"), true);
    model.insert(QStringLiteral("capabilities"), capabilities);
    model.insert(QStringLiteral("compat"), QVariantMap());
    // webui 不再写 32768/4096 这类保守默认值：上限留空，由候选 / 快照当场填好
    // （fillModelLimits）；实在查不到就留空，保存时后端再做一次兜底
    // 上限 / 能力项：候选或快照里有就当场填好（applyModelLimits 只填空字段）
    const QVariantMap known = modelLimits(m_activeProviderId, id);
    applyModelLimits(model, known);
    m_models.append(model);
    if (m_defaultModel.isEmpty())
        m_defaultModel = m_activeProviderId + QLatin1Char('/') + id;
    markDirty();
    renderSettings();
    // webui fillModelLimits：候选 / 快照里连能力项都没有时，再问一次解析接口把缺的补齐。
    // 必须在 model 已进 m_models 之后发——宿主回填走 applyStoredModelLimits，
    // 草稿里没这一条就只能丢掉
    if (!known.contains(QStringLiteral("capabilities")))
        emit modelLimitsResolveRequested(m_activeProviderId, id);
}

bool SettingsDialog::validateSettings()
{
    QStringList errors;
    for (int i = 0; i < m_providers.size(); ++i) {
        const QVariantMap provider = m_providers.at(i).toMap();
        const QUrl url(provider.value(QStringLiteral("base_url")).toString());
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
            errors << QStringLiteral("%1 的 API 地址无效")
                          .arg(provider.value(QStringLiteral("name")).toString());
    }
    for (int i = 0; i < m_models.size(); ++i) {
        const QVariantMap model = m_models.at(i).toMap();
        if (model.value(QStringLiteral("context_window")).toInt()
            < model.value(QStringLiteral("max_output_tokens")).toInt())
            errors << QStringLiteral("%1 的 context window 不能小于最大输出")
                          .arg(model.value(QStringLiteral("id")).toString());
    }
    bool hasDefault = false;
    QString fallback;
    for (int i = 0; i < m_models.size(); ++i) {
        const QVariantMap model = m_models.at(i).toMap();
        const QVariant on = model.value(QStringLiteral("enabled"));
        if (on.isValid() && !on.toBool())
            continue;
        const QString key = model.value(QStringLiteral("provider")).toString()
            + QLatin1Char('/') + model.value(QStringLiteral("id")).toString();
        if (key == m_defaultModel)
            hasDefault = true;
        if (fallback.isEmpty())
            fallback = key;
    }
    if (!hasDefault) {
        if (fallback.isEmpty())
            errors << QStringLiteral("请至少添加一个已启用模型");
        else
            m_defaultModel = fallback;
    }
    setStatus(errors.isEmpty() ? QString() : errors.first());
    return errors.isEmpty();
}

// 未保存修改的二次确认（关闭按钮 / 窗口关闭 / Esc 三条路径共用）
bool SettingsDialog::confirmDiscard()
{
    if (!m_dirty)
        return true;
    return ConfirmDialog::ask(this, QStringLiteral("放弃未保存的设置"),
                              QStringLiteral("模型设置尚未保存，确定要关闭吗？"),
                              QStringLiteral("放弃更改"), true);
}

void SettingsDialog::attemptClose()
{
    if (!confirmDiscard())
        return;
    m_saveButton->setEnabled(true);
    done(QDialog::Rejected);
}

void SettingsDialog::reject()
{
    // QDialog 按 Esc 会走 reject()，不经过 closeEvent——脏检查必须在这里也走一遍，
    // 否则未保存的修改会被无声丢弃
    attemptClose();
}

void SettingsDialog::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscard()) {
        event->ignore();
        return;
    }
    m_saveButton->setEnabled(true);
    event->accept();
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *widget = qobject_cast<QWidget *>(watched);
        const QVariant id = widget ? widget->property("providerId") : QVariant();
        if (id.isValid()) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (widget->rect().contains(me->pos())
                && id.toString() != m_activeProviderId) {
                m_activeProviderId = id.toString();
                m_candidateSearch->clear();
                renderSettings();
            }
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace gs

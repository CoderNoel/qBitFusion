/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  qBitFusion contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "modernwidgets.h"

#include <algorithm>

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>

#include "gui/uithememanager.h"
#include "modernappearance.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    QColor withAlpha(QColor color, const qreal alpha)
    {
        color.setAlphaF(alpha);
        return color;
    }

    // Counts are plain tabular figures in the secondary colour: quiet, and they line up in a column.
    QFont countFont(const QFont &base)
    {
        QFont font = base;
        font.setFeature(QFont::Tag("tnum"), 1);
        return font;
    }

    constexpr qreal RADIUS = 6;
    constexpr int BODY_WIDTH = 380;

    // Same rule as the stylesheet: selection is a light tint of the accent.
    QColor selectionFill(const QPalette &pal)
    {
        return withAlpha(pal.color(QPalette::Active, QPalette::Highlight), ModernAppearance::selectionAlpha());
    }
}

std::pair<QString, QString> Modern::splitCount(const QString &text)
{
    static const QRegularExpression pattern {u"^(.*\\S)\\s*\\((\\d+)\\)$"_s};
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return {text, {}};
    return {match.captured(1), match.captured(2)};
}

// ---------------------------------------------------------------- NavButton

Modern::NavButton::NavButton(const QIcon &icon, const QString &text, QWidget *parent)
    : QAbstractButton(parent)
{
    setIcon(icon);
    setText(text);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setIconSize({16, 16});
    setAttribute(Qt::WA_Hover);
}

void Modern::NavButton::setBadge(const QString &badge)
{
    if (badge == m_badge)
        return;
    m_badge = badge;
    updateGeometry();
    update();
}

QSize Modern::NavButton::sizeHint() const
{
    const QFontMetrics metrics {font()};
    int width = 10 + iconSize().width() + 7 + metrics.horizontalAdvance(text()) + 12;
    if (!m_badge.isEmpty())
        width += 6 + QFontMetrics(countFont(font())).horizontalAdvance(m_badge);
    return {width, std::max(30, metrics.height() + 12)};
}

void Modern::NavButton::paintEvent([[maybe_unused]] QPaintEvent *event)
{
    QPainter painter {this};
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const QRectF area = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    // The current page is a light tint of the accent; hover is a neutral overlay.
    if (isChecked() || m_hovered || isDown())
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(isChecked() ? selectionFill(pal) : withAlpha(pal.color(QPalette::Text), (isDown() ? 0.085 : 0.05)));
        painter.drawRoundedRect(area, RADIUS, RADIUS);
    }

    if (hasFocus())
    {
        painter.setPen(QPen(pal.color(QPalette::Highlight), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(area.adjusted(1, 1, -1, -1), RADIUS - 1, RADIUS - 1);
    }

    int x = 10;
    const QRect iconRect {x, (height() - iconSize().height()) / 2, iconSize().width(), iconSize().height()};
    painter.setOpacity(isChecked() ? 1.0 : 0.75);
    icon().paint(&painter, iconRect, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
    painter.setOpacity(1.0);
    x = iconRect.right() + 8;

    QFont textFont = font();
    textFont.setWeight(isChecked() ? QFont::DemiBold : QFont::Normal);
    painter.setFont(textFont);
    painter.setPen(pal.color(isChecked() ? QPalette::WindowText : QPalette::PlaceholderText));
    const int textWidth = QFontMetrics(textFont).horizontalAdvance(text());
    painter.drawText(QRect(x, 0, textWidth + 2, height()), Qt::AlignVCenter | Qt::AlignLeft, text());

    if (!m_badge.isEmpty())
    {
        painter.setFont(countFont(font()));
        painter.setPen(pal.color(QPalette::PlaceholderText));
        painter.drawText(QRect(x + textWidth + 6, 0, width() - (x + textWidth + 6) - 8, height())
            , Qt::AlignVCenter | Qt::AlignLeft, m_badge);
    }
}

void Modern::NavButton::enterEvent(QEnterEvent *event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void Modern::NavButton::leaveEvent(QEvent *event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

// ---------------------------------------------------------------- brand

QWidget *Modern::createBrandWidget(QWidget *parent)
{
    auto *brand = new QWidget(parent);
    brand->setObjectName(u"qbfBrand"_s);
    auto *layout = new QHBoxLayout(brand);
    layout->setContentsMargins(6, 0, 16, 0);
    layout->setSpacing(8);

    auto *logo = new QLabel(brand);
    logo->setPixmap(UIThemeManager::instance()->getIcon(u"qbittorrent"_s).pixmap(QSize(22, 22)));
    layout->addWidget(logo);

    auto *name = new QLabel(u"qBitFusion"_s, brand);
    name->setObjectName(u"qbfBrandName"_s);
    QFont font = name->font();
    font.setPointSizeF(font.pointSizeF() + 0.5);
    font.setWeight(QFont::DemiBold);
    name->setFont(font);
    layout->addWidget(name);
    return brand;
}

// ---------------------------------------------------------------- EmptyState

Modern::EmptyState::EmptyState(QAbstractItemView *view, QAbstractItemModel *sourceModel)
    : QWidget(view->viewport())
    , m_view {view}
    , m_sourceModel {sourceModel}
{
    setObjectName(u"qbfEmptyState"_s);
    setAttribute(Qt::WA_StyledBackground, false);

    auto *layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(6);

    m_title = new QLabel(this);
    m_title->setObjectName(u"qbfEmptyTitle"_s);
    m_title->setAlignment(Qt::AlignCenter);
    QFont titleFont = m_title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.5);
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    layout->addWidget(m_title, 0, Qt::AlignHCenter);

    m_body = new QLabel(this);
    m_body->setObjectName(u"qbfEmptyBody"_s);
    m_body->setAlignment(Qt::AlignCenter);
    m_body->setWordWrap(true);
    m_body->setFixedWidth(BODY_WIDTH);
    layout->addWidget(m_body, 0, Qt::AlignHCenter);

    m_buttons = new QWidget(this);
    auto *buttonsLayout = new QHBoxLayout(m_buttons);
    buttonsLayout->setContentsMargins(0, 10, 0, 0);
    buttonsLayout->setSpacing(10);
    layout->addWidget(m_buttons, 0, Qt::AlignHCenter);

    view->viewport()->installEventFilter(this);
    if (QAbstractItemModel *model = view->model())
    {
        for (const auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
            connect(model, signal, this, &EmptyState::refresh);
        connect(model, &QAbstractItemModel::modelReset, this, &EmptyState::refresh);
        connect(model, &QAbstractItemModel::layoutChanged, this, &EmptyState::refresh);
    }
    refresh();
}

QPushButton *Modern::EmptyState::addPrimaryAction(QAction *action, const QString &text)
{
    auto *button = new QPushButton(action->icon(), text, m_buttons);
    button->setObjectName(u"qbfPrimaryButton"_s);
    button->setCursor(Qt::PointingHandCursor);
    connect(button, &QPushButton::clicked, action, &QAction::trigger);
    m_buttons->layout()->addWidget(button);
    return button;
}

QPushButton *Modern::EmptyState::addSecondaryAction(QAction *action, const QString &text)
{
    auto *button = new QPushButton(action->icon(), text, m_buttons);
    button->setCursor(Qt::PointingHandCursor);
    connect(button, &QPushButton::clicked, action, &QAction::trigger);
    m_buttons->layout()->addWidget(button);
    return button;
}

bool Modern::EmptyState::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_view->viewport()) && (event->type() == QEvent::Resize))
        setGeometry(m_view->viewport()->rect());
    return QWidget::eventFilter(watched, event);
}

void Modern::EmptyState::refresh()
{
    const QAbstractItemModel *model = m_view->model();
    const bool isEmpty = !model || (model->rowCount() == 0);
    setVisible(isEmpty);
    if (!isEmpty)
        return;

    setGeometry(m_view->viewport()->rect());
    const bool hasAnything = m_sourceModel && (m_sourceModel->rowCount() > 0);
    if (hasAnything)
    {
        m_title->setText(tr("No matching torrents"));
        m_body->setText(tr("Nothing matches the current filter or search."));
        m_buttons->hide();
    }
    else
    {
        m_title->setText(tr("No torrents"));
        m_body->setText(tr("Add a torrent file or a magnet link, or drop a .torrent file on this window."));
        m_buttons->show();
    }
    // A wrapped label inside an aligned layout cell does not get height-for-width; size it here.
    m_body->setFixedHeight(m_body->heightForWidth(BODY_WIDTH));
}

// ---------------------------------------------------------------- FilterRowDelegate

void Modern::FilterRowDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const auto [name, count] = splitCount(opt.text);
    const QPalette &pal = opt.palette;
    const bool selected = opt.state.testFlag(QStyle::State_Selected);
    const bool hovered = opt.state.testFlag(QStyle::State_MouseOver);
    const QRect row = opt.rect.adjusted(4, 1, -4, -1);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (selected || hovered)
    {
        painter->setPen(Qt::NoPen);
        // Every filter list always has a selected row. Only one that narrows the list gets the
        // accent tint; "All" (the first row), selected, is the resting state and stays neutral.
        const bool narrows = selected && (index.row() > 0);
        painter->setBrush(narrows ? selectionFill(pal)
                : withAlpha(pal.color(QPalette::Text), (selected ? 0.075 : 0.05)));
        painter->drawRoundedRect(QRectF(row), RADIUS, RADIUS);
    }

    int x = row.left() + 8;
    const QSize iconSize {16, 16};
    if (!opt.icon.isNull())
    {
        const QRect iconRect {x, row.center().y() - (iconSize.height() / 2) + 1, iconSize.width(), iconSize.height()};
        opt.icon.paint(painter, iconRect, Qt::AlignCenter, (opt.state.testFlag(QStyle::State_Enabled) ? QIcon::Normal : QIcon::Disabled));
        x = iconRect.right() + 9;
    }

    int right = row.right() - 8;
    if (!count.isEmpty())
    {
        const QFont font = countFont(opt.font);
        const int width = QFontMetrics(font).horizontalAdvance(count);
        painter->setFont(font);
        painter->setPen(pal.color(QPalette::PlaceholderText));
        painter->drawText(QRect(right - width, row.top(), width, row.height()), Qt::AlignVCenter | Qt::AlignRight, count);
        right -= width + 8;
    }

    QFont font = opt.font;
    if (selected)
        font.setWeight(QFont::Medium);
    painter->setFont(font);
    painter->setPen(pal.color(QPalette::Text));
    const QRect textRect {x, row.top(), std::max(0, right - x), row.height()};
    painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(font).elidedText(name, Qt::ElideRight, textRect.width()));
    painter->restore();
}

QSize Modern::FilterRowDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(28, QFontMetrics(option.font).height() + 12));
    size.setWidth(size.width() + 24);
    return size;
}

// ---------------------------------------------------------------- AccentPicker

namespace
{
    struct AccentPreset
    {
        const char *name;
        QRgb rgb;
    };

    // The first entry is ModernAppearance::defaultAccent().
    const AccentPreset ACCENT_PRESETS[] = {
        {QT_TRANSLATE_NOOP("Modern::AccentPicker", "Ember"), 0xffff5b2e},
        {QT_TRANSLATE_NOOP("Modern::AccentPicker", "Blue"), 0xff3b82f6},
        {QT_TRANSLATE_NOOP("Modern::AccentPicker", "Sky"), 0xff0ea5e9},
        {QT_TRANSLATE_NOOP("Modern::AccentPicker", "Teal"), 0xff14b8a6},
        {QT_TRANSLATE_NOOP("Modern::AccentPicker", "Green"), 0xff22a559},
        {QT_TRANSLATE_NOOP("Modern::AccentPicker", "Amber"), 0xffe8a317},
        {QT_TRANSLATE_NOOP("Modern::AccentPicker", "Rose"), 0xfff43f5e},
        {QT_TRANSLATE_NOOP("Modern::AccentPicker", "Violet"), 0xff8b5cf6},
        {QT_TRANSLATE_NOOP("Modern::AccentPicker", "Graphite"), 0xff737373}
    };

    // A round colour chip; the chosen one gets a ring.
    class Swatch final : public QAbstractButton
    {
    public:
        Swatch(const QColor &color, const QString &name, QWidget *parent)
            : QAbstractButton(parent)
            , m_color {color}
        {
            setCheckable(true);
            setAutoExclusive(true);
            setCursor(Qt::PointingHandCursor);
            setToolTip(name);
            setAccessibleName(name);
            setFixedSize(26, 26);
        }

        QColor color() const
        {
            return m_color;
        }

        void setColor(const QColor &color)
        {
            m_color = color;
            update();
        }

    private:
        void paintEvent(QPaintEvent *) override
        {
            QPainter painter {this};
            painter.setRenderHint(QPainter::Antialiasing);
            const QPointF center = QRectF(rect()).center();
            if (isChecked() || hasFocus())
            {
                painter.setPen(QPen(isChecked() ? palette().color(QPalette::WindowText) : palette().color(QPalette::Mid), 1.5));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(center, 11.5, 11.5);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_color);
            painter.drawEllipse(center, 8, 8);
        }

        QColor m_color;
    };
}

Modern::AccentPicker::AccentPicker(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    const QColor current = ModernAppearance::accentSetting();
    const QColor effective = current.isValid() ? current : ModernAppearance::defaultAccent();
    bool matched = false;
    for (const AccentPreset &preset : ACCENT_PRESETS)
    {
        auto *swatch = new Swatch(QColor::fromRgb(preset.rgb), tr(preset.name), this);
        layout->addWidget(swatch);
        if (!matched && (swatch->color().rgb() == effective.rgb()))
        {
            swatch->setChecked(true);
            matched = true;
        }
        connect(swatch, &QAbstractButton::clicked, this, [this, swatch]
        {
            m_color = swatch->color();
            emit colorChanged();
        });
    }

    // A custom colour shows as one more chip.
    auto *customSwatch = new Swatch(effective, tr("Custom colour"), this);
    customSwatch->setVisible(!matched);
    customSwatch->setChecked(!matched);
    layout->addWidget(customSwatch);
    connect(customSwatch, &QAbstractButton::clicked, this, [this, customSwatch]
    {
        m_color = customSwatch->color();
        emit colorChanged();
    });

    auto *customButton = new QPushButton(tr("Custom..."), this);
    layout->addSpacing(6);
    layout->addWidget(customButton);
    layout->addStretch();
    connect(customButton, &QPushButton::clicked, this, [this, customSwatch]
    {
        const QColor picked = QColorDialog::getColor(m_color, this, tr("Accent colour"));
        if (!picked.isValid())
            return;
        customSwatch->setColor(picked);
        customSwatch->setVisible(true);
        customSwatch->setChecked(true);
        m_color = picked;
        emit colorChanged();
    });

    m_color = effective;
}

QColor Modern::AccentPicker::color() const
{
    return m_color;
}

void Modern::styleSidebarViews(QWidget *sidebar)
{
    sidebar->setBackgroundRole(QPalette::Window);
    for (QAbstractItemView *view : sidebar->findChildren<QAbstractItemView *>())
    {
        view->setItemDelegate(new FilterRowDelegate(view));
        view->setProperty("qbfSidebarList", true);
        view->setFrameShape(QFrame::NoFrame);
        view->setMouseTracking(true);
        view->viewport()->setAttribute(Qt::WA_Hover);
        // The views may already be polished; make the stylesheet see the new property.
        view->style()->unpolish(view);
        view->style()->polish(view);
        view->updateGeometry();   // these lists size themselves to their rows
    }
}

void Modern::flattenSections(QWidget *root)
{
    const QList<QGroupBox *> boxes = root->findChildren<QGroupBox *>();
    for (QGroupBox *box : boxes)
    {
        if (QLayout *layout = box->layout())
        {
            const QMargins margins = layout->contentsMargins();
            layout->setContentsMargins(0, std::min(margins.top(), 6), 0, std::min(margins.bottom(), 6));
        }
    }
}

void Modern::markAsHint(QLabel *label)
{
    label->setProperty("qbfHint", true);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

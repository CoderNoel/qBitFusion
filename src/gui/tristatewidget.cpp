/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2019  Mike Tzou (Chocobo1)
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

#include "tristatewidget.h"

#include <algorithm>

#include <QApplication>
#include <QFontMetrics>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QString>
#include <QStyle>
#include <QStyleOptionMenuItem>

#include "gui/modern/modernappearance.h"

namespace
{
    // Geometry of a modern menu row (see QMenu::item in modern.qss), so this custom item lines up
    // with the ordinary ones around it.
    constexpr int ROW_PADDING = 6;
    constexpr int BOX_LEFT = 6;
    constexpr int BOX_SIZE = 16;
    constexpr int TEXT_LEFT = 30;
    constexpr int RIGHT_PADDING = 28;
}

TriStateWidget::TriStateWidget(const QString &text, QWidget *parent)
    : QWidget {parent}
    , m_text {text}
{
    setMouseTracking(true);  // for visual effects via mouse navigation
    setFocusPolicy(Qt::TabFocus);  // for visual effects via keyboard navigation
}

void TriStateWidget::setCheckState(const Qt::CheckState checkState)
{
    m_checkState = checkState;
}

void TriStateWidget::setCloseOnInteraction(const bool enabled)
{
    m_closeOnInteraction = enabled;
}

QSize TriStateWidget::minimumSizeHint() const
{
    if (ModernAppearance::isActive())
    {
        // Measured and drawn with the menu font, like the items around it.
        const QSize textSize = QFontMetrics(QApplication::font("QMenu")).size(Qt::TextSingleLine, m_text);
        return {(TEXT_LEFT + textSize.width() + RIGHT_PADDING), (std::max(textSize.height(), 17) + (2 * ROW_PADDING))};
    }

    QStyleOptionMenuItem opt;
    opt.initFrom(this);
    opt.menuHasCheckableItems = true;
    const QSize contentSize = fontMetrics().size(Qt::TextSingleLine, m_text);
    return style()->sizeFromContents(QStyle::CT_MenuItem, &opt, contentSize, this);
}

void TriStateWidget::paintEvent(QPaintEvent *)
{
    if (ModernAppearance::isActive())
    {
        // The base style would draw a stock check mark here: a stylesheet's menu rules only reach
        // a QMenu's own items, not a widget placed in one.
        QPainter painter {this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setFont(QApplication::font("QMenu"));
        const QPalette pal = palette();

        if (hasFocus() || rect().contains(mapFromGlobal(QCursor::pos())))
        {
            QColor hover = pal.color(QPalette::WindowText);
            hover.setAlphaF(0.085);
            painter.setPen(Qt::NoPen);
            painter.setBrush(hover);
            painter.drawRoundedRect(rect(), 5, 5);
        }

        const QRectF box {BOX_LEFT + 0.5, ((height() - BOX_SIZE) / 2.0) + 0.5, BOX_SIZE - 1.0, BOX_SIZE - 1.0};
        if (m_checkState == Qt::Unchecked)
        {
            painter.setPen(QPen(pal.color(QPalette::Dark), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(box, 4, 4);
        }
        else
        {
            const QColor accent = ModernAppearance::color(ModernAppearance::Tone::Accent);
            painter.setPen(QPen(accent, 1));
            painter.setBrush(accent);
            painter.drawRoundedRect(box, 4, 4);

            QPen mark {pal.color(QPalette::HighlightedText), 1.7};
            mark.setCapStyle(Qt::RoundCap);
            mark.setJoinStyle(Qt::RoundJoin);
            painter.setPen(mark);
            const QPointF c = box.center();
            if (m_checkState == Qt::Checked)
                painter.drawPolyline(QList<QPointF> {{c.x() - 3.6, c.y() + 0.2}, {c.x() - 1.1, c.y() + 2.7}, {c.x() + 3.8, c.y() - 2.6}});
            else
                painter.drawLine(QPointF(c.x() - 3.2, c.y()), QPointF(c.x() + 3.2, c.y()));
        }

        painter.setPen(pal.color(isEnabled() ? QPalette::WindowText : QPalette::PlaceholderText));
        painter.drawText(QRect(TEXT_LEFT, 0, std::max(0, width() - TEXT_LEFT - 8), height()), (Qt::AlignVCenter | Qt::AlignLeft), m_text);
        return;
    }

    QStyleOptionMenuItem opt;
    opt.initFrom(this);
    opt.menuItemType = QStyleOptionMenuItem::Normal;
    opt.checkType = QStyleOptionMenuItem::NonExclusive;
    opt.menuHasCheckableItems = true;
    opt.text = m_text;

    switch (m_checkState)
    {
    case Qt::PartiallyChecked:
        opt.state |= QStyle::State_NoChange;
        break;
    case Qt::Checked:
        opt.checked = true;
        break;
    case Qt::Unchecked:
        opt.checked = false;
        break;
    };

    if ((opt.state & QStyle::State_HasFocus)
        || rect().contains(mapFromGlobal(QCursor::pos())))
        {
        opt.state |= QStyle::State_Selected;

        if (QApplication::mouseButtons() != Qt::NoButton)
            opt.state |= QStyle::State_Sunken;
    }

    QPainter painter {this};
    style()->drawControl(QStyle::CE_MenuItem, &opt, &painter, this);
}

void TriStateWidget::mouseReleaseEvent(QMouseEvent *event)
{
    toggleCheckState();

    if (m_closeOnInteraction)
    {
        // parent `triggered` signal will be emitted
        QWidget::mouseReleaseEvent(event);
    }
    else
    {
        update();
        // need to emit `triggered` signal manually
        emit triggered(m_checkState == Qt::Checked);
    }
}

void TriStateWidget::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Return)
        || (event->key() == Qt::Key_Enter))
        {
        toggleCheckState();

        if (!m_closeOnInteraction)
        {
            update();
            // need to emit parent `triggered` signal manually
            emit triggered(m_checkState == Qt::Checked);
            return;
        }
    }

    QWidget::keyPressEvent(event);
}

void TriStateWidget::toggleCheckState()
{
    switch (m_checkState)
    {
    case Qt::Unchecked:
    case Qt::PartiallyChecked:
        m_checkState = Qt::Checked;
        break;
    case Qt::Checked:
        m_checkState = Qt::Unchecked;
        break;
    };
}

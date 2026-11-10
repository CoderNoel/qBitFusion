/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2025  Vladimir Golovnev <glassez@yandex.ru>
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

#include "transferlistfilterswidgetitem.h"

#include <algorithm>

#include <QCheckBox>
#include <QFont>
#include <QPainter>
#include <QString>
#include <QStyleOptionViewItem>
#include <QVBoxLayout>

#include "modern/modernappearance.h"

namespace
{
    class ArrowCheckBox final : public QCheckBox
    {
    public:
        using QCheckBox::QCheckBox;

    private:
        void paintEvent(QPaintEvent *) override
        {
            QPainter painter {this};

            if (ModernAppearance::isActive())
            {
                // Quiet section label; the collapse chevron stays out of the way until needed.
                painter.setRenderHint(QPainter::Antialiasing);
                const QRect area = rect().adjusted(14, 0, -12, 0);
                painter.setFont(font());
                painter.setPen(palette().color(QPalette::PlaceholderText));
                painter.drawText(area, (Qt::AlignVCenter | Qt::AlignLeft), text());
                if (!underMouse() && isChecked())
                    return;   // the chevron only shows on hover or while the section is collapsed

                const QPointF center {area.right() - 4.0, area.center().y() + 0.5};
                QPolygonF chevron;
                if (isChecked())
                    chevron << (center + QPointF(-3.5, -1.75)) << (center + QPointF(0, 1.75)) << (center + QPointF(3.5, -1.75));
                else
                    chevron << (center + QPointF(-1.75, -3.5)) << (center + QPointF(1.75, 0)) << (center + QPointF(-1.75, 3.5));
                painter.setPen(QPen(painter.pen().color(), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawPolyline(chevron);
                return;
            }

            QStyleOptionViewItem indicatorOption;
            indicatorOption.initFrom(this);
            indicatorOption.rect = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &indicatorOption, this);
            indicatorOption.state |= (QStyle::State_Children
                | (isChecked() ? QStyle::State_Open : QStyle::State_None));
            style()->drawPrimitive(QStyle::PE_IndicatorBranch, &indicatorOption, &painter, this);

            QStyleOptionButton labelOption;
            initStyleOption(&labelOption);
            labelOption.rect = style()->subElementRect(QStyle::SE_CheckBoxContents, &labelOption, this);
            style()->drawControl(QStyle::CE_CheckBoxLabel, &labelOption, &painter, this);
        }
    };
}

TransferListFiltersWidgetItem::TransferListFiltersWidgetItem(const QString &caption, QWidget *filterWidget, QWidget *parent)
    : QWidget(parent)
    , m_caption {new ArrowCheckBox(caption, this)}
    , m_filterWidget {filterWidget}
{
    QFont font;
    font.setBold(true);
    font.setCapitalization(QFont::AllUppercase);
    if (ModernAppearance::isActive())
    {
        // Sentence case, regular weight: a label, not a shouted heading.
        font.setBold(false);
        font.setCapitalization(QFont::MixedCase);
        font.setWeight(QFont::Medium);
        font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1.0));
        m_caption->setMinimumHeight(28);
        m_caption->setCursor(Qt::PointingHandCursor);
        m_caption->setAttribute(Qt::WA_Hover);
        m_caption->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    m_caption->setFont(font);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 0);
    layout->setSpacing(2);
    if (ModernAppearance::isActive())
    {
        layout->setContentsMargins(0, 6, 0, 0);
        layout->setSpacing(0);
        layout->setAlignment(Qt::AlignTop);
    }
    else
    {
        layout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    }
    layout->addWidget(m_caption);
    layout->addWidget(m_filterWidget);

    m_filterWidget->setVisible(m_caption->isChecked());

    connect(m_caption, &QCheckBox::toggled, m_filterWidget, &QWidget::setVisible);
    connect(m_caption, &QCheckBox::toggled, this, &TransferListFiltersWidgetItem::toggled);
}

bool TransferListFiltersWidgetItem::isChecked() const
{
    return m_caption->isChecked();
}

void TransferListFiltersWidgetItem::setChecked(const bool value)
{
    m_caption->setChecked(value);
}

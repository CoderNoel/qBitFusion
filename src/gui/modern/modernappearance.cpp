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

#include "modernappearance.h"

#include <algorithm>

#include <QApplication>
#include <QFile>
#include <QPainter>
#include <QPainterPath>

#include "base/settingvalue.h"

using namespace Qt::Literals::StringLiterals;

// Resources live in the static qbt_gui library and must be initialised explicitly.
static void initModernResources()
{
    Q_INIT_RESOURCE(modern);
}

namespace
{
    const QString KEY_MODERN = u"Appearance/ModernUI"_s;
    bool g_active = false;

    struct Scheme
    {
        QColor window, base, alternate, button, text, placeholder, mid, midlight, dark, light, shadow;
        QColor highlight, highlightedText, link, tooltipBase, tooltipText, disabledText;
    };

    // Neutral slate surfaces, one electric-blue accent. Contrast checked against WCAG AA for text.
    const Scheme DARK {
        .window = QColor(0x13, 0x15, 0x1a), .base = QColor(0x19, 0x1c, 0x22), .alternate = QColor(0x1d, 0x20, 0x27)
        , .button = QColor(0x22, 0x26, 0x2e), .text = QColor(0xe8, 0xea, 0xef), .placeholder = QColor(0x8e, 0x96, 0xa4)
        , .mid = QColor(0x2c, 0x31, 0x3b), .midlight = QColor(0x26, 0x2a, 0x33), .dark = QColor(0x46, 0x4d, 0x5a)
        , .light = QColor(0x34, 0x3a, 0x45), .shadow = QColor(0x00, 0x00, 0x00)
        , .highlight = QColor(0x4f, 0x7c, 0xff), .highlightedText = QColor(0xff, 0xff, 0xff), .link = QColor(0x82, 0xa4, 0xff)
        , .tooltipBase = QColor(0x24, 0x28, 0x31), .tooltipText = QColor(0xe8, 0xea, 0xef), .disabledText = QColor(0x5f, 0x67, 0x74)
    };

    const Scheme LIGHT {
        .window = QColor(0xf3, 0xf4, 0xf7), .base = QColor(0xff, 0xff, 0xff), .alternate = QColor(0xf9, 0xfa, 0xfc)
        , .button = QColor(0xff, 0xff, 0xff), .text = QColor(0x1a, 0x1d, 0x23), .placeholder = QColor(0x68, 0x70, 0x7d)
        , .mid = QColor(0xdc, 0xdf, 0xe5), .midlight = QColor(0xe9, 0xeb, 0xf0), .dark = QColor(0xae, 0xb4, 0xbf)
        , .light = QColor(0xff, 0xff, 0xff), .shadow = QColor(0x9a, 0xa0, 0xab)
        , .highlight = QColor(0x2d, 0x63, 0xf6), .highlightedText = QColor(0xff, 0xff, 0xff), .link = QColor(0x2d, 0x63, 0xf6)
        , .tooltipBase = QColor(0x1f, 0x23, 0x2b), .tooltipText = QColor(0xf3, 0xf4, 0xf7), .disabledText = QColor(0xa6, 0xac, 0xb7)
    };
}

bool ModernAppearance::isEnabledSetting()
{
    return SettingValue<bool>(KEY_MODERN).get(true);
}

void ModernAppearance::setEnabledSetting(const bool enabled)
{
    SettingValue<bool> setting {KEY_MODERN};
    setting = enabled;
}

bool ModernAppearance::isActive()
{
    return g_active;
}

QPalette ModernAppearance::palette(const bool dark)
{
    const Scheme &s = dark ? DARK : LIGHT;
    QPalette p;
    for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
    {
        p.setColor(group, QPalette::Window, s.window);
        p.setColor(group, QPalette::WindowText, s.text);
        p.setColor(group, QPalette::Base, s.base);
        p.setColor(group, QPalette::AlternateBase, s.alternate);
        p.setColor(group, QPalette::Button, s.button);
        p.setColor(group, QPalette::ButtonText, s.text);
        p.setColor(group, QPalette::Text, s.text);
        p.setColor(group, QPalette::BrightText, Qt::white);
        p.setColor(group, QPalette::PlaceholderText, s.placeholder);
        p.setColor(group, QPalette::Light, s.light);
        p.setColor(group, QPalette::Midlight, s.midlight);
        p.setColor(group, QPalette::Mid, s.mid);
        p.setColor(group, QPalette::Dark, s.dark);
        p.setColor(group, QPalette::Shadow, s.shadow);
        p.setColor(group, QPalette::Highlight, s.highlight);
        p.setColor(group, QPalette::HighlightedText, s.highlightedText);
        p.setColor(group, QPalette::Link, s.link);
        p.setColor(group, QPalette::LinkVisited, s.link);
        p.setColor(group, QPalette::ToolTipBase, s.tooltipBase);
        p.setColor(group, QPalette::ToolTipText, s.tooltipText);
#if (QT_VERSION >= QT_VERSION_CHECK(6, 6, 0))
        p.setColor(group, QPalette::Accent, s.highlight);
#endif
    }
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::HighlightedText})
        p.setColor(QPalette::Disabled, role, s.disabledText);
    // Keep selection readable when the window is inactive.
    p.setColor(QPalette::Inactive, QPalette::Highlight, s.highlight.darker(dark ? 135 : 112));
    return p;
}

void ModernAppearance::apply(const bool dark)
{
    initModernResources();
    g_active = true;
    qApp->setPalette(palette(dark));

    QFile file {u":/qbitfusion/modern.qss"_s};
    if (!file.open(QIODevice::ReadOnly))
        return;

    // A few colours cannot be expressed as palette roles (translucent accent tints): substitute
    // per-scheme tokens. Everything else in the sheet follows the palette.
    QString sheet = QString::fromUtf8(file.readAll());
    sheet.replace(u"@selection@"_s, dark ? u"rgba(79, 124, 255, 0.30)"_s : u"rgba(45, 99, 246, 0.13)"_s);
    sheet.replace(u"@selectionText@"_s, dark ? u"#ffffff"_s : u"#10131a"_s);
    sheet.replace(u"@hover@"_s, dark ? u"rgba(255, 255, 255, 0.05)"_s : u"rgba(16, 24, 40, 0.045)"_s);
    qApp->setStyleSheet(sheet);
}

void ModernAppearance::paintProgressPill(QPainter *painter, const QRect &cellRect, const int progress, const QString &text
        , const QColor &fill, const bool selected, const QPalette &pal)
{
    const int height = std::clamp(cellRect.height() - 8, 10, 18);
    const QRectF pill = QRectF(cellRect.adjusted(6, 0, -6, 0)).adjusted(0, (cellRect.height() - height) / 2.0, 0, 0);
    const QRectF track {pill.left(), pill.top(), pill.width(), static_cast<qreal>(height)};
    const qreal radius = height / 2.0;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    Q_UNUSED(selected);   // selection is a soft tint now; the pill keeps its normal colours
    const QColor trackColor = pal.color(QPalette::Mid);
    painter->setPen(Qt::NoPen);
    painter->setBrush(trackColor);
    painter->drawRoundedRect(track, radius, radius);

    const qreal ratio = std::clamp(progress, 0, 100) / 100.0;
    const QColor fillColor = fill.isValid() ? fill : pal.color(QPalette::Highlight);
    if (ratio > 0)
    {
        QPainterPath clip;
        clip.addRoundedRect(track, radius, radius);
        painter->setClipPath(clip);
        painter->setBrush(fillColor);
        painter->drawRect(QRectF(track.left(), track.top(), std::max(track.width() * ratio, radius * 2), track.height()));
        painter->setClipping(false);
    }

    // Text: readable both over the fill (white) and over the track (normal text colour).
    QFont font = painter->font();
    font.setPointSizeF(std::max(7.0, font.pointSizeF() - 0.5));
    font.setWeight(QFont::DemiBold);
    painter->setFont(font);
    const QColor overTrack = pal.color(QPalette::Text);
    const qreal split = track.left() + (track.width() * ratio);

    painter->setClipRect(QRectF(track.left(), track.top(), split - track.left(), track.height()));
    painter->setPen(Qt::white);
    painter->drawText(track, Qt::AlignCenter, text);
    painter->setClipRect(QRectF(split, track.top(), track.right() - split, track.height()));
    painter->setPen(overTrack);
    painter->drawText(track, Qt::AlignCenter, text);
    painter->restore();
}

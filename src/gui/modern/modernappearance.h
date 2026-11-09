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

#pragma once

#include <QColor>
#include <QPalette>

class QPainter;
class QRect;

// qBitFusion's modern look: a curated light/dark palette plus one palette-driven stylesheet,
// layered on qBittorrent's existing theme pipeline. A user-selected custom .qbtheme always wins,
// and "Use classic qBittorrent appearance" restores the stock look.
namespace ModernAppearance
{
    bool isEnabledSetting();
    void setEnabledSetting(bool enabled);

    // Applied for this session (decided once at startup; toggling takes effect after restart).
    bool isActive();

    void apply(bool dark);
    QPalette palette(bool dark);

    // Rounded "pill" progress bar used by item views while the modern look is active.
    void paintProgressPill(QPainter *painter, const QRect &cellRect, int progress, const QString &text
            , const QColor &fill, bool selected, const QPalette &palette);
}

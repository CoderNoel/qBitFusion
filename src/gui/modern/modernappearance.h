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
#include <QIcon>
#include <QPalette>

class QPainter;
class QStyle;
class QRect;
class QString;

// qBitFusion's modern look: curated light/dark palettes, one palette-driven stylesheet, an original
// monochrome icon set tinted at paint time, and the redesigned main window shell (see MainWindow).
// A user-selected custom .qbtheme always wins, and Options > Behavior can restore the classic look.
namespace ModernAppearance
{
    enum class Tone
    {
        Text,       // default icon color
        Muted,      // secondary text and inactive states
        Accent,     // the one accent color
        Info,       // downloading
        Success,    // seeding, completed, connected
        Warning,    // stalled, queued, firewalled
        Danger,     // errors, disconnected
        Special,    // checking, moving
        Inverse     // glyphs on primary (inverse-neutral) buttons
    };

    bool isEnabledSetting();
    void setEnabledSetting(bool enabled);

    // Applied for this session (decided once at startup; toggling takes effect after restart).
    bool isActive();
    bool isDark();

    // Opacity of the accent tint used for selected rows, tabs and pages.
    qreal selectionAlpha();

    // The widget style for the modern look: the named base style with a few behavior tweaks.
    QStyle *createStyle(const QString &baseStyleName);

    void apply(bool dark);

    // The accent color tints selection, progress, focus and "on" states. The user picks it in
    // Options; an invalid color means the default. Setting it re-applies the look immediately.
    QColor defaultAccent();
    QColor accentSetting();
    void setAccentSetting(const QColor &color);
    QPalette palette(bool dark);
    QColor color(Tone tone);

    // Color for a UI theme color id ("TransferList.Downloading", "PiecesBar.Piece", ...), or an
    // invalid color when the modern look has no opinion and the stock default should be used.
    QColor themeColor(const QString &colorId);

    // Icon from the qBitFusion set for a theme icon id, or a null icon when the set has none.
    // Monochrome icons follow the current light/dark scheme at paint time.
    QIcon icon(const QString &iconId, bool forDarkSurface);

    // Slim progress track with the value beside it, used by item views while the modern look is active.
    void paintProgress(QPainter *painter, const QRect &cellRect, int progress, const QString &text
            , const QColor &fill, bool enabled, const QPalette &palette);
}

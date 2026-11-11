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

#include <optional>

#include <QDateTime>
#include <QString>

#include "base/3rdparty/expected.hpp"
#include "base/path.h"
#include "hybridtypes.h"

// Per-torrent policy sidecar (spec 06 "Settings and persistence", ADR-006).
// Holds ONLY re-derivable, non-secret policy. Never: piece bitmaps, URLs, magnets, tokens, cookies.

namespace Hybrid
{
    inline constexpr int SIDECAR_SCHEMA_VERSION = 1;

    struct SidecarData
    {
        TransportPolicy policy = TransportPolicy::TorrentOnly;
        bool p2pFallbackConsent = false;   // persisted distinctly; never inferred from credentials
        QString providerId;                // e.g. "realdebrid"
        QString providerResourceId;        // remote torrent id, account-scoped; [A-Za-z0-9_-]{1,64}
        Reason lastReason = Reason::None;
        QDateTime lastTransitionUtc;
        quint64 switchCount = 0;

        friend bool operator==(const SidecarData &, const SidecarData &) = default;
    };

    QByteArray serializeSidecar(const SidecarData &data);
    // Unknown future fields are ignored; corrupt or secret-looking content is rejected (caller
    // falls back to defaults, which are the safest: torrent_only is upstream behavior).
    nonstd::expected<SidecarData, QString> parseSidecar(const QByteArray &json);

    bool isSafeIdentifier(const QString &value);

    class SidecarStore
    {
    public:
        explicit SidecarStore(Path directory);

        Path directory() const;
        Path pathFor(const QString &torrentID) const;

        std::optional<SidecarData> load(const QString &torrentID) const;
        nonstd::expected<void, QString> save(const QString &torrentID, const SidecarData &data) const;
        bool remove(const QString &torrentID) const;

    private:
        Path m_directory;
    };
}

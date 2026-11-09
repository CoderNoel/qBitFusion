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

#include "hybridtorrentsink.h"

#include <libtorrent/torrent_info.hpp>

#include "base/hybrid/ltlayout.h"
#include "infohash.h"
#include "torrentimpl.h"
#include "torrentinfo.h"

BitTorrent::HybridTorrentSink::HybridTorrentSink(Torrent *torrent)
    : m_torrent {static_cast<TorrentImpl *>(torrent)}
{
}

BitTorrent::Torrent *BitTorrent::HybridTorrentSink::torrent() const
{
    return m_torrent;
}

Hybrid::TorrentIdentity BitTorrent::HybridTorrentSink::identity() const
{
    const InfoHash hash = m_torrent->infoHash();
    Hybrid::TorrentIdentity identity;
    if (hash.v1().isValid())
        identity.infoHashV1 = hash.v1().toString();
    if (hash.v2().isValid())
        identity.infoHashV2 = hash.v2().toString();
    identity.name = m_torrent->name();
    return identity;
}

bool BitTorrent::HybridTorrentSink::hasMetadata() const
{
    return m_torrent->hasMetadata();
}

bool BitTorrent::HybridTorrentSink::isChecking() const
{
    return m_torrent->isChecking();
}

int BitTorrent::HybridTorrentSink::pieceCount() const
{
    return m_torrent->hasMetadata() ? m_torrent->piecesCount() : 0;
}

QList<Hybrid::TorrentFileEntry> BitTorrent::HybridTorrentSink::nativeFiles() const
{
    const auto info = m_torrent->info().nativeInfo();
    return info ? Hybrid::nativeFileEntries(*info) : QList<Hybrid::TorrentFileEntry> {};
}

QList<Hybrid::PieceSlice> BitTorrent::HybridTorrentSink::pieceSlices(const int piece) const
{
    const auto info = m_torrent->info().nativeInfo();
    return info ? Hybrid::nativePieceSlices(*info, piece) : QList<Hybrid::PieceSlice> {};
}

qint64 BitTorrent::HybridTorrentSink::pieceLength(const int piece) const
{
    const auto info = m_torrent->info().nativeInfo();
    return info ? Hybrid::nativePieceLength(*info, piece) : 0;
}

QBitArray BitTorrent::HybridTorrentSink::havePieces() const
{
    return m_torrent->pieces();
}

QList<int> BitTorrent::HybridTorrentSink::wantedMissingPieces() const
{
    QList<int> result;
    if (!m_torrent->hasMetadata())
        return result;

    const TorrentInfo info = m_torrent->info();
    const int n = m_torrent->piecesCount();
    QBitArray wanted(n);
    const QList<DownloadPriority> priorities = m_torrent->filePriorities();
    for (qsizetype i = 0; i < priorities.size(); ++i)
    {
        if (priorities[i] == DownloadPriority::Ignored)
            continue;
        const TorrentInfo::PieceRange range = info.filePieces(static_cast<int>(i));
        if (range.isEmpty())
            continue;
        wanted.fill(true, range.first(), range.last() + 1);
    }

    const QBitArray have = m_torrent->pieces();
    result.reserve(n);
    for (int p = 0; p < n; ++p)
    {
        if (wanted.testBit(p) && !((p < have.size()) && have.testBit(p)))
            result.append(p);
    }
    return result;
}

void BitTorrent::HybridTorrentSink::setTransportHold(const bool hold)
{
    m_torrent->setTransportHold(hold);
}

bool BitTorrent::HybridTorrentSink::isTransportHoldSettled() const
{
    return m_torrent->isTransportHeld() && (m_torrent->peersCount() == 0);
}

void BitTorrent::HybridTorrentSink::addPiece(const int piece, std::vector<char> &&data)
{
    m_torrent->addExternalPiece(piece, std::move(data));
}

void BitTorrent::HybridTorrentSink::setHttpRate(const qint64 bytesPerSecond)
{
    m_torrent->setHybridHttpRate(bytesPerSecond);
}

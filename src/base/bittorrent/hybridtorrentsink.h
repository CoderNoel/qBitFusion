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

#include "base/hybrid/torrentpiecesink.h"

namespace BitTorrent
{
    class Torrent;
    class TorrentImpl;

    // Adapts qBittorrent's TorrentImpl to the hybrid coordinator's narrow sink interface.
    // All reads use qBittorrent's cached status (no blocking libtorrent queries on the GUI thread).
    class HybridTorrentSink final : public Hybrid::TorrentPieceSink
    {
    public:
        explicit HybridTorrentSink(Torrent *torrent);

        Torrent *torrent() const;

        Hybrid::TorrentIdentity identity() const override;
        bool hasMetadata() const override;
        bool isChecking() const override;
        int pieceCount() const override;
        QList<Hybrid::TorrentFileEntry> nativeFiles() const override;
        QList<Hybrid::PieceSlice> pieceSlices(int piece) const override;
        qint64 pieceLength(int piece) const override;
        QBitArray havePieces() const override;
        QList<int> wantedMissingPieces() const override;
        void setTransportHold(bool hold) override;
        bool isTransportHoldSettled() const override;
        void addPiece(int piece, std::vector<char> &&data) override;

        void setHttpRate(qint64 bytesPerSecond);

    private:
        TorrentImpl *m_torrent = nullptr;
    };
}

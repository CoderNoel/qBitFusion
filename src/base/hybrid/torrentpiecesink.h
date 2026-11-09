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

#include <vector>

#include <QBitArray>
#include <QList>

#include "filebinding.h"
#include "pieceassembler.h"
#include "provider.h"

// The coordinator's only window into a torrent (spec 03 "ITorrentPieceSink", spec 06 "Narrow
// wrapper around libtorrent"). Implemented by TorrentImpl in the app and by a direct
// lt::torrent_handle adapter in integration tests. No libtorrent types cross this boundary.

namespace Hybrid
{
    class TorrentPieceSink
    {
    public:
        virtual ~TorrentPieceSink() = default;

        virtual TorrentIdentity identity() const = 0;
        virtual bool hasMetadata() const = 0;
        virtual bool isChecking() const = 0;

        virtual int pieceCount() const = 0;
        // Native libtorrent order, including pad files (never filtered).
        virtual QList<TorrentFileEntry> nativeFiles() const = 0;
        // file_storage::map_block(piece, 0, piece_size_for_req(piece)), pad flags resolved.
        virtual QList<PieceSlice> pieceSlices(int piece) const = 0;
        virtual qint64 pieceLength(int piece) const = 0;

        // libtorrent's verified pieces (authoritative, possibly slightly stale).
        virtual QBitArray havePieces() const = 0;
        // Missing pieces the user wants (file priority > 0), in download order.
        virtual QList<int> wantedMissingPieces() const = 0;

        // Transport hold: true = no swarm activity at all (graceful pause, not auto-managed);
        // false = restore whatever the user's own start/stop/queue state implies.
        virtual void setTransportHold(bool hold) = 0;
        virtual bool isTransportHoldSettled() const = 0;

        // Asynchronous, no overwrite_existing. Verdict arrives via the coordinator's notify* calls.
        virtual void addPiece(int piece, std::vector<char> &&data) = 0;
    };
}

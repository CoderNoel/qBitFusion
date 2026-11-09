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

#include <QtTypes>
#include <QBitArray>
#include <QList>

// Short-lived per-piece producer ownership (spec 02 "Control plane", spec 03 section 5).
// This is NOT a completion ledger: "Verified" is only ever mirrored from libtorrent and is never
// persisted. Invariant I3: no two producers intentionally own the same piece.

namespace Hybrid
{
    enum class PieceOwner : quint8
    {
        Unclaimed,
        HttpFetching,
        HttpSubmitted,
        P2PActive,
        Verified
    };

    struct OwnershipCounts
    {
        int unclaimed = 0;
        int httpFetching = 0;
        int httpSubmitted = 0;
        int p2pActive = 0;
        int verified = 0;
    };

    class PieceOwnership
    {
    public:
        void reset(int pieceCount);
        int pieceCount() const;
        PieceOwner owner(int piece) const;
        OwnershipCounts counts() const;

        // Mirror libtorrent's authoritative have-bitmap. Verified wins; pieces libtorrent no longer
        // has (e.g. after recheck) drop back to Unclaimed unless an HTTP submission is pending.
        void reconcileVerified(const QBitArray &have);

        // Mirror libtorrent's download queue (pieces with peer blocks requested/in flight).
        // Unclaimed -> P2PActive; P2PActive not in queue -> Unclaimed. HTTP-owned pieces are left
        // alone but reported via collisions() so the caller can prove I3 in tests.
        void reconcileP2PQueue(const QList<int> &downloadQueue);

        // Claim for HTTP: only Unclaimed pieces. Records the transport epoch.
        bool claimForHttp(int piece, quint64 epoch);
        // HttpFetching(epoch) -> HttpSubmitted. Fails for stale epochs or other owners.
        bool markSubmitted(int piece, quint64 epoch);
        // HttpFetching -> Unclaimed (fetch failed/cancelled). No-op for other states.
        void releaseFetch(int piece);
        // On epoch change: every HttpFetching piece is released. HttpSubmitted is kept until verdict.
        QList<int> releaseAllFetching();

        // libtorrent verdicts.
        void markVerified(int piece);
        // HttpSubmitted -> Unclaimed; returns consecutive HTTP hash failures for that piece.
        int markHashFailed(int piece);

        // Ordered selection of Unclaimed pieces that libtorrent wants and lacks.
        QList<int> pickForHttp(const QList<int> &orderedCandidates, int maxCount) const;

        // Pieces that were HTTP-owned while libtorrent reported them in the peer queue.
        int collisions() const;

    private:
        bool valid(int piece) const;

        std::vector<PieceOwner> m_owner;
        std::vector<quint64> m_epoch;
        std::vector<quint8> m_hashFailures;
        int m_collisions = 0;
    };
}

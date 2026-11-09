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

#include "pieceownership.h"

#include <algorithm>

void Hybrid::PieceOwnership::reset(const int pieceCount)
{
    const auto count = static_cast<std::size_t>(std::max(0, pieceCount));
    m_owner.assign(count, PieceOwner::Unclaimed);
    m_epoch.assign(count, 0);
    m_hashFailures.assign(count, 0);
    m_collisions = 0;
}

int Hybrid::PieceOwnership::pieceCount() const
{
    return static_cast<int>(m_owner.size());
}

bool Hybrid::PieceOwnership::valid(const int piece) const
{
    return (piece >= 0) && (static_cast<std::size_t>(piece) < m_owner.size());
}

Hybrid::PieceOwner Hybrid::PieceOwnership::owner(const int piece) const
{
    return valid(piece) ? m_owner[piece] : PieceOwner::Unclaimed;
}

Hybrid::OwnershipCounts Hybrid::PieceOwnership::counts() const
{
    OwnershipCounts c;
    for (const PieceOwner o : m_owner)
    {
        switch (o)
        {
        case PieceOwner::Unclaimed:
            ++c.unclaimed;
            break;
        case PieceOwner::HttpFetching:
            ++c.httpFetching;
            break;
        case PieceOwner::HttpSubmitted:
            ++c.httpSubmitted;
            break;
        case PieceOwner::P2PActive:
            ++c.p2pActive;
            break;
        case PieceOwner::Verified:
            ++c.verified;
            break;
        }
    }
    return c;
}

void Hybrid::PieceOwnership::reconcileVerified(const QBitArray &have)
{
    const int n = std::min(pieceCount(), static_cast<int>(have.size()));
    for (int i = 0; i < n; ++i)
    {
        if (have.testBit(i))
        {
            m_owner[i] = PieceOwner::Verified;
            m_hashFailures[i] = 0;
        }
        else if (m_owner[i] == PieceOwner::Verified)
        {
            // libtorrent lost the piece (recheck / file removed). Never trust our mirror over it.
            m_owner[i] = PieceOwner::Unclaimed;
        }
    }
}

void Hybrid::PieceOwnership::reconcileP2PQueue(const QList<int> &downloadQueue)
{
    std::vector<bool> inQueue(m_owner.size(), false);
    for (const int piece : downloadQueue)
    {
        if (valid(piece))
            inQueue[piece] = true;
    }

    for (std::size_t i = 0; i < m_owner.size(); ++i)
    {
        switch (m_owner[i])
        {
        case PieceOwner::Unclaimed:
            if (inQueue[i])
                m_owner[i] = PieceOwner::P2PActive;
            break;
        case PieceOwner::P2PActive:
            if (!inQueue[i])
                m_owner[i] = PieceOwner::Unclaimed;
            break;
        case PieceOwner::HttpFetching:
        case PieceOwner::HttpSubmitted:
            // An HTTP-owned piece appearing in the peer queue is a double-writer hazard. libtorrent
            // also lists pieces we injected via add_piece while they are being written, so only
            // HttpFetching counts as a genuine collision.
            if (inQueue[i] && (m_owner[i] == PieceOwner::HttpFetching))
                ++m_collisions;
            break;
        case PieceOwner::Verified:
            break;
        }
    }
}

bool Hybrid::PieceOwnership::claimForHttp(const int piece, const quint64 epoch)
{
    if (!valid(piece) || (m_owner[piece] != PieceOwner::Unclaimed))
        return false;
    m_owner[piece] = PieceOwner::HttpFetching;
    m_epoch[piece] = epoch;
    return true;
}

bool Hybrid::PieceOwnership::markSubmitted(const int piece, const quint64 epoch)
{
    if (!valid(piece) || (m_owner[piece] != PieceOwner::HttpFetching) || (m_epoch[piece] != epoch))
        return false;
    m_owner[piece] = PieceOwner::HttpSubmitted;
    return true;
}

void Hybrid::PieceOwnership::releaseFetch(const int piece)
{
    if (valid(piece) && (m_owner[piece] == PieceOwner::HttpFetching))
        m_owner[piece] = PieceOwner::Unclaimed;
}

QList<int> Hybrid::PieceOwnership::releaseAllFetching()
{
    QList<int> released;
    for (std::size_t i = 0; i < m_owner.size(); ++i)
    {
        if (m_owner[i] == PieceOwner::HttpFetching)
        {
            m_owner[i] = PieceOwner::Unclaimed;
            released.append(static_cast<int>(i));
        }
    }
    return released;
}

void Hybrid::PieceOwnership::markVerified(const int piece)
{
    if (!valid(piece))
        return;
    m_owner[piece] = PieceOwner::Verified;
    m_hashFailures[piece] = 0;
}

int Hybrid::PieceOwnership::markHashFailed(const int piece)
{
    if (!valid(piece))
        return 0;
    if (m_owner[piece] == PieceOwner::HttpSubmitted)
    {
        m_owner[piece] = PieceOwner::Unclaimed;
        if (m_hashFailures[piece] < 255)
            ++m_hashFailures[piece];
        return m_hashFailures[piece];
    }
    if (m_owner[piece] == PieceOwner::Verified)
        m_owner[piece] = PieceOwner::Unclaimed;
    return 0;
}

QList<int> Hybrid::PieceOwnership::pickForHttp(const QList<int> &orderedCandidates, const int maxCount) const
{
    QList<int> picked;
    if (maxCount <= 0)
        return picked;
    for (const int piece : orderedCandidates)
    {
        if (valid(piece) && (m_owner[piece] == PieceOwner::Unclaimed))
        {
            picked.append(piece);
            if (picked.size() >= maxCount)
                break;
        }
    }
    return picked;
}

int Hybrid::PieceOwnership::collisions() const
{
    return m_collisions;
}

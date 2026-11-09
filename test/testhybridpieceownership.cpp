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

#include <QTest>

#include "base/hybrid/pieceownership.h"

using namespace Hybrid;

namespace
{
    QBitArray bits(const int n, const QList<int> &set)
    {
        QBitArray b(n);
        for (const int i : set)
            b.setBit(i);
        return b;
    }
}

class TestHybridPieceOwnership final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestHybridPieceOwnership)

public:
    TestHybridPieceOwnership() = default;

private slots:
    void claimLifecycle() const
    {
        PieceOwnership o;
        o.reset(4);
        QVERIFY(o.claimForHttp(1, 7));
        QVERIFY(!o.claimForHttp(1, 7));         // no double claim
        QVERIFY(!o.markSubmitted(1, 6));        // stale epoch rejected
        QVERIFY(o.markSubmitted(1, 7));
        QVERIFY(!o.markSubmitted(1, 7));        // no double submit
        QCOMPARE(o.owner(1), PieceOwner::HttpSubmitted);
        o.markVerified(1);
        QCOMPARE(o.owner(1), PieceOwner::Verified);
        QVERIFY(!o.claimForHttp(1, 8));         // never re-fetch verified pieces
    }

    void p2pQueueExcludesHttp() const
    {
        PieceOwnership o;
        o.reset(6);
        o.reconcileP2PQueue({2, 3});
        QCOMPARE(o.owner(2), PieceOwner::P2PActive);
        QVERIFY(!o.claimForHttp(2, 1));         // I3: peer-active pieces are not HTTP candidates
        const QList<int> picked = o.pickForHttp({0, 1, 2, 3, 4, 5}, 10);
        QCOMPARE(picked, (QList<int> {0, 1, 4, 5}));
        o.reconcileP2PQueue({3});
        QCOMPARE(o.owner(2), PieceOwner::Unclaimed); // drained
        QCOMPARE(o.collisions(), 0);
    }

    void collisionIsCounted() const
    {
        PieceOwnership o;
        o.reset(3);
        QVERIFY(o.claimForHttp(0, 1));
        o.reconcileP2PQueue({0});
        QCOMPARE(o.collisions(), 1);
        QCOMPARE(o.owner(0), PieceOwner::HttpFetching);
    }

    void epochSwitchReleasesFetchingKeepsSubmitted() const
    {
        PieceOwnership o;
        o.reset(4);
        o.claimForHttp(0, 1);
        o.claimForHttp(1, 1);
        o.markSubmitted(1, 1);
        const QList<int> released = o.releaseAllFetching();
        QCOMPARE(released, QList<int> {0});
        QCOMPARE(o.owner(0), PieceOwner::Unclaimed);
        QCOMPARE(o.owner(1), PieceOwner::HttpSubmitted); // awaits libtorrent verdict
    }

    void hashFailureReleases() const
    {
        PieceOwnership o;
        o.reset(2);
        o.claimForHttp(0, 1);
        o.markSubmitted(0, 1);
        QCOMPARE(o.markHashFailed(0), 1);
        QCOMPARE(o.owner(0), PieceOwner::Unclaimed);
        o.claimForHttp(0, 2);
        o.markSubmitted(0, 2);
        QCOMPARE(o.markHashFailed(0), 2);
    }

    void verifiedMirrorsLibtorrent() const
    {
        PieceOwnership o;
        o.reset(4);
        o.reconcileVerified(bits(4, {0, 2}));
        QCOMPARE(o.counts().verified, 2);
        // Recheck lost piece 2: our mirror must not overrule libtorrent.
        o.reconcileVerified(bits(4, {0}));
        QCOMPARE(o.owner(2), PieceOwner::Unclaimed);
    }

    void pickRespectsLimitAndOrder() const
    {
        PieceOwnership o;
        o.reset(10);
        QCOMPARE(o.pickForHttp({9, 3, 5, 1}, 2), (QList<int> {9, 3}));
        QVERIFY(o.pickForHttp({1, 2}, 0).isEmpty());
        QVERIFY(o.pickForHttp({-1, 42}, 5).isEmpty()); // out of range ignored
    }

    void randomizedNoDoubleOwnership() const
    {
        // Property: across random interleavings no piece is ever HTTP-owned and P2P-owned at once.
        PieceOwnership o;
        const int n = 64;
        o.reset(n);
        quint32 seed = 99;
        const auto rnd = [&seed](const int m) { seed = (seed * 1664525u) + 1013904223u; return static_cast<int>((seed >> 8) % static_cast<quint32>(m)); };
        quint64 epoch = 1;
        QList<int> queue;
        for (int step = 0; step < 20000; ++step)
        {
            const int p = rnd(n);
            switch (rnd(7))
            {
            case 0: o.claimForHttp(p, epoch); break;
            case 1: o.markSubmitted(p, epoch); break;
            case 2: o.releaseFetch(p); break;
            case 3: ++epoch; o.releaseAllFetching(); break;
            case 4:
                queue.clear();
                for (int i = 0; i < n; ++i)
                {
                    if ((o.owner(i) == PieceOwner::Unclaimed || o.owner(i) == PieceOwner::P2PActive) && rnd(4) == 0)
                        queue.append(i);
                }
                o.reconcileP2PQueue(queue);
                break;
            case 5: o.markVerified(p); break;
            case 6: o.markHashFailed(p); break;
            }
            const OwnershipCounts c = o.counts();
            QCOMPARE(c.unclaimed + c.httpFetching + c.httpSubmitted + c.p2pActive + c.verified, n);
        }
        QCOMPARE(o.collisions(), 0);
    }
};

QTEST_APPLESS_MAIN(TestHybridPieceOwnership)
#include "testhybridpieceownership.moc"

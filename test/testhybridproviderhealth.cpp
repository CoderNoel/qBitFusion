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

#include "base/hybrid/providerhealth.h"

using namespace Hybrid;

namespace
{
    struct FakeTime
    {
        qint64 now = 1'000'000;
    };

    ProviderHealth make(FakeTime &t, double rnd = 0.5)
    {
        return ProviderHealth({}, [&t] { return t.now; }, [rnd] { return rnd; });
    }
}

class TestHybridProviderHealth final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestHybridProviderHealth)

public:
    TestHybridProviderHealth() = default;

private slots:
    void transientNeedsThreshold() const
    {
        FakeTime t;
        ProviderHealth h = make(t);
        QVERIFY(!h.recordFailure(FailureCategory::Temporary).circuitOpened);
        t.now += 1000;
        QVERIFY(!h.recordFailure(FailureCategory::Temporary).circuitOpened);
        t.now += 1000;
        const HealthDecision d = h.recordFailure(FailureCategory::Temporary);
        QVERIFY(d.circuitOpened);
        QVERIFY(h.isOpen());
        QCOMPARE(d.reason, Reason::ProviderOutage);
        QCOMPARE(d.retryAtMs, t.now + 30'000); // jitter midpoint
    }

    void failuresOutsideWindowDoNotAccumulate() const
    {
        FakeTime t;
        ProviderHealth h = make(t);
        h.recordFailure(FailureCategory::Temporary);
        t.now += 61'000;
        h.recordFailure(FailureCategory::Temporary);
        t.now += 61'000;
        QVERIFY(!h.recordFailure(FailureCategory::Temporary).circuitOpened);
    }

    void permanentOpensImmediately() const
    {
        FakeTime t;
        ProviderHealth h = make(t);
        const HealthDecision d = h.recordFailure(FailureCategory::Auth);
        QVERIFY(d.circuitOpened && d.needsUser);
        QCOMPARE(h.nextProbeAtMs(), std::numeric_limits<qint64>::max()); // no automatic probing
        QVERIFY(!h.recordProbeResult(true));
        h.resetAfterUserAction();
        QVERIFY(!h.needsUser());
        QVERIFY(!h.recordProbeResult(true));
        QVERIFY(h.recordProbeResult(true));  // 2 successes required
        QVERIFY(!h.isOpen());
    }

    void forbiddenIsNeverRetriedAutomatically() const
    {
        FakeTime t;
        ProviderHealth h = make(t);
        QVERIFY(h.recordFailure(FailureCategory::Permission).needsUser);
        t.now += 3'600'000;
        QCOMPARE(h.nextProbeAtMs(), std::numeric_limits<qint64>::max());
    }

    void linkExpiryRefreshesOnce() const
    {
        FakeTime t;
        ProviderHealth h = make(t);
        HealthDecision d = h.recordFailure(FailureCategory::LinkExpired, std::nullopt, false);
        QVERIFY(d.refreshLink && !d.circuitOpened);
        d = h.recordFailure(FailureCategory::LinkExpired, std::nullopt, true);
        QVERIFY(d.circuitOpened);
        QCOMPARE(d.reason, Reason::ContentUnavailable);
    }

    void retryAfterIsHonoured() const
    {
        FakeTime t;
        ProviderHealth h = make(t);
        const HealthDecision d = h.recordFailure(FailureCategory::RateLimit, 120);
        QVERIFY(!d.circuitOpened);
        QCOMPARE(d.retryAtMs, t.now + 120'000);
        h.recordFailure(FailureCategory::RateLimit, 120);
        const HealthDecision opened = h.recordFailure(FailureCategory::RateLimit, 600);
        QVERIFY(opened.circuitOpened);
        QVERIFY(opened.retryAtMs >= t.now + 600'000); // Retry-After beats shorter backoff
    }

    void backoffDoublesToCapWithJitter() const
    {
        FakeTime t;
        ProviderHealth h = make(t, 1.0); // max jitter +20%
        h.recordFailure(FailureCategory::Gone);
        QCOMPARE(h.nextProbeAtMs() - t.now, 36'000);
        qint64 last = 0;
        for (int i = 0; i < 10; ++i)
        {
            h.recordProbeResult(false);
            last = h.nextProbeAtMs() - t.now;
        }
        QCOMPARE(last, 360'000); // 5 min cap * 1.2
    }

    void flappingRequiresConsecutiveSuccesses() const
    {
        FakeTime t;
        ProviderHealth h = make(t);
        h.recordFailure(FailureCategory::BadResponse);
        QVERIFY(!h.recordProbeResult(true));
        QVERIFY(!h.recordProbeResult(false)); // resets the streak
        QVERIFY(!h.recordProbeResult(true));
        QVERIFY(h.recordProbeResult(true));
        QCOMPARE(h.lastReason(), Reason::RecoveryConfirmed);
    }

    void dwell() const
    {
        FakeTime t;
        ProviderHealth h = make(t);
        QVERIFY(h.dwellSatisfied());
        h.noteSourceSwitch();
        QVERIFY(!h.dwellSatisfied());
        t.now += 29'999;
        QVERIFY(!h.dwellSatisfied());
        t.now += 1;
        QVERIFY(h.dwellSatisfied());
    }
};

QTEST_APPLESS_MAIN(TestHybridProviderHealth)
#include "testhybridproviderhealth.moc"

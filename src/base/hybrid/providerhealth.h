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

#include <functional>
#include <limits>
#include <optional>

#include <QtTypes>
#include <QList>

#include "hybridtypes.h"

// Circuit breaker + recovery hysteresis (spec 04 "Classification: avoid flapping").
// All numbers are initial hypotheses to be tuned by measurement, not wire-compatible constants.

namespace Hybrid
{
    struct HealthConfig
    {
        int failureThreshold = 3;
        qint64 failureWindowMs = 60'000;
        qint64 minDwellMs = 30'000;
        qint64 backoffInitialMs = 30'000;
        qint64 backoffMaxMs = 300'000;
        double jitterFraction = 0.2;
        int recoverySuccessesRequired = 2;
    };

    struct HealthDecision
    {
        bool circuitOpened = false;   // transition to fallback/wait now
        bool needsUser = false;       // auth/permission/mismatch: no automatic recovery probes
        bool refreshLink = false;     // retry once with a freshly resolved link
        qint64 retryAtMs = 0;         // earliest next attempt (0 = immediately)
        Reason reason = Reason::None;
    };

    class ProviderHealth
    {
    public:
        using Clock = std::function<qint64 ()>;
        using Random = std::function<double ()>;  // uniform [0, 1)

        ProviderHealth(HealthConfig config, Clock clock, Random random);

        const HealthConfig &config() const;
        bool isOpen() const;
        bool needsUser() const;
        Reason lastReason() const;

        HealthDecision recordFailure(FailureCategory category, std::optional<qint64> retryAfterSeconds = std::nullopt
                , bool linkAlreadyRefreshed = false);
        void recordSuccess();

        // Recovery probing while open. Returns true when enough consecutive successes close it.
        qint64 nextProbeAtMs() const;
        bool recordProbeResult(bool ok);

        // User fixed credentials / re-authorized: allow probing again.
        void resetAfterUserAction();

        // Anti ping-pong: a source switch is allowed only after the dwell time, unless permanent.
        void noteSourceSwitch();
        bool dwellSatisfied() const;

    private:
        qint64 jittered(qint64 delayMs) const;
        void open(Reason reason, bool needsUser, std::optional<qint64> retryAfterMs);

        HealthConfig m_config;
        Clock m_clock;
        Random m_random;

        QList<qint64> m_failureTimes;
        bool m_open = false;
        bool m_needsUser = false;
        Reason m_reason = Reason::None;
        qint64 m_backoffMs = 0;
        qint64 m_nextProbeAtMs = 0;
        int m_consecutiveProbeSuccesses = 0;
        qint64 m_lastSwitchMs = std::numeric_limits<qint64>::min() / 2;
    };

    Reason reasonFor(FailureCategory category);
}

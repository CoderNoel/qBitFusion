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

#include "providerhealth.h"

#include <algorithm>
#include <cmath>

Hybrid::Reason Hybrid::reasonFor(const FailureCategory category)
{
    switch (category)
    {
    case FailureCategory::None:
        return Reason::None;
    case FailureCategory::Auth:
        return Reason::ProviderUnauthorized;
    case FailureCategory::Permission:
        return Reason::ProviderForbidden;
    case FailureCategory::Gone:
    case FailureCategory::LinkExpired:
        return Reason::ContentUnavailable;
    case FailureCategory::RateLimit:
        return Reason::ProviderRateLimited;
    case FailureCategory::Temporary:
        return Reason::ProviderOutage;
    case FailureCategory::BadResponse:
        return Reason::NoByteRangeSupport;
    case FailureCategory::Integrity:
        return Reason::IntegrityFailure;
    case FailureCategory::Disk:
        return Reason::DiskError;
    }
    return Reason::None;
}

Hybrid::ProviderHealth::ProviderHealth(HealthConfig config, Clock clock, Random random)
    : m_config {std::move(config)}
    , m_clock {std::move(clock)}
    , m_random {std::move(random)}
{
    m_backoffMs = m_config.backoffInitialMs;
}

const Hybrid::HealthConfig &Hybrid::ProviderHealth::config() const
{
    return m_config;
}

bool Hybrid::ProviderHealth::isOpen() const
{
    return m_open;
}

bool Hybrid::ProviderHealth::needsUser() const
{
    return m_needsUser;
}

Hybrid::Reason Hybrid::ProviderHealth::lastReason() const
{
    return m_reason;
}

qint64 Hybrid::ProviderHealth::jittered(const qint64 delayMs) const
{
    const double r = m_random ? std::clamp(m_random(), 0.0, 1.0) : 0.5;
    const double factor = 1.0 + (m_config.jitterFraction * ((2.0 * r) - 1.0));
    return std::max<qint64>(0, static_cast<qint64>(std::llround(static_cast<double>(delayMs) * factor)));
}

void Hybrid::ProviderHealth::open(const Reason reason, const bool needsUser, const std::optional<qint64> retryAfterMs)
{
    const qint64 now = m_clock();
    if (!m_open)
        m_backoffMs = m_config.backoffInitialMs;
    else
        m_backoffMs = std::min(m_backoffMs * 2, m_config.backoffMaxMs);

    m_open = true;
    m_needsUser = m_needsUser || needsUser;
    m_reason = reason;
    m_consecutiveProbeSuccesses = 0;
    const qint64 delay = std::max(jittered(m_backoffMs), retryAfterMs.value_or(0));
    m_nextProbeAtMs = now + delay;
}

Hybrid::HealthDecision Hybrid::ProviderHealth::recordFailure(const FailureCategory category, const std::optional<qint64> retryAfterSeconds
        , const bool linkAlreadyRefreshed)
{
    HealthDecision decision;
    decision.reason = reasonFor(category);
    const qint64 now = m_clock();
    const std::optional<qint64> retryAfterMs = retryAfterSeconds
            ? std::optional<qint64>(*retryAfterSeconds * 1000) : std::nullopt;

    switch (category)
    {
    case FailureCategory::None:
        return decision;

    case FailureCategory::LinkExpired:
        if (!linkAlreadyRefreshed)
        {
            decision.refreshLink = true;
            return decision;
        }
        [[fallthrough]];
    case FailureCategory::Gone:
    case FailureCategory::BadResponse:
        // Availability / protocol risk: open immediately. Recovery requires revalidation, which
        // probes perform, so no user action is strictly required.
        open(decision.reason, false, retryAfterMs);
        decision.circuitOpened = true;
        decision.retryAtMs = m_nextProbeAtMs;
        return decision;

    case FailureCategory::Auth:
    case FailureCategory::Permission:
    case FailureCategory::Integrity:
        // Retrying cannot fix these; never try to bypass a 403. Provider bytes that failed the
        // original torrent's hash quarantine the binding until the user intervenes (spec 04).
        open(decision.reason, true, retryAfterMs);
        decision.circuitOpened = true;
        decision.needsUser = true;
        decision.retryAtMs = 0;
        return decision;

    case FailureCategory::Disk:
        decision.needsUser = true;
        return decision;

    case FailureCategory::RateLimit:
    case FailureCategory::Temporary:
        break;
    }

    // Transient: count within the window, honour Retry-After for the next attempt.
    m_failureTimes.append(now);
    m_failureTimes.removeIf([this, now](const qint64 t) { return (now - t) > m_config.failureWindowMs; });

    if (m_failureTimes.size() >= m_config.failureThreshold)
    {
        open(decision.reason, false, retryAfterMs);
        decision.circuitOpened = true;
        decision.retryAtMs = m_nextProbeAtMs;
        m_failureTimes.clear();
    }
    else
    {
        decision.retryAtMs = retryAfterMs ? (now + *retryAfterMs) : 0;
    }
    return decision;
}

void Hybrid::ProviderHealth::recordSuccess()
{
    if (!m_open)
        m_failureTimes.clear();
}

qint64 Hybrid::ProviderHealth::nextProbeAtMs() const
{
    return m_needsUser ? std::numeric_limits<qint64>::max() : m_nextProbeAtMs;
}

bool Hybrid::ProviderHealth::recordProbeResult(const bool ok)
{
    if (!m_open)
        return true;
    if (m_needsUser)
        return false;

    if (!ok)
    {
        m_consecutiveProbeSuccesses = 0;
        open(m_reason, false, std::nullopt);  // doubles backoff
        return false;
    }

    ++m_consecutiveProbeSuccesses;
    if (m_consecutiveProbeSuccesses >= m_config.recoverySuccessesRequired)
    {
        m_open = false;
        m_reason = Reason::RecoveryConfirmed;
        m_backoffMs = m_config.backoffInitialMs;
        m_failureTimes.clear();
        m_consecutiveProbeSuccesses = 0;
        return true;
    }
    // Next confirmation probe soon, not after the full backoff.
    m_nextProbeAtMs = m_clock() + jittered(std::min<qint64>(m_config.backoffInitialMs, 5'000));
    return false;
}

void Hybrid::ProviderHealth::resetAfterUserAction()
{
    m_needsUser = false;
    m_open = true; // still require probes to confirm before using the provider
    m_consecutiveProbeSuccesses = 0;
    m_backoffMs = m_config.backoffInitialMs;
    m_nextProbeAtMs = m_clock();
}

void Hybrid::ProviderHealth::noteSourceSwitch()
{
    m_lastSwitchMs = m_clock();
}

bool Hybrid::ProviderHealth::dwellSatisfied() const
{
    return (m_clock() - m_lastSwitchMs) >= m_config.minDwellMs;
}

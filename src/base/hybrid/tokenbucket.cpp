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

#include "tokenbucket.h"

#include <algorithm>
#include <cmath>
#include <limits>

Hybrid::TokenBucket::TokenBucket(const qint64 bytesPerSecond, const qint64 burstBytes)
{
    setRate(bytesPerSecond, burstBytes);
}

void Hybrid::TokenBucket::setRate(const qint64 bytesPerSecond, const qint64 burstBytes)
{
    m_rate = std::max<qint64>(0, bytesPerSecond);
    // Default burst: 1/4 second worth of data, at least 16 KiB, so short pieces are not starved.
    m_burst = (burstBytes > 0) ? burstBytes : std::max<qint64>(16 * 1024, m_rate / 4);
    m_tokens = std::min<double>(m_tokens, static_cast<double>(m_burst));
}

qint64 Hybrid::TokenBucket::rate() const
{
    return m_rate;
}

bool Hybrid::TokenBucket::isUnlimited() const
{
    return m_rate <= 0;
}

void Hybrid::TokenBucket::refill(const qint64 nowMs)
{
    if (m_lastMs < 0)
    {
        m_lastMs = nowMs;
        m_tokens = static_cast<double>(m_burst);
        return;
    }
    const qint64 elapsed = std::max<qint64>(0, nowMs - m_lastMs);
    m_lastMs = nowMs;
    m_tokens = std::min<double>(static_cast<double>(m_burst), m_tokens + ((static_cast<double>(m_rate) * static_cast<double>(elapsed)) / 1000.0));
}

qint64 Hybrid::TokenBucket::take(const qint64 wanted, const qint64 nowMs)
{
    if (wanted <= 0)
        return 0;
    if (isUnlimited())
        return wanted;

    refill(nowMs);
    const auto granted = std::min<qint64>(wanted, static_cast<qint64>(std::floor(m_tokens)));
    m_tokens -= static_cast<double>(std::max<qint64>(0, granted));
    return std::max<qint64>(0, granted);
}

qint64 Hybrid::TokenBucket::msUntilAvailable(const qint64 nowMs)
{
    if (isUnlimited())
        return 0;
    refill(nowMs);
    if (m_tokens >= 1.0)
        return 0;
    const double missing = 1.0 - m_tokens;
    return std::max<qint64>(1, static_cast<qint64>(std::ceil((missing * 1000.0) / static_cast<double>(m_rate))));
}

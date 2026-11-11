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

#include <QObject>
#include <QString>

// UI- and libtorrent-independent vocabulary for the hybrid (debrid + BitTorrent) transport.
// Spec: qbitfusion/docs/SPECIFICATION.md sections 01 (policies), 04 (states, reasons), 10 (status schema).

namespace Hybrid
{
    Q_NAMESPACE

    // Per-torrent transport policy. Never silently promote DebridOnly to DebridPreferred (ADR-004).
    enum class TransportPolicy : int
    {
        TorrentOnly = 0,
        DebridOnly = 1,
        DebridPreferred = 2
    };
    Q_ENUM_NS(TransportPolicy)

    // Normative per-torrent states (spec 04). Actual state is re-derived after restart.
    enum class TransportState : int
    {
        Disabled,
        AwaitingMetadata,
        ProbingProvider,
        HttpReady,
        HttpActive,
        HttpDraining,
        P2PActive,
        RecoveryProbing,
        WaitingForProvider,
        WaitingForUser,
        Paused,
        Checking,
        Completed,
        Error
    };
    Q_ENUM_NS(TransportState)

    // Which producer currently supplies pieces. Display only; libtorrent owns progress.
    enum class ActiveTransport : int
    {
        None,
        Http,
        P2P,
        Switching
    };
    Q_ENUM_NS(ActiveTransport)

    // Reason codes for transitions and user-visible errors (spec 01 "Errors the user must understand").
    enum class Reason : int
    {
        None,
        UserRequest,
        MetadataVerified,
        MetadataUnavailableWithoutP2P,
        ProviderReady,
        ProviderUnauthorized,
        ProviderRateLimited,
        ProviderForbidden,
        Uncached,
        ProviderFetching,
        ContentUnavailable,
        UnmatchedTorrentFiles,
        ProviderLacksPieces,
        NoByteRangeSupport,
        ProviderOutage,
        IntegrityFailure,
        NoSwarmPeers,
        WaitingForRetry,
        DiskError,
        PermissionRequiredForP2P,
        RecoveryConfirmed,
        PeersDrained,
        RecheckStarted,
        RecheckFinished,
        DownloadComplete,
        TorrentRemoved
    };
    Q_ENUM_NS(Reason)

    // Provider-side availability as presented to the user.
    enum class ProviderStatus : int
    {
        Unknown,
        Cached,
        FetchingRemotely,
        NotAvailable,
        RateLimited,
        AuthenticationRequired,
        ContentRestricted,
        Offline
    };
    Q_ENUM_NS(ProviderStatus)

    // Classified failure category from provider API or data plane (spec 04 classification table).
    enum class FailureCategory : int
    {
        None,
        Auth,          // 401: retry cannot fix credentials
        Permission,    // 403: never bypass
        Gone,          // 404 after refresh / removed
        LinkExpired,   // 404 from CDN before refresh
        RateLimit,     // 429
        Temporary,     // 5xx, timeouts, TLS/connection failures
        BadResponse,   // wrong Content-Range, 200 to Range, transformed body
        Integrity,     // libtorrent hash failure
        Disk           // local storage
    };
    Q_ENUM_NS(FailureCategory)

    // Persisted, stable identifiers. Changing them breaks sidecar/WebAPI compatibility.
    QString wireName(TransportPolicy policy);
    TransportPolicy policyFromWireName(const QString &str, TransportPolicy fallback = TransportPolicy::TorrentOnly);
    QString wireName(TransportState state);
    QString wireName(ActiveTransport transport);
    QString wireName(Reason reason);
    Reason reasonFromWireName(const QString &str);
    QString wireName(ProviderStatus status);
    QString wireName(FailureCategory category);

    // Localized, user-facing presentation. Never include URLs, tokens or hashes.
    QString displayName(TransportPolicy policy);
    QString displayName(TransportState state);
    QString displayText(Reason reason);
    QString displayName(ProviderStatus status);

    // Whether the policy ever permits contacting the BitTorrent swarm, given consent.
    constexpr bool policyAllowsP2P(const TransportPolicy policy, const bool fallbackConsent)
    {
        switch (policy)
        {
        case TransportPolicy::TorrentOnly:
            return true;
        case TransportPolicy::DebridOnly:
            return false;
        case TransportPolicy::DebridPreferred:
            return fallbackConsent;
        }
        return false;
    }
}

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

#include "hybridtypes.h"

#include <QCoreApplication>
#include <QMetaEnum>

using namespace Qt::Literals::StringLiterals;

namespace
{
    QString tr(const char *text)
    {
        return QCoreApplication::translate("Hybrid", text);
    }

    template <typename Enum>
    QString metaKey(const Enum value)
    {
        return QString::fromLatin1(QMetaEnum::fromType<Enum>().valueToKey(static_cast<int>(value)));
    }

    // Stable wire identifiers for reasons (snake_case; used in events, sidecar and WebAPI).
    struct ReasonId
    {
        Hybrid::Reason reason;
        QStringView id;
    };

    constexpr ReasonId REASON_IDS[] =
    {
        {Hybrid::Reason::None, u"none"},
        {Hybrid::Reason::UserRequest, u"user_request"},
        {Hybrid::Reason::MetadataVerified, u"metadata_verified"},
        {Hybrid::Reason::MetadataUnavailableWithoutP2P, u"metadata_unavailable_without_p2p"},
        {Hybrid::Reason::ProviderReady, u"provider_ready"},
        {Hybrid::Reason::ProviderUnauthorized, u"provider_unauthorized"},
        {Hybrid::Reason::ProviderRateLimited, u"provider_rate_limited"},
        {Hybrid::Reason::ProviderForbidden, u"provider_forbidden"},
        {Hybrid::Reason::Uncached, u"uncached"},
        {Hybrid::Reason::ProviderFetching, u"provider_fetching"},
        {Hybrid::Reason::ContentUnavailable, u"content_unavailable"},
        {Hybrid::Reason::UnmatchedTorrentFiles, u"unmatched_torrent_files"},
        {Hybrid::Reason::ProviderLacksPieces, u"provider_lacks_pieces"},
        {Hybrid::Reason::NoByteRangeSupport, u"no_byte_range_support"},
        {Hybrid::Reason::ProviderOutage, u"provider_outage"},
        {Hybrid::Reason::IntegrityFailure, u"integrity_failure"},
        {Hybrid::Reason::NoSwarmPeers, u"no_swarm_peers"},
        {Hybrid::Reason::WaitingForRetry, u"waiting_for_retry"},
        {Hybrid::Reason::DiskError, u"disk_error"},
        {Hybrid::Reason::PermissionRequiredForP2P, u"permission_required_for_p2p"},
        {Hybrid::Reason::RecoveryConfirmed, u"recovery_confirmed"},
        {Hybrid::Reason::PeersDrained, u"peers_drained"},
        {Hybrid::Reason::RecheckStarted, u"recheck_started"},
        {Hybrid::Reason::RecheckFinished, u"recheck_finished"},
        {Hybrid::Reason::DownloadComplete, u"download_complete"},
        {Hybrid::Reason::TorrentRemoved, u"torrent_removed"}
    };
}

QString Hybrid::wireName(const TransportPolicy policy)
{
    switch (policy)
    {
    case TransportPolicy::TorrentOnly:
        return u"torrent_only"_s;
    case TransportPolicy::DebridOnly:
        return u"debrid_only"_s;
    case TransportPolicy::DebridPreferred:
        return u"debrid_preferred"_s;
    }
    return u"torrent_only"_s;
}

Hybrid::TransportPolicy Hybrid::policyFromWireName(const QString &str, const TransportPolicy fallback)
{
    if (str == u"torrent_only")
        return TransportPolicy::TorrentOnly;
    if (str == u"debrid_only")
        return TransportPolicy::DebridOnly;
    if (str == u"debrid_preferred")
        return TransportPolicy::DebridPreferred;
    return fallback;
}

QString Hybrid::wireName(const TransportState state)
{
    // UPPER_SNAKE to match the normative names in spec 04.
    QString key = metaKey(state);
    QString result;
    result.reserve(key.size() + 8);
    for (qsizetype i = 0; i < key.size(); ++i)
    {
        const QChar ch = key.at(i);
        const bool boundary = (i > 0) && ch.isUpper()
                && (key.at(i - 1).isLower() || ((i + 1 < key.size()) && key.at(i + 1).isLower() && key.at(i - 1).isUpper()));
        if (boundary)
            result.append(u'_');
        result.append(ch.toUpper());
    }
    return result;
}

QString Hybrid::wireName(const ActiveTransport transport)
{
    switch (transport)
    {
    case ActiveTransport::None:
        return u"none"_s;
    case ActiveTransport::Http:
        return u"http"_s;
    case ActiveTransport::P2P:
        return u"p2p"_s;
    case ActiveTransport::Switching:
        return u"switching"_s;
    }
    return u"none"_s;
}

QString Hybrid::wireName(const Reason reason)
{
    for (const ReasonId &entry : REASON_IDS)
    {
        if (entry.reason == reason)
            return entry.id.toString();
    }
    return u"none"_s;
}

Hybrid::Reason Hybrid::reasonFromWireName(const QString &str)
{
    for (const ReasonId &entry : REASON_IDS)
    {
        if (entry.id == str)
            return entry.reason;
    }
    return Reason::None;
}

QString Hybrid::wireName(const ProviderStatus status)
{
    switch (status)
    {
    case ProviderStatus::Unknown:
        return u"unknown"_s;
    case ProviderStatus::Cached:
        return u"cached"_s;
    case ProviderStatus::FetchingRemotely:
        return u"fetching_remotely"_s;
    case ProviderStatus::NotAvailable:
        return u"not_available"_s;
    case ProviderStatus::RateLimited:
        return u"rate_limited"_s;
    case ProviderStatus::AuthenticationRequired:
        return u"authentication_required"_s;
    case ProviderStatus::ContentRestricted:
        return u"content_restricted"_s;
    case ProviderStatus::Offline:
        return u"offline"_s;
    }
    return u"unknown"_s;
}

QString Hybrid::wireName(const FailureCategory category)
{
    return metaKey(category).toLower();
}

QString Hybrid::displayName(const TransportPolicy policy)
{
    switch (policy)
    {
    case TransportPolicy::TorrentOnly:
        return tr("BitTorrent (standard)");
    case TransportPolicy::DebridOnly:
        return tr("Debrid only");
    case TransportPolicy::DebridPreferred:
        return tr("Debrid preferred, BitTorrent fallback");
    }
    return {};
}

QString Hybrid::displayName(const TransportState state)
{
    switch (state)
    {
    case TransportState::Disabled:
        return tr("BitTorrent");
    case TransportState::AwaitingMetadata:
        return tr("Awaiting metadata");
    case TransportState::ProbingProvider:
        return tr("Checking provider");
    case TransportState::HttpReady:
        return tr("Provider ready");
    case TransportState::HttpActive:
        return tr("HTTPS");
    case TransportState::HttpDraining:
        return tr("Switching");
    case TransportState::P2PActive:
        return tr("BitTorrent");
    case TransportState::RecoveryProbing:
        return tr("BitTorrent, checking provider");
    case TransportState::WaitingForProvider:
        return tr("Waiting for provider");
    case TransportState::WaitingForUser:
        return tr("Action required");
    case TransportState::Paused:
        return tr("Paused");
    case TransportState::Checking:
        return tr("Checking");
    case TransportState::Completed:
        return tr("Completed");
    case TransportState::Error:
        return tr("Error");
    }
    return {};
}

QString Hybrid::displayText(const Reason reason)
{
    switch (reason)
    {
    case Reason::None:
        return {};
    case Reason::UserRequest:
        return tr("Changed by user");
    case Reason::MetadataVerified:
        return tr("Torrent metadata verified");
    case Reason::MetadataUnavailableWithoutP2P:
        return tr("Metadata unavailable without BitTorrent peers");
    case Reason::ProviderReady:
        return tr("Provider copy verified; remaining pieces will be checked against the original torrent");
    case Reason::ProviderUnauthorized:
        return tr("Provider unauthorized");
    case Reason::ProviderRateLimited:
        return tr("Provider rate-limited");
    case Reason::ProviderForbidden:
        return tr("Content restricted by provider");
    case Reason::Uncached:
        return tr("Uncached remotely");
    case Reason::ProviderFetching:
        return tr("Provider fetching");
    case Reason::ContentUnavailable:
        return tr("Content unavailable");
    case Reason::UnmatchedTorrentFiles:
        return tr("Unmatched torrent files");
    case Reason::ProviderLacksPieces:
        return tr("Remaining pieces are not on the provider");
    case Reason::NoByteRangeSupport:
        return tr("Provider does not support byte ranges");
    case Reason::ProviderOutage:
        return tr("Provider unavailable");
    case Reason::IntegrityFailure:
        return tr("Integrity verification failed");
    case Reason::NoSwarmPeers:
        return tr("No available swarm peers");
    case Reason::WaitingForRetry:
        return tr("Waiting for retry");
    case Reason::DiskError:
        return tr("Disk error");
    case Reason::PermissionRequiredForP2P:
        return tr("Permission required for BitTorrent fallback");
    case Reason::RecoveryConfirmed:
        return tr("Provider available again");
    case Reason::PeersDrained:
        return tr("Outstanding peer pieces finished");
    case Reason::RecheckStarted:
        return tr("Recheck started");
    case Reason::RecheckFinished:
        return tr("Recheck finished");
    case Reason::DownloadComplete:
        return tr("Download complete");
    case Reason::TorrentRemoved:
        return tr("Torrent removed");
    }
    return {};
}

QString Hybrid::displayName(const ProviderStatus status)
{
    switch (status)
    {
    case ProviderStatus::Unknown:
        return tr("Unknown");
    case ProviderStatus::Cached:
        return tr("Cached");
    case ProviderStatus::FetchingRemotely:
        return tr("Fetching remotely");
    case ProviderStatus::NotAvailable:
        return tr("Not available");
    case ProviderStatus::RateLimited:
        return tr("Rate-limited");
    case ProviderStatus::AuthenticationRequired:
        return tr("Authentication required");
    case ProviderStatus::ContentRestricted:
        return tr("Content restricted");
    case ProviderStatus::Offline:
        return tr("Offline");
    }
    return {};
}

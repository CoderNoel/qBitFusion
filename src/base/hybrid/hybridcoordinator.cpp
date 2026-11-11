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

#include "hybridcoordinator.h"

#include <algorithm>
#include <limits>
#include <memory>

#include <QNetworkAccessManager>
#include <QRandomGenerator>
#include <QTimer>

#include "base/global.h"
#include "rangesource.h"
#include "tokenbucket.h"
#include "torrentpiecesink.h"

using namespace Qt::Literals::StringLiterals;

// ---------------------------------------------------------------------------------- TransferBudget

Hybrid::TransferBudget::TransferBudget(const int maxPieces, const qint64 maxBufferedBytes)
    : m_maxPieces {std::max(1, maxPieces)}
    , m_maxBytes {std::max<qint64>(1, maxBufferedBytes)}
{
}

void Hybrid::TransferBudget::setLimits(const int maxPieces, const qint64 maxBufferedBytes)
{
    m_maxPieces = std::max(1, maxPieces);
    m_maxBytes = std::max<qint64>(1, maxBufferedBytes);
}

bool Hybrid::TransferBudget::tryAcquire(const qint64 bytes)
{
    if ((bytes <= 0) || (m_pieces >= m_maxPieces))
        return false;
    // Always allow one piece even if it alone exceeds the cap, so huge pieces cannot deadlock;
    // otherwise the configured in-flight byte cap is never exceeded (spec 00 "HTTP memory bound").
    if ((m_pieces > 0) && ((m_bytes + bytes) > m_maxBytes))
        return false;
    ++m_pieces;
    m_bytes += bytes;
    m_peakBytes = std::max(m_peakBytes, m_bytes);
    return true;
}

void Hybrid::TransferBudget::release(const qint64 bytes)
{
    m_pieces = std::max(0, m_pieces - 1);
    m_bytes = std::max<qint64>(0, m_bytes - bytes);
}

int Hybrid::TransferBudget::piecesInFlight() const
{
    return m_pieces;
}

qint64 Hybrid::TransferBudget::bytesInFlight() const
{
    return m_bytes;
}

qint64 Hybrid::TransferBudget::peakBytes() const
{
    return m_peakBytes;
}

// ------------------------------------------------------------------------------ HybridCoordinator

namespace
{
    Hybrid::ProviderStatus statusFor(const Hybrid::Availability availability)
    {
        switch (availability)
        {
        case Hybrid::Availability::Cached:
            return Hybrid::ProviderStatus::Cached;
        case Hybrid::Availability::RemoteFetching:
            return Hybrid::ProviderStatus::FetchingRemotely;
        case Hybrid::Availability::Uncached:
        case Hybrid::Availability::Removed:
            return Hybrid::ProviderStatus::NotAvailable;
        case Hybrid::Availability::Unknown:
            break;
        }
        return Hybrid::ProviderStatus::Unknown;
    }

    Hybrid::ProviderStatus statusFor(const Hybrid::FailureCategory category)
    {
        switch (category)
        {
        case Hybrid::FailureCategory::Auth:
            return Hybrid::ProviderStatus::AuthenticationRequired;
        case Hybrid::FailureCategory::Permission:
            return Hybrid::ProviderStatus::ContentRestricted;
        case Hybrid::FailureCategory::RateLimit:
            return Hybrid::ProviderStatus::RateLimited;
        case Hybrid::FailureCategory::Gone:
            return Hybrid::ProviderStatus::NotAvailable;
        default:
            return Hybrid::ProviderStatus::Offline;
        }
    }

    constexpr qint64 PREFLIGHT_BYTES = 1024;

    bool isPermanent(const Hybrid::FailureCategory category)
    {
        return (category == Hybrid::FailureCategory::Auth) || (category == Hybrid::FailureCategory::Permission);
    }

    // RangeError::detail is already redacted (scheme and host only). Adds what the validator saw.
    QString describeRangeError(const Hybrid::RangeError &error)
    {
        return u"%1 [HTTP %2, check %3]"_s.arg((error.detail.isEmpty() ? u"range request failed"_s : error.detail)
                , QString::number(error.httpStatus), QString::number(static_cast<int>(error.verdict)));
    }
}

Hybrid::HybridCoordinator::HybridCoordinator(TorrentPieceSink *sink, Provider *provider, QNetworkAccessManager *network
        , TransferBudget *budget, TokenBucket *bucket, CoordinatorConfig config, Clock clock, QObject *parent)
    : QObject(parent)
    , m_sink {sink}
    , m_provider {provider}
    , m_network {network}
    , m_budget {budget}
    , m_bucket {bucket}
    , m_config {std::move(config)}
    , m_clock {std::move(clock)}
    , m_health {m_config.health, m_clock, [] { return QRandomGenerator::global()->generateDouble(); }}
    , m_eventTimer {new QTimer(this)}
    , m_drainTimer {new QTimer(this)}
    , m_dispatchTimer {new QTimer(this)}
    , m_rateTimer {new QTimer(this)}
{
    m_eventTimer->setSingleShot(true);
    connect(m_eventTimer, &QTimer::timeout, this, [this] { fire(m_timerEvent); });

    m_drainTimer->setSingleShot(true);
    connect(m_drainTimer, &QTimer::timeout, this, [this]
    {
        // Bounded drain: hash-pending pieces keep their ownership; libtorrent will still deliver
        // their verdict and the arbiter reconciles it (spec 04 step 7).
        if (m_machine.state() == TransportState::HttpDraining)
            fire(MachineEvent::HttpJobsSettled);
    });

    m_dispatchTimer->setSingleShot(true);
    connect(m_dispatchTimer, &QTimer::timeout, this, &HybridCoordinator::schedule);

    m_rateTimer->setInterval(1000);
    connect(m_rateTimer, &QTimer::timeout, this, [this]
    {
        // Exponential moving average of HTTP goodput, for display only.
        const qint64 sample = m_rateWindowBytes;
        m_rateWindowBytes = 0;
        const qint64 previous = m_status.httpRateBps;
        m_status.httpRateBps = (previous == 0) ? sample : ((previous * 2) + sample) / 3;
        if ((previous != m_status.httpRateBps) || (sample > 0))
            emit statusChanged();
    });

    if (m_provider)
        m_status.providerName = m_provider->displayName();
}

Hybrid::HybridCoordinator::~HybridCoordinator()
{
    abortHttp();
}

Hybrid::MachineContext Hybrid::HybridCoordinator::context() const
{
    return {m_policy, m_consent, m_sink->hasMetadata()};
}

Hybrid::HybridStatus Hybrid::HybridCoordinator::status() const
{
    HybridStatus s = m_status;
    s.policy = m_policy;
    s.state = m_machine.state();
    s.activeTransport = activeTransport(s.state);
    s.p2pFallbackApproved = m_consent && (m_policy == TransportPolicy::DebridPreferred);
    s.httpInFlight = static_cast<int>(m_assemblies.size());
    return s;
}

Hybrid::TransportState Hybrid::HybridCoordinator::state() const
{
    return m_machine.state();
}

quint64 Hybrid::HybridCoordinator::epoch() const
{
    return m_epoch;
}

const Hybrid::PieceOwnership &Hybrid::HybridCoordinator::ownership() const
{
    return m_ownership;
}

bool Hybrid::HybridCoordinator::wantsTransportHold() const
{
    const TransportState s = m_machine.state();
    if (m_policy == TransportPolicy::TorrentOnly)
        return false;
    // Metadata via P2P is allowed only when the user has consented to swarm activity.
    if ((s == TransportState::AwaitingMetadata) && policyAllowsP2P(m_policy, m_consent))
        return false;
    if ((s == TransportState::Completed) && policyAllowsP2P(m_policy, m_consent))
        return false; // seeding follows normal qBittorrent rules once the user allowed P2P
    return !swarmAllowed(s, context());
}

void Hybrid::HybridCoordinator::start(const TransportPolicy policy, const bool p2pConsent)
{
    m_policy = policy;
    m_consent = (policy == TransportPolicy::DebridOnly) ? false : p2pConsent;
    m_ownership.reset(m_sink->hasMetadata() ? m_sink->pieceCount() : 0);
    if (m_sink->hasMetadata())
        m_ownership.reconcileVerified(m_sink->havePieces());
    m_machine.resetForRestart({m_policy, m_consent, m_sink->hasMetadata()});
    if (m_policy != TransportPolicy::TorrentOnly)
        m_rateTimer->start();
    applyHold();
    enterState({TransportState::Disabled, m_machine.state(), Reason::None, true});
}

void Hybrid::HybridCoordinator::setPolicy(const TransportPolicy policy, const bool p2pConsent)
{
    const bool consent = (policy == TransportPolicy::DebridOnly) ? false : p2pConsent;
    const bool policyChanged = (policy != m_policy);
    const bool consentChanged = (consent != m_consent);
    if (!policyChanged && !consentChanged)
        return;

    m_policy = policy;
    m_consent = consent;
    if (m_policy != TransportPolicy::TorrentOnly)
        m_rateTimer->start();
    else
        m_rateTimer->stop();

    if ((m_machine.state() == TransportState::Disabled) && (m_policy != TransportPolicy::TorrentOnly))
        fire(MachineEvent::Enable, Reason::UserRequest);
    else
        fire(policyChanged ? MachineEvent::PolicyChanged : MachineEvent::ConsentChanged, Reason::UserRequest);

    emit policyPersistRequested(m_policy, m_consent, m_status.lastReason, m_status.switchCount);
    emit statusChanged();
}

void Hybrid::HybridCoordinator::setProvider(Provider *provider)
{
    if (provider == m_provider)
        return;
    abortHttp();
    bumpEpoch();
    m_provider = provider;
    m_links.clear();
    m_linkRefreshed.clear();
    m_bound = false;
    m_status.providerName = provider ? provider->displayName() : QString();
    m_health.resetAfterUserAction();
    if (m_policy != TransportPolicy::TorrentOnly)
        fire(MachineEvent::PolicyChanged, Reason::UserRequest);
}

void Hybrid::HybridCoordinator::fire(const MachineEvent event, const Reason reason)
{
    const Transition t = m_machine.handle(event, context(), reason);
    if (t.changed)
        enterState(t);
}

void Hybrid::HybridCoordinator::bumpEpoch()
{
    ++m_epoch;
}

void Hybrid::HybridCoordinator::abortHttp()
{
    for (auto it = m_assemblies.begin(); it != m_assemblies.end(); ++it)
    {
        if (it.value())
        {
            it.value()->abort();
            it.value()->deleteLater();
        }
        if (m_budget)
            m_budget->release(m_assemblyBytes.value(it.key()));
    }
    m_assemblies.clear();
    m_assemblyBytes.clear();
    m_ownership.releaseAllFetching();
    m_dispatchTimer->stop();
}

void Hybrid::HybridCoordinator::applyHold()
{
    const bool hold = wantsTransportHold();
    if (hold == m_holding)
        return;
    m_holding = hold;
    m_sink->setTransportHold(hold);
}

void Hybrid::HybridCoordinator::armTimer(const qint64 atMs, const MachineEvent event)
{
    m_timerEvent = event;
    const qint64 delay = std::clamp<qint64>(atMs - m_clock(), 0, std::numeric_limits<int>::max());
    m_eventTimer->start(static_cast<int>(delay));
}

void Hybrid::HybridCoordinator::enterState(const Transition &transition)
{
    using S = TransportState;

    m_eventTimer->stop();
    m_drainTimer->stop();

    const ActiveTransport before = activeTransport(transition.from);
    const ActiveTransport after = activeTransport(transition.to);
    if ((before != after) && (after != ActiveTransport::None) && (before != ActiveTransport::None))
    {
        ++m_status.switchCount;
        m_health.noteSourceSwitch();
    }
    if (transition.reason != Reason::None)
        m_status.lastReason = transition.reason;
    m_status.lastTransitionUtc = QDateTime::currentDateTimeUtc();

    switch (transition.to)
    {
    case S::Disabled:
    case S::Paused:
    case S::Checking:
    case S::Error:
        abortHttp();
        bumpEpoch();
        break;

    case S::AwaitingMetadata:
        abortHttp();
        if (m_policy == TransportPolicy::DebridOnly)
        {
            // No provider metadata route in the MVP: never fall back to swarm metadata (I5).
            const ProviderCapabilities caps = m_provider ? m_provider->capabilities() : ProviderCapabilities {};
            if (!caps.canSupplyTorrentMetadata)
            {
                applyHold();
                fire(MachineEvent::ProviderNeedsUser, Reason::MetadataUnavailableWithoutP2P);
                return;
            }
        }
        break;

    case S::ProbingProvider:
        bumpEpoch();
        abortHttp();
        if (m_sink->hasMetadata() && (m_ownership.pieceCount() != m_sink->pieceCount()))
        {
            m_ownership.reset(m_sink->pieceCount());
        }
        if (m_sink->hasMetadata())
            m_ownership.reconcileVerified(m_sink->havePieces());
        applyHold();
        probe(false);
        break;

    case S::HttpActive:
        applyHold();
        schedule();
        break;

    case S::HttpDraining:
        bumpEpoch();
        abortHttp();
        m_drainTimer->start(m_config.drainTimeoutMs);
        checkDrained();
        break;

    case S::P2PActive:
        abortHttp();
        armRecoveryProbe();
        break;

    case S::RecoveryProbing:
        probe(true);
        break;

    case S::HttpReady:
        // Provider confirmed: drain peers with a graceful pause before HTTP takes over (spike E6).
        applyHold();
        if (m_sink->isTransportHoldSettled())
        {
            QMetaObject::invokeMethod(this, [this] { notifyHoldSettled(); }, Qt::QueuedConnection);
        }
        break;

    case S::WaitingForProvider:
        abortHttp();
        {
            const qint64 next = m_health.isOpen() ? m_health.nextProbeAtMs()
                    : (m_clock() + m_config.health.backoffInitialMs);
            if (next != std::numeric_limits<qint64>::max())
                armTimer(std::max(next, m_dispatchNotBeforeMs), MachineEvent::RetryDue);
        }
        break;

    case S::WaitingForUser:
        abortHttp();
        break;

    case S::Completed:
        abortHttp();
        break;
    }

    applyHold();
    emit transitioned(transition.from, transition.to, transition.reason);
    emit policyPersistRequested(m_policy, m_consent, m_status.lastReason, m_status.switchCount);
    emit statusChanged();
}

// ------------------------------------------------------------------------------- provider probing

void Hybrid::HybridCoordinator::probe(const bool recovery)
{
    if (!m_provider || (m_provider->accountState() != AccountState::Connected))
    {
        // Not signed in / expired sign-in needs the user, not retries; a missing provider (feature
        // disabled) is simply unavailable.
        const auto category = m_provider ? FailureCategory::Auth : FailureCategory::Temporary;
        QMetaObject::invokeMethod(this, [this, recovery, category]
        {
            onProbeResult(false, recovery, category, (category == FailureCategory::Auth) ? Reason::ProviderUnauthorized : Reason::ProviderOutage, std::nullopt);
        }, Qt::QueuedConnection);
        return;
    }

    m_status.torrentFileCount = 0;
    m_status.providerFileCount = 0;
    m_status.boundFileCount = 0;
    m_status.providerPieces = 0;
    m_status.peerOnlyPieces = 0;
    m_status.matchFailure.clear();
    m_status.failureDetail.clear();

    const quint64 epoch = m_epoch;
    m_provider->resolveManifest(m_sink->identity(), m_config.allowRemoteCreate, this
            , [this, epoch, recovery](const ProviderResult<ProviderManifest> &result)
    {
        if (epoch != m_epoch)
            return; // stale network callback (spec 04 step 7)

        if (!result)
        {
            m_status.failureDetail = result.error().message;
            onProbeResult(false, recovery, result.error().category, reasonFor(result.error().category), result.error().retryAfterSeconds);
            return;
        }

        m_manifest = result.value();
        m_status.providerStatus = statusFor(m_manifest.availability);
        if (m_manifest.availability != Availability::Cached)
        {
            const Reason reason = (m_manifest.availability == Availability::RemoteFetching) ? Reason::ProviderFetching
                    : (m_manifest.availability == Availability::Removed) ? Reason::ContentUnavailable : Reason::Uncached;
            onProbeResult(false, recovery, FailureCategory::Temporary, reason, std::nullopt);
            return;
        }

        const QList<TorrentFileEntry> nativeFiles = m_sink->nativeFiles();
        m_status.torrentFileCount = static_cast<int>(std::count_if(nativeFiles.cbegin(), nativeFiles.cend()
                , [](const TorrentFileEntry &file) { return !file.isPad; }));
        m_status.providerFileCount = static_cast<int>(m_manifest.files.size());

        const auto binding = bindFiles(nativeFiles, m_manifest.files);
        if (!binding)
        {
            // Wrong layout is a hard, non-retriable signal: never guess (ADR-005).
            m_bound = false;
            m_status.matchFailure = describe(binding.error().error);
            onProbeResult(false, recovery, FailureCategory::Permission, Reason::UnmatchedTorrentFiles, std::nullopt);
            return;
        }
        m_binding = binding.value();
        m_bound = true;
        m_status.boundFileCount = static_cast<int>(m_binding.byNativeIndex.size());
        rebuildPieceCoverage();

        // Coverage is decided per piece (ADR-009, docs/DECISIONS.md). A piece goes over HTTP
        // only if the provider exposes every byte of it; a piece touching a file it does not expose (for
        // example a small text file sharing a piece with a large video) is left to peers. Such
        // pieces must never keep the provider from supplying everything else.
        for (const int piece : m_sink->wantedMissingPieces())
            ++(isPieceBound(piece) ? m_status.providerPieces : m_status.peerOnlyPieces);

        if ((m_status.providerPieces == 0) && (m_status.peerOnlyPieces > 0))
        {
            // The provider has nothing usable for what is still missing. Not retriable.
            onProbeResult(false, recovery, FailureCategory::Permission, Reason::ProviderLacksPieces, std::nullopt);
            return;
        }

        preflight(recovery);
    });
}

void Hybrid::HybridCoordinator::preflight(const bool recovery)
{
    // Range preflight on one bound file: a small range must yield 206 + exact Content-Range and
    // total. HEAD alone is insufficient (spec 03 section 4 step 5). The range is the first KiB, not
    // a single byte: live Real-Debrid download servers were seen to answer "bytes=0-0" with a 206
    // whose Content-Length did not match, while piece requests never end at offset 0.
    if (m_binding.byNativeIndex.isEmpty())
    {
        onProbeResult(true, recovery, FailureCategory::None, Reason::ProviderReady, std::nullopt);
        return;
    }

    const int fileIndex = m_binding.byNativeIndex.constBegin().key();
    const quint64 epoch = m_epoch;
    ensureLinks({fileIndex}, [this, epoch, fileIndex, recovery](const bool ok, const ProviderError &error)
    {
        if (epoch != m_epoch)
            return;
        if (!ok)
        {
            // A link that cannot be resolved is not evidence about byte ranges: report it as what
            // it is, so the log and the Source column do not point at the wrong cause.
            m_status.failureDetail = error.message;
            const Reason reason = (error.category == FailureCategory::BadResponse) ? Reason::ProviderOutage : reasonFor(error.category);
            onProbeResult(false, recovery, error.category, reason, error.retryAfterSeconds);
            return;
        }

        const TemporaryLink link = m_links.value(fileIndex);
        RangeRequest request {link.url, {0, std::min<qint64>(link.fileSize, PREFLIGHT_BYTES) - 1}, link.fileSize, m_config.timeouts};
        auto *fetch = new RangeFetch(m_network, request, m_provider->dataUrlPolicy(), nullptr, m_clock, this);
        connect(fetch, &RangeFetch::succeeded, this, [this, fetch, epoch, recovery]
        {
            fetch->deleteLater();
            if (epoch == m_epoch)
                onProbeResult(true, recovery, FailureCategory::None, Reason::ProviderReady, std::nullopt);
        });
        connect(fetch, &RangeFetch::failed, this, [this, fetch, epoch, recovery, fileIndex](const RangeError &rangeError)
        {
            fetch->deleteLater();
            if (epoch != m_epoch)
                return;
            // The next attempt asks the provider for a new link instead of repeating this one: a
            // link names one download server, and that server may be the part that is failing.
            m_links.remove(fileIndex);
            m_status.failureDetail = describeRangeError(rangeError);
            const Reason reason = (rangeError.category == FailureCategory::BadResponse) ? Reason::NoByteRangeSupport : reasonFor(rangeError.category);
            onProbeResult(false, recovery, rangeError.category, reason, rangeError.retryAfterSeconds);
        });
        fetch->start();
    });
}

void Hybrid::HybridCoordinator::onProbeResult(const bool ok, const bool recovery, const FailureCategory category
        , const Reason reason, const std::optional<qint64> retryAfter)
{
    if (ok)
    {
        m_status.providerStatus = ProviderStatus::Cached;
        m_status.retryAfterSeconds = 0;
        if (!recovery)
        {
            m_health.recordProbeResult(true);
            m_health.recordSuccess();
            fire(MachineEvent::ProviderReady, Reason::ProviderReady);
            return;
        }

        const bool recovered = m_health.recordProbeResult(true);
        if (recovered && m_health.dwellSatisfied())
        {
            fire(MachineEvent::RecoveryProbeOk, Reason::RecoveryConfirmed);
        }
        else
        {
            fire(MachineEvent::RecoveryProbeFailed, Reason::WaitingForRetry);
            armTimer(std::max(m_health.nextProbeAtMs(), m_clock() + 1000), MachineEvent::RecoveryDue);
        }
        return;
    }

    // Reasons that describe the content, not the account, keep the status the manifest reported.
    const bool keepProviderStatus = (reason == Reason::Uncached) || (reason == Reason::ProviderFetching)
            || (reason == Reason::UnmatchedTorrentFiles) || (reason == Reason::ProviderLacksPieces);
    if ((category != FailureCategory::None) && !keepProviderStatus)
        m_status.providerStatus = statusFor(category);
    m_status.retryAfterSeconds = retryAfter.value_or(0);

    // Unavailability during probing is a single decisive observation: open the circuit so the
    // backoff schedule governs the next attempt.
    if (!m_health.isOpen() || isPermanent(category))
    {
        const FailureCategory c = isPermanent(category) ? category : FailureCategory::Gone;
        m_health.recordFailure(c, retryAfter, true);
    }
    else
    {
        // Already open: every failed probe must push the next attempt into the future (doubling
        // backoff). Without this, nextProbeAtMs stays in the past and probing busy-loops.
        m_health.recordProbeResult(false);
    }

    const bool needsUser = isPermanent(category) || (reason == Reason::UnmatchedTorrentFiles);
    if (recovery)
        fire(needsUser ? MachineEvent::ProviderNeedsUser : MachineEvent::RecoveryProbeFailed, reason);
    else
        fire(needsUser ? MachineEvent::ProviderNeedsUser : MachineEvent::ProviderUnavailable, reason);

    if (recovery && (m_machine.state() == TransportState::P2PActive) && !needsUser)
        armTimer(m_health.nextProbeAtMs(), MachineEvent::RecoveryDue);
}

// ------------------------------------------------------------------------------- data plane

bool Hybrid::HybridCoordinator::isPieceBound(const int piece) const
{
    return m_bound && (piece >= 0) && (piece < m_pieceBound.size()) && m_pieceBound.testBit(piece);
}

void Hybrid::HybridCoordinator::rebuildPieceCoverage()
{
    // Computed once per binding: schedule() consults it for every missing piece.
    const int count = m_sink->pieceCount();
    m_pieceBound = QBitArray(count);
    for (int piece = 0; piece < count; ++piece)
    {
        const QList<PieceSlice> slices = m_sink->pieceSlices(piece);
        const bool bound = std::all_of(slices.cbegin(), slices.cend(), [this](const PieceSlice &slice)
        {
            return slice.isPad || m_binding.byNativeIndex.contains(slice.nativeFileIndex);
        });
        m_pieceBound.setBit(piece, bound);
    }
}

bool Hybrid::HybridCoordinator::hasProviderWork() const
{
    if (!m_bound)
        return true; // unknown until the next probe
    const QList<int> wanted = m_sink->wantedMissingPieces();
    return std::any_of(wanted.cbegin(), wanted.cend(), [this](const int piece) { return isPieceBound(piece); });
}

void Hybrid::HybridCoordinator::armRecoveryProbe()
{
    // Going back to the provider is pointless when it needs the user or has nothing more to give:
    // each attempt would pause the peers only to return here.
    if (m_health.needsUser() || !m_provider || (m_provider->accountState() != AccountState::Connected) || !hasProviderWork())
        return;
    armTimer(std::max(m_health.nextProbeAtMs(), m_clock() + m_config.health.minDwellMs), MachineEvent::RecoveryDue);
}

void Hybrid::HybridCoordinator::ensureLinks(const QList<int> &fileIndexes, std::function<void (bool, const ProviderError &)> done)
{
    QList<int> missing;
    for (const int index : fileIndexes)
    {
        if (!m_links.contains(index))
            missing.append(index);
    }
    if (missing.isEmpty())
    {
        done(true, {});
        return;
    }
    if (!m_provider)
    {
        done(false, {FailureCategory::Temporary, u"no provider"_s, std::nullopt});
        return;
    }

    // Resolve sequentially; per-torrent link fan-out is small and provider APIs are rate-limited.
    const int index = missing.first();
    const FileBinding binding = m_binding.byNativeIndex.value(index);
    const quint64 epoch = m_epoch;
    m_provider->resolveFileLink(m_manifest.resourceId, binding.providerFileId, this
            , [this, epoch, index, fileIndexes, done = std::move(done), expected = binding.size](const ProviderResult<TemporaryLink> &result)
    {
        if (epoch != m_epoch)
            return;
        if (!result)
        {
            done(false, result.error());
            return;
        }
        TemporaryLink link = result.value();
        if ((link.fileSize >= 0) && (link.fileSize != expected))
        {
            done(false, {FailureCategory::BadResponse, u"provider file size changed"_s, std::nullopt});
            return;
        }
        link.fileSize = expected;
        m_links.insert(index, link);
        ensureLinks(fileIndexes, done);
    });
}

void Hybrid::HybridCoordinator::schedule()
{
    if (!httpDispatchAllowed(m_machine.state()) || m_sink->isChecking() || !m_provider)
        return;

    const qint64 now = m_clock();
    if (now < m_dispatchNotBeforeMs)
    {
        m_dispatchTimer->start(static_cast<int>(std::min<qint64>(m_dispatchNotBeforeMs - now, 60'000)));
        return;
    }

    const QList<int> wanted = m_sink->wantedMissingPieces();
    if (wanted.isEmpty() && m_assemblies.isEmpty() && (m_ownership.counts().httpSubmitted == 0))
        return; // completion arrives through notifyFinished()

    // Only pieces the provider can supply in full are candidates. Filtering before picking matters:
    // an unbound piece must not occupy a slot that a later, bound piece could use.
    QList<int> candidates;
    candidates.reserve(wanted.size());
    for (const int piece : wanted)
    {
        if (isPieceBound(piece))
            candidates.append(piece);
    }

    if (candidates.isEmpty())
    {
        // Possibly done with the provider. Decided from the event loop: schedule() also runs while
        // a state is being entered, and a nested transition would be announced out of order.
        QMetaObject::invokeMethod(this, &HybridCoordinator::checkHttpExhausted, Qt::QueuedConnection);
        return;
    }

    const int freeSlots = m_config.maxPiecesInFlight - static_cast<int>(m_assemblies.size());
    if (freeSlots <= 0)
        return;

    for (const int piece : m_ownership.pickForHttp(candidates, freeSlots))
    {
        const qint64 length = m_sink->pieceLength(piece);
        if (m_budget && !m_budget->tryAcquire(length))
        {
            // Global budget exhausted: try again shortly (other torrents release memory).
            m_dispatchTimer->start(100);
            break;
        }
        if (!m_ownership.claimForHttp(piece, m_epoch))
        {
            if (m_budget)
                m_budget->release(length);
            continue;
        }
        m_assemblyBytes.insert(piece, length);
        dispatch(piece);
    }
    m_status.httpInFlight = static_cast<int>(m_assemblies.size());
}

void Hybrid::HybridCoordinator::checkHttpExhausted()
{
    if (!httpDispatchAllowed(m_machine.state()) || m_sink->isChecking() || !m_bound)
        return;
    if (!m_assemblies.isEmpty() || (m_ownership.counts().httpSubmitted != 0))
        return; // verdicts still pending; a failed piece would become a candidate again
    if (m_sink->wantedMissingPieces().isEmpty() || hasProviderWork())
        return;

    // Everything the provider can supply is verified; what is left needs bytes it does not expose.
    fire(MachineEvent::HttpExhausted, Reason::ProviderLacksPieces);
}

void Hybrid::HybridCoordinator::dispatch(const int piece)
{
    PieceJob job;
    job.piece = piece;
    job.epoch = m_epoch;
    job.length = m_sink->pieceLength(piece);
    job.slices = m_sink->pieceSlices(piece);

    QList<int> files;
    for (const PieceSlice &slice : asConst(job.slices))
    {
        if (!slice.isPad && !files.contains(slice.nativeFileIndex))
            files.append(slice.nativeFileIndex);
    }

    // Placeholder entry so concurrency accounting is immediate even while links resolve.
    m_assemblies.insert(piece, nullptr);
    const quint64 epoch = m_epoch;
    ensureLinks(files, [this, job = std::move(job), files, epoch](const bool ok, const ProviderError &error) mutable
    {
        if ((epoch != m_epoch) || !m_assemblies.contains(job.piece))
            return;
        if (!ok)
        {
            onAssemblyFailed(job.piece, epoch, {.category = error.category, .retryAfterSeconds = error.retryAfterSeconds
                    , .detail = error.message}, files.value(0, -1));
            return;
        }

        QHash<int, FileSource> sources;
        for (const int index : files)
        {
            const TemporaryLink link = m_links.value(index);
            sources.insert(index, {link.url, link.fileSize});
        }

        auto *assembly = new PieceAssembly(m_network, job, sources, m_provider->dataUrlPolicy(), m_bucket, m_clock
                , m_config.timeouts, this);
        m_assemblies.insert(job.piece, assembly);
        connect(assembly, &PieceAssembly::progressed, this, [this](const qint64 bytes)
        {
            m_status.httpBytesReceived += bytes;
            m_rateWindowBytes += bytes;
        });
        connect(assembly, &PieceAssembly::completed, this, &HybridCoordinator::onAssemblyCompleted);
        connect(assembly, &PieceAssembly::failed, this, &HybridCoordinator::onAssemblyFailed);
        assembly->start();
    });
}

void Hybrid::HybridCoordinator::onAssemblyCompleted(const int piece, const quint64 epoch, const PieceBuffer &buffer)
{
    const QPointer<PieceAssembly> assembly = m_assemblies.take(piece);
    if (assembly)
        assembly->deleteLater();
    if (m_budget)
        m_budget->release(m_assemblyBytes.take(piece));

    // Never submit stale-generation bytes, during checking, or after a transport switch.
    if ((epoch != m_epoch) || !httpDispatchAllowed(m_machine.state()) || m_sink->isChecking() || !buffer)
    {
        m_ownership.releaseFetch(piece);
        return;
    }
    if (!m_ownership.markSubmitted(piece, epoch))
        return;

    m_status.httpBytesSubmitted += static_cast<qint64>(buffer->size());
    m_sink->addPiece(piece, std::move(*buffer));
    m_health.recordSuccess();
    schedule();
}

void Hybrid::HybridCoordinator::onAssemblyFailed(const int piece, const quint64 epoch, const RangeError &error, const int nativeFileIndex)
{
    const QPointer<PieceAssembly> assembly = m_assemblies.take(piece);
    if (assembly)
        assembly->deleteLater();
    if (m_budget)
        m_budget->release(m_assemblyBytes.take(piece));
    if (epoch != m_epoch)
        return; // already released by releaseAllFetching()

    m_ownership.releaseFetch(piece);
    m_status.failureDetail = describeRangeError(error);
    handleHttpFailure(error.category, error.retryAfterSeconds, nativeFileIndex);
}

void Hybrid::HybridCoordinator::handleHttpFailure(const FailureCategory category, const std::optional<qint64> retryAfter, const int nativeFileIndex)
{
    const bool refreshed = m_linkRefreshed.contains(nativeFileIndex);
    const HealthDecision decision = m_health.recordFailure(category, retryAfter, refreshed);

    if (decision.refreshLink)
    {
        // One bounded refresh per file (spec 04: "Refresh link once per limited window").
        m_links.remove(nativeFileIndex);
        m_linkRefreshed.insert(nativeFileIndex);
        schedule();
        return;
    }

    if (decision.circuitOpened)
    {
        m_status.providerStatus = statusFor(category);
        fire(MachineEvent::HttpOutage, decision.reason);
        return;
    }

    if (decision.retryAtMs > m_clock())
        m_dispatchNotBeforeMs = decision.retryAtMs;
    schedule();
}

void Hybrid::HybridCoordinator::checkDrained()
{
    if (m_machine.state() != TransportState::HttpDraining)
        return;
    if (m_assemblies.isEmpty() && (m_ownership.counts().httpSubmitted == 0))
    {
        m_drainTimer->stop();
        fire(MachineEvent::HttpJobsSettled);
    }
}

// ------------------------------------------------------------------------------- notifications

void Hybrid::HybridCoordinator::notifyMetadataReady()
{
    m_ownership.reset(m_sink->pieceCount());
    m_ownership.reconcileVerified(m_sink->havePieces());
    fire(MachineEvent::MetadataReady, Reason::MetadataVerified);
}

void Hybrid::HybridCoordinator::notifyCheckingStarted()
{
    fire(MachineEvent::RecheckStarted, Reason::RecheckStarted);
}

void Hybrid::HybridCoordinator::notifyCheckingFinished()
{
    m_ownership.reconcileVerified(m_sink->havePieces());
    fire(MachineEvent::RecheckFinished, Reason::RecheckFinished);
}

void Hybrid::HybridCoordinator::notifyUserStopped()
{
    fire(MachineEvent::UserPause, Reason::UserRequest);
}

void Hybrid::HybridCoordinator::notifyUserStarted()
{
    fire(MachineEvent::UserResume, Reason::UserRequest);
}

void Hybrid::HybridCoordinator::notifyFinished()
{
    fire(MachineEvent::Completed, Reason::DownloadComplete);
}

void Hybrid::HybridCoordinator::notifyPieceFinished(const int piece)
{
    const PieceOwner previous = m_ownership.owner(piece);
    m_ownership.markVerified(piece);
    if (previous == PieceOwner::HttpSubmitted)
    {
        ++m_status.httpVerifiedPieces;
        m_linkRefreshed.clear();
    }
    if (m_machine.state() == TransportState::HttpDraining)
        checkDrained();
    else
        schedule();
}

void Hybrid::HybridCoordinator::notifyHashFailed(const int piece)
{
    // Libtorrent alerts are always reconciled, even for older generations (spec 04 step 7).
    if (m_ownership.owner(piece) != PieceOwner::HttpSubmitted)
        return; // a peer's bad piece: libtorrent's own smart-ban handles it

    m_ownership.markHashFailed(piece);
    ++m_status.httpHashFailures;
    // Provider bytes failed the original torrent's hash: quarantine the binding immediately.
    m_bound = false;
    m_links.clear();
    const HealthDecision decision = m_health.recordFailure(FailureCategory::Integrity);
    m_status.providerStatus = ProviderStatus::Offline;
    if (m_machine.state() == TransportState::HttpDraining)
        checkDrained();
    else if (decision.circuitOpened)
        fire(MachineEvent::HttpOutage, Reason::IntegrityFailure);
}

void Hybrid::HybridCoordinator::notifyHoldSettled()
{
    if (m_machine.state() == TransportState::HttpReady)
        fire(MachineEvent::PeersDrained, Reason::PeersDrained);
}

void Hybrid::HybridCoordinator::notifyFilePrioritiesChanged()
{
    // Newly unwanted pieces: cancel their HTTP work; the planner rebuilds from libtorrent.
    const QList<int> wanted = m_sink->wantedMissingPieces();
    const QSet<int> wantedSet {wanted.cbegin(), wanted.cend()};
    for (auto it = m_assemblies.begin(); it != m_assemblies.end();)
    {
        if (!wantedSet.contains(it.key()))
        {
            if (it.value())
            {
                it.value()->abort();
                it.value()->deleteLater();
            }
            if (m_budget)
                m_budget->release(m_assemblyBytes.take(it.key()));
            m_ownership.releaseFetch(it.key());
            it = m_assemblies.erase(it);
        }
        else
        {
            ++it;
        }
    }
    schedule();

    // Newly wanted files may be ones the provider has: let a peers-only torrent go back to it.
    if ((m_machine.state() == TransportState::P2PActive) && !m_eventTimer->isActive())
        armRecoveryProbe();
}

void Hybrid::HybridCoordinator::notifyProviderAccountChanged()
{
    m_links.clear();
    m_linkRefreshed.clear();
    m_bound = false;
    m_health.resetAfterUserAction();
    const TransportState s = m_machine.state();
    if ((s == TransportState::WaitingForUser) || (s == TransportState::WaitingForProvider))
        fire(MachineEvent::RetryDue, Reason::UserRequest);
}

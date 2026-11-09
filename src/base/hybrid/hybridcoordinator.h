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

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>

#include "filebinding.h"
#include "hybridtypes.h"
#include "pieceassembler.h"
#include "pieceownership.h"
#include "provider.h"
#include "providerhealth.h"
#include "transportstatemachine.h"

class QNetworkAccessManager;
class QTimer;

namespace Hybrid
{
    class TokenBucket;
    class TorrentPieceSink;

    // Process-wide limits shared by all coordinators (spec 02 "Resource limits").
    class TransferBudget
    {
    public:
        TransferBudget(int maxPieces, qint64 maxBufferedBytes);

        void setLimits(int maxPieces, qint64 maxBufferedBytes);
        bool tryAcquire(qint64 bytes);
        void release(qint64 bytes);

        int piecesInFlight() const;
        qint64 bytesInFlight() const;
        qint64 peakBytes() const;

    private:
        int m_maxPieces;
        qint64 m_maxBytes;
        int m_pieces = 0;
        qint64 m_bytes = 0;
        qint64 m_peakBytes = 0;
    };

    struct CoordinatorConfig
    {
        int maxPiecesInFlight = 4;
        bool allowRemoteCreate = false;   // mutating provider call; explicit user setting
        RangeTimeouts timeouts;
        HealthConfig health;
        int drainTimeoutMs = 10'000;
    };

    struct HybridStatus
    {
        TransportPolicy policy = TransportPolicy::TorrentOnly;
        TransportState state = TransportState::Disabled;
        ActiveTransport activeTransport = ActiveTransport::None;
        ProviderStatus providerStatus = ProviderStatus::Unknown;
        QString providerName;
        bool p2pFallbackApproved = false;
        Reason lastReason = Reason::None;
        QDateTime lastTransitionUtc;
        qint64 httpBytesReceived = 0;
        qint64 httpBytesSubmitted = 0;
        int httpVerifiedPieces = 0;
        int httpHashFailures = 0;
        int httpInFlight = 0;
        qint64 httpRateBps = 0;
        quint64 switchCount = 0;
        qint64 retryAfterSeconds = 0;
    };

    class HybridCoordinator final : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(HybridCoordinator)

    public:
        using Clock = std::function<qint64 ()>;

        HybridCoordinator(TorrentPieceSink *sink, Provider *provider, QNetworkAccessManager *network
                , TransferBudget *budget, TokenBucket *bucket, CoordinatorConfig config, Clock clock
                , QObject *parent = nullptr);
        ~HybridCoordinator() override;

        // Restart-safe: everything is re-derived from libtorrent + provider (invariant I6).
        void start(TransportPolicy policy, bool p2pConsent);
        void setPolicy(TransportPolicy policy, bool p2pConsent);
        void setProvider(Provider *provider);

        HybridStatus status() const;
        TransportState state() const;
        quint64 epoch() const;
        const PieceOwnership &ownership() const;
        bool wantsTransportHold() const;

        // Torrent lifecycle notifications (main thread).
        void notifyMetadataReady();
        void notifyCheckingStarted();
        void notifyCheckingFinished();
        void notifyUserStopped();
        void notifyUserStarted();
        void notifyFinished();
        void notifyPieceFinished(int piece);
        void notifyHashFailed(int piece);
        void notifyHoldSettled();
        void notifyFilePrioritiesChanged();
        void notifyProviderAccountChanged();

    signals:
        void statusChanged();
        // Structured, secret-free transition event (spec 04 "Event and observability contract").
        void transitioned(Hybrid::TransportState from, Hybrid::TransportState to, Hybrid::Reason reason);
        void policyPersistRequested(Hybrid::TransportPolicy policy, bool p2pConsent, Hybrid::Reason reason, quint64 switchCount);

    private:
        MachineContext context() const;
        void fire(MachineEvent event, Reason reason = Reason::None);
        void enterState(const Transition &transition);
        void applyHold();
        void abortHttp();
        void bumpEpoch();
        void armTimer(qint64 atMs, MachineEvent event);

        void probe(bool recovery);
        void onProbeResult(bool ok, bool recovery, FailureCategory category, Reason reason, std::optional<qint64> retryAfter);
        void preflight(bool recovery);

        void schedule();
        void dispatch(int piece);
        void ensureLinks(const QList<int> &fileIndexes, std::function<void (bool ok, const ProviderError &error)> done);
        void onAssemblyCompleted(int piece, quint64 epoch, const PieceBuffer &buffer);
        void onAssemblyFailed(int piece, quint64 epoch, const RangeError &error, int nativeFileIndex);
        void handleHttpFailure(FailureCategory category, std::optional<qint64> retryAfter, int nativeFileIndex);
        void checkDrained();
        bool isPieceBound(int piece) const;

        TorrentPieceSink *m_sink = nullptr;
        QPointer<Provider> m_provider;
        QNetworkAccessManager *m_network = nullptr;
        TransferBudget *m_budget = nullptr;
        TokenBucket *m_bucket = nullptr;
        CoordinatorConfig m_config;
        Clock m_clock;

        TransportStateMachine m_machine;
        ProviderHealth m_health;
        PieceOwnership m_ownership;
        TransportPolicy m_policy = TransportPolicy::TorrentOnly;
        bool m_consent = false;
        bool m_holding = false;
        quint64 m_epoch = 1;

        ProviderManifest m_manifest;
        BindingResult m_binding;
        bool m_bound = false;
        QHash<int, TemporaryLink> m_links;          // memory only
        QSet<int> m_linkRefreshed;
        QHash<int, QPointer<PieceAssembly>> m_assemblies;
        QHash<int, qint64> m_assemblyBytes;
        qint64 m_dispatchNotBeforeMs = 0;

        QTimer *m_eventTimer = nullptr;
        MachineEvent m_timerEvent = MachineEvent::RetryDue;
        QTimer *m_drainTimer = nullptr;
        QTimer *m_dispatchTimer = nullptr;
        QTimer *m_rateTimer = nullptr;
        qint64 m_rateWindowBytes = 0;

        HybridStatus m_status;
    };
}

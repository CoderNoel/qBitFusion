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

#include <memory>
#include <optional>

#include <QHash>
#include <QObject>

#include "base/bittorrent/hybridhooks.h"
#include "base/bittorrent/infohash.h"
#include "hybridcoordinator.h"
#include "hybridsidecar.h"
#include "tokenbucket.h"

class QNetworkAccessManager;
class QTimer;

namespace BitTorrent
{
    class HybridTorrentSink;
    class Torrent;
}

namespace Hybrid
{
    class RealDebridProvider;

    // Process-wide owner of the hybrid transport (spec 06 "hybridmanager"): settings, provider
    // registry, global budgets and one coordinator per hybrid torrent. Torrents without an explicit
    // hybrid policy are never touched (invariant I7).
    class HybridManager final : public QObject, public BitTorrent::HybridHooks
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(HybridManager)

    public:
        static void initInstance();   // before BitTorrent::Session::initInstance()
        static void freeInstance();
        static HybridManager *instance();

        void attachSession();         // after BitTorrent::Session::initInstance()

        // ---- global settings (Downloads > Hybrid sources)
        bool isEnabled() const;
        void setEnabled(bool enabled);
        TransportPolicy defaultPolicy() const;
        void setDefaultPolicy(TransportPolicy policy);
        bool defaultP2PConsent() const;
        void setDefaultP2PConsent(bool consent);
        bool allowRemoteCreate() const;
        void setAllowRemoteCreate(bool allow);
        int maxPiecesPerTorrent() const;
        void setMaxPiecesPerTorrent(int value);
        int maxGlobalPieces() const;
        void setMaxGlobalPieces(int value);
        int maxBufferedMiB() const;
        void setMaxBufferedMiB(int value);

        RealDebridProvider *realDebrid() const;
        Provider *activeProvider() const;

        // ---- per torrent
        void setPendingPolicy(const BitTorrent::TorrentID &id, TransportPolicy policy, bool p2pConsent);
        TransportPolicy policyFor(const BitTorrent::TorrentID &id) const;
        bool p2pConsentFor(const BitTorrent::TorrentID &id) const;
        void setTorrentPolicy(BitTorrent::Torrent *torrent, TransportPolicy policy, bool p2pConsent);
        std::optional<HybridStatus> statusFor(const BitTorrent::TorrentID &id) const;
        int hybridTorrentCount() const;

        // ---- BitTorrent::HybridHooks
        bool shouldHoldOnAdd(const BitTorrent::TorrentID &id) const override;
        bool shouldHoldOnLoad(const BitTorrent::TorrentID &id) const override;
        bool needsPieceAlerts() const override;
        void handlePieceFinished(BitTorrent::Torrent *torrent, int pieceIndex) override;
        void handlePieceHashFailed(BitTorrent::Torrent *torrent, int pieceIndex) override;
        void handleTorrentPaused(BitTorrent::Torrent *torrent) override;
        void handleRecheckRequested(BitTorrent::Torrent *torrent) override;

    signals:
        void torrentStatusChanged(const BitTorrent::TorrentID &id);
        void settingsChanged();
        void providerAccountChanged();

    private:
        HybridManager();
        ~HybridManager() override;

        struct Entry
        {
            std::shared_ptr<BitTorrent::HybridTorrentSink> sink;
            std::shared_ptr<HybridCoordinator> coordinator;
        };

        SidecarData sidecarFor(const BitTorrent::TorrentID &id) const;
        void saveSidecar(const BitTorrent::TorrentID &id, const SidecarData &data);
        void ensureEntry(BitTorrent::Torrent *torrent);
        void dropEntry(const BitTorrent::TorrentID &id);
        HybridCoordinator *coordinatorFor(const BitTorrent::Torrent *torrent) const;
        CoordinatorConfig coordinatorConfig() const;
        void onTick();

        static HybridManager *m_instance;

        QNetworkAccessManager *m_network = nullptr;
        SidecarStore m_store;
        TransferBudget m_budget;
        TokenBucket m_bucket;
        RealDebridProvider *m_realDebrid = nullptr;
        QTimer *m_tick = nullptr;
        bool m_pieceAlerts = false;

        mutable QHash<BitTorrent::TorrentID, SidecarData> m_sidecarCache;
        QHash<BitTorrent::TorrentID, SidecarData> m_pending;
        QHash<BitTorrent::TorrentID, Entry> m_entries;
    };
}

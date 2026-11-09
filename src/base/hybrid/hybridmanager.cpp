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

#include "hybridmanager.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QNetworkAccessManager>
#include <QTimer>

#include "base/bittorrent/hybridtorrentsink.h"
#include "base/bittorrent/session.h"
#include "base/bittorrent/torrent.h"
#include "base/global.h"
#include "base/logger.h"
#include "base/profile.h"
#include "base/settingvalue.h"
#include "providerrealdebrid.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    const QString KEY_ENABLED = u"Hybrid/Enabled"_s;
    const QString KEY_DEFAULT_POLICY = u"Hybrid/DefaultPolicy"_s;
    const QString KEY_DEFAULT_CONSENT = u"Hybrid/DefaultP2PConsent"_s;
    const QString KEY_ALLOW_REMOTE_CREATE = u"Hybrid/AllowRemoteCreate"_s;
    const QString KEY_MAX_PER_TORRENT = u"Hybrid/MaxHttpPiecesPerTorrent"_s;
    const QString KEY_MAX_GLOBAL = u"Hybrid/MaxHttpPiecesGlobal"_s;
    const QString KEY_MAX_BUFFER_MIB = u"Hybrid/MaxBufferedMiB"_s;

    template <typename T>
    T setting(const QString &key, const T &def)
    {
        return SettingValue<T>(key).get(def);
    }

    template <typename T>
    void setSetting(const QString &key, const T &value)
    {
        SettingValue<T> settingValue {key};
        settingValue = value;
    }

    qint64 nowMs()
    {
        return QDateTime::currentMSecsSinceEpoch();
    }
}

Hybrid::HybridManager *Hybrid::HybridManager::m_instance = nullptr;

void Hybrid::HybridManager::initInstance()
{
    if (!m_instance)
    {
        m_instance = new HybridManager;
        BitTorrent::Session::setHybridHooks(m_instance);
    }
}

void Hybrid::HybridManager::freeInstance()
{
    BitTorrent::Session::setHybridHooks(nullptr);
    delete m_instance;
    m_instance = nullptr;
}

Hybrid::HybridManager *Hybrid::HybridManager::instance()
{
    return m_instance;
}

Hybrid::HybridManager::HybridManager()
    : m_network {new QNetworkAccessManager(this)}
    , m_store {specialFolderLocation(SpecialFolder::Data) / Path(u"hybrid"_s)}
    , m_budget {8, 64 * 1024 * 1024}
    , m_tick {new QTimer(this)}
{
    m_network->setAutoDeleteReplies(false);
    m_budget.setLimits(maxGlobalPieces(), static_cast<qint64>(maxBufferedMiB()) * 1024 * 1024);

    m_realDebrid = new RealDebridProvider(m_network, this);
    connect(m_realDebrid, &Provider::accountStateChanged, this, [this]
    {
        for (const Entry &entry : asConst(m_entries))
            entry.coordinator->notifyProviderAccountChanged();
        emit providerAccountChanged();
    });
    m_realDebrid->loadStoredCredentials();

    // Piece verdict alerts are only requested when hybrid torrents can exist (I7).
    m_pieceAlerts = isEnabled();
    if (!m_pieceAlerts)
    {
        const QDir dir {m_store.directory().data()};
        for (const QString &name : dir.entryList({u"*.hybrid.json"_s}, QDir::Files))
        {
            const auto data = m_store.load(name.section(u'.', 0, 0));
            if (data && (data->policy != TransportPolicy::TorrentOnly))
            {
                m_pieceAlerts = true;
                break;
            }
        }
    }

    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &HybridManager::onTick);
}

Hybrid::HybridManager::~HybridManager()
{
    m_entries.clear();
}

void Hybrid::HybridManager::attachSession()
{
    auto *session = BitTorrent::Session::instance();
    connect(session, &BitTorrent::Session::torrentAdded, this, [this](BitTorrent::Torrent *torrent)
    {
        const BitTorrent::TorrentID id = torrent->id();
        SidecarData data;
        if (m_pending.contains(id))
        {
            data = m_pending.take(id);
        }
        else if (isEnabled())
        {
            data.policy = defaultPolicy();
            data.p2pFallbackConsent = (data.policy == TransportPolicy::DebridPreferred) && defaultP2PConsent();
        }
        if (data.policy == TransportPolicy::TorrentOnly)
            return;
        data.providerId = RealDebridProvider::ID;
        saveSidecar(id, data);
        ensureEntry(torrent);
    });
    connect(session, &BitTorrent::Session::torrentsLoaded, this, [this](const QList<BitTorrent::Torrent *> &torrents)
    {
        for (BitTorrent::Torrent *torrent : torrents)
        {
            if (sidecarFor(torrent->id()).policy != TransportPolicy::TorrentOnly)
                ensureEntry(torrent);
        }
    });
    connect(session, &BitTorrent::Session::torrentAboutToBeRemoved, this, [this](BitTorrent::Torrent *torrent)
    {
        dropEntry(torrent->id());
        m_store.remove(torrent->id().toString());   // remove sidecar (spec F07)
        m_sidecarCache.remove(torrent->id());
    });
    connect(session, &BitTorrent::Session::torrentFinished, this, [this](BitTorrent::Torrent *torrent)
    {
        if (HybridCoordinator *c = coordinatorFor(torrent))
            c->notifyFinished();
    });
    connect(session, &BitTorrent::Session::torrentStopped, this, [this](BitTorrent::Torrent *torrent)
    {
        if (HybridCoordinator *c = coordinatorFor(torrent))
            c->notifyUserStopped();
    });
    connect(session, &BitTorrent::Session::torrentStarted, this, [this](BitTorrent::Torrent *torrent)
    {
        if (HybridCoordinator *c = coordinatorFor(torrent))
            c->notifyUserStarted();
    });
    connect(session, &BitTorrent::Session::torrentMetadataReceived, this, [this](BitTorrent::Torrent *torrent)
    {
        if (HybridCoordinator *c = coordinatorFor(torrent))
            c->notifyMetadataReady();
    });
    connect(session, &BitTorrent::Session::torrentFinishedChecking, this, [this](BitTorrent::Torrent *torrent)
    {
        if (HybridCoordinator *c = coordinatorFor(torrent))
            c->notifyCheckingFinished();
    });
    m_tick->start();
}

// ----------------------------------------------------------------------------------- settings

bool Hybrid::HybridManager::isEnabled() const
{
    return setting<bool>(KEY_ENABLED, false);
}

void Hybrid::HybridManager::setEnabled(const bool enabled)
{
    setSetting<bool>(KEY_ENABLED, enabled);
    if (enabled && !m_pieceAlerts)
    {
        m_pieceAlerts = true;
        if (auto *session = BitTorrent::Session::instance())
            session->setHybridAlertsEnabled();
    }
    for (const Entry &entry : asConst(m_entries))
        entry.coordinator->setProvider(activeProvider());
    emit settingsChanged();
}

Hybrid::TransportPolicy Hybrid::HybridManager::defaultPolicy() const
{
    return policyFromWireName(setting<QString>(KEY_DEFAULT_POLICY, u"debrid_only"_s), TransportPolicy::DebridOnly);
}

void Hybrid::HybridManager::setDefaultPolicy(const TransportPolicy policy)
{
    setSetting<QString>(KEY_DEFAULT_POLICY, wireName(policy));
    emit settingsChanged();
}

bool Hybrid::HybridManager::defaultP2PConsent() const
{
    return setting<bool>(KEY_DEFAULT_CONSENT, false);
}

void Hybrid::HybridManager::setDefaultP2PConsent(const bool consent)
{
    setSetting<bool>(KEY_DEFAULT_CONSENT, consent);
    emit settingsChanged();
}

bool Hybrid::HybridManager::allowRemoteCreate() const
{
    return setting<bool>(KEY_ALLOW_REMOTE_CREATE, false);
}

void Hybrid::HybridManager::setAllowRemoteCreate(const bool allow)
{
    setSetting<bool>(KEY_ALLOW_REMOTE_CREATE, allow);
    emit settingsChanged();
}

int Hybrid::HybridManager::maxPiecesPerTorrent() const
{
    return std::clamp(setting<int>(KEY_MAX_PER_TORRENT, 4), 1, 32);
}

void Hybrid::HybridManager::setMaxPiecesPerTorrent(const int value)
{
    setSetting<int>(KEY_MAX_PER_TORRENT, std::clamp(value, 1, 32));
    emit settingsChanged();
}

int Hybrid::HybridManager::maxGlobalPieces() const
{
    return std::clamp(setting<int>(KEY_MAX_GLOBAL, 8), 1, 64);
}

void Hybrid::HybridManager::setMaxGlobalPieces(const int value)
{
    setSetting<int>(KEY_MAX_GLOBAL, std::clamp(value, 1, 64));
    m_budget.setLimits(maxGlobalPieces(), static_cast<qint64>(maxBufferedMiB()) * 1024 * 1024);
    emit settingsChanged();
}

int Hybrid::HybridManager::maxBufferedMiB() const
{
    return std::clamp(setting<int>(KEY_MAX_BUFFER_MIB, 64), 8, 1024);
}

void Hybrid::HybridManager::setMaxBufferedMiB(const int value)
{
    setSetting<int>(KEY_MAX_BUFFER_MIB, std::clamp(value, 8, 1024));
    m_budget.setLimits(maxGlobalPieces(), static_cast<qint64>(maxBufferedMiB()) * 1024 * 1024);
    emit settingsChanged();
}

Hybrid::RealDebridProvider *Hybrid::HybridManager::realDebrid() const
{
    return m_realDebrid;
}

Hybrid::Provider *Hybrid::HybridManager::activeProvider() const
{
    return isEnabled() ? m_realDebrid : nullptr;
}

Hybrid::CoordinatorConfig Hybrid::HybridManager::coordinatorConfig() const
{
    CoordinatorConfig config;
    config.maxPiecesInFlight = maxPiecesPerTorrent();
    config.allowRemoteCreate = allowRemoteCreate();
    return config;
}

// ----------------------------------------------------------------------------------- per torrent

Hybrid::SidecarData Hybrid::HybridManager::sidecarFor(const BitTorrent::TorrentID &id) const
{
    const auto it = m_sidecarCache.constFind(id);
    if (it != m_sidecarCache.cend())
        return it.value();
    const SidecarData data = m_store.load(id.toString()).value_or(SidecarData {});
    m_sidecarCache.insert(id, data);
    return data;
}

void Hybrid::HybridManager::saveSidecar(const BitTorrent::TorrentID &id, const SidecarData &data)
{
    m_sidecarCache.insert(id, data);
    if (const auto result = m_store.save(id.toString(), data); !result)
        LogMsg(tr("Hybrid sources: failed to save torrent policy. Reason: %1").arg(result.error()), Log::WARNING);
}

void Hybrid::HybridManager::setPendingPolicy(const BitTorrent::TorrentID &id, const TransportPolicy policy, const bool p2pConsent)
{
    SidecarData data;
    data.policy = policy;
    data.p2pFallbackConsent = (policy == TransportPolicy::DebridPreferred) && p2pConsent;
    m_pending.insert(id, data);
}

Hybrid::TransportPolicy Hybrid::HybridManager::policyFor(const BitTorrent::TorrentID &id) const
{
    return sidecarFor(id).policy;
}

bool Hybrid::HybridManager::p2pConsentFor(const BitTorrent::TorrentID &id) const
{
    return sidecarFor(id).p2pFallbackConsent;
}

void Hybrid::HybridManager::setTorrentPolicy(BitTorrent::Torrent *torrent, const TransportPolicy policy, const bool p2pConsent)
{
    const BitTorrent::TorrentID id = torrent->id();
    SidecarData data = sidecarFor(id);
    data.policy = policy;
    data.p2pFallbackConsent = (policy == TransportPolicy::DebridPreferred) && p2pConsent;
    data.providerId = RealDebridProvider::ID;
    data.lastReason = Reason::UserRequest;
    data.lastTransitionUtc = QDateTime::currentDateTimeUtc();
    saveSidecar(id, data);

    if (policy == TransportPolicy::TorrentOnly)
    {
        if (const auto it = m_entries.constFind(id); it != m_entries.cend())
            it->coordinator->setPolicy(policy, false);   // coordinator releases its hold
        dropEntry(id);
        BitTorrent::HybridTorrentSink(torrent).setTransportHold(false);   // and nothing else holds it
        emit torrentStatusChanged(id);
        return;
    }

    if (!m_pieceAlerts)
    {
        // First hybrid torrent this run: piece verdict alerts must be switched on.
        m_pieceAlerts = true;
        BitTorrent::Session::instance()->setHybridAlertsEnabled();
    }

    if (const auto it = m_entries.constFind(id); it != m_entries.cend())
        it->coordinator->setPolicy(policy, data.p2pFallbackConsent);
    else
        ensureEntry(torrent);
    emit torrentStatusChanged(id);
}

std::optional<Hybrid::HybridStatus> Hybrid::HybridManager::statusFor(const BitTorrent::TorrentID &id) const
{
    const auto it = m_entries.constFind(id);
    if (it == m_entries.cend())
        return std::nullopt;
    return it->coordinator->status();
}

int Hybrid::HybridManager::hybridTorrentCount() const
{
    return static_cast<int>(m_entries.size());
}

void Hybrid::HybridManager::ensureEntry(BitTorrent::Torrent *torrent)
{
    const BitTorrent::TorrentID id = torrent->id();
    if (m_entries.contains(id))
        return;

    const SidecarData data = sidecarFor(id);
    Entry entry;
    entry.sink = std::make_shared<BitTorrent::HybridTorrentSink>(torrent);
    entry.coordinator = std::make_shared<HybridCoordinator>(entry.sink.get(), activeProvider(), m_network, &m_budget, &m_bucket
            , coordinatorConfig(), nowMs);
    HybridCoordinator *coordinator = entry.coordinator.get();
    BitTorrent::HybridTorrentSink *sink = entry.sink.get();

    connect(coordinator, &HybridCoordinator::statusChanged, this, [this, id, coordinator, sink]
    {
        sink->setHttpRate(coordinator->status().httpRateBps);
        emit torrentStatusChanged(id);
    });
    connect(coordinator, &HybridCoordinator::policyPersistRequested, this
            , [this, id](const TransportPolicy policy, const bool consent, const Reason reason, const quint64 switches)
    {
        SidecarData d = sidecarFor(id);
        d.policy = policy;
        d.p2pFallbackConsent = consent;
        d.lastReason = reason;
        d.lastTransitionUtc = QDateTime::currentDateTimeUtc();
        d.switchCount = switches;
        saveSidecar(id, d);
    });
    connect(coordinator, &HybridCoordinator::transitioned, this, [](const TransportState from, const TransportState to, const Reason reason)
    {
        // Structured, low-cardinality event: no names, hashes, URLs or tokens (spec 04).
        LogMsg(tr("Hybrid sources: %1 -> %2 (%3)").arg(wireName(from), wireName(to), wireName(reason)), Log::INFO);
    });

    m_entries.insert(id, std::move(entry));
    coordinator->start(data.policy, data.p2pFallbackConsent);
    if (torrent->isStopped())
        coordinator->notifyUserStopped();
}

void Hybrid::HybridManager::dropEntry(const BitTorrent::TorrentID &id)
{
    m_entries.remove(id);
}

Hybrid::HybridCoordinator *Hybrid::HybridManager::coordinatorFor(const BitTorrent::Torrent *torrent) const
{
    const auto it = m_entries.constFind(torrent->id());
    return (it == m_entries.cend()) ? nullptr : it->coordinator.get();
}

void Hybrid::HybridManager::onTick()
{
    // HTTP shares the user's effective global download limit (spec 02: never bypass limits).
    const int limit = BitTorrent::Session::instance()->downloadSpeedLimit();
    if (m_bucket.rate() != limit)
        m_bucket.setRate(limit);
}

// ----------------------------------------------------------------------------------- hooks

bool Hybrid::HybridManager::shouldHoldOnAdd(const BitTorrent::TorrentID &id) const
{
    if (const auto it = m_pending.constFind(id); it != m_pending.cend())
        return it->policy != TransportPolicy::TorrentOnly;
    return isEnabled() && (defaultPolicy() != TransportPolicy::TorrentOnly);
}

bool Hybrid::HybridManager::shouldHoldOnLoad(const BitTorrent::TorrentID &id) const
{
    // Fails closed: a hybrid sidecar keeps the torrent held even if the feature was disabled.
    return sidecarFor(id).policy != TransportPolicy::TorrentOnly;
}

bool Hybrid::HybridManager::needsPieceAlerts() const
{
    return m_pieceAlerts;
}

void Hybrid::HybridManager::handlePieceFinished(BitTorrent::Torrent *torrent, const int pieceIndex)
{
    if (HybridCoordinator *c = coordinatorFor(torrent))
        c->notifyPieceFinished(pieceIndex);
}

void Hybrid::HybridManager::handlePieceHashFailed(BitTorrent::Torrent *torrent, const int pieceIndex)
{
    if (HybridCoordinator *c = coordinatorFor(torrent))
        c->notifyHashFailed(pieceIndex);
}

void Hybrid::HybridManager::handleTorrentPaused(BitTorrent::Torrent *torrent)
{
    if (HybridCoordinator *c = coordinatorFor(torrent))
        c->notifyHoldSettled();
}

void Hybrid::HybridManager::handleRecheckRequested(BitTorrent::Torrent *torrent)
{
    if (HybridCoordinator *c = coordinatorFor(torrent))
        c->notifyCheckingStarted();
}

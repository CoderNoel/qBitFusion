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

#include "directdownloadmanager.h"

#include <algorithm>
#include <array>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkProxy>
#include <QTimer>
#include <QUuid>

#include "base/bittorrent/session.h"
#include "base/global.h"
#include "base/hybrid/securestore.h"
#include "base/hybrid/urlguard.h"
#include "base/logger.h"
#include "base/net/downloadmanager.h"
#include "base/net/proxyconfigurationmanager.h"
#include "base/preferences.h"
#include "base/profile.h"
#include "base/utils/io.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    constexpr int TICK_MS = 1000;
    constexpr int STATE_VERSION = 1;
    constexpr std::array<int, 6> RETRY_DELAYS_S {5, 15, 30, 60, 120, 300};
    constexpr qint64 MAX_RETRY_AFTER_S = 600;

    void log(const QString &message, const Log::MsgType type = Log::INFO)
    {
        if (Logger::instance())
            LogMsg(message, type);
    }

    QString tr(const char *text)
    {
        return QCoreApplication::translate("DirectDownload", text);
    }

    QString wireName(const DirectDownload::State state)
    {
        switch (state)
        {
        case DirectDownload::State::Queued:
        case DirectDownload::State::Downloading:
            return u"queued"_s;
        case DirectDownload::State::Paused:
            return u"paused"_s;
        case DirectDownload::State::Completed:
            return u"completed"_s;
        case DirectDownload::State::Failed:
            return u"failed"_s;
        case DirectDownload::State::NeedsLink:
            return u"needs_link"_s;
        }
        return u"paused"_s;
    }

    DirectDownload::State stateFromWireName(const QString &name)
    {
        if (name == u"queued")
            return DirectDownload::State::Queued;
        if (name == u"completed")
            return DirectDownload::State::Completed;
        if (name == u"failed")
            return DirectDownload::State::Failed;
        if (name == u"needs_link")
            return DirectDownload::State::NeedsLink;
        return DirectDownload::State::Paused;
    }

    qint64 fileSize(const Path &path)
    {
        const QFileInfo info {path.data()};
        return info.exists() ? info.size() : 0;
    }

    // Cookies from qBittorrent's cookie manager (Options > Advanced > Manage cookies), as for
    // torrent downloads by URL. Cookies that servers set are kept for this session only.
    class AppCookieJar final : public QNetworkCookieJar
    {
    public:
        using QNetworkCookieJar::QNetworkCookieJar;

        QList<QNetworkCookie> cookiesForUrl(const QUrl &url) const override
        {
            QList<QNetworkCookie> cookies = QNetworkCookieJar::cookiesForUrl(url);
            if (const auto *downloadManager = Net::DownloadManager::instance())
                cookies += downloadManager->cookiesForUrl(url);
            return cookies;
        }
    };
}

DirectDownload::Manager *DirectDownload::Manager::m_instance = nullptr;

Path DirectDownload::ItemInfo::filePath() const
{
    return savePath / Path((state == State::Completed) ? name : (name + Manager::partSuffix()));
}

QString DirectDownload::Manager::partSuffix()
{
    return u".part"_s;
}

void DirectDownload::Manager::initInstance()
{
    if (m_instance)
        return;
    ManagerConfig config;
    config.stateFile = specialFolderLocation(SpecialFolder::Data) / Path(u"directdownloads.json"_s);
    m_instance = new Manager(std::move(config));
}

void DirectDownload::Manager::freeInstance()
{
    delete m_instance;
    m_instance = nullptr;
}

DirectDownload::Manager *DirectDownload::Manager::instance()
{
    return m_instance;
}

DirectDownload::Manager::Manager(ManagerConfig config, QObject *parent)
    : QObject(parent)
    , m_config {std::move(config)}
    , m_network {new QNetworkAccessManager(this)}
    , m_tick {new QTimer(this)}
{
    if (!m_config.clock)
        m_config.clock = [] { return QDateTime::currentMSecsSinceEpoch(); };

    m_network->setAutoDeleteReplies(false);
    m_network->setCookieJar(new AppCookieJar(m_network));

    if (m_config.useAppSettings)
    {
        applyAppSettings();
        if (auto *proxyManager = Net::ProxyConfigurationManager::instance())
            connect(proxyManager, &Net::ProxyConfigurationManager::proxyConfigurationChanged, this, &Manager::applyAppSettings);
        if (auto *pref = Preferences::instance())
            connect(pref, &Preferences::changed, this, &Manager::applyAppSettings);
    }

    load();

    m_lastTickMs = now();
    connect(m_tick, &QTimer::timeout, this, &Manager::onTick);
    m_tick->start(TICK_MS);
    QTimer::singleShot(0, this, &Manager::schedule);
}

DirectDownload::Manager::~Manager()
{
    for (Item &item : m_items)
    {
        if (item.transfer)
        {
            item.info.downloaded = item.transfer->bytesOnDisk();
            stopTransfer(item);
        }
    }
    save();
}

qint64 DirectDownload::Manager::now() const
{
    return m_config.clock();
}

void DirectDownload::Manager::applyAppSettings()
{
    const auto *pref = Preferences::instance();
    const auto *proxyManager = Net::ProxyConfigurationManager::instance();
    if (!pref || !proxyManager)
        return;

    // Same rules as Net::DownloadManager::applyProxySettings(): direct downloads are general-purpose traffic.
    QNetworkProxy proxy {QNetworkProxy::NoProxy};
    const Net::ProxyConfiguration config = proxyManager->proxyConfiguration();
    if (pref->useProxyForGeneralPurposes()
            && ((config.type == Net::ProxyType::HTTP) || (config.type == Net::ProxyType::SOCKS5)))
    {
        proxy = QNetworkProxy(((config.type == Net::ProxyType::HTTP) ? QNetworkProxy::HttpProxy : QNetworkProxy::Socks5Proxy)
                , config.ip, config.port
                , (config.authEnabled ? config.username : QString())
                , (config.authEnabled ? config.password : QString()));
        proxy.setCapabilities(config.hostnameLookupEnabled
                ? (proxy.capabilities() | QNetworkProxy::HostNameLookupCapability)
                : (proxy.capabilities() & ~QNetworkProxy::HostNameLookupCapability));
    }
    m_network->setProxy(proxy);
}

DirectDownload::LinkProbe *DirectDownload::Manager::createProbe(const QUrl &link, QObject *parent)
{
    qint64 limit = 100 * 1024 * 1024;
    if (m_config.useAppSettings)
    {
        if (const auto *pref = Preferences::instance())
            limit = pref->getTorrentFileSizeLimit();
    }
    return new LinkProbe(m_network, link, limit, parent);
}

DirectDownload::Manager::Item *DirectDownload::Manager::find(const QString &id)
{
    const auto it = m_items.find(id);
    return (it == m_items.end()) ? nullptr : &it.value();
}

bool DirectDownload::Manager::isNameTaken(const Path &folder, const QString &name, const QString &exceptId) const
{
    if ((folder / Path(name)).exists() || (folder / Path(name + partSuffix())).exists())
        return true;
    return std::ranges::any_of(m_items, [&](const Item &item)
    {
        return (item.info.id != exceptId) && (item.info.savePath == folder)
                && (item.info.name.compare(name, Qt::CaseInsensitive) == 0);
    });
}

QString DirectDownload::Manager::add(const QUrl &link, const ProbeResult &probe, const Path &savePath
        , const QString &name, const bool startPaused)
{
    Item item;
    item.info.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.info.savePath = savePath;
    item.info.name = uniqueFileName(sanitizeFileName(name), [this, &savePath](const QString &candidate)
    {
        return isNameTaken(savePath, candidate, {});
    });
    item.info.host = (probe.finalUrl.isValid() ? probe.finalUrl : link).host().toLower();
    item.info.totalSize = probe.totalSize;
    item.info.resumable = probe.rangeSupported;
    item.info.addedOn = QDateTime::currentDateTime();
    item.info.state = startPaused ? State::Paused : State::Queued;
    item.link = link;
    item.resolvedUrl = probe.finalUrl;
    item.validator = probe.validator;

    const QString secretKey = m_config.secretKeyPrefix + item.info.id;
    item.info.linkSaved = Hybrid::SecureStore::isPersistent() && Hybrid::SecureStore::write(secretKey, link.toEncoded());
    if (!item.info.linkSaved)
    {
        Hybrid::SecureStore::remove(secretKey);
        log(tr("Direct download: the link for \"%1\" could not be kept in the credential store. "
                "After a restart, paste the link again to continue.").arg(item.info.name), Log::WARNING);
    }

    const QString id = item.info.id;
    log(tr("Direct download added: \"%1\" from %2").arg(item.info.name, item.info.host));
    m_items.insert(id, std::move(item));
    m_order.append(id);
    emit itemAdded(id);
    save();
    schedule();
    return id;
}

void DirectDownload::Manager::schedule()
{
    int active = activeCount();
    const qint64 nowMs = now();
    for (const QString &id : asConst(m_order))
    {
        if (active >= m_config.maxActive)
            break;
        Item *item = find(id);
        if (item && (item->info.state == State::Queued) && (item->retryAtMs <= nowMs))
        {
            startItem(*item);
            ++active;
        }
    }
}

int DirectDownload::Manager::activeCount() const
{
    return static_cast<int>(std::ranges::count_if(m_items, [](const Item &item)
    {
        return item.info.state == State::Downloading;
    }));
}

void DirectDownload::Manager::startItem(Item &item)
{
    if (item.link.isEmpty())
    {
        item.info.state = State::NeedsLink;
        emit itemChanged(item.info.id);
        return;
    }

    const QUrl url = item.resolvedUrl.isValid() ? item.resolvedUrl : item.link;
    item.usingResolvedUrl = item.resolvedUrl.isValid();

    if (!QDir().mkpath(item.info.savePath.data()))
    {
        setFailed(item, tr("Cannot create the folder %1").arg(item.info.savePath.toString()));
        return;
    }

    const Path partPath = item.info.filePath();
    qint64 offset = fileSize(partPath);
    if ((item.info.totalSize >= 0) && (offset > item.info.totalSize))
    {
        QFile::remove(partPath.data());
        offset = 0;
    }

    TransferRequest request;
    request.url = url;
    request.filePath = partPath.data();
    request.offset = offset;
    request.expectedTotal = item.info.totalSize;
    request.validator = item.validator;

    const QString id = item.info.id;
    auto *transfer = new Transfer(m_network, std::move(request), &m_bucket, m_config.clock, this);
    connect(transfer, &Transfer::responseStarted, this, [this, id](const TransferHead &head)
    {
        Item *it = find(id);
        if (!it || !it->transfer)
            return;
        if (head.restartedFromZero)
            log(tr("Direct download: the server could not resume \"%1\", downloading it from the start").arg(it->info.name), Log::WARNING);
        if (head.total >= 0)
            it->info.totalSize = head.total;
        if (!head.validator.isEmpty())
            it->validator = head.validator;
        it->info.resumable = head.rangeSupported;
        it->resolvedUrl = it->transfer->currentUrl();
        it->info.downloaded = it->transfer->bytesOnDisk();
        it->bytesAtStart = std::min(it->bytesAtStart, it->info.downloaded);
        it->bytesAtLastTick = std::min(it->bytesAtLastTick, it->info.downloaded);
        markDirty();
        emit itemChanged(id);
    });
    connect(transfer, &Transfer::progressed, this, [this, id]
    {
        if (Item *it = find(id); it && it->transfer)
            it->info.downloaded = it->transfer->bytesOnDisk();
    });
    connect(transfer, &Transfer::succeeded, this, [this, id] { onSucceeded(id); });
    connect(transfer, &Transfer::failed, this, [this, id](const Failure &failure) { onFailed(id, failure); });

    item.transfer = transfer;
    item.info.state = State::Downloading;
    item.info.error.clear();
    item.info.downloaded = offset;
    item.info.speed = 0;
    item.bytesAtStart = offset;
    item.bytesAtLastTick = offset;
    item.retryAtMs = 0;
    emit itemChanged(item.info.id);

    transfer->start();
}

void DirectDownload::Manager::stopTransfer(Item &item)
{
    if (!item.transfer)
        return;
    item.transfer->disconnect(this);
    item.transfer->abort();
    item.transfer->deleteLater();
    item.transfer = nullptr;
    item.info.speed = 0;
}

void DirectDownload::Manager::onSucceeded(const QString &id)
{
    Item *item = find(id);
    if (!item || !item->transfer)
        return;

    item->info.downloaded = item->transfer->bytesOnDisk();
    stopTransfer(*item);
    if (item->info.totalSize < 0)
        item->info.totalSize = item->info.downloaded;

    const Path partPath = item->info.filePath();
    const QString finalName = uniqueFileName(item->info.name, [this, item](const QString &candidate)
    {
        return (item->info.savePath / Path(candidate)).exists()
                || std::ranges::any_of(m_items, [&](const Item &other)
                   {
                       return (other.info.id != item->info.id) && (other.info.savePath == item->info.savePath)
                               && (other.info.name.compare(candidate, Qt::CaseInsensitive) == 0);
                   });
    });
    if (!QFile::rename(partPath.data(), (item->info.savePath / Path(finalName)).data()))
    {
        setFailed(*item, tr("Cannot rename the finished file %1").arg(partPath.toString()));
        return;
    }

    item->info.name = finalName;
    item->info.state = State::Completed;
    item->info.completedOn = QDateTime::currentDateTime();
    item->info.error.clear();
    // The link is not needed any more: forget it.
    Hybrid::SecureStore::remove(m_config.secretKeyPrefix + id);
    item->link.clear();
    item->resolvedUrl.clear();
    item->info.linkSaved = false;

    log(tr("Direct download finished: \"%1\"").arg(item->info.name));
    emit itemChanged(id);
    emit itemFinished(id);
    save();
    schedule();
}

void DirectDownload::Manager::onFailed(const QString &id, const Failure &failure)
{
    Item *item = find(id);
    if (!item || !item->transfer)
        return;

    item->info.downloaded = item->transfer->bytesOnDisk();
    const bool madeProgress = item->info.downloaded > item->bytesAtStart;
    stopTransfer(*item);

    if (failure.restartFromZero)
    {
        if (item->restartedFromZero)
        {
            setFailed(*item, failure.detail);
            return;
        }
        log(tr("Direct download: \"%1\" changed on the server, downloading it from the start. Detail: %2")
                .arg(item->info.name, failure.detail), Log::WARNING);
        item->restartedFromZero = true;
        QFile::remove(item->info.filePath().data());
        item->info.downloaded = 0;
        item->info.totalSize = -1;
        item->validator = {};
        item->info.state = State::Queued;
        item->retryAtMs = 0;
        markDirty();
        emit itemChanged(id);
        schedule();
        return;
    }

    switch (failure.category)
    {
    case Hybrid::FailureCategory::LinkExpired:
    case Hybrid::FailureCategory::Gone:
    case Hybrid::FailureCategory::Permission:
    case Hybrid::FailureCategory::Auth:
        if (item->usingResolvedUrl && item->link.isValid())
        {
            // The address the link redirected to may have expired; the link itself resolves again.
            item->resolvedUrl.clear();
            item->info.state = State::Queued;
            item->retryAtMs = 0;
            emit itemChanged(id);
            schedule();
            return;
        }
        setFailed(*item, tr("%1. The link may have expired: copy a new one and use \"Replace link\".").arg(failure.detail));
        return;

    case Hybrid::FailureCategory::Temporary:
    case Hybrid::FailureCategory::RateLimit:
        {
            if (madeProgress)
                item->attempts = 0;
            ++item->attempts;
            if (item->attempts > static_cast<int>(RETRY_DELAYS_S.size()))
            {
                setFailed(*item, failure.detail);
                return;
            }
            qint64 delay = RETRY_DELAYS_S[item->attempts - 1];
            if (failure.retryAfterSeconds)
                delay = std::clamp<qint64>(*failure.retryAfterSeconds, delay, MAX_RETRY_AFTER_S);
            item->info.state = State::Queued;
            item->info.error = failure.detail;
            item->retryAtMs = now() + (delay * 1000);
            emit itemChanged(id);
            return;
        }

    default:
        setFailed(*item, failure.detail);
        return;
    }
}

void DirectDownload::Manager::setFailed(Item &item, const QString &error)
{
    item.info.state = State::Failed;
    item.info.error = error;
    item.info.speed = 0;
    log(tr("Direct download failed: \"%1\". Reason: %2").arg(item.info.name, error), Log::WARNING);
    emit itemChanged(item.info.id);
    save();
    schedule();
}

void DirectDownload::Manager::pause(const QString &id)
{
    Item *item = find(id);
    if (!item || (item->info.state == State::Completed) || (item->info.state == State::Paused))
        return;
    if (item->transfer)
    {
        item->info.downloaded = item->transfer->bytesOnDisk();
        stopTransfer(*item);
    }
    if (item->info.state != State::NeedsLink)
        item->info.state = State::Paused;
    item->retryAtMs = 0;
    emit itemChanged(id);
    save();
    schedule();
}

void DirectDownload::Manager::resume(const QString &id)
{
    Item *item = find(id);
    if (!item || ((item->info.state != State::Paused) && (item->info.state != State::Failed)))
        return;
    item->info.state = item->link.isValid() ? State::Queued : State::NeedsLink;
    item->info.error.clear();
    item->attempts = 0;
    item->retryAtMs = 0;
    item->restartedFromZero = false;
    emit itemChanged(id);
    save();
    schedule();
}

void DirectDownload::Manager::remove(const QString &id, const bool deleteFiles)
{
    Item *item = find(id);
    if (!item)
        return;
    stopTransfer(*item);
    Hybrid::SecureStore::remove(m_config.secretKeyPrefix + id);
    if (deleteFiles)
        QFile::remove(item->info.filePath().data());
    log(tr("Direct download removed: \"%1\"").arg(item->info.name));

    m_items.remove(id);
    m_order.removeAll(id);
    emit itemRemoved(id);
    save();
    schedule();
}

bool DirectDownload::Manager::replaceLink(const QString &id, const QUrl &link)
{
    Item *item = find(id);
    if (!item || (item->info.state == State::Completed) || (checkLink(link) != LinkCheck::Allowed))
        return false;

    stopTransfer(*item);
    item->link = link;
    item->resolvedUrl.clear();
    const QString secretKey = m_config.secretKeyPrefix + id;
    item->info.linkSaved = Hybrid::SecureStore::isPersistent() && Hybrid::SecureStore::write(secretKey, link.toEncoded());
    if (!item->info.linkSaved)
        Hybrid::SecureStore::remove(secretKey);
    item->info.host = link.host().toLower();
    item->info.state = State::Queued;
    item->info.error.clear();
    item->attempts = 0;
    item->retryAtMs = 0;
    item->restartedFromZero = false;
    log(tr("Direct download: new link for \"%1\" from %2").arg(item->info.name, item->info.host));
    emit itemChanged(id);
    save();
    schedule();
    return true;
}

QStringList DirectDownload::Manager::ids() const
{
    return m_order;
}

std::optional<DirectDownload::ItemInfo> DirectDownload::Manager::info(const QString &id) const
{
    const auto it = m_items.constFind(id);
    if (it == m_items.cend())
        return std::nullopt;
    ItemInfo info = it->info;
    if ((info.state == State::Queued) && (it->retryAtMs > now()))
        info.retryInSeconds = ((it->retryAtMs - now()) + 999) / 1000;
    return info;
}

qint64 DirectDownload::Manager::totalSpeed() const
{
    qint64 total = 0;
    for (const Item &item : m_items)
        total += item.info.speed;
    return total;
}

void DirectDownload::Manager::setMaxActive(const int value)
{
    m_config.maxActive = std::max(1, value);
    schedule();
}

void DirectDownload::Manager::setSpeedLimit(const qint64 bytesPerSecond)
{
    m_bucket.setRate(bytesPerSecond);
}

void DirectDownload::Manager::onTick()
{
    const qint64 nowMs = now();
    const qint64 elapsed = std::max<qint64>(1, nowMs - m_lastTickMs);
    m_lastTickMs = nowMs;

    for (auto it = m_items.begin(); it != m_items.end(); ++it)
    {
        Item &item = it.value();
        if (item.info.state == State::Downloading)
        {
            const qint64 delta = std::max<qint64>(0, item.info.downloaded - item.bytesAtLastTick);
            const qint64 instant = (delta * 1000) / elapsed;
            item.info.speed = (item.info.speed == 0) ? instant : ((item.info.speed + instant) / 2);
            item.bytesAtLastTick = item.info.downloaded;
            emit itemChanged(item.info.id);
        }
        else if ((item.info.state == State::Queued) && (item.retryAtMs > 0))
        {
            emit itemChanged(item.info.id);  // the retry countdown
        }
    }

    if (m_config.useAppSettings)
    {
        // Direct downloads follow the user's global download limit, like the hybrid HTTP path.
        if (const auto *session = BitTorrent::Session::instance())
        {
            const qint64 limit = session->downloadSpeedLimit();
            if (m_bucket.rate() != limit)
                m_bucket.setRate(limit);
        }
    }

    schedule();
    if (m_dirty)
        save();
}

void DirectDownload::Manager::markDirty()
{
    m_dirty = true;
}

void DirectDownload::Manager::load()
{
    QFile file {m_config.stateFile.data()};
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if ((error.error != QJsonParseError::NoError) || !doc.isObject())
    {
        log(tr("Direct downloads: cannot read the saved list. Reason: %1").arg(error.errorString()), Log::WARNING);
        return;
    }

    for (const QJsonValue &value : doc.object().value(u"items").toArray())
    {
        const QJsonObject obj = value.toObject();
        Item item;
        item.info.id = obj.value(u"id").toString();
        item.info.name = sanitizeFileName(obj.value(u"name").toString());
        item.info.savePath = Path(obj.value(u"savePath").toString());
        if (item.info.id.isEmpty() || !item.info.savePath.isAbsolute() || m_items.contains(item.info.id))
            continue;

        item.info.host = obj.value(u"host").toString();
        item.info.totalSize = obj.value(u"totalSize").toInteger(-1);
        item.info.resumable = obj.value(u"resumable").toBool();
        item.info.state = stateFromWireName(obj.value(u"state").toString());
        item.info.error = obj.value(u"error").toString();
        item.info.addedOn = QDateTime::fromString(obj.value(u"addedOn").toString(), Qt::ISODate);
        item.info.completedOn = QDateTime::fromString(obj.value(u"completedOn").toString(), Qt::ISODate);
        item.validator.etag = obj.value(u"etag").toString().toLatin1();
        item.validator.lastModified = obj.value(u"lastModified").toString().toLatin1();

        if (item.info.state == State::Completed)
        {
            item.info.downloaded = item.info.totalSize;
        }
        else
        {
            item.info.downloaded = fileSize(item.info.filePath());
            if (const auto secret = Hybrid::SecureStore::read(m_config.secretKeyPrefix + item.info.id))
            {
                item.link = QUrl::fromEncoded(*secret);
                item.info.linkSaved = item.link.isValid();
            }
            if (!item.link.isValid())
            {
                item.link.clear();
                item.info.state = State::NeedsLink;
            }
        }

        const QString id = item.info.id;
        m_items.insert(id, std::move(item));
        m_order.append(id);
    }
}

void DirectDownload::Manager::save()
{
    m_dirty = false;
    if (m_config.stateFile.isEmpty())
        return;

    QJsonArray items;
    for (const QString &id : asConst(m_order))
    {
        const Item &item = m_items[id];
        // No link here: links are secrets and live in the credential store only.
        items.append(QJsonObject {
            {u"id"_s, item.info.id},
            {u"name"_s, item.info.name},
            {u"savePath"_s, item.info.savePath.data()},
            {u"host"_s, item.info.host},
            {u"totalSize"_s, item.info.totalSize},
            {u"resumable"_s, item.info.resumable},
            {u"state"_s, wireName(item.info.state)},
            {u"error"_s, item.info.error},
            {u"addedOn"_s, item.info.addedOn.toString(Qt::ISODate)},
            {u"completedOn"_s, item.info.completedOn.toString(Qt::ISODate)},
            {u"etag"_s, QString::fromLatin1(item.validator.etag)},
            {u"lastModified"_s, QString::fromLatin1(item.validator.lastModified)}
        });
    }

    const QJsonObject root {{u"version"_s, STATE_VERSION}, {u"items"_s, items}};
    QDir().mkpath(m_config.stateFile.parentPath().data());
    const auto result = Utils::IO::saveToFile(m_config.stateFile, QJsonDocument(root).toJson());
    if (!result)
        log(tr("Direct downloads: cannot save the list. Reason: %1").arg(result.error()), Log::WARNING);
}

QString DirectDownload::displayName(const State state)
{
    switch (state)
    {
    case State::Queued:
        return tr("Queued");
    case State::Downloading:
        return tr("Downloading");
    case State::Paused:
        return tr("Paused");
    case State::Completed:
        return tr("Completed");
    case State::Failed:
        return tr("Failed");
    case State::NeedsLink:
        return tr("Link needed");
    }
    return {};
}

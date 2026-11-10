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

#include "providerrealdebrid.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimeZone>
#include <QTimer>

#include "base/version.h"
#include "hybridsidecar.h"
#include "rangevalidation.h"
#include "securestore.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    // Public open-source client id documented by Real-Debrid (not a secret).
    const QString OPEN_SOURCE_CLIENT_ID = u"X245A4XAIBGVM"_s;
    const QString DEVICE_GRANT = u"http://oauth.net/grant_type/device/1.0"_s;
    const QString CREDENTIAL_KEY = u"realdebrid"_s;
    const QString API_HOST = u"api.real-debrid.com"_s;

    constexpr int MIN_CALL_INTERVAL_MS = 300;          // <= 200/min, under the documented 250/min
    constexpr qint64 MAX_API_RESPONSE = 8 * 1024 * 1024;
    constexpr int API_TIMEOUT_MS = 20'000;

    QString tr(const char *text)
    {
        return QCoreApplication::translate("Hybrid::RealDebridProvider", text);
    }

    Hybrid::Availability availabilityFor(const QString &status)
    {
        if (status == u"downloaded")
            return Hybrid::Availability::Cached;
        if ((status == u"magnet_error") || (status == u"error") || (status == u"virus") || (status == u"dead"))
            return Hybrid::Availability::Removed;
        if (status.isEmpty())
            return Hybrid::Availability::Unknown;
        return Hybrid::Availability::RemoteFetching;  // queued, downloading, magnet_conversion, compressing, uploading, waiting_files_selection
    }

    QString basename(const QString &path)
    {
        return path.section(u'/', -1);
    }
}

Hybrid::RealDebridProvider::RealDebridProvider(QNetworkAccessManager *network, QObject *parent)
    : Provider(parent)
    , m_network {network}
    , m_apiBase {u"https://api.real-debrid.com/rest/1.0/"_s}
    , m_oauthBase {u"https://api.real-debrid.com/oauth/v2/"_s}
    , m_rateTimer {new QTimer(this)}
    , m_deviceTimer {new QTimer(this)}
{
    m_rateTimer->setSingleShot(true);
    connect(m_rateTimer, &QTimer::timeout, this, &RealDebridProvider::pump);
    m_deviceTimer->setSingleShot(true);
    connect(m_deviceTimer, &QTimer::timeout, this, &RealDebridProvider::pollDeviceCredentials);
}

Hybrid::RealDebridProvider::~RealDebridProvider() = default;

QString Hybrid::RealDebridProvider::id() const
{
    return ID;
}

QString Hybrid::RealDebridProvider::displayName() const
{
    return u"Real-Debrid"_s;
}

Hybrid::ProviderCapabilities Hybrid::RealDebridProvider::capabilities() const
{
    return {
        .canCheckCachedWithoutMutation = false,  // only via torrents already in the account
        .canSupplyTorrentMetadata = false,
        .canListFiles = true,
        .providesPerFileDownloadUrl = true,
        .supportsLinkRefresh = true,
        .canQueueRemoteTorrent = true
    };
}

Hybrid::UrlPolicy Hybrid::RealDebridProvider::dataUrlPolicy() const
{
    // Download hosts observed for Real-Debrid are subdomains of real-debrid.com. Anything else is
    // refused (fail closed) until verified with a live fixture.
    return {.allowedHostSuffixes = m_testMode ? QStringList {} : QStringList {u"real-debrid.com"_s}
            , .allowLoopbackForTesting = m_testMode};
}

Hybrid::AccountState Hybrid::RealDebridProvider::accountState() const
{
    return m_state;
}

QString Hybrid::RealDebridProvider::accountName() const
{
    return m_accountName;
}

int Hybrid::RealDebridProvider::premiumDaysLeft() const
{
    return m_premiumDays;
}

bool Hybrid::RealDebridProvider::isUsingPrivateToken() const
{
    return m_privateToken;
}

void Hybrid::RealDebridProvider::setEndpointsForTesting(const QUrl &apiBase, const QUrl &oauthBase)
{
    m_apiBase = apiBase;
    m_oauthBase = oauthBase;
    m_testMode = true;
}

void Hybrid::RealDebridProvider::setAccountState(const AccountState state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit accountStateChanged();
}

// ----------------------------------------------------------------------------------- credentials

void Hybrid::RealDebridProvider::loadStoredCredentials()
{
    const std::optional<QByteArray> blob = SecureStore::read(CREDENTIAL_KEY);
    if (!blob)
    {
        setAccountState(AccountState::NotConfigured);
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(*blob).object();
    m_privateToken = obj.value(u"private"_s).toBool();
    m_clientId = obj.value(u"client_id"_s).toString();
    m_clientSecret = obj.value(u"client_secret"_s).toString();
    m_refreshToken = obj.value(u"refresh_token"_s).toString();
    m_accessToken = obj.value(u"access_token"_s).toString();
    m_accessExpiry = QDateTime::fromSecsSinceEpoch(obj.value(u"expires_at"_s).toInteger(0), QTimeZone::UTC);
    if (m_accessToken.isEmpty() && m_refreshToken.isEmpty())
    {
        setAccountState(AccountState::NotConfigured);
        return;
    }
    setAccountState(AccountState::Connected);
    refreshAccountInfo();
}

void Hybrid::RealDebridProvider::persistCredentials()
{
    QJsonObject obj;
    obj[u"private"_s] = m_privateToken;
    obj[u"client_id"_s] = m_clientId;
    obj[u"client_secret"_s] = m_clientSecret;
    obj[u"refresh_token"_s] = m_refreshToken;
    obj[u"access_token"_s] = m_accessToken;
    obj[u"expires_at"_s] = m_accessExpiry.isValid() ? m_accessExpiry.toSecsSinceEpoch() : 0;
    SecureStore::write(CREDENTIAL_KEY, QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void Hybrid::RealDebridProvider::usePrivateToken(const QString &token)
{
    cancelDeviceAuthorization();
    m_privateToken = true;
    m_clientId.clear();
    m_clientSecret.clear();
    m_refreshToken.clear();
    m_accessToken = token.trimmed();
    m_accessExpiry = {};
    persistCredentials();
    setAccountState(AccountState::Connected);
    refreshAccountInfo();
}

void Hybrid::RealDebridProvider::signOut()
{
    cancelDeviceAuthorization();
    SecureStore::remove(CREDENTIAL_KEY);
    m_privateToken = false;
    m_clientId.clear();
    m_clientSecret.clear();
    m_refreshToken.clear();
    m_accessToken.clear();
    m_accessExpiry = {};
    m_accountName.clear();
    m_premiumDays = 0;
    // Drop queued authenticated calls: they must not run with stale state.
    while (!m_queue.isEmpty())
    {
        PendingCall c = m_queue.dequeue();
        if (c.context && c.callback)
            c.callback(nonstd::make_unexpected(ProviderError {FailureCategory::Auth, tr("Signed out"), std::nullopt}));
    }
    setAccountState(AccountState::NotConfigured);
}

void Hybrid::RealDebridProvider::refreshAccountInfo()
{
    call("GET", u"user"_s, {}, {}, this, [this](const JsonResult &result)
    {
        if (!result)
        {
            if (result.error().category == FailureCategory::Auth)
                setAccountState(AccountState::AuthenticationRequired);
            emit accountStateChanged();
            return;
        }
        const QJsonObject user = result.value().object();
        m_accountName = user.value(u"username"_s).toString();
        m_premiumDays = static_cast<int>(user.value(u"premium"_s).toInteger(0) / 86400);
        setAccountState(AccountState::Connected);
        emit accountStateChanged();
    });
}

// ----------------------------------------------------------------------------------- device flow

void Hybrid::RealDebridProvider::startDeviceAuthorization()
{
    cancelDeviceAuthorization();
    QUrlQuery query;
    query.addQueryItem(u"client_id"_s, OPEN_SOURCE_CLIENT_ID);
    query.addQueryItem(u"new_credentials"_s, u"yes"_s);
    QUrl url = m_oauthBase.resolved(QUrl(u"device/code"_s));
    url.setQuery(query);
    enqueue({"GET", url, {}, false, this, [this](const JsonResult &result)
    {
        if (!result)
        {
            emit authorizationFinished(false, result.error().message);
            return;
        }
        const QJsonObject obj = result.value().object();
        m_deviceCode = obj.value(u"device_code"_s).toString();
        m_deviceInterval = std::max(1, obj.value(u"interval"_s).toInt(5));
        m_deviceDeadline = QDateTime::currentDateTimeUtc().addSecs(obj.value(u"expires_in"_s).toInt(1800));
        const QUrl verification {obj.value(u"verification_url"_s).toString()};
        if (m_deviceCode.isEmpty() || (verification.scheme() != u"https" && !m_testMode))
        {
            emit authorizationFinished(false, tr("Unexpected response from Real-Debrid"));
            return;
        }
        emit deviceCodeReady(obj.value(u"user_code"_s).toString(), verification, obj.value(u"expires_in"_s).toInt(1800));
        m_deviceTimer->start(m_deviceInterval * 1000);
    }});
}

void Hybrid::RealDebridProvider::cancelDeviceAuthorization()
{
    m_deviceTimer->stop();
    m_deviceCode.clear();
}

void Hybrid::RealDebridProvider::pollDeviceCredentials()
{
    if (m_deviceCode.isEmpty())
        return;
    if (QDateTime::currentDateTimeUtc() > m_deviceDeadline)
    {
        cancelDeviceAuthorization();
        emit authorizationFinished(false, tr("The authorization code expired. Please try again."));
        return;
    }

    QUrlQuery query;
    query.addQueryItem(u"client_id"_s, OPEN_SOURCE_CLIENT_ID);
    query.addQueryItem(u"code"_s, m_deviceCode);
    QUrl url = m_oauthBase.resolved(QUrl(u"device/credentials"_s));
    url.setQuery(query);
    enqueue({"GET", url, {}, false, this, [this](const JsonResult &result)
    {
        if (m_deviceCode.isEmpty())
            return;
        const QJsonObject obj = result ? result.value().object() : QJsonObject {};
        const QString clientId = obj.value(u"client_id"_s).toString();
        const QString clientSecret = obj.value(u"client_secret"_s).toString();
        if (clientId.isEmpty() || clientSecret.isEmpty())
        {
            m_deviceTimer->start(m_deviceInterval * 1000); // not authorized yet
            return;
        }

        QUrlQuery form;
        form.addQueryItem(u"client_id"_s, clientId);
        form.addQueryItem(u"client_secret"_s, clientSecret);
        form.addQueryItem(u"code"_s, m_deviceCode);
        form.addQueryItem(u"grant_type"_s, DEVICE_GRANT);
        enqueue({"POST", m_oauthBase.resolved(QUrl(u"token"_s)), form, false, this, [this, clientId, clientSecret](const JsonResult &token)
        {
            cancelDeviceAuthorization();
            const QJsonObject t = token ? token.value().object() : QJsonObject {};
            if (t.value(u"access_token"_s).toString().isEmpty())
            {
                emit authorizationFinished(false, token ? tr("Unexpected response from Real-Debrid") : token.error().message);
                return;
            }
            m_privateToken = false;
            m_clientId = clientId;
            m_clientSecret = clientSecret;
            m_accessToken = t.value(u"access_token"_s).toString();
            m_refreshToken = t.value(u"refresh_token"_s).toString();
            m_accessExpiry = QDateTime::currentDateTimeUtc().addSecs(std::max(60, t.value(u"expires_in"_s).toInt(3600)) - 60);
            persistCredentials();
            setAccountState(AccountState::Connected);
            refreshAccountInfo();
            emit authorizationFinished(true, {});
        }});
    }});
}

void Hybrid::RealDebridProvider::refreshAccessToken(std::function<void (bool)> done)
{
    if (m_privateToken || m_refreshToken.isEmpty())
    {
        done(false);
        return;
    }
    m_refreshWaiters.append(std::move(done));
    if (m_refreshing)
        return;
    m_refreshing = true;

    QUrlQuery form;
    form.addQueryItem(u"client_id"_s, m_clientId);
    form.addQueryItem(u"client_secret"_s, m_clientSecret);
    form.addQueryItem(u"code"_s, m_refreshToken);
    form.addQueryItem(u"grant_type"_s, DEVICE_GRANT);
    send({"POST", m_oauthBase.resolved(QUrl(u"token"_s)), form, false, this, [this](const JsonResult &result)
    {
        m_refreshing = false;
        const QJsonObject t = result ? result.value().object() : QJsonObject {};
        const bool ok = !t.value(u"access_token"_s).toString().isEmpty();
        if (ok)
        {
            m_accessToken = t.value(u"access_token"_s).toString();
            if (!t.value(u"refresh_token"_s).toString().isEmpty())
                m_refreshToken = t.value(u"refresh_token"_s).toString();
            m_accessExpiry = QDateTime::currentDateTimeUtc().addSecs(std::max(60, t.value(u"expires_in"_s).toInt(3600)) - 60);
            persistCredentials();
        }
        else
        {
            setAccountState(AccountState::AuthenticationRequired);
        }
        const auto waiters = std::exchange(m_refreshWaiters, {});
        for (const auto &w : waiters)
            w(ok);
    }});
}

// ----------------------------------------------------------------------------------- HTTP plumbing

void Hybrid::RealDebridProvider::call(const QByteArray &verb, const QString &path, const QUrlQuery &query, const QUrlQuery &form
        , QObject *context, JsonCallback callback)
{
    QUrl url = m_apiBase.resolved(QUrl(path));
    if (!query.isEmpty())
        url.setQuery(query);
    enqueue({verb, url, form, true, context, std::move(callback)});
}

void Hybrid::RealDebridProvider::enqueue(PendingCall call)
{
    m_queue.enqueue(std::move(call));
    if (!m_rateTimer->isActive())
        pump();
}

void Hybrid::RealDebridProvider::pump()
{
    if (m_queue.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 wait = (m_lastCallMs + MIN_CALL_INTERVAL_MS) - now;
    if (wait > 0)
    {
        m_rateTimer->start(static_cast<int>(wait));
        return;
    }
    m_lastCallMs = now;
    PendingCall next = m_queue.dequeue();
    if (next.context)
        send(std::move(next));
    if (!m_queue.isEmpty())
        m_rateTimer->start(MIN_CALL_INTERVAL_MS);
}

void Hybrid::RealDebridProvider::send(PendingCall pending)
{
    const auto deliver = [](const PendingCall &c, const JsonResult &r)
    {
        if (c.context && c.callback)
        {
            QMetaObject::invokeMethod(c.context, [cb = c.callback, r] { cb(r); }, Qt::QueuedConnection);
        }
    };

    if (checkUrl(pending.url, {.allowLoopbackForTesting = m_testMode}) != UrlCheck::Allowed)
    {
        deliver(pending, nonstd::make_unexpected(ProviderError {FailureCategory::BadResponse, tr("Refused provider endpoint"), std::nullopt}));
        return;
    }

    if (pending.authenticated)
    {
        if (m_accessToken.isEmpty())
        {
            deliver(pending, nonstd::make_unexpected(ProviderError {FailureCategory::Auth, tr("Not signed in to Real-Debrid"), std::nullopt}));
            return;
        }
        if (!m_privateToken && m_accessExpiry.isValid() && (QDateTime::currentDateTimeUtc() >= m_accessExpiry) && (pending.attempt == 0))
        {
            ++pending.attempt;
            refreshAccessToken([this, pending](const bool ok) mutable
            {
                if (ok)
                    send(std::move(pending));
                else if (pending.context && pending.callback)
                    pending.callback(nonstd::make_unexpected(ProviderError {FailureCategory::Auth, tr("Real-Debrid session expired. Please sign in again."), std::nullopt}));
            });
            return;
        }
    }

    QNetworkRequest request {pending.url};
    request.setRawHeader("User-Agent", "qBitFusion/" QBT_VERSION);
    request.setRawHeader("Accept", "application/json");
    // Never follow redirects with credentials attached (spec 07 section B).
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(API_TIMEOUT_MS);
    if (pending.authenticated && (m_testMode || mayCarryCredentials(pending.url, {API_HOST})))
        request.setRawHeader("Authorization", "Bearer " + m_accessToken.toLatin1());

    QNetworkReply *reply = nullptr;
    const QByteArray body = pending.form.toString(QUrl::FullyEncoded).toLatin1();
    if (pending.verb == "GET")
    {
        reply = m_network->get(request);
    }
    else
    {
        request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);
        reply = m_network->sendCustomRequest(request, pending.verb, body);
    }

    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](const qint64 received, const qint64)
    {
        if (received > MAX_API_RESPONSE)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, pending, deliver]() mutable
    {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();

        if ((status == 401) && pending.authenticated && !m_privateToken && (pending.attempt == 0))
        {
            ++pending.attempt;
            refreshAccessToken([this, pending](const bool ok) mutable
            {
                if (ok)
                    send(std::move(pending));
                else if (pending.context && pending.callback)
                    pending.callback(nonstd::make_unexpected(ProviderError {FailureCategory::Auth, tr("Real-Debrid session expired. Please sign in again."), std::nullopt}));
            });
            return;
        }

        if ((status >= 200) && (status < 300))
        {
            if (data.trimmed().isEmpty())
            {
                deliver(pending, QJsonDocument());
                return;
            }
            QJsonParseError error;
            const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
            if (error.error != QJsonParseError::NoError)
            {
                deliver(pending, nonstd::make_unexpected(ProviderError {FailureCategory::BadResponse, tr("Malformed response from Real-Debrid"), std::nullopt}));
                return;
            }
            deliver(pending, doc);
            return;
        }

        ProviderError error = errorFrom(status, data);
        error.retryAfterSeconds = parseRetryAfterSeconds(reply->rawHeader("Retry-After"));
        if ((error.category == FailureCategory::Auth) && pending.authenticated)
            setAccountState(AccountState::AuthenticationRequired);
        deliver(pending, nonstd::make_unexpected(error));
    });
}

Hybrid::ProviderError Hybrid::RealDebridProvider::errorFrom(const int httpStatus, const QByteArray &body)
{
    const QJsonObject obj = QJsonDocument::fromJson(body).object();
    const int code = obj.value(u"error_code"_s).toInt(-1000);

    ProviderError error;
    if ((httpStatus == 401) || (code == 8))
    {
        error.category = FailureCategory::Auth;
        error.message = tr("Real-Debrid authorization required");
    }
    else if ((code == 35) || (code == 9) || (code == 14) || (code == 15) || (code == 20) || (code == 22) || (code == 23)
            || (code == 36) || (code == 28) || (httpStatus == 403) || (httpStatus == 451))
    {
        // Includes "Infringing file" (35): final, never bypassed.
        error.category = FailureCategory::Permission;
        error.message = tr("Real-Debrid refused access to this content");
    }
    else if ((httpStatus == 429) || (code == 34) || (code == 5) || (code == 21))
    {
        error.category = FailureCategory::RateLimit;
        error.message = tr("Real-Debrid rate limit reached");
    }
    else if ((httpStatus == 404) || (code == 7))
    {
        error.category = FailureCategory::Gone;
        error.message = tr("Not found on Real-Debrid");
    }
    else if ((httpStatus >= 500) || (httpStatus == 0) || (code == 25) || (code == 17) || (code == 19) || (code == -1))
    {
        error.category = FailureCategory::Temporary;
        error.message = tr("Real-Debrid is temporarily unavailable");
    }
    else
    {
        error.category = FailureCategory::BadResponse;
        error.message = tr("Unexpected response from Real-Debrid") + u" (HTTP %1, code %2)"_s.arg(httpStatus).arg(code);
    }
    return error;
}

// ----------------------------------------------------------------------------------- torrent API

void Hybrid::RealDebridProvider::fetchInfo(const QString &resourceId, QObject *context, std::function<void (const JsonResult &)> callback)
{
    if (!isSafeIdentifier(resourceId))
    {
        callback(nonstd::make_unexpected(ProviderError {FailureCategory::BadResponse, tr("Invalid remote id"), std::nullopt}));
        return;
    }
    call("GET", u"torrents/info/"_s + resourceId, {}, {}, context, std::move(callback));
}

void Hybrid::RealDebridProvider::findByHash(const QString &v1Hash, QObject *context, std::function<void (const ProviderResult<QString> &)> callback)
{
    QUrlQuery query;
    query.addQueryItem(u"limit"_s, u"2500"_s);
    call("GET", u"torrents"_s, query, {}, context, [v1Hash, callback](const JsonResult &result)
    {
        if (!result)
        {
            callback(nonstd::make_unexpected(result.error()));
            return;
        }
        for (const QJsonValue &item : result.value().array())
        {
            const QJsonObject t = item.toObject();
            if (t.value(u"hash"_s).toString().compare(v1Hash, Qt::CaseInsensitive) == 0)
            {
                callback(t.value(u"id"_s).toString());
                return;
            }
        }
        callback(QString());
    });
}

Hybrid::ProviderManifest Hybrid::RealDebridProvider::manifestFrom(const QJsonDocument &info)
{
    const QJsonObject obj = info.object();
    ProviderManifest manifest;
    manifest.resourceId = obj.value(u"id"_s).toString();
    manifest.availability = availabilityFor(obj.value(u"status"_s).toString());
    for (const QJsonValue &value : obj.value(u"files"_s).toArray())
    {
        const QJsonObject f = value.toObject();
        if (f.value(u"selected"_s).toInt() != 1)
            continue;
        manifest.files.append({QString::number(f.value(u"id"_s).toInteger(-1)), f.value(u"path"_s).toString(), f.value(u"bytes"_s).toInteger(-1)});
    }
    return manifest;
}

void Hybrid::RealDebridProvider::checkAvailability(const TorrentIdentity &identity, QObject *context, ProviderCallback<Availability> callback)
{
    resolveManifest(identity, false, context, [callback](const ProviderResult<ProviderManifest> &result)
    {
        if (!result)
            callback(nonstd::make_unexpected(result.error()));
        else
            callback(result.value().availability);
    });
}

void Hybrid::RealDebridProvider::resolveManifest(const TorrentIdentity &identity, const bool allowRemoteCreate, QObject *context
        , ProviderCallback<ProviderManifest> callback)
{
    if (identity.infoHashV1.size() != 40)
    {
        callback(nonstd::make_unexpected(ProviderError {FailureCategory::Permission, tr("Real-Debrid requires a v1 (SHA-1) torrent"), std::nullopt}));
        return;
    }

    const QPointer<QObject> guard {context};
    const auto loadInfo = [this, guard, callback, allowRemoteCreate](const QString &id)
    {
        fetchInfo(id, guard, [this, guard, id, callback, allowRemoteCreate](const JsonResult &info)
        {
            if (!info)
            {
                callback(nonstd::make_unexpected(info.error()));
                return;
            }
            const QString status = info.value().object().value(u"status"_s).toString();
            if ((status == u"waiting_files_selection") && allowRemoteCreate)
            {
                QUrlQuery form;
                form.addQueryItem(u"files"_s, u"all"_s);
                call("POST", u"torrents/selectFiles/"_s + id, {}, form, guard, [this, guard, id, callback](const JsonResult &selected)
                {
                    if (!selected)
                    {
                        callback(nonstd::make_unexpected(selected.error()));
                        return;
                    }
                    fetchInfo(id, guard, [callback](const JsonResult &again)
                    {
                        if (!again)
                            callback(nonstd::make_unexpected(again.error()));
                        else
                            callback(manifestFrom(again.value()));
                    });
                });
                return;
            }
            callback(manifestFrom(info.value()));
        });
    };

    findByHash(identity.infoHashV1, context, [this, guard, identity, allowRemoteCreate, callback, loadInfo](const ProviderResult<QString> &found)
    {
        if (!found)
        {
            callback(nonstd::make_unexpected(found.error()));
            return;
        }
        if (!found.value().isEmpty())
        {
            loadInfo(found.value());
            return;
        }
        if (!allowRemoteCreate)
        {
            // Non-mutating mode: nothing in the account for this hash.
            callback(ProviderManifest {{}, Availability::Uncached, {}});
            return;
        }

        // Mutating, explicitly allowed: add by info-hash only (no trackers, no name sent).
        QUrlQuery form;
        form.addQueryItem(u"magnet"_s, u"magnet:?xt=urn:btih:"_s + identity.infoHashV1.toLower());
        call("POST", u"torrents/addMagnet"_s, {}, form, guard, [callback, loadInfo](const JsonResult &added)
        {
            if (!added)
            {
                callback(nonstd::make_unexpected(added.error()));
                return;
            }
            const QString id = added.value().object().value(u"id"_s).toString();
            if (id.isEmpty())
            {
                callback(nonstd::make_unexpected(ProviderError {FailureCategory::BadResponse, tr("Unexpected response from Real-Debrid"), std::nullopt}));
                return;
            }
            loadInfo(id);
        });
    });
}

void Hybrid::RealDebridProvider::resolveFileLink(const QString &resourceId, const QString &providerFileId, QObject *context
        , ProviderCallback<TemporaryLink> callback)
{
    const QPointer<QObject> guard {context};
    fetchInfo(resourceId, context, [this, guard, providerFileId, callback](const JsonResult &info)
    {
        if (!info)
        {
            callback(nonstd::make_unexpected(info.error()));
            return;
        }
        const QJsonObject obj = info.value().object();
        if (availabilityFor(obj.value(u"status"_s).toString()) != Availability::Cached)
        {
            callback(nonstd::make_unexpected(ProviderError {FailureCategory::Gone, tr("Content is no longer available on Real-Debrid"), std::nullopt}));
            return;
        }

        struct Selected { qint64 id; QString path; qint64 bytes; };
        QList<Selected> selected;
        for (const QJsonValue &v : obj.value(u"files"_s).toArray())
        {
            const QJsonObject f = v.toObject();
            if (f.value(u"selected"_s).toInt() == 1)
                selected.append({f.value(u"id"_s).toInteger(-1), f.value(u"path"_s).toString(), f.value(u"bytes"_s).toInteger(-1)});
        }
        std::sort(selected.begin(), selected.end(), [](const Selected &a, const Selected &b) { return a.id < b.id; });
        const QJsonArray links = obj.value(u"links"_s).toArray();
        const auto it = std::find_if(selected.cbegin(), selected.cend(), [&providerFileId](const Selected &s) { return QString::number(s.id) == providerFileId; });
        if ((it == selected.cend()) || (links.size() != selected.size()))
        {
            // Mapping between links and files is undocumented: refuse rather than guess.
            callback(nonstd::make_unexpected(ProviderError {FailureCategory::BadResponse
                    , tr("Cannot map Real-Debrid links to files") + u" (%1 links, %2 files)"_s.arg(links.size()).arg(selected.size()), std::nullopt}));
            return;
        }
        const Selected file = *it;
        const bool sizeIsUnique = (std::count_if(selected.cbegin(), selected.cend()
                , [&file](const Selected &s) { return s.bytes == file.bytes; }) == 1);
        const QString hostLink = links.at(static_cast<qsizetype>(it - selected.cbegin())).toString();

        QUrlQuery form;
        form.addQueryItem(u"link"_s, hostLink);
        call("POST", u"unrestrict/link"_s, {}, form, guard, [this, file, sizeIsUnique, callback](const JsonResult &result)
        {
            if (!result)
            {
                callback(nonstd::make_unexpected(result.error()));
                return;
            }
            const QJsonObject u = result.value().object();
            const QUrl download {u.value(u"download"_s).toString()};
            const qint64 size = u.value(u"filesize"_s).toInteger(-1);
            const QString name = u.value(u"filename"_s).toString();
            // The exact byte size identifies the file. The name only disambiguates files of equal
            // size: the service may present it differently from the torrent's path. Either way
            // every piece is still verified against the torrent's own hashes by libtorrent.
            const bool nameMatches = (name == basename(file.path));
            if ((size != file.bytes) || (!nameMatches && !sizeIsUnique))
            {
                callback(nonstd::make_unexpected(ProviderError {FailureCategory::BadResponse
                        , tr("Real-Debrid link does not match the expected file") + ((size != file.bytes) ? u" (size)"_s : u" (name)"_s), std::nullopt}));
                return;
            }
            const UrlCheck hostCheck = checkUrl(download, dataUrlPolicy());
            if (hostCheck != UrlCheck::Allowed)
            {
                // Scheme and host only: the path and query of a download link are never shown.
                callback(nonstd::make_unexpected(ProviderError {FailureCategory::BadResponse
                        , tr("Real-Debrid returned an unexpected download host") + u" (%1: %2)"_s.arg(describe(hostCheck), redactUrl(download).left(120)), std::nullopt}));
                return;
            }
            callback(TemporaryLink {download, size});
        });
    });
}

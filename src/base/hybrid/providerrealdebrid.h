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

#include <functional>

#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QUrl>
#include <QUrlQuery>

#include "provider.h"

class QNetworkAccessManager;
class QTimer;

// Real-Debrid adapter (spec 05 "Provider plan: Real-Debrid first").
// Contract verified against https://api.real-debrid.com/ on 2026-11-09 (see qbitfusion/docs/DECISIONS.md).
// Known gaps, all failing closed:
//  - instantAvailability could not be confirmed, so the only non-mutating availability check is
//    finding an existing remote torrent with the same v1 info-hash in the user's account.
//  - How `links` map to selected files is undocumented; every resolved link is verified by the
//    unrestricted filename + filesize before use.
//  - Real-Debrid identifies torrents by SHA-1; v2-only torrents are unsupported.

namespace Hybrid
{
    class RealDebridProvider final : public Provider
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(RealDebridProvider)

    public:
        static inline const QString ID = QStringLiteral("realdebrid");

        explicit RealDebridProvider(QNetworkAccessManager *network, QObject *parent = nullptr);
        ~RealDebridProvider() override;

        QString id() const override;
        QString displayName() const override;
        ProviderCapabilities capabilities() const override;
        UrlPolicy dataUrlPolicy() const override;
        AccountState accountState() const override;

        void checkAvailability(const TorrentIdentity &identity, QObject *context, ProviderCallback<Availability> callback) override;
        void resolveManifest(const TorrentIdentity &identity, bool allowRemoteCreate, QObject *context
                , ProviderCallback<ProviderManifest> callback) override;
        void resolveFileLink(const QString &resourceId, const QString &providerFileId, QObject *context
                , ProviderCallback<TemporaryLink> callback) override;

        // Account management (UI). Secrets are written straight to the OS credential store.
        void loadStoredCredentials();
        void startDeviceAuthorization();
        void cancelDeviceAuthorization();
        void usePrivateToken(const QString &token);   // local private use only (docs warning)
        void signOut();
        void refreshAccountInfo();

        QString accountName() const;
        int premiumDaysLeft() const;
        bool isUsingPrivateToken() const;

        // Test fixture support only: point at a local mock API and allow loopback.
        void setEndpointsForTesting(const QUrl &apiBase, const QUrl &oauthBase);

    signals:
        void deviceCodeReady(const QString &userCode, const QUrl &verificationUrl, int expiresInSeconds);
        void authorizationFinished(bool ok, const QString &message);

    private:
        using JsonResult = ProviderResult<QJsonDocument>;
        using JsonCallback = std::function<void (const JsonResult &)>;

        struct PendingCall
        {
            QByteArray verb;
            QUrl url;
            QUrlQuery form;
            bool authenticated = true;
            QPointer<QObject> context;
            JsonCallback callback;
            int attempt = 0;
        };

        void call(const QByteArray &verb, const QString &path, const QUrlQuery &query, const QUrlQuery &form
                , QObject *context, JsonCallback callback);
        void enqueue(PendingCall call);
        void pump();
        void send(PendingCall call);
        void refreshAccessToken(std::function<void (bool)> done);
        void persistCredentials();
        void setAccountState(AccountState state);
        void pollDeviceCredentials();

        void fetchInfo(const QString &resourceId, QObject *context, std::function<void (const JsonResult &)> callback);
        void findByHash(const QString &v1Hash, QObject *context, std::function<void (const ProviderResult<QString> &)> callback);
        static ProviderManifest manifestFrom(const QJsonDocument &info);
        static ProviderError errorFrom(int httpStatus, const QByteArray &body);

        QNetworkAccessManager *m_network = nullptr;
        QUrl m_apiBase;
        QUrl m_oauthBase;
        bool m_testMode = false;

        AccountState m_state = AccountState::NotConfigured;
        bool m_privateToken = false;
        QString m_clientId;
        QString m_clientSecret;
        QString m_refreshToken;
        QString m_accessToken;
        QDateTime m_accessExpiry;
        bool m_refreshing = false;
        QList<std::function<void (bool)>> m_refreshWaiters;

        QString m_accountName;
        int m_premiumDays = 0;

        QQueue<PendingCall> m_queue;
        QTimer *m_rateTimer = nullptr;
        qint64 m_lastCallMs = 0;

        QString m_deviceCode;
        int m_deviceInterval = 5;
        QDateTime m_deviceDeadline;
        QTimer *m_deviceTimer = nullptr;
    };
}

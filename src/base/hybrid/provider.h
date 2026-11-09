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
#include <optional>

#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

#include "base/3rdparty/expected.hpp"
#include "filebinding.h"
#include "hybridtypes.h"
#include "urlguard.h"

// Provider adapter contract (spec 05 "Interface segregation"). Adapters own remote API semantics
// only — never piece downloading, torrent storage or transport decisions.
//
// Contract (spec 05 "Provider-neutral contract"):
//  1. No value returned here ever contains a credential.
//  2. Availability::Cached is advisory until manifest binding and Range preflight succeed.
//  3. Mutating calls (remote torrent creation) only happen when the caller passes allowRemoteCreate.
//  4. Resource IDs are scoped to the account currently signed in.
//  5. Access denial (403) is final; adapters never try alternative endpoints.
//  6. Every call is cancellable by destroying the context object and has a bounded timeout.

namespace Hybrid
{
    struct TorrentIdentity
    {
        QString infoHashV1;     // lower-case hex, 40 chars, empty if v2-only
        QString infoHashV2;     // lower-case hex, 64 chars, empty if v1-only
        QString name;           // display only; never sent to logs by the coordinator
    };

    struct ProviderCapabilities
    {
        bool canCheckCachedWithoutMutation = false;
        bool canSupplyTorrentMetadata = false;
        bool canListFiles = true;
        bool providesPerFileDownloadUrl = true;
        bool supportsLinkRefresh = true;
        bool canQueueRemoteTorrent = false;
    };

    enum class Availability
    {
        Cached,
        RemoteFetching,
        Uncached,
        Removed,
        Unknown
    };

    struct ProviderError
    {
        FailureCategory category = FailureCategory::Temporary;
        QString message;                        // redacted, user-presentable
        std::optional<qint64> retryAfterSeconds;
    };

    struct ProviderManifest
    {
        QString resourceId;                     // account-scoped remote torrent id
        Availability availability = Availability::Unknown;
        QList<ProviderFile> files;
    };

    struct TemporaryLink
    {
        QUrl url;                               // signed; memory only, never persisted or logged
        qint64 fileSize = -1;
    };

    template <typename T>
    using ProviderResult = nonstd::expected<T, ProviderError>;

    template <typename T>
    using ProviderCallback = std::function<void (const ProviderResult<T> &)>;

    enum class AccountState
    {
        NotConfigured,
        Connected,
        AuthenticationRequired,
        Error
    };

    class Provider : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(Provider)

    public:
        using QObject::QObject;

        virtual QString id() const = 0;             // stable, [a-z0-9_-]
        virtual QString displayName() const = 0;
        virtual ProviderCapabilities capabilities() const = 0;
        virtual UrlPolicy dataUrlPolicy() const = 0;
        virtual AccountState accountState() const = 0;

        // `context` bounds the callback lifetime: if it is destroyed first, the callback is dropped.
        virtual void checkAvailability(const TorrentIdentity &identity, QObject *context
                , ProviderCallback<Availability> callback) = 0;
        virtual void resolveManifest(const TorrentIdentity &identity, bool allowRemoteCreate, QObject *context
                , ProviderCallback<ProviderManifest> callback) = 0;
        virtual void resolveFileLink(const QString &resourceId, const QString &providerFileId, QObject *context
                , ProviderCallback<TemporaryLink> callback) = 0;

    signals:
        void accountStateChanged();
    };
}

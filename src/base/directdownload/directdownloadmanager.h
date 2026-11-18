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

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>

#include "base/hybrid/tokenbucket.h"
#include "base/path.h"
#include "httptransfer.h"

class QNetworkAccessManager;
class QTimer;

// Direct downloads: plain HTTP(S) files, kept apart from libtorrent because a link is not a torrent
// (no pieces, no hashes, no infohash). One resumable GET per file into "<name>.part" in the save
// folder, renamed when complete.
//
// What is stored where:
// - <profile>/data/directdownloads.json: names, folders, sizes, states, the host for display. No links.
// - The link itself: the OS credential store (Hybrid::SecureStore), only until the file is complete.
//   Without a persistent store (or for a link too long for it) the item asks for the link again
//   after a restart.

namespace DirectDownload
{
    enum class State
    {
        Queued,
        Downloading,
        Paused,
        Completed,
        Failed,
        NeedsLink   // after a restart, when the link could not be kept
    };

    struct ItemInfo
    {
        QString id;
        QString name;
        Path savePath;
        QString host;            // "https://host/…": safe to show
        qint64 totalSize = -1;   // -1 if the server did not say
        qint64 downloaded = 0;
        qint64 speed = 0;        // bytes per second
        State state = State::Queued;
        QString error;           // redacted
        QDateTime addedOn;
        QDateTime completedOn;
        qint64 retryInSeconds = -1;
        bool linkSaved = false;  // kept in the credential store for resume after a restart
        bool resumable = false;  // the server accepts byte ranges

        Path filePath() const;   // the complete file, or the .part file while incomplete
    };

    struct ManagerConfig
    {
        Path stateFile;
        QString secretKeyPrefix = QStringLiteral("directdownload/");
        int maxActive = 3;
        // Production: the app's proxy, cookies and global download limit. Tests turn this off.
        bool useAppSettings = true;
        std::function<qint64 ()> clock;
    };

    class Manager final : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(Manager)

    public:
        static void initInstance();
        static void freeInstance();
        static Manager *instance();

        explicit Manager(ManagerConfig config, QObject *parent = nullptr);
        ~Manager() override;

        static QString partSuffix();

        // Probes a pasted link with this manager's network settings. Owned by `parent`.
        LinkProbe *createProbe(const QUrl &link, QObject *parent);

        // `name` must be a sanitized file name; it is made unique in `savePath`. Returns the id.
        QString add(const QUrl &link, const ProbeResult &probe, const Path &savePath, const QString &name, bool startPaused = false);

        void pause(const QString &id);
        void resume(const QString &id);
        void remove(const QString &id, bool deleteFiles);
        // For an expired or forgotten link: continues from the bytes already on disk.
        bool replaceLink(const QString &id, const QUrl &link);

        QStringList ids() const;
        std::optional<ItemInfo> info(const QString &id) const;
        qint64 totalSpeed() const;
        int activeCount() const;

        void setMaxActive(int value);
        void setSpeedLimit(qint64 bytesPerSecond);  // tests; production follows the session limit

    signals:
        void itemAdded(const QString &id);
        void itemChanged(const QString &id);
        void itemRemoved(const QString &id);
        void itemFinished(const QString &id);

    private:
        struct Item
        {
            ItemInfo info;
            QUrl link;          // secret, memory only
            QUrl resolvedUrl;   // secret, memory only: the link after redirects
            Validator validator;
            Transfer *transfer = nullptr;
            bool usingResolvedUrl = false;
            bool restartedFromZero = false;
            int attempts = 0;
            qint64 retryAtMs = 0;
            qint64 bytesAtStart = 0;
            qint64 bytesAtLastTick = 0;
        };

        static Manager *m_instance;

        qint64 now() const;
        Item *find(const QString &id);
        void schedule();
        void startItem(Item &item);
        void stopTransfer(Item &item);
        void onSucceeded(const QString &id);
        void onFailed(const QString &id, const Failure &failure);
        void onTick();
        void applyAppSettings();
        void setFailed(Item &item, const QString &error);
        bool isNameTaken(const Path &folder, const QString &name, const QString &exceptId) const;

        void load();
        void markDirty();
        void save();

        ManagerConfig m_config;
        QNetworkAccessManager *m_network = nullptr;
        Hybrid::TokenBucket m_bucket;
        QTimer *m_tick = nullptr;
        QStringList m_order;
        QHash<QString, Item> m_items;
        qint64 m_lastTickMs = 0;
        bool m_dirty = false;
    };

    QString displayName(State state);
}

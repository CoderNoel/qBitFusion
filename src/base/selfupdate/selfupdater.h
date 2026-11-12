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

#include <optional>

#include <QObject>
#include <QString>
#include <QStringList>

#include "updatemanifest.h"

class QProcess;

namespace Net
{
    struct DownloadResult;
}

namespace SelfUpdate
{
    // Checks qBitFusion's release feed, downloads a newer package in the background, verifies it and
    // unpacks it into <program folder>/update/staged. Installing happens the next time the program
    // starts (launchStagedUpdate()), so running downloads are never interrupted.
    class SelfUpdater final : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(SelfUpdater)

    public:
        enum class Outcome
        {
            Current,
            Ready,     // a newer version is staged and will be installed on the next start
            Failed
        };

        explicit SelfUpdater(QObject *parent = nullptr);

        // True on Windows for a release build (revision > 0) whose program folder is writable.
        static bool isSupported();

        // Starts one check. Ignored while a check is already running.
        void check();
        bool isBusy() const;

        // Called early at start-up, after the single-instance check. If a verified update is staged,
        // starts the installer and returns true: the caller must then exit at once.
        static bool launchStagedUpdate(const QStringList &arguments);

        struct AppliedUpdate
        {
            bool ok = false;
            QString version;
            QString error;
        };
        // What the last install did, once; nothing if there was no install since the last call.
        static std::optional<AppliedUpdate> takeAppliedUpdate();

    signals:
        // For Ready, `detail` is the new version; for Failed, the reason.
        void finished(SelfUpdate::SelfUpdater::Outcome outcome, const QString &detail);

    private:
        void onFeedFile(const Net::DownloadResult &result, QByteArray &target);
        void evaluateFeed();
        void onPackageDownloaded(const Net::DownloadResult &result);
        void onPackageExtracted();
        void finish(Outcome outcome, const QString &detail);

        bool m_busy = false;
        int m_pendingFeedFiles = 0;
        bool m_feedFailed = false;
        QString m_feedError;
        QByteArray m_manifestBytes;
        QByteArray m_signatureBytes;
        Manifest m_manifest;
        QString m_packagePath;
        QProcess *m_extractor = nullptr;
    };
}

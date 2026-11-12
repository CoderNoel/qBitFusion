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

#include "selfupdater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>

#include "base/global.h"
#include "base/logger.h"
#include "base/net/downloadmanager.h"
#include "base/preferences.h"
#include "base/version.h"
#include "applyupdate.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    // Releases are signed in CI with the matching private key (repository secret QBF_UPDATE_SIGNING_KEY).
    // Replacing this key means older builds can no longer verify, and so no longer install, new releases.
    const QByteArray UPDATE_PUBLIC_KEY = QByteArray::fromBase64("7SVT90hdBXEuzf6C7GWDmBxFVZBbldD4EniDC1rJD6g=");

    const QString OFFICIAL_FEED = u"https://github.com/CoderNoel/qBitFusion/releases/latest/download/"_s;
    const QString OFFICIAL_PACKAGES = u"https://github.com/CoderNoel/qBitFusion/releases/download/"_s;
    const QString PLATFORM = u"windows-x64"_s;
    const QString FEED_OVERRIDE_ENV = u"QBITFUSION_UPDATE_FEED"_s;   // for testing a release before publishing it

    QString feedBase()
    {
        const QString override = qEnvironmentVariable(FEED_OVERRIDE_ENV.toLatin1().constData()).trimmed();
        if (override.isEmpty())
            return OFFICIAL_FEED;
        return override.endsWith(u'/') ? override : (override + u'/');
    }

    QStringList allowedPackagePrefixes()
    {
        const QString base = feedBase();
        return (base == OFFICIAL_FEED) ? QStringList {OFFICIAL_PACKAGES} : QStringList {OFFICIAL_PACKAGES, base};
    }

    QDir programDir()
    {
        return QDir(QCoreApplication::applicationDirPath());
    }

    QString updateDir()
    {
        return programDir().filePath(u"update"_s);
    }

    QString stagedDir()
    {
        return QDir(updateDir()).filePath(u"staged"_s);
    }

    QString userAgent()
    {
        return u"qBitFusion/%1 updater (+https://github.com/CoderNoel/qBitFusion)"_s.arg(SelfUpdate::currentVersion().toString());
    }

    QByteArray sha256OfFile(const QString &path)
    {
        QFile file {path};
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QCryptographicHash hash {QCryptographicHash::Sha256};
        if (!hash.addData(&file))
            return {};
        return hash.result();
    }

    SelfUpdate::Version readStagedVersion(const QString &markerName)
    {
        QFile marker {QDir(stagedDir()).filePath(markerName)};
        if (!marker.open(QIODevice::ReadOnly))
            return {};
        return SelfUpdate::Version::fromString(QString::fromUtf8(marker.readAll()));
    }

    void removeStaged()
    {
        QDir(stagedDir()).removeRecursively();
    }
}

SelfUpdate::SelfUpdater::SelfUpdater(QObject *parent)
    : QObject(parent)
{
}

bool SelfUpdate::SelfUpdater::isSupported()
{
#ifdef Q_OS_WIN
    static const bool supported = []
    {
        const bool releaseBuild = (currentVersion().revision > 0) || qEnvironmentVariableIsSet(FEED_OVERRIDE_ENV.toLatin1().constData());
        if (!releaseBuild)
            return false;

        // The update replaces files next to the program, so that folder must be writable for this user.
        if (!QDir().mkpath(updateDir()))
            return false;
        QFile probe {QDir(updateDir()).filePath(u".write-test"_s)};
        const bool writable = probe.open(QIODevice::WriteOnly);
        probe.close();
        probe.remove();
        return writable;
    }();
    return supported;
#else
    return false;
#endif
}

bool SelfUpdate::SelfUpdater::isBusy() const
{
    return m_busy;
}

void SelfUpdate::SelfUpdater::check()
{
    if (m_busy || !isSupported())
        return;
    m_busy = true;

    // An update downloaded earlier and not installed yet needs no new download.
    if (const Version staged = readStagedVersion(u"ready"_s); staged.isValid() && (staged > currentVersion()))
    {
        finish(Outcome::Ready, staged.toString());
        return;
    }

    m_manifestBytes.clear();
    m_signatureBytes.clear();
    m_feedFailed = false;
    m_feedError.clear();
    m_pendingFeedFiles = 2;

    const bool useProxy = Preferences::instance()->useProxyForGeneralPurposes();
    auto *manager = Net::DownloadManager::instance();
    const QString base = feedBase();
    manager->download(Net::DownloadRequest(base + u"update.json"_s).userAgent(userAgent()).limit(64 * 1024), useProxy, this
            , [this](const Net::DownloadResult &result) { onFeedFile(result, m_manifestBytes); });
    manager->download(Net::DownloadRequest(base + u"update.json.sig"_s).userAgent(userAgent()).limit(4 * 1024), useProxy, this
            , [this](const Net::DownloadResult &result) { onFeedFile(result, m_signatureBytes); });
}

void SelfUpdate::SelfUpdater::onFeedFile(const Net::DownloadResult &result, QByteArray &target)
{
    if (result.status == Net::DownloadStatus::Success)
    {
        target = result.data;
    }
    else
    {
        m_feedFailed = true;
        m_feedError = result.errorString;
    }

    if (--m_pendingFeedFiles == 0)
        evaluateFeed();
}

void SelfUpdate::SelfUpdater::evaluateFeed()
{
    if (m_feedFailed)
    {
        finish(Outcome::Failed, tr("Could not reach the update server: %1").arg(m_feedError));
        return;
    }

    if (!verifySignature(m_manifestBytes, m_signatureBytes, UPDATE_PUBLIC_KEY))
    {
        finish(Outcome::Failed, tr("The update information is not signed by qBitFusion. It was ignored."));
        return;
    }

    const auto manifest = parseManifest(m_manifestBytes, PLATFORM, allowedPackagePrefixes());
    if (!manifest)
    {
        finish(Outcome::Failed, tr("The update information is not valid: %1").arg(manifest.error()));
        return;
    }
    m_manifest = manifest.value();

    if (m_manifest.version <= currentVersion())
    {
        finish(Outcome::Current, currentVersion().toString());
        return;
    }

    LogMsg(tr("qBitFusion %1 is available. Downloading it in the background.").arg(m_manifest.version.toString()));
    const bool useProxy = Preferences::instance()->useProxyForGeneralPurposes();
    Net::DownloadManager::instance()->download(Net::DownloadRequest(m_manifest.package.url).userAgent(userAgent())
            .saveToFile(true).limit(m_manifest.package.size + 1), useProxy, this
            , [this](const Net::DownloadResult &result) { onPackageDownloaded(result); });
}

void SelfUpdate::SelfUpdater::onPackageDownloaded(const Net::DownloadResult &result)
{
    if (result.status != Net::DownloadStatus::Success)
    {
        finish(Outcome::Failed, tr("Could not download the update: %1").arg(result.errorString));
        return;
    }

    m_packagePath = result.filePath.toString();
    if ((QFileInfo(m_packagePath).size() != m_manifest.package.size) || (sha256OfFile(m_packagePath) != m_manifest.package.sha256))
    {
        QFile::remove(m_packagePath);
        finish(Outcome::Failed, tr("The downloaded update does not match its signed description. It was discarded."));
        return;
    }

    // Unpack with the tar.exe that ships with Windows (it reads zip files), so no extra library is needed.
    const QString incoming = QDir(updateDir()).filePath(u"incoming"_s);
    QDir(incoming).removeRecursively();
    if (!QDir().mkpath(incoming))
    {
        QFile::remove(m_packagePath);
        finish(Outcome::Failed, tr("Cannot write to the program folder."));
        return;
    }

    const QString tar = QDir(qEnvironmentVariable("SystemRoot", u"C:\\Windows"_s)).filePath(u"System32/tar.exe"_s);
    m_extractor = new QProcess(this);
    m_extractor->setProgram(tar);
    m_extractor->setArguments({u"-xf"_s, QDir::toNativeSeparators(m_packagePath), u"-C"_s, QDir::toNativeSeparators(incoming)});
    connect(m_extractor, &QProcess::finished, this, &SelfUpdater::onPackageExtracted);
    connect(m_extractor, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error)
    {
        if (error == QProcess::FailedToStart)
            onPackageExtracted();
    });
    m_extractor->start();
}

void SelfUpdate::SelfUpdater::onPackageExtracted()
{
    const bool extracted = (m_extractor->exitStatus() == QProcess::NormalExit) && (m_extractor->exitCode() == 0)
            && (m_extractor->error() != QProcess::FailedToStart);
    m_extractor->deleteLater();
    m_extractor = nullptr;
    QFile::remove(m_packagePath);

    const QDir incoming {QDir(updateDir()).filePath(u"incoming"_s)};
    const QStringList topLevel = incoming.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    const QString exeName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    if (!extracted || (topLevel.size() != 1) || !QFile::exists(QDir(incoming.filePath(topLevel.first())).filePath(exeName)))
    {
        QDir(incoming.path()).removeRecursively();
        finish(Outcome::Failed, tr("The update package could not be unpacked."));
        return;
    }

    removeStaged();
    if (!QDir().mkpath(stagedDir()) || !QDir().rename(incoming.filePath(topLevel.first()), QDir(stagedDir()).filePath(u"app"_s)))
    {
        QDir(incoming.path()).removeRecursively();
        removeStaged();
        finish(Outcome::Failed, tr("Cannot write to the program folder."));
        return;
    }
    QDir(incoming.path()).removeRecursively();

    QSaveFile marker {QDir(stagedDir()).filePath(u"ready"_s)};
    if (!marker.open(QIODevice::WriteOnly) || (marker.write(m_manifest.version.toString().toUtf8()) < 0) || !marker.commit())
    {
        removeStaged();
        finish(Outcome::Failed, tr("Cannot write to the program folder."));
        return;
    }

    finish(Outcome::Ready, m_manifest.version.toString());
}

void SelfUpdate::SelfUpdater::finish(const Outcome outcome, const QString &detail)
{
    m_busy = false;
    if (outcome == Outcome::Failed)
        LogMsg(tr("qBitFusion update check failed. %1").arg(detail), Log::WARNING);
    else if (outcome == Outcome::Ready)
        LogMsg(tr("qBitFusion %1 is downloaded and will be installed the next time qBitFusion starts.").arg(detail));
    emit finished(outcome, detail);
}

bool SelfUpdate::SelfUpdater::launchStagedUpdate(const QStringList &arguments)
{
#ifdef Q_OS_WIN
    const QDir staged {stagedDir()};
    if (!staged.exists())
        return false;

    const Version ready = readStagedVersion(u"ready"_s);
    const QString stagedExe = QDir(staged.filePath(u"app"_s)).filePath(QFileInfo(QCoreApplication::applicationFilePath()).fileName());
    if (!ready.isValid() || (ready <= currentVersion()) || !QFile::exists(stagedExe))
    {
        // Nothing to install: a finished or failed install, or a version that is not newer.
        removeStaged();
        return false;
    }

    // Marked first, so that a failing install cannot be retried at every start.
    if (!QFile::rename(staged.filePath(u"ready"_s), staged.filePath(u"applying"_s)))
        return false;

    QStringList installerArgs {QString::fromWCharArray(APPLY_SWITCH), QString::number(QCoreApplication::applicationPid())
        , QDir::toNativeSeparators(programDir().absolutePath()), u"--"_s};
    installerArgs += arguments;
    if (!QProcess::startDetached(stagedExe, installerArgs, staged.filePath(u"app"_s)))
    {
        removeStaged();
        return false;
    }
    return true;
#else
    Q_UNUSED(arguments);
    return false;
#endif
}

std::optional<SelfUpdate::SelfUpdater::AppliedUpdate> SelfUpdate::SelfUpdater::takeAppliedUpdate()
{
    QFile report {QDir(updateDir()).filePath(u"result.txt"_s)};
    if (!report.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QStringList lines = QString::fromUtf8(report.readAll()).split(u'\n');
    report.close();
    report.remove();

    AppliedUpdate applied;
    applied.ok = (lines.value(0).trimmed() == u"ok");
    applied.version = lines.value(1).trimmed();
    applied.error = lines.value(2).trimmed();
    return applied;
}

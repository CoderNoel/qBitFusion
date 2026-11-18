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

#include <QDialog>
#include <QList>
#include <QUrl>

#include "base/directdownload/httptransfer.h"
#include "base/path.h"
#include "base/settingvalue.h"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class FileSystemPathComboEdit;

namespace DirectDownload
{
    class Manager;
}

// Checks each pasted http(s) link once, then offers the files among them for download.
// Links that turn out to be torrents are handed back (signals) to the torrent add flow.
class AddDirectDownloadDialog final : public QDialog
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(AddDirectDownloadDialog)

public:
    AddDirectDownloadDialog(DirectDownload::Manager *manager, const QList<QUrl> &links, QWidget *parent = nullptr);
    ~AddDirectDownloadDialog() override;

signals:
    // `source` is safe to log (scheme and host only).
    void torrentDataFound(const QString &source, const QByteArray &data);
    void magnetFound(const QString &magnetUri);
    // The server compressed the .torrent: the torrent path fetches it again itself.
    void torrentLinkFound(const QString &link);

private:
    struct Entry
    {
        QUrl link;  // a secret: never shown
        QTreeWidgetItem *item = nullptr;
        std::optional<DirectDownload::ProbeResult> file;
        bool done = false;
        bool failed = false;
    };

    void onProbeFinished(int index, const DirectDownload::ProbeResult &result);
    void onProbeFailed(int index, const DirectDownload::Failure &failure);
    void updateState();
    void accept() override;

    DirectDownload::Manager *m_manager = nullptr;
    QList<Entry> m_entries;
    QTreeWidget *m_list = nullptr;
    QLabel *m_summary = nullptr;
    FileSystemPathComboEdit *m_savePath = nullptr;
    QPushButton *m_downloadButton = nullptr;
    SettingValue<Path> m_storeSavePath;
    SettingValue<QSize> m_storeDialogSize;
};

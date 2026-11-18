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

#include <QStringList>
#include <QWidget>

#include "base/settingvalue.h"

class QAction;
class QLabel;
class QSortFilterProxyModel;
class QTreeView;
class DirectDownloadModel;

namespace DirectDownload
{
    class Manager;
}

// The Links page: direct downloads with their own list and actions, next to Transfers.
class DirectDownloadWidget final : public QWidget
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(DirectDownloadWidget)

public:
    // `addLinksAction` opens the dialog for pasting links (the same one as for magnet links).
    DirectDownloadWidget(DirectDownload::Manager *manager, QAction *addLinksAction, QWidget *parent = nullptr);
    ~DirectDownloadWidget() override;

    int count() const;

signals:
    void countChanged(int count);

private:
    QStringList selectedIds() const;
    void updateActions();
    void showContextMenu(const QPoint &pos);
    void startSelected();
    void pauseSelected();
    void removeSelected();
    void replaceLink();
    void openSelected();
    void openFolder();
    void copyNames();

    DirectDownload::Manager *m_manager = nullptr;
    DirectDownloadModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QTreeView *m_view = nullptr;
    QLabel *m_summary = nullptr;
    QAction *m_actionStart = nullptr;
    QAction *m_actionPause = nullptr;
    QAction *m_actionRemove = nullptr;
    QAction *m_actionReplaceLink = nullptr;
    QAction *m_actionOpenFolder = nullptr;
    SettingValue<QByteArray> m_storeHeaderState;
};

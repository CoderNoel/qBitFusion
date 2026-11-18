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

#include <QAbstractTableModel>
#include <QStringList>

namespace DirectDownload
{
    class Manager;
    struct ItemInfo;
}

// The rows of the Links page: one per direct download, in the order they were added.
class DirectDownloadModel final : public QAbstractTableModel
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(DirectDownloadModel)

public:
    enum Column
    {
        COL_NAME,
        COL_SIZE,
        COL_PROGRESS,
        COL_SPEED,
        COL_ETA,
        COL_STATUS,
        COL_SOURCE,
        COL_ADDED,

        COL_COUNT
    };

    enum Role
    {
        SortRole = Qt::UserRole,
        IdRole,
        StateRole
    };

    explicit DirectDownloadModel(DirectDownload::Manager *manager, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    QString idAt(int row) const;

private:
    void onAdded(const QString &id);
    void onChanged(const QString &id);
    void onRemoved(const QString &id);

    QVariant display(const DirectDownload::ItemInfo &info, int column) const;
    QVariant sortValue(const DirectDownload::ItemInfo &info, int column) const;

    DirectDownload::Manager *m_manager = nullptr;
    QStringList m_ids;
};

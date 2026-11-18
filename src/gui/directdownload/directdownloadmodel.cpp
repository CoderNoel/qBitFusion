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

#include "directdownloadmodel.h"

#include <QLocale>

#include "base/directdownload/directdownloadmanager.h"
#include "base/utils/misc.h"
#include "gui/uithememanager.h"

using namespace Qt::Literals::StringLiterals;
using DirectDownload::State;

namespace
{
    qint64 etaSeconds(const DirectDownload::ItemInfo &info)
    {
        if ((info.state != State::Downloading) || (info.speed <= 0) || (info.totalSize <= 0))
            return -1;
        return std::max<qint64>(0, info.totalSize - info.downloaded) / info.speed;
    }

    qreal progressOf(const DirectDownload::ItemInfo &info)
    {
        if (info.state == State::Completed)
            return 1;
        if (info.totalSize <= 0)
            return 0;
        return std::clamp<qreal>(static_cast<qreal>(info.downloaded) / static_cast<qreal>(info.totalSize), 0, 1);
    }

    QString statusText(const DirectDownload::ItemInfo &info)
    {
        if ((info.state == State::Queued) && (info.retryInSeconds > 0))
            return DirectDownloadModel::tr("Retrying in %1").arg(Utils::Misc::userFriendlyDuration(info.retryInSeconds, -1, Utils::Misc::TimeResolution::Seconds));
        return DirectDownload::displayName(info.state);
    }

    QString iconName(const State state)
    {
        switch (state)
        {
        case State::Queued:
            return u"queued"_s;
        case State::Downloading:
            return u"downloading"_s;
        case State::Paused:
            return u"stopped"_s;
        case State::Completed:
            return u"checked-completed"_s;
        case State::Failed:
            return u"error"_s;
        case State::NeedsLink:
            return u"insert-link"_s;
        }
        return u"insert-link"_s;
    }
}

DirectDownloadModel::DirectDownloadModel(DirectDownload::Manager *manager, QObject *parent)
    : QAbstractTableModel(parent)
    , m_manager {manager}
    , m_ids {manager->ids()}
{
    connect(manager, &DirectDownload::Manager::itemAdded, this, &DirectDownloadModel::onAdded);
    connect(manager, &DirectDownload::Manager::itemChanged, this, &DirectDownloadModel::onChanged);
    connect(manager, &DirectDownload::Manager::itemRemoved, this, &DirectDownloadModel::onRemoved);
}

int DirectDownloadModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_ids.size());
}

int DirectDownloadModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : COL_COUNT;
}

QString DirectDownloadModel::idAt(const int row) const
{
    return m_ids.value(row);
}

QVariant DirectDownloadModel::headerData(const int section, const Qt::Orientation orientation, const int role) const
{
    if (orientation != Qt::Horizontal)
        return {};

    if (role == Qt::TextAlignmentRole)
    {
        switch (section)
        {
        case COL_SIZE:
        case COL_SPEED:
        case COL_ETA:
            return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
    if (role != Qt::DisplayRole)
        return {};

    switch (section)
    {
    case COL_NAME:
        return tr("Name");
    case COL_SIZE:
        return tr("Size");
    case COL_PROGRESS:
        return tr("Progress");
    case COL_SPEED:
        return tr("Speed");
    case COL_ETA:
        return tr("ETA", "i18n: Estimated Time of Arrival / Time left");
    case COL_STATUS:
        return tr("Status");
    case COL_SOURCE:
        return tr("Source");
    case COL_ADDED:
        return tr("Added On");
    default:
        return {};
    }
}

QVariant DirectDownloadModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || (index.row() >= m_ids.size()))
        return {};

    const auto info = m_manager->info(m_ids.at(index.row()));
    if (!info)
        return {};

    switch (role)
    {
    case Qt::DisplayRole:
        return display(*info, index.column());
    case SortRole:
        return sortValue(*info, index.column());
    case IdRole:
        return info->id;
    case StateRole:
        return static_cast<int>(info->state);
    case Qt::DecorationRole:
        if (index.column() == COL_NAME)
            return UIThemeManager::instance()->getIcon(iconName(info->state));
        return {};
    case Qt::ToolTipRole:
        if (index.column() == COL_NAME)
            return info->filePath().toString();
        if ((index.column() == COL_STATUS) && !info->error.isEmpty())
            return info->error;
        if ((index.column() == COL_STATUS) && (info->state == State::NeedsLink))
            return tr("The link was not kept after the restart. Copy the link again and use \"Replace link\" to continue.");
        if ((index.column() == COL_SOURCE) && !info->resumable && (info->state != State::Completed))
            return tr("This server does not support resuming. A paused or interrupted download starts again from the beginning.");
        return {};
    case Qt::TextAlignmentRole:
        switch (index.column())
        {
        case COL_SIZE:
        case COL_SPEED:
        case COL_ETA:
            return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return {};
        }
    default:
        return {};
    }
}

QVariant DirectDownloadModel::display(const DirectDownload::ItemInfo &info, const int column) const
{
    switch (column)
    {
    case COL_NAME:
        return info.name;
    case COL_SIZE:
        return (info.totalSize >= 0) ? Utils::Misc::friendlyUnit(info.totalSize) : tr("Unknown");
    case COL_PROGRESS:
        if ((info.totalSize <= 0) && (info.state != State::Completed))
            return Utils::Misc::friendlyUnit(info.downloaded);
        return u"%1%"_s.arg(QLocale().toString(progressOf(info) * 100, 'f', 1));
    case COL_SPEED:
        return (info.state == State::Downloading) ? Utils::Misc::friendlyUnit(info.speed, true) : QString();
    case COL_ETA:
        {
            const qint64 eta = etaSeconds(info);
            return (eta >= 0) ? Utils::Misc::userFriendlyDuration(eta) : QString();
        }
    case COL_STATUS:
        return statusText(info);
    case COL_SOURCE:
        return info.host;
    case COL_ADDED:
        return QLocale().toString(info.addedOn, QLocale::ShortFormat);
    default:
        return {};
    }
}

QVariant DirectDownloadModel::sortValue(const DirectDownload::ItemInfo &info, const int column) const
{
    switch (column)
    {
    case COL_NAME:
        return info.name;
    case COL_SIZE:
        return info.totalSize;
    case COL_PROGRESS:
        return progressOf(info);
    case COL_SPEED:
        return info.speed;
    case COL_ETA:
        {
            const qint64 eta = etaSeconds(info);
            return (eta >= 0) ? eta : std::numeric_limits<qint64>::max();
        }
    case COL_STATUS:
        return static_cast<int>(info.state);
    case COL_SOURCE:
        return info.host;
    case COL_ADDED:
        return info.addedOn;
    default:
        return {};
    }
}

void DirectDownloadModel::onAdded(const QString &id)
{
    const int row = static_cast<int>(m_ids.size());
    beginInsertRows({}, row, row);
    m_ids.append(id);
    endInsertRows();
}

void DirectDownloadModel::onChanged(const QString &id)
{
    const int row = static_cast<int>(m_ids.indexOf(id));
    if (row >= 0)
        emit dataChanged(index(row, 0), index(row, COL_COUNT - 1));
}

void DirectDownloadModel::onRemoved(const QString &id)
{
    const int row = static_cast<int>(m_ids.indexOf(id));
    if (row < 0)
        return;
    beginRemoveRows({}, row, row);
    m_ids.removeAt(row);
    endRemoveRows();
}

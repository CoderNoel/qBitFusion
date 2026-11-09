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

#include "ltlayout.h"

#include <libtorrent/file_storage.hpp>
#include <libtorrent/info_hash.hpp>
#include <libtorrent/torrent_info.hpp>

#include <QByteArray>

namespace
{
    QString toHex(const char *data, const int size)
    {
        return QString::fromLatin1(QByteArray::fromRawData(data, size).toHex());
    }
}

QList<Hybrid::TorrentFileEntry> Hybrid::nativeFileEntries(const lt::torrent_info &info)
{
    const lt::file_storage &fs = info.layout();
    QList<TorrentFileEntry> files;
    files.reserve(fs.num_files());
    for (const lt::file_index_t index : fs.file_range())
    {
        TorrentFileEntry entry;
        entry.nativeIndex = static_cast<int>(index);
        // Original metadata path (layout() is never affected by local renames), '/'-separated.
        entry.path = QString::fromStdString(fs.file_path(index)).replace(u'\\', u'/');
        entry.size = fs.file_size(index);
        entry.isPad = fs.pad_file_at(index);
        files.append(entry);
    }
    return files;
}

QList<Hybrid::PieceSlice> Hybrid::nativePieceSlices(const lt::torrent_info &info, const int piece)
{
    QList<PieceSlice> slices;
    if ((piece < 0) || (piece >= info.num_pieces()))
        return slices;

    const lt::piece_index_t index {piece};
    const lt::file_storage &fs = info.layout();
    const int size = info.piece_size_for_req(index);
    qint64 cursor = 0;
    for (const lt::file_slice &slice : info.map_block(index, 0, size))
    {
        slices.append({static_cast<int>(slice.file_index), slice.offset, slice.size, cursor, fs.pad_file_at(slice.file_index)});
        cursor += slice.size;
    }
    return slices;
}

qint64 Hybrid::nativePieceLength(const lt::torrent_info &info, const int piece)
{
    if ((piece < 0) || (piece >= info.num_pieces()))
        return 0;
    return info.piece_size_for_req(lt::piece_index_t {piece});
}

Hybrid::TorrentIdentity Hybrid::nativeIdentity(const lt::info_hash_t &hashes, const QString &name)
{
    TorrentIdentity identity;
    if (hashes.has_v1())
        identity.infoHashV1 = toHex(hashes.v1.data(), static_cast<int>(hashes.v1.size()));
    if (hashes.has_v2())
        identity.infoHashV2 = toHex(hashes.v2.data(), static_cast<int>(hashes.v2.size()));
    identity.name = name;
    return identity;
}

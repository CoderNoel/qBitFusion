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

#include <QtTypes>
#include <QHash>
#include <QList>
#include <QString>

#include "base/3rdparty/expected.hpp"

// Exact torrent-file <-> provider-file binding (spec 03 section 4 "Binding algorithm").
// Native libtorrent file indexes are the mapping truth. Provider names are untrusted and are used
// for matching only — never for local storage paths.

namespace Hybrid
{
    struct TorrentFileEntry
    {
        int nativeIndex = -1;
        QString path;       // relative path inside the torrent as stored in metadata, '/' separated
        qint64 size = 0;
        bool isPad = false;
    };

    struct ProviderFile
    {
        QString providerFileId;
        QString path;       // untrusted
        qint64 size = -1;
    };

    struct FileBinding
    {
        int nativeIndex = -1;
        QString providerFileId;
        qint64 size = 0;
    };

    enum class BindingError
    {
        NoProviderFiles,
        UnsafeProviderPath,
        DuplicateProviderPath,
        SizeMismatch,
        NoMatchingLayout
    };

    struct BindingFailure
    {
        BindingError error;
        int nativeIndex = -1;   // offending torrent file when applicable
    };

    struct BindingResult
    {
        QHash<int, FileBinding> byNativeIndex;  // bound non-pad files
        QList<int> unboundNativeIndexes;        // non-pad files the provider does not expose
        bool providerPathsIncludeRoot = false;
    };

    // Returns std::nullopt-equivalent error when any provider path is unsafe, duplicated, or when a
    // path matches but size differs (strong signal of a different release -> quarantine).
    // Files the provider does not list are reported as unbound, not an error: whether they are
    // required depends on wanted pieces (see requiresOnlyBound()).
    nonstd::expected<BindingResult, BindingFailure> bindFiles(const QList<TorrentFileEntry> &torrentFiles
            , const QList<ProviderFile> &providerFiles);

    // Normalizes an untrusted relative path. Returns empty string if unsafe: absolute, drive letter,
    // "..", ".", empty segment, NUL or control characters.
    QString normalizeRelativePath(const QString &path);

    QString describe(BindingError error);
}

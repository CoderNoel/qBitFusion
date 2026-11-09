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

#include "filebinding.h"

#include <optional>

#include <QCoreApplication>

using namespace Qt::Literals::StringLiterals;

namespace
{
    // Strips the first path segment ("Root/a/b" -> "a/b"). Returns empty if there is no separator.
    QString stripRoot(const QString &path)
    {
        const qsizetype slash = path.indexOf(u'/');
        return (slash < 0) ? QString() : path.mid(slash + 1);
    }

    struct Attempt
    {
        Hybrid::BindingResult result;
        std::optional<Hybrid::BindingFailure> failure;
        int matched = 0;
    };

    Attempt attemptBinding(const QList<Hybrid::TorrentFileEntry> &torrentFiles
            , const QHash<QString, const Hybrid::ProviderFile *> &providerByPath, const bool stripTorrentRoot)
    {
        Attempt attempt;
        attempt.result.providerPathsIncludeRoot = !stripTorrentRoot;

        for (const Hybrid::TorrentFileEntry &file : torrentFiles)
        {
            if (file.isPad)
                continue;

            QString key = Hybrid::normalizeRelativePath(file.path);
            if (stripTorrentRoot)
                key = stripRoot(key);

            const Hybrid::ProviderFile *providerFile = key.isEmpty() ? nullptr : providerByPath.value(key);
            if (!providerFile)
            {
                attempt.result.unboundNativeIndexes.append(file.nativeIndex);
                continue;
            }

            if (providerFile->size != file.size)
            {
                attempt.failure = Hybrid::BindingFailure {Hybrid::BindingError::SizeMismatch, file.nativeIndex};
                return attempt;
            }

            attempt.result.byNativeIndex.insert(file.nativeIndex
                    , Hybrid::FileBinding {file.nativeIndex, providerFile->providerFileId, file.size});
            ++attempt.matched;
        }

        return attempt;
    }
}

QString Hybrid::normalizeRelativePath(const QString &path)
{
    QString p = path;
    p.replace(u'\\', u'/');
    while (p.startsWith(u'/'))
        p.remove(0, 1); // providers commonly prefix "/" — treat as relative to the torrent

    if (p.isEmpty())
        return {};

    // Drive letters / UNC / schemes.
    if ((p.size() >= 2) && (p.at(1) == u':'))
        return {};

    for (const QChar ch : p)
    {
        if ((ch.unicode() < 0x20) || (ch.unicode() == 0x7F))
            return {};
    }

    const QStringList segments = p.split(u'/');
    for (const QString &segment : segments)
    {
        if (segment.isEmpty() || (segment == u".") || (segment == u".."))
            return {};
    }

    // Unicode-equivalent names must compare equal; case is significant.
    return p.normalized(QString::NormalizationForm_C);
}

nonstd::expected<Hybrid::BindingResult, Hybrid::BindingFailure> Hybrid::bindFiles(const QList<TorrentFileEntry> &torrentFiles
        , const QList<ProviderFile> &providerFiles)
{
    if (providerFiles.isEmpty())
        return nonstd::make_unexpected(BindingFailure {BindingError::NoProviderFiles});

    QHash<QString, const ProviderFile *> providerByPath;
    providerByPath.reserve(providerFiles.size());
    for (const ProviderFile &file : providerFiles)
    {
        const QString key = normalizeRelativePath(file.path);
        if (key.isEmpty() || file.providerFileId.isEmpty() || (file.size < 0))
            return nonstd::make_unexpected(BindingFailure {BindingError::UnsafeProviderPath});
        if (providerByPath.contains(key))
            return nonstd::make_unexpected(BindingFailure {BindingError::DuplicateProviderPath});
        providerByPath.insert(key, &file);
    }

    // Providers differ on whether the torrent's root folder is part of the path. Try both layouts
    // and accept only an unambiguous winner. Never fall back to basename matching.
    const Attempt withRoot = attemptBinding(torrentFiles, providerByPath, false);
    const Attempt withoutRoot = attemptBinding(torrentFiles, providerByPath, true);

    const auto usable = [](const Attempt &a) { return !a.failure && (a.matched > 0); };

    if (usable(withRoot) && (!usable(withoutRoot) || (withRoot.matched >= withoutRoot.matched)))
        return withRoot.result;
    if (usable(withoutRoot))
        return withoutRoot.result;

    if (withRoot.failure)
        return nonstd::make_unexpected(*withRoot.failure);
    if (withoutRoot.failure)
        return nonstd::make_unexpected(*withoutRoot.failure);
    return nonstd::make_unexpected(BindingFailure {BindingError::NoMatchingLayout});
}

QString Hybrid::describe(const BindingError error)
{
    switch (error)
    {
    case BindingError::NoProviderFiles:
        return QCoreApplication::translate("Hybrid", "Provider returned no files");
    case BindingError::UnsafeProviderPath:
        return QCoreApplication::translate("Hybrid", "Provider returned an unsafe file path");
    case BindingError::DuplicateProviderPath:
        return QCoreApplication::translate("Hybrid", "Provider file list is ambiguous");
    case BindingError::SizeMismatch:
        return QCoreApplication::translate("Hybrid", "Provider file size does not match the torrent");
    case BindingError::NoMatchingLayout:
        return QCoreApplication::translate("Hybrid", "This debrid file does not match the torrent's file layout. No HTTP data will be used.");
    }
    return {};
}

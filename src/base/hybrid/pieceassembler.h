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

#include <memory>
#include <vector>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include "rangesource.h"

// Assemble one complete canonical torrent piece from provider byte ranges (spec 03 section 4
// "Assembly algorithm"). Output goes to libtorrent add_piece; this class never touches disk.

namespace Hybrid
{
    struct PieceSlice
    {
        int nativeFileIndex = -1;
        qint64 fileOffset = 0;
        qint64 length = 0;
        qint64 pieceOffset = 0;
        bool isPad = false;     // bytes defined by torrent metadata as zeros, never fetched

        friend bool operator==(const PieceSlice &, const PieceSlice &) = default;
    };

    struct FileSource
    {
        QUrl url;               // temporary signed link, memory only
        qint64 fileSize = -1;
    };

    struct PieceJob
    {
        int piece = -1;
        qint64 length = 0;
        quint64 epoch = 0;
        QList<PieceSlice> slices;
    };

    using PieceBuffer = std::shared_ptr<std::vector<char>>;

    // Validates that slices exactly tile [0, length) in order with positive lengths.
    bool slicesTilePiece(const QList<PieceSlice> &slices, qint64 length);

    class PieceAssembly final : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(PieceAssembly)

    public:
        PieceAssembly(QNetworkAccessManager *manager, PieceJob job, QHash<int, FileSource> sources
                , UrlPolicy policy, TokenBucket *bucket, RangeFetch::Clock clock, RangeTimeouts timeouts
                , QObject *parent = nullptr);

        const PieceJob &job() const;
        void start();
        void abort();

    signals:
        void completed(int piece, quint64 epoch, const Hybrid::PieceBuffer &buffer);
        void failed(int piece, quint64 epoch, const Hybrid::RangeError &error, int nativeFileIndex);
        void progressed(qint64 newBytes);

    private:
        void fetchNext();
        void finishFailure(const RangeError &error, int nativeFileIndex);

        QNetworkAccessManager *m_manager = nullptr;
        PieceJob m_job;
        QHash<int, FileSource> m_sources;
        UrlPolicy m_policy;
        TokenBucket *m_bucket = nullptr;
        RangeFetch::Clock m_clock;
        RangeTimeouts m_timeouts;

        PieceBuffer m_buffer;
        qsizetype m_nextSlice = 0;
        QPointer<RangeFetch> m_current;
        bool m_done = false;
    };
}

Q_DECLARE_METATYPE(Hybrid::PieceBuffer)

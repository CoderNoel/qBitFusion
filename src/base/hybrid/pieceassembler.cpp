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

#include "pieceassembler.h"

#include <cstring>

#include "base/global.h"

using namespace Qt::Literals::StringLiterals;

bool Hybrid::slicesTilePiece(const QList<PieceSlice> &slices, const qint64 length)
{
    if ((length <= 0) || slices.isEmpty())
        return false;

    qint64 cursor = 0;
    for (const PieceSlice &slice : slices)
    {
        if ((slice.length <= 0) || (slice.fileOffset < 0) || (slice.pieceOffset != cursor))
            return false;
        if (slice.length > (length - cursor))
            return false;
        cursor += slice.length;
    }
    return cursor == length;
}

Hybrid::PieceAssembly::PieceAssembly(QNetworkAccessManager *manager, PieceJob job, QHash<int, FileSource> sources
        , UrlPolicy policy, TokenBucket *bucket, RangeFetch::Clock clock, RangeTimeouts timeouts, QObject *parent)
    : QObject(parent)
    , m_manager {manager}
    , m_job {std::move(job)}
    , m_sources {std::move(sources)}
    , m_policy {std::move(policy)}
    , m_bucket {bucket}
    , m_clock {std::move(clock)}
    , m_timeouts {timeouts}
{
}

const Hybrid::PieceJob &Hybrid::PieceAssembly::job() const
{
    return m_job;
}

void Hybrid::PieceAssembly::start()
{
    if (!slicesTilePiece(m_job.slices, m_job.length))
    {
        finishFailure({.category = FailureCategory::BadResponse, .detail = u"piece slices do not tile the piece"_s}, -1);
        return;
    }

    for (const PieceSlice &slice : asConst(m_job.slices))
    {
        if (slice.isPad)
            continue;
        const auto it = m_sources.constFind(slice.nativeFileIndex);
        if ((it == m_sources.cend()) || (it->fileSize < 0) || ((slice.fileOffset + slice.length) > it->fileSize))
        {
            finishFailure({.category = FailureCategory::BadResponse, .detail = u"no exact provider binding for slice"_s}, slice.nativeFileIndex);
            return;
        }
    }

    // Bounded: one piece-sized allocation, accounted by the caller's memory budget before start().
    m_buffer = std::make_shared<std::vector<char>>(static_cast<std::size_t>(m_job.length), '\0');
    fetchNext();
}

void Hybrid::PieceAssembly::abort()
{
    if (m_done)
        return;
    m_done = true;
    if (m_current)
    {
        m_current->abort();
        m_current->deleteLater();
    }
    m_buffer.reset();
}

void Hybrid::PieceAssembly::finishFailure(const RangeError &error, const int nativeFileIndex)
{
    if (m_done)
        return;
    m_done = true;
    m_buffer.reset();
    emit failed(m_job.piece, m_job.epoch, error, nativeFileIndex);
}

void Hybrid::PieceAssembly::fetchNext()
{
    if (m_done)
        return;

    // Pad slices are already zero (buffer is zero-initialized); skip them.
    while ((m_nextSlice < m_job.slices.size()) && m_job.slices.at(m_nextSlice).isPad)
        ++m_nextSlice;

    if (m_nextSlice >= m_job.slices.size())
    {
        m_done = true;
        emit completed(m_job.piece, m_job.epoch, m_buffer);
        m_buffer.reset();
        return;
    }

    const PieceSlice slice = m_job.slices.at(m_nextSlice);
    const FileSource source = m_sources.value(slice.nativeFileIndex);

    RangeRequest request;
    request.url = source.url;
    request.range = ByteRange {slice.fileOffset, slice.fileOffset + slice.length - 1};
    request.expectedTotal = source.fileSize;
    request.timeouts = m_timeouts;

    auto *fetch = new RangeFetch(m_manager, request, m_policy, m_bucket, m_clock, this);
    m_current = fetch;
    connect(fetch, &RangeFetch::progressed, this, &PieceAssembly::progressed);
    connect(fetch, &RangeFetch::failed, this, [this, fetch, fileIndex = slice.nativeFileIndex](const RangeError &error)
    {
        fetch->deleteLater();
        finishFailure(error, fileIndex);
    });
    connect(fetch, &RangeFetch::succeeded, this, [this, fetch, slice](const QByteArray &data)
    {
        fetch->deleteLater();
        if (m_done)
            return;
        if (data.size() != slice.length)
        {
            finishFailure({.category = FailureCategory::BadResponse, .verdict = RangeVerdict::LengthMismatch
                    , .detail = u"slice length mismatch"_s}, slice.nativeFileIndex);
            return;
        }
        std::memcpy(m_buffer->data() + slice.pieceOffset, data.constData(), static_cast<std::size_t>(data.size()));
        ++m_nextSlice;
        fetchNext();
    });
    fetch->start();
}

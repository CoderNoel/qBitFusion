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

#include <optional>

#include <QtTypes>
#include <QByteArray>
#include <QString>

#include "hybridtypes.h"

// Strict HTTP Range response semantics (spec 03 section 7, spec 05 "HTTP client").
// Pure functions: no I/O, no Qt network types, fuzzable.

namespace Hybrid
{
    struct ByteRange
    {
        qint64 first = 0;   // inclusive
        qint64 last = 0;    // inclusive

        qint64 length() const { return last - first + 1; }
        friend bool operator==(const ByteRange &, const ByteRange &) = default;
    };

    struct ContentRange
    {
        qint64 first = 0;
        qint64 last = 0;
        qint64 total = -1;  // -1 when the server sent "*" (unknown total); rejected by validateRangeResponse

        friend bool operator==(const ContentRange &, const ContentRange &) = default;
    };

    // Parses "bytes <first>-<last>/<total|*>". Rejects whitespace tricks, signs, overflow,
    // multiple ranges, first > last and last >= total. 64-bit throughout.
    std::optional<ContentRange> parseContentRange(QByteArrayView header);

    // Formats "bytes=<first>-<last>" for a request.
    QByteArray makeRangeHeader(const ByteRange &range);

    // Parses Retry-After as delta-seconds (HTTP-date is treated as unknown -> std::nullopt).
    std::optional<qint64> parseRetryAfterSeconds(QByteArrayView header);

    struct RangeResponseHead
    {
        int httpStatus = 0;
        QByteArray contentRange;     // raw header, empty when absent
        QByteArray contentEncoding;  // raw header, empty when absent
        QByteArray contentType;      // raw header, empty when absent
        qint64 contentLength = -1;   // -1 when absent
    };

    enum class RangeVerdict
    {
        Accept,
        ServerIgnoredRange,      // 200 to a Range request: unsafe, MVP rejects
        NotSatisfiable,          // 416
        MissingContentRange,
        MalformedContentRange,
        WrongRange,              // offsets differ from the request
        TotalSizeMismatch,       // provider file size changed / wrong file
        TransformedBody,         // Content-Encoding other than identity
        MultipartBody,           // multipart/byteranges surprise
        LengthMismatch,          // Content-Length disagrees with range
        HttpError                // any other status; classify with classifyHttpStatus
    };

    // Short plain-text summary of a server's error page, safe for logs: markup is dropped and every
    // word that could be a link, path, query, token or network address is replaced. Empty if nothing
    // readable remains. The input is untrusted.
    QString summarizeErrorPage(const QByteArray &body);

    // Validates response headers for a single-range request before any body byte is trusted.
    RangeVerdict validateRangeResponse(const RangeResponseHead &head, const ByteRange &requested, qint64 expectedTotal);

    // Maps HTTP status (and verdicts) to a coordinator failure category (spec 04 table).
    FailureCategory classifyHttpStatus(int httpStatus, bool linkAlreadyRefreshed);
    FailureCategory classifyVerdict(RangeVerdict verdict);
}

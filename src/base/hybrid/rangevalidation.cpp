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

#include "rangevalidation.h"

#include <limits>

#include <QRegularExpression>

#include "base/global.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    // Parses a non-empty run of ASCII digits into a non-negative qint64 without overflow.
    std::optional<qint64> parseUnsigned(const QByteArrayView digits)
    {
        if (digits.isEmpty() || (digits.size() > 19))
            return std::nullopt;

        qint64 value = 0;
        for (const char ch : digits)
        {
            if ((ch < '0') || (ch > '9'))
                return std::nullopt;
            const int digit = ch - '0';
            if (value > ((std::numeric_limits<qint64>::max() - digit) / 10))
                return std::nullopt;
            value = (value * 10) + digit;
        }
        return value;
    }

    bool equalsIgnoreCase(const QByteArrayView a, const QByteArrayView b)
    {
        return (a.compare(b, Qt::CaseInsensitive) == 0);
    }
}

std::optional<Hybrid::ContentRange> Hybrid::parseContentRange(const QByteArrayView header)
{
    // RFC 9110 14.4: Content-Range = range-unit SP ( range-resp / unsatisfied-range )
    // We only accept: "bytes" SP first "-" last "/" ( total / "*" )
    const QByteArrayView prefix = "bytes ";
    if ((header.size() <= prefix.size()) || !equalsIgnoreCase(header.first(prefix.size()), prefix))
        return std::nullopt;

    const QByteArrayView spec = header.sliced(prefix.size());
    const qsizetype dash = spec.indexOf('-');
    const qsizetype slash = spec.indexOf('/');
    if ((dash <= 0) || (slash <= (dash + 1)) || (slash == (spec.size() - 1)))
        return std::nullopt;
    if (spec.indexOf('-', dash + 1) != -1 || spec.indexOf('/', slash + 1) != -1 || spec.contains(','))
        return std::nullopt;

    const std::optional<qint64> first = parseUnsigned(spec.first(dash));
    const std::optional<qint64> last = parseUnsigned(spec.sliced(dash + 1, (slash - dash - 1)));
    if (!first || !last || (*first > *last))
        return std::nullopt;

    const QByteArrayView totalPart = spec.sliced(slash + 1);
    qint64 total = -1;
    if (totalPart != "*")
    {
        const std::optional<qint64> parsedTotal = parseUnsigned(totalPart);
        if (!parsedTotal || (*last >= *parsedTotal))
            return std::nullopt;
        total = *parsedTotal;
    }

    return ContentRange {.first = *first, .last = *last, .total = total};
}

QByteArray Hybrid::makeRangeHeader(const ByteRange &range)
{
    return "bytes=" + QByteArray::number(range.first) + '-' + QByteArray::number(range.last);
}

std::optional<qint64> Hybrid::parseRetryAfterSeconds(const QByteArrayView header)
{
    const QByteArrayView trimmed = header.trimmed();
    const std::optional<qint64> seconds = parseUnsigned(trimmed);
    if (!seconds)
        return std::nullopt;
    // Clamp absurd values; a day is plenty for backoff purposes.
    return std::min<qint64>(*seconds, 24 * 60 * 60);
}

QString Hybrid::summarizeErrorPage(const QByteArray &body)
{
    QString text = QString::fromLatin1(body.left(8192));

    static const QRegularExpression blocks {u"<(script|style|head)\\b.*?</\\1\\s*>"_s
            , (QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption)};
    static const QRegularExpression tags {u"<[^>]*>"_s};
    // Anything shaped like a URL, path, query string, e-mail, long identifier or IP address.
    static const QRegularExpression risky {u"\\S*(?:[/\\\\@%=&?#]|\\d{1,3}(?:\\.\\d{1,3}){3}|[0-9A-Fa-f]*:[0-9A-Fa-f:]{2,}|[0-9A-Za-z_\\-]{16,})\\S*"_s};
    text.remove(blocks);
    text.replace(tags, u" "_s);
    text.replace(risky, u"[…]"_s);

    QString clean;
    clean.reserve(text.size());
    for (const QChar ch : asConst(text))
        clean.append(((ch.unicode() >= 0x20) && (ch.unicode() < 0x7F)) || (ch == u'…') ? ch : u' ');
    clean = clean.simplified();
    if (clean.size() > 200)
        clean = clean.left(200) + u'…';
    return clean;
}

Hybrid::RangeVerdict Hybrid::validateRangeResponse(const RangeResponseHead &head, const ByteRange &requested, const qint64 expectedTotal)
{
    if (head.httpStatus == 200)
        return RangeVerdict::ServerIgnoredRange;
    if (head.httpStatus == 416)
        return RangeVerdict::NotSatisfiable;
    if (head.httpStatus != 206)
        return RangeVerdict::HttpError;

    if (!head.contentEncoding.isEmpty() && !equalsIgnoreCase(head.contentEncoding.trimmed(), "identity"))
        return RangeVerdict::TransformedBody;

    if (head.contentType.trimmed().toLower().startsWith("multipart/"))
        return RangeVerdict::MultipartBody;

    if (head.contentRange.isEmpty())
        return RangeVerdict::MissingContentRange;

    const std::optional<ContentRange> range = parseContentRange(head.contentRange);
    if (!range || (range->total < 0))
        return RangeVerdict::MalformedContentRange;

    if ((range->first != requested.first) || (range->last != requested.last))
        return RangeVerdict::WrongRange;

    if (range->total != expectedTotal)
        return RangeVerdict::TotalSizeMismatch;

    if ((head.contentLength >= 0) && (head.contentLength != requested.length()))
        return RangeVerdict::LengthMismatch;

    return RangeVerdict::Accept;
}

Hybrid::FailureCategory Hybrid::classifyHttpStatus(const int httpStatus, const bool linkAlreadyRefreshed)
{
    if ((httpStatus >= 200) && (httpStatus < 300))
        return FailureCategory::None;

    switch (httpStatus)
    {
    case 401:
        return FailureCategory::Auth;
    case 403:
    case 451:
        return FailureCategory::Permission;
    case 404:
    case 410:
        return linkAlreadyRefreshed ? FailureCategory::Gone : FailureCategory::LinkExpired;
    case 408:
        return FailureCategory::Temporary;
    case 416:
        return FailureCategory::BadResponse;
    case 429:
        return FailureCategory::RateLimit;
    default:
        break;
    }

    if (httpStatus >= 500)
        return FailureCategory::Temporary;
    if (httpStatus == 0)
        return FailureCategory::Temporary; // transport-level failure (timeout, TLS, reset)
    return FailureCategory::BadResponse;
}

Hybrid::FailureCategory Hybrid::classifyVerdict(const RangeVerdict verdict)
{
    switch (verdict)
    {
    case RangeVerdict::Accept:
        return FailureCategory::None;
    case RangeVerdict::HttpError:
        return FailureCategory::Temporary; // caller should prefer classifyHttpStatus
    default:
        return FailureCategory::BadResponse;
    }
}

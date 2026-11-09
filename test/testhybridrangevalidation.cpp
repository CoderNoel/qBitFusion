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

#include <limits>

#include <QTest>

#include "base/hybrid/rangevalidation.h"

using namespace Hybrid;

class TestHybridRangeValidation final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestHybridRangeValidation)

public:
    TestHybridRangeValidation() = default;

private slots:
    void parseValid() const
    {
        QCOMPARE(parseContentRange("bytes 0-0/1"), (ContentRange {0, 0, 1}));
        QCOMPARE(parseContentRange("bytes 100-199/1000"), (ContentRange {100, 199, 1000}));
        QCOMPARE(parseContentRange("BYTES 1-2/3"), (ContentRange {1, 2, 3}));
        QCOMPARE(parseContentRange("bytes 5-9/*"), (ContentRange {5, 9, -1}));
    }

    void parse64Bit() const
    {
        // T09: offsets beyond 4 GiB must not truncate.
        const auto r = parseContentRange("bytes 5368709120-5368725503/10737418240");
        QVERIFY(r.has_value());
        QCOMPARE(r->first, Q_INT64_C(5368709120));
        QCOMPARE(r->last, Q_INT64_C(5368725503));
        QCOMPARE(r->total, Q_INT64_C(10737418240));
        QVERIFY(parseContentRange("bytes 0-9223372036854775806/9223372036854775807").has_value());
    }

    void parseRejectsMalformed_data() const
    {
        QTest::addColumn<QByteArray>("header");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("no unit") << QByteArray("0-1/2");
        QTest::newRow("wrong unit") << QByteArray("items 0-1/2");
        QTest::newRow("unsatisfied") << QByteArray("bytes */100");
        QTest::newRow("first>last") << QByteArray("bytes 9-5/10");
        QTest::newRow("last>=total") << QByteArray("bytes 0-10/10");
        QTest::newRow("negative") << QByteArray("bytes -1-5/10");
        QTest::newRow("plus sign") << QByteArray("bytes +1-5/10");
        QTest::newRow("spaces") << QByteArray("bytes 1 -5/10");
        QTest::newRow("multi") << QByteArray("bytes 0-1,3-4/10");
        QTest::newRow("double slash") << QByteArray("bytes 0-1/10/20");
        QTest::newRow("trailing junk") << QByteArray("bytes 0-1/10x");
        QTest::newRow("overflow") << QByteArray("bytes 0-1/99999999999999999999");
        QTest::newRow("hex") << QByteArray("bytes 0x0-0x1/16");
        QTest::newRow("missing total") << QByteArray("bytes 0-1/");
        QTest::newRow("missing last") << QByteArray("bytes 0-/10");
    }

    void parseRejectsMalformed() const
    {
        QFETCH(QByteArray, header);
        QVERIFY(!parseContentRange(header).has_value());
    }

    void fuzzNeverCrashesOrAcceptsGarbage() const
    {
        // Cheap deterministic fuzz: random printable mutations of a valid header.
        const QByteArray base = "bytes 1024-2047/4096";
        quint32 seed = 12345;
        for (int i = 0; i < 20000; ++i)
        {
            QByteArray h = base;
            const int edits = 1 + (i % 4);
            for (int e = 0; e < edits; ++e)
            {
                seed = (seed * 1103515245u) + 12345u;
                const int pos = static_cast<int>(seed % static_cast<quint32>(h.size()));
                seed = (seed * 1103515245u) + 12345u;
                h[pos] = static_cast<char>(32 + (seed % 95));
            }
            const auto r = parseContentRange(h);
            if (r)
            {
                QVERIFY(r->first <= r->last);
                QVERIFY((r->total < 0) || (r->last < r->total));
            }
        }
    }

    void rangeHeader() const
    {
        QCOMPARE(makeRangeHeader({0, 16383}), QByteArray("bytes=0-16383"));
        QCOMPARE(makeRangeHeader({Q_INT64_C(5000000000), Q_INT64_C(5000016383)}), QByteArray("bytes=5000000000-5000016383"));
    }

    void retryAfter() const
    {
        QCOMPARE(parseRetryAfterSeconds("120"), std::optional<qint64>(120));
        QCOMPARE(parseRetryAfterSeconds(" 7 "), std::optional<qint64>(7));
        QCOMPARE(parseRetryAfterSeconds("999999999"), std::optional<qint64>(86400));
        QVERIFY(!parseRetryAfterSeconds("Wed, 21 Oct 2015 07:28:00 GMT").has_value());
        QVERIFY(!parseRetryAfterSeconds("-5").has_value());
    }

    void validateAccepts() const
    {
        RangeResponseHead head {206, "bytes 100-199/1000", {}, "application/octet-stream", 100};
        QCOMPARE(validateRangeResponse(head, {100, 199}, 1000), RangeVerdict::Accept);
        head.contentEncoding = "identity";
        QCOMPARE(validateRangeResponse(head, {100, 199}, 1000), RangeVerdict::Accept);
        head.contentLength = -1;
        QCOMPARE(validateRangeResponse(head, {100, 199}, 1000), RangeVerdict::Accept);
    }

    void validateRejects() const
    {
        const ByteRange req {100, 199};
        // Scenario F: poisoned responses.
        QCOMPARE(validateRangeResponse({200, {}, {}, {}, 1000}, req, 1000), RangeVerdict::ServerIgnoredRange);
        QCOMPARE(validateRangeResponse({416, {}, {}, {}, -1}, req, 1000), RangeVerdict::NotSatisfiable);
        QCOMPARE(validateRangeResponse({503, {}, {}, {}, -1}, req, 1000), RangeVerdict::HttpError);
        QCOMPARE(validateRangeResponse({206, {}, {}, {}, 100}, req, 1000), RangeVerdict::MissingContentRange);
        QCOMPARE(validateRangeResponse({206, "bytes 100-199/*", {}, {}, 100}, req, 1000), RangeVerdict::MalformedContentRange);
        QCOMPARE(validateRangeResponse({206, "bytes 101-200/1000", {}, {}, 100}, req, 1000), RangeVerdict::WrongRange);
        QCOMPARE(validateRangeResponse({206, "bytes 100-199/1001", {}, {}, 100}, req, 1000), RangeVerdict::TotalSizeMismatch);
        QCOMPARE(validateRangeResponse({206, "bytes 100-199/1000", "gzip", {}, 100}, req, 1000), RangeVerdict::TransformedBody);
        QCOMPARE(validateRangeResponse({206, "bytes 100-199/1000", "br", {}, 100}, req, 1000), RangeVerdict::TransformedBody);
        QCOMPARE(validateRangeResponse({206, "bytes 100-199/1000", {}, "multipart/byteranges; boundary=x", 100}, req, 1000), RangeVerdict::MultipartBody);
        QCOMPARE(validateRangeResponse({206, "bytes 100-199/1000", {}, {}, 99}, req, 1000), RangeVerdict::LengthMismatch);
    }

    void classify() const
    {
        QCOMPARE(classifyHttpStatus(206, false), FailureCategory::None);
        QCOMPARE(classifyHttpStatus(401, false), FailureCategory::Auth);
        QCOMPARE(classifyHttpStatus(403, false), FailureCategory::Permission);
        QCOMPARE(classifyHttpStatus(451, false), FailureCategory::Permission);
        QCOMPARE(classifyHttpStatus(404, false), FailureCategory::LinkExpired);
        QCOMPARE(classifyHttpStatus(404, true), FailureCategory::Gone);
        QCOMPARE(classifyHttpStatus(429, false), FailureCategory::RateLimit);
        QCOMPARE(classifyHttpStatus(503, false), FailureCategory::Temporary);
        QCOMPARE(classifyHttpStatus(0, false), FailureCategory::Temporary);
        QCOMPARE(classifyHttpStatus(400, false), FailureCategory::BadResponse);
        QCOMPARE(classifyVerdict(RangeVerdict::ServerIgnoredRange), FailureCategory::BadResponse);
        QCOMPARE(classifyVerdict(RangeVerdict::Accept), FailureCategory::None);
    }
};

QTEST_APPLESS_MAIN(TestHybridRangeValidation)
#include "testhybridrangevalidation.moc"

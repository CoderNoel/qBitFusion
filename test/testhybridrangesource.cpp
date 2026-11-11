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

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include "base/hybrid/pieceassembler.h"
#include "base/hybrid/rangesource.h"
#include "base/hybrid/tokenbucket.h"

using namespace Hybrid;
using namespace Qt::Literals::StringLiterals;

namespace
{
    QByteArray makeFile(const int size, const int seed)
    {
        QByteArray data(size, Qt::Uninitialized);
        quint32 s = static_cast<quint32>(seed);
        for (int i = 0; i < size; ++i)
        {
            s = (s * 1664525u) + 1013904223u;
            data[i] = static_cast<char>(s >> 24);
        }
        return data;
    }

    // Minimal HTTP/1.1 fixture server ("local mock CDN"). One behavior per path.
    class FixtureServer final : public QTcpServer
    {
    public:
        enum class Mode
        {
            Good,
            IgnoreRange200,
            WrongContentRange,
            Gzip,
            Truncate,
            TooLong,
            Unavailable503,
            RedirectPrivate,
            Stall
        };

        QHash<QString, QByteArray> files;
        Mode mode = Mode::Good;
        int requests = 0;

        explicit FixtureServer(QObject *parent = nullptr) : QTcpServer(parent)
        {
            connect(this, &QTcpServer::newConnection, this, [this]
            {
                while (QTcpSocket *s = nextPendingConnection())
                {
                    connect(s, &QTcpSocket::readyRead, s, [this, s] { handle(s); });
                    connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                }
            });
        }

        QUrl url(const QString &path) const
        {
            return QUrl(u"http://127.0.0.1:%1/%2"_s.arg(serverPort()).arg(path));
        }

    private:
        void handle(QTcpSocket *s)
        {
            QByteArray &buf = m_pending[s];
            buf += s->readAll();
            if (!buf.contains("\r\n\r\n"))
                return;
            ++requests;
            const QList<QByteArray> lines = buf.split('\n');
            const QByteArray path = lines.first().split(' ').value(1).mid(1);
            QByteArray range;
            for (const QByteArray &l : lines)
            {
                if (l.toLower().startsWith("range:"))
                    range = l.mid(6).trimmed();
            }
            m_pending.remove(s);

            const QByteArray file = files.value(QString::fromLatin1(path));
            const qsizetype dash = range.indexOf('-');
            const qint64 first = range.mid(6, dash - 6).toLongLong();
            const qint64 last = range.mid(dash + 1).toLongLong();
            QByteArray body = file.mid(first, last - first + 1);
            QByteArray head;
            switch (mode)
            {
            case Mode::Good:
            case Mode::Truncate:
            case Mode::TooLong:
            case Mode::Gzip:
            case Mode::WrongContentRange:
                {
                    const qint64 crFirst = (mode == Mode::WrongContentRange) ? first + 1 : first;
                    head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + QByteArray::number(crFirst) + '-'
                            + QByteArray::number(last) + '/' + QByteArray::number(file.size()) + "\r\n";
                    if (mode == Mode::Gzip)
                        head += "Content-Encoding: gzip\r\n";
                    if (mode == Mode::Truncate)
                    {
                        // Announce full length, send half, close.
                        head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
                        s->write(head + body.left(body.size() / 2));
                        s->flush();
                        s->disconnectFromHost();
                        return;
                    }
                    if (mode == Mode::TooLong)
                        body += "EXTRA-BYTES";
                    head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
                }
                break;
            case Mode::IgnoreRange200:
                body = file;
                head = "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
                break;
            case Mode::Unavailable503:
                body = "busy";
                head = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 42\r\nContent-Length: 4\r\n";
                break;
            case Mode::RedirectPrivate:
                body.clear();
                head = "HTTP/1.1 302 Found\r\nLocation: http://192.168.1.1/secret\r\nContent-Length: 0\r\n";
                break;
            case Mode::Stall:
                return; // never answer
            }
            s->write(head + "Connection: close\r\n\r\n" + body);
            s->flush();
            s->disconnectFromHost();
        }

        QHash<QTcpSocket *, QByteArray> m_pending;
    };

    qint64 nowMs()
    {
        return QDateTime::currentMSecsSinceEpoch();
    }
}

class TestHybridRangeSource final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestHybridRangeSource)

public:
    TestHybridRangeSource() = default;

private slots:
    void initTestCase()
    {
        qRegisterMetaType<Hybrid::RangeError>();
        qRegisterMetaType<Hybrid::PieceBuffer>();
        m_server.files.insert(u"a.bin"_s, makeFile(100'000, 1));
        m_server.files.insert(u"b.bin"_s, makeFile(70'001, 2));
        QVERIFY(m_server.listen(QHostAddress::LocalHost));
    }

    void init()
    {
        m_server.mode = FixtureServer::Mode::Good;
    }

    void goodRange()
    {
        const RangeRequest req {m_server.url(u"a.bin"_s), {1000, 17383}, 100'000, {}};
        RangeFetch f {&m_network, req, m_policy, nullptr, nowMs};
        QSignalSpy ok {&f, &RangeFetch::succeeded};
        QSignalSpy bad {&f, &RangeFetch::failed};
        f.start();
        QVERIFY(ok.wait(5000));
        QCOMPARE(bad.count(), 0);
        QCOMPARE(ok.first().first().toByteArray(), m_server.files.value(u"a.bin"_s).mid(1000, 16384));
    }

    void poisoned_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("category");
        QTest::newRow("200 ignores range") << static_cast<int>(FixtureServer::Mode::IgnoreRange200) << static_cast<int>(FailureCategory::BadResponse);
        QTest::newRow("wrong content-range") << static_cast<int>(FixtureServer::Mode::WrongContentRange) << static_cast<int>(FailureCategory::BadResponse);
        QTest::newRow("gzip body") << static_cast<int>(FixtureServer::Mode::Gzip) << static_cast<int>(FailureCategory::BadResponse);
        QTest::newRow("truncated") << static_cast<int>(FixtureServer::Mode::Truncate) << -1;
        QTest::newRow("too long") << static_cast<int>(FixtureServer::Mode::TooLong) << static_cast<int>(FailureCategory::BadResponse);
        QTest::newRow("503") << static_cast<int>(FixtureServer::Mode::Unavailable503) << static_cast<int>(FailureCategory::Temporary);
        QTest::newRow("redirect to LAN") << static_cast<int>(FixtureServer::Mode::RedirectPrivate) << static_cast<int>(FailureCategory::BadResponse);
    }

    void poisoned()
    {
        QFETCH(int, mode);
        QFETCH(int, category);
        m_server.mode = static_cast<FixtureServer::Mode>(mode);
        const RangeRequest req {m_server.url(u"a.bin"_s), {0, 16383}, 100'000, {}};
        RangeFetch f {&m_network, req, m_policy, nullptr, nowMs};
        QSignalSpy ok {&f, &RangeFetch::succeeded};
        QSignalSpy bad {&f, &RangeFetch::failed};
        f.start();
        QVERIFY(bad.wait(5000));
        QCOMPARE(ok.count(), 0); // no bytes ever reach the assembler
        const auto error = bad.first().first().value<RangeError>();
        if (category >= 0)
            QCOMPARE(static_cast<int>(error.category), category);
        QVERIFY(!error.detail.contains(u"a.bin"_s)); // path/query never in diagnostics
        if (m_server.mode == FixtureServer::Mode::Unavailable503)
            QCOMPARE(error.retryAfterSeconds, std::optional<qint64>(42));
    }

    void inactivityTimeout()
    {
        m_server.mode = FixtureServer::Mode::Stall;
        RangeRequest req {m_server.url(u"a.bin"_s), {0, 99}, 100'000, {}};
        req.timeouts = {.inactivityMs = 300, .totalMs = 5000};
        RangeFetch f {&m_network, req, m_policy, nullptr, nowMs};
        QSignalSpy bad {&f, &RangeFetch::failed};
        f.start();
        QVERIFY(bad.wait(3000));
        QCOMPARE(bad.first().first().value<RangeError>().category, FailureCategory::Temporary);
    }

    void refusesNonTestPolicy()
    {
        // Production policy refuses plain-http loopback outright, before any socket is opened.
        const int before = m_server.requests;
        RangeFetch f {&m_network, {m_server.url(u"a.bin"_s), {0, 99}, 100'000, {}}, UrlPolicy {}, nullptr, nowMs};
        QSignalSpy bad {&f, &RangeFetch::failed};
        f.start();
        QCOMPARE(bad.count(), 1);
        QCOMPARE(m_server.requests, before);
    }

    void throttled()
    {
        TokenBucket bucket {200'000, 16 * 1024};
        RangeFetch f {&m_network, {m_server.url(u"a.bin"_s), {0, 99'999}, 100'000, {}}, m_policy, &bucket, nowMs};
        QSignalSpy ok {&f, &RangeFetch::succeeded};
        QElapsedTimer t;
        t.start();
        f.start();
        QVERIFY(ok.wait(5000));
        // 100 kB at 200 kB/s with 16 kB burst: comfortably over 300 ms.
        QVERIFY2(t.elapsed() >= 300, qPrintable(QString::number(t.elapsed())));
    }

    void assembleCrossFilePieceWithPad()
    {
        // Piece spanning the end of a.bin, a 3-byte pad and the start of b.bin.
        PieceJob job;
        job.piece = 6;
        job.epoch = 9;
        job.length = 16'384;
        job.slices = {
            {0, 99'000, 1000, 0, false},
            {1, 0, 3, 1000, true},
            {2, 0, 15'381, 1003, false}
        };
        QHash<int, FileSource> sources {
            {0, {m_server.url(u"a.bin"_s), 100'000}},
            {2, {m_server.url(u"b.bin"_s), 70'001}}
        };
        PieceAssembly a {&m_network, job, sources, m_policy, nullptr, nowMs, {}};
        QSignalSpy done {&a, &PieceAssembly::completed};
        a.start();
        QVERIFY(done.wait(5000));
        QCOMPARE(done.first().at(0).toInt(), 6);
        QCOMPARE(done.first().at(1).toULongLong(), 9ULL);
        const auto buffer = done.first().at(2).value<PieceBuffer>();
        const QByteArray expected = m_server.files.value(u"a.bin"_s).mid(99'000, 1000) + QByteArray(3, '\0')
                + m_server.files.value(u"b.bin"_s).left(15'381);
        QCOMPARE(QByteArray(buffer->data(), static_cast<qsizetype>(buffer->size())), expected);
    }

    void assemblyRejectsUnboundOrGappySlices()
    {
        PieceJob gappy {1, 100, 1, {{0, 0, 40, 0, false}, {0, 50, 60, 50, false}}};
        QVERIFY(!slicesTilePiece(gappy.slices, gappy.length));
        PieceAssembly a {&m_network, gappy, {{0, {m_server.url(u"a.bin"_s), 100'000}}}, m_policy, nullptr, nowMs, {}};
        QSignalSpy bad {&a, &PieceAssembly::failed};
        a.start();
        QCOMPARE(bad.count(), 1);

        // Slice in a file the provider does not expose (T05 boundary) -> fail, do not guess.
        PieceJob unbound {2, 100, 1, {{5, 0, 100, 0, false}}};
        PieceAssembly b {&m_network, unbound, {}, m_policy, nullptr, nowMs, {}};
        QSignalSpy bad2 {&b, &PieceAssembly::failed};
        b.start();
        QCOMPARE(bad2.count(), 1);
        QCOMPARE(bad2.first().at(3).toInt(), 5);
    }

private:
    FixtureServer m_server;
    QNetworkAccessManager m_network;
    const UrlPolicy m_policy {.allowLoopbackForTesting = true};
};

QTEST_MAIN(TestHybridRangeSource)
#include "testhybridrangesource.moc"

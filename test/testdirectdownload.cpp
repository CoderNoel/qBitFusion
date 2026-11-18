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

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "base/directdownload/directdownloadmanager.h"
#include "base/directdownload/httptransfer.h"
#include "base/directdownload/linkclassifier.h"
#include "base/hybrid/securestore.h"
#include "base/hybrid/tokenbucket.h"
#include "base/hybrid/urlguard.h"

using namespace DirectDownload;
using namespace Qt::Literals::StringLiterals;

namespace
{
    const QString SECRET = u"SECRETKEY0123456789"_s;

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

    QByteArray readFile(const QString &path)
    {
        QFile file {path};
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    void writeFile(const QString &path, const QByteArray &data)
    {
        QFile file {path};
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(data), data.size());
    }

    qint64 nowMs()
    {
        return QDateTime::currentMSecsSinceEpoch();
    }

    // A small HTTP/1.1 server with one behavior per path ("a local CDN").
    class FixtureServer final : public QTcpServer
    {
    public:
        struct Route
        {
            QByteArray body;
            QByteArray contentType = "application/octet-stream";
            QByteArray disposition;
            QByteArray etag = "\"v1\"";
            int status = 0;                 // non-zero: answer with this status and a small page
            QByteArray location;            // for redirects
            bool supportsRange = true;
            bool gzip = false;
            qint64 truncateAt = -1;         // close the connection once this absolute offset is sent (once)
            int slowChunk = 0;              // send this many bytes every 10 ms
        };

        struct Seen
        {
            QByteArray path;
            QByteArray range;
            QByteArray ifRange;
        };

        QHash<QByteArray, Route> routes;
        QList<Seen> seen;

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
            const QList<QByteArray> lines = buf.split('\n');
            m_pending.remove(s);

            Seen request;
            request.path = lines.first().split(' ').value(1).mid(1);
            request.path = request.path.left(request.path.indexOf('?') < 0 ? request.path.size() : request.path.indexOf('?'));
            for (const QByteArray &line : lines)
            {
                const QByteArray lower = line.toLower();
                if (lower.startsWith("range:"))
                    request.range = line.mid(6).trimmed();
                else if (lower.startsWith("if-range:"))
                    request.ifRange = line.mid(9).trimmed();
            }
            seen.append(request);

            if (!routes.contains(request.path))
            {
                send(s, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\n", "not found");
                return;
            }
            Route &route = routes[request.path];

            if (!route.location.isEmpty())
            {
                send(s, "HTTP/1.1 302 Found\r\nLocation: " + route.location + "\r\nContent-Length: 0\r\n", {});
                return;
            }
            if (route.status != 0)
            {
                const QByteArray page = "<html><body>Refused for this account</body></html>";
                send(s, "HTTP/1.1 " + QByteArray::number(route.status) + " Refused\r\nContent-Type: text/html\r\nContent-Length: "
                        + QByteArray::number(page.size()) + "\r\n", page);
                return;
            }

            const QByteArray &file = route.body;
            qint64 first = 0;
            bool partial = false;
            if (route.supportsRange && request.range.startsWith("bytes=") && request.range.endsWith('-'))
            {
                const bool validatorOk = request.ifRange.isEmpty() || (request.ifRange == route.etag);
                if (validatorOk)
                {
                    first = request.range.mid(6, request.range.size() - 7).toLongLong();
                    partial = true;
                }
            }

            QByteArray head;
            if (partial && (first >= file.size()))
            {
                send(s, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + QByteArray::number(file.size())
                        + "\r\nContent-Length: 3\r\n", "416");
                return;
            }
            const QByteArray body = file.mid(first);
            if (partial)
            {
                head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + QByteArray::number(first) + '-'
                        + QByteArray::number(file.size() - 1) + '/' + QByteArray::number(file.size()) + "\r\n";
            }
            else
            {
                head = "HTTP/1.1 200 OK\r\n";
            }
            head += "Content-Type: " + route.contentType + "\r\nETag: " + route.etag + "\r\nAccept-Ranges: "
                    + (route.supportsRange ? "bytes" : "none") + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
            if (!route.disposition.isEmpty())
                head += "Content-Disposition: " + route.disposition + "\r\n";
            if (route.gzip)
                head += "Content-Encoding: gzip\r\n";

            if ((route.truncateAt >= 0) && (route.truncateAt > first))
            {
                const qint64 cut = route.truncateAt - first;
                route.truncateAt = -1;
                s->write(head + "Connection: close\r\n\r\n" + body.left(cut));
                s->flush();
                s->disconnectFromHost();
                return;
            }
            if (route.slowChunk > 0)
            {
                s->write(head + "Connection: close\r\n\r\n");
                auto *timer = new QTimer(s);
                auto offset = std::make_shared<qint64>(0);
                const int chunk = route.slowChunk;
                connect(timer, &QTimer::timeout, s, [s, timer, offset, body, chunk]
                {
                    if (s->state() != QAbstractSocket::ConnectedState)
                    {
                        timer->stop();
                        return;
                    }
                    s->write(body.mid(*offset, chunk));
                    *offset += chunk;
                    if (*offset >= body.size())
                    {
                        timer->stop();
                        s->flush();
                        s->disconnectFromHost();
                    }
                });
                timer->start(10);
                return;
            }
            send(s, head, body);
        }

        void send(QTcpSocket *s, const QByteArray &head, const QByteArray &body)
        {
            s->write(head + "Connection: close\r\n\r\n" + body);
            s->flush();
            s->disconnectFromHost();
        }

        QHash<QTcpSocket *, QByteArray> m_pending;
    };
}

class TestDirectDownload final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestDirectDownload)

public:
    TestDirectDownload() = default;

private slots:
    void initTestCase()
    {
        qRegisterMetaType<DirectDownload::Failure>();
        qRegisterMetaType<DirectDownload::ProbeResult>();
        qRegisterMetaType<DirectDownload::TransferHead>();
        QVERIFY(m_server.listen(QHostAddress::LocalHost));
        QVERIFY(m_dir.isValid());
    }

    void init()
    {
        m_server.routes.clear();
        m_server.seen.clear();
    }

    // ------------------------------------------------------------------ classification

    void classifyInput()
    {
        QCOMPARE(DirectDownload::classifyInput(u"  "_s), InputKind::Empty);
        QCOMPARE(DirectDownload::classifyInput(u"magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"_s), InputKind::Magnet);
        QCOMPARE(DirectDownload::classifyInput(u"0123456789abcdef0123456789ABCDEF01234567"_s), InputKind::InfoHash);
        QCOMPARE(DirectDownload::classifyInput(u"https://example.org/file.torrent"_s), InputKind::TorrentFileUrl);
        QCOMPARE(DirectDownload::classifyInput(u"https://example.org/FILE.TORRENT?x=1"_s), InputKind::TorrentFileUrl);
        QCOMPARE(DirectDownload::classifyInput(u"https://torrentio.example/realdebrid/%1/abc/null/0/Movie.mkv"_s.arg(SECRET)), InputKind::WebUrl);
        QCOMPARE(DirectDownload::classifyInput(u"http://tracker.example/download.php?id=5"_s), InputKind::WebUrl);
        QCOMPARE(DirectDownload::classifyInput(u"C:\\files\\a.torrent"_s), InputKind::Other);
        QCOMPARE(DirectDownload::classifyInput(u"file:///C:/a.torrent"_s), InputKind::Other);
        QCOMPARE(DirectDownload::classifyInput(u"ftp://example.org/a.iso"_s), InputKind::Other);
    }

    void classifyContent()
    {
        const QByteArray torrent = "d8:announce35:udp://tracker.example:1337/announce4:infod4:name3:abcee";
        QVERIFY(looksLikeTorrent(torrent));
        QVERIFY(looksLikeTorrent("d4:info"));
        QVERIFY(!looksLikeTorrent("de"));
        QVERIFY(!looksLikeTorrent("d0:x"));
        QVERIFY(!looksLikeTorrent("dummy text"));
        QVERIFY(!looksLikeTorrent("\x1A\x45\xDF\xA3 matroska"));

        ResponseHead head;
        head.contentType = "application/octet-stream";
        QCOMPARE(DirectDownload::classifyContent(head, torrent), ContentKind::Torrent);  // content wins
        QCOMPARE(DirectDownload::classifyContent(head, "\x1A\x45\xDF\xA3\x01\x00"), ContentKind::File);

        head.contentType = "video/x-matroska";
        QCOMPARE(DirectDownload::classifyContent(head, "\x1A\x45\xDF\xA3\x01\x00"), ContentKind::File);

        head.contentType = "application/x-bittorrent; charset=binary";
        QCOMPARE(DirectDownload::classifyContent(head, "garbage"), ContentKind::Torrent);

        head.contentType = "text/html; charset=utf-8";
        QCOMPARE(DirectDownload::classifyContent(head, "<!DOCTYPE html><html>"), ContentKind::WebPage);
        head.contentType = "application/octet-stream";
        QCOMPARE(DirectDownload::classifyContent(head, "\xEF\xBB\xBF  <html lang=en>"), ContentKind::WebPage);
        head.contentType = "text/html";
        head.contentDisposition = "attachment; filename=\"page.html\"";
        QCOMPARE(DirectDownload::classifyContent(head, "<html>"), ContentKind::File);
    }

    void contentDisposition_data()
    {
        QTest::addColumn<QByteArray>("header");
        QTest::addColumn<QString>("expected");

        QTest::newRow("quoted") << QByteArray("attachment; filename=\"Movie 2024.mkv\"") << u"Movie 2024.mkv"_s;
        QTest::newRow("token") << QByteArray("attachment; filename=Movie.mkv") << u"Movie.mkv"_s;
        QTest::newRow("ext utf-8") << QByteArray("attachment; filename*=UTF-8''%E2%82%AC%20rates.txt") << u"\u20AC rates.txt"_s;
        QTest::newRow("ext wins") << QByteArray("attachment; filename=\"fallback.txt\"; filename*=utf-8''real.txt") << u"real.txt"_s;
        QTest::newRow("raw utf-8") << QByteArray("attachment; filename=\"\xC3\xA9t\xC3\xA9.mp4\"") << u"\u00E9t\u00E9.mp4"_s;
        QTest::newRow("escaped quote") << QByteArray("attachment; filename=\"a\\\"b.txt\"") << u"a\"b.txt"_s;
        QTest::newRow("traversal") << QByteArray("attachment; filename=\"../../Windows/evil.exe\"") << u"evil.exe"_s;
        QTest::newRow("backslash") << QByteArray("attachment; filename=\"..\\\\..\\\\evil.dll\"") << u"evil.dll"_s;
        QTest::newRow("inline") << QByteArray("inline") << QString();
        QTest::newRow("empty") << QByteArray() << QString();
        QTest::newRow("spaces") << QByteArray("attachment ;  filename = \"x.bin\" ; size=3") << u"x.bin"_s;
    }

    void contentDisposition()
    {
        QFETCH(QByteArray, header);
        QFETCH(QString, expected);
        QCOMPARE(fileNameFromContentDisposition(header), expected);
    }

    void fileNames()
    {
        QCOMPARE(fileNameFromUrl(QUrl(u"https://x.download.example.com/d/ABC123/Some.Movie.2024.mkv"_s)), u"Some.Movie.2024.mkv"_s);
        QCOMPARE(fileNameFromUrl(QUrl(u"https://x.example.com/d/My%20File.mp4?token=abc"_s)), u"My File.mp4"_s);
        // A bare last segment is often a download token: never used as a name.
        QCOMPARE(fileNameFromUrl(QUrl(u"https://x.example.com/d/%1"_s.arg(SECRET))), QString());
        QCOMPARE(fileNameFromUrl(QUrl(u"https://x.example.com/"_s)), QString());

        QCOMPARE(sanitizeFileName(u"a<b>c:d\"e|f?g*h.txt"_s), u"a_b_c_d_e_f_g_h.txt"_s);
        QCOMPARE(sanitizeFileName(u"name. . "_s), u"name"_s);
        QCOMPARE(sanitizeFileName(u"CON.txt"_s), u"_CON.txt"_s);
        QCOMPARE(sanitizeFileName(u"lpt1"_s), u"_lpt1"_s);
        QCOMPARE(sanitizeFileName(u"..."_s), u"download"_s);
        QCOMPARE(sanitizeFileName(u"tab\there.bin"_s), u"tab_here.bin"_s);
        const QString longName = QString(400, u'x') + u".mkv"_s;
        const QString shortened = sanitizeFileName(longName);
        QVERIFY(shortened.size() <= 180);
        QVERIFY(shortened.endsWith(u".mkv"_s));

        QCOMPARE(chooseFileName("attachment; filename=\"a.mkv\"", QUrl(u"https://h.example/b.mp4"_s), "video/mp4"), u"a.mkv"_s);
        QCOMPARE(chooseFileName({}, QUrl(u"https://h.example/b.mp4"_s), "video/mp4"), u"b.mp4"_s);
        QCOMPARE(chooseFileName({}, QUrl(u"https://h.example/%1"_s.arg(SECRET)), "video/mp4"), u"download.mp4"_s);
        QCOMPARE(chooseFileName({}, QUrl(u"https://h.example/x"_s), "application/octet-stream"), u"download"_s);

        const QStringList taken {u"a.mkv"_s, u"a (1).mkv"_s};
        QCOMPARE(uniqueFileName(u"a.mkv"_s, [&](const QString &n) { return taken.contains(n); }), u"a (2).mkv"_s);
        QCOMPARE(uniqueFileName(u"b.mkv"_s, [&](const QString &n) { return taken.contains(n); }), u"b.mkv"_s);
    }

    void linkPolicy()
    {
        QCOMPARE(checkLink(QUrl(u"https://example.org/a"_s)), LinkCheck::Allowed);
        QCOMPARE(checkLink(QUrl(u"http://127.0.0.1:11470/a"_s)), LinkCheck::Allowed);  // the user chose it
        QCOMPARE(checkLink(QUrl(u"ftp://example.org/a"_s)), LinkCheck::SchemeNotAllowed);
        QCOMPARE(checkLink(QUrl(u"/relative"_s)), LinkCheck::InvalidUrl);

        const QUrl pub {u"https://addon.example.org/resolve/x"_s};
        QCOMPARE(checkRedirect(pub, QUrl(u"https://cdn.example.net/d/file.mkv"_s)), LinkCheck::Allowed);
        QCOMPARE(checkRedirect(pub, QUrl(u"/other/path"_s)), LinkCheck::Allowed);
        QCOMPARE(checkRedirect(pub, QUrl(u"http://cdn.example.net/file"_s)), LinkCheck::SchemeDowngrade);
        QCOMPARE(checkRedirect(pub, QUrl(u"https://127.0.0.1/admin"_s)), LinkCheck::RedirectToLocalNetwork);
        QCOMPARE(checkRedirect(pub, QUrl(u"https://192.168.1.1/admin"_s)), LinkCheck::RedirectToLocalNetwork);
        QCOMPARE(checkRedirect(pub, QUrl(u"https://[::1]/admin"_s)), LinkCheck::RedirectToLocalNetwork);
        QCOMPARE(checkRedirect(pub, QUrl(u"https://router/admin"_s)), LinkCheck::RedirectToLocalNetwork);
        QCOMPARE(checkRedirect(pub, QUrl(u"https://nas.local/admin"_s)), LinkCheck::RedirectToLocalNetwork);
        QCOMPARE(checkRedirect(pub, QUrl(u"ftp://example.org/x"_s)), LinkCheck::SchemeNotAllowed);
        // Inside the local network a redirect may stay there.
        QCOMPARE(checkRedirect(QUrl(u"http://127.0.0.1:11470/a"_s), QUrl(u"http://127.0.0.1:11470/b"_s)), LinkCheck::Allowed);
    }

    void redaction()
    {
        const QUrl addon {u"https://torrentio.example/realdebrid/%1/abcdef/null/0/Movie.mkv"_s.arg(SECRET)};
        const QString redacted = Hybrid::redactUrl(addon);
        QVERIFY(!redacted.contains(SECRET));
        QVERIFY(!redacted.contains(u"Movie"_s));
        QCOMPARE(redacted, u"https://torrentio.example/…"_s);

        const QUrl signedLink {u"https://user:%1@cdn.example.net/d/X?sig=%1"_s.arg(SECRET)};
        QVERIFY(!Hybrid::redactUrl(signedLink).contains(SECRET));
    }

    // ------------------------------------------------------------------ probe

    void probeFile()
    {
        FixtureServer::Route route;
        route.body = makeFile(300'000, 1);
        route.contentType = "video/x-matroska";
        route.disposition = "attachment; filename=\"Big Movie.mkv\"";
        m_server.routes.insert(SECRET.toLatin1() + "/resolve", {.location = "/cdn/" + SECRET.toLatin1()});
        m_server.routes.insert("cdn/" + SECRET.toLatin1(), route);

        const ProbeResult result = runProbe(m_server.url(SECRET + u"/resolve"_s));
        QCOMPARE(result.kind, ContentKind::File);
        QCOMPARE(result.fileName, u"Big Movie.mkv"_s);
        QCOMPARE(result.totalSize, qint64 {300'000});
        QVERIFY(result.rangeSupported);
        QCOMPARE(result.validator.etag, QByteArray("\"v1\""));
        QCOMPARE(result.finalUrl.path(), u"/cdn/"_s + SECRET);
        QCOMPARE(m_server.seen.first().range, QByteArray("bytes=0-"));
    }

    void probeTorrentWithoutExtension()
    {
        const QByteArray torrent = "d8:announce30:http://tracker.example/announce4:infod6:lengthi5e4:name5:a.txt12:piece lengthi16384e6:pieces20:"
                + QByteArray(20, 'p') + "ee";
        m_server.routes.insert("download.php", {.body = torrent, .contentType = "application/octet-stream"});

        const ProbeResult result = runProbe(m_server.url(u"download.php?id=7"_s));
        QCOMPARE(result.kind, ContentKind::Torrent);
        QCOMPARE(result.torrentData, torrent);
        QVERIFY(result.magnetUri.isEmpty());
    }

    void probeBigTorrent()
    {
        // Longer than the sniffed prefix: the probe keeps reading until the end.
        QByteArray torrent = "d7:comment" + QByteArray::number(200'000) + ':' + QByteArray(200'000, 'c') + "e";
        m_server.routes.insert("big", {.body = torrent, .contentType = "application/x-bittorrent"});
        const ProbeResult result = runProbe(m_server.url(u"big"_s));
        QCOMPARE(result.kind, ContentKind::Torrent);
        QCOMPARE(result.torrentData.size(), torrent.size());
    }

    void probeMagnetRedirect()
    {
        const QByteArray magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567";
        m_server.routes.insert("m", {.location = magnet});
        const ProbeResult result = runProbe(m_server.url(u"m"_s));
        QCOMPARE(result.kind, ContentKind::Torrent);
        QCOMPARE(result.magnetUri.toLatin1(), magnet);
    }

    void probeWebPage()
    {
        m_server.routes.insert("page", {.body = "<!doctype html><html><body>Sign in</body></html>", .contentType = "text/html"});
        const Failure failure = runProbeFailure(m_server.url(u"page"_s));
        QCOMPARE(failure.category, Hybrid::FailureCategory::BadResponse);
        QVERIFY(failure.detail.contains(u"web page"_s));
    }

    void probeRefusedKeepsSecretsOut()
    {
        m_server.routes.insert("api/" + SECRET.toLatin1() + "/file", {.status = 403});
        const Failure failure = runProbeFailure(m_server.url(u"api/%1/file?token=%1"_s.arg(SECRET)));
        QCOMPARE(failure.httpStatus, 403);
        QCOMPARE(failure.category, Hybrid::FailureCategory::Permission);
        QVERIFY2(!failure.detail.contains(SECRET), qPrintable(failure.detail));
        QVERIFY(failure.detail.contains(u"127.0.0.1"_s));
        QVERIFY(failure.detail.contains(u"Refused for this account"_s));
    }

    void probeRedirectToOtherSchemeRefused()
    {
        // The hop rules themselves are covered by linkPolicy; this checks that hops are checked at all.
        m_server.routes.insert("ftp", {.location = "ftp://example.org/x"});
        const Failure failure = runProbeFailure(m_server.url(u"ftp"_s));
        QCOMPARE(failure.category, Hybrid::FailureCategory::BadResponse);
        QVERIFY(failure.detail.contains(u"http"_s));
    }

    void probeNotFound()
    {
        const Failure failure = runProbeFailure(m_server.url(u"missing.bin"_s));
        QCOMPARE(failure.httpStatus, 404);
        QCOMPARE(failure.category, Hybrid::FailureCategory::LinkExpired);
    }

    // ------------------------------------------------------------------ transfer

    void transferWhole()
    {
        const QByteArray data = makeFile(1'000'000, 2);
        m_server.routes.insert("whole.bin", {.body = data});
        const QString path = m_dir.filePath(u"whole.bin.part"_s);
        QFile::remove(path);

        const auto [ok, head] = runTransfer({.url = m_server.url(u"whole.bin"_s), .filePath = path});
        QVERIFY(ok);
        QCOMPARE(head.total, data.size());
        QVERIFY(head.rangeSupported);
        QCOMPARE(readFile(path), data);
    }

    void transferResume()
    {
        const QByteArray data = makeFile(700'001, 3);
        m_server.routes.insert("resume.bin", {.body = data});
        const QString path = m_dir.filePath(u"resume.bin.part"_s);
        writeFile(path, data.left(250'000));

        const auto [ok, head] = runTransfer({.url = m_server.url(u"resume.bin"_s), .filePath = path, .offset = 250'000
                , .expectedTotal = data.size(), .validator = {.etag = "\"v1\""}});
        QVERIFY(ok);
        QVERIFY(!head.restartedFromZero);
        QCOMPARE(m_server.seen.last().range, QByteArray("bytes=250000-"));
        QCOMPARE(m_server.seen.last().ifRange, QByteArray("\"v1\""));
        QCOMPARE(readFile(path), data);
    }

    void transferServerIgnoresRange()
    {
        const QByteArray data = makeFile(400'000, 4);
        m_server.routes.insert("norange.bin", {.body = data, .supportsRange = false});
        const QString path = m_dir.filePath(u"norange.bin.part"_s);
        writeFile(path, QByteArray(100'000, 'x'));  // stale bytes that must not survive

        const auto [ok, head] = runTransfer({.url = m_server.url(u"norange.bin"_s), .filePath = path, .offset = 100'000
                , .expectedTotal = data.size()});
        QVERIFY(ok);
        QVERIFY(head.restartedFromZero);
        QCOMPARE(readFile(path), data);
    }

    void transferChangedFileRestartsWhole()
    {
        // If-Range with an old ETag: the server sends the new file whole.
        const QByteArray data = makeFile(300'000, 5);
        m_server.routes.insert("changed.bin", {.body = data, .etag = "\"v2\""});
        const QString path = m_dir.filePath(u"changed.bin.part"_s);
        writeFile(path, makeFile(300'000, 99).left(120'000));

        const auto [ok, head] = runTransfer({.url = m_server.url(u"changed.bin"_s), .filePath = path, .offset = 120'000
                , .expectedTotal = data.size(), .validator = {.etag = "\"v1\""}});
        QVERIFY(ok);
        QVERIFY(head.restartedFromZero);
        QCOMPARE(readFile(path), data);
    }

    void transferSizeChangedAsksForRestart()
    {
        const QByteArray data = makeFile(300'000, 6);
        m_server.routes.insert("size.bin", {.body = data});
        const QString path = m_dir.filePath(u"size.bin.part"_s);
        writeFile(path, data.left(1000));

        const Failure failure = runTransferFailure({.url = m_server.url(u"size.bin"_s), .filePath = path, .offset = 1000
                , .expectedTotal = 299'999});
        QVERIFY(failure.restartFromZero);
    }

    void transferTruncatedKeepsBytes()
    {
        const QByteArray data = makeFile(600'000, 7);
        m_server.routes.insert("cut.bin", {.body = data, .truncateAt = 200'000});
        const QString path = m_dir.filePath(u"cut.bin.part"_s);
        QFile::remove(path);

        const Failure failure = runTransferFailure({.url = m_server.url(u"cut.bin"_s), .filePath = path});
        QCOMPARE(failure.category, Hybrid::FailureCategory::Temporary);
        QCOMPARE(QFileInfo(path).size(), qint64 {200'000});
        QCOMPARE(readFile(path), data.left(200'000));

        // The next attempt continues where the first stopped.
        const auto [ok, head] = runTransfer({.url = m_server.url(u"cut.bin"_s), .filePath = path, .offset = 200'000
                , .expectedTotal = data.size(), .validator = {.etag = "\"v1\""}});
        QVERIFY(ok);
        QCOMPARE(m_server.seen.last().range, QByteArray("bytes=200000-"));
        QCOMPARE(readFile(path), data);
    }

    void transferAlreadyComplete()
    {
        const QByteArray data = makeFile(50'000, 8);
        m_server.routes.insert("done.bin", {.body = data});
        const QString path = m_dir.filePath(u"done.bin.part"_s);
        writeFile(path, data);
        const auto [ok, head] = runTransfer({.url = m_server.url(u"done.bin"_s), .filePath = path, .offset = data.size()
                , .expectedTotal = data.size()});
        QVERIFY(ok);
        QCOMPARE(readFile(path), data);
    }

    void transferRejectsCompressedBody()
    {
        m_server.routes.insert("gz.bin", {.body = makeFile(1000, 9), .gzip = true});
        const QString path = m_dir.filePath(u"gz.bin.part"_s);
        const Failure failure = runTransferFailure({.url = m_server.url(u"gz.bin"_s), .filePath = path});
        QCOMPARE(failure.category, Hybrid::FailureCategory::BadResponse);
    }

    void transferRespectsSpeedLimit()
    {
        const QByteArray data = makeFile(300'000, 10);
        m_server.routes.insert("limited.bin", {.body = data});
        const QString path = m_dir.filePath(u"limited.bin.part"_s);
        QFile::remove(path);

        Hybrid::TokenBucket bucket {200'000};
        QNetworkAccessManager network;
        Transfer transfer {&network, {.url = m_server.url(u"limited.bin"_s), .filePath = path}, &bucket, nowMs};
        QSignalSpy done {&transfer, &Transfer::succeeded};
        QElapsedTimer timer;
        timer.start();
        transfer.start();
        QVERIFY(done.wait(10'000));
        // 300 000 bytes at 200 000 B/s with a burst of one second: at least half a second.
        QVERIFY2(timer.elapsed() >= 400, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(readFile(path), data);
    }

    // ------------------------------------------------------------------ manager

    void managerDownloadsAndForgetsLink()
    {
        const QByteArray data = makeFile(500'000, 11);
        m_server.routes.insert("add/" + SECRET.toLatin1(), {.location = "/cdn/m.bin"});
        m_server.routes.insert("cdn/m.bin", {.body = data, .disposition = "attachment; filename=\"Show S01E01.mkv\""});

        const QString folder = m_dir.filePath(u"m1"_s);
        const QString stateFile = m_dir.filePath(u"m1-state.json"_s);
        Manager manager {testConfig(stateFile)};

        const QUrl link = m_server.url(u"add/%1"_s.arg(SECRET));
        const ProbeResult probe = runProbe(link);
        const QString id = manager.add(link, probe, Path(folder), probe.fileName);
        QSignalSpy finished {&manager, &Manager::itemFinished};
        QVERIFY(finished.wait(10'000));

        const ItemInfo info = *manager.info(id);
        QCOMPARE(info.state, State::Completed);
        QCOMPARE(info.name, u"Show S01E01.mkv"_s);
        QCOMPARE(readFile(folder + u"/Show S01E01.mkv"_s), data);
        QVERIFY(!QFile::exists(folder + u"/Show S01E01.mkv.part"_s));
        QCOMPARE(info.host, u"127.0.0.1"_s);
        QVERIFY(!Hybrid::SecureStore::read(m_secretPrefix + id));  // forgotten once complete

        const QByteArray state = readFile(stateFile);
        QVERIFY(state.contains("Show S01E01.mkv"));
        QVERIFY(!state.contains(SECRET.toLatin1()));
        QVERIFY(!state.contains("/cdn/"));
        manager.remove(id, false);
    }

    void managerNamesAreUnique()
    {
        const QByteArray data = makeFile(10'000, 12);
        m_server.routes.insert("same.bin", {.body = data});
        const QString folder = m_dir.filePath(u"m2"_s);
        QDir().mkpath(folder);
        writeFile(folder + u"/same.bin"_s, "existing");

        Manager manager {testConfig(m_dir.filePath(u"m2-state.json"_s))};
        const QUrl link = m_server.url(u"same.bin"_s);
        const ProbeResult probe = runProbe(link);
        const QString a = manager.add(link, probe, Path(folder), probe.fileName, true);
        const QString b = manager.add(link, probe, Path(folder), probe.fileName, true);
        QCOMPARE(manager.info(a)->name, u"same (1).bin"_s);
        QCOMPARE(manager.info(b)->name, u"same (2).bin"_s);
        manager.remove(a, true);
        manager.remove(b, true);
    }

    void managerPauseResume()
    {
        const QByteArray data = makeFile(2'000'000, 13);
        m_server.routes.insert("slow.bin", {.body = data, .slowChunk = 32 * 1024});
        const QString folder = m_dir.filePath(u"m3"_s);
        Manager manager {testConfig(m_dir.filePath(u"m3-state.json"_s))};

        const QUrl link = m_server.url(u"slow.bin"_s);
        const QString id = manager.add(link, runProbe(link), Path(folder), u"slow.bin"_s);
        QTRY_VERIFY_WITH_TIMEOUT(manager.info(id)->downloaded > 300'000, 10'000);
        manager.pause(id);
        QCOMPARE(manager.info(id)->state, State::Paused);
        const qint64 kept = manager.info(id)->downloaded;
        QCOMPARE(QFileInfo(folder + u"/slow.bin.part"_s).size(), kept);

        m_server.routes["slow.bin"].slowChunk = 0;
        manager.resume(id);
        QSignalSpy finished {&manager, &Manager::itemFinished};
        QVERIFY(finished.wait(10'000));
        QCOMPARE(readFile(folder + u"/slow.bin"_s), data);
        QCOMPARE(m_server.seen.last().range, "bytes=" + QByteArray::number(kept) + '-');
        manager.remove(id, true);
    }

    void managerResumesAfterRestart()
    {
        if (!Hybrid::SecureStore::isPersistent())
            QSKIP("No persistent credential store on this platform");

        const QByteArray data = makeFile(800'000, 14);
        m_server.routes.insert("restart.bin", {.body = data, .slowChunk = 32 * 1024});
        const QString folder = m_dir.filePath(u"m4"_s);
        const QString stateFile = m_dir.filePath(u"m4-state.json"_s);
        const QUrl link = m_server.url(u"restart.bin?sig=%1"_s.arg(SECRET));

        QString id;
        qint64 kept = 0;
        {
            Manager first {testConfig(stateFile)};
            id = first.add(link, runProbe(link), Path(folder), u"restart.bin"_s);
            QTRY_VERIFY_WITH_TIMEOUT(first.info(id)->downloaded > 200'000, 10'000);
            QVERIFY(first.info(id)->linkSaved);
        }  // "the app quits" mid-download
        kept = QFileInfo(folder + u"/restart.bin.part"_s).size();
        QVERIFY(kept > 0);
        QVERIFY(!readFile(stateFile).contains(SECRET.toLatin1()));

        m_server.routes["restart.bin"].slowChunk = 0;
        Manager second {testConfig(stateFile)};
        QVERIFY(second.ids().contains(id));
        QSignalSpy finished {&second, &Manager::itemFinished};
        QVERIFY(finished.wait(10'000));
        QCOMPARE(readFile(folder + u"/restart.bin"_s), data);
        QCOMPARE(m_server.seen.last().range, "bytes=" + QByteArray::number(kept) + '-');
        second.remove(id, true);
    }

    void managerAsksForLinkWhenNotKept()
    {
        // A state file whose link is not in the credential store: the item waits for a new link.
        const QString folder = m_dir.filePath(u"m5"_s);
        const QString stateFile = m_dir.filePath(u"m5-state.json"_s);
        const QByteArray data = makeFile(100'000, 15);
        QDir().mkpath(folder);
        writeFile(folder + u"/lost.bin.part"_s, data.left(40'000));
        const QJsonObject saved {{u"id"_s, u"lost-1"_s}, {u"name"_s, u"lost.bin"_s}, {u"savePath"_s, QDir::fromNativeSeparators(folder)}
                , {u"totalSize"_s, 100'000}, {u"state"_s, u"queued"_s}, {u"etag"_s, u"\"v1\""_s}};
        writeFile(stateFile, QJsonDocument(QJsonObject {{u"version"_s, 1}, {u"items"_s, QJsonArray {saved}}}).toJson());

        Manager manager {testConfig(stateFile)};
        QCOMPARE(manager.info(u"lost-1"_s)->state, State::NeedsLink);
        QCOMPARE(manager.info(u"lost-1"_s)->downloaded, qint64 {40'000});

        m_server.routes.insert("lost.bin", {.body = data});
        QVERIFY(manager.replaceLink(u"lost-1"_s, m_server.url(u"lost.bin"_s)));
        QSignalSpy finished {&manager, &Manager::itemFinished};
        QVERIFY(finished.wait(10'000));
        QCOMPARE(readFile(folder + u"/lost.bin"_s), data);
        QCOMPARE(m_server.seen.last().range, QByteArray("bytes=40000-"));
        manager.remove(u"lost-1"_s, true);
    }

    void managerRetriesAfterDrop()
    {
        const QByteArray data = makeFile(400'000, 16);
        m_server.routes.insert("drop.bin", {.body = data});
        const QString folder = m_dir.filePath(u"m6"_s);
        qint64 skew = 0;
        ManagerConfig config = testConfig(m_dir.filePath(u"m6-state.json"_s));
        config.clock = [&skew] { return nowMs() + skew; };
        Manager manager {config};

        const QUrl link = m_server.url(u"drop.bin"_s);
        const ProbeResult probe = runProbe(link);
        m_server.routes["drop.bin"].truncateAt = 150'000;  // after the probe: the download itself is cut
        const QString id = manager.add(link, probe, Path(folder), u"drop.bin"_s);
        QTRY_VERIFY_WITH_TIMEOUT(manager.info(id)->retryInSeconds > 0, 10'000);
        QCOMPARE(manager.info(id)->state, State::Queued);
        QVERIFY(!manager.info(id)->error.isEmpty());

        skew = 60'000;  // let the back-off pass
        QSignalSpy finished {&manager, &Manager::itemFinished};
        QVERIFY(finished.wait(10'000));
        QCOMPARE(readFile(folder + u"/drop.bin"_s), data);
        QCOMPARE(m_server.seen.last().range, QByteArray("bytes=150000-"));
        manager.remove(id, true);
    }

    void managerExpiredLinkFails()
    {
        m_server.routes.insert("exp.bin", {.body = makeFile(10'000, 17)});
        const QString folder = m_dir.filePath(u"m7"_s);
        Manager manager {testConfig(m_dir.filePath(u"m7-state.json"_s))};
        const QUrl link = m_server.url(u"exp.bin?token=%1"_s.arg(SECRET));
        const ProbeResult probe = runProbe(link);
        m_server.routes["exp.bin"].status = 403;  // the link expires between probe and download

        const QString id = manager.add(link, probe, Path(folder), u"exp.bin"_s);
        QTRY_COMPARE_WITH_TIMEOUT(manager.info(id)->state, State::Failed, 10'000);
        QVERIFY(manager.info(id)->error.contains(u"403"_s));
        QVERIFY(!manager.info(id)->error.contains(SECRET));
        manager.remove(id, true);
    }

private:
    ManagerConfig testConfig(const QString &stateFile) const
    {
        ManagerConfig config;
        config.stateFile = Path(stateFile);
        config.secretKeyPrefix = m_secretPrefix;
        config.useAppSettings = false;
        return config;
    }

    ProbeResult runProbe(const QUrl &url)
    {
        QNetworkAccessManager network;
        LinkProbe probe {&network, url, 10 * 1024 * 1024};
        QSignalSpy finished {&probe, &LinkProbe::finished};
        QSignalSpy failed {&probe, &LinkProbe::failed};
        probe.start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty() || !failed.isEmpty(), 10'000); }();
        if (!failed.isEmpty())
            qWarning() << "probe failed:" << failed.first().first().value<Failure>().detail;
        [&] { QCOMPARE(finished.size(), 1); }();
        return finished.isEmpty() ? ProbeResult {} : finished.first().first().value<ProbeResult>();
    }

    Failure runProbeFailure(const QUrl &url)
    {
        QNetworkAccessManager network;
        LinkProbe probe {&network, url, 10 * 1024 * 1024};
        QSignalSpy finished {&probe, &LinkProbe::finished};
        QSignalSpy failed {&probe, &LinkProbe::failed};
        probe.start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty() || !failed.isEmpty(), 10'000); }();
        [&] { QCOMPARE(failed.size(), 1); }();
        return failed.isEmpty() ? Failure {} : failed.first().first().value<Failure>();
    }

    std::pair<bool, TransferHead> runTransfer(const TransferRequest &request)
    {
        QNetworkAccessManager network;
        Transfer transfer {&network, request, nullptr, nowMs};
        QSignalSpy head {&transfer, &Transfer::responseStarted};
        QSignalSpy ok {&transfer, &Transfer::succeeded};
        QSignalSpy failed {&transfer, &Transfer::failed};
        transfer.start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!ok.isEmpty() || !failed.isEmpty(), 10'000); }();
        if (!failed.isEmpty())
            qWarning() << "transfer failed:" << failed.first().first().value<Failure>().detail;
        return {!ok.isEmpty(), head.isEmpty() ? TransferHead {} : head.first().first().value<TransferHead>()};
    }

    Failure runTransferFailure(const TransferRequest &request)
    {
        QNetworkAccessManager network;
        Transfer transfer {&network, request, nullptr, nowMs};
        QSignalSpy ok {&transfer, &Transfer::succeeded};
        QSignalSpy failed {&transfer, &Transfer::failed};
        transfer.start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!ok.isEmpty() || !failed.isEmpty(), 10'000); }();
        [&] { QCOMPARE(failed.size(), 1); }();
        return failed.isEmpty() ? Failure {} : failed.first().first().value<Failure>();
    }

    FixtureServer m_server;
    QTemporaryDir m_dir;
    const QString m_secretPrefix = u"qbf-test-directdownload/"_s;
};

QTEST_GUILESS_MAIN(TestDirectDownload)
#include "testdirectdownload.moc"

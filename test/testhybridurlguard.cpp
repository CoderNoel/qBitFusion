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

#include <QTest>

#include "base/hybrid/urlguard.h"

using namespace Hybrid;
using namespace Qt::Literals::StringLiterals;

class TestHybridUrlGuard final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestHybridUrlGuard)

public:
    TestHybridUrlGuard() = default;

private slots:
    void checkUrl_data() const
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<int>("expected");

        const auto row = [](const char *name, const QString &url, const UrlCheck c)
        {
            QTest::newRow(name) << url << static_cast<int>(c);
        };
        row("https public", u"https://cdn.example.com/file?token=x"_s, UrlCheck::Allowed);
        row("https public ip", u"https://8.8.8.8/f"_s, UrlCheck::Allowed);
        row("http", u"http://cdn.example.com/f"_s, UrlCheck::SchemeNotAllowed);
        row("file", u"file:///etc/passwd"_s, UrlCheck::InvalidUrl);
        row("ftp", u"ftp://cdn.example.com/f"_s, UrlCheck::SchemeNotAllowed);
        row("userinfo", u"https://user:pass@cdn.example.com/f"_s, UrlCheck::CredentialsInUrl);
        row("localhost", u"https://localhost/f"_s, UrlCheck::PrivateAddress);
        row("loopback v4", u"https://127.0.0.1/f"_s, UrlCheck::PrivateAddress);
        row("loopback v6", u"https://[::1]/f"_s, UrlCheck::PrivateAddress);
        row("rfc1918 10", u"https://10.1.2.3/f"_s, UrlCheck::PrivateAddress);
        row("rfc1918 192", u"https://192.168.1.1/f"_s, UrlCheck::PrivateAddress);
        row("rfc1918 172", u"https://172.16.0.1/f"_s, UrlCheck::PrivateAddress);
        row("metadata", u"https://169.254.169.254/latest"_s, UrlCheck::PrivateAddress);
        row("cgnat", u"https://100.64.1.1/f"_s, UrlCheck::PrivateAddress);
        row("v4 mapped", u"https://[::ffff:10.0.0.1]/f"_s, UrlCheck::PrivateAddress);
        row("ula v6", u"https://[fd00::1]/f"_s, UrlCheck::PrivateAddress);
        row("link-local v6", u"https://[fe80::1]/f"_s, UrlCheck::PrivateAddress);
        row("nat64", u"https://[64:ff9b::a00:1]/f"_s, UrlCheck::PrivateAddress);
        row("unspecified", u"https://0.0.0.0/f"_s, UrlCheck::PrivateAddress);
        row("single label", u"https://intranet/f"_s, UrlCheck::PrivateAddress);
        row(".local", u"https://nas.local/f"_s, UrlCheck::PrivateAddress);
        row("odd port", u"https://cdn.example.com:22/f"_s, UrlCheck::PortNotAllowed);
        row("relative", u"/just/a/path"_s, UrlCheck::InvalidUrl);
    }

    void checkUrl() const
    {
        QFETCH(QString, url);
        QFETCH(int, expected);
        QCOMPARE(static_cast<int>(Hybrid::checkUrl(QUrl(url), {})), expected);
    }

    void allowlist() const
    {
        const UrlPolicy policy {.allowedHostSuffixes = {u"real-debrid.com"_s, u"rdeb.io"_s}};
        QCOMPARE(Hybrid::checkUrl(QUrl(u"https://api.real-debrid.com/x"_s), policy), UrlCheck::Allowed);
        QCOMPARE(Hybrid::checkUrl(QUrl(u"https://real-debrid.com/x"_s), policy), UrlCheck::Allowed);
        QCOMPARE(Hybrid::checkUrl(QUrl(u"https://abc.rdeb.io/x"_s), policy), UrlCheck::Allowed);
        // Suffix confusion must not pass.
        QCOMPARE(Hybrid::checkUrl(QUrl(u"https://evilreal-debrid.com/x"_s), policy), UrlCheck::HostNotAllowed);
        QCOMPARE(Hybrid::checkUrl(QUrl(u"https://real-debrid.com.evil.net/x"_s), policy), UrlCheck::HostNotAllowed);
        QCOMPARE(Hybrid::checkUrl(QUrl(u"https://8.8.8.8/x"_s), policy), UrlCheck::HostNotAllowed);
    }

    void loopbackOnlyInTests() const
    {
        const UrlPolicy test {.allowLoopbackForTesting = true};
        QCOMPARE(Hybrid::checkUrl(QUrl(u"http://127.0.0.1:8080/f"_s), test), UrlCheck::Allowed);
        QCOMPARE(Hybrid::checkUrl(QUrl(u"http://localhost:8080/f"_s), test), UrlCheck::Allowed);
        // The test switch does not open other private ranges or plain http to the internet.
        QCOMPARE(Hybrid::checkUrl(QUrl(u"http://10.0.0.1/f"_s), test), UrlCheck::SchemeNotAllowed);
        QCOMPARE(Hybrid::checkUrl(QUrl(u"https://10.0.0.1/f"_s), test), UrlCheck::PrivateAddress);
    }

    void redirects() const
    {
        const QUrl from {u"https://cdn.example.com/a"_s};
        QCOMPARE(checkRedirect(from, QUrl(u"https://cdn2.example.com/b"_s), {}), UrlCheck::Allowed);
        QCOMPARE(checkRedirect(from, QUrl(u"/relative"_s), {}), UrlCheck::Allowed);
        QCOMPARE(checkRedirect(from, QUrl(u"http://cdn.example.com/b"_s), {}), UrlCheck::SchemeNotAllowed);
        QCOMPARE(checkRedirect(from, QUrl(u"https://192.168.0.1/b"_s), {}), UrlCheck::PrivateAddress);
        QCOMPARE(checkRedirect(from, QUrl(u"https://169.254.169.254/"_s), {}), UrlCheck::PrivateAddress);
    }

    void credentialsOnlyToApiHost() const
    {
        const QStringList api {u"api.real-debrid.com"_s};
        QVERIFY(mayCarryCredentials(QUrl(u"https://api.real-debrid.com/rest/1.0/user"_s), api));
        QVERIFY(!mayCarryCredentials(QUrl(u"https://download.real-debrid.com/d/x"_s), api));
        QVERIFY(!mayCarryCredentials(QUrl(u"http://api.real-debrid.com/x"_s), api));
    }

    void redaction() const
    {
        const QString r = redactUrl(QUrl(u"https://CDN.example.com/d/ABCSECRET/file.mkv?token=SECRET#frag"_s));
        QVERIFY(!r.contains(u"SECRET"_s));
        QVERIFY(!r.contains(u"token"_s));
        QVERIFY(r.startsWith(u"https://cdn.example.com/"_s));
    }
};

QTEST_APPLESS_MAIN(TestHybridUrlGuard)
#include "testhybridurlguard.moc"

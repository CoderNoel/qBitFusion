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

#include <filesystem>
#include <fstream>
#include <string>

#include <QTemporaryDir>
#include <QTest>

#include "base/global.h"
#include "base/selfupdate/applyupdate.h"
#include "base/selfupdate/updatemanifest.h"

using namespace SelfUpdate;
using namespace Qt::Literals::StringLiterals;
namespace fs = std::filesystem;

namespace
{
    // A throwaway test key, unrelated to the release signing key. The signature below was made with
    // its private half over exactly MANIFEST.
    const QByteArray TEST_PUBLIC_KEY = QByteArray::fromBase64("yMX/LsPkSsrzY1z4Tjj/JAKOFkRxSvZ3v1XOScHWMJs=");
    const QByteArray MANIFEST = R"({"format":1,"version":"5.3.1.1","upstream":"5.3.1","notes":"https://github.com/CoderNoel/qBitFusion/releases/tag/v5.3.1.1","windows-x64":{"url":"https://github.com/CoderNoel/qBitFusion/releases/download/v5.3.1.1/qBitFusion-5.3.1.1-windows-x64.zip","size":32596015,"sha256":"4d8eaa8c5e5cd0dcd3cc4fbde6547cd48cb2e0b43d868f97496630738e782f20"}})";
    const QByteArray SIGNATURE = "tvyvXPEoZgEpRe3pVlseWTJ0Nk+PCC3od0oaIpvL1G3khTZR7VCsU+Xbn965c9ykx6gCpV79lENekTyYfvjdAA==";
    const QStringList OFFICIAL {u"https://github.com/CoderNoel/qBitFusion/releases/download/"_s};

    void writeFile(const fs::path &path, const std::string &content)
    {
        fs::create_directories(path.parent_path());
        std::ofstream {path, std::ios::binary | std::ios::trunc} << content;
    }

    std::string readFile(const fs::path &path)
    {
        std::ifstream in {path, std::ios::binary};
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
}

class TestSelfUpdate final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestSelfUpdate)

public:
    TestSelfUpdate() = default;

private slots:
    void versionParsingAndOrder() const
    {
        QCOMPARE(Version::fromString(u"5.3.0.2"_s), (Version {5, 3, 0, 2}));
        QCOMPARE(Version::fromString(u" 5.10.12.300 "_s), (Version {5, 10, 12, 300}));
        QVERIFY(!Version::fromString(u"5.3.0"_s).isValid());
        QVERIFY(!Version::fromString(u"5.3.0.2-beta"_s).isValid());
        QVERIFY(!Version::fromString(u"v5.3.0.2"_s).isValid());
        QVERIFY(!Version::fromString(u""_s).isValid());

        QVERIFY((Version {5, 3, 0, 2}) > (Version {5, 3, 0, 1}));
        QVERIFY((Version {5, 3, 1, 1}) > (Version {5, 3, 0, 9}));     // a new upstream release resets the revision
        QVERIFY((Version {5, 10, 0, 1}) > (Version {5, 9, 9, 9}));    // numeric, not text, comparison
        QCOMPARE((Version {5, 3, 0, 2}).toString(), u"5.3.0.2"_s);
    }

    void signatureVerifies() const
    {
        QVERIFY(verifySignature(MANIFEST, SIGNATURE, TEST_PUBLIC_KEY));
    }

    void signatureRejectsTampering() const
    {
        QByteArray changed = MANIFEST;
        changed.replace("5.3.1.1\",\"upstream", "5.3.1.9\",\"upstream");
        QVERIFY(!verifySignature(changed, SIGNATURE, TEST_PUBLIC_KEY));
        QVERIFY(!verifySignature(MANIFEST + ' ', SIGNATURE, TEST_PUBLIC_KEY));

        QByteArray badSignature = QByteArray::fromBase64(SIGNATURE);
        badSignature[10] = static_cast<char>(badSignature[10] ^ 1);
        QVERIFY(!verifySignature(MANIFEST, badSignature.toBase64(), TEST_PUBLIC_KEY));

        QByteArray otherKey = TEST_PUBLIC_KEY;
        otherKey[0] = static_cast<char>(otherKey[0] ^ 1);
        QVERIFY(!verifySignature(MANIFEST, SIGNATURE, otherKey));

        QVERIFY(!verifySignature(MANIFEST, {}, TEST_PUBLIC_KEY));
        QVERIFY(!verifySignature(MANIFEST, "not base64 at all", TEST_PUBLIC_KEY));
        QVERIFY(!verifySignature(MANIFEST, SIGNATURE, TEST_PUBLIC_KEY.left(31)));
    }

    void manifestParses() const
    {
        const auto manifest = parseManifest(MANIFEST, u"windows-x64"_s, OFFICIAL);
        QVERIFY2(manifest, qPrintable(manifest ? QString() : manifest.error()));
        QCOMPARE(manifest->version, (Version {5, 3, 1, 1}));
        QCOMPARE(manifest->upstreamVersion, u"5.3.1"_s);
        QCOMPARE(manifest->package.size, 32596015);
        QCOMPARE(manifest->package.sha256.toHex(), QByteArray("4d8eaa8c5e5cd0dcd3cc4fbde6547cd48cb2e0b43d868f97496630738e782f20"));
    }

    void manifestRejects_data() const
    {
        QTest::addColumn<QByteArray>("json");
        const QByteArray good = MANIFEST;
        const auto with = [&good](const QByteArray &from, const QByteArray &to) { return QByteArray(good).replace(from, to); };

        QTest::newRow("not json") << QByteArray("{");
        QTest::newRow("format 2") << with("\"format\":1", "\"format\":2");
        QTest::newRow("revision 0") << with("\"version\":\"5.3.1.1\"", "\"version\":\"5.3.1.0\"");
        QTest::newRow("bad version") << with("\"version\":\"5.3.1.1\"", "\"version\":\"latest\"");
        QTest::newRow("other platform") << with("windows-x64", "linux-x64");
        QTest::newRow("foreign host") << with("https://github.com/CoderNoel/qBitFusion/releases/download/", "https://example.com/");
        QTest::newRow("other repository") << with("CoderNoel/qBitFusion/releases/download/", "someone/qBitFusion/releases/download/");
        QTest::newRow("plain http") << with("https://github.com/CoderNoel", "http://github.com/CoderNoel");
        QTest::newRow("dot dot") << with("/v5.3.1.1/qBitFusion", "/../../evil/qBitFusion");
        QTest::newRow("size zero") << with("\"size\":32596015", "\"size\":0");
        QTest::newRow("short hash") << with("4d8eaa8c5e5cd0dcd3cc4fbde6547cd48cb2e0b43d868f97496630738e782f20", "4d8eaa8c");
    }

    void manifestRejects() const
    {
        QFETCH(QByteArray, json);
        QVERIFY(!parseManifest(json, u"windows-x64"_s, OFFICIAL));
    }

    void windowsQuoting_data() const
    {
        QTest::addColumn<QString>("argument");
        QTest::addColumn<QString>("quoted");
        QTest::newRow("plain") << u"file.torrent"_s << u"file.torrent"_s;
        QTest::newRow("empty") << QString() << u"\"\""_s;
        QTest::newRow("space") << u"C:\\My Files\\a.torrent"_s << u"\"C:\\My Files\\a.torrent\""_s;
        QTest::newRow("trailing backslash") << u"C:\\My Dir\\"_s << u"\"C:\\My Dir\\\\\""_s;
        QTest::newRow("quote") << u"say \"hi\""_s << u"\"say \\\"hi\\\"\""_s;
        QTest::newRow("backslash before quote") << u"a\\\"b"_s << u"\"a\\\\\\\"b\""_s;
        QTest::newRow("magnet") << u"magnet:?xt=urn:btih:abc&dn=x"_s << u"magnet:?xt=urn:btih:abc&dn=x"_s;
    }

    void windowsQuoting() const
    {
        QFETCH(QString, argument);
        QFETCH(QString, quoted);
        QCOMPARE(quoteWindowsArgument(argument), quoted);
    }

    void applyReplacesFilesAndKeepsProfile() const
    {
        const QTemporaryDir root;
        QVERIFY(root.isValid());
        const fs::path base = root.path().toStdWString();
        const fs::path staged = base / L"staged";
        const fs::path install = base / L"install";

        writeFile(staged / L"qbitfusion.exe", "new exe");
        writeFile(staged / L"plugins" / L"style.dll", "new plugin");
        writeFile(staged / L"added.dll", "added");
        writeFile(staged / L"profile" / L"README.txt", "must not be copied");

        writeFile(install / L"qbitfusion.exe", "old exe");
        writeFile(install / L"plugins" / L"style.dll", "old plugin");
        writeFile(install / L"kept.dll", "untouched");
        writeFile(install / L"profile" / L"qBittorrent" / L"config" / L"qBittorrent.ini", "settings");

        const ApplyResult result = applyFiles(staged, install, install / L"update" / L"previous", {L"profile", L"update"});
        QVERIFY2(result.ok, qPrintable(QString::fromStdWString(result.error)));
        QCOMPARE(result.filesReplaced, 3);

        QCOMPARE(readFile(install / L"qbitfusion.exe"), "new exe");
        QCOMPARE(readFile(install / L"plugins" / L"style.dll"), "new plugin");
        QCOMPARE(readFile(install / L"added.dll"), "added");
        QCOMPARE(readFile(install / L"kept.dll"), "untouched");
        QCOMPARE(readFile(install / L"profile" / L"qBittorrent" / L"config" / L"qBittorrent.ini"), "settings");
        QVERIFY(!fs::exists(install / L"profile" / L"README.txt"));

        QCOMPARE(readFile(install / L"update" / L"previous" / L"qbitfusion.exe"), "old exe");
        QVERIFY(!fs::exists(install / L"qbitfusion.exe.qbfnew"));
    }

    void applyFailsCleanlyOnEmptyUpdate() const
    {
        const QTemporaryDir root;
        const fs::path base = root.path().toStdWString();
        fs::create_directories(base / L"staged" / L"profile");
        writeFile(base / L"install" / L"qbitfusion.exe", "old exe");

        const ApplyResult result = applyFiles(base / L"staged", base / L"install", base / L"install" / L"update" / L"previous", {L"profile"});
        QVERIFY(!result.ok);
        QCOMPARE(readFile(base / L"install" / L"qbitfusion.exe"), "old exe");
    }
};

QTEST_APPLESS_MAIN(TestSelfUpdate)
#include "testselfupdate.moc"

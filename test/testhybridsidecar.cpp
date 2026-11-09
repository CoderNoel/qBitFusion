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

#include <QFile>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTest>

#include "base/hybrid/hybridsidecar.h"

using namespace Hybrid;
using namespace Qt::Literals::StringLiterals;

namespace
{
    const QString ID = u"0123456789abcdef0123456789abcdef01234567"_s;
}

class TestHybridSidecar final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestHybridSidecar)

public:
    TestHybridSidecar() = default;

private slots:
    void roundTrip() const
    {
        SidecarData d;
        d.policy = TransportPolicy::DebridPreferred;
        d.p2pFallbackConsent = true;
        d.providerId = u"realdebrid"_s;
        d.providerResourceId = u"ABCDEF123"_s;
        d.lastReason = Reason::ProviderOutage;
        d.lastTransitionUtc = QDateTime::fromMSecsSinceEpoch(1'700'000'000'123, QTimeZone::UTC);
        d.switchCount = 3;
        const auto parsed = parseSidecar(serializeSidecar(d));
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed.value(), d);
    }

    void debridOnlyNeverCarriesConsent() const
    {
        const auto parsed = parseSidecar("{\"schema_version\":1,\"policy\":\"debrid_only\",\"p2p_fallback_consent\":true}");
        QVERIFY(parsed.has_value());
        QVERIFY(!parsed->p2pFallbackConsent);
    }

    void rejectsSecretsAndUrls_data() const
    {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("url value") << QByteArray("{\"schema_version\":1,\"policy\":\"debrid_only\",\"x\":\"https://cdn/x?token=1\"}");
        QTest::newRow("magnet") << QByteArray("{\"schema_version\":1,\"policy\":\"debrid_only\",\"x\":\"magnet:?xt=urn:btih:abc\"}");
        QTest::newRow("token key") << QByteArray("{\"schema_version\":1,\"policy\":\"debrid_only\",\"access_token\":\"abc\"}");
        QTest::newRow("url key") << QByteArray("{\"schema_version\":1,\"policy\":\"debrid_only\",\"download_url\":\"abc\"}");
        QTest::newRow("bad resource") << QByteArray("{\"schema_version\":1,\"policy\":\"debrid_only\",\"provider_resource_id\":\"../x\"}");
        QTest::newRow("no schema") << QByteArray("{\"policy\":\"debrid_only\"}");
        QTest::newRow("truncated") << QByteArray("{\"schema_version\":1,\"policy\":\"deb");
        QTest::newRow("array") << QByteArray("[]");
    }

    void rejectsSecretsAndUrls() const
    {
        QFETCH(QByteArray, json);
        QVERIFY(!parseSidecar(json).has_value());
    }

    void unknownPolicyFallsBackToUpstream() const
    {
        const auto parsed = parseSidecar("{\"schema_version\":7,\"policy\":\"quantum\",\"future\":1}");
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->policy, TransportPolicy::TorrentOnly);
    }

    void serializeDropsUnsafeIdentifiers() const
    {
        SidecarData d;
        d.providerResourceId = u"https://evil/x"_s;
        QVERIFY(!serializeSidecar(d).contains("evil"));
    }

    void storeAtomicIO() const
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const SidecarStore store {Path(dir.path()) / Path(u"hybrid"_s)};
        QVERIFY(!store.load(ID).has_value());

        SidecarData d;
        d.policy = TransportPolicy::DebridOnly;
        QVERIFY(store.save(ID, d).has_value());
        QCOMPARE(store.load(ID)->policy, TransportPolicy::DebridOnly);

        // A corrupt file (crash, manual edit) yields "no sidecar" -> safe defaults.
        QFile f {store.pathFor(ID).data()};
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{\"schema_ver");
        f.close();
        QVERIFY(!store.load(ID).has_value());

        QVERIFY(store.remove(ID));
        QVERIFY(!QFile::exists(store.pathFor(ID).data()));
    }

    void storeRejectsPathTricks() const
    {
        QTemporaryDir dir;
        const SidecarStore store {Path(dir.path())};
        QVERIFY(!store.save(u"../../etc"_s, {}).has_value());
        QVERIFY(!store.load(u"..\\x"_s).has_value());
    }
};

QTEST_APPLESS_MAIN(TestHybridSidecar)
#include "testhybridsidecar.moc"

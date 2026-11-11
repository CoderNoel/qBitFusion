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

// End-to-end coordinator tests against a REAL libtorrent session (no mocks of libtorrent).
// Phase P3/P4 evidence: provider bytes reach disk only through add_piece + libtorrent hashing,
// failover preserves verified pieces, and held torrents make zero swarm contact.

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#include <QDateTime>
#include <QFile>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "base/hybrid/hybridcoordinator.h"
#include "base/hybrid/ltlayout.h"
#include "base/hybrid/tokenbucket.h"
#include "base/hybrid/torrentpiecesink.h"

using namespace Hybrid;
using namespace Qt::Literals::StringLiterals;
namespace fs = std::filesystem;

namespace
{
    constexpr int PIECE = 16 * 1024;

    QByteArray makeData(const int size, quint32 seed)
    {
        QByteArray d(size, Qt::Uninitialized);
        for (int i = 0; i < size; ++i)
        {
            seed = (seed * 1664525u) + 1013904223u;
            d[i] = static_cast<char>(seed >> 24);
        }
        return d;
    }

    QByteArray readAll(const fs::path &p)
    {
        QFile f {QString::fromStdWString(p.wstring())};
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    // Counts inbound TCP connections: used as the torrent's tracker and as a known peer.
    class Listener final : public QTcpServer
    {
    public:
        int connections = 0;
        Listener()
        {
            connect(this, &QTcpServer::newConnection, this, [this]
            {
                while (QTcpSocket *s = nextPendingConnection())
                {
                    ++connections;
                    s->close();
                    s->deleteLater();
                }
            });
            listen(QHostAddress::LocalHost);
        }
    };

    // Local "CDN": serves torrent files by native index with strict 206 semantics.
    class Cdn final : public QTcpServer
    {
    public:
        enum class Mode { Good, CorruptFile, Unavailable };
        QHash<int, QByteArray> files;
        Mode mode = Mode::Good;
        int corruptFile = -1;
        int requests = 0;

        Cdn()
        {
            connect(this, &QTcpServer::newConnection, this, [this]
            {
                while (QTcpSocket *s = nextPendingConnection())
                {
                    connect(s, &QTcpSocket::readyRead, s, [this, s] { serve(s); });
                    connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                }
            });
            listen(QHostAddress::LocalHost);
        }

        QUrl url(const int index) const
        {
            return QUrl(u"http://127.0.0.1:%1/f%2?sig=SECRET"_s.arg(serverPort()).arg(index));
        }

    private:
        void serve(QTcpSocket *s)
        {
            QByteArray &buf = m_pending[s];
            buf += s->readAll();
            if (!buf.contains("\r\n\r\n"))
                return;
            ++requests;
            const QList<QByteArray> lines = buf.split('\n');
            m_pending.remove(s);
            const QByteArray target = lines.first().split(' ').value(1);
            const int index = target.mid(2, target.indexOf('?') - 2).toInt();
            QByteArray range;
            for (const QByteArray &l : lines)
            {
                if (l.toLower().startsWith("range:"))
                    range = l.mid(6).trimmed();
            }

            if (mode == Mode::Unavailable)
            {
                s->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                s->disconnectFromHost();
                return;
            }

            const QByteArray file = files.value(index);
            const qsizetype dash = range.indexOf('-');
            const qint64 first = range.mid(6, dash - 6).toLongLong();
            const qint64 last = range.mid(dash + 1).toLongLong();
            QByteArray body = file.mid(first, last - first + 1);
            if ((mode == Mode::CorruptFile) && (index == corruptFile) && !body.isEmpty())
                body[body.size() / 2] = static_cast<char>(body[body.size() / 2] ^ 0x5A);

            s->write("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + QByteArray::number(first) + '-' + QByteArray::number(last)
                    + '/' + QByteArray::number(file.size()) + "\r\nContent-Length: " + QByteArray::number(body.size())
                    + "\r\nConnection: close\r\n\r\n" + body);
            s->disconnectFromHost();
        }

        QHash<QTcpSocket *, QByteArray> m_pending;
    };

    class FixtureProvider final : public Provider
    {
    public:
        QList<ProviderFile> files;
        Cdn *cdn = nullptr;
        bool available = true;
        int manifestCalls = 0;

        QString id() const override { return u"fixture"_s; }
        QString displayName() const override { return u"Fixture"_s; }
        ProviderCapabilities capabilities() const override { return {.canCheckCachedWithoutMutation = true}; }
        UrlPolicy dataUrlPolicy() const override { return {.allowLoopbackForTesting = true}; }
        AccountState accountState() const override { return AccountState::Connected; }

        void checkAvailability(const TorrentIdentity &, QObject *context, ProviderCallback<Availability> cb) override
        {
            const ProviderResult<Availability> r = available ? ProviderResult<Availability>(Availability::Cached)
                    : nonstd::make_unexpected(ProviderError {FailureCategory::Temporary, u"down"_s, std::nullopt});
            QMetaObject::invokeMethod(context, [cb, r] { cb(r); }, Qt::QueuedConnection);
        }

        void resolveManifest(const TorrentIdentity &, bool, QObject *context, ProviderCallback<ProviderManifest> cb) override
        {
            ++manifestCalls;
            const ProviderResult<ProviderManifest> r = available
                    ? ProviderResult<ProviderManifest>(ProviderManifest {u"res1"_s, Availability::Cached, files})
                    : nonstd::make_unexpected(ProviderError {FailureCategory::Temporary, u"down"_s, std::nullopt});
            QMetaObject::invokeMethod(context, [cb, r] { cb(r); }, Qt::QueuedConnection);
        }

        void resolveFileLink(const QString &, const QString &fileId, QObject *context, ProviderCallback<TemporaryLink> cb) override
        {
            const int index = fileId.toInt();
            const ProviderResult<TemporaryLink> r = TemporaryLink {cdn->url(index), cdn->files.value(index).size()};
            QMetaObject::invokeMethod(context, [cb, r] { cb(r); }, Qt::QueuedConnection);
        }
    };

    // TorrentPieceSink over a raw libtorrent handle (the app uses TorrentImpl instead).
    class LtSink final : public TorrentPieceSink
    {
    public:
        lt::torrent_handle handle;
        std::shared_ptr<const lt::torrent_info> info;
        int holds = 0;
        int releases = 0;

        TorrentIdentity identity() const override { return nativeIdentity(info->info_hashes(), u"fixture"_s); }
        bool hasMetadata() const override { return true; }
        bool isChecking() const override { return handle.status({}).state == lt::torrent_status::checking_files; }
        int pieceCount() const override { return info->num_pieces(); }
        QList<TorrentFileEntry> nativeFiles() const override { return nativeFileEntries(*info); }
        QList<PieceSlice> pieceSlices(const int p) const override { return nativePieceSlices(*info, p); }
        qint64 pieceLength(const int p) const override { return nativePieceLength(*info, p); }

        QBitArray havePieces() const override
        {
            QBitArray b(info->num_pieces());
            for (int p = 0; p < info->num_pieces(); ++p)
                b.setBit(p, handle.have_piece(lt::piece_index_t {p}));
            return b;
        }

        QList<int> wantedMissingPieces() const override
        {
            QList<int> out;
            for (int p = 0; p < info->num_pieces(); ++p)
            {
                if (!handle.have_piece(lt::piece_index_t {p}))
                    out.append(p);
            }
            return out;
        }

        void setTransportHold(const bool hold) override
        {
            if (hold)
            {
                ++holds;
                handle.unset_flags(lt::torrent_flags::auto_managed);
                handle.pause(lt::torrent_handle::graceful_pause);
            }
            else
            {
                ++releases;
                handle.resume();
            }
        }

        bool isTransportHoldSettled() const override
        {
            const lt::torrent_status st = handle.status({});
            return bool(st.flags & lt::torrent_flags::paused) && (st.num_peers == 0);
        }

        void addPiece(const int piece, std::vector<char> &&data) override
        {
            handle.add_piece(lt::piece_index_t {piece}, std::move(data));
        }
    };

    lt::session_params isolated()
    {
        lt::settings_pack p;
        p.set_int(lt::settings_pack::alert_mask, lt::alert_category::all);
        p.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
        p.set_bool(lt::settings_pack::enable_dht, false);
        p.set_bool(lt::settings_pack::enable_lsd, false);
        p.set_bool(lt::settings_pack::enable_upnp, false);
        p.set_bool(lt::settings_pack::enable_natpmp, false);
        p.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
        return lt::session_params(std::move(p));
    }

    qint64 now()
    {
        return QDateTime::currentMSecsSinceEpoch();
    }
}

class TestHybridCoordinator final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestHybridCoordinator)

public:
    TestHybridCoordinator() = default;

private:
    struct Rig
    {
        QTemporaryDir dir;
        fs::path src;
        fs::path save;
        Cdn cdn;
        Listener tracker;
        Listener peer;
        FixtureProvider provider;
        std::unique_ptr<lt::session> ses;
        LtSink sink;
        QNetworkAccessManager network;
        TransferBudget budget {8, 4 * PIECE};
        TokenBucket bucket;
        std::unique_ptr<HybridCoordinator> coord;
        QTimer pump;
        bool finished = false;
        int hashFailures = 0;
    };

    void build(Rig &r, const lt::create_flags_t flags, const CoordinatorConfig &config)
    {
        QVERIFY(r.dir.isValid());
        r.src = fs::path(r.dir.path().toStdWString()) / "src";
        r.save = fs::path(r.dir.path().toStdWString()) / "save";
        const fs::path root = r.src / "Fixture";
        fs::create_directories(root / "sub");
        const QList<std::pair<std::string, int>> files {{"a.bin", 100'000}, {"sub/b.bin", 70'001}, {"c.bin", 130'000}};
        quint32 seed = 7;
        for (const auto &[name, size] : files)
        {
            const QByteArray data = makeData(size, seed++);
            std::ofstream(root / name, std::ios::binary).write(data.constData(), data.size());
        }

        lt::create_torrent ct(lt::list_files(root.string(), flags), PIECE, flags);
        ct.add_tracker("http://127.0.0.1:" + std::to_string(r.tracker.serverPort()) + "/announce");
        lt::set_piece_hashes(ct, r.src.string());
        const std::vector<char> buf = ct.generate_buf();
        r.sink.info = lt::load_torrent_buffer(lt::span<char const>(buf.data(), static_cast<std::ptrdiff_t>(buf.size()))).ti;

        // Provider manifest from the torrent's own layout (root-relative paths, as Real-Debrid does).
        r.provider.cdn = &r.cdn;
        for (const TorrentFileEntry &f : nativeFileEntries(*r.sink.info))
        {
            if (f.isPad)
                continue;
            r.cdn.files.insert(f.nativeIndex, readAll(r.src / f.path.toStdString()));
            r.provider.files.append({QString::number(f.nativeIndex), u"/"_s + f.path.section(u'/', 1), f.size});
        }

        r.ses = std::make_unique<lt::session>(isolated());
        lt::add_torrent_params atp;
        atp.ti = std::make_shared<lt::torrent_info>(*r.sink.info);
        atp.save_path = r.save.string();
        atp.flags |= lt::torrent_flags::paused;            // hold from the instant of creation
        atp.flags &= ~lt::torrent_flags::auto_managed;
        atp.peers.emplace_back(boost::asio::ip::make_address("127.0.0.1"), r.peer.serverPort());
        r.sink.handle = r.ses->add_torrent(atp);
        QTRY_VERIFY_WITH_TIMEOUT(r.sink.handle.status({}).state != lt::torrent_status::checking_resume_data
                && r.sink.handle.status({}).state != lt::torrent_status::checking_files, 10000);

        r.coord = std::make_unique<HybridCoordinator>(&r.sink, &r.provider, &r.network, &r.budget, &r.bucket, config, now);
        r.pump.setInterval(5);
        connect(&r.pump, &QTimer::timeout, r.coord.get(), [&r]
        {
            std::vector<lt::alert *> alerts;
            r.ses->pop_alerts(&alerts);
            for (lt::alert *a : alerts)
            {
                if (const auto *pf = lt::alert_cast<lt::piece_finished_alert>(a))
                    r.coord->notifyPieceFinished(static_cast<int>(pf->piece_index));
                else if (const auto *hf = lt::alert_cast<lt::hash_failed_alert>(a))
                {
                    ++r.hashFailures;
                    r.coord->notifyHashFailed(static_cast<int>(hf->piece_index));
                }
                else if (lt::alert_cast<lt::torrent_finished_alert>(a))
                {
                    r.finished = true;
                    r.coord->notifyFinished();
                }
                else if (lt::alert_cast<lt::torrent_paused_alert>(a))
                    r.coord->notifyHoldSettled();
            }
        });
        r.pump.start();
    }

    static CoordinatorConfig fastConfig()
    {
        CoordinatorConfig c;
        c.maxPiecesInFlight = 4;
        c.health.backoffInitialMs = 200;
        c.health.backoffMaxMs = 400;
        c.health.minDwellMs = 0;
        c.timeouts = {.inactivityMs = 2000, .totalMs = 5000};
        c.drainTimeoutMs = 2000;
        return c;
    }

    static bool outputMatches(Rig &r)
    {
        r.sink.handle.flush_cache();
        QTest::qWait(300);
        for (const TorrentFileEntry &f : nativeFileEntries(*r.sink.info))
        {
            if (!f.isPad && (readAll(r.src / f.path.toStdString()) != readAll(r.save / f.path.toStdString())))
                return false;
        }
        return true;
    }

    // Removes the torrent's first file from what the provider exposes, as when only some files
    // of a torrent were selected on the debrid service. Returns its native index.
    static int hideFirstProviderFile(Rig &r)
    {
        const int hidden = r.provider.files.first().providerFileId.toInt();
        r.provider.files.removeFirst();
        return hidden;
    }

    static int piecesNotTouching(const Rig &r, const int nativeIndex)
    {
        int count = 0;
        for (int p = 0; p < r.sink.info->num_pieces(); ++p)
        {
            const QList<PieceSlice> slices = nativePieceSlices(*r.sink.info, p);
            if (std::none_of(slices.cbegin(), slices.cend(), [nativeIndex](const PieceSlice &s) { return s.nativeFileIndex == nativeIndex; }))
                ++count;
        }
        return count;
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<Hybrid::RangeError>();
        qRegisterMetaType<Hybrid::PieceBuffer>();
    }

    void httpOnlyDebridOnly_data()
    {
        QTest::addColumn<int>("flags");
        QTest::newRow("v1") << static_cast<int>(static_cast<std::uint32_t>(lt::create_torrent::v1_only));
        QTest::newRow("hybrid v1+v2 with pads") << 0;
        QTest::newRow("v2") << static_cast<int>(static_cast<std::uint32_t>(lt::create_torrent::v2_only));
    }

    void httpOnlyDebridOnly()
    {
        // Scenario A: provider returns exact bytes, P2P forbidden.
        QFETCH(int, flags);
        Rig r;
        build(r, lt::create_flags_t(static_cast<std::uint32_t>(flags)), fastConfig());
        r.coord->start(TransportPolicy::DebridOnly, true /* must be ignored */);
        QVERIFY(r.coord->wantsTransportHold());

        QTRY_VERIFY_WITH_TIMEOUT(r.finished, 20000);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::Completed, 5000);
        QVERIFY(outputMatches(r));
        QCOMPARE(r.coord->status().httpVerifiedPieces, r.sink.info->num_pieces());
        QCOMPARE(r.hashFailures, 0);
        QCOMPARE(r.coord->ownership().collisions(), 0);
        QVERIFY(r.budget.peakBytes() <= 4 * PIECE);              // configured memory cap honoured
        QVERIFY(r.coord->wantsTransportHold());                  // debrid-only never seeds silently
        QCOMPARE(r.sink.releases, 0);
        QCOMPARE(r.tracker.connections, 0);                      // zero swarm contact
        QCOMPARE(r.peer.connections, 0);
        QVERIFY(!r.coord->status().p2pFallbackApproved);
    }

    void corruptProviderIsQuarantined()
    {
        // Scenario F: mutated provider bytes are caught by libtorrent and never marked verified.
        Rig r;
        build(r, lt::create_torrent::v1_only, fastConfig());
        r.cdn.mode = Cdn::Mode::CorruptFile;
        r.cdn.corruptFile = 2;   // c.bin
        r.coord->start(TransportPolicy::DebridOnly, false);

        QTRY_VERIFY_WITH_TIMEOUT(r.hashFailures > 0, 20000);
        QTRY_VERIFY_WITH_TIMEOUT((r.coord->state() == TransportState::WaitingForProvider)
                || (r.coord->state() == TransportState::HttpDraining), 5000);
        QCOMPARE(r.coord->status().lastReason, Reason::IntegrityFailure);
        QVERIFY(!r.finished);
        QVERIFY(r.coord->wantsTransportHold());
        QCOMPARE(r.tracker.connections, 0);
        QCOMPARE(r.peer.connections, 0);
        // Every libtorrent-verified piece is genuinely correct.
        const QBitArray have = r.sink.havePieces();
        for (int p = 0; p < have.size(); ++p)
        {
            if (!have.testBit(p))
                continue;
            for (const PieceSlice &s : nativePieceSlices(*r.sink.info, p))
                QVERIFY(s.nativeFileIndex != 2);
        }
    }

    void repeatedOutageBacksOffInsteadOfBusyLooping()
    {
        // Regression: a probe failing while the circuit is already open must advance the backoff.
        Rig r;
        build(r, lt::create_torrent::v1_only, fastConfig());   // backoff 200 ms doubling to 400 ms
        r.provider.available = false;
        r.coord->start(TransportPolicy::DebridOnly, false);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::WaitingForProvider, 5000);
        QTest::qWait(2500);
        // ~2.5 s / >=160 ms per attempt (with -20% jitter) => at most ~16; a busy loop makes thousands.
        QVERIFY2(r.provider.manifestCalls <= 16, qPrintable(QString::number(r.provider.manifestCalls)));
        QVERIFY(r.provider.manifestCalls >= 3);   // and it does keep retrying
        QCOMPARE(r.tracker.connections, 0);
        QCOMPARE(r.peer.connections, 0);
    }

    void noConsentNeverTouchesSwarm()
    {
        Rig r;
        build(r, lt::create_torrent::v1_only, fastConfig());
        r.provider.available = false;
        r.coord->start(TransportPolicy::DebridPreferred, false);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::WaitingForUser, 5000);
        QCOMPARE(r.coord->status().lastReason, Reason::PermissionRequiredForP2P);
        QTest::qWait(1500);
        QCOMPARE(r.sink.releases, 0);
        QCOMPARE(r.tracker.connections, 0);
        QCOMPARE(r.peer.connections, 0);

        // Explicit consent releases the hold — and only then.
        r.coord->setPolicy(TransportPolicy::DebridPreferred, true);
        QCOMPARE(r.coord->state(), TransportState::P2PActive);
        QCOMPARE(r.sink.releases, 1);
    }

    void partialProviderCoverageUsesProviderThenPeers()
    {
        // Regression (live report): a torrent whose first file is not on the provider used to be
        // rejected whole ("unmatched torrent files") and downloaded from peers only. The provider
        // must supply every piece it has in full; only the others go to peers. The hidden file
        // spans more pieces than there are HTTP slots, which used to starve the picker.
        Rig r;
        build(r, lt::create_torrent::v1_only, fastConfig());
        const int hidden = hideFirstProviderFile(r);
        const int n = r.sink.info->num_pieces();
        const int fromProvider = piecesNotTouching(r, hidden);
        QVERIFY(fromProvider > 0);
        QVERIFY((n - fromProvider) > fastConfig().maxPiecesInFlight);

        r.coord->start(TransportPolicy::DebridPreferred, true);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::P2PActive, 20000);
        QCOMPARE(r.coord->status().lastReason, Reason::ProviderLacksPieces);
        QCOMPARE(r.coord->status().httpVerifiedPieces, fromProvider);
        QCOMPARE(r.coord->status().providerPieces, fromProvider);
        QCOMPARE(r.coord->status().peerOnlyPieces, n - fromProvider);
        QCOMPARE(r.coord->status().boundFileCount, r.coord->status().torrentFileCount - 1);
        QVERIFY(!r.coord->wantsTransportHold());
        QCOMPARE(r.hashFailures, 0);

        // Every piece the provider could not supply is still missing, and nothing else is.
        const QBitArray haveAtSwitch = r.sink.havePieces();
        QCOMPARE(haveAtSwitch.count(true), fromProvider);
        qint64 missingBytes = 0;
        for (int p = 0; p < n; ++p)
            missingBytes += haveAtSwitch.testBit(p) ? 0 : r.sink.info->piece_size(lt::piece_index_t {p});

        // The provider has nothing more to give: no probing back and forth while peers work.
        QTest::qWait(1200);
        QCOMPARE(r.coord->state(), TransportState::P2PActive);
        QCOMPARE(r.provider.manifestCalls, 1);
        QCOMPARE(r.coord->status().switchCount, quint64 {1});

        const qint64 payloadAtSwitch = r.sink.handle.status({}).total_payload_download;
        lt::session seedSes {isolated()};
        lt::add_torrent_params seedAtp;
        seedAtp.ti = std::make_shared<lt::torrent_info>(*r.sink.info);
        seedAtp.save_path = r.src.string();
        seedAtp.flags |= lt::torrent_flags::seed_mode;
        seedSes.add_torrent(seedAtp);
        QTRY_VERIFY_WITH_TIMEOUT(seedSes.listen_port() != 0, 5000);
        r.sink.handle.connect_peer({boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(seedSes.listen_port())});

        QTRY_VERIFY_WITH_TIMEOUT(r.finished, 30000);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::Completed, 5000);
        QVERIFY(outputMatches(r));
        const qint64 p2pBytes = r.sink.handle.status({}).total_payload_download - payloadAtSwitch;
        QVERIFY2(p2pBytes <= (missingBytes + PIECE), qPrintable(u"p2p=%1 missing=%2"_s.arg(p2pBytes).arg(missingBytes)));
        QCOMPARE(r.coord->status().httpVerifiedPieces, fromProvider);
        QCOMPARE(r.coord->ownership().collisions(), 0);
    }

    void partialProviderCoverageDebridOnlyStopsWithoutSwarm()
    {
        // Same layout under debrid-only: take what the provider has, then stop and say why.
        // Peers are never contacted and the provider is not polled in a loop.
        Rig r;
        build(r, lt::create_torrent::v1_only, fastConfig());
        const int hidden = hideFirstProviderFile(r);
        const int fromProvider = piecesNotTouching(r, hidden);

        r.coord->start(TransportPolicy::DebridOnly, true /* must be ignored */);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::WaitingForUser, 20000);
        QCOMPARE(r.coord->status().lastReason, Reason::ProviderLacksPieces);
        QCOMPARE(r.coord->status().httpVerifiedPieces, fromProvider);
        QCOMPARE(r.sink.havePieces().count(true), fromProvider);

        QTest::qWait(1200);
        QCOMPARE(r.coord->state(), TransportState::WaitingForUser);
        QCOMPARE(r.provider.manifestCalls, 1);

        // Asking again finds nothing usable for what is missing and stops at once.
        r.coord->notifyUserStarted();
        QTRY_COMPARE_WITH_TIMEOUT(r.provider.manifestCalls, 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::WaitingForUser, 5000);
        QCOMPARE(r.coord->status().lastReason, Reason::ProviderLacksPieces);
        QCOMPARE(r.coord->status().providerPieces, 0);
        QVERIFY(r.coord->status().peerOnlyPieces > 0);

        QVERIFY(!r.finished);
        QVERIFY(r.coord->wantsTransportHold());
        QCOMPARE(r.sink.releases, 0);
        QCOMPARE(r.tracker.connections, 0);
        QCOMPARE(r.peer.connections, 0);
        QCOMPARE(r.hashFailures, 0);
    }

    void outageFallsBackToP2PWithoutRedownload()
    {
        // Scenario B: ~30% via HTTP, provider then returns 503; P2P finishes only missing pieces.
        Rig r;
        CoordinatorConfig config = fastConfig();
        config.maxPiecesInFlight = 1;
        build(r, lt::create_torrent::v1_only, config);
        r.bucket.setRate(48 * 1024, 16 * 1024);   // slow HTTP so the outage lands mid-transfer
        r.coord->start(TransportPolicy::DebridPreferred, true);

        const int n = r.sink.info->num_pieces();
        QTRY_VERIFY_WITH_TIMEOUT(r.coord->status().httpVerifiedPieces >= (n * 3 / 10), 20000);
        r.cdn.mode = Cdn::Mode::Unavailable;
        r.provider.available = false;
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::P2PActive, 15000);
        QVERIFY(!r.coord->wantsTransportHold());
        QVERIFY(r.sink.releases >= 1);

        const QBitArray haveAtSwitch = r.sink.havePieces();
        qint64 missingBytes = 0;
        for (int p = 0; p < n; ++p)
            missingBytes += haveAtSwitch.testBit(p) ? 0 : r.sink.info->piece_size(lt::piece_index_t {p});
        QVERIFY(missingBytes > 0);
        const qint64 payloadAtSwitch = r.sink.handle.status({}).total_payload_download;

        // Local seed stands in for the swarm.
        lt::session seedSes {isolated()};
        lt::add_torrent_params seedAtp;
        seedAtp.ti = std::make_shared<lt::torrent_info>(*r.sink.info);
        seedAtp.save_path = r.src.string();
        seedAtp.flags |= lt::torrent_flags::seed_mode;
        seedSes.add_torrent(seedAtp);
        QTRY_VERIFY_WITH_TIMEOUT(seedSes.listen_port() != 0, 5000);
        r.sink.handle.connect_peer({boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(seedSes.listen_port())});

        QTRY_VERIFY_WITH_TIMEOUT(r.finished, 30000);
        QVERIFY(outputMatches(r));
        const qint64 p2pBytes = r.sink.handle.status({}).total_payload_download - payloadAtSwitch;
        QVERIFY2(p2pBytes <= (missingBytes + PIECE), qPrintable(u"p2p=%1 missing=%2"_s.arg(p2pBytes).arg(missingBytes)));
        QVERIFY(r.coord->status().switchCount >= 1);
        QCOMPARE(r.coord->ownership().collisions(), 0);
    }
};

QTEST_MAIN(TestHybridCoordinator)
#include "testhybridcoordinator.moc"

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

#include <map>
#include <set>

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
#include <QElapsedTimer>
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

        // One status call per read, like the app's cached bitfield; a call per piece made the
        // test's own main thread the bottleneck once both sources were busy.
        QBitArray havePieces() const override
        {
            const lt::typed_bitfield<lt::piece_index_t> pieces = handle.status(lt::torrent_handle::query_pieces).pieces;
            QBitArray b(info->num_pieces());
            for (int p = 0; p < info->num_pieces(); ++p)
                b.setBit(p, (p < pieces.size()) && pieces.get_bit(lt::piece_index_t {p}));
            return b;
        }

        QList<int> wantedMissingPieces() const override
        {
            const QBitArray have = havePieces();
            QList<int> out;
            for (int p = 0; p < have.size(); ++p)
            {
                if (!have.testBit(p))
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
            // Independent of the coordinator: at the moment of submission, no peer may have
            // requested or delivered any block of this piece (design rule 4).
            for (const lt::partial_piece_info &info : handle.get_download_queue())
            {
                if (static_cast<int>(info.piece_index) == piece)
                    ++submitsIntoPeerPieces;
            }
            if (submitted.contains(piece))
                ++resubmissions;
            submitted.insert(piece);
            handle.add_piece(lt::piece_index_t {piece}, std::move(data));
        }

        // Combined mode. The priority restored is whatever the piece had before (here libtorrent's
        // default, as the test never sets priorities itself).
        void setPeerExclusion(const QList<int> &exclude, const QList<int> &include) override
        {
            ++exclusionCalls;
            std::vector<std::pair<lt::piece_index_t, lt::download_priority_t>> changes;
            for (const int piece : include)
            {
                if (const auto it = saved.find(piece); it != saved.end())
                {
                    changes.emplace_back(lt::piece_index_t {piece}, it->second);
                    saved.erase(it);
                }
            }
            for (const int piece : exclude)
            {
                if (saved.contains(piece))
                    continue;
                saved[piece] = handle.piece_priority(lt::piece_index_t {piece});
                everExcluded.insert(piece);
                changes.emplace_back(lt::piece_index_t {piece}, lt::dont_download);
            }
            handle.prioritize_pieces(changes);
        }

        quint64 peerExclusionGeneration() const override { return 0; }

        void requestPeerQueue(QObject *context, std::function<void (const QList<int> &)> done) override
        {
            QList<int> pieces;
            for (const lt::partial_piece_info &info : handle.get_download_queue())
                pieces.append(static_cast<int>(info.piece_index));
            QMetaObject::invokeMethod(context, [done = std::move(done), pieces] { done(pieces); }, Qt::QueuedConnection);
        }

        qint64 peerDownloadRate() const override { return handle.status({}).download_payload_rate; }

        std::map<int, lt::download_priority_t> saved;
        std::set<int> everExcluded;
        std::set<int> submitted;
        int exclusionCalls = 0;
        int submitsIntoPeerPieces = 0;
        int resubmissions = 0;
    };

    lt::session_params isolated()
    {
        lt::settings_pack p;
        // What the app subscribes to. "all" includes the per-peer debug log, which overflows the
        // alert queue once a seed is connected and then drops piece verdicts.
        p.set_int(lt::settings_pack::alert_mask, lt::alert_category::status | lt::alert_category::piece_progress
                | lt::alert_category::error | lt::alert_category::storage);
        p.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
        p.set_bool(lt::settings_pack::enable_dht, false);
        p.set_bool(lt::settings_pack::enable_lsd, false);
        p.set_bool(lt::settings_pack::enable_upnp, false);
        p.set_bool(lt::settings_pack::enable_natpmp, false);
        p.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
        // TCP only: on loopback an outgoing uTP connection to the local seed was seen to stay in
        // its handshake for many seconds, so the swarm side of a test depended on luck.
        p.set_bool(lt::settings_pack::enable_outgoing_utp, false);
        p.set_bool(lt::settings_pack::enable_incoming_utp, false);
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
        int haveAtFinish = -1;      // pieces verified when libtorrent said "finished"
        int hashFailures = 0;
        QList<TransportState> states;
        QList<Reason> reasons;
        std::unique_ptr<lt::session> seed;
    };

    void build(Rig &r, const lt::create_flags_t flags, const CoordinatorConfig &config, const int scale = 1)
    {
        QVERIFY(r.dir.isValid());
        r.src = fs::path(r.dir.path().toStdWString()) / "src";
        r.save = fs::path(r.dir.path().toStdWString()) / "save";
        const fs::path root = r.src / "Fixture";
        fs::create_directories(root / "sub");
        const QList<std::pair<std::string, int>> files {{"a.bin", 100'000 * scale}, {"sub/b.bin", 70'001 * scale}, {"c.bin", 130'000 * scale}};
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
        connect(r.coord.get(), &HybridCoordinator::transitioned, r.coord.get(), [&r](TransportState, const TransportState to, const Reason reason)
        {
            r.states.append(to);
            r.reasons.append(reason);
        });
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
                    if (!r.finished)
                        r.haveAtFinish = r.sink.havePieces().count(true);
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
        // libtorrent may report completion before its last writes reach the disk (spike Q1b), so
        // allow the flush some time on larger fixtures.
        r.sink.handle.flush_cache();
        for (int attempt = 0; attempt < 25; ++attempt)
        {
            QTest::qWait(200);
            bool same = true;
            for (const TorrentFileEntry &f : nativeFileEntries(*r.sink.info))
            {
                if (!f.isPad && (readAll(r.src / f.path.toStdString()) != readAll(r.save / f.path.toStdString())))
                {
                    same = false;
                    break;
                }
            }
            if (same)
                return true;
        }
        return false;
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

    static CoordinatorConfig combinedConfig()
    {
        CoordinatorConfig c = fastConfig();
        c.combinedSources = true;
        c.peerQueueIntervalMs = 100;
        c.peerStallMs = 1500;
        return c;
    }

    // A local seed standing in for the swarm, limited to `bytesPerSecond` (0 = unlimited).
    static void startSeed(Rig &r, const int bytesPerSecond)
    {
        r.seed = std::make_unique<lt::session>(isolated());
        lt::add_torrent_params seedAtp;
        seedAtp.ti = std::make_shared<lt::torrent_info>(*r.sink.info);
        seedAtp.save_path = r.src.string();   // checked normally: seed_mode served no v2 data here
        const lt::torrent_handle seedHandle = r.seed->add_torrent(seedAtp);
        if (bytesPerSecond > 0)
            seedHandle.set_upload_limit(bytesPerSecond);
        QTRY_VERIFY_WITH_TIMEOUT(r.seed->listen_port() != 0, 5000);
        // A peer that connects before the seed's torrent is ready is turned away and retried only
        // much later.
        QTRY_VERIFY_WITH_TIMEOUT(seedHandle.status({}).state == lt::torrent_status::seeding, 20000);
        r.sink.handle.connect_peer({boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(r.seed->listen_port())});
    }

    static qint64 piecesBytes(const Rig &r, const int count)
    {
        return static_cast<qint64>(count) * r.sink.info->piece_length();
    }

    // One line describing a run, for failure messages.
    static QString describe(Rig &r)
    {
        const OwnershipCounts c = r.coord->ownership().counts();
        const lt::torrent_status st = r.sink.handle.status({});
        QStringList path;
        for (qsizetype i = 0; i < r.states.size(); ++i)
            path << (wireName(r.states[i]) + u'(' + wireName(r.reasons[i]) + u')');
        return u"have %1/%2 at finish %3, provider %4, hidden %5, owners f%6 s%7 p%8 v%9, peers %10, p2p bytes %11; %12"_s
                .arg(r.sink.havePieces().count(true)).arg(r.sink.info->num_pieces()).arg(r.haveAtFinish)
                .arg(r.coord->status().httpVerifiedPieces).arg(r.sink.saved.size())
                .arg(c.httpFetching).arg(c.httpSubmitted).arg(c.p2pActive).arg(c.verified)
                .arg(st.num_peers).arg(st.total_payload_download).arg(path.join(u" > "));
    }

    // Combined-mode invariants that must hold at the end of every run.
    static void verifyCombinedRun(Rig &r)
    {
        const int n = r.sink.info->num_pieces();
        QVERIFY2(outputMatches(r), qPrintable(describe(r)));
        QCOMPARE(r.haveAtFinish, n);                       // never "finished" with hidden pieces missing
        QCOMPARE(r.hashFailures, 0);
        QCOMPARE(r.coord->ownership().collisions(), 0);
        QCOMPARE(r.sink.submitsIntoPeerPieces, 0);         // no provider bytes into a peer's piece
        QCOMPARE(r.sink.resubmissions, 0);                 // no piece fetched over HTTPS twice
        QVERIFY(r.sink.saved.empty());                     // every hidden piece was given back
        for (const lt::download_priority_t priority : r.sink.handle.get_piece_priorities())
            QVERIFY(priority == lt::default_priority);     // exactly the priority it had before
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
        QCOMPARE(r.sink.exclusionCalls, 0);   // serialized mode never touches piece priorities
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
        QCOMPARE(r.sink.exclusionCalls, 0);   // serialized mode never touches piece priorities
    }

    // ------------------------------------------------------------------ combined mode (ADR-011)

    void filePiecePrioritiesMatchLibtorrent_data()
    {
        QTest::addColumn<int>("flags");
        QTest::newRow("v1") << static_cast<int>(static_cast<std::uint32_t>(lt::create_torrent::v1_only));
        QTest::newRow("hybrid v1+v2 with pads") << 0;
        QTest::newRow("v2") << static_cast<int>(static_cast<std::uint32_t>(lt::create_torrent::v2_only));
    }

    void filePiecePrioritiesMatchLibtorrent()
    {
        // The priority a hidden piece gets back is computed, not read from libtorrent. It must be
        // exactly what libtorrent itself derives from the file priorities, boundary pieces included.
        QFETCH(int, flags);
        Rig r;
        build(r, lt::create_flags_t(static_cast<std::uint32_t>(flags)), fastConfig());
        const QList<TorrentFileEntry> files = nativeFileEntries(*r.sink.info);
        const QList<QList<lt::download_priority_t>> patterns
        {
            {lt::dont_download, lt::top_priority, lt::low_priority},
            {lt::low_priority, lt::dont_download, lt::top_priority},
            {lt::top_priority, lt::download_priority_t {3}, lt::dont_download}
        };
        for (const QList<lt::download_priority_t> &pattern : patterns)
        {
            std::vector<lt::download_priority_t> native;
            qsizetype next = 0;
            for (const TorrentFileEntry &file : files)
                native.push_back(file.isPad ? lt::default_priority : pattern.value(next++, lt::default_priority));
            r.sink.handle.prioritize_files(native);
            const std::vector<lt::download_priority_t> expected = filePiecePriorities(*r.sink.info, native);
            QTRY_VERIFY_WITH_TIMEOUT(r.sink.handle.get_piece_priorities() == expected, 5000);
        }
    }

    void combinedDownloadsFromBothWithoutCollisions_data()
    {
        QTest::addColumn<int>("flags");
        QTest::newRow("v1") << static_cast<int>(static_cast<std::uint32_t>(lt::create_torrent::v1_only));
        QTest::newRow("hybrid v1+v2 with pads") << 0;
    }

    void combinedDownloadsFromBothWithoutCollisions()
    {
        // Provider and one seed, equally throttled, on the same torrent at once.
        QFETCH(int, flags);
        Rig r;
        build(r, lt::create_flags_t(static_cast<std::uint32_t>(flags)), combinedConfig(), 20);
        r.bucket.setRate(512 * 1024, 64 * 1024);
        r.coord->start(TransportPolicy::DebridPreferred, true);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::CombinedActive, 10000);
        QVERIFY(!r.coord->wantsTransportHold());
        QCOMPARE(r.coord->status().activeTransport, ActiveTransport::Combined);
        startSeed(r, 512 * 1024);

        QTRY_VERIFY_WITH_TIMEOUT(r.finished, 60000);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::Completed, 5000);
        verifyCombinedRun(r);

        const int n = r.sink.info->num_pieces();
        const HybridStatus st = r.coord->status();
        const int fromProvider = st.httpVerifiedPieces;
        qInfo("pieces: %d, provider: %d, peers: %d, yielded to peers: %d, tail: %s", n, fromProvider, n - fromProvider
                , st.piecesYieldedToPeers, qPrintable(r.reasons.contains(Reason::TailToPeers) ? wireName(Reason::TailToPeers)
                    : r.reasons.contains(Reason::TailToProvider) ? wireName(Reason::TailToProvider) : u"none"_s));
        QVERIFY2(fromProvider >= (n / 5), qPrintable(describe(r)));          // both sources
        QVERIFY2((n - fromProvider) >= (n / 5), qPrintable(describe(r)));    // did real work
        QVERIFY(!r.sink.everExcluded.empty());
        // The last pieces went to one source, unless peers already held all of them (no hand-over needed).
        QVERIFY(!(r.reasons.contains(Reason::TailToProvider) && r.reasons.contains(Reason::TailToPeers)));

        // Nothing fetched twice: peers delivered only what the provider did not, the provider only
        // whole pieces it submitted once.
        const qint64 p2pBytes = r.sink.handle.status({}).total_payload_download;
        QVERIFY2(p2pBytes <= (piecesBytes(r, n - fromProvider) + (4 * PIECE))
                , qPrintable(u"p2p=%1 peers' pieces=%2"_s.arg(p2pBytes).arg(piecesBytes(r, n - fromProvider))));
        QVERIFY(st.httpBytesSubmitted <= piecesBytes(r, fromProvider));
    }

    void combinedTailGoesToFasterSource_data()
    {
        QTest::addColumn<int>("httpRate");
        QTest::addColumn<int>("seedRate");
        QTest::addColumn<Reason>("expected");
        QTest::newRow("provider faster") << (2048 * 1024) << (96 * 1024) << Reason::TailToProvider;
        QTest::newRow("peers faster") << (96 * 1024) << (2048 * 1024) << Reason::TailToPeers;
    }

    void combinedTailGoesToFasterSource()
    {
        // The last pieces go to one source, chosen by measured rate; neither ends "finished" early.
        QFETCH(int, httpRate);
        QFETCH(int, seedRate);
        QFETCH(Reason, expected);
        Rig r;
        build(r, lt::create_torrent::v1_only, combinedConfig(), 20);
        r.bucket.setRate(httpRate, 64 * 1024);
        r.coord->start(TransportPolicy::DebridPreferred, true);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::CombinedActive, 10000);
        startSeed(r, seedRate);

        QTRY_VERIFY_WITH_TIMEOUT(r.finished, 60000);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::Completed, 5000);
        verifyCombinedRun(r);
        const int n = r.sink.info->num_pieces();
        const int fromProvider = r.coord->status().httpVerifiedPieces;
        if (expected == Reason::TailToProvider)
        {
            QVERIFY2(r.reasons.contains(Reason::TailToProvider), qPrintable(describe(r)));
            QVERIFY2(!r.reasons.contains(Reason::TailToPeers), qPrintable(describe(r)));
            QVERIFY2(fromProvider > (n / 2), qPrintable(describe(r)));
        }
        else
        {
            // Much faster peers may already hold every remaining piece before the threshold is
            // reached; then there is nothing to hand over and the provider just stops getting work.
            QVERIFY2(!r.reasons.contains(Reason::TailToProvider), qPrintable(describe(r)));
            QVERIFY2(fromProvider < (n / 5), qPrintable(describe(r)));
        }
    }

    void combinedProviderOutageLeavesRestToPeers()
    {
        // The provider fails mid-download: peers, already running, take every missing piece and
        // fetch nothing that is already verified.
        Rig r;
        build(r, lt::create_torrent::v1_only, combinedConfig(), 20);
        r.bucket.setRate(512 * 1024, 64 * 1024);
        r.coord->start(TransportPolicy::DebridPreferred, true);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::CombinedActive, 10000);
        startSeed(r, 256 * 1024);

        const int n = r.sink.info->num_pieces();
        QTRY_VERIFY_WITH_TIMEOUT(r.coord->status().httpVerifiedPieces >= (n / 5), 30000);
        r.cdn.mode = Cdn::Mode::Unavailable;
        r.provider.available = false;
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::P2PActive, 15000);
        QVERIFY(!r.coord->wantsTransportHold());
        r.seed->get_torrents().front().set_upload_limit(0);   // let the swarm finish quickly

        QTRY_VERIFY_WITH_TIMEOUT(r.finished, 60000);
        verifyCombinedRun(r);
        const int fromProvider = r.coord->status().httpVerifiedPieces;
        const qint64 p2pBytes = r.sink.handle.status({}).total_payload_download;
        QVERIFY2(p2pBytes <= (piecesBytes(r, n - fromProvider) + (4 * PIECE))
                , qPrintable(u"p2p=%1 peers' pieces=%2"_s.arg(p2pBytes).arg(piecesBytes(r, n - fromProvider))));
    }

    void combinedPeersStallProviderFinishes()
    {
        // The only seed disappears mid-download (leaving partial pieces): the provider finishes,
        // completing those pieces without discarding the blocks peers delivered.
        Rig r;
        build(r, lt::create_torrent::v1_only, combinedConfig(), 20);
        r.bucket.setRate(384 * 1024, 64 * 1024);
        r.coord->start(TransportPolicy::DebridPreferred, true);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::CombinedActive, 10000);
        startSeed(r, 384 * 1024);

        const int n = r.sink.info->num_pieces();
        QTRY_VERIFY_WITH_TIMEOUT((r.sink.havePieces().count(true) - r.coord->status().httpVerifiedPieces) >= (n / 5), 30000);
        r.seed.reset();   // peers gone

        QTRY_VERIFY_WITH_TIMEOUT(r.finished, 60000);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::Completed, 5000);
        verifyCombinedRun(r);
        QVERIFY(r.reasons.contains(Reason::TailToProvider));
        QVERIFY(!r.reasons.contains(Reason::TailToPeers));
    }

    void combinedSettingNeverReachesSwarmWithoutPermission_data()
    {
        QTest::addColumn<TransportPolicy>("policy");
        QTest::addColumn<bool>("consent");
        QTest::newRow("debrid only") << TransportPolicy::DebridOnly << true /* must be ignored */;
        QTest::newRow("preferred without consent") << TransportPolicy::DebridPreferred << false;
    }

    void combinedSettingNeverReachesSwarmWithoutPermission()
    {
        // Combined mode switched on globally changes nothing for torrents that may not use peers.
        QFETCH(TransportPolicy, policy);
        QFETCH(bool, consent);
        Rig r;
        build(r, lt::create_torrent::v1_only, combinedConfig());
        r.coord->start(policy, consent);
        QVERIFY(!r.coord->isCombined());
        QVERIFY(r.coord->wantsTransportHold());

        QTRY_VERIFY_WITH_TIMEOUT(r.finished, 20000);
        QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::Completed, 5000);
        QVERIFY(outputMatches(r));
        QVERIFY(!r.states.contains(TransportState::CombinedActive));
        QVERIFY(r.coord->wantsTransportHold());   // and no seeding afterwards either
        QCOMPARE(r.sink.exclusionCalls, 0);
        QCOMPARE(r.sink.releases, 0);
        QCOMPARE(r.tracker.connections, 0);   // zero swarm contact
        QCOMPARE(r.peer.connections, 0);
    }

    void combinedThroughputBeatsEitherSource()
    {
        // Measurement for ADR-011: HTTPS throttled below what the "line" can take, one seed
        // throttled to the same rate. Combined mode should approach the sum of both.
        constexpr int rate = 768 * 1024;
        // QBF_MEASURE_SCALE=60 gives a ~17 MiB fixture for steadier numbers (not run by default).
        const int scale = std::max(20, qEnvironmentVariableIntValue("QBF_MEASURE_SCALE"));

        const auto finishSeconds = [](Rig &r, const QElapsedTimer &clock)
        {
            while (!r.finished && (clock.elapsed() < 90'000))
                QTest::qWait(10);
            return clock.elapsed() / 1000.0;
        };

        double httpsOnly = 0;
        {
            Rig r;
            build(r, lt::create_torrent::v1_only, fastConfig(), scale);
            r.bucket.setRate(rate, 64 * 1024);
            QElapsedTimer clock;
            clock.start();
            r.coord->start(TransportPolicy::DebridOnly, false);
            httpsOnly = finishSeconds(r, clock);
            QVERIFY(r.finished);
            QVERIFY(outputMatches(r));
        }

        double peersOnly = 0;
        qint64 totalBytes = 0;
        {
            Rig r;
            build(r, lt::create_torrent::v1_only, fastConfig(), scale);
            totalBytes = r.sink.info->total_size();
            QElapsedTimer clock;
            clock.start();
            r.sink.handle.resume();   // plain BitTorrent, no coordinator involved
            startSeed(r, rate);
            peersOnly = finishSeconds(r, clock);
            QVERIFY(r.finished);
            QVERIFY(outputMatches(r));
        }

        double combined = 0;
        {
            Rig r;
            build(r, lt::create_torrent::v1_only, combinedConfig(), scale);
            r.bucket.setRate(rate, 64 * 1024);
            QElapsedTimer clock;
            clock.start();
            r.coord->start(TransportPolicy::DebridPreferred, true);
            QTRY_COMPARE_WITH_TIMEOUT(r.coord->state(), TransportState::CombinedActive, 10000);
            startSeed(r, rate);
            combined = finishSeconds(r, clock);
            QVERIFY(r.finished);
            verifyCombinedRun(r);
        }

        const auto mib = [totalBytes](const double seconds) { return (totalBytes / (1024.0 * 1024.0)) / seconds; };
        qInfo("%.1f MiB, each source limited to %d KiB/s: HTTPS only %.2f s (%.2f MiB/s), peers only %.2f s (%.2f MiB/s), combined %.2f s (%.2f MiB/s)"
                , totalBytes / (1024.0 * 1024.0), rate / 1024, httpsOnly, mib(httpsOnly), peersOnly, mib(peersOnly), combined, mib(combined));
        QVERIFY2(combined < (0.8 * std::min(httpsOnly, peersOnly)), "combined mode is not faster than either source alone");
    }
};

QTEST_MAIN(TestHybridCoordinator)
#include "testhybridcoordinator.moc"

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

// qBitFusion P1 feasibility spike (CORE-002, docs/SPECIFICATION.md section 03 §9, gates G1/G2, first layer of G4).
//
// Proves or disproves, against the exact pinned libtorrent build, the assumptions the hybrid design
// depends on. Every check prints PASS/FAIL/INFO and the process exits non-zero on any FAIL.
// No network access beyond 127.0.0.1. All data is generated deterministically.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/version.hpp>
#include <libtorrent/write_resume_data.hpp>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace
{
    constexpr int PIECE_SIZE = 16 * 1024;

    // ------------------------------------------------------------------ reporting
    struct Report
    {
        int pass = 0;
        int fail = 0;

        void check(const bool ok, const std::string &id, const std::string &what, const std::string &detail = {})
        {
            std::printf("[%s] %-10s %s%s%s\n", ok ? "PASS" : "FAIL", id.c_str(), what.c_str()
                    , detail.empty() ? "" : " | ", detail.c_str());
            std::fflush(stdout);
            ok ? ++pass : ++fail;
        }

        static void info(const std::string &id, const std::string &what)
        {
            std::printf("[INFO] %-10s %s\n", id.c_str(), what.c_str());
            std::fflush(stdout);
        }
    };

    std::string stateName(const lt::torrent_status::state_t s)
    {
        switch (s)
        {
        case lt::torrent_status::checking_files: return "checking_files";
        case lt::torrent_status::downloading_metadata: return "downloading_metadata";
        case lt::torrent_status::downloading: return "downloading";
        case lt::torrent_status::finished: return "finished";
        case lt::torrent_status::seeding: return "seeding";
        case lt::torrent_status::checking_resume_data: return "checking_resume_data";
        default: return "state#" + std::to_string(static_cast<int>(s));
        }
    }

    // ------------------------------------------------------------------ deterministic data
    std::vector<char> makeData(const std::size_t size, std::uint64_t seed)
    {
        std::vector<char> out(size);
        seed = seed ? seed : 0x9E3779B97F4A7C15ULL;
        for (std::size_t i = 0; i < size; ++i)
        {
            seed ^= seed >> 12;
            seed ^= seed << 25;
            seed ^= seed >> 27;
            out[i] = static_cast<char>((seed * 0x2545F4914F6CDD1DULL) >> 56);
        }
        return out;
    }

    std::vector<char> readFile(const fs::path &p)
    {
        std::ifstream in(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    // ------------------------------------------------------------------ leak detector
    // Counts TCP connections to a local port. Used as the torrent's tracker and as a known peer:
    // while a torrent is held (paused) for HTTP-only transfer, both counters must stay zero.
    class Listener
    {
    public:
        Listener()
            : m_acceptor {m_io, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)}
        {
            accept();
            m_thread = std::thread([this] { m_io.run(); });
        }

        ~Listener()
        {
            m_io.stop();
            m_thread.join();
        }

        int port() const { return m_acceptor.local_endpoint().port(); }
        int connections() const { return m_connections.load(); }

    private:
        void accept()
        {
            m_acceptor.async_accept([this](const boost::system::error_code &ec, boost::asio::ip::tcp::socket socket)
            {
                if (!ec)
                {
                    ++m_connections;
                    boost::system::error_code ignored;
                    socket.close(ignored);
                }
                if (m_acceptor.is_open())
                    accept();
            });
        }

        boost::asio::io_context m_io;
        boost::asio::ip::tcp::acceptor m_acceptor;
        std::atomic<int> m_connections {0};
        std::thread m_thread;
    };

    // ------------------------------------------------------------------ fixtures
    struct Fixture
    {
        std::string label;
        fs::path srcDir;                       // parent of the torrent root folder
        std::shared_ptr<const lt::torrent_info> ti;
    };

    // Three non-piece-aligned files so v1 pieces cross file boundaries (T02).
    Fixture makeFixture(const fs::path &base, const std::string &label, const lt::create_flags_t flags, const int trackerPort)
    {
        Fixture fx;
        fx.label = label;
        fx.srcDir = base / ("src-" + label);
        const fs::path root = fx.srcDir / "fixture";
        fs::create_directories(root);

        const std::vector<std::pair<std::string, std::size_t>> files =
        {
            {"a.bin", 100'000},
            {"sub/b.bin", 70'001},
            {"c.bin", 130'000}
        };
        std::uint64_t seed = 1;
        for (const auto &[name, size] : files)
        {
            fs::create_directories((root / name).parent_path());
            const std::vector<char> data = makeData(size, seed++);
            std::ofstream(root / name, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
        }

        lt::create_torrent ct(lt::list_files(root.string(), flags), PIECE_SIZE, flags);
        ct.add_tracker("http://127.0.0.1:" + std::to_string(trackerPort) + "/announce");
        lt::set_piece_hashes(ct, fx.srcDir.string());
        const std::vector<char> buf = ct.generate_buf();
        // ABI 100 (deprecated-functions=OFF, as qBittorrent CI builds it) removes torrent_info(span, from_span).
        fx.ti = lt::load_torrent_buffer(lt::span<char const>(buf.data(), static_cast<std::ptrdiff_t>(buf.size()))).ti;
        return fx;
    }

    // Build the exact canonical piece using map_block — the same algorithm the product will use.
    std::vector<char> canonicalPiece(const Fixture &fx, const lt::piece_index_t piece, std::string *sliceDesc = nullptr)
    {
        const lt::file_storage &fs = fx.ti->layout();
        const int size = fx.ti->piece_size_for_req(piece);
        std::vector<char> out(static_cast<std::size_t>(size), '\0');
        std::int64_t cursor = 0;
        for (const lt::file_slice &slice : fx.ti->map_block(piece, 0, size))
        {
            const bool pad = fs.pad_file_at(slice.file_index);
            if (sliceDesc)
            {
                *sliceDesc += "[f" + std::to_string(static_cast<int>(slice.file_index)) + (pad ? " pad" : "")
                        + " off=" + std::to_string(slice.offset) + " len=" + std::to_string(slice.size) + "]";
            }
            if (!pad)
            {
                std::ifstream in(fx.srcDir / fs.file_path(slice.file_index), std::ios::binary);
                in.seekg(slice.offset);
                in.read(out.data() + cursor, static_cast<std::streamsize>(slice.size));
            }
            cursor += slice.size;
        }
        return out;
    }

    bool filesMatch(const Fixture &fx, const fs::path &savePath)
    {
        const lt::file_storage &fs = fx.ti->layout();
        for (const lt::file_index_t f : fs.file_range())
        {
            if (fs.pad_file_at(f))
                continue;
            if (readFile(fx.srcDir / fs.file_path(f)) != readFile(savePath / fs.file_path(f)))
                return false;
        }
        return true;
    }

    // Describes the first differing byte (file, offset, torrent piece) for diagnosis.
    std::string firstMismatch(const Fixture &fx, const fs::path &savePath)
    {
        const lt::file_storage &fs = fx.ti->layout();
        for (const lt::file_index_t f : fs.file_range())
        {
            if (fs.pad_file_at(f))
                continue;
            const std::vector<char> a = readFile(fx.srcDir / fs.file_path(f));
            const std::vector<char> b = readFile(savePath / fs.file_path(f));
            const std::size_t n = std::min(a.size(), b.size());
            std::size_t i = 0;
            while ((i < n) && (a[i] == b[i]))
                ++i;
            if ((i < n) || (a.size() != b.size()))
            {
                const std::int64_t torrentOffset = fs.file_offset(f) + static_cast<std::int64_t>(i);
                return fs.file_path(f) + " size src=" + std::to_string(a.size()) + " dst=" + std::to_string(b.size())
                        + " first diff at " + std::to_string(i) + " (piece " + std::to_string(torrentOffset / fx.ti->piece_length()) + ")";
            }
        }
        return "identical";
    }

    // ------------------------------------------------------------------ sessions
    lt::session_params isolatedParams()
    {
        lt::settings_pack p;
        p.set_int(lt::settings_pack::alert_mask, lt::alert_category::all);
        p.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
        p.set_bool(lt::settings_pack::enable_dht, false);
        p.set_bool(lt::settings_pack::enable_lsd, false);
        p.set_bool(lt::settings_pack::enable_upnp, false);
        p.set_bool(lt::settings_pack::enable_natpmp, false);
        p.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
        p.set_int(lt::settings_pack::alert_queue_size, 100'000);
        return lt::session_params(std::move(p));
    }

    // Pumps alerts and keeps the facts the experiments assert on.
    struct Events
    {
        lt::session &ses;
        std::set<int> finished;
        std::map<int, int> hashFailed;
        std::map<int, Clock::time_point> finishedAt;
        int trackerAnnounces = 0;
        int peerConnects = 0;
        bool torrentFinished = false;
        bool pausedAlert = false;
        bool checked = false;
        int listenPort = 0;
        std::vector<lt::add_torrent_params> resume;
        std::vector<std::string> order;   // piece_finished / hash_failed ordering

        void pump(const std::chrono::milliseconds wait = 20ms)
        {
            ses.wait_for_alert(wait);
            std::vector<lt::alert *> alerts;
            ses.pop_alerts(&alerts);
            for (lt::alert *a : alerts)
            {
                if (const auto *pf = lt::alert_cast<lt::piece_finished_alert>(a))
                {
                    const int p = static_cast<int>(pf->piece_index);
                    finished.insert(p);
                    finishedAt[p] = Clock::now();
                    order.push_back("pass:" + std::to_string(p));
                }
                else if (const auto *hf = lt::alert_cast<lt::hash_failed_alert>(a))
                {
                    const int p = static_cast<int>(hf->piece_index);
                    ++hashFailed[p];
                    order.push_back("fail:" + std::to_string(p));
                }
                else if (lt::alert_cast<lt::tracker_announce_alert>(a))
                    ++trackerAnnounces;
                else if (lt::alert_cast<lt::peer_connect_alert>(a))
                    ++peerConnects;
                else if (lt::alert_cast<lt::torrent_finished_alert>(a))
                    torrentFinished = true;
                else if (lt::alert_cast<lt::torrent_paused_alert>(a))
                    pausedAlert = true;
                else if (lt::alert_cast<lt::torrent_checked_alert>(a))
                    checked = true;
                else if (const auto *ls = lt::alert_cast<lt::listen_succeeded_alert>(a))
                {
                    if (ls->socket_type == lt::socket_type_t::tcp)
                        listenPort = ls->port;
                }
                else if (const auto *rd = lt::alert_cast<lt::save_resume_data_alert>(a))
                    resume.push_back(rd->params);
            }
        }

        bool until(const std::function<bool ()> &pred, const std::chrono::milliseconds timeout)
        {
            const auto deadline = Clock::now() + timeout;
            while (Clock::now() < deadline)
            {
                pump();
                if (pred())
                    return true;
            }
            return pred();
        }
    };

    lt::add_torrent_params heldParams(const Fixture &fx, const fs::path &savePath, const int fakePeerPort)
    {
        lt::add_torrent_params atp;
        atp.ti = std::make_shared<lt::torrent_info>(*fx.ti);
        atp.save_path = savePath.string();
        // The hybrid "transport hold": paused and not auto-managed, so the queue never resumes it.
        atp.flags |= lt::torrent_flags::paused;
        atp.flags &= ~lt::torrent_flags::auto_managed;
        atp.peers.emplace_back(boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(fakePeerPort));
        return atp;
    }

    bool waitUsable(Events &ev, const lt::torrent_handle &h)
    {
        return ev.until([&]
        {
            const auto st = h.status().state;
            return (st != lt::torrent_status::checking_files) && (st != lt::torrent_status::checking_resume_data);
        }, 10s);
    }

    std::vector<int> missingPieces(const lt::torrent_handle &h, const int n)
    {
        std::vector<int> out;
        for (int p = 0; p < n; ++p)
        {
            if (!h.have_piece(lt::piece_index_t {p}))
                out.push_back(p);
        }
        return out;
    }

    // ------------------------------------------------------------------ experiments
    // E1/E2/E3/E8: inject pieces into a held torrent; corrupt piece; zero swarm contact.
    void injectionExperiment(Report &r, const Fixture &fx, const fs::path &base, Listener &tracker, Listener &fakePeer
            , const double fraction, lt::session &ses, Events &ev, lt::torrent_handle &outHandle, std::set<int> &outInjected)
    {
        const std::string id = "E1-" + fx.label;
        const int n = fx.ti->num_pieces();
        const int trackerBefore = tracker.connections();
        const int peerBefore = fakePeer.connections();

        lt::torrent_handle h = ses.add_torrent(heldParams(fx, base / ("leech-" + fx.label), fakePeer.port()));
        outHandle = h;
        const bool usable = waitUsable(ev, h);
        const lt::torrent_status st0 = h.status();
        r.check(usable, id, "held torrent leaves checking state while paused", "state=" + stateName(st0.state));
        r.check(bool(st0.flags & lt::torrent_flags::paused), id, "torrent remains paused after add");

        // Boundary piece description for the record.
        for (int p = 0; p < n; ++p)
        {
            std::string desc;
            canonicalPiece(fx, lt::piece_index_t {p}, &desc);
            if (desc.find("][") != std::string::npos)
            {
                Report::info(id, "piece " + std::to_string(p) + " size=" + std::to_string(fx.ti->piece_size_for_req(lt::piece_index_t {p}))
                        + " slices " + desc);
                break;
            }
        }

        const int count = std::max(1, static_cast<int>(n * fraction));
        std::map<int, Clock::time_point> submittedAt;
        for (int p = 0; p < count; ++p)
        {
            submittedAt[p] = Clock::now();
            h.add_piece(lt::piece_index_t {p}, canonicalPiece(fx, lt::piece_index_t {p}));
            outInjected.insert(p);
        }
        const bool allPassed = ev.until([&]
        {
            return std::all_of(outInjected.begin(), outInjected.end(), [&](const int p) { return ev.finished.count(p) > 0; });
        }, 15s);
        r.check(allPassed && ev.hashFailed.empty(), id, "add_piece on held torrent -> piece_finished_alert for all "
                + std::to_string(count) + "/" + std::to_string(n) + " pieces (incl. file-boundary pieces)");

        std::vector<long long> lat;
        for (const auto &[p, t] : submittedAt)
        {
            if (ev.finishedAt.count(p))
                lat.push_back(std::chrono::duration_cast<std::chrono::microseconds>(ev.finishedAt[p] - t).count());
        }
        std::sort(lat.begin(), lat.end());
        if (!lat.empty())
            Report::info(id, "add_piece->piece_finished latency us: p50=" + std::to_string(lat[lat.size() / 2]) + " max=" + std::to_string(lat.back()));

        // have_piece may lag piece_finished_alert (spec 03 §2). Measure, don't assume.
        bool haveImmediately = true;
        for (const int p : outInjected)
            haveImmediately = haveImmediately && h.have_piece(lt::piece_index_t {p});
        const bool haveEventually = ev.until([&]
        {
            return std::all_of(outInjected.begin(), outInjected.end(), [&](const int p) { return h.have_piece(lt::piece_index_t {p}); });
        }, 5s);
        r.check(haveEventually, id, "have_piece() true for every injected piece", haveImmediately ? "immediately after alerts" : "lagged behind alerts");

        // E2: corrupt one byte of the next piece -> hash failure, then the correct bytes pass.
        if (count < n)
        {
            const std::string id2 = "E2-" + fx.label;
            const int p = count;
            std::vector<char> bad = canonicalPiece(fx, lt::piece_index_t {p});
            bad[bad.size() / 2] ^= 0x5A;
            h.add_piece(lt::piece_index_t {p}, std::move(bad));
            const bool failed = ev.until([&] { return ev.hashFailed.count(p) > 0; }, 10s);
            r.check(failed && !h.have_piece(lt::piece_index_t {p}), id2, "corrupt piece -> hash_failed_alert, piece not verified");
            h.add_piece(lt::piece_index_t {p}, canonicalPiece(fx, lt::piece_index_t {p}));
            const bool recovered = ev.until([&] { return ev.finished.count(p) > 0; }, 10s);
            r.check(recovered, id2, "same piece re-submitted with correct bytes -> verified");
            if (recovered)
                outInjected.insert(p);
        }

        // E3: zero swarm contact while held.
        std::this_thread::sleep_for(2s);
        ev.pump();
        const std::string id3 = "E3-" + fx.label;
        r.check(tracker.connections() == trackerBefore, id3, "held torrent: zero tracker connections"
                , "announces=" + std::to_string(ev.trackerAnnounces));
        r.check(fakePeer.connections() == peerBefore, id3, "held torrent: zero connections to known peer");
        r.check(bool(h.status().flags & lt::torrent_flags::paused), id3, "torrent still paused after injection");
    }

    void runVariant(Report &r, const fs::path &base, const std::string &label, const lt::create_flags_t flags
            , Listener &tracker, Listener &fakePeer, const bool fullSuite)
    {
        std::printf("\n=== variant %s ===\n", label.c_str());
        const Fixture fx = makeFixture(base, label, flags, tracker.port());
        const int n = fx.ti->num_pieces();
        int pads = 0;
        for (const lt::file_index_t f : fx.ti->layout().file_range())
            pads += fx.ti->layout().pad_file_at(f) ? 1 : 0;
        Report::info("FX-" + label, "pieces=" + std::to_string(n) + " files=" + std::to_string(fx.ti->layout().num_files())
                + " pad_files=" + std::to_string(pads) + " v1=" + std::to_string(fx.ti->info_hashes().has_v1())
                + " v2=" + std::to_string(fx.ti->info_hashes().has_v2()));

        const fs::path leechPath = base / ("leech-" + label);
        std::set<int> injected;
        lt::add_torrent_params resumeParams;

        {
            lt::session ses(isolatedParams());
            Events ev {ses};
            lt::torrent_handle h;
            injectionExperiment(r, fx, base, tracker, fakePeer, fullSuite ? 0.30 : 1.0, ses, ev, h, injected);

            if (!fullSuite)
            {
                const bool done = ev.until([&] { return h.status().is_finished; }, 10s);
                r.check(done && filesMatch(fx, leechPath), "E8-" + label, "all pieces via add_piece -> finished, files byte-identical to source");
                return;
            }

            // E4 (part 1): save resume data, then tear the session down ("crash-safe restart").
            h.save_resume_data(lt::torrent_handle::save_info_dict | lt::torrent_handle::flush_disk_cache);
            const bool gotResume = ev.until([&] { return !ev.resume.empty(); }, 10s);
            r.check(gotResume, "E4-" + label, "save_resume_data_alert received");
            if (gotResume)
                resumeParams = ev.resume.front();
        }

        // E4 (part 2): restart from resume data; verified pieces must come back, nothing else.
        lt::session ses(isolatedParams());
        Events ev {ses};
        const std::vector<char> buf = lt::write_resume_data_buf(resumeParams);
        lt::error_code ec;
        lt::add_torrent_params atp = lt::read_resume_data(lt::span<char const>(buf.data(), static_cast<std::ptrdiff_t>(buf.size())), ec);
        r.check(!ec, "E4-" + label, "resume data round-trips through bencode", ec.message());
        atp.save_path = leechPath.string();
        atp.flags |= lt::torrent_flags::paused;
        atp.flags &= ~lt::torrent_flags::auto_managed;
        lt::torrent_handle h = ses.add_torrent(atp);
        waitUsable(ev, h);
        const std::vector<int> missing = missingPieces(h, n);
        const int verifiedAfterRestart = n - static_cast<int>(missing.size());
        r.check(verifiedAfterRestart == static_cast<int>(injected.size()), "E4-" + label
                , "after restart libtorrent reports exactly the injected pieces as verified"
                , std::to_string(verifiedAfterRestart) + "/" + std::to_string(injected.size()));

        // E5: provider "fails" at ~30%: release the hold and let P2P finish from a local seed.
        lt::session seedSes(isolatedParams());
        Events seedEv {seedSes};
        lt::add_torrent_params seedAtp;
        seedAtp.ti = std::make_shared<lt::torrent_info>(*fx.ti);
        seedAtp.save_path = fx.srcDir.string();
        seedAtp.flags |= lt::torrent_flags::seed_mode;
        lt::torrent_handle seed = seedSes.add_torrent(seedAtp);
        seedEv.until([&] { return seedEv.listenPort != 0; }, 5s);

        std::int64_t missingBytes = 0;
        for (const int p : missing)
            missingBytes += fx.ti->piece_size(lt::piece_index_t {p});

        const auto switchStart = Clock::now();
        h.resume();
        h.connect_peer(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(seedEv.listenPort)));
        const bool completed = ev.until([&] { seedEv.pump(1ms); return h.status().is_finished; }, 30s);
        const auto switchMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - switchStart).count();
        const lt::torrent_status st = h.status();
        r.check(completed, "E5-" + label, "P2P completes remaining pieces after HTTP stops", std::to_string(switchMs) + " ms");
        const bool matchNow = filesMatch(fx, leechPath);
        if (!matchNow)
        {
            Report::info("E5-" + label, "mismatch right after is_finished: " + firstMismatch(fx, leechPath));
            h.flush_cache();
            std::this_thread::sleep_for(2s);
            ev.pump();
            Report::info("E5-" + label, "after flush_cache + 2 s: " + firstMismatch(fx, leechPath));
            ev.finished.clear();
            ev.checked = false;
            h.force_recheck();
            ev.until([&] { return ev.checked; }, 15s);
            Report::info("E5-" + label, "libtorrent force_recheck verdict: " + std::to_string(h.status().num_pieces) + "/" + std::to_string(n) + " pieces valid");
        }
        r.check(filesMatch(fx, leechPath), "E5-" + label, "final files byte-identical to source (original infohash, single tree)"
                , matchNow ? "" : "only after flush (timing), see INFO");
        r.check(st.total_payload_download <= (missingBytes + lt::default_block_size), "E5-" + label
                , "P2P downloaded only missing pieces (no verified piece re-fetched)"
                , "p2p_bytes=" + std::to_string(st.total_payload_download) + " missing_bytes=" + std::to_string(missingBytes));
    }

    // E6: P2P -> HTTP handover via graceful pause; HTTP fills everything including partial pieces.
    void handoverExperiment(Report &r, const fs::path &base, Listener &tracker)
    {
        std::printf("\n=== handover P2P -> HTTP ===\n");
        const Fixture fx = makeFixture(base, "handover", lt::create_torrent::v1_only, tracker.port());
        const int n = fx.ti->num_pieces();
        const fs::path leechPath = base / "leech-handover";

        lt::session seedSes(isolatedParams());
        Events seedEv {seedSes};
        lt::add_torrent_params seedAtp;
        seedAtp.ti = std::make_shared<lt::torrent_info>(*fx.ti);
        seedAtp.save_path = fx.srcDir.string();
        seedAtp.flags |= lt::torrent_flags::seed_mode;
        lt::torrent_handle seed = seedSes.add_torrent(seedAtp);
        seed.set_upload_limit(24 * 1024); // slow peer so pieces are genuinely in flight
        seedEv.until([&] { return seedEv.listenPort != 0; }, 5s);

        lt::session ses(isolatedParams());
        Events ev {ses};
        lt::add_torrent_params atp;
        atp.ti = std::make_shared<lt::torrent_info>(*fx.ti);
        atp.save_path = leechPath.string();
        atp.flags &= ~lt::torrent_flags::auto_managed;
        atp.flags &= ~lt::torrent_flags::paused;
        lt::torrent_handle h = ses.add_torrent(atp);
        waitUsable(ev, h);
        h.connect_peer(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(seedEv.listenPort)));
        ev.until([&] { seedEv.pump(1ms); return !ev.finished.empty(); }, 20s);
        std::this_thread::sleep_for(300ms);

        const std::vector<lt::partial_piece_info> queueBefore = h.get_download_queue();
        h.pause(lt::torrent_handle::graceful_pause);
        const bool paused = ev.until([&] { seedEv.pump(1ms); return ev.pausedAlert; }, 20s);
        const std::vector<lt::partial_piece_info> queueAfter = h.get_download_queue();
        int partialBlocks = 0;
        for (const auto &pi : queueAfter)
        {
            for (int b = 0; b < pi.blocks_in_piece; ++b)
                partialBlocks += (pi.blocks[b].state == lt::block_info::finished || pi.blocks[b].state == lt::block_info::writing) ? 1 : 0;
        }
        r.check(paused, "E6", "graceful_pause -> torrent_paused_alert");
        Report::info("E6", "download queue before pause=" + std::to_string(queueBefore.size()) + " pieces, after pause="
                + std::to_string(queueAfter.size()) + " pieces with " + std::to_string(partialBlocks) + " finished/writing blocks; verified="
                + std::to_string(ev.finished.size()) + "/" + std::to_string(n));
        r.check(h.status().num_peers == 0, "E6", "no peer connections once gracefully paused");

        // HTTP takes over every missing piece, including partially downloaded ones, without overwrite.
        const std::vector<int> missing = missingPieces(h, n);
        int zeroPrio = -1;
        if (missing.size() > 1)
        {
            zeroPrio = missing.back();
            h.piece_priority(lt::piece_index_t {zeroPrio}, lt::dont_download);
        }
        for (const int p : missing)
            h.add_piece(lt::piece_index_t {p}, canonicalPiece(fx, lt::piece_index_t {p}));
        const bool allPassed = ev.until([&]
        {
            return std::all_of(missing.begin(), missing.end(), [&](const int p) { return ev.finished.count(p) > 0; });
        }, 20s);
        r.check(allPassed && ev.hashFailed.empty(), "E6", "add_piece (no overwrite_existing) completes all " + std::to_string(missing.size())
                + " missing pieces after handover, incl. partials");
        if (zeroPrio >= 0)
            Report::info("E6", std::string("add_piece on a priority-0 piece ") + (ev.finished.count(zeroPrio) ? "was accepted and verified" : "was NOT verified"));
        if (zeroPrio >= 0)
            h.piece_priority(lt::piece_index_t {zeroPrio}, lt::default_priority);
        const bool finished = ev.until([&] { return h.status().is_finished; }, 10s);
        r.check(finished && filesMatch(fx, leechPath), "E6", "torrent finished; files byte-identical to source");
    }
}

int main(int argc, char **argv)
{
    std::printf("qBitFusion ltspike | libtorrent %s (%s)\n", lt::version(), LIBTORRENT_REVISION);
    const fs::path base = (argc > 1) ? fs::path(argv[1]) : (fs::temp_directory_path() / "qbitfusion-ltspike");
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    std::printf("work dir: %s\n", base.string().c_str());

    Report r;
    try
    {
        Listener tracker;
        Listener fakePeer;
        runVariant(r, base, "v1", lt::create_torrent::v1_only, tracker, fakePeer, true);
        runVariant(r, base, "hybrid", {}, tracker, fakePeer, false);
        runVariant(r, base, "v2", lt::create_torrent::v2_only, tracker, fakePeer, false);
        handoverExperiment(r, base, tracker);
    }
    catch (const std::exception &e)
    {
        r.check(false, "EXC", std::string("unexpected exception: ") + e.what());
    }

    std::printf("\nSUMMARY: %d passed, %d failed\n", r.pass, r.fail);
    return (r.fail == 0) ? 0 : 1;
}

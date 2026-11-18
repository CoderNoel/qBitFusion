# Decision log

Evidence behind the design: the build baseline, the libtorrent feasibility spike, the decisions that changed
the [specification](SPECIFICATION.md) (its section 11 defines the format), and what was seen against the
real service. Reproduce the spike with:

```powershell
pwsh qbitfusion/scripts/bootstrap-windows.ps1
pwsh qbitfusion/scripts/build-windows.ps1 -Spike
```

## Gate G0 | Baseline build — PASSED (2026-11-09)

- **Versions:** qBittorrent `release-5.3.0` @ `9213075d`, libtorrent 2.1.2 (`a610c6ef4`, static, `deprecated-functions=OFF` → `TORRENT_ABI_VERSION=100`), Qt 6.10.3 msvc2022_64, Boost 1.91.0 headers, OpenSSL 3.6.5 / zlib 1.3.2 (vcpkg `x64-windows-static-md-release`), MSVC 14.44.35207, CMake 4.4.3, Ninja.
- **Command:** `build-windows.ps1 -BuildDir build-baseline` (CI flags: `/DQT_FORCE_ASSERTS /WX`, `-DTESTING=ON`, `-DMSVC_RUNTIME_DYNAMIC=ON`).
- **Result:** `qbittorrent.exe` built with warnings-as-errors; `ctest` 22/22 passed (1.53 s).

## Gate G1 / first layer of G2 and G4 | libtorrent spike — PASSED 35/35 (2026-11-09)

Harness: `qbitfusion/spike/ltspike.cpp`. Fixtures: 3 files (100 000 / 70 001 / 130 000 bytes), 16 KiB pieces, generated deterministically; v1-only, hybrid (3 pad files) and v2-only variants. Isolated sessions on 127.0.0.1 with DHT/LSD/UPnP/NAT-PMP off. Each torrent carries an HTTP tracker and a known peer that point at local counting listeners.

| ID | Question (spec 11) | Observed (libtorrent 2.1.2) | Consequence |
| --- | --- | --- | --- |
| Q1 | Ordering after `add_piece` | `piece_finished_alert` 0.6–3 ms after submission (p50 0.6 ms v1, 1.3 ms v2); `have_piece()` already true when the alert is read; corrupt piece → `hash_failed_alert`, piece stays missing, and correct bytes re-submitted are accepted. | Coordinator marks Verified on `piece_finished_alert`; on `hash_failed_alert` it releases ownership and counts failures. |
| Q1b | Disk flush vs completion | After P2P completion, `is_finished` was true while the last file was still 98 304 / 100 000 bytes on disk. After `flush_cache()` + 2 s the files were identical, and `force_recheck` reported 19/19 valid. | Never read or move output on `is_finished` alone. Rely on qBittorrent's existing finish/move pipeline, which waits for libtorrent. Not data loss. |
| Q2 | Suppress P2P without priority games | A torrent added `paused` and not `auto_managed` ("transport hold") leaves checking (state `downloading`) and accepts `add_piece` for every piece. | **ADR-007 mechanism: serialized source mode.** HTTP runs only while the torrent is held; no per-piece priority overlay is needed, so the user's file priorities are never rewritten. |
| Q4 (part) | Swarm contact while held | Tracker listener: 0 connections, 0 announces; known-peer listener: 0 connections, across all variants, for the entire HTTP phase + 2 s. | First layer of G4 holds. The magnet metadata path (`addTorrent_impl` / `downloadMetadata`) was gated afterwards and is covered by the coordinator tests. **Still open:** DHT-enabled sessions, packet capture. |
| Q5 | v2 / hybrid pad files and piece size | Hybrid: `map_block` returns pad slices (`pad_file_at`), zero-filled pieces verify. v2-only: `piece_size_for_req()` gives short end-of-file pieces; all 20/20 verify. Output byte-identical in both. | Assembler zero-fills only metadata pad slices; piece length comes from `piece_size_for_req()`. |
| E4 | Crash-safe resume | After `save_resume_data` + session teardown + `read_resume_data`, libtorrent reports exactly the 6 injected pieces as verified. | Sidecar never stores piece state (ADR-006 holds). |
| E5 | No re-download after failover | P2P fetched 201 697 bytes = exactly the missing bytes; output identical; infohash unchanged; one storage tree. | Verified-progress preservation is demonstrated for HTTP→P2P. |
| E6 | P2P→HTTP handover | `graceful_pause` → `torrent_paused_alert`; 0 peers; download queue empty afterwards (5 pieces in flight before). `add_piece` without `overwrite_existing` completed all 17 missing pieces; output identical. `add_piece` on a priority-0 piece was also accepted. | Recovery = graceful pause → wait for paused alert → reconcile queue → HTTP. No `overwrite_existing` anywhere. |

### Limits of this evidence (do not overstate)

- Single seed on loopback; no multi-peer endgame, no real WAN latency.
- G2 overlap (HTTP and P2P on different pieces *simultaneously*) was **not** attempted. The serialized mode makes it unnecessary for the MVP.
- Partial-piece handover after graceful pause was observed with 0 partial blocks left. A partial piece surviving a pause is not yet exercised.
- Windows only. Linux/macOS not run.

## ADR-009 | Provider coverage is decided per piece — ACCEPTED (2026-11-11)

**Trigger.** In live use, a two-file torrent (a 71-byte text file at offset 0, then an 80 GiB video) was added with `debrid_preferred`. The Real-Debrid account held the torrent with only the video selected. qBitFusion logged `PROBING_PROVIDER -> P2P_ACTIVE (unmatched_torrent_files)` and downloaded everything from peers.

**Cause.** The MVP failed closed on the whole torrent when *any* wanted piece touched a file the provider does not expose. Piece 0 holds 71 bytes of the text file, so one piece disqualified 5148.

**Decision.** The specification's "selected-file boundary" rule applied as written: *"If exact data is missing, assign piece to allowed P2P or fail clearly."*

- A piece is an HTTP candidate only if every byte of it lies in an exactly bound provider file (path and size, ADR-005 unchanged; no basename or fuzzy matching was added).
- Candidates are filtered *before* picking, so unbound pieces cannot occupy HTTP slots.
- When every candidate is verified and other wanted pieces remain, the new machine event `HttpExhausted` hands the rest to peers if the policy permits (`debrid_preferred` with consent), asks for consent (`debrid_preferred` without), or stops in `WAITING_FOR_USER` with reason `provider_lacks_pieces` (`debrid_only`). No swarm contact under `debrid_only`.
- With nothing left for the provider to give, no recovery probe is armed: probing would pause the peers only to return.
- Sources stay serialized (ADR-007): HTTP and P2P never run at the same time, so the collision rule is untouched.

**Evidence.** `testhybridstatemachine::providerExhaustedHandsTheRestToPeersOnlyIfPermitted`; `testhybridcoordinator::partialProviderCoverageUsesProviderThenPeers` (real libtorrent session: HTTP verifies exactly the fully covered pieces, peers fetch only the rest, output byte-identical, one source switch, one manifest call, 0 collisions) and `…DebridOnlyStopsWithoutSwarm` (0 tracker/peer connections, 0 hold releases). 32/32 ctest suites.

## ADR-010 | Direct downloads get their own engine and page — ACCEPTED (2026-11-18)

**Trigger.** Plain file links (Stremio stream links through a debrid add-on, debrid download links) failed
in "Download from URL", because every http(s) link was fetched and parsed as a `.torrent`.

**Decision.**

- A direct download is not a torrent: no pieces, hashes or info-hash. It is kept out of libtorrent and
  out of the hybrid transport, in `src/base/directdownload/`, with one resumable `GET` per file into
  `<name>.part`, renamed when complete. `Net::DownloadManager` is not reused for the body for the
  reasons in INTEGRATION-NOTES.md (no `Range`, whole body in memory, transparent decompression).
- Pasted http(s) links without `.torrent` are requested once and classified by the answer: bencoded
  first bytes or `application/x-bittorrent` is a torrent (handed to the add-torrent flow with the bytes
  already fetched), a redirect to `magnet:` is a magnet, HTML without `attachment` is refused, anything
  else is a file. Magnets, hashes, files and `.torrent` links keep the upstream path, so torrent behavior
  is unchanged.
- The interface is a separate **Links** page, chosen by the user over rows in the Transfers list: that
  list's model, sidebar filters, details panel and context menu all assume a torrent, and an abstraction
  over them would touch the upstream files most likely to conflict on every merge.
- Links are secrets (add-on links can carry an API key, debrid links are signed). They are kept in the
  Credential Manager only until the file is complete, never in settings, the saved list or logs.
- One connection per file for now. A rough measurement against a distant public server gave about 1.5
  times the speed with four range connections, so parallel ranges are listed as open work rather than
  dismissed.

**Evidence.** `testdirectdownload` (42 cases against a local server). In a test instance with its own
profile: three public films (download.blender.org, and archive.org through its redirects) were checked,
named and sized correctly in the add dialog. One was paused, the app was quit while another was still
downloading, and after the restart the running one continued by itself and the paused one when started.
Both match archive.org's published MD5. Afterwards no link was left in the Credential Manager, and none
had appeared in `directdownloads.json`, `qBittorrent.ini` or the log.

## ADR-011 | Combined sources: provider and peers on one torrent at once — ACCEPTED (2026-11-18)

**Trigger.** With serialized sources (ADR-007) a torrent uses either the provider or peers. On a line
faster than the debrid server delivers, the rest of the line stays idle even when the torrent has a
healthy swarm.

**Decision.** An opt-in mode, off by default (Options > Hybrid sources, `Hybrid/CombinedSources`), in
which HTTPS and peers fill different pieces of the same torrent at the same time. The choices below were
put to the user as options and accepted as recommended.

- **Who may use it.** Only torrents set to "Debrid preferred, BitTorrent fallback" with the user's P2P
  consent. `isCombined()` is false for "Debrid only" and for "Debrid preferred" without consent, and the
  state machine refuses `COMBINED_ACTIVE` without P2P permission whatever it is told (the 50 000-event
  check now includes the new events and a context that asks for combined mode where P2P is forbidden).
  When on, it applies from the start of every such download, not only when HTTPS is slow: there is no
  trustworthy measure of the line's capacity to adapt on. The Options text says that peers are then
  contacted from the start.
- **New state** `COMBINED_ACTIVE`: swarm allowed, HTTP dispatch allowed. It replaces `HTTP_ACTIVE` after
  a successful probe and `HTTP_READY` after a successful recovery probe. A provider failure goes
  straight to `P2P_ACTIVE` (peers are already running, nothing to drain); in-flight HTTP work is cancelled
  and submitted pieces keep their exclusion until libtorrent's verdict. Without combined mode every
  transition is as before.
- **How pieces are split: a moving window.** HTTPS walks the wanted pieces in index order with its usual
  number in flight (`Hybrid/MaxHttpPiecesPerTorrent`); peers pick rarest-first among the rest. Only the
  pieces HTTPS holds are hidden from peers, with libtorrent piece priority 0 — the one mechanism that
  keeps libtorrent's picker off a piece. Each side skips what the other holds or has verified, so when one
  side runs out the other simply takes the rest.
- **No collisions, one referee.** The piece ownership arbiter stays the single referee. A piece is hidden
  before its first HTTP byte is requested. Its bytes are given to `add_piece` only after a read of
  libtorrent's download queue that was requested after the piece was hidden (and after the last time the
  torrent had to hide it again) and that does not list it. If a peer requested the piece just before it
  was hidden, HTTPS lets go of it and shows it to peers again (`piecesYieldedToPeers`). `overwrite_existing`
  is still never used.
- **The user's priorities.** File priorities are never touched. The exclusion is in memory only
  (`TorrentImpl::setHybridPeerExclusion`): the priority given back is the one qBittorrent itself set (its
  first/last-piece vector, or libtorrent's own file-to-piece rule, mirrored in `Hybrid::filePiecePriorities`
  and tested against libtorrent), resume data is written with those values, and a file priority change,
  the first/last-piece option or a reload re-applies the exclusion. A restart therefore starts without
  any exclusion.
- **Side effects of priority 0, handled.** libtorrent counts a hidden piece as unwanted. Progress and
  "wanted size" add the hidden bytes back. If libtorrent reports "finished" while a hidden piece is still
  missing, qBittorrent's completion handling (moving files, notifications, the "run on finish" program)
  waits until it is in.
- **End of the download: the faster source.** When no more than twice the HTTPS window is left outside
  HTTPS, the measured rates decide (ties go to the provider). Provider faster: HTTP work in flight is
  cancelled, peers are held with the graceful pause, and HTTPS finishes alone (`HTTP_READY` →
  `HTTP_ACTIVE`, reason `tail_to_provider`). Peers faster: HTTPS stops (at most one window of partial
  HTTP pieces is lost) and peers finish (`P2P_ACTIVE`, reason `tail_to_peers`), with no probing back
  unless peers then send nothing for 20 s and the 30 s source dwell has passed (`peers_stalled`).
  If peers already hold every remaining piece when the threshold is reached, nothing is handed over. Racing both sources for the same pieces was
  rejected: it breaks the collision rule.
- **Display.** The Source column reads "Real-Debrid + peers" (state name "Debrid + peers"); its tooltip
  splits HTTPS ↓ and Peers ↓. Speed and ETA show the sum, as they already added the HTTPS rate.

**Evidence.** `testhybridcoordinator`, real libtorrent sessions on loopback, a local range server as the
provider and one local seed (366–368 pieces of 16 KiB). Every combined run checks: output byte-identical;
no hash failure; no provider piece passed to `add_piece` while libtorrent's download queue listed it
(read by the test sink itself at that moment); no piece submitted twice; peer payload no larger than the
pieces HTTPS did not supply plus four pieces; every hidden piece back at its exact priority; libtorrent
never "finished" with a piece missing.

| Test | Result |
| --- | --- |
| `combinedDownloadsFromBothWithoutCollisions` (v1, hybrid with pads; both sources 512 KiB/s) | Roughly half the pieces from each source (e.g. 199 / 168), 1–7 pieces handed back to peers that had started them, 0 collisions. |
| `combinedTailGoesToFasterSource` | Provider 2 MiB/s vs seed 96 KiB/s: `tail_to_provider`, peers held, provider finishes. Provider 96 KiB/s vs seed 2 MiB/s: peers finish, never `tail_to_provider`. |
| `combinedProviderOutageLeavesRestToPeers` | `503` after 20%: `COMBINED_ACTIVE → P2P_ACTIVE` directly, peers fetch only the missing pieces. |
| `combinedPeersStallProviderFinishes` | The only seed is shut down after delivering 20%: provider takes the rest, `tail_to_provider`. |
| `combinedSettingNeverReachesSwarmWithoutPermission` | Setting on with "Debrid only", and with "Debrid preferred" without consent: never `COMBINED_ACTIVE`, 0 tracker and peer connections, no piece priority touched. |
| `filePiecePrioritiesMatchLibtorrent` | The restored priorities equal libtorrent's own for v1, hybrid and v2 layouts and three file-priority patterns. |
| `testhybridstatemachine` | New transitions, and the 50 000-event check with combined mode asked for where P2P is forbidden. |

**Measurement** (`combinedThroughputBeatsEitherSource`, provider and seed each limited to 768 KiB/s,
times include the provider probe and connecting the seed):

| Fixture | HTTPS only | Peers only | Both at once |
| --- | --- | --- | --- |
| 5.7 MiB (in every test run; three runs) | 8.0–8.4 s, 0.68–0.72 MiB/s | 10.0 s, 0.57 MiB/s | 5.6–6.1 s, 0.94–1.02 MiB/s |
| 17.2 MiB (`QBF_MEASURE_SCALE=60`) | 24.5 s, 0.70 MiB/s | 25.9 s, 0.66 MiB/s | 15.1 s, 1.14 MiB/s |

On the larger fixture the two sources together reach 84% of the sum of their separate rates, about 1.6
times either alone. The test asserts only that combined is at least 20% faster than the faster source.

Three test-harness faults were found and fixed on the way, none in the product: the test sessions
subscribed to every alert, so the per-peer debug log overflowed the queue and dropped piece verdicts (now
the same categories as the app); an outgoing uTP connection to the local seed could stay in its handshake
for seconds (the test sessions now use TCP); and the seed in `seed_mode` served no data for the hybrid
v1+v2 torrent (it now checks its files first). The comparison of output now allows libtorrent up to 5 s
to flush (spike Q1b).

**Limits.** Loopback only, one seed, 16 KiB pieces (one block per piece, so a piece partly fetched by a
peer and then finished by HTTPS is not exercised here; libtorrent's `add_piece` keeps such blocks, as
in the serialized hand-over). Not yet run against the real service or a real swarm. The `TorrentImpl`
parts (resume-data values, re-applying after priority changes, the deferred completion) are exercised
only through the app, not by an automated test.

## Live Real-Debrid observations (2026-11-11, one account, one torrent — not a general proof)

First run of the adapter against the real service: an installed build, one account, one torrent.

| Observation | Consequence |
| --- | --- |
| `GET /torrents` + `/torrents/info/{id}` found the torrent by info-hash; file paths bound exactly with the root folder stripped; `links` had one entry for the one selected file. | The list-by-hash lookup and the one-link-per-selected-file assumption held for this case. |
| `POST /unrestrict/link` returned two download hosts of the form `<site>.download.real-debrid.com`. | The `real-debrid.com` host suffix allowlist held. |
| A `Range: bytes=0-0` preflight got `206` with the right `Content-Range` but a `Content-Length` that was not 1 (value not captured). | Preflight now requests the first KiB. Piece requests never end at offset 0, except a 1-byte slice at the start of a file (untested). The strict `Content-Length` check is unchanged. |
| The same download link returned `503` three times in a row (56 s, then 127 s apart), with a 2251-byte error page. | A failed preflight now drops the cached link, so the next attempt unrestricts again (a new link may name another server). Whether that alone clears a `503` is **not established**: the attempt that succeeded changed the range, the link and the `User-Agent` at once. |
| After those changes: `PROBING_PROVIDER -> HTTP_ACTIVE (provider_ready)`; verified pieces rose from 570 to 587 in about 40 s at ~10 MiB/s with 0 peers connected. | The live smoke test of specification phase P5 is partly done. The torrent later completed over HTTPS (`HTTP_ACTIVE -> COMPLETED (download_complete)`). Link expiry during a transfer and the hand-over of leftover pieces to peers were not observed. |

Diagnostics added for this: a redacted failure detail in the log and the Source tooltip (scheme and host only, header sizes, and a summary of the server's error page with links, paths, tokens and addresses removed; `testhybridrangevalidation::errorPageSummaryIsRedacted`).

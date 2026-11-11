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

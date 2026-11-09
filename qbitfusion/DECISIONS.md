# qBitFusion decision log (spike evidence)

Template per docs/SPECIFICATION.md section 11. Reproduce with:

```powershell
pwsh qbitfusion/scripts/bootstrap-windows.ps1
pwsh qbitfusion/scripts/build-windows.ps1 -Spike
```

## Gate G0 | Baseline build — PASSED (2026-11-09)

- **Versions:** qBittorrent `release-5.3.0` @ `9213075d`, libtorrent 2.1.2 (`a610c6ef4`, static, `deprecated-functions=OFF` → `TORRENT_ABI_VERSION=100`), Qt 6.10.3 msvc2022_64, Boost 1.91.0 headers, OpenSSL 3.6.5 / zlib 1.3.2 (vcpkg `x64-windows-static-md-release`), MSVC 14.44.35207, CMake 4.4.3, Ninja.
- **Command:** `build-windows.ps1 -BuildDir build-baseline` (CI flags: `/DQT_FORCE_ASSERTS /WX`, `-DTESTING=ON`, `-DMSVC_RUNTIME_DYNAMIC=ON`).
- **Result:** `qbittorrent.exe` built with warnings-as-errors; `ctest` 22/22 passed (1.53 s).
- **Open:** manual launch with a normal torrent, not yet done.

## Gate G1 / first layer of G2 and G4 | libtorrent spike — PASSED 35/35 (2026-11-09)

Harness: `qbitfusion/spike/ltspike.cpp`. Fixtures: 3 files (100 000 / 70 001 / 130 000 bytes), 16 KiB pieces, generated deterministically; v1-only, hybrid (3 pad files) and v2-only variants. Isolated sessions on 127.0.0.1 with DHT/LSD/UPnP/NAT-PMP off. Each torrent carries an HTTP tracker and a known peer that point at local counting listeners.

| ID | Question (spec 11) | Observed (libtorrent 2.1.2) | Consequence |
|---|---|---|---|
| Q1 | Ordering after `add_piece` | `piece_finished_alert` 0.6–3 ms after submission (p50 0.6 ms v1, 1.3 ms v2); `have_piece()` already true when the alert is read; corrupt piece → `hash_failed_alert`, piece stays missing, and correct bytes re-submitted are accepted. | Coordinator marks Verified on `piece_finished_alert`; on `hash_failed_alert` it releases ownership and counts failures. |
| Q1b | Disk flush vs completion | After P2P completion, `is_finished` was true while the last file was still 98 304 / 100 000 bytes on disk. After `flush_cache()` + 2 s the files were identical, and `force_recheck` reported 19/19 valid. | Never read or move output on `is_finished` alone. Rely on qBittorrent's existing finish/move pipeline, which waits for libtorrent. Not data loss. |
| Q2 | Suppress P2P without priority games | A torrent added `paused` and not `auto_managed` ("transport hold") leaves checking (state `downloading`) and accepts `add_piece` for every piece. | **ADR-007 mechanism: serialized source mode.** HTTP runs only while the torrent is held; no per-piece priority overlay is needed, so the user's file priorities are never rewritten. |
| Q4 (part) | Swarm contact while held | Tracker listener: 0 connections, 0 announces; known-peer listener: 0 connections, across all variants, for the entire HTTP phase + 2 s. | First layer of G4 holds. **Still open:** magnet metadata path (`addTorrent_impl` / `downloadMetadata`), DHT-enabled sessions, packet capture. |
| Q5 | v2 / hybrid pad files and piece size | Hybrid: `map_block` returns pad slices (`pad_file_at`), zero-filled pieces verify. v2-only: `piece_size_for_req()` gives short end-of-file pieces; all 20/20 verify. Output byte-identical in both. | Assembler zero-fills only metadata pad slices; piece length comes from `piece_size_for_req()`. |
| E4 | Crash-safe resume | After `save_resume_data` + session teardown + `read_resume_data`, libtorrent reports exactly the 6 injected pieces as verified. | Sidecar never stores piece state (ADR-006 holds). |
| E5 | No re-download after failover | P2P fetched 201 697 bytes = exactly the missing bytes; output identical; infohash unchanged; one storage tree. | Verified-progress preservation is demonstrated for HTTP→P2P. |
| E6 | P2P→HTTP handover | `graceful_pause` → `torrent_paused_alert`; 0 peers; download queue empty afterwards (5 pieces in flight before). `add_piece` without `overwrite_existing` completed all 17 missing pieces; output identical. `add_piece` on a priority-0 piece was also accepted. | Recovery = graceful pause → wait for paused alert → reconcile queue → HTTP. No `overwrite_existing` anywhere. |

### Limits of this evidence (do not overstate)
- Single seed on loopback; no multi-peer endgame, no real WAN latency.
- G2 overlap (HTTP and P2P on different pieces *simultaneously*) was **not** attempted. The serialized mode makes it unnecessary for the MVP.
- Partial-piece handover after graceful pause was observed with 0 partial blocks left. A partial piece surviving a pause is not yet exercised.
- Windows only. Linux/macOS not run.

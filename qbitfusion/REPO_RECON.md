# REPO_RECON | qBittorrent release-5.3.0 vs. the qBitFusion spec

Task: CORE-001 (Phase P0). Recorded 2026-11-09. Facts below were read from the pinned source, not assumed.

## Baseline

| Item | Value |
|---|---|
| Upstream | `https://github.com/qbittorrent/qBittorrent.git`, remote `upstream` |
| Tag | `release-5.3.0` → commit `9213075dd5f7b352208361c6a84c1c115efc898e` (2026-10-19) |
| Working branch | `feature/hybrid-debrid` |
| Host | Windows 11 Pro 10.0.26300, MSVC 14.44.35207 (VS 2022 Build Tools 17.14), Windows SDK 10.0.26100, CMake 4.4.3, Ninja |
| Toolchain recipe | `qbitfusion/scripts/bootstrap-windows.ps1`, a copy of `.github/workflows/ci_windows.yaml` |

## Where the code differs from the docs

1. **One specification file.** The specification is `docs/SPECIFICATION.md`; its numbered sections are what "spec NN" refers to. `contracts/hybrid-status.schema.json` (spec 10) does not exist yet.
2. **libtorrent version.** `INSTALL` says libtorrent `1.2.19–1.2.x || 2.0.10–2.0.x` and `CMakeLists.txt` sets `minLibtorrentVersion 2.0.10`. `ci_windows.yaml` builds **2.1.2**, 2.0.15 and 1.2.20 with Boost 1.91 / Qt 6.10.3. The 2.1.x baseline is therefore CI-tested; `INSTALL` is stale. Code already branches on `LIBTORRENT_VERSION_NUM >= 20100` (e.g. `sessionimpl.cpp` `initializeNativeSession`).
3. **Spec folder proposal.** `src/base/hybrid/` does not exist upstream; it is new. The base library is `qbt_base` (`src/base/CMakeLists.txt`) and tests are QtTest executables listed in `test/CMakeLists.txt` (target `check`, enabled with `-DTESTING=ON`).

## Integration points (verified)

| Need (spec 06) | Upstream location | Notes |
|---|---|---|
| Alert routing | `SessionImpl::handleAlert` (`sessionimpl.cpp` ~6127) | Switch on alert type. **No `piece_finished_alert` / `hash_failed_alert` handling anywhere in `src/`.** |
| Alert mask | `SessionImpl::loadLTSettings` (`sessionimpl.cpp` ~1973) | Mask lacks `piece_progress_notification`, so `piece_finished_alert` is never posted. Hybrid must add it — only when the feature is enabled, to keep I7. `hash_failed_alert` is in `status_notification` (already on). |
| Per-torrent plugin seam | `NativeSessionExtension` / `NativeTorrentExtension` (`bittorrent/native*extension.*`) | Already an `lt::torrent_plugin`; `on_state` pauses torrents leaving metadata/checking. Candidate for hybrid hooks (`on_piece_pass`, `on_piece_failed`) without touching the alert mask. |
| Native handle bridge | `TorrentImpl` (`torrentimpl.cpp`) | Async libtorrent queries via `invokeAsync` (`torrentimpl.cpp` ~3068) on `SessionImpl::m_asyncWorker` (single thread). `fetchDownloadingPieces()` already wraps `get_download_queue`. No `add_piece` wrapper exists. |
| Stop / start | `TorrentImpl::stop` / `start` (`torrentimpl.cpp` ~2014/2033) | Pause = `unset auto_managed` + `pause()`. State is derived in `updateState()` from `m_isStopped` plus native flags, so a hybrid "transport hold" (native pause while HTTP runs) must not look like a user stop. |
| File priorities | `TorrentImpl::prioritizeFiles` (~3030) | Rewrites native file priorities and re-applies first/last piece priority; overwrites piece priorities (spec 03 §2 warning confirmed). |
| Add torrent | `SessionImpl::addTorrent_impl` (~2887) | Magnets go into libtorrent immediately (paused only if stopped/queued). **G4:** a `debrid_only` magnet must be intercepted before this point. |
| Metadata preview | `SessionImpl::downloadMetadata` (~3300) | The add-dialog adds a hidden `upload_mode` torrent to fetch magnet metadata from the swarm. **G4 leak path** for `debrid_only`; must be blocked when that source is chosen. |
| HTTP | `Net::DownloadManager` (`src/base/net/downloadmanager.*`, `downloadhandlerimpl.cpp`) | `DownloadRequest` has no custom headers (no `Range`), buffers the whole body (`readAll`), gunzips transparently, and enforces only a total size limit. **Insufficient** for the data plane, which justifies a dedicated `QNetworkAccessManager` range adapter (spec 05 exit clause). |
| Settings | `Preferences` (`src/base/preferences.*`), `SettingValue<T>` | Use for global hybrid keys. |
| Per-torrent persistence | Resume data via `BencodeResumeDataStorage` / `DBResumeDataStorage` | Hybrid uses an isolated sidecar instead (ADR-006). Older builds ignore it. |

## Hard assumptions still unproven (block P2+)

Each must be proven by the P1 spike (`qbitfusion/spike/`) against the built libtorrent 2.1.2:

- `add_piece` hashes and accepts pieces while the torrent is **paused** (the planned debrid-only / HTTP-active mechanism).
- A paused, non-auto-managed torrent makes zero tracker/DHT/peer contact (G4, first layer).
- Verified pieces survive save-resume → restart; a P2P resume fetches only missing pieces.
- `map_block` slices for a piece crossing a file boundary, and v2/hybrid pad-file slices.
- `graceful_pause` drains peer queues before HTTP takeover (G2 serialized-source fallback).

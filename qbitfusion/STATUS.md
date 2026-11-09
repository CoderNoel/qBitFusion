# qBitFusion status report

**Issue/task ID:** CORE-001…005, NET-001…003, PROVIDER-001/002 (adapter, not live-tested), STATE-001/002, SEC-001, SEC-002 (partial), UX-001, UX-002 (desktop only), QA-001 (partial).

**Baseline commit and environment:** qBittorrent `release-5.3.0` @ `9213075d` on branch `feature/hybrid-debrid` (uncommitted). Windows 11 Pro 10.0.26300, MSVC 14.44.35207, CMake 4.4.3/Ninja, Qt 6.10.3, libtorrent 2.1.2 (`a610c6ef4`, ABI 100), Boost 1.91.0, OpenSSL 3.6.5, zlib 1.3.2. Reproduce: `pwsh qbitfusion/scripts/bootstrap-windows.ps1`, then `pwsh qbitfusion/scripts/build-windows.ps1`.

## What changed

| Area | Files |
|---|---|
| Hybrid core (new, provider-neutral) | `src/base/hybrid/*`: types, strict Range validation, URL/SSRF guard, exact file binding, piece ownership arbiter, provider health/circuit breaker, state machine, policy sidecar, token bucket, range fetcher, piece assembler, coordinator, libtorrent layout adapter, manager, Windows credential store, Real-Debrid adapter |
| Upstream seams (small) | `bittorrent/hybridhooks.h` (new), `hybridtorrentsink.*` (new); `session.h/sessionimpl.*` (+hold at add/load, piece alerts, recheck hook); `torrentimpl.*` (transport hold through one `applyNativeRunState()`, HTTP rate, `addExternalPiece`); `app/application.cpp` (init/free) |
| GUI | `src/gui/hybrid/*` (new): Options page, source picker, consent prompt, device sign-in dialog; `optionsdialog.*`, `transferlistmodel.*`, `transferlistwidget.*`, `addnewtorrentdialog.*`, `guiaddtorrentmanager.cpp` |
| Evidence and tooling | `qbitfusion/REPO_RECON.md`, `DECISIONS.md`, `spike/`, `scripts/` |

## Why existing APIs were sufficient or insufficient

- libtorrent `add_piece`, `map_block`, `layout()`, `piece_size_for_req`, `graceful_pause` and resume data are sufficient (spike 35/35). No storage plugin, proxy or web seeds are used.
- `Net::DownloadManager` cannot set `Range`, buffers whole bodies and gunzips, so it is insufficient. A dedicated `QNetworkAccessManager` adapter was written; no new library.
- Credentials use Windows Credential Manager (`advapi32`, already a system library).

## Public interfaces changed

- `BitTorrent::Session`: `static setHybridHooks()/hybridHooks()`, `virtual setHybridAlertsEnabled()`.
- `TorrentImpl` (internal): `setTransportHold`, `isTransportHeld`, `setHybridHttpRate`, `hybridHttpRate`, `addExternalPiece`.
- `TransferListModel::TR_SOURCE` column appended (saved header state stays compatible).
- Settings keys `Hybrid/*`. Per-torrent sidecars `<profile>/data/hybrid/<id>.hybrid.json` (non-secret).

## Tests added and commands/results

`pwsh qbitfusion/scripts/build-windows.ps1` gives **32/32 ctest suites passing** (22 upstream + 10 hybrid), built with CI flags `/WX /DQT_FORCE_ASSERTS`.

- Unit suites: range parsing + 20k-case fuzz, URL guard, binding, ownership (20k random ops), health, state machine (50k-event `debrid_only` privacy property), sidecar, token bucket, range fetcher against poisoned local responses.
- `testhybridcoordinator` (real libtorrent session, local CDN + fixture provider), 9/9 cases, 5 repeated runs stable:
  - HTTP-only v1 / hybrid+pads / v2: byte-identical, memory cap held, 0 tracker/peer connections.
  - Corrupt provider bytes: rejected by hash, quarantined.
  - No consent: never releases the hold.
  - 503 at ~30%: P2P fetches only missing bytes.
  - Repeated outage backs off (busy-loop regression).
- Spike `scripts/build-windows.ps1 -Spike`: 35/35.
- Manual (isolated profiles, DHT/LSD/PeX/UPnP off):
  - Feature on: torrent added held, Source column and Options page render, restart re-holds before load.
  - Feature off: stock add dialog, 0 hybrid log lines, 0 sidecars.

## Safety / security / privacy

- `debrid_only` torrents are held from creation and on restart. The add-dialog magnet metadata preview is gated by the chosen source.
- Bearer tokens go only to `api.real-debrid.com`, with redirects never followed. CDN URLs are HTTPS-only with a host allowlist, private/loopback/link-local IPs blocked, and are redacted in errors.
- No secrets in settings, logs or sidecars. Transition logs carry no names, hashes or URLs.
- `overwrite_existing` is never used.

## Risks / unknowns / unverified claims

1. **Real-Debrid is not live-tested** (no credentials used). Assumed or unverified: the download host suffix `real-debrid.com`, the `/torrents` list-by-hash lookup, the `links` order (each link is validated by filename and size, so a wrong guess fails closed), and the OAuth endpoints as documented.
2. **TorBox not implemented**: official docs could not be read (JavaScript-only pages).
3. **G4 is not release-proven**: no packet capture, DHT-enabled session or WebUI/RSS/watch-folder add paths tested. Those paths do pass through `addTorrent_impl` and the hold hook.
4. A recheck of a *held* torrent may not progress if libtorrent refuses to check paused torrents. Untested.
5. Fail closed but limiting: pieces spanning a file the provider doesn't expose (T05) put the whole torrent on "Unmatched torrent files". v2-only torrents are unsupported on Real-Debrid.
6. The provider data plane does not use qBittorrent's proxy settings; it connects directly (the UI does not yet state this).
7. Non-Windows credential storage is memory-only. Linux/macOS builds were not run.
8. WebUI/WebAPI extension (spec 10) is not implemented.

## Next work package unblocked

Live Real-Debrid smoke test with a developer-owned lawful torrent (P5 "Done"), then the P6 packet-level G4 harness, then WebAPI and TorBox (P7/P8).

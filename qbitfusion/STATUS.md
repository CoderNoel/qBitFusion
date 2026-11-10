# qBitFusion status report

**Issue/task ID:** CORE-001…005, NET-001…003, PROVIDER-001/002 (adapter; one live run, see DECISIONS.md), STATE-001/002, SEC-001, SEC-002 (partial), UX-001, UX-002 (desktop only), QA-001 (partial).

**Baseline commit and environment:** qBittorrent `release-5.3.0` @ `9213075d` on branch `feature/hybrid-debrid` (uncommitted). Windows 11 Pro 10.0.26300, MSVC 14.44.35207, CMake 4.4.3/Ninja, Qt 6.10.3, libtorrent 2.1.2 (`a610c6ef4`, ABI 100), Boost 1.91.0, OpenSSL 3.6.5, zlib 1.3.2. Reproduce: `pwsh qbitfusion/scripts/bootstrap-windows.ps1`, then `pwsh qbitfusion/scripts/build-windows.ps1`. Install for the current user (Release build, Start menu + desktop shortcuts, Settings > Apps entry, isolated portable profile): `pwsh qbitfusion/scripts/install-windows.ps1`.

## What changed

| Area | Files |
|---|---|
| Hybrid core (new, provider-neutral) | `src/base/hybrid/*`: types, strict Range validation, URL/SSRF guard, exact file binding, piece ownership arbiter, provider health/circuit breaker, state machine, policy sidecar, token bucket, range fetcher, piece assembler, coordinator, libtorrent layout adapter, manager, Windows credential store, Real-Debrid adapter |
| Upstream seams (small) | `bittorrent/hybridhooks.h` (new), `hybridtorrentsink.*` (new); `session.h/sessionimpl.*` (+hold at add/load, piece alerts, recheck hook); `torrentimpl.*` (transport hold through one `applyNativeRunState()`, HTTP rate, `addExternalPiece`); `app/application.cpp` (init/free) |
| GUI | `src/gui/hybrid/*` (new): Options page, source picker, consent prompt, device sign-in dialog; `optionsdialog.*`, `transferlistmodel.*`, `transferlistwidget.*`, `addnewtorrentdialog.*`, `guiaddtorrentmanager.cpp` |
| Modern appearance and shell | `src/gui/modern/*` (new): neutral light/dark palettes with one user-chosen accent (Options > Behavior, ember orange by default), one stylesheet with per-scheme tokens, an original 108-icon monochrome set tinted at paint time, the app mark (`brand/`, drawn to `qbitfusion/design/logo/brief.md` and traced as vector), and shell widgets (page switcher, sidebar rows, empty state, accent picker). `mainwindow.*`: header (brand, Transfers/Search/RSS/Log switcher, search, Add, quick toggles, overflow menu holding every former menu) and an action bar over the list; the menu bar is hidden and its actions are registered on the window so shortcuts keep working. Also touched: `uithememanager.cpp` (icons, colours, style), `hidabletabwidget.*`, `statusbar.cpp`, `transferlist*`, `transferlistfilters*`, `properties/*`, `progressbarpainter.cpp`, `optionsdialog.*`. Classic look: Options > Behavior toggle, applies after restart. |
| Rebrand | User-visible name is qBitFusion: window titles (application display name), About, tray, notifications, dialogs, `qbittorrent.rc` metadata and icon, executable `qbitfusion.exe`. Settings and profile folder names are unchanged. About credits qBittorrent; crash dialogs say not to report to upstream; upstream's update check is disabled (it would install stock qBittorrent). |
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

`pwsh qbitfusion/scripts/build-windows.ps1` gives **32/32 ctest suites passing** (22 upstream + 10 hybrid), built with CI flags `/WX /DQT_FORCE_ASSERTS`. Last full run: 2026-11-11, on the tree described here.

- Unit suites: range parsing + 20k-case fuzz, URL guard, binding, ownership (20k random ops), health, state machine (50k-event `debrid_only` privacy property), sidecar, token bucket, range fetcher against poisoned local responses.
- `testhybridcoordinator` (real libtorrent session, local CDN + fixture provider), 9/9 cases, 5 repeated runs stable:
  - HTTP-only v1 / hybrid+pads / v2: byte-identical, memory cap held, 0 tracker/peer connections.
  - Corrupt provider bytes: rejected by hash, quarantined.
  - No consent: never releases the hold.
  - 503 at ~30%: P2P fetches only missing bytes.
  - Repeated outage backs off (busy-loop regression).
  - Provider lacks one file (ADR-009): HTTP verifies exactly the fully covered pieces, then peers fetch only the rest (`debrid_preferred`), or the torrent stops with `provider_lacks_pieces` and no swarm contact (`debrid_only`).
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

1. **Real-Debrid has one live run only** (2026-11-11, the user's account and one torrent; details in `DECISIONS.md`). It reached `HTTP_ACTIVE` and verified pieces at about 10 MiB/s with no peers connected. Seen to hold in that run: the `real-debrid.com` download host suffix, the `/torrents` list-by-hash lookup, one link for one selected file. The same torrent later completed over HTTPS (`HTTP_ACTIVE -> COMPLETED (download_complete)` in the app log, 2026-11-11 04:48). Not observed: several selected files (the `links` order), link expiry mid-transfer, the OAuth refresh. A link is now accepted on exact size when that size is unique among the selected files; the name is required only to tell equal-sized files apart. Download servers answered `503` repeatedly for one link and mishandled a 1-byte range; the fixes for both went in together, so which one mattered is not established.
2. **TorBox not implemented**: official docs could not be read (JavaScript-only pages).
3. **G4 is not release-proven**: no packet capture, DHT-enabled session or WebUI/RSS/watch-folder add paths tested. Those paths do pass through `addTorrent_impl` and the hold hook.
4. A recheck of a *held* torrent may not progress if libtorrent refuses to check paused torrents. Untested.
5. Pieces that include data from a file the provider doesn't expose (T05) come from peers after the provider part is done (ADR-009). Under `debrid_only` such a torrent cannot complete: it stops with "Remaining pieces are not on the provider" once the provider part is verified. It does not say so up front. v2-only torrents are unsupported on Real-Debrid.
6. The provider data plane does not use qBittorrent's proxy settings; it connects directly (the UI does not yet state this).
7. Non-Windows credential storage is memory-only. Linux/macOS builds were not run.
8. WebUI/WebAPI extension (spec 10) is not implemented.
9. Redesign: strings that now say qBitFusion, and all new strings, are English only; other languages fall back to English for them. Checked by screenshot on Windows at 200% scaling in dark and light: main window (Transfers, RSS), Options (Behavior, Hybrid sources), the main and Add menus, the empty state. Not inspected: Search and Log pages in detail, torrent creator, statistics, add-torrent dialog after the restyle, 100% scaling, keyboard shortcuts with the menu bar hidden (registered on the window by design, not exercised).

10. Cosmetic, hybrid only: the "Downloaded" and "this session" totals do not count bytes received over HTTPS (seen live: 67.3% of 80 GiB verified, "Downloaded: 9.16 GiB (0 B this session)"). The ETA now adds the HTTPS rate; it has no test of its own. Checked once in the installed copy by screenshot (2026-11-11): 8.3 MiB/s, 32.7% of 80.43 GiB left, ETA 53m.
11. The Source column is moved next to Status once, the first time the feature is in use (`GUI/HybridSourceColumnPlaced`). Checked in the installed copy by screenshot.

12. UI polish pass (2026-11-11), checked by screenshot in the test build at 150% scaling, dark scheme only: list progress bar, status-bar speed-limit icon, add-torrent dialog, header at narrow widths, menus, sidebar, properties panel, Options pages (Behavior, Downloads, Hybrid sources, Speed, Advanced). Changed but not checked by screenshot: Statistics, Torrent Creator, About, delete confirmation, download-from-URL, RSS warning, sliders, the light scheme. Known and left: Title Case labels inherited from upstream, uneven label/field alignment inside some Options rows, last column cut on the Trackers and Peers tabs, framing of the Search/RSS/Log pages, the properties grid at very narrow widths.

## Next work package unblocked

Finish the Real-Debrid live checks listed in risk 1 (P5 "Done"), then the P6 packet-level G4 harness, then WebAPI and TorBox (P7/P8).

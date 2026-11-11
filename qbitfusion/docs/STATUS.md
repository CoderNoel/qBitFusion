# Project status

What is built, how it was verified, and what is still open. The [README](../../README.md) has the short
version; design decisions and raw observations are in [DECISIONS.md](DECISIONS.md).

Last reviewed: 2026-11-11.

## Baseline

- qBittorrent `release-5.3.0` (commit `9213075dd`).
- Built and tested with: Windows 11, MSVC 14.44, CMake 4.4 with Ninja, Qt 6.10.3, libtorrent 2.1.2
  (`deprecated-functions=OFF`), Boost 1.91.0, OpenSSL 3.6, zlib 1.3.
- Reproduce with `pwsh qbitfusion/scripts/bootstrap-windows.ps1`, then
  `pwsh qbitfusion/scripts/build-windows.ps1`.

## What is built

| Area | Where | Contents |
| --- | --- | --- |
| Hybrid core | `src/base/hybrid/` | Types, strict Range validation, URL guard, exact file binding, piece ownership arbiter, provider health and back-off, the transport state machine, per-torrent policy sidecar, token bucket, range fetcher, piece assembler, coordinator, libtorrent layout adapter, manager, Windows credential store, Real-Debrid adapter. |
| Hooks in qBittorrent's core | `src/base/bittorrent/` | `hybridhooks.h` and `hybridtorrentsink.*` (new). `session.h`, `sessionimpl.*`: hold at add and load, piece alerts, recheck hook. `torrentimpl.*`: transport hold, HTTPS rate, `addExternalPiece`. `app/application.cpp`: start-up and shutdown. |
| Hybrid GUI | `src/gui/hybrid/` | Options page, source picker, consent prompt, device sign-in dialog, the Source column and its context menu. |
| Modern look | `src/gui/modern/` | Light and dark palettes with a configurable accent, one stylesheet, a 108-glyph monochrome icon set tinted at paint time, the app mark, and the shell widgets (page switcher, sidebar rows, empty state, accent picker). `mainwindow.*` builds the header and action bar; the menu bar is hidden and its actions stay registered on the window so shortcuts keep working. |
| Naming | throughout the GUI | The user-visible name is qBitFusion and the executable is `qbitfusion.exe`. Settings and profile folder names are unchanged from qBittorrent. Upstream's update check is disabled, because it would offer stock qBittorrent. |
| Tooling | `qbitfusion/` | Windows scripts (toolchain, build, install, package, test instance, window capture), the libtorrent feasibility spike, and a branding check. |

## Interfaces changed in upstream code

These are the points to re-check when merging a new upstream release (see [UPSTREAM.md](UPSTREAM.md)).

- `BitTorrent::Session`: `static setHybridHooks()` / `hybridHooks()`, `virtual setHybridAlertsEnabled()`.
- `TorrentImpl` (internal): `setTransportHold`, `isTransportHeld`, `setHybridHttpRate`, `hybridHttpRate`,
  `addExternalPiece`.
- `TransferListModel::TR_SOURCE`: a column appended at the end, so saved header state stays compatible.
- Settings keys under `Hybrid/`. Per-torrent sidecars in `<profile>/data/hybrid/<id>.hybrid.json`, which
  hold no secrets.

Why new code was needed: `Net::DownloadManager` cannot send `Range`, buffers whole bodies and
decompresses transparently, so the data path has its own `QNetworkAccessManager` adapter. libtorrent's
`add_piece`, `map_block`, `piece_size_for_req`, `graceful_pause` and resume data were sufficient as they
are. No library was added.

## How it was verified

**Automated.** `pwsh qbitfusion/scripts/build-windows.ps1` builds with warnings as errors and
`QT_FORCE_ASSERTS`, and runs 32 test suites (22 from qBittorrent, 10 for the hybrid code). All pass.

- Unit suites: range parsing with a 20 000-case fuzz, URL guard, file binding, piece ownership (20 000
  random operations), provider health, the state machine (including a 50 000-event check that
  `debrid_only` never permits P2P), sidecar, token bucket, and the range fetcher against hostile local
  responses.
- `testhybridcoordinator` drives a real libtorrent session against a local HTTP server and a fixture
  provider:
  - HTTPS-only download of v1, hybrid (with pad files) and v2 torrents: output byte-identical, memory
    bound held, no tracker or peer connections.
  - Corrupt provider bytes: rejected by hash.
  - No consent: the torrent is never released to the swarm.
  - A `503` at about 30%: peers fetch only the missing bytes.
  - Repeated outage: the coordinator backs off instead of looping.
  - Provider lacks one file: HTTPS verifies exactly the fully covered pieces, then peers fetch the rest
    (`debrid_preferred`), or the torrent stops with `provider_lacks_pieces` and no swarm contact
    (`debrid_only`).
- The libtorrent spike (`build-windows.ps1 -Spike`) passes 35 of 35 checks; its findings are in
  DECISIONS.md.

**Manual, with isolated profiles.** Feature on: a torrent is held when added and again after a restart;
the Source column and the Options page work. Feature off: the stock add dialog, no hybrid log lines, no
sidecars.

**Live.** One Real-Debrid account and one torrent of about 80 GiB (2026-11-11). It reached `HTTP_ACTIVE`,
verified pieces at about 10 MiB/s with no peers connected, and later completed over HTTPS. The details,
including two problems found and fixed on the way, are in DECISIONS.md.

**Continuous integration.** The Windows workflow builds the same configuration on a GitHub runner and runs
the same 32 suites; it passes. The Python, WebUI and file-health checks inherited from qBittorrent pass. The Linux and
macOS workflows are set to manual, because this code has not been built there yet.

## Privacy and security properties

- A `debrid_only` torrent is held (paused, not auto-managed) from the moment it is created and again on
  restart. The magnet metadata preview in the add dialog only runs when the chosen source allows P2P.
- Bearer tokens are sent only to `api.real-debrid.com`, and redirects are never followed there. Download
  URLs must be HTTPS on an allow-listed host; private, loopback and link-local addresses are refused.
- No secrets in settings, logs or sidecars. Source-change log lines carry no names, hashes or URLs; a
  failure detail carries the scheme and host, header sizes and a redacted summary of the server's error page.
- `overwrite_existing` is never used.

## Known gaps and unverified claims

Hybrid transport:

1. **One live run only.** Not yet observed against the real service: several selected files in one torrent
   (the order of `links`), a link expiring during a transfer, the OAuth refresh, and creating a torrent on
   the account from the app. A download link is accepted on exact size when that size is unique among the
   selected files; the name is only needed to tell equal-sized files apart. Download servers answered
   `503` repeatedly for one link and mishandled a one-byte range; the fixes for both went in together, so
   which one mattered is not established.
2. **Only one provider.** Real-Debrid. v2-only torrents cannot use it.
3. **No packet-level proof for `debrid_only`.** There is no packet capture, no run with DHT enabled, and
   the Web UI, RSS and watched-folder add paths were not exercised. Those paths do go through the same
   add function and hold hook.
4. **Recheck of a held torrent** may not progress if libtorrent declines to check a paused torrent.
   Untested.
5. **Partial provider coverage under `debrid_only`.** The download stops with "Remaining pieces are not on
   the provider" once the provider part is verified. It does not say so at the start.
6. **Proxy settings are not used** by the provider data path; it connects directly, and the interface does
   not say so yet.
7. **Other platforms.** Linux and macOS have not been built. Credential storage outside Windows is
   memory-only.
8. **Web UI and Web API** have no hybrid controls.
9. **Statistics.** The "Downloaded" and session totals do not count bytes received over HTTPS. The ETA
   includes the HTTPS rate, but has no test of its own.
10. **Parallelism.** Four parallel HTTPS requests per torrent by default
    (`Hybrid/MaxHttpPiecesPerTorrent`). Whether that limits fast connections has not been measured.

Interface:

11. **Review coverage.** Checked by screenshot at 150% scaling in the dark scheme: main window, menus,
    sidebar, properties panel, add-torrent dialog, and the Behavior, Downloads, Hybrid sources, Speed and
    Advanced pages of Options. Changed since their last check: Statistics, Torrent Creator, About, the
    delete confirmation, download-from-URL, the RSS warning, sliders, and the light scheme. Not checked at
    100% scaling.
12. **Left as inherited.** Title Case labels, uneven label and field alignment in some Options rows, the
    last column cut off on the Trackers and Peers tabs, the framing of the Search, RSS and Log pages, and
    the properties grid at very narrow widths.
13. **Translations.** Text that now says qBitFusion, and all new text, is English only; other languages
    fall back to English for it.
14. **One-time layout changes.** The first time the feature is used, the Source column is placed next to
    Status and the Name column is widened (`GUI/HybridSourceColumnPlaced`, `GUI/ModernNameColumnSized`).

Distribution:

15. No installer, no code signing, no update check.

## Next

Finish the live Real-Debrid checks in item 1, then the packet-level check for `debrid_only`, then the
Web API and a second provider.

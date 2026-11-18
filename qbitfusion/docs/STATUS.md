# Project status

What is built, how it was verified, and what is still open. The [README](../../README.md) has the short
version; design decisions and raw observations are in [DECISIONS.md](DECISIONS.md).

Last reviewed: 2026-11-18.

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
| Direct downloads | `src/base/directdownload/`, `src/gui/directdownload/` | Link and content classification, file naming, redirect rules (`linkclassifier.*`); the link check and the resumable transfer (`httptransfer.*`); queue, retries, persistence and credential storage (`directdownloadmanager.*`). GUI: the Links page, its model, and the dialog that checks pasted links. `mainwindow.*` and `app/application.cpp` route http(s) links through the check. |
| Welcome tour | `src/gui/onboarding/` | Five pages with drawn, animated illustrations: what is new, how hybrid downloads work, connecting Real-Debrid (device sign-in on the page, then a check that the account answers, then the default source), Links, and where everything is. Shown once per `Onboarding::TOUR_VERSION` (setting `GUI/OnboardingVersion`), from `MainWindow::showEvent()`; Help > Welcome tour opens it again. |
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
- Direct downloads: `GUIAddTorrentManager::addTorrentData()` (new, adds a `.torrent` that the link check
  already downloaded). `MainWindow::downloadFromURLList()`, the main window's paste handler, links dropped on the
  transfer list (`TransferListWidget::dropEvent()`) and `Application::processParams()` send http(s) links without `.torrent` through `MainWindow::addLinks()`;
  magnets, hashes, files and `.torrent` links take the upstream path unchanged. `Modern::EmptyState`
  gained `setTexts()`. The list is saved in `<profile>/data/directdownloads.json` (no links); settings keys
  `DirectDownload/SavePath`, `DirectDownload/AddDialogSize`, `DirectDownload/ListHeaderState`. Links live
  in the Credential Manager under `qBitFusion/directdownload/<id>` until the file is complete.

Why new code was needed: `Net::DownloadManager` cannot send `Range`, buffers whole bodies and
decompresses transparently, so the data path has its own `QNetworkAccessManager` adapter. libtorrent's
`add_piece`, `map_block`, `piece_size_for_req`, `graceful_pause` and resume data were sufficient as they
are. No library was added.

## How it was verified

**Automated.** `pwsh qbitfusion/scripts/build-windows.ps1` builds with warnings as errors and
`QT_FORCE_ASSERTS`, and runs 34 test suites (22 from qBittorrent, 10 for the hybrid code, one for the
self-updater, one for direct downloads). All pass.

- `testdirectdownload` (42 cases) runs against a local HTTP server: input and content classification (a
  torrent served as `application/octet-stream` without a `.torrent` extension, HTML pages, magnet
  redirects), Content-Disposition parsing including path traversal, Windows-safe and unique names, the
  redirect rules, that refusals report only the host, and transfers: whole, resumed with `Range` and
  `If-Range`, a server that ignores ranges, a changed ETag, a changed size, a dropped connection that keeps
  its bytes and resumes, an already complete file, a compressed body, and the speed limit. The manager
  tests cover completion (the link is then deleted from the credential store and the saved list holds no
  link), unique names, pause and resume, resume after a restart from the credential store, the "link
  needed" state, retry after a dropped connection, and an expired link.

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

Direct downloads:

- A link is shown, logged and saved as its host only. Error details carry the scheme and host, the HTTP
  status and a redacted summary of the server's error page. Qt's own error strings are not used, because
  they can contain the full URL.
- Until a file is complete its link is kept in the Credential Manager, then deleted. Without a persistent
  store (other platforms, or a link longer than 2,560 bytes) the item asks for the link again after a
  restart.
- A pasted link may point anywhere over http or https, since the user chose it. Redirects may not go from
  https to http, nor from a public host into the local network (loopback, private, link-local,
  single-label and `.local` names). DNS answers are not checked, so a public name that resolves to a
  private address is not caught.

Hybrid transport:

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

15. No installer and no code signing.
16. **Self-update** (2026-11-13): unit tests cover the version order, the Ed25519 signature check, the
    rejection of unsigned, altered or foreign-hosted update descriptions, and the file swap that keeps the
    profile. The whole path (download, unpack, install at start-up, restart) was tried once on this
    machine against a local test server. The Release and upstream-sync workflows had not run on GitHub yet
    when this was written. A version that installs but then fails to start is not rolled back.

Direct downloads:

17. **No live debrid run.** Tested against the local test server and, in a test instance, against public
    file servers (download.blender.org, archive.org with its redirects), including pause, quit and
    restart; both files resumed across the restart matched archive.org's MD5. Not yet with a real Stremio
    add-on link or a Real-Debrid download link.
18. **One connection per file.** A rough measurement on 2026-11-18 (while a build was running): a public
    server in France served about 4 MiB/s on one connection and about 6.3 MiB/s on four. Splitting a file
    into parallel ranges is open work; it needs the segment state saved for resume and must respect the
    provider's connection limits.
19. **No checksum.** Only the size and the ETag are checked. Writes are not flushed to the disk with
    `fsync`, so after a power cut the end of a partial file may hold zeros; the download then continues
    after them.
20. **Fixed limits.** Three downloads at a time, a retry schedule of 5 s to 5 min with six attempts, and
    the add dialog always shown. None of this is in Options yet. The status bar, the speed graph and the
    session statistics count torrent traffic only; the Links page shows its own total speed.
21. **Other add paths.** The Web UI, the Web API, RSS and watched folders still treat every link as a
    torrent.
22. **Placeholder videos.** Some add-ons redirect to a short "still caching" video when the debrid service
    does not have the file yet. That video is downloaded like any other file.

Welcome tour:

23. **Welcome tour states not all seen.** Checked by screenshot: every page, and the connected state with
    a real account (verified, debrid sources off). The not-connected, waiting-for-approval and error
    states of the Real-Debrid step were not shown on screen, because test instances share the Real-Debrid
    login in the Credential Manager with the installed copy, and signing out there would remove it.
    They reuse the same provider calls as the sign-in dialog in Options.
24. **Test instances share the provider login.** The Credential Manager entries are per Windows user, not
    per profile, so a test instance uses the same Real-Debrid account as the installed copy.

## Next

Finish the live Real-Debrid checks in item 1, then the packet-level check for `debrid_only`, then the
Web API and a second provider.

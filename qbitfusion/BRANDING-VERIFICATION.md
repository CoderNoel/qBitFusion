# qBitFusion branding verification

Issue/task ID: BRAND-001 (local task), approved orange qf identity and Windows shell integration.

Baseline commit and environment: `f68ae5841bd81114c3331b4f511bab7beaa01b63`,
`release-5.3.0-2-gf68ae5841`, on an already modified working tree. Windows 11 Pro
10.0.26300; MSVC 19.44.35229.0 (VC tools 14.44.35207); Qt 6.10.3.0 (installed
DLL version checked); libtorrent 2.1.2.0 (pinned installed headers checked);
CMake 4.4.3; ImageMagick 7.1.2-31 Q16-HDRI. Recorded 2026-11-11.

## What changed

- Kept the approved artwork in `design/logo/refinement-04/qf-upstream-style.png`.
  `scripts/export-branding.ps1` exports a 1024px transparent PNG, a 15-resolution
  ICO and white/dark monochrome tray variants into `src/gui/modern/brand/`.
  The export removes low-alpha specks outside the disc without redrawing its
  lettering or gradient. The original artwork is retained.
- Qt uses the approved PNG for application, title-bar, toolbar, About and normal
  tray branding. Only brand icons bypass the modern-appearance setting; ordinary
  classic/custom-theme icons retain their existing selection behavior.
- The Windows executable embeds the new ICO. An explicit CMake resource dependency
  ensures changing the ICO rebuilds the RC object during incremental builds.
- The installer ships a standalone ICO and points Start menu/desktop shortcuts
  and Settings > Apps at it. It notifies Windows Shell of the replaced items,
  without clearing the icon cache or restarting Explorer.
- Rebuilt and reinstalled the existing per-user installation, then restarted it.
  Existing portable profile contents are excluded from the installer replacement.

## Existing APIs and public interfaces

Existing Qt `QIcon`, qrc resources and Windows ICON resources were sufficient.
Existing installer COM shortcuts and the system `SHChangeNotify` API handle shell
updates. No new application dependency, public C++ interface, torrent lifecycle
path, transport/provider interface, file association or application ID changed.
ImageMagick is an export-time tool already installed on this machine.

## Pinned source references inspected

- `src/qbittorrent.rc:3`: executable icon group, already pointed to the old brand ICO.
- `src/app/CMakeLists.txt:126`: Windows resource compilation and new ICON dependencies.
- `src/app/application.cpp:920`: application window icon from UIThemeManager.
- `src/gui/modern/modernappearance.cpp:505`: color/monochrome brand lookup.
- `src/gui/uithememanager.cpp:222`: shared Qt icon selection/cache and classic-mode behavior.
- `src/gui/aboutdialog.cpp:60`: About logo uses the same theme icon.
- `src/gui/modern/modernwidgets.cpp:190`: header uses the same theme icon.
- `qbitfusion/scripts/install-windows.ps1:76`: installed standalone icon asset.

## Tests added and commands/results

`tests/branding/` is a standalone Qt/native Windows fixture, independent of torrent
data. It checks nonempty rendered alpha, transparent corners, orange identity,
Windows extraction at every ICO size and Windows Shell shortcut resolution.

The meaningful failure reproduced before the CMake dependency fix: the standalone
new ICO passed, but the freshly linked executable still supplied an invisible
16px icon; the fixture exited **5**. Rebuilding its RC object resolved this.

Commands below run from the repository root. Building requires an x64 MSVC developer
PowerShell, as initialized by `scripts/build-windows.ps1`.

```powershell
pwsh qbitfusion/scripts/export-branding.ps1
$qtBrandingPath = (Get-Content .deps/toolchain.json | ConvertFrom-Json).qt
cmake -S qbitfusion/tests/branding -B build/branding -G Ninja `
  "-DCMAKE_PREFIX_PATH=$qtBrandingPath" -DCMAKE_BUILD_TYPE=Release
cmake --build build/branding
pwsh qbitfusion/scripts/build-windows.ps1 -Release
$env:PATH = "$qtBrandingPath/bin;$env:PATH"
$env:QT_FORCE_STDERR_LOGGING = '1'
& build/branding/branding_checks.exe -platform offscreen `
  src/gui/modern/brand/qbitfusion.ico build-release/qbitfusion.exe
```

Results: export/configure/build/check commands succeeded. Qt rendered all three
resources at 15 sizes and six display scales (1, 1.25, 1.5, 2, 3 and 4): **270
passing render checks**. Native Windows extracted the new ICO and embedded EXE
icon correctly at all 15 sizes: **30 passing extraction checks**.

Sizes: 16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128 and 256px.
These cover the Win32 table for title bar/tray, taskbar/search/app list and Start
pins at 100–400% scaling in [Microsoft's icon construction guidance](https://learn.microsoft.com/en-us/windows/apps/design/iconography/app-icon-construction).

After `pwsh qbitfusion/scripts/install-windows.ps1 -NoBuild` (exit 0):

```powershell
& build/branding/branding_checks.exe -platform offscreen `
  "$env:LOCALAPPDATA/Programs/qBitFusion/qbitfusion.ico" `
  "$env:LOCALAPPDATA/Programs/qBitFusion/qbitfusion.exe" `
  (Join-Path ([Environment]::GetFolderPath('Programs')) 'qBitFusion.lnk') `
  (Join-Path ([Environment]::GetFolderPath('Desktop')) 'qBitFusion.lnk')
Get-StartApps | Where-Object Name -eq 'qBitFusion'
```

Results: **exit 0**, 30 installed-file extraction checks and both small/large
shell-resolved images for both shortcuts passed. Both shortcuts target the
installed EXE and use `qbitfusion.ico,0`. Start app registration resolves to
`%LOCALAPPDATA%\Programs\qBitFusion\qbitfusion.exe`.
Installed/source ICO hashes match; installed/built EXE hashes match.

- ICO SHA-256: `C9CD33F5055EE7908B319923419F24406F6EB7A1C0E3CEA41777EE16B1D43A03`.
- EXE SHA-256: `BF28A2F35BB4DAC2F85E560C4B30BC48B043938BB333AE97593C21F6610BAF55`.

The export/installer scripts pass PowerShell parser checks. `git diff --check`
passes for modified tracked integration files. Release compilation passed the
existing `/WX` configuration. Existing CMake/private-Qt and deployment warnings
about optional Vulkan, translations and DX compiler components remain.

## Feature-off regression and manual protocol

Observed the updated installed application's title-bar and header logos. Used a
separate empty profile at `build/branding-classic-profile` with configuration
`branding-check`, `Appearance/ModernUI=false`, `Hybrid/Enabled=false`, and
DHT/LSD/PeX/port forwarding disabled. Observed the classic toolbar/menu UI and
the new title-bar and About logos; zero torrents and no hybrid sidecars or hybrid
transition messages. Closed that test instance normally with Ctrl+Q; its log
records completed resume saving and session shutdown. The regular installed
instance remains running, and stock qBittorrent PID 1884 was never stopped.

This is a branding/feature-off smoke check, not a new transport, privacy-packet
capture or upstream behavioral-equivalence claim. No torrent lifecycle path
was modified, so the broader transport fixture suite was not rerun.

## Safety, risks and remaining limits

No provider tokens, signed URLs or credentials were read or changed. No torrent
content, storage paths, privacy permissions, file associations or user settings
were edited. Installation was restricted to the existing qBitFusion program
directory and shortcuts. The isolated test profile remains under ignored `build/`.

Windows Shell extraction and app registration are verified. Direct screenshots
of the Start/Search panels were unavailable through the computer-use window
inventory, so their live cached presentation is not claimed as visually inspected.
If a particular pre-existing pin retains an old cached image, check it manually;
unpin/re-pin that shortcut once if necessary. New pins resolve the updated shortcut
and the EXE independently carries the correct icon.

Non-Windows native packaging and separately installed custom `.qbtheme` files
were not exercised. PNG branding is raster artwork, not a vector reconstruction.

Next work package unblocked: continued interface work using the approved qf
identity. No provider/core integration work is part of this task.

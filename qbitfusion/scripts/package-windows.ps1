<#
.SYNOPSIS
  Build a portable Windows package of qBitFusion: one zip that runs after extracting, with the Qt and
  MSVC runtimes beside the program. Nothing from any user profile goes into it.

.DESCRIPTION
  The package contains an empty "profile" folder. Its presence puts the program in portable mode, so
  it keeps its settings there and never reads or changes a stock qBittorrent profile in %APPDATA%.
  Output: <OutDir>\qBitFusion-<Version>-windows-x64.zip and a .sha256 file next to it.

.EXAMPLE
  pwsh qbitfusion/scripts/package-windows.ps1 -Version 0.1.0-preview
  pwsh qbitfusion/scripts/package-windows.ps1 -Version 0.1.0-preview -NoBuild   # package the existing Release build
#>
param(
    [Parameter(Mandatory)] [string]$Version,
    [string]$BuildDir = "build-release",
    [string]$OutDir = "build-package",
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$tc = Get-Content (Join-Path $repo ".deps/toolchain.json") | ConvertFrom-Json
$build = Join-Path $repo $BuildDir

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "build-windows.ps1") -Release -BuildDir $BuildDir
}
$builtExe = Join-Path $build "qbitfusion.exe"
if (-not (Test-Path $builtExe)) { throw "No Release build at $builtExe. Run without -NoBuild." }

$name = "qBitFusion-$Version-windows-x64"
$out = Join-Path $repo $OutDir
$stage = Join-Path $out $name
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $stage -Force | Out-Null

Copy-Item $builtExe $stage
Copy-Item (Join-Path $repo "src/gui/modern/brand/qbitfusion.ico") $stage

& (Join-Path $tc.qt "bin\windeployqt.exe") --release --no-translations --no-system-d3d-compiler `
    --no-system-dxc-compiler --no-compiler-runtime `
    --no-opengl-sw --no-quick-import --dir $stage (Join-Path $stage "qbitfusion.exe") | Out-Null

# The app links the MSVC runtime dynamically; ship it app-local so no redistributable install is needed.
$vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt = Get-ChildItem "$vsPath\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crt) { throw "MSVC x64 CRT redist folder not found under $vsPath." }
Copy-Item (Join-Path $crt.FullName "*.dll") $stage

# License texts travel with the binary.
foreach ($file in "COPYING", "COPYING.GPLv2", "COPYING.GPLv3", "AUTHORS") {
    Copy-Item (Join-Path $repo $file) (Join-Path $stage "$file.txt")
}

$commit = (& git -C $repo rev-parse --short HEAD).Trim()
$readme = @"
qBitFusion $Version (Windows x64, portable)
Built from commit $commit.
Created and maintained by Noel (CoderNoel), https://github.com/CoderNoel

RUN
  Extract the whole folder anywhere you can write to, then start qbitfusion.exe.
  Windows may warn about an unknown publisher: the program is not code-signed.

SETTINGS
  Everything is kept in the "profile" folder next to the program. Keep that folder
  when you replace the program with a newer package. qBitFusion does not read or
  change the settings of a stock qBittorrent installation.
  Do not delete the "profile" folder: without it the program would use
  qBittorrent's own settings location.

STATUS
  Experimental. Windows only, one debrid provider (Real-Debrid). What has and has
  not been verified is listed on the project page:
  https://github.com/CoderNoel/qBitFusion

LICENSE
  qBitFusion is a fork of qBittorrent and is licensed under the GNU General Public
  License, version 2 or later, with the OpenSSL linking exception. See COPYING.txt.
  The source code for this build is the commit named above in the project's
  repository, https://github.com/CoderNoel/qBitFusion. qBitFusion is not affiliated with or endorsed by the qBittorrent project.

  This package also contains:
    Qt 6 (LGPL v3)                       https://www.qt.io/
    libtorrent (BSD 3-Clause)            https://libtorrent.org/
    Boost (Boost Software License 1.0)   https://www.boost.org/
    OpenSSL (Apache License 2.0)         https://www.openssl.org/
    zlib (zlib License)                  https://zlib.net/
    Microsoft Visual C++ runtime
  Peer country lookups use the IP to Country Lite database by DB-IP (CC BY 4.0).
"@
Set-Content (Join-Path $stage "README.txt") $readme -Encoding utf8

# Its presence enables portable mode. A zip cannot be relied on to keep an empty folder.
New-Item -ItemType Directory (Join-Path $stage "profile") | Out-Null
Set-Content (Join-Path $stage "profile\README.txt") "qBitFusion keeps its settings, torrents and logs in this folder. Keep it next to qbitfusion.exe." -Encoding utf8

$zip = Join-Path $out "$name.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Set-Content "$zip.sha256" "$hash  $name.zip" -Encoding ascii
Remove-Item $stage -Recurse -Force

"{0}  ({1:N1} MiB)" -f $zip, ((Get-Item $zip).Length / 1MB)
"SHA-256: $hash"

<#
.SYNOPSIS
  Reproduce qBittorrent release-5.3.0's Windows CI toolchain locally (CORE-001 / Gate G0).

.DESCRIPTION
  Mirrors .github/workflows/ci_windows.yaml at release-5.3.0:
    - vcpkg triplet x64-windows-static-md-release: boost-build, openssl, zlib
    - Boost 1.91.0 headers staged with b2 --with-headers
    - Qt 6.10.3 win64_msvc2022_64 (qtbase qtsvg qttools + qtimageformats) via aqtinstall
    - libtorrent v2.1.2, static, C++20, deprecated-functions=OFF
  Everything is installed under <repo>/.deps (git-excluded). No user profile data is touched.

.PARAMETER Libtorrent
  libtorrent tag to build (default 2.1.2, the pinned baseline).

.PARAMETER QtVersion
  Qt version to install (default 6.10.3, the version the Windows workflow uses).

.PARAMETER Boost
  Boost version whose headers are staged (default 1.91.0).
#>
param(
    [string]$Libtorrent = "2.1.2",
    [string]$QtVersion = "6.10.3",
    [string]$Boost = "1.91.0"
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$deps = Join-Path $repo ".deps"
New-Item -ItemType Directory -Force $deps | Out-Null

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

# --- MSVC developer environment -------------------------------------------------------------
Step "MSVC environment"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "MSVC x64 toolset not found" }
Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
Write-Host "cl: $((Get-Command cl.exe).Source)"

# --- vcpkg ------------------------------------------------------------------------------------
$vcpkg = Join-Path $deps "vcpkg"
$triplet = "x64-windows-static-md-release"
Step "vcpkg ($triplet)"
if (-not (Test-Path "$vcpkg/vcpkg.exe")) {
    if (-not (Test-Path $vcpkg)) { git clone --depth 1 https://github.com/microsoft/vcpkg.git $vcpkg }
    & "$vcpkg/bootstrap-vcpkg.bat" -disableMetrics
}
$overlay = Join-Path $vcpkg "triplets_overlay"
New-Item -ItemType Directory -Force $overlay | Out-Null
Set-Content -Path "$overlay/$triplet.cmake" -Value @(
    "set(VCPKG_TARGET_ARCHITECTURE x64)",
    "set(VCPKG_LIBRARY_LINKAGE static)",
    "set(VCPKG_CRT_LINKAGE dynamic)",
    "set(VCPKG_BUILD_TYPE release)")
& "$vcpkg/vcpkg.exe" install --clean-after-build --overlay-triplets="$overlay" `
    "boost-build:$triplet" "openssl:$triplet" "zlib:$triplet"

# --- Boost headers -----------------------------------------------------------------------------
$boostDir = Join-Path $deps "boost"
$boostTag = $Boost -replace '\.', '_'
Step "Boost $Boost"
if (-not (Test-Path "$boostDir/lib/cmake")) {
    $tgz = Join-Path $deps "boost_$boostTag.tar.gz"
    if (-not (Test-Path $tgz)) {
        curl.exe -fL -o $tgz "https://archives.boost.io/release/$Boost/source/boost_$boostTag.tar.gz"
    }
    tar -xf $tgz -C $deps
    if (Test-Path $boostDir) { Remove-Item -Recurse -Force $boostDir }
    Move-Item (Join-Path $deps "boost_$boostTag") $boostDir
    Push-Location $boostDir
    & "$vcpkg/installed/$triplet/tools/boost-build/b2.exe" stage toolset=msvc --stagedir=.\ --with-headers
    Pop-Location
}

# --- Qt ----------------------------------------------------------------------------------------
$qtRoot = Join-Path $deps "Qt"
$qtDir = Join-Path $qtRoot "$QtVersion/msvc2022_64"
Step "Qt $QtVersion"
if (-not (Test-Path "$qtDir/bin/qmake.exe")) {
    $venv = Join-Path $deps "aqt-venv"
    if (-not (Test-Path $venv)) { python -m venv $venv }
    & "$venv/Scripts/python.exe" -m pip install --quiet --upgrade aqtinstall
    & "$venv/Scripts/aqt.exe" install-qt windows desktop $QtVersion win64_msvc2022_64 `
        --outputdir $qtRoot --archives qtbase qtsvg qttools --modules qtimageformats
}

# --- libtorrent --------------------------------------------------------------------------------
$ltSrc = Join-Path $deps "libtorrent-$Libtorrent"
$ltInstall = Join-Path $deps "libtorrent-$Libtorrent-install"
Step "libtorrent $Libtorrent"
if (-not (Test-Path "$ltInstall/lib/cmake/LibtorrentRasterbar")) {
    if (-not (Test-Path $ltSrc)) {
        git clone --branch "v$Libtorrent" --depth 1 --recurse-submodules --shallow-submodules `
            https://github.com/arvidn/libtorrent.git $ltSrc
    }
    $env:CFLAGS = "/guard:cf"; $env:CXXFLAGS = "/guard:cf"; $env:LDFLAGS = "/GUARD:CF"
    cmake -S $ltSrc -B "$ltSrc/build" -G Ninja `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo `
        -DCMAKE_CXX_STANDARD=20 `
        -DCMAKE_INSTALL_PREFIX="$ltInstall" `
        -DCMAKE_TOOLCHAIN_FILE="$vcpkg/scripts/buildsystems/vcpkg.cmake" `
        "-DVCPKG_TARGET_TRIPLET=$triplet" `
        -DVCPKG_OVERLAY_TRIPLETS="$overlay" `
        -DBOOST_ROOT="$boostDir/lib/cmake" `
        -DBUILD_SHARED_LIBS=OFF `
        -Ddeprecated-functions=OFF `
        -Dstatic_runtime=OFF
    cmake --build "$ltSrc/build"
    cmake --install "$ltSrc/build"
    Remove-Item Env:CFLAGS, Env:CXXFLAGS, Env:LDFLAGS
}

# --- environment file read by the other scripts --------------------------------------------------------
@{
    vcpkg      = $vcpkg
    triplet    = $triplet
    overlay    = $overlay
    boost      = $boostDir
    qt         = $qtDir
    libtorrent = $ltInstall
    libtorrentVersion = $Libtorrent
} | ConvertTo-Json | Set-Content (Join-Path $deps "toolchain.json")

Step "Toolchain ready"
Get-Content (Join-Path $deps "toolchain.json")

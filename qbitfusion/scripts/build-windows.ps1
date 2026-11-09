<#
.SYNOPSIS
  Configure, build and test qBittorrent (and optionally the P1 spike) with the toolchain from
  bootstrap-windows.ps1. Flags mirror .github/workflows/ci_windows.yaml at release-5.3.0.

.EXAMPLE
  pwsh qbitfusion/scripts/build-windows.ps1              # build + ctest (target "check")
  pwsh qbitfusion/scripts/build-windows.ps1 -Spike       # build + run the libtorrent spike only
  pwsh qbitfusion/scripts/build-windows.ps1 -BuildDir build-off -NoTests
  pwsh qbitfusion/scripts/build-windows.ps1 -Release     # optimised app build in build-release, no tests
#>
param(
    [string]$BuildDir = "build",
    [string]$Config = "RelWithDebInfo",
    [switch]$Spike,
    [switch]$NoTests,
    [switch]$Reconfigure,
    # Shipping build: Release, no test targets, no forced asserts. Used by install-windows.ps1.
    [switch]$Release
)

if ($Release) {
    if (-not $PSBoundParameters.ContainsKey("BuildDir")) { $BuildDir = "build-release" }
    $Config = "Release"
    $NoTests = $true
}

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$tc = Get-Content (Join-Path $repo ".deps/toolchain.json") | ConvertFrom-Json

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
Remove-Item Env:VCPKG_ROOT -ErrorAction SilentlyContinue  # VS dev shell points this at its own vcpkg

$common = @(
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_TOOLCHAIN_FILE=$($tc.vcpkg)/scripts/buildsystems/vcpkg.cmake",
    "-DVCPKG_TARGET_TRIPLET=$($tc.triplet)",
    "-DVCPKG_OVERLAY_TRIPLETS=$($tc.overlay)",
    "-DBOOST_ROOT=$($tc.boost)/lib/cmake",
    "-DLibtorrentRasterbar_DIR=$($tc.libtorrent)/lib/cmake/LibtorrentRasterbar",
    "-DCMAKE_PREFIX_PATH=$($tc.qt)"
)

if ($Spike) {
    $spikeBuild = Join-Path $repo "$BuildDir-spike"
    cmake -S (Join-Path $repo "qbitfusion/spike") -B $spikeBuild @common
    cmake --build $spikeBuild
    & (Join-Path $spikeBuild "ltspike.exe") (Join-Path $repo "$BuildDir-spike/work")
    exit $LASTEXITCODE
}

$build = Join-Path $repo $BuildDir
if ($Reconfigure -or -not (Test-Path "$build/CMakeCache.txt")) {
    $env:CXXFLAGS = if ($Release) { "/WX" } else { "/DQT_FORCE_ASSERTS /WX" }
    $testing = if ($Release) { "-DTESTING=OFF" } else { "-DTESTING=ON" }
    cmake -S $repo -B $build @common `
        "-DMSVC_RUNTIME_DYNAMIC=ON" `
        $testing `
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    Remove-Item Env:CXXFLAGS
}

cmake --build $build
if (-not $NoTests) {
    $env:PATH = "$($tc.qt)/bin;$env:PATH"
    cmake --build $build --target check
}

<#
.SYNOPSIS
  Install qBitFusion for the current user from a Release build: a self-contained folder in
  %LOCALAPPDATA%\Programs\qBitFusion, Start menu and desktop shortcuts, and an entry in
  Settings > Apps so it can be removed like any other app. No administrator rights needed.

.DESCRIPTION
  The installed copy runs in portable mode (a "profile" folder next to qbittorrent.exe), so it
  never reads or changes a stock qBittorrent profile in %APPDATA%\qBittorrent and can run side by
  side with stock qBittorrent. Reinstalling keeps that profile. File associations for .torrent and
  magnet links are not touched.

.EXAMPLE
  pwsh qbitfusion/scripts/install-windows.ps1              # build Release, then install
  pwsh qbitfusion/scripts/install-windows.ps1 -NoBuild     # install the existing Release build
  pwsh qbitfusion/scripts/install-windows.ps1 -Uninstall   # remove app, shortcuts and entry (keeps profile)
#>
param(
    [string]$BuildDir = "build-release",
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Programs\qBitFusion"),
    [switch]$NoBuild,
    [switch]$NoDesktopShortcut,
    [switch]$Uninstall,
    [switch]$RemoveProfile
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$appName = "qBitFusion"
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$appName"
$startMenuLink = Join-Path ([Environment]::GetFolderPath("Programs")) "$appName.lnk"
$desktopLink = Join-Path ([Environment]::GetFolderPath("Desktop")) "$appName.lnk"
$exe = Join-Path $InstallDir "qbittorrent.exe"
$profileDir = Join-Path $InstallDir "profile"

function Assert-NotRunning {
    $running = Get-Process qbittorrent -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) }
    if ($running) {
        throw "$appName is running from $InstallDir (PID $($running.Id -join ', ')). Quit it first (File > Exit)."
    }
}

if ($Uninstall) {
    Assert-NotRunning
    Remove-Item $startMenuLink, $desktopLink -ErrorAction SilentlyContinue
    Remove-Item $uninstallKey -Recurse -ErrorAction SilentlyContinue
    if (Test-Path $InstallDir) {
        Get-ChildItem $InstallDir | Where-Object { $RemoveProfile -or $_.Name -ne "profile" } |
            Remove-Item -Recurse -Force
        if ($RemoveProfile) { Remove-Item $InstallDir -Recurse -Force }
    }
    Write-Host "$appName uninstalled." $(if (-not $RemoveProfile -and (Test-Path $profileDir)) { "Settings and torrents kept in $profileDir." })
    exit 0
}

$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$tc = Get-Content (Join-Path $repo ".deps/toolchain.json") | ConvertFrom-Json
$build = Join-Path $repo $BuildDir

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "build-windows.ps1") -Release -BuildDir $BuildDir
}
$builtExe = Join-Path $build "qbittorrent.exe"
if (-not (Test-Path $builtExe)) { throw "No Release build at $builtExe. Run without -NoBuild." }

Assert-NotRunning

# Stage into a fresh folder first, so a failed deploy never leaves a half-updated install.
$stage = Join-Path $build "install-stage"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $stage | Out-Null
Copy-Item $builtExe $stage

& (Join-Path $tc.qt "bin\windeployqt.exe") --release --no-translations --no-system-d3d-compiler `
    --no-opengl-sw --no-quick-import --dir $stage (Join-Path $stage "qbittorrent.exe") | Out-Null

# The app links the MSVC runtime dynamically; ship it app-local so no redistributable install is needed.
$vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt = Get-ChildItem "$vsPath\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crt) { throw "MSVC x64 CRT redist folder not found under $vsPath." }
Copy-Item (Join-Path $crt.FullName "*.dll") $stage

# Swap the staged files in, keeping the portable profile.
New-Item -ItemType Directory $InstallDir -Force | Out-Null
Get-ChildItem $InstallDir | Where-Object Name -ne "profile" | Remove-Item -Recurse -Force
Copy-Item (Join-Path $stage "*") $InstallDir -Recurse
New-Item -ItemType Directory $profileDir -Force | Out-Null   # its presence enables portable mode
Remove-Item $stage -Recurse -Force

# Shortcuts.
$shell = New-Object -ComObject WScript.Shell
$links = @($startMenuLink) + $(if ($NoDesktopShortcut) { @() } else { @($desktopLink) })
foreach ($path in $links) {
    $lnk = $shell.CreateShortcut($path)
    $lnk.TargetPath = $exe
    $lnk.WorkingDirectory = $InstallDir
    $lnk.IconLocation = "$exe,0"
    $lnk.Description = "qBitFusion: qBittorrent with debrid-assisted downloads"
    $lnk.Save()
}

# Settings > Apps entry (per user).
$selfCopy = Join-Path $InstallDir "uninstall.ps1"
Copy-Item $PSCommandPath $selfCopy
$version = (Get-Item $exe).VersionInfo.ProductVersion.TrimStart("v")
$size = [int]((Get-ChildItem $InstallDir -Recurse -File | Where-Object { -not $_.FullName.StartsWith($profileDir) } |
    Measure-Object Length -Sum).Sum / 1KB)
New-Item $uninstallKey -Force | Out-Null
$values = @{
    DisplayName     = $appName
    DisplayVersion  = $version
    Publisher       = "qBitFusion contributors"
    DisplayIcon     = "$exe,0"
    InstallLocation = $InstallDir
    UninstallString = "pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `"$selfCopy`" -Uninstall"
}
foreach ($name in $values.Keys) { Set-ItemProperty $uninstallKey $name $values[$name] }
Set-ItemProperty $uninstallKey EstimatedSize $size -Type DWord
Set-ItemProperty $uninstallKey NoModify 1 -Type DWord
Set-ItemProperty $uninstallKey NoRepair 1 -Type DWord

Write-Host "$appName $version installed to $InstallDir"
Write-Host "Profile (settings, torrents, logs): $profileDir"

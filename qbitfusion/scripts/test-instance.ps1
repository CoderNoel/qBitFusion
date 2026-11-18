<#
.SYNOPSIS
  Start a qBitFusion test instance from a build directory with its own throw-away profile, for UI
  checks. It never reads or writes the installed copy's profile or a stock qBittorrent profile.
  Prints the process id.

.EXAMPLE
  $id = pwsh qbitfusion/scripts/test-instance.ps1
  $id = pwsh qbitfusion/scripts/test-instance.ps1 -Arguments path\to\some.torrent   # opens its add dialog
  $id = pwsh qbitfusion/scripts/test-instance.ps1 -ShowWelcomeTour   # the welcome tour, as after installing
  pwsh qbitfusion/scripts/capture-windows.ps1 -ProcessId $id -OutDir build/ui-smoke/audit -Prefix main
#>
param(
    [string]$BuildDir = "build",
    [string]$ProfileDir = "build/ui-smoke/profile",
    [string[]]$Arguments = @(),
    # The welcome tour is skipped in test instances (it would cover the window in every capture).
    # Pass this to see it, as a new or updated install would.
    [switch]$ShowWelcomeTour
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$tc = Get-Content (Join-Path $repo ".deps/toolchain.json") | ConvertFrom-Json
$exe = Join-Path $repo "$BuildDir/qbitfusion.exe"
if (-not (Test-Path $exe)) { throw "No build at $exe. Run build-windows.ps1 first." }

$profilePath = Join-Path $repo $ProfileDir
New-Item -ItemType Directory -Force $profilePath | Out-Null

$ini = Join-Path $profilePath "qBittorrent/config/qBittorrent.ini"
New-Item -ItemType Directory -Force (Split-Path $ini) | Out-Null
$settings = [System.Collections.Generic.List[string]]::new()
if (Test-Path $ini) {
    foreach ($line in Get-Content $ini) { if ($line -notmatch '^OnboardingVersion=') { $settings.Add($line) } }
}
if (-not $ShowWelcomeTour) {
    $gui = $settings.IndexOf('[GUI]')
    if ($gui -ge 0) { $settings.Insert($gui + 1, 'OnboardingVersion=999') }
    else { $settings.AddRange([string[]]@('', '[GUI]', 'OnboardingVersion=999')) }
}
Set-Content -Path $ini -Value $settings

$env:PATH = (Join-Path $tc.qt "bin") + ";" + $env:PATH
$resolved = foreach ($a in $Arguments) { if (Test-Path $a) { (Resolve-Path $a).Path } else { $a } }
$process = Start-Process -FilePath $exe -ArgumentList (@("--profile=`"$profilePath`"") + $resolved) -PassThru
$process.Id

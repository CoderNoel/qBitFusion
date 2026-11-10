<#
.SYNOPSIS
  Start a qBitFusion test instance from a build directory with its own throw-away profile, for UI
  checks. It never reads or writes the installed copy's profile or a stock qBittorrent profile.
  Prints the process id.

.EXAMPLE
  $id = pwsh qbitfusion/scripts/test-instance.ps1
  $id = pwsh qbitfusion/scripts/test-instance.ps1 -Arguments build/ui-smoke/fixture/sample.torrent
  pwsh qbitfusion/scripts/capture-windows.ps1 -ProcessId $id -OutDir build/ui-smoke/audit -Prefix main
#>
param(
    [string]$BuildDir = "build",
    [string]$ProfileDir = "build/ui-smoke/profile",
    [string[]]$Arguments = @()
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$tc = Get-Content (Join-Path $repo ".deps/toolchain.json") | ConvertFrom-Json
$exe = Join-Path $repo "$BuildDir/qbitfusion.exe"
if (-not (Test-Path $exe)) { throw "No build at $exe. Run build-windows.ps1 first." }

$profilePath = Join-Path $repo $ProfileDir
New-Item -ItemType Directory -Force $profilePath | Out-Null

$env:PATH = (Join-Path $tc.qt "bin") + ";" + $env:PATH
$resolved = foreach ($a in $Arguments) { if (Test-Path $a) { (Resolve-Path $a).Path } else { $a } }
$process = Start-Process -FilePath $exe -ArgumentList (@("--profile=`"$profilePath`"") + $resolved) -PassThru
$process.Id

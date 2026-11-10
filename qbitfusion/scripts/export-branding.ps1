<#
.SYNOPSIS
  Export the approved artwork as Qt logos and a multi-resolution Windows icon.
  Requires ImageMagick 7; does not redraw the approved lettering or gradient.
#>
param(
    [string]$Source = "$PSScriptRoot/../design/logo/refinement-04/qf-upstream-style.png"
)
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$sourceImage = (Resolve-Path -LiteralPath $Source).Path
$brand = Join-Path $repo "src/gui/modern/brand"
$scratchRoot = Join-Path $repo "build"
$scratch = Join-Path $scratchRoot ("branding-export-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
try {
    $mask = Join-Path $scratch "disc.png"
    $alpha = Join-Path $scratch "alpha.png"
    $glyph = Join-Path $scratch "glyph.png"
    $logo = Join-Path $brand "qbitfusion.png"

    # Discard low-alpha specks outside the approved opaque disc, preserving its original
    # antialiasing and every pixel of the lettering/gradient. No artistic reconstruction.
    & magick $sourceImage -alpha extract -threshold 50% -morphology Dilate Disk:2 $mask
    & magick $sourceImage -alpha extract $mask -compose Multiply -composite $alpha
    & magick $sourceImage $alpha -alpha off -compose CopyOpacity -composite `
        -filter Lanczos -resize 1024x1024 -strip -define png:color-type=6 $logo

    # All Win32 shell sizes across 100-400% scaling, plus 128px Explorer preview.
    & magick $logo -define icon:auto-resize=256,128,96,80,72,64,60,48,40,36,32,30,24,20,16 `
        (Join-Path $brand "qbitfusion.ico")

    # Preserve the qf lettering and pale rim as a silhouette for monochrome tray styles.
    # Their blue channel is bright; the orange disc's blue channel is dark.
    & magick $logo -channel B -separate +channel -threshold 35% $glyph
    & magick $logo -alpha extract $glyph -compose Multiply -composite $alpha
    foreach ($variant in @(@("light", "white"), @("dark", "#1a1d23"))) {
        & magick -size 1024x1024 "xc:$($variant[1])" $alpha -alpha off `
            -compose CopyOpacity -composite -filter Lanczos -resize 256x256 -strip `
            -define png:color-type=6 (Join-Path $brand "tray-$($variant[0]).png")
    }
    Write-Host "Exported approved qf logo, 15-size Windows ICO, and light/dark monochrome tray icons."
}
finally {
    # Only delete the unique scratch directory inside this checkout's build directory.
    $resolvedScratch = [IO.Path]::GetFullPath($scratch)
    $resolvedRoot = [IO.Path]::GetFullPath($scratchRoot) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedScratch.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a scratch directory outside the build directory."
    }
    Remove-Item -LiteralPath $resolvedScratch -Recurse -Force
}

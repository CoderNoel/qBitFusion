# Branding check

A small stand-alone Windows program that checks the application icon the way Windows sees it. It is not
part of the main test suite, because it needs a built executable.

It verifies that:

- the logo and tray images render with real content and transparent corners at 15 sizes and six display
  scales;
- Windows can extract every size from `qbitfusion.ico` and from the icon embedded in `qbitfusion.exe`;
- optionally, that shortcuts resolve to the right icon.

Run it from the repository root, in the developer shell that `build-windows.ps1` sets up:

```powershell
$qt = (Get-Content .deps/toolchain.json | ConvertFrom-Json).qt
cmake -S qbitfusion/tests/branding -B build/branding -G Ninja "-DCMAKE_PREFIX_PATH=$qt" -DCMAKE_BUILD_TYPE=Release
cmake --build build/branding
pwsh qbitfusion/scripts/build-windows.ps1 -Release

$env:PATH = "$qt/bin;$env:PATH"
& build/branding/branding_checks.exe -platform offscreen `
    src/gui/modern/brand/qbitfusion.ico build-release/qbitfusion.exe
```

It exits with 0 when every check passes. Paths to `.lnk` shortcut files can be added after the two
arguments to check installed shortcuts as well.

The icon files are generated from `qbitfusion/design/logo/qbitfusion-logo-source.png` by
`qbitfusion/scripts/export-branding.ps1` (needs ImageMagick). When the icon changes, rebuild the
executable before running this check: the icon is compiled into it.

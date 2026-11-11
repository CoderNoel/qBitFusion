<#
.SYNOPSIS
  Capture the visible top-level windows of one process to PNG using PrintWindow.
  Only that process's windows are captured — never the rest of the desktop.
.EXAMPLE
  $id = pwsh qbitfusion/scripts/test-instance.ps1
  pwsh qbitfusion/scripts/capture-windows.ps1 -ProcessId $id -OutDir build/ui-smoke -Prefix check
#>
param(
    [Parameter(Mandatory)] [int]$ProcessId,
    [Parameter(Mandatory)] [string]$OutDir,
    [string]$Prefix = "window"
)

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public static class QbfCapture {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
  public static List<IntPtr> Windows(uint target) {
    var r = new List<IntPtr>();
    EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid == target && IsWindowVisible(h)) { var sb = new StringBuilder(256); GetWindowText(h, sb, 256); if (sb.Length > 0) r.Add(h); }
      return true; }, IntPtr.Zero);
    return r;
  }
  public static string Title(IntPtr h) { var sb = new StringBuilder(256); GetWindowText(h, sb, 256); return sb.ToString(); }
}
'@

# Per-monitor DPI aware (v2), so window rectangles are in physical pixels whatever the host shell is.
# Without it a scaled display gives a bitmap sized in logical pixels and the capture is cropped.
[void][QbfCapture]::SetThreadDpiAwarenessContext([IntPtr](-4))

New-Item -ItemType Directory -Force $OutDir | Out-Null
$i = 0
foreach ($h in [QbfCapture]::Windows([uint32]$ProcessId)) {
    $r = New-Object QbfCapture+RECT
    [void][QbfCapture]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $hgt = $r.B - $r.T
    if ($w -lt 50 -or $hgt -lt 50) { continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][QbfCapture]::PrintWindow($h, $hdc, 2)   # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc); $g.Dispose()
    $file = Join-Path $OutDir ("{0}-{1}.png" -f $Prefix, $i)
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    "{0} | {1}x{2} | {3}" -f $file, $w, $hgt, [QbfCapture]::Title($h)
    $i++
}

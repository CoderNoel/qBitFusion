<#
.SYNOPSIS
  Minimal, targeted window helpers for UI smoke tests of a qBitFusion test instance.
  Messages are posted to that process's own windows only; no global input is synthesized.
.EXAMPLE
  . qbitfusion/scripts/window-tools.ps1
  $h = Find-QbfWindow -ProcessId 1234 -Title 'qBittorrent v5.3.0'
  Send-QbfClose $h
#>
if (-not ('QbfWin' -as [type])) {
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class QbfWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool r);
  public static IntPtr Find(uint pid, string prefix) { IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid) { var sb = new StringBuilder(256); GetWindowText(h, sb, 256); if (found == IntPtr.Zero && sb.Length > 0 && sb.ToString().StartsWith(prefix)) found = h; }
      return true; }, IntPtr.Zero); return found; }
}
'@
}

function Find-QbfWindow([int]$ProcessId, [string]$Title) { [QbfWin]::Find([uint32]$ProcessId, $Title) }
function Send-QbfClose([IntPtr]$Handle) { [void][QbfWin]::PostMessage($Handle, 0x0010, [IntPtr]0, [IntPtr]0) }
function Send-QbfKey([IntPtr]$Handle, [int]$VirtualKey, [switch]$Alt) {
    if ($Alt) {
        [void][QbfWin]::PostMessage($Handle, 0x0104, [IntPtr]$VirtualKey, [IntPtr]0x20000001)
        [void][QbfWin]::PostMessage($Handle, 0x0105, [IntPtr]$VirtualKey, [IntPtr]0xE0000001)
    } else {
        [void][QbfWin]::PostMessage($Handle, 0x0100, [IntPtr]$VirtualKey, [IntPtr]1)
        [void][QbfWin]::PostMessage($Handle, 0x0101, [IntPtr]$VirtualKey, [IntPtr]0xC0000001)
    }
}
function Move-QbfWindow([IntPtr]$Handle, [int]$Width, [int]$Height) { [void][QbfWin]::MoveWindow($Handle, 40, 40, $Width, $Height, $true) }
# Click at client coordinates (physical pixels) by posting mouse messages to the window. Works without
# activating the window, so it never steals focus from whatever the user is doing.
function Send-QbfClick([IntPtr]$Handle, [int]$X, [int]$Y) {
    $lParam = [IntPtr](($Y -shl 16) -bor ($X -band 0xFFFF))
    [void][QbfWin]::PostMessage($Handle, 0x0200, [IntPtr]0, $lParam)   # WM_MOUSEMOVE
    Start-Sleep -Milliseconds 120
    [void][QbfWin]::PostMessage($Handle, 0x0201, [IntPtr]1, $lParam)   # WM_LBUTTONDOWN
    Start-Sleep -Milliseconds 60
    [void][QbfWin]::PostMessage($Handle, 0x0202, [IntPtr]0, $lParam)   # WM_LBUTTONUP
}

# Ask an app to exit the way an installer does (Restart Manager semantics): the session-end pair
# WM_QUERYENDSESSION / WM_ENDSESSION with ENDSESSION_CLOSEAPP. Unlike WM_CLOSE this is not swallowed
# by "close to tray", needs no confirmation dialog, and still runs the app's normal shutdown, so
# qBittorrent saves resume data first. Returns $true once the process has exited.
function Request-QbfExit([int]$ProcessId, [int]$TimeoutSeconds = 60) {
    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $process) { return $true }
    $handle = $process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { $handle = Find-QbfWindow -ProcessId $ProcessId -Title "qBitFusion" }
    if ($handle -eq [IntPtr]::Zero) { return $false }
    [void][QbfWin]::PostMessage($handle, 0x0011, [IntPtr]0, [IntPtr]1)   # WM_QUERYENDSESSION
    Start-Sleep -Seconds 3
    [void][QbfWin]::PostMessage($handle, 0x0016, [IntPtr]1, [IntPtr]1)   # WM_ENDSESSION
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 500 }
    return -not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
}

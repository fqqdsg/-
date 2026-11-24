param(
    [string]$Proc = "FastPeek",
    [int]$X = 147,
    [int]$Y = 223
)
# Replays the message sequence a real double-click produces, at a physical
# client pixel (dpi scaling handled like click.ps1):
# down / up / dblclk / up  -- no input is injected into the desktop
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class C3 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
}
"@
$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
$scale = [C3]::GetDpiForWindow($h) / 96.0
if ($scale -le 0) { $scale = 1.0 }
$lx = [int][Math]::Round($X / $scale)
$ly = [int][Math]::Round($Y / $scale)
$lp = [IntPtr](($ly -shl 16) -bor ($lx -band 0xFFFF))
[void][C3]::PostMessage($h, 0x0200, [IntPtr]::Zero, $lp)   # WM_MOUSEMOVE
Start-Sleep -Milliseconds 60
[void][C3]::PostMessage($h, 0x0201, [IntPtr]1, $lp)        # WM_LBUTTONDOWN
Start-Sleep -Milliseconds 40
[void][C3]::PostMessage($h, 0x0202, [IntPtr]::Zero, $lp)   # WM_LBUTTONUP
Start-Sleep -Milliseconds 40
[void][C3]::PostMessage($h, 0x0203, [IntPtr]1, $lp)        # WM_LBUTTONDBLCLK
Start-Sleep -Milliseconds 40
[void][C3]::PostMessage($h, 0x0202, [IntPtr]::Zero, $lp)   # WM_LBUTTONUP
Write-Output "posted double-click at client($X,$Y)"

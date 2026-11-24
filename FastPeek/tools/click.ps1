param(
    [string]$Proc = "FastPeek",
    [int]$X = 1219,
    [int]$Y = 450
)
# Posts a click at PHYSICAL client pixel (X, Y): no real input reaches the
# desktop.  The window is per-monitor DPI aware while this shell is not, so a
# posted message gets its coordinates scaled up by the window dpi - divide them
# back down so the numbers above stay comparable with what a real click sends.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class C2 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
}
"@
$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
$scale = [C2]::GetDpiForWindow($h) / 96.0
if ($scale -le 0) { $scale = 1.0 }
$lx = [int][Math]::Round($X / $scale)
$ly = [int][Math]::Round($Y / $scale)
$lp = [IntPtr](($ly -shl 16) -bor ($lx -band 0xFFFF))
[void][C2]::PostMessage($h, 0x0200, [IntPtr]::Zero, $lp)              # WM_MOUSEMOVE
Start-Sleep -Milliseconds 80
[void][C2]::PostMessage($h, 0x0201, [IntPtr]1, $lp)                   # WM_LBUTTONDOWN (MK_LBUTTON)
Start-Sleep -Milliseconds 60
[void][C2]::PostMessage($h, 0x0202, [IntPtr]::Zero, $lp)              # WM_LBUTTONUP
Write-Output "click physical($X,$Y) sent($lx,$ly) to $h"

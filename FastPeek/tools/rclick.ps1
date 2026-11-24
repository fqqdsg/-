param(
    [string]$Proc = "FastPeek",
    [int]$X = 640,
    [int]$Y = 450
)
# Right-click at PHYSICAL client pixel (X, Y), same dpi handling as click.ps1.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class R2 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
}
"@
$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
$scale = [R2]::GetDpiForWindow($h) / 96.0
if ($scale -le 0) { $scale = 1.0 }
$lx = [int][Math]::Round($X / $scale)
$ly = [int][Math]::Round($Y / $scale)
$lp = [IntPtr](($ly -shl 16) -bor ($lx -band 0xFFFF))
[void][R2]::PostMessage($h, 0x0204, [IntPtr]2, $lp)       # WM_RBUTTONDOWN (MK_RBUTTON)
Start-Sleep -Milliseconds 60
[void][R2]::PostMessage($h, 0x0205, [IntPtr]::Zero, $lp)  # WM_RBUTTONUP
Write-Output "right-click physical($X,$Y) sent($lx,$ly) to $h"

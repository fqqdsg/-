param(
    [string]$Proc = "FastPeek",
    [int]$X = 601,
    [int]$Y = 223
)
# posts only WM_MOUSEMOVE at a physical client pixel so hover can be checked
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class C4 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
}
"@
$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
$scale = [C4]::GetDpiForWindow($h) / 96.0
if ($scale -le 0) { $scale = 1.0 }
$lx = [int][Math]::Round($X / $scale)
$ly = [int][Math]::Round($Y / $scale)
$lp = [IntPtr](($ly -shl 16) -bor ($lx -band 0xFFFF))
[void][C4]::PostMessage($h, 0x0200, [IntPtr]::Zero, $lp)
Write-Output "posted move to client($X,$Y)"

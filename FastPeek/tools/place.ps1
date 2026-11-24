param([int]$W = 1280, [int]$H = 900, [int]$X = 0, [int]$Y = 0)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class P {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
}
"@
try { [void][P]::SetProcessDpiAwarenessContext([IntPtr](-4)) } catch { }
$p = Get-Process FastPeek -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
[void][P]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, $X, $Y, $W, $H, 0x0040)
Start-Sleep -Milliseconds 600
Write-Output "placed at $X,$Y ${W}x${H}"

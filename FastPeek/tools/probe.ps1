param([string]$Proc = "FastPeek")
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class M {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  public struct RECT { public int L,T,R,B; }
  public struct POINT { public int X,Y; }
}
"@
$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
$c = New-Object M+RECT; [void][M]::GetClientRect($h, [ref]$c)
$w = New-Object M+RECT; [void][M]::GetWindowRect($h, [ref]$w)
$pt = New-Object M+POINT; [void][M]::ClientToScreen($h, [ref]$pt)
Write-Output "client=$($c.R)x$($c.B) window=($($w.L),$($w.T))-($($w.R),$($w.B)) clientOrigin=$($pt.X),$($pt.Y) dpi=$([M]::GetDpiForWindow($h))"

param(
    [string]$Proc = "FastPeek",
    [string]$Keys = "13",
    [int]$Delay = 350
)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class K {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessage2(IntPtr h, uint msg, IntPtr w, IntPtr l);
  public struct RECT { public int L, T, R, B; }
}
"@
$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
foreach ($k in $Keys.Split(",")) {
    $code = [Convert]::ToInt32($k.Trim(), 10)
    [K]::PostMessage($h, 0x0100, [IntPtr]$code, [IntPtr]1) | Out-Null   # WM_KEYDOWN
    Start-Sleep -Milliseconds 60
    [K]::PostMessage($h, 0x0101, [IntPtr]$code, [IntPtr]1) | Out-Null   # WM_KEYUP
    Start-Sleep -Milliseconds $Delay
}
Write-Output "sent $Keys to $h"

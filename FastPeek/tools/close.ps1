param([string]$Proc = "FastPeek")
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class C { [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l); }
"@
$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
[C]::PostMessage($p.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 900
$alive = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if ($alive) { Write-Output "still running" } else { Write-Output "closed" }

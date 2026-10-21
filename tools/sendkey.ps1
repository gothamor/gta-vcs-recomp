# Focuses the vcs.exe window and holds a key for a moment (testing menus without a person at the keyboard).
#   pwsh tools/sendkey.ps1 -Key Return [-HoldMs 150] [-Times 1]
# Key names: System.Windows.Forms.Keys (Return, ShiftKey, Space, Up, Down, Left, Right, Escape, W, A, ...)
param([string]$Key = "Return", [int]$HoldMs = 150, [int]$Times = 1, [string]$Process = "vcs")
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KeyIn {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
}
"@
$p = Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error "no $Process window"; exit 1 }
[KeyIn]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 100
$vk = [byte][System.Windows.Forms.Keys]::$Key
$scan = [byte][KeyIn]::MapVirtualKey($vk, 0)
for ($i = 0; $i -lt $Times; $i++) {
  [KeyIn]::keybd_event($vk, $scan, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds $HoldMs
  [KeyIn]::keybd_event($vk, $scan, 2, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 250
}

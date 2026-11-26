param([string[]]$cases = @('control', 'jump'), [int]$hotkeyPid = 0, [string]$clickKind = 'double', [int[]]$settles = @(700), [switch]$fast)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
foreach ($d in 'start', 'target') {
    $p = Join-Path $root $d
    [void][IO.Directory]::CreateDirectory($p)
    foreach ($n in 1..3) { [IO.File]::WriteAllText((Join-Path $p "wsjumpfile-$d-$n.txt"), "test $d $n") }
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class X {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
'@
$shell = New-Object -ComObject Shell.Application
$before = @($shell.Windows() | ForEach-Object { $_.HWND })
Start-Process explorer.exe (Join-Path $root 'target')
$mine = $null
for ($i = 0; $i -lt 60 -and -not $mine; $i++) {
    Start-Sleep -Milliseconds 200
    $mine = $shell.Windows() | Where-Object { $before -notcontains $_.HWND -and $_.LocationURL -like '*ctrlg/target*' } | Select-Object -First 1
}
"explorer=" + $(if ($mine) { $mine.HWND } else { 'none' })
try {
    foreach ($c in $cases) { foreach ($s in $settles) {
        "--- $c settle $s"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'driver.ps1') -root $root -case $c -hotkeyPid $hotkeyPid -clickKind $clickKind -settle $s $(if ($fast) { '-fast' })
        Start-Sleep -Milliseconds 500 }
    }
} finally {
    if ($mine) { [void][X]::PostMessageW([IntPtr]$mine.HWND, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }
}

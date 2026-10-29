# Runs a scene file with the elevated agent, the way the live tests are meant to be (F:\wswt\RULES.md "Live testing"):
#   tools\screenshots\run.ps1 [-Scene scenes\release013.txt] [-Exe <WinShun.exe under test>] [-UserExe <the user's own>]
# waits for 20 s without the user's input, takes F:\wswt\ui.lock (waits while another session has it), runs agent.ps1
# elevated (no prompt on this machine) and waits for it, then puts everything back whatever happened: the theme and
# wallpaper (shots.py restore), the cover, the user's own WinShun (the copy that was running: the installed one, or
# dist; not elevated), the lock. Run it from a normal
# (not elevated) PowerShell: that is how the user's WinShun is started again.
param([string]$Scene = 'scenes\release013.txt', [string]$Exe, [string]$UserExe)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class IdleMs {
    [StructLayout(LayoutKind.Sequential)] struct LII { public uint size, time; }
    [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LII i);
    public static uint Get() { var i = new LII { size = 8 }; GetLastInputInfo(ref i); return (uint)Environment.TickCount - i.time; }
}
'@
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here '..\..')).Path
# the user's own copy: given, else the one running now (seen from this unelevated shell), else dist's
if (-not $UserExe) { $UserExe = Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path } | Select-Object -First 1 -ExpandProperty Path }
$userExe = if ($UserExe) { $UserExe } else { Join-Path $root 'dist\WinShun\WinShun.exe' }
$lock = 'F:\wswt\ui.lock'
$sw = [Diagnostics.Stopwatch]::StartNew()
while ([IdleMs]::Get() -lt 20000) {
    if ($sw.Elapsed.TotalMinutes -gt 10) { throw 'the user is at the computer: not starting' }
    Start-Sleep -Seconds 2
}
$sw.Restart()
while ($true) {
    try { $fs = [IO.File]::Open($lock, 'CreateNew', 'Write'); $fs.Close(); break } catch { }
    if ($sw.Elapsed.TotalMinutes -gt 15) { throw "$lock is held by another session: not starting" }
    Start-Sleep -Seconds 3
}
try {
    $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $here 'agent.ps1'), '-Scene', $Scene)
    if ($Exe) { $args2 += @('-Exe', $Exe) }
    $args2 += @('-UserExe', "`"$userExe`"") # C:\Program Files: quoted for the new process's command line
    $p = Start-Process powershell.exe -Verb RunAs -PassThru -WindowStyle Hidden -ArgumentList $args2
    $p.WaitForExit()
    Write-Host "agent exit code $($p.ExitCode); log: $(Join-Path $here 'work\agent.txt')"
} finally {
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($py) { & $py (Join-Path $here 'shots.py') restore }
    # the cover window, if the agent died with it up (its stop file ends it)
    New-Item -ItemType File (Join-Path $here 'work\cover.stop') -Force | Out-Null; Start-Sleep -Seconds 2
    Remove-Item (Join-Path $here 'work\cover.stop') -ErrorAction SilentlyContinue
    # the copy under test, on the profile the agent made (a bare --quit would ask the user's own copy), gone first
    $profileDir = Join-Path $here 'work\profile'
    if ($Exe -and $Exe -ne $userExe) {
        if (Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe }) {
            Start-Process $Exe -ArgumentList '--profile', "`"$profileDir`"", '--quit' -WindowStyle Normal
            $sw.Restart()
            while ((Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe }) -and $sw.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 300 }
            foreach ($left in @(Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe })) { Stop-Process -Id $left.Id -Force }
        }
    }
    # the demo copy, if the agent stopped before its profile stop: the same exe as the user's when that is the
    # installed one, told apart by its --profile
    $demo = { @(Get-CimInstance Win32_Process -Filter "Name='WinShun.exe'" | Where-Object { $_.CommandLine -like '*--profile*' }) }
    if ((& $demo).Count) {
        Start-Process $userExe -ArgumentList '--profile', "`"$profileDir`"", '--quit' -WindowStyle Normal
        $sw.Restart()
        while ((& $demo).Count -and $sw.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 300 }
        foreach ($left in (& $demo)) { Stop-Process -Id $left.ProcessId -Force }
    }
    # the user's own copy, from this (not elevated) shell, never from a .bak
    $mine = { @(Get-CimInstance Win32_Process -Filter "Name='WinShun.exe'" | Where-Object { $_.ExecutablePath -eq $userExe -and $_.CommandLine -notlike '*--profile*' }) }
    if (-not (& $mine).Count) {
        Start-Process $userExe -ArgumentList '--background' -WindowStyle Normal
        Start-Sleep -Seconds 6
    }
    Write-Host ("the user's WinShun running: " + [bool](& $mine).Count)
    if (Test-Path $lock) { [IO.File]::Delete($lock) }
}

# Issue #287 end to end, the part at normal rights: as issue 277's run-e2e.ps1
# (a staged WinShun on its own profile, an isolated SYSTEM service that takes
# it for WinShun's own UI), with "hold the hotkey, press again, let go to
# paste" turned on and three lines copied for it to record. e2e-hold.ps1 then
# presses the hotkey over an administrator's window the ways a hand would;
# results.txt has what its text box held after each.
#
# Not run while another WinShun is. Kept as they were: the clipboard's text, the Explorer verbs under HKCU, Windows' clipboard
# history (counted before and after; not run when it is nearly full).
#
#   ./run-e2e.ps1 -Build <build dir> -Stage <a deployed WinShun folder to copy> -Root <work dir under dist> -Run <empty dir for the logs>
param(
    [Parameter(Mandatory)] [string] $Build,
    [Parameter(Mandatory)] [string] $Stage,
    [Parameter(Mandatory)] [string] $Root,
    [Parameter(Mandatory)] [string] $Run
)
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path
$Stage = (Resolve-Path $Stage).Path
New-Item -ItemType Directory -Force $Root, $Run | Out-Null
$Root = (Resolve-Path $Root).Path
$Run = (Resolve-Path $Run).Path
$app = Join-Path $Root 'WinShun'
$profileDir = Join-Path $Root 'profile'
$exe = Join-Path $app 'WinShun.exe'
$pipe = "WinShun.Test287E2E.$PID"
$lines = 1..3 | ForEach-Object { "WinShun issue 287 line $_ of $PID" }
$lock = 'F:\wswt\ui.lock' # other sessions' live tests wait on it (F:\wswt\RULES.md)
$verbs = 'Directory', 'Directory\Background', 'Drive' | ForEach-Object { "HKCU:\Software\Classes\$_\shell\WinShun.SearchHere\command" }
function Note($t) { $text = "{0:HH:mm:ss.fff} {1}" -f (Get-Date), $t; Add-Content (Join-Path $Run 'run.log') $text -Encoding UTF8; $text }
function VerbCommands { $verbs | ForEach-Object { try { (Get-ItemProperty -LiteralPath $_ -ErrorAction Stop).'(default)' } catch { '' } } }
function HistoryCount { [int](powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'history-count.ps1') | Select-Object -Last 1) }

$locked = $false
$running = @(Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path } | Select-Object -ExpandProperty Path -Unique)
$verbsBefore = VerbCommands
$verbKeys = 'Directory', 'Directory/Background', 'Drive' | ForEach-Object { 'HKCU/Software/Classes/' + $_ + '/shell/WinShun.SearchHere' } | ForEach-Object { $_ -replace '/', [IO.Path]::DirectorySeparatorChar }
for ($i = 0; $i -lt $verbKeys.Count; ++$i) { reg export $verbKeys[$i] (Join-Path $Run "verb$i.reg") /y 2>$null | Out-Null }
$hadText = $false
$textBefore = $null
try {
    if (Test-Path (Split-Path $lock)) {
        $deadline = (Get-Date).AddSeconds(120)
        while (-not $locked) {
            try { [IO.File]::Open($lock, 'CreateNew', 'Write').Dispose(); $locked = $true }
            catch { if ((Get-Date) -gt $deadline) { throw 'the UI lock is held' }; Start-Sleep -Milliseconds 500 }
        }
    }
    Note "user's WinShun running: $($running -join ', ')"
    # Not quit for the test: how it was started (which profile) is not known, so it could not be put back.
    if ($running) { throw 'a WinShun is running: run this when none is' }

    # The staged copy: a deployed folder, with this build's programs over it.
    robocopy $Stage $app /E /NFL /NDL /NJH /NJS /NP | Out-Null
    Get-ChildItem $app -File | Where-Object { $_.Extension -in '.exe', '.dll' -and (Test-Path (Join-Path $Build $_.Name)) } |
        ForEach-Object { Copy-Item (Join-Path $Build $_.Name) $_.FullName -Force }
    $walked = Join-Path $Root 'walked'
    New-Item -ItemType Directory -Force $walked, (Join-Path $profileDir 'WinShun') | Out-Null
    Set-Content (Join-Path $walked 'one file.txt') 'issue 287'
    @"
[Setup]
Completed=true
[Launcher]
ExplorerMenu=false
[Index]
Access=walk
Scope=folders
Folders=$($walked -replace '\\', '/')
[Clipboard]
Enabled=true
WinV=false
Hotkey=Ctrl+Alt+F11
HoldToStep=true
PasteElevated=true
"@ | Set-Content (Join-Path $profileDir 'WinShun\WinShun.ini') -Encoding UTF8

    $historyBefore = HistoryCount
    Note "Windows clipboard history before: $historyBefore"
    if ($historyBefore -gt 20) { throw "Windows' clipboard history is nearly full ($historyBefore): the test's three lines would push the user's out" }

    $service = Join-Path $app 'WinShunService.exe'
    $elevated = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $PSScriptRoot 'e2e-hold.ps1')`"",
        '-Run', "`"$Run`"", '-Service', "`"$service`"", '-Client', "`"$exe`"", '-Pipe', $pipe)
    Start-Process powershell.exe -Verb RunAs -ArgumentList $elevated -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds(60)
    while (-not (Test-Path (Join-Path $Run 'up'))) {
        if ((Get-Date) -gt $deadline -or (Test-Path (Join-Path $Run 'cleaned'))) { throw 'the elevated part did not come up' }
        Start-Sleep -Milliseconds 300
    }
    $env:WINSHUN_PIPE = $pipe
    $env:WINSHUN_LOG_INFO = '1'
    $staged = Start-Process $exe -ArgumentList '--profile', "`"$profileDir`"", '--background' -WindowStyle Normal -PassThru
    Remove-Item Env:WINSHUN_PIPE, Env:WINSHUN_LOG_INFO
    Note "staged WinShun pid $($staged.Id)"
    Start-Sleep -Seconds 8 # up, its hotkey registered, the clipboard listened to

    $textBefore = Get-Clipboard -Raw -ErrorAction SilentlyContinue
    $hadText = $null -ne $textBefore
    foreach ($text in $lines) { Set-Clipboard -Value $text; Start-Sleep -Milliseconds 1500 } # recorded: line 3 on top
    Set-Content (Join-Path $Run 'go') 'go'
    $deadline = (Get-Date).AddSeconds(150)
    while (-not (Test-Path (Join-Path $Run 'done')) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    # What each way of pressing should leave in the text box: the row it
    # ends on, of the list as it is by then (line 3 is the newest, the first
    # row; an entry pasted moves to the top). -1: nothing pasted.
    $ways = [ordered]@{ 'tap' = -1; 'step' = 1; 'two-steps' = 2; 'held' = 0; 'quick-step' = 1; 'other-key' = -1; 'part-up' = -1; 'enter' = 0 }
    $order = [Collections.Generic.List[string]]@($lines[2], $lines[1], $lines[0])
    $expected = [ordered]@{}
    foreach ($name in $ways.Keys) {
        $row = $ways[$name]
        if ($row -lt 0) { $expected[$name] = ''; continue }
        $text = $order[$row]
        $expected[$name] = $text
        $order.RemoveAt($row); $order.Insert(0, $text)
    }
    $got = @{}
    if (Test-Path (Join-Path $Run 'results.txt')) {
        Get-Content (Join-Path $Run 'results.txt') -Encoding UTF8 | ForEach-Object { $name, $text = $_ -split '	', 2; $got[$name] = "$text" }
    }
    $failed = 0
    foreach ($name in $expected.Keys) {
        $ok = $got.ContainsKey($name) -and $got[$name] -eq $expected[$name]
        if (-not $ok) { ++$failed }
        Note ("{0}: {1,-10} text box [{2}] expected [{3}]" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $name, $got[$name], $expected[$name])
    }
    Note $(if ($failed -eq 0) { "PASS: all $($expected.Count) ways of pressing" } else { "FAIL: $failed of $($expected.Count)" })
    Copy-Item (Join-Path $profileDir 'WinShun\*.log') $Run -ErrorAction SilentlyContinue
    Get-ChildItem (Join-Path $profileDir 'WinShun') -Recurse -File | ForEach-Object { Note "profile file: $($_.FullName.Substring($profileDir.Length)) $($_.Length)" }
    Note "Windows clipboard history after: $(HistoryCount) (before: $historyBefore, plus the test's three lines)"
} catch {
    Note "ERROR: $_"
} finally {
    if (Test-Path $exe) {
        & $exe --profile $profileDir --quit
        $deadline = (Get-Date).AddSeconds(25)
        while ((Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Root) }) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
        Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($app) -and $_.ProcessName -ne 'WinShunService' } |
            ForEach-Object { Note "kill staged $($_.Id) $($_.ProcessName)"; Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
    }
    Set-Content (Join-Path $Run 'stop') 'stop'
    $deadline = (Get-Date).AddSeconds(30)
    while (-not (Test-Path (Join-Path $Run 'cleaned')) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    Note "elevated part cleaned: $(Test-Path (Join-Path $Run 'cleaned'))"
    if ($hadText) { Set-Clipboard -Value $textBefore; Note 'clipboard text put back' }
    $verbsAfter = VerbCommands
    for ($i = 0; $i -lt $verbs.Count; ++$i) {
        if ($verbsAfter[$i] -ne $verbsBefore[$i]) {
            Note "Explorer verb changed, put back: $($verbs[$i])"
            reg delete $verbKeys[$i] /f 2>$null | Out-Null
            if (Test-Path (Join-Path $Run "verb$i.reg")) { reg import (Join-Path $Run "verb$i.reg") 2>$null | Out-Null }
        }
    }
    Note "Explorer verbs as before: $(-not (Compare-Object @(VerbCommands) @($verbsBefore)))"
    # From this process, at normal rights: never from the elevated part.
    if ($locked) { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
}

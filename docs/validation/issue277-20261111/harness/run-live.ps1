# Issue #277 live test, the part at normal rights: starts live-paste.ps1
# elevated (UAC), waits for its windows and service, runs tst_servicepaste
# against them as an ordinary process (what WinShun's UI is), then has the
# elevated part remove everything.
#
#   ./run-live.ps1 -Build <build dir> -Run <empty dir for the logs> [-QtBin <Qt>\bin]
param(
    [Parameter(Mandatory)] [string] $Build,
    [Parameter(Mandatory)] [string] $Run,
    [string] $QtBin = 'C:\Qt\6.12.0\msvc2022_64\bin'
)
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path
New-Item -ItemType Directory -Force $Run | Out-Null
$Run = (Resolve-Path $Run).Path
$pipe = "WinShun.Test277.$PID"
$client = Join-Path $Build 'tst_servicepaste.exe'
$lock = 'F:\wswt\ui.lock' # other sessions' live tests wait on it (F:\wswt\RULES.md)
$locked = $false
try {
    if (Test-Path (Split-Path $lock)) {
        $deadline = (Get-Date).AddSeconds(120)
        while (-not $locked) {
            try { [IO.File]::Open($lock, 'CreateNew', 'Write').Dispose(); $locked = $true }
            catch { if ((Get-Date) -gt $deadline) { throw 'the UI lock is held' }; Start-Sleep -Milliseconds 500 }
        }
    }
    $elevated = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $PSScriptRoot 'live-paste.ps1')`"",
        '-Run', "`"$Run`"", '-Service', "`"$(Join-Path $Build 'WinShunService.exe')`"", '-Client', "`"$client`"", '-Pipe', $pipe,
        '-Before', "`"$(Join-Path $Build 'tst_service.exe'),$(Join-Path $Build 'tst_paster.exe')`"", '-QtBin', "`"$QtBin`"")
    Start-Process powershell.exe -Verb RunAs -ArgumentList $elevated -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds(180)
    while (-not (Test-Path (Join-Path $Run 'up'))) {
        if ((Get-Date) -gt $deadline -or (Test-Path (Join-Path $Run 'cleaned'))) { throw 'the elevated part did not come up' }
        Start-Sleep -Milliseconds 300
    }
    $front, $behind = (Get-Content (Join-Path $Run 'windows.txt') -Raw).Trim() -split ' '
    $env:PATH = "$QtBin;$env:PATH"
    $env:WINSHUN_PIPE = $pipe
    $env:WINSHUN_PASTE_FRONT = $front
    $env:WINSHUN_PASTE_BEHIND = $behind
    $test = Start-Process -FilePath $client -ArgumentList @('-o', "`"$(Join-Path $Run 'tst_servicepaste.txt')`",txt") -Wait -PassThru -WindowStyle Hidden
    "tst_servicepaste exit $($test.ExitCode)"
    Start-Sleep -Milliseconds 500 # the windows' last key into their log
} finally {
    Set-Content -Path (Join-Path $Run 'stop') -Value 'stop'
    $deadline = (Get-Date).AddSeconds(30)
    while (-not (Test-Path (Join-Path $Run 'cleaned')) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    "cleaned: $(Test-Path (Join-Path $Run 'cleaned'))"
    if ($locked) { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
}

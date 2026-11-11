# Issue #143 live test, the part at normal rights: makes two folders, starts
# live-nav.ps1 elevated (UAC), waits for its dialog and service, runs
# tst_servicedialognav against them as an ordinary process (what WinShun's UI
# is), then has the elevated part say where the dialog is and remove
# everything.
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
$pipe = "WinShun.Test143.$PID"
$client = Join-Path $Build 'tst_servicedialognav.exe'
$from = Join-Path $Run 'ws143-from'
$target = Join-Path $Run "ws143 $([char]0x56FE)$([char]0x7EB8).dwg" # a folder named as a file, Chinese and a space
New-Item -ItemType Directory -Force $from, $target | Out-Null
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
    $elevated = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $PSScriptRoot 'live-nav.ps1')`"",
        '-Run', "`"$Run`"", '-Service', "`"$(Join-Path $Build 'WinShunService.exe')`"", '-Client', "`"$client`"", '-Pipe', $pipe,
        '-From', "`"$from`"")
    Start-Process powershell.exe -Verb RunAs -ArgumentList $elevated -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds(120)
    while (-not (Test-Path (Join-Path $Run 'up'))) {
        if ((Get-Date) -gt $deadline -or (Test-Path (Join-Path $Run 'cleaned'))) { throw 'the elevated part did not come up' }
        Start-Sleep -Milliseconds 300
    }
    $env:PATH = "$QtBin;$env:PATH"
    $env:WINSHUN_PIPE = $pipe
    $env:WINSHUN_NAV_DIALOG = (Get-Content (Join-Path $Run 'windows.txt') -Raw).Trim()
    $env:WINSHUN_NAV_FOLDER = $target
    $test = Start-Process -FilePath $client -ArgumentList @('-o', "`"$(Join-Path $Run 'tst_servicedialognav.txt')`",txt") -Wait -PassThru -WindowStyle Hidden
    "tst_servicedialognav exit $($test.ExitCode)"
} finally {
    Set-Content -Path (Join-Path $Run 'stop') -Value 'stop'
    $deadline = (Get-Date).AddSeconds(30)
    while (-not (Test-Path (Join-Path $Run 'cleaned')) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    "cleaned: $(Test-Path (Join-Path $Run 'cleaned'))"
    if ($locked) { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
}
Get-Content (Join-Path $Run 'state-before.txt'), (Join-Path $Run 'state-after.txt') -Encoding UTF8 -ErrorAction SilentlyContinue

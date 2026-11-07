# Issue #243 live relay test, elevated part: a High window, two isolated SYSTEM
# console services (one installed-from tst_keylistener.exe, one not) and the
# System-IL sender; waits for the stop file (or 240 s), then removes all of it.
param([string]$Run)
$ErrorActionPreference = 'Stop'
$probe = 'F:\电脑便捷工具\build\issue243\probe'
$build = 'F:\电脑便捷工具\build\issue243\build'
$cfg = Get-Content (Join-Path $Run 'config.json') -Raw | ConvertFrom-Json
$log = Join-Path $Run 'harness.log'
function Note($t) { Add-Content -Path $log -Value ("{0:HH:mm:ss.fff} {1}" -f (Get-Date), $t) -Encoding UTF8 }
$tasks = @('WsRelaySvcA', 'WsRelaySvcB', 'WsRelaySend')
$window = $null
try {
    $svc = Join-Path $build 'WinShunService.exe'
    $test = Join-Path $build 'tst_keylistener.exe'
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $a = New-ScheduledTaskAction -Execute $svc -Argument "--console --pipe $($cfg.pipeA) --installed-from `"$test`""
    Register-ScheduledTask -TaskName $tasks[0] -Action $a -Principal $principal -Force | Out-Null
    $b = New-ScheduledTaskAction -Execute $svc -Argument "--console --pipe $($cfg.pipeB)"
    Register-ScheduledTask -TaskName $tasks[1] -Action $b -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName $tasks[0]
    Start-ScheduledTask -TaskName $tasks[1]
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline -and -not ((Test-Path "\\.\pipe\$($cfg.pipeA)") -and (Test-Path "\\.\pipe\$($cfg.pipeB)"))) { Start-Sleep -Milliseconds 200 }
    Note "pipes A=$(Test-Path "\\.\pipe\$($cfg.pipeA)") B=$(Test-Path "\\.\pipe\$($cfg.pipeB)")"
    $window = Start-Process (Join-Path $probe 'relaysend.exe') -ArgumentList 'window', 'relay-elevated', $cfg.vk, "`"$(Join-Path $Run 'window.log')`"" -PassThru
    Note "elevated window pid $($window.Id)"
    $send = "sys-launch `"$(Join-Path $probe 'relaysend.exe')`" send $($cfg.ready) $($cfg.done) $($cfg.vk) `"$(Join-Path $Run 'sender.log')`" relay-elevated `"WinShun issue #243 relay test`" `"$(Join-Path $probe 'hkpmissing.exe')`""
    $c = New-ScheduledTaskAction -Execute (Join-Path $probe 'hkprobe.exe') -Argument $send
    Register-ScheduledTask -TaskName $tasks[2] -Action $c -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName $tasks[2]
    Set-Content -Path (Join-Path $Run 'up') -Value 'up'
    Note 'up'
    $deadline = (Get-Date).AddSeconds(240)
    while ((Get-Date) -lt $deadline -and -not (Test-Path (Join-Path $Run 'stop'))) { Start-Sleep -Milliseconds 300 }
    Note 'stopping'
} catch {
    Note "ERROR: $_"
} finally {
    foreach ($t in $tasks) {
        try { Stop-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue } catch {}
        try { Unregister-ScheduledTask -TaskName $t -Confirm:$false -ErrorAction SilentlyContinue } catch {}
    }
    # Only this build's copies, never the installed service.
    Get-CimInstance Win32_Process -Filter "Name='WinShunService.exe' OR Name='relaysend.exe' OR Name='hkpmissing.exe'" |
        Where-Object { $_.ExecutablePath -and ($_.ExecutablePath.StartsWith($build) -or $_.ExecutablePath.StartsWith($probe)) } |
        ForEach-Object { Note "kill $($_.ProcessId) $($_.ExecutablePath)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Note 'cleaned'
    Set-Content -Path (Join-Path $Run 'cleaned') -Value 'cleaned'
}

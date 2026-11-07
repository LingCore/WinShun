# Issue #243 end-to-end, elevated part: an isolated SYSTEM console service for
# the staged WinShun.exe, a High window; on the go file the System-IL sender
# (hkprobe sys-launch relaysend e2e); on the stop file (or 300 s) all removed.
param([string]$Run)
$ErrorActionPreference = 'Stop'
$probe = 'F:\电脑便捷工具\build\issue243\probe'
$build = 'F:\电脑便捷工具\build\issue243\build'
$cfg = Get-Content (Join-Path $Run 'config.json') -Raw | ConvertFrom-Json
$log = Join-Path $Run 'harness.log'
function Note($t) { Add-Content -Path $log -Value ("{0:HH:mm:ss.fff} {1}" -f (Get-Date), $t) -Encoding UTF8 }
$tasks = @('WsE2eSvc', 'WsE2eSend')
try {
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $a = New-ScheduledTaskAction -Execute (Join-Path $build 'WinShunService.exe') -Argument "--console --pipe $($cfg.pipe) --installed-from `"$($cfg.stageExe)`""
    Register-ScheduledTask -TaskName $tasks[0] -Action $a -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName $tasks[0]
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline -and -not (Test-Path "\\.\pipe\$($cfg.pipe)")) { Start-Sleep -Milliseconds 200 }
    Note "pipe $(Test-Path "\\.\pipe\$($cfg.pipe)")"
    $w = Start-Process (Join-Path $probe 'relaysend.exe') -ArgumentList 'window', 'relay-elevated', $cfg.vk, "`"$(Join-Path $Run 'window.log')`"" -PassThru
    Note "elevated window pid $($w.Id)"
    Set-Content -Path (Join-Path $Run 'up') -Value 'up'
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline -and -not (Test-Path (Join-Path $Run 'go')) -and -not (Test-Path (Join-Path $Run 'stop'))) { Start-Sleep -Milliseconds 300 }
    if (Test-Path (Join-Path $Run 'go')) {
        Get-CimInstance Win32_Process -Filter "Name='WinShunService.exe'" | ForEach-Object { Note "service $($_.ProcessId): $($_.CommandLine)" }
        $send = "sys-launch `"$(Join-Path $probe 'relaysend.exe')`" e2e `"$(Join-Path $Run 'sender.log')`" $($cfg.vk) relay-elevated `"$(Join-Path $probe 'hkpmissing.exe')`" `"$Run`""
        $c = New-ScheduledTaskAction -Execute (Join-Path $probe 'hkprobe.exe') -Argument $send
        Register-ScheduledTask -TaskName $tasks[1] -Action $c -Principal $principal -Force | Out-Null
        Start-ScheduledTask -TaskName $tasks[1]
        Note 'sender started'
    }
    $deadline = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $deadline -and -not (Test-Path (Join-Path $Run 'stop'))) { Start-Sleep -Milliseconds 300 }
    Note 'stopping'
} catch {
    Note "ERROR: $_"
} finally {
    foreach ($t in $tasks) {
        try { Stop-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue } catch {}
        try { Unregister-ScheduledTask -TaskName $t -Confirm:$false -ErrorAction SilentlyContinue } catch {}
    }
    Get-CimInstance Win32_Process -Filter "Name='WinShunService.exe' OR Name='relaysend.exe' OR Name='hkpmissing.exe'" |
        Where-Object { $_.ExecutablePath -and ($_.ExecutablePath.StartsWith($build) -or $_.ExecutablePath.StartsWith($probe)) } |
        ForEach-Object { Note "kill $($_.ProcessId) $($_.ExecutablePath)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Note 'cleaned'
    Set-Content -Path (Join-Path $Run 'cleaned') -Value 'cleaned'
}

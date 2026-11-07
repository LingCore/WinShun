# Issue #243 end-to-end, the unelevated part (run from my shell): the shared
# ui.lock, the user's WinShun quit and always restarted, the staged copy run
# against an isolated SYSTEM service, the elevated harness driven by files.
$ErrorActionPreference = 'Stop'
$probe = 'F:\电脑便捷工具\build\issue243\probe'
$root = 'F:\电脑便捷工具\dist\issue243'
$stage = "$root\WinShun"; $prof = "$root\profile"
$installed = 'C:\Program Files\WinShun\WinShun.exe'
$lockPath = 'F:\wswt\ui.lock'
$id = [guid]::NewGuid().ToString('N').Substring(0, 8)
$run = Join-Path $probe "e2e-$id"
New-Item -ItemType Directory $run | Out-Null
function Note($t) { $line = "{0:HH:mm:ss.fff} {1}" -f (Get-Date), $t; Add-Content (Join-Path $run 'run.log') $line; $line }

$lock = [IO.File]::Open($lockPath, 'CreateNew', 'Write', 'None') # throws if another session holds it
try {
    $bytes = [Text.Encoding]::UTF8.GetBytes("claude-e0 issue #243 e2e $(Get-Date -Format s)")
    $lock.Write($bytes, 0, $bytes.Length); $lock.Flush()
    $taskBefore = (schtasks /query /tn '\WinShun' /xml 2>$null) -join "`n"
    $cfg = @{ pipe = "WinShun.Test.E2E.$id"; vk = '84'; stageExe = "$stage\WinShun.exe" }
    [IO.File]::WriteAllText((Join-Path $run 'config.json'), ($cfg | ConvertTo-Json), [Text.UTF8Encoding]::new($true)) # BOM: read by PowerShell 5.1

    Note 'quit installed WinShun'
    # Asked again every 5 s: one still starting up may not take it yet.
    $deadline = (Get-Date).AddSeconds(60); $asked = [datetime]::MinValue
    while ((Get-Date) -lt $deadline -and (Get-Process WinShun -ErrorAction SilentlyContinue)) {
        if (((Get-Date) - $asked).TotalSeconds -ge 5 -and (Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $installed })) {
            & $installed --quit; $asked = Get-Date
        }
        Start-Sleep -Milliseconds 300
    }
    $left = @(Get-CimInstance Win32_Process -Filter "Name='WinShun.exe'")
    Note "WinShun left: $($left.Count) $(($left | ForEach-Object { "$($_.ProcessId):$($_.ExecutablePath)" }) -join ' ')"
    if ($left.Count) { throw 'the installed WinShun did not quit' }

    $h = Start-Process powershell.exe -Verb RunAs -PassThru -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$probe\live-e2e.ps1", '-Run', $run
    $deadline = (Get-Date).AddSeconds(40)
    while ((Get-Date) -lt $deadline -and -not (Test-Path "$run\up") -and -not (Test-Path "$run\cleaned")) { Start-Sleep -Milliseconds 300 }
    Note "harness up: $(Test-Path "$run\up")"
    if (-not (Test-Path "$run\up")) { throw 'harness did not come up' }

    $env:WINSHUN_PIPE = $cfg.pipe; $env:WINSHUN_LOG_INFO = '1'
    $app = Start-Process "$stage\WinShun.exe" -ArgumentList '--profile', $prof, '--background' -WindowStyle Normal -PassThru
    Remove-Item Env:WINSHUN_PIPE, Env:WINSHUN_LOG_INFO
    Note "stage pid $($app.Id)"
    $deadline = (Get-Date).AddSeconds(40); $capture = $null
    while ((Get-Date) -lt $deadline -and -not $capture) {
        $capture = Get-CimInstance Win32_Process -Filter "Name='WinShunCapture.exe'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prof) }
        Start-Sleep -Milliseconds 300
    }
    Note "capture plugin: $($capture.ProcessId)"
    Start-Sleep -Seconds 6 # its hotkeys registered, the listener started anew with them
    Set-Content "$run\go" 'go'
    $deadline = (Get-Date).AddSeconds(70)
    $senderDone = { (Test-Path "$run\sender.log") -and (Select-String -Path "$run\sender.log" -Pattern 'e2e: done' -Quiet) }
    while ((Get-Date) -lt $deadline -and -not (& $senderDone)) { Start-Sleep -Milliseconds 500 }
    Note "sender done: $(& $senderDone)"
} catch {
    Note "ERROR: $_"
} finally {
    if (Test-Path "$stage\WinShun.exe") {
        Note 'quit stage'
        & "$stage\WinShun.exe" --profile $prof --quit
        $deadline = (Get-Date).AddSeconds(25)
        while ((Get-Date) -lt $deadline -and (Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($root) })) { Start-Sleep -Milliseconds 300 }
        Get-Process WinShun, WinShunPluginHost, WinShunCapture -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($_.Path.StartsWith($root)) } | ForEach-Object { Note "kill stage $($_.Id) $($_.Path)"; Stop-Process -Id $_.Id -Force }
    }
    Set-Content "$run\stop" 'stop'
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline -and -not (Test-Path "$run\cleaned")) { Start-Sleep -Milliseconds 300 }
    Note "harness cleaned: $(Test-Path "$run\cleaned")"
    if (-not (Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $installed })) {
        Start-Process $installed -ArgumentList '--background' -WindowStyle Normal
        Note 'installed WinShun restarted'
    }
    Start-Sleep -Seconds 3
    $taskAfter = (schtasks /query /tn '\WinShun' /xml 2>$null) -join "`n"
    Note "autostart task unchanged: $($taskBefore -eq $taskAfter)"
    $lock.Dispose()
    [IO.File]::Delete($lockPath)
    Note "lock released; run dir $run"
}

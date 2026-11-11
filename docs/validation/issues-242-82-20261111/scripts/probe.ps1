param([string]$Query = 'pl', [int]$Seconds = 9, [string]$Tag = 'probe')
$ErrorActionPreference = 'Stop'
$task = $PSScriptRoot
$root = (Resolve-Path (Join-Path $task '..\..')).Path
$exe = Join-Path $root 'dist\issues-242-82\WinShun\WinShun.exe'
$prof = Join-Path $task 'profile'
$src = [IO.File]::ReadAllText((Join-Path $task 'live.ps1'))
$m = [regex]::Match($src, "(?s)Add-Type @'\r?\n(.*?)\r?\n'@")
Add-Type $m.Groups[1].Value
[W]::Aware()
$log = Join-Path $task "$Tag.txt"
function Log([string]$s) { $line = (Get-Date -Format 'HH:mm:ss.fff') + ' ' + $s; Add-Content -LiteralPath $log -Value $line -Encoding UTF8; Write-Output $line }
function WsProcess { Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } }
function Name([uint32]$id) { try { (Get-Process -Id $id).ProcessName } catch { '?' } }
try {
    if (Get-Process WinShun -ErrorAction SilentlyContinue) { throw 'a WinShun is running' }
    Start-Process -FilePath $exe -ArgumentList '--profile', ('"' + $prof + '"'), '--background' -WindowStyle Normal
    Start-Sleep -Seconds 8
    $ws = WsProcess | Select-Object -First 1
    $wsPid = [uint32]$ws.Id
    Log "WinShun pid $wsPid; BatchRename running: $(@(Get-Process BatchRename -ErrorAction SilentlyContinue).Count)"
    foreach ($round in 1..2) {
        Start-Process -FilePath $exe -ArgumentList '--profile', ('"' + $prof + '"'), '--query', $Query -WindowStyle Normal
        $clock = [Diagnostics.Stopwatch]::StartNew(); $last = ''
        while ($clock.ElapsedMilliseconds -lt $Seconds * 1000) {
            $fg = [W]::GetForegroundWindow(); $id = [W]::Pid($fg)
            $box = @([W]::Top() | Where-Object { [W]::Pid($_) -eq $wsPid -and ([W]::Rect($_).R - [W]::Rect($_).L) -gt 200 }).Count
            $plug = @([W]::Top() | Where-Object { (Name ([W]::Pid($_))) -eq 'BatchRename' } | ForEach-Object { [W]::Cls($_) + ':' + [W]::Text($_) + ':' + ([W]::Rect($_).R - [W]::Rect($_).L) }) -join ','
            $now = "fg $(Name $id)/$([W]::Cls($fg))/'$([W]::Text($fg))' launcher=$box plugin=[$plug]"
            if ($now -ne $last) { Log ("round $round +{0,5} ms {1}" -f $clock.ElapsedMilliseconds, $now); $last = $now }
            Start-Sleep -Milliseconds 30
        }
        foreach ($i in 1..2) { if ([W]::Pid([W]::GetForegroundWindow()) -eq $wsPid) { [W]::Key(0x1B); Start-Sleep -Milliseconds 400 } }
        Start-Sleep -Seconds 1
    }
}
catch { Log ('ABORTED: ' + $_.Exception.Message) }
finally {
    WsProcess | ForEach-Object { $_.Kill(); [void]$_.WaitForExit(5000) }
    Start-Sleep -Milliseconds 800
    Get-Process BatchRename, WinShunPluginHost -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$prof\*" -or $_.Path -like (Split-Path $exe) + '\*' } | ForEach-Object { $_.Kill() }
}

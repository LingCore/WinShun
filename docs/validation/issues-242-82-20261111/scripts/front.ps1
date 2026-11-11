param([Parameter(Mandatory)][string]$Tag)
# Host check for GitHub #242: a plugin (day.rename 1.1.1) whose hidden window takes the front as
# its process starts. Does the launcher stay, get the keyboard back, and still go on a real click?
$ErrorActionPreference = 'Stop'
$task = $PSScriptRoot
$root = (Resolve-Path (Join-Path $task '..\..')).Path
$exe = Join-Path $root 'dist\host-front\WinShun\WinShun.exe'
$prof = Join-Path $task 'profile'
$src = [IO.File]::ReadAllText((Join-Path $root 'docs\validation\issues-242-82-20261111\scripts\live.ps1'))
Add-Type ([regex]::Match($src, "(?s)Add-Type @'\r?\n(.*?)\r?\n'@").Groups[1].Value)
Add-Type -AssemblyName System.Windows.Forms
[W]::Aware()
$log = Join-Path $task "$Tag.txt"
function Log([string]$s) { $line = (Get-Date -Format 'HH:mm:ss.fff') + ' ' + $s; Add-Content -LiteralPath $log -Value $line -Encoding UTF8; Write-Output $line }
function WsProcess { Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } }
function Name([uint32]$id) { try { (Get-Process -Id $id).ProcessName } catch { '?' } }
function Boxes([uint32]$wsPid) { @([W]::Top() | Where-Object { [W]::Pid($_) -eq $wsPid -and ([W]::Rect($_).R - [W]::Rect($_).L) -gt 200 }) }
function PluginShown { @([W]::Top() | Where-Object { (Name ([W]::Pid($_))) -eq 'BatchRename' -and ([W]::Rect($_).R - [W]::Rect($_).L) -gt 200 }) }
$keys = 'Directory\Background\shell', 'Directory\shell', 'Drive\shell' | ForEach-Object { "HKCU:\Software\Classes\$_\WinShun.SearchHere" }
$saved = @{}
foreach ($k in $keys) { if (Test-Path -LiteralPath $k) { $saved[$k] = @((Get-ItemProperty -LiteralPath $k).Icon, (Get-ItemProperty -LiteralPath "$k\command").'(default)') } }
$form = $null
try {
    if (Get-Process WinShun -ErrorAction SilentlyContinue) { throw 'a WinShun is running' }
    $env:WINSHUN_LOG_INFO = '1'
    Start-Process -FilePath $exe -ArgumentList '--profile', ('"' + $prof + '"'), '--background' -WindowStyle Normal
    Start-Sleep -Seconds 8
    $ws = WsProcess | Select-Object -First 1
    $wsPid = [uint32]$ws.Id
    Log ("WinShun {0} pid {1}" -f (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.Substring(0, 12), $wsPid)
    foreach ($round in 1..2) {
        Start-Process -FilePath $exe -ArgumentList '--profile', ('"' + $prof + '"'), '--query', 'pl' -WindowStyle Normal
        $clock = [Diagnostics.Stopwatch]::StartNew(); $last = ''; $gone = 0; $seen = $false
        while ($clock.ElapsedMilliseconds -lt 6000) {
            $fg = [W]::GetForegroundWindow(); $box = (Boxes $wsPid).Count
            if ($box) { $seen = $true } elseif ($seen) { $gone++ }
            $now = "fg $(Name ([W]::Pid($fg)))/$([W]::Cls($fg))/visible=$([W]::IsWindowVisible($fg)) launcher=$box"
            if ($now -ne $last) { Log ("round $round +{0,5} ms {1}" -f $clock.ElapsedMilliseconds, $now); $last = $now }
            Start-Sleep -Milliseconds 30
        }
        $front = [W]::Pid([W]::GetForegroundWindow()) -eq $wsPid
        Log ("round {0}: after 6 s launcher shown {1}, in front {2}, samples with it gone {3}, plugin windows {4}" -f $round, ((Boxes $wsPid).Count -gt 0), $front, $gone, (PluginShown).Count)
        if ($front) {
            $t = [Diagnostics.Stopwatch]::StartNew(); [W]::Key(0x0D); $shown = @()
            while ($t.ElapsedMilliseconds -lt 8000) { Start-Sleep -Milliseconds 100; $shown = @(PluginShown); if ($shown.Count) { break } }
            $ms = $t.ElapsedMilliseconds; Start-Sleep -Milliseconds 1200
            Log ("round {0}: Enter -> plugin window {1} after {2} ms, in front {3}, launcher shown {4}" -f $round, ($shown.Count -gt 0), $ms, ($shown -contains [W]::GetForegroundWindow()), ((Boxes $wsPid).Count -gt 0))
            foreach ($s in $shown) { [void][W]::PostMessageW($s, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
            Start-Sleep -Milliseconds 1000
        }
        foreach ($i in 1..2) { if ([W]::Pid([W]::GetForegroundWindow()) -eq $wsPid) { [W]::Key(0x1B); Start-Sleep -Milliseconds 400 } }
        Start-Sleep -Seconds 1
    }
    # A real click on another window still sends the launcher away.
    $form = New-Object Windows.Forms.Form; $form.Text = 'ws-front-click'; $form.StartPosition = 'Manual'; $form.Location = New-Object Drawing.Point 60, 60; $form.Size = New-Object Drawing.Size 500, 300; $form.TopMost = $true
    $form.Show(); [Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 400
    Start-Process -FilePath $exe -ArgumentList '--profile', ('"' + $prof + '"'), '--query', 'zqxwvj' -WindowStyle Normal
    foreach ($i in 1..25) { Start-Sleep -Milliseconds 200; [Windows.Forms.Application]::DoEvents(); if ([W]::Pid([W]::GetForegroundWindow()) -eq $wsPid) { break } }
    $before = (Boxes $wsPid).Count
    $r = [W]::Rect($form.Handle); $x = $r.L + 200; $y = $r.T + 200
    if ([W]::RootAt($x, $y) -ne $form.Handle) { throw 'the test window is not on top at the click point' }
    [W]::Click($x, $y)
    foreach ($i in 1..10) { Start-Sleep -Milliseconds 100; [Windows.Forms.Application]::DoEvents() }
    Log ("click elsewhere: launcher shown before {0}, after {1}; foreground {2}" -f ($before -gt 0), ((Boxes $wsPid).Count -gt 0), [W]::Text([W]::GetForegroundWindow()))
    foreach ($line in (Get-Item -LiteralPath (Join-Path $prof 'WinShun.log') | Select-String -Pattern 'Launcher:' | ForEach-Object { $_.Line })) { Log "log: $line" }
}
catch { Log ('ABORTED: ' + $_.Exception.Message + ' @ ' + $_.InvocationInfo.ScriptLineNumber) }
finally {
    if ($form) { $form.Close() }
    WsProcess | ForEach-Object { $_.Kill(); [void]$_.WaitForExit(5000) }
    Start-Sleep -Milliseconds 800
    Get-Process BatchRename, WinShunPluginHost -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$prof\*" -or $_.Path -like (Split-Path $exe) + '\*' } | ForEach-Object { $_.Kill() }
    foreach ($k in $saved.Keys) { Set-ItemProperty -LiteralPath $k -Name Icon -Value $saved[$k][0]; Set-ItemProperty -LiteralPath "$k\command" -Name '(default)' -Value $saved[$k][1] }
    Log ("left button down: {0}; staged WinShun left: {1}; search-here: {2}" -f (([W]::GetAsyncKeyState(1) -band 0x8000) -ne 0), @(WsProcess).Count, (Get-ItemProperty -LiteralPath ($keys[0] + '\command')).'(default)')
}

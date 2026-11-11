# GitHub #111 (the log of a copy set to run as administrator), by real input:
# a staged WinShun.exe with "run this program as administrator" in its
# properties (the RUNASADMIN flag under HKCU), started as a double-click
# starts it, so with administrator rights. Its drop pie's helper has to start
# with normal rights all the same; then a file is held in Explorer, the key
# under Esc pressed, the pointer moved up onto the pie's first chip and
# released. Passed: the helper runs, not elevated, and the file is moved into
# the favourite folder.
#
# Kept as they were: no WinShun of the user's may be running (checked, never
# touched), the Explorer verbs under HKCU, the flag (taken off again).
# Windows PowerShell 5.1, normal rights; UAC must not ask (it would wait).
#
#   powershell -File pie-runasadmin.ps1 -Tag fixed -Build <build dir> -Dist <a deployed WinShun folder> -Out <dir under build> -Stage <dir under dist>
param(
    [Parameter(Mandatory)] [string] $Tag,
    [Parameter(Mandatory)] [string] $Build,
    [Parameter(Mandatory)] [string] $Dist,
    [Parameter(Mandatory)] [string] $Out,
    [Parameter(Mandatory)] [string] $Stage
)
. "$PSScriptRoot\ws-live.ps1"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -Namespace WsLive -Name Rights -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
[DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
[DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr token, int cls, out int value, int size, out int needed);
[DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
// "elevated", "not elevated", or why it cannot be told (a process with more rights than ours keeps its token).
public static string Of(uint pid) {
    IntPtr process = OpenProcess(0x1000, false, pid);
    if (process == IntPtr.Zero) return "OpenProcess error " + Marshal.GetLastWin32Error();
    try {
        IntPtr token;
        if (!OpenProcessToken(process, 8, out token)) return "token kept from us, error " + Marshal.GetLastWin32Error();
        try {
            int elevated, needed;
            if (!GetTokenInformation(token, 20, out elevated, 4, out needed)) return "GetTokenInformation error " + Marshal.GetLastWin32Error();
            return elevated != 0 ? "elevated" : "not elevated";
        } finally { CloseHandle(token); }
    } finally { CloseHandle(process); }
}
'@
New-Item -ItemType Directory -Force $Out, $Stage | Out-Null
$Out = (Resolve-Path $Out).Path; $Stage = (Resolve-Path $Stage).Path; $Build = (Resolve-Path $Build).Path; $Dist = (Resolve-Path $Dist).Path
$log = Join-Path $Out "pie-$Tag.txt"
function Say([string] $s) { $line = "$(Get-Date -Format 'HH:mm:ss.fff') $s"; Add-Content -LiteralPath $log -Value $line -Encoding UTF8; $line }
Set-Content -LiteralPath $log -Value "pie run $Tag, build $Build" -Encoding UTF8

if (Get-Process WinShun -ErrorAction SilentlyContinue) { Say 'a WinShun.exe is running: not touching it'; exit 3 }
$lockPath = 'F:\wswt\ui.lock'
try { $lock = [IO.File]::Open($lockPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read) }
catch { Say "ui.lock is held by someone else: not running ($($_.Exception.Message))"; exit 3 }
$explorer = [IntPtr]::Zero; $buttonDown = $false; $started = $false
$src = Join-Path $Out "pie-src-$Tag"; $dst = Join-Path $Out "pie-dst-$Tag"
$name = "wsq111-$Tag.txt"
$prof = Join-Path $Out "prof-$Tag"
$stageExe = Join-Path $Stage 'WinShun.exe'
$layers = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
$verbKeys = 'Directory', 'Directory\Background', 'Drive' | ForEach-Object { "HKCU\Software\Classes\$_\shell\WinShun.SearchHere" }
function VerbCommands { $verbKeys | ForEach-Object { try { (Get-ItemProperty -LiteralPath "Registry::$_\command" -ErrorAction Stop).'(default)' } catch { '' } } }
function StageProcesses { @(Get-CimInstance Win32_Process -Filter "Name='WinShun.exe'") }
$verbsBefore = @(VerbCommands)
for ($i = 0; $i -lt $verbKeys.Count; ++$i) { reg export $verbKeys[$i] (Join-Path $Out "verb$i.reg") /y 2>$null | Out-Null }
try {
    $who = [Text.Encoding]::UTF8.GetBytes("#111 pie under RUNASADMIN $Tag, $(Get-Date -Format s)`n"); $lock.Write($who, 0, $who.Length); $lock.Flush()
    if ([WsLive.N]::GetAsyncKeyState(1) -band 0x8000) { throw 'the left button is held already: stopped' }

    foreach ($d in $src, $dst, $prof) { if (Test-Path $d) { Remove-Item -Recurse -Force $d }; New-Item -ItemType Directory -Force $d | Out-Null }
    [IO.File]::WriteAllText((Join-Path $src $name), "dragged by the #111 live test $Tag")
    # The stage: a deployed folder, with this build's programs over it.
    robocopy $Dist $Stage /E /XD data /NFL /NDL /NJH /NJS /NP | Out-Null
    Get-ChildItem $Stage -File | Where-Object { $_.Extension -in '.exe', '.dll' -and (Test-Path (Join-Path $Build $_.Name)) } |
        ForEach-Object { Copy-Item (Join-Path $Build $_.Name) $_.FullName -Force }
    Say "stage exe $((Get-FileHash $stageExe).Hash.Substring(0, 12)) written $((Get-Item (Join-Path $Build 'WinShun.exe')).LastWriteTime.ToString('s'))"
    New-Item -ItemType Directory -Force (Join-Path $prof 'WinShun') | Out-Null
    $ini = @"
[Setup]
Completed=true

[Launcher]
ExplorerMenu=false
DoubleCtrl=false
Hotkey=
History=false
DialogJump=false
DialogBar=false
ExplorerTyping=false
OpusTyping=false
FolderMenu=off

[Favorites]
DropPie=true

[Clipboard]
Enabled=false

[Content]
Index=false
"@
    [IO.File]::WriteAllText((Join-Path $prof 'WinShun\WinShun.ini'), $ini, (New-Object System.Text.UTF8Encoding $false))
    $favorites = [ordered]@{ format = 'winshun-favorites'; version = 1; favorites = @([ordered]@{ kind = 'folder'; path = $dst; name = "pie-dst-$Tag" }) }
    [IO.File]::WriteAllText((Join-Path $prof 'favorites.json'), ($favorites | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding $false))

    # The tick in the properties, and a start as from a double-click (the shell's: it elevates).
    if (-not (Test-Path $layers)) { New-Item -Path $layers -Force | Out-Null }
    Set-ItemProperty -LiteralPath $layers -Name $stageExe -Value '~ RUNASADMIN'
    Say "flag set: $((Get-ItemProperty -LiteralPath $layers).$stageExe)"
    $env:WINSHUN_LOG_INFO = '1'
    $started = $true
    Start-Process $stageExe -ArgumentList @('--profile', "`"$prof`"", '--background') -WindowStyle Normal | Out-Null
    Remove-Item Env:WINSHUN_LOG_INFO
    $deadline = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline -and [WsLive.N]::FindWindowW('WinShun.MessageWindow', 'WinShun') -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200 }
    if ([WsLive.N]::FindWindowW('WinShun.MessageWindow', 'WinShun') -eq [IntPtr]::Zero) { throw 'the stage did not come up' }
    $main = Pid-Of ([WsLive.N]::FindWindowW('WinShun.MessageWindow', 'WinShun'))
    Say "stage pid $main up: $([WsLive.Rights]::Of($main))"
    Start-Sleep -Seconds 6 # the favourites gathered on the worker, the helper started
    $all = StageProcesses
    foreach ($p in $all) { Say "WinShun.exe pid $($p.ProcessId) parent $($p.ParentProcessId): $([WsLive.Rights]::Of($p.ProcessId)); command line: $($p.CommandLine)" }
    $helper = $all | Where-Object { $_.CommandLine -like '*--drop-pie-helper*' } | Select-Object -First 1
    Say "HELPER: $(if ($helper) { "pid $($helper.ProcessId), $([WsLive.Rights]::Of($helper.ProcessId))" } else { 'none' })"
    $pids = @($all | ForEach-Object { [uint32] $_.ProcessId })

    # A window of its own for the source folder (never a tab of the user's).
    $before = @(Top-Windows { param($h) (Class-Of $h) -eq 'CabinetWClass' })
    Start-Process explorer.exe -ArgumentList "/separate,`"$src`"" | Out-Null
    $deadline = (Get-Date).AddSeconds(12)
    while ((Get-Date) -lt $deadline -and $explorer -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 250
        $explorer = @(Top-Windows { param($h) (Class-Of $h) -eq 'CabinetWClass' -and [WsLive.N]::IsWindowVisible($h) -and $before -notcontains $h -and (Title-Of $h) -like "pie-src-$Tag*" }) | Select-Object -First 1
        if (-not $explorer) { $explorer = [IntPtr]::Zero }
    }
    if ($explorer -eq [IntPtr]::Zero) { throw 'no Explorer window of its own for the source folder' }
    Say "explorer $explorer '$(Title-Of $explorer)' $(Fmt (Rect-Of $explorer))"
    Start-Sleep -Seconds 2
    $item = $null
    $deadline = (Get-Date).AddSeconds(6)
    while ((Get-Date) -lt $deadline -and -not $item) {
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($explorer)
        $cond = New-Object System.Windows.Automation.PropertyCondition ([System.Windows.Automation.AutomationElement]::NameProperty), $name
        $item = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
        if (-not $item) { Start-Sleep -Milliseconds 300 }
    }
    if (-not $item) { throw "the file $name is not found in the window" }
    $box = $item.Current.BoundingRectangle
    $x = [int]($box.X + [Math]::Min(40, $box.Width / 3)); $y = [int]($box.Y + $box.Height / 2)
    Say "file item at $([int]$box.X),$([int]$box.Y) $([int]$box.Width)x$([int]$box.Height); pressing at $x,$y"
    if ((Fg).hwnd -ne $explorer) { Click-At ([int]((Rect-Of $explorer).Left + 200)) ([int]((Rect-Of $explorer).Top + 20)) $explorer; Start-Sleep -Milliseconds 400 } # its title bar: to the front
    if ((Fg).hwnd -ne $explorer) { throw "the Explorer window is not in front ($((Fg).cls)): stopped" }
    $stageWindowsBefore = @(Top-Windows { param($h) $pids -contains (Pid-Of $h) -and [WsLive.N]::IsWindowVisible($h) })
    Move-To $x $y; Start-Sleep -Milliseconds 300
    if ((Root-At $x $y) -ne $explorer) { throw 'the file is not on top under the pointer: stopped' }

    $pie = [IntPtr]::Zero
    try {
        Left-Down; $buttonDown = $true
        Start-Sleep -Milliseconds 300
        if ((Fg).hwnd -ne $explorer) { throw "the front changed while the button was down ($((Fg).cls))" }
        [void](Key 0xC0 0x29) # the key under Esc
        Say 'key under Esc sent with the button held'
        $deadline = (Get-Date).AddMilliseconds(900)
        while ((Get-Date) -lt $deadline -and $pie -eq [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 30
            $pie = @(Top-Windows { param($h) $pids -contains (Pid-Of $h) -and [WsLive.N]::IsWindowVisible($h) -and $stageWindowsBefore -notcontains $h }) | Select-Object -First 1
            if (-not $pie) { $pie = [IntPtr]::Zero }
        }
        $cursor = New-Object WsLive.N+POINT; [void][WsLive.N]::GetCursorPos([ref] $cursor)
        if ($pie -ne [IntPtr]::Zero) {
            Say "pie window $(Class-Of $pie) $(Fmt (Rect-Of $pie)) of pid $(Pid-Of $pie); pointer $($cursor.X),$($cursor.Y)"
        } else { Say "no pie window within 900 ms; pointer $($cursor.X),$($cursor.Y)" }
        # Up, in small steps, onto chip 0 (above the middle): 120 px = 80 logical at 150 %.
        for ($i = 1; $i -le 12; $i++) { Move-To $cursor.X ($cursor.Y - 10 * $i); Start-Sleep -Milliseconds 25 }
        Start-Sleep -Milliseconds 600
        $pr = if ($pie -ne [IntPtr]::Zero) { Rect-Of $pie } else { $null }
        $sx = if ($pr) { $pr.Left - 60 } else { $cursor.X - 300 }; $sy = if ($pr) { $pr.Top - 60 } else { $cursor.Y - 250 }
        Shot ([Math]::Max(0, $sx)) ([Math]::Max(0, $sy)) 640 480 (Join-Path $Out "pie-$Tag-over-chip.jpg") 0.5
    } finally {
        if ($buttonDown) { Left-Up; $buttonDown = $false }
        Start-Sleep -Milliseconds 100
        if ([WsLive.N]::GetAsyncKeyState(1) -band 0x8000) { Left-Up; Say 'left button was still down: sent another up' }
    }
    Say 'released'
    Start-Sleep -Seconds 3
    $moved = Test-Path (Join-Path $dst $name); $left = Test-Path (Join-Path $src $name)
    Say "RESULT: in the favourite folder $moved, still in the source $left => $(if ($moved -and -not $left) { 'PASS (moved)' } elseif ($moved) { 'copied' } else { 'FAIL (not dropped)' })"
} catch {
    Say "STOPPED: $($_.Exception.Message)"
} finally {
    if ($buttonDown -or ([WsLive.N]::GetAsyncKeyState(1) -band 0x8000)) { Left-Up; Say 'button released in finally' }
    if ($explorer -ne [IntPtr]::Zero -and [WsLive.N]::IsWindow($explorer)) { [void][WsLive.N]::PostMessageW($explorer, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    # The flag first: with it, "--quit" itself could not be started with normal rights.
    Remove-ItemProperty -LiteralPath $layers -Name $stageExe -ErrorAction SilentlyContinue
    Say "flag left: [$((Get-ItemProperty -LiteralPath $layers -ErrorAction SilentlyContinue).$stageExe)]"
    if ($started) {
        try { $q = Start-Process $stageExe -ArgumentList @('--profile', "`"$prof`"", '--quit') -PassThru -WindowStyle Normal; [void]$q.WaitForExit(20000) } catch { Say "quit: $($_.Exception.Message)" }
        $deadline = (Get-Date).AddSeconds(25)
        while ((Get-Process WinShun -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    }
    Get-Process WinShunPluginHost -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Stage*" } | Stop-Process -Force -ErrorAction SilentlyContinue
    $verbsAfter = @(VerbCommands)
    for ($i = 0; $i -lt $verbKeys.Count; ++$i) {
        if ($verbsAfter[$i] -ne $verbsBefore[$i]) {
            Say "Explorer verb changed, put back: $($verbKeys[$i])"
            reg delete $verbKeys[$i] /f 2>$null | Out-Null
            if (Test-Path (Join-Path $Out "verb$i.reg")) { reg import (Join-Path $Out "verb$i.reg") 2>$null | Out-Null }
        }
    }
    Say "Explorer verbs as before: $(-not (Compare-Object @(VerbCommands) $verbsBefore))"
    $wlog = Join-Path $prof 'WinShun.log'
    if (Test-Path $wlog) { Select-String -LiteralPath $wlog -Pattern 'pie|Pie' | Select-Object -Last 15 | ForEach-Object { Say ("log: " + $_.Line) } }
    $lock.Dispose(); [IO.File]::Delete($lockPath)
    Say "lock released $(-not (Test-Path $lockPath)); WinShun.exe left $(@(Get-Process WinShun -ErrorAction SilentlyContinue).Count)"
}

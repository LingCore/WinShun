# Issue #271 live test: where WinShun's search box really goes on this PC's
# taskbar with [Taskbar] SearchBoxSide = tray and = auto. A copy of the build
# is started with a profile of its own (nothing but the taskbar box on) while
# no other WinShun runs, the box's window is measured against the taskbar and
# the tray, and the copy is told to quit. Normal rights; sends no input.
# The Explorer menu's registry keys (which a copy with the menu off removes
# as it exits) are saved first and put back.
#
#   ./live-side.ps1 -Build <build dir> -Run <empty dir for the profile and the log> [-QtBin <Qt>\bin]
param(
    [Parameter(Mandatory)] [string] $Build,
    [Parameter(Mandatory)] [string] $Run,
    [string] $QtBin = 'C:\Qt\6.12.0\msvc2022_64\bin'
)
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path
New-Item -ItemType Directory -Force $Run | Out-Null
$Run = (Resolve-Path $Run).Path
$exe = Join-Path $Build 'WinShun.exe'
$log = Join-Path $Run 'live-side.log'
function Note($t) { $line = "{0:HH:mm:ss.fff} {1}" -f (Get-Date), $t; Add-Content -Path $log -Value $line -Encoding UTF8; $line }
if (Get-Process WinShun -ErrorAction SilentlyContinue) { throw 'a WinShun is running: this test starts its own, one at a time' }
Add-Type -Namespace Ws -Name Native -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr context);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string type, string title);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
'@
[void][Ws.Native]::SetProcessDpiAwarenessContext([IntPtr](-4)) # physical pixels, as the box measures
function Find([IntPtr] $parent, [string] $type) { [Ws.Native]::FindWindowExW($parent, [IntPtr]::Zero, $type, [NullString]::Value) }
function RectOf([IntPtr] $window) { $r = New-Object Ws.Native+RECT; [void][Ws.Native]::GetWindowRect($window, [ref]$r); $r }
# The box: a window of its own or one inside the taskbar, whichever it is.
function Box {
    $tray = Find ([IntPtr]::Zero) 'Shell_TrayWnd'
    foreach ($parent in @([IntPtr]::Zero, $tray)) {
        $box = Find $parent 'WinShun.TaskbarSearchBox'
        if ($box -ne [IntPtr]::Zero -and [Ws.Native]::IsWindowVisible($box)) { return $box }
    }
    [IntPtr]::Zero
}
$keys = 'Directory\shell', 'Directory\Background\shell', 'Drive\shell' | ForEach-Object { "HKCU\Software\Classes\$_\WinShun.SearchHere" }
$saved = @()
$lock = 'F:\wswt\ui.lock' # other sessions' live tests wait on it (F:\wswt\RULES.md)
$locked = $false
$results = @{}
try {
    if (Test-Path (Split-Path $lock)) {
        try { [IO.File]::Open($lock, 'CreateNew', 'Write').Dispose(); $locked = $true } catch { throw 'the UI lock is held' }
    }
    for ($i = 0; $i -lt $keys.Count; ++$i) {
        $file = Join-Path $Run "searchhere-$i.reg"
        & reg.exe export $keys[$i] $file /y 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { $saved += $file }
    }
    Note "Explorer menu keys saved: $($saved.Count) of $($keys.Count)"
    $env:PATH = "$QtBin;$env:PATH"
    $env:WINSHUN_LOG_INFO = '1'
    foreach ($side in 'tray', 'auto') {
        $profile = Join-Path $Run "profile-$side"
        New-Item -ItemType Directory -Force (Join-Path $profile 'WinShun') | Out-Null
        Set-Content -Path (Join-Path $profile 'WinShun\WinShun.ini') -Encoding ASCII -Value @"
[Setup]
Completed=true
[Launcher]
ExplorerMenu=false
DoubleCtrl=false
Hotkey=
History=false
DialogJump=false
DialogBar=false
[Content]
Index=false
[Clipboard]
Enabled=false
[Update]
Automatic=false
[Taskbar]
SearchBox=true
SearchBoxStyle=box
SearchBoxSide=$side
"@
        $app = Start-Process -FilePath $exe -ArgumentList @('--profile', "`"$profile`"", '--background') -PassThru -WindowStyle Normal
        try {
            $box = [IntPtr]::Zero
            $deadline = (Get-Date).AddSeconds(45)
            while ((Get-Date) -lt $deadline -and $box -eq [IntPtr]::Zero -and -not $app.HasExited) { Start-Sleep -Milliseconds 300; $box = Box }
            if ($box -eq [IntPtr]::Zero) { Note "${side}: the box did not come onto the taskbar"; continue }
            Start-Sleep -Seconds 4 # the taskbar settled, the box where a settled reading puts it
            $box = Box
            $tray = Find ([IntPtr]::Zero) 'Shell_TrayWnd'
            $b = RectOf $box; $bar = RectOf $tray; $notify = RectOf (Find $tray 'TrayNotifyWnd')
            $scale = [Ws.Native]::GetDpiForWindow($tray) / 96.0
            $results[$side] = @{ box = $b; bar = $bar; notify = $notify; scale = $scale }
            Note ("${side}: box {0}..{1} ({2} px), taskbar {3}..{4}, tray from {5}, scale {6}; box to tray {7} px, box from the taskbar's left end {8} px" -f
                $b.Left, $b.Right, ($b.Right - $b.Left), $bar.Left, $bar.Right, $notify.Left, $scale, ($notify.Left - $b.Right), ($b.Left - $bar.Left))
        } finally {
            if (-not $app.HasExited) {
                Start-Process -FilePath $exe -ArgumentList @('--profile', "`"$profile`"", '--quit') -WindowStyle Normal
                if (-not $app.WaitForExit(15000)) { Note "${side}: did not quit in 15 s, stopped"; Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
            }
            Start-Sleep -Milliseconds 800
        }
    }
} finally {
    foreach ($file in $saved) { & reg.exe import $file 2>$null | Out-Null }
    Note "Explorer menu keys put back: $($saved.Count)"
    if ($locked) { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
}
# The verdict: beside the tray (its right end within 16 DIPs of the tray,
# in the right half of the taskbar) with `tray`; in the left half with `auto`
# on a taskbar with centred icons.
$tray = $results['tray']; $auto = $results['auto']
$ok = $false
if ($tray -and $auto) {
    $middle = ($tray.bar.Left + $tray.bar.Right) / 2
    $beside = ($tray.notify.Left - $tray.box.Right) -ge 0 -and ($tray.notify.Left - $tray.box.Right) -le 16 * $tray.scale -and $tray.box.Left -gt $middle
    $centred = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name TaskbarAl -ErrorAction SilentlyContinue).TaskbarAl -ne 0
    $leftEnd = if ($centred) { $auto.box.Right -lt $middle } else { $auto.box.Left -gt $middle }
    $ok = $beside -and $leftEnd
    Note "icons centred: $centred; tray: beside the tray $beside; auto: $(if ($centred) { 'in the left half' } else { 'right of the icons' }) $leftEnd"
}
Note "verdict: $(if ($ok) { 'PASSED' } else { 'FAILED' })"
exit $(if ($ok) { 0 } else { 1 })

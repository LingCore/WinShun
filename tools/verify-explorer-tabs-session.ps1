<#
.SYNOPSIS
    Live check of winshun.explorer-tabs keeping and reopening the open tabs
    (GitHub #273), with real File Explorer windows on this desktop.

.DESCRIPTION
    Starts the plugin program by itself (no Win顺), restricted to a folder it
    makes under -WorkDir (WINSHUN_TABS_TEST_ROOT): Explorer windows showing
    anything else are neither read, written down nor touched. It then

      1. writes what a logon before would have left (a window with three
         tabs, a window with one), starts the plugin as after a logon and
         checks that they come back: the tabs, the tab in front, each
         window's place, and the two not merged into one;
      2. checks that the plugin writes down what is open now;
      3. ends the plugin as a shutdown would (the windows still open), closes
         the windows, starts it as after a logon: back again;
      4. closes them by hand and asks for them by the keyword's row;
      5. asks again with everything open: nothing may be opened twice;
      6. leaves one tab open and asks: the missing tabs join its window;
      7. with the setting off, nothing comes back by itself;
      8. and a folder opened from the desktop still becomes a tab.

    Needs Windows 11 22H2 or newer (File Explorer with tabs). Presses no keys
    and moves no mouse (one mouse event that moves nothing, to bring the
    desktop to the front for step 8). Everything it makes is removed at the end.

.EXAMPLE
    ./tools/verify-explorer-tabs-session.ps1 -Plugin build\issues-273-272\src\explorer-tabs-plugin\WinShunExplorerTabs.exe -WorkDir build\issues-273-272\tabs-live
#>
param(
    [Parameter(Mandatory)] [string] $Plugin,
    [Parameter(Mandatory)] [string] $WorkDir
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$Plugin = (Resolve-Path $Plugin).Path
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
$root = Join-Path $WorkDir 'root'
$data = Join-Path $WorkDir 'data'
$names = 'ws273 甲', 'ws273 乙', 'ws273 丙', 'ws273 丁' # 'ws273' marks the test windows' titles
$failed = [Collections.Generic.List[string]]::new()
$shell = New-Object -ComObject Shell.Application

Add-Type -Namespace WsTabs -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string type, string title);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
[DllImport("user32.dll")] public static extern bool GetWindowPlacement(IntPtr window, ref WINDOWPLACEMENT placement);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, System.Text.StringBuilder text, int max);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential)] public struct WINDOWPLACEMENT { public int Length, Flags, ShowCmd, MinX, MinY, MaxX, MaxY; public RECT Normal; }
'@

function Check([bool] $ok, [string] $what) {
    Write-Host ("  {0} {1}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' }), $what))
    if (-not $ok) { $failed.Add($what) }
}

# The test folders' Explorer tabs: window handle -> the folders its tabs show, in the order opened.
function TestWindows {
    $byWindow = [ordered]@{}
    foreach ($w in @($shell.Windows())) {
        try { $path = $w.Document.Folder.Self.Path } catch { continue }
        if ($path -and $path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
            $key = [string]$w.HWND
            if (-not $byWindow.Contains($key)) { $byWindow[$key] = [Collections.Generic.List[string]]::new() }
            $byWindow[$key].Add((Split-Path $path -Leaf))
        }
    }
    $byWindow
}

function Layout {
    $windows = TestWindows
    (@($windows.Values | ForEach-Object { $_ -join '+' }) | Sort-Object) -join ' | '
}

# Where a window is when not maximized, as the plugin writes it down.
function Placement([string] $window) {
    $p = [WsTabs.Native+WINDOWPLACEMENT]::new()
    $p.Length = [Runtime.InteropServices.Marshal]::SizeOf($p)
    [void][WsTabs.Native]::SetThreadDpiAwarenessContext([IntPtr](-4)) # per monitor, as the plugin: real pixels
    [void][WsTabs.Native]::GetWindowPlacement([IntPtr][long]$window, [ref]$p)
    '{0},{1},{2},{3}' -f $p.Normal.Left, $p.Normal.Top, $p.Normal.Right, $p.Normal.Bottom
}

function Title([string] $window) {
    $text = [Text.StringBuilder]::new(512)
    [void][WsTabs.Native]::GetWindowText([IntPtr][long]$window, $text, 512)
    $text.ToString()
}

# The desktop in front, as when a folder is opened from it. (A mouse event
# that moves nothing first: Windows then lets this process hand the front on.)
function DesktopToFront {
    [WsTabs.Native]::mouse_event(1, 0, 0, 0, [UIntPtr]::Zero)
    [void][WsTabs.Native]::SetForegroundWindow([WsTabs.Native]::FindWindow('Progman', $null))
    Start-Sleep -Milliseconds 500
}

function WaitFor([scriptblock] $done, [int] $seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        if (& $done) { return $true }
        Start-Sleep -Milliseconds 250
    }
    $false
}

function CloseTestWindows {
    [void](WaitFor {
        $windows = TestWindows
        foreach ($key in $windows.Keys) { [void][WsTabs.Native]::PostMessage([IntPtr][long]$key, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) } # WM_CLOSE
        $windows.Count -eq 0
    } 15)
}

$script:requestId = 0
function StartPlugin([hashtable] $settings, [bool] $asLogon) {
    $info = [Diagnostics.ProcessStartInfo]::new($Plugin)
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.Environment['WINSHUN_TABS_TEST_ROOT'] = $root
    $info.Environment['WINSHUN_TABS_TEST_MARK'] = 'ws273'
    if ($asLogon) { $info.Environment['WINSHUN_TABS_LOGON'] = '1' }
    $process = [Diagnostics.Process]::Start($info)
    $script:host273 = $process
    $script:hostIn = [IO.StreamWriter]::new($process.StandardInput.BaseStream, [Text.UTF8Encoding]::new($false))
    $script:hostIn.AutoFlush = $true
    [void](Call 'initialize' @{ settings = $settings; locale = 'zh'; theme = 'light'; pluginDir = (Split-Path $Plugin); dataDir = $data })
}

# One request, its answer (log lines and other notifications in between are shown and passed over).
function Call([string] $method, [hashtable] $params) {
    $id = ++$script:requestId
    $script:hostIn.WriteLine((@{ jsonrpc = '2.0'; id = $id; method = $method; params = $params } | ConvertTo-Json -Depth 8 -Compress))
    while ($true) {
        $line = $script:host273.StandardOutput.ReadLine()
        if ($null -eq $line) { throw "the plugin ended during $method" }
        $message = $line | ConvertFrom-Json
        if ($message.id -eq $id) { return $message.result }
        if ($message.params.text) { Write-Host "    plugin: $($message.params.text)" }
    }
}

function StopPlugin {
    if ($script:host273 -and -not $script:host273.HasExited) {
        try { [void](Call 'shutdown' @{}) } catch { }
        if (-not $script:host273.WaitForExit(5000)) { $script:host273.Kill() }
    }
    $script:host273 = $null
}

function OpenFolder([string] $name) {
    Start-Process explorer.exe -ArgumentList ('"{0}"' -f (Join-Path $root $name))
}

try {
    if (Test-Path $WorkDir) { Remove-Item $WorkDir -Recurse -Force }
    foreach ($name in $names) { [void](New-Item -ItemType Directory -Force (Join-Path $root $name)) }
    [void](New-Item -ItemType Directory -Force $data)
    $session = Join-Path $data 'session.txt'
    $both = (@(($names[0..2] -join '+'), $names[3]) | Sort-Object) -join ' | '
    # What a logon before would have left: a window with three tabs (the
    # second in front) and a window with one, each at a place of its own.
    $left = @('WinShun.ExplorerTabs.Session 1', 'open 1',
        'window 150 120 1250 820 0 1', "tab $(Join-Path $root $names[0])", "tab $(Join-Path $root $names[1])", "tab $(Join-Path $root $names[2])",
        'window 420 260 1420 900 0 0', "tab $(Join-Path $root $names[3])")
    [IO.File]::WriteAllLines($session, $left, [Text.UTF8Encoding]::new($false))

    Write-Host '1. After a logon the windows and tabs come back by themselves'
    StartPlugin @{ merge = $true; restore = $true } $true
    Check (WaitFor { (Layout) -eq $both } 40) "three tabs in one window, one in another: $both (now: $(Layout))"
    Start-Sleep -Seconds 3 # the engine hears of the last window after it is there: still two windows then
    Check ((Layout) -eq $both) 'and stayed two windows (not merged while they were opened)'
    $windows = TestWindows
    $three = @($windows.Keys | Where-Object { $windows[$_].Count -eq 3 })[0]
    $one = @($windows.Keys | Where-Object { $windows[$_].Count -eq 1 })[0]
    Check ((Title $three) -like "*$($names[1])*") "the second tab is the one in front (window title: $(Title $three))"
    Check ((Placement $three) -eq '150,120,1250,820') "the three-tab window is where it was (150,120,1250,820; now $(Placement $three))"
    Check ((Placement $one) -eq '420,260,1420,900') "the other window as well (420,260,1420,900; now $(Placement $one))"

    Write-Host '2. What is open is written down'
    $kept = { (Get-Content $session -Raw -Encoding UTF8) -replace "`r", '' }
    $current = "window 150 120 1250 820 0 1`ntab $(Join-Path $root $names[0])`ntab $(Join-Path $root $names[1])`ntab $(Join-Path $root $names[2])`n"
    Check (WaitFor { (& $kept).Contains("open 1`n") -and (& $kept).Contains($current) -and (& $kept).Contains("window 420 260 1420 900 0 0`ntab $(Join-Path $root $names[3])`n") } 20) 'session.txt holds both windows as they are now, tabs in order, the tab in front'
    Get-Content $session -Encoding UTF8 | ForEach-Object { Write-Host "    $_" }

    Write-Host '3. A shutdown with the windows open, a logon: back from what the plugin wrote itself'
    StopPlugin # as at shutdown: the windows are still open
    CloseTestWindows
    Check ((& $kept) -match '(?m)^open 1') 'the file still says the windows were open'
    StartPlugin @{ merge = $true; restore = $true } $true
    Check (WaitFor { (Layout) -eq $both } 40) "the same windows and tabs are back (now: $(Layout))"

    Write-Host '4. Closed by hand, then asked for (the second row of the keyword)'
    CloseTestWindows
    Check (WaitFor { (& $kept) -match '(?m)^open 0' } 20) 'the file says nothing was open'
    $rows = (Call 'query' @{ queryId = 1; command = 'tabs'; keyword = 'tabs'; text = ''; raw = 'tabs' }).items
    $row = @($rows | Where-Object { $_.id -eq 'restore' })
    Check ($row.Count -eq 1) "the row is there: $($row.title) / $($row.subtitle)"
    [void](Call 'action' @{ command = 'tabs'; action = 'default'; item = @{ id = 'restore' } })
    Check (WaitFor { (Layout) -eq $both } 40) "back again (now: $(Layout))"

    Write-Host '5. Nothing is opened twice'
    Start-Sleep -Seconds 3
    [void](Call 'action' @{ command = 'tabs'; action = 'default'; item = @{ id = 'restore' } })
    Start-Sleep -Seconds 6
    Check ((Layout) -eq $both) "still $both"

    Write-Host '6. A tab that is open already keeps its window: the others join it'
    CloseTestWindows
    OpenFolder $names[1] # only the second folder is open, in a window of its own
    Check (WaitFor { (Layout) -eq $names[1] } 10) 'only one tab left open'
    Start-Sleep -Seconds 2
    [void](Call 'action' @{ command = 'tabs'; action = 'default'; item = @{ id = 'restore' } })
    $joined = (@((($names[1], $names[0], $names[2]) -join '+'), $names[3]) | Sort-Object) -join ' | '
    Check (WaitFor { (Layout) -eq $joined } 40) "its window got the two missing tabs, the other window came back (now: $(Layout))"

    Start-Sleep -Seconds 2
    Check (WaitFor { (& $kept).Contains("tab $(Join-Path $root $names[1])`ntab $(Join-Path $root $names[0])`ntab $(Join-Path $root $names[2])`n") } 20) 'and they are written down in the order of the tab strip (second, first, third)'

    Write-Host '7. Without the setting nothing comes back by itself'
    StopPlugin
    CloseTestWindows
    StartPlugin @{ merge = $true; restore = $false } $true
    Start-Sleep -Seconds 8
    Check ((Layout) -eq '') "no window opened (now: $(Layout))"

    Write-Host '8. New folder windows still become tabs (what the plugin is for)'
    OpenFolder $names[0]
    Check (WaitFor { (Layout) -eq $names[0] } 10) 'a first window'
    Start-Sleep -Seconds 1
    DesktopToFront # asked for from the desktop, not from the Explorer window (that would keep it a window)
    OpenFolder $names[1]
    Check (WaitFor { (Layout) -eq ($names[0..1] -join '+') } 15) "the second folder became a tab of it (now: $(Layout))"
}
finally {
    StopPlugin
    CloseTestWindows
    Remove-Item $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed.Count) { Write-Host "`n$($failed.Count) check(s) FAILED" ; exit 1 }
Write-Host "`nAll checks passed"

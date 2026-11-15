<#
.SYNOPSIS
    Live check of winshun.explorer-tabs with Win+E (GitHub #331), with real
    File Explorer windows and real key presses on this desktop.

.DESCRIPTION
    Starts the plugin program by itself (no Win顺, not restricted: Win+E opens
    Home, which no test folder can stand for) and checks that

      1. Win+E from the desktop opens a window (there is none to join);
      2. Win+E again, that window in front, gives it a second tab, not a
         second window -- although its tab in front shows the same place;
      3. Ctrl+N in that window still opens a window;
      4. Win+E, then Ctrl+N at once: the key before does not make it a tab.

    Refuses to run while File Explorer windows are open (their tabs would be
    joined) or while Win顺's own copy of the plugin runs. Presses Win+E and
    Ctrl+N for real: leave the keyboard and mouse alone meanwhile. The windows
    it opens are closed at the end.

.EXAMPLE
    ./tools/verify-explorer-tabs-wine.ps1 -Plugin build\run-latest\WinShunExplorerTabs.exe -WorkDir build\issue-331\wine-live
#>
param(
    [Parameter(Mandatory)] [string] $Plugin,
    [Parameter(Mandatory)] [string] $WorkDir
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$Plugin = (Resolve-Path $Plugin).Path
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
$data = Join-Path $WorkDir 'data'
$trace = Join-Path $WorkDir 'trace.txt'
$failed = [Collections.Generic.List[string]]::new()

Add-Type -Namespace WsWinE -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string type, string title);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string type, string title);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
[DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
[DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr window, int attribute, out int value, int size);
'@

function Check([bool] $ok, [string] $what) {
    Write-Host ("  {0} {1}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' }), $what))
    if (-not $ok) { $failed.Add($what) }
}

# The Explorer windows on screen (not hidden by the plugin, not cloaked): handle -> number of tabs.
function Windows {
    $found = [ordered]@{}
    $w = [IntPtr]::Zero
    while (($w = [WsWinE.Native]::FindWindowEx([IntPtr]::Zero, $w, 'CabinetWClass', [NullString]::Value)) -ne [IntPtr]::Zero) {
        $cloaked = 0
        [void][WsWinE.Native]::DwmGetWindowAttribute($w, 14, [ref]$cloaked, 4)
        if (-not [WsWinE.Native]::IsWindowVisible($w) -or $cloaked) { continue }
        $tabs = 0
        $t = [IntPtr]::Zero
        while (($t = [WsWinE.Native]::FindWindowEx($w, $t, 'ShellTabWindowClass', [NullString]::Value)) -ne [IntPtr]::Zero) { $tabs++ }
        $found[[string]$w] = $tabs
    }
    $found
}

function Layout { (@((Windows).Values) | Sort-Object) -join '+' } # "2": one window, two tabs; "1+2": two windows

function WaitFor([scriptblock] $done, [int] $seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        if (& $done) { return $true }
        Start-Sleep -Milliseconds 250
    }
    $false
}

# Stays `$layout` for two seconds once reached (a window hidden for a moment, a tab still coming).
function Settles([string] $layout) {
    if (-not (WaitFor { (Layout) -eq $layout } 12)) { return $false }
    Start-Sleep -Seconds 2
    (Layout) -eq $layout
}

function Press([byte[]] $keys) {
    foreach ($k in $keys) { [WsWinE.Native]::keybd_event($k, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 30 }
    [array]::Reverse($keys)
    foreach ($k in $keys) { [WsWinE.Native]::keybd_event($k, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 30 }
}
function WinE { Press ([byte[]](0x5B, 0x45)) }
function CtrlN { Press ([byte[]](0x11, 0x4E)) }

function ToFront([IntPtr] $window) {
    [WsWinE.Native]::mouse_event(1, 0, 0, 0, [UIntPtr]::Zero) # moves nothing: Windows then lets this process hand the front on
    [void][WsWinE.Native]::SetForegroundWindow($window)
    Start-Sleep -Milliseconds 600
}

$script:requestId = 0
function Call([string] $method, [hashtable] $params) {
    $id = ++$script:requestId
    $script:hostIn.WriteLine((@{ jsonrpc = '2.0'; id = $id; method = $method; params = $params } | ConvertTo-Json -Depth 8 -Compress))
    while ($true) {
        $line = $script:tabsHost.StandardOutput.ReadLine()
        if ($null -eq $line) { throw "the plugin ended during $method" }
        $message = $line | ConvertFrom-Json
        if ($message.id -eq $id) { return $message.result }
        if ($message.params.text) { Write-Host "    plugin: $($message.params.text)" }
    }
}

if ((Windows).Count) { throw 'File Explorer windows are open: close them first (the test would add tabs to them).' }
if (Get-Process WinShunExplorerTabs -ErrorAction SilentlyContinue) { throw "Win顺's own copy of the plugin is running: pause or turn it off first." }

try {
    if (Test-Path $WorkDir) { Remove-Item $WorkDir -Recurse -Force }
    [void](New-Item -ItemType Directory -Force $data)
    $info = [Diagnostics.ProcessStartInfo]::new($Plugin)
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.Environment['WINSHUN_TABS_TRACE'] = $trace
    $script:tabsHost = [Diagnostics.Process]::Start($info)
    $script:hostIn = [IO.StreamWriter]::new($script:tabsHost.StandardInput.BaseStream, [Text.UTF8Encoding]::new($false))
    $script:hostIn.AutoFlush = $true
    [void](Call 'initialize' @{ settings = @{ merge = $true; reuse = $true; restore = $false }; locale = 'zh'; theme = 'light'; pluginDir = (Split-Path $Plugin); dataDir = $data })
    Start-Sleep -Seconds 2 # the open windows read, the keyboard listened to

    Write-Host '1. Win+E from the desktop'
    ToFront ([WsWinE.Native]::FindWindow('Progman', [NullString]::Value))
    WinE
    Check (Settles '1') "one window with one tab (is: $(Layout))"
    $first = [IntPtr][long]@((Windows).Keys)[0]

    Write-Host '2. Win+E again, that window in front'
    ToFront $first
    Check ([WsWinE.Native]::GetForegroundWindow() -eq $first) 'the Explorer window is in front'
    WinE
    Check (Settles '2') "still one window, now with two tabs (is: $(Layout))"

    Write-Host '3. Ctrl+N in that window'
    ToFront $first
    CtrlN
    Check (Settles '1+2') "a second window (is: $(Layout))"

    Write-Host '4. Win+E, then Ctrl+N at once'
    foreach ($w in @((Windows).Keys)) { if ([IntPtr][long]$w -ne $first) { [void][WsWinE.Native]::PostMessage([IntPtr][long]$w, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) } }
    [void](WaitFor { (Layout) -eq '2' } 10)
    ToFront $first
    WinE
    [void](WaitFor { (Layout) -eq '3' } 12)
    ToFront $first
    CtrlN
    Check (Settles '1+3') "Win+E a third tab, Ctrl+N a window (is: $(Layout))"
} finally {
    if ($script:tabsHost -and -not $script:tabsHost.HasExited) {
        try { [void](Call 'shutdown' @{}) } catch { }
        if (-not $script:tabsHost.WaitForExit(5000)) { $script:tabsHost.Kill() }
    }
    [void](WaitFor {
        $open = Windows
        foreach ($w in $open.Keys) { [void][WsWinE.Native]::PostMessage([IntPtr][long]$w, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
        $open.Count -eq 0
    } 15)
    if (Test-Path $trace) { Write-Host '--- trace'; Get-Content $trace -Encoding UTF8 | ForEach-Object { Write-Host "    $_" } }
    if (Test-Path $WorkDir) { Remove-Item $WorkDir -Recurse -Force }
}
if ($failed.Count) { Write-Host "FAILED: $($failed.Count)"; exit 1 }
Write-Host 'All checks passed.'

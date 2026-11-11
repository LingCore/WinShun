param([Parameter(Mandatory)][ValidateSet('red','green')][string]$Phase)
# Live check for GitHub #82 (typing in an Opus tab that shows This PC) and #242
# (day.rename 1.1.1 inside a real WinShun). Normal rights; a staged copy with
# its own --profile. Every click and key is guarded; aborts rather than retry.
$ErrorActionPreference = 'Stop'
$task = $PSScriptRoot
$root = (Resolve-Path (Join-Path $task '..\..')).Path
$exe = Join-Path $root 'dist\issues-242-82\WinShun\WinShun.exe'
$prof = Join-Path $task 'profile'
$out = Join-Path $task "live-$Phase"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$log = Join-Path $out 'run.txt'
$opusDir = 'C:\Program Files\GPSoftware\Directory Opus'
$rt = Join-Path $opusDir 'dopusrt.exe'
$rightFolder = Join-Path $task 'opus-right'
$ocrImage = [IO.Path]::GetFileNameWithoutExtension((Get-ChildItem (Join-Path $prof 'plugins/day.ocr') -Filter *.exe | Select-Object -First 1).Name)

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class W {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
        public int cbSize; public int flags; public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; public RECT rcCaret; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUT {
        [FieldOffset(0)] public uint type; [FieldOffset(8)] public MOUSEINPUT mi; [FieldOffset(8)] public KEYBDINPUT ki; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc p, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int how);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint thread, ref GUITHREADINFO info);
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
    public static void Aware() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }
    public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassNameW(h, s, 256); return s.ToString(); }
    public static string Text(IntPtr h) { var s = new StringBuilder(256); GetWindowTextW(h, s, 256); return s.ToString(); }
    public static RECT Rect(IntPtr h) { RECT r; GetWindowRect(h, out r); return r; }
    public static uint Pid(IntPtr h) { uint pid; GetWindowThreadProcessId(h, out pid); return pid; }
    public static IntPtr[] Top() { var list = new List<IntPtr>(); EnumWindows((h, l) => { if (IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero); return list.ToArray(); }
    public static IntPtr[] Children(IntPtr parent, string cls) {
        var list = new List<IntPtr>();
        EnumChildWindows(parent, (h, l) => { if (IsWindowVisible(h) && Cls(h) == cls) list.Add(h); return true; }, IntPtr.Zero);
        return list.ToArray(); }
    public static IntPtr Focus(IntPtr root) {
        uint pid; var info = new GUITHREADINFO(); info.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
        return GetGUIThreadInfo(GetWindowThreadProcessId(root, out pid), ref info) ? info.hwndFocus : IntPtr.Zero; }
    public static IntPtr RootAt(int x, int y) { POINT p; p.X = x; p.Y = y; IntPtr h = WindowFromPoint(p); return h == IntPtr.Zero ? h : GetAncestor(h, 2); }
    public static bool Front(IntPtr h) {
        uint pid; uint target = GetWindowThreadProcessId(GetForegroundWindow(), out pid); uint me = GetCurrentThreadId();
        AttachThreadInput(me, target, true); bool ok = SetForegroundWindow(h); AttachThreadInput(me, target, false); return ok; }
    static void Mouse(uint flags) { var i = new INPUT[1]; i[0].type = 0; i[0].mi.dwFlags = flags; SendInput(1, i, Marshal.SizeOf(typeof(INPUT))); }
    public static void Click(int x, int y) {
        SetCursorPos(x, y); System.Threading.Thread.Sleep(120);
        try { Mouse(0x0002); System.Threading.Thread.Sleep(40); } finally { Mouse(0x0004); } }
    public static void Key(ushort vk) {
        var i = new INPUT[2]; i[0].type = 1; i[0].ki.wVk = vk; i[1].type = 1; i[1].ki.wVk = vk; i[1].ki.dwFlags = 2;
        SendInput(2, i, Marshal.SizeOf(typeof(INPUT))); }
}
'@
[W]::Aware()

function Log([string]$s) {
    $line = (Get-Date -Format 'HH:mm:ss.fff') + ' ' + $s
    Add-Content -LiteralPath $log -Value $line -Encoding UTF8
    Write-Output $line
}
function Fmt($r) { '[{0},{1} {2}x{3}]' -f $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T) }
function Shot([string]$name, $r) {
    $w = $r.R - $r.L; $h = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try { $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $h)) } finally { $g.Dispose() }
    $bmp.Save((Join-Path $out "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}
function Opus([string]$arguments) {
    $p = Start-Process -FilePath $rt -ArgumentList $arguments -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(5000)) { $p.Kill(); throw "dopusrt did not return: $arguments" }
}
function OpusPaths {
    $file = Join-Path $out ('paths-' + [Guid]::NewGuid().ToString('N') + '.xml')
    Opus ('/info "' + $file + '",paths')
    $xml = [IO.File]::ReadAllText($file); [IO.File]::Delete($file)
    return $xml
}
function WsProcess { Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } }
function WsWindows([uint32]$wsPid) {
    [W]::Top() | Where-Object { [W]::Pid($_) -eq $wsPid } | ForEach-Object {
        $r = [W]::Rect($_); if (($r.R - $r.L) -gt 200 -and ($r.B - $r.T) -gt 40) { [pscustomobject]@{ h = $_; cls = [W]::Cls($_); rect = $r } } }
}
function PluginWindows([string]$image) {
    $ids = @(Get-Process $image -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$prof\*" } | ForEach-Object { [uint32]$_.Id })
    [W]::Top() | Where-Object { $ids -contains [W]::Pid($_) } | ForEach-Object {
        $r = [W]::Rect($_); if (($r.R - $r.L) -gt 200) { [pscustomobject]@{ h = $_; cls = [W]::Cls($_); title = [W]::Text($_); rect = $r } } }
}

$startedOpus = $false
$wasMinimized = $false
$original = @{}
$lister = [IntPtr]::Zero
try {
    if (WsProcess) { throw 'a staged WinShun is already running' }
    if (Get-Process WinShun -ErrorAction SilentlyContinue) { throw 'the user''s WinShun is running; not starting a second one' }

    # --- Opus: the user's lister, or one started for this run -----------------
    $lister = [W]::Top() | Where-Object { [W]::Cls($_) -eq 'dopus.lister' } | Select-Object -First 1
    if (-not $lister) {
        Start-Process -FilePath (Join-Path $opusDir 'dopus.exe') -WindowStyle Normal
        $startedOpus = $true
        foreach ($i in 1..40) { Start-Sleep -Milliseconds 500; $lister = [W]::Top() | Where-Object { [W]::Cls($_) -eq 'dopus.lister' } | Select-Object -First 1; if ($lister) { break } }
        if (-not $lister) { throw 'no Opus lister came up' }
        Start-Sleep -Seconds 2
    }
    Log ("lister 0x{0:x} {1}, started here: {2}" -f [int64]$lister, (Fmt ([W]::Rect($lister))), $startedOpus)
    $wasMinimized = [W]::IsIconic($lister)
    if ($wasMinimized) { [void][W]::ShowWindow($lister, 9); Start-Sleep -Milliseconds 800; Log ("lister was minimized; restored to " + (Fmt ([W]::Rect($lister)))) }
    $xml = OpusPaths
    Log ('paths before: ' + ($xml -replace '\s+', ' '))
    foreach ($m in [regex]::Matches($xml, '<path\b([^>]*)>([^<]*)</path>')) {
        if ($m.Groups[1].Value -match 'tab_state="\d"' -and $m.Groups[1].Value -match 'side="(\d)"') { $original[$Matches[1]] = $m.Groups[2].Value }
    }
    if ($original.Count -ne 2) { throw "expected a lister with two file lists, got $($original.Count) visible tabs" }
    Opus '/acmd Go /mycomputer OPENINLEFT'
    Opus ('/acmd Go "' + $rightFolder + '" OPENINRIGHT')
    Start-Sleep -Milliseconds 1500
    $lists = @([W]::Children($lister, 'dopus.filedisplay') | Sort-Object { [W]::Rect($_).L })
    if ($lists.Count -ne 2) { throw "expected two file displays, found $($lists.Count)" }
    Log ("left list 0x{0:x} {1} (This PC); right list 0x{2:x} {3} (folder)" -f [int64]$lists[0], (Fmt ([W]::Rect($lists[0]))), [int64]$lists[1], (Fmt ([W]::Rect($lists[1]))))
    Log ('paths set: ' + ((OpusPaths) -replace '\s+', ' '))

    # --- the staged WinShun -------------------------------------------------------
    $ver = (Get-Item $exe).VersionInfo.FileVersion
    $hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.Substring(0, 12)
    Start-Process -FilePath $exe -ArgumentList '--profile', ('"' + $prof + '"'), '--background' -WindowStyle Normal
    $ws = $null
    foreach ($i in 1..40) { Start-Sleep -Milliseconds 500; $ws = WsProcess | Select-Object -First 1; if ($ws) { break } }
    if (-not $ws) { throw 'WinShun did not start' }
    Start-Sleep -Seconds 6
    $wsPid = [uint32]$ws.Id
    Log "WinShun $ver ($hash) pid $wsPid up; windows now: $((WsWindows $wsPid | ForEach-Object { $_.cls + ' ' + (Fmt $_.rect) }) -join '; ')"

    function TypeIn([string]$name, [IntPtr]$list) {
        $r = [W]::Rect($list); $x = [int](($r.L + $r.R) / 2); $y = $r.B - 150
        if ([W]::RootAt($x, $y) -ne $lister) { [void][W]::Front($lister); Start-Sleep -Milliseconds 600 }
        if ([W]::RootAt($x, $y) -ne $lister) { throw "$name`: the lister is not on top at the click point" }
        [W]::Click($x, $y); Start-Sleep -Milliseconds 600
        if ([W]::GetForegroundWindow() -ne $lister -or [W]::Focus($lister) -ne $list) { throw "$name`: the list did not take the keyboard" }
        [W]::Key(0x51); Start-Sleep -Milliseconds 1800
        $fg = [W]::GetForegroundWindow()
        $boxes = @(WsWindows $wsPid)
        Shot $name ([W]::Rect($lister))
        $inList = $false
        foreach ($b in $boxes) { if ($b.rect.L -ge $r.L - 2 -and $b.rect.R -le $r.R + 2 -and $b.rect.B -le $r.B + 2) { $inList = $true } }
        Log ("{0}: box shown {1} {2}; inside that list {3}; foreground {4} (WinShun: {5}); Opus focus still the list: {6}" -f $name, ($boxes.Count -gt 0),
            (($boxes | ForEach-Object { Fmt $_.rect }) -join ' '), $inList, [W]::Cls($fg), ([W]::Pid($fg) -eq $wsPid), ([W]::Focus($lister) -eq $list))
        foreach ($i in 1..2) { if ([W]::Pid([W]::GetForegroundWindow()) -eq $wsPid) { [W]::Key(0x1B); Start-Sleep -Milliseconds 500 } }
        if ([W]::GetForegroundWindow() -eq $lister) { [W]::Key(0x1B); Start-Sleep -Milliseconds 300 } # Opus's own find field, if the key went there
        if (@(WsWindows $wsPid).Count -gt 0) { [W]::Click($x, $y); Start-Sleep -Milliseconds 500 } # an IME composition kept it: focus away closes it
        Log ("{0}: after Esc, box shown {1}" -f $name, (@(WsWindows $wsPid).Count -gt 0))
    }
    TypeIn 'opus-left-thispc' $lists[0]
    TypeIn 'opus-right-folder' $lists[1]

    if ($Phase -eq 'green') {
        # --- #242: three launches of day.rename after a fresh start ------------
        foreach ($plugin in @(@{ key = 'pl'; image = 'BatchRename' }, @{ key = 'ocr'; image = $ocrImage })) {
        foreach ($n in 1..3) {
            $tag = $plugin.image + '-' + $n
            Start-Process -FilePath $exe -ArgumentList '--profile', ('"' + $prof + '"'), '--query', $plugin.key -WindowStyle Normal
            $ok = $false
            foreach ($i in 1..25) { Start-Sleep -Milliseconds 200; if ([W]::Pid([W]::GetForegroundWindow()) -eq $wsPid) { $ok = $true; break } }
            if (-not $ok) { throw "plugin $tag`: the launcher did not come to the front" }
            Start-Sleep -Milliseconds 3000 # the keyword typed, no Enter: nothing of the plugin's may show
            $typed = @(PluginWindows $plugin.image)
            $box = @(WsWindows $wsPid)
            if ($box.Count) { Shot "plugin-$tag-typed" $box[0].rect }
            Log ("plugin {0}: keyword typed, 3 s, no Enter: plugin windows {1}; launcher windows {2}" -f $tag, $typed.Count, $box.Count)
            if ([W]::Pid([W]::GetForegroundWindow()) -ne $wsPid) { throw "plugin $tag`: the launcher lost the front before Enter" }
            $clock = [Diagnostics.Stopwatch]::StartNew()
            [W]::Key(0x0D)
            $shown = @()
            while ($clock.ElapsedMilliseconds -lt 10000) { Start-Sleep -Milliseconds 100; $shown = @(PluginWindows $plugin.image); if ($shown.Count) { break } }
            $ms = $clock.ElapsedMilliseconds
            Start-Sleep -Milliseconds 1500
            $fg = [W]::GetForegroundWindow()
            $box = @(WsWindows $wsPid)
            $shown = @(PluginWindows $plugin.image)
            if ($shown.Count) { Shot "plugin-$tag-after" $shown[0].rect }
            Log ("plugin {0}: Enter -> plugin window {1} after {2} ms ({3}); launcher still shown {4}; foreground is the plugin {5} ({6})" -f $tag,
                ($shown.Count -gt 0), $ms, (($shown | ForEach-Object { $_.title + ' ' + (Fmt $_.rect) }) -join ' '), ($box.Count -gt 0),
                ($shown.Count -gt 0 -and $shown.h -contains $fg), [W]::Cls($fg))
            foreach ($s in $shown) { [void][W]::PostMessageW($s.h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) } # WM_CLOSE: the plugin hides it
            Start-Sleep -Milliseconds 1200
            foreach ($i in 1..2) { if ([W]::Pid([W]::GetForegroundWindow()) -eq $wsPid) { [W]::Key(0x1B); Start-Sleep -Milliseconds 400 } }
            Log ("plugin {0}: closed; plugin windows {1}, launcher windows {2}" -f $tag, @(PluginWindows $plugin.image).Count, @(WsWindows $wsPid).Count)
        }
        }
    }
    foreach ($line in (Get-ChildItem -LiteralPath $prof -Recurse -Include WinShun.log, events.jsonl -File | Select-String -Pattern 'Opus typing|Folder typing|typing-handoff' | ForEach-Object { $_.Line })) { Log "log: $line" }
}
catch { Log ("ABORTED: " + $_.Exception.Message + ' @ ' + $_.InvocationInfo.ScriptLineNumber) }
finally {
    WsProcess | ForEach-Object { $_.Kill(); [void]$_.WaitForExit(5000) }
    Start-Sleep -Milliseconds 800
    Get-Process BatchRename, $ocrImage, WinShunPluginHost -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$prof\*" -or $_.Path -like (Split-Path $exe) + '\*' } | ForEach-Object { $_.Kill() }
    if ($Phase -eq 'green' -and $lister -ne [IntPtr]::Zero -and [W]::IsWindow($lister) -and $original.Count -eq 2) {
        # tst_shelllaunch's live Opus case: the right list (the fixture folder) has the keyboard.
        try {
            $lists = @([W]::Children($lister, 'dopus.filedisplay') | Sort-Object { [W]::Rect($_).L })
            $r = [W]::Rect($lists[1]); $x = [int](($r.L + $r.R) / 2); $y = $r.B - 150
            if ([W]::RootAt($x, $y) -ne $lister) { [void][W]::Front($lister); Start-Sleep -Milliseconds 600 }
            if ([W]::RootAt($x, $y) -eq $lister) {
                [W]::Click($x, $y); Start-Sleep -Milliseconds 600
                $info = New-Object Diagnostics.ProcessStartInfo (Join-Path $root 'build\press-feedback\tst_shelllaunch.exe'), ('opusTypingNavigation -o "' + (Join-Path $out 'tst_shelllaunch.txt') + '",txt')
                $info.UseShellExecute = $false; $info.CreateNoWindow = $true; $info.RedirectStandardOutput = $true
                $info.EnvironmentVariables['WINSHUN_OPUS_TEST_FOLDER'] = $rightFolder
                $info.EnvironmentVariables['PATH'] = 'C:\Qt\6.12.0\msvc2022_64\bin;' + $env:PATH
                $p = [Diagnostics.Process]::Start($info); [void]$p.StandardOutput.ReadToEnd(); [void]$p.WaitForExit(60000)
                $text = [IO.File]::ReadAllText((Join-Path $out 'tst_shelllaunch.txt'))
                Log ("tst_shelllaunch opusTypingNavigation: exit {0}; {1}" -f $p.ExitCode, (($text -split "`n" | Where-Object { $_ -match '^(Totals|PASS|FAIL|SKIP)' }) -join ' | '))
            } else { Log 'tst_shelllaunch: skipped, lister not on top' }
        } catch { Log ('tst_shelllaunch: ' + $_.Exception.Message) }
    }
    if ($lister -ne [IntPtr]::Zero -and [W]::IsWindow($lister) -and $original.Count -eq 2) {
        try {
            Opus ('/acmd Go "' + $original['1'] + '" OPENINLEFT'); Opus ('/acmd Go "' + $original['2'] + '" OPENINRIGHT')
            Start-Sleep -Milliseconds 800
            Log ('paths restored: ' + ((OpusPaths) -replace '\s+', ' '))
            if ($startedOpus) { Opus '/acmd Close PROGRAM'; Log 'Opus, started here, closed' }
            elseif ($wasMinimized) { [void][W]::ShowWindow($lister, 6); Log 'lister minimized again' }
        } catch { Log ('restore: ' + $_.Exception.Message) }
    }
    Log ("left button down: {0}; staged WinShun left: {1}" -f (([W]::GetAsyncKeyState(1) -band 0x8000) -ne 0), @(WsProcess).Count)
}

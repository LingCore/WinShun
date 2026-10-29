# Elevated input agent for the shots: sets the windows up by real input and
# has shots.py cut them out. Run it from an elevated PowerShell (WinShun runs
# elevated; input into it and the backdrop behind it need the same):
#   agent.ps1 scenes\all.txt   runs that file, then exits
#   agent.ps1                  runs each batch written to work\cmd.txt, until 15 idle minutes
# It logs to work\agent.txt. Every key and click first checks that WinShun (or
# a program `allow`ed) is in front, and a click also that the point is on that
# window; a batch stops at the first check that fails. Never Ctrl+1/2/3
# (PixPin's hotkeys on this machine). Keys are virtual-key codes in hex.
#   # comment | run <file> | let <name> <value> ({name} in later lines becomes the value) | sleep <ms> | exit
#   open <query> | settings | allow <process name>
#   tap <vk> | down <vk> | up <vk> | chord <mod vk> <vk> | type <text> (ASCII; other text: clip + Ctrl+V)
#   clickfg <dx> <dy> (physical px from the front window's frame) | clickb <dx> <up from bottom> | dblclickb (the same, twice)
#   clickws <x> <y> (on a window of WinShun's) | clickbox <x> <y> (on its taskbar box) | wheel <dx> <dy> <delta>
#   dragws <x1> <y1> <x2> <y2> (left button held from one point to the other, both on a window of WinShun's)
#   dragfg <dx1> <dy1> <dx2> <dy2> (the same from the front window's frame: a shape drawn in the edit window)
#   shot <name> (the front window) | shotabs <name> <x> <y> <w> <h>        -> work\shots\<name>.png
#   matte <name> [winshun|beside] (cut out by shots.py shoot)               -> work\shots\<name>.png
#   scene [--theme light|dark] [--wallpaper NAME] | restore
#   profile start | profile stop (WinShun on work\profile, made afresh; then the user's own again)
#   notepad <file> | paint <file> (files of this folder) | explorer open <folder of the demo> | explorer close
#   explorer place <x> <y> <w> <h> (the demo's Explorer windows restored and moved there, physical px)
#   canceldialogs | clip <text> | clipfile <path>
#   cover on [paper] | cover off (a window of the wallpaper over the whole screen, so nothing of this computer shows behind the program shot)
#   edge <file of scenes\> | closewin <title part> (Edge as an app window on a page; closewin: a window of an allowed program)
#   place <title part> <x> <y> <w> <h> | front <title part> | hover <x> <y> (the pointer, over a window of an allowed program or WinShun)
#   pie start <item> <dx> <dy> | pie hover <sector> <of> <left|right> [<turn deg> <reach>] | pie end (the drop pie: a file of the demo's Explorer window
#     dragged, shaken at (dx,dy) from the frame, held over a sector; pie end: Esc, button up -- never released over a sector)
#   matte <name> [winshun|beside|hold] [with <window class>]   (hold: the pointer stays, a drag going on)
#   profile start [--unelevated] ...: WinShun not elevated (a drop from Explorer can't reach an elevated one)
param([string]$Scene, [string]$Exe, [string]$UserExe) # Exe: another WinShun.exe than dist's (a build under test); UserExe: the user's own copy when not dist's (run.ps1 passes the one that was running)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class A {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int size);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint data; public uint dwFlags; public uint time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size=40)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; [FieldOffset(8)] public MOUSEINPUT mi; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    public static void Key(ushort vk, bool up) {
        var i = new INPUT[1]; i[0].type = 1; i[0].ki.wVk = vk;
        bool ext = vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28 || vk == 0x2E || vk == 0x24 || vk == 0x23 || vk == 0x21 || vk == 0x22;
        i[0].ki.dwFlags = (up ? 2u : 0u) | (ext ? 1u : 0u);
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Type(string text) {
        foreach (char c in text) {
            var i = new INPUT[2];
            i[0].type = 1; i[0].ki.wScan = c; i[0].ki.dwFlags = 4;
            i[1].type = 1; i[1].ki.wScan = c; i[1].ki.dwFlags = 6;
            SendInput(2, i, Marshal.SizeOf(typeof(INPUT)));
        }
    }
    public static void Wheel(int delta) {
        var i = new INPUT[1]; i[0].type = 0; i[0].mi.dwFlags = 0x800; i[0].mi.data = (uint)delta;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Button(bool up) {
        var i = new INPUT[1]; i[0].type = 0; i[0].mi.dwFlags = up ? 4u : 2u;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Click() {
        var i = new INPUT[2]; i[0].type = 0; i[0].mi.dwFlags = 2; i[1].type = 0; i[1].mi.dwFlags = 4;
        SendInput(2, i, Marshal.SizeOf(typeof(INPUT)));
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
    [DllImport("winmm.dll")] public static extern uint timeBeginPeriod(uint p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] public static extern uint GetDpiForSystem();
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
    delegate bool EnumProcType(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProcType f, IntPtr l);
    public static string Title(IntPtr h) { var s = new System.Text.StringBuilder(300); GetWindowTextW(h, s, 300); return s.ToString(); }
    public static string Class(IntPtr h) { var s = new System.Text.StringBuilder(128); GetClassNameW(h, s, 128); return s.ToString(); }
    // the first visible top-level window whose title has `part` in it and which is at least 100 px each way
    public static IntPtr FindTitle(string part) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            if (IsWindowVisible(h) && Title(h).IndexOf(part, StringComparison.OrdinalIgnoreCase) >= 0) {
                RECT r; GetWindowRect(h, out r);
                if (r.R - r.L >= 100 && r.B - r.T >= 100) { found = h; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    // the first visible window of a process at least minW wide that is not a drag image: {l, t, r, b}, or null
    public static int[] BigWindow(uint pid, int minW) {
        int[] found = null;
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            RECT r;
            if (Pid(h) == pid && IsWindowVisible(h) && GetWindowRect(h, out r) && r.R - r.L >= minW && Class(h) != "SysDragImage") {
                found = new int[] { r.L, r.T, r.R, r.B }; return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
    // to the top of the (not topmost) windows, without activating it
    public static void Raise(IntPtr h) { SetWindowPos(h, IntPtr.Zero, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010); }
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    public static int[] WinRect(IntPtr h) { RECT r; GetWindowRect(h, out r); return new int[] { r.L, r.T, r.R - r.L, r.B - r.T }; }
    // how many visible windows of this class there are (SysDragImage: the picture of a file in the air)
    public static int CountClass(string cls) {
        int n = 0;
        EnumWindows(delegate (IntPtr h, IntPtr l) { if (IsWindowVisible(h) && Class(h) == cls) n++; return true; }, IntPtr.Zero);
        return n;
    }
    public static void Front(IntPtr h) {
        if (IsIconic(h)) ShowWindow(h, 9);
        uint a = GetCurrentThreadId(); uint pid;
        uint b = GetWindowThreadProcessId(GetForegroundWindow(), out pid);
        if (a != b) AttachThreadInput(a, b, true);
        SetForegroundWindow(h);
        if (a != b) AttachThreadInput(a, b, false);
    }
    // the pointer to a place by input (absolute, over the whole virtual screen), as a hand moves it
    public static void MoveAbs(int x, int y) {
        int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
        var i = new INPUT[1]; i[0].type = 0;
        i[0].mi.dx = (int)Math.Round((x - vx) * 65535.0 / (vw - 1));
        i[0].mi.dy = (int)Math.Round((y - vy) * 65535.0 / (vh - 1));
        i[0].mi.dwFlags = 0x8000u | 0x4000u | 0x0001u;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static uint Pid(IntPtr h) { uint p; GetWindowThreadProcessId(h, out p); return p; }
    public static RECT Frame(IntPtr h) { RECT r; DwmGetWindowAttribute(h, 9, out r, 16); return r; }
    public static IntPtr RootAt(int x, int y) { var p = new POINT(); p.X = x; p.Y = y; return GetAncestor(WindowFromPoint(p), 2); }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    public static string ClassAt(int x, int y) { var p = new POINT(); p.X = x; p.Y = y; var s = new System.Text.StringBuilder(64); GetClassNameW(WindowFromPoint(p), s, 64); return s.ToString(); }
}
'@
[A]::SetProcessDPIAware() | Out-Null
[A]::timeBeginPeriod(1) | Out-Null
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here '..\..')).Path
$work = Join-Path $here 'work'
$shots = Join-Path $work 'shots'
$profileDir = Join-Path $work 'profile'
New-Item -ItemType Directory -Force $shots | Out-Null
$cmdFile = Join-Path $work 'cmd.txt'
$log = Join-Path $work 'agent.txt'
$userExe = if ($UserExe) { $UserExe } else { Join-Path $root 'dist\WinShun\WinShun.exe' } # the user's own copy
$exe = if ($Exe) { $Exe } else { $userExe }
$tool = Join-Path $here 'shots.py'
$demo = 'F:\Win' + [char]0x987A + [char]0x6F14 + [char]0x793A # the demo folder the shots show (stage.py's DEMO)
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { $python = (Get-Command py -ErrorAction Stop).Source }
[Console]::OutputEncoding = [Text.Encoding]::UTF8; $env:PYTHONIOENCODING = 'utf-8'

function Note($s) { # never throws: a reader may hold the log for a moment
    $line = "{0:HH:mm:ss.fff} {1}`r`n" -f (Get-Date), $s
    for ($i = 0; $i -lt 40; $i++) { try { [IO.File]::AppendAllText($log, $line, [Text.Encoding]::UTF8); return } catch { Start-Sleep -Milliseconds 50 } }
}
$allowed = @()
function WsPid { $p = Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Select-Object -First 1; if ($p) { [uint32]$p.Id } else { 0 } }
function InFront {
    $pid0 = [A]::Pid([A]::GetForegroundWindow())
    # any WinShun of $exe: `open` starts a second one that hands the query over and exits, and WsPid may pick that one
    if (@(Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | ForEach-Object Id) -contains $pid0) { return $true }
    try { $name = (Get-Process -Id $pid0).ProcessName } catch { return $false }
    return $allowed -contains $name
}
function Guard { if (-not (InFront)) { throw "not in front (pid $([A]::Pid([A]::GetForegroundWindow())))" } }
function Vk($s) { return [uint16]([Convert]::ToInt32($s, 16)) }
function WaitFront { $sw = [Diagnostics.Stopwatch]::StartNew(); while ($sw.ElapsedMilliseconds -lt 5000 -and -not (InFront)) { Start-Sleep -Milliseconds 20 }; Guard }
function Local($p) { # a path in this folder (relative ones from here), or an error
    $full = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $here $p }))
    if (-not $full.StartsWith($here + '\')) { throw "only files of $here here: $p" }
    return $full
}
function Shots { # shots.py with these arguments, its output in the log
    $out = & $python $tool @args 2>&1 | Out-String
    Note $out.Trim()
    if ($LASTEXITCODE -ne 0) { throw "shots.py $($args[0]) failed ($LASTEXITCODE)" }
}
function QuitOne($path) { # one WinShun.exe of this path off (the program ends on --quit; a second one hands the message over)
    $running = @(Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $path })
    if (-not $running) { return }
    Start-Process -FilePath $path -ArgumentList '--quit' -WindowStyle Normal
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (@(Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $path }) -and $sw.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 200 }
    if (@(Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $path })) { throw 'WinShun did not quit' }
}
function QuitWs { # the copy shots are taken with, and the user's own (dist) if that is another: one instance only
    QuitOne $exe
    if ($userExe -ne $exe) { QuitOne $userExe }
}
function RunUnelevated($program, $arguments, $show = 1) {
    if ($program -eq 'explorer.exe') { $program = Join-Path $env:windir 'explorer.exe' } # started by the shell, not by this elevated process: a normal user's program
    $shell = New-Object -ComObject Shell.Application
    $none = 0
    $desktop = $shell.Windows().FindWindowSW([ref]$none, [ref]$none, 8, [ref]$none, 1) # SWC_DESKTOP, SWFO_NEEDDISPATCH
    $desktop.Document.Application.ShellExecute($program, $arguments, (Split-Path $program), 'open', $show)
}
function StartWs($more, $program = $exe, [bool]$unelevated = $false) {
    if ($unelevated) { # through cmd, which hands the environment on (WINSHUN_RECENT=own, the log's info lines) to the program
        $envs = @(); if ($program -ne $userExe) { $envs += 'set "WINSHUN_LOG_INFO=1"' }; if ($env:WINSHUN_RECENT) { $envs += 'set "WINSHUN_RECENT=' + $env:WINSHUN_RECENT + '"' }
        $line = '/c ' + (($envs + @('ver>nul')) -join '&& ') + '&& start "" "' + $program + '" ' + ((@('--background') + $more) -join ' ')
        RunUnelevated (Join-Path $env:windir 'System32\cmd.exe') $line 0
    }
    else { Start-Process -FilePath $program -ArgumentList (@('--background') + $more) -WindowStyle Normal }
    Start-Sleep -Seconds 6
    $up = Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $program } | Select-Object -First 1
    if (-not $up) { throw 'WinShun did not stay up' }
    Note "WinShun up (pid $($up.Id), unelevated $unelevated) $more"
}
function Save($bmp, $name) { $bmp.Save((Join-Path $shots "$name.png"), [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose() }


function CoverOff { # the wallpaper window away
    $stop = Join-Path $work 'cover.stop'
    if (-not $script:coverProc) { return }
    New-Item -ItemType File $stop -Force | Out-Null
    $script:coverProc.WaitForExit(8000) | Out-Null
    if (-not $script:coverProc.HasExited) { Stop-Process -Id $script:coverProc.Id -Force }
    $script:coverProc = $null
    Remove-Item $stop -ErrorAction SilentlyContinue
}
# ---- the drop pie: a left-button drag of a file in Explorer, held while the pie is looked at. The button is
# only ever let go by `pie end` (or by the cleanup when a batch fails): Esc first, so nothing is dropped.
$script:dragging = $false
$script:pie = $null
function Glide([int]$x, [int]$y, [int]$ms) {
    $p = New-Object A+POINT
    [A]::GetCursorPos([ref]$p) | Out-Null
    $x0 = $p.X; $y0 = $p.Y
    $steps = [Math]::Max(1, [int]($ms / 8))
    for ($k = 1; $k -le $steps; $k++) {
        [A]::MoveAbs([int]($x0 + ($x - $x0) * $k / $steps), [int]($y0 + ($y - $y0) * $k / $steps))
        Start-Sleep -Milliseconds 8
    }
}
function ExplorerOfDemo { # the hwnd of an Explorer window showing a folder of the demo
    foreach ($w in @((New-Object -ComObject Shell.Application).Windows())) {
        try { if ($w.Document.Folder.Self.Path.StartsWith($demo)) { return [IntPtr]$w.HWND } } catch {}
    }
    return [IntPtr]::Zero
}
function ItemRect($hwnd, [string]$name) { # the screen rect of a list item of an Explorer window, by UI Automation
    $root = [Windows.Automation.AutomationElement]::FromHandle($hwnd)
    $c1 = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty, $name)
    $c2 = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)
    $el = $root.FindFirst([Windows.Automation.TreeScope]::Descendants, (New-Object Windows.Automation.AndCondition($c1, $c2)))
    if (-not $el) { return $null }
    $r = $el.Current.BoundingRectangle
    return @([int]$r.Left, [int]$r.Top, [int]$r.Right, [int]$r.Bottom)
}
function ItemNameAt([int]$x, [int]$y) {
    try {
        $e = [Windows.Automation.AutomationElement]::FromPoint((New-Object Windows.Point($x, $y)))
        $w = [Windows.Automation.TreeWalker]::ControlViewWalker
        for ($i = 0; $e -and $i -lt 6; $i++) {
            if ($e.Current.ControlType -eq [Windows.Automation.ControlType]::ListItem) { return $e.Current.Name }
            $e = $w.GetParent($e)
        }
    } catch {}
    return ''
}
function PieStart([string]$spec) { # <item> <dx> <dy>: where the shake is, from the Explorer window's frame
    if ($script:dragging) { throw 'a drag is going on already' }
    $q = $spec.Trim() -split '\s+'
    $dy = [int]$q[$q.Count - 1]; $dx = [int]$q[$q.Count - 2]
    $item = ($q[0..($q.Count - 3)]) -join ' '
    $h = ExplorerOfDemo
    if ($h -eq [IntPtr]::Zero) { throw 'pie: no Explorer window on the demo folder' }
    [A]::Front($h); Start-Sleep -Milliseconds 500; Guard
    if ([A]::Pid([A]::GetForegroundWindow()) -ne [A]::Pid($h)) { throw 'pie: Explorer is not in front' }
    $r = ItemRect $h $item
    if (-not $r) { throw "pie: no item '$item'" }
    $sx = $r[0] + 45; $sy = [int](($r[1] + $r[3]) / 2)
    Glide $sx $sy 250; Start-Sleep -Milliseconds 250
    if ([A]::RootAt($sx, $sy) -ne $h) { throw 'pie: the start point is not on the Explorer window' }
    if ((ItemNameAt $sx $sy) -ne $item) { throw "pie: the item under the pointer is not '$item'" }
    $f = [A]::Frame($h)
    $tx = $f.L + $dx; $ty = $f.T + $dy
    $script:pie = @{ hwnd = $h; item = $item }
    $script:dragging = $true
    [A]::Button($false)
    Start-Sleep -Milliseconds 120
    Glide ($sx + 12) ($sy + 2) 50; Glide ($sx + 40) ($sy + 4) 80; Start-Sleep -Milliseconds 80
    Glide $tx $ty 450; Start-Sleep -Milliseconds 150
    if ([A]::Pid([A]::GetForegroundWindow()) -ne [A]::Pid($h)) { throw 'pie: Explorer lost the front during the drag' }
    # three quick swings
    $c = New-Object A+POINT; [A]::GetCursorPos([ref]$c) | Out-Null
    $px = $c.X; $py = $c.Y
    Glide ($px + 40) $py 60; Glide ($px + 130) ($py + 3) 90; Glide ($px + 40) $py 100
    Glide ($px + 130) ($py - 3) 100; Glide ($px + 40) $py 100; Glide ($px + 90) $py 60
    $win = $null
    for ($i = 0; $i -lt 30; $i++) {
        $win = [A]::BigWindow((WsPid), 500)
        if ($win -and ($win[2] - $win[0]) -ge 700) { break }
        Start-Sleep -Milliseconds 60
    }
    if (-not $win) {
        $c = New-Object A+POINT; [A]::GetCursorPos([ref]$c) | Out-Null
        Note "pie: none after the shake; pointer $($c.X),$($c.Y), drag images $([A]::CountClass('SysDragImage')), button down $(([A]::GetAsyncKeyState(1) -band 0x8000) -ne 0), front pid $([A]::Pid([A]::GetForegroundWindow())) (Explorer's $([A]::Pid($h)))"
        throw 'pie: no pie after the shake'
    }
    Start-Sleep -Milliseconds 250
    $win = [A]::BigWindow((WsPid), 500)
    $dpr = [A]::GetDpiForSystem() / 96.0
    $script:pie.rect = $win
    $script:pie.cx = [int]($win[0] + 378 * $dpr); $script:pie.cy = [int]($win[1] + 276 * $dpr)
    Note "pie up: window $($win -join ','), centre $($script:pie.cx),$($script:pie.cy), dpr $dpr"
    $c = New-Object A+POINT; [A]::GetCursorPos([ref]$c) | Out-Null
    Glide ($c.X + 3) ($c.Y + 2) 16; Start-Sleep -Milliseconds 260
}
function PieHover([string]$spec) { # <sector> <of> <left|right>: across the inner half, then onto the sector of the outer ring
    if (-not $script:dragging) { throw 'pie hover: no drag' }
    $q = $spec -split '\s+'
    $k = [int]$q[0]; $n = [int]$q[1]; $side = $q[2]
    $turn = if ($q.Count -gt 3) { [double]$q[3] } else { 0 }        # degrees off the sector's middle (clockwise)
    $reach = if ($q.Count -gt 4) { [double]$q[4] } else { 173 }     # logical px from the centre (the ring is 120-226)
    $dpr = [A]::GetDpiForSystem() / 96.0
    $cx = $script:pie.cx; $cy = $script:pie.cy
    $inner = [int](85 * $dpr); $outer = [int]($reach * $dpr) # a point of the inner half, a point of the outer ring (logical px from the centre)
    $ix = if ($side -eq 'left') { $cx - $inner } else { $cx + $inner }
    Glide $ix $cy 180; Start-Sleep -Milliseconds 200
    $a = 2 * [Math]::PI * $k / $n + $turn * [Math]::PI / 180
    Glide ([int]($cx + $outer * [Math]::Sin($a))) ([int]($cy - $outer * [Math]::Cos($a))) 220
    Start-Sleep -Milliseconds 700
    if ([A]::Pid([A]::GetForegroundWindow()) -ne [A]::Pid($script:pie.hwnd)) { throw 'pie: Explorer lost the front' }
}
function PieEnd { # Esc (the source cancels the drag), then the button up
    if (-not $script:dragging) { return }
    try {
        if ([A]::Pid([A]::GetForegroundWindow()) -eq [A]::Pid($script:pie.hwnd)) { [A]::Key(0x1B, $false); [A]::Key(0x1B, $true) }
        Start-Sleep -Milliseconds 500
    } finally {
        [A]::Button($true)
        $script:dragging = $false
        Start-Sleep -Milliseconds 300
    }
    if (([A]::GetAsyncKeyState(1) -band 0x8000) -ne 0) { Note 'WARNING: the left button is still down' }
}

$vars = @{}
function RunLines($lines) {
    foreach ($line in $lines) {
        foreach ($k in $vars.Keys) { $line = $line.Replace("{$k}", $vars[$k]) }
        $a = $line.Trim() -split '\s+', 2
        if (-not $a[0] -or $a[0].StartsWith('#')) { continue }
        $arg = if ($a.Count -gt 1) { $a[1] } else { '' }
        switch ($a[0]) {
            'run' { RunLines (Get-Content (Local $arg) -Encoding UTF8) }
            'let' { $p = $arg -split '\s+', 2; $vars[$p[0]] = $p[1] }
            'open' { Start-Process -FilePath $exe -ArgumentList '--query', $arg -WindowStyle Normal; WaitFront; Start-Sleep -Milliseconds 600 }
            'settings' { Start-Process -FilePath $exe -ArgumentList '--settings' -WindowStyle Normal; WaitFront; Start-Sleep -Milliseconds 800 }
            'allow' { $script:allowed += $arg }
            'tap' { Guard; $k = Vk $arg; [A]::Key($k, $false); [A]::Key($k, $true) }
            'ifseed' { # <command>: only when WinShun took the system clipboard's content into the demo history at its start (a row that isn't the demo's own): that is what the Delete after it is for; with nothing seeded the Delete would take the demo's picture
                $n = & $python -I (Join-Path $here 'seedrows.py') (Join-Path $profileDir 'clipboard\clipboard.db')
                if ([int]$n -gt 0) { RunLines @($arg) } else { Note "ifseed: no seed row in the history, '$arg' skipped" }
            }
            'tapopt' { if (InFront) { $k = Vk $arg; [A]::Key($k, $false); [A]::Key($k, $true) } else { Note "tapopt $arg skipped: not in front" } } # a key only when WinShun's still in front (the Esc that closes what a first Esc may have closed already)
            'down' { Guard; [A]::Key((Vk $arg), $false) }
            'up' { [A]::Key((Vk $arg), $true) }
            'chord' {
                $p = $arg -split '\s+'; $m = Vk $p[0]; $k = Vk $p[1]
                if ($m -eq 0x11 -and $k -ge 0x31 -and $k -le 0x33) { throw 'never Ctrl+1/2/3 here' }
                Guard; [A]::Key($m, $false); [A]::Key($k, $false); [A]::Key($k, $true); [A]::Key($m, $true)
            }
            { $_ -in 'clickfg', 'clickb', 'dblclickb' } {
                Guard
                $p = $arg -split '\s+'; $h = [A]::GetForegroundWindow(); $r = [A]::Frame($h)
                $x = $r.L + [int]$p[0]
                $y = if ($a[0] -eq 'clickfg') { $r.T + [int]$p[1] } else { $r.B - [int]$p[1] }
                [A]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 120
                if ([A]::RootAt($x, $y) -ne $h) { throw "click at $x,$y would land on another window" }
                Guard; [A]::Click()
                if ($a[0] -eq 'dblclickb') { Start-Sleep -Milliseconds 90; [A]::Click() }
            }
            'clickws' { # absolute physical px, on a window of WinShun's own (the bar by a dialog)
                $p = $arg -split '\s+'; $x = [int]$p[0]; $y = [int]$p[1]
                [A]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 120
                if ([A]::Pid([A]::RootAt($x, $y)) -ne (WsPid)) { throw "click at $x,$y would land on another program" }
                [A]::Click()
            }
            'dragws' { # absolute physical px, both ends on a window of WinShun's own (text selected with the mouse)
                $p = $arg -split '\s+'; $x1 = [int]$p[0]; $y1 = [int]$p[1]; $x2 = [int]$p[2]; $y2 = [int]$p[3]
                foreach ($q in @(@($x2, $y2), @($x1, $y1))) {
                    if ([A]::Pid([A]::RootAt($q[0], $q[1])) -ne (WsPid)) { throw "drag at $($q[0]),$($q[1]) would land on another program" }
                }
                [A]::SetCursorPos($x1, $y1) | Out-Null; Start-Sleep -Milliseconds 120
                [A]::Button($false)
                for ($k = 1; $k -le 12; $k++) {
                    [A]::SetCursorPos([int]($x1 + ($x2 - $x1) * $k / 12), [int]($y1 + ($y2 - $y1) * $k / 12)) | Out-Null
                    Start-Sleep -Milliseconds 20
                }
                [A]::Button($true)
            }
            'dragpin' { # <x1> <y1> <x2> <y2>: a pin moved: down on a window of WinShun's, up anywhere (the pin goes with it)
                $p = $arg -split '\s+'; $x1 = [int]$p[0]; $y1 = [int]$p[1]; $x2 = [int]$p[2]; $y2 = [int]$p[3]
                if ([A]::Pid([A]::RootAt($x1, $y1)) -ne (WsPid)) { throw "dragpin at $x1,$y1 is not on a window of WinShun's" }
                [A]::SetCursorPos($x1, $y1) | Out-Null; Start-Sleep -Milliseconds 150
                [A]::Button($false)
                try {
                    for ($k = 1; $k -le 16; $k++) {
                        [A]::SetCursorPos([int]($x1 + ($x2 - $x1) * $k / 16), [int]($y1 + ($y2 - $y1) * $k / 16)) | Out-Null
                        Start-Sleep -Milliseconds 25
                    }
                    Start-Sleep -Milliseconds 100
                } finally { [A]::Button($true) }
            }
            'dragfg' { # <dx1> <dy1> <dx2> <dy2>: physical px from the front window's frame (a shape drawn in the edit window)
                Guard
                $p = $arg -split '\s+'; $h = [A]::GetForegroundWindow(); $r = [A]::Frame($h)
                $x1 = $r.L + [int]$p[0]; $y1 = $r.T + [int]$p[1]; $x2 = $r.L + [int]$p[2]; $y2 = $r.T + [int]$p[3]
                foreach ($q in @(@($x2, $y2), @($x1, $y1))) {
                    if ([A]::RootAt($q[0], $q[1]) -ne $h) { throw "drag at $($q[0]),$($q[1]) would land on another window" }
                }
                [A]::SetCursorPos($x1, $y1) | Out-Null; Start-Sleep -Milliseconds 150
                [A]::Button($false)
                try {
                    for ($k = 1; $k -le 12; $k++) {
                        [A]::SetCursorPos([int]($x1 + ($x2 - $x1) * $k / 12), [int]($y1 + ($y2 - $y1) * $k / 12)) | Out-Null
                        Start-Sleep -Milliseconds 25
                    }
                    Start-Sleep -Milliseconds 80
                } finally { [A]::Button($true) }
            }
            'clickbox' { # absolute physical px, on WinShun's search box in the taskbar
                $p = $arg -split '\s+'; $x = [int]$p[0]; $y = [int]$p[1]
                [A]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 120
                if ([A]::ClassAt($x, $y) -ne 'WinShun.TaskbarSearchBox') { throw "click at $x,$y is not on the taskbar box" }
                [A]::Click()
            }
            'wheel' {
                Guard
                $p = $arg -split '\s+'; $h = [A]::GetForegroundWindow(); $r = [A]::Frame($h)
                $x = $r.L + [int]$p[0]; $y = $r.T + [int]$p[1]
                [A]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 120
                if ([A]::RootAt($x, $y) -ne $h) { throw "wheel at $x,$y would land on another window" }
                [A]::Wheel([int]$p[2])
            }
            'type' { Guard; [A]::Type($arg) }
            'shot' {
                $r = [A]::Frame([A]::GetForegroundWindow())
                $bmp = New-Object Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
                $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $g.Dispose()
                Save $bmp $arg
            }
            'shotabs' { # name x y w h
                $p = $arg -split '\s+'
                $bmp = New-Object Drawing.Bitmap ([int]$p[3]), ([int]$p[4])
                $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen([int]$p[1], [int]$p[2], 0, 0, $bmp.Size); $g.Dispose()
                Save $bmp $p[0]
            }
            'matte' { # <name> [winshun|beside|hold] [with <window class>]
                $m = $arg -split '\s+'
                $out = Join-Path $shots "$($m[0]).png"
                $extra = @()
                if ($m -contains 'with') { $extra += @('--with', $m[[array]::IndexOf($m, 'with') + 1]) }
                if ($m.Count -gt 1 -and $m[1] -eq 'winshun') { Shots shoot -o $out --winshun @extra }
                elseif ($m.Count -gt 1 -and $m[1] -eq 'beside') { Guard; Shots shoot -o $out --beside @extra }
                elseif ($m.Count -gt 1 -and $m[1] -eq 'hold') { Guard; Shots shoot -o $out --hold @extra }
                else { Guard; Shots shoot -o $out @extra }
            }
            'scene' { Shots scene @($arg -split '\s+') }
            'restore' { Shots restore }
            'quit' { QuitWs } # WinShun off (when it wasn't running before)
            'profile' { # start [--unelevated] [shots.py profile options]: WinShun on work\profile, with a fresh clipboard history; stop: the user's own again
                QuitWs
                $more = @($arg -split '\s+' | Select-Object -Skip 1 | Where-Object { $_ })
                $arg = ($arg -split '\s+')[0]
                if ($arg -eq 'start') {
                    $un = -not ($more -contains '--elevated') # WinShun as the user runs it now: not elevated (--elevated: as before 0.8.0)
                    $script:unelevatedRun = $un
                    $more = @($more | Where-Object { $_ -ne '--unelevated' -and $_ -ne '--elevated' })
                    # made again each time: the demo dates counted from now, the clipboard history filled in
                    Shots profile $profileDir @more
                    # the recent folders of the profile's history only, not Windows' (this user's); the shell can't hand an environment over
                    $env:WINSHUN_RECENT = 'own'
                    StartWs @('--profile', "`"$profileDir`"") $exe $un
                } elseif ($arg -eq 'stop') { Remove-Item Env:WINSHUN_RECENT -ErrorAction SilentlyContinue; StartWs @() $userExe $true }
                else { throw "profile what? '$arg'" }
            }
            'notepad' { # Notepad on a file of this folder (made if missing), which then counts as "in front"
                $f = Local $arg
                if (-not (Test-Path $f)) { New-Item -ItemType File $f | Out-Null }
                Start-Process -FilePath 'notepad.exe' -ArgumentList "`"$f`""
                $script:allowed += 'Notepad'; WaitFront; Start-Sleep -Milliseconds 1200
            }
            'paint' { # Paint on a picture of this folder, which then counts as "in front"
                Start-Process -FilePath 'mspaint.exe' -ArgumentList "`"$(Local $arg)`""
                $script:allowed += 'mspaint'; WaitFront; Start-Sleep -Milliseconds 2500
            }
            'explorer' { # open <folder of the demo> | close: the Explorer windows on the demo folder
                $p = $arg -split '\s+', 2
                if ($p[0] -eq 'open') {
                    if (-not $p[1].StartsWith($demo)) { throw 'explorer only on the demo folder' }
                    # an elevated Explorer window is not seen by a WinShun that isn't (UIPI): then the shell opens it
                    if ($script:unelevatedRun) { RunUnelevated 'explorer.exe' "`"$($p[1])`"" } else { Start-Process -FilePath 'explorer.exe' -ArgumentList "`"$($p[1])`"" }
                    Start-Sleep -Milliseconds 2000
                } elseif ($p[0] -eq 'front') { # the demo's Explorer window to the front (by its handle, not by a title another window may share)
                    $h = ExplorerOfDemo
                    if ($h -eq [IntPtr]::Zero) { throw 'explorer front: no Explorer window on the demo folder' }
                    [A]::Front($h); [A]::Raise($h); Start-Sleep -Milliseconds 600; Guard
                } elseif ($p[0] -eq 'place') { # <x> <y> <w> <h>, physical px: the demo's Explorer windows, not maximized
                    $q = $p[1] -split '\s+'
                    foreach ($w in @((New-Object -ComObject Shell.Application).Windows())) {
                        try { $path = $w.Document.Folder.Self.Path } catch { continue }
                        if (-not $path.StartsWith($demo)) { continue }
                        $h = [IntPtr]$w.HWND
                        [A]::ShowWindow($h, 9) | Out-Null # SW_RESTORE
                        [A]::MoveWindow($h, [int]$q[0], [int]$q[1], [int]$q[2], [int]$q[3], $true) | Out-Null
                    }
                    Start-Sleep -Milliseconds 600
                } else {
                    foreach ($w in @((New-Object -ComObject Shell.Application).Windows())) {
                        try { if ($w.Document.Folder.Self.Path.StartsWith($demo)) { $w.Quit() } } catch {}
                    }
                }
            }
            'restart' { # WinShun again on the same profile (not made afresh), as it was started: a hook that Windows dropped, or one that was not up
                QuitWs
                if ($script:unelevatedRun) { $env:WINSHUN_RECENT = 'own' }
                StartWs @('--profile', "`"$profileDir`"") $exe $script:unelevatedRun
            }
            'unelevated' { $script:unelevatedRun = ($arg -eq 'on') } # programs the agent opens (Explorer) not elevated, as WinShun's own is
            'calc' { # start [<x> <y> <w> <h>] | end: a Calculator window to point at (the one that is open, put where asked and back again afterwards; else one opened here and closed again)
                if ($arg -like 'start*') {
                    $q = @($arg -split '\s+' | Select-Object -Skip 1)
                    $script:allowed += 'ApplicationFrameHost'
                    $h = [A]::FindTitle('Calculator'); if ($h -eq [IntPtr]::Zero) { $h = [A]::FindTitle([string][char]0x8BA1 + [char]0x7B97 + [char]0x5668) }
                    $script:calcStarted = ($h -eq [IntPtr]::Zero)
                    $script:calcWas = $null
                    if ($script:calcStarted) {
                        Start-Process -FilePath 'calc.exe'
                        $sw = [Diagnostics.Stopwatch]::StartNew()
                        while ($h -eq [IntPtr]::Zero -and $sw.Elapsed.TotalSeconds -lt 10) { Start-Sleep -Milliseconds 300; $h = [A]::FindTitle('Calculator') }
                        if ($h -eq [IntPtr]::Zero) { throw 'calc: no Calculator window' }
                    } else { $script:calcWas = [A]::WinRect($h) }
                    if ($q.Count -ge 4) { [A]::ShowWindow($h, 9) | Out-Null; [A]::MoveWindow($h, [int]$q[0], [int]$q[1], [int]$q[2], [int]$q[3], $true) | Out-Null; Start-Sleep -Milliseconds 600 }
                    [A]::Front($h); [A]::Raise($h); Start-Sleep -Milliseconds 600; Guard
                } else {
                    $h = [A]::FindTitle('Calculator')
                    if ($h -ne [IntPtr]::Zero) {
                        if ($script:calcStarted) { [A]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
                        elseif ($script:calcWas) { [A]::MoveWindow($h, $script:calcWas[0], $script:calcWas[1], $script:calcWas[2], $script:calcWas[3], $true) | Out-Null }
                    }
                    $script:calcStarted = $false; $script:calcWas = $null; Start-Sleep -Milliseconds 700
                }
            }
            'minwin' { # <title part>: a window of the user's that stands in the way, minimized (restored by `restorewins`)
                $h = [A]::FindTitle($arg)
                if ($h -ne [IntPtr]::Zero) {
                    if (-not $script:minimized) { $script:minimized = @() }
                    $script:minimized += $h; [A]::ShowWindow($h, 6) | Out-Null; Start-Sleep -Milliseconds 500
                }
            }
            # restorewins: Where-Object, as with none minimized @($null) would be one null handle
            'restorewins' { foreach ($h in @($script:minimized | Where-Object { $_ })) { [A]::ShowWindow($h, 9) | Out-Null }; $script:minimized = @(); Start-Sleep -Milliseconds 500 }
            'winstate' { # save|restore <title part>: where a window was, and maximized or not, put back afterwards (a program remembers the window it closes with)
                $q = $arg -split '\s+', 2
                $h = [A]::FindTitle($q[1])
                if ($q[0] -eq 'save') {
                    if ($h -eq [IntPtr]::Zero) { throw "winstate: no window '$($q[1])'" }
                    if (-not $script:winStates) { $script:winStates = @{} }
                    $script:winStates[$q[1]] = @{ rect = [A]::WinRect($h); zoomed = [A]::IsZoomed($h) }
                } elseif ($script:winStates -and $script:winStates.ContainsKey($q[1]) -and $h -ne [IntPtr]::Zero) {
                    $st = $script:winStates[$q[1]]
                    [A]::ShowWindow($h, 9) | Out-Null
                    [A]::MoveWindow($h, $st.rect[0], $st.rect[1], $st.rect[2], $st.rect[3], $true) | Out-Null
                    if ($st.zoomed) { [A]::ShowWindow($h, 3) | Out-Null }
                    Start-Sleep -Milliseconds 800
                }
            }
            'controlpanel' { # the Control Panel (a window of Explorer's, Chinese, with controls UI Automation knows), not elevated
                $script:allowed += 'explorer'
                if ($script:unelevatedRun) { RunUnelevated (Join-Path $env:windir 'System32\control.exe') '' } else { Start-Process -FilePath 'control.exe' }
                Start-Sleep -Milliseconds 3500
            }
            'cover' { # on [paper] | off: the wallpaper in a window over the whole screen, under whatever is shown after
                $p = $arg -split '\s+'
                $stop = Join-Path $work 'cover.stop'
                if ($p[0] -eq 'on') {
                    Remove-Item $stop -ErrorAction SilentlyContinue
                    $pythonw = Join-Path (Split-Path $python) 'pythonw.exe'
                    if (-not (Test-Path $pythonw)) { throw "no pythonw next to $python" }
                    $paper = if ($p.Count -gt 1 -and $p[1]) { $p[1] } else { 'dawn' }
                    $script:coverProc = Start-Process -FilePath $pythonw -ArgumentList "`"$tool`"", 'cover-window', '--bg', $paper, '--stop', "`"$stop`"" -PassThru
                    $sw = [Diagnostics.Stopwatch]::StartNew()
                    while ([A]::FindTitle('WinShun shot cover') -eq [IntPtr]::Zero -and $sw.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 200 }
                    if ([A]::FindTitle('WinShun shot cover') -eq [IntPtr]::Zero) { throw 'the cover did not come up' }
                } elseif ($p[0] -eq 'raise') { [A]::Raise([A]::FindTitle('WinShun shot cover')) # over the user's windows that came up again
                } else { CoverOff }
            }
            'edge' { # <file of scenes\>: Edge as an app window on that page (a profile of its own: nothing of this user's shows)
                $f = Local $arg
                $edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
                if (-not $edge) { throw 'no Edge' }
                $uri = ([Uri]$f).AbsoluteUri
                Start-Process -FilePath $edge -ArgumentList "--app=$uri", "--user-data-dir=`"$(Join-Path $work 'edge-profile')`"", '--guest', '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--disable-sync'
                $script:allowed += 'msedge'
                Start-Sleep -Milliseconds 3500
            }
            'closewin' { # <title part>: WM_CLOSE to a window of an allowed program
                $h = [A]::FindTitle($arg)
                if ($h -ne [IntPtr]::Zero) {
                    $name = try { (Get-Process -Id ([int][A]::Pid($h))).ProcessName } catch { '' }
                    if ($allowed -contains $name) { [A]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
                    else { throw "closewin: '$arg' is a window of $name, not one of ours" }
                }
                Start-Sleep -Milliseconds 700
            }
            'place' { # <title part> <x> <y> <w> <h>, physical px
                $p = $arg -split '\s+'
                $n = $p.Count
                $title = ($p[0..($n - 5)]) -join ' '
                $h = [A]::FindTitle($title)
                if ($h -eq [IntPtr]::Zero) { throw "place: no window '$title'" }
                [A]::ShowWindow($h, 9) | Out-Null
                [A]::MoveWindow($h, [int]$p[$n - 4], [int]$p[$n - 3], [int]$p[$n - 2], [int]$p[$n - 1], $true) | Out-Null
                Start-Sleep -Milliseconds 800
            }
            'front' { # <title part>
                $h = [A]::FindTitle($arg)
                if ($h -eq [IntPtr]::Zero) { throw "front: no window '$arg'" }
                [A]::Front($h); [A]::Raise($h); Start-Sleep -Milliseconds 500; Guard
            }
            'hover' { # <x> <y>: the pointer there, over a window of an allowed program or of WinShun's
                $p = $arg -split '\s+'; $x = [int]$p[0]; $y = [int]$p[1]
                [A]::MoveAbs($x, $y); Start-Sleep -Milliseconds 150
                $pid1 = [A]::Pid([A]::RootAt($x, $y))
                $name = try { (Get-Process -Id ([int]$pid1)).ProcessName } catch { '' }
                $mine = @(Get-Process WinShun -ErrorAction SilentlyContinue | ForEach-Object Id) -contains $pid1
                if (-not ($mine -or $allowed -contains $name)) { throw "hover at $x,$y is over $name" }
                Start-Sleep -Milliseconds 250
            }
            'hoverwin' { # <title part> <dx> <dy>: the pointer on that window's frame + (dx, dy), over a window of an allowed program or of WinShun's
                $p = $arg -split '\s+'; $n = $p.Count
                $h = [A]::FindTitle((($p[0..($n - 3)]) -join ' '))
                if ($h -eq [IntPtr]::Zero) { throw "hoverwin: no window '$arg'" }
                $f = [A]::Frame($h)
                $x = $f.L + [int]$p[$n - 2]; $y = $f.T + [int]$p[$n - 1]
                [A]::MoveAbs($x, $y); Start-Sleep -Milliseconds 150
                $pid1 = [A]::Pid([A]::RootAt($x, $y))
                $name = try { (Get-Process -Id ([int]$pid1)).ProcessName } catch { '' }
                $mine = @(Get-Process WinShun -ErrorAction SilentlyContinue | ForEach-Object Id) -contains $pid1
                if (-not ($mine -or $allowed -contains $name)) { throw "hoverwin at $x,$y is over $name" }
                Start-Sleep -Milliseconds 250
            }
            'pie' { # start <item> <dx> <dy> | hover <sector> <of> <left|right> | end
                $p = $arg -split '\s+', 2
                if ($p[0] -eq 'start') { PieStart $p[1] } elseif ($p[0] -eq 'hover') { PieHover $p[1] } elseif ($p[0] -eq 'end') { PieEnd } else { throw "pie what? '$arg'" }
            }
            'settingsapp' { # <page>: Windows' Settings on ms-settings:<page> (the shell opens it)
                if ($arg -notmatch '^[a-z0-9-]+$') { throw "settingsapp: odd page '$arg'" }
                Start-Process -FilePath 'explorer.exe' -ArgumentList "ms-settings:$arg"
                $script:allowed += 'ApplicationFrameHost'; $script:allowed += 'SystemSettings'
                Start-Sleep -Milliseconds 3500
            }
            'canceldialogs' { # Notepad's open dialogs (from Ctrl+O) get Cancel
                $h = [IntPtr]::Zero; $n = 0
                while (($h = [A]::FindWindowExW([IntPtr]::Zero, $h, '#32770', [NullString]::Value)) -ne [IntPtr]::Zero) {
                    if (-not [A]::IsWindowVisible($h)) { continue }
                    try { $name = (Get-Process -Id ([int][A]::Pid($h))).ProcessName } catch { continue }
                    if ($name -eq 'Notepad') { [A]::PostMessageW($h, 0x0111, [IntPtr]2, [IntPtr]::Zero) | Out-Null; $n++ }
                }
                Note "cancelled $n dialogs"; Start-Sleep -Milliseconds 500
            }
            'clip' { Set-Clipboard -Value $arg; Start-Sleep -Milliseconds 400 }
            'clipfile' { Set-Clipboard -Path $arg; Start-Sleep -Milliseconds 400 }
            'sleep' { Start-Sleep -Milliseconds ([int]$arg) }
            'exit' { Note 'exit'; exit }
            default { throw "unknown: $line" }
        }
        Note "ok: $line"
    }
}

function Batch($lines) {
    try { RunLines $lines }
    catch {
        try { PieEnd } catch { Note "pie cleanup: $_" }
        try { CoverOff } catch { Note "cover cleanup: $_" }
        foreach ($k in 0x10, 0x11, 0x12, 0x5B) { [A]::Key([uint16]$k, $true) } # nothing left held
        Note "BLOCKED: $_"
        return $false
    }
    Note 'batch done'
    return $true
}

if (-not (Test-Path $exe)) { throw "no $exe; build and deploy first" }
Note "agent up, WinShun pid $(WsPid)"
if ($Scene) {
    if (-not (Batch @("run $Scene"))) { Write-Host "stopped, see $log"; exit 1 }
    exit
}
$idle = [Diagnostics.Stopwatch]::StartNew()
while ($idle.Elapsed.TotalMinutes -lt 15) {
    if (-not (Test-Path $cmdFile)) { Start-Sleep -Milliseconds 200; continue }
    $lines = Get-Content $cmdFile -Encoding UTF8
    [IO.File]::Delete($cmdFile)
    $idle.Restart()
    Batch $lines | Out-Null
}
Note 'idle: exit'

param([string]$root, [string]$case = 'jump', [int]$hotkeyPid = 0, [string]$clickKind = 'double', [int]$settle = 700, [switch]$fast)
# One case of: an Open dialog of another process, (jump) taken to the folder
# of a test Explorer window by WinShun's Ctrl+G (posted as WM_HOTKEY 2 to its
# message window), then a file in the list double-clicked for real.
# Output: lines "key=value" on stdout.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class W {
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
    [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO i);
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint thread, ref GUITHREADINFO info);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder b, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder b, int n);
    [DllImport("kernel32.dll")] public static extern uint GetTickCount();
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
        public int cbSize; public int flags; public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; public RECT rcCaret; }
    public static string Cls(IntPtr h) { var b = new StringBuilder(128); GetClassNameW(h, b, 128); return b.ToString(); }
    public static string Txt(IntPtr h) { var b = new StringBuilder(256); GetWindowTextW(h, b, 256); return b.ToString(); }
    public static string State(IntPtr dialog) {
        uint pid; uint t = GetWindowThreadProcessId(dialog, out pid);
        var g = new GUITHREADINFO(); g.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
        if (!GetGUIThreadInfo(t, ref g)) return "?";
        return "focus=" + Cls(g.hwndFocus) + " capture=" + (g.hwndCapture == IntPtr.Zero ? "none" : Cls(g.hwndCapture)) + " fg=" + Cls(GetForegroundWindow());
    }
}
'@
[void][W]::SetProcessDpiAwarenessContext([IntPtr](-4))
function Out-Fact([string]$k, $v) { Write-Output ("{0}={1}" -f $k, $v) }
function Wait-Quiet {
    for ($i = 0; $i -lt 300; $i++) {
        $l = New-Object W+LASTINPUTINFO; $l.cbSize = 8; [void][W]::GetLastInputInfo([ref]$l)
        if ([W]::GetTickCount() - $l.dwTime -ge 2500) { return }
        Start-Sleep -Milliseconds 200
    }
}
$start = Join-Path $root 'start'; $target = Join-Path $root 'target'
$result = Join-Path $root ("result-" + $case + ".txt")
if (Test-Path $result) { [IO.File]::Delete($result) }
$dialogProc = Start-Process powershell.exe -PassThru -WindowStyle Hidden -ArgumentList '-NoProfile', '-STA', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'dialog.ps1'), $start, $result
$dialog = [IntPtr]::Zero
for ($i = 0; $i -lt 100 -and $dialog -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $dialog = [W]::FindWindowW('#32770', 'wsctrlg test dialog') }
Out-Fact 'dialog' $dialog
if ($dialog -eq [IntPtr]::Zero) { Out-Fact 'error' 'no dialog'; exit 2 }
try {
    Start-Sleep -Milliseconds 2500   # WinShun sees it, an auto jump (if on) happens
    $root1 = [System.Windows.Automation.AutomationElement]::FromHandle($dialog)
    function Find-Item([string]$prefix) {
        $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
        try {
            foreach ($e in $root1.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) { if ($e.Current.Name -like ($prefix + '*')) { return $e } }
        } catch { }   # the list is being rebuilt
        return $null
    }
    Out-Fact 'auto_jumped' ([bool](Find-Item 'wsjumpfile-target'))
    Out-Fact 'state_opened' ([W]::State($dialog))
    if ($case -eq 'jump' -and -not (Find-Item 'wsjumpfile-target')) {
        $msg = [IntPtr]::Zero; $found = [IntPtr]::Zero
        foreach ($parent in [IntPtr]::Zero, [IntPtr](-3)) {
            $msg = [IntPtr]::Zero
            while (($msg = [W]::FindWindowExW($parent, $msg, 'WinShun.MessageWindow', [NullString]::Value)) -ne [IntPtr]::Zero) {
                $p = 0; [void][W]::GetWindowThreadProcessId($msg, [ref]$p)
                Out-Fact 'message_window_candidate' ("$msg pid $p parent $parent")
                if ($found -eq [IntPtr]::Zero -and ($hotkeyPid -eq 0 -or $p -eq $hotkeyPid)) { $found = $msg }
            }
        }
        Out-Fact 'message_window' $found
        if ([W]::GetForegroundWindow() -ne $dialog) { Out-Fact 'error' ('dialog not in front before hotkey: ' + [W]::Cls([W]::GetForegroundWindow())); exit 3 }
        [void][W]::PostMessageW($found, 0x312, [IntPtr]2, [IntPtr]0x470002)
        for ($i = 0; $i -lt 40 -and -not (Find-Item 'wsjumpfile-target'); $i++) { Start-Sleep -Milliseconds 100 }
        Out-Fact 'jumped' ([bool](Find-Item 'wsjumpfile-target'))
        Start-Sleep -Milliseconds $settle
    }
    $prefix = if ($case -eq 'control') { 'wsjumpfile-start' } else { 'wsjumpfile-target' }
    $item = Find-Item $prefix
    if (-not $item) { Out-Fact 'error' "no list item $prefix"; exit 4 }
    Out-Fact 'state_before_click' ([W]::State($dialog))
    $attempts = 0
    for ($n = 1; $n -le 4 -and -not (Test-Path $result); $n++) {
        $item = Find-Item $prefix
        if (-not $item) { break }
        $r = $item.Current.BoundingRectangle
        $x = [int]($r.X + [Math]::Min(60, $r.Width / 2)); $y = [int]($r.Y + $r.Height / 2)
        if (-not $fast -or $n -gt 1) { Wait-Quiet }
        if ([W]::GetForegroundWindow() -ne $dialog) { Out-Fact 'error' ('dialog not in front before click: ' + [W]::Cls([W]::GetForegroundWindow()) + ' ' + [W]::Txt([W]::GetForegroundWindow())); break }
        $was = New-Object W+POINT; [void][W]::GetCursorPos([ref]$was)
        [void][W]::SetCursorPos($x, $y); Start-Sleep -Milliseconds 150
        $pt = New-Object W+POINT; $pt.X = $x; $pt.Y = $y
        if ([W]::GetAncestor([W]::WindowFromPoint($pt), 2) -ne $dialog) { Out-Fact 'error' 'list item not under the pointer'; [void][W]::SetCursorPos($was.X, $was.Y); break }
        $attempts = $n
        try {
            [W]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [W]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
            if ($clickKind -eq 'double') {
                Start-Sleep -Milliseconds 90
                [W]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [W]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
            }
        } finally { if ([W]::GetAsyncKeyState(1) -band 0x8000) { [W]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero) } }
        Start-Sleep -Milliseconds 300
        Out-Fact "state_after_click_$n" ([W]::State($dialog))
        $sel = $null
        try { $sel = (Find-Item $prefix).GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected } catch {}
        Out-Fact "selected_after_click_$n" $sel
        [void][W]::SetCursorPos($was.X, $was.Y)
        for ($i = 0; $i -lt 12 -and -not (Test-Path $result); $i++) { Start-Sleep -Milliseconds 100 }
        if ($clickKind -ne 'double') { break }
    }
    Out-Fact 'attempts' $attempts
    Out-Fact 'opened' (Test-Path $result)
    if (Test-Path $result) { Out-Fact 'result' ([IO.Path]::GetFileName([IO.File]::ReadAllText($result))) }
} finally {
    if (-not $dialogProc.HasExited) { try { $dialogProc.Kill() } catch {} }
}

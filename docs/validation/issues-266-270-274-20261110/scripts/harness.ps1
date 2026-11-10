param(
    [string] $Work = 'F:\电脑便捷工具\build\issues-266-270-274\i270',
    [switch] $ProbeOnly,
    [switch] $Calibrate
)
# #270 live test, run ELEVATED (as an all-users uninstaller runs). Real Explorer
# restarts, real Win+V by SendInput, real uninstaller made of the real .iss code.
$ErrorActionPreference = 'Stop'
$Stage = Join-Path $Work 'stage'
$Tiny = Join-Path $Work 'tiny\ws270-tiny-setup.exe'
$Folder = Join-Path $Work 'work\ws270-folder'
$Log = Join-Path $Work 'harness.log'
$Shots = Join-Path $Work 'shots'
New-Item -ItemType Directory -Force $Shots | Out-Null
Set-Content $Log "harness $(Get-Date -Format o)" -Encoding utf8
function L([string] $s) { $line = "$(Get-Date -Format 'HH:mm:ss.fff') $s"; Add-Content $Log $line -Encoding utf8 }

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, IntPtr title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int size);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint a, bool i, uint pid);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [DllImport("advapi32.dll")] public static extern bool OpenProcessToken(IntPtr p, uint a, out IntPtr t);
    [DllImport("advapi32.dll")] public static extern bool GetTokenInformation(IntPtr t, int c, out int v, int n, out int r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] struct KI { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct IN { public uint type; public KI ki; public long pad; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, IN[] i, int size);
    static IN K(ushort vk, ushort scan, bool up, bool ext) {
        var i = new IN(); i.type = 1; i.ki.vk = vk; i.ki.scan = scan;
        i.ki.flags = (up ? 2u : 0u) | (ext ? 1u : 0u); return i;
    }
    public static uint WinV() {
        var a = new IN[] { K(0x5B, 0x5B, false, true), K(0x56, 0x2F, false, false), K(0x56, 0x2F, true, false), K(0x5B, 0x5B, true, true) };
        return SendInput((uint)a.Length, a, Marshal.SizeOf(typeof(IN)));
    }
    public static uint Esc() {
        var a = new IN[] { K(0x1B, 0x01, false, false), K(0x1B, 0x01, true, false) };
        return SendInput((uint)a.Length, a, Marshal.SizeOf(typeof(IN)));
    }
    public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassNameW(h, s, 256); return s.ToString(); }
    public static string Title(IntPtr h) { var s = new StringBuilder(512); GetWindowTextW(h, s, 512); return s.ToString(); }
    public static uint Pid(IntPtr h) { uint p; GetWindowThreadProcessId(h, out p); return p; }
    public static bool Front(IntPtr target) {
        IntPtr fg = GetForegroundWindow();
        if (fg == target) return true;
        uint p; uint other = GetWindowThreadProcessId(fg, out p), me = GetCurrentThreadId();
        bool att = other != 0 && other != me && AttachThreadInput(me, other, true);
        ShowWindow(target, 9); BringWindowToTop(target); SetForegroundWindow(target);
        if (att) AttachThreadInput(me, other, false);
        return GetForegroundWindow() == target;
    }
    // Visible, not cloaked, non-empty top-level windows of the given processes.
    public static List<string> Shown(HashSet<uint> pids) {
        var r = new List<string>();
        EnumWindows((h, p) => {
            if (!pids.Contains(Pid(h)) || !IsWindowVisible(h)) return true;
            int cloaked; DwmGetWindowAttribute(h, 14, out cloaked, 4);
            RECT rc; GetWindowRect(h, out rc);
            if (cloaked == 0 && rc.R - rc.L > 20 && rc.B - rc.T > 20)
                r.Add(Cls(h) + " '" + Title(h) + "' " + (rc.R - rc.L) + "x" + (rc.B - rc.T));
            return true; }, IntPtr.Zero);
        return r;
    }
    public static List<string> ShownAll() {
        var r = new List<string>();
        EnumWindows((h, p) => {
            if (!IsWindowVisible(h)) return true;
            int cloaked; DwmGetWindowAttribute(h, 14, out cloaked, 4);
            RECT rc; GetWindowRect(h, out rc);
            if (cloaked == 0 && rc.R - rc.L > 20 && rc.B - rc.T > 20)
                r.Add(Pid(h) + "|" + Cls(h) + "|" + Title(h) + "|" + (rc.R - rc.L) + "x" + (rc.B - rc.T) + "@" + rc.L + "," + rc.T);
            return true; }, IntPtr.Zero);
        return r;
    }
    public static IntPtr ByTitle(string t) {
        IntPtr f = IntPtr.Zero;
        EnumWindows((h, p) => { if (IsWindowVisible(h) && Title(h) == t) { f = h; return false; } return true; }, IntPtr.Zero);
        return f;
    }
    public static List<IntPtr> Windows(string cls) {
        var r = new List<IntPtr>();
        EnumWindows((h, p) => { if (IsWindowVisible(h) && Cls(h) == cls) r.Add(h); return true; }, IntPtr.Zero);
        return r;
    }
    public static List<IntPtr> Dialogs(HashSet<uint> pids) {
        var r = new List<IntPtr>();
        EnumWindows((h, p) => { if (IsWindowVisible(h) && Cls(h) == "#32770" && pids.Contains(Pid(h))) r.Add(h); return true; }, IntPtr.Zero);
        return r;
    }
    public static string Text(IntPtr dlg) { IntPtr s = GetDlgItem(dlg, 0xFFFF); return s == IntPtr.Zero ? "(no static)" : Title(s); }
    public static bool Click(IntPtr dlg, int id) {
        IntPtr b = GetDlgItem(dlg, id);
        if (b == IntPtr.Zero) return false;
        return PostMessageW(dlg, 0x0111, (IntPtr)id, b); // WM_COMMAND, BN_CLICKED
    }
    // 1 elevated, 0 not, -1 unknown
    public static int Elevated(uint pid) {
        IntPtr p = OpenProcess(0x1000, false, pid); if (p == IntPtr.Zero) return -1;
        IntPtr t; int v = -1, n;
        if (OpenProcessToken(p, 8, out t)) { int e; if (GetTokenInformation(t, 20, out e, 4, out n)) v = e != 0 ? 1 : 0; CloseHandle(t); }
        CloseHandle(p); return v;
    }
}
'@
[void][W]::SetProcessDpiAwarenessContext([IntPtr](-4))
Add-Type -Path (Join-Path $PSScriptRoot 'Diff.cs') -ReferencedAssemblies System.Drawing, System.Drawing.Primitives, System.Drawing.Common, System.Collections, System.Runtime.InteropServices

$adv = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
function HasValue($path, $name) { (Test-Path $path) -and ((Get-Item $path).GetValueNames() -contains $name) }
$orig = @{
    Disabled = if (HasValue $adv 'DisabledHotkeys') { (Get-ItemProperty $adv).DisabledHotkeys } else { $null }
    Recorded = if (HasValue 'HKCU:\Software\WinShun' 'ExplorerHotkeys') { (Get-ItemProperty 'HKCU:\Software\WinShun').ExplorerHotkeys } else { $null }
}
L "original DisabledHotkeys=[$($orig.Disabled)] ExplorerHotkeys=[$($orig.Recorded)]"
$explorerSees = $orig.Disabled   # what the running Explorer read at its start
$results = [ordered]@{}

$guard = [W]::ByTitle('ws270 guard (medium)')
if ($guard -eq [IntPtr]::Zero) { L 'no guard window (guard.ps1 not running)'; return }
$guardPid = [W]::Pid($guard)
L "guard window pid $guardPid elevated=$([W]::Elevated($guardPid))"
function Pump([int] $ms) { $end = (Get-Date).AddMilliseconds($ms); while ((Get-Date) -lt $end) { [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 20 } }

function Shell {
    $h = [W]::GetShellWindow(); $sp = if ($h -ne [IntPtr]::Zero) { [W]::Pid($h) } else { 0 }
    $tray = [W]::FindWindowW('Shell_TrayWnd', [IntPtr]::Zero) -ne [IntPtr]::Zero
    [pscustomobject]@{ Pid = $sp; Elevated = if ($sp) { [W]::Elevated($sp) } else { -1 }; Tray = $tray }
}
function WaitShell([int] $oldPid, [int] $seconds = 40) {
    $end = (Get-Date).AddSeconds($seconds)
    do { Pump 250; $s = Shell } while ((Get-Date) -lt $end -and -not ($s.Tray -and $s.Pid -and $s.Pid -ne $oldPid))
    Pump 6000   # Explorer takes its hotkeys a moment after the taskbar is up
    return Shell
}
function InputHosts { $set = New-Object 'System.Collections.Generic.HashSet[uint32]'; Get-Process TextInputHost -ErrorAction SilentlyContinue | ForEach-Object { [void]$set.Add([uint32]$_.Id) }; return , $set }
function Grab {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size); $g.Dispose()
    return $bmp
}
function PName([uint32] $id) { try { (Get-Process -Id $id).ProcessName } catch { '?' } }
function ProbeWinV([string] $label) {
    Pump 400
    if (-not [W]::Front($guard)) { Pump 400; if (-not [W]::Front($guard)) { throw "guard window not in front before Win+V ($label): $([W]::Cls([W]::GetForegroundWindow()))" } }
    Pump 300
    # Windows' panel takes neither the foreground nor a window of its own that
    # EnumWindows shows (docs/pitfalls.md): judged by a panel-sized block of the
    # screen that changed (calibrated: panel 191-471 tiles, noise 0-4).
    $before = Grab
    $sent = [W]::WinV()
    Pump 2000
    $fg = [W]::GetForegroundWindow(); $fgName = PName ([W]::Pid($fg))
    $after = Grab
    $change = [Diff]::Largest($before, $after, 12, 0.6)
    $opened = [Diff]::TilesOf($change) -ge 100
    $after.Save((Join-Path $Shots "$label.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    L "  Win+V [$label] sent=$sent fg=$fgName '$([W]::Title($fg))' change=$change => opened=$opened"
    # Closed with Esc, which goes to the guard in front (harmless) and closes the panel.
    if ($fg -eq $guard) { [void][W]::Esc(); Pump 900 } else { L "  no Esc: '$fgName' is in front" }
    if ($opened) {
        $closed = Grab; $still = [Diff]::Largest($before, $closed, 12, 0.6); $closed.Dispose()
        L "  after Esc: $still$(if ([Diff]::TilesOf($still) -ge 100) { '  PANEL STILL UP' })"
    }
    $before.Dispose(); $after.Dispose()
    return $opened
}
function Restart([string] $why) {
    $old = (Shell).Pid
    $p = Start-Process (Join-Path $Stage 'WinShun.exe') -ArgumentList '--restart-explorer' -PassThru -WindowStyle Normal
    if (-not $p.WaitForExit(60000)) { throw 'WinShun.exe --restart-explorer did not end in 60 s' }
    $s = WaitShell $old
    L "  restart ($why): exit=$($p.ExitCode) shell $old -> $($s.Pid) elevated=$($s.Elevated) tray=$($s.Tray)"
    $script:explorerSees = (Get-ItemProperty $adv -ErrorAction SilentlyContinue).DisabledHotkeys
    return $s
}
function Take {
    Set-ItemProperty $adv -Name DisabledHotkeys -Value 'V' -Type String
    if (-not (Test-Path 'HKCU:\Software\WinShun')) { New-Item 'HKCU:\Software\WinShun' | Out-Null }
    Set-ItemProperty 'HKCU:\Software\WinShun' -Name ExplorerHotkeys -Value 'V' -Type String
    Restart 'take Win+V' | Out-Null
}
function OpenFolder {
    Start-Process explorer.exe -ArgumentList "`"$Folder`""
    $end = (Get-Date).AddSeconds(20)
    do { Pump 300; $w = @([W]::Windows('CabinetWClass') | Where-Object { [W]::Title($_) -like 'ws270-folder*' }) } while ((Get-Date) -lt $end -and $w.Count -eq 0)
    L "  folder window open: $($w.Count) '$(if ($w.Count) { [W]::Title($w[0]) })'"
    return $w.Count -gt 0
}
function FolderOpen { @([W]::Windows('CabinetWClass') | Where-Object { [W]::Title($_) -like 'ws270-folder*' }).Count -gt 0 }
function CloseFolder { [W]::Windows('CabinetWClass') | Where-Object { [W]::Title($_) -like 'ws270-folder*' } | ForEach-Object { [void][W]::PostMessageW($_, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }; Pump 1500 }
function Install {
    $p = Start-Process $Tiny -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -PassThru
    if (-not $p.WaitForExit(60000)) { throw 'tiny setup did not end' }
    if (-not (Test-Path (Join-Path $Stage 'unins000.exe'))) { throw "tiny setup exit $($p.ExitCode): no unins000.exe" }
}
# The uninstaller's processes: unins000.exe and the copy it runs from %TEMP%, started since $since.
function UninstallerPids([datetime] $since) {
    $pids = New-Object 'System.Collections.Generic.HashSet[uint32]'
    Get-Process | Where-Object { try { $_.StartTime -ge $since -and ($_.Path -like "$env:TEMP\*" -or $_.Path -like "$Stage\unins000*") } catch { $false } } |
        ForEach-Object { [void]$pids.Add([uint32]$_.Id) }
    return , $pids
}
function Uninstall([bool] $silent) {
    $since = (Get-Date).AddSeconds(-1)
    $unins = Join-Path $Stage 'unins000.exe'
    $first = if ($silent) { Start-Process $unins -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -PassThru } else { Start-Process $unins -PassThru }
    $seen = @{}; $texts = @()
    $end = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $end) {
        Pump 300
        $pids = UninstallerPids $since
        foreach ($d in [W]::Dialogs($pids)) {
            if ($seen.ContainsKey([string]$d)) { continue }
            $t = [W]::Text($d); $seen[[string]$d] = $true; $texts += $t
            $btn = if ([W]::GetDlgItem($d, 6) -ne [IntPtr]::Zero) { 6 } elseif ([W]::GetDlgItem($d, 1) -ne [IntPtr]::Zero) { 1 } else { 2 }
            Pump 700   # as a person would read it
            L "  dialog: '$($t -replace "`r?`n", ' / ')' -> button $btn"
            [void][W]::Click($d, $btn)
        }
        if ($pids.Count -eq 0 -and $first.HasExited -and -not (Test-Path (Join-Path $Stage 'unins000.exe'))) { break }
    }
    if (Test-Path (Join-Path $Stage 'unins000.exe')) { throw 'uninstall did not finish in 120 s' }
    return , $texts
}
function Keys { "DisabledHotkeys=" + $(if (HasValue $adv 'DisabledHotkeys') { "[" + (Get-ItemProperty $adv).DisabledHotkeys + "]" } else { '(none)' }) + " ExplorerHotkeys=" + $(if (HasValue 'HKCU:\Software\WinShun' 'ExplorerHotkeys') { "[" + (Get-ItemProperty 'HKCU:\Software\WinShun').ExplorerHotkeys + "]" } else { '(none)' }) }

if ($Calibrate) {
    L 'calibration: Win+V twice with nothing taken'
    ProbeWinV 'cal-1' | Out-Null
    ProbeWinV 'cal-2' | Out-Null
    return
}
if ($ProbeOnly) {
    try {
        Install
        $since = (Get-Date).AddSeconds(-1)
        $first = Start-Process (Join-Path $Stage 'unins000.exe') -PassThru
        $end = (Get-Date).AddSeconds(30); $d = @()
        do { Pump 300; $pids = UninstallerPids $since; $d = @([W]::Dialogs($pids)) } while ((Get-Date) -lt $end -and $d.Count -eq 0)
        Get-Process | Where-Object { $pids.Contains([uint32]$_.Id) } | ForEach-Object { L "  uninstaller process $($_.Id) $($_.Path)" }
        foreach ($h in $d) {
            $ids = (1, 2, 6, 7 | Where-Object { [W]::GetDlgItem($h, $_) -ne [IntPtr]::Zero }) -join ','
            L "  dialog class=$([W]::Cls($h)) title='$([W]::Title($h))' static='$([W]::Text($h))' buttons=$ids"
            Pump 500
            if (-not [W]::Click($h, 7)) { [void][W]::PostMessageW($h, 0x0466, [IntPtr]7, [IntPtr]::Zero); L '  (task dialog click)' }
        }
        if ($d.Count -eq 0) { L '  NO dialog seen' }
        Pump 3000
        L "  after No: unins000 still there: $(Test-Path (Join-Path $Stage 'unins000.exe'))"
    } finally {
        if (Test-Path (Join-Path $Stage 'unins000.exe')) { try { Uninstall $true | Out-Null; L 'tiny install removed' } catch { L "remove: $_" } }
        L "end: $(Keys); shell $((Shell).Pid)"
    }
    return
}
try {
    if ((Shell).Elevated -ne 0) { throw "Explorer is not a normal-rights process to begin with: $((Shell) | Out-String)" }
    L "start: shell $((Shell).Pid) elevated=$((Shell).Elevated); folder windows open: $([W]::Windows('CabinetWClass').Count)"

    L 'P0 baseline (Win+V is Windows'')'
    $results['P0 Win+V opens with nothing taken'] = ProbeWinV 'p0-baseline'

    L 'P1 taken (DisabledHotkeys=V, Explorer restarted)'
    Take
    $results['P1 Win+V dead while taken'] = -not (ProbeWinV 'p1-taken')

    L 'A: silent all-users uninstall, no folder window'
    $s0 = Shell; Install
    $texts = Uninstall $true
    $s1 = WaitShell $s0.Pid 20
    L "  $(Keys); shell $($s0.Pid) -> $($s1.Pid) elevated=$($s1.Elevated); dialogs=$($texts.Count)"
    $script:explorerSees = (Get-ItemProperty $adv -ErrorAction SilentlyContinue).DisabledHotkeys
    $results['A keys given back'] = -not (HasValue $adv 'DisabledHotkeys') -and -not (HasValue 'HKCU:\Software\WinShun' 'ExplorerHotkeys')
    $results['A Explorer restarted'] = $s1.Pid -ne $s0.Pid -and $s1.Tray
    $results['A Explorer normal rights'] = $s1.Elevated -eq 0
    $results['A Win+V works'] = ProbeWinV 'a-after'

    L 'B: interactive uninstall with a folder window open'
    Take
    $results['B folder window opened'] = OpenFolder
    $s0 = Shell; Install
    $texts = Uninstall $false
    $s1 = WaitShell $s0.Pid 20
    L "  $(Keys); shell $($s0.Pid) -> $($s1.Pid) elevated=$($s1.Elevated); folder window still open: $(FolderOpen)"
    $script:explorerSees = (Get-ItemProperty $adv -ErrorAction SilentlyContinue).DisabledHotkeys
    $results['B asked first (Win+V named)'] = @($texts | Where-Object { $_ -like '*Win+V*' -and $_ -like '*资源管理器*' }).Count -eq 1
    $results['B Explorer restarted after Yes'] = $s1.Pid -ne $s0.Pid -and $s1.Tray
    $results['B Explorer normal rights'] = $s1.Elevated -eq 0
    $results['B Win+V works'] = ProbeWinV 'b-after'

    L 'C: silent uninstall with a folder window open (left to the next sign-in)'
    Take
    $results['C folder window opened'] = OpenFolder
    $s0 = Shell; Install
    $texts = Uninstall $true
    Pump 4000; $s1 = Shell
    L "  $(Keys); shell $($s0.Pid) -> $($s1.Pid); folder window still open: $(FolderOpen); dialogs=$($texts.Count)"
    $results['C keys given back'] = -not (HasValue $adv 'DisabledHotkeys')
    $results['C Explorer left alone'] = $s1.Pid -eq $s0.Pid -and (FolderOpen)
    $results['C Win+V still dead until a restart (the #270 state)'] = -not (ProbeWinV 'c-norestart')
    CloseFolder
    Restart 'C: by hand, as the reply tells' | Out-Null
    $results['C Win+V works after the restart'] = ProbeWinV 'c-restarted'
} catch {
    L "ABORTED: $($_.Exception.Message) at $($_.InvocationInfo.ScriptLineNumber)"
    $results['run completed'] = $false
} finally {
    try { CloseFolder } catch { L "close folder: $_" }
    if (Test-Path (Join-Path $Stage 'unins000.exe')) {
        try { Uninstall $true | Out-Null; L 'leftover tiny install removed' } catch { L "leftover uninstall: $_" }
    }
    # Back to what it was, and Explorer started again if it read something else.
    if ($null -eq $orig.Disabled) { Remove-ItemProperty $adv -Name DisabledHotkeys -ErrorAction SilentlyContinue } else { Set-ItemProperty $adv -Name DisabledHotkeys -Value $orig.Disabled }
    if ($null -eq $orig.Recorded) { Remove-ItemProperty 'HKCU:\Software\WinShun' -Name ExplorerHotkeys -ErrorAction SilentlyContinue } else { Set-ItemProperty 'HKCU:\Software\WinShun' -Name ExplorerHotkeys -Value $orig.Recorded }
    if ("$explorerSees" -ne "$($orig.Disabled)") { try { Restart 'restore' | Out-Null } catch { L "restore restart: $_" } }
    L "end: $(Keys); shell $((Shell).Pid) elevated=$((Shell).Elevated) tray=$((Shell).Tray)"
    foreach ($k in $results.Keys) { L ("RESULT {0,-55} {1}" -f $k, $(if ($results[$k]) { 'PASS' } else { 'FAIL' })) }
}

# GitHub #299 repro: an elevated WinShun (as with the "run as administrator" flag), Start-menu typing and
# the taskbar box on. Runs elevated; started by run.ps1. Writes everything to $Out. Hard cap on every wait.
param([string] $Exe, [string] $Profile, [string] $Out, [string] $Scenario = 'mouse')
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$logFile = Join-Path $Out 'repro.log'
function Say([string] $text) { Add-Content -LiteralPath $logFile -Value ("{0:HH:mm:ss.fff} {1}" -f (Get-Date), $text) -Encoding UTF8 }
Add-Type -AssemblyName System.Windows.Forms, System.Drawing, UIAutomationClient, UIAutomationTypes
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public int type; public InputUnion u; }
  [StructLayout(LayoutKind.Explicit)] public struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy, data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public short vk, scan; public int flags, time; public IntPtr extra; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc p, IntPtr l);
  public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
  public static IntPtr FindByClass(string cls) {
    IntPtr found = IntPtr.Zero;
    EnumProc top = (h, l) => { if (Cls(h) == cls && IsWindowVisible(h)) { found = h; return false; }
      EnumChildWindows(h, (c, l2) => { if (Cls(c) == cls && IsWindowVisible(c)) { found = c; return false; } return true; }, IntPtr.Zero);
      return found == IntPtr.Zero; };
    EnumWindows(top, IntPtr.Zero); return found; }
  public static string Boxes() {
    var sb = new StringBuilder();
    EnumProc one = (h, l) => { string c = Cls(h); if (c.StartsWith("WinShun.Taskbar")) { RECT r; GetWindowRect(h, out r); sb.Append(c.Substring(8) + " 0x" + h.ToInt64().ToString("x") + " vis=" + IsWindowVisible(h) + " " + r.L + "," + r.T + "," + r.R + "," + r.B + "; "); } return true; };
    EnumWindows((h, l) => { one(h, l); EnumChildWindows(h, one, IntPtr.Zero); return true; }, IntPtr.Zero);
    return sb.Length == 0 ? "none" : sb.ToString(); }
  public static void Key(short vk, bool up) { var i = new INPUT[1]; i[0].type = 1; i[0].u.ki.vk = vk; i[0].u.ki.flags = up ? 2 : 0; SendInput(1, i, Marshal.SizeOf(typeof(INPUT))); }
  public static void Click() { var i = new INPUT[2]; i[0].type = 0; i[0].u.mi.flags = 2; i[1].type = 0; i[1].u.mi.flags = 4; SendInput(2, i, Marshal.SizeOf(typeof(INPUT))); }
}
'@
[W]::SetProcessDPIAware() | Out-Null
function Front {
    $h = [W]::GetForegroundWindow(); $procId = 0; [void][W]::GetWindowThreadProcessId($h, [ref] $procId)
    $name = try { (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { '?' }
    "front=0x{0:x} class={1} program={2}" -f $h.ToInt64(), [W]::Cls($h), $name
}
function Tap([int] $vk) { [W]::Key($vk, $false); Start-Sleep -Milliseconds 40; [W]::Key($vk, $true); Start-Sleep -Milliseconds 90 }
function Shot([string] $name) {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    # the lower part is enough: the taskbar and the launcher over it
    $crop = $bmp.Clone((New-Object System.Drawing.Rectangle 0, ([int]($b.Height * 0.25)), $b.Width, ([int]($b.Height * 0.75))), $bmp.PixelFormat)
    $crop.Save((Join-Path $Out "$name.png")); $g.Dispose(); $bmp.Dispose(); $crop.Dispose()
}
function ClickAt([int] $x, [int] $y) { [W]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 150; [W]::Click() }
function StartButton {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $cond = New-Object System.Windows.Automation.PropertyCondition ([System.Windows.Automation.AutomationElement]::AutomationIdProperty), 'StartButton'
    $e = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
    if (-not $e) { Say 'Start button not found by UI Automation: the place measured before is used'; return @(785, 1563) }
    $r = $e.Current.BoundingRectangle
    @([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
}
function BoxOnly([string] $tag) {
    $box = [IntPtr]::Zero; $w = 0
    while ($w -lt 30 -and ($box = [W]::FindByClass('WinShun.TaskbarSearchBox')) -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200; $w++ }
    if ($box -ne [IntPtr]::Zero) { $script:boxRect = New-Object W+RECT; [W]::GetWindowRect($box, [ref] $script:boxRect) | Out-Null }
    $r = $script:boxRect
    Say "windows: $([W]::Boxes())"
    Say ("== ${tag}: box only; found=$($box -ne [IntPtr]::Zero) rect {0},{1},{2},{3}; before: $(Front)" -f $r.L, $r.T, $r.R, $r.B)
    ClickAt ([int](($r.L + $r.R) / 2)) ([int](($r.T + $r.B) / 2))
    Start-Sleep -Milliseconds 1500
    Say "after click on box: $(Front)"
    Tap 0x45; Tap 0x52
    Start-Sleep -Milliseconds 2000
    Say "after typing er: $(Front)"
    Shot "$tag-box-typed"
    Tap 0x1B; Start-Sleep -Milliseconds 700
    if ([W]::Cls([W]::GetForegroundWindow()) -like 'Qt*') { Tap 0x1B; Start-Sleep -Milliseconds 700 }
    Say "end: $(Front)"
}
function Round([string] $tag, [bool] $winKey) {
    Say "== ${tag}: open Start by $(if ($winKey) { 'Win key' } else { 'mouse' }); before: $(Front)"
    if ($winKey) { Tap 0x5B } else { $p = StartButton; ClickAt $p[0] $p[1] }
    Start-Sleep -Milliseconds 1500
    Say "Start open: $(Front)"
    Tap 0x51; Tap 0x57   # q w
    Start-Sleep -Milliseconds 2000
    Say "after typing qw: $(Front)"
    Shot "$tag-1-start-typed"
    Tap 0x1B; Start-Sleep -Milliseconds 700
    Say "after Esc: $(Front)"
    if ([W]::Cls([W]::GetForegroundWindow()) -like 'Qt*') { Tap 0x1B; Start-Sleep -Milliseconds 700; Say "after 2nd Esc: $(Front)" }
    Shot "$tag-2-after-esc"
    $box = [IntPtr]::Zero; $w = 0
    while ($w -lt 30 -and ($box = [W]::FindByClass('WinShun.TaskbarSearchBox')) -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200; $w++ }
    Say "box back after $($w * 200) ms; windows: $([W]::Boxes())"
    if ($box -eq [IntPtr]::Zero) { Say 'taskbar box window not found now: its place at the start is used' } else { $script:boxRect = New-Object W+RECT; [W]::GetWindowRect($box, [ref] $script:boxRect) | Out-Null }
    $r = $script:boxRect
    if ($r.R -eq 0) { Say "end of round (no box): $(Front)"; return }
    Say ("box rect {0},{1},{2},{3}" -f $r.L, $r.T, $r.R, $r.B)
    ClickAt ([int](($r.L + $r.R) / 2)) ([int](($r.T + $r.B) / 2))
    Start-Sleep -Milliseconds 1500
    Say "after click on box: $(Front)"
    Tap 0x45; Tap 0x52   # e r
    Start-Sleep -Milliseconds 2000
    Say "after typing er: $(Front)"
    Shot "$tag-3-box-typed"
    Tap 0x1B; Start-Sleep -Milliseconds 700
    if ([W]::Cls([W]::GetForegroundWindow()) -like 'Qt*') { Tap 0x1B; Start-Sleep -Milliseconds 700 }
    Say "end of round: $(Front)"
}

$lock = 'F:\wswt\ui.lock'
$mine = $false
$app = $null
try {
    $fs = [IO.File]::Open($lock, [IO.FileMode]::CreateNew); $fs.Close(); $mine = $true
    if (Get-Process WinShun -ErrorAction SilentlyContinue) { throw 'a WinShun is running' }
    $env:WINSHUN_LOG_INFO = '1'
    $app = Start-Process -FilePath $Exe -ArgumentList '--profile', "`"$Profile`"", '--background' -WorkingDirectory (Split-Path $Exe) -WindowStyle Normal -PassThru
    Say "started WinShun pid $($app.Id)"
    $waited = 0
    while ($waited -lt 40 -and [W]::FindByClass('WinShun.TaskbarSearchBox') -eq [IntPtr]::Zero) { Start-Sleep 1; $waited++ }
    Say "taskbar box after $waited s: $([W]::FindByClass('WinShun.TaskbarSearchBox') -ne [IntPtr]::Zero)"
    Start-Sleep 4
    $script:boxRect = New-Object W+RECT; [W]::GetWindowRect([W]::FindByClass('WinShun.TaskbarSearchBox'), [ref] $script:boxRect) | Out-Null
    if ($script:boxRect.R -eq 0) { Say 'no taskbar box in this run: its place is not clicked' }
    Say ("box at the start {0},{1},{2},{3}" -f $script:boxRect.L, $script:boxRect.T, $script:boxRect.R, $script:boxRect.B)
    Shot '0-before'
    if ($Scenario -eq 'traystart') { foreach ($n in 1, 2) { ClickAt 300 1563; Start-Sleep -Milliseconds 800; Say "clicked the empty taskbar: $(Front)"; Round "tray$n" $false; Start-Sleep 3 } }
    if ($Scenario -eq 'compare') { BoxOnly 'control1'; Start-Sleep 3; BoxOnly 'control2'; Start-Sleep 3; Round 'mouse' $false; Start-Sleep 3; BoxOnly 'after' }
    if ($Scenario -eq 'mouse' -or $Scenario -eq 'both') { Round 'mouse' $false; Start-Sleep 2 }
    if ($Scenario -eq 'win' -or $Scenario -eq 'both') { Round 'win' $true; Start-Sleep 1; Round 'win2' $true }
}
catch { Say "FAILED: $_" }
finally {
    if ($app) {
        try { Start-Process -FilePath $Exe -ArgumentList '--profile', "`"$Profile`"", '--quit' -WorkingDirectory (Split-Path $Exe) | Out-Null } catch { }
        if (-not $app.WaitForExit(20000)) { Say 'quit timed out: ending the test copy'; try { $app.Kill() } catch { } }
    }
    Get-Process WinShun, WinShunPluginHost -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith((Split-Path $Exe)) } | ForEach-Object { try { $_.Kill() } catch { } }
    if ($mine) { [IO.File]::Delete($lock) }
    Say 'done'
    Set-Content -LiteralPath (Join-Path $Out 'done.flag') -Value 'done'
}

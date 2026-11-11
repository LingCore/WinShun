# Shared helpers for the #82 / #150 live runs (dot-source it). Physical pixels
# throughout: the process is made per-monitor DPI aware before anything else.
$ErrorActionPreference = 'Stop'
if (-not ('WsLive.N' -as [type])) {
Add-Type -Namespace WsLive -Name N -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr v);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
[DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
[DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr hwnd);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hwnd, System.Text.StringBuilder sb, int n);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hwnd, System.Text.StringBuilder sb, int n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
public delegate bool EnumProc(IntPtr hwnd, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc p, IntPtr l);
[DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
[DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
[DllImport("user32.dll")] public static extern IntPtr GetKeyboardLayout(uint thread);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hgt, bool repaint);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[DllImport("user32.dll", SetLastError = true)] public static extern bool RegisterHotKey(IntPtr h, int id, uint mods, uint vk);
[DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr h, int id);
[DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
[StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO { public int cbSize; public int flags; public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; public RECT rcCaret; }
[DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint thread, ref GUITHREADINFO info);
[StructLayout(LayoutKind.Explicit, Size = 40)] public struct INPUT {
    [FieldOffset(0)] public uint type;
    [FieldOffset(8)] public ushort wVk; [FieldOffset(10)] public ushort wScan; [FieldOffset(12)] public uint kFlags; [FieldOffset(16)] public uint kTime; [FieldOffset(24)] public IntPtr kExtra;
    [FieldOffset(8)] public int dx; [FieldOffset(12)] public int dy; [FieldOffset(16)] public uint mouseData; [FieldOffset(20)] public uint mFlags; [FieldOffset(24)] public uint mTime; [FieldOffset(32)] public IntPtr mExtra;
}
[DllImport("user32.dll", SetLastError = true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
public static uint Send(INPUT[] inputs) { return SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT))); }
public static INPUT KeyEvent(ushort vk, ushort scan, bool up) { return new INPUT { type = 1, wVk = vk, wScan = scan, kFlags = up ? 2u : 0u }; }
public static INPUT Mouse(uint flags) { return new INPUT { type = 0, mFlags = flags }; }
public static INPUT MoveTo(int x, int y) {
    // Absolute over the virtual desktop (MOUSEEVENTF_MOVE | ABSOLUTE | VIRTUALDESK).
    int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
    return new INPUT { type = 0, dx = (int)Math.Round((x - vx) * 65535.0 / (vw - 1)), dy = (int)Math.Round((y - vy) * 65535.0 / (vh - 1)), mFlags = 0x0001 | 0x8000 | 0x4000 };
}
'@
}
[void][WsLive.N]::SetProcessDpiAwarenessContext([IntPtr]-4)

function Rect-Of([IntPtr] $h) { $r = New-Object WsLive.N+RECT; [void][WsLive.N]::GetWindowRect($h, [ref] $r); $r }
function Frame-Of([IntPtr] $h) { $r = New-Object WsLive.N+RECT; [void][WsLive.N]::DwmGetWindowAttribute($h, 9, [ref] $r, 16); $r } # DWMWA_EXTENDED_FRAME_BOUNDS
function Client-Of([IntPtr] $h) {
    $r = New-Object WsLive.N+RECT; [void][WsLive.N]::GetClientRect($h, [ref] $r)
    $p = New-Object WsLive.N+POINT; [void][WsLive.N]::ClientToScreen($h, [ref] $p)
    $out = New-Object WsLive.N+RECT; $out.Left = $p.X; $out.Top = $p.Y; $out.Right = $p.X + $r.Right; $out.Bottom = $p.Y + $r.Bottom; $out
}
function Fmt($r) { "[$($r.Left),$($r.Top) $($r.Right - $r.Left)x$($r.Bottom - $r.Top)]" }
function Class-Of([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 256; [void][WsLive.N]::GetClassNameW($h, $sb, 256); $sb.ToString() }
function Title-Of([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 512; [void][WsLive.N]::GetWindowTextW($h, $sb, 512); $sb.ToString() }
function Pid-Of([IntPtr] $h) { [uint32] $p = 0; [void][WsLive.N]::GetWindowThreadProcessId($h, [ref] $p); $p }
function Thread-Of([IntPtr] $h) { [uint32] $p = 0; [WsLive.N]::GetWindowThreadProcessId($h, [ref] $p) }
function Fg { $h = [WsLive.N]::GetForegroundWindow(); [pscustomobject]@{ hwnd = $h; pid = (Pid-Of $h); cls = (Class-Of $h) } }
function Root-At([int] $x, [int] $y) { $p = New-Object WsLive.N+POINT; $p.X = $x; $p.Y = $y; [WsLive.N]::GetAncestor([WsLive.N]::WindowFromPoint($p), 2) }
function Top-Windows([scriptblock] $where) {
    $list = New-Object System.Collections.Generic.List[IntPtr]
    $cb = [WsLive.N+EnumProc] { param($h, $l) if (& $where $h) { $list.Add($h) }; return $true }
    [void][WsLive.N]::EnumWindows($cb, [IntPtr]::Zero); $list
}
function Child-Windows([IntPtr] $parent, [string] $class) {
    $list = New-Object System.Collections.Generic.List[IntPtr]
    $cb = [WsLive.N+EnumProc] { param($h, $l) if ((Class-Of $h) -eq $class -and [WsLive.N]::IsWindowVisible($h)) { $list.Add($h) }; return $true }
    [void][WsLive.N]::EnumChildWindows($parent, $cb, [IntPtr]::Zero); $list
}
function Focus-Of([IntPtr] $h) {
    $info = New-Object WsLive.N+GUITHREADINFO; $info.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($info)
    [void][WsLive.N]::GetGUIThreadInfo((Thread-Of $h), [ref] $info); $info.hwndFocus
}
function Key([uint16] $vk, [uint16] $scan = 0) { [WsLive.N]::Send(@([WsLive.N]::KeyEvent($vk, $scan, $false), [WsLive.N]::KeyEvent($vk, $scan, $true))) }
function Move-To([int] $x, [int] $y) { [void][WsLive.N]::Send(@([WsLive.N]::MoveTo($x, $y))) }
function Left-Down { [void][WsLive.N]::Send(@([WsLive.N]::Mouse(0x0002))) }
function Left-Up { [void][WsLive.N]::Send(@([WsLive.N]::Mouse(0x0004))) }
function Click-At([int] $x, [int] $y, [IntPtr] $expectRoot) {
    Move-To $x $y; Start-Sleep -Milliseconds 120
    $root = Root-At $x $y
    if ($root -ne $expectRoot) { throw "click at $x,$y would land on $(Class-Of $root) ($root), not the expected window ($expectRoot): stopped" }
    Left-Down; Start-Sleep -Milliseconds 40; Left-Up
}
function Shot([int] $x, [int] $y, [int] $w, [int] $h, [string] $path, [double] $scale = 0.5) {
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($x, $y, 0, 0, $bmp.Size); $g.Dispose()
    $sw = [int]($w * $scale); $sh = [int]($h * $scale)
    $small = New-Object System.Drawing.Bitmap $sw, $sh
    $g = [System.Drawing.Graphics]::FromImage($small); $g.InterpolationMode = 'HighQualityBicubic'; $g.DrawImage($bmp, 0, 0, $sw, $sh); $g.Dispose()
    $codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
    $params = New-Object System.Drawing.Imaging.EncoderParameters 1
    $params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 82L
    $small.Save($path, $codec, $params); $small.Dispose(); $bmp.Dispose()
}

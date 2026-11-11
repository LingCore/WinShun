# Issue #286 live: the search box by an Open dialog, searching only the folder
# the dialog shows. A staged WinShun on its own profile (the box turned on, a
# small walked index), a real Open dialog of an ordinary PowerShell in a test
# folder; the word typed into the box finds files there, deeper and elsewhere.
# The profile starts with "only this folder" on. Screenshots of dialog and box:
# only there (the folder's name before the text), then everywhere after a
# click on that name. Keys go only while the staged WinShun is in front.
#
#   ./run-live.ps1 -Build <build dir> -Stage <a deployed WinShun folder to copy> -Root <work dir under dist> -Run <empty dir for the shots>
param(
    [Parameter(Mandatory)] [string] $Build,
    [Parameter(Mandatory)] [string] $Stage,
    [Parameter(Mandatory)] [string] $Root,
    [Parameter(Mandatory)] [string] $Run
)
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path
$Stage = (Resolve-Path $Stage).Path
New-Item -ItemType Directory -Force $Root, $Run | Out-Null
$Root = (Resolve-Path $Root).Path
$Run = (Resolve-Path $Run).Path
$app = Join-Path $Root 'WinShun'
$profileDir = Join-Path $Root 'profile'
$exe = Join-Path $app 'WinShun.exe'
$ini = Join-Path $profileDir 'WinShun\WinShun.ini'
$lock = 'F:\wswt\ui.lock'
function Note($t) { $text = "{0:HH:mm:ss.fff} {1}" -f (Get-Date), $t; Add-Content (Join-Path $Run 'run.log') $text -Encoding UTF8; $text }
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -Namespace Ws -Name N -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[StructLayout(LayoutKind.Explicit, Size = 40)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public ushort vk; [FieldOffset(10)] public ushort scan; [FieldOffset(12)] public uint flags; }
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(System.Drawing.Point p);
[DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
[DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern IntPtr GetTopWindow(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
'@
[void][Ws.N]::SetProcessDPIAware()
function Rect($h) { $r = New-Object Ws.N+RECT; [void][Ws.N]::GetWindowRect($h, [ref]$r); $r }
function PidOf($h) { $p = 0; [void][Ws.N]::GetWindowThreadProcessId($h, [ref]$p); $p }
function Keys([object[]] $list) { # each: vk, up (0/1), a character instead (0: none)
    $inputs = foreach ($k in $list) {
        $i = New-Object Ws.N+INPUT; $i.type = 1
        if ($k[2]) { $i.scan = [uint16]$k[2]; $i.flags = 4 + 2 * $k[1] } else { $i.vk = [uint16]$k[0]; $i.flags = 2 * $k[1] }
        $i
    }
    $sent = [Ws.N]::SendInput($inputs.Count, [Ws.N+INPUT[]]$inputs, [Runtime.InteropServices.Marshal]::SizeOf([type][Ws.N+INPUT]))
    if ($sent -ne $inputs.Count) { throw "SendInput sent $sent of $($inputs.Count)" }
}
function FrontIsStaged { (PidOf ([Ws.N]::GetForegroundWindow())) -eq $script:staged.Id }
function Shot([string] $name) {
    $d = Rect $script:dialog; $b = Rect $script:bar
    $l = [Math]::Min($d.L, $b.L); $t = [Math]::Min($d.T, $b.T); $r = [Math]::Max($d.R, $b.R); $bt = [Math]::Max($d.B, $b.B)
    $bmp = New-Object System.Drawing.Bitmap ($r - $l), ($bt - $t)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($l, $t, 0, 0, $bmp.Size)
    $g.Dispose(); $bmp.Save((Join-Path $Run "$name.png")); $bmp.Dispose()
    Note "shot $name"
}
$locked = $false
$running = @(Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path } | Select-Object -ExpandProperty Path -Unique)
$dialogHost = $null
try {
    if (Test-Path (Split-Path $lock)) {
        $deadline = (Get-Date).AddSeconds(120)
        while (-not $locked) {
            try { [IO.File]::Open($lock, 'CreateNew', 'Write').Dispose(); $locked = $true }
            catch { if ((Get-Date) -gt $deadline) { throw 'the UI lock is held' }; Start-Sleep -Milliseconds 500 }
        }
    }
    Note "user's WinShun running: $($running -join ', ')"
    # Not quit for the test: how it was started (which profile) is not known, so it could not be put back.
    if ($running) { throw 'a WinShun is running: run this when none is' }

    robocopy $Stage $app /E /NFL /NDL /NJH /NJS /NP | Out-Null
    Get-ChildItem $app -File | Where-Object { $_.Extension -in '.exe', '.dll' -and (Test-Path (Join-Path $Build $_.Name)) } |
        ForEach-Object { Copy-Item (Join-Path $Build $_.Name) $_.FullName -Force }
    $walked = Join-Path $Root 'walked'
    $here = Join-Path $walked 'here286'
    New-Item -ItemType Directory -Force (Join-Path $here 'deeper'), (Join-Path $walked 'elsewhere'), (Join-Path $profileDir 'WinShun') | Out-Null
    Set-Content (Join-Path $here 'wsq286 in the folder.txt') '1'
    Set-Content (Join-Path $here 'deeper\wsq286 one level down.txt') '2'
    Set-Content (Join-Path $walked 'elsewhere\wsq286 somewhere else.txt') '3'
    @"
[Setup]
Completed=true
[Launcher]
ExplorerMenu=false
DialogBar=true
DialogBarPlace=below
DialogBarFolderOnly=true
[Index]
Access=walk
Scope=folders
Folders=$($walked -replace '\\', '/')
[Clipboard]
Enabled=false
"@ | Set-Content $ini -Encoding UTF8
    $script:staged = Start-Process $exe -ArgumentList '--profile', "`"$profileDir`"", '--background' -WindowStyle Normal -PassThru
    Note "staged WinShun pid $($script:staged.Id)"
    Start-Sleep -Seconds 8

    $title = "ws286 dialog $PID"
    $dialogCode = "Add-Type -AssemblyName System.Windows.Forms; `$d = New-Object System.Windows.Forms.OpenFileDialog; `$d.Title = '$title'; `$d.InitialDirectory = '$here'; [void]`$d.ShowDialog()"
    $dialogHost = Start-Process powershell.exe -ArgumentList '-NoProfile', '-STA', '-Command', "`"$dialogCode`"" -WindowStyle Minimized -PassThru
    $deadline = (Get-Date).AddSeconds(20)
    do { Start-Sleep -Milliseconds 300; $script:dialog = [Ws.N]::FindWindowW('#32770', $title) } while ($script:dialog -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
    if ($script:dialog -eq [IntPtr]::Zero) { throw 'the dialog did not open' }
    # In front for certain: an Alt tap lets this process take the foreground.
    Keys @(@(0x12, 0, 0), @(0x12, 1, 0))
    [void][Ws.N]::SetForegroundWindow($script:dialog)
    Start-Sleep -Milliseconds 500
    if ([Ws.N]::GetForegroundWindow() -ne $script:dialog) { throw "the dialog is not in front (pid $(PidOf ([Ws.N]::GetForegroundWindow())) is)" }
    # The box: the staged WinShun's visible window under the dialog.
    $deadline = (Get-Date).AddSeconds(15)
    $script:bar = [IntPtr]::Zero
    while ($script:bar -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 400
        $d = Rect $script:dialog
        for ($h = [Ws.N]::GetTopWindow([IntPtr]::Zero); $h -ne [IntPtr]::Zero; $h = [Ws.N]::GetWindow($h, 2)) {
            if ([Ws.N]::IsWindowVisible($h) -and (PidOf $h) -eq $script:staged.Id) {
                $r = Rect $h
                if (($r.R - $r.L) -gt 300 -and ($r.B - $r.T) -gt 20 -and $r.T -ge ($d.B - 40)) { $script:bar = $h; break }
            }
        }
    }
    if ($script:bar -eq [IntPtr]::Zero) { throw 'the search box did not come' }
    $b = Rect $script:bar
    $g0 = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero); $scale = $g0.DpiX / 96.0; $g0.Dispose() # the primary screen's, where the dialog opens
    Note "dialog $([int64]$script:dialog), box $([int64]$script:bar) at $($b.L),$($b.T) $($b.R - $b.L)x$($b.B - $b.T)"
    # A click in the box, where nothing else is on top.
    # (Listary puts its own bar on a dialog's bottom edge, over part of the box.)
    $y = $b.T + 24
    $x = 0
    foreach ($share in 0.2, 0.3, 0.12, 0.6, 0.75) {
        $at = $b.L + [int](($b.R - $b.L) * $share)
        $top = [Ws.N]::GetAncestor([Ws.N]::WindowFromPoint((New-Object System.Drawing.Point $at, $y)), 2)
        if ((PidOf $top) -eq $script:staged.Id) { $x = $at; break }
        Note "over the box at ${at}: $((Get-Process -Id (PidOf $top)).ProcessName)"
    }
    if (-not $x) { throw 'something else is over the box' }
    [void][Ws.N]::SetCursorPos($x, $y)
    [Ws.N]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [Ws.N]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 600
    if (-not (FrontIsStaged)) { throw 'the box did not take the keyboard' }
    Keys ('wsq286'.ToCharArray() | ForEach-Object { , @(0, 0, [int]$_); , @(0, 1, [int]$_) })
    Start-Sleep -Milliseconds 1500
    Shot '1-only-this-folder'
    Note "ini: $((Select-String -Path $ini -Pattern 'DialogBarFolderOnly').Line)"
    # A click on the folder's name before the text: everywhere again. (Its key,
    # Ctrl+Shift+D, is another program's global hotkey on this PC.)
    $x = $b.L + [int](70 * $scale); $y = $b.T + [int](22 * $scale)
    $top = [Ws.N]::GetAncestor([Ws.N]::WindowFromPoint((New-Object System.Drawing.Point $x, $y)), 2)
    if ((PidOf $top) -ne $script:staged.Id) { throw 'something else is over the folder name' }
    [void][Ws.N]::SetCursorPos($x, $y)
    [Ws.N]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [Ws.N]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    [void][Ws.N]::SetCursorPos($x + [int](300 * $scale), $y) # off it: no hover colour in the shot
    Start-Sleep -Milliseconds 1500
    Shot '2-everywhere'
    Note "ini after the click: $((Select-String -Path $ini -Pattern 'DialogBarFolderOnly').Line)"
} catch {
    Note "ERROR: $_"
} finally {
    if ($dialogHost -and -not $dialogHost.HasExited) { Stop-Process -Id $dialogHost.Id -Force -ErrorAction SilentlyContinue }
    if (Test-Path $exe) {
        & $exe --profile $profileDir --quit
        $deadline = (Get-Date).AddSeconds(25)
        while ((Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Root) }) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
        Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($app) } |
            ForEach-Object { Note "kill staged $($_.Id) $($_.ProcessName)"; Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
    }
    if ($locked) { [IO.File]::Delete($lock) }
}

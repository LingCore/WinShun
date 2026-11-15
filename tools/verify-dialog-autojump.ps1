<#
.SYNOPSIS
    Live check of "a file dialog goes to the file manager's folder when you
    come back to it" (Launcher/DialogAutoJump, when = back; GitHub #334),
    with the real Win顺, a real Open dialog and real File Explorer windows.

.DESCRIPTION
    Starts the Win顺 in -AppDir (a deployed folder) with a profile of its own
    under -WorkDir, an Open dialog in a PowerShell of its own, and two File
    Explorer windows on folders it makes (A and B). Then, bringing windows to
    the front as a user switching between them would:

      1. dialog -> A -> dialog: the dialog shows A;
      2. dialog -> B -> dialog: it shows B, though nothing was changed in B's
         window (it showed B all along) -- what #334 reported as not happening;
      3. the dialog taken elsewhere by hand, then B looked at and back: it
         stays where it was taken;
      4. dialog -> A -> dialog: it shows A again.

    Win顺 runs one copy per user: quit yours first and start it again after
    (the script refuses to run while one is up). No keys are typed except
    Alt+D into the test dialog; the mouse is not moved. Everything it starts
    is closed at the end.

.EXAMPLE
    ./tools/verify-dialog-autojump.ps1 -AppDir build\issue-334\stage -WorkDir build\issue-334\live
#>
param(
    [Parameter(Mandatory)] [string] $AppDir,
    [Parameter(Mandatory)] [string] $WorkDir
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$exe = Join-Path (Resolve-Path $AppDir).Path 'WinShun.exe'
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
$profileDir = Join-Path $WorkDir 'profile'
$folders = @{}
foreach ($name in 'start', 'A', 'B') { $folders[$name] = Join-Path $WorkDir "ws334 $name" }
$title = 'ws334 dialog'
$failed = [Collections.Generic.List[string]]::new()
Add-Type -AssemblyName System.Windows.Forms

if (!('WsJump.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
namespace WsJump {
    public static class Native {
        public delegate bool Visitor(IntPtr window, IntPtr data);
        [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, Visitor visitor, IntPtr data);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder text, int max);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr window, StringBuilder text, int max);
        [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr window);
        [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr window, uint which);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string type, string title);
        [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
        [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
        [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr w, string l, uint flags, uint ms, out IntPtr result);
        static string Class(IntPtr w) { var s = new StringBuilder(128); GetClassName(w, s, 128); return s.ToString(); }
        public static bool InFront(IntPtr window) {
            IntPtr front = GetForegroundWindow();
            return front == window || (front != IntPtr.Zero && GetAncestor(front, 3) == window); // GA_ROOTOWNER
        }
        // The folder in the dialog's address bar ("Address: <folder>" on its crumbs), "" if none.
        public static string Folder(IntPtr dialog) {
            string found = "";
            EnumChildWindows(dialog, (w, data) => {
                if (Class(w) != "ToolbarWindow32" || Class(GetParent(w)) != "Breadcrumb Parent") return true;
                var s = new StringBuilder(1024);
                GetWindowText(w, s, 1024);
                string text = s.ToString();
                int colon = text.IndexOf(": ", StringComparison.Ordinal);
                found = colon >= 0 ? text.Substring(colon + 2) : text;
                return false;
            }, IntPtr.Zero);
            return found;
        }
        // The address bar's text box, there after Alt+D: given `folder` and Enter.
        public static bool Type(IntPtr dialog, string folder) {
            IntPtr box = IntPtr.Zero;
            EnumChildWindows(dialog, (w, data) => {
                if (Class(w) != "Edit" || Class(GetParent(GetParent(w))) != "ComboBoxEx32" || Class(GetParent(GetParent(GetParent(w)))) != "msctls_progress32") return true;
                box = w;
                return false;
            }, IntPtr.Zero);
            if (box == IntPtr.Zero) return false;
            IntPtr result;
            SendMessageTimeout(box, 0x000C, IntPtr.Zero, folder, 2, 2000, out result); // WM_SETTEXT
            PostMessage(box, 0x0100, (IntPtr)0x0D, IntPtr.Zero); // WM_KEYDOWN, Enter
            return true;
        }
    }
}
'@
}

function Check([bool] $ok, [string] $what) {
    Write-Host ("  {0} {1}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' }), $what))
    if (-not $ok) { $failed.Add($what) }
}

function WaitFor([scriptblock] $done, [double] $seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        if (& $done) { return $true }
        Start-Sleep -Milliseconds 100
    }
    $false
}

function ToFront([IntPtr] $window) {
    $ok = WaitFor {
        [WsJump.Native]::mouse_event(1, 0, 0, 0, [UIntPtr]::Zero) # moves nothing: Windows then lets this process hand the front on
        [void][WsJump.Native]::SetForegroundWindow($window)
        Start-Sleep -Milliseconds 150
        [WsJump.Native]::InFront($window)
    } 5
    if (-not $ok) { throw "could not bring window $window to the front" }
    Start-Sleep -Milliseconds 800 # as long as a user stays in a window at the least
}

function Shows([string] $name) { [string]::Equals([WsJump.Native]::Folder($dialog).TrimEnd('\'), $folders[$name], [StringComparison]::OrdinalIgnoreCase) }

# The Explorer window showing a folder of the test.
function ExplorerOn([string] $name) {
    $shell = New-Object -ComObject Shell.Application
    foreach ($w in @($shell.Windows())) {
        try { if ($w.Document.Folder.Self.Path -eq $folders[$name]) { return [IntPtr][long]$w.HWND } } catch { }
    }
    [IntPtr]::Zero
}

if (Get-Process WinShun -ErrorAction SilentlyContinue) { throw 'Win顺 is running: quit it first (one copy per user), and start it again afterwards.' }

$app = $null
$dialogHost = $null
$opened = @()
try {
    if (Test-Path $WorkDir) { Remove-Item $WorkDir -Recurse -Force }
    foreach ($folder in $folders.Values) { [void](New-Item -ItemType Directory -Force $folder) }
    [void](New-Item -ItemType Directory -Force (Join-Path $profileDir 'WinShun'))
    # Nothing of the user's is touched or taken over: no menu entries, no hotkeys, no index of file contents.
    Set-Content -LiteralPath (Join-Path $profileDir 'WinShun\WinShun.ini') -Encoding utf8 -Value @'
[Setup]
Completed=true
[Launcher]
ExplorerMenu=false
DoubleCtrl=false
Hotkey=
History=false
DialogJump=true
DialogBar=false
DialogAutoJump=true
DialogAutoJumpWhen=back
DialogAutoJumpAsked=true
[Content]
Index=false
'@
    $app = Start-Process -FilePath $exe -ArgumentList '--profile', "`"$profileDir`"", '--background' -WorkingDirectory (Split-Path $exe) -PassThru
    Start-Sleep -Seconds 4
    if ($app.HasExited) { throw "Win顺 did not stay up (exit $($app.ExitCode))" }

    foreach ($name in 'A', 'B') {
        Start-Process explorer.exe -ArgumentList ('"{0}"' -f $folders[$name])
        if (-not (WaitFor { (ExplorerOn $name) -ne [IntPtr]::Zero } 15)) { throw "no Explorer window on $name" }
        $opened += ExplorerOn $name
    }
    $a, $b = $opened
    $script = "Add-Type -AssemblyName System.Windows.Forms; `$d = New-Object System.Windows.Forms.OpenFileDialog; `$d.Title = '$title'; `$d.InitialDirectory = '$($folders['start'])'; [void]`$d.ShowDialog()"
    $dialogHost = Start-Process powershell.exe -ArgumentList '-NoProfile', '-STA', '-WindowStyle', 'Hidden', '-Command', "`"$script`"" -PassThru
    if (-not (WaitFor { [WsJump.Native]::FindWindow('#32770', $title) -ne [IntPtr]::Zero } 20)) { throw 'the Open dialog did not come up' }
    $dialog = [WsJump.Native]::FindWindow('#32770', $title)
    ToFront $dialog
    Check (WaitFor { Shows 'start' } 5) "the dialog opens where it was told (shows: $([WsJump.Native]::Folder($dialog)))"

    Write-Host '1. To A and back'
    ToFront $a
    ToFront $dialog
    Check (WaitFor { Shows 'A' } 5) "the dialog shows A (shows: $([WsJump.Native]::Folder($dialog)))"

    Write-Host '2. To B, a window showing B all along, and back (#334)'
    ToFront $b
    ToFront $dialog
    Check (WaitFor { Shows 'B' } 5) "the dialog shows B (shows: $([WsJump.Native]::Folder($dialog)))"

    Write-Host '3. Taken elsewhere by hand, B only looked at'
    [System.Windows.Forms.SendKeys]::SendWait('%d')
    Start-Sleep -Milliseconds 500
    Check ([WsJump.Native]::Type($dialog, $folders['start']) -and (WaitFor { Shows 'start' } 5)) "the dialog went where it was typed (shows: $([WsJump.Native]::Folder($dialog)))"
    ToFront $b
    ToFront $dialog
    Start-Sleep -Seconds 3
    Check (Shows 'start') "and stays there (shows: $([WsJump.Native]::Folder($dialog)))"

    Write-Host '4. To A and back again'
    ToFront $a
    ToFront $dialog
    Check (WaitFor { Shows 'A' } 5) "the dialog shows A (shows: $([WsJump.Native]::Folder($dialog)))"
} finally {
    if ($dialogHost -and -not $dialogHost.HasExited) { Stop-Process -Id $dialogHost.Id -Force }
    foreach ($w in $opened) { [void][WsJump.Native]::PostMessage($w, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) } # WM_CLOSE
    if ($app -and -not $app.HasExited) {
        Start-Process -FilePath $exe -ArgumentList '--profile', "`"$profileDir`"", '--quit' -WorkingDirectory (Split-Path $exe) -Wait
        if (-not $app.WaitForExit(15000)) { Stop-Process -Id $app.Id -Force }
    }
    Start-Sleep -Seconds 1
    if (Test-Path $WorkDir) { Remove-Item $WorkDir -Recurse -Force -ErrorAction SilentlyContinue }
}
if ($failed.Count) { Write-Host "FAILED: $($failed.Count)"; exit 1 }
Write-Host 'All checks passed.'

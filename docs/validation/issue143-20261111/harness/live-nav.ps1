# Issue #143 live test, elevated part: an administrator's Open dialog of the
# Windows XP style (dialog.ps1) and an isolated SYSTEM console service that
# takes -Client for WinShun's own UI. Says where the dialog is and what its
# file name box holds before (state-before.txt) and, once the stop file is
# there, after (state-after.txt); then removes all of it.
#
#   powershell -ExecutionPolicy Bypass -File live-nav.ps1 -Run <dir> -Service <WinShunService.exe> -Client <tst_servicedialognav.exe>
#       -Pipe <name> -From <folder the dialog opens in>
param(
    [Parameter(Mandatory)] [string] $Run,
    [Parameter(Mandatory)] [string] $Service,
    [Parameter(Mandatory)] [string] $Client,
    [Parameter(Mandatory)] [string] $Pipe,
    [Parameter(Mandatory)] [string] $From
)
$ErrorActionPreference = 'Stop'
$log = Join-Path $Run 'harness.log'
function Note($t) { Add-Content -Path $log -Value ("{0:HH:mm:ss.fff} {1}" -f (Get-Date), $t) -Encoding UTF8 }
$task = 'WsNav143Svc'
$title = "WinShun issue 143 $PID"
$name = 'keep me.txt'
Add-Type -Namespace Ws -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string type, string title);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string type, string title);
[DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dialog, int id);
[DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr wParam, System.Text.StringBuilder text);
public static string Text(IntPtr window) {
    var text = new System.Text.StringBuilder(2048);
    SendMessageW(window, 0x000D, (IntPtr)text.Capacity, text); // WM_GETTEXT
    return text.ToString();
}
'@
# Where the dialog is (its "Look in" box names the folder), and what its file
# name box holds.
function State([IntPtr] $dialog) {
    $lookIn = [Ws.Native]::Text([Ws.Native]::GetDlgItem($dialog, 0x471)) # cmb2
    $combo = [Ws.Native]::GetDlgItem($dialog, 0x47C) # cmb13
    $inner = [Ws.Native]::FindWindowExW($combo, [IntPtr]::Zero, 'ComboBox', [NullString]::Value)
    if ($inner -eq [IntPtr]::Zero) { $inner = $combo }
    $box = [Ws.Native]::Text([Ws.Native]::FindWindowExW($inner, [IntPtr]::Zero, 'Edit', [NullString]::Value))
    "alive=$([Ws.Native]::IsWindow($dialog)) visible=$([Ws.Native]::IsWindowVisible($dialog)) lookIn=[$lookIn] fileName=[$box]"
}
$child = $null
try {
    Note "integrity: $((whoami /groups | Select-String 'Mandatory Label').ToString().Trim())"
    $child = Start-Process powershell.exe -PassThru -WindowStyle Hidden -ArgumentList @('-STA', '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', "`"$(Join-Path $PSScriptRoot 'dialog.ps1')`"", '-Title', "`"$title`"", '-Folder', "`"$From`"", '-Name', "`"$name`"")
    $dialog = [IntPtr]::Zero
    $wait = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $wait) {
        $dialog = [Ws.Native]::FindWindowW('#32770', $title)
        if ($dialog -ne [IntPtr]::Zero -and [Ws.Native]::FindWindowExW($dialog, [IntPtr]::Zero, 'SHELLDLL_DefView', [NullString]::Value) -ne [IntPtr]::Zero) { break }
        $dialog = [IntPtr]::Zero
        Start-Sleep -Milliseconds 200
    }
    if ($dialog -eq [IntPtr]::Zero) { throw 'the dialog did not come up' }
    Start-Sleep -Milliseconds 800 # its list filled
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $action = New-ScheduledTaskAction -Execute $Service -Argument "--console --pipe $Pipe --installed-from `"$Client`""
    Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName $task
    $wait = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $wait -and -not (Test-Path "\\.\pipe\$Pipe")) { Start-Sleep -Milliseconds 200 }
    Note "pipe $(Test-Path "\\.\pipe\$Pipe")"
    $before = State $dialog
    Note "before: $before"
    Set-Content -Path (Join-Path $Run 'state-before.txt') -Value $before -Encoding UTF8
    Set-Content -Path (Join-Path $Run 'windows.txt') -Value ([int64]$dialog)
    Set-Content -Path (Join-Path $Run 'up') -Value 'up'
    $deadline = (Get-Date).AddSeconds(150)
    while (-not (Test-Path (Join-Path $Run 'stop')) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
    Start-Sleep -Milliseconds 500 # the list of the folder it went to
    $after = State $dialog
    Note "after: $after"
    Set-Content -Path (Join-Path $Run 'state-after.txt') -Value $after -Encoding UTF8
} catch {
    Note "ERROR: $_"
} finally {
    if ($child -and -not $child.HasExited) { Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue }
    try { Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue } catch {}
    try { Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue } catch {}
    # Only this build's copy, never the installed service.
    Get-CimInstance Win32_Process -Filter "Name='WinShunService.exe'" |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath -eq $Service } |
        ForEach-Object { Note "kill $($_.ProcessId) $($_.ExecutablePath)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Note 'cleaned'
    Set-Content -Path (Join-Path $Run 'cleaned') -Value 'cleaned'
}

# Issue #277 live test, elevated part: two administrator windows (one in
# front, one behind) that log the keys they get, and an isolated SYSTEM
# console service that takes -Client for WinShun's own UI. Waits for the stop
# file (or 150 s), then removes all of it. Writes nothing to the clipboard.
#
#   powershell -ExecutionPolicy Bypass -File live-paste.ps1 -Run <dir> -Service <WinShunService.exe> -Client <tst_servicepaste.exe> -Pipe <name>
#       [-Before <tst_service.exe>,<tst_paster.exe> -QtBin <Qt>\bin]
param(
    [Parameter(Mandatory)] [string] $Run,
    [Parameter(Mandatory)] [string] $Service,
    [Parameter(Mandatory)] [string] $Client,
    [Parameter(Mandatory)] [string] $Pipe,
    # Tests that need an administrator or the foreground (tst_service,
    # tst_paster), run first; their reports go to -Run. -QtBin: Qt's DLLs.
    [string] $Before, # separated by commas
    [string] $QtBin
)
$ErrorActionPreference = 'Stop'
$log = Join-Path $Run 'harness.log'
$keys = Join-Path $Run 'window.log'
function Note($t) { Add-Content -Path $log -Value ("{0:HH:mm:ss.fff} {1}" -f (Get-Date), $t) -Encoding UTF8 }
$task = 'WsPaste277Svc'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Namespace Ws -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
'@
try {
    Note "integrity: $((whoami /groups | Select-String 'Mandatory Label').ToString().Trim())"
    if ($QtBin) { $env:PATH = "$QtBin;$env:PATH" }
    foreach ($exe in @($Before -split ',' | Where-Object { $_ })) {
        $name = [IO.Path]::GetFileNameWithoutExtension($exe)
        $p = Start-Process -FilePath $exe -ArgumentList @('-o', "`"$(Join-Path $Run "$name.txt")`",txt") -Wait -PassThru
        Note "$name exit $($p.ExitCode)"
    }
    function New-Target([string] $name, [int] $left, [bool] $top) {
        $form = New-Object System.Windows.Forms.Form
        $form.Text = "WinShun issue 277 $name"
        $form.Name = $name
        $form.StartPosition = 'Manual'
        $form.Left = $left; $form.Top = 120; $form.Width = 360; $form.Height = 160
        $form.TopMost = $top
        $form.KeyPreview = $true
        $form.Add_KeyDown({ param($s, $e) Add-Content -Path $keys -Value ("{0:HH:mm:ss.fff} {1} down {2} ctrl={3} shift={4} alt={5}" -f (Get-Date), $s.Name, $e.KeyCode, $e.Control, $e.Shift, $e.Alt) })
        $form.Add_KeyUp({ param($s, $e) Add-Content -Path $keys -Value ("{0:HH:mm:ss.fff} {1} up {2}" -f (Get-Date), $s.Name, $e.KeyCode) })
        return $form
    }
    $behind = New-Target 'behind' 520 $false
    $front = New-Target 'front' 120 $true
    # Twice: started with a hidden console, a process's first ShowWindow
    # hides whatever it is asked to show.
    $behind.Show(); $behind.Hide(); $behind.Show()
    $deadline = (Get-Date).AddSeconds(150)
    $state = @{ up = $false }
    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 200
    $timer.Add_Tick({
        if (-not $state.up) {
            $state.up = $true
            # In front for certain: an Alt tap lets this process take the foreground.
            [Ws.Native]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
            [Ws.Native]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
            [void][Ws.Native]::SetForegroundWindow($front.Handle)
            $front.Activate()
            $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
            $action = New-ScheduledTaskAction -Execute $Service -Argument "--console --pipe $Pipe --installed-from `"$Client`""
            Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
            Start-ScheduledTask -TaskName $task
            $wait = (Get-Date).AddSeconds(20)
            while ((Get-Date) -lt $wait -and -not (Test-Path "\\.\pipe\$Pipe")) { Start-Sleep -Milliseconds 200 }
            Note "pipe $(Test-Path "\\.\pipe\$Pipe")"
            [void][Ws.Native]::SetForegroundWindow($front.Handle)
            $front.Activate()
            Note "front=$([int64]$front.Handle) behind=$([int64]$behind.Handle) foreground=$([int64][Ws.Native]::GetForegroundWindow())"
            Set-Content -Path (Join-Path $Run 'windows.txt') -Value "$([int64]$front.Handle) $([int64]$behind.Handle)"
            Set-Content -Path (Join-Path $Run 'up') -Value 'up'
        }
        if ((Test-Path (Join-Path $Run 'stop')) -or (Get-Date) -gt $deadline) {
            $timer.Stop()
            $front.Close()
        }
    })
    $timer.Start()
    [System.Windows.Forms.Application]::Run($front)
    Note 'stopping'
} catch {
    Note "ERROR: $_"
} finally {
    try { Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue } catch {}
    try { Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue } catch {}
    # Only this build's copy, never the installed service.
    Get-CimInstance Win32_Process -Filter "Name='WinShunService.exe'" |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath -eq $Service } |
        ForEach-Object { Note "kill $($_.ProcessId) $($_.ExecutablePath)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Note 'cleaned'
    Set-Content -Path (Join-Path $Run 'cleaned') -Value 'cleaned'
}

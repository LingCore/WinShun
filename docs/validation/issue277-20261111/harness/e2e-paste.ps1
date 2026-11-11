# Issue #277 end to end, elevated part: an administrator's window with a text
# box in front, an isolated SYSTEM console service that takes -Client (the
# staged WinShun.exe) for WinShun's own UI, and the keys a user would press:
# the clipboard's hotkey (Ctrl+Alt+F11 in the staged profile), then Enter.
# Elevated because Windows drops keys an ordinary process sends while an
# administrator's window is in front. Driven by files in -Run: writes "up",
# waits for "go", writes "done"; removes everything at "stop" (or 180 s).
param(
    [Parameter(Mandatory)] [string] $Run,
    [Parameter(Mandatory)] [string] $Service,
    [Parameter(Mandatory)] [string] $Client,
    [Parameter(Mandatory)] [string] $Pipe
)
$ErrorActionPreference = 'Stop'
$log = Join-Path $Run 'harness.log'
function Note($t) { Add-Content -Path $log -Value ("{0:HH:mm:ss.fff} {1}" -f (Get-Date), $t) -Encoding UTF8 }
$task = 'WsPaste277E2E'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Namespace Ws -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder name, int max);
'@
function Front {
    $h = [Ws.Native]::GetForegroundWindow()
    $procId = 0
    [void][Ws.Native]::GetWindowThreadProcessId($h, [ref]$procId)
    $name = New-Object System.Text.StringBuilder 128
    [void][Ws.Native]::GetClassName($h, $name, 128)
    $exe = try { (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { '?' }
    return "hwnd=$([int64]$h) $exe class=$name"
}
function Key([byte] $vk, [bool] $up) { [Ws.Native]::keybd_event($vk, 0, $(if ($up) { 2 } else { 0 }), [UIntPtr]::Zero) }
try {
    Note "integrity: $((whoami /groups | Select-String 'Mandatory Label').ToString().Trim())"
    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'WinShun issue 277 administrator window'
    $form.StartPosition = 'Manual'
    $form.Left = 160; $form.Top = 160; $form.Width = 520; $form.Height = 220
    $box = New-Object System.Windows.Forms.TextBox
    $box.Multiline = $true
    $box.Dock = 'Fill'
    $form.Controls.Add($box)
    $box.Add_TextChanged({ Note "text box: [$($box.Text)]" })
    $state = @{ step = 0; at = Get-Date }
    $deadline = (Get-Date).AddSeconds(180)
    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 100
    $timer.Add_Tick({
        $since = ((Get-Date) - $state.at).TotalMilliseconds
        switch ($state.step) {
            0 {
                # Shown again: started with a hidden console, a process's first
                # ShowWindow hides whatever it is asked to show.
                $form.Visible = $false; $form.Visible = $true
                # In front for certain: an Alt tap lets this process take the foreground.
                Key 0x12 $false; Key 0x12 $true
                [void][Ws.Native]::SetForegroundWindow($form.Handle)
                $form.Activate(); $box.Focus()
                $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
                $action = New-ScheduledTaskAction -Execute $Service -Argument "--console --pipe $Pipe --installed-from `"$Client`""
                Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
                Start-ScheduledTask -TaskName $task
                $wait = (Get-Date).AddSeconds(20)
                while ((Get-Date) -lt $wait -and -not (Test-Path "\\.\pipe\$Pipe")) { Start-Sleep -Milliseconds 200 }
                Note "pipe $(Test-Path "\\.\pipe\$Pipe")"
                Set-Content -Path (Join-Path $Run 'up') -Value 'up'
                $state.step = 1
            }
            1 {
                if (Test-Path (Join-Path $Run 'go')) {
                    [void][Ws.Native]::SetForegroundWindow($form.Handle)
                    $form.Activate(); $box.Focus()
                    $state.step = 2; $state.at = Get-Date
                }
            }
            2 {
                if ($since -ge 800) {
                    Note "before the hotkey: front $(Front); own window $([int64]$form.Handle)"
                    Key 0x11 $false; Key 0x12 $false; Key 0x7A $false # Ctrl+Alt+F11
                    Key 0x7A $true; Key 0x12 $true; Key 0x11 $true
                    $state.step = 3; $state.at = Get-Date
                }
            }
            3 {
                if ($since -ge 2000) {
                    Note "after the hotkey: front $(Front)"
                    Key 0x0D $false; Key 0x0D $true # Enter: the first entry
                    $state.step = 4; $state.at = Get-Date
                }
            }
            4 {
                if ($since -ge 1000 -and -not $state.early) { $state.early = $true; Note "1 s after Enter: front $(Front)" }
                if ($since -ge 3000) {
                    Note "after Enter: front $(Front); text box holds [$($box.Text)]"
                    Set-Content -Path (Join-Path $Run 'result.txt') -Value $box.Text -Encoding UTF8
                    Set-Content -Path (Join-Path $Run 'done') -Value 'done'
                    $state.step = 5
                }
            }
        }
        if ((Test-Path (Join-Path $Run 'stop')) -or (Get-Date) -gt $deadline) {
            $timer.Stop()
            $form.Close()
        }
    })
    $timer.Start()
    [System.Windows.Forms.Application]::Run($form)
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

# Issue #287 end to end, elevated part: an administrator's window with a text
# box in front, an isolated SYSTEM console service that takes -Client (the
# staged WinShun.exe) for WinShun's own UI, and the clipboard's hotkey
# (Ctrl+Alt+F11 in the staged profile) pressed the ways a hand would: tapped,
# held and pressed again, held a second, another key in between. What the
# text box holds after each goes to results.txt (name, tab, text). Elevated
# because Windows drops keys an ordinary process sends while an
# administrator's window is in front. Driven by files in -Run: writes "up",
# waits for "go", writes "done"; removes everything at "stop" (or 300 s).
param(
    [Parameter(Mandatory)] [string] $Run,
    [Parameter(Mandatory)] [string] $Service,
    [Parameter(Mandatory)] [string] $Client,
    [Parameter(Mandatory)] [string] $Pipe
)
$ErrorActionPreference = 'Stop'
$log = Join-Path $Run 'harness.log'
function Note($t) { Add-Content -Path $log -Value ("{0:HH:mm:ss.fff} {1}" -f (Get-Date), $t) -Encoding UTF8 }
$task = 'WsPaste287E2E'
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
function Key([byte] $vk, [bool] $up, [bool] $extended = $false) {
    [Ws.Native]::keybd_event($vk, 0, $(if ($up) { 2 } else { 0 }) + $(if ($extended) { 1 } else { 0 }), [UIntPtr]::Zero)
}
$ctrl = 0x11; $alt = 0x12; $f11 = 0x7A
function Down { Key $ctrl $false; Key $alt $false; Key $f11 $false; Key $f11 $true } # the hotkey, its modifiers kept down
function Again { Key $f11 $false; Key $f11 $true }
function Up { Key $alt $true; Key $ctrl $true }
function FrontIsWinShun { (Front) -match ' WinShun class=' }
# A key for the clipboard's window only: never into whatever else is in front.
function ToWinShun([byte] $vk, [bool] $extended = $false) {
    if (FrontIsWinShun) { Key $vk $false $extended; Key $vk $true $extended; return $true }
    Note "not sent (vk $vk): front $(Front)"
    return $false
}
try {
    Note "integrity: $((whoami /groups | Select-String 'Mandatory Label').ToString().Trim())"
    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'WinShun issue 287 administrator window'
    $form.StartPosition = 'Manual'
    $form.Left = 160; $form.Top = 160; $form.Width = 520; $form.Height = 220
    $box = New-Object System.Windows.Forms.TextBox
    $box.Multiline = $true
    $box.Dock = 'Fill'
    $form.Controls.Add($box)
    $box.Add_TextChanged({ Note "text box: [$($box.Text)]" })
    $results = Join-Path $Run 'results.txt'
    function StartWay([string] $name) {
        $box.Text = ''
        Key 0x12 $false; Key 0x12 $true
        [void][Ws.Native]::SetForegroundWindow($form.Handle)
        $form.Activate(); $box.Focus()
        Note "--- $name"
    }
    function CheckWay([string] $name) {
        Note "${name}: front $(Front); text box holds [$($box.Text)]"
        Add-Content -Path $results -Value "$name`t$($box.Text)" -Encoding UTF8
        if (FrontIsWinShun) { [void](ToWinShun 0x1B) } # still up: closed for the next one
    }
    # Each: milliseconds after the one before, what to do then.
    $plan = @(
        @(0, { StartWay 'tap' }), @(800, { Note "front $(Front); own window $([int64]$form.Handle)"; Down; Up }), @(2500, { CheckWay 'tap' }),
        @(800, { StartWay 'step' }), @(800, { Down }), @(700, { Note "held: front $(Front)"; Again }), @(400, { Up }), @(2500, { CheckWay 'step' }),
        @(800, { StartWay 'two-steps' }), @(800, { Down }), @(700, { Again }), @(300, { Again }), @(400, { Up }), @(2500, { CheckWay 'two-steps' }),
        @(800, { StartWay 'held' }), @(800, { Down }), @(1500, { Up }), @(2500, { CheckWay 'held' }),
        # Twice within the time the window takes to come, let go at once.
        @(800, { StartWay 'quick-step' }), @(800, { Down; Start-Sleep -Milliseconds 15; Again; Start-Sleep -Milliseconds 15; Up }), @(3000, { CheckWay 'quick-step' }),
        # Another key while held (Down arrow): letting go pastes nothing.
        @(800, { StartWay 'other-key' }), @(800, { Down }), @(700, { Again }), @(300, { [void](ToWinShun 0x28 $true) }), @(300, { Up }), @(2500, { CheckWay 'other-key' }),
        # Alt let go and pressed again, then the hotkey: a press as ever (it closes), nothing pasted.
        @(800, { StartWay 'part-up' }), @(800, { Down }), @(700, { Again }), @(300, { Key $alt $true }), @(300, { Key $alt $false; Again }), @(300, { Up }), @(2500, { CheckWay 'part-up' }),
        # As before this change: opened with a tap, Enter pastes the first row.
        @(800, { StartWay 'enter' }), @(800, { Down; Up }), @(2000, { [void](ToWinShun 0x0D) }), @(2500, { CheckWay 'enter' }),
        @(300, { Set-Content -Path (Join-Path $Run 'done') -Value 'done' })
    )
    $state = @{ step = -2; at = Get-Date }
    $deadline = (Get-Date).AddSeconds(300)
    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 50
    $timer.Add_Tick({
        $since = ((Get-Date) - $state.at).TotalMilliseconds
        if ($state.step -eq -2) {
            $state.step = -1
            # Shown again: started with a hidden console, a process's first
            # ShowWindow hides whatever it is asked to show.
            $form.Visible = $false; $form.Visible = $true
            $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
            $action = New-ScheduledTaskAction -Execute $Service -Argument "--console --pipe $Pipe --installed-from `"$Client`""
            Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
            Start-ScheduledTask -TaskName $task
            $wait = (Get-Date).AddSeconds(20)
            while ((Get-Date) -lt $wait -and -not (Test-Path "\\.\pipe\$Pipe")) { Start-Sleep -Milliseconds 200 }
            Note "pipe $(Test-Path "\\.\pipe\$Pipe")"
            Set-Content -Path (Join-Path $Run 'up') -Value 'up'
        } elseif ($state.step -eq -1) {
            if (Test-Path (Join-Path $Run 'go')) { $state.step = 0; $state.at = Get-Date }
        } elseif ($state.step -lt $plan.Count -and $since -ge $plan[$state.step][0]) {
            $do = $plan[$state.step][1]
            $state.step++
            try { & $do } catch { Note "ERROR in step $($state.step): $_"; Key $f11 $true; Key $alt $true; Key $ctrl $true }
            $state.at = Get-Date
        }
        if ((Test-Path (Join-Path $Run 'stop')) -or (Get-Date) -gt $deadline) {
            $timer.Stop()
            Key $f11 $true; Key $alt $true; Key $ctrl $true # nothing left down
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

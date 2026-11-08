Import-Module "$PSScriptRoot\WinShun.psm1"
$dataDir = Get-WinShunDataDir
function Write-Result([string] $line) {
    [IO.File]::AppendAllText((Join-Path $dataDir 'result.txt'), "$line pid=$PID $(Get-Date -Format o)`n")
}
if (Test-WinShunAction) {
    Write-Result "once action start"
    Add-Type -AssemblyName System.Windows.Forms
    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'WinShun once test window'
    $form.Width = 420
    $form.Height = 220
    $form.StartPosition = 'CenterScreen'
    $state = @{ handle = [IntPtr]::Zero; activeAtShow = $false; activeAfter = $false }
    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 1200
    $timer.Add_Tick({ $state.activeAfter = ([System.Windows.Forms.Form]::ActiveForm -eq $form); $timer.Stop(); $form.Close() }.GetNewClosure())
    $form.Add_Shown({ $state.handle = $form.Handle; $form.Activate(); $state.activeAtShow = ([System.Windows.Forms.Form]::ActiveForm -eq $form); $timer.Start() }.GetNewClosure())
    $form.ShowDialog() | Out-Null
    Write-Result "once foreground=$($state.activeAfter) atShow=$($state.activeAtShow) handle=$($state.handle)"
    Send-WinShunEffects @((New-WinShunEffect -Hide))
} else {
    $q = Get-WinShunQuery
    Write-Result "once query '$($q.text)'"
    Send-WinShunReply @((New-WinShunItem -Id 'open' -Title "Open the once test window ($($q.text))" -Subtitle 'Enter starts a process that opens a window' -Score 100))
}

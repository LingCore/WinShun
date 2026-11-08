Import-Module "$PSScriptRoot\WinShun.psm1"
Add-Type -AssemblyName System.Windows.Forms

function Write-Result([string] $dataDir, [string] $line) {
    if (-not (Test-Path $dataDir)) { New-Item -ItemType Directory -Path $dataDir -Force | Out-Null }
    [IO.File]::AppendAllText((Join-Path $dataDir 'result.txt'), "$line pid=$PID $(Get-Date -Format o)`n")
}

# Opens a window and records whether it is the foreground window 1.2 s later.
# Form.ActiveForm is WinForms' GetForegroundWindow mapped to this process's
# forms: our form while it is in front, nothing while another program is.
# (No Add-Type -MemberDefinition: the plugin job forbids starting csc.exe.)
function Show-TestWindow([string] $dataDir, [string] $tag) {
    $form = New-Object System.Windows.Forms.Form
    $form.Text = "WinShun $tag test window"
    $form.Width = 420
    $form.Height = 220
    $form.StartPosition = 'CenterScreen'
    $state = @{ handle = [IntPtr]::Zero; activeAtShow = $false; activeAfter = $false }
    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 1200
    $timer.Add_Tick({ $state.activeAfter = ([System.Windows.Forms.Form]::ActiveForm -eq $form); $timer.Stop(); $form.Close() }.GetNewClosure())
    $form.Add_Shown({ $state.handle = $form.Handle; $form.Activate(); $state.activeAtShow = ([System.Windows.Forms.Form]::ActiveForm -eq $form); $timer.Start() }.GetNewClosure())
    $form.ShowDialog() | Out-Null
    Write-Result $dataDir "$tag foreground=$($state.activeAfter) atShow=$($state.activeAtShow) handle=$($state.handle)"
}

Start-WinShunPlugin -Name 'test.fgt' -OnQuery {
    param($q, $plugin)
    Write-Result $plugin.dataDir "resident query '$($q.text)'"
    @((New-WinShunItem -Id 'open' -Title "Open the resident test window ($($q.text))" -Subtitle 'Enter opens a window of this plugin' -Score 100))
} -OnAction {
    param($a, $plugin)
    Write-Result $plugin.dataDir "resident action start"
    Show-TestWindow $plugin.dataDir 'resident'
    @((New-WinShunEffect -Hide))
}

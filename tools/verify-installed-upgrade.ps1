# Run only when the user has said so for this run: it closes and REPLACES the Win顺 installed on this PC.
# The real thing on this PC (first run 2026-11-12, docs/validation/installer-held-files-20261112): the installed Win顺 (Program Files, for all users,
# with the index service) upgraded by the new Setup, started the way the in-app update starts it, while Win顺 runs
# and another program has one of its libraries loaded. Run unelevated; Setup asks for administrator rights itself.
param([Parameter(Mandatory)] [string] $Setup, [Parameter(Mandatory)] [string] $Out, [string] $Round = 'upgrade', [switch] $Hold)
$ErrorActionPreference = 'Stop'
$app = 'C:\Program Files\WinShun'
New-Item -ItemType Directory -Force $Out | Out-Null
$report = [ordered]@{ round = $Round }
function Elevated([int] $id) { try { [void](Get-Process -Id $id).MainModule.FileName; $false } catch { $true } }
function State {
    $ui = @(Get-Process WinShun -ErrorAction SilentlyContinue)
    [ordered]@{
        version = (Get-Item "$app\WinShun.exe").VersionInfo.ProductVersion
        exeSha = (Get-FileHash "$app\WinShun.exe").Hash.ToLower()
        running = $ui.Count
        runningFrom = @($ui | ForEach-Object { try { $_.MainModule.FileName } catch { 'elevated (path not readable)' } })
        service = ((sc.exe query WinShun | Select-String 'STATE').Line -replace '\s+', ' ').Trim()
        serviceBinary = ((sc.exe qc WinShun | Select-String 'BINARY_PATH_NAME').Line -replace '\s+', ' ').Trim()
        aside = @(Get-ChildItem $app -Recurse -File -Filter '*.wsold*' | ForEach-Object { $_.FullName.Substring($app.Length + 1) })
        uninstallVersion = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{C582F95C-EAF2-4AF2-961D-E35C4506652A}_is1').DisplayVersion
        task = ((schtasks /query /tn \WinShun /xml | Select-String '<Command>|<Arguments>').Line | ForEach-Object { $_.Trim() }) -join ' '
        iniBytes = (Get-Item "$env:APPDATA\WinShun\WinShun.ini").Length
        historySha = (Get-FileHash "$env:LOCALAPPDATA\WinShun\history.txt").Hash.ToLower()
        indexBytes = (Get-Item "$env:LOCALAPPDATA\WinShun\index.bin").Length
    }
}
$holder = $null
try {
    if (-not (Get-Process WinShun -ErrorAction SilentlyContinue)) {
        Start-Process "$app\WinShun.exe" -ArgumentList '--background' -WorkingDirectory $app
        Start-Sleep 8
    }
    $report.before = State
    if ($Hold) {
        # Another program with Qt6Core.dll of the program folder loaded (as an AI tool's relay had it, a plugin's process).
        $flag = Join-Path $Out 'holder.flag'
        Remove-Item $flag -ErrorAction SilentlyContinue
        $command = "[void][Runtime.InteropServices.NativeLibrary]::Load('$app\Qt6Core.dll'); Set-Content -LiteralPath '$flag' -Value 1; Start-Sleep 900"
        $holder = Start-Process (Get-Process -Id $PID).Path -ArgumentList '-NoProfile', '-Command', $command -WindowStyle Hidden -PassThru
        for ($i = 0; $i -lt 100 -and -not (Test-Path $flag); $i++) { Start-Sleep -Milliseconds 100 }
        $report.holderStarted = Test-Path $flag
    }
    $log = Join-Path $Out "$Round-setup.log"
    $watch = [Diagnostics.Stopwatch]::StartNew()
    # As Updater::runSetup: unelevated, Setup asks for the rights itself.
    $setupProcess = Start-Process $Setup -ArgumentList "/SILENT /SUPPRESSMSGBOXES /NORESTART /SP- /ALLUSERS /LOG=`"$log`"" -PassThru
    if (-not $setupProcess.WaitForExit(300000)) { $report.setup = 'still running after 300 s (left alone)'; throw 'Setup did not end in time' }
    $report.setupExit = $setupProcess.ExitCode
    $report.setupSeconds = [Math]::Round($watch.Elapsed.TotalSeconds, 1)
    Start-Sleep 10 # the new version starts in the tray ([Run])
    $report.after = State
    $report.newVersionElevated = @(Get-Process WinShun -ErrorAction SilentlyContinue | ForEach-Object { Elevated $_.Id })
    if ($holder) { $report.holderStillRunning = -not $holder.HasExited }
    $report.logLines = @(Select-String -LiteralPath $log -Pattern 'moved out of the way|Update blocked by|Put back|Still held|Still running from|held file' | ForEach-Object { ($_.Line -replace '^\S+ \S+\s+', '') })
}
catch { $report.error = "$_" }
finally {
    if ($holder -and -not $holder.HasExited) { Stop-Process -Id $holder.Id -Force; [void]$holder.WaitForExit(5000) }
    $report | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Out "$Round.json") -Encoding utf8
    $report | ConvertTo-Json -Depth 5
}

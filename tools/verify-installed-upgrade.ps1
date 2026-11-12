<#
.SYNOPSIS
    Updates a real Win顺 on this PC and checks what came of it. Ask the user
    before each run: it closes and REPLACES the Win顺 installed here.
.DESCRIPTION
    The other half of tools/verify-installer.ps1 (which runs a test build of
    the installer and touches no real Win顺): this one is the real program, the
    real service and the real packages. Run unelevated, from PowerShell 7;
    Setup asks for administrator rights itself (a UAC prompt each time).

    -Mode Setup    The installed copy (Program Files, all users) updated by
                   -Setup, started as the in-app update starts it (silent).
    -Mode Wizard   The same by the wizard, as after a double click: its pages
                   are clicked through by an elevated helper (one more UAC).
    -Mode InApp    The whole in-app update: the installed copy, started with a
                   stand-in update service on this PC (tools/update-feed.py
                   over -Feed, a release folder of the newer -Version), checks,
                   downloads, verifies and runs Setup by itself. The installed
                   copy must have WINSHUN_UPDATE_AUTOINSTALL (0.25.1 or later).
    -Mode Portable A portable copy unpacked from -PortableZip into -Out updates
                   itself from -Feed the same way (the files swapped by
                   WinShunUpdate.exe). The installed copy is quit meanwhile and
                   started again afterwards.

    -Hold: another program keeps Qt6Core.dll of the program folder loaded
    throughout (GitHub #300). The report is <Out>\<Mode>.json; first run and
    what it does not cover: docs/validation/installer-held-files-20261112.
.EXAMPLE
    ./tools/verify-installed-upgrade.ps1 -Mode Setup -Setup dist\release026\WinShun-0.26.0-x64-setup.exe -Out build\release026\upgrade -Hold
    ./tools/verify-installed-upgrade.ps1 -Mode InApp -Feed dist\release026 -Version 0.26.0 -Out build\release026\upgrade -Hold
#>
param(
    [ValidateSet('Setup', 'Wizard', 'InApp', 'Portable')] [string] $Mode = 'Setup',
    [string] $Setup,
    [string] $Feed,
    [string] $Version,
    [string] $PortableZip,
    [Parameter(Mandatory)] [string] $Out,
    [switch] $Hold,
    [int] $Port = 47631
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$installed = 'C:\Program Files\WinShun'
$Out = [IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force $Out | Out-Null
$report = [ordered]@{ mode = $Mode; hold = [bool]$Hold }
$uninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{C582F95C-EAF2-4AF2-961D-E35C4506652A}_is1'

function Elevated([int] $id) { try { [void](Get-Process -Id $id).MainModule.FileName; $false } catch { $true } }
function Running([string] $app) {
    @(Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object {
            try { $_.MainModule.FileName.StartsWith($app, [StringComparison]::OrdinalIgnoreCase) } catch { $false } })
}
function State([string] $app) {
    $ui = @(Get-Process WinShun -ErrorAction SilentlyContinue)
    $state = [ordered]@{
        version = (Get-Item "$app\WinShun.exe").VersionInfo.ProductVersion
        exeSha = (Get-FileHash "$app\WinShun.exe").Hash.ToLower()
        runningFromHere = (Running $app).Count
        running = $ui.Count
        elevated = @($ui | ForEach-Object { Elevated $_.Id })
        aside = @(Get-ChildItem $app -Recurse -File -Filter '*.wsold*' | ForEach-Object { $_.FullName.Substring($app.Length + 1) })
        service = ((sc.exe query WinShun | Select-String 'STATE').Line -replace '\s+', ' ').Trim()
        serviceBinary = ((sc.exe qc WinShun | Select-String 'BINARY_PATH_NAME').Line -replace '\s+', ' ').Trim()
        task = ((schtasks /query /tn \WinShun /xml | Select-String '<Command>|<Arguments>').Line | ForEach-Object { $_.Trim() }) -join ' '
        installedVersion = (Get-ItemProperty $uninstallKey -ErrorAction SilentlyContinue).DisplayVersion
    }
    if ($app -eq $installed) {
        $state.iniBytes = (Get-Item "$env:APPDATA\WinShun\WinShun.ini").Length
        $state.historySha = (Get-FileHash "$env:LOCALAPPDATA\WinShun\history.txt").Hash.ToLower()
        $state.indexBytes = (Get-Item "$env:LOCALAPPDATA\WinShun\index.bin").Length
    }
    $state
}
function Quit([string] $app) {
    if (-not (Get-Process WinShun -ErrorAction SilentlyContinue)) { return }
    Start-Process "$app\WinShun.exe" -ArgumentList '--quit' -WorkingDirectory $app
    for ($i = 0; $i -lt 60 -and (Get-Process WinShun -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
    if (Get-Process WinShun -ErrorAction SilentlyContinue) { throw 'Win顺 did not quit' }
}
function StartApp([string] $app, [hashtable] $environment = @{}) {
    # The child gets this process's environment: set for the start, taken back at once.
    foreach ($name in $environment.Keys) { Set-Item "Env:$name" $environment[$name] }
    try { Start-Process "$app\WinShun.exe" -ArgumentList '--background' -WorkingDirectory $app }
    finally { foreach ($name in $environment.Keys) { Remove-Item "Env:$name" } }
    for ($i = 0; $i -lt 40 -and -not (Running $app).Count; $i++) { Start-Sleep -Milliseconds 500 }
    Start-Sleep 5
}
$helpers = [Collections.Generic.List[Diagnostics.Process]]::new()
function HoldLibrary([string] $app) {
    # Another program with Qt6Core.dll of the program folder loaded (as an AI tool's relay had it, a plugin's process).
    $flag = Join-Path $Out 'holder.flag'
    Remove-Item $flag -ErrorAction SilentlyContinue
    $command = "[void][Runtime.InteropServices.NativeLibrary]::Load('$app\Qt6Core.dll'); Set-Content -LiteralPath '$flag' -Value 1; Start-Sleep 1800"
    $holder = Start-Process (Get-Process -Id $PID).Path -ArgumentList '-NoProfile', '-Command', $command -WindowStyle Hidden -PassThru
    $helpers.Add($holder)
    for ($i = 0; $i -lt 100 -and -not (Test-Path $flag); $i++) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path $flag)) { throw 'the program that holds the library did not start' }
    Remove-Item $flag
    $holder
}
function StartFeed {
    $feedLog = Join-Path $Out "$Mode-feed.log"
    $server = Start-Process python -ArgumentList "`"$root\tools\update-feed.py`"", "`"$([IO.Path]::GetFullPath($Feed))`"", $Version, $Port `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $feedLog -RedirectStandardError "$feedLog.err"
    $helpers.Add($server)
    for ($i = 0; $i -lt 50; $i++) {
        try { [void](Invoke-WebRequest "http://127.0.0.1:$Port/stable.json" -TimeoutSec 2); return $feedLog } catch { Start-Sleep -Milliseconds 200 }
    }
    throw "the stand-in update service did not start: $feedLog.err"
}
function WaitForVersion([string] $app, [string] $version, [int] $seconds) {
    # The new files in place and the new version running from there.
    for ($i = 0; $i -lt $seconds; $i++) {
        $now = try { (Get-Item "$app\WinShun.exe").VersionInfo.ProductVersion } catch { '' }
        if ($now -eq $version -and (Running $app).Count -and -not (Get-Process 'WinShun-*-setup*', WinShunUpdate -ErrorAction SilentlyContinue)) { Start-Sleep 8; return $true }
        Start-Sleep 1
    }
    $false
}
function LogLines([string] $log, [string] $pattern) {
    if (-not (Test-Path -LiteralPath $log)) { return @() }
    @(Select-String -LiteralPath $log -Pattern $pattern | ForEach-Object { ($_.Line -replace '^\d{4}-\d\d-\d\d[ T][\d:.]+\s+', '') })
}
$setupPattern = 'moved out of the way|Update blocked by|Put back|Still held|Still running from|held file'
$feedEnvironment = @{ WINSHUN_UPDATE_FEED = "http://127.0.0.1:$Port/stable.json"; WINSHUN_UPDATE_AUTOINSTALL = '1'; WINSHUN_LOG_INFO = '1' }

try {
    switch ($Mode) {
        'Setup' {
            if (-not (Running $installed).Count) { StartApp $installed }
            $report.before = State $installed
            $holder = if ($Hold) { HoldLibrary $installed }
            $log = Join-Path $Out 'Setup-setup.log'
            $watch = [Diagnostics.Stopwatch]::StartNew()
            # As Updater::runSetup: unelevated, Setup asks for the rights itself.
            $process = Start-Process $Setup -ArgumentList "/SILENT /SUPPRESSMSGBOXES /NORESTART /SP- /ALLUSERS /LOG=`"$log`"" -PassThru
            if (-not $process.WaitForExit(300000)) { throw 'Setup did not end in 300 s (left alone)' }
            $report.setupExit = $process.ExitCode
            $report.seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 1)
            Start-Sleep 10 # the new version starts in the tray ([Run])
            $report.after = State $installed
            if ($holder) { $report.holderStillRunning = -not $holder.HasExited }
            $report.setupLog = LogLines $log $setupPattern
        }
        'Wizard' {
            if (-not (Running $installed).Count) { StartApp $installed }
            $report.before = State $installed
            $holder = if ($Hold) { HoldLibrary $installed }
            $log = Join-Path $Out 'Wizard-setup.log'
            $steps = Join-Path $Out 'Wizard-steps.log'
            Remove-Item $steps -ErrorAction SilentlyContinue
            $watch = [Diagnostics.Stopwatch]::StartNew()
            $process = Start-Process $Setup -ArgumentList "/SP- /ALLUSERS /LOG=`"$log`"" -PassThru
            # The wizard runs as administrator: only an elevated program may press its buttons.
            $clicker = Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass',
                '-File', "`"$root\tools\installer-wizard-clicks.ps1`"", '-Log', "`"$steps`""
            if (-not $process.WaitForExit(300000)) { throw 'Setup did not end in 300 s (left alone)' }
            $report.setupExit = $process.ExitCode
            $report.seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 1)
            [void]$clicker.WaitForExit(15000)
            Start-Sleep 8
            $report.after = State $installed
            if ($holder) { $report.holderStillRunning = -not $holder.HasExited }
            $report.pages = @(if (Test-Path $steps) { Get-Content $steps -Encoding UTF8 })
            $report.setupLog = LogLines $log $setupPattern
        }
        'InApp' {
            $report.before = State $installed
            $feedLog = StartFeed
            Quit $installed
            Remove-Item "$env:LOCALAPPDATA\WinShun\Updates\setup.log" -ErrorAction SilentlyContinue
            $holder = if ($Hold) { HoldLibrary $installed }
            $watch = [Diagnostics.Stopwatch]::StartNew()
            StartApp $installed $feedEnvironment # checks 20 s after its start, then installs by itself
            $report.updated = WaitForVersion $installed $Version 420
            $report.seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 1)
            $report.after = State $installed
            if ($holder) { $report.holderStillRunning = -not $holder.HasExited }
            $report.feedRequests = @(Get-Content $feedLog | ForEach-Object { ($_ -replace '^\S+ ', '') })
            $report.setupLog = LogLines "$env:LOCALAPPDATA\WinShun\Updates\setup.log" $setupPattern
            $report.helperLog = @(if (Test-Path "$env:LOCALAPPDATA\WinShun\Updates\update.log") { Get-Content "$env:LOCALAPPDATA\WinShun\Updates\update.log" -Tail 6 })
            $report.appLog = LogLines "$env:LOCALAPPDATA\WinShun\WinShun.log" 'Update[: ]|已更新到|没有更新到' | Select-Object -Last 8
            # Started by Setup with the test's environment: once more, without it.
            Quit $installed
            StartApp $installed
            $report.runningAfterRestart = (Running $installed).Count
        }
        'Portable' {
            $app = Join-Path $Out 'portable\WinShun'
            if (Test-Path (Join-Path $Out 'portable')) { throw "$Out\portable is there already: a fresh -Out, please" }
            $keys = 'Directory\Background\shell', 'Directory\shell', 'Drive\shell'
            foreach ($key in $keys) { reg export "HKCU\Software\Classes\$key\WinShun.SearchHere" (Join-Path $Out "$($key -replace '\\', '_').reg") /y 2>&1 | Out-Null }
            $taskBefore = (State $installed).task
            $feedLog = StartFeed
            Quit $installed
            try {
                Expand-Archive -LiteralPath $PortableZip -DestinationPath (Join-Path $Out 'portable')
                # Its first start copies the settings of the usual places into its data folder (read only on their side).
                StartApp $app
                Quit $app
                Set-Content -LiteralPath "$app\data\kept-through-the-update.txt" -Value 'marker'
                $report.before = State $app
                $report.dataBefore = @(Get-ChildItem "$app\data" -File | Select-Object -ExpandProperty Name | Sort-Object)
                $holder = if ($Hold) { HoldLibrary $app }
                $watch = [Diagnostics.Stopwatch]::StartNew()
                StartApp $app $feedEnvironment
                $report.updated = WaitForVersion $app $Version 300
                $report.seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 1)
                $report.after = State $app
                $report.markerKept = Test-Path "$app\data\kept-through-the-update.txt"
                $report.dataAfter = @(Get-ChildItem "$app\data" -File | Select-Object -ExpandProperty Name | Sort-Object)
                if ($holder) { $report.holderStillRunning = -not $holder.HasExited }
                $report.feedRequests = @(Get-Content $feedLog | ForEach-Object { ($_ -replace '^\S+ ', '') })
                $updates = Get-ChildItem "$app\data" -Recurse -Filter 'update.log' | Select-Object -First 1
                $report.helperLog = @(if ($updates) { Get-Content $updates.FullName -Tail 12 })
                $report.leftBehind = @(Get-ChildItem $app -Directory | Where-Object Name -match 'update|backup|stage' | Select-Object -ExpandProperty Name)
            }
            finally {
                try { Quit $app } catch { Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { try { $_.MainModule.FileName.StartsWith($Out, [StringComparison]::OrdinalIgnoreCase) } catch { $false } } | Stop-Process -Force }
                # What a portable copy took over on this PC goes back to the installed one.
                foreach ($key in $keys) { $file = Join-Path $Out "$($key -replace '\\', '_').reg"; if (Test-Path $file) { reg import $file 2>&1 | Out-Null } }
                StartApp $installed
                if ((State $installed).task -ne $taskBefore) {
                    Start-Process "$installed\WinShun.exe" -ArgumentList '--take-autostart' -Wait
                    $report.autostartPutBack = $true
                }
                $report.installedAfter = State $installed
            }
        }
    }
}
catch { $report.error = "$_" }
finally {
    foreach ($process in $helpers) { if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } }
    $report | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Out "$Mode.json") -Encoding utf8
    $report | ConvertTo-Json -Depth 6
}

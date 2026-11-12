<#
.SYNOPSIS
    Runs the installer for real, on this PC, against programs that hold its
    files (GitHub #300): must pass before every release.
.DESCRIPTION
    installer\WinShun.iss is compiled twice with /DTestInstall (its own AppId
    and program name, no shortcuts, no [Run], no uninstall entry, never
    elevated) over two small stand-in program folders, "version 1" and
    "version 2", and installed into a folder under -WorkDir. Nothing of the
    Win顺 installed or running on this PC is asked for, closed or replaced: the
    test build knows it by no name. Each case installs version 1, puts
    something in the way, runs the silent Setup of version 2 as the in-app
    update does (/SILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CURRENTUSER /LOG=)
    with a time limit, and checks what is on disk afterwards.

    What it does not cover: the real program closing on --quit, the index
    service, the upgrade from an installed earlier release, the wizard's
    pages. Those need a spare Windows (a VM); see docs/releasing.md.
.EXAMPLE
    ./tools/verify-installer.ps1 -WorkDir build\release026\installer-test
#>
param(
    [Parameter(Mandatory)] [string] $WorkDir, # a fresh folder; everything the test writes is in it
    [string] $Iscc = $env:ISCC,
    [int] $TimeLimitSec = 60 # for one Setup: a hang is a failure
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$work = [IO.Path]::GetFullPath($WorkDir)
if (Test-Path -LiteralPath $work) { throw "WorkDir already exists; choose a fresh folder: $work" }
if (-not $Iscc) {
    $Iscc = @(Get-ChildItem "$env:LOCALAPPDATA\Programs\Inno Setup*\ISCC.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending) + @(Get-Item "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "$env:ProgramFiles\Inno Setup 6\ISCC.exe" -ErrorAction SilentlyContinue) |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Iscc -or -not (Test-Path $Iscc)) { throw 'Inno Setup 6.5 or later is needed, or pass -Iscc' }
New-Item -ItemType Directory -Path $work | Out-Null

# ---- the two stand-in versions -------------------------------------------------
# Real programs and a real library (Windows' own, copied), so that they can run
# and be loaded; version 2's differ by bytes after their end, which changes
# nothing in how they run.
$waiter = Join-Path $env:SystemRoot 'System32\waitfor.exe'
$library = Join-Path $env:SystemRoot 'System32\version.dll'
function New-Version([int] $number) {
    $folder = Join-Path $work "source$number"
    New-Item -ItemType Directory -Path $folder, "$folder\bin", "$folder\fonts" | Out-Null
    foreach ($name in 'WsSetupTest.exe', 'bin\Helper.exe', 'WinShunService.exe', 'WinShunMcp.exe', 'WinShunSearch.exe') {
        Copy-Item $waiter (Join-Path $folder $name)
    }
    Copy-Item $library "$folder\Lib.dll"
    [IO.File]::WriteAllBytes("$folder\fonts\Font.ttf", [byte[]](1..64))
    if ($number -gt 1) {
        foreach ($name in 'WsSetupTest.exe', 'bin\Helper.exe', 'Lib.dll', 'fonts\Font.ttf') {
            $stream = [IO.File]::Open((Join-Path $folder $name), 'Append'); $stream.Write([byte[]](0..15), 0, 16); $stream.Dispose()
        }
    }
    Set-Content -LiteralPath "$folder\version.txt" -Value $number -NoNewline
    & $Iscc /Q /DTestInstall "/DAppVersion=$number.0.0" "/DSourceDir=$folder" "/DOutputDir=$work" (Join-Path $root 'installer\WinShun.iss')
    if ($LASTEXITCODE) { throw "ISCC failed for version $number" }
    Join-Path $work "WsSetupTest-$number.0.0.exe"
}
$setup1 = New-Version 1
$setup2 = New-Version 2
$size = @{}
foreach ($name in 'Lib.dll', 'fonts\Font.ttf', 'WsSetupTest.exe') {
    $size[$name] = @((Get-Item "$work\source1\$name").Length, (Get-Item "$work\source2\$name").Length)
}

# ---- helpers ---------------------------------------------------------------------
$started = [Collections.Generic.List[Diagnostics.Process]]::new()
function Install([string] $setup, [string] $app, [string] $log) {
    # As Updater::runSetup starts it; the folder only for the first install (an update finds it by the AppId... not here:
    # the test build writes no uninstall entry, so the folder is given every time).
    $arguments = "/SILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CURRENTUSER /DIR=`"$app`" /LOG=`"$log`""
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $setup -ArgumentList $arguments -PassThru
    if (-not $process.WaitForExit($TimeLimitSec * 1000)) {
        # Only this Setup and what it started (its own temporary copy), by id.
        Get-CimInstance Win32_Process -Filter "ParentProcessId = $($process.Id)" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        return @{ exit = 'timeout'; seconds = [int]$watch.Elapsed.TotalSeconds }
    }
    @{ exit = $process.ExitCode; seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 1) }
}
function Hold([string] $how, [string] $file) {
    # Another program, not Setup's and not from the program folder: PowerShell itself.
    $ready = "$file.held-flag"
    $body = switch ($how) {
        'loaded' { "[void][Runtime.InteropServices.NativeLibrary]::Load('$file')" } # as a program that loaded the library
        'no-sharing' { "`$s = [IO.File]::Open('$file', 'Open', 'Read', 'None')" } # nobody else may touch it
        'reader' { "`$s = [IO.File]::Open('$file', 'Open', 'Read', 'Read, Delete')" } # as security software reads
    }
    $command = "$body; Set-Content -LiteralPath '$ready' -Value 1; Start-Sleep 600"
    $process = Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList '-NoProfile', '-Command', $command -WindowStyle Hidden -PassThru
    $started.Add($process)
    for ($i = 0; $i -lt 100 -and -not (Test-Path -LiteralPath $ready); $i++) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path -LiteralPath $ready)) { throw "the holder of $file did not start" }
    Remove-Item -LiteralPath $ready
    $process
}
function Release([Diagnostics.Process] $process) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    [void]$process.WaitForExit(5000)
}
function AsideFiles([string] $app) { @(Get-ChildItem -LiteralPath $app -Recurse -File | Where-Object Name -like '*.wsold*') }
function Version([string] $app) { (Get-Content -LiteralPath "$app\version.txt" -Raw).Trim() }
function Is([string] $app, [string] $name, [int] $version) { (Get-Item -LiteralPath "$app\$name").Length -eq $size[$name][$version - 1] }

$results = [Collections.Generic.List[object]]::new()
function Case([string] $name, [scriptblock] $body) {
    $app = Join-Path $work "app-$name"
    $log1 = Join-Path $work "$name-install.log"
    $log2 = Join-Path $work "$name-update.log"
    $failures = [Collections.Generic.List[string]]::new()
    $check = { param([bool] $ok, [string] $what) if (-not $ok) { $failures.Add($what) } }
    $first = Install $setup1 $app $log1
    & $check ($first.exit -eq 0 -and (Version $app) -eq '1') "version 1 did not install (exit $($first.exit))"
    $update = $null
    try {
        if ($failures.Count -eq 0) { $update = & $body $app $log2 $check }
    }
    catch { $failures.Add("the case itself failed: $_") }
    finally {
        foreach ($process in $started) { if (-not $process.HasExited) { Release $process } }
        $started.Clear()
        Get-Process WsSetupTest, Helper -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($work, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    $results.Add([pscustomobject]@{ case = $name; passed = $failures.Count -eq 0; seconds = if ($update) { $update.seconds } else { $null }
            exit = if ($update) { $update.exit } else { $null }; failures = @($failures) })
    Write-Host ("{0,-28} {1}  {2}" -f $name, $(if ($failures.Count) { 'FAILED' } else { 'passed' }), ($failures -join '; '))
}
function LogHas([string] $log, [string] $text) { (Test-Path -LiteralPath $log) -and [bool](Select-String -LiteralPath $log -SimpleMatch $text -Quiet) }

# ---- the cases ---------------------------------------------------------------------
Case 'plain-update' {
    param($app, $log, $check)
    $r = Install $setup2 $app $log
    & $check ($r.exit -eq 0) "exit $($r.exit)"
    & $check ((Version $app) -eq '2' -and (Is $app 'Lib.dll' 2)) 'version 2 is not in place'
    & $check (@(AsideFiles $app).Count -eq 0) 'files were moved aside with nothing holding them'
    $r
}
Case 'own-programs-running' {
    # The program itself (it does not know --quit: ended after the wait) and a helper from its folder.
    param($app, $log, $check)
    $own = Start-Process -FilePath "$app\WsSetupTest.exe" -ArgumentList '/T', '600', 'wsverifyinstaller1' -WindowStyle Hidden -PassThru
    $helper = Start-Process -FilePath "$app\bin\Helper.exe" -ArgumentList '/T', '600', 'wsverifyinstaller2' -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 500
    $r = Install $setup2 $app $log
    & $check ($r.exit -eq 0) "exit $($r.exit)"
    & $check ($own.HasExited -and $helper.HasExited) 'a program running from the folder was left running'
    & $check ((Version $app) -eq '2' -and (Is $app 'WsSetupTest.exe' 2)) 'version 2 is not in place'
    $r
}
Case 'library-loaded-elsewhere' {
    # GitHub #300: another program has a library loaded and never lets go.
    param($app, $log, $check)
    $holder = Hold 'loaded' "$app\Lib.dll"
    $r = Install $setup2 $app $log
    & $check ($r.exit -eq 0) "exit $($r.exit): the update did not go through"
    & $check ((Version $app) -eq '2' -and (Is $app 'Lib.dll' 2)) 'version 2 is not in place'
    & $check (-not $holder.HasExited) 'the other program was ended (it is not ours to end)'
    & $check (@(AsideFiles $app).Count -eq 1) "expected the one old copy beside it, found $(@(AsideFiles $app).Count)"
    & $check (LogHas $log 'moved out of the way') 'the log does not say what was moved'
    Release $holder
    $again = Install $setup2 $app "$log.again.log"
    & $check ($again.exit -eq 0 -and @(AsideFiles $app).Count -eq 0) 'the old copy was not removed by the next Setup once let go'
    $r
}
Case 'file-read-by-another' {
    # As security software reads: sharing reading and deleting, not writing.
    param($app, $log, $check)
    $holder = Hold 'reader' "$app\fonts\Font.ttf"
    $r = Install $setup2 $app $log
    & $check ($r.exit -eq 0) "exit $($r.exit): the update did not go through"
    & $check ((Version $app) -eq '2' -and (Is $app 'fonts\Font.ttf' 2)) 'version 2 is not in place'
    $r
}
Case 'file-nobody-may-touch' {
    # Open with no sharing: it can be neither replaced nor moved. Setup gives up soon,
    # says who, and leaves version 1 whole, the library it had moved aside back in place.
    param($app, $log, $check)
    $loaded = Hold 'loaded' "$app\Lib.dll"
    $locked = Hold 'no-sharing' "$app\fonts\Font.ttf"
    $r = Install $setup2 $app $log
    & $check ($r.exit -ne 0 -and $r.exit -ne 'timeout') "exit $($r.exit): expected Setup to give up, in time"
    & $check ($r.seconds -lt 30) "gave up only after $($r.seconds) s"
    & $check (LogHas $log 'Update blocked by:') 'the log does not name what blocked it'
    & $check ((Version $app) -eq '1' -and (Is $app 'Lib.dll' 1) -and (Is $app 'fonts\Font.ttf' 1) -and (Is $app 'WsSetupTest.exe' 1)) 'version 1 is not whole'
    & $check (@(AsideFiles $app).Count -eq 0) 'files stayed moved aside after Setup gave up'
    Release $locked
    $again = Install $setup2 $app "$log.again.log"
    & $check ($again.exit -eq 0 -and (Version $app) -eq '2' -and (Is $app 'fonts\Font.ttf' 2)) 'the update did not go through once the file was free'
    $r
}

$failed = @($results | Where-Object { -not $_.passed })
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $work 'installer-test.json') -Encoding utf8
# The program folders and the Setups go; the logs and the report stay for the record.
Get-ChildItem -LiteralPath $work -Directory | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $setup1, $setup2 -ErrorAction SilentlyContinue
Write-Host "$($results.Count - $failed.Count) of $($results.Count) passed; report: $(Join-Path $work 'installer-test.json')"
if ($failed) { exit 1 }

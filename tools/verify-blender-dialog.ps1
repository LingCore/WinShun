param(
    [string]$BlenderPath = 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe',
    [string]$BuildDir = '',
    [string]$OutputDir = '',
    [ValidateSet('en_US', 'zh_HANS')][string]$Language = 'en_US',
    [switch]$RuntimeOnly
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$BuildDir) { $BuildDir = Join-Path $repo 'build/release' }
if (!$OutputDir) { $OutputDir = Join-Path $repo ('build/blender-dialog-' + [guid]::NewGuid().ToString('N')) }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ((Test-Path -LiteralPath $OutputDir) -and (Get-ChildItem -LiteralPath $OutputDir -Force | Select-Object -First 1)) {
    throw 'OutputDir must be empty; existing results are preserved.'
}
foreach ($binary in @($BlenderPath, "$BuildDir/tst_filedialog.exe", "$BuildDir/WinShun.exe")) {
    if (!(Test-Path -LiteralPath $binary)) { throw "Build/install prerequisite missing: $binary" }
}
$qtSetting = Get-Content "$BuildDir/CMakeCache.txt" | Where-Object { $_.StartsWith('Qt6_DIR:PATH=') } | Select-Object -First 1
if (!$qtSetting) { throw 'Qt6_DIR is missing from this build cache.' }
$qtBin = [IO.Path]::GetFullPath((Join-Path $qtSetting.Split('=', 2)[1] '../../../bin'))
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$blenderProfile = Join-Path $OutputDir 'blender-user'
foreach ($folder in @($blenderProfile, "$blenderProfile/config", "$blenderProfile/scripts", "$blenderProfile/datafiles", "$blenderProfile/extensions", "$OutputDir/blender-temp")) {
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
}
$environment = @{
    BLENDER_USER_RESOURCES=$blenderProfile; BLENDER_USER_CONFIG="$blenderProfile/config";
    BLENDER_USER_SCRIPTS="$blenderProfile/scripts"; BLENDER_USER_DATAFILES="$blenderProfile/datafiles";
    BLENDER_USER_EXTENSIONS="$blenderProfile/extensions"; TEMP="$OutputDir/blender-temp";
    TMP="$OutputDir/blender-temp"; TMPDIR="$OutputDir/blender-temp";
    WINSHUN_TEST_BLENDER_LANGUAGE=$Language; WINSHUN_TEST_BLENDER_OUTPUT=$OutputDir
}
$saved = @{}
$fixture = $null
try {
    foreach ($key in $environment.Keys) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
        [Environment]::SetEnvironmentVariable($key, $environment[$key], 'Process')
    }
    $script = Join-Path $repo 'tests/blender/file_dialog_fixture.py'
    $fixture = Start-Process -FilePath $BlenderPath -ArgumentList @('--factory-startup', '--disable-autoexec', '--window-geometry', '50', '50', '900', '650', '--python', ('"'+$script+'"')) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$OutputDir/blender-stdout.txt" -RedirectStandardError "$OutputDir/blender-stderr.txt"
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
}
$savedPath = $env:PATH
$testEnvironment = @('WINSHUN_TEST_BLENDER_PID', 'WINSHUN_TEST_BLENDER_STATE')
$savedTest = @{}
function Read-Fixture {
    try { return Get-Content -LiteralPath "$OutputDir/blender-state.json" -Raw | ConvertFrom-Json } catch { return $null }
}
function Wait-FileView([int]$AfterSequence = -1) {
    $waiting = [Diagnostics.Stopwatch]::StartNew()
    while ($waiting.Elapsed.TotalSeconds -lt 30) {
        if ($fixture.HasExited) { throw 'Owned Blender fixture exited; inspect blender-stderr.txt.' }
        $state = Read-Fixture
        if ($state -and $state.pid -eq $fixture.Id -and $state.extension_listening -and $state.sequence -gt $AfterSequence) {
            $view = $state.windows | Where-Object { $_.visible -and $_.owner -ne 0 -and $_.token -ne 0 -and $_.class -eq 'GHOST_WindowClass' } | Select-Object -First 1
            if ($view) { return $view }
        }
        Start-Sleep -Milliseconds 100
    }
    throw 'Owned Blender file browser did not become ready.'
}
try {
    $null = Wait-FileView
    $env:PATH = $qtBin + ';' + $env:PATH
    foreach ($key in $testEnvironment) { $savedTest[$key] = [Environment]::GetEnvironmentVariable($key, 'Process') }
    $env:WINSHUN_TEST_BLENDER_PID = [string]$fixture.Id
    $env:WINSHUN_TEST_BLENDER_STATE = "$OutputDir/blender-state.json"
    $afterSequence = -1
    if (!$RuntimeOnly) {
        Push-Location $OutputDir
        try {
            & "$BuildDir/tst_filedialog.exe" realBlenderFileView -o filedialog.txt,txt
            if ($LASTEXITCODE -ne 0) { throw "Native integration test failed: $OutputDir/filedialog.txt" }
        } finally { Pop-Location }
        $afterSequence = (Read-Fixture).sequence
        @{open=$true} | ConvertTo-Json | Set-Content -LiteralPath "$OutputDir/blender-command.json" -Encoding utf8
    }
    $view = Wait-FileView -AfterSequence $afterSequence
    $state = Read-Fixture
    $runtimeArgs = @('--probe-runtime', ('"'+$OutputDir+'/runtime"'), '--renderer', 'd3d11', '--dark', '--settle-ms', '700',
        '--file-dialog-window', [string]$view.hwnd, '--file-dialog-pid', [string]$fixture.Id,
        '--file-dialog-sample', ('"'+$state.sample+'"'), '--file-dialog-state', ('"'+$OutputDir+'/blender-state.json"'))
    $runtimeProcess = Start-Process -FilePath "$BuildDir/WinShun.exe" -ArgumentList $runtimeArgs -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput "$OutputDir/runtime-stdout.txt" -RedirectStandardError "$OutputDir/runtime-stderr.txt"
    if ($runtimeProcess.ExitCode -ne 0) { throw "Search-bar integration failed: $OutputDir/runtime/runtime.json" }
    if (!$RuntimeOnly) { Get-Content "$OutputDir/filedialog.txt" -Tail 3 }
    Get-Content "$OutputDir/runtime/runtime.json" -Raw
    Write-Output "Verified isolated Blender instance only. Results: $OutputDir"
} finally {
    $env:PATH = $savedPath
    foreach ($key in $savedTest.Keys) { [Environment]::SetEnvironmentVariable($key, $savedTest[$key], 'Process') }
    if ($fixture -and !$fixture.HasExited) {
        Set-Content -LiteralPath "$OutputDir/blender-stop" -Value 'owned fixture complete'
        if (!$fixture.WaitForExit(5000)) { $fixture.Kill() } # only this factory-settings test process
    }
}

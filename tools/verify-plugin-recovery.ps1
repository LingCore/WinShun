<#
.SYNOPSIS
    Verify plugin startup, upgrade recovery and install-and-enable with real local packages.
.EXAMPLE
    ./tools/verify-plugin-recovery.ps1 -AppDir F:\wsstage\WinShun -OutputDir F:\wsstage\issue138-check
.NOTES
    Requires a current deployed build with --probe-runtime. Creates a fresh output folder;
    never exits the user's WinShun or changes the system clipboard. LocalSend's receive
    setting is off. The runtime probe isolates HKCU and removes its temporary CNG identity.
#>
param(
    [Parameter(Mandatory)] [string] $AppDir,
    [Parameter(Mandatory)] [string] $OutputDir,
    [string] $LibraryPluginsDir = (Join-Path $PSScriptRoot '..\build\release\library-plugins'),
    [ValidateSet('core','clipboard','capture','capture-and-lan','upgrade-local-index','upgrade-offline','upgrade-offline-english',
        'upgrade-retry','upgrade-local-package','upgrade-partial','install-approve','install-decline')]
    [string[]] $Cases = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$AppDir = [IO.Path]::GetFullPath($AppDir)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$LibraryPluginsDir = [IO.Path]::GetFullPath($LibraryPluginsDir)
$exe = Join-Path $AppDir 'WinShun.exe'
if (-not (Test-Path -LiteralPath $exe) -or -not (Test-Path -LiteralPath (Join-Path $AppDir 'Qt6Core.dll'))) {
    throw 'AppDir must be a deployed WinShun folder with its Qt DLLs.'
}
if (Test-Path -LiteralPath $OutputDir) { throw 'OutputDir already exists; choose a fresh folder.' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WsRecoveryProbeKeys {
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool RegisterHotKey(IntPtr window, int id, uint modifiers, uint key);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool UnregisterHotKey(IntPtr window, int id);
}
'@
$keyId = 0x3A13
$testKey = 0
$testHotkey = ''
foreach ($number in 24..13) {
    $vk = [uint32](0x70 + $number - 1)
    if ([WsRecoveryProbeKeys]::RegisterHotKey([IntPtr]::Zero, $keyId, 7, $vk)) {
        [void][WsRecoveryProbeKeys]::UnregisterHotKey([IntPtr]::Zero, $keyId)
        $testKey = $vk
        $testHotkey = 'Ctrl+Alt+Shift+F' + $number
        break
    }
}
if (-not $testKey) { throw 'No free test hotkey among Ctrl+Alt+Shift+F13..F24.' }
function Test-ProbeKeyBusy {
    if ([WsRecoveryProbeKeys]::RegisterHotKey([IntPtr]::Zero, $keyId, 7, $testKey)) {
        [void][WsRecoveryProbeKeys]::UnregisterHotKey([IntPtr]::Zero, $keyId)
        return $false
    }
    if ([Runtime.InteropServices.Marshal]::GetLastWin32Error() -ne 1409) { throw 'Cannot inspect the test hotkey.' }
    return $true
}

$fixtures = Join-Path $OutputDir 'fixtures'
New-Item -ItemType Directory -Path $fixtures | Out-Null
foreach ($case in @('capture', 'both')) {
    $folder = Join-Path $fixtures $case
    New-Item -ItemType Directory -Path $folder | Out-Null
    Copy-Item -LiteralPath (Join-Path $LibraryPluginsDir 'winshun.capture') -Destination $folder -Recurse
    if ($case -eq 'both') {
        Copy-Item -LiteralPath (Join-Path $LibraryPluginsDir 'winshun.localsend') -Destination $folder -Recurse
    }
}
$packages = Join-Path $fixtures 'packages'
New-Item -ItemType Directory -Path $packages | Out-Null
$captureZip = Join-Path $packages 'capture.zip'
Compress-Archive -Path (Join-Path $fixtures 'capture\winshun.capture\*') -DestinationPath $captureZip
function Write-Index([string] $path, [string] $id, [string] $name, [string] $package) {
    $item = @{id=$id; name=$name; version='1.0.0'; url=[IO.Path]::GetFileName($package);
        sha256=(Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLower(); size=(Get-Item -LiteralPath $package).Length}
    @{schema=1; plugins=@($item)} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $path -Encoding utf8
}
$captureIndex = Join-Path $packages 'capture-index.json'
Write-Index $captureIndex 'winshun.capture' 'Screenshot and recording' $captureZip
$optionalDir = Join-Path $fixtures 'test.optional'
New-Item -ItemType Directory -Path $optionalDir | Out-Null
@{manifest=1; id='test.optional'; name='Optional feature test'; version='1.0.0'; permissions=@('files:read');
    commands=@(@{id='test'; keyword='optional'; title='Optional feature test'; run=@{url='https://example.invalid/?q=%s'}})} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $optionalDir 'plugin.json') -Encoding utf8
$optionalZip = Join-Path $packages 'optional.zip'
Compress-Archive -Path (Join-Path $optionalDir '*') -DestinationPath $optionalZip
$optionalIndex = Join-Path $packages 'optional-index.json'
Write-Index $optionalIndex 'test.optional' 'Optional feature test' $optionalZip

# A bound, non-listening loopback socket reliably refuses connections without
# reaching GitHub, the user's proxy, or an unrelated service on a fixed port.
$refused = [Net.Sockets.Socket]::new([Net.Sockets.AddressFamily]::InterNetwork,
    [Net.Sockets.SocketType]::Stream, [Net.Sockets.ProtocolType]::Tcp)
$refused.Bind([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 0))
$offlineIndex = 'http://127.0.0.1:' + $refused.LocalEndPoint.Port + '/index.json'
$reports = [Collections.Generic.List[object]]::new()
function Run-Case([string] $name, [string[]] $options, [string] $index, [int] $expectedWarnings, [bool] $captureExpected) {
    if ($Cases.Count -and $name -notin $Cases) { return $null }
    if (Test-ProbeKeyBusy) { throw 'The preceding probe did not release its hotkey.' }
    $out = Join-Path $OutputDir $name
    $previous = $env:WINSHUN_PLUGIN_INDEX
    $process = $null
    try {
        $env:WINSHUN_PLUGIN_INDEX = $index
        $arguments = @('--probe-runtime', ('"'+$out+'"'), '--renderer', 'd3d11', '--probe-hotkey', $testHotkey) + $options
        $readyFile = Join-Path $out 'capture-ready.txt'
        if ($captureExpected) { $arguments += @('--probe-ready-file', ('"'+$readyFile+'"')) }
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
        $ready = $null
        while (-not $process.HasExited) {
            if ((Test-ProbeKeyBusy) -and $null -eq $ready) {
                $ready = $timer.ElapsedMilliseconds
                Set-Content -LiteralPath $readyFile -Value $ready -Encoding utf8
            }
            if ($timer.Elapsed.TotalSeconds -gt 45) { throw "Probe exceeded its deadline: $name (PID $($process.Id))." }
            Start-Sleep -Milliseconds 30
        }
        $process.WaitForExit()
        $code = $process.ExitCode
        $report = Get-Content -LiteralPath (Join-Path $out 'runtime.json') -Raw -Encoding utf8 | ConvertFrom-Json
        if ($code -ne $expectedWarnings -or $report.warnings -ne $expectedWarnings) {
            throw "$name returned $code, warnings $($report.warnings), expected $expectedWarnings. See its preview.log."
        }
        if ($captureExpected -and ($null -eq $ready -or $ready -ge 10000)) { throw "$name did not make its capture hotkey ready within 10 seconds." }
        if ((-not $captureExpected) -and $null -ne $ready) { throw "$name unexpectedly started capture." }
        if (Test-ProbeKeyBusy) { throw "$name left its capture hotkey registered." }
        $report | Add-Member -NotePropertyName case -NotePropertyValue $name
        $report | Add-Member -NotePropertyName hotkey_ready_ms -NotePropertyValue $ready
        $reports.Add($report)
        Write-Host "$name passed; private $([math]::Round($report.total_private_bytes/1MB,1)) MiB; hotkey $ready ms"
        return $report
    } finally {
        if ($null -ne $process -and -not $process.HasExited) {
            $process.Kill($true) # only the isolated child launched above and its descendants
            $process.WaitForExit()
        }
        $env:WINSHUN_PLUGIN_INDEX = $previous
    }
}
try {
    $null = Run-Case 'core' @() $captureIndex 0 $false
    $null = Run-Case 'clipboard' @('--with-clipboard') $captureIndex 0 $false
    $null = Run-Case 'capture' @('--with-clipboard','--runtime-plugins',('"'+(Join-Path $fixtures 'capture')+'"')) $captureIndex 0 $true
    $null = Run-Case 'capture-and-lan' @('--with-clipboard','--runtime-plugins',('"'+(Join-Path $fixtures 'both')+'"')) $captureIndex 0 $true
    $online = Run-Case 'upgrade-local-index' @('--legacy-capture','--runtime-ui') $captureIndex 0 $true
    if ($null -ne $online -and (-not $online.capture_installed -or $online.capture_hotkey -ne $testHotkey -or $online.recovery_after)) { throw 'Upgrade lost its settings or did not complete.' }
    $offline = Run-Case 'upgrade-offline' @('--legacy-capture','--runtime-ui','--settle-ms','5000') $offlineIndex 1 $false
    if ($null -ne $offline -and ($offline.capture_installed -or -not $offline.recovery_after -or $offline.recovering)) { throw 'Offline recovery state is wrong.' }
    $offlineEnglish = Run-Case 'upgrade-offline-english' @('--legacy-capture','--runtime-ui','--english','--settle-ms','5000') $offlineIndex 1 $false
    if ($null -ne $offlineEnglish -and ($offlineEnglish.capture_installed -or $offlineEnglish.recovering -or
        -not $offlineEnglish.recovery_after.Contains('Retry') -or $offlineEnglish.recovery_after -match '[\u4e00-\u9fff]')) {
        throw 'The production English recovery message is missing or untranslated.'
    }
    $retry = Run-Case 'upgrade-retry' @('--legacy-capture','--runtime-ui','--settle-ms','5000','--retry-index',('"'+$captureIndex+'"')) $offlineIndex 1 $true
    if ($null -ne $retry -and (-not $retry.recovery_before -or $retry.recovery_after -or -not $retry.capture_installed)) { throw 'The real retry control did not complete recovery.' }
    $local = Run-Case 'upgrade-local-package' @('--legacy-capture','--runtime-ui','--settle-ms','5000','--local-package',('"'+$captureZip+'"')) $offlineIndex 1 $true
    if ($null -ne $local -and (-not $local.recovery_before -or $local.recovery_after -or -not $local.capture_installed)) { throw 'Local package recovery did not complete.' }
    $partial = Run-Case 'upgrade-partial' @('--legacy-capture','--legacy-semantic','--runtime-ui') $captureIndex 1 $true
    if ($null -ne $partial -and (-not $partial.capture_installed -or -not $partial.recovery_after)) { throw 'Partial recovery lost its failure feedback.' }
    foreach ($approve in @($true, $false)) {
        $options = @('--runtime-ui','--install-gallery','test.optional')
        if ($approve) { $options += '--approve-install' }
        $name = if ($approve) { 'install-approve' } else { 'install-decline' }
        $install = Run-Case $name $options $optionalIndex 0 $false
        if ($null -ne $install -and ($install.permission_requests -ne 1 -or $install.enabled_before_consent -or $install.enabled_after_consent -ne $approve)) {
            throw 'Install-and-enable did not preserve the permission decision.'
        }
    }
    $reports | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDir 'results.json') -Encoding utf8
} finally {
    $refused.Dispose()
}

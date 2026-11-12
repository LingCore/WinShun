<#
.SYNOPSIS
    Verify first-run capture installation, offline retry and layout.
.NOTES
    Uses the real application and capture binaries with local package/index
    fixtures. The probe owns its profile, HKCU redirection and hotkeys. It never
    changes the clipboard or stops an existing WinShun process. No build or
    deployment is performed here.
#>
param(
    [Parameter(Mandatory)] [string] $AppExe,
    [string] $LibraryPluginsDir = (Join-Path $PSScriptRoot '..\build\release\library-plugins'),
    [string] $QtBin = 'C:\Qt\6.12.0\msvc2022_64\bin',
    [Parameter(Mandatory)] [string] $OutputDir,
    [ValidateSet('capture','offline','retry','incompatible','english','english-capture')]
    [string[]] $Cases = @('capture','offline','retry','incompatible','english','english-capture')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$AppExe = [IO.Path]::GetFullPath($AppExe)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$LibraryPluginsDir = [IO.Path]::GetFullPath($LibraryPluginsDir)
if (-not (Test-Path -LiteralPath $AppExe -PathType Leaf)) { throw 'Build WinShun first.' }
if (-not (Test-Path -LiteralPath (Join-Path (Split-Path $AppExe) 'Qt6Core.dll') -PathType Leaf)) {
    throw 'AppExe must be in a deployed folder with its Qt DLLs; the capture starter deliberately does not search PATH.'
}
if (-not (Test-Path -LiteralPath (Join-Path (Split-Path $AppExe) 'fonts/AlibabaPuHuiTi-3-55-Regular.ttf') -PathType Leaf)) {
    throw 'Deploy the bundled fonts too, so layout previews use the same font as the packaged app.'
}
if (Test-Path -LiteralPath $OutputDir) { throw 'OutputDir already exists; choose a fresh folder.' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$fixtures = Join-Path $OutputDir 'fixtures'
New-Item -ItemType Directory -Path $fixtures | Out-Null
Copy-Item -LiteralPath (Join-Path $LibraryPluginsDir 'winshun.capture') -Destination $fixtures -Recurse
$plugin = Join-Path $fixtures 'winshun.capture'
$manifest = Get-Content -LiteralPath (Join-Path $plugin 'plugin.json') -Raw | ConvertFrom-Json
$package = Join-Path $fixtures 'capture.zip'
Compress-Archive -Path (Join-Path $plugin '*') -DestinationPath $package
$item = @{
    id = 'winshun.capture'; name = 'Screen capture'; version = $manifest.version
    url = 'capture.zip'; sha256 = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
    size = (Get-Item -LiteralPath $package).Length
}
$index = Join-Path $fixtures 'index.json'
@{schema=1; plugins=@($item)} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $index -Encoding utf8
$item.minWinShun = '999.0.0'
$incompatible = Join-Path $fixtures 'incompatible.json'
@{schema=1; plugins=@($item)} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $incompatible -Encoding utf8

$previousPath = $env:PATH
$previousIndex = $env:WINSHUN_PLUGIN_INDEX
$previousRetry = $env:WINSHUN_ONBOARDING_RETRY_INDEX
$summary = @()
try {
    $env:PATH = $QtBin + ';' + $env:PATH
    $env:WINSHUN_ONBOARDING_RETRY_INDEX = $index
    foreach ($case in $Cases) {
        $caseDir = Join-Path $OutputDir $case
        $english = $case.StartsWith('english')
        $action = if ($case -eq 'english') { 'apply' } elseif ($case -eq 'english-capture') { 'capture' } else { $case }
        $env:WINSHUN_PLUGIN_INDEX = if ($case -in @('offline','retry')) { 'http://127.0.0.1:1/unreachable.json' }
            elseif ($case -eq 'incompatible') { $incompatible } else { $index }
        $arguments = @('--probe-runtime', ('"' + $caseDir + '"'), '--onboarding-probe', $action, '--renderer', 'd3d11')
        if ($english) { $arguments += @('--english', '--dark', '--scale', '1.5') }
        $process = Start-Process -FilePath $AppExe -ArgumentList $arguments -WorkingDirectory (Split-Path $AppExe) -WindowStyle Hidden -PassThru
        if (-not $process.WaitForExit(90000)) {
            $process.Kill() # only this probe process, created just above
            throw "Onboarding probe timed out: $case"
        }
        $resultFile = Join-Path $caseDir 'onboarding.json'
        if (-not (Test-Path -LiteralPath $resultFile -PathType Leaf)) {
            throw "Onboarding probe produced no report: $case; exit=$($process.ExitCode); see $caseDir/preview.log"
        }
        $result = Get-Content -LiteralPath $resultFile -Raw | ConvertFrom-Json
        $summary += @{case=$case; exit=$process.ExitCode; passed=$result.passed; warnings=$result.warnings; unexpected=$result.unexpected_warnings; report=$resultFile}
        $summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDir 'summary.json') -Encoding utf8
        Write-Output "$case`: exit=$($process.ExitCode), passed=$($result.passed), unexpected warnings=$($result.unexpected_warnings)"
        if ($process.ExitCode -ne 0 -or -not $result.passed) { throw "Onboarding probe failed: $case; see $resultFile" }
    }
}
finally {
    $env:PATH = $previousPath
    $env:WINSHUN_PLUGIN_INDEX = $previousIndex
    $env:WINSHUN_ONBOARDING_RETRY_INDEX = $previousRetry
}

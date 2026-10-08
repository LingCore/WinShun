<#
.SYNOPSIS
    Package a release: build and deploy (build.ps1 -Deploy), then in dist    the installer WinShun-<version>-x64-setup.exe (installer\WinShun.iss,
    Inno Setup 6.5 or later), the portable WinShun-<version>-x64.zip and
    SHA256SUMS.txt. With -Publish, also the GitHub release v<version> (gh
    must be logged in).

    The version comes from project(VERSION) in CMakeLists.txt; the release
    notes from docs/release-notes/v<version>.md. Their opening paragraph and
    "## 新功能 · What's new" list are what the app's update dialog shows
    (src/core/Release.cpp), so keep that format.

.EXAMPLE
    ./scripts/release.ps1             # setup.exe, zip and SHA256SUMS.txt in dist    ./scripts/release.ps1 -Publish    # and the GitHub release
#>
param(
    [switch] $Publish,
    [string] $QtDir = $env:QTDIR,
    [string] $Iscc = $env:ISCC # Inno Setup's compiler; found under the usual places otherwise
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(WinShun\s+VERSION\s+(\d+\.\d+\.\d+)') { throw 'No project(VERSION) in CMakeLists.txt' }
$version = $Matches[1]
$tag = "v$version"
$notes = Join-Path $root "docs\release-notes\$tag.md"
if ($Publish -and -not (Test-Path $notes)) { throw "Write the release notes first: $notes" }
Write-Host "WinShun $version"

# build.ps1 -Deploy replaces dist\WinShun, which a running copy holds on to.
$running = Get-Process WinShun -ErrorAction SilentlyContinue
if ($running) { throw 'Quit WinShun first (tray menu > 退出): dist\WinShun is about to be replaced.' }

# Inno Setup: a per-user install of a recent version first (the one under
# Program Files may be too old for the Windows 11 wizard style).
if (-not $Iscc) {
    $Iscc = @(Get-ChildItem "$env:LOCALAPPDATA\Programs\Inno Setup*\ISCC.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending) + @(Get-Item "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "$env:ProgramFiles\Inno Setup 6\ISCC.exe" -ErrorAction SilentlyContinue) |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Iscc -or -not (Test-Path $Iscc)) { throw 'Inno Setup 6.5 or later is needed (winget install JRSoftware.InnoSetup), or pass -Iscc' }

$buildArgs = @{ Deploy = $true }
if ($QtDir) { $buildArgs.QtDir = $QtDir }
& (Join-Path $PSScriptRoot 'build.ps1') @buildArgs

$dist = Join-Path $root 'dist'
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $dist 'WinShun\LICENSE.txt')
$zip = Join-Path $dist "WinShun-$version-x64.zip"
if (Test-Path $zip) { Remove-Item $zip }
# One folder, WinShun\, inside: unzipping over an old copy replaces it in place.
Compress-Archive -Path (Join-Path $dist 'WinShun') -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Packaged $zip"

$setup = Join-Path $dist "WinShun-$version-x64-setup.exe"
& $Iscc /Q "/DAppVersion=$version" "/DSourceDir=$(Join-Path $dist 'WinShun')" "/DOutputDir=$dist" (Join-Path $root 'installer\WinShun.iss')
if ($LASTEXITCODE -or -not (Test-Path $setup)) { throw 'Inno Setup failed' }
Write-Host "Packaged $setup"

$sums = Join-Path $dist 'SHA256SUMS.txt'
$lines = foreach ($file in $setup, $zip) { "$((Get-FileHash $file -Algorithm SHA256).Hash.ToLower())  $(Split-Path $file -Leaf)" }
Set-Content $sums ($lines -join "`n") -Encoding ascii -NoNewline
Get-Content $sums

if ($Publish) {
    gh release create $tag $setup $zip $sums --repo LingCore/WinShun --title "Win顺 $version" --notes-file $notes --target main
    if ($LASTEXITCODE) { throw 'gh release create failed' }
    Write-Host "Published $tag"
}

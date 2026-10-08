<#
.SYNOPSIS
    Package a release: build and deploy (build.ps1 -Deploy), zip dist\WinShun
    into dist\WinShun-<version>-x64.zip with a .sha256 next to it, and with
    -Publish create the GitHub release v<version> (gh must be logged in).

    The version comes from project(VERSION) in CMakeLists.txt; the release
    notes from docs/release-notes/v<version>.md. Their opening paragraph and
    "## 新功能 · What's new" list are what the app's update dialog shows
    (src/core/Release.cpp), so keep that format.

.EXAMPLE
    ./scripts/release.ps1             # dist\WinShun-0.2.0-x64.zip + .sha256
    ./scripts/release.ps1 -Publish    # and the GitHub release
#>
param(
    [switch] $Publish,
    [string] $QtDir = $env:QTDIR
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

$buildArgs = @{ Deploy = $true }
if ($QtDir) { $buildArgs.QtDir = $QtDir }
& (Join-Path $PSScriptRoot 'build.ps1') @buildArgs

$dist = Join-Path $root 'dist'
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $dist 'WinShun\LICENSE.txt')
$zip = Join-Path $dist "WinShun-$version-x64.zip"
if (Test-Path $zip) { Remove-Item $zip }
# One folder, WinShun\, inside: unzipping over an old copy replaces it in place.
Compress-Archive -Path (Join-Path $dist 'WinShun') -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Set-Content "$zip.sha256" "$hash  $(Split-Path $zip -Leaf)" -Encoding ascii -NoNewline
Write-Host "Packaged $zip"
Write-Host "SHA-256  $hash"

if ($Publish) {
    gh release create $tag $zip "$zip.sha256" --repo LingCore/WinShun --title "Win顺 $version" --notes-file $notes
    if ($LASTEXITCODE) { throw 'gh release create failed' }
    Write-Host "Published $tag"
}

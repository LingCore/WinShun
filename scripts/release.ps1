<#
.SYNOPSIS
    Package a release: build and deploy (build.ps1 -Deploy), then the
    installer dist\WinShun-<version>-x64-setup.exe (installer\WinShun.iss,
    Inno Setup 6.5 or later), the portable dist\WinShun-<version>-x64-portable.zip
    (the same folder), a complete offline setup, the shared enhanced OCR ZIP
    and dist\SHA256SUMS.txt. With -Publish, also the GitHub
    release v<version> (gh must be logged in).

    The version comes from project(VERSION) in CMakeLists.txt; the release
    notes from docs/release-notes/v<version>.md. Their opening paragraph and
    "## 新功能 · What's new" list are what the app's update dialog shows
    (src/core/Release.cpp), so keep that format.

.EXAMPLE
    ./scripts/release.ps1             # setup.exe and SHA256SUMS.txt in dist\
    ./scripts/release.ps1 -Publish    # and the GitHub release
    ./scripts/release.ps1 -OutputDir dist\release-preview # leave other deployed copies running
    ./scripts/release.ps1 -BuildDir build\release-preview -OutputDir dist\release-preview
#>
param(
    [switch] $Publish,
    [string] $QtDir = $env:QTDIR,
    [string] $Iscc = $env:ISCC, # Inno Setup's compiler; found under the usual places otherwise
    [string] $OutputDir, # optional isolated deployment and package directory
    [string] $BuildDir, # optional isolated CMake binary directory
    [string] $Tests # ctest -R regex of the tests this release's changes touch; none run otherwise
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = if ($OutputDir) { [IO.Path]::GetFullPath($OutputDir) } else { Join-Path $root 'dist' }
$deployment = Join-Path $dist 'WinShun'
$binary = if ($BuildDir) { [IO.Path]::GetFullPath($BuildDir) } else { Join-Path $root 'build\release' }

$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(WinShun\s+VERSION\s+(\d+\.\d+\.\d+)') { throw 'No project(VERSION) in CMakeLists.txt' }
$version = $Matches[1]
$tag = "v$version"
$notes = Join-Path $root "docs\release-notes\$tag.md"
if ($Publish -and -not (Test-Path $notes)) { throw "Write the release notes first: $notes" }
Write-Host "WinShun $version"

# build.ps1 -Deploy replaces dist\WinShun, which a running copy holds on to.
$running = Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -and [IO.Path]::GetFullPath($_.Path).StartsWith($deployment + '\', [StringComparison]::OrdinalIgnoreCase)
}
if ($running) { throw "The copy in $deployment is running; leave it untouched and choose another -OutputDir." }
if (Test-Path -LiteralPath (Join-Path $deployment 'data')) { throw "User data exists in $deployment; choose a fresh -OutputDir." }

# Inno Setup: a per-user install of a recent version first (the one under
# Program Files may be too old for the Windows 11 wizard style).
if (-not $Iscc) {
    $Iscc = @(Get-ChildItem "$env:LOCALAPPDATA\Programs\Inno Setup*\ISCC.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending) + @(Get-Item "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "$env:ProgramFiles\Inno Setup 6\ISCC.exe" -ErrorAction SilentlyContinue) |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Iscc -or -not (Test-Path $Iscc)) { throw 'Inno Setup 6.5 or later is needed (winget install JRSoftware.InnoSetup), or pass -Iscc' }

$buildArgs = @{ DeployDir = $deployment }
if ($BuildDir) { $buildArgs.BuildDir = $binary }
if ($Tests) { $buildArgs.Tests = $Tests }
if ($PSBoundParameters.ContainsKey('QtDir')) { $buildArgs.QtDir = $QtDir } # else build.ps1 looks at QTDIR itself
& (Join-Path $PSScriptRoot 'build.ps1') @buildArgs

Copy-Item (Join-Path $root 'LICENSE') (Join-Path $dist 'WinShun\LICENSE.txt')

$setup = Join-Path $dist "WinShun-$version-x64-setup.exe"
& $Iscc /Q "/DAppVersion=$version" "/DSourceDir=$(Join-Path $dist 'WinShun')" "/DOutputDir=$dist" (Join-Path $root 'installer\WinShun.iss')
if ($LASTEXITCODE -or -not (Test-Path $setup)) { throw 'Inno Setup failed' }
Write-Host "Packaged $setup"

# The portable copy (GitHub #13): the same folder, unzipped anywhere, with a
# "data" folder beside the program, where it then keeps its settings and
# index (GitHub #19, Settings::portableFolder) instead of %APPDATA% and
# %LOCALAPPDATA%. Staged, so the deployed folder stays an installed one.
$portable = Join-Path $dist "WinShun-$version-x64-portable.zip"
Remove-Item $portable -ErrorAction SilentlyContinue
$stageRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$stage = [IO.Path]::GetFullPath((Join-Path $stageRoot ("WinShun-portable-$version-" + [guid]::NewGuid().ToString('N'))))
if (-not $stage.StartsWith($stageRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Portable staging path escaped the temporary directory.' }
New-Item -ItemType Directory $stage | Out-Null
try {
    Copy-Item (Join-Path $dist 'WinShun') $stage -Recurse
    New-Item -ItemType Directory (Join-Path $stage 'WinShun\data') | Out-Null
    Copy-Item (Join-Path $root 'installer\portable-data-README.txt') (Join-Path $stage 'WinShun\data\README.txt')
    Compress-Archive -Path (Join-Path $stage 'WinShun') -DestinationPath $portable -CompressionLevel Optimal
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
Write-Host "Packaged $portable"

# The official plugins that are not in the setup (GitHub #138): one
# <id>-<version>.wsplugin each from the build's library-plugins\<id> (the
# package's files at the top of the zip, as install::inspect reads them),
# for a release of the plugin library (LingCore/WinShun-plugins). Not part
# of this release's assets: the library lists each on its own.
$libraryOut = Join-Path $dist 'library-plugins'
New-Item -ItemType Directory -Force $libraryOut | Out-Null
foreach ($folder in Get-ChildItem (Join-Path $binary 'library-plugins') -Directory) {
    $manifest = Get-Content (Join-Path $folder.FullName 'plugin.json') -Raw -Encoding utf8 | ConvertFrom-Json
    if ($manifest.id -ne $folder.Name) { throw "library-plugins\$($folder.Name): its manifest says $($manifest.id)" }
    $package = Join-Path $libraryOut "$($manifest.id)-$($manifest.version).wsplugin"
    Remove-Item $package -ErrorAction SilentlyContinue
    $zip = [IO.Path]::ChangeExtension($package, '.zip') # Compress-Archive will not write another extension
    Compress-Archive -Path (Join-Path $folder.FullName '*') -DestinationPath $zip -CompressionLevel Optimal
    Move-Item $zip $package
    Write-Host "Packaged $package"
}

# The enhanced OCR component is shared by the clipboard and capture plugin.
# It is an asset of the same release, with pinned per-file hashes in the app.
$ocrManifest = Get-Content (Join-Path $root 'resources\ocr\package.json') -Raw | ConvertFrom-Json
$ocrPackage = Join-Path $dist $ocrManifest.name
& python -X utf8 (Join-Path $root 'tools\package_ocr.py') --build-dir $binary --output $ocrPackage
if ($LASTEXITCODE) { throw 'Enhanced OCR packaging failed' }

# A complete offline installer, using the same verified binaries. No second build.
$offlineSetup = Join-Path $dist "WinShun-$version-x64-offline-setup.exe"
$offlineStage = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('WinShun-offline-'+[guid]::NewGuid().ToString('N'))))
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
if (-not $offlineStage.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid offline staging path' }
try {
    Copy-Item -LiteralPath $deployment -Destination $offlineStage -Recurse
    & python -X utf8 (Join-Path $root 'tools\package_ocr.py') --build-dir $binary --output $ocrPackage --expand-to $offlineStage
    if ($LASTEXITCODE) { throw 'Offline OCR deployment failed' }
    & $Iscc /Q "/DAppVersion=$version" "/DSourceDir=$offlineStage" "/DOutputDir=$dist" '/DPackageSuffix=-offline' (Join-Path $root 'installer\WinShun.iss')
    if ($LASTEXITCODE -or -not (Test-Path -LiteralPath $offlineSetup)) { throw 'Offline Inno Setup failed' }
} finally {
    if (Test-Path -LiteralPath $offlineStage) { Remove-Item -LiteralPath $offlineStage -Recurse -Force }
}

$sums = Join-Path $dist 'SHA256SUMS.txt'
$lines = foreach ($file in $setup, $portable, $offlineSetup, $ocrPackage) {
    "$((Get-FileHash $file -Algorithm SHA256).Hash.ToLower())  $(Split-Path $file -Leaf)"
}
Set-Content $sums ($lines -join "`n") -Encoding ascii -NoNewline
Get-Content $sums

if ($Publish) {
    gh release create $tag $setup $portable $offlineSetup $ocrPackage $sums --repo LingCore/WinShun --title "Win顺 $version" --notes-file $notes --target main
    if ($LASTEXITCODE) { throw 'gh release create failed' }
    Write-Host "Published $tag"

    # The Scoop bucket (bucket\winshun.json, `scoop bucket add winshun
    # https://github.com/LingCore/WinShun`): the new version, the portable
    # zip's URL and the SHA-256 computed above, only once the release is out
    # (a manifest ahead of its zip would break `scoop update`). Only the three
    # values change, in place (no JSON round trip: formatting and encoding stay).
    $manifestPath = Join-Path $root 'bucket\winshun.json'
    $manifest = [IO.File]::ReadAllText($manifestPath)
    $zipHash = (Get-FileHash $portable -Algorithm SHA256).Hash.ToLower()
    $zipUrl = "https://github.com/LingCore/WinShun/releases/download/$tag/$(Split-Path $portable -Leaf)"
    $manifest = [regex]::Replace($manifest, '(?m)^(    "version": ")[^"]*(",)', { param($m) $m.Groups[1].Value + $version + $m.Groups[2].Value })
    $manifest = [regex]::Replace($manifest, '(?m)^(    "url": ")[^"]*(",)', { param($m) $m.Groups[1].Value + $zipUrl + $m.Groups[2].Value })
    $manifest = [regex]::Replace($manifest, '(?m)^(    "hash": ")[0-9a-f]{64}(",)', { param($m) $m.Groups[1].Value + $zipHash + $m.Groups[2].Value })
    [IO.File]::WriteAllText($manifestPath, $manifest, (New-Object Text.UTF8Encoding($false)))
    $parsed = $manifest | ConvertFrom-Json
    if ($parsed.version -ne $version -or $parsed.url -ne $zipUrl -or $parsed.hash -ne $zipHash) { throw 'bucket\winshun.json was not updated as expected' }
    # The bucket is main's: never commit it onto, or push, another branch.
    $branch = (git -C $root rev-parse --abbrev-ref HEAD).Trim()
    if ($branch -ne 'main') { throw "The Scoop manifest is committed on main; this is $branch. The release $tag is out: update bucket\winshun.json by hand." }
    # Only that file: the working tree may hold other changes.
    git -C $root add -- bucket/winshun.json
    git -C $root commit --only -m "Scoop: winshun $version" -- bucket/winshun.json
    if ($LASTEXITCODE) { throw 'committing the Scoop manifest failed' }
    git -C $root push origin HEAD:main
    if ($LASTEXITCODE) { throw "pushing the Scoop manifest failed: run git push origin HEAD:main, the release $tag is out" }
    Write-Host "Scoop manifest: $version"
}

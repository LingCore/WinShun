<#
.SYNOPSIS
    Builds and packs WinShunOcrTest.exe, the clipboard OCR test a user runs
    (tools/ocrtest/README.md), into dist\ocrtest\WinShunOcrTest.zip.
.DESCRIPTION
    Qt and the C++ runtime go beside it, so it runs on a PC without either.
    Its symbols stay here (dist\ocrtest\symbols), for the dumps it brings back.
.EXAMPLE
    . .\scripts\devshell.ps1; .\scripts\ocrtest.ps1
#>
param(
    [string] $BuildDir = 'build\release',
    [string] $QtDir = 'C:\Qt\6.12.0\msvc2022_64',
    [string] $OutDir = 'dist\ocrtest'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$out = Join-Path $root $OutDir
$pack = Join-Path $out 'WinShunOcrTest'
$staging = Join-Path $out 'staging'

cmake --build $build --target WinShunOcrTest
if ($LASTEXITCODE) { throw 'build failed' }
try {
    Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $staging | Out-Null
    Copy-Item (Join-Path $build 'WinShunOcrTest.exe') $staging
    # Of the databases only SQLite. windeployqt insists on the platform plugin
    # for anything linking QtGui; the test runs a QCoreApplication and never
    # loads it, so it goes again afterwards.
    $log = & "$QtDir\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler `
        --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime `
        --skip-plugin-types qmltooling,generic,networkinformation,tls,iconengines,imageformats,styles `
        --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql `
        (Join-Path $staging 'WinShunOcrTest.exe') 2>&1
    if ($LASTEXITCODE) { $log | Select-Object -Last 10 | Write-Host; throw 'windeployqt failed' }
    Remove-Item (Join-Path $staging 'platforms') -Recurse -Force -ErrorAction SilentlyContinue
    # The clipboard keeps PNG, GIF and JPEG; PNG is built into QtGui.
    $imageFormats = Join-Path $staging 'imageformats'
    New-Item -ItemType Directory -Force $imageFormats | Out-Null
    foreach ($codec in @('qgif', 'qjpeg')) {
        Copy-Item "$QtDir\plugins\imageformats\$codec.dll" $imageFormats
    }
    # The C++ runtime beside it, as scripts/build.ps1 ships it with WinShun.
    $crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
        Where-Object { Test-Path (Join-Path $_.FullName 'vcruntime140.dll') } |
        Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
    if (-not $crt) { throw "No x64 VC++ runtime under $env:VCToolsRedistDir" }
    Get-ChildItem $crt -Filter '*.dll' | Where-Object { $_.Name -match '^(msvcp140|vcruntime140)' } |
        ForEach-Object { Copy-Item $_.FullName $staging }
    Copy-Item (Join-Path $root 'tools\ocrtest\使用说明.txt') $staging
    Remove-Item $pack -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item $staging $pack
    $zip = Join-Path $out 'WinShunOcrTest.zip'
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path $pack -DestinationPath $zip
    $symbols = Join-Path $out 'symbols'
    New-Item -ItemType Directory -Force $symbols | Out-Null
    Copy-Item (Join-Path $build 'WinShunOcrTest.pdb') $symbols -Force
    Copy-Item (Join-Path $build 'WinShunOcrTest.exe') $symbols -Force
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
    Write-Host ("{0}  {1:N1} MB  SHA256 {2}" -f $zip, ((Get-Item $zip).Length / 1MB), $hash)
} finally {
    Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
}

<#
.SYNOPSIS
    Configure, build, test and (optionally) package WinShun.

.EXAMPLE
    ./scripts/build.ps1                      # Release build + tests
    ./scripts/build.ps1 -Config Debug
    ./scripts/build.ps1 -Deploy              # also produce dist/WinShun with all Qt DLLs
    ./scripts/build.ps1 -QtDir C:\Qt\6.12.0\msvc2022_64
#>
param(
    [ValidateSet('Debug', 'Release')] [string] $Config = 'Release',
    [string] $QtDir = $env:QTDIR,
    [switch] $Deploy,
    [switch] $SkipTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# Qt: use -QtDir / %QTDIR%, otherwise the newest MSVC kit under C:\Qt. A
# %QTDIR% without Qt in it (left over from a Qt since removed, in a program
# started before it was changed) is passed over; a -QtDir given is not.
if ($QtDir -and -not $PSBoundParameters.ContainsKey('QtDir') -and -not (Test-Path "$QtDir\bin\qmake.exe")) {
    Write-Host "No Qt in QTDIR ($QtDir): looking under C:\Qt"
    $QtDir = $null
}
if (-not $QtDir) {
    $QtDir = Get-ChildItem 'C:\Qt' -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'msvc2022_64' } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $QtDir -or -not (Test-Path "$QtDir\bin\qmake.exe")) { throw "Qt not found. Pass -QtDir C:\Qt\<version>\msvc2022_64" }
$env:QTDIR = $QtDir
Write-Host "Qt:  $QtDir"

# MSVC developer environment (cl, rc, link on PATH).
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ workload was not found.' }
    $env:PATH = "$(Split-Path $vswhere);$env:PATH" # VsDevCmd calls vswhere itself
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

$preset = $Config.ToLower()
Push-Location $root
try {
    cmake --preset $preset
    if ($LASTEXITCODE) { throw 'configure failed' }
    cmake --build --preset $preset
    if ($LASTEXITCODE) { throw 'build failed' }
    if (-not $SkipTests) {
        ctest --preset $preset
        if ($LASTEXITCODE) { throw 'tests failed' }
    }
    if ($Deploy) {
        $dist = Join-Path $root 'dist\WinShun'
        if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
        New-Item -ItemType Directory -Force $dist | Out-Null
        Copy-Item "$root\build\$preset\WinShun.exe" $dist
        Copy-Item "$root\build\$preset\fonts" $dist -Recurse # UI font (see main.cpp)
        # The button users pin to the taskbar (src/stub).
        Copy-Item "$root\build\$preset\WinShunSearch.exe" $dist
        # Documents are read by WinShunExtract.exe (src/extract), PDFs with
        # PDFium; their licenses go along.
        Copy-Item "$root\build\$preset\WinShunExtract.exe", "$root\build\$preset\pdfium.dll" $dist
        $pdfium = Get-ChildItem "$root\build\$preset\_deps" -Directory -Filter 'pdfium-*' | Sort-Object Name | Select-Object -Last 1
        New-Item -ItemType Directory -Force "$dist\licenses\pdfium" | Out-Null
        Copy-Item "$($pdfium.FullName)\LICENSE" "$dist\licenses\pdfium\LICENSE.txt"
        Copy-Item "$($pdfium.FullName)\licenses\*" "$dist\licenses\pdfium"
        Copy-Item "$root\third_party\miniz\LICENSE" "$dist\licenses\miniz.txt"
        # Only what the app uses: no shader compilers (software renderer), no
        # QML debugger, network, TLS or SVG plugins, and of the database
        # drivers only SQLite (the clipboard history).
        & "$QtDir\bin\windeployqt.exe" --qmldir "$root\src\app\qml" --no-translations --no-system-d3d-compiler `
            --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime `
            --skip-plugin-types qmltooling,generic,networkinformation,tls,iconengines,imageformats `
            --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql `
            "--$preset" "$dist\WinShun.exe"
        if ($LASTEXITCODE) { throw 'windeployqt failed' }
        # App-local C++ runtime, so users need no VC++ redistributable installed.
        $crt = Join-Path $env:VCToolsRedistDir 'x64\Microsoft.VC143.CRT'
        foreach ($dll in 'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
            Copy-Item (Join-Path $crt $dll) $dist
        }
        Write-Host "Deployed to $dist"
    }
} finally {
    Pop-Location
}

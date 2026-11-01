<#
.SYNOPSIS
    Configure, build, test and (optionally) package WinShun.

.EXAMPLE
    ./scripts/build.ps1                      # Release build + tests
    ./scripts/build.ps1 -Config Debug
    ./scripts/build.ps1 -Deploy              # also produce dist/WinShun with all Qt DLLs
    ./scripts/build.ps1 -DeployDir F:\wsstage\WinShun   # the same, into that folder (dist\WinShun untouched)
    ./scripts/build.ps1 -BuildDir F:\wsstage\build -DeployDir F:\wsstage\WinShun
    ./scripts/build.ps1 -QtDir C:\Qt\6.12.0\msvc2022_64
#>
param(
    [ValidateSet('Debug', 'Release')] [string] $Config = 'Release',
    [string] $QtDir = $env:QTDIR,
    [switch] $Deploy,
    # Where to deploy (implies -Deploy); default dist\WinShun. For a staged
    # copy to compare with, or test from, while the deployed one is running.
    [string] $DeployDir,
    [string] $BuildDir, # optional isolated CMake binary directory; defaults to build/<config>
    [switch] $SkipTests,
    [switch] $IncludeOcr # full offline deployment; the standard package downloads enhanced OCR on demand
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($DeployDir) { $Deploy = $true }

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
$binary = if ($BuildDir) { [IO.Path]::GetFullPath($BuildDir) } else { Join-Path $root "build\$preset" }
$dist = if ($DeployDir) { [IO.Path]::GetFullPath($DeployDir) } else { Join-Path $root 'dist\WinShun' }
function Assert-DeploymentTarget {
    $target = $dist.TrimEnd('\', '/')
    foreach ($protected in @([IO.Path]::GetPathRoot($target), $root, $binary)) {
        $protected = $protected.TrimEnd('\', '/')
        if ($protected.Equals($target, [StringComparison]::OrdinalIgnoreCase) -or
            $protected.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Deployment would replace a drive, source or build directory: $dist"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $dist 'data')) { throw "User data exists in $dist; choose a fresh -DeployDir." }
    $running = Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and [IO.Path]::GetFullPath($_.Path).StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase)
    }
    if ($running) { throw "The copy in $dist is running; choose a fresh -DeployDir." }
}
if ($Deploy) { Assert-DeploymentTarget }
Push-Location $root
try {
    cmake --preset $preset -B $binary
    if ($LASTEXITCODE) { throw 'configure failed' }
    cmake --build $binary --config $Config
    if ($LASTEXITCODE) { throw 'build failed' }
    if (-not $SkipTests) {
        ctest --test-dir $binary -C $Config --output-on-failure
        if ($LASTEXITCODE) { throw 'tests failed' }
    }
    if ($Deploy) {
        Assert-DeploymentTarget # a running copy or data may have appeared during the build
        if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
        New-Item -ItemType Directory -Force $dist | Out-Null
        Copy-Item "$binary\WinShun.exe" $dist
        New-Item -ItemType Directory -Force "$dist\fonts" | Out-Null
        Copy-Item "$binary\fonts\AlibabaPuHuiTi-3-55-Regular.ttf" "$dist\fonts"
        # The button users pin to the taskbar (src/stub).
        Copy-Item "$binary\WinShunSearch.exe" $dist
        # The MCP server AI clients start (src/mcp); it relays to WinShun over a pipe.
        Copy-Item "$binary\WinShunMcp.exe" $dist
        # The command line ws.exe (src/cli), alone in bin\: that folder goes on PATH, never one with Qt's DLLs.
        New-Item -ItemType Directory -Force "$dist\bin" | Out-Null
        Copy-Item "$binary\bin\ws.exe" "$dist\bin"
        # Plugin processes are started by WinShunPluginHost.exe (src/plugin/runner), with the user's normal rights.
        Copy-Item "$binary\WinShunPluginHost.exe" $dist
        # The index service (src/service): reads the MFT and change journals for a Win顺 with normal rights.
        Copy-Item "$binary\WinShunService.exe" $dist
        # The in-app update's helper (src/updater): swaps a portable copy's files while Win顺 is closed.
        Copy-Item "$binary\WinShunUpdate.exe" $dist
        # The plugins that come with Win顺 (plugins\, staged by the build with
        # their SDK files and WinShunTools.exe): installed off by default.
        Copy-Item "$binary\plugins" "$dist\plugins" -Recurse
        # Documents are read by WinShunExtract.exe (src/extract), PDFs with
        # PDFium; their licenses go along.
        Copy-Item "$binary\WinShunExtract.exe", "$binary\pdfium.dll" $dist
        $pdfium = Get-ChildItem "$binary\_deps" -Directory -Filter 'pdfium-*' | Sort-Object Name | Select-Object -Last 1
        New-Item -ItemType Directory -Force "$dist\licenses\pdfium" | Out-Null
        Copy-Item "$($pdfium.FullName)\LICENSE" "$dist\licenses\pdfium\LICENSE.txt"
        Copy-Item "$($pdfium.FullName)\licenses\*" "$dist\licenses\pdfium"
        Copy-Item "$root\third_party\miniz\LICENSE" "$dist\licenses\miniz.txt"
        # The icons: Phosphor (MIT), compiled into WinShun.exe.
        Copy-Item "$root\third_party\phosphor\LICENSE" "$dist\licenses\phosphor.txt"
        # The words in pictures (platform/PaddleOcr): ONNX Runtime and the
        # PP-OCRv6 tiny models, with their licenses.
        if ($IncludeOcr) {
            Copy-Item "$binary\onnxruntime.dll" $dist
            Copy-Item "$binary\ocr" "$dist\ocr" -Recurse
        }
        $ort = Get-ChildItem "$binary\_deps" -Directory -Filter 'onnxruntime-win-x64-*' | Sort-Object Name | Select-Object -Last 1
        New-Item -ItemType Directory -Force "$dist\licenses\onnxruntime" | Out-Null
        Copy-Item "$($ort.FullName)\LICENSE" "$dist\licenses\onnxruntime\LICENSE.txt"
        Copy-Item "$($ort.FullName)\ThirdPartyNotices.txt" "$dist\licenses\onnxruntime"
        Copy-Item "$root\third_party\ppocr\LICENSE" "$dist\licenses\paddleocr.txt"
        Copy-Item "$root\resources\fonts\LICENSE.txt" "$dist\licenses\alibaba-font.txt"
        # Only what the app uses: no shader compilers (software renderer), no
        # QML debugger, network, TLS or SVG plugins, and of the database
        # drivers only SQLite (the clipboard history).
        # The library plugins share the host's Qt, including their QML imports.
        & "$QtDir\bin\windeployqt.exe" --qmldir "$root\src" --no-translations --no-system-d3d-compiler `
            --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime `
            --skip-plugin-types qmltooling,generic,networkinformation,tls,iconengines,imageformats `
            --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql `
            "--$preset" "$dist\WinShun.exe"
        if ($LASTEXITCODE) { throw 'windeployqt failed' }
        # Only the clipboard's selected codecs; PNG and BMP are built into QtGui.
        $imageFormats = Join-Path $dist 'imageformats'
        New-Item -ItemType Directory -Force $imageFormats | Out-Null
        $codecSuffix = if ($Config -eq 'Debug') { 'd' } else { '' }
        foreach ($codec in @('qgif', 'qjpeg')) {
            Copy-Item -LiteralPath "$QtDir\plugins\imageformats\$codec$codecSuffix.dll" -Destination $imageFormats
        }
        # App-local C++ runtime, so users need no VC++ redistributable installed
        # (msvcp140_1: onnxruntime.dll).
        $crt = Join-Path $env:VCToolsRedistDir 'x64\Microsoft.VC143.CRT'
        foreach ($dll in 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
            Copy-Item (Join-Path $crt $dll) $dist
        }
        Write-Host "Deployed to $dist"
    }
} finally {
    Pop-Location
}

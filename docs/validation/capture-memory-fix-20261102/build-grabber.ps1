param(
    [Parameter(Mandatory)][string]$QtDir,
    [string]$OutputDir = "$PSScriptRoot/../../../build/capture-memory-probe",
    [string]$GrabberSource = "$PSScriptRoot/../../../src/capture/Grabber.cpp",
    [switch]$SimulateHdr
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path "$PSScriptRoot/../../..").Path
$vsPath = & 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module "$vsPath/Common7/Tools/Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$OutputDir = (Resolve-Path $OutputDir).Path
$GrabberSource = (Resolve-Path $GrabberSource).Path
$oldPath = $env:PATH
$oldHdr = $env:WINSHUN_TEST_HDR_CAPTURE
Push-Location $OutputDir
try {
    $env:PATH = "$QtDir/bin;$env:PATH"
    & cl /nologo /std:c++20 /Zc:__cplusplus /permissive- /bigobj /EHsc /O2 /MD /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
        "/I$QtDir/include" "/I$QtDir/include/QtCore" "/I$QtDir/include/QtGui" "/I$workspaceRoot/src/capture" `
        /Fe:grabber-probe.exe "$PSScriptRoot/grabber-probe.cpp" $GrabberSource "$workspaceRoot/src/capture/Hdr.cpp" `
        /link "/LIBPATH:$QtDir/lib" Qt6Core.lib Qt6Gui.lib windowsapp.lib d3d11.lib dxgi.lib user32.lib gdi32.lib ole32.lib oleaut32.lib windowscodecs.lib psapi.lib
    if ($LASTEXITCODE) { throw "Grabber probe build failed: $LASTEXITCODE" }
    $env:WINSHUN_TEST_HDR_CAPTURE = if ($SimulateHdr) { '1' } else { $null }
    & ./grabber-probe.exe
    if ($LASTEXITCODE) { throw "Grabber probe failed: $LASTEXITCODE" }
} finally {
    Pop-Location
    $env:PATH = $oldPath
    $env:WINSHUN_TEST_HDR_CAPTURE = $oldHdr
}

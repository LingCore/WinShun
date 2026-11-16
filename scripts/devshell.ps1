<#
.SYNOPSIS
    Puts the MSVC developer environment (cl, rc, link, ninja) into this PowerShell session.
.DESCRIPTION
    Dot-source it: . .\scripts\devshell.ps1
    Then e.g.: cmake --build build\<task> --target tst_settings
    Used by build.ps1. Nothing happens when cl.exe is already on PATH.
#>
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ workload was not found.' }
    # VsDevCmd calls vswhere itself; without it on PATH, cmd says so in the
    # console's code page (GBK on Chinese Windows), read back as mojibake.
    $env:PATH = "$(Split-Path $vswhere);$env:PATH"
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

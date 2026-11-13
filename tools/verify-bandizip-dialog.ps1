# Bandizip's "Select extraction path" window with the real WinShun: the search bar comes up under it, finds a
# folder, and choosing the folder writes it into the window's path box. The window is then cancelled: nothing
# is extracted here (tst_pathbox's realBandizip presses its OK and looks at the extracted file).
#
# Only a Bandizip started here, on an archive made here, is touched; a Bandizip the user has open is left
# alone. Bandizip is not installed by this script: without it, it says so and fails.
#
#   ./tools/verify-bandizip-dialog.ps1 -BuildDir build/dialog-pathbox -OutputDir build/dialog-pathbox/bandizip
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$Bandizip = "$env:ProgramFiles\Bandizip\Bandizip.exe",
    [string]$QtBin = 'C:\Qt\6.12.0\msvc2022_64\bin'
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $Bandizip)) { throw "Bandizip is not installed at $Bandizip (pass -Bandizip)" }
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$null = New-Item -ItemType Directory -Force $OutputDir
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path

if (!('WinShun.BandizipProbe' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace WinShun {
    public static class BandizipProbe {
        public delegate bool Visitor(IntPtr window, IntPtr data);
        [DllImport("user32.dll")] static extern bool EnumWindows(Visitor callback, IntPtr data);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
        [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
        [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
        [DllImport("user32.dll", EntryPoint="PostMessageW")] public static extern bool Post(IntPtr window, uint message, IntPtr w, IntPtr l);
        public static IntPtr WindowOf(uint process) {
            IntPtr found = IntPtr.Zero;
            EnumWindows((window, data) => {
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                if (owner != process || !IsWindowVisible(window)) return true;
                found = window;
                return false;
            }, IntPtr.Zero);
            return found;
        }
    }
}
'@
}

$work = Join-Path $OutputDir 'work'
$savedPath = $env:PATH
$process = $null
try {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    $null = New-Item -ItemType Directory -Force $work
    Set-Content -LiteralPath "$work\inside.txt" -Value 'winshun' -Encoding ascii
    Compress-Archive -LiteralPath "$work\inside.txt" -DestinationPath "$work\probe.zip"
    $process = Start-Process -FilePath $Bandizip -ArgumentList 'x', '-target:dlg', "-o:`"$work`"", "`"$work\probe.zip`"" -PassThru
    $window = [IntPtr]::Zero
    for ($i = 0; $i -lt 100 -and $window -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 100
        $window = [WinShun.BandizipProbe]::WindowOf($process.Id)
    }
    if ($window -eq [IntPtr]::Zero) { throw 'Bandizip did not show its window' }

    $env:PATH = "$QtBin;$env:PATH"
    $runtimeArgs = @('--probe-runtime', ('"' + $OutputDir + '\runtime"'), '--renderer', 'd3d11', '--settle-ms', '700',
        '--file-dialog-window', [string]$window.ToInt64(), '--file-dialog-pid', [string]$process.Id)
    $runtime = Start-Process -FilePath "$BuildDir\WinShun.exe" -ArgumentList $runtimeArgs -WindowStyle Hidden -Wait -PassThru `
        -RedirectStandardOutput "$OutputDir\runtime-stdout.txt" -RedirectStandardError "$OutputDir\runtime-stderr.txt"
    if (Test-Path -LiteralPath "$OutputDir\runtime\runtime.json") { Get-Content -LiteralPath "$OutputDir\runtime\runtime.json" -Raw }
    if ($runtime.ExitCode -ne 0) { throw "The search bar did not drive the window (exit $($runtime.ExitCode)): $OutputDir\runtime\runtime.json" }
    if (![WinShun.BandizipProbe]::IsWindow($window)) { throw 'The window was closed: choosing a folder must not confirm it' }

    $null = [WinShun.BandizipProbe]::Post($window, 0x0111, [IntPtr]2, [WinShun.BandizipProbe]::GetDlgItem($window, 2)) # Cancel
    if (!$process.WaitForExit(10000)) { throw 'Bandizip did not close on Cancel' }
    $extracted = @(Get-ChildItem -LiteralPath $work -Recurse -File | Where-Object { $_.Name -ne 'probe.zip' -and $_.FullName -ne "$work\inside.txt" })
    if ($extracted.Count) { throw "Something was extracted though the window was cancelled: $($extracted.FullName -join ', ')" }
    Write-Output "Verified with the real Bandizip (a window started here only). Results: $OutputDir"
} finally {
    $env:PATH = $savedPath
    if ($process -and !$process.HasExited) { Stop-Process -Id $process.Id -Force }
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}

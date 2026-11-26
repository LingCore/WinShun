# GitHub #452: the clipboard opened over a game in exclusive full screen.
# Builds the stand-in game (fsgame.cpp, beside this script), starts it in real
# exclusive full screen on the primary monitor and runs
#   WinShun.exe --probe-runtime <out> --full-screen-game-check <hwnd>
# beside whatever WinShun is running (a profile of its own, its own chord).
# Needs a real graphics card; the screen is the game's for about 20 s: hands
# off the keyboard and mouse meanwhile.
#
#   .\run-game-check.ps1 -WinShun <build>\WinShun.exe -OutDir <build>\game-check [-Mode stay]
#
# -Mode stay: out of full screen the game stays a window behind the others
# instead of minimizing itself (what some games do).
# Windows notes a compatibility layer (HIGHDPIAWARE) for a program it saw take
# the whole screen; the one for fsgame.exe is removed at the end.
param(
    [Parameter(Mandatory)] [string] $WinShun,
    [Parameter(Mandatory)] [string] $OutDir,
    [ValidateSet('minimize', 'stay')] [string] $Mode = 'minimize'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
. (Join-Path $root 'scripts\devshell.ps1')
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
Add-Type -Namespace W452 -Name U -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
'@
$game = $null
$probe = $null
$layers = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
$exe = Join-Path $OutDir 'fsgame.exe'
try {
    Push-Location $OutDir
    try { & cl.exe /nologo /utf-8 /EHsc /O2 /W4 /DUNICODE /D_UNICODE (Join-Path $PSScriptRoot 'fsgame.cpp') "/Fe:$exe" /link /SUBSYSTEM:WINDOWS | Out-Host }
    finally { Pop-Location }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) { throw 'fsgame.cpp did not build' }

    $gameLog = Join-Path $OutDir "fsgame-$Mode.log"
    $gameArgs = @("`"$gameLog`"")
    if ($Mode -eq 'stay') { $gameArgs += 'stay' }
    $game = Start-Process $exe -ArgumentList $gameArgs -PassThru
    $hwnd = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(15)
    while ($hwnd -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $hwnd = [W452.U]::FindWindowW('FsGame', 'FsGame')
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw 'the game window never came' }
    Start-Sleep -Seconds 2

    $env:WINSHUN_TEST_NO_CLIP_HISTORY = '1'
    $probeOut = Join-Path $OutDir "probe-$Mode"
    New-Item -ItemType Directory -Force $probeOut | Out-Null
    $probe = Start-Process $WinShun -ArgumentList '--probe-runtime', "`"$probeOut`"", '--full-screen-game-check', $hwnd.ToInt64() `
        -PassThru -RedirectStandardError (Join-Path $OutDir "probe-$Mode-stderr.txt") -RedirectStandardOutput (Join-Path $OutDir "probe-$Mode-stdout.txt")
    if (-not $probe.WaitForExit(240000)) { throw 'the probe did not finish in 240 s' }
    "probe exit $($probe.ExitCode)"
    Get-Content (Join-Path $probeOut 'full-screen-game.json') -ErrorAction SilentlyContinue
    '--- game log'
    Get-Content $gameLog -ErrorAction SilentlyContinue
    '--- probe stderr (last lines)'
    Get-Content (Join-Path $OutDir "probe-$Mode-stderr.txt") -Tail 15 -ErrorAction SilentlyContinue
}
finally {
    if ($probe -and -not $probe.HasExited) { $probe.Kill() }
    if ($game -and -not $game.HasExited) { $game.Kill() }
    if ((Test-Path $layers) -and (Get-Item $layers).GetValueNames() -contains $exe) { Remove-ItemProperty $layers -Name $exe }
}

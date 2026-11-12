# The real hotkey (Ctrl+F22, SendInput) on a staged copy with its own profile: time until the
# launcher is on the screen, a moment after the last opening and after IdleTrim gave the memory back.
#   live.ps1 -Variant release025|new [-Idle 6]
param([Parameter(Mandatory)] [ValidateSet('release025', 'new')] [string] $Variant, [int] $Idle = 6, [int] $GapSec = 27)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = 'F:\电脑便捷工具'
$stage = "$root\dist\issue280\WinShun"
$profileDir = "$here\prof-$Variant"
$out = "$here\$Variant.txt"
$lock = 'F:\wswt\ui.lock'
$keys = 'HKCU\Software\Classes\Directory\shell\WinShun.SearchHere', 'HKCU\Software\Classes\Directory\Background\shell\WinShun.SearchHere',
    'HKCU\Software\Classes\Drive\shell\WinShun.SearchHere'

if (Get-Process WinShun -ErrorAction SilentlyContinue) { throw 'A WinShun is running: not started beside it.' }
$stream = [IO.File]::Open($lock, 'CreateNew')
$started = $null
try {
    $bytes = [Text.Encoding]::UTF8.GetBytes("issue280 real hotkey ($Variant), session f6fd5001, $(Get-Date -Format s)")
    $stream.Write($bytes, 0, $bytes.Length); $stream.Close()
    Add-Type -Namespace Live -Name Input -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO info);
[DllImport("kernel32.dll")] public static extern uint GetTickCount();
public static uint IdleMs() { var i = new LASTINPUTINFO(); i.cbSize = 8; GetLastInputInfo(ref i); return GetTickCount() - i.dwTime; }
'@
    function Wait-Quiet { $sw = [Diagnostics.Stopwatch]::StartNew(); while ([Live.Input]::IdleMs() -lt 3000 -and $sw.Elapsed.TotalSeconds -lt 120) { Start-Sleep -Milliseconds 200 } }

    # The per-user "search here" entries: a staged copy rewrites or removes them.
    for ($i = 0; $i -lt $keys.Count; ++$i) { reg export $keys[$i] "$here\searchhere-$i.reg" /y 2>$null | Out-Null }

    if (-not (Test-Path $stage)) {
        robocopy "$root\dist\release025\WinShun" $stage /E /XD data /NFL /NDL /NJH /NJS /NP | Out-Null
        Copy-Item "$stage\WinShun.exe" "$stage\WinShun-release025.exe"
    }
    Copy-Item ($Variant -eq 'new' ? "$root\build\issue280\WinShun.exe" : "$stage\WinShun-release025.exe") "$stage\WinShun.exe" -Force

    if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }
    New-Item -ItemType Directory -Force "$profileDir\WinShun" | Out-Null
    Set-Content -LiteralPath "$profileDir\WinShun\WinShun.ini" -Encoding utf8 -Value @'
[Setup]
Completed=true
[Launcher]
Hotkey=Ctrl+F22
DoubleCtrl=false
ExplorerMenu=false
ExplorerTyping=false
History=false
DialogJump=false
DialogBar=false
[Clipboard]
Enabled=false
WinV=false
[Content]
Index=false
[Update]
Automatic=false
[Mcp]
Enabled=false
'@
    $env:WINSHUN_LOG_INFO = "1"; $env:AB_TYPE = "1"
    $started = Start-Process "$stage\WinShun.exe" -ArgumentList '--profile', "`"$profileDir`"", '--background' -WorkingDirectory $stage -PassThru
    # Ready: the index loaded and the memory given back for the first time.
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt 300 -and -not (Select-String -LiteralPath "$profileDir\WinShun.log" -Pattern 'Idle trim: \d+ MB' -Quiet -ErrorAction SilentlyContinue)) { Start-Sleep 1 }
    if ($started.HasExited) { throw 'the staged copy went away' }
    "ready after $([int]$sw.Elapsed.TotalSeconds) s" | Tee-Object $out
    Start-Sleep 3
    $private = { [int]((Get-Counter "\Process(WinShun)\Working Set - Private" -ErrorAction SilentlyContinue).CounterSamples[0].CookedValue / 1MB) }

    Wait-Quiet
    "== first (after the start, memory given back): private $(& $private) MB" | Tee-Object $out -Append
    & "$here\ab.exe" hotkey $started.Id 1 600 1500 | Tee-Object $out -Append
    "== warm" | Tee-Object $out -Append
    Wait-Quiet
    & "$here\ab.exe" hotkey $started.Id 5 600 1500 | Tee-Object $out -Append
    for ($i = 1; $i -le $Idle; ++$i) {
        Start-Sleep $GapSec
        Wait-Quiet
        "== idle $i ($GapSec s): private $(& $private) MB" | Tee-Object $out -Append
        & "$here\ab.exe" hotkey $started.Id 1 600 0 | Tee-Object $out -Append
    }
    Start-Sleep 30
    "== at rest: private $(& $private) MB" | Tee-Object $out -Append
} finally {
    if ($started -and -not $started.HasExited) {
        Start-Process "$stage\WinShun.exe" -ArgumentList '--profile', "`"$profileDir`"", '--quit' -WorkingDirectory $stage -Wait
        if (-not $started.WaitForExit(15000)) { $started.Kill() }
    }
    for ($i = 0; $i -lt $keys.Count; ++$i) {
        reg delete $keys[$i] /f 2>$null | Out-Null
        if (Test-Path "$here\searchhere-$i.reg") { reg import "$here\searchhere-$i.reg" 2>$null | Out-Null }
    }
    if (Test-Path "$profileDir\diagnostics") { Copy-Item "$profileDir\diagnostics\events.jsonl" "$here\$Variant-events.jsonl" -Force -ErrorAction SilentlyContinue }
    Select-String -LiteralPath "$profileDir\WinShun.log" -Pattern 'Idle trim' -ErrorAction SilentlyContinue | Select-Object -First 12 | ForEach-Object { $_.Line } | Set-Content "$here\$Variant-trim.txt"
    Remove-Item -LiteralPath $lock -Force
}

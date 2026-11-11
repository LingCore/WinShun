param([string]$Only = '')
# Live check for GitHub #242 (an instant keyword typed alone keeps the search's rows)
# and #288 (the place that restarts WinShun as administrator). A staged copy with its
# own --profile, started with normal rights; every key is guarded; aborts, never retries.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$wt = (Get-Item 'F:\wswt\i288').Target
if ($wt -is [array]) { $wt = $wt[0] }
$exe = Join-Path $wt 'dist\i288\WinShun\WinShun.exe'
$prof = Join-Path $wt 'build\i288\live\profile'
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$log = Join-Path $out 'run.txt'
$lock = 'F:\wswt\ui.lock'
$admin = -join ([char]0x7BA1, [char]0x7406, [char]0x5458)                        # the word for "administrator"
$restartName = -join ([char]0x4EE5, [char]0x7BA1, [char]0x7406, [char]0x5458, [char]0x8EAB, [char]0x4EFD, [char]0x91CD, [char]0x65B0, [char]0x542F, [char]0x52A8)

$src = [IO.File]::ReadAllText((Join-Path (Split-Path (Split-Path $wt)) 'docs\validation\issues-242-82-20261111\scripts\live.ps1'))
Add-Type -AssemblyName System.Drawing
Add-Type ([regex]::Match($src, "(?s)Add-Type @'\r?\n(.*?)\r?\n'@").Groups[1].Value)
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Tok {
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr token, int cls, out int value, int size, out int needed);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    // "elevated", "normal", or why it could not be asked (a process with more rights than ours refuses).
    public static string Of(uint pid) {
        IntPtr p = OpenProcess(0x1000, false, pid);
        if (p == IntPtr.Zero) return "no access to the process (" + Marshal.GetLastWin32Error() + ")";
        try {
            IntPtr t;
            if (!OpenProcessToken(p, 8, out t)) return "no access to its token (" + Marshal.GetLastWin32Error() + "): more rights than ours";
            try { int v, n; return GetTokenInformation(t, 20, out v, 4, out n) ? (v != 0 ? "elevated" : "normal") : "?"; }
            finally { CloseHandle(t); }
        } finally { CloseHandle(p); }
    }
}
'@
[W]::Aware()

function Log([string]$s) { $line = (Get-Date -Format 'HH:mm:ss.fff') + ' ' + $s; Add-Content -LiteralPath $log -Value $line -Encoding UTF8; Write-Output $line }
function Ws { @(Get-CimInstance Win32_Process -Filter "Name='WinShun.exe'") }
function Boxes([uint32]$wsPid) { @([W]::Top() | Where-Object { [W]::Pid($_) -eq $wsPid -and ([W]::Rect($_).R - [W]::Rect($_).L) -gt 200 }) }
function Shot([string]$name, [uint32]$wsPid) {
    $b = Boxes $wsPid | Select-Object -First 1
    if (-not $b) { Log "shot ${name}: no launcher window"; return }
    $r = [W]::Rect($b); $w = $r.R - $r.L; $h = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try { $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $h)) } finally { $g.Dispose() }
    $bmp.Save((Join-Path $out "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Log "shot ${name}: ${w}x${h}"
}
function Ask([string]$text, [uint32]$wsPid) {
    Start-Process -FilePath $exe -ArgumentList '--profile', ('"' + $prof + '"'), '--query', ('"' + $text + '"') -WindowStyle Normal
    foreach ($i in 1..40) { Start-Sleep -Milliseconds 150; if ((Boxes $wsPid).Count -and [W]::Pid([W]::GetForegroundWindow()) -eq $wsPid) { break } }
    Start-Sleep -Milliseconds 1800
    return ((Boxes $wsPid).Count -gt 0)
}
function Key([uint16]$vk, [uint32]$wsPid) {
    if ([W]::Pid([W]::GetForegroundWindow()) -ne $wsPid) { throw "key ${vk}: the launcher is not in front" }
    [W]::Key($vk)
}
function Close([uint32]$wsPid) { foreach ($i in 1..2) { if ([W]::Pid([W]::GetForegroundWindow()) -eq $wsPid) { [W]::Key(0x1B); Start-Sleep -Milliseconds 400 } } }

$keys = 'Directory\Background\shell', 'Directory\shell', 'Drive\shell' | ForEach-Object { "HKCU:\Software\Classes\$_\WinShun.SearchHere" }
$saved = @{}
foreach ($k in $keys) { if (Test-Path -LiteralPath $k) { $saved[$k] = @((Get-ItemProperty -LiteralPath $k).Icon, (Get-ItemProperty -LiteralPath "$k\command").'(default)') } }
$taskBefore = (schtasks /query /tn \WinShun /xml 2>$null | Select-String -Pattern '<Command>') -join ''
$own = $false
try {
    if (@(Ws).Count) { throw 'a WinShun is running' }
    $fs = [IO.File]::Open($lock, 'CreateNew'); $bytes = [Text.Encoding]::UTF8.GetBytes('issues 288/242 live test'); $fs.Write($bytes, 0, $bytes.Length); $fs.Close(); $own = $true

    # The profile: nothing that reaches outside it, and one instant command whose keyword is a program's name.
    if (Test-Path -LiteralPath $prof) { throw "the profile is there already: $prof" }
    New-Item -ItemType Directory -Force -Path (Join-Path $prof 'WinShun'), (Join-Path $prof 'plugins\test.instant') | Out-Null
    [IO.File]::WriteAllText((Join-Path $prof 'WinShun\WinShun.ini'), "[Launcher]`r`nExplorerMenu=false`r`nDoubleCtrl=false`r`nHotkey=`r`nHistory=false`r`nDialogJump=false`r`nDialogBar=false`r`n[Content]`r`nIndex=false`r`n[Plugin.test.instant]`r`nEnabled=true`r`n")
    [IO.File]::WriteAllText((Join-Path $prof 'plugins\test.instant\plugin.json'), @'
{
  "manifest": 1,
  "id": "test.instant",
  "name": {"zh": "Instant test", "en": "Instant test"},
  "version": "1.0.0",
  "description": {"zh": "instant keyword", "en": "instant keyword"},
  "author": "test",
  "license": "MIT",
  "permissions": [],
  "commands": [
    {"id": "main", "keyword": "notepad", "mode": "instant", "title": {"zh": "Instant test", "en": "Instant test"}, "icon": "glyph:U+E70B",
     "run": {"items": [
       {"id": "one", "title": "ws-instant-row-1", "subtitle": "first row of the plugin", "open": {"path": "%TEMP%"}},
       {"id": "two", "title": "ws-instant-row-2", "subtitle": "second row of the plugin", "open": {"path": "%TEMP%"}}
     ]}}
  ]
}
'@)
    $env:WINSHUN_LOG_INFO = '1'
    Start-Process -FilePath $exe -ArgumentList '--profile', ('"' + $prof + '"'), '--background' -WindowStyle Normal -WorkingDirectory (Split-Path $exe)
    Start-Sleep -Seconds 10
    $ws = Ws | Select-Object -First 1
    if (-not $ws) { throw 'the staged WinShun did not stay up' }
    $wsPid = [uint32]$ws.ProcessId
    Log ("WinShun {0} pid {1}, rights: {2}" -f (Get-Item -LiteralPath $exe).LastWriteTime.ToString('HH:mm:ss'), $wsPid, [Tok]::Of($wsPid))

    if ($Only -ne 'admin') {
        # 242: the keyword alone, then with a space.
        if (-not (Ask 'notepad' $wsPid)) { throw 'no launcher for notepad' }
        Shot '242-keyword-alone' $wsPid
        Key 0x28 $wsPid; Key 0x28 $wsPid; Start-Sleep -Milliseconds 400   # Down, Down: onto the search's own rows
        Shot '242-keyword-alone-down2' $wsPid
        Close $wsPid
        if (-not (Ask 'notepad ' $wsPid)) { throw 'no launcher for "notepad "' }
        Shot '242-keyword-space' $wsPid
        Close $wsPid
        if (-not (Ask 'notepa' $wsPid)) { throw 'no launcher for notepa' }
        Shot '242-keyword-partial' $wsPid
        Close $wsPid
    }

    # 288: listed with normal rights; Enter restarts as administrator; not listed then.
    if (-not (Ask $admin $wsPid)) { throw 'no launcher for the word' }
    Shot '288-normal-rights' $wsPid
    Close $wsPid
    if (-not (Ask $restartName $wsPid)) { throw 'no launcher for the name' }
    Shot '288-by-name' $wsPid
    Key 0x0D $wsPid
    $new = $null
    foreach ($i in 1..60) { Start-Sleep -Milliseconds 500; $new = Ws | Where-Object { $_.ProcessId -ne $wsPid } | Select-Object -First 1; if ($new -and -not (Ws | Where-Object { $_.ProcessId -eq $wsPid })) { break } }
    if (-not $new) { throw 'no new WinShun after Enter' }
    Start-Sleep -Seconds 8
    $newPid = [uint32]$new.ProcessId
    Log ("after Enter: pid {0} -> {1}, rights: {2}, command line readable: {3}" -f $wsPid, $newPid, [Tok]::Of($newPid), [bool]$new.CommandLine)
    [void](Ask $admin $newPid)
    Shot '288-elevated' $newPid
}
catch { Log ('ABORTED: ' + $_.Exception.Message + ' @ ' + $_.InvocationInfo.ScriptLineNumber) }
finally {
    if ($own) {
        if (@(Ws).Count) { Start-Process -FilePath $exe -ArgumentList '--profile', ('"' + $prof + '"'), '--quit' -WindowStyle Normal; foreach ($i in 1..20) { Start-Sleep -Milliseconds 500; if (-not @(Ws).Count) { break } } }
        $left = @(Ws)
        if ($left.Count) {
            Log ("still running after --quit: " + (($left | ForEach-Object { $_.ProcessId }) -join ',') + '; ending it as administrator')
            $ids = ($left | ForEach-Object { $_.ProcessId }) -join ','
            $p = Start-Process powershell.exe -Verb RunAs -PassThru -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', "Stop-Process -Id $ids -Force"
            [void]$p.WaitForExit(15000); Start-Sleep -Seconds 1
        }
        foreach ($k in $saved.Keys) { Set-ItemProperty -LiteralPath $k -Name Icon -Value $saved[$k][0]; Set-ItemProperty -LiteralPath "$k\command" -Name '(default)' -Value $saved[$k][1] }
        $taskAfter = (schtasks /query /tn \WinShun /xml 2>$null | Select-String -Pattern '<Command>') -join ''
        $here0 = if ($saved.Count) { (Get-ItemProperty -LiteralPath ($keys[0] + '\command')).'(default)' } else { '(none before)' }
        Log ("left: WinShun processes {0}; autostart task unchanged {1}; search-here: {2}" -f @(Ws).Count, ($taskBefore -eq $taskAfter), $here0)
        [IO.File]::Delete($lock)
    }
}

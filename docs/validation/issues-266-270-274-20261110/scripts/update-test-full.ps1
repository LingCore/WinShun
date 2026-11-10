param(
    [Parameter(Mandatory = $true)] [string] $NewExe,   # the WinShun.exe that the update brings
    [string] $Old = 'F:\电脑便捷工具\dist\release022\WinShun',   # the version updated from (its helper runs)
    [string] $Root = 'F:\电脑便捷工具\build\issues-266-270-274\upd',
    [string] $Access = 'direct'
)
# Whole #274 flow, final step included: the old 0.22.0 helper swaps the files and
# starts the new exe unelevated; the new exe must wait for the helper to end and
# then restart itself as administrator (this machine: ConsentPromptBehaviorAdmin=0,
# so no UAC box). Run from an UNELEVATED shell: the old app starts the helper so.
$ErrorActionPreference = 'Stop'
$app = Join-Path $Root 'WinShun'
$updates = Join-Path $Root 'Updates'
$reg = Join-Path $Root 'reg'
$out = Join-Path $Root 'result.txt'
$keys = 'Directory\shell\WinShun.SearchHere', 'Directory\Background\shell\WinShun.SearchHere', 'Drive\shell\WinShun.SearchHere', 'Directory\Background\shell\WinShun.PasteFile'

if (Get-Process WinShun -ErrorAction SilentlyContinue) { throw 'A WinShun.exe is running: not touching it.' }
$lock = [IO.File]::Open('F:\wswt\ui.lock', 'CreateNew', 'Write', 'None')
$lockText = [Text.Encoding]::UTF8.GetBytes("claude-47 #274 update test $(Get-Date -Format o)")
$lock.Write($lockText, 0, $lockText.Length); $lock.Flush()
Add-Type -Namespace T274 -Name P -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)] public static extern System.IntPtr OpenProcess(uint access, bool inherit, int pid);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool QueryFullProcessImageNameW(System.IntPtr h, uint flags, System.Text.StringBuilder name, ref uint size);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(System.IntPtr h);
public static string PathOf(int pid) {
    System.IntPtr h = OpenProcess(0x1000, false, pid);
    if (h == System.IntPtr.Zero) return "";
    var sb = new System.Text.StringBuilder(1024); uint n = 1024;
    string r = QueryFullProcessImageNameW(h, 0, sb, ref n) ? sb.ToString() : "";
    CloseHandle(h); return r;
}
'@
function Ours { Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { [T274.P]::PathOf($_.Id) -like "$app\*" } }
function Events {
    $f = Get-ChildItem -Path "$app\data" -Recurse -Filter 'events.jsonl' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($f) { Get-Content $f.FullName | Where-Object { $_ -match '"app-start"|elevation' } } else { @() }
}
try {
    if (Test-Path $Root) { Remove-Item -Recurse -Force $Root }
    New-Item -ItemType Directory -Force $app, $updates, $reg | Out-Null
    "update test (full, no mutex held by the test) $(Get-Date -Format o) access=$Access new=$NewExe" | Set-Content $out

    $i = 0
    foreach ($k in $keys) { $i++; reg export "HKCU\Software\Classes\$k" (Join-Path $reg "$i.reg") /y 2>&1 | Out-Null }
    $taskBefore = (schtasks /query /tn \WinShun /xml 2>$null) -join "`n"

    robocopy $Old $app /E /XD data /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed: $LASTEXITCODE" }
    New-Item -ItemType Directory -Force "$app\data\WinShun" | Out-Null
    $ini = @"
[Setup]
Completed=true

[Index]
Access=$Access

[Launcher]
ExplorerMenu=false
DoubleCtrl=false
Hotkey=
History=false
DialogJump=false
DialogBar=false

[Content]
Index=false

[Clipboard]
Enabled=false
"@
    [IO.File]::WriteAllText("$app\data\WinShun\WinShun.ini", $ini, (New-Object System.Text.UTF8Encoding $false))

    $stage = Join-Path $app '.winshun-update-new'
    robocopy $Old $stage /E /XD data /NFL /NDL /NJH /NJS /NP | Out-Null
    Copy-Item $NewExe "$stage\WinShun.exe" -Force
    $newHash = (Get-FileHash "$stage\WinShun.exe").Hash
    Copy-Item "$app\WinShunUpdate.exe" "$updates\WinShunUpdate.exe" -Force   # the OLD version's helper

    $helperArgs = @('--portable', '--pid', '999999', '--ready', 'Local\WsTest274Ready', '--app', "`"$app`"",
        '--stage', "`"$stage`"", '--backup', "`"$app\.winshun-update-old`"", '--log', "`"$updates\update.log`"")
    $helper = Start-Process "$updates\WinShunUpdate.exe" -ArgumentList $helperArgs -PassThru
    if (-not $helper.WaitForExit(240000)) { "helper still running after 240 s" | Add-Content $out; $helper.Kill() }
    $helperEnd = Get-Date
    "helper exit code: $($helper.ExitCode)" | Add-Content $out
    "--- update.log" | Add-Content $out
    Get-Content "$updates\update.log" -ErrorAction SilentlyContinue | Add-Content $out

    # The new copy should now restart itself as administrator, once.
    $deadline = (Get-Date).AddSeconds(60)
    $elevatedStart = $null
    while ((Get-Date) -lt $deadline -and -not $elevatedStart) {
        Start-Sleep -Milliseconds 500
        $elevatedStart = Events | Where-Object { $_ -match '"app-start"' } | ForEach-Object { $_ | ConvertFrom-Json } |
            Where-Object { $_.details.rights.elevated -eq $true } | Select-Object -First 1
    }
    if ($elevatedStart) { "elevated start seen $([int]((Get-Date) - $helperEnd).TotalSeconds) s after the helper ended" | Add-Content $out }
    else { "NO elevated start within 60 s of the helper's end" | Add-Content $out }
    Start-Sleep -Seconds 5   # would a second restart follow?
    $running = Ours
    "running from the test folder: $(@($running).Count) pid=$($running.Id -join ',')" | Add-Content $out
    "exe in place is the new one: $((Get-FileHash "$app\WinShun.exe").Hash -eq $newHash)" | Add-Content $out
    "stage left: $(Test-Path $stage); backup left: $(Test-Path "$app\.winshun-update-old")" | Add-Content $out
    "--- app-start / elevation events" | Add-Content $out
    Events | ForEach-Object {
        $e = $_ | ConvertFrom-Json
        if ($e.event -eq 'app-start') { "$($e.time) app-start version=$($e.details.version) elevated=$($e.details.rights.elevated)" }
        else { "$($e.time) $($e.event) $($e.details | ConvertTo-Json -Compress)" } } | Add-Content $out
} finally {
    $running = Ours
    if ($running) {
        # It may be elevated: quit it from an elevated process (no UAC box here).
        $quit = Join-Path $Root 'quit.ps1'
        @"
& '$app\WinShun.exe' --quit | Out-Null
`$end = (Get-Date).AddSeconds(25)
while ((Get-Date) -lt `$end -and (Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { `$_.Path -like '$app\*' })) { Start-Sleep -Milliseconds 300 }
Get-Process WinShun -ErrorAction SilentlyContinue | Where-Object { `$_.Path -like '$app\*' } | ForEach-Object { Stop-Process -Id `$_.Id -Force; 'killed ' + `$_.Id }
"@ | Set-Content $quit -Encoding utf8BOM
        $q = Start-Process "$PSHOME\pwsh.exe" -Verb RunAs -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile', '-File', "`"$quit`""
        if (-not $q.WaitForExit(60000)) { "elevated quit did not end in 60 s" | Add-Content $out }
        $left = Ours
        "quit: exit code $($q.ExitCode), left running: $(@($left).Count)" | Add-Content $out
    }
    foreach ($k in $keys) { reg delete "HKCU\Software\Classes\$k" /f 2>&1 | Out-Null }
    Get-ChildItem $reg -Filter '*.reg' -ErrorAction SilentlyContinue | ForEach-Object { reg import $_.FullName 2>&1 | Out-Null }
    foreach ($k in $keys) { "$k => " + (Get-ItemProperty "HKCU:\Software\Classes\$k\command" -ErrorAction SilentlyContinue).'(default)' | Add-Content $out }
    $taskAfter = (schtasks /query /tn \WinShun /xml 2>$null) -join "`n"
    "autostart task unchanged: $($taskBefore -eq $taskAfter)" | Add-Content $out
    $lock.Dispose(); Remove-Item 'F:\wswt\ui.lock' -Force -ErrorAction SilentlyContinue
}
Get-Content $out

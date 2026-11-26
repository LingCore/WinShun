param(
    [Parameter(Mandatory)] [string] $WinShun,   # the WinShun.exe to check
    [Parameter(Mandatory)] [string] $OutDir,    # this run's folder (results, the probe's output)
    [string] $Photoshop = 'C:\Program Files\Adobe\Adobe Photoshop 2024\Photoshop.exe',
    [int] $PictureWidth = 240,                  # the RED picture's width in WinShun's history (2/3 as high)
    [switch] $ClickClipboard,                   # click the clipboard's search field first: it takes the front (--click-first)
    [switch] $CopyFirst                         # Ctrl+C the picture, Esc, then Photoshop's own Ctrl+V (--copy-first)
)
# GitHub #432 end to end: Photoshop in front holds a GREEN picture it read as it
# came there; WinShun's clipboard is opened over it by its hotkey (keys routed,
# Photoshop keeps the front) and Enter pastes the RED picture of its history
# (WinShun.exe --probe-runtime ... --activation-paste-check <hwnd>). The colour
# in the middle of Photoshop's document says what went in. Nothing enters
# Windows' clipboard history or WinShun's (marked copies, WINSHUN_TEST_NO_CLIP_HISTORY);
# the clipboard as it was is put back after. -ClickClipboard: its search field
# is clicked first, the clipboard takes the front (the probe's --click-first).
# -CopyFirst: Ctrl+C copies the picture instead, Esc, and Photoshop's own
# Ctrl+V pastes (--copy-first).
# Windows PowerShell 5.1, -STA.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing, System.Runtime.WindowsRuntime
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class U432 {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);
  public static bool Front(IntPtr h) { mouse_event(0, 0, 0, 0, UIntPtr.Zero); return SetForegroundWindow(h); }
}
"@
New-Item -ItemType Directory -Force $OutDir | Out-Null
$log = [System.Collections.Generic.List[string]]::new()
function Say($s) { $line = "$(Get-Date -Format HH:mm:ss.fff) $s"; $log.Add($line); Write-Host $line }

$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
[Windows.ApplicationModel.DataTransfer.Clipboard, Windows.ApplicationModel.DataTransfer, ContentType = WindowsRuntime] | Out-Null
function HistoryCount {
    try {
        $t = $asTask.MakeGenericMethod([Windows.ApplicationModel.DataTransfer.ClipboardHistoryItemsResult]).Invoke($null, @([Windows.ApplicationModel.DataTransfer.Clipboard]::GetHistoryItemsAsync()))
        $t.Wait(-1) | Out-Null; @($t.Result.Items).Count
    } catch { -1 }
}
function Marked([System.Windows.Forms.DataObject]$d) {
    foreach ($f in 'ExcludeClipboardContentFromMonitorProcessing', 'CanIncludeInClipboardHistory', 'CanUploadToCloudClipboard') {
        $d.SetData($f, (New-Object System.IO.MemoryStream (, [byte[]](0, 0, 0, 0))))
    }
    [System.Windows.Forms.Clipboard]::SetDataObject($d, $true, 10, 50)
}
function PutPicture([System.Drawing.Color]$c, [int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear($c); $g.Dispose()
    $d = New-Object System.Windows.Forms.DataObject; $d.SetData([System.Windows.Forms.DataFormats]::Bitmap, $bmp); Marked $d
}

# The clipboard as it was, each format that can be copied (text, a picture, files): put back after.
$saved = New-Object System.Windows.Forms.DataObject
$savedFormats = 0
$was = [System.Windows.Forms.Clipboard]::GetDataObject()
if ($was) {
    foreach ($f in $was.GetFormats($false)) {
        try { $v = $was.GetData($f, $false); if ($null -ne $v) { $saved.SetData($f, $v); $savedFormats++ } } catch { }
    }
}
$historyBefore = HistoryCount
Say "Win+V history: $historyBefore items; clipboard formats saved: $savedFormats"

$docPath = Join-Path $OutDir 'canvas.png'
$bmp = New-Object System.Drawing.Bitmap 600, 600; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::White); $g.Dispose(); $bmp.Save($docPath, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
$startedPs = -not (Get-Process Photoshop -ErrorAction SilentlyContinue)
$app = $null; $doc = $null; $probe = $null; $verdict = 'NOT RUN'
function Sample { $s = $doc.ColorSamplers.Add(@(300, 300)); $c = $s.Color.RGB; $r = "$([int]$c.Red),$([int]$c.Green),$([int]$c.Blue)"; $s.Delete(); $r }
try {
    PutPicture ([System.Drawing.Color]::Lime) 200 150
    Say "clipboard: GREEN (marked); starting Photoshop"
    Start-Process $Photoshop -ArgumentList "`"$docPath`""
    $deadline = (Get-Date).AddSeconds(150)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 1000
        try { $app = [Runtime.InteropServices.Marshal]::GetActiveObject('Photoshop.Application'); if ($app.Documents.Count -ge 1) { break } } catch { }
    }
    if (-not $app -or $app.Documents.Count -lt 1) { throw 'Photoshop did not open the document' }
    $doc = $app.ActiveDocument
    Start-Sleep -Seconds 3
    $hwnd = (Get-Process Photoshop | Select-Object -First 1).MainWindowHandle
    if ([U432]::GetForegroundWindow() -ne $hwnd) { [U432]::Front($hwnd) | Out-Null; Start-Sleep -Milliseconds 1500 }
    Say "Photoshop window $hwnd in front: $([U432]::GetForegroundWindow() -eq $hwnd); document centre before: $(Sample)"

    $env:WINSHUN_TEST_NO_CLIP_HISTORY = '1'
    $probeOut = Join-Path $OutDir 'probe'
    New-Item -ItemType Directory -Force $probeOut | Out-Null
    Say "probe: $WinShun --activation-paste-check $hwnd (red picture $PictureWidth wide)"
    $probeArgs = @('--probe-runtime', "`"$probeOut`"", '--activation-paste-check', ([int64]$hwnd), '--picture-width', $PictureWidth, '--settle-ms', '1500', '--renderer', 'd3d11')
    if ($ClickClipboard) { $probeArgs += '--click-first' }
    if ($CopyFirst) { $probeArgs += '--copy-first' }
    $probe = Start-Process $WinShun -ArgumentList $probeArgs `
        -PassThru -RedirectStandardError (Join-Path $OutDir 'probe-stderr.txt') -RedirectStandardOutput (Join-Path $OutDir 'probe-stdout.txt')
    $null = $probe.Handle # else ExitCode stays empty (Windows PowerShell)
    if (-not $probe.WaitForExit(90000)) { $probe.Kill(); throw 'the probe did not finish in 90 s' }
    $probeJson = Get-Content (Join-Path $probeOut 'activation-paste.json') -Raw -ErrorAction SilentlyContinue
    Say "probe exit $($probe.ExitCode): $(($probeJson -replace '\s+', ' '))"
    Start-Sleep -Milliseconds 800
    $centre = Sample
    $verdict = switch ($centre) { '255,0,0' { 'PASS: the RED picture chosen in the clipboard went in' } '0,255,0' { 'FAIL: Photoshop pasted the GREEN picture it read as it came to the front' } default { "FAIL: nothing pasted ($centre)" } }
    Say "document centre after: $centre -> $verdict"
}
catch { Say "ERROR: $($_.Exception.Message) (line $($_.InvocationInfo.ScriptLineNumber))" }
finally {
    if ($probe -and -not $probe.HasExited) { $probe.Kill() }
    try { if ($doc) { $doc.Close(2) } } catch { Say "close: $($_.Exception.Message)" }
    if ($startedPs -and $app) { try { $app.Quit() } catch { } }
    if ($savedFormats -gt 0) { Marked $saved } else { [System.Windows.Forms.Clipboard]::Clear() }
    Start-Sleep -Milliseconds 500
    $historyAfter = HistoryCount
    Say "Win+V history after: $historyAfter items (before $historyBefore)$(if ($historyAfter -ne $historyBefore) { ' CHANGED' })"
    $log | Set-Content -Encoding UTF8 (Join-Path $OutDir 'result.txt')
}

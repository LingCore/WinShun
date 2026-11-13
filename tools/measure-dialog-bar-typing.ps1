# Typing in the search bar under a file dialog, timed key by key with the real WinShun: a system Open dialog
# shown by a PowerShell started here, 6000 files made in a temporary folder to search through. For each key:
# how long until the field had the letter, the rows came, the frame with them was shown, the longest the
# UI thread was kept by one thing, and which event that was.
#
#   ./tools/measure-dialog-bar-typing.ps1 -BuildDir build/<task> -OutputDir build/<task>/typing          # the text set as the field sets it
#   ./tools/measure-dialog-bar-typing.ps1 -BuildDir build/<task> -OutputDir build/<task>/typing -Keys    # real keys (SendInput)
#
# The dialog must be able to come to the front: while someone is working in another program Windows refuses
# that, the bar never shows, and the run ends with dialog_bar_visible false. Keys are sent only while the
# bar's own window is in front. The engine alone, over the real index: wsbench --launcher-sim --scope files --each-key.
param([Parameter(Mandatory)][string]$BuildDir, [Parameter(Mandatory)][string]$OutputDir, [switch]$Keys,
    [string]$QtBin = 'C:\Qt\6.12.0\msvc2022_64\bin')
$ErrorActionPreference = 'Stop'
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$null = New-Item -ItemType Directory -Force $OutputDir
$Out = (Resolve-Path -LiteralPath $OutputDir).Path
$Extra = if ($Keys) { @('--file-dialog-keys') } else { @() }
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class T1 {
 public delegate bool P(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(P p, IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
 public static IntPtr Find(uint pid){ IntPtr r=IntPtr.Zero; EnumWindows((h,l)=>{uint p; GetWindowThreadProcessId(h,out p); var c=new StringBuilder(64); GetClassNameW(h,c,64); if(p==pid && IsWindowVisible(h) && c.ToString()=="#32770"){r=h; return false;} return true;},IntPtr.Zero); return r; }
}
'@
$fixture = Start-Process pwsh -ArgumentList '-NoProfile','-Command',"Add-Type -AssemblyName System.Windows.Forms; `$d = New-Object System.Windows.Forms.OpenFileDialog; `$d.InitialDirectory = 'C:\Windows'; [void]`$d.ShowDialog()" -PassThru -WindowStyle Hidden
try {
  $w=[IntPtr]::Zero; for($i=0;$i -lt 100 -and $w -eq [IntPtr]::Zero;$i++){ Start-Sleep -Milliseconds 100; $w=[T1]::Find($fixture.Id) }
  if ($w -eq [IntPtr]::Zero) { throw 'no dialog' }
  Start-Sleep 1
  $savedPath = $env:PATH; $env:PATH = "$QtBin;$env:PATH"
  Remove-Item "$Out\runtime" -Recurse -Force -ErrorAction SilentlyContinue
  $a = @('--probe-runtime',"`"$Out\runtime`"",'--renderer','d3d11','--settle-ms','700','--file-dialog-window',"$($w.ToInt64())",'--file-dialog-pid',"$($fixture.Id)",'--file-dialog-typing') + $Extra
  $r = Start-Process "$BuildDir\WinShun.exe" -ArgumentList $a -PassThru -Wait -WindowStyle Hidden -RedirectStandardError "$Out\err.txt"
  "exit $($r.ExitCode)"
  $j = Get-Content "$Out\runtime\runtime.json" -Raw | ConvertFrom-Json
  $j.dialog_typing | Format-Table query, set_query_ms, query_ms, rows_ms, frame_ms, longest_kept_ms, rows, height, slowest_event -AutoSize | Out-String -Width 220
  $j | Select-Object dialog_bar_visible, dialog_search_found, dialog_selection_confirmed, warnings | Format-List
} finally { if ($savedPath) { $env:PATH = $savedPath }; if(!$fixture.HasExited){ Stop-Process -Id $fixture.Id -Force } }


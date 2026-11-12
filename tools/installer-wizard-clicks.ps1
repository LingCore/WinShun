# Presses the wizard's buttons for tools/verify-installed-upgrade.ps1 -Mode Wizard. Run elevated (the wizard is):
# it looks for the Setup of Win顺 (an Inno Setup wizard, class TWizardForm, of a process named WinShun-*-setup*),
# and each time a page is up presses its Next / Install / Finish button with BM_CLICK, as a click does. A question
# of Setup's own (a dialog of the same process) is answered with its default button and written down. Nothing else
# on the screen is touched. Ends when the wizard is gone, or after -Seconds.
param([Parameter(Mandatory)] [string] $Log, [int] $Seconds = 240)
$ErrorActionPreference = 'Stop'
function Say([string] $text) { Add-Content -LiteralPath $Log -Value ("{0:HH:mm:ss} {1}" -f (Get-Date), $text) -Encoding UTF8 }
Add-Type @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class Wiz {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc p, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint ms, out IntPtr result);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dialog, int id);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  public static string Cls(IntPtr h) { var s = new StringBuilder(128); GetClassName(h, s, 128); return s.ToString(); }
  public static string Text(IntPtr h) { var s = new StringBuilder(512); GetWindowText(h, s, 512); return s.ToString(); }
  public static uint Pid(IntPtr h) { uint p; GetWindowThreadProcessId(h, out p); return p; }
  public static List<IntPtr> Top(uint pid) { var r = new List<IntPtr>(); EnumWindows((h, l) => { if (Pid(h) == pid && IsWindowVisible(h)) r.Add(h); return true; }, IntPtr.Zero); return r; }
  public static List<IntPtr> Kids(IntPtr parent) { var r = new List<IntPtr>(); EnumChildWindows(parent, (h, l) => { r.Add(h); return true; }, IntPtr.Zero); return r; }
  public static void Click(IntPtr button) { IntPtr x; SendMessageTimeout(button, 0x00F5, IntPtr.Zero, IntPtr.Zero, 2, 3000, out x); } // BM_CLICK
}
'@
$go = 'Next', 'Install', 'Finish', '下一步', '安装', '完成', '结束'
$until = (Get-Date).AddSeconds($Seconds)
$seen = $false
$lastPage = ''
$clicks = 0
Say 'looking for the wizard'
while ((Get-Date) -lt $until) {
    $setups = @(Get-Process | Where-Object { $_.ProcessName -like 'WinShun-*-setup*' })
    if (-not $setups.Count) { if ($seen) { Say 'the wizard is gone'; break }; Start-Sleep -Milliseconds 500; continue }
    foreach ($setup in $setups) {
        foreach ($window in [Wiz]::Top([uint32]$setup.Id)) {
            $class = [Wiz]::Cls($window)
            if ($class -eq 'TWizardForm') {
                $seen = $true
                # The page: its heading is the first visible static text that is not a button.
                $buttons = @([Wiz]::Kids($window) | Where-Object { [Wiz]::Cls($_) -eq 'TNewButton' -and [Wiz]::IsWindowVisible($_) -and [Wiz]::IsWindowEnabled($_) })
                $names = @($buttons | ForEach-Object { ([Wiz]::Text($_) -replace '&', '') -replace '\(.\)', '' -replace '\s*[>]$', '' })
                $page = ($names -join ' | ')
                $target = $null
                for ($i = 0; $i -lt $buttons.Count; $i++) { if ($go | Where-Object { $names[$i].Trim().StartsWith($_) }) { $target = $buttons[$i]; $targetName = $names[$i] } }
                if ($page -ne $lastPage) { Say "page with buttons: $page"; $lastPage = $page }
                if ($target -and $clicks -lt 12) {
                    Start-Sleep -Milliseconds 700
                    Say "pressed: $targetName"
                    [Wiz]::Click($target)
                    $clicks++
                    $lastPage = ''
                    Start-Sleep -Milliseconds 800
                }
            }
            elseif ($class -eq '#32770' -or $class -like 'TMessageForm*' -or $class -eq 'TaskDialog') {
                $texts = @([Wiz]::Kids($window) | ForEach-Object { [Wiz]::Text($_) } | Where-Object { $_ }) -join ' / '
                Say "a question of Setup's: [$([Wiz]::Text($window))] $texts"
                $ok = [Wiz]::GetDlgItem($window, 1) # IDOK: the default answer
                if ($ok -ne [IntPtr]::Zero) { Say 'answered with its default button'; [Wiz]::Click($ok) }
                Start-Sleep -Milliseconds 800
            }
        }
    }
    Start-Sleep -Milliseconds 400
}
Say "done ($clicks presses)"

param([long]$Window = 0, [ValidateRange(0, 30)][int]$DelaySeconds = 3, [string]$OutputFile = '')
$ErrorActionPreference = 'Stop'
# Read-only window structure; deliberately excludes titles, edit text and paths.
if (!('WinShun.DialogDiagnostic' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
namespace WinShun {
    public static class DialogDiagnostic {
        [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
        public delegate bool Visitor(IntPtr window, IntPtr data);
        [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
        [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
        [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
        [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
        [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr window);
        [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr window);
        [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr window, Visitor callback, IntPtr data);
        [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr window, StringBuilder text, int count);
        [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] static extern IntPtr GetWindowLongPtr(IntPtr window, int index);
        public static string ClassName(IntPtr window) { var text = new StringBuilder(256); GetClassNameW(window, text, text.Capacity); return text.ToString(); }
        public static string Style(IntPtr window) { return unchecked((uint)GetWindowLongPtr(window, -16).ToInt64()).ToString("X8"); }
        public static IntPtr[] Children(IntPtr window) {
            var result = new List<IntPtr>();
            EnumChildWindows(window, (child, data) => { result.Add(child); return true; }, IntPtr.Zero);
            return result.ToArray();
        }
    }
}
'@
}
if ($DelaySeconds) { Start-Sleep -Seconds $DelaySeconds }
$target = if ($Window) { [IntPtr]::new($Window) } else { [WinShun.DialogDiagnostic]::GetForegroundWindow() }
if (![WinShun.DialogDiagnostic]::IsWindow($target)) { throw 'The selected window no longer exists.' }
$processNumber = [uint32]0
$null = [WinShun.DialogDiagnostic]::GetWindowThreadProcessId($target, [ref]$processNumber)
$origin = [WinShun.DialogDiagnostic+Rect]::new()
$null = [WinShun.DialogDiagnostic]::GetWindowRect($target, [ref]$origin)
$controls = foreach ($child in [WinShun.DialogDiagnostic]::Children($target)) {
    $bounds = [WinShun.DialogDiagnostic+Rect]::new()
    $null = [WinShun.DialogDiagnostic]::GetWindowRect($child, [ref]$bounds)
    [ordered]@{
        window=$child.ToInt64().ToString('X'); parent=[WinShun.DialogDiagnostic]::GetParent($child).ToInt64().ToString('X');
        class=[WinShun.DialogDiagnostic]::ClassName($child); id=[WinShun.DialogDiagnostic]::GetDlgCtrlID($child);
        style=[WinShun.DialogDiagnostic]::Style($child); visible=[WinShun.DialogDiagnostic]::IsWindowVisible($child);
        enabled=[WinShun.DialogDiagnostic]::IsWindowEnabled($child);
        rect=@(($bounds.Left-$origin.Left), ($bounds.Top-$origin.Top), ($bounds.Right-$bounds.Left), ($bounds.Bottom-$bounds.Top))
    }
}
$report = [ordered]@{
    schema=1; process=(Get-Process -Id $processNumber).ProcessName; window=$target.ToInt64().ToString('X');
    class=[WinShun.DialogDiagnostic]::ClassName($target); controls=@($controls)
} | ConvertTo-Json -Depth 6
if ($OutputFile) { $report | Set-Content -LiteralPath $OutputFile -Encoding utf8 } else { $report }

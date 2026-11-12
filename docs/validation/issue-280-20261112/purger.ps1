# Elevated: when <dir>\purge.request appears, writes the modified pages out and empties the
# standby list (what Windows does while memory is short), then answers with purge.done.
# Ends when <dir>\stop appears, or after -Minutes.
param([Parameter(Mandatory)] [string] $Dir, [int] $Minutes = 40)
$ErrorActionPreference = 'Stop'
try {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Purger {
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct TOKEN_PRIVILEGES { public int Count; public long Luid; public int Attr; }
    [DllImport("ntdll.dll")] static extern int NtSetSystemInformation(int cls, ref int info, int len);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr h, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool LookupPrivilegeValue(string sys, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TOKEN_PRIVILEGES n, int len, IntPtr prev, IntPtr retLen);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    public static bool Privilege() {
        IntPtr token; long luid;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out token)) return false;
        if (!LookupPrivilegeValue(null, "SeProfileSingleProcessPrivilege", out luid)) return false;
        var tp = new TOKEN_PRIVILEGES { Count = 1, Luid = luid, Attr = 2 };
        return AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero) && Marshal.GetLastWin32Error() == 0;
    }
    public static string Purge() {
        int flush = 3, purge = 4; // MemoryFlushModifiedList, MemoryPurgeStandbyList
        int a = NtSetSystemInformation(80, ref flush, 4);
        int b = NtSetSystemInformation(80, ref purge, 4);
        return string.Format("flush=0x{0:X} purge=0x{1:X}", a, b);
    }
}
'@
    $priv = [Purger]::Privilege()
    Set-Content -LiteralPath (Join-Path $Dir 'purger.ready') -Value "priv=$priv pid=$PID"
    $until = (Get-Date).AddMinutes($Minutes)
    $count = 0
    while ((Get-Date) -lt $until -and -not (Test-Path -LiteralPath (Join-Path $Dir 'stop'))) {
        $request = Join-Path $Dir 'purge.request'
        if (Test-Path -LiteralPath $request) {
            Remove-Item -LiteralPath $request -Force
            $result = [Purger]::Purge()
            ++$count
            Add-Content -LiteralPath (Join-Path $Dir 'purger.log') -Value "$(Get-Date -Format T) $count $result"
            Set-Content -LiteralPath (Join-Path $Dir 'purge.done') -Value $result
        }
        Start-Sleep -Milliseconds 50
    }
} catch {
    Set-Content -LiteralPath (Join-Path $Dir 'purger.error') -Value $_.ToString()
} finally {
    Remove-Item -LiteralPath (Join-Path $Dir 'purger.ready') -Force -ErrorAction SilentlyContinue
}

<#
.SYNOPSIS
    The elevated helper of WinShun.exe --idle-open-check --idle-open-pressure <dir>.

.DESCRIPTION
    Each time <dir>\purge.request appears, writes Windows' modified pages out
    and empties the standby list, then answers with <dir>\purge.done: what a
    program gave back is then no longer in memory, as when other programs took
    it (GitHub #471, #500). The whole PC's file cache goes with it, so the
    machine is slow for a moment after each one. Ends when <dir>\stop appears,
    or after 20 minutes. Run it elevated, before the probe:

        Start-Process pwsh -Verb RunAs -WindowStyle Hidden -ArgumentList '-NoProfile','-File','tools\purge-standby.ps1','-Dir','<dir>'
#>param([Parameter(Mandatory)] [string] $Dir)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MemoryLists {
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    struct TokPriv1Luid { public int Count; public long Luid; public int Attr; }
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr h, int acc, out IntPtr tok);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool LookupPrivilegeValue(string host, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AdjustTokenPrivileges(IntPtr tok, bool disableAll, ref TokPriv1Luid newState, int len, IntPtr prev, IntPtr relen);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("ntdll.dll")] static extern int NtSetSystemInformation(int infoClass, ref int info, int length);
    public static void EnablePrivilege(string name) {
        IntPtr tok;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out tok)) throw new Exception("OpenProcessToken " + Marshal.GetLastWin32Error());
        TokPriv1Luid tp; tp.Count = 1; tp.Luid = 0; tp.Attr = 2;
        if (!LookupPrivilegeValue(null, name, out tp.Luid)) throw new Exception("LookupPrivilegeValue " + Marshal.GetLastWin32Error());
        if (!AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero) || Marshal.GetLastWin32Error() != 0)
            throw new Exception("AdjustTokenPrivileges " + Marshal.GetLastWin32Error());
    }
    // SystemMemoryListInformation = 80; 3 = MemoryFlushModifiedList, 4 = MemoryPurgeStandbyList
    public static int Command(int command) { return NtSetSystemInformation(80, ref command, 4); }
}
'@
[MemoryLists]::EnablePrivilege('SeProfileSingleProcessPrivilege')
$log = Join-Path $Dir 'purge.log'
"started $(Get-Date -Format o)" | Add-Content -LiteralPath $log
$until = (Get-Date).AddMinutes(20)
while ((Get-Date) -lt $until -and -not (Test-Path -LiteralPath (Join-Path $Dir 'stop'))) {
    $request = Join-Path $Dir 'purge.request'
    if (Test-Path -LiteralPath $request) {
        $flush = [MemoryLists]::Command(3)
        Start-Sleep -Milliseconds 1500 # the modified page writer takes a moment
        $purge = [MemoryLists]::Command(4)
        [IO.File]::Delete($request)
        "purged $(Get-Date -Format o) flush=0x$($flush.ToString('x')) purge=0x$($purge.ToString('x'))" | Add-Content -LiteralPath $log
        [IO.File]::WriteAllText((Join-Path $Dir 'purge.done'), 'done')
    }
    Start-Sleep -Milliseconds 100
}
"ended $(Get-Date -Format o)" | Add-Content -LiteralPath $log

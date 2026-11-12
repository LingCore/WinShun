// GitHub #265 A/B harness (C# 5, csc v4). Not part of the product.
//   ab toggle <pid> <cycles> <holdMs> <gapMs>  open/close the launcher by WM_COPYDATA "toggle",
//                                                time from the send to a new uncloaked window of <pid>
//   ab hotkey <pid> <cycles> <holdMs> <gapMs>  the same, opened by the hotkey Ctrl+F22 (SendInput)
//   ab spin <threads> <normal|above>             busy threads in that priority class until killed
//   ab empty <pid>                               empty its working set, as Windows' trimmer does
//   ab purge <outFile>                           (elevated) flush the modified list, purge standby
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

static class Ab
{
    struct RECT { public int L, T, R, B; }
    struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct TOKEN_PRIVILEGES { public int Count; public long Luid; public int Attr; }
    delegate bool EnumProc(IntPtr h, IntPtr l);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int v, int size);
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, ref COPYDATASTRUCT l, uint flags, uint timeout, out IntPtr result);
    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint p);
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("ntdll.dll")] static extern int NtSetSystemInformation(int cls, ref int info, int len);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr h, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool LookupPrivilegeValue(string sys, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TOKEN_PRIVILEGES n, int len, IntPtr prev, IntPtr retLen);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [StructLayout(LayoutKind.Sequential)]
    struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit, Size = 40)]
    struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("psapi.dll")] static extern bool EmptyWorkingSet(IntPtr h);
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_MEMORY_COUNTERS { public int cb; public uint PageFaultCount; public UIntPtr PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage; }
    [DllImport("psapi.dll")] static extern bool GetProcessMemoryInfo(IntPtr h, out PROCESS_MEMORY_COUNTERS c, int size);

    static int Main(string[] args)
    {
        if (args.Length >= 3 && args[0] == "spin") return Spin(int.Parse(args[1]), args[2]);
        if (args.Length >= 2 && args[0] == "empty") return Empty(uint.Parse(args[1]));
        if (args.Length >= 2 && args[0] == "purge") return Purge(args[1]);
        if (args.Length >= 5 && args[0] == "toggle") return Toggle(uint.Parse(args[1]), int.Parse(args[2]), int.Parse(args[3]), int.Parse(args[4]), false);
        if (args.Length >= 5 && args[0] == "hotkey") return Toggle(uint.Parse(args[1]), int.Parse(args[2]), int.Parse(args[3]), int.Parse(args[4]), true);
        if (args.Length >= 5 && args[0] == "tap") { tapMode = true; return Toggle(uint.Parse(args[1]), int.Parse(args[2]), int.Parse(args[3]), int.Parse(args[4]), true); }
        Console.Error.WriteLine("usage: ab toggle <pid> <cycles> <holdMs> <gapMs> | spin <n> | purge <out>");
        return 2;
    }

    static int Empty(uint pid)
    {
        IntPtr h = OpenProcess(0x0400 | 0x0100, false, pid); // QUERY_INFORMATION | SET_QUOTA
        bool ok = h != IntPtr.Zero && EmptyWorkingSet(h);
        if (h != IntPtr.Zero) CloseHandle(h);
        Console.WriteLine("emptied=" + ok);
        return ok ? 0 : 1;
    }

    static uint Faults(uint pid)
    {
        IntPtr h = OpenProcess(0x1000, false, pid); // QUERY_LIMITED_INFORMATION
        var c = new PROCESS_MEMORY_COUNTERS();
        bool ok = h != IntPtr.Zero && GetProcessMemoryInfo(h, out c, Marshal.SizeOf(typeof(PROCESS_MEMORY_COUNTERS)));
        if (h != IntPtr.Zero) CloseHandle(h);
        return ok ? c.PageFaultCount : 0;
    }

    static int Spin(int n, string cls)
    {
        Process.GetCurrentProcess().PriorityClass = cls == "above" ? ProcessPriorityClass.AboveNormal : ProcessPriorityClass.Normal;
        for (int i = 0; i < n; ++i) {
            var t = new Thread(() => { long x = 0; while (true) { x++; } });
            t.IsBackground = true;
            t.Start();
        }
        Thread.Sleep(Timeout.Infinite);
        return 0;
    }

    static bool Privilege(string name)
    {
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out token)) return false;
        var tp = new TOKEN_PRIVILEGES { Count = 1, Attr = 2 };
        if (!LookupPrivilegeValue(null, name, out tp.Luid)) return false;
        return AdjustTokenPrivileges(token, false, ref tp, Marshal.SizeOf(tp), IntPtr.Zero, IntPtr.Zero) && Marshal.GetLastWin32Error() == 0;
    }

    static int Purge(string outFile)
    {
        bool priv = Privilege("SeProfileSingleProcessPrivilege");
        int flush = 3, purge = 4;
        int a = NtSetSystemInformation(80, ref flush, 4);
        int b = NtSetSystemInformation(80, ref purge, 4);
        File.WriteAllText(outFile, string.Format("priv={0} flush=0x{1:X} purge=0x{2:X}", priv, a, b));
        return a == 0 && b == 0 ? 0 : 1;
    }

    static IntPtr MessageWindow(uint pid)
    {
        IntPtr h = IntPtr.Zero;
        while ((h = FindWindowEx(IntPtr.Zero, h, "WinShun.MessageWindow", null)) != IntPtr.Zero) {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner == pid) return h;
        }
        // message-only windows live under HWND_MESSAGE
        h = IntPtr.Zero;
        while ((h = FindWindowEx(new IntPtr(-3), h, "WinShun.MessageWindow", null)) != IntPtr.Zero) {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner == pid) return h;
        }
        return IntPtr.Zero;
    }

    static HashSet<IntPtr> Shown(uint pid)
    {
        var found = new HashSet<IntPtr>();
        EnumWindows((h, l) => {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner != pid || !IsWindowVisible(h)) return true;
            int cloaked;
            if (DwmGetWindowAttribute(h, 14, out cloaked, 4) == 0 && cloaked != 0) return true;
            RECT r;
            if (GetWindowRect(h, out r) && r.R - r.L >= 200 && r.B - r.T >= 60) found.Add(h);
            return true;
        }, IntPtr.Zero);
        return found;
    }

    static bool Send(IntPtr target, string command, out double ackMs, Stopwatch clock)
    {
        IntPtr text = Marshal.StringToHGlobalUni(command);
        try {
            var data = new COPYDATASTRUCT { dwData = new IntPtr(0x5753484E), cbData = (command.Length + 1) * 2, lpData = text };
            IntPtr result;
            double start = clock.Elapsed.TotalMilliseconds;
            IntPtr ok = SendMessageTimeout(target, 0x004A, IntPtr.Zero, ref data, 0, 15000, out result);
            ackMs = clock.Elapsed.TotalMilliseconds - start;
            return ok != IntPtr.Zero;
        } finally {
            Marshal.FreeHGlobal(text);
        }
    }

    static INPUT Key(ushort vk, bool up)
    {
        var i = new INPUT { type = 1 };
        i.ki.wVk = vk;
        i.ki.dwFlags = up ? 2u : 0u;
        return i;
    }

    static bool tapMode = false;

    static bool Send1(ushort vk, bool up)
    {
        var k = new[] { Key(vk, up) };
        return SendInput(1, k, Marshal.SizeOf(typeof(INPUT))) == 1;
    }

    // Double Shift, each tap held 60 ms (not "sent whole"); returns once the second is let go.
    static bool DoubleShift(Stopwatch clock, out double t0)
    {
        t0 = 0;
        if (!Send1(0xA0, false)) return false; Thread.Sleep(60);
        if (!Send1(0xA0, true)) return false; Thread.Sleep(100);
        if (!Send1(0xA0, false)) return false; Thread.Sleep(60);
        t0 = clock.Elapsed.TotalMilliseconds;
        return Send1(0xA0, true);
    }

    static bool PressHotkey()
    {
        var keys = new[] { Key(0x11, false), Key(0x85, false), Key(0x85, true), Key(0x11, true) }; // Ctrl+F22
        return SendInput((uint)keys.Length, keys, Marshal.SizeOf(typeof(INPUT))) == keys.Length;
    }

    static int Toggle(uint pid, int cycles, int holdMs, int gapMs, bool keys)
    {
        SetProcessDpiAwarenessContext(new IntPtr(-4));
        timeBeginPeriod(1);
        Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.High;
        Thread.CurrentThread.Priority = ThreadPriority.Highest;
        IntPtr target = MessageWindow(pid);
        if (target == IntPtr.Zero) { Console.WriteLine("no message window"); return 3; }
        var clock = Stopwatch.StartNew();
        for (int i = 0; i < cycles; ++i) {
            var before = Shown(pid);
            uint f0 = Faults(pid);
            double t0 = clock.Elapsed.TotalMilliseconds, ack;
            if (keys && tapMode) {
                ack = 0;
                double at;
                if (!DoubleShift(clock, out at)) { Console.WriteLine("SendInput failed"); return 4; }
                t0 = at;
            } else if (keys) {
                ack = 0;
                if (!PressHotkey()) { Console.WriteLine("SendInput failed"); return 4; }
            } else if (!Send(target, "toggle", out ack, clock)) { Console.WriteLine("send failed"); return 4; }
            IntPtr window = IntPtr.Zero;
            double seen = -1;
            while (clock.Elapsed.TotalMilliseconds - t0 < 15000) {
                foreach (var h in Shown(pid)) if (!before.Contains(h)) { window = h; break; }
                if (window != IntPtr.Zero) { seen = clock.Elapsed.TotalMilliseconds - t0; break; }
                Thread.Sleep(1);
            }
            Console.WriteLine(string.Format("open {0} ack_ms={1:F1} seen_ms={2:F1} faults={3}", i + 1, ack, seen, Faults(pid) - f0));
            Console.Out.Flush();
            if (window == IntPtr.Zero) return 5;
            if (Environment.GetEnvironmentVariable("AB_TYPE") == "1") {
                // A search, as when used: "exe" typed into the launcher (its own profile, no history kept).
                Thread.Sleep(250);
                uint front;
                GetWindowThreadProcessId(GetForegroundWindow(), out front);
                if (front != pid) Console.WriteLine("not in front: nothing typed");
                else foreach (ushort vk in new ushort[] { 0x45, 0x58, 0x45 }) {
                    var press = new[] { Key(vk, false), Key(vk, true) };
                    SendInput(2, press, Marshal.SizeOf(typeof(INPUT)));
                    Thread.Sleep(90);
                }
                Thread.Sleep(900);
            }
            Thread.Sleep(holdMs);
            double c0 = clock.Elapsed.TotalMilliseconds, cack;
            if (!Send(target, "toggle", out cack, clock)) { Console.WriteLine("send failed"); return 4; }
            while (clock.Elapsed.TotalMilliseconds - c0 < 5000 && Shown(pid).Contains(window)) Thread.Sleep(1);
            if (Shown(pid).Contains(window)) { Console.WriteLine("did not close"); return 6; }
            Thread.Sleep(gapMs);
        }
        return 0;
    }
}

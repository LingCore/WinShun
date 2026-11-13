// WinShun 搜索诊断：发给反馈“搜索变慢、结果列表变灰”的用户，双击运行。
// 不改动 Win顺：用已装版本的 --query / --dump 搜索几个词，记录每次搜索的
// 耗时、Win顺 的硬缺页（从硬盘读回内存）与内存，以及系统内存、CPU 和占用
// 最高的进程，结果写到桌面一个 txt。不收集文件名、搜索结果或设置。
// 构建：tools/search-probe/build.ps1（系统自带 .NET Framework 的 csc，C# 5）。
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using Microsoft.Win32;

[assembly: System.Reflection.AssemblyTitle("WinShun 搜索诊断")]
[assembly: System.Reflection.AssemblyProduct("WinShun")]
[assembly: System.Reflection.AssemblyVersion("1.0.0.0")]
[assembly: System.Reflection.AssemblyFileVersion("1.0.0.0")]

static class Program
{
    const string ToolVersion = "1.2";
    const int MaxDumps = 2; // 结果迟迟不出时给 Win顺 拍的转储（只含线程调用栈）
    static int dumpsTaken;
    static string dumpDir; // 转储放这里，最后和报告一起打包
    static readonly string[] Words = { "a", "sd", "re", "微信", "abc", "config", "zq", "test" };
    const int IdleSeconds = 45; // Win顺 交还内存在失去焦点 20 秒后（IdleTrim），留足余量

    // ---- native ----
    [DllImport("ntdll.dll")]
    static extern int NtQuerySystemInformation(int infoClass, IntPtr buffer, int length, out int returned);

    [StructLayout(LayoutKind.Sequential)]
    struct PERFORMANCE_INFORMATION
    {
        public int cb;
        public UIntPtr CommitTotal, CommitLimit, CommitPeak, PhysicalTotal, PhysicalAvailable, SystemCache,
            KernelTotal, KernelPaged, KernelNonpaged, PageSize;
        public int HandleCount, ProcessCount, ThreadCount;
    }
    [DllImport("psapi.dll", SetLastError = true)]
    static extern bool GetPerformanceInfo(out PERFORMANCE_INFORMATION info, int size);

    [StructLayout(LayoutKind.Sequential)]
    struct MEMORYSTATUSEX
    {
        public int dwLength, dwMemoryLoad;
        public ulong ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual,
            ullAvailExtendedVirtual;
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX status);

    [DllImport("kernel32.dll")]
    static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
    [DllImport("kernel32.dll")]
    static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern bool QueryFullProcessImageNameW(IntPtr process, int flags, StringBuilder name, ref int size);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string className, string title);
    [DllImport("user32.dll")]
    static extern int GetWindowThreadProcessId(IntPtr window, out int pid);
    [DllImport("user32.dll")]
    static extern IntPtr SendMessageTimeoutW(IntPtr window, int msg, IntPtr w, IntPtr l, int flags, int timeout, out IntPtr result);

    [DllImport("dbghelp.dll", SetLastError = true)]
    static extern bool MiniDumpWriteDump(IntPtr process, int pid, Microsoft.Win32.SafeHandles.SafeFileHandle file, int type,
        IntPtr exception, IntPtr userStream, IntPtr callback);

    // 小型转储：线程、调用栈、模块列表（MiniDumpNormal | WithThreadInfo | WithUnloadedModules）
    static string WriteDump(int pid, string label)
    {
        IntPtr h = OpenProcess(0x0400 | 0x0010 /* QUERY_INFORMATION | VM_READ */, false, pid);
        if (h == IntPtr.Zero)
            return "打不开 Win顺 进程（" + Marshal.GetLastWin32Error() + "）";
        try {
            string path = Path.Combine(dumpDir, "WinShun_" + label + "_" + DateTime.Now.ToString("HHmmss_fff") + ".dmp");
            using (var fs = new FileStream(path, FileMode.Create, FileAccess.ReadWrite, FileShare.None)) {
                if (!MiniDumpWriteDump(h, pid, fs.SafeFileHandle, 0x1000 | 0x20, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero))
                    return "转储失败（" + Marshal.GetLastWin32Error() + "）";
            }
            return Path.GetFileName(path);
        } finally {
            CloseHandle(h);
        }
    }

    // Win顺 界面线程的消息窗口：它多久回一个空消息 = 界面线程忙多久
    static IntPtr GuiWindow(int pid)
    {
        for (IntPtr h = IntPtr.Zero; (h = FindWindowExW(IntPtr.Zero, h, "WinShun.MessageWindow", null)) != IntPtr.Zero;) {
            int owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner == pid)
                return h;
        }
        return IntPtr.Zero;
    }

    // 在后台每 25 ms 发一次 WM_NULL，记下每次的回应时间
    class Pinger : IDisposable
    {
        readonly IntPtr window;
        readonly Thread thread;
        volatile bool stop;
        readonly List<double> times = new List<double>();
        public Pinger(IntPtr window)
        {
            this.window = window;
            thread = new Thread(Run);
            thread.IsBackground = true;
            if (window != IntPtr.Zero)
                thread.Start();
        }
        void Run()
        {
            var sw = new Stopwatch();
            while (!stop) {
                sw.Restart();
                IntPtr result;
                SendMessageTimeoutW(window, 0 /* WM_NULL */, IntPtr.Zero, IntPtr.Zero, 0, 10000, out result);
                double ms = sw.Elapsed.TotalMilliseconds;
                lock (times) times.Add(ms);
                Thread.Sleep(25);
            }
        }
        public void Dispose()
        {
            stop = true;
            if (thread.IsAlive)
                thread.Join(11000);
        }
        public double Max { get { lock (times) return times.Count == 0 ? -1 : times.Max(); } }
        public int Over(double ms) { lock (times) return times.Count(t => t >= ms); }
        public int Count { get { lock (times) return times.Count; } }
    }

    class Proc
    {
        public int Pid;
        public string Name;
        public long HardFaults, PageFaults, CpuTicks, WorkingSet, Private;
    }

    // SYSTEM_PROCESS_INFORMATION (x64): one list for every process, no handles needed.
    static Dictionary<int, Proc> Snapshot()
    {
        int size = 1 << 21;
        for (int attempt = 0; attempt < 8; ++attempt) {
            IntPtr buffer = Marshal.AllocHGlobal(size);
            try {
                int returned;
                int status = NtQuerySystemInformation(5, buffer, size, out returned);
                if (status == unchecked((int)0xC0000004)) { // STATUS_INFO_LENGTH_MISMATCH
                    size = Math.Max(size * 2, returned + (1 << 16));
                    continue;
                }
                if (status != 0)
                    throw new Exception("NtQuerySystemInformation 0x" + status.ToString("X8"));
                var all = new Dictionary<int, Proc>();
                long offset = 0;
                while (true) {
                    IntPtr p = new IntPtr(buffer.ToInt64() + offset);
                    int next = Marshal.ReadInt32(p, 0);
                    var proc = new Proc();
                    proc.HardFaults = (uint)Marshal.ReadInt32(p, 16);
                    proc.CpuTicks = Marshal.ReadInt64(p, 40) + Marshal.ReadInt64(p, 48); // user + kernel, 100 ns
                    int nameBytes = (ushort)Marshal.ReadInt16(p, 56);
                    IntPtr name = Marshal.ReadIntPtr(p, 64);
                    proc.Name = name == IntPtr.Zero ? "System Idle Process" : Marshal.PtrToStringUni(name, nameBytes / 2);
                    proc.Pid = (int)Marshal.ReadIntPtr(p, 80).ToInt64();
                    proc.PageFaults = (uint)Marshal.ReadInt32(p, 128);
                    proc.WorkingSet = Marshal.ReadIntPtr(p, 144).ToInt64();
                    proc.Private = Marshal.ReadIntPtr(p, 200).ToInt64();
                    all[proc.Pid] = proc;
                    if (next == 0)
                        break;
                    offset += next;
                }
                return all;
            } finally {
                Marshal.FreeHGlobal(buffer);
            }
        }
        throw new Exception("进程列表读取失败");
    }

    static string ImagePath(int pid)
    {
        IntPtr h = OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, false, pid);
        if (h == IntPtr.Zero)
            return null;
        try {
            var sb = new StringBuilder(32768);
            int len = sb.Capacity;
            return QueryFullProcessImageNameW(h, 0, sb, ref len) ? sb.ToString(0, len) : null;
        } finally {
            CloseHandle(h);
        }
    }

    static double Mb(long bytes) { return bytes / 1048576.0; }
    static string F0(double v) { return v.ToString("0"); }
    static string F1(double v) { return v.ToString("0.0"); }

    // ---- Win顺 ----
    static int RunWs(string exe, params string[] args)
    {
        var psi = new ProcessStartInfo(exe, string.Join(" ", args.Select(a => "\"" + a.Replace("\"", "\\\"") + "\"").ToArray()));
        psi.UseShellExecute = false;
        using (var p = Process.Start(psi)) {
            if (!p.WaitForExit(10000))
                return -1;
            return p.ExitCode;
        }
    }

    static Dictionary<string, object> Dump(string exe, string file)
    {
        try { if (File.Exists(file)) File.Delete(file); } catch { }
        RunWs(exe, "--dump", file);
        var sw = Stopwatch.StartNew();
        var json = new JavaScriptSerializer();
        json.MaxJsonLength = int.MaxValue;
        while (sw.ElapsedMilliseconds < 4000) {
            if (File.Exists(file)) {
                try {
                    var o = json.DeserializeObject(File.ReadAllText(file, Encoding.UTF8)) as Dictionary<string, object>;
                    if (o != null) {
                        object state;
                        if (o.TryGetValue("state", out state) && state is Dictionary<string, object>)
                            return (Dictionary<string, object>)state;
                        return o;
                    }
                } catch { } // still being written
            }
            Thread.Sleep(40);
        }
        return null;
    }

    static string Str(Dictionary<string, object> d, string key)
    {
        object v;
        return d != null && d.TryGetValue(key, out v) && v != null ? Convert.ToString(v) : "";
    }
    static bool Bool(Dictionary<string, object> d, string key)
    {
        object v;
        return d != null && d.TryGetValue(key, out v) && v is bool && (bool)v;
    }
    static double Num(Dictionary<string, object> d, string key)
    {
        object v;
        if (d == null || !d.TryGetValue(key, out v) || v == null)
            return -1;
        try { return Convert.ToDouble(v); } catch { return -1; }
    }

    class Row
    {
        public string Round, Word;
        public double EngineMs = -1, ShownMs = -1, WallMs, WsBefore, WsAfter, AvailMb, CpuMs, GuiMax = -1;
        public int GuiOver100;
        public long Hard, Soft;
        public bool Done;
        public string Dump; // 结果迟迟不出时拍的转储
    }

    // 列表的样子：结果数 + 前几行（只在内存里比较，不写进报告）
    static string Signature(Dictionary<string, object> state)
    {
        var sb = new StringBuilder(); // 只看前几行：内容匹配排在名字后面，不会让它变
        object rows;
        if (state != null && state.TryGetValue("rows", out rows) && rows is object[]) {
            foreach (var r in ((object[])rows).Take(5)) {
                var d = r as Dictionary<string, object>;
                sb.Append('|').Append(Str(d, "path"));
            }
        }
        return sb.ToString();
    }
    static string lastSignature = "";

    static double AvailMb()
    {
        var m = new MEMORYSTATUSEX();
        m.dwLength = Marshal.SizeOf(typeof(MEMORYSTATUSEX));
        return GlobalMemoryStatusEx(ref m) ? m.ullAvailPhys / 1048576.0 : -1;
    }

    static Row Measure(string exe, string file, int pid, string round, string word)
    {
        var row = new Row { Round = round, Word = word ?? "（对照：只读状态）" };
        Proc before;
        Snapshot().TryGetValue(pid, out before);
        row.AvailMb = AvailMb();
        var sw = Stopwatch.StartNew();
        Dictionary<string, object> state = null;
        using (var ping = new Pinger(GuiWindow(pid))) {
            if (word == null) {
                state = Dump(exe, file);
                row.Done = state != null;
            } else {
                RunWs(exe, "--query", word);
                while (sw.ElapsedMilliseconds < 6000) {
                    state = Dump(exe, file);
                    if (state != null && Str(state, "query") == word) {
                        string sig = Signature(state);
                        if (row.ShownMs < 0 && sig != lastSignature)
                            row.ShownMs = sw.Elapsed.TotalMilliseconds; // 新结果已在列表里（含本工具读状态的开销）
                        if ((forceDump || row.ShownMs < 0 && sw.ElapsedMilliseconds > 1500) && row.Dump == null && dumpsTaken < MaxDumps) {
                            ++dumpsTaken; // 1.5 秒还没出结果：拍下此刻各线程在做什么
                            row.Dump = WriteDump(pid, round.Substring(0, 1) + "_" + dumpsTaken);
                        }
                        if (!Bool(state, "searching")) {
                            row.Done = true;
                            break;
                        }
                    }
                    Thread.Sleep(30);
                }
                if (state != null && Str(state, "query") == word) {
                    if (row.ShownMs >= 0) // 新结果到了，这个数才是这个词的（否则还是上一个词的）
                        row.EngineMs = Num(state, "elapsedUs") / 1000.0;
                    lastSignature = Signature(state);
                }
            }
            sw.Stop();
            row.GuiMax = ping.Max;
            row.GuiOver100 = ping.Over(100);
        }
        row.WallMs = sw.Elapsed.TotalMilliseconds;
        Proc after;
        Snapshot().TryGetValue(pid, out after);
        if (before != null && after != null) {
            row.Hard = after.HardFaults - before.HardFaults;
            row.Soft = (after.PageFaults - before.PageFaults) - row.Hard;
            row.WsBefore = Mb(before.WorkingSet);
            row.WsAfter = Mb(after.WorkingSet);
            row.CpuMs = (after.CpuTicks - before.CpuTicks) / 10000.0;
        }
        return row;
    }

    static string Reg(string key, string name)
    {
        try {
            using (var k = Registry.LocalMachine.OpenSubKey(key)) {
                object v = k == null ? null : k.GetValue(name);
                return v == null ? "" : v.ToString().Trim();
            }
        } catch { return ""; }
    }

    static void Say(string text) { Console.WriteLine(text); }

    static bool quiet; // --quiet：不打开记事本、不等回车（自测用）
    static bool forceDump; // --force-dump：不等卡住就拍转储（自测用）

    static int Main(string[] args)
    {
        try { Console.OutputEncoding = Encoding.UTF8; } catch { }
        Console.Title = "Win顺 搜索诊断";
        var report = new StringBuilder();
        quiet = args.Contains("--quiet");
        forceDump = args.Contains("--force-dump");
        int outAt = Array.IndexOf(args, "--out");
        string desktop = outAt >= 0 && outAt + 1 < args.Length ? args[outAt + 1]
            : Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        string reportPath = Path.Combine(desktop, "WinShun搜索诊断_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".txt");
        string work = Path.Combine(Path.GetTempPath(), "WinShunSearchProbe_" + Process.GetCurrentProcess().Id);
        Directory.CreateDirectory(work);
        string dumpFile = Path.Combine(work, "state.json");
        dumpDir = Path.Combine(work, "dumps");
        Directory.CreateDirectory(dumpDir);
        DateTime started = DateTime.Now;
        try {
            Say("Win顺 搜索诊断 " + ToolVersion);
            Say("测试大约 2 分钟，期间搜索框会自己弹出、换词、隐藏几次。");
            Say("请不要打字或点击 Win顺 窗口，等待完成即可。");
            Say("");
            if (IntPtr.Size != 8) {
                Say("需要 64 位 Windows。");
                return Finish(report, null);
            }

            // Win顺 正在运行的那个
            var procs = Snapshot();
            Proc ws = null;
            string exe = null;
            foreach (var p in procs.Values.Where(p => string.Equals(p.Name, "WinShun.exe", StringComparison.OrdinalIgnoreCase))
                         .OrderByDescending(p => p.Private)) {
                string path = ImagePath(p.Pid);
                if (path != null) {
                    ws = p;
                    exe = path;
                    break;
                }
            }
            if (ws == null) {
                Say("没有找到正在运行的 Win顺。请先打开 Win顺，再运行本工具。");
                report.AppendLine("没有找到正在运行的 Win顺（WinShun.exe）。");
                return Finish(report, reportPath);
            }

            report.AppendLine("Win顺 搜索诊断 " + ToolVersion + "    " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
            report.AppendLine("说明：只含耗时、内存、CPU 数字、进程名和 4 个内容搜索开关，不含文件名、搜索结果或其他设置。");
            report.AppendLine();

            // ---- 系统 ----
            string nt = @"SOFTWARE\Microsoft\Windows NT\CurrentVersion";
            int build;
            int.TryParse(Reg(nt, "CurrentBuild"), out build);
            string product = Reg(nt, "ProductName");
            if (build >= 22000)
                product = product.Replace("Windows 10", "Windows 11");
            report.AppendLine("【系统】");
            report.AppendLine("Windows：" + product + " " + Reg(nt, "DisplayVersion") + "（" + build + "." + Reg(nt, "UBR") + "）");
            report.AppendLine("CPU：" + Reg(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0", "ProcessorNameString") + "，逻辑核心 " + Environment.ProcessorCount);
            var mem = new MEMORYSTATUSEX();
            mem.dwLength = Marshal.SizeOf(typeof(MEMORYSTATUSEX));
            GlobalMemoryStatusEx(ref mem);
            var perf = new PERFORMANCE_INFORMATION();
            GetPerformanceInfo(out perf, Marshal.SizeOf(typeof(PERFORMANCE_INFORMATION)));
            double page = (double)perf.PageSize.ToUInt64();
            report.AppendLine("物理内存：共 " + F0(mem.ullTotalPhys / 1048576.0) + " MB，可用 " + F0(mem.ullAvailPhys / 1048576.0)
                + " MB，占用 " + mem.dwMemoryLoad + "%");
            report.AppendLine("提交内存：已用 " + F0(perf.CommitTotal.ToUInt64() * page / 1048576) + " MB / 上限 "
                + F0(perf.CommitLimit.ToUInt64() * page / 1048576) + " MB（上限 − 物理内存 ≈ 页面文件 "
                + F0((perf.CommitLimit.ToUInt64() - perf.PhysicalTotal.ToUInt64()) * page / 1048576) + " MB）");
            report.AppendLine("系统缓存：" + F0(perf.SystemCache.ToUInt64() * page / 1048576) + " MB");

            Say("正在统计 CPU 和内存（3 秒）…");
            long idle0, kernel0, user0, idle1, kernel1, user1;
            GetSystemTimes(out idle0, out kernel0, out user0);
            var cpu0 = Snapshot();
            var clock = Stopwatch.StartNew();
            Thread.Sleep(3000);
            GetSystemTimes(out idle1, out kernel1, out user1);
            var cpu1 = Snapshot();
            double span = clock.Elapsed.TotalMilliseconds * 10000.0; // 100 ns
            long total = (kernel1 - kernel0) + (user1 - user0);
            double busy = total > 0 ? 100.0 * (total - (idle1 - idle0)) / total : -1;
            report.AppendLine("CPU 占用（3 秒平均）：" + F0(busy) + "%");
            Proc compression = cpu1.Values.FirstOrDefault(p => p.Name == "Memory Compression");
            if (compression != null)
                report.AppendLine("内存压缩（Memory Compression）：" + F0(Mb(compression.WorkingSet)) + " MB");
            report.AppendLine();

            report.AppendLine("【CPU 占用最高的进程（3 秒内，100% = 全部核心）】");
            foreach (var p in cpu1.Values.Where(p => p.Pid != 0 && cpu0.ContainsKey(p.Pid))
                         .Select(p => new { p.Name, Pct = 100.0 * (p.CpuTicks - cpu0[p.Pid].CpuTicks) / (span * Environment.ProcessorCount) })
                         .OrderByDescending(x => x.Pct).Take(10))
                report.AppendLine("  " + F1(p.Pct).PadLeft(5) + "%  " + p.Name);
            report.AppendLine();
            report.AppendLine("【内存占用最高的进程（私有内存 / 实际在内存中）】");
            foreach (var g in cpu1.Values.Where(p => p.Pid != 0).GroupBy(p => p.Name)
                         .Select(g => new { Name = g.Key, Count = g.Count(), Private = g.Sum(p => p.Private), Ws = g.Sum(p => p.WorkingSet) })
                         .OrderByDescending(x => x.Private).Take(12))
                report.AppendLine("  " + F0(Mb(g.Private)).PadLeft(6) + " MB / " + F0(Mb(g.Ws)).PadLeft(6) + " MB  " + g.Name
                    + (g.Count > 1 ? "（" + g.Count + " 个）" : ""));
            report.AppendLine();

            // ---- Win顺 ----
            string version = "";
            try { version = FileVersionInfo.GetVersionInfo(exe).ProductVersion; } catch { }
            ws = cpu1[ws.Pid];
            report.AppendLine("【Win顺】");
            report.AppendLine("版本：" + version + "，位置：" + (exe.StartsWith(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), StringComparison.OrdinalIgnoreCase) ? "安装版" : "其他（便携版？）"));
            report.AppendLine("WinShun.exe：私有内存 " + F0(Mb(ws.Private)) + " MB，实际在内存中 " + F0(Mb(ws.WorkingSet)) + " MB，开机以来硬缺页 " + ws.HardFaults);
            foreach (var p in cpu1.Values.Where(p => p.Name.StartsWith("WinShun", StringComparison.OrdinalIgnoreCase) && p.Pid != ws.Pid).OrderBy(p => p.Name))
                report.AppendLine("  " + p.Name + "：私有 " + F0(Mb(p.Private)) + " MB，在内存中 " + F0(Mb(p.WorkingSet)) + " MB");
            // 内容搜索相关开关（只读这几项）
            string exeDir = Path.GetDirectoryName(exe);
            string[] inis = {
                Path.Combine(exeDir, "WinShun.ini"), Path.Combine(exeDir, @"data\WinShun.ini"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), @"WinShun\WinShun.ini"),
            };
            string ini = inis.FirstOrDefault(File.Exists);
            if (ini != null) {
                var keys = new[] { "Enabled", "Index", "Documents", "IncludeSystemFolders" };
                var names = new[] { "内容搜索", "内容索引", "搜索文档", "包含系统文件夹" };
                var found = new Dictionary<string, string>();
                string section = "";
                foreach (string line in ReadShared(ini)) {
                    string t = line.Trim();
                    if (t.StartsWith("[")) { section = t; continue; }
                    int eq = t.IndexOf('=');
                    if (section == "[Content]" && eq > 0 && keys.Contains(t.Substring(0, eq).Trim()))
                        found[t.Substring(0, eq).Trim()] = t.Substring(eq + 1).Trim();
                }
                report.AppendLine("设置：" + string.Join("，", keys.Select((k, i) => names[i] + " " + (found.ContainsKey(k) ? found[k] : "默认")).ToArray()));
            }
            IntPtr gui = GuiWindow(ws.Pid);
            if (gui != IntPtr.Zero) {
                using (var ping = new Pinger(gui)) {
                    Thread.Sleep(3000);
                    ping.Dispose();
                    report.AppendLine("界面线程（不搜索时 3 秒）：最长 " + F0(ping.Max) + " ms 才回应，≥100 ms 的 " + ping.Over(100) + " 次 / 共 " + ping.Count + " 次");
                }
            } else {
                report.AppendLine("（没找到 Win顺 的消息窗口，界面线程测不了）");
            }
            try { // 读数自检：与 .NET 读到的工作集比较
                long check = Process.GetProcessById(ws.Pid).WorkingSet64;
                if (Math.Abs(check - ws.WorkingSet) > Math.Max(check, ws.WorkingSet) / 5)
                    report.AppendLine("（注意：内存读数自检不一致 " + F0(Mb(check)) + " / " + F0(Mb(ws.WorkingSet)) + " MB）");
            } catch { }
            report.AppendLine();

            // ---- 搜索 ----
            var rows = new List<Row>();
            Say("第 1 轮：连续搜索 " + Words.Length + " 个词…");
            RunWs(exe, "--query", "zzqqxxwwvvjj"); // 先打开搜索框，排除第一次打开窗口的时间
            Thread.Sleep(1500);
            lastSignature = Signature(Dump(exe, dumpFile));
            rows.Add(Measure(exe, dumpFile, ws.Pid, "对照", null));
            foreach (var w in Words) {
                var r = Measure(exe, dumpFile, ws.Pid, "1 连续", w);
                rows.Add(r);
                Say("  " + w + "  " + (r.EngineMs >= 0 ? F0(r.EngineMs) + " ms" : "未取到") + "，硬缺页 " + r.Hard);
            }
            rows.Add(Measure(exe, dumpFile, ws.Pid, "对照", null));

            Say("第 2 轮：隐藏搜索框，空闲 " + IdleSeconds + " 秒（模拟过一会儿再搜）…");
            var st = Dump(exe, dumpFile);
            if (Bool(st, "visible"))
                RunWs(exe, "--toggle");
            for (int s = IdleSeconds; s > 0; s -= 5) {
                Console.Write("\r  还剩 " + s + " 秒   ");
                Thread.Sleep(5000);
            }
            Console.WriteLine();
            Proc idleBefore = Snapshot()[ws.Pid];
            foreach (var w in Words) {
                var r = Measure(exe, dumpFile, ws.Pid, "2 空闲后", w);
                rows.Add(r);
                Say("  " + w + "  " + (r.EngineMs >= 0 ? F0(r.EngineMs) + " ms" : "未取到") + "，硬缺页 " + r.Hard);
            }
            st = Dump(exe, dumpFile);
            if (Bool(st, "visible"))
                RunWs(exe, "--toggle"); // 收起，回到测试前的样子

            report.AppendLine("【搜索测试】");
            report.AppendLine("空闲 " + IdleSeconds + " 秒后 Win顺 实际在内存中：" + F0(Mb(idleBefore.WorkingSet)) + " MB（第 1 轮结束时 "
                + F1(rows.Where(r => r.Round == "1 连续").Select(r => r.WsAfter).LastOrDefault()) + " MB）");
            report.AppendLine("搜索耗时 = Win顺 自己记的名字搜索时间；结果出现 = 从发出搜索到列表换成新结果（含本工具读状态的开销，约 50–200 ms）。");
            report.AppendLine("界面卡顿 = 期间 Win顺 界面线程最长多久没回应 / ≥100 ms 的次数；内容未完 = 6 秒内文件内容还没搜完。");
            report.AppendLine("硬缺页 = 搜索期间要从硬盘读回内存的次数（每次约 4 KB 或更多）。");
            report.AppendLine();
            report.AppendLine("轮次      词          搜索耗时ms  结果出现ms  界面卡顿ms  总等待ms  硬缺页   软缺页  Win顺CPUms  内存前→后MB      系统可用MB");
            foreach (var r in rows)
                report.AppendLine(r.Round.PadRight(8) + "  " + PadWide(r.Word, 12) + (r.EngineMs >= 0 ? F1(r.EngineMs) : "-").PadLeft(10)
                    + (r.ShownMs >= 0 ? F0(r.ShownMs) : "-").PadLeft(12)
                    + ((r.GuiMax >= 0 ? F0(r.GuiMax) : "-") + "/" + r.GuiOver100).PadLeft(12)
                    + F0(r.WallMs).PadLeft(10) + r.Hard.ToString().PadLeft(8) + r.Soft.ToString().PadLeft(9) + F0(r.CpuMs).PadLeft(11)
                    + ("  " + F0(r.WsBefore) + "→" + F0(r.WsAfter)).PadRight(17) + F0(r.AvailMb).PadLeft(10) + (r.Done ? "" : "  内容未完")
                    + (r.Dump != null ? "  转储 " + r.Dump : ""));
            report.AppendLine("（搜索耗时为“-”：6 秒内新结果没出现，Win顺 记的还是上一个词的数字，不算。）");
            report.AppendLine();

            // ---- 初步判断 ----
            var all = rows.Where(r => r.Round != "对照").ToList();
            var searches = all.Where(r => r.EngineMs >= 0).ToList();
            report.AppendLine("【初步判断】");
            int late = all.Count(r => r.ShownMs < 0 || r.ShownMs > 1000);
            if (late > 0)
                report.AppendLine(all.Count + " 次搜索里有 " + late + " 次新结果超过 1 秒才出现（或 6 秒内没出现），"
                    + "而名字搜索本身只要 " + F0(Median(searches.Select(r => r.EngineMs))) + " ms：搜索在开始前被挡住了。");
            if (searches.Count == 0) {
                report.AppendLine("没有取到这次搜索的耗时（6 秒内新结果都没出现，或测试期间有人打字）。");
            } else {
                var slow = searches.Where(r => r.EngineMs >= 200).ToList();
                double controlHard = rows.Where(r => r.Round == "对照").Select(r => (double)r.Hard).DefaultIfEmpty(0).Max();
                var firstAfterIdle = rows.FirstOrDefault(r => r.Round == "2 空闲后");
                if (firstAfterIdle != null)
                    report.AppendLine("空闲后第一次搜索拿回内存约 " + F0((firstAfterIdle.Hard + firstAfterIdle.Soft) * 4 / 1024.0) + " MB，其中硬缺页 "
                        + firstAfterIdle.Hard + " 次（软缺页从内存里拿回，很快；硬缺页要读硬盘，慢），耗时 " + F0(firstAfterIdle.EngineMs) + " ms");
                report.AppendLine("搜索耗时中位数：第 1 轮 " + F0(Median(searches.Where(r => r.Round == "1 连续").Select(r => r.EngineMs)))
                    + " ms，第 2 轮 " + F0(Median(searches.Where(r => r.Round == "2 空闲后").Select(r => r.EngineMs))) + " ms（正常电脑约 10–30 ms）");
                var shown = all.Where(r => r.ShownMs >= 0).ToList();
                if (shown.Count > 0)
                    report.AppendLine("结果出现中位数 " + F0(Median(shown.Select(r => r.ShownMs))) + " ms，最长 " + F0(shown.Max(r => r.ShownMs))
                        + " ms；界面线程最长卡 " + F0(all.Max(r => r.GuiMax)) + " ms");
                if (all.Count(r => r.GuiMax >= 200 && r.Dump == null) >= 2) // 拍转储会暂停 Win顺 一下，那几行不算
                    report.AppendLine("搜索期间 Win顺 界面线程多次被卡住 ≥200 ms：结果算好了也要等界面线程空下来才显示。");
                else
                    report.AppendLine("界面线程基本不卡（≥200 ms 的不到 2 次），不是界面的问题。");
                if (all.Count(r => !r.Done) > all.Count / 2)
                    report.AppendLine("多数搜索 6 秒内文件内容还没搜完（“全部”会接着搜文件内容），期间 Win顺 CPU 占用很高。");
                if (slow.Count == 0) {
                    report.AppendLine("名字搜索本身都不慢（都在 200 ms 以内）。");
                } else {
                    double slowHard = Median(slow.Select(r => (double)r.Hard));
                    report.AppendLine(slow.Count + " 次搜索超过 200 ms。");
                    if (slowHard > Math.Max(300, controlHard * 3))
                        report.AppendLine("慢的搜索伴随大量硬缺页（中位数 " + F0(slowHard) + "）：Win顺 的索引被挤出内存，搜索时要从硬盘读回——内存不足造成。");
                    else if (Median(slow.Select(r => r.CpuMs)) < Median(slow.Select(r => r.EngineMs)) * 0.3)
                        report.AppendLine("慢的搜索没有明显硬缺页，Win顺 自己也没怎么用 CPU：多半在等 CPU（被其他程序占满）或等索引更新。");
                    else
                        report.AppendLine("慢的搜索没有明显硬缺页，Win顺 在用 CPU 计算：可能是 CPU 太忙或被降频。");
                }
            }
            report.AppendLine();

            // ---- 日志里的相关行（不含文件路径） ----
            // 便携版的日志在程序旁边：几个位置里挑最近写过的那份
            string[] logs = {
                Path.Combine(exeDir, @"data\WinShun.log"), Path.Combine(exeDir, "WinShun.log"),
                Path.Combine(exeDir, @"data\logs\WinShun.log"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"WinShun\WinShun.log"),
            };
            string logFile = logs.Where(File.Exists).OrderByDescending(f => File.GetLastWriteTime(f)).FirstOrDefault();
            if (logFile == null) {
                foreach (var dir in new[] { exeDir, Path.Combine(exeDir, "data") }.Where(Directory.Exists)) {
                    logFile = Directory.GetFiles(dir, "WinShun.log", SearchOption.AllDirectories)
                        .OrderByDescending(f => File.GetLastWriteTime(f)).FirstOrDefault();
                    if (logFile != null) break;
                }
            }
            if (logFile != null) {
                // 这几种行不含路径和搜索词：只有毫秒数和在做哪一步
                var wanted = ReadShared(logFile).Where(l => l.Contains("search took") || l.Contains("content scan took")
                    || l.Contains("Idle trim")).ToList();
                var slowLines = wanted.Where(l => !l.Contains("Idle trim")).ToList();
                report.AppendLine("【日志：搜索慢、内容扫描停得慢（最近 30 条）】  " + (logFile.StartsWith(exeDir, StringComparison.OrdinalIgnoreCase) ? "程序旁的日志" : "AppData 的日志")
                    + "，最后写入 " + File.GetLastWriteTime(logFile).ToString("MM-dd HH:mm"));
                foreach (var l in slowLines.Skip(Math.Max(0, slowLines.Count - 30)))
                    report.AppendLine("  " + l);
                if (slowLines.Count == 0)
                    report.AppendLine("  （没有）");
                string since = started.AddMinutes(-1).ToString("yyyy-MM-ddTHH:mm");
                var during = wanted.Where(l => l.Length > 16 && string.CompareOrdinal(l.Substring(0, 16), since) >= 0 && l.Contains("Idle trim")).ToList();
                if (during.Count > 0) {
                    report.AppendLine("  测试期间交还内存：");
                    foreach (var l in during)
                        report.AppendLine("  " + l);
                }
                report.AppendLine();
            } else {
                report.AppendLine("（没找到 Win顺 的日志）");
            }
            Say("");
            return Finish(report, reportPath);
        } catch (Exception e) {
            report.AppendLine();
            report.AppendLine("出错：" + e);
            Say("出错：" + e.Message);
            return Finish(report, reportPath);
        } finally {
            try { Directory.Delete(work, true); } catch { }
        }
    }

    static IEnumerable<string> ReadShared(string path)
    {
        using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
        using (var reader = new StreamReader(fs, Encoding.UTF8)) {
            var all = new List<string>();
            string line;
            while ((line = reader.ReadLine()) != null)
                all.Add(line);
            return all;
        }
    }

    static double Median(IEnumerable<double> values)
    {
        var v = values.OrderBy(x => x).ToList();
        if (v.Count == 0) return -1;
        return v.Count % 2 == 1 ? v[v.Count / 2] : (v[v.Count / 2 - 1] + v[v.Count / 2]) / 2;
    }

    // 中文按两格宽对齐
    static string PadWide(string s, int width)
    {
        int w = s.Sum(c => c > 0x2E80 ? 2 : 1);
        return s + new string(' ', Math.Max(1, width - w));
    }

    static int Finish(StringBuilder report, string path)
    {
        if (path != null && report.Length > 0) {
            File.WriteAllText(path, report.ToString(), new UTF8Encoding(true));
            string send = path;
            var dumps = dumpDir != null && Directory.Exists(dumpDir) ? Directory.GetFiles(dumpDir, "*.dmp") : new string[0];
            if (dumps.Length > 0) { // 报告和转储打成一个包
                send = Path.ChangeExtension(path, ".zip");
                using (var zip = System.IO.Compression.ZipFile.Open(send, System.IO.Compression.ZipArchiveMode.Create)) {
                    System.IO.Compression.ZipFileExtensions.CreateEntryFromFile(zip, path, Path.GetFileName(path));
                    foreach (var d in dumps)
                        System.IO.Compression.ZipFileExtensions.CreateEntryFromFile(zip, d, Path.GetFileName(d));
                }
            }
            Say("完成。结果已保存到桌面：");
            Say(send);
            Say("请把这个文件发给 Win顺 作者。");
            if (dumps.Length > 0)
                Say("（压缩包里的 .dmp 是 Win顺 当时的线程快照，用来查它卡在哪；可能含少量内存里的文字片段。）");
            if (!quiet)
                try { Process.Start("notepad.exe", "\"" + path + "\""); } catch { }
        }
        if (quiet)
            return 0;
        Say("");
        Say("按回车键关闭…");
        Console.ReadLine();
        return 0;
    }
}

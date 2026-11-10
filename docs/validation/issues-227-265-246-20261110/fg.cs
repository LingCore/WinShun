// GitHub #265 experiment: which of SetForegroundWindow / BringWindowToTop / SetFocus waits
// under a load of threads above normal priority, and when the foreground actually changes.
//   fg <cycles> [textbox]
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

static class Fg
{
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr SetFocus(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool AllowSetForegroundWindow(uint pid);
    [DllImport("kernel32.dll")] static extern uint GetCurrentProcessId();
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentThread();
    [DllImport("kernel32.dll")] static extern bool SetThreadPriority(IntPtr h, int p);
    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint p);
    [StructLayout(LayoutKind.Explicit, Size = 40)]
    struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(32)] public IntPtr extra; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int size);

    static volatile IntPtr watched = IntPtr.Zero;
    static IntPtr other = IntPtr.Zero;
    static volatile int switchedAtTick = -1;

    [STAThread]
    static int Main(string[] args)
    {
        int cycles = int.Parse(args[0]);
        bool textbox = args.Length > 1 && args[1] == "textbox";
        timeBeginPeriod(1);
        var form = new Form { Text = "wsfg experiment", Width = 400, Height = 200, StartPosition = FormStartPosition.Manual, Left = 50, Top = 50, ShowInTaskbar = false };
        if (textbox) form.Controls.Add(new TextBox { Left = 10, Top = 10, Width = 300 });
        var clock = Stopwatch.StartNew();
        other = GetForegroundWindow(); // the user's window in front, before ours
        var watcher = new Thread(() => {
            SetThreadPriority(GetCurrentThread(), 15);
            while (true) {
                IntPtr w = watched;
                if (w != IntPtr.Zero && switchedAtTick < 0 && GetForegroundWindow() == w) switchedAtTick = (int)clock.ElapsedMilliseconds;
                Thread.Sleep(1);
            }
        });
        watcher.IsBackground = true;
        watcher.Start();
        form.Shown += (s, e) => {
            var t = new System.Windows.Forms.Timer { Interval = 1500 };
            int done = 0;
            t.Tick += (s2, e2) => {
                if (GetForegroundWindow() == form.Handle) { SetForegroundWindow(other); return; } // the other window gets it back
                other = GetForegroundWindow();
                SetThreadPriority(GetCurrentThread(), 15); // as OpeningBoost raises Win顺's GUI thread
                var nothing = new[] { new INPUT { type = 0, extra = IntPtr.Zero } };
                if (!AllowSetForegroundWindow(GetCurrentProcessId())) SendInput(1, nothing, Marshal.SizeOf(typeof(INPUT)));
                switchedAtTick = -1;
                watched = form.Handle;
                long t0 = clock.ElapsedMilliseconds;
                SetForegroundWindow(form.Handle);
                long t1 = clock.ElapsedMilliseconds;
                BringWindowToTop(form.Handle);
                long t2 = clock.ElapsedMilliseconds;
                SetFocus(form.Handle);
                long t3 = clock.ElapsedMilliseconds;
                SetThreadPriority(GetCurrentThread(), 0);
                Console.WriteLine(string.Format("cycle {0}: SetForegroundWindow {1} ms, BringWindowToTop {2} ms, SetFocus {3} ms, foreground switched at +{4} ms, front={5}",
                    done + 1, t1 - t0, t2 - t1, t3 - t2, switchedAtTick < 0 ? -1 : switchedAtTick - t0, GetForegroundWindow() == form.Handle));
                if (++done >= cycles) { t.Stop(); form.Close(); }
            };
            t.Start();
        };
        Application.Run(form);
        return 0;
    }
}

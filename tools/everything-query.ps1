<#
.SYNOPSIS
    Asks whatever answers Everything's IPC on this PC (Everything itself, or
    Win顺 with 设置 › 系统整合 › 文件管理器 › “让其他软件把 Win顺 当作 Everything 搜索”)
    one query, as a program built on the Everything SDK does, and prints the
    rows: path\name, folders marked [dir].
.DESCRIPTION
    Sends QUERY2W (WM_COPYDATA, dwData 18) with what EverythingSearchClient
    0.9's SearchClient.Search(query) sends (GitHub #399): search flags 0, all
    results, NAME|PATH|SIZE|DATE_CREATED|DATE_MODIFIED|ATTRIBUTES, sort 0;
    the answer (EVERYTHING_IPC_LIST2) comes back to a message-only window.
    Our own code, no Everything SDK or library loaded. Read-only.
.EXAMPLE
    ./tools/everything-query.ps1 'file:"D:\images" ext:jpg;png'
#>
param([Parameter(Mandatory)][string]$Query, [int]$TimeoutMs = 15000)
$ErrorActionPreference = 'Stop'
if (-not ('EvQuery' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class EvQuery {
    const uint WM_COPYDATA = 0x004A;
    const uint QUERY2W = 18;
    const uint REPLY = 0x5157; // any id: our own reply message
    [StructLayout(LayoutKind.Sequential)] struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }
    [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam; public uint time; public int x; public int y; }
    delegate IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WNDCLASSW { public uint style; public WndProc lpfnWndProc; public int cbClsExtra; public int cbWndExtra; public IntPtr hInstance; public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground; public string lpszMenuName; public string lpszClassName; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern ushort RegisterClassW(ref WNDCLASSW c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateWindowExW(uint ex, string cls, string name, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr DefWindowProcW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowW(string cls, string name);
    [DllImport("user32.dll")] static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, ref COPYDATASTRUCT l);
    [DllImport("user32.dll")] static extern bool PeekMessageW(out MSG m, IntPtr h, uint min, uint max, uint remove);
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG m);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessageW(ref MSG m);
    [DllImport("user32.dll")] static extern uint MsgWaitForMultipleObjects(uint n, IntPtr[] h, bool all, uint ms, uint mask);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string n);

    static WndProc proc = Receive;
    static bool registered;
    static List<string> rows;
    static uint total;

    static IntPtr Receive(IntPtr h, uint m, IntPtr w, IntPtr l) {
        if (m == WM_COPYDATA) {
            var cd = Marshal.PtrToStructure<COPYDATASTRUCT>(l);
            if ((uint)cd.dwData.ToInt64() == REPLY) {
                var data = new byte[cd.cbData];
                Marshal.Copy(cd.lpData, data, 0, cd.cbData);
                Parse(data);
                return (IntPtr)1;
            }
        }
        return DefWindowProcW(h, m, w, l);
    }

    static string Text(byte[] d, ref int at) {
        int length = BitConverter.ToInt32(d, at); at += 4;
        string s = Encoding.Unicode.GetString(d, at, length * 2); at += (length + 1) * 2;
        return s;
    }

    static void Parse(byte[] d) {
        // EVERYTHING_IPC_LIST2: totitems, numitems, offset, request_flags, sort_type; ITEM2 {flags, data_offset}[]
        total = BitConverter.ToUInt32(d, 0);
        uint count = BitConverter.ToUInt32(d, 4);
        var list = new List<string>();
        for (int i = 0; i < count; ++i) {
            uint flags = BitConverter.ToUInt32(d, 20 + i * 8);
            int at = (int)BitConverter.ToUInt32(d, 24 + i * 8);
            string name = Text(d, ref at); // NAME first, then PATH
            string path = Text(d, ref at);
            list.Add(((flags & 1) != 0 ? "[dir] " : "") + path + "\\" + name);
        }
        rows = list;
    }

    public static string[] Run(string query, int timeoutMs, out uint totalItems) {
        IntPtr everything = FindWindowW("EVERYTHING_TASKBAR_NOTIFICATION", null);
        if (everything == IntPtr.Zero) throw new InvalidOperationException("no Everything IPC window");
        if (!registered) {
            var c = new WNDCLASSW { lpfnWndProc = proc, hInstance = GetModuleHandleW(null), lpszClassName = "EvQueryReply" };
            RegisterClassW(ref c);
            registered = true;
        }
        IntPtr me = CreateWindowExW(0, "EvQueryReply", "", 0, 0, 0, 0, 0, new IntPtr(-3), IntPtr.Zero, GetModuleHandleW(null), IntPtr.Zero);
        rows = null;
        try {
            // EVERYTHING_IPC_QUERY2: reply_hwnd, reply_copydata_message, search_flags, offset, max_results, request_flags, sort_type, then the text
            byte[] words = Encoding.Unicode.GetBytes(query + "\0");
            byte[] q = new byte[28 + words.Length];
            BitConverter.GetBytes((uint)me.ToInt64()).CopyTo(q, 0);
            BitConverter.GetBytes(REPLY).CopyTo(q, 4);
            BitConverter.GetBytes(0u).CopyTo(q, 8);
            BitConverter.GetBytes(0u).CopyTo(q, 12);
            BitConverter.GetBytes(0xFFFFFFFFu).CopyTo(q, 16);
            BitConverter.GetBytes(0x1u | 0x2u | 0x10u | 0x20u | 0x40u | 0x100u).CopyTo(q, 20);
            BitConverter.GetBytes(0u).CopyTo(q, 24);
            words.CopyTo(q, 28);
            IntPtr buffer = Marshal.AllocHGlobal(q.Length);
            try {
                Marshal.Copy(q, 0, buffer, q.Length);
                var cd = new COPYDATASTRUCT { dwData = (IntPtr)QUERY2W, cbData = q.Length, lpData = buffer };
                if (SendMessageW(everything, WM_COPYDATA, me, ref cd) == IntPtr.Zero) throw new InvalidOperationException("query refused");
            } finally { Marshal.FreeHGlobal(buffer); }
            var until = Environment.TickCount64 + timeoutMs;
            while (rows == null && Environment.TickCount64 < until) {
                MsgWaitForMultipleObjects(0, null, false, 50, 0x04FF);
                MSG m;
                while (PeekMessageW(out m, IntPtr.Zero, 0, 0, 1)) { TranslateMessage(ref m); DispatchMessageW(ref m); }
            }
            if (rows == null) throw new TimeoutException("no answer");
            totalItems = total;
            return rows.ToArray();
        } finally { DestroyWindow(me); }
    }
}
'@
}
$total = [uint32]0
$rows = [EvQuery]::Run($Query, $TimeoutMs, [ref]$total)
"query: $Query"
"total: $total"
$rows | ForEach-Object { "  $_" }

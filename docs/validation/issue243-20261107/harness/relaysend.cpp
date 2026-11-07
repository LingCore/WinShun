// Issue #243 live sender for tst_keylistener::elevatedForegroundPlainHotkey.
//   window <title> <vkHex> <log>   an elevated window that logs the key's
//                                  WM_KEYDOWN / WM_KEYUP it gets (120 s)
//   send <ready> <done> <vkHex> <log> <elevTitle> <testTitle> <missingExe>
//                                  (System IL in the user's session, via
//                                  hkprobe sys-launch): waits for <ready>,
//                                  fronts the elevated window and presses the
//                                  key 3 times, the test's window once,
//                                  csrss's error box once, then sets <done>.
#include <windows.h>
#include <cstdio>
#include <share.h>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

static FILE* g_log = nullptr;
static UINT g_vk = 0;
static void logf(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(g_log, "%llu %s\n", (unsigned long long)GetTickCount64(), buf);
    fflush(g_log);
}

static LRESULT CALLBACK wproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && w == g_vk)
        logf("window got %s vk=0x%x", (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) ? "down" : "up", (unsigned)w);
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

static int window(const wchar_t* title)
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = wproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RelaySendWindow";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 260, 260, 420, 160, nullptr, nullptr,
        wc.hInstance, nullptr);
    logf("window up %p", (void*)h);
    SetTimer(h, 77, 120000, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == h && msg.wParam == 77) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    logf("window exit");
    return 0;
}

static void key(WORD vk, bool up)
{
    INPUT in {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}

static void tapKey(WORD vk)
{
    key(vk, false);
    Sleep(30);
    key(vk, true);
}

static std::string describe(HWND w)
{
    wchar_t cls[128] {}, title[256] {};
    GetClassNameW(w, cls, 128);
    InternalGetWindowText(w, title, 256);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    char c[256], t[512], out[900];
    WideCharToMultiByte(CP_UTF8, 0, cls, -1, c, sizeof c, nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, title, -1, t, sizeof t, nullptr, nullptr);
    sprintf_s(out, "%p %s \"%s\" pid=%lu", (void*)w, c, t, pid);
    return out;
}

static bool front(HWND w)
{
    for (int i = 0; i < 10; ++i) {
        tapKey(0xE8); // unassigned: this process has the last input, it may set the front
        SetForegroundWindow(w);
        Sleep(150);
        if (GetForegroundWindow() == w) return true;
    }
    return false;
}

static HWND findPopup()
{
    struct Ctx { HWND found; } ctx {nullptr};
    EnumWindows([](HWND w, LPARAM l) -> BOOL {
        wchar_t cls[64], title[256];
        if (!IsWindowVisible(w) || !GetClassNameW(w, cls, 64) || wcscmp(cls, L"#32770") != 0) return TRUE;
        InternalGetWindowText(w, title, 256);
        if (wcsstr(title, L"hkpmissing")) {
            ((Ctx*)l)->found = w;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

static void press(const char* phase, HWND target)
{
    if (!front(target)) {
        logf("%s: could not front %s (front is %s)", phase, describe(target).c_str(), describe(GetForegroundWindow()).c_str());
        return;
    }
    logf("%s: press over %s", phase, describe(GetForegroundWindow()).c_str());
    tapKey((WORD)g_vk);
    Sleep(800);
    logf("%s: front after %s", phase, describe(GetForegroundWindow()).c_str());
}

static int send(const wchar_t* ready, const wchar_t* done, const wchar_t* elevTitle, const wchar_t* testTitle, const wchar_t* missing)
{
    HANDLE r = nullptr;
    for (int i = 0; i < 900 && !r; ++i, Sleep(100)) r = OpenEventW(SYNCHRONIZE, FALSE, ready);
    HANDLE d = OpenEventW(EVENT_MODIFY_STATE, FALSE, done);
    if (!r || !d) {
        logf("send: events not found (%p %p)", r, d);
        return 1;
    }
    if (WaitForSingleObject(r, 60000) != WAIT_OBJECT_0) {
        logf("send: never ready");
        return 2;
    }
    logf("send: ready");
    HWND elev = FindWindowW(L"RelaySendWindow", elevTitle);
    HWND test = FindWindowW(L"Static", testTitle);
    logf("send: elevated %s, test %s", elev ? describe(elev).c_str() : "none", test ? describe(test).c_str() : "none");
    for (int i = 0; elev && i < 3; ++i)
        press("elevated", elev);
    if (test)
        press("test", test);
    STARTUPINFOW si {sizeof si};
    PROCESS_INFORMATION pi {};
    std::wstring cmd = L"\"" + std::wstring(missing) + L"\"";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_DEFAULT_ERROR_MODE, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        HWND p = nullptr;
        for (int i = 0; i < 80 && !p; ++i, Sleep(100)) p = findPopup();
        if (p) {
            press("csrss", p);
            if (front(p)) tapKey(VK_RETURN); // OK
            Sleep(300);
            logf("csrss: box still there: %d", IsWindow(p) && IsWindowVisible(p));
        } else {
            logf("csrss: no box");
        }
    } else {
        logf("csrss: start failed %lu", GetLastError());
    }
    SetEvent(d);
    logf("send: done");
    return 0;
}

static std::string imageOf(HWND w)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    std::string out;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        char a[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, a, sizeof a, nullptr, nullptr);
        out = a;
    }
    CloseHandle(p);
    return out;
}

static bool isCapture(HWND w)
{
    return imageOf(w).find("WinShunCapture.exe") != std::string::npos;
}

static void logTop(const char* when)
{
    int n = 0;
    for (HWND w = GetTopWindow(nullptr); w && n < 5; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!IsWindowVisible(w)) continue;
        RECT r;
        GetWindowRect(w, &r);
        logf("%s z%d topmost=%d rect=%ld,%ld,%ld,%ld %s %s", when, n++, (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
            r.left, r.top, r.right, r.bottom, describe(w).c_str(), imageOf(w).c_str());
    }
}

static void shot(const std::wstring& file)
{
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 24, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    const DWORD stride = ((DWORD)w * 3 + 3) & ~3u, size = stride * (DWORD)h;
    BITMAPFILEHEADER fh {0x4D42, sizeof fh + sizeof(BITMAPINFOHEADER) + size, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
    FILE* f = _wfsopen(file.c_str(), L"wb", _SH_DENYNO);
    if (f) {
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
        fwrite(bits, size, 1, f);
        fclose(f);
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    logf("shot %dx%d at %d,%d", w, h, x, y);
}

// The capture overlay in front: Esc into it alone (it closes).
static void escCapture(const char* phase)
{
    HWND fg = GetForegroundWindow();
    if (!isCapture(fg)) {
        logf("%s: no Esc, front is %s %s", phase, describe(fg).c_str(), imageOf(fg).c_str());
        return;
    }
    tapKey(VK_ESCAPE);
    Sleep(700);
    fg = GetForegroundWindow();
    logf("%s: after Esc front is %s %s", phase, describe(fg).c_str(), imageOf(fg).c_str());
}

// The real Win顺 and its screenshot plugin, hotkey <vk> alone: over an
// elevated window, then over csrss's error box.
static int e2e(const wchar_t* elevTitle, const wchar_t* missing, const std::wstring& dir)
{
    logf("e2e: start");
    HWND elev = FindWindowW(L"RelaySendWindow", elevTitle);
    if (elev && front(elev)) {
        logf("elevated: press over %s", describe(GetForegroundWindow()).c_str());
        tapKey((WORD)g_vk);
        Sleep(1500);
        HWND fg = GetForegroundWindow();
        logf("elevated: front after %s %s capture=%d", describe(fg).c_str(), imageOf(fg).c_str(), isCapture(fg));
        logTop("elevated");
        shot(dir + L"\\over-elevated.bmp");
        escCapture("elevated");
    } else {
        logf("elevated: could not front (%p)", (void*)elev);
    }
    Sleep(800);
    STARTUPINFOW si {sizeof si};
    PROCESS_INFORMATION pi {};
    std::wstring cmd = L"\"" + std::wstring(missing) + L"\"";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_DEFAULT_ERROR_MODE, nullptr, nullptr, &si, &pi)) {
        logf("csrss: start failed %lu", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    HWND p = nullptr;
    for (int i = 0; i < 80 && !p; ++i, Sleep(100)) p = findPopup();
    if (p && front(p)) {
        logf("csrss: press over %s", describe(GetForegroundWindow()).c_str());
        tapKey((WORD)g_vk);
        Sleep(1500);
        HWND fg = GetForegroundWindow();
        logf("csrss: front after %s %s capture=%d", describe(fg).c_str(), imageOf(fg).c_str(), isCapture(fg));
        logTop("csrss");
        shot(dir + L"\\over-csrss.bmp");
        escCapture("csrss");
        Sleep(500);
        if (front(p) && GetForegroundWindow() == p) tapKey(VK_RETURN);
        Sleep(300);
        logf("csrss: box still there: %d", IsWindow(p) && IsWindowVisible(p));
    } else {
        logf("csrss: no box in front (%p)", (void*)p);
    }
    logf("e2e: done");
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) return 1;
    std::wstring mode = argv[1];
    if (mode == L"e2e" && argc >= 7) {
        g_log = _wfsopen(argv[2], L"a", _SH_DENYNO);
        g_vk = wcstoul(argv[3], nullptr, 16);
        return e2e(argv[4], argv[5], argv[6]);
    }
    if (mode == L"window" && argc >= 5) {
        g_vk = wcstoul(argv[3], nullptr, 16);
        g_log = _wfsopen(argv[4], L"a", _SH_DENYNO);
        return window(argv[2]);
    }
    if (mode == L"send" && argc >= 9) {
        g_vk = wcstoul(argv[4], nullptr, 16);
        g_log = _wfsopen(argv[5], L"a", _SH_DENYNO);
        return send(argv[2], argv[3], argv[6], argv[7], argv[8]);
    }
    return 2;
}

// Issue #243 probe: which RegisterHotKey registrations fire over which
// foreground windows. Modes:
//   recv <tag> <vk> <log>        register <vk> bare and Ctrl+Alt+Shift+<vk>, log WM_HOTKEY (90 s)
//   window <title>               a plain window (90 s)
//   spawn <exe>                  CreateProcess with CREATE_DEFAULT_ERROR_MODE (the hard-error box)
//   uia-launch <cmdline>         (SYSTEM, session 0) user token + High IL + UIAccess in the console session
//   sys-launch <cmdline>         (SYSTEM, session 0) own token moved to the console session (System IL)
//   send <log> <dir>             (System IL, user session) foregrounds each target, injects the keys
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <psapi.h>
#include <cstdio>
#include <share.h>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

static FILE* g_log = nullptr;
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

static std::string ilOfProcess(DWORD pid, DWORD* uiaOut = nullptr)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return "open-failed(" + std::to_string(GetLastError()) + ")";
    HANDLE t = nullptr;
    std::string r;
    if (OpenProcessToken(p, TOKEN_QUERY, &t)) {
        BYTE buf[256];
        DWORD n = 0;
        if (GetTokenInformation(t, TokenIntegrityLevel, buf, sizeof buf, &n)) {
            PSID sid = ((TOKEN_MANDATORY_LABEL*)buf)->Label.Sid;
            char s[32];
            sprintf_s(s, "0x%lx", *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1));
            r = s;
        }
        DWORD uia = 0;
        GetTokenInformation(t, TokenUIAccess, &uia, sizeof uia, &n);
        if (uiaOut) *uiaOut = uia;
        r += uia ? "+uia" : "";
        CloseHandle(t);
    } else {
        r = "token-failed(" + std::to_string(GetLastError()) + ")";
    }
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &size)) {
        char a[MAX_PATH * 2];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, a, sizeof a, nullptr, nullptr);
        r += std::string(" ") + a;
    }
    CloseHandle(p);
    return r;
}

static std::string describe(HWND w)
{
    if (!w) return "null";
    wchar_t cls[128] {}, title[256] {};
    GetClassNameW(w, cls, 128);
    InternalGetWindowText(w, title, 256); // never a message: a busy window cannot hold the probe up
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    char c[256], t[512];
    WideCharToMultiByte(CP_UTF8, 0, cls, -1, c, sizeof c, nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, title, -1, t, sizeof t, nullptr, nullptr);
    char out[1200];
    sprintf_s(out, "hwnd=%p class=%s title=\"%s\" pid=%lu il=%s", (void*)w, c, t, pid, ilOfProcess(pid).c_str());
    return out;
}

static int recv(const char* tag, UINT vk, const wchar_t* log)
{
    g_log = _wfsopen(log, L"a", _SH_DENYNO);
    logf("%s start pid=%lu self=%s", tag, GetCurrentProcessId(), ilOfProcess(GetCurrentProcessId()).c_str());
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    BOOL a = RegisterHotKey(nullptr, 1, MOD_NOREPEAT, vk);
    DWORD ea = GetLastError();
    BOOL b = RegisterHotKey(nullptr, 2, MOD_NOREPEAT | MOD_CONTROL | MOD_ALT | MOD_SHIFT, vk);
    DWORD eb = GetLastError();
    logf("%s registered bare=%d(%lu) mods=%d(%lu)", tag, a, a ? 0 : ea, b, b ? 0 : eb);
    const UINT_PTR timer = SetTimer(nullptr, 0, 90000, nullptr);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == nullptr && msg.wParam == timer) break;
        if (msg.message == WM_HOTKEY)
            logf("%s HOTKEY id=%s fg=%s", tag, msg.wParam == 1 ? "bare" : "mods", describe(GetForegroundWindow()).c_str());
    }
    logf("%s exit", tag);
    return 0;
}

static LRESULT CALLBACK wproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

static int window(const wchar_t* title)
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = wproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HkProbeWindow";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 420, 160, nullptr, nullptr,
        wc.hInstance, nullptr);
    SetTimer(h, 77, 90000, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == h && msg.wParam == 77) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static int spawn(wchar_t* cmd)
{
    STARTUPINFOW si {sizeof si};
    PROCESS_INFORMATION pi {};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_DEFAULT_ERROR_MODE, nullptr, nullptr, &si, &pi))
        return (int)GetLastError();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

static bool enable(const wchar_t* name)
{
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &t)) return false;
    TOKEN_PRIVILEGES p {1};
    LookupPrivilegeValueW(nullptr, name, &p.Privileges[0].Luid);
    p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(t, FALSE, &p, 0, nullptr, nullptr);
    DWORD e = GetLastError();
    CloseHandle(t);
    return e == ERROR_SUCCESS;
}

static int launch(wchar_t* cmd, bool uia)
{
    enable(SE_TCB_NAME);
    enable(SE_ASSIGNPRIMARYTOKEN_NAME);
    enable(SE_INCREASE_QUOTA_NAME);
    DWORD session = WTSGetActiveConsoleSessionId();
    HANDLE base = nullptr;
    if (uia) {
        if (!WTSQueryUserToken(session, &base)) return 100 + (int)GetLastError();
    } else {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &base)) return 200 + (int)GetLastError();
    }
    HANDLE token = nullptr;
    if (!DuplicateTokenEx(base, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &token)) return 300;
    if (uia) {
        DWORD one = 1;
        BYTE sid[SECURITY_MAX_SID_SIZE] {};
        DWORD sidSize = sizeof sid;
        CreateWellKnownSid(WinHighLabelSid, nullptr, sid, &sidSize);
        TOKEN_MANDATORY_LABEL label {{sid, SE_GROUP_INTEGRITY}};
        if (!SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof label + sidSize)) return 400;
        if (!SetTokenInformation(token, TokenUIAccess, &one, sizeof one)) return 401;
    } else {
        if (!SetTokenInformation(token, TokenSessionId, &session, sizeof session)) return 402;
    }
    void* env = nullptr;
    CreateEnvironmentBlock(&env, token, FALSE);
    STARTUPINFOW si {sizeof si};
    wchar_t desk[] = L"winsta0\\default";
    si.lpDesktop = desk;
    PROCESS_INFORMATION pi {};
    if (!CreateProcessAsUserW(token, nullptr, cmd, nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env,
            nullptr, &si, &pi))
        return 500 + (int)GetLastError();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
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

static HWND findPopup()
{
    // The hard-error box: a #32770 window of csrss.exe, visible.
    struct Ctx { HWND found; } ctx {nullptr};
    EnumWindows([](HWND w, LPARAM l) -> BOOL {
        wchar_t cls[64];
        if (!IsWindowVisible(w) || !GetClassNameW(w, cls, 64) || wcscmp(cls, L"#32770") != 0) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!p) return TRUE;
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        bool csrss = QueryFullProcessImageNameW(p, 0, path, &n) && wcsstr(_wcslwr(path), L"csrss.exe");
        CloseHandle(p);
        if (csrss) {
            ((Ctx*)l)->found = w;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

static bool front(HWND w)
{
    for (int i = 0; i < 10; ++i) {
        tapKey(0xE8); // the masking key: last input is ours, the foreground may move
        SetForegroundWindow(w);
        Sleep(150);
        if (GetForegroundWindow() == w) return true;
    }
    return false;
}

static void sendAll(const char* what, HWND target)
{
    logf("send %s fg=%s", what, describe(GetForegroundWindow()).c_str());
    const WORD keys[] = {VK_F20, VK_F21, VK_F22};
    for (WORD vk : keys) {
        if (GetForegroundWindow() != target) { logf("send %s abort: foreground moved to %s", what, describe(GetForegroundWindow()).c_str()); break; }
        logf("send %s bare vk=0x%x", what, vk);
        tapKey(vk);
        Sleep(300);
        logf("send %s mods vk=0x%x", what, vk);
        key(VK_CONTROL, false);
        key(VK_MENU, false);
        key(VK_SHIFT, false);
        Sleep(20);
        tapKey(vk);
        Sleep(20);
        key(VK_SHIFT, true);
        key(VK_MENU, true);
        key(VK_CONTROL, true);
        Sleep(300);
    }
}

static int send(const wchar_t* log, const wchar_t* missingExe)
{
    g_log = _wfsopen(log, L"a", _SH_DENYNO);
    logf("sender start pid=%lu self=%s", GetCurrentProcessId(), ilOfProcess(GetCurrentProcessId()).c_str());
    Sleep(1500); // receivers come up
    const wchar_t* titles[] = {L"hkp-normal", L"hkp-elevated"};
    for (const wchar_t* t : titles) {
        HWND w = nullptr;
        for (int i = 0; i < 50 && !w; ++i, Sleep(100)) w = FindWindowW(L"HkProbeWindow", t);
        if (!w || !front(w)) {
            logf("send skip %ls (found=%p)", t, (void*)w);
            continue;
        }
        char a[64];
        WideCharToMultiByte(CP_UTF8, 0, t, -1, a, 64, nullptr, nullptr);
        sendAll(a, w);
    }
    // The hard-error box, started by us (CREATE_DEFAULT_ERROR_MODE) with the user's... SYSTEM token: csrss shows it all the same.
    std::wstring cmd = L"\"" + std::wstring(missingExe) + L"\"";
    int s = spawn(cmd.data());
    logf("spawn missing exe: %d", s);
    HWND p = nullptr;
    for (int i = 0; i < 80 && !p; ++i, Sleep(100)) p = findPopup();
    if (p && front(p)) {
        sendAll("csrss-popup", p);
        if (GetForegroundWindow() == p) tapKey(VK_RETURN); // OK
        Sleep(300);
        logf("after enter popup still there: %d", IsWindow(p) && IsWindowVisible(p));
    } else {
        logf("send skip popup (found=%p) fg=%s", (void*)p, describe(GetForegroundWindow()).c_str());
    }
    logf("sender done");
    return 0;
}

static bool isCapture(HWND w)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return ilOfProcess(pid).find("WinShunCapture.exe") != std::string::npos;
}

static void logTop(const char* when)
{
    int n = 0;
    for (HWND w = GetTopWindow(nullptr); w && n < 6; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!IsWindowVisible(w)) continue;
        RECT r;
        GetWindowRect(w, &r);
        logf("%s z%d topmost=%d rect=%ld,%ld,%ld,%ld %s", when, n++, (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, r.left,
            r.top, r.right, r.bottom, describe(w).c_str());
    }
}

// The real screenshot over a hard-error box: the user's own capture hotkey
// (Ctrl+1 here), then where the overlay is, a picture taken by the medium
// side (shoot-now -> shot-done), Esc into the overlay only, Enter into the box only.
static int captureTest(const wchar_t* log, const wchar_t* missingExe, const wchar_t* dir)
{
    g_log = _wfsopen(log, L"a", _SH_DENYNO);
    logf("capture-test start self=%s", ilOfProcess(GetCurrentProcessId()).c_str());
    std::wstring cmd = L"\"" + std::wstring(missingExe) + L"\"";
    logf("spawn missing exe: %d", spawn(cmd.data()));
    HWND p = nullptr;
    for (int i = 0; i < 80 && !p; ++i, Sleep(100)) p = findPopup();
    if (!p || !front(p)) {
        logf("no popup in front (found=%p)", (void*)p);
        return 1;
    }
    logTop("before");
    if (GetForegroundWindow() == p) {
        key(VK_CONTROL, false);
        Sleep(20);
        tapKey('1');
        Sleep(20);
        key(VK_CONTROL, true);
        logf("sent Ctrl+1");
    }
    Sleep(1500);
    logf("fg after hotkey: %s", describe(GetForegroundWindow()).c_str());
    logTop("after");
    const std::wstring now = std::wstring(dir) + L"\\shoot-now", done = std::wstring(dir) + L"\\shot-done";
    FILE* f = _wfsopen(now.c_str(), L"w", _SH_DENYNO);
    if (f) fclose(f);
    for (int i = 0; i < 100 && GetFileAttributesW(done.c_str()) == INVALID_FILE_ATTRIBUTES; ++i) Sleep(100);
    HWND fg = GetForegroundWindow();
    if (!isCapture(fg)) {
        for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT))
            if (IsWindowVisible(w) && isCapture(w)) { front(w); break; }
        fg = GetForegroundWindow();
    }
    if (isCapture(fg)) {
        tapKey(VK_ESCAPE);
        logf("Esc into %s", describe(fg).c_str());
    } else {
        logf("no capture window to Esc; fg=%s", describe(fg).c_str());
    }
    Sleep(500);
    if (IsWindow(p) && front(p) && GetForegroundWindow() == p) tapKey(VK_RETURN);
    Sleep(300);
    logf("popup still there: %d", IsWindow(p) && IsWindowVisible(p));
    logTop("end");
    logf("capture-test done");
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) return 1;
    std::wstring mode = argv[1];
    auto narrow = [](const wchar_t* w) {
        static char b[4][1024];
        static int i = 0;
        i = (i + 1) % 4;
        WideCharToMultiByte(CP_UTF8, 0, w, -1, b[i], 1024, nullptr, nullptr);
        return b[i];
    };
    if (mode == L"recv" && argc >= 5) return recv(narrow(argv[2]), (UINT)wcstoul(argv[3], nullptr, 16), argv[4]);
    if (mode == L"window" && argc >= 3) return window(argv[2]);
    if (mode == L"spawn" && argc >= 3) return spawn(argv[2]);
    if ((mode == L"uia-launch" || mode == L"sys-launch") && argc >= 3) {
        // The rest of the command line, as given.
        std::wstring rest;
        for (int i = 2; i < argc; ++i) {
            if (i > 2) rest += L' ';
            rest += L'"' + std::wstring(argv[i]) + L'"';
        }
        int r = launch(rest.data(), mode == L"uia-launch");
        FILE* f = nullptr;
        _wfopen_s(&f, (std::wstring(argv[0]) + L".launch.txt").c_str(), L"a");
        if (f) { fwprintf(f, L"%ls -> %d\n", mode.c_str(), r); fclose(f); }
        return r;
    }
    if (mode == L"send" && argc >= 4) return send(argv[2], argv[3]);
    if (mode == L"capture-test" && argc >= 5) return captureTest(argv[2], argv[3], argv[4]);
    return 2;
}

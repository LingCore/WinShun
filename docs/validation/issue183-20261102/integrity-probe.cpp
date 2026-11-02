#include <windows.h>
#include <cstdio>
#include <string>

// Standalone Windows experiment. Only sends keys while its own fixture is
// foreground; never invokes a browser or writes a user's configuration.
int wmain(int argc, wchar_t** argv)
{
    if (argc > 1) {
        const HWND window = CreateWindowExW(0, L"STATIC", L"hotkey listener", 0,
            0, 0, 1, 1, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        const bool plain = RegisterHotKey(nullptr, 1, MOD_NOREPEAT, VK_F1);
        const bool chord = RegisterHotKey(nullptr, 2, MOD_NOREPEAT | MOD_CONTROL | MOD_ALT, 'Z');
        std::printf("registered:%d,%d\n", plain, chord);
        std::fflush(stdout);
        const ULONGLONG until = GetTickCount64() + 7000;
        while (GetTickCount64() < until) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_HOTKEY) {
                    std::printf("hotkey:%llu\n", static_cast<unsigned long long>(msg.wParam));
                    std::fflush(stdout);
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(5);
        }
        DestroyWindow(window);
        return 0;
    }
    wchar_t exe[32768];
    GetModuleFileNameW(nullptr, exe, 32768);
    const HWND previous = GetForegroundWindow();
    const HWND fixture = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"WinShun issue 183 keyboard fixture",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 460, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    for (bool low : {false, true}) {
        SECURITY_ATTRIBUTES sa {sizeof sa, nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        CreatePipe(&read, &write, &sa, 0);
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si {sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = write;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi {};
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --listener";
        HANDLE token = nullptr, restricted = nullptr;
        BOOL started = FALSE;
        if (low) {
            OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &token);
            CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &restricted);
            SID_IDENTIFIER_AUTHORITY mandatory = SECURITY_MANDATORY_LABEL_AUTHORITY;
            PSID sid = nullptr;
            AllocateAndInitializeSid(&mandatory, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &sid);
            TOKEN_MANDATORY_LABEL label {{sid, SE_GROUP_INTEGRITY}};
            SetTokenInformation(restricted, TokenIntegrityLevel, &label, sizeof label + GetLengthSid(sid));
            FreeSid(sid);
            started = CreateProcessAsUserW(restricted, exe, command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        } else {
            started = CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        }
        const DWORD error = GetLastError();
        CloseHandle(write);
        if (restricted) CloseHandle(restricted);
        if (token) CloseHandle(token);
        if (!started) { std::printf("start failed:%lu\n", error); return 2; }
        std::string output;
        auto pump = [&](int ms) {
            const ULONGLONG until = GetTickCount64() + ms;
            do {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
                DWORD available = 0, got = 0;
                if (PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) && available) {
                    char buffer[4096];
                    if (ReadFile(read, buffer, sizeof buffer, &got, nullptr)) output.append(buffer, got);
                }
                Sleep(5);
            } while (GetTickCount64() < until);
        };
        pump(500);
        if (output.find("registered:1,1") == std::string::npos) { std::puts(output.c_str()); return 3; }
        SetForegroundWindow(fixture);
        pump(100);
        if (GetForegroundWindow() != fixture) return 4;
        auto key = [](WORD vk, bool up) {
            INPUT input {}; input.type = INPUT_KEYBOARD; input.ki.wVk = vk;
            input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
            return SendInput(1, &input, sizeof input) == 1;
        };
        key(VK_F1, false); pump(40); key(VK_F1, true); pump(300);
        key(VK_CONTROL, false); key(VK_MENU, false); key('Z', false); pump(40);
        key('Z', true); key(VK_MENU, true); key(VK_CONTROL, true); pump(300);
        std::printf("%s listener, medium foreground:\n%s", low ? "low" : "medium", output.c_str());
        // Only this experiment's child is terminated, releasing its hotkeys.
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(read);
    }
    DestroyWindow(fixture);
    if (previous) SetForegroundWindow(previous);
}

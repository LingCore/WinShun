// A stand-in for WinShun.exe in tst_updater's runs of WinShunUpdate.exe. As
// far as the helper looks, it comes up as Win顺 does: a window of App's
// class WinShun.MessageWindow, in the process the helper started. It writes
// data\ran.txt beside itself: what version.txt beside it says, and whether
// it runs as administrator. Then it ends by itself.

#include <windows.h>

#include <string>

namespace {

std::wstring folderOfThisExe()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return path.substr(0, path.find_last_of(L'\\'));
}

std::string readSmallFile(const std::wstring& path)
{
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return "<none>";
    char buffer[64] {};
    DWORD read = 0;
    ::ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    ::CloseHandle(file);
    return std::string(buffer, read);
}

bool elevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    const bool ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    const std::wstring folder = folderOfThisExe();
    const std::string line = readSmallFile(folder + L"\\version.txt") + (elevated() ? " elevated" : " normal");
    ::CreateDirectoryW((folder + L"\\data").c_str(), nullptr);
    const HANDLE out = ::CreateFileW((folder + L"\\data\\ran.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        ::CloseHandle(out);
    }

    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = ::DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"WinShun.MessageWindow";
    ::RegisterClassW(&windowClass);
    const HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass.lpszClassName, L"WinShun",
        WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window)
        return 2;
    // The helper looks every 250 ms; well after that, gone, so that the
    // test can remove the folder (from a test without the rights to end a
    // copy that runs as administrator).
    ::SetTimer(window, 1, 2000, nullptr);
    MSG message;
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_TIMER)
            break;
        ::DispatchMessageW(&message);
    }
    ::DestroyWindow(window);
    return 0;
}

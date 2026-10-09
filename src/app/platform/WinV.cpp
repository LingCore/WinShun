#include "WinV.h"

#include "Win32Util.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace ws::winv {

namespace {

constexpr wchar_t kAdvancedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
constexpr wchar_t kDisabledHotkeys[] = L"DisabledHotkeys";

std::wstring disabledHotkeys()
{
    wchar_t value[256] {};
    DWORD size = sizeof value - sizeof(wchar_t);
    if (::RegGetValueW(HKEY_CURRENT_USER, kAdvancedKey, kDisabledHotkeys, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
            nullptr, value, &size)
        != ERROR_SUCCESS)
        return {};
    return value;
}

} // namespace

bool releasedByExplorer(wchar_t key)
{
    const std::wstring keys = disabledHotkeys();
    return std::ranges::any_of(keys, [key](wchar_t c) { return std::towupper(c) == key; });
}

bool setReleasedByExplorer(wchar_t key, bool released)
{
    std::wstring keys = disabledHotkeys();
    std::erase_if(keys, [key](wchar_t c) { return std::towupper(c) == key; });
    if (released)
        keys += key;
    win32::UniqueKey advanced;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kAdvancedKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, advanced.out(), nullptr)
        != ERROR_SUCCESS)
        return false;
    if (keys.empty())
        return ::RegDeleteValueW(advanced.get(), kDisabledHotkeys) == ERROR_SUCCESS || !releasedByExplorer(key);
    return ::RegSetValueExW(advanced.get(), kDisabledHotkeys, 0, REG_SZ, reinterpret_cast<const BYTE*>(keys.c_str()),
               static_cast<DWORD>((keys.size() + 1) * sizeof(wchar_t)))
        == ERROR_SUCCESS;
}

bool canRestartExplorer()
{
    return ::GetShellWindow() != nullptr;
}

namespace {

bool waitForShell(int ms)
{
    for (int waited = 0; waited < ms; waited += 100) {
        if (::GetShellWindow())
            return true;
        ::Sleep(100);
    }
    return ::GetShellWindow() != nullptr;
}

// A process of this session by its file name ("sihost.exe"), 0 if none.
DWORD findProcess(const wchar_t* name)
{
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    win32::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid())
        return 0;
    PROCESSENTRY32W entry {sizeof entry};
    for (BOOL more = ::Process32FirstW(snapshot.get(), &entry); more; more = ::Process32NextW(snapshot.get(), &entry)) {
        DWORD processSession = 0;
        if (::_wcsicmp(entry.szExeFile, name) == 0 && ::ProcessIdToSessionId(entry.th32ProcessID, &processSession)
            && processSession == session)
            return entry.th32ProcessID;
    }
    return 0;
}

// Explorer, started as the user would (never with our administrator rights,
// or everything started from the taskbar would get them): with the old
// shell's token, else as a child of one of the user's own processes, whose
// rights a child gets.
bool startShell(HANDLE token)
{
    wchar_t windows[MAX_PATH] {};
    ::GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring exe = std::wstring(windows) + L"\\explorer.exe";
    PROCESS_INFORMATION started {};
    STARTUPINFOW startup {sizeof startup};
    // Needs the Secondary Logon service, which can be turned off.
    if (token && ::CreateProcessWithTokenW(token, 0, exe.c_str(), nullptr, 0, nullptr, windows, &startup, &started)) {
        ::CloseHandle(started.hThread);
        ::CloseHandle(started.hProcess);
        return true;
    }
    for (const wchar_t* name : {L"sihost.exe", L"ctfmon.exe", L"explorer.exe"}) {
        const DWORD pid = findProcess(name);
        win32::UniqueHandle parent(pid ? ::OpenProcess(PROCESS_CREATE_PROCESS, FALSE, pid) : nullptr);
        if (!parent.valid())
            continue;
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<char> buffer(size);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &size))
            continue;
        HANDLE parentHandle = parent.get();
        STARTUPINFOEXW extended {};
        extended.StartupInfo.cb = sizeof extended;
        extended.lpAttributeList = attributes;
        std::wstring command = L"\"" + exe + L"\"";
        const bool ok = ::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parentHandle,
                            sizeof parentHandle, nullptr, nullptr)
            && ::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT,
                nullptr, windows, &extended.StartupInfo, &started);
        ::DeleteProcThreadAttributeList(attributes);
        if (ok) {
            ::CloseHandle(started.hThread);
            ::CloseHandle(started.hProcess);
            return true;
        }
    }
    return false;
}

} // namespace

bool restartExplorer()
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(::GetShellWindow(), &pid);
    if (pid == 0)
        return false;
    win32::UniqueHandle shell(::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!shell.valid())
        return false;
    // Its token, to start the new one with the same rights.
    win32::UniqueHandle token;
    {
        HANDLE own = nullptr;
        HANDLE primary = nullptr;
        if (::OpenProcessToken(shell.get(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &own)) {
            if (::DuplicateTokenEx(own, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &primary))
                token = win32::UniqueHandle(primary);
            ::CloseHandle(own);
        }
    }
    // A non-zero exit code: Winlogon may start it again by itself (it does
    // only for the shell it started at sign-in).
    if (!::TerminateProcess(shell.get(), 1))
        return false;
    ::WaitForSingleObject(shell.get(), 5000);
    if (waitForShell(3000))
        return true;
    return startShell(token.get()) && waitForShell(15000);
}

} // namespace ws::winv

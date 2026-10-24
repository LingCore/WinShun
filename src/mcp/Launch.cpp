#include "Launch.h"

#include <windows.h>
#include <shellapi.h>

#include <iterator>
#include <string_view>

namespace ws::launch {

namespace {

bool fileExists(const std::wstring& path)
{
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

bool winShunRunning(const wchar_t* mutex)
{
    if (const HANDLE handle = ::OpenMutexW(SYNCHRONIZE, FALSE, mutex)) {
        ::CloseHandle(handle);
        return true;
    }
    return ::GetLastError() == ERROR_ACCESS_DENIED; // there, but an elevated process's: not ours to open
}

bool runAutostartTask()
{
    wchar_t system[MAX_PATH] {};
    if (!::GetSystemDirectoryW(system, MAX_PATH))
        return false;
    std::wstring command = L"\"" + std::wstring(system) + L"\\schtasks.exe\" /Run /TN WinShun";
    STARTUPINFOW startup {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process {};
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    DWORD code = 1;
    if (::WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0)
        ::GetExitCodeProcess(process.hProcess, &code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return code == 0;
}

std::wstring winShunExe()
{
    wchar_t self[MAX_PATH * 2] {};
    const DWORD length = ::GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (length > 0 && length < std::size(self)) {
        std::wstring folder(self, length);
        folder.resize(folder.find_last_of(L'\\'));
        if (std::wstring beside = folder + L"\\WinShun.exe"; fileExists(beside))
            return beside;
        // ws.exe is alone in <install>\bin, so that only that folder goes on
        // PATH (the install folder has Qt's DLLs): Win顺 is one up.
        const std::size_t slash = folder.find_last_of(L'\\');
        if (slash != std::wstring::npos && ::CompareStringOrdinal(folder.c_str() + slash + 1, -1, L"bin", -1, TRUE) == CSTR_EQUAL) {
            if (std::wstring above = folder.substr(0, slash) + L"\\WinShun.exe"; fileExists(above))
                return above;
        }
    }
    wchar_t command[MAX_PATH * 2 + 32] {};
    DWORD bytes = sizeof command - sizeof(wchar_t);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"WinShun", RRF_RT_REG_SZ,
            nullptr, command, &bytes)
        != ERROR_SUCCESS)
        return {};
    std::wstring exe(command);
    if (!exe.empty() && exe.front() == L'"') {
        const std::size_t close = exe.find(L'"', 1);
        exe = exe.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1);
    } else {
        exe = exe.substr(0, exe.find(L' '));
    }
    return fileExists(exe) ? exe : std::wstring();
}

bool startWinShun()
{
    if (runAutostartTask())
        return true;
    const std::wstring exe = winShunExe();
    if (exe.empty())
        return false;
    const std::wstring folder = exe.substr(0, exe.find_last_of(L'\\'));
    return reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, nullptr, exe.c_str(), L"--background", folder.c_str(), SW_SHOWNORMAL)) > 32;
}

} // namespace ws::launch

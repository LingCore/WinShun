// WinShunUpdate.exe: the part of an in-app update that must run while Win顺
// does not (Updater). Plain Win32 with the C runtime linked in and no Qt:
// Updater copies this one file into its Updates folder and runs it from
// there, so it holds nothing of the folder it replaces.
//
//   --portable    --pid <Win顺> --ready <event> --app <folder> --stage <folder> --backup <folder> --log <file>
//       Waits for that Win顺 to end (it saved its index the usual way), then
//       puts the staged files in place (Swap.h), starts the new version and
//       waits for it to come up. Anything failing puts the old files back
//       and starts the old version, which then says the update did not
//       happen (Updater: its version is not the one asked for).
//   --watch-setup --pid <Setup> --ready <event> --start <WinShun.exe> --log <file>
//       Setup closes Win顺 and starts the new version itself; one that
//       failed after closing it starts nothing. Once it has ended, the
//       installed copy is started when none runs.
//
// <event> is set once the process is held, so that Win顺 quits only then.
// The named mutex Local\WinShun.Updating is held throughout: a Win顺 that
// starts meanwhile leaves the stage and backup alone.

#include "Swap.h"

#include <windows.h>

#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <map>
#include <string>

namespace {

std::wstring g_logFile;
// Win顺's single-instance mutex (main.cpp); --instance names another for tests,
// which then leave the user's own Win顺 alone.
std::wstring g_instance = L"Local\\WinShun.Instance";

void log(const std::wstring& text)
{
    if (g_logFile.empty())
        return;
    SYSTEMTIME now;
    ::GetLocalTime(&now);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond);
    const std::wstring line = stamp + text + L"\r\n";
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr, nullptr);
    const HANDLE file = ::CreateFileW(g_logFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    ::CloseHandle(file);
}

bool instanceRunning()
{
    const HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, g_instance.c_str());
    if (mutex) {
        ::CloseHandle(mutex);
        return true;
    }
    return ::GetLastError() == ERROR_ACCESS_DENIED; // an elevated copy's
}

bool startsWithFolder(const std::wstring& path, const std::wstring& folder)
{
    const std::wstring prefix = folder + L'\\';
    return path.size() > prefix.size()
        && ::CompareStringOrdinal(path.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
               static_cast<int>(prefix.size()), TRUE)
        == CSTR_EQUAL;
}

// The processes started from `folder` (the MCP relay an AI client keeps,
// the plugin host, the taskbar button's stub): its files cannot be moved
// while they run. Given a while, then ended; never one from elsewhere.
void endProcessesFrom(const std::wstring& folder)
{
    for (int round = 0; round < 2; ++round) {
        std::map<DWORD, HANDLE> ours;
        const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        PROCESSENTRY32W entry {sizeof(entry)};
        for (BOOL more = ::Process32FirstW(snapshot, &entry); more; more = ::Process32NextW(snapshot, &entry)) {
            if (entry.th32ProcessID == ::GetCurrentProcessId())
                continue;
            const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE,
                FALSE, entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t image[MAX_PATH * 2];
            DWORD length = static_cast<DWORD>(std::size(image));
            if (::QueryFullProcessImageNameW(process, 0, image, &length) && startsWithFolder(image, folder))
                ours.emplace(entry.th32ProcessID, process);
            else
                ::CloseHandle(process);
        }
        ::CloseHandle(snapshot);
        for (auto& [pid, process] : ours) {
            if (round == 0 && ::WaitForSingleObject(process, 10000) == WAIT_OBJECT_0) {
                ::CloseHandle(process);
                continue;
            }
            log(L"ending process " + std::to_wstring(pid) + L" from the program folder");
            ::TerminateProcess(process, 1);
            ::WaitForSingleObject(process, 5000);
            ::CloseHandle(process);
        }
        if (ours.empty())
            return;
    }
}

HANDLE start(const std::wstring& exe, const std::wstring& folder)
{
    std::wstring command = L"\"" + exe + L"\" --background";
    STARTUPINFOW startup {sizeof(startup)};
    PROCESS_INFORMATION info {};
    if (!::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            folder.empty() ? nullptr : folder.c_str(), &startup, &info)) {
        log(L"cannot start " + exe + L" (" + std::to_wstring(::GetLastError()) + L")");
        return nullptr;
    }
    ::CloseHandle(info.hThread);
    log(L"started " + exe + L" (pid " + std::to_wstring(info.dwProcessId) + L")");
    return info.hProcess;
}

struct FindWindowOf {
    DWORD pid;
    bool found = false;
};

// Up: its message window exists (App made it; what --quit and the second
// start talk to). A first start can take long: the antivirus reads every new file.
bool cameUp(HANDLE process)
{
    FindWindowOf search {::GetProcessId(process)};
    for (int waited = 0; waited < 180000; waited += 250) {
        if (::WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            ::GetExitCodeProcess(process, &code);
            log(L"the new version ended at once (exit code " + std::to_wstring(code) + L")");
            return false;
        }
        HWND window = nullptr;
        while ((window = ::FindWindowExW(nullptr, window, L"WinShun.MessageWindow", nullptr))) {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(window, &pid);
            if (pid == search.pid)
                return true;
        }
        ::Sleep(250);
    }
    log(L"the new version did not come up in time");
    return false;
}

int portable(DWORD pid, const std::wstring& app, const std::wstring& stage, const std::wstring& backup, HANDLE winshun)
{
    if (winshun && ::WaitForSingleObject(winshun, 120000) != WAIT_OBJECT_0) {
        log(L"Win顺 (pid " + std::to_wstring(pid) + L") did not end: nothing replaced");
        ws::update::removeTree(stage);
        return 2;
    }
    // Without its handle (an elevated Win顺 kept it from us): its instance
    // mutex, which goes when it has ended.
    for (int waited = 0; instanceRunning() && waited < (winshun ? 30000 : 120000); waited += 250)
        ::Sleep(250);
    if (instanceRunning()) {
        log(L"Win顺 still runs: nothing replaced");
        ws::update::removeTree(stage);
        return 2;
    }
    endProcessesFrom(app);

    const std::wstring exe = app + L"\\WinShun.exe";
    const ws::update::SwapPlan plan {app, stage, backup};
    ws::update::SwapMoves moves;
    if (!ws::update::swapIn(plan, moves, log)) {
        ws::update::removeTree(stage);
        if (moves.backedUp.empty())
            ws::update::removeTree(backup);
        if (const HANDLE old = start(exe, app))
            ::CloseHandle(old);
        return 3;
    }
    const HANDLE fresh = start(exe, app);
    if (fresh && cameUp(fresh)) {
        ::CloseHandle(fresh);
        if (!ws::update::removeTree(backup))
            log(L"some of the backup was left: " + backup);
        ws::update::removeTree(stage);
        log(L"done");
        return 0;
    }
    if (fresh) {
        ::TerminateProcess(fresh, 1);
        ::WaitForSingleObject(fresh, 10000);
        ::CloseHandle(fresh);
    }
    endProcessesFrom(app);
    if (ws::update::rollBack(plan, moves, log)) {
        ws::update::removeTree(stage);
        ws::update::removeTree(backup);
    } else {
        log(L"kept " + backup + L" and " + stage + L" for a manual repair");
    }
    if (const HANDLE old = start(exe, app))
        ::CloseHandle(old);
    return 4;
}

int watchSetup(HANDLE setup, const std::wstring& exe)
{
    if (setup) {
        ::WaitForSingleObject(setup, INFINITE);
        DWORD code = 0;
        ::GetExitCodeProcess(setup, &code);
        log(L"Setup ended (exit code " + std::to_wstring(code) + L")");
    }
    // Its [Run] starts the new version without waiting: given time to take the mutex.
    ::Sleep(8000);
    if (instanceRunning())
        return 0;
    log(L"no Win顺 runs after Setup: starting the installed one");
    if (const HANDLE process = start(exe, {}))
        ::CloseHandle(process);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (!argv)
        return 1;
    std::map<std::wstring, std::wstring> options;
    std::wstring mode;
    for (int i = 1; i < count; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--portable" || arg == L"--watch-setup")
            mode = arg;
        else if (arg.rfind(L"--", 0) == 0 && i + 1 < count)
            options[arg] = argv[++i];
    }
    ::LocalFree(argv);
    g_logFile = options[L"--log"];
    if (!options[L"--instance"].empty())
        g_instance = options[L"--instance"];
    for (const wchar_t* key : {L"--app", L"--stage", L"--backup"}) { // "C:\\" -> "C:"
        std::wstring& folder = options[key];
        while (folder.size() > 2 && folder.back() == L'\\')
            folder.pop_back();
    }

    const HANDLE updating = ::CreateMutexW(nullptr, FALSE, L"Local\\WinShun.Updating");
    const DWORD pid = static_cast<DWORD>(std::wcstoul(options[L"--pid"].c_str(), nullptr, 10));
    const HANDLE process = pid ? ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!process)
        log(L"cannot hold process " + std::to_wstring(pid) + L" (" + std::to_wstring(::GetLastError()) + L")");
    if (const HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, options[L"--ready"].c_str())) {
        ::SetEvent(ready);
        ::CloseHandle(ready);
    }

    int code = 1;
    if (mode == L"--portable" && !options[L"--app"].empty() && !options[L"--stage"].empty()
        && !options[L"--backup"].empty()) {
        log(L"portable update of " + options[L"--app"]);
        code = portable(pid, options[L"--app"], options[L"--stage"], options[L"--backup"], process);
    } else if (mode == L"--watch-setup" && !options[L"--start"].empty()) {
        code = watchSetup(process, options[L"--start"]);
    } else {
        log(L"bad arguments");
    }
    if (process)
        ::CloseHandle(process);
    if (updating)
        ::CloseHandle(updating);
    return code;
}

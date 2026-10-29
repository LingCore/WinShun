// WinShunService.exe: the index service (GitHub #21). Win顺 itself runs with
// the user's normal rights; this service (LocalSystem, "WinShun") does the
// things those rights cannot: read the NTFS volumes' master file tables and
// their change journals, and, for the content index's first build, the
// contents of files off the volume by their MFT records (no file is opened,
// so the antivirus does not scan each one). It streams the parsed records
// over the pipe \\.\pipe\WinShun.Volumes (src/core/VolumeProtocol.h) to any
// interactive user's program that asks; the index itself stays in
// WinShun.exe. It never hands out raw sectors, raw records or handles; a
// file's contents go only to a client whose own token passes an access check
// (AccessCheck for FILE_GENERIC_READ) against that file's security
// descriptor, and only for files the service found itself from the folder
// record and name the client gave (never a path, a cluster or an offset). It
// holds no volume open between requests, and keeps nothing in memory.
// It also owns a UIAccess key-listener child per requesting GUI process,
// in that user's session, for double Ctrl over elevated / UIAccess windows.
// Only the listener crosses that boundary; the GUI and plugins stay normal.
//
//   WinShunService.exe                  as a service (the Service Control Manager starts it)
//   WinShunService.exe --install        registers and starts it (the installer; administrator)
//   WinShunService.exe --uninstall      stops and removes it
//   WinShunService.exe --console [--pipe NAME] [--no-privileges] [--no-bulk]
//                                       the same server in this console (administrator), for
//                                       tests: Win顺 and tst_service reach it with WINSHUN_PIPE=NAME;
//                                       --no-privileges drops every privilege but
//                                       SeChangeNotifyPrivilege first, disabling the input listener;
//                                       --no-bulk: without the bulk reads, as an older service
//
// Like Everything's service, it filters nothing by the caller's permissions
// where names are concerned: any interactive user on this computer can learn
// the names, folders, attributes and times of every file on every local NTFS
// volume. Contents it filters: each file's by the caller's own rights.
// docs/architecture.md says so.

#include "Log.h"
#include "InputListener.h"
#include "Server.h"
#include "ServiceLocation.h"
#include "VolumeProtocol.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace {

using ws::service::DeviceWatch;
using ws::service::Server;

constexpr wchar_t kDisplayName[] = L"Win顺 索引服务 (WinShun Index Service)";
constexpr wchar_t kDescription[]
    = L"Reads the NTFS volumes' master file tables and change journals for Win顺 (WinShun), which runs with normal "
      L"rights. 为以普通权限运行的 Win顺 读取 NTFS 磁盘的主文件表和变更日志。";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent = nullptr;
DeviceWatch* g_deviceWatch = nullptr;

bool dropPrivileges(bool inputListener);

std::wstring ownPath()
{
    wchar_t path[MAX_PATH * 2] {};
    const DWORD n = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return n > 0 && n < std::size(path) ? std::wstring(path, n) : std::wstring();
}

void report(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHintMs = 0)
{
    SERVICE_STATUS status {};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    status.dwWin32ExitCode = exitCode;
    status.dwWaitHint = waitHintMs;
    ::SetServiceStatus(g_statusHandle, &status);
}

DWORD WINAPI handler(DWORD control, DWORD eventType, void* eventData, void*)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        report(SERVICE_STOP_PENDING, NO_ERROR, 20'000);
        ::SetEvent(g_stopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    case SERVICE_CONTROL_DEVICEEVENT:
        if (g_deviceWatch)
            g_deviceWatch->onEvent(eventType, eventData);
        return NO_ERROR; // never vetoes: what is held is let go of
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// Runs the server until `stop` is set, telling `running` once it listens.
// The device watch takes its notifications from the service handle, or
// from a window of its own. Returns the Win32 error that kept it from
// listening, else 0.
template <typename Running>
DWORD serve(const std::wstring& pipeName, HANDLE stop, HANDLE recipient, DWORD recipientFlags, Running running,
    bool bulkReads = true)
{
    Server::Options options;
    options.pipeName = pipeName;
    options.bulkReads = bulkReads;
    options.keyListener = ws::service::canStartInputListener();
    Server server(options);
    DeviceWatch watch(recipient, recipientFlags, [&](const std::wstring& root) { server.closeVolume(root, 0, 2000); });
    g_deviceWatch = &watch;
    server.setDeviceWatch(&watch);
    DWORD error = 0;
    if (!server.start(&error)) {
        ws::service::log::write(L"cannot listen on \\\\.\\pipe\\%ls (error %lu)", pipeName.c_str(), error);
        g_deviceWatch = nullptr;
        return error ? error : ERROR_PIPE_BUSY;
    }
    running();
    ::WaitForSingleObject(stop, INFINITE);
    server.stop();
    g_deviceWatch = nullptr;
    return 0;
}

void WINAPI serviceMain(DWORD, wchar_t**)
{
    g_statusHandle = ::RegisterServiceCtrlHandlerExW(ws::volproto::kServiceName, handler, nullptr);
    if (!g_statusHandle)
        return;
    report(SERVICE_START_PENDING, NO_ERROR, 5000);
    if (!dropPrivileges(true)) {
        report(SERVICE_STOPPED, ::GetLastError());
        return;
    }
    g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ws::service::log::secureFolder(); // lock down %ProgramData%\WinShun before the first line
    ws::service::log::write(L"starting, version %hs", WINSHUN_VERSION);
    // An older portable copy may have registered the service from a
    // user-writable folder (a copy of WinShun run from Downloads, Desktop,
    // D:\tools...). Then a program with the user's normal rights could replace
    // this executable and have it run as SYSTEM at the next start. We do not
    // refuse to serve (that would only slow the user's search, and an
    // attacker's replacement would not check either), but we say so, loudly:
    // the settings offer a one-click "repair" that reinstalls into a protected
    // folder (installIndexService -> --install, which moves the copy).
    if (std::wstring why; !ws::svcloc::pathProtected(ownPath(), &why)) {
        ws::service::log::write(L"warning: this service runs from an unprotected location (%ls); it could be "
                                L"replaced by a non-administrator. Reinstall it from Win顺's settings to move it.",
            why.c_str());
        ws::service::log::toEventLog(L"The Win顺 (WinShun) index service is installed in a folder that "
                                     L"non-administrators can change (" + why
            + L"), which is a security risk. Reinstall it from Win顺's settings (搜索范围 → 文件夹) to move it to a "
              L"protected folder.");
    }
    // RUNNING only once the pipe is there; the start takes milliseconds.
    const DWORD error = serve(ws::volproto::kPipeName, g_stopEvent, g_statusHandle, DEVICE_NOTIFY_SERVICE_HANDLE,
        [] { report(SERVICE_RUNNING); });
    if (error)
        ws::service::log::toEventLog(L"WinShun index service: cannot listen on \\\\.\\pipe\\WinShun.Volumes (error "
            + std::to_wstring(error) + L")");
    report(SERVICE_STOPPED, error);
}

// Keep only traversal and (if requested) the three session-launch privileges.
// Enforced in console mode as well as by SCM, so integration tests cannot
// accidentally pass by relying on SYSTEM's other privileges.
bool dropPrivileges(bool inputListener)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token))
        return false;
    DWORD size = 0;
    ::GetTokenInformation(token, TokenPrivileges, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    bool ok = size > 0 && ::GetTokenInformation(token, TokenPrivileges, buffer.data(), size, &size);
    if (ok) {
        auto* privileges = reinterpret_cast<TOKEN_PRIVILEGES*>(buffer.data());
        LUID keep[4] {};
        const wchar_t* names[] {SE_CHANGE_NOTIFY_NAME, SE_TCB_NAME, SE_ASSIGNPRIMARYTOKEN_NAME, SE_INCREASE_QUOTA_NAME};
        const int count = inputListener ? 4 : 1;
        for (int i = 0; i < count; ++i)
            ::LookupPrivilegeValueW(nullptr, names[i], &keep[i]);
        for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
            LUID_AND_ATTRIBUTES& p = privileges->Privileges[i];
            bool retain = false;
            for (int k = 0; k < count; ++k)
                retain = retain || (p.Luid.LowPart == keep[k].LowPart && p.Luid.HighPart == keep[k].HighPart);
            if (!retain)
                p.Attributes = SE_PRIVILEGE_REMOVED;
        }
        ok = ::AdjustTokenPrivileges(token, FALSE, privileges, 0, nullptr, nullptr) && ::GetLastError() == ERROR_SUCCESS;
    }
    ::CloseHandle(token);
    return ok;
}

BOOL WINAPI onConsoleControl(DWORD)
{
    ::SetEvent(g_stopEvent);
    return TRUE;
}

int runConsole(const std::wstring& pipeName, bool noPrivileges, bool noBulk)
{
    ws::service::log::toConsole();
    if (!dropPrivileges(!noPrivileges)) {
        std::fwprintf(stderr, L"could not drop the privileges (error %lu)\n", ::GetLastError());
        return 1;
    }
    g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::SetConsoleCtrlHandler(onConsoleControl, TRUE);
    return static_cast<int>(serve(
        pipeName, g_stopEvent, nullptr, 0,
        [&] {
            // "ready" on stdout: the tests wait for it.
            std::fwprintf(stdout, L"ready: WinShun index service %hs on \\\\.\\pipe\\%ls; Ctrl+C stops it\n",
                WINSHUN_VERSION, pipeName.c_str());
            std::fflush(stdout);
        },
        !noBulk));
}

// ---- --install / --uninstall ------------------------------------------------------

struct ScHandle {
    SC_HANDLE value = nullptr;
    ~ScHandle()
    {
        if (value)
            ::CloseServiceHandle(value);
    }
};

int fail(const wchar_t* what)
{
    const DWORD error = ::GetLastError();
    std::fwprintf(stderr, L"%ls failed (error %lu)\n", what, error);
    return static_cast<int>(error ? error : 1);
}

// Waits for the service to reach `state`, `limitMs` at most.
bool waitForState(SC_HANDLE service, DWORD state, DWORD limitMs)
{
    const ULONGLONG deadline = ::GetTickCount64() + limitMs;
    for (;;) {
        SERVICE_STATUS status {};
        if (!::QueryServiceStatus(service, &status))
            return false;
        if (status.dwCurrentState == state)
            return true;
        if (::GetTickCount64() >= deadline)
            return false;
        ::Sleep(250);
    }
}

// Stops the service by name and waits for it to stop, so its executable can be
// replaced. Harmless when it is not installed or already stopped.
void stopServiceByName()
{
    ScHandle manager {::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value)
        return;
    ScHandle service {::OpenServiceW(manager.value, ws::volproto::kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS)};
    if (!service.value)
        return;
    SERVICE_STATUS status {};
    if (::ControlService(service.value, SERVICE_CONTROL_STOP, &status) || ::GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
        waitForState(service.value, SERVICE_STOPPED, 15'000);
}

// The executable path the SCM runs the service from, unquoted; empty when it
// is not installed or its configuration cannot be read. QueryServiceConfig is
// open to any user, which is how the settings judge the location as well.
std::wstring registeredImagePath()
{
    ScHandle manager {::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value)
        return {};
    ScHandle service {::OpenServiceW(manager.value, ws::volproto::kServiceName, SERVICE_QUERY_CONFIG)};
    if (!service.value)
        return {};
    DWORD needed = 0;
    ::QueryServiceConfigW(service.value, nullptr, 0, &needed);
    if (needed == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return {};
    std::vector<std::byte> buffer(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!::QueryServiceConfigW(service.value, config, needed, &needed) || !config->lpBinaryPathName)
        return {};
    std::wstring path = config->lpBinaryPathName;
    if (path.size() >= 2 && path.front() == L'"') { // registered as "C:\...\WinShunService.exe"
        const std::size_t close = path.find(L'"', 1);
        path = close != std::wstring::npos ? path.substr(1, close - 1) : path.substr(1);
    }
    return path;
}

// Registers (or re-points and reconfigures, if it already exists) the service
// to run `exePath`, and starts it. `exePath` is the final, protected location.
int registerService(const std::wstring& exePath)
{
    const std::wstring path = L"\"" + exePath + L"\"";
    ScHandle manager {::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT)};
    if (!manager.value)
        return fail(L"OpenSCManager");
    // LocalSystem: a virtual service account is not an administrator and
    // cannot open the volumes. Automatic start, not delayed: delayed would
    // come up minutes after the user's Win顺, which would walk the disks
    // meanwhile.
    ScHandle service {::CreateServiceW(manager.value, ws::volproto::kServiceName, kDisplayName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, path.c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr)};
    if (!service.value) {
        if (::GetLastError() != ERROR_SERVICE_EXISTS)
            return fail(L"CreateService");
        service.value = ::OpenServiceW(manager.value, ws::volproto::kServiceName, SERVICE_ALL_ACCESS);
        if (!service.value)
            return fail(L"OpenService");
        // Installed before (an upgrade, or from another folder): the path and
        // start type are made ours again.
        if (!::ChangeServiceConfigW(service.value, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, kDisplayName))
            return fail(L"ChangeServiceConfig");
    }
    SERVICE_DESCRIPTIONW description {const_cast<wchar_t*>(kDescription)};
    ::ChangeServiceConfig2W(service.value, SERVICE_CONFIG_DESCRIPTION, &description);
    // Volume reads still use the DACL. The extra privileges only launch the
    // user's UIAccess input listener (enabled on a private thread token).
    wchar_t privileges[] = L"SeChangeNotifyPrivilege\0SeTcbPrivilege\0SeAssignPrimaryTokenPrivilege\0SeIncreaseQuotaPrivilege\0";
    SERVICE_REQUIRED_PRIVILEGES_INFOW required {privileges};
    if (!::ChangeServiceConfig2W(service.value, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &required))
        return fail(L"ChangeServiceConfig2 privileges");
    SERVICE_SID_INFO sid {SERVICE_SID_TYPE_UNRESTRICTED};
    ::ChangeServiceConfig2W(service.value, SERVICE_CONFIG_SERVICE_SID_INFO, &sid);
    // Started again a second after a crash, three times a day.
    SC_ACTION actions[3] {{SC_ACTION_RESTART, 1000}, {SC_ACTION_RESTART, 1000}, {SC_ACTION_RESTART, 1000}};
    SERVICE_FAILURE_ACTIONSW failures {86400, nullptr, nullptr, 3, actions};
    ::ChangeServiceConfig2W(service.value, SERVICE_CONFIG_FAILURE_ACTIONS, &failures);
    // Required privileges are applied only at process creation. --install
    // is also used to upgrade/repoint an existing service from settings.
    SERVICE_STATUS current {};
    if (!::QueryServiceStatus(service.value, &current))
        return fail(L"QueryServiceStatus");
    if (current.dwCurrentState != SERVICE_STOPPED) {
        if (current.dwCurrentState != SERVICE_STOP_PENDING
            && !::ControlService(service.value, SERVICE_CONTROL_STOP, &current))
            return fail(L"ControlService stop for upgrade");
        if (!waitForState(service.value, SERVICE_STOPPED, 15'000))
            return fail(L"wait for service upgrade");
    }
    if (!::StartServiceW(service.value, 0, nullptr) && ::GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        return fail(L"StartService");
    if (!waitForState(service.value, SERVICE_RUNNING, 15'000)) {
        std::fwprintf(stderr, L"the service did not start in time\n");
        return 1;
    }
    std::fwprintf(stdout, L"installed and running: %ls\n", path.c_str());
    return 0;
}

int install()
{
    const std::wstring own = ownPath();
    if (own.empty())
        return fail(L"GetModuleFileName");
    ws::service::log::secureFolder(); // with full rights now: fix a weak log folder left by an older version
    // Already somewhere only administrators / SYSTEM can write (the Program
    // Files install, run by the installer): register it where it is.
    if (std::wstring why; ws::svcloc::pathProtected(own, &why))
        return registerService(own);
    // A portable copy, in a user-writable folder (Downloads, Desktop,
    // D:\tools...): copy it into the protected folder and register that copy,
    // or a program with the user's normal rights could replace this SYSTEM
    // service's executable and run as SYSTEM at the next start.
    const std::wstring dir = ws::svcloc::protectedServiceDir();
    if (dir.empty()) {
        std::fwprintf(stderr, L"could not locate Program Files\n");
        return static_cast<int>(ERROR_PATH_NOT_FOUND);
    }
    std::wstring error;
    if (!ws::svcloc::ensureProtectedDir(dir, &error)) {
        std::fwprintf(stderr, L"the protected folder is not usable: %ls\n", error.c_str());
        return static_cast<int>(ERROR_ACCESS_DENIED);
    }
    // Stop a copy a previous portable install registered here, so its file can
    // be replaced; then put this one in place and register it.
    stopServiceByName();
    std::wstring placed;
    if (!ws::svcloc::copyExeInto(dir, L"WinShunService.exe", own, &placed, &error)) {
        std::fwprintf(stderr, L"could not place the service in %ls: %ls\n", dir.c_str(), error.c_str());
        return static_cast<int>(ERROR_ACCESS_DENIED);
    }
    std::fwprintf(stdout, L"copied the service to %ls\n", placed.c_str());
    return registerService(placed);
}

int uninstall()
{
    // Where the service runs from, before it is removed: a copy this code
    // placed in the protected folder is deleted with it; the installer's {app}
    // copy (or any other) is left to whoever put it there.
    const std::wstring imagePath = registeredImagePath();
    ScHandle manager {::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value)
        return fail(L"OpenSCManager");
    ScHandle service {
        ::OpenServiceW(manager.value, ws::volproto::kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE)};
    if (!service.value) {
        if (::GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
            return 0; // nothing to do
        return fail(L"OpenService");
    }
    SERVICE_STATUS status {};
    if (::ControlService(service.value, SERVICE_CONTROL_STOP, &status) || ::GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
        waitForState(service.value, SERVICE_STOPPED, 15'000);
    if (!::DeleteService(service.value) && ::GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
        return fail(L"DeleteService");
    // Our placed copy and its folder (only when the service ran from there);
    // if the file is still held, it goes at the next restart.
    if (const std::wstring dir = ws::svcloc::protectedServiceDir();
        !dir.empty() && ws::svcloc::parentIs(imagePath, dir)) {
        if (!::DeleteFileW(imagePath.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND)
            ::MoveFileExW(imagePath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        if (!::RemoveDirectoryW(dir.c_str()))
            ::MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    std::fwprintf(stdout, L"removed\n");
    return 0;
}

int usage()
{
    std::fwprintf(stderr,
        L"WinShunService.exe: the index service of Win顺 (WinShun) %hs\n"
        L"  --install              register and start the service (as administrator)\n"
        L"  --uninstall            stop and remove it\n"
        L"  --console [--pipe NAME] [--no-privileges] [--no-bulk]\n"
        L"                         serve in this console instead (as administrator), for tests\n",
        WINSHUN_VERSION);
    return 1;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX); // never a dialog about a drive
    if (argc > 1) {
        const std::wstring_view option = argv[1];
        if (option == L"--listen-keys" && argc == 4) {
            wchar_t* end = nullptr;
            const unsigned long pid = std::wcstoul(argv[2], &end, 10);
            if (!pid || !end || *end)
                return ERROR_INVALID_PARAMETER;
            const auto window = std::wcstoull(argv[3], &end, 10);
            if (!window || !end || *end)
                return ERROR_INVALID_PARAMETER;
            return ws::service::runInputListener(pid, reinterpret_cast<HWND>(window));
        }
        if (option == L"--navigate-dialog" && argc == 4) {
            wchar_t* end = nullptr;
            const auto window = std::wcstoull(argv[2], &end, 10);
            if (!window || !end || *end)
                return ERROR_INVALID_PARAMETER;
            return ws::service::runDialogNav(reinterpret_cast<HWND>(window), argv[3]);
        }
        if (option == L"--install")
            return install();
        if (option == L"--uninstall")
            return uninstall();
        if (option == L"--console") {
            std::wstring pipe = ws::volproto::kPipeName;
            bool noPrivileges = false;
            bool noBulk = false;
            for (int i = 2; i < argc; ++i) {
                if (std::wstring_view(argv[i]) == L"--pipe" && i + 1 < argc)
                    pipe = argv[++i];
                else if (std::wstring_view(argv[i]) == L"--no-privileges")
                    noPrivileges = true;
                else if (std::wstring_view(argv[i]) == L"--no-bulk")
                    noBulk = true;
                else
                    return usage();
            }
            return runConsole(pipe, noPrivileges, noBulk);
        }
        return usage();
    }
    SERVICE_TABLE_ENTRYW table[] {{const_cast<wchar_t*>(ws::volproto::kServiceName), serviceMain}, {nullptr, nullptr}};
    if (!::StartServiceCtrlDispatcherW(table)) {
        if (::GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            return usage(); // started by hand
        return fail(L"StartServiceCtrlDispatcher");
    }
    return 0;
}

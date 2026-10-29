#include "InputListener.h"

#include "DialogNav.h"
#include "KeyListener.h"
#include "KeyListenerProtocol.h"
#include "Log.h"
#include "Server.h"
#include "ServiceLocation.h"
#include "Win32Util.h"

#include <objbase.h> // CoInitializeEx for the navigator's UI Automation
#include <psapi.h> // the input listener's working set (K32GetProcessMemoryInfo, in kernel32)
#include <userenv.h> // the user's own environment block for a session helper
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ws::service {
namespace {
using win32::UniqueHandle;

bool targetOf(HWND window, DWORD pid)
{
    DWORD owner = 0;
    wchar_t name[64] {};
    return ::GetWindowThreadProcessId(window, &owner) && owner == pid && ::GetClassNameW(window, name, 64)
        && std::wcscmp(name, keyproto::kWindowClass) == 0;
}

std::vector<std::byte> tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS kind)
{
    DWORD size = 0;
    ::GetTokenInformation(token, kind, nullptr, 0, &size);
    std::vector<std::byte> bytes(size);
    if (!size || !::GetTokenInformation(token, kind, bytes.data(), size, &size))
        bytes.clear();
    return bytes;
}

// Privileges are enabled on this thread's private token, never on a volume
// reader's thread. Reverted before waiting on a client or serving its data.
class LaunchPrivileges {
public:
    LaunchPrivileges()
    {
        HANDLE original = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_DUPLICATE, &original))
            return;
        const UniqueHandle owned(original);
        HANDLE copy = nullptr;
        if (!::DuplicateTokenEx(original, TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE, nullptr,
                SecurityImpersonation, TokenImpersonation, &copy))
            return;
        const UniqueHandle token(copy);
        for (const wchar_t* name : {SE_TCB_NAME, SE_ASSIGNPRIMARYTOKEN_NAME, SE_INCREASE_QUOTA_NAME}) {
            TOKEN_PRIVILEGES p {1};
            if (!::LookupPrivilegeValueW(nullptr, name, &p.Privileges[0].Luid))
                return;
            p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (!::AdjustTokenPrivileges(copy, FALSE, &p, 0, nullptr, nullptr) || ::GetLastError() != ERROR_SUCCESS)
                return;
        }
        active = ::SetThreadToken(nullptr, copy) != FALSE;
    }
    ~LaunchPrivileges()
    {
        if (active && !::RevertToSelf())
            std::terminate(); // never continue a server thread with the launch token
    }
    bool active = false;
};

// One helper of each kind per client process, including while it is being
// started: a key listener (for the connection's life) and a dialog navigator
// (for its one job) are counted apart, since the UI holds a listener open.
std::mutex claimsMutex;
std::set<DWORD> listeners;
std::set<DWORD> navigators;
struct Claim {
    Claim(std::set<DWORD>& registry, DWORD pid)
        : registry(registry)
        , pid(pid)
    {
        const std::lock_guard lock(claimsMutex);
        held = registry.insert(pid).second;
    }
    ~Claim()
    {
        if (held) {
            const std::lock_guard lock(claimsMutex);
            registry.erase(pid);
        }
    }
    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;
    std::set<DWORD>& registry;
    DWORD pid;
    bool held = false;
};

// An environment entry's name: up to its '=' (one that starts with '=', as
// "=C:=C:\x", keeps that one).
std::wstring_view nameOf(std::wstring_view entry)
{
    return entry.substr(0, std::min(entry.find(L'=', 1), entry.size()));
}

bool sameName(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size()
        && ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE)
        == CSTR_EQUAL;
}

// The environment block a session helper starts with: the user's own
// (CreateEnvironmentBlock for their token: their profile's folders and
// variables, nothing of the service's), except for the system's variables.
// Those come from HKCU\Environment too when the user names one there, and
// whatever the user runs at normal rights may write that key; a
// high-integrity uiAccess helper must not learn where Windows is from it (a
// %SystemRoot% expanded in a COM server's path, the PATH searched for a DLL
// loaded by name: the windir hijack of elevated tasks). So SystemRoot, windir,
// SystemDrive and ComSpec come from where Windows really is, PATH is
// System32, Windows and Wbem only, and TEMP / TMP are the user's profile's
// Temp: the helpers write no temporary files, this only keeps the two from
// pointing where some program chose. __COMPAT_LAYER (compatibility layers
// applied to the helper as it starts) is left out. Sorted by name, as
// CreateProcess wants it. Empty on failure.
std::vector<wchar_t> helperEnvironment(HANDLE token)
{
    void* raw = nullptr;
    if (!::CreateEnvironmentBlock(&raw, token, FALSE))
        return {};
    std::vector<std::wstring> entries;
    for (const wchar_t* entry = static_cast<const wchar_t*>(raw); *entry; entry += std::wcslen(entry) + 1)
        entries.emplace_back(entry);
    ::DestroyEnvironmentBlock(raw);

    wchar_t windows[MAX_PATH] {};
    wchar_t system[MAX_PATH] {};
    const UINT windowsLength = ::GetSystemWindowsDirectoryW(windows, MAX_PATH);
    const UINT systemLength = ::GetSystemDirectoryW(system, MAX_PATH);
    if (!windowsLength || windowsLength >= MAX_PATH || windows[1] != L':' || !systemLength || systemLength >= MAX_PATH) {
        ::SetLastError(ERROR_PATH_NOT_FOUND);
        return {};
    }
    std::wstring temp = std::wstring(windows) + L"\\Temp";
    wchar_t profile[MAX_PATH] {};
    DWORD profileLength = MAX_PATH;
    if (::GetUserProfileDirectoryW(token, profile, &profileLength))
        temp = std::wstring(profile) + L"\\AppData\\Local\\Temp";
    const std::pair<std::wstring_view, std::wstring> fixed[] {
        {L"ComSpec", std::wstring(system) + L"\\cmd.exe"},
        {L"Path", std::wstring(system) + L';' + windows + L';' + system + L"\\Wbem"},
        {L"SystemDrive", std::wstring(windows, 2)},
        {L"SystemRoot", windows},
        {L"TEMP", temp},
        {L"TMP", temp},
        {L"windir", windows},
    };
    std::erase_if(entries, [&fixed](const std::wstring& entry) {
        const std::wstring_view name = nameOf(entry);
        return sameName(name, L"__COMPAT_LAYER")
            || std::ranges::any_of(fixed, [name](const auto& pair) { return sameName(name, pair.first); });
    });
    for (const auto& [name, value] : fixed)
        entries.push_back(std::wstring(name) + L'=' + value);
    std::ranges::sort(entries, [](const std::wstring& a, const std::wstring& b) {
        const std::wstring_view x = nameOf(a);
        const std::wstring_view y = nameOf(b);
        return ::CompareStringOrdinal(x.data(), static_cast<int>(x.size()), y.data(), static_cast<int>(y.size()), TRUE)
            == CSTR_LESS_THAN;
    });
    std::vector<wchar_t> block;
    for (const std::wstring& entry : entries)
        block.insert(block.end(), entry.c_str(), entry.c_str() + entry.size() + 1);
    block.push_back(L'\0');
    return block;
}

// Launches the service's own executable, with `argsSuffix` appended, as a
// uiAccess + high-integrity process in the requesting client's interactive
// session (the only boundary the service crosses). Used for the double-Ctrl
// input listener (--listen-keys) and the elevated file-dialog navigator
// (--navigate-dialog, GitHub #104). The child runs on the user's own token:
// not SYSTEM, not a linked administrator token, no volume/file privileges.
bool launchSessionHelper(
    Connection& conn, const std::wstring& argsSuffix, HANDLE job, UniqueHandle& child, DWORD& childPid, const wchar_t*& operation)
{
    operation = L"validate listener session";
    DWORD session = 0;
    // Session 0 cannot resolve the user's HWNDs. The worker validates the
    // window class and owning PID after entering this authenticated session.
    if (!::GetNamedPipeClientSessionId(conn.channel.handle(), &session) || session == 0) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    operation = L"open client token";
    const UniqueHandle client(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, conn.pid));
    HANDLE clientTokenRaw = nullptr;
    if (!client.valid() || !::OpenProcessToken(client.get(), TOKEN_QUERY, &clientTokenRaw))
        return false;
    const UniqueHandle clientToken(clientTokenRaw);
    const auto clientUser = tokenInfo(clientToken.get(), TokenUser);
    operation = L"enable launch privileges";
    const LaunchPrivileges privileges;
    if (!privileges.active)
        return false;
    operation = L"WTSQueryUserToken";
    HANDLE userRaw = nullptr;
    if (!::WTSQueryUserToken(session, &userRaw))
        return false;
    const UniqueHandle user(userRaw);
    const auto sessionUser = tokenInfo(user.get(), TokenUser);
    operation = L"validate session user";
    if (clientUser.empty() || sessionUser.empty()
        || !::EqualSid(reinterpret_cast<const TOKEN_USER*>(clientUser.data())->User.Sid,
            reinterpret_cast<const TOKEN_USER*>(sessionUser.data())->User.Sid)) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    // WTS supplies the session's user token, never SYSTEM or a linked
    // administrator token. UIAccess does not grant volume/file privileges.
    operation = L"DuplicateTokenEx";
    HANDLE tokenRaw = nullptr;
    if (!::DuplicateTokenEx(user.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &tokenRaw))
        return false;
    const UniqueHandle token(tokenRaw);
    DWORD uiAccess = 1;
    BYTE sid[SECURITY_MAX_SID_SIZE] {};
    DWORD sidSize = sizeof sid;
    TOKEN_MANDATORY_LABEL label {{sid, SE_GROUP_INTEGRITY}};
    operation = L"set listener integrity and UIAccess";
    if (!::CreateWellKnownSid(WinHighLabelSid, nullptr, sid, &sidSize)
        || !::SetTokenInformation(token.get(), TokenIntegrityLevel, &label, sizeof label + sidSize)
        || !::SetTokenInformation(token.get(), TokenUIAccess, &uiAccess, sizeof uiAccess)
        || !::AdjustTokenPrivileges(token.get(), TRUE, nullptr, 0, nullptr, nullptr))
        return false;
    // High IL protects the listener from writes/injection by medium-IL
    // processes. Only the listener gets this label; the GUI stays medium.
    operation = L"resolve listener executable";
    wchar_t exe[32768] {};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (!n || n >= std::size(exe))
        return false;
    wchar_t system[MAX_PATH] {};
    if (!::GetSystemDirectoryW(system, MAX_PATH))
        return false;
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + argsSuffix;
    // The user's own environment with the system's variables fixed
    // (helperEnvironment), not the service's (LocalSystem's profile folders
    // and variables); never inherited.
    operation = L"CreateEnvironmentBlock";
    std::vector<wchar_t> environment = helperEnvironment(token.get());
    if (environment.empty())
        return false;
    STARTUPINFOW startup {sizeof startup};
    wchar_t desktop[] = L"winsta0\\default";
    startup.lpDesktop = desktop;
    PROCESS_INFORMATION process {};
    operation = L"CreateProcessAsUser";
    if (!::CreateProcessAsUserW(token.get(), exe, command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), system, &startup,
            &process))
        return false;
    child.reset(process.hProcess);
    const UniqueHandle thread(process.hThread);
    operation = L"assign listener job and resume";
    if (!::AssignProcessToJobObject(job, child.get()) || ::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const DWORD error = ::GetLastError();
        ::TerminateProcess(child.get(), error); // only our suspended child, never the user's program
        ::SetLastError(error);
        return false;
    }
    childPid = process.dwProcessId;
    return true;
}

std::wstring processImagePath(DWORD pid)
{
    const UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.valid())
        return {};
    wchar_t path[MAX_PATH * 2] {};
    DWORD size = static_cast<DWORD>(std::size(path));
    return ::QueryFullProcessImageNameW(process.get(), 0, path, &size) ? std::wstring(path, size) : std::wstring();
}
} // namespace

// The dialog navigator of GitHub #104 is for Win顺's own UI only: the client
// must be WinShun.exe sitting next to this service executable. (The double-tap
// listener is not kept so; Server::handshake says why.) The pipe's ACL
// already keeps out AppContainer and low-integrity callers.
bool requesterIsWinShun(DWORD pid)
{
    const std::wstring client = processImagePath(pid);
    wchar_t ownPath[MAX_PATH * 2] {};
    const DWORD n = ::GetModuleFileNameW(nullptr, ownPath, static_cast<DWORD>(std::size(ownPath)));
    return !client.empty() && n && n < std::size(ownPath)
        && svcloc::isWinShunBeside(client, std::wstring_view(ownPath, n));
}

bool canStartInputListener()
{
    // Keep these disabled on the process token. A launch temporarily enables
    // them only on its own impersonation token, even while volume readers run.
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &raw))
        return false;
    const UniqueHandle token(raw);
    for (const wchar_t* name : {SE_TCB_NAME, SE_ASSIGNPRIMARYTOKEN_NAME, SE_INCREASE_QUOTA_NAME}) {
        TOKEN_PRIVILEGES p {1};
        if (!::LookupPrivilegeValueW(nullptr, name, &p.Privileges[0].Luid)
            || !::AdjustTokenPrivileges(raw, FALSE, &p, 0, nullptr, nullptr))
            return false;
    }
    const LaunchPrivileges privileges;
    return privileges.active;
}

void serveInputListener(Server& server, Connection& conn, std::span<const std::byte> payload)
{
    using namespace volproto;
    Reader r(payload);
    std::uint64_t value = 0;
    if (!r.u64(value) || !r.done() || !value) {
        replyError(conn, ERROR_INVALID_PARAMETER, L"invalid listener window", 1000);
        return;
    }
    // For any client the pipe lets in, unlike dialog navigation (Server::handshake says why).
    const Claim claim(listeners, conn.pid);
    if (!claim.held) {
        replyError(conn, ERROR_ALREADY_EXISTS, L"already listening for this process", 1000);
        return;
    }
    // Closing this job handles every exit path, pipe loss, service shutdown
    // and crash alike. The worker never outlives the request that owns it.
    const UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    UniqueHandle child;
    DWORD pid = 0;
    const wchar_t* operation = L"create listener job";
    const std::wstring args = L"--listen-keys " + std::to_wstring(conn.pid) + L" " + std::to_wstring(value);
    if (!server.options().keyListener || !job.valid()
        || !::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)
        || !launchSessionHelper(conn, args, job.get(), child, pid, operation)) {
        const DWORD error = server.options().keyListener ? ::GetLastError() : ERROR_NOT_SUPPORTED;
        log::write(L"input listener for pid %lu: %ls failed (%lu)", conn.pid, operation, error);
        replyError(conn, error, operation, 1000);
        return;
    }
    Writer w;
    w.u32(pid);
    if (!conn.channel.write(w.finish(Type::KeyListenerReady), 1000))
        return;
    log::write(L"input listener %lu for pid %lu", pid, conn.pid);
    Type type {};
    std::vector<std::byte> next;
    while (conn.channel.read(type, next, keyproto::kLeaseMs, conn.abort)) {
        if (type != Type::KeyListenerPing || !next.empty() || ::WaitForSingleObject(child.get(), 0) != WAIT_TIMEOUT
            || !reply(conn, Type::KeyListenerPing, 1000))
            break;
    }
}

int runInputListener(DWORD clientPid, HWND target)
{
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw))
        return ERROR_ACCESS_DENIED;
    const UniqueHandle token(raw);
    DWORD uiAccess = 0, size = 0;
    const auto integrity = tokenInfo(token.get(), TokenIntegrityLevel);
    if (!::GetTokenInformation(token.get(), TokenUIAccess, &uiAccess, sizeof uiAccess, &size) || !uiAccess
        || integrity.empty())
        return ERROR_ACCESS_DENIED;
    const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
    if (*::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1) < SECURITY_MANDATORY_HIGH_RID)
        return ERROR_ACCESS_DENIED;
    const UniqueHandle client(::OpenProcess(SYNCHRONIZE, FALSE, clientPid));
    if (!client.valid() || !targetOf(target, clientPid))
        return ERROR_INVALID_WINDOW_HANDLE;
    KeyListener::Callbacks callbacks;
    callbacks.doubleCtrl = [=](const KeyListener::Tap& tap) {
        if (!targetOf(target, clientPid))
            return;
        // UIAccess may grant the GUI foreground activation over an elevated
        // program. The GUI still applies its game/exclusion policy first.
        ::AllowSetForegroundWindow(clientPid);
        ::PostMessageW(target, keyproto::kTap, keyproto::tapWord(::GetCurrentProcessId(), tap.key, !tap.sent.empty()),
            keyproto::tapTimes(tap.time, tap.firstPress ? tap.time - *tap.firstPress : 0));
    };
    KeyListener listener(std::move(callbacks));
    if (!listener.isActive())
        return ERROR_NOT_READY;
    listener.setKeys(KeyListener::Keys::All); // the GUI keeps the one set, and its side
    // The first to see a double tap: its pages kept in memory (GitHub #111).
    // Trimmed away under memory pressure, the next tap waited for ~100 pages
    // read back from the disk before the GUI was told. A hard floor at what
    // it uses once running (~8 MB, most of it Windows' own DLLs shared with
    // every program) keeps Windows' trimming off it. It takes no privilege
    // (measured 2026-10-27, medium and high integrity). A program emptying
    // it on purpose (EmptyWorkingSet, a "memory cleaner") still does.
    PROCESS_MEMORY_COUNTERS memory {};
    memory.cb = sizeof memory;
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &memory, sizeof memory)) {
        constexpr SIZE_T margin = 1 << 20, most = 16 << 20;
        const SIZE_T keep = std::min(memory.WorkingSetSize + margin, most);
        if (!::SetProcessWorkingSetSizeEx(::GetCurrentProcess(), keep, keep + (32 << 20),
                QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE))
            log::write(L"input listener: working set floor of %zu KB refused (%lu)", keep / 1024, ::GetLastError());
    }
    do {
        if (!targetOf(target, clientPid))
            break;
        ::PostMessageW(target, keyproto::kReady, ::GetCurrentProcessId(), keyproto::kReportsAlt | keyproto::kReportsShift);
    } while (::WaitForSingleObject(client.get(), keyproto::kHeartbeatMs) == WAIT_TIMEOUT);
    return 0;
}

void serveDialogNav(Server& server, Connection& conn, std::span<const std::byte> payload)
{
    using namespace volproto;
    Reader r(payload);
    std::uint64_t window = 0;
    std::wstring folder;
    // The folder is checked as a string only: an absolute path without quotes
    // or control characters, of a sane length. Whether it exists is the
    // helper's to check, as the user; the service never opens it as SYSTEM.
    if (!r.u64(window) || !r.string(folder) || !r.done() || !window || !dialognav::isValidFolderRequest(folder)) {
        replyError(conn, ERROR_INVALID_PARAMETER, L"invalid dialog navigation request", 1000);
        return;
    }
    // The requester must be an interactive session, and Win顺's own UI.
    DWORD session = 0;
    if (!::GetNamedPipeClientSessionId(conn.channel.handle(), &session) || session == 0
        || !requesterIsWinShun(conn.pid)) {
        replyError(conn, ERROR_ACCESS_DENIED, L"not authorised for dialog navigation", 1000);
        return;
    }
    // One navigator at a time per client; held until the job below has closed
    // (it is destroyed first), so a lingering helper still counts.
    const Claim claim(navigators, conn.pid);
    if (!claim.held) {
        replyError(conn, ERROR_BUSY, L"already navigating for this process", 1000);
        return;
    }
    const UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    UniqueHandle child;
    DWORD childPid = 0;
    const wchar_t* operation = L"create navigator job";
    // Quoted by the CommandLineToArgvW rules, so a drive root ("C:\" ->
    // "C:\\") arrives intact; isValidFolderRequest has refused quotes anyway.
    const std::wstring args = L"--navigate-dialog " + std::to_wstring(window) + L" " + dialognav::quoteArgument(folder);
    if (!server.options().keyListener || !job.valid()
        || !::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)
        || !launchSessionHelper(conn, args, job.get(), child, childPid, operation)) {
        const DWORD error = server.options().keyListener ? ::GetLastError() : ERROR_NOT_SUPPORTED;
        log::write(L"dialog nav for pid %lu: %ls failed (%lu)", conn.pid, operation, error);
        replyError(conn, error, operation, 1000);
        return;
    }
    // The navigator does its one job and exits; a bounded wait, then the job
    // closing here kills it if it somehow lingered.
    DWORD code = static_cast<DWORD>(WAIT_TIMEOUT);
    if (::WaitForSingleObject(child.get(), 10000) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        code = ::GetExitCodeProcess(child.get(), &exitCode) ? exitCode : ERROR_PROCESS_ABORTED;
    }
    if (code == 0) {
        log::write(L"dialog nav %lu for pid %lu: navigated", childPid, conn.pid);
        reply(conn, Type::DialogNavDone, 1000);
    } else {
        log::write(L"dialog nav %lu for pid %lu: did not go (exit %lu)", childPid, conn.pid, code);
        replyError(conn, code ? code : ERROR_GEN_FAILURE, L"the dialog did not go to the folder", 1000);
    }
}

namespace {
// runDialogNav's checks and its one job. May throw: what it reads (a control's
// text, a UI Automation name, a module's path) is sized by other programs.
int navigateDialog(HWND dialog, const std::wstring& folder)
{
    // Must be the uiAccess + high-integrity helper the service launches: the
    // same check as the input listener.
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw))
        return ERROR_ACCESS_DENIED;
    const UniqueHandle token(raw);
    DWORD uiAccess = 0, size = 0;
    const auto integrity = tokenInfo(token.get(), TokenIntegrityLevel);
    if (!::GetTokenInformation(token.get(), TokenUIAccess, &uiAccess, sizeof uiAccess, &size) || !uiAccess
        || integrity.empty())
        return ERROR_ACCESS_DENIED;
    const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
    if (*::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1) < SECURITY_MANDATORY_HIGH_RID)
        return ERROR_ACCESS_DENIED;
    // The window must be a supported file dialog in this very session.
    if (!dialog || !::IsWindow(dialog) || dialognav::classify(dialog) == dialognav::Kind::None)
        return ERROR_INVALID_WINDOW_HANDLE;
    DWORD winPid = 0, winSession = 0, ownSession = 0;
    ::GetWindowThreadProcessId(dialog, &winPid);
    if (!winPid || !::ProcessIdToSessionId(winPid, &winSession)
        || !::ProcessIdToSessionId(::GetCurrentProcessId(), &ownSession) || winSession != ownSession)
        return ERROR_ACCESS_DENIED;
    // Its process must be at least a normal UI's integrity (never drive a
    // low-integrity / AppContainer window on request).
    const UniqueHandle dialogProc(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, winPid));
    HANDLE dialogTokenRaw = nullptr;
    if (!dialogProc.valid() || !::OpenProcessToken(dialogProc.get(), TOKEN_QUERY, &dialogTokenRaw))
        return ERROR_ACCESS_DENIED;
    const UniqueHandle dialogToken(dialogTokenRaw);
    const auto dialogIl = tokenInfo(dialogToken.get(), TokenIntegrityLevel);
    if (dialogIl.empty())
        return ERROR_ACCESS_DENIED;
    const PSID dsid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(dialogIl.data())->Label.Sid;
    if (*::GetSidSubAuthority(dsid, *::GetSidSubAuthorityCount(dsid) - 1) < SECURITY_MANDATORY_MEDIUM_RID)
        return ERROR_ACCESS_DENIED;
    // The folder: the service's string checks again, then that it exists; this
    // process runs as the user, so it is their view.
    if (!dialognav::isValidFolderRequest(folder))
        return ERROR_INVALID_PARAMETER;
    if (!dialognav::isExistingDirectory(folder.c_str()))
        return ERROR_PATH_NOT_FOUND;
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED); // UI Automation for SHBrowseForFolder
    const bool went = dialognav::navigateToFolder(dialog, folder);
    ::CoUninitialize();
    return went ? 0 : static_cast<int>(ERROR_NOT_FOUND);
}
} // namespace

int runDialogNav(HWND dialog, const std::wstring& folder)
{
    // Nothing escapes the helper's one job: a failed allocation (or any other
    // exception) ends it as not done, which the service reports as such.
    try {
        return navigateDialog(dialog, folder);
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}
} // namespace ws::service

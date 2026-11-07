#include "InputListener.h"

#include "DialogNav.h"
#include "KeyListener.h"
#include "KeyListenerProtocol.h"
#include "Log.h"
#include "OwnInput.h"
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
#include <latch>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
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
std::set<DWORD> displays;
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
std::vector<wchar_t> helperEnvironment(HANDLE token, bool display = false)
{
    void* raw = nullptr;
    if (!::CreateEnvironmentBlock(&raw, token, FALSE))
        return {};
    std::vector<std::wstring> entries;
    for (const wchar_t* entry = static_cast<const wchar_t*>(raw); *entry; entry += std::wcslen(entry) + 1)
        entries.emplace_back(entry);
    ::DestroyEnvironmentBlock(raw);
    if (display) {
        // Qt can load native plugins and QML modules from environment paths.
        // A UIAccess renderer must not inherit user-selected code loaders.
        constexpr std::wstring_view allowed[] {L"USERPROFILE", L"APPDATA", L"LOCALAPPDATA", L"USERNAME",
            L"USERDOMAIN", L"USERDOMAIN_ROAMINGPROFILE", L"HOMEDRIVE", L"HOMEPATH", L"SESSIONNAME"};
        std::erase_if(entries, [&allowed](const std::wstring& entry) {
            return !std::ranges::any_of(allowed, [&](auto name) { return sameName(nameOf(entry), name); });
        });
        entries.emplace_back(L"QML_DISABLE_DISK_CACHE=1");
    }

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
// (--navigate-dialog, GitHub #104), and the protected clipboard renderer.
// The child runs on the user's own token:
// not SYSTEM, not a linked administrator token, no volume/file privileges.
bool launchSessionHelper(
    Connection& conn, const std::wstring& argsSuffix, HANDLE job, UniqueHandle& child, DWORD& childPid, const wchar_t*& operation,
    bool display = false)
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
    std::wstring executable(exe);
    if (display) {
        executable.resize(executable.find_last_of(L"\\/") + 1);
        executable += L"WinShun.exe";
        operation = L"validate protected clipboard display";
        if (!svcloc::pathProtected(executable) || !requesterIsWinShun(conn.pid)) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return false;
        }
    }
    std::wstring command = L"\"" + executable + L"\" " + argsSuffix;
    // The user's own environment with the system's variables fixed
    // (helperEnvironment), not the service's (LocalSystem's profile folders
    // and variables); never inherited.
    operation = L"CreateEnvironmentBlock";
    std::vector<wchar_t> environment = helperEnvironment(token.get(), display);
    if (environment.empty())
        return false;
    STARTUPINFOW startup {sizeof startup};
    wchar_t desktop[] = L"winsta0\\default";
    startup.lpDesktop = desktop;
    PROCESS_INFORMATION process {};
    operation = L"CreateProcessAsUser";
    if (!::CreateProcessAsUserW(token.get(), executable.c_str(), command.data(), nullptr, nullptr, FALSE,
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

// A process's integrity level (SECURITY_MANDATORY_*_RID), or none when it
// cannot be read: a protected process such as csrss.exe, whose error boxes
// ("the program can't start because a DLL is missing") come to the front too.
std::optional<DWORD> integrityOf(DWORD pid)
{
    const UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE raw = nullptr;
    if (!process.valid() || !::OpenProcessToken(process.get(), TOKEN_QUERY, &raw))
        return std::nullopt;
    const UniqueHandle token(raw);
    const auto label = tokenInfo(token.get(), TokenIntegrityLevel);
    if (label.empty())
        return std::nullopt;
    const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
    return *::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1);
}

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

// GitHub #243: the UI's global hotkeys with no modifier (keyproto::relayable:
// F1-F24, PrintScreen). Windows gives them to no normal-rights RegisterHotKey
// while a window above the UI's integrity is in front (run as administrator;
// docs/pitfalls.md). Over such a window this holds the key back from it, as
// the UI's RegisterHotKey would have, and tells the UI. Over any other the
// UI's own registration hears the key: the low-level hook is in only while
// such a window is in front, so no key anyone types waits on this otherwise,
// and a key is never told twice. One relay per listener, on its own thread.
class PlainKeyRelay {
public:
    PlainKeyRelay(DWORD clientPid, HWND target, const std::vector<std::uint32_t>& keys)
        : m_clientPid(clientPid)
        , m_target(target)
        , m_clientIntegrity(integrityOf(clientPid).value_or(SECURITY_MANDATORY_MEDIUM_RID))
    {
        for (const std::uint32_t vk : keys)
            m_wanted[vk & 0xFF] = keyproto::relayable(vk);
        std::latch ready(1);
        m_thread = std::thread([this, &ready] {
            MSG msg;
            ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the queue, for WM_QUIT
            m_threadId = ::GetCurrentThreadId();
            ready.count_down();
            run();
        });
        ready.wait();
    }
    ~PlainKeyRelay()
    {
        ::PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        if (m_thread.joinable())
            m_thread.join();
    }
    PlainKeyRelay(const PlainKeyRelay&) = delete;
    PlainKeyRelay& operator=(const PlainKeyRelay&) = delete;

private:
    void run()
    {
        s_relay = this;
        // A hook answering late holds up every key typed into that window.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const HWINEVENTHOOK events = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            &PlainKeyRelay::onForeground, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        // Looked at again now and then too: a missed change, or the hook
        // Windows took away from a thread that once answered too late.
        const UINT_PTR timer = ::SetTimer(nullptr, 0, 2000, &PlainKeyRelay::onTimer);
        follow(::GetForegroundWindow(), false);
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&msg);
        ::KillTimer(nullptr, timer);
        if (events)
            ::UnhookWinEvent(events);
        hook(false);
        s_relay = nullptr;
    }

    // Whether the window in front is above the UI: Windows keeps the UI's
    // plain hotkeys from it. One whose process cannot be read counts as above.
    bool above(HWND window) const
    {
        DWORD pid = 0;
        if (!window || !::GetWindowThreadProcessId(window, &pid) || !pid || pid == m_clientPid)
            return false;
        const std::optional<DWORD> integrity = integrityOf(pid);
        return !integrity || *integrity > m_clientIntegrity;
    }

    // `again`: hooked already, hooked anew (the timer's; Windows drops a
    // hook that answered too late without saying so), a key held still held.
    void follow(HWND window, bool again)
    {
        m_front = window;
        m_above = above(window);
        if (again && m_above && m_hook) {
            ::UnhookWindowsHookEx(m_hook);
            m_hook = nullptr;
        }
        hook(m_above);
    }

    void hook(bool on)
    {
        if (on && !m_hook) {
            m_hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &PlainKeyRelay::keyboard, ::GetModuleHandleW(nullptr), 0);
            if (!m_hook && !std::exchange(m_hookRefused, true))
                log::write(L"input listener: plain hotkeys over administrator windows: hook refused (%lu)", ::GetLastError());
        } else if (!on && m_hook) {
            ::UnhookWindowsHookEx(m_hook);
            m_hook = nullptr;
            m_held.fill(false); // their releases go wherever they go now
        }
    }

    // True: held back from the window in front (told of, or its repeat or release).
    bool onKey(const KBDLLHOOKSTRUCT& event, bool up)
    {
        const std::uint32_t vk = event.vkCode & 0xFF;
        if (!m_wanted[vk] || isOwnInput(event.dwExtraInfo))
            return false;
        if (up)
            return std::exchange(m_held[vk], false);
        if (m_held[vk] && held(static_cast<int>(vk)))
            return true; // a repeat (MOD_NOREPEAT)
        m_held[vk] = false;
        // Changed since the last event about it: as it is now, without
        // hooking or unhooking from inside the hook.
        const HWND front = ::GetForegroundWindow();
        const bool over = front == m_front ? m_above : above(front);
        if (!over || held(VK_SHIFT) || held(VK_CONTROL) || held(VK_MENU) || held(VK_LWIN) || held(VK_RWIN)
            || !targetOf(m_target, m_clientPid))
            return false; // with a modifier it is another hotkey, which reaches the UI by itself
        m_held[vk] = true;
        ::AllowSetForegroundWindow(m_clientPid); // as for a double tap: the UI may come to the front
        ::PostMessageW(m_target, keyproto::kHotkey,
            keyproto::hotkeyWord(::GetCurrentProcessId(), vk, (event.flags & LLKHF_INJECTED) != 0), event.time);
        return true;
    }

    static LRESULT CALLBACK keyboard(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && s_relay) {
            const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
            if ((up || wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) && s_relay->onKey(event, up))
                return 1;
        }
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    static void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD)
    {
        if (s_relay)
            s_relay->follow(window, false);
    }

    static void CALLBACK onTimer(HWND, UINT, UINT_PTR, DWORD)
    {
        if (s_relay)
            s_relay->follow(::GetForegroundWindow(), true);
    }

    static inline PlainKeyRelay* s_relay = nullptr; // the relay thread's; one per listener
    const DWORD m_clientPid;
    const HWND m_target;
    const DWORD m_clientIntegrity;
    std::array<bool, 256> m_wanted {};
    // The rest on the relay thread only.
    std::array<bool, 256> m_held {}; // told of, not let go of yet
    HWND m_front = nullptr;
    bool m_above = false;
    HHOOK m_hook = nullptr;
    bool m_hookRefused = false;
    DWORD m_threadId = 0;
    std::thread m_thread;
};

std::wstring processImagePath(DWORD pid)
{
    const UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.valid())
        return {};
    wchar_t path[MAX_PATH * 2] {};
    DWORD size = static_cast<DWORD>(std::size(path));
    return ::QueryFullProcessImageNameW(process.get(), 0, path, &size) ? std::wstring(path, size) : std::wstring();
}

std::wstring ownImagePath()
{
    wchar_t path[MAX_PATH * 2] {};
    const DWORD n = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return n && n < std::size(path) ? std::wstring(path, n) : std::wstring();
}
} // namespace

// The session helpers that matter are for Win顺's own UI only (the double-tap
// listener is not kept so; Server::handshake says why). The pipe's ACL
// already keeps out AppContainer and low-integrity callers.
bool requesterIsWinShun(DWORD pid)
{
    const std::wstring client = processImagePath(pid);
    const std::wstring own = ownImagePath();
    return !client.empty() && !own.empty() && svcloc::isWinShunBeside(client, own);
}

bool mayNavigateDialogs(const Server& server, DWORD pid)
{
    const std::wstring client = processImagePath(pid);
    const std::wstring own = ownImagePath();
    return !client.empty() && !own.empty() && svcloc::servesDialogNav(client, own, server.options().installedFrom);
}

bool canStartClipboardDisplay(DWORD pid)
{
    return requesterIsWinShun(pid) && svcloc::pathProtected(processImagePath(pid));
}

void serveClipboardDisplay(Server& server, Connection& conn, std::span<const std::byte> payload)
{
    using namespace volproto;
    Reader r(payload);
    std::uint64_t value = 0;
    std::wstring pipe;
    const std::wstring prefix = L"WinShun.Clipboard." + std::to_wstring(conn.pid) + L".";
    if (!r.u64(value) || !r.string(pipe) || !r.done() || !value
        || pipe.size() != prefix.size() + 32 || !pipe.starts_with(prefix)
        || !std::all_of(pipe.begin() + prefix.size(), pipe.end(), [](wchar_t c) {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
        })) {
        replyError(conn, ERROR_INVALID_PARAMETER, L"invalid clipboard display request", 1000);
        return;
    }
    if (!server.options().keyListener || !canStartClipboardDisplay(conn.pid)) {
        replyError(conn, ERROR_ACCESS_DENIED, L"clipboard display requires the protected WinShun install", 1000);
        return;
    }
    const Claim claim(displays, conn.pid);
    if (!claim.held) {
        replyError(conn, ERROR_BUSY, L"clipboard display already leased", 1000);
        return;
    }
    const UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    UniqueHandle child;
    DWORD pid = 0;
    const wchar_t* operation = L"create clipboard display job";
    const std::wstring args = L"--clipboard-display --display-client " + std::to_wstring(conn.pid)
        + L" --display-window " + std::to_wstring(value) + L" --display-pipe " + pipe;
    // Session 0 cannot resolve interactive HWNDs. The display validates this
    // HWND against the authenticated caller PID after entering that session.
    if (!job.valid() || !::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)
        || !launchSessionHelper(conn, args, job.get(), child, pid, operation, true)) {
        replyError(conn, ::GetLastError(), operation, 1000);
        return;
    }
    Writer w;
    w.u32(pid);
    if (!conn.channel.write(w.finish(Type::ClipboardDisplayReady), 1000))
        return;
    Type type {};
    std::vector<std::byte> next;
    while (conn.channel.read(type, next, keyproto::kLeaseMs, conn.abort)) {
        if (type != Type::ClipboardDisplayPing || !next.empty()
            || ::WaitForSingleObject(child.get(), 0) != WAIT_TIMEOUT
            || !reply(conn, Type::ClipboardDisplayPing, 1000))
            break;
    }
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
    if (!r.u64(value) || !value) {
        replyError(conn, ERROR_INVALID_PARAMETER, L"invalid listener window", 1000);
        return;
    }
    // Minor 6 (#243): the UI's plain hotkeys to hold back over administrator
    // windows, F1-F24 and PrintScreen alone.
    std::vector<std::uint32_t> keys;
    if (!r.done()) {
        std::uint32_t count = 0;
        if (!r.u32(count) || count > keyproto::kMaxRelayedKeys) {
            replyError(conn, ERROR_INVALID_PARAMETER, L"invalid hotkeys to relay", 1000);
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t vk = 0;
            if (!r.u32(vk) || !keyproto::relayable(vk) || std::ranges::find(keys, vk) != keys.end()) {
                replyError(conn, ERROR_INVALID_PARAMETER, L"invalid hotkeys to relay", 1000);
                return;
            }
            keys.push_back(vk);
        }
        if (!r.done()) {
            replyError(conn, ERROR_INVALID_PARAMETER, L"invalid listener request", 1000);
            return;
        }
    }
    // Double taps for any client the pipe lets in, unlike dialog navigation
    // (Server::handshake says why). Keys held back from an administrator's
    // window only for Win顺's own UI, as the navigator acts there.
    if (!keys.empty() && !mayNavigateDialogs(server, conn.pid)) {
        replyError(conn, ERROR_ACCESS_DENIED, L"hotkeys are relayed for Win顺's own UI only", 1000);
        return;
    }
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
    std::wstring args = L"--listen-keys " + std::to_wstring(conn.pid) + L" " + std::to_wstring(value);
    for (std::size_t i = 0; i < keys.size(); ++i)
        args += (i ? L"," : L" ") + std::to_wstring(keys[i]);
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
    log::write(L"input listener %lu for pid %lu, %zu plain hotkeys relayed", pid, conn.pid, keys.size());
    Type type {};
    std::vector<std::byte> next;
    while (conn.channel.read(type, next, keyproto::kLeaseMs, conn.abort)) {
        if (type != Type::KeyListenerPing || !next.empty() || ::WaitForSingleObject(child.get(), 0) != WAIT_TIMEOUT
            || !reply(conn, Type::KeyListenerPing, 1000))
            break;
    }
}

int runInputListener(DWORD clientPid, HWND target, const std::vector<std::uint32_t>& keys)
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
    std::optional<PlainKeyRelay> relay;
    if (!keys.empty())
        relay.emplace(clientPid, target, keys);
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
        || !mayNavigateDialogs(server, conn.pid)) {
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

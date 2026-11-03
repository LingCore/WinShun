#include "Engine.h"

#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
// After shlobj.h: exdisp.h needs what it brings in (docobj.h) under WIN32_LEAN_AND_MEAN.
#include <exdisp.h>
#include <exdispid.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace tabs {

namespace {

// Explorer's own commands for a tab window (what Ctrl+T, Ctrl+W and
// Ctrl+<n> send; ExplorerTabUtility uses the same).
constexpr WPARAM kNewTab = 0xA21B;
constexpr WPARAM kCloseTab = 0xA021;
constexpr WPARAM kSelectTab = 0xA221; // lParam: 1-based position in the tab strip

constexpr UINT kShown = WM_APP + 1; // lParam: a new Explorer window just shown (hidden or not)
constexpr UINT kReconnect = WM_APP + 2;
constexpr UINT kRegistered = WM_APP + 3; // from the ShellWindows events, handled after they return
constexpr UINT kRevoked = WM_APP + 4;

constexpr UINT_PTR kTimerTabSeen = 1; // the new tab's window: the target to the front
constexpr UINT_PTR kTimerDeadline = 2;
constexpr UINT_PTR kTimerRetry = 3;
constexpr UINT_PTR kTimerCloseB = 4; // B's selection read once its view is there, then B closes
constexpr UINT_PTR kTimerSettle = 5; // the tab shows the folder: selection, timings, done
constexpr UINT_PTR kTimerLeftHidden = 6; // a window hidden at SHOW that never registered
constexpr UINT_PTR kTimerLateTab = 7; // closing a spare tab (closeSpare)

constexpr std::int64_t kOpDeadlineMs = 6000;
constexpr std::int64_t kNoRegistrationMs = 3000;

double nowMs()
{
    static const double freq = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    ::QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / freq;
}

std::int64_t tickMs()
{
    return static_cast<std::int64_t>(::GetTickCount64());
}

std::string utf8(std::wstring_view w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring className(HWND w)
{
    wchar_t buffer[64] {};
    ::GetClassNameW(w, buffer, 64);
    return buffer;
}

// A window's text without a message to it: another process's window
// answers from what the system keeps, so a busy Explorer cannot stall it.
std::wstring textOf(HWND w)
{
    wchar_t buffer[512] {};
    ::InternalGetWindowText(w, buffer, 512);
    return buffer;
}

bool isExplorerTop(HWND w)
{
    return w && ::GetAncestor(w, GA_PARENT) == ::GetDesktopWindow() && className(w) == L"CabinetWClass";
}

// The tabs of an Explorer window in z-order: the active one first.
std::vector<HWND> tabsOf(HWND top)
{
    std::vector<HWND> tabs;
    for (HWND t = ::FindWindowExW(top, nullptr, L"ShellTabWindowClass", nullptr); t;
        t = ::FindWindowExW(top, t, L"ShellTabWindowClass", nullptr))
        tabs.push_back(t);
    return tabs;
}

HWND activeTab(HWND top)
{
    return ::FindWindowExW(top, nullptr, L"ShellTabWindowClass", nullptr);
}

bool cloaked(HWND w)
{
    DWORD value = 0;
    return SUCCEEDED(::DwmGetWindowAttribute(w, DWMWA_CLOAKED, &value, sizeof value)) && value != 0;
}

// Top-level Explorer windows in z-order (the one in front first).
std::vector<HWND> explorerWindows()
{
    std::vector<HWND> windows;
    for (HWND w = ::FindWindowExW(nullptr, nullptr, L"CabinetWClass", nullptr); w; w = ::FindWindowExW(nullptr, w, L"CabinetWClass", nullptr))
        windows.push_back(w);
    return windows;
}

class Handle {
public:
    explicit Handle(HANDLE h = nullptr) : m_h(h) {}
    ~Handle()
    {
        if (m_h && m_h != INVALID_HANDLE_VALUE)
            ::CloseHandle(m_h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return m_h; }
    HANDLE* put() { return &m_h; }
    explicit operator bool() const { return m_h && m_h != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_h;
};

DWORD integrityOf(HANDLE token)
{
    DWORD size = 0;
    ::GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    if (!size || !::GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size))
        return SECURITY_MANDATORY_SYSTEM_RID; // not known: as if above
    const PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data())->Label.Sid;
    return *::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1);
}

DWORD ownIntegrity()
{
    Handle token;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.put()))
        return SECURITY_MANDATORY_MEDIUM_RID;
    return integrityOf(token.get());
}

struct PidlFree {
    void operator()(void* p) const { ::CoTaskMemFree(p); }
};
using Pidl = std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, PidlFree>;
using ChildPidl = std::unique_ptr<std::remove_pointer_t<PITEMID_CHILD>, PidlFree>;

ComPtr<IShellBrowser> shellBrowserOf(IWebBrowser2* web)
{
    ComPtr<IServiceProvider> services;
    ComPtr<IShellBrowser> browser;
    if (web && SUCCEEDED(web->QueryInterface(IID_PPV_ARGS(&services))))
        services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    return browser;
}

ComPtr<IFolderView> folderViewOf(IWebBrowser2* web)
{
    ComPtr<IShellView> view;
    ComPtr<IFolderView> folderView;
    if (const ComPtr<IShellBrowser> browser = shellBrowserOf(web); browser && SUCCEEDED(browser->QueryActiveShellView(&view)) && view)
        view.As(&folderView);
    return folderView;
}

// The folder a tab shows, as an ID list; null while it has no view yet.
Pidl folderOf(IWebBrowser2* web)
{
    ComPtr<IPersistFolder2> folder;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (const ComPtr<IFolderView> view = folderViewOf(web);
        view && SUCCEEDED(view->GetFolder(IID_PPV_ARGS(&folder))) && SUCCEEDED(folder->GetCurFolder(&pidl)))
        return Pidl(pidl);
    return nullptr;
}

std::wstring parsingName(PCIDLIST_ABSOLUTE pidl, SIGDN form = SIGDN_DESKTOPABSOLUTEPARSING)
{
    std::wstring name;
    PWSTR text = nullptr;
    if (pidl && SUCCEEDED(::SHGetNameFromIDList(pidl, form, &text)) && text) {
        name = text;
        ::CoTaskMemFree(text);
    }
    return name;
}

std::wstring locationUrl(IWebBrowser2* web)
{
    BSTR url = nullptr;
    std::wstring s;
    if (web && SUCCEEDED(web->get_LocationURL(&url)) && url)
        s.assign(url, ::SysStringLen(url));
    ::SysFreeString(url);
    return s;
}

// Where a tab is: its view's folder, else its address (a view not made yet).
struct Place {
    std::wstring name;
    Pidl pidl;
};
Place placeOf(IWebBrowser2* web)
{
    Place place;
    place.pidl = folderOf(web);
    if (place.pidl)
        place.name = parsingName(place.pidl.get());
    if (place.name.empty()) {
        place.name = normalized(locationUrl(web));
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (!place.name.empty() && SUCCEEDED(::SHParseDisplayName(place.name.c_str(), nullptr, &pidl, 0, nullptr)))
            place.pidl.reset(pidl);
    }
    return place;
}

// Navigate2 by ID list: no URL rules in the way ('#' in a name, ::{GUID}).
HRESULT navigate(IWebBrowser2* web, PCIDLIST_ABSOLUTE pidl)
{
    const UINT size = ::ILGetSize(pidl);
    SAFEARRAY* bytes = ::SafeArrayCreateVector(VT_UI1, 0, size);
    if (!bytes)
        return E_OUTOFMEMORY;
    void* data = nullptr;
    ::SafeArrayAccessData(bytes, &data);
    std::memcpy(data, pidl, size);
    ::SafeArrayUnaccessData(bytes);
    VARIANT target {};
    target.vt = VT_ARRAY | VT_UI1;
    target.parray = bytes;
    VARIANT empty {};
    const HRESULT hr = web->Navigate2(&target, &empty, &empty, &empty, &empty);
    ::VariantClear(&target);
    return hr;
}

bool disconnected(HRESULT hr)
{
    return hr == RPC_E_DISCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED)
        || hr == CO_E_OBJNOTCONNECTED;
}

bool keysForBypass()
{
    return (::GetAsyncKeyState(VK_CONTROL) & 0x8000) && (::GetAsyncKeyState(VK_SHIFT) & 0x8000);
}

bool leftButtonDown()
{
    return ::GetAsyncKeyState(::GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000;
}

} // namespace

// ---- hidden windows ----------------------------------------------------------------

// The windows the plugin hid, and the journal that outlives the process.
class Hider {
public:
    explicit Hider(std::wstring journal) : m_journal(std::move(journal)) {}

    bool hidden(HWND w) const
    {
        const std::lock_guard lock(m_mutex);
        return m_windows.contains(w);
    }
    std::size_t count() const
    {
        const std::lock_guard lock(m_mutex);
        return m_windows.size();
    }

    // Written down first, then hidden: a process that dies in between
    // leaves a line too many, which the next start checks before acting.
    bool hide(HWND w)
    {
        const std::lock_guard ops(m_ops); // a show of the same window waits for this
        const LONG_PTR style = ::GetWindowLongPtrW(w, GWL_EXSTYLE);
        const bool added = !(style & WS_EX_LAYERED);
        {
            const std::lock_guard lock(m_mutex);
            if (!m_windows.emplace(w, added).second)
                return false;
            writeLocked();
        }
        if (added)
            ::SetWindowLongPtrW(w, GWL_EXSTYLE, style | WS_EX_LAYERED); // a message to Explorer's thread: waits for it
        ::SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA);
        return true;
    }

    void show(HWND w)
    {
        const std::lock_guard ops(m_ops);
        bool added = false;
        {
            const std::lock_guard lock(m_mutex);
            const auto it = m_windows.find(w);
            if (it == m_windows.end())
                return;
            added = it->second;
            m_windows.erase(it);
            writeLocked();
        }
        reveal(w, added);
    }

    // The window closed (or became a tab): nothing to show again.
    void forget(HWND w)
    {
        const std::lock_guard lock(m_mutex);
        if (m_windows.erase(w))
            writeLocked();
    }

    void showAll()
    {
        const std::lock_guard ops(m_ops);
        std::map<HWND, bool> windows;
        {
            const std::lock_guard lock(m_mutex);
            windows.swap(m_windows);
            writeLocked();
        }
        for (const auto& [w, added] : windows)
            reveal(w, added);
    }

    static void reveal(HWND w, bool removeLayered)
    {
        if (!::IsWindow(w))
            return;
        ::SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);
        if (removeLayered)
            ::SetWindowLongPtrW(w, GWL_EXSTYLE, ::GetWindowLongPtrW(w, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }

private:
    void writeLocked() const
    {
        if (m_journal.empty())
            return;
        if (m_windows.empty()) {
            ::DeleteFileW(m_journal.c_str());
            return;
        }
        FILE* f = nullptr;
        if (_wfopen_s(&f, m_journal.c_str(), L"wb") != 0 || !f)
            return;
        for (const auto& [w, added] : m_windows)
            std::fprintf(f, "%llx %d\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(w)), added ? 1 : 0);
        std::fclose(f);
    }

    std::wstring m_journal;
    mutable std::mutex m_mutex;
    // hide and show one after the other: a show while a hide still waits on
    // Explorer's thread would otherwise be undone by that hide.
    std::mutex m_ops;
    std::map<HWND, bool> m_windows; // window -> WS_EX_LAYERED was added by us
};

int Engine::restoreJournal(const std::wstring& journal)
{
    FILE* f = nullptr;
    if (journal.empty() || _wfopen_s(&f, journal.c_str(), L"rb") != 0 || !f)
        return 0;
    int shown = 0;
    unsigned long long value = 0;
    int added = 0;
    while (fscanf_s(f, "%llx %d", &value, &added) == 2) {
        const HWND w = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
        // Only what is still an invisible Explorer window: the handle may
        // belong to something else by now.
        BYTE alpha = 255;
        DWORD flags = 0;
        if (isExplorerTop(w) && (::GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_LAYERED)
            && ::GetLayeredWindowAttributes(w, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) {
            Hider::reveal(w, added != 0);
            ++shown;
        }
    }
    std::fclose(f);
    ::DeleteFileW(journal.c_str());
    return shown;
}

// ---- the engine ----------------------------------------------------------------------

struct Engine::Impl {
    explicit Impl(Options o) : options(std::move(o)), hider(options.journal), ownLevel(ownIntegrity()) {}

    // What the watch thread and the shell thread share (under `mutex`).
    struct Early {
        double shownAt = 0;
        double hiddenAt = 0; // 0: not hidden at SHOW
        std::int64_t tick = 0;
        bool bypass = false;
        bool dragging = false;
        bool movedTab = false;
    };

    Options options;
    Hider hider;
    const DWORD ownLevel;
    mutable std::mutex mutex;
    Settings settings; // under mutex
    std::map<HWND, Early> early; // windows seen at SHOW, under mutex
    std::set<HWND> knownTops; // Explorer windows the cache knows, under mutex
    std::set<HWND> undecided; // registered, not decided yet (may still be hidden at SHOW), under mutex
    std::set<HWND> knownTabs; // their tabs, under mutex
    std::map<DWORD, DWORD> levels; // process id -> integrity, under mutex
    std::atomic<bool> paused {false};
    // The open windows are known (knownTops): until then no window counts as new.
    std::atomic<bool> connected {false};
    std::atomic<int> mergedCount {0};

    std::thread watchThread;
    std::thread shellThread;
    std::atomic<DWORD> watchThreadId {0};
    std::atomic<HWND> shellWindow {nullptr};
    std::mutex startMutex;
    std::condition_variable started;
    bool shellReady = false;

    // ---- shared helpers ----

    void log(const std::string& text) const
    {
        if (options.log)
            options.log(text);
    }

    void trace(const std::string& line) const
    {
        if (options.trace.empty())
            return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, options.trace.c_str(), L"ab") == 0 && f) {
            std::fprintf(f, "%s\n", line.c_str());
            std::fclose(f);
        }
    }

    Settings currentSettings() const
    {
        const std::lock_guard lock(mutex);
        return settings;
    }

    bool aboveUs(HWND w)
    {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(w, &pid);
        {
            const std::lock_guard lock(mutex);
            if (const auto it = levels.find(pid); it != levels.end())
                return it->second > ownLevel;
        }
        DWORD level = SECURITY_MANDATORY_SYSTEM_RID; // cannot look: as if above (UIPI would refuse us anyway)
        if (Handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)); process) {
            Handle token;
            if (::OpenProcessToken(process.get(), TOKEN_QUERY, token.put()))
                level = integrityOf(token.get());
        }
        const std::lock_guard lock(mutex);
        levels[pid] = level;
        return level > ownLevel;
    }

    // An Explorer window that may receive a tab (or count as "another window").
    bool usable(HWND w) const { return ::IsWindowVisible(w) && !cloaked(w) && !hider.hidden(w); }

    // A new window is hidden before its folder is known when it can be
    // (hideEarly): at SHOW (watch thread) or at registration (shell thread,
    // when it registered before it showed). Fills in e; no COM calls.
    void hideIfNew(const Settings& s, HWND w, Early& e)
    {
        Candidate b;
        const std::vector<HWND> tabs = tabsOf(w);
        b.tabs = static_cast<int>(tabs.size());
        b.bypass = options.forceBypass || keysForBypass();
        b.dragging = leftButtonDown();
        {
            const std::lock_guard lock(mutex);
            b.movedTab = std::ranges::any_of(tabs, [&](HWND t) { return knownTabs.contains(t); });
        }
        b.aboveUs = aboveUs(w);
        int others = 0;
        for (HWND other : explorerWindows()) {
            // Restricted: only test windows count as somewhere to go.
            if (other != w && usable(other) && (!options.restriction.active() || textOf(other).find(options.testMark) != std::wstring::npos))
                ++others;
        }
        bool named = false;
        if (!options.testMark.empty()) {
            named = textOf(w).find(options.testMark) != std::wstring::npos
                || (!tabs.empty() && textOf(tabs.front()).find(options.testMark) != std::wstring::npos);
        }
        e.tick = tickMs();
        e.bypass = b.bypass;
        e.dragging = b.dragging;
        e.movedTab = b.movedTab;
        if (hideEarly(s, paused, options.restriction, b, others, named) && hider.hide(w))
            e.hiddenAt = nowMs();
    }

    // ---- the watch thread ----

    static void CALLBACK onEvent(HWINEVENTHOOK, DWORD event, HWND w, LONG object, LONG child, DWORD, DWORD)
    {
        if (object != OBJID_WINDOW || child != CHILDID_SELF || !s_self)
            return;
        s_self->watched(event, w);
    }
    static inline Impl* s_self = nullptr;

    void watched(DWORD event, HWND w)
    {
        if (!connected || !isExplorerTop(w))
            return;
        const double at = nowMs();
        const Settings s = currentSettings();
        {
            const std::lock_guard lock(mutex);
            if (knownTops.contains(w) && !undecided.contains(w)) {
                // A window the shell thread knows: not new. One hidden before
                // it showed gets a taskbar button now; that goes.
                if (event == EVENT_OBJECT_SHOW && hider.hidden(w))
                    if (const HWND shell = shellWindow)
                        ::PostMessageW(shell, kShown, 0, reinterpret_cast<LPARAM>(w));
                return;
            }
            if (event == EVENT_OBJECT_NAMECHANGE) {
                // (restricted only) shown before its title named the test folder
                const auto it = early.find(w);
                if (it == early.end() || it->second.hiddenAt > 0)
                    return;
            } else if (early.contains(w))
                return;
        }
        Early e;
        e.shownAt = at;
        hideIfNew(s, w, e);
        {
            const std::lock_guard lock(mutex);
            if (event == EVENT_OBJECT_NAMECHANGE) {
                if (e.hiddenAt > 0)
                    early[w].hiddenAt = e.hiddenAt;
            } else
                early[w] = e;
        }
        if (e.hiddenAt > 0 || event == EVENT_OBJECT_SHOW) {
            if (const HWND shell = shellWindow)
                ::PostMessageW(shell, kShown, 0, reinterpret_cast<LPARAM>(w));
        }
    }

    void watchLoop()
    {
        watchThreadId = ::GetCurrentThreadId();
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the queue exists before start() returns
        {
            const std::lock_guard lock(startMutex);
        }
        started.notify_all();
        const HWINEVENTHOOK show = ::SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, onEvent, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        // Restricted (tests): a window first shown under the generic title
        // "文件资源管理器" can be hidden once its title names the test folder.
        // Only Explorer's own process: name changes are everywhere.
        HWINEVENTHOOK names = nullptr;
        if (options.restriction.active() && !options.testMark.empty()) {
            DWORD explorer = 0;
            ::GetWindowThreadProcessId(::GetShellWindow(), &explorer);
            names = ::SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, onEvent, explorer, 0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        }
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&msg);
        if (names)
            ::UnhookWinEvent(names);
        if (show)
            ::UnhookWinEvent(show);
    }

    // ---- the shell thread ----

    struct Entry {
        ComPtr<IUnknown> identity;
        ComPtr<IWebBrowser2> web;
        HWND top = nullptr;
        HWND tab = nullptr;
        int openOrder = 0;
    };

    struct Op {
        HWND b = nullptr;
        ComPtr<IWebBrowser2> bWeb;
        HWND target = nullptr;
        std::vector<HWND> tabsBefore;
        Place place;
        HWND spareTab = nullptr;
        ComPtr<IWebBrowser2> spareWeb;
        HWND newTab = nullptr; // the tab window seen after 0xA21B (before it registers)
        // Opened before B's folder was known (B hidden, a tab is likely):
        // the tab loads Home meanwhile. Until decided it is not navigated
        // and the target not brought forward; if B is not merged there, the
        // tab is closed again.
        bool speculative = false;
        bool decided = true;
        bool fronted = false;
        int navigations = 0;
        bool navigated = false; // Navigate2 accepted
        bool shown = false; // the tab shows the folder
        bool bClosed = false;
        int closeWaits = 0;
        int selectionWaits = 0;
        int settleWaits = 0;
        std::vector<ChildPidl> selection;
        std::int64_t deadline = 0;
        // timings (QPC ms) for the trace
        double tShow = 0, tHidden = 0, tRegistered = 0, tDecided = 0, tPosted = 0, tTabSeen = 0, tFront = 0, tSpare = 0,
               tNavigate = 0, tShownInTab = 0, tQuit = 0, tGone = 0;
        bool front = false;
    };

    class Sink : public IDispatch {
    public:
        explicit Sink(Impl* owner) : m_owner(owner) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
        {
            if (riid == IID_IUnknown || riid == IID_IDispatch || riid == DIID_DShellWindowsEvents) {
                *out = static_cast<IDispatch*>(this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        // Owned by Impl and unadvised before it goes: no counting needed.
        ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override
        {
            *n = 0;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override
        {
            // Explorer's thread that fires this waits for it to return (and
            // so does whoever opened the window): the work is done after.
            if (id == DISPID_WINDOWREGISTERED)
                ::PostMessageW(m_owner->shellWindow, kRegistered, 0, 0);
            else if (id == DISPID_WINDOWREVOKED)
                ::PostMessageW(m_owner->shellWindow, kRevoked, 0, 0);
            return S_OK;
        }

    private:
        Impl* m_owner;
    };

    Sink sink {this};
    ComPtr<IShellWindows> shellWindows;
    ComPtr<IConnectionPoint> connection;
    DWORD cookie = 0;
    ComPtr<ITaskbarList> taskbar;
    std::vector<Entry> cache; // in the order they registered
    int nextOpenOrder = 0;
    std::vector<Closed> closedTabs;
    std::map<HWND, std::wstring> lastPlaces; // tab -> where it was when we last looked (for "dragged out")
    std::deque<Op> queue; // queue.front() is the one running
    int wastedTabs = 0; // speculative tabs closed again
    struct Closing {
        HWND top = nullptr;
        std::vector<HWND> before; // its tabs before the spare one (to find it while not known)
        HWND tab = nullptr;
        std::int64_t until = 0;
        HWND thenSelect = nullptr;
        bool front = false;
        bool sent = false; // a close is on its way, unanswered
        bool active = false;
    } closing; // a spare tab being closed (closeSpare)
    UINT taskbarCreated = 0;

    void publishKnown()
    {
        std::set<HWND> tops, tabs;
        for (const Entry& e : cache) {
            tops.insert(e.top);
            tabs.insert(e.tab);
        }
        const std::lock_guard lock(mutex);
        knownTops = std::move(tops);
        knownTabs = std::move(tabs);
    }

    std::optional<Entry> entryAt(long index)
    {
        VARIANT v {};
        v.vt = VT_I4;
        v.lVal = index;
        ComPtr<IDispatch> item;
        Entry e;
        if (shellWindows->Item(v, &item) != S_OK || !item || FAILED(item.As(&e.web)) || FAILED(item.As(&e.identity)))
            return std::nullopt;
        SHANDLE_PTR top = 0;
        if (FAILED(e.web->get_HWND(&top)))
            return std::nullopt;
        e.top = reinterpret_cast<HWND>(top);
        if (const ComPtr<IShellBrowser> browser = shellBrowserOf(e.web.Get()))
            browser->GetWindow(&e.tab);
        return e;
    }

    bool connect()
    {
        disconnect();
        if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shellWindows))))
            return false;
        long count = 0;
        shellWindows->get_Count(&count);
        for (long i = 0; i < count; ++i) {
            if (auto e = entryAt(i)) {
                e->openOrder = nextOpenOrder++;
                cache.push_back(std::move(*e));
            }
        }
        publishKnown();
        connected = true;
        ComPtr<IConnectionPointContainer> container;
        if (SUCCEEDED(shellWindows.As(&container)) && SUCCEEDED(container->FindConnectionPoint(DIID_DShellWindowsEvents, &connection)))
            connection->Advise(&sink, &cookie);
        if (!taskbar && SUCCEEDED(::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))))
            taskbar->HrInit();
        return true;
    }

    void disconnect()
    {
        connected = false;
        while (!queue.empty())
            fail("Explorer went away");
        if (connection && cookie)
            connection->Unadvise(cookie);
        connection.Reset();
        cookie = 0;
        shellWindows.Reset();
        cache.clear();
        publishKnown();
    }

    // The entries added since the cache was last brought up to date: new
    // ones come at the end of the list, so it is read backwards until a
    // known one (a few calls, not one per open tab).
    std::vector<Entry> takeNew()
    {
        std::vector<Entry> fresh;
        long count = 0;
        if (const HRESULT hr = shellWindows ? shellWindows->get_Count(&count) : E_FAIL; FAILED(hr)) {
            if (disconnected(hr))
                ::PostMessageW(shellWindow, kReconnect, 0, 0);
            return fresh;
        }
        for (long i = count - 1; i >= 0; --i) {
            auto e = entryAt(i);
            if (!e)
                continue;
            if (std::ranges::any_of(cache, [&](const Entry& c) { return c.identity.Get() == e->identity.Get(); }))
                break;
            fresh.push_back(std::move(*e));
        }
        std::ranges::reverse(fresh);
        for (Entry& e : fresh)
            e.openOrder = nextOpenOrder++;
        return fresh;
    }

    void registered()
    {
        const double at = nowMs();
        // A new window is hidden first: the calls below wait on its busy
        // thread, and one that registered before it showed shows meanwhile.
        for (HWND top : explorerWindows()) {
            bool known = false;
            {
                const std::lock_guard lock(mutex);
                known = knownTops.contains(top);
            }
            if (!known) {
                hideOnRegistration(top);
                speculate(top);
            }
        }
        std::vector<Entry> fresh = takeNew();
        for (Entry& e : fresh) {
            const bool newWindow = std::ranges::none_of(cache, [&](const Entry& c) { return c.top == e.top; });
            cache.push_back(e);
            publishKnown();
            if (!queue.empty() && !queue.front().spareTab && e.top == queue.front().target
                && std::ranges::find(queue.front().tabsBefore, e.tab) == queue.front().tabsBefore.end()) {
                spareRegistered(e, at);
                continue;
            }
            if (newWindow && isExplorerTop(e.top)) {
                hideOnRegistration(e.top);
                speculate(e.top);
                arrived(e, at);
            }
        }
    }

    void revoked()
    {
        // Which one is not said: whatever is gone leaves the cache; a tab
        // whose window changed (dragged into another window) is moved.
        const std::int64_t now = tickMs();
        std::erase_if(closedTabs, [&](const Closed& c) { return now - c.atMs > kDraggedOutMs; });
        std::erase_if(cache, [&](Entry& e) {
            if (!::IsWindow(e.tab)) {
                if (const auto it = lastPlaces.find(e.tab); it != lastPlaces.end()) {
                    closedTabs.push_back({it->second, now});
                    lastPlaces.erase(it);
                }
                return true;
            }
            e.top = ::GetAncestor(e.tab, GA_ROOT);
            return false;
        });
        publishKnown();
    }

    std::wstring placeName(IWebBrowser2* web, HWND tab)
    {
        std::wstring name = placeOf(web).name;
        if (!name.empty())
            lastPlaces[tab] = name;
        return name;
    }

    // B registered: decide what becomes of it.
    // A window that registered before it was hidden (it registered first, or
    // its SHOW could not hide it yet) is hidden now, before the calls that
    // find its folder: those wait on its busy thread while it may show.
    void hideOnRegistration(HWND b)
    {
        {
            const std::lock_guard lock(mutex);
            undecided.insert(b);
            if (const auto it = early.find(b); it != early.end() && it->second.hiddenAt > 0)
                return;
        }
        if (hider.hidden(b))
            return;
        Early e;
        hideIfNew(currentSettings(), b, e);
        {
            const std::lock_guard lock(mutex);
            if (const auto it = early.find(b); it != early.end())
                it->second.hiddenAt = e.hiddenAt; // keeps what was seen at SHOW
            else if (e.hiddenAt > 0)
                early[b] = e;
        }
        if (e.hiddenAt > 0) {
            if (taskbar)
                taskbar->DeleteTab(b);
            ::SetTimer(shellWindow, kTimerLeftHidden, 250, nullptr); // shown again if it never registers
        }
    }

    void arrived(const Entry& entry, double at)
    {
        const HWND b = entry.top;
        Early e;
        {
            const std::lock_guard lock(mutex);
            if (const auto it = early.find(b); it != early.end())
                e = it->second;
        }
        Candidate candidate;
        Place place = placeOf(entry.web.Get());
        candidate.place = place.name;
        if (!place.name.empty())
            lastPlaces[entry.tab] = place.name;
        candidate.tabs = static_cast<int>(tabsOf(b).size());
        candidate.bypass = e.bypass || options.forceBypass || keysForBypass();
        candidate.dragging = e.dragging || (e.shownAt == 0 && leftButtonDown());
        candidate.movedTab = e.movedTab;
        candidate.aboveUs = aboveUs(b);

        // The other windows. Where their tabs are costs a call or three
        // each, so only tabs whose title is B's folder's name are asked
        // (all of them while restricted: a target must be all test folders).
        const std::wstring bTitle = textOf(entry.tab);
        const std::wstring bDisplay = parsingName(place.pidl.get(), SIGDN_NORMALDISPLAY);
        const Settings s = currentSettings();
        std::vector<Window> others;
        std::map<std::uintptr_t, std::vector<HWND>> tabHandles;
        const Op* speculation = !queue.empty() && queue.front().speculative && !queue.front().decided && queue.front().b == b ? &queue.front() : nullptr;
        const std::vector<HWND> z = explorerWindows();
        for (std::size_t zi = 0; zi < z.size(); ++zi) {
            const HWND top = z[zi];
            if (top == b)
                continue;
            Window w;
            w.id = reinterpret_cast<std::uintptr_t>(top);
            w.zOrder = static_cast<int>(zi);
            w.openOrder = INT_MAX;
            w.usable = usable(top);
            w.aboveUs = aboveUs(top);
            bool any = false;
            for (HWND tab : tabsOf(top)) {
                if (speculation && top == speculation->target && std::ranges::find(speculation->tabsBefore, tab) == speculation->tabsBefore.end())
                    continue; // the tab opened for B itself
                const auto it = std::ranges::find_if(cache, [&](const Entry& c) { return c.tab == tab; });
                std::wstring tabPlace;
                if (it != cache.end()) {
                    any = true;
                    w.openOrder = std::min(w.openOrder, it->openOrder);
                    const std::wstring title = textOf(tab);
                    // (restricted: a tab not named as a test folder is not one;
                    // left unknown, which rules its window out)
                    if (options.restriction.active() ? title.find(options.testMark) != std::wstring::npos
                                                     : s.reuse && (title == bTitle || (!bDisplay.empty() && title == bDisplay)))
                        tabPlace = placeName(it->web.Get(), tab);
                }
                w.tabs.push_back(std::move(tabPlace));
                tabHandles[w.id].push_back(tab);
            }
            if (any)
                others.push_back(std::move(w));
        }
        const Decision d = decide(s, paused, options.restriction, candidate, others, closedTabs, tickMs());
        const double decided = nowMs();
        log("B " + utf8(candidate.place) + ": " + whyName(d.why));
        {
            const std::lock_guard lock(mutex);
            early.erase(b);
            undecided.erase(b);
        }
        if (speculation) {
            Op& op = queue.front();
            if (d.verdict == Verdict::NewTab && reinterpret_cast<HWND>(d.window) == op.target) {
                // As guessed: the tab is there (or coming); it goes to B's folder.
                op.decided = true;
                op.bWeb = entry.web;
                op.place = std::move(place);
                op.tShow = e.shownAt;
                op.tHidden = e.hiddenAt > 0 ? e.hiddenAt : decided;
                op.tRegistered = at;
                op.tDecided = decided;
                hideB(b);
                // the time allowed counts from here, as for any other merge
                op.deadline = tickMs() + kOpDeadlineMs;
                ::SetTimer(shellWindow, kTimerDeadline, static_cast<UINT>(kOpDeadlineMs), nullptr);
                if (op.spareWeb)
                    tryNavigate();
                return;
            }
            char ids[64];
            std::snprintf(ids, sizeof ids, " guessed=%p decided=%p", static_cast<void*>(op.target), reinterpret_cast<void*>(d.window));
            dropSpeculation((std::string(whyName(d.why)) + ids).c_str());
        }
        if (d.verdict == Verdict::Keep) {
            showAgain(b);
            trace("keep why=" + std::string(whyName(d.why)) + " place=" + utf8(candidate.place) + timing("show", e.shownAt)
                + timing("hidden", e.hiddenAt) + timing("reg", at) + timing("decided", decided));
            return;
        }
        hideB(b);
        const HWND target = reinterpret_cast<HWND>(d.window);
        if (d.verdict == Verdict::SwitchTab) {
            const std::vector<HWND>& handles = tabHandles[d.window];
            const HWND wanted = d.tab >= 0 && d.tab < static_cast<int>(handles.size()) ? handles[d.tab] : nullptr;
            bool switched = false, front = false;
            if (closing.active && closing.top == target) {
                // the guessed tab is still being closed there (it must be the
                // active one for that): the switch follows its closing
                closing.thenSelect = wanted;
                closing.front = true;
                switched = front = true;
            } else {
                switched = wanted && selectTab(target, wanted);
                front = toFront(target);
            }
            closeB(entry.web.Get(), b);
            ++mergedCount;
            trace("switch place=" + utf8(candidate.place) + " switched=" + std::to_string(switched) + " front=" + std::to_string(front)
                + timing("show", e.shownAt) + timing("hidden", e.hiddenAt) + timing("reg", at) + timing("decided", decided)
                + timing("done", nowMs()));
            return;
        }
        Op op;
        op.b = b;
        op.bWeb = entry.web;
        op.target = target;
        op.place = std::move(place);
        op.tShow = e.shownAt;
        op.tHidden = e.hiddenAt > 0 ? e.hiddenAt : decided;
        op.tRegistered = at;
        op.tDecided = decided;
        queue.push_back(std::move(op));
        if (queue.size() == 1)
            begin();
    }

    static std::string timing(const char* name, double t)
    {
        if (t <= 0)
            return {};
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, " %s=%.1f", name, t);
        return buffer;
    }

    void hideB(HWND b)
    {
        if (!hider.hidden(b))
            hider.hide(b);
        if (taskbar)
            taskbar->DeleteTab(b); // no taskbar button flashing up for it either
    }

    void showAgain(HWND b)
    {
        if (!hider.hidden(b))
            return;
        hider.show(b);
        if (taskbar && ::IsWindow(b) && ::IsWindowVisible(b)) // an invisible one never had a button
            taskbar->AddTab(b);
    }

    void closeB(IWebBrowser2* web, HWND b)
    {
        const HRESULT hr = web ? web->Quit() : E_POINTER;
        if (FAILED(hr) && ::IsWindow(b))
            ::PostMessageW(b, WM_CLOSE, 0, 0); // a window of one tab closes on it
        hider.forget(b);
    }

    // Explorer selects a tab by its place in the strip (Ctrl+1…9), but
    // its windows are in z-order: each place is tried until the wanted tab
    // is the active one (ExplorerTabUtility does the same).
    static bool selectTab(HWND top, HWND wanted)
    {
        if (activeTab(top) == wanted)
            return true;
        const std::size_t n = tabsOf(top).size();
        for (std::size_t i = 0; i < n; ++i) {
            DWORD_PTR result = 0;
            ::SendMessageTimeoutW(top, WM_COMMAND, kSelectTab, static_cast<LPARAM>(i + 1), SMTO_ABORTIFHUNG, 500, &result);
            if (activeTab(top) == wanted)
                return true;
        }
        return false;
    }

    // The target to the front. Explorer's window B (now invisible) is the
    // one Windows let come to the front; joined to its input for the
    // moment, the plugin may hand the front on to the target.
    static bool toFront(HWND target)
    {
        if (::IsIconic(target))
            ::ShowWindowAsync(target, SW_RESTORE);
        const HWND front = ::GetForegroundWindow();
        if (front == target)
            return true;
        const DWORD frontThread = front ? ::GetWindowThreadProcessId(front, nullptr) : 0;
        const DWORD self = ::GetCurrentThreadId();
        const bool joined = frontThread && frontThread != self && ::AttachThreadInput(self, frontThread, TRUE);
        ::SetForegroundWindow(target);
        ::BringWindowToTop(target);
        if (joined)
            ::AttachThreadInput(self, frontThread, FALSE);
        return ::GetForegroundWindow() == target;
    }

    // B going hands activation to whatever comes next; when that is nothing
    // or the desktop, the window with the new tab takes it.
    static bool activationFellThrough()
    {
        const HWND front = ::GetForegroundWindow();
        if (!front)
            return true;
        wchar_t name[32] {};
        ::GetClassNameW(front, name, 32);
        return std::wstring_view(name) == L"Progman" || std::wstring_view(name) == L"WorkerW" || std::wstring_view(name) == L"Shell_TrayWnd";
    }

    // Where a merged window would go now (decide's order, without B's
    // folder): only windows known with all their tabs, usable, not above us.
    HWND guessTarget(HWND b)
    {
        const Settings s = currentSettings();
        HWND best = nullptr;
        int bestOrder = INT_MAX;
        const std::vector<HWND> z = explorerWindows();
        for (std::size_t zi = 0; zi < z.size(); ++zi) {
            const HWND top = z[zi];
            if (top == b || !usable(top) || aboveUs(top))
                continue;
            int openOrder = INT_MAX;
            for (const Entry& c : cache) {
                if (c.top == top)
                    openOrder = std::min(openOrder, c.openOrder);
            }
            if (openOrder == INT_MAX)
                continue; // not a window the cache knows
            if (options.restriction.active()) {
                const std::vector<HWND> tabs = tabsOf(top);
                if (tabs.empty() || !std::ranges::all_of(tabs, [&](HWND t) { return textOf(t).find(options.testMark) != std::wstring::npos; }))
                    continue;
            }
            const int order = s.target == Target::First ? openOrder : static_cast<int>(zi);
            if (order < bestOrder) {
                best = top;
                bestOrder = order;
            }
        }
        return best;
    }

    // B was just hidden (so it is likely to be merged): the tab is opened
    // now, while B's folder is still being found out, so that its loading
    // of Home overlaps that.
    void speculate(HWND b)
    {
        if (!queue.empty() || closing.active || !::IsWindow(b) || !hider.hidden(b))
            return;
        const HWND target = guessTarget(b);
        if (!target)
            return;
        Op op;
        op.b = b;
        op.target = target;
        op.speculative = true;
        op.decided = false;
        queue.push_back(std::move(op));
        begin();
    }

    // Closing the tab opened for B. 0xA021 (like the tab's own Quit) closes
    // the window's ACTIVE tab, and a tab still being made can ignore it. So:
    // only while it is the active one, one close at a time (sent with a
    // callback, never a second one still queued behind it), again until it
    // is gone; then, if asked, the tab B is switched to is selected and the
    // window brought forward. 3 s at most; the shell thread never waits.
    void closeSpare(HWND top, const std::vector<HWND>& before, HWND tab, HWND thenSelect, bool front)
    {
        closing = {top, before, tab, tickMs() + 3000, thenSelect, front, false, true};
        ::SetTimer(shellWindow, kTimerLateTab, 50, nullptr);
        closingTick();
    }

    static void CALLBACK closeAnswered(HWND, UINT, ULONG_PTR self, LRESULT)
    {
        reinterpret_cast<Impl*>(self)->closing.sent = false;
    }

    void closingTick()
    {
        Closing& c = closing;
        if (!c.active || c.sent)
            return;
        if (!c.tab) {
            for (HWND t : tabsOf(c.top)) {
                if (std::ranges::find(c.before, t) == c.before.end()) {
                    c.tab = t;
                    break;
                }
            }
        }
        const bool gone = c.tab && !::IsWindow(c.tab);
        if (!gone && tickMs() < c.until && ::IsWindow(c.top)) {
            if (c.tab && activeTab(c.top) == c.tab
                && ::SendMessageCallbackW(c.tab, WM_COMMAND, kCloseTab, 1, closeAnswered, reinterpret_cast<ULONG_PTR>(this)))
                c.sent = true;
            return; // (not active: someone chose another tab; it is left alone until it is again)
        }
        ::KillTimer(shellWindow, kTimerLateTab);
        c.active = false;
        trace(std::string("spec tab ") + (gone ? "closed" : "LEFT"));
        if (c.thenSelect && ::IsWindow(c.thenSelect))
            selectTab(c.top, c.thenSelect);
        if (c.front && ::IsWindow(c.top))
            toFront(c.top);
    }

    // The guess was wrong (B stays a window, goes elsewhere or to a tab
    // already there): the tab opened for it is closed, not brought forward.
    void dropSpeculation(const char* why)
    {
        for (UINT_PTR t : {kTimerTabSeen, kTimerDeadline, kTimerRetry, kTimerCloseB, kTimerSettle})
            ::KillTimer(shellWindow, t);
        Op& op = queue.front();
        ++wastedTabs;
        trace(std::string("spec dropped why=") + why);
        closeSpare(op.target, op.tabsBefore, op.spareTab ? op.spareTab : op.newTab, nullptr, false);
        queue.pop_front();
        if (!queue.empty())
            begin();
    }

    void begin()
    {
        if (queue.empty())
            return;
        Op& op = queue.front();
        if (!::IsWindow(op.target) || !::IsWindow(op.b)) {
            fail("a window went away");
            return;
        }
        op.tabsBefore = tabsOf(op.target);
        op.deadline = tickMs() + kOpDeadlineMs;
        op.tPosted = nowMs();
        ::PostMessageW(activeTab(op.target), WM_COMMAND, kNewTab, 0);
        ::SetTimer(shellWindow, kTimerTabSeen, 10, nullptr);
        ::SetTimer(shellWindow, kTimerDeadline, static_cast<UINT>(kOpDeadlineMs), nullptr);
    }

    void tabSeenTick()
    {
        if (queue.empty()) {
            ::KillTimer(shellWindow, kTimerTabSeen);
            return;
        }
        Op& op = queue.front();
        for (HWND t : tabsOf(op.target)) {
            if (std::ranges::find(op.tabsBefore, t) == op.tabsBefore.end()) {
                ::KillTimer(shellWindow, kTimerTabSeen);
                op.tTabSeen = nowMs();
                op.newTab = t;
                return;
            }
        }
    }

    void spareRegistered(const Entry& e, double at)
    {
        Op& op = queue.front();
        op.spareTab = e.tab;
        op.spareWeb = e.web;
        op.tSpare = at;
        if (op.decided)
            tryNavigate();
    }

    void tryNavigate()
    {
        Op& op = queue.front();
        ++op.navigations;
        op.tNavigate = nowMs();
        const HRESULT hr = options.failNavigation ? E_FAIL : op.place.pidl ? navigate(op.spareWeb.Get(), op.place.pidl.get()) : E_INVALIDARG;
        if (SUCCEEDED(hr)) {
            op.navigated = true;
            // Forward only now: it waits on Explorer's thread (busy making
            // the tab), which would hold up the navigation behind it.
            if (!op.fronted) {
                op.fronted = true;
                op.front = toFront(op.target);
                op.tFront = nowMs();
            }
            ::SetTimer(shellWindow, kTimerCloseB, 1, nullptr);
            ::SetTimer(shellWindow, kTimerSettle, 30, nullptr);
            return;
        }
        // A tab just made sometimes refuses the first navigation (E_FAIL): once more.
        if (op.navigations < 2) {
            ::SetTimer(shellWindow, kTimerRetry, 150, nullptr);
            return;
        }
        fail("navigation failed");
    }

    // B goes once what it has selected is read (a folder opened to show a
    // file selects it after the view is there; up to a second is waited).
    void closeBTick()
    {
        Op& op = queue.front();
        const ComPtr<IFolderView> view = folderViewOf(op.bWeb.Get());
        READYSTATE state = READYSTATE_UNINITIALIZED;
        op.bWeb->get_ReadyState(&state);
        if ((!view || state != READYSTATE_COMPLETE) && ++op.closeWaits < 20) {
            ::SetTimer(shellWindow, kTimerCloseB, 50, nullptr);
            return;
        }
        int count = 0;
        if (view)
            view->ItemCount(SVGIO_SELECTION, &count);
        // "Show in folder" selects the file a little after the view is
        // ready: B (hidden) waits up to 0.6 s more for one.
        if (view && count == 0 && ++op.selectionWaits < 12) {
            ::SetTimer(shellWindow, kTimerCloseB, 50, nullptr);
            return;
        }
        ::KillTimer(shellWindow, kTimerCloseB);
        ComPtr<IEnumIDList> items;
        if (view && count > 0 && count <= 1000
            && SUCCEEDED(view->Items(SVGIO_SELECTION, IID_PPV_ARGS(&items))) && items) {
            PITEMID_CHILD child = nullptr;
            while (items->Next(1, &child, nullptr) == S_OK)
                op.selection.emplace_back(child);
        }
        op.tQuit = nowMs();
        closeB(op.bWeb.Get(), op.b);
        op.bClosed = true;
        ++mergedCount;
    }

    // Until the tab shows the folder (for the timings and the selection).
    void settleTick()
    {
        Op& op = queue.front();
        if (op.tGone == 0 && op.bClosed && !::IsWindow(op.b)) {
            op.tGone = nowMs();
            if (activationFellThrough())
                op.front = toFront(op.target);
        }
        if (!op.shown) {
            const Pidl now = folderOf(op.spareWeb.Get());
            if (now && op.place.pidl
                && (::ILIsEqual(now.get(), op.place.pidl.get()) || samePlace(parsingName(now.get()), op.place.name))) {
                op.shown = true;
                op.tShownInTab = nowMs();
            } else if (op.tGone > 0 && nowMs() - op.tGone > 1500) {
                // B is gone and the tab has not said it shows the folder: the
                // next window should not wait for it.
                trace("settle-timeout place=" + utf8(op.place.name) + " tabShows=" + utf8(now ? parsingName(now.get()) : std::wstring(L"-")));
                finish();
                return;
            }
        }
        if (op.shown && op.bClosed) {
            if (!op.selection.empty()) {
                ComPtr<IShellView> view;
                const ComPtr<IShellBrowser> browser = shellBrowserOf(op.spareWeb.Get());
                if (browser && SUCCEEDED(browser->QueryActiveShellView(&view)) && view) {
                    bool first = true;
                    for (const ChildPidl& item : op.selection) {
                        const UINT flags = SVSI_SELECT | (first ? SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED : 0);
                        view->SelectItem(item.get(), flags);
                        first = false;
                    }
                }
            }
            if (op.tGone > 0 || ++op.settleWaits > 60)
                finish();
            return;
        }
        if (++op.settleWaits > 200) // 6 s: the folder never showed; B is gone or going, the tab stays
            finish();
    }

    void finish()
    {
        ::KillTimer(shellWindow, kTimerSettle);
        ::KillTimer(shellWindow, kTimerDeadline);
        ::KillTimer(shellWindow, kTimerTabSeen);
        Op& op = queue.front();
        trace("tab place=" + utf8(op.place.name) + " spec=" + std::to_string(op.speculative) + " front=" + std::to_string(op.front) + " selected=" + std::to_string(op.selection.size())
            + " navigations=" + std::to_string(op.navigations) + timing("show", op.tShow) + timing("hidden", op.tHidden)
            + timing("reg", op.tRegistered) + timing("decided", op.tDecided) + timing("posted", op.tPosted) + timing("tab", op.tTabSeen)
            + timing("front", op.tFront) + timing("spare", op.tSpare) + timing("navigate", op.tNavigate)
            + timing("shownInTab", op.tShownInTab) + timing("quit", op.tQuit) + timing("gone", op.tGone));
        queue.pop_front();
        begin();
    }

    void fail(const char* why)
    {
        if (queue.empty())
            return;
        for (UINT_PTR t : {kTimerTabSeen, kTimerDeadline, kTimerRetry, kTimerCloseB, kTimerSettle})
            ::KillTimer(shellWindow, t);
        Op& op = queue.front();
        log(std::string("B not merged (") + why + "): " + utf8(op.place.name));
        trace(std::string("fail why=") + why + " place=" + utf8(op.place.name));
        if (op.bClosed) {
            // B went already: the tab stays, it is where the folder is.
        } else {
            showAgain(op.b);
            if (::IsWindow(op.target)) // the tab that was opened for it
                closeSpare(op.target, op.tabsBefore, op.spareTab ? op.spareTab : op.newTab, nullptr, false);
            if (::IsWindow(op.b))
                toFront(op.b);
        }
        queue.pop_front();
        begin();
    }

    // Shown at SHOW but never registered (or registered as nothing new):
    // shown again after a while.
    void leftHiddenTick()
    {
        std::vector<HWND> stale;
        {
            const std::lock_guard lock(mutex);
            const std::int64_t now = tickMs();
            for (auto it = early.begin(); it != early.end();) {
                if (now - it->second.tick > kNoRegistrationMs) {
                    if (it->second.hiddenAt > 0)
                        stale.push_back(it->first);
                    undecided.erase(it->first);
                    it = early.erase(it);
                } else
                    ++it;
            }
            if (early.empty())
                ::KillTimer(shellWindow, kTimerLeftHidden);
        }
        for (HWND w : stale) {
            if (std::ranges::none_of(queue, [&](const Op& op) { return op.b == w; })) {
                log("a window hidden at SHOW never registered: shown again");
                showAgain(w);
            }
        }
    }

    static LRESULT CALLBACK windowProc(HWND w, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(w, GWLP_USERDATA));
        if (self)
            return self->handle(w, message, wParam, lParam);
        return ::DefWindowProcW(w, message, wParam, lParam);
    }

    LRESULT handle(HWND w, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == taskbarCreated && taskbarCreated) {
            log("Explorer restarted: watching the new one");
            hider.showAll();
            taskbar.Reset();
            connect();
            return 0;
        }
        switch (message) {
        case kShown:
            if (const HWND shown = reinterpret_cast<HWND>(lParam); hider.hidden(shown)) {
                if (taskbar)
                    taskbar->DeleteTab(shown);
                bool known = false;
                {
                    const std::lock_guard lock(mutex);
                    known = knownTops.contains(shown) && !undecided.contains(shown);
                }
                if (!known)
                    speculate(shown);
            }
            ::SetTimer(w, kTimerLeftHidden, 250, nullptr);
            return 0;
        case kReconnect:
            hider.showAll();
            connect();
            return 0;
        case kRegistered:
            registered();
            return 0;
        case kRevoked:
            revoked();
            return 0;
        case WM_TIMER:
            if (wParam == kTimerLeftHidden) {
                leftHiddenTick();
                return 0;
            }
            if (wParam == kTimerLateTab) {
                closingTick();
                return 0;
            }
            if (queue.empty()) {
                ::KillTimer(w, wParam);
                return 0;
            }
            switch (wParam) {
            case kTimerTabSeen:
                tabSeenTick();
                break;
            case kTimerDeadline:
                ::KillTimer(w, kTimerDeadline);
                if (queue.front().bClosed)
                    finish();
                else
                    fail("timed out");
                break;
            case kTimerRetry:
                ::KillTimer(w, kTimerRetry);
                tryNavigate();
                break;
            case kTimerCloseB:
                ::KillTimer(w, kTimerCloseB);
                closeBTick();
                break;
            case kTimerSettle:
                settleTick();
                break;
            }
            return 0;
        case WM_CLOSE:
            ::DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        }
        return ::DefWindowProcW(w, message, wParam, lParam);
    }

    void shellLoop()
    {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        WNDCLASSEXW wc {sizeof wc};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WinShun.ExplorerTabs";
        ::RegisterClassExW(&wc);
        // A top-level window (never shown): a message-only one does not get
        // the TaskbarCreated broadcast that says Explorer restarted.
        const HWND w = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        ::SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        taskbarCreated = ::RegisterWindowMessageW(L"TaskbarCreated");
        ::ChangeWindowMessageFilterEx(w, taskbarCreated, MSGFLT_ALLOW, nullptr);
        shellWindow = w;
        {
            // start() returns now: reading every open window below can take
            // seconds when Explorer is busy, and the plugin must answer meanwhile.
            const std::lock_guard lock(startMutex);
            shellReady = true;
        }
        started.notify_all();
        if (!connect())
            log("IShellWindows is not there: waiting for Explorer");
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&msg);
        disconnect();
        taskbar.Reset();
        shellWindow = nullptr;
        if (SUCCEEDED(com))
            ::CoUninitialize();
    }
};

Engine::Engine(Options options) : m(std::make_unique<Impl>(std::move(options)))
{
    m->settings = m->options.settings;
}

Engine::~Engine()
{
    stop();
}

void Engine::start()
{
    if (m->shellThread.joinable())
        return;
    if (const int shown = restoreJournal(m->options.journal))
        m->log("shown again " + std::to_string(shown) + " window(s) left hidden last time");
    Impl::s_self = m.get();
    m->shellThread = std::thread([this] { m->shellLoop(); });
    {
        std::unique_lock lock(m->startMutex);
        m->started.wait(lock, [this] { return m->shellReady; });
    }
    m->watchThread = std::thread([this] { m->watchLoop(); });
}

void Engine::stop()
{
    if (m->watchThread.joinable()) {
        while (!m->watchThreadId)
            std::this_thread::yield();
        ::PostThreadMessageW(m->watchThreadId, WM_QUIT, 0, 0);
        m->watchThread.join();
    }
    if (m->shellThread.joinable()) {
        if (const HWND w = m->shellWindow)
            ::PostMessageW(w, WM_CLOSE, 0, 0);
        m->shellThread.join();
    }
    Impl::s_self = nullptr;
    m->hider.showAll();
}

void Engine::configure(const Settings& settings)
{
    const std::lock_guard lock(m->mutex);
    m->settings = settings;
}

void Engine::setPaused(bool paused)
{
    m->paused = paused;
}

bool Engine::paused() const
{
    return m->paused;
}

int Engine::merged() const
{
    return m->mergedCount;
}

} // namespace tabs

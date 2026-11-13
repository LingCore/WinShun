#include "Keeper.h"

#include "Explorer.h"

#include <shellapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <utility>

namespace tabs {

namespace {

constexpr DWORD kLookEveryMs = 3000;
constexpr DWORD kShellWaitMs = 60000; // at a logon: until Explorer answers
constexpr DWORD kWindowWaitMs = 8000; // a window asked for, until it is there
constexpr DWORD kTabWaitMs = 5000;
constexpr DWORD kNavigateWaitMs = 2000;
constexpr DWORD kHoldAfterMs = 1500; // the engine hears of the last window a moment after it is there
// Started this long after its shell at most, a start counts as part of the
// logon (Win顺 started by hand hours later brings nothing back by itself).
constexpr ULONGLONG kLogonMs = 10 * 60 * 1000;
// With Explorer's own "Restore previous folder windows at logon" on, its
// windows come first: what they lack is added after.
constexpr ULONGLONG kOwnRestoreMs = 20 * 1000;
// There for the rest of the logon once this plugin has started in it: the
// global atom table belongs to the window station, and an atom outlives the
// process that added it.
constexpr wchar_t kLogonAtom[] = L"WinShun.ExplorerTabs.Logon";

std::string utf8(std::wstring_view w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string readFile(const std::wstring& path)
{
    std::string text;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return text;
    char buffer[4096];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;)
        text.append(buffer, n);
    std::fclose(f);
    return text;
}

// Whole or not at all: written beside, then moved over the old one.
bool writeFile(const std::wstring& path, const std::string& text)
{
    const std::wstring beside = path + L".new";
    FILE* f = nullptr;
    if (_wfopen_s(&f, beside.c_str(), L"wb") != 0 || !f)
        return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (written && closed && ::MoveFileExW(beside.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    ::DeleteFileW(beside.c_str());
    return false;
}

// How long the shell (the taskbar's Explorer) has been running; 0: not there.
ULONGLONG shellAgeMs()
{
    DWORD pid = 0;
    const HWND shell = ::GetShellWindow();
    if (!shell || !::GetWindowThreadProcessId(shell, &pid))
        return 0;
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return 0;
    FILETIME created {}, exited {}, kernel {}, user {}, now {};
    const bool ok = ::GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    ::CloseHandle(process);
    if (!ok)
        return 0;
    ::GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& t) { return (static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return ticks(now) > ticks(created) ? std::max<ULONGLONG>((ticks(now) - ticks(created)) / 10000, 1) : 1;
}

// Folder options → View → "Restore previous folder windows at logon".
bool explorerRestoresItsOwn()
{
    DWORD value = 0;
    DWORD size = sizeof value;
    return ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"PersistBrowsers",
               RRF_RT_REG_DWORD, nullptr, &value, &size)
            == ERROR_SUCCESS
        && value == 1;
}

// A window the engine is hiding for a moment (a new one becoming a tab).
bool hiddenForMerge(HWND top)
{
    BYTE alpha = 255;
    DWORD flags = 0;
    return (::GetWindowLongPtrW(top, GWL_EXSTYLE) & WS_EX_LAYERED) && ::GetLayeredWindowAttributes(top, nullptr, &alpha, &flags)
        && (flags & LWA_ALPHA) && alpha == 0;
}

Pidl parse(const std::wstring& place)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    return SUCCEEDED(::SHParseDisplayName(place.c_str(), nullptr, &pidl, 0, nullptr)) ? Pidl(pidl) : nullptr;
}

} // namespace

struct Keeper::Impl {
    explicit Impl(KeeperOptions o) : options(std::move(o)) {}

    KeeperOptions options;
    std::thread thread;
    HANDLE wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> stopping {false};
    std::atomic<bool> restoreAsked {false};
    std::atomic<bool> busy {false};
    mutable std::mutex mutex;
    Session session; // under mutex

    // The thread's own.
    ComPtr<IShellWindows> shellWindows;
    ComPtr<IUIAutomation> automation; // made for the first window with several tabs
    std::string lastLook; // what the windows looked like at the last look
    std::string keptLook; // and at the look the file was last brought up to date from
    std::string written; // the file's text

    ~Impl() { ::CloseHandle(wake); }

    void log(const std::string& text) const
    {
        if (options.log)
            options.log(text);
    }

    // Waits, answering what Windows sends this thread meanwhile (an STA);
    // false when the plugin is stopping.
    bool wait(DWORD ms)
    {
        const ULONGLONG until = ::GetTickCount64() + ms;
        while (!stopping) {
            const ULONGLONG now = ::GetTickCount64();
            if (now >= until)
                return true;
            if (::MsgWaitForMultipleObjects(1, &wake, FALSE, static_cast<DWORD>(until - now), QS_ALLINPUT) == WAIT_OBJECT_0) {
                if (restoreAsked && !busy)
                    return true; // asked for meanwhile: the loop takes it
                continue;
            }
            MSG msg;
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                ::DispatchMessageW(&msg);
        }
        return false;
    }

    // Until `done` or `ms` are over; false when not done.
    template <typename Done>
    bool waitFor(Done done, DWORD ms)
    {
        const ULONGLONG until = ::GetTickCount64() + ms;
        while (!done()) {
            if (::GetTickCount64() >= until || !wait(30))
                return false;
        }
        return true;
    }

    bool connect()
    {
        long count = 0;
        if (shellWindows && SUCCEEDED(shellWindows->get_Count(&count)))
            return true;
        shellWindows.Reset();
        return ::GetShellWindow()
            && SUCCEEDED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shellWindows)));
    }

    std::vector<TabEntry> entries()
    {
        std::vector<TabEntry> all;
        long count = 0;
        if (!connect() || FAILED(shellWindows->get_Count(&count)))
            return all;
        for (long i = 0; i < count; ++i) {
            if (auto e = tabEntryAt(shellWindows.Get(), i); e && e->tab && isExplorerTop(e->top))
                all.push_back(std::move(*e));
        }
        return all;
    }

    // The windows as they look, without a call into Explorer.
    std::string look() const
    {
        std::string all;
        char number[96];
        for (HWND top : explorerWindows()) {
            if (hiddenForMerge(top))
                return {}; // in between: looked at again next time
            WINDOWPLACEMENT placement {sizeof placement};
            ::GetWindowPlacement(top, &placement);
            const RECT& r = placement.rcNormalPosition;
            std::snprintf(number, sizeof number, "%p %ld %ld %ld %ld %d|", static_cast<void*>(top), r.left, r.top, r.right, r.bottom,
                ::IsZoomed(top) != FALSE);
            all += number;
            for (HWND tab : tabsOf(top)) {
                std::snprintf(number, sizeof number, "%p=", static_cast<void*>(tab));
                all += number + utf8(textOf(tab)) + '|';
            }
            all += '\n';
        }
        return all.empty() ? "none" : all;
    }

    // The titles of a window's tabs as the tab strip shows them, left to
    // right. Explorer tells this through UI Automation only: IShellWindows
    // lists the tabs in the order of its own table (a place freed by a tab
    // that closed is given to the next one), and the tab windows are in
    // z-order. Empty when it does not tell.
    std::vector<std::wstring> stripTitles(HWND top)
    {
        std::vector<std::wstring> titles;
        if (!automation
            && FAILED(::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))
            && FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
            return titles;
        if (ComPtr<IUIAutomation2> timed; SUCCEEDED(automation.As(&timed))) {
            timed->put_ConnectionTimeout(1000); // a busy Explorer is asked again at the next change
            timed->put_TransactionTimeout(1000);
        }
        VARIANT type {};
        type.vt = VT_I4;
        type.lVal = UIA_TabItemControlTypeId;
        ComPtr<IUIAutomationElement> window;
        ComPtr<IUIAutomationCondition> isTab;
        ComPtr<IUIAutomationElementArray> items;
        int count = 0;
        if (FAILED(automation->ElementFromHandle(top, &window)) || !window
            || FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &isTab))
            || FAILED(window->FindAll(TreeScope_Descendants, isTab.Get(), &items)) || !items || FAILED(items->get_Length(&count)))
            return titles;
        for (int i = 0; i < count; ++i) {
            ComPtr<IUIAutomationElement> item;
            BSTR name = nullptr;
            if (SUCCEEDED(items->GetElement(i, &item)) && item && SUCCEEDED(item->get_CurrentName(&name)) && name)
                titles.emplace_back(name, ::SysStringLen(name));
            ::SysFreeString(name);
        }
        return titles;
    }

    // Asks Explorer where each tab is. The windows from the back to the
    // front (opened again in that order, the one in front ends in front),
    // each one's tabs as its tab strip has them.
    struct Snapshot {
        std::vector<SavedWindow> saved;
        std::vector<OpenWindow> open;
    };
    Snapshot snapshot()
    {
        Snapshot result;
        const std::vector<TabEntry> all = entries();
        std::vector<HWND> tops = explorerWindows();
        std::ranges::reverse(tops);
        for (HWND top : tops) {
            struct Tab {
                HWND window;
                std::wstring place;
            };
            std::vector<Tab> tabs;
            bool outside = false;
            for (const TabEntry& e : all) {
                if (e.top != top)
                    continue;
                tabs.push_back({e.tab, placeOf(e.web.Get()).name});
                outside = outside || (options.restriction.active() && !isUnder(tabs.back().place, options.restriction.root));
            }
            if (outside || tabs.empty())
                continue; // (dev) not a test window: as if not there
            if (tabs.size() > 1) {
                // Each title of the strip takes the first tab of that title
                // not taken yet; a strip that does not fit the tabs (one
                // just opening or closing) leaves them as they are.
                std::vector<Tab> ordered;
                std::vector<Tab> left = tabs;
                for (const std::wstring& title : stripTitles(top)) {
                    const auto it = std::ranges::find_if(left, [&](const Tab& t) { return textOf(t.window) == title; });
                    if (it == left.end())
                        break;
                    ordered.push_back(std::move(*it));
                    left.erase(it);
                }
                if (left.empty() && ordered.size() == tabs.size())
                    tabs = std::move(ordered);
            }
            SavedWindow saved;
            OpenWindow open;
            open.id = reinterpret_cast<std::uintptr_t>(top);
            const HWND front = activeTab(top);
            for (Tab& tab : tabs) {
                open.tabs.push_back(tab.place);
                if (tab.place.empty())
                    continue; // no folder yet (or one above our rights)
                if (tab.window == front)
                    saved.active = static_cast<int>(saved.tabs.size());
                saved.tabs.push_back(std::move(tab.place));
            }
            WINDOWPLACEMENT placement {sizeof placement};
            if (::GetWindowPlacement(top, &placement)) {
                saved.left = placement.rcNormalPosition.left;
                saved.top = placement.rcNormalPosition.top;
                saved.right = placement.rcNormalPosition.right;
                saved.bottom = placement.rcNormalPosition.bottom;
                saved.maximized = ::IsZoomed(top) || (::IsIconic(top) && (placement.flags & WPF_RESTORETOMAXIMIZED));
            }
            result.open.push_back(std::move(open));
            if (!saved.tabs.empty())
                result.saved.push_back(std::move(saved));
        }
        return result;
    }

    void save(const Session& next)
    {
        const std::string text = serialize(next);
        if (text == written)
            return;
        if (writeFile(options.file, text))
            written = text;
        else
            log("could not write the open tabs to " + utf8(options.file));
    }

    // One look; the file follows a change that is the same two looks running.
    void keep()
    {
        // Windows closing because Windows shuts down, or because the shell
        // went away, are not windows the user closed.
        if (::GetSystemMetrics(SM_SHUTTINGDOWN) || !::GetShellWindow())
            return;
        const std::string now = look();
        const bool settled = !now.empty() && now == lastLook;
        lastLook = now;
        if (!settled || now == keptLook || !connect())
            return;
        Snapshot shot = snapshot();
        if (::GetSystemMetrics(SM_SHUTTINGDOWN) || !::GetShellWindow() || look() != now)
            return; // changed while asked: at the next settled look
        keptLook = now;
        Session next;
        {
            const std::lock_guard lock(mutex);
            session = noted(std::move(session), std::move(shot.saved));
            next = session;
        }
        save(next);
    }

    // A new Explorer window on `pidl`; null when none came.
    HWND openWindow(PCIDLIST_ABSOLUTE pidl)
    {
        const std::vector<HWND> before = explorerWindows();
        SHELLEXECUTEINFOW info {sizeof info};
        info.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        info.lpIDList = const_cast<PIDLIST_ABSOLUTE>(pidl);
        info.nShow = SW_SHOWNORMAL;
        // "Open in new window": File Explorer's own, also where another file
        // manager has taken over the folders' default action.
        info.lpVerb = L"opennewwindow";
        if (!::ShellExecuteExW(&info)) {
            info.lpVerb = nullptr;
            if (!::ShellExecuteExW(&info))
                return nullptr;
        }
        HWND top = nullptr;
        waitFor([&] {
            for (const TabEntry& e : entries()) {
                if (std::ranges::find(before, e.top) == before.end()) {
                    top = e.top;
                    return true;
                }
            }
            return false;
        }, kWindowWaitMs);
        return top;
    }

    // A new tab of `top` on `pidl`; null when none came.
    HWND addTab(HWND top, PCIDLIST_ABSOLUTE pidl, const std::wstring& place)
    {
        const std::vector<HWND> before = tabsOf(top);
        const HWND front = activeTab(top);
        if (!front || !::PostMessageW(front, WM_COMMAND, kNewTab, 0))
            return nullptr;
        HWND tab = nullptr;
        ComPtr<IWebBrowser2> web;
        const bool there = waitFor([&] {
            if (!tab) {
                for (HWND t : tabsOf(top)) {
                    if (std::ranges::find(before, t) == before.end())
                        tab = t;
                }
            }
            if (!tab || !connect())
                return false;
            long count = 0;
            shellWindows->get_Count(&count);
            for (long i = count - 1; i >= 0 && !web; --i) { // new ones come at the end
                if (auto e = tabEntryAt(shellWindows.Get(), i); e && e->tab == tab)
                    web = e->web;
            }
            return web != nullptr;
        }, kTabWaitMs);
        if (!there)
            return nullptr;
        // A tab just made sometimes refuses the first navigation (E_FAIL): once more.
        if (FAILED(navigate(web.Get(), pidl)) && (!wait(150) || FAILED(navigate(web.Get(), pidl))))
            return tab;
        waitFor([&] { return samePlace(placeOf(web.Get()).name, place); }, kNavigateWaitMs);
        return tab;
    }

    void restoreNow(bool atLogon)
    {
        struct Held {
            Impl& self;
            explicit Held(Impl& s) : self(s)
            {
                self.busy = true;
                if (self.options.hold)
                    self.options.hold(true);
            }
            ~Held()
            {
                if (self.options.hold)
                    self.options.hold(false);
                self.busy = false;
            }
        } held(*this);

        if (!waitFor([&] { return connect(); }, kShellWaitMs))
            return;
        if (atLogon && explorerRestoresItsOwn())
            waitFor([] { return shellAgeMs() >= kOwnRestoreMs; }, static_cast<DWORD>(kOwnRestoreMs));
        std::vector<SavedWindow> wanted;
        {
            const std::lock_guard lock(mutex);
            wanted = session.previous;
        }
        const std::vector<Reopen> plan = planRestore(wanted, snapshot().open);
        int windows = 0;
        int tabs = 0;
        for (const Reopen& reopen : plan) {
            if (stopping)
                break;
            HWND top = reinterpret_cast<HWND>(reopen.into);
            HWND front = nullptr;
            for (std::size_t i = 0; i < reopen.window.tabs.size() && !stopping; ++i) {
                const std::wstring& place = reopen.window.tabs[i];
                const Pidl pidl = parse(place);
                if (!pidl) {
                    log("not there any more: " + utf8(place));
                    continue;
                }
                HWND tab = nullptr;
                if (!top) {
                    if (!(top = openWindow(pidl.get()))) {
                        log("no window came for " + utf8(place));
                        continue;
                    }
                    ++windows;
                    tab = activeTab(top);
                    if (reopen.window.placed()) {
                        WINDOWPLACEMENT placement {sizeof placement};
                        placement.showCmd = reopen.window.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
                        placement.rcNormalPosition = {reopen.window.left, reopen.window.top, reopen.window.right, reopen.window.bottom};
                        ::SetWindowPlacement(top, &placement);
                    }
                } else if (!::IsWindow(top) || !(tab = addTab(top, pidl.get(), place))) {
                    log("no tab came for " + utf8(place));
                    continue;
                }
                ++tabs;
                if (static_cast<int>(i) == reopen.window.active)
                    front = tab;
            }
            if (top && front && ::IsWindow(top))
                selectTab(top, front);
        }
        log("opened again " + std::to_string(tabs) + " tab(s), " + std::to_string(windows) + " new window(s)"
            + (atLogon ? " after the logon" : ""));
        wait(kHoldAfterMs);
    }

    void run()
    {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // The first start in this logon: what the file holds is what the
        // logon before left.
        bool firstInLogon = options.asIfLogon;
        if (!::GlobalFindAtomW(kLogonAtom)) {
            ::GlobalAddAtomW(kLogonAtom);
            firstInLogon = true;
        }
        Session stored = parseSession(readFile(options.file));
        const bool wasOpen = stored.open;
        if (firstInLogon)
            stored = newLogon(std::move(stored));
        {
            const std::lock_guard lock(mutex);
            session = stored;
        }
        if (firstInLogon)
            save(stored);
        else
            written = serialize(stored);
        if (firstInLogon && options.restoreAtLogon && wasOpen) {
            const bool shellThere = waitFor([] { return shellAgeMs() > 0; }, kShellWaitMs);
            if (shellThere && (options.asIfLogon || shellAgeMs() <= kLogonMs))
                restoreNow(true);
        }
        while (wait(kLookEveryMs)) {
            if (restoreAsked.exchange(false))
                restoreNow(false);
            else
                keep();
        }
        shellWindows.Reset();
        automation.Reset();
        if (SUCCEEDED(com))
            ::CoUninitialize();
    }
};

Keeper::Keeper(KeeperOptions options) : m(std::make_unique<Impl>(std::move(options))) {}

Keeper::~Keeper()
{
    stop();
}

void Keeper::start()
{
    if (!m->thread.joinable() && !m->options.file.empty())
        m->thread = std::thread([this] { m->run(); });
}

void Keeper::stop()
{
    if (!m->thread.joinable())
        return;
    m->stopping = true;
    ::SetEvent(m->wake);
    m->thread.join();
}

Keeper::Left Keeper::left() const
{
    const std::lock_guard lock(m->mutex);
    return {static_cast<int>(m->session.previous.size()), tabCount(m->session.previous)};
}

bool Keeper::restoring() const
{
    return m->busy;
}

void Keeper::restore()
{
    m->restoreAsked = true;
    ::SetEvent(m->wake);
}

} // namespace tabs

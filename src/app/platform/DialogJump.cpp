#include "DialogJump.h"

#include <QDebug>

#include <ole2.h> // before exdisp.h: WIN32_LEAN_AND_MEAN keeps it out of windows.h

#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace ws {

namespace {

// Controls of the common file dialogs (dlgs.h).
constexpr int kFileNameEdit = 0x480; // edt1
constexpr int kFileNameCombo = 0x47C; // cmb13

DialogJump* g_instance = nullptr; // for the WinEvent callback
std::atomic<bool> g_jumping {false}; // one jump at a time

bool hasClass(HWND hwnd, const wchar_t* name)
{
    wchar_t buffer[64] {};
    return ::GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer))) > 0 && ::wcscmp(buffer, name) == 0;
}

// Down a path of window classes, taking the first child of each; nullptr
// where one is missing.
HWND descend(HWND hwnd, std::initializer_list<const wchar_t*> classes)
{
    for (const wchar_t* name : classes) {
        if (!hwnd)
            break; // FindWindowEx would search the top-level windows
        hwnd = ::FindWindowExW(hwnd, nullptr, name, nullptr);
    }
    return hwnd;
}

// The address bar of a dialog of Windows Vista and later: crumbs, and a text
// box in their place while it is being edited.
HWND addressBar(HWND dialog)
{
    return descend(dialog, {L"WorkerW", L"ReBarWindow32", L"Address Band Root", L"msctls_progress32"});
}

HWND crumbBar(HWND dialog)
{
    return descend(addressBar(dialog), {L"Breadcrumb Parent", L"ToolbarWindow32"});
}

HWND addressBox(HWND dialog) // only while it is shown
{
    const HWND edit = descend(addressBar(dialog), {L"ComboBoxEx32", L"ComboBox", L"Edit"});
    return edit && ::IsWindowVisible(edit) ? edit : nullptr;
}

// The file name box of a dialog in the Windows XP style.
HWND fileNameBox(HWND dialog)
{
    if (const HWND combo = ::GetDlgItem(dialog, kFileNameCombo)) {
        const HWND list = hasClass(combo, L"ComboBoxEx32") ? ::FindWindowExW(combo, nullptr, L"ComboBox", nullptr) : combo;
        if (const HWND edit = list ? ::FindWindowExW(list, nullptr, L"Edit", nullptr) : nullptr)
            return edit;
    }
    return ::GetDlgItem(dialog, kFileNameEdit);
}

enum class Kind { None, Modern, Legacy };

Kind dialogKind(HWND hwnd)
{
    if (!hwnd || !hasClass(hwnd, L"#32770"))
        return Kind::None;
    // Windows Vista and later: IFileDialog, and GetOpenFileName without a hook.
    if (::FindWindowExW(hwnd, nullptr, L"DUIViewWndClassName", nullptr))
        return crumbBar(hwnd) ? Kind::Modern : Kind::None;
    // The Windows XP style, still shown for GetOpenFileName with a hook or template.
    if (::FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) && fileNameBox(hwnd) && ::GetDlgItem(hwnd, IDOK))
        return Kind::Legacy;
    return Kind::None;
}

// The folder on disk that an Explorer tab shows; empty for Home, This PC, a
// search and the like.
std::wstring folderPath(IShellBrowser* browser)
{
    ComPtr<IShellView> view;
    ComPtr<IFolderView> folderView;
    ComPtr<IPersistFolder2> folder;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folderView))
        || FAILED(folderView->GetFolder(IID_PPV_ARGS(&folder))) || FAILED(folder->GetCurFolder(&pidl)) || !pidl)
        return {};
    std::wstring path;
    PWSTR name = nullptr;
    if (SUCCEEDED(::SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &name))) {
        path = name;
        ::CoTaskMemFree(name);
    }
    ::ILFree(pidl);
    return path;
}

// The folder of the frontmost File Explorer window, in the tab it shows. A
// window showing no folder on disk is passed over for the next one back.
// Needs COM.
std::wstring explorerFolder()
{
    ComPtr<IShellWindows> windows;
    long count = 0;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))
        || FAILED(windows->get_Count(&count)))
        return {};
    struct Tab {
        HWND window;
        HWND tab;
        std::wstring path;
    };
    std::vector<Tab> tabs; // one per tab of each window
    for (long i = 0; i < count; ++i) {
        VARIANT index {};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> browser;
        HWND tab = nullptr;
        if (windows->Item(index, &item) != S_OK || !item || FAILED(item.As(&services))
            || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))
            || FAILED(browser->GetWindow(&tab)) || !tab)
            continue;
        tabs.push_back({::GetAncestor(tab, GA_ROOT), tab, folderPath(browser.Get())});
    }

    struct Search {
        const std::vector<Tab>& tabs;
        std::wstring found;
    } search {tabs, {}};
    // Top-level windows come front to back.
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(param);
            // Windows 11 has a child window per tab: the first one shown is on top.
            HWND shown = nullptr;
            for (HWND child = ::FindWindowExW(hwnd, nullptr, L"ShellTabWindowClass", nullptr); child && !shown;
                child = ::FindWindowExW(hwnd, child, L"ShellTabWindowClass", nullptr)) {
                if (::IsWindowVisible(child))
                    shown = child;
            }
            for (const Tab& tab : search.tabs) {
                if (tab.window == hwnd && (!shown || tab.tab == shown || tab.tab == hwnd) && !tab.path.empty()) {
                    search.found = tab.path;
                    return FALSE;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool send(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DWORD_PTR result = 0;
    return ::SendMessageTimeoutW(hwnd, msg, wParam, lParam, SMTO_ABORTIFHUNG, 2000, &result) != 0;
}

template <typename Done> bool waitFor(Done done, std::chrono::milliseconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= end)
            return false;
        ::Sleep(5);
    }
    return true;
}

HWND focusedControl(HWND dialog)
{
    GUITHREADINFO info {sizeof(GUITHREADINFO)};
    const DWORD thread = ::GetWindowThreadProcessId(dialog, nullptr);
    return thread && ::GetGUIThreadInfo(thread, &info) ? info.hwndFocus : nullptr;
}

// Types the folder into the address bar, as in Explorer. Unlike the file
// name box, the address bar never accepts the dialog: in a folder picker,
// OK with a folder in the box would choose it and close the dialog.
bool goModern(HWND dialog, const std::wstring& folder)
{
    const HWND bar = addressBar(dialog);
    const HWND crumbs = crumbBar(dialog);
    if (!bar || !crumbs)
        return false;
    const HWND focus = focusedControl(dialog);
    HWND box = addressBox(dialog); // the user may be editing it already
    if (!box) {
        // A click past the last crumb, in the dialog's own coordinates: a
        // program that is not DPI aware has scaled ones.
        RECT client {};
        const DPI_AWARENESS_CONTEXT ours = ::SetThreadDpiAwarenessContext(::GetWindowDpiAwarenessContext(crumbs));
        ::GetClientRect(crumbs, &client);
        ::SetThreadDpiAwarenessContext(ours);
        const LPARAM at = MAKELPARAM(std::max<LONG>(client.right - 4, 0), client.bottom / 2);
        ::PostMessageW(crumbs, WM_LBUTTONDOWN, MK_LBUTTON, at);
        ::PostMessageW(crumbs, WM_LBUTTONUP, 0, at);
        if (!waitFor([&] { return (box = addressBox(dialog)) != nullptr; }, 1000ms))
            return false;
    }
    if (!send(box, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(folder.c_str()))
        || !send(box, WM_KEYDOWN, VK_RETURN, 0x001C0001) || !send(box, WM_KEYUP, VK_RETURN, 0xC01C0001))
        return false;
    // The crumbs come back with the keyboard focus: return it to where it was
    // (usually the file name).
    if (waitFor([&] { return !::IsWindowVisible(box); }, 2000ms) && focus && !::IsChild(bar, focus)
        && ::IsChild(dialog, focus) && ::IsWindowVisible(focus))
        send(dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(focus), TRUE);
    return true;
}

// The Windows XP style has no address bar: the folder goes into the file name
// box and OK opens it, then the name typed before comes back. With the
// trailing backslash a Save dialog cannot take it for a file name: a folder
// that is gone only gets an error message.
bool goLegacy(HWND dialog, const std::wstring& folder)
{
    const HWND box = fileNameBox(dialog);
    const HWND ok = ::GetDlgItem(dialog, IDOK);
    if (!box || !ok)
        return false;
    wchar_t typed[1024] {};
    DWORD_PTR length = 0;
    ::SendMessageTimeoutW(box, WM_GETTEXT, std::size(typed), reinterpret_cast<LPARAM>(typed), SMTO_ABORTIFHUNG, 2000, &length);
    const std::wstring target = folder.ends_with(L'\\') ? folder : folder + L'\\';
    if (!send(box, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(target.c_str()))
        || !send(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(ok)))
        return false;
    send(box, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(typed));
    return true;
}

} // namespace

DialogJump::DialogJump(std::function<bool(bool)> setHotkey)
    : m_setHotkey(std::move(setHotkey))
{
    g_instance = this;
    m_recheck.setInterval(100);
    QObject::connect(&m_recheck, &QTimer::timeout, &m_recheck, [this] {
        const HWND foreground = ::GetForegroundWindow();
        if (foreground == m_pending && dialogKind(foreground) != Kind::None) {
            m_recheck.stop();
            setRegistered(true);
        } else if (foreground != m_pending || --m_retries <= 0) {
            m_recheck.stop();
        }
    });
    // Out of context: delivered through this (the GUI) thread's message loop.
    m_hook = ::SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &DialogJump::onForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
    follow(::GetForegroundWindow());
}

DialogJump::~DialogJump()
{
    if (m_hook)
        ::UnhookWinEvent(m_hook);
    g_instance = nullptr;
    setRegistered(false);
}

void CALLBACK DialogJump::onForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD)
{
    if (g_instance)
        g_instance->follow(hwnd);
}

void DialogJump::follow(HWND foreground)
{
    m_recheck.stop();
    const bool fileDialog = dialogKind(foreground) != Kind::None;
    setRegistered(fileDialog);
    // A dialog may come to the front before all of its controls are there.
    if (!fileDialog && foreground && hasClass(foreground, L"#32770")) {
        m_pending = foreground;
        m_retries = 5;
        m_recheck.start();
    }
}

void DialogJump::setRegistered(bool on)
{
    if (on == m_registered)
        return;
    if (!on) {
        m_setHotkey(false);
        m_registered = false;
        return;
    }
    m_registered = m_setHotkey(true);
    if (!m_registered && !std::exchange(m_warned, true))
        qWarning() << "Ctrl+G is taken by another program: no jumping to Explorer's folder in file dialogs";
}

void DialogJump::jump()
{
    const HWND dialog = ::GetForegroundWindow();
    const Kind kind = dialogKind(dialog);
    if (kind == Kind::None || g_jumping.exchange(true))
        return;
    // Asking Explorer and waiting for the dialog take a while: not on the GUI
    // thread, which may be running a file dialog of our own.
    std::thread([dialog, kind] {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const std::wstring folder = explorerFolder();
        if (!folder.empty() && !(kind == Kind::Modern ? goModern(dialog, folder) : goLegacy(dialog, folder)))
            qWarning() << "Ctrl+G: the file dialog did not go to the folder" << (kind == Kind::Legacy ? "(old style)" : "");
        if (SUCCEEDED(com))
            ::CoUninitialize();
        g_jumping = false;
    }).detach();
}

} // namespace ws

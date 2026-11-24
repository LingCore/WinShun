#include "ShellContextMenu.h"

#include <QDir>
#include <QFileInfo>

#include <commctrl.h>
#include <imm.h>
#include <shellapi.h> // before shobjidl: CMIC_MASK_UNICODE is SEE_MASK_UNICODE
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ws::shellcontext {

namespace {

constexpr UINT kFirstId = 1; // the handler's commands: kFirstId + its own numbering
constexpr UINT kLastId = 0x7FFF;
constexpr UINT_PTR kSubclassId = 0x5753; // "WS"
constexpr UINT_PTR kKeySubclassId = 0x574B; // "WK": watchKeys

UINT g_lastKey = 0; // lastKeyPressed(); the GUI thread's alone
HWND g_launcher = nullptr; // watchKeys': the window the commands' dialogs hand the focus back to

// A dialog of a command's closed and left the focus with this hidden window
// (its owner): to the launcher, while that is still up (the command kept
// it: 删除's progress window, a confirmation). Hidden, it is left alone.
LRESULT CALLBACK ownerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE && g_launcher && ::IsWindowVisible(g_launcher))
        ::SetForegroundWindow(g_launcher);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK keyWatchProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        g_lastKey = static_cast<UINT>(wParam);
        if (wParam == VK_PROCESSKEY) {
            // The IME took the press. ImmGetVirtualKey knows nothing of a
            // TSF text service's (Microsoft Pinyin: it gives VK_PROCESSKEY
            // back); the scan code in lParam is still the key's own.
            const UINT scan = (static_cast<UINT>(lParam) >> 16) & 0xFF;
            const bool extended = (lParam & (1 << 24)) != 0;
            if (const UINT key = ::MapVirtualKeyW(scan | (extended ? 0xE000u : 0u), MAPVK_VSC_TO_VK_EX))
                g_lastKey = key;
            else if (const UINT imm = ::ImmGetVirtualKey(hwnd); imm != VK_PROCESSKEY)
                g_lastKey = imm;
        }
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

struct IdLists {
    std::vector<PIDLIST_ABSOLUTE> items;
    ~IdLists()
    {
        for (PIDLIST_ABSOLUTE item : items)
            ::CoTaskMemFree(item);
    }
};

// The handler for the items' menu, through a shell item array: their
// folder's, for items of one folder.
ComPtr<IContextMenu> handlerFor(const std::vector<PIDLIST_ABSOLUTE>& items, UINT count)
{
    const std::vector<PCIDLIST_ABSOLUTE> view(items.begin(), items.begin() + count); // the array wants them const
    ComPtr<IShellItemArray> array;
    ComPtr<IContextMenu> menu;
    if (FAILED(::SHCreateShellItemArrayFromIDLists(count, const_cast<PCIDLIST_ABSOLUTE*>(view.data()), &array))
        || FAILED(array->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu))))
        return nullptr;
    return menu;
}

struct MenuHandler {
    ComPtr<IContextMenu> menu;
    bool singleFallback = false;
};

// The handler for the menu of `paths`, the first of them the one the menu
// was asked on: all of them when they are one folder's; items of several
// folders get the menu of that first one alone. (The item array binds a
// menu for its first item when their folders differ; the desktop folder's
// GetUIObjectOf with whole ID lists, as for the drag-out data object, gives
// the desktop's own short menu, whose 删除 does nothing to the files.)
MenuHandler handlerFor(const QStringList& paths)
{
    IdLists lists;
    bool oneFolder = true;
    for (const QString& path : paths) {
        // Qt paths use '/', which the shell rejects with E_INVALIDARG.
        const QString native = QDir::toNativeSeparators(path);
        PIDLIST_ABSOLUTE item = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(reinterpret_cast<const wchar_t*>(native.utf16()), nullptr, &item, 0, nullptr)))
            lists.items.push_back(item);
        if (QFileInfo(path).absolutePath().compare(QFileInfo(paths.first()).absolutePath(), Qt::CaseInsensitive) != 0)
            oneFolder = false;
    }
    if (lists.items.empty())
        return {};
    const auto count = static_cast<UINT>(lists.items.size());
    if (oneFolder && count > 1) {
        if (ComPtr<IContextMenu> menu = handlerFor(lists.items, count))
            return {std::move(menu), false};
    }
    return {handlerFor(lists.items, 1), paths.size() > 1};
}

// While the menu is up, its owner hands these on to the handler: 发送到
// and 打开方式 fill in on WM_INITMENUPOPUP (and stay empty without it),
// items an extension draws itself need WM_MEASUREITEM / WM_DRAWITEM, and
// WM_MENUCHAR picks by letter. The launcher's window is subclassed for
// just that long: a Qt window, which knows nothing of these.
struct Forwarder {
    IContextMenu2* menu2 = nullptr;
    IContextMenu3* menu3 = nullptr;
};

LRESULT CALLBACK forwardProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
{
    switch (msg) {
    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_MENUCHAR: {
        const auto* forwarder = reinterpret_cast<const Forwarder*>(data);
        if (forwarder->menu3) {
            LRESULT result = 0;
            // E_FAIL for the top-level menu itself (not the handler's): on to Qt's handling, which is none.
            if (SUCCEEDED(forwarder->menu3->HandleMenuMsg2(msg, wParam, lParam, &result)))
                return result;
        } else if (forwarder->menu2 && SUCCEEDED(forwarder->menu2->HandleMenuMsg(msg, wParam, lParam))) {
            return 0;
        }
        break;
    }
    default:
        break;
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

// What a command opens belongs to this window, never to the launcher's: an
// owned window goes out of sight with its owner, and the launcher goes
// (for 打开 and the like) before the command runs. A hidden top-level
// window of this thread; made once, kept for the process.
HWND dialogOwner()
{
    static const HWND owner = [] {
        WNDCLASSEXW wc {sizeof wc};
        wc.lpfnWndProc = &ownerProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WinShun.MenuOwner";
        ::RegisterClassExW(&wc);
        return ::CreateWindowExW(
            WS_EX_TOOLWINDOW, wc.lpszClassName, L"WinShun", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    }();
    return owner;
}

std::wstring verbOf(IContextMenu* menu, UINT_PTR command)
{
    wchar_t verb[128] {};
    if (FAILED(menu->GetCommandString(command, GCS_VERBW, nullptr, reinterpret_cast<char*>(verb), 128)))
        return {};
    std::wstring text(verb);
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

} // namespace

Choice showMenu(const QStringList& paths, HWND owner, POINT pos, bool extended,
    const std::function<void(const Choice&)>& picked)
{
    Choice choice;
    const MenuHandler resolved = handlerFor(paths);
    const ComPtr<IContextMenu> menu = resolved.menu;
    if (!menu)
        return choice;
    choice.singleFallback = resolved.singleFallback;
    const HMENU popup = ::CreatePopupMenu();
    if (!popup)
        return choice;
    // CMF_ITEMMENU: a menu on items, as Explorer asks for one. Not
    // CMF_CANRENAME: 重命名 renames in the view that asked, and there is
    // none here; left out, the item stays off the menu.
    UINT flags = CMF_NORMAL | CMF_ITEMMENU;
    if (extended)
        flags |= CMF_EXTENDEDVERBS;
    if (FAILED(menu->QueryContextMenu(popup, 0, kFirstId, kLastId, flags))) {
        ::DestroyMenu(popup);
        return choice;
    }
    Forwarder forwarder;
    ComPtr<IContextMenu2> menu2;
    ComPtr<IContextMenu3> menu3;
    if (SUCCEEDED(menu.As(&menu3)))
        forwarder.menu3 = menu3.Get();
    else if (SUCCEEDED(menu.As(&menu2)))
        forwarder.menu2 = menu2.Get();
    const bool subclassed = ::SetWindowSubclass(owner, &forwardProc, kSubclassId, reinterpret_cast<DWORD_PTR>(&forwarder));
    // TPM_RETURNCMD: the choice comes back here, no WM_COMMAND. Not
    // TPM_NONOTIFY (shell::popupMenu has it): the owner must hear
    // WM_INITMENUPOPUP for the submenus. The owner is the window in front,
    // so a click elsewhere closes the menu, and that click goes no further.
    const int id = static_cast<int>(::TrackPopupMenuEx(
        popup, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pos.x, pos.y, owner, nullptr));
    if (subclassed)
        ::RemoveWindowSubclass(owner, &forwardProc, kSubclassId);
    ::PostMessageW(owner, WM_NULL, 0, 0);
    if (id >= static_cast<int>(kFirstId)) {
        choice.chosen = true;
        const UINT_PTR command = static_cast<UINT_PTR>(id) - kFirstId;
        choice.verb = verbOf(menu.Get(), command); // before the command: it may change the handler's mind
        choice.shift = ::GetKeyState(VK_SHIFT) < 0;
        choice.ctrl = ::GetKeyState(VK_CONTROL) < 0;
        if (picked)
            picked(choice);
        CMINVOKECOMMANDINFOEX info {};
        info.cbSize = sizeof info;
        info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        if (choice.shift)
            info.fMask |= CMIC_MASK_SHIFT_DOWN;
        if (choice.ctrl)
            info.fMask |= CMIC_MASK_CONTROL_DOWN;
        info.hwnd = dialogOwner();
        info.lpVerb = MAKEINTRESOURCEA(command);
        info.lpVerbW = MAKEINTRESOURCEW(command);
        info.nShow = SW_SHOWNORMAL;
        info.ptInvoke = pos;
        choice.invoked = SUCCEEDED(menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info)));
    }
    ::DestroyMenu(popup);
    return choice;
}

void watchKeys(HWND window)
{
    g_launcher = window;
    ::SetWindowSubclass(window, &keyWatchProc, kKeySubclassId, 0);
}

UINT lastKeyPressed()
{
    return g_lastKey;
}

bool keepsLauncher(std::wstring_view verb)
{
    static constexpr std::wstring_view kStays[] {
        L"copy", L"cut", L"delete", L"link", L"copyaspath", L"pintohome", L"unpinfromhome", L"taskbarpin",
        L"taskbarunpin", L"pintostartscreen", L"unpinfromstartscreen"};
    return std::find(std::begin(kStays), std::end(kStays), verb) != std::end(kStays);
}

} // namespace ws::shellcontext

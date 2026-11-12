// File Explorer's windows and tabs as the plugin reads them: the top-level
// windows (CabinetWClass), their tab windows (ShellTabWindowClass), and what
// IShellWindows tells about a tab (its folder, as an ID list and as a parsing
// name). Shared by the engine that merges new windows (Engine) and the one
// that keeps and restores the open tabs (Session). Calls that take a tab's
// IWebBrowser2 wait on Explorer: on an STA thread of the caller's own.
#pragma once

#include "Rules.h"

#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
// After shlobj.h: exdisp.h needs what it brings in (docobj.h) under WIN32_LEAN_AND_MEAN.
#include <exdisp.h>
#include <wrl/client.h>

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace tabs {

using Microsoft::WRL::ComPtr;

// Explorer's own commands for a tab window (what Ctrl+T, Ctrl+W and
// Ctrl+<n> send; ExplorerTabUtility uses the same).
inline constexpr WPARAM kNewTab = 0xA21B;
inline constexpr WPARAM kCloseTab = 0xA021;
inline constexpr WPARAM kSelectTab = 0xA221; // lParam: 1-based position in the tab strip

inline std::wstring className(HWND w)
{
    wchar_t buffer[64] {};
    ::GetClassNameW(w, buffer, 64);
    return buffer;
}

// A window's text without a message to it: another process's window
// answers from what the system keeps, so a busy Explorer cannot stall it.
inline std::wstring textOf(HWND w)
{
    wchar_t buffer[512] {};
    ::InternalGetWindowText(w, buffer, 512);
    return buffer;
}

inline bool isExplorerTop(HWND w)
{
    return w && ::GetAncestor(w, GA_PARENT) == ::GetDesktopWindow() && className(w) == L"CabinetWClass";
}

// The tabs of an Explorer window in z-order: the active one first.
inline std::vector<HWND> tabsOf(HWND top)
{
    std::vector<HWND> tabs;
    for (HWND t = ::FindWindowExW(top, nullptr, L"ShellTabWindowClass", nullptr); t;
        t = ::FindWindowExW(top, t, L"ShellTabWindowClass", nullptr))
        tabs.push_back(t);
    return tabs;
}

inline HWND activeTab(HWND top)
{
    return ::FindWindowExW(top, nullptr, L"ShellTabWindowClass", nullptr);
}

inline bool cloaked(HWND w)
{
    DWORD value = 0;
    return SUCCEEDED(::DwmGetWindowAttribute(w, DWMWA_CLOAKED, &value, sizeof value)) && value != 0;
}

// Top-level Explorer windows in z-order (the one in front first).
inline std::vector<HWND> explorerWindows()
{
    std::vector<HWND> windows;
    for (HWND w = ::FindWindowExW(nullptr, nullptr, L"CabinetWClass", nullptr); w; w = ::FindWindowExW(nullptr, w, L"CabinetWClass", nullptr))
        windows.push_back(w);
    return windows;
}

struct PidlFree {
    void operator()(void* p) const { ::CoTaskMemFree(p); }
};
using Pidl = std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, PidlFree>;
using ChildPidl = std::unique_ptr<std::remove_pointer_t<PITEMID_CHILD>, PidlFree>;

inline ComPtr<IShellBrowser> shellBrowserOf(IWebBrowser2* web)
{
    ComPtr<IServiceProvider> services;
    ComPtr<IShellBrowser> browser;
    if (web && SUCCEEDED(web->QueryInterface(IID_PPV_ARGS(&services))))
        services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    return browser;
}

inline ComPtr<IFolderView> folderViewOf(IWebBrowser2* web)
{
    ComPtr<IShellView> view;
    ComPtr<IFolderView> folderView;
    if (const ComPtr<IShellBrowser> browser = shellBrowserOf(web); browser && SUCCEEDED(browser->QueryActiveShellView(&view)) && view)
        view.As(&folderView);
    return folderView;
}

// The folder a tab shows, as an ID list; null while it has no view yet.
inline Pidl folderOf(IWebBrowser2* web)
{
    ComPtr<IPersistFolder2> folder;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (const ComPtr<IFolderView> view = folderViewOf(web);
        view && SUCCEEDED(view->GetFolder(IID_PPV_ARGS(&folder))) && SUCCEEDED(folder->GetCurFolder(&pidl)))
        return Pidl(pidl);
    return nullptr;
}

inline std::wstring parsingName(PCIDLIST_ABSOLUTE pidl, SIGDN form = SIGDN_DESKTOPABSOLUTEPARSING)
{
    std::wstring name;
    PWSTR text = nullptr;
    if (pidl && SUCCEEDED(::SHGetNameFromIDList(pidl, form, &text)) && text) {
        name = text;
        ::CoTaskMemFree(text);
    }
    return name;
}

inline std::wstring locationUrl(IWebBrowser2* web)
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
inline Place placeOf(IWebBrowser2* web)
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
inline HRESULT navigate(IWebBrowser2* web, PCIDLIST_ABSOLUTE pidl)
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

inline bool disconnected(HRESULT hr)
{
    return hr == RPC_E_DISCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED)
        || hr == CO_E_OBJNOTCONNECTED;
}

// Explorer selects a tab by its place in the strip (Ctrl+1…9), but its
// windows are in z-order: each place is tried until the wanted tab is the
// active one (ExplorerTabUtility does the same). One second in all, however
// many tabs.
inline bool selectTab(HWND top, HWND wanted)
{
    if (activeTab(top) == wanted)
        return true;
    const ULONGLONG until = ::GetTickCount64() + 1000;
    const std::size_t n = tabsOf(top).size();
    for (std::size_t i = 0; i < n; ++i) {
        const ULONGLONG now = ::GetTickCount64();
        if (now >= until)
            break;
        DWORD_PTR result = 0;
        ::SendMessageTimeoutW(top, WM_COMMAND, kSelectTab, static_cast<LPARAM>(i + 1), SMTO_ABORTIFHUNG, static_cast<UINT>(until - now), &result);
        if (activeTab(top) == wanted)
            return true;
    }
    return false;
}

// A tab as IShellWindows lists it.
struct TabEntry {
    ComPtr<IWebBrowser2> web;
    HWND top = nullptr; // its Explorer window
    HWND tab = nullptr; // its ShellTabWindowClass
};

inline std::optional<TabEntry> tabEntryAt(IShellWindows* windows, long index)
{
    VARIANT v {};
    v.vt = VT_I4;
    v.lVal = index;
    ComPtr<IDispatch> item;
    TabEntry e;
    SHANDLE_PTR top = 0;
    if (windows->Item(v, &item) != S_OK || !item || FAILED(item.As(&e.web)) || FAILED(e.web->get_HWND(&top)))
        return std::nullopt;
    e.top = reinterpret_cast<HWND>(top);
    if (const ComPtr<IShellBrowser> browser = shellBrowserOf(e.web.Get()))
        browser->GetWindow(&e.tab);
    return e;
}

} // namespace tabs

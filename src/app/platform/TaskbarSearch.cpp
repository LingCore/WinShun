#include "TaskbarSearch.h"

#include "Win32Util.h"

#include <knownfolders.h>
#include <sddl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <propkey.h> // after shobjidl.h, which defines PROPERTYKEY
#include <propvarutil.h>
#include <wrl/client.h>

#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ws::taskbar {

namespace {

std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    std::wstring folder;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)))
        folder = path;
    ::CoTaskMemFree(path);
    return folder;
}

// The program a shortcut starts.
std::wstring shortcutTarget(const std::wstring& link)
{
    ComPtr<IShellLinkW> shortcut;
    ComPtr<IPersistFile> file;
    wchar_t target[MAX_PATH] {};
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shortcut)))
        || FAILED(shortcut.As(&file)) || FAILED(file->Load(link.c_str(), STGM_READ))
        || FAILED(shortcut->GetPath(target, MAX_PATH, nullptr, 0)))
        return {};
    return target;
}

// The first shortcut in `folder` (not below) that starts `target`.
std::wstring findShortcut(const std::wstring& folder, const std::wstring& target)
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::filesystem::path& path = entry.path();
        if (_wcsicmp(path.extension().c_str(), L".lnk") == 0
            && _wcsicmp(shortcutTarget(path.wstring()).c_str(), target.c_str()) == 0)
            return path.wstring();
    }
    return {};
}

// "S-1-5-21-…": the user Win顺 runs as.
std::wstring userSid()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    const win32::UniqueHandle owned(token);
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    wchar_t* text = nullptr;
    if (size == 0 || !::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)
        || !::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text))
        return {};
    std::wstring sid = text;
    ::LocalFree(text);
    return sid;
}

// The taskbar on `monitor`: the main one, or the one another monitor has.
HWND taskbarOn(HMONITOR monitor)
{
    const HWND main = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (main && ::MonitorFromWindow(main, MONITOR_DEFAULTTONEAREST) == monitor)
        return main;
    for (HWND other = nullptr; (other = ::FindWindowExW(nullptr, other, L"Shell_SecondaryTrayWnd", nullptr));) {
        if (::MonitorFromWindow(other, MONITOR_DEFAULTTONEAREST) == monitor)
            return other;
    }
    return nullptr;
}

// Windows 11 centres the taskbar's icons unless told not to (TaskbarAl 0);
// Windows 10 has them at the start, and no such value.
bool iconsCentred()
{
    DWORD value = 0;
    DWORD size = sizeof value;
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            L"TaskbarAl", RRF_RT_REG_DWORD, nullptr, &value, &size)
        == ERROR_SUCCESS)
        return value != 0;
    wchar_t build[16] {};
    DWORD bytes = sizeof build - sizeof(wchar_t);
    return ::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber",
               RRF_RT_REG_SZ, nullptr, build, &bytes)
            == ERROR_SUCCESS
        && std::wcstol(build, nullptr, 10) >= 22000;
}

std::optional<Spot> spotOf(HWND bar, const std::optional<POINT>& pointer)
{
    Spot spot;
    MONITORINFO info {sizeof info};
    if (!::GetWindowRect(bar, &spot.taskbar)
        || !::GetMonitorInfoW(::MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST), &info)
        || !::IntersectRect(&spot.taskbar, &spot.taskbar, &info.rcMonitor))
        return {};
    const RECT& r = spot.taskbar;
    const RECT& m = info.rcMonitor;
    const bool across = r.right - r.left >= r.bottom - r.top;
    if (across)
        spot.edge = r.top + r.bottom >= m.top + m.bottom ? Edge::Bottom : Edge::Top;
    else
        spot.edge = r.left + r.right >= m.left + m.right ? Edge::Right : Edge::Left;
    if (pointer)
        spot.anchor = *pointer;
    else if (across)
        spot.anchor = {iconsCentred() ? (r.left + r.right) / 2 : r.left, (r.top + r.bottom) / 2};
    else
        spot.anchor = {(r.left + r.right) / 2, r.top};
    return spot;
}

} // namespace

bool isTaskbar(HWND window)
{
    wchar_t name[32] {};
    return window && ::GetClassNameW(window, name, static_cast<int>(std::size(name)))
        && (std::wcscmp(name, L"Shell_TrayWnd") == 0 || std::wcscmp(name, L"Shell_SecondaryTrayWnd") == 0);
}

HANDLE createEvent()
{
    // Everything for the system and administrators (Win顺); setting it and
    // waiting on it for the user. The label lets processes of medium
    // integrity write to it: made by an elevated process, it would be high,
    // and they could not.
    const std::wstring sid = userSid();
    if (sid.empty())
        return nullptr;
    const std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100002;;;" + sid + L")S:(ML;;NW;;;ME)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return nullptr;
    SECURITY_ATTRIBUTES attributes {sizeof attributes, descriptor, FALSE};
    const HANDLE event = ::CreateEventW(&attributes, FALSE, FALSE, kEventName);
    ::LocalFree(descriptor);
    return event;
}

std::optional<Spot> locate(bool atPointer)
{
    POINT pointer {};
    ::GetCursorPos(&pointer);
    if (atPointer) {
        const HWND under = ::GetAncestor(::WindowFromPoint(pointer), GA_ROOT);
        if (isTaskbar(under))
            return spotOf(under, pointer);
    }
    const HWND bar = taskbarOn(::MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST));
    return bar ? spotOf(bar, std::nullopt) : std::nullopt;
}

std::optional<Spot> locateAt(POINT button)
{
    const HWND bar = taskbarOn(::MonitorFromPoint(button, MONITOR_DEFAULTTONEAREST));
    return bar ? spotOf(bar, button) : locate(false);
}

bool windowsSearchShown()
{
    // The policy ("Configure search on the taskbar") wins over the user's
    // choice; either way 0 is hidden. Without a value Windows shows it.
    DWORD mode = 1;
    DWORD size = sizeof mode;
    if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search",
            L"SearchOnTaskbarMode", RRF_RT_REG_DWORD, nullptr, &mode, &size)
        == ERROR_SUCCESS)
        return mode != 0;
    size = sizeof mode;
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search",
            L"SearchboxTaskbarMode", RRF_RT_REG_DWORD, nullptr, &mode, &size)
        != ERROR_SUCCESS)
        return true;
    return mode != 0;
}

std::wstring buttonShortcut(const std::wstring& button, const std::wstring& name)
{
    std::wstring link;
    for (const KNOWNFOLDERID& id : {FOLDERID_CommonPrograms, FOLDERID_Programs}) {
        const std::wstring folder = knownFolder(id);
        if (link.empty() && !folder.empty())
            link = findShortcut(folder, button);
    }
    ComPtr<IShellLinkW> shortcut;
    ComPtr<IPersistFile> file;
    ComPtr<IPropertyStore> properties;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shortcut)))
        || FAILED(shortcut.As(&file)) || FAILED(shortcut.As(&properties)))
        return link;
    if (!link.empty()) {
        // The installer's (or ours, from before the id): given the id if it
        // lacks it. Not read: left as it is (saved, it would be an empty one).
        if (FAILED(file->Load(link.c_str(), STGM_READWRITE)))
            return link;
        PROPVARIANT id {};
        const bool has = SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &id)) && id.vt == VT_LPWSTR
            && std::wcscmp(id.pwszVal, kButtonAppId) == 0;
        ::PropVariantClear(&id);
        if (has)
            return link;
    } else {
        const std::wstring programs = knownFolder(FOLDERID_Programs);
        if (programs.empty())
            return {};
        link = programs + L'\\' + name + L".lnk";
        const std::wstring folder = std::filesystem::path(button).parent_path().wstring();
        if (FAILED(shortcut->SetPath(button.c_str())) || FAILED(shortcut->SetWorkingDirectory(folder.c_str())))
            return {};
    }
    PROPVARIANT id {};
    if (SUCCEEDED(::InitPropVariantFromString(kButtonAppId, &id)) && SUCCEEDED(properties->SetValue(PKEY_AppUserModel_ID, id))
        && SUCCEEDED(properties->Commit()) && SUCCEEDED(file->Save(link.c_str(), TRUE)))
        ::SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, link.c_str(), nullptr); // for the Start menu to see it
    ::PropVariantClear(&id);
    return link;
}

bool buttonPinned(const std::wstring& button)
{
    const std::wstring data = knownFolder(FOLDERID_RoamingAppData);
    return !data.empty()
        && !findShortcut(data + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar", button).empty();
}

} // namespace ws::taskbar

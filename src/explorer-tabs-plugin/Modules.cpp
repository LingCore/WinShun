#include "Modules.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <string_view>
#include <vector>

namespace tabs {

namespace {

std::string utf8(std::wstring_view w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The folder of a file, with its trailing backslash.
std::wstring folderOf(std::wstring_view path)
{
    const std::size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring_view::npos ? std::wstring() : std::wstring(path.substr(0, slash + 1));
}

bool under(std::wstring_view path, std::wstring_view folder)
{
    return !folder.empty() && path.size() > folder.size()
        && ::CompareStringOrdinal(path.data(), static_cast<int>(folder.size()), folder.data(), static_cast<int>(folder.size()), TRUE)
        == CSTR_EQUAL;
}

} // namespace

std::string foreignModules()
{
    std::vector<HMODULE> modules(1024);
    DWORD needed = 0;
    if (!::EnumProcessModulesEx(::GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
            &needed, LIST_MODULES_ALL))
        return "?";
    modules.resize(std::min<std::size_t>(modules.size(), needed / sizeof(HMODULE)));
    wchar_t buffer[MAX_PATH] {};
    const UINT length = ::GetWindowsDirectoryW(buffer, MAX_PATH);
    const std::wstring windows = length > 0 && length < MAX_PATH ? std::wstring(buffer, length) + L'\\' : std::wstring();
    std::wstring own;
    if (const DWORD n = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH); n > 0 && n < MAX_PATH)
        own = folderOf(std::wstring_view(buffer, n));
    std::string list;
    for (const HMODULE module : modules) {
        const DWORD n = ::GetModuleFileNameW(module, buffer, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            continue;
        const std::wstring_view path(buffer, n);
        if (under(path, windows) || under(path, own))
            continue;
        if (!list.empty())
            list += "; ";
        list += utf8(path);
    }
    return list;
}

} // namespace tabs

#include "Win32Util.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>

namespace ws::win32 {

namespace {

constexpr DWORD kAwayAfterMs = 2 * 60 * 1000;

} // namespace

bool userAway()
{
    // For testing (wsbench --service --content while typing): WINSHUN_USER=present or away.
    wchar_t forced[16] {};
    if (::GetEnvironmentVariableW(L"WINSHUN_USER", forced, 16) > 0)
        return std::wstring_view(forced) == L"away";
    LASTINPUTINFO input {sizeof input, 0};
    SYSTEM_POWER_STATUS power {};
    QUERY_USER_NOTIFICATION_STATE state {};
    return ::GetLastInputInfo(&input) && ::GetTickCount() - input.dwTime >= kAwayAfterMs
        && ::GetSystemPowerStatus(&power) && power.ACLineStatus == 1
        && SUCCEEDED(::SHQueryUserNotificationState(&state))
        && (state == QUNS_NOT_PRESENT || state == QUNS_ACCEPTS_NOTIFICATIONS || state == QUNS_QUIET_TIME);
}

std::wstring longPath(std::wstring_view path)
{
    if (path.starts_with(L"\\\\?\\"))
        return std::wstring(path);
    if (path.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + std::wstring(path.substr(2));
    return L"\\\\?\\" + std::wstring(path);
}

std::wstring expandEnvironment(std::wstring_view text)
{
    const std::wstring input(text);
    DWORD needed = ::ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (needed == 0)
        return input;
    std::wstring out(needed, L'\0');
    needed = ::ExpandEnvironmentStringsW(input.c_str(), out.data(), needed);
    if (needed == 0)
        return input;
    out.resize(needed - 1); // drop the terminator
    return out;
}

std::wstring knownFolder(const GUID& id)
{
    PWSTR path = nullptr;
    std::wstring folder;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path)) && path)
        folder = path;
    ::CoTaskMemFree(path);
    return folder;
}

std::vector<std::wstring> userFolders()
{
    std::vector<std::wstring> folders;
    const auto add = [&](std::wstring path) {
        if (!path.empty() && std::ranges::none_of(folders, [&](const std::wstring& existing) { return equalsIgnoreCase(existing, path); }))
            folders.push_back(std::move(path));
    };
    // Keep an explicitly supplied profile root useful to existing callers.
    const std::wstring profile = expandEnvironment(L"%USERPROFILE%");
    if (profile != L"%USERPROFILE%")
        add(profile);
    for (const GUID& id : {FOLDERID_Profile, FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads,
             FOLDERID_Pictures, FOLDERID_Music, FOLDERID_Videos})
        add(knownFolder(id));
    return folders;
}

} // namespace ws::win32

#include "Integrity.h"

#include "Win32Util.h"

#include <optional>

namespace ws::integrity {

namespace {

// The token's mandatory integrity level (SECURITY_MANDATORY_*_RID).
std::optional<DWORD> levelOf(HANDLE token)
{
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[64];
    DWORD size = 0;
    if (!::GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof buffer, &size))
        return std::nullopt;
    const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
    const UCHAR count = *::GetSidSubAuthorityCount(sid);
    if (count == 0)
        return std::nullopt;
    return *::GetSidSubAuthority(sid, count - 1);
}

DWORD ownLevel()
{
    static const DWORD level = [] {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            return DWORD(SECURITY_MANDATORY_MEDIUM_RID);
        const win32::UniqueHandle owned(token);
        return levelOf(token).value_or(DWORD(SECURITY_MANDATORY_MEDIUM_RID));
    }();
    return level;
}

} // namespace

bool processElevated()
{
    static const bool elevated = [] {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        const win32::UniqueHandle owned(token);
        TOKEN_ELEVATION elevation {};
        DWORD size = 0;
        return ::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size)
            && elevation.TokenIsElevated != 0;
    }();
    return elevated;
}

bool higherThanOurs(HWND window)
{
    if (ownLevel() >= SECURITY_MANDATORY_HIGH_RID || !window)
        return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid == 0)
        return true;
    // Allowed on elevated processes too (measured 2026-10-14, pitfalls).
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE token = nullptr;
    if (!process.valid() || !::OpenProcessToken(process.get(), TOKEN_QUERY, &token))
        return true;
    const win32::UniqueHandle owned(token);
    const std::optional<DWORD> level = levelOf(token);
    return !level || *level > ownLevel();
}

bool lowerThanOurs(HWND window)
{
    DWORD pid = 0;
    if (!window || !::GetWindowThreadProcessId(window, &pid) || pid == 0)
        return false;
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE token = nullptr;
    if (!process.valid() || !::OpenProcessToken(process.get(), TOKEN_QUERY, &token))
        return false;
    const win32::UniqueHandle owned(token);
    const std::optional<DWORD> level = levelOf(token);
    return level && *level < ownLevel();
}

std::optional<DWORD> levelOf(HWND window)
{
    DWORD pid = 0;
    if (!window || !::GetWindowThreadProcessId(window, &pid) || pid == 0)
        return std::nullopt;
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE token = nullptr;
    if (!process.valid() || !::OpenProcessToken(process.get(), TOKEN_QUERY, &token))
        return std::nullopt;
    const win32::UniqueHandle owned(token);
    return levelOf(token);
}

} // namespace ws::integrity

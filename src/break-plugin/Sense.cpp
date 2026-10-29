#include "Sense.h"

#include <windows.h>
#include <powerbase.h>
#include <shellapi.h>

#include <string_view>

namespace brk {

std::int64_t idleMs()
{
    // For trying it out unattended (as src/core's WINSHUN_USER): "present"
    // is someone at the PC who is not typing right now.
    static const bool present = [] {
        wchar_t user[16] {};
        return ::GetEnvironmentVariableW(L"WINSHUN_USER", user, 16) > 0 && std::wstring_view(user) == L"present";
    }();
    if (present)
        return 2'000;
    if (displayKeptOn())
        return 0;
    LASTINPUTINFO input {sizeof input, 0};
    if (!::GetLastInputInfo(&input))
        return 0;
    // Both in GetTickCount's 32-bit ms, so the difference survives its wrap
    // after 49.7 days (a 64-bit tick minus dwTime would not).
    return static_cast<DWORD>(::GetTickCount() - input.dwTime);
}

bool displayKeptOn()
{
    EXECUTION_STATE state = 0;
    return ::CallNtPowerInformation(SystemExecutionState, nullptr, 0, &state, sizeof state) == 0
        && (state & ES_DISPLAY_REQUIRED);
}

int notificationState()
{
    QUERY_USER_NOTIFICATION_STATE state {};
    return SUCCEEDED(::SHQueryUserNotificationState(&state)) ? static_cast<int>(state) : -1;
}

int focusAssist()
{
    // Undocumented but long stable (Windows 10 1803 to 11; Stretchly reads
    // it the same way, through the windows-focus-assist package): the WNF
    // state the shell publishes when Focus assist / Do not disturb changes.
    using QueryWnf = LONG(NTAPI*)(const unsigned long long*, const void*, const void*, ULONG*, void*, ULONG*);
    static const auto query = reinterpret_cast<QueryWnf>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryWnfStateData")));
    if (!query)
        return -1;
    constexpr unsigned long long kQuietHoursProfile = 0x0D83063EA3BF1C75ull; // WNF_SHEL_QUIETHOURS_ACTIVE_PROFILE_CHANGED
    ULONG stamp = 0;
    DWORD profile = 0;
    ULONG size = sizeof profile;
    if (query(&kQuietHoursProfile, nullptr, nullptr, &stamp, &profile, &size) < 0 || size != sizeof profile)
        return -1;
    return profile <= 2 ? static_cast<int>(profile) : -1;
}

bool quietTime()
{
    switch (notificationState()) {
    case QUNS_BUSY: // a full-screen program
    case QUNS_RUNNING_D3D_FULL_SCREEN:
    case QUNS_PRESENTATION_MODE:
        return true;
    default:
        break;
    }
    const int focus = focusAssist();
    return focus == 1 || focus == 2;
}

} // namespace brk

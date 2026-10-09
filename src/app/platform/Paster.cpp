#include "Paster.h"

#include <array>
#include <cwchar>

namespace ws::paste {

namespace {

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

INPUT key(WORD vk, bool up, bool extended = false)
{
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)); // for programs that read scan codes
    input.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    return input;
}

} // namespace

bool modifiersDown()
{
    return held(VK_SHIFT) || held(VK_CONTROL) || held(VK_MENU) || held(VK_LWIN) || held(VK_RWIN);
}

HWND usableTarget(HWND window)
{
    if (!window || !::IsWindow(window))
        return nullptr;
    const HWND root = ::GetAncestor(window, GA_ROOT);
    if (!root || !::IsWindowVisible(root))
        return nullptr;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(root, &pid);
    if (pid == ::GetCurrentProcessId())
        return nullptr;
    // The taskbar (Win顺 opened from the tray icon) takes no paste.
    wchar_t className[64] {};
    ::GetClassNameW(root, className, static_cast<int>(std::size(className)));
    if (std::wcscmp(className, L"Shell_TrayWnd") == 0 || std::wcscmp(className, L"Shell_SecondaryTrayWnd") == 0)
        return nullptr;
    return root;
}

void activate(HWND target)
{
    if (::IsIconic(target))
        ::ShowWindow(target, SW_RESTORE);
    ::SetForegroundWindow(target);
}

bool isForeground(HWND target)
{
    const HWND foreground = ::GetForegroundWindow();
    return foreground && ::GetAncestor(foreground, GA_ROOT) == target;
}

bool sendPasteKeys(HWND target)
{
    wchar_t className[64] {};
    ::GetClassNameW(target, className, static_cast<int>(std::size(className)));
    const bool shiftInsert = std::wcscmp(className, L"mintty") == 0 || std::wcscmp(className, L"PuTTY") == 0;
    const std::array<INPUT, 4> keys = shiftInsert
        ? std::array {key(VK_SHIFT, false), key(VK_INSERT, false, true), key(VK_INSERT, true, true), key(VK_SHIFT, true)}
        : std::array {key(VK_CONTROL, false), key('V', false), key('V', true), key(VK_CONTROL, true)};
    return ::SendInput(static_cast<UINT>(keys.size()), const_cast<INPUT*>(keys.data()), sizeof(INPUT)) == keys.size();
}

} // namespace ws::paste

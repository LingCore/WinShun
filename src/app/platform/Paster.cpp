#include "Paster.h"

#include "KeyRouter.h"

#include <cstdint>
#include <cwchar>
#include <vector>

namespace ws::paste {

namespace {

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

INPUT key(const pastekeys::Key& key)
{
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key.vk;
    // For programs that read scan codes; 0xE0 above it: an extended key.
    const UINT scan = ::MapVirtualKeyW(key.vk, MAPVK_VK_TO_VSC_EX);
    input.ki.wScan = static_cast<WORD>(scan & 0xFF);
    const bool extended = key.extended || (scan & 0xFF00) != 0;
    input.ki.dwFlags = (key.up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    // Past KeyRouter, should it still be taking keys (pastekeys::Key::mask).
    input.ki.dwExtraInfo = key.mask ? kOwnMask : kOwnInput;
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

bool sendPasteKeys(HWND target, Guard* guard)
{
    wchar_t className[64] {};
    ::GetClassNameW(target, className, static_cast<int>(std::size(className)));
    const pastekeys::Keys keys = std::wcscmp(className, L"mintty") == 0 || std::wcscmp(className, L"PuTTY") == 0
        ? pastekeys::Keys::ShiftInsert
        : pastekeys::Keys::CtrlV;
    // The modifiers right before the keys go: pressed again meanwhile, the
    // user keeps them (pastekeys::sequence). A press still on its way through
    // the low-level hooks is not in the key state yet: only the guard's hook
    // sees it come, in order with the keys.
    unsigned sides = 0;
    for (const std::uint16_t side : pastekeys::kSides) {
        if (held(side))
            sides |= pastekeys::sideBit(side);
    }
    std::vector<pastekeys::Key> sequence = pastekeys::sequence(keys, sides, guard != nullptr);
    const bool watched = guard && guard->watch(sequence);
    if (guard && !watched)
        sequence = pastekeys::sequence(keys, sides, false); // its hook gone already
    std::vector<INPUT> inputs;
    inputs.reserve(sequence.size());
    for (const pastekeys::Key& k : sequence)
        inputs.push_back(key(k));
    const auto count = static_cast<UINT>(inputs.size());
    const UINT sent = ::SendInput(count, inputs.data(), sizeof(INPUT));
    if (watched && sent < count)
        guard->went(sent);
    return sent == count;
}

} // namespace ws::paste

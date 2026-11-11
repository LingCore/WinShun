#include "Paster.h"

#include "KeyRouter.h"
#include "PasteInput.h"

#include <cstdint>
#include <cwchar>
#include <string_view>
#include <vector>

namespace ws::paste {

namespace {

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
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

Result sendPasteKeys(HWND target, Guard* guard, InputSender send)
{
    if (!target || !isForeground(target))
        return {.error = ERROR_INVALID_WINDOW_HANDLE};
    wchar_t className[64] {};
    ::GetClassNameW(target, className, static_cast<int>(std::size(className)));
    const pastekeys::Keys keys = pastekeys::forWindowClass(className);
    // The modifiers right before the keys go: pressed again meanwhile, the
    // user keeps them (pastekeys::sequence). A press still on its way through
    // the low-level hooks is not in the key state yet: only the guard's hook
    // sees it come, in order with the keys.
    const unsigned sides = pastekeys::heldSides();
    std::vector<pastekeys::Key> sequence = pastekeys::sequence(keys, sides, guard != nullptr);
    const bool watched = guard && guard->watch(sequence);
    if (guard && !watched)
        sequence = pastekeys::sequence(keys, sides, false); // its hook gone already
    std::vector<INPUT> inputs = pastekeys::inputs(sequence);
    const auto count = static_cast<UINT>(inputs.size());
    ::SetLastError(ERROR_SUCCESS);
    const UINT sent = send(count, inputs.data(), sizeof(INPUT));
    const DWORD error = sent == count ? ERROR_SUCCESS : ::GetLastError();
    if (watched && sent < count)
        guard->went(sent);
    Result result {.method = sent == count ? Result::Method::Keys : Result::Method::None,
        .sent = sent, .requested = count, .error = error};
    // A partial send may already have pasted: never replay it. If Windows
    // refused every key, standard Edit/RichEdit controls support a targeted
    // paste without changing the clipboard formats, caret or selection.
    // Other applications keep their own Ctrl+V semantics (no fake key messages).
    if (sent != 0 || !isForeground(target))
        return result;
    GUITHREADINFO info {sizeof info};
    if (!::GetGUIThreadInfo(::GetWindowThreadProcessId(target, nullptr), &info)
        || !info.hwndFocus || ::GetAncestor(info.hwndFocus, GA_ROOT) != target
        || !::IsWindowEnabled(info.hwndFocus))
        return result;
    ::GetClassNameW(info.hwndFocus, className, static_cast<int>(std::size(className)));
    const std::wstring_view control(className);
    if (control != L"Edit" && control != L"RichEdit20W" && control != L"RichEdit20A" && control != L"RICHEDIT50W")
        return result;
    if (::GetWindowLongPtrW(info.hwndFocus, GWL_STYLE) & ES_READONLY)
        return result;
    DWORD_PTR ignored = 0;
    if (isForeground(target) && ::SendMessageTimeoutW(info.hwndFocus, WM_PASTE, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &ignored))
        result.method = Result::Method::EditMessage;
    return result;
}

} // namespace ws::paste

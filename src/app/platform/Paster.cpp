#include "Paster.h"

#include "KeyRouter.h"
#include "OwnInput.h"
#include "PasteInput.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <string_view>
#include <vector>

namespace ws::paste {

namespace {

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

// The window reactivate() gives the front to for a moment: 1×1, off every
// screen, a tool window (no taskbar button). Made on first use, on the
// thread that pastes (the GUI thread), and kept.
HWND standIn()
{
    static const HWND window = [] {
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW type {.cbSize = sizeof(WNDCLASSEXW), .lpfnWndProc = ::DefWindowProcW, .hInstance = instance,
            .lpszClassName = L"WinShun.PasteStandIn"};
        ::RegisterClassExW(&type);
        return ::CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"", WS_POPUP, -32000, -32000, 1, 1,
            nullptr, nullptr, instance, nullptr);
    }();
    return window && ::IsWindow(window) ? window : nullptr;
}

// The front for a window of this thread, as win::bringToFront gets it:
// directly when Windows lets this process (it got the latest input), else
// after an input that does nothing (a mouse event without a move), which
// makes this process the one that sent the latest input.
bool takeFront(HWND window)
{
    const auto tryIt = [window] {
        ::SetForegroundWindow(window);
        return ::GetForegroundWindow() == window;
    };
    const auto sendNothing = [] {
        INPUT nothing {};
        nothing.type = INPUT_MOUSE;
        nothing.mi.dwExtraInfo = kOwnInput;
        ::SendInput(1, &nothing, sizeof nothing);
    };
    if (::AllowSetForegroundWindow(::GetCurrentProcessId()) && tryIt())
        return true;
    sendNothing();
    return tryIt();
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

bool readsClipboardOnActivation(std::wstring_view program) noexcept
{
    static constexpr std::array<std::wstring_view, 2> kPrograms {L"Photoshop.exe", L"Illustrator.exe"};
    return std::ranges::any_of(kPrograms, [program](std::wstring_view known) {
        return known.size() == program.size()
            && ::CompareStringOrdinal(known.data(), static_cast<int>(known.size()), program.data(),
                   static_cast<int>(program.size()), TRUE)
            == CSTR_EQUAL;
    });
}

bool readsClipboardOnActivation(HWND target)
{
    DWORD pid = 0;
    if (!target || !::GetWindowThreadProcessId(target, &pid) || !pid)
        return false;
    const std::unique_ptr<void, decltype(&::CloseHandle)> process(
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid), &::CloseHandle);
    if (!process)
        return false;
    std::array<wchar_t, 1024> path {};
    auto size = static_cast<DWORD>(path.size());
    if (!::QueryFullProcessImageNameW(process.get(), 0, path.data(), &size))
        return false;
    const std::wstring_view full(path.data(), size);
    const std::size_t slash = full.find_last_of(L"\\/");
    return readsClipboardOnActivation(slash == std::wstring_view::npos ? full : full.substr(slash + 1));
}

bool reactivate(HWND target)
{
    const HWND window = standIn();
    if (!window || !target || !isForeground(target))
        return false;
    ::ShowWindow(window, SW_SHOWNOACTIVATE);
    const bool taken = takeFront(window);
    if (taken)
        activate(target); // in front now, Win顺 may give it away
    ::ShowWindow(window, SW_HIDE);
    return taken;
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

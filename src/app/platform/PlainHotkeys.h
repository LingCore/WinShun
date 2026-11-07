#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <vector>

// GitHub #243: a global hotkey with no modifier (F1, PrintScreen) that a
// normal-rights process registers does not fire while a window run as
// administrator is in front (UIPI; with Ctrl, Alt, Shift or Win it does,
// docs/pitfalls.md). Win顺's own hotkeys and those it holds for plugins are
// registered through here, so that the service's UIAccess listener can hold
// such keys back from that window and tell of them (keyproto::kHotkey);
// deliver() then hands the key to the window that registered it, as the
// WM_HOTKEY Windows would have posted. Everything on the GUI thread.
namespace ws::plainkeys {

// RegisterHotKey(window, id, modifiers, vk), noting a relayable key (no
// modifier; keyproto::relayable). Windows' error stays in GetLastError.
bool registerHotkey(HWND window, int id, UINT modifiers, UINT vk);
// UnregisterHotKey(window, id), forgetting what was noted for it.
void unregisterHotkey(HWND window, int id);
// Every window's: the window is going.
void unregisterAll(HWND window);

// The relayable keys registered now, sorted, each once.
std::vector<std::uint32_t> relayed();
// Posts WM_HOTKEY to the window holding `vk` alone, as Windows would.
// False when none does (registered no longer: the word came late).
bool deliver(std::uint32_t vk);
// Called (on the GUI thread) whenever relayed() changes. One listener.
void setChanged(std::function<void()> changed);

} // namespace ws::plainkeys

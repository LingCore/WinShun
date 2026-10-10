#pragma once

#include <windows.h>

#include <atomic>
#include <bitset>
#include <functional>
#include <thread>
#include <vector>

namespace ws {

// Ctrl+1…9 (or Alt+1…9) on Win顺's own search boxes, taken from the
// keyboard before anything else acts on them. Other programs often hold
// these as global hotkeys (PixPin takes Ctrl+1 and up), and Windows hands a
// global hotkey to its program before the window in front sees the key; a
// low-level keyboard hook comes before both, and one set later before the
// hooks set earlier (AutoHotkey's, Listary's).
//
// It lives only while one of `windows` is in front (the app makes it and
// lets it go as they are activated), on a thread of its own, and takes only
// the digit with exactly that modifier held: no Shift, no Win, not Ctrl and
// Alt together (AltGr types characters with them), not Alt with the number
// pad (Alt codes: Alt+0169 types ©), nor while an input method is
// composing. The rest, and every key while another window is in front,
// passes on untouched. A key taken is masked (vkE8, as KeyRouter does), so
// the modifier does not count as pressed alone: no double Ctrl, no menu on
// Alt's release.
class NumberKeys {
public:
    enum class Modifier { Ctrl, Alt };

    // `onNumber(window, n)` runs on the hook's thread: post to the GUI thread.
    NumberKeys(std::vector<HWND> windows, Modifier modifier, std::function<void(HWND, int)> onNumber);
    ~NumberKeys();

    NumberKeys(const NumberKeys&) = delete;
    NumberKeys& operator=(const NumberKeys&) = delete;

    bool isActive() const noexcept { return m_active.load(); }

    // The digits 1…9 whose chord with `modifier` is a global hotkey already
    // (RegisterHotKey refuses it: another program's, or one of Win顺's own),
    // for the settings to name. Hooks other programs keep (AutoHotkey's) do
    // not show. GUI thread.
    static std::vector<int> heldAsHotkeys(Modifier modifier);
    // The text field in front has an input method's composition: its keys
    // are the input method's.
    void setComposing(bool composing) noexcept { m_composing.store(composing); }

private:
    static LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam);
    bool handle(WPARAM message, const KBDLLHOOKSTRUCT& event); // true: taken
    int numberOf(UINT vk) const noexcept; // 1…9, or 0

    const std::vector<HWND> m_windows;
    const Modifier m_modifier;
    std::function<void(HWND, int)> m_onNumber;
    std::bitset<256> m_taken; // keys held whose press was taken; only touched on the hook's thread
    std::atomic<bool> m_composing {false};
    std::atomic<DWORD> m_threadId {0};
    std::atomic<bool> m_active {false};
    std::jthread m_thread;
};

} // namespace ws

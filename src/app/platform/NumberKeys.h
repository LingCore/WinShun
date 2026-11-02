#pragma once

#include <qnamespace.h>

#include <windows.h>

#include <atomic>
#include <bitset>
#include <functional>
#include <thread>
#include <vector>

namespace ws::numberkeys {

// Ctrl+1…9 (or Alt+1…9) on Win顺's own search boxes reach them as the
// window's own keys. A digit another program holds as a global hotkey
// (PixPin takes Ctrl+1 and up) would go to that program instead: Windows
// hands a global hotkey to its program before the window in front sees the
// key. While such a window is in front, those digits are taken first
// (Hook); Win顺's own shortcuts among them stay what they are.

enum class Modifier { Ctrl, Alt };

// The digits 1…9 whose chord with `modifier` is a global hotkey already
// (RegisterHotKey refuses it: another program's, or one of Win顺's own),
// for the settings to name. Hooks other programs keep (AutoHotkey's) do
// not show. GUI thread.
std::vector<int> heldAsHotkeys(Modifier modifier);
// The same as bits: 1 << n for each digit n held.
unsigned heldMask(Modifier modifier);

// Which digit the key pressed is for the hook: `vk` with `modifier` held
// (and Shift or not), no other of Ctrl, Alt and Win, and its bit in
// `digits`; else 0. `held`: the modifiers down as it is pressed, Win as
// Qt::MetaModifier.
constexpr int takenDigit(unsigned vk, Qt::KeyboardModifiers held, Modifier modifier, unsigned digits) noexcept
{
    if (vk < '1' || vk > '9' || !(digits & (1u << (vk - '0'))))
        return 0;
    const Qt::KeyboardModifier wanted = modifier == Modifier::Ctrl ? Qt::ControlModifier : Qt::AltModifier;
    return (held & ~Qt::ShiftModifier) == Qt::KeyboardModifiers(wanted) ? static_cast<int>(vk - '0') : 0;
}

// The number keys of a window of Win顺's in front taken from the keyboard,
// before other programs' global hotkeys (GitHub #175): a low-level keyboard
// hook, which every key press in the system waits on, so it lives only while
// such a window is in front and another program holds one of its digits
// (App::updateNumberKeys), on a thread of its own, and decides at once from
// the key alone. It takes `digits` with the modifier (Shift too), only while
// a window of this process is in front; the rest goes on. A key it wants
// goes to the hooks after it first, as KeyRouter does: a remapper there
// (AutoHotkey) keeps what it takes. The modifier is then not pressed alone
// (a mask key goes in place of the digit): no menu bar for Alt, no double
// Ctrl.
//
// While a process takes the keyboard's Raw Input, Windows never calls its
// low-level keyboard hook first in line with one of its windows in front
// (docs/pitfalls.md): KeyListener's is set aside for as long as the hook
// lives, and the keys it would have heard are handed to it from here
// (KeyListener::hookedKey), so that a double Ctrl still closes the launcher.
class Hook {
public:
    struct Key {
        int digit = 0;
        Qt::KeyboardModifiers modifiers; // held with it
        bool press = true;
        bool autoRepeat = false;
        quint32 vk = 0;
        quint32 scanCode = 0;
    };

    // `onKey` runs on the hook's thread: post to the GUI thread from it.
    Hook(Modifier modifier, unsigned digits, std::function<void(const Key&)> onKey);
    ~Hook();

    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;

    bool isActive() const noexcept { return m_active.load(); }
    Modifier modifier() const noexcept { return m_modifier; }
    unsigned digits() const noexcept { return m_digits; }

private:
    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    LRESULT handle(int code, WPARAM message, LPARAM lParam);

    const Modifier m_modifier;
    const unsigned m_digits;
    const DWORD m_process;
    std::function<void(const Key&)> m_onKey;
    std::bitset<256> m_taken; // digits held whose press was taken; hook thread only
    std::atomic<DWORD> m_threadId {0};
    std::atomic<bool> m_active {false};
    std::jthread m_thread;
};

} // namespace ws::numberkeys

#pragma once

#include <windows.h>
#include <imm.h>

#include <array>
#include <functional>
#include <optional>
#include <span>

namespace ws::win {

// The input method's state in a window of ours, as IMM32 keeps it in the
// window's input context and TSF keeps it in the thread manager's compartments.
// Qt gives a focused text field's window the thread's default context
// (IACE_DEFAULT) and takes it away otherwise. Windows bridges IMM32 calls to
// TSF input methods, except during a cold first-focus activation after boot
// when a TSF text service initializes asynchronously and can push Chinese mode
// after the initial synchronous switch.
struct InputMode {
    bool open = false;
    DWORD conversion = 0;
    DWORD sentence = 0;

    bool operator==(const InputMode&) const = default;
};

// Its English mode, the rest of the conversion mode kept. Both said: on
// this machine a lone Shift flips WeChat's open status (1 -> 0); Microsoft
// Pinyin is said to flip the native bit instead. Set every time, never
// judged from the state first: WeChat's native bit says nothing (switched
// back to Chinese by Shift, it reads open with no native bit).
inline InputMode lettersMode(InputMode mode)
{
    mode.conversion &= ~static_cast<DWORD>(IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE);
    mode.open = false;
    return mode;
}

// Still as the switch left it, as far as letters go: input methods change
// other bits on their own (symbols, roman, the sentence mode), which say
// nothing of the user having switched back.
inline bool sameLetters(const InputMode& a, const InputMode& b)
{
    constexpr DWORD kBits = IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE;
    return a.open == b.open && (a.conversion & kBits) == (b.conversion & kBits);
}

// Whether `mode` is currently outside English/half-width letters (IME open, or
// native / full-width bit still set).
inline bool needsLettersSwitch(const InputMode& mode)
{
    constexpr DWORD kBits = IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE;
    return mode.open || (mode.conversion & kBits) != 0;
}

// Combines an IMM32 mode reading with an optional TSF compartment reading so a
// Chinese state present in either layer is never overlooked during cold-start
// synchronization.
inline InputMode combineInputMode(InputMode imm, std::optional<bool> tsfOpen, std::optional<DWORD> tsfConversion)
{
    constexpr DWORD kBits = IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE;
    if (tsfOpen.value_or(false))
        imm.open = true;
    if (tsfConversion)
        imm.conversion |= (*tsfConversion & kBits);
    return imm;
}

struct LettersSwitch {
    HIMC context = nullptr; // the window's (the thread's default); none: nothing could be said
    bool changed = false; // setting it changed what it was (else nothing to put back)
    InputMode before;
    InputMode after; // read back after setting: what the input method made of it
    std::optional<DWORD> tsfBeforeConversion; // raw TSF conversion compartment before switching, if present
};

// Merges a subsequent switch during the same opening (e.g. after a cold-boot
// TSF text service asynchronously pushed Chinese mode) into the recorded switch
// so hiding still restores the user's true original state.
inline LettersSwitch mergeLettersSwitch(const LettersSwitch& previous, const LettersSwitch& latest)
{
    if (!latest.context)
        return previous;
    if (!previous.changed)
        return latest;
    LettersSwitch merged = latest;
    merged.before = previous.before;
    if (previous.tsfBeforeConversion)
        merged.tsfBeforeConversion = previous.tsfBeforeConversion;
    merged.changed = true;
    return merged;
}

// Bounded post-open settle checkpoints (ms between consecutive checks) while no
// user key has arrived yet. The very first activation of a process after boot
// gets a longer window to catch slow cold TSF TIP activation.
inline constexpr std::array<int, 4> kFirstActivationLettersSettleMs {40, 110, 300, 150};
inline constexpr std::array<int, 2> kWarmLettersSettleMs {40, 120};
inline constexpr int kMaxLettersGuardRetries = 4;

inline std::span<const int> lettersSettleSchedule(bool firstActivation) noexcept
{
    return firstActivation ? std::span<const int>(kFirstActivationLettersSettleMs)
                           : std::span<const int>(kWarmLettersSettleMs);
}

// Puts the window's input method into its English mode (both IMM32 and TSF
// compartments when available; setting it again when already English changes
// nothing). Only this window's context on this thread: never the keyboard
// layout (that would reorder the user's language list), nor another program's
// input method.
LettersSwitch switchToLetters(HWND window);

// Whether the window's IMM32 context or TSF thread compartments currently sit
// outside English/half-width letters.
bool inputModeNeedsLetters(HWND window);

// Back to how it was before the switch, unless the user changed it since.
void restoreInputMode(const LettersSwitch& done);

// Watches a launcher window for asynchronous IME context/state notifications
// (WM_IME_SETCONTEXT, WM_IME_NOTIFY open/conversion) and fresh user keyboard /
// layout changes (WM_KEYDOWN, WM_SYSKEYDOWN, WM_INPUTLANGCHANGE) so the caller
// can catch cold-boot TSF resets without ever undoing a manual Shift or
// Ctrl+Space switch inside the search box.
class InputModeWatch {
public:
    InputModeWatch(HWND window, std::function<void()> onImeEvent, std::function<void()> onUserInput);
    ~InputModeWatch();
    InputModeWatch(const InputModeWatch&) = delete;
    InputModeWatch& operator=(const InputModeWatch&) = delete;

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);

    HWND m_window = nullptr;
    std::function<void()> m_onImeEvent;
    std::function<void()> m_onUserInput;
};

} // namespace ws::win

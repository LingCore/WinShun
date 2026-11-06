#pragma once

#include <windows.h>
#include <imm.h>

namespace ws::win {

// The input method's state in a window of ours, as IMM32 keeps it in the
// window's input context. Qt gives a focused text field's window the
// thread's default context (IACE_DEFAULT) and takes it away otherwise.
// Windows hands IMM32 calls on to TSF input methods (their open/close and
// conversion compartments), so the Chinese ones of either kind see it.
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

struct LettersSwitch {
    HIMC context = nullptr; // the window's (the thread's default); none: nothing could be said
    bool changed = false; // setting it changed what it was (else nothing to put back)
    InputMode before;
    InputMode after; // read back after setting: what the input method made of it
};

// Puts the window's input method into its English mode (as it may be
// already: setting it again changes nothing). Only this window's context,
// on this thread: never the keyboard layout (that would reorder the user's language list), nor
// another program's input method.
LettersSwitch switchToLetters(HWND window);

// Back to how it was before the switch, unless the user changed it since.
void restoreInputMode(const LettersSwitch& done);

} // namespace ws::win

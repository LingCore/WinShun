#pragma once

#include <cstdint>

namespace ws {

// Marks the input Win顺 sends itself: SendInput's dwExtraInfo, which the
// hooks see as such and Raw Input as ExtraInformation. KeyRouter and
// TypeToSearch let it through; KeyListener does not take it for another
// program's.
inline constexpr std::uintptr_t kOwnInput = 0x5753'4B52; // "WSKR"

// KeyRouter's masking key sent just before a Shift or Alt pressed alone
// goes on to the program, so that no menu bar opens: for a double Alt, no
// key in between. The one it sends in place of a key it took (Alt+1) is
// kOwnInput, a key in between. TypeToSearch masks a Shift held over keys it
// holds back the same way (no Chinese/English switch of an input method).
inline constexpr std::uintptr_t kOwnMask = 0x5753'4B4D; // "WSKM"
inline constexpr std::uint16_t kMaskKey = 0xE8; // the masking key: unassigned, no program acts on it

constexpr bool isOwnInput(std::uintptr_t extra) noexcept
{
    return extra == kOwnInput || extra == kOwnMask;
}

} // namespace ws

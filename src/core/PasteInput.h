#pragma once

#include "OwnInput.h"
#include "PasteKeys.h"

#include <windows.h>

#include <span>
#include <vector>

// The paste keys (PasteKeys.h) as Windows takes them: for the UI's own paste
// (Paster) and the service's helper, which presses them in a window run as
// administrator (GitHub #277).
namespace ws::pastekeys {

// The sides of the modifiers down right now: sequence()'s `held`.
inline unsigned heldSides() noexcept
{
    unsigned sides = 0;
    for (const std::uint16_t side : kSides) {
        if (::GetAsyncKeyState(side) & 0x8000)
            sides |= sideBit(side);
    }
    return sides;
}

// `keys` for SendInput, marked as Win顺's own (OwnInput.h): its listeners
// pass over them.
inline std::vector<INPUT> inputs(std::span<const Key> keys)
{
    std::vector<INPUT> out;
    out.reserve(keys.size());
    for (const Key& key : keys) {
        INPUT input {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key.vk;
        // For programs that read scan codes; 0xE0 above it: an extended key.
        const UINT scan = ::MapVirtualKeyW(key.vk, MAPVK_VK_TO_VSC_EX);
        input.ki.wScan = static_cast<WORD>(scan & 0xFF);
        const bool extended = key.extended || (scan & 0xFF00) != 0;
        input.ki.dwFlags = (key.up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        // Past KeyRouter, should it still be taking keys (Key::mask).
        input.ki.dwExtraInfo = key.mask ? kOwnMask : kOwnInput;
        out.push_back(input);
    }
    return out;
}

} // namespace ws::pastekeys

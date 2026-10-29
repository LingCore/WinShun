#pragma once

#include "PasteKeys.h"

#include <windows.h>

#include <cstddef>
#include <span>

// Pasting into another program the way the user would: bring its window back
// to the front and press its paste keys there (SendInput). Elevated programs
// get them only while Win顺 is elevated too (integrity::higherThanOurs).
namespace ws::paste {

// Whether Shift, Ctrl, Alt or a Windows key is held down right now. A paste
// waits for them: with Shift still down, Ctrl+V would arrive as Ctrl+Shift+V.
bool modifiersDown();

// The top-level window of `window`, if it is still there, shown, and neither
// one of Win顺's nor the taskbar; else nullptr.
HWND usableTarget(HWND window);

// Makes `target` the foreground window. Works while Win顺 is in front.
void activate(HWND target);
bool isForeground(HWND target);

// A low-level hook of Win顺's own that the paste keys pass, in order with the
// user's keys (HoldWatch, the paste a hold ends with): the modifiers stay as
// the hand has them whatever it presses meanwhile (pastekeys::Guard).
class Guard {
public:
    virtual ~Guard() = default;
    // Right before `keys` go (the GUI thread). False: no hook to pass now.
    virtual bool watch(std::span<const pastekeys::Key> keys) = 0;
    // As they went: only the first `count` of them.
    virtual void went(std::size_t count) = 0;
};

// Presses the paste keys of `target`: Ctrl+V, or Shift+Insert in the
// terminals that do not take Ctrl+V (Git Bash's mintty, PuTTY); past `guard`,
// if there is one (pastekeys::sequence).
bool sendPasteKeys(HWND target, Guard* guard = nullptr);

} // namespace ws::paste

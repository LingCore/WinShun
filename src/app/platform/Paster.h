#pragma once

#include <windows.h>

// Pasting into another program the way the user would: bring its window back
// to the front and press its paste keys there (SendInput). Win顺 runs as
// administrator, so the keys reach elevated programs too.
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

// Presses the paste keys of `target`: Ctrl+V, or Shift+Insert in the
// terminals that do not take Ctrl+V (Git Bash's mintty, PuTTY).
bool sendPasteKeys(HWND target);

} // namespace ws::paste

#pragma once

#include <windows.h>

namespace ws::integrity {

// Whether Win顺 itself runs elevated (asked once). Elevated, it starts what
// the user opens with the user's normal rights through Explorer; not
// elevated, it starts them itself.
bool processElevated();

// Whether `window` belongs to a process of higher integrity than Win顺's (an
// elevated program while Win顺 is not). Windows (UIPI) then drops the keys
// Win顺 would send it (SendInput reports nothing), refuses its messages and
// AttachThreadInput, and while it is in front Win顺's hooks and Raw Input
// see no keys. Not asked when Win顺 is elevated itself: false. A process
// that cannot be asked counts as higher (GitHub #21, docs/pitfalls.md).
bool higherThanOurs(HWND window);

// Whether `window` belongs to a process of lower integrity than Win顺's: the
// desktop shell's while Win顺 is elevated, unless UAC is off (or this is the
// built-in Administrator), where Explorer is elevated too. False when it
// cannot be told.
bool lowerThanOurs(HWND window);

} // namespace ws::integrity

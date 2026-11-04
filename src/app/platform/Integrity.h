#pragma once

#include <windows.h>

#include <optional>

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

// The mandatory integrity level of `window`'s process
// (SECURITY_MANDATORY_*_RID: 0x2000 normal rights, 0x3000 run as
// administrator, 0x4000 system); none when it cannot be asked.
std::optional<DWORD> levelOf(HWND window);

// Whether `window`'s process runs elevated (TokenElevation): run as
// administrator. Not a UIAccess program at high integrity (Magnifier), nor
// one that cannot be asked.
bool elevated(HWND window);

} // namespace ws::integrity

#pragma once

#include <windows.h>

#include <optional>
#include <string>

// Win顺 in the place of Windows' search on the taskbar.
//
// The button on the taskbar is WinShunSearch.exe (src/stub), which the user
// pins there: Win顺 runs elevated, and a pinned program that needs elevation
// asks UAC on every click. A click starts it; it lets Win顺 take the
// foreground and sets the event Win顺 waits on, then exits (about 10 ms from
// the click: no process has to stay behind for it). Win顺 then opens over the
// taskbar at the button, as Windows' own search does; Win+S does too, once
// Explorer has given it up (winv::).
//
// Windows' own search button cannot be hidden for the user: on Windows 11
// (25H2) the User Choice Protection Driver keeps every program but Windows'
// own from writing SearchboxTaskbarMode, elevated or not. So the settings
// only say whether it is there, and open the taskbar settings, where it is
// turned off in one click.
namespace ws::taskbar {

// The event WinShunSearch.exe sets to show or hide the launcher (its source
// names it too). Made so that the user's own processes may set it with their
// normal rights; made by Win顺 the usual way, only elevated ones could.
inline constexpr wchar_t kEventName[] = L"Local\\WinShun.TaskbarSearch";
HANDLE createEvent(); // null if it could not be made

enum class Edge { Bottom, Top, Left, Right };

// Where a window opens over the taskbar, in physical pixels.
struct Spot {
    RECT taskbar {}; // the part of it on its monitor (auto-hidden: a sliver at the edge)
    Edge edge = Edge::Bottom; // of the monitor, the taskbar is at
    POINT anchor {}; // the button: along the taskbar, the window is centred on it
};

// `atPointer`: the button was just clicked, so the taskbar under the pointer,
// at the pointer. Otherwise (Win+S), or when the pointer is on no taskbar
// (started from the Start menu, say): the taskbar of the monitor under the
// pointer, where its icons are (centred, as Windows 11 has them by default:
// its middle). Nothing when that monitor has no taskbar.
std::optional<Spot> locate(bool atPointer);

// Windows' own search box or button is on the taskbar. Read only, see above.
bool windowsSearchShown();

// Programs cannot pin themselves (Windows gives that to the user alone), so
// the user pins a Start menu shortcut to `button` (WinShunSearch.exe): the
// installer's, else one made in the user's own Start menu, named `name`.
// Empty when there is none and none could be made. COM must be initialised.
std::wstring buttonShortcut(const std::wstring& button, const std::wstring& name);
// A shortcut to `button` is pinned to the taskbar. COM must be initialised.
bool buttonPinned(const std::wstring& button);

} // namespace ws::taskbar

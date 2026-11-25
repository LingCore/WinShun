// The windows winshun.topmost keeps on top (main.cpp): which windows count,
// the list of them, and the one switch there is, Windows' own "topmost" bit
// (WS_EX_TOPMOST). Nothing is kept beside the windows themselves: a window is
// on top when its bit says so, whoever set it.
#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace topmost {

// What decides whether a top-level window is one of the user's (as Alt+Tab
// lists them): plain facts, so tests/tst_topmost.cpp can go through them.
struct Facts {
    bool visible = false;
    bool cloaked = false; // on another virtual desktop, or a suspended app's
    bool owned = false; // has an owner (a dialog of another window)
    bool toolWindow = false; // WS_EX_TOOLWINDOW
    bool appWindow = false; // WS_EX_APPWINDOW: on the taskbar whatever else it is
    bool noActivate = false; // WS_EX_NOACTIVATE: never in front (an overlay)
    bool titled = false;
    bool shell = false; // the desktop or a taskbar
    bool ours = false; // a window of this plugin's
};
bool listed(const Facts& facts) noexcept;

struct Window {
    HWND hwnd = nullptr;
    unsigned long pid = 0;
    std::wstring title;
    std::wstring program; // "notepad.exe"; empty when Windows will not say
    std::wstring path; // its file, for the icon
    bool pinned = false;
};

Facts factsOf(HWND hwnd);
bool pinned(HWND hwnd);
// The user's windows, front to back.
std::vector<Window> windows();
// The window the keyboard is in: its top-level window, or the one that owns
// it (a dialog's program). None when that is no window of the user's (the
// desktop, the taskbar, nothing).
HWND current();
Window describe(HWND hwnd);

enum class Result {
    Done,
    Gone, // no such window any more
    Refused, // Windows will not let this program change it: it runs as administrator
    Hung, // its program does not answer
};
// On top, or no longer. Never waits for a program that does not answer.
Result setPinned(HWND hwnd, bool on);

// Whether `text` (in lower case) is in the title or the program's name.
bool matches(const Window& window, std::wstring_view lowered);
std::wstring lowered(std::wstring_view text);

} // namespace topmost

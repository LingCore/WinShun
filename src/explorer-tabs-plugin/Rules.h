// What to do with a folder window that just opened: the rules of the
// winshun.explorer-tabs plugin ("资源管理器标签页"), as plain C++ (no windows,
// no COM; only CompareStringOrdinal for Windows' case rules) so
// tests/tst_explorertabs.cpp can drive them with made-up windows.
//
// A new File Explorer window (B) becomes a tab of a window that is already
// open (A) -- or, when A already shows B's folder in a tab, that tab comes
// to the front -- unless the user wants a window: Ctrl+Shift held, a tab
// dragged out of a window, a folder tabs cannot host (Control Panel), a
// window above the plugin's integrity level, or no other window to go to.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tabs {

enum class Target {
    Recent, // the Explorer window used last (the top one in z-order)
    First, // the Explorer window opened first
};

struct Settings {
    bool merge = true; // new folder windows become tabs
    Target target = Target::Recent;
    bool reuse = true; // a folder already open in a tab: that tab, no second one
};

// A dev-only restriction (runtime.env of a test copy of the manifest): act
// only on folders under `root`. Nothing else is hidden, merged or used as A.
struct Restriction {
    std::wstring root; // empty: no restriction
    bool active() const { return !root.empty(); }
};

// The new window B, as far as it is known.
struct Candidate {
    std::wstring place; // parsing name: C:\a\b, \\server\share, ::{GUID}…; empty: not known
    int tabs = 1;
    bool bypass = false; // Ctrl+Shift held when it opened
    bool dragging = false; // the left button held when it opened: a tab dragged out
    bool movedTab = false; // its tab was a tab of another window before
    bool aboveUs = false; // its process runs above the plugin's integrity level
};

// Another Explorer window, a possible A.
struct Window {
    std::uintptr_t id = 0;
    int zOrder = 0; // 0 is the window in front
    int openOrder = 0; // 0 is the window opened first
    bool usable = true; // shown, on this virtual desktop, not one we are hiding
    bool aboveUs = false;
    std::vector<std::wstring> tabs; // the places its tabs show, the active tab first; "" not known
};

// A tab that closed lately (a tab dragged out closes in its window, and a new
// window with its folder opens).
struct Closed {
    std::wstring place;
    std::int64_t atMs = 0;
};

constexpr std::int64_t kDraggedOutMs = 2000;

enum class Verdict {
    Keep, // B stays a window (shown again if it was hidden)
    NewTab, // a new tab in `window`, B closes
    SwitchTab, // `window`'s tab `tab` shows the folder: to the front, B closes
};

enum class Why {
    Merged,
    Off,
    Paused,
    Bypass,
    Dragging,
    MovedTab,
    DraggedOut,
    AboveUs,
    Unknown, // no place known for B
    Special, // a folder tabs cannot host
    MultiTab,
    NoWindow, // no other usable Explorer window
    OutsideTestRoot,
    Reused,
};

struct Decision {
    Verdict verdict = Verdict::Keep;
    Why why = Why::Unknown;
    std::uintptr_t window = 0;
    int tab = -1; // for SwitchTab: the index in Window::tabs
};

// The path written one way: '/' as '\', no trailing '\' (but "C:\"), no
// "file:" URL prefix, no enclosing quotes.
std::wstring normalized(std::wstring_view place);
// The same place (case-insensitive, after normalized()).
bool samePlace(std::wstring_view a, std::wstring_view b);
// `place` is `root` or inside it.
bool isUnder(std::wstring_view place, std::wstring_view root);
// Control Panel and its kin open in a window of their own: a tab cannot
// show them (Explorer turns such a tab into the classic Control Panel).
bool tabsCannotHost(std::wstring_view place);

// At SHOW, before B's folder is known: whether to hide B at once (the
// decision comes at its registration). `otherWindows`: other usable
// Explorer windows; `namedForTest`: B's title names the test folder
// (restriction only: nothing else may be hidden).
bool hideEarly(const Settings& s, bool paused, const Restriction& r, const Candidate& b, int otherWindows, bool namedForTest);

Decision decide(const Settings& s, bool paused, const Restriction& r, const Candidate& b, const std::vector<Window>& others,
    const std::vector<Closed>& closed, std::int64_t nowMs);

const char* whyName(Why why);

} // namespace tabs

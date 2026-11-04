// What to do with a folder window that just opened: the rules of the
// winshun.explorer-tabs plugin ("资源管理器标签页"), as plain C++ (no windows,
// no COM; only CompareStringOrdinal for Windows' case rules) so
// tests/tst_explorertabs.cpp can drive them with made-up windows.
//
// A new File Explorer window (B) becomes a tab of a window that is already
// open (A) -- or, when A already shows B's folder in a tab, that tab comes
// to the front -- unless the user wants a window: Ctrl+Shift held, opened
// from File Explorer itself (Ctrl+N, "open in new window", a tab dragged
// out) or from a program listed in the settings, a folder tabs cannot host
// (Control Panel), a window above the plugin's integrity level, or no other
// window to go to.
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
    // Programs (exe names, lower case) whose folder windows stay windows:
    // scripts and terminals that ask for one (`explorer /n`).
    std::vector<std::wstring> windowsFor;
};

// "a.exe; B.EXE, c" -> {"a.exe", "b.exe", "c.exe"}: separated by ; , or
// line breaks, trimmed, lower case, ".exe" added when there is no dot.
std::vector<std::wstring> programList(std::wstring_view text);

// File Explorer has tabs from Windows 11 22H2 (build 22621). Before that (Windows
// 10, Windows 11 21H2) every window is one ShellTabWindowClass and the new-tab
// command does nothing, so the plugin must not hide a window to wait for a tab.
// 0 (build unknown): assume yes.
constexpr unsigned kFirstBuildWithTabs = 22621;
bool explorerHasTabs(unsigned buildNumber);

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
    bool requested = false; // a window was asked for (openerWantsWindow)
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
    Requested, // opened from File Explorer (Ctrl+N, "open in new window", a tab dragged out) or a listed program
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

// The window in front when B opened (the last one in front that is neither
// B nor a window the plugin hid): who asked for B.
struct Opener {
    std::wstring windowClass; // "" none
    std::wstring exe; // its program's file name (or path), any case
    bool broughtByUs = false; // put in front by the plugin, no input from the user since
    bool testWindow = false; // (restricted) its title names the test folder
};

// File Explorer itself asked for a window (Ctrl+N, "open in new window", a
// tab dragged out: the window in front is an Explorer window the user is
// working in), or a program listed in windowsFor did. The desktop, Win顺,
// the Start menu and other programs: B may become a tab. Restricted, an
// Explorer window counts only when it is a test window.
bool openerWantsWindow(const Opener& o, const Settings& s, const Restriction& r);

// Where a tab for a window not decided yet would most likely go (decide's
// order without B's folder): the `id` of the first usable candidate not
// above us, 0 if none. Each Window's `tabs` are not looked at.
std::uintptr_t guessTarget(const Settings& s, const std::vector<Window>& candidates);

// The tab opened on a guess is kept (and navigated) only when the decision
// is a new tab in that very window; else it is closed again.
bool confirmsGuess(const Decision& d, std::uintptr_t guessed);

} // namespace tabs

// The open File Explorer windows and their tabs, kept on disk so they can be
// opened again after the next logon (GitHub #273): what is written, how it
// is read back, and what has to be opened given what is open already. Plain
// C++ (no windows, no COM) so tests/tst_explorertabs.cpp can drive it; the
// thread that looks at Explorer and opens the windows is Keeper.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tabs {

struct SavedWindow {
    // Where the window is when not maximized (GetWindowPlacement's normal
    // position); all zero: not known, Explorer picks.
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool maximized = false;
    int active = 0; // the tab in front: an index in `tabs`, -1 leave as it comes
    std::vector<std::wstring> tabs; // parsing names (C:\a, ::{GUID}), as the tab strip has them

    bool placed() const { return right > left && bottom > top; }
    bool operator==(const SavedWindow&) const = default;
};

struct Session {
    // Windows were open when this was last written. False: the user closed
    // the last one, and `windows` is what it was (nothing comes back by itself).
    bool open = false;
    std::vector<SavedWindow> windows; // the windows open last
    std::vector<SavedWindow> previous; // what the logon before this one left

    bool operator==(const Session&) const = default;
};

// UTF-8 text, a line per window and per tab. parseSession() reads whatever
// is well formed and leaves out the rest (a file cut short, a newer version).
std::string serialize(const Session& session);
Session parseSession(std::string_view text);

// `stored` after a look at Explorer: `now` is what is open. With nothing
// open the last windows are kept and only `open` goes.
Session noted(Session stored, std::vector<SavedWindow> now);

// The first start after a logon: what the logon before left becomes
// `previous`, and stays that while this logon's windows are written.
Session newLogon(Session stored);

int tabCount(const std::vector<SavedWindow>& windows);

// An Explorer window open now.
struct OpenWindow {
    std::uintptr_t id = 0;
    std::vector<std::wstring> tabs; // "" not known
};

// What to open to have `saved` back.
struct Reopen {
    std::uintptr_t into = 0; // an open window that gets the tabs; 0: a new window
    SavedWindow window; // the tabs to open (never empty)
};

// Tabs open already (in any window) are left out, and so are places a tab
// cannot show (Control Panel). A saved window with a tab still open gets its
// other tabs added to the window holding that one (Windows' own "restore
// previous folder windows" brings back one tab per window); its place and
// the tab in front are then left alone.
std::vector<Reopen> planRestore(const std::vector<SavedWindow>& saved, const std::vector<OpenWindow>& open);

} // namespace tabs

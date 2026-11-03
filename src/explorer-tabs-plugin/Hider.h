// The windows winshun.explorer-tabs hid (layered, alpha 0, no taskbar
// button) and the journal that outlives the process (<dataDir>\hidden.txt).
// What is done to a window comes in as functions, so tests/tst_explorertabs
// can check the order of things without any window (Engine.cpp passes the
// real ones).
//
// The rule both ways: the journal lists a window from before it is hidden
// until after it is shown again. A process that ends anywhere in between
// (killed by the host, a crash) leaves a line that the next start, or the
// crash filter, acts on; never a hidden window without a line.
#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tabs {

struct WindowOps {
    // Made invisible: WS_EX_LAYERED added when `addLayered`, alpha 0.
    std::function<void(HWND w, bool addLayered)> conceal;
    // Visible again: alpha 255, WS_EX_LAYERED taken off when
    // `removeLayered`, its taskbar button back. False when it could not be.
    std::function<bool(HWND w, bool removeLayered)> reveal;
    // WS_EX_LAYERED is already set (then it is left on when shown again).
    std::function<bool(HWND w)> layered;
};

struct JournalLine {
    HWND window = nullptr;
    bool addedLayered = false;
};

std::string journalText(const std::vector<JournalLine>& lines);
std::vector<JournalLine> parseJournal(std::string_view text); // lines it cannot read are skipped

class Hider {
public:
    Hider(std::wstring journal, WindowOps ops);

    bool hidden(HWND w) const;
    std::size_t count() const;

    // Written down first, then hidden. False: already hidden, or closed.
    bool hide(HWND w);
    // Shown first, then struck from the journal.
    void show(HWND w);
    // The window closed (or became a tab): nothing to show again.
    void forget(HWND w);
    // Everything shown again (each one struck after it is shown).
    void showAll();
    // From now on nothing more is hidden (the plugin is stopping).
    void close();
    // Shows what it lists now without waiting for a hide or show under
    // way (stop(): a hide may be waiting on a busy Explorer); the journal
    // is left as it is.
    void revealSnapshot();

private:
    void writeLocked() const;

    std::wstring m_journal;
    WindowOps m_ops;
    mutable std::mutex m_mutex;
    // hide and show one after the other: a show while a hide still waits on
    // Explorer's thread would otherwise be undone by that hide.
    std::mutex m_serial;
    std::map<HWND, bool> m_windows; // window -> WS_EX_LAYERED was added by us
    bool m_closed = false;
};

// Shows the windows a journal lists (left hidden by a process that ended).
// `stillHidden` says whether a listed handle is still one of ours, hidden
// (handles are reused); the others are dropped. Lines whose window could not
// be shown stay in the file for the next time; with none left the file goes.
int restoreJournal(const std::wstring& journal, const std::function<bool(HWND)>& stillHidden,
    const std::function<bool(HWND, bool)>& reveal);

} // namespace tabs

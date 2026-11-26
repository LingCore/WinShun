#pragma once

#include <chrono>
#include <optional>

namespace ws {

// Whether the launcher is over every other window.
//
// Not pinned, it is: a menu, gone at the first click elsewhere. Pinned, it is
// a window of its own, over the others only if set so (Settings::pinnedOnTop,
// on unless turned off). Kept over them, it covered what was opened from it
// too: the document just asked for came up behind the list it was picked
// from, and going through results one after another meant moving the box
// away each time. So what it opens is let in front: it stands back among the
// other windows as it opens something, and is over them again once it is in
// front itself (a click on it, its shortcut, its taskbar button).
//
// Nothing may come to the front for what it opened (a command that shows
// nothing, a program that fails to start): kStandsBackFor after, a front lost
// is a click elsewhere like any other, and it is over that window as set.
class PinnedStacking {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds kStandsBackFor {20};

    // Something was opened from it (a file, a program, a window of Win顺's).
    void opened(Clock::time_point now) { m_opened = now; }
    // In front again, or shown anew.
    void inFront() { m_opened.reset(); }
    // Another window took the front. True: no longer standing back (apply onTop again).
    bool lostFront(Clock::time_point now)
    {
        if (!m_opened || now - *m_opened <= kStandsBackFor)
            return false;
        m_opened.reset();
        return true;
    }

    bool standsBack() const { return m_opened.has_value(); }
    bool onTop(bool pinned, bool pinnedOnTop) const { return !pinned || (pinnedOnTop && !m_opened); }

private:
    std::optional<Clock::time_point> m_opened;
};

} // namespace ws

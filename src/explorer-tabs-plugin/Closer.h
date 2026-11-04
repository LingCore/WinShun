// Closing the tabs winshun.explorer-tabs opened and no longer needs (a guess
// that was wrong, a merge that failed). Plain C++ around a few functions, so
// tests/tst_explorertabs can drive it with made-up tabs.
//
// Explorer's close command (0xA021), like a tab's own IWebBrowser2::Quit,
// closes the window's ACTIVE tab, not the one it is sent to, and a tab still
// being made can ignore it. So a close is sent only while the tab is the
// active one, one at a time (the next only after the last was answered),
// again until the tab is gone; 3 s at most, then it is left. A tab not known
// yet is the one tab of the window that was not there before -- if there are
// two such, it waits rather than guess. One close per window at a time: the
// engine opens no new tab in a window while a close is pending there.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace tabs {

struct TabOps {
    std::function<std::vector<HWND>(HWND top)> tabsOf; // the active tab first
    std::function<bool(HWND)> exists;
    std::function<bool(HWND tab)> sendClose; // asynchronous; answered() when Explorer has handled it
    std::function<std::int64_t()> now;
};

class SpareCloser {
public:
    static constexpr std::int64_t kLimitMs = 3000;

    struct Done {
        HWND top = nullptr;
        bool closed = false; // false: left (still there after kLimitMs, or never found)
        HWND thenSelect = nullptr; // the tab to select now (a switch that waited for the close)
        bool front = false; // the window to the front now
    };

    explicit SpareCloser(TabOps ops) : m_ops(std::move(ops)) {}

    // `before`: the window's tabs before the spare one was opened; `tab` may
    // be null (not seen yet).
    void close(HWND top, std::vector<HWND> before, HWND tab);
    bool pending(HWND top) const;
    bool any() const { return !m_closings.empty(); }
    // `tab` is (or, not known yet, may be) a tab being closed in `top`.
    bool isSpare(HWND top, HWND tab) const;
    // A switch to `select` in `top` follows the close pending there (the
    // spare tab must stay the active one until it is gone). False: none pending.
    bool deferSwitch(HWND top, HWND select);
    void answered(HWND tab);
    // Sends what can be sent; returns the closes that are over.
    std::vector<Done> tick();

private:
    struct Closing {
        HWND top = nullptr;
        std::vector<HWND> before;
        HWND tab = nullptr;
        std::int64_t until = 0;
        HWND thenSelect = nullptr;
        bool front = false;
        bool sent = false; // a close is on its way, unanswered
    };
    TabOps m_ops;
    std::vector<Closing> m_closings;
};

} // namespace tabs

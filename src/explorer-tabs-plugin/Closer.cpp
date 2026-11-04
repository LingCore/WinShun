#include "Closer.h"

#include <algorithm>

namespace tabs {

void SpareCloser::close(HWND top, std::vector<HWND> before, HWND tab)
{
    Closing c;
    c.top = top;
    c.before = std::move(before);
    c.tab = tab;
    c.until = m_ops.now() + kLimitMs;
    m_closings.push_back(std::move(c));
}

bool SpareCloser::pending(HWND top) const
{
    return std::ranges::any_of(m_closings, [&](const Closing& c) { return c.top == top; });
}

bool SpareCloser::isSpare(HWND top, HWND tab) const
{
    return std::ranges::any_of(m_closings, [&](const Closing& c) {
        return c.top == top && (c.tab ? c.tab == tab : std::ranges::find(c.before, tab) == c.before.end());
    });
}

bool SpareCloser::deferSwitch(HWND top, HWND select)
{
    for (Closing& c : m_closings) {
        if (c.top == top) {
            c.thenSelect = select;
            c.front = true;
            return true;
        }
    }
    return false;
}

void SpareCloser::answered(HWND tab)
{
    for (Closing& c : m_closings) {
        if (c.tab == tab)
            c.sent = false;
    }
}

std::vector<SpareCloser::Done> SpareCloser::tick()
{
    std::vector<Done> done;
    const std::int64_t now = m_ops.now();
    for (auto it = m_closings.begin(); it != m_closings.end();) {
        Closing& c = *it;
        const std::vector<HWND> tabs = m_ops.exists(c.top) ? m_ops.tabsOf(c.top) : std::vector<HWND> {};
        if (!c.tab) {
            HWND found = nullptr;
            int fresh = 0;
            for (HWND t : tabs) {
                if (std::ranges::find(c.before, t) == c.before.end()) {
                    found = t;
                    ++fresh;
                }
            }
            if (fresh == 1)
                c.tab = found;
        }
        const bool gone = c.tab && !m_ops.exists(c.tab);
        if (!gone && now < c.until && m_ops.exists(c.top)) {
            if (c.tab && !c.sent && !tabs.empty() && tabs.front() == c.tab && m_ops.sendClose(c.tab))
                c.sent = true;
            ++it; // (not active: someone chose another tab; it is left alone until it is again)
            continue;
        }
        done.push_back({c.top, gone, c.thenSelect, c.front});
        it = m_closings.erase(it);
    }
    return done;
}

} // namespace tabs

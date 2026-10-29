#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace ws::share {

// When a share that cannot be reached is tried again (GitHub #116). Pure:
// the time comes in from outside, so tests go through days of it at once.
//
// The probe (share::reachable) is asked first and often: it costs seconds at
// most, never Windows' minute. It can be wrong, though (a NAS slow to say
// hello, a name whose first address is dead), and then a share stayed
// offline for good. So Windows itself is asked too (share::openRoot), the
// way File Explorer gets there: before a share is called offline, now and
// then while it is, and whenever the user asks.
struct RetryTimes {
    std::chrono::milliseconds first {std::chrono::minutes(1)}; // the probe again once the server went away; doubling up to
    std::chrono::milliseconds last {std::chrono::minutes(5)};
    std::chrono::milliseconds windows {std::chrono::minutes(15)}; // Windows itself asked at least this often meanwhile
    std::chrono::milliseconds answer {std::chrono::seconds(15)}; // its answer waited for; it goes on trying after
    std::chrono::milliseconds problem {std::chrono::minutes(30)}; // it answers, not to us (signing in...): until the user acts
    std::chrono::milliseconds shown {std::chrono::seconds(1)}; // 立即核对 shows it checks at least this long, however soon the answer

    bool operator==(const RetryTimes&) const = default;
};

class Retry {
public:
    using Clock = std::chrono::steady_clock;

    explicit Retry(RetryTimes times = {}) noexcept
        : m_times(times)
        , m_next(times.first)
    {
    }

    // Not reached: how long until the probe asks again, twice as long each
    // time, up to `last`. `cutShort`: the last wait was (woken early: the
    // network changed, the user asked), and this one is no longer.
    std::chrono::milliseconds away(bool cutShort = false) noexcept
    {
        if (cutShort && m_last.count() > 0)
            return m_last;
        m_last = m_next;
        m_next = std::min(m_next * 2, std::max(m_times.last, m_times.first));
        return m_last;
    }
    // The probe answers again after it did not: should reading fail all the
    // same (its disks not mounted yet), the next try is soon.
    void answered() noexcept
    {
        m_next = m_times.first;
        m_last = {};
    }
    // Read in full: the next time it goes away starts over, Windows asked first.
    void read() noexcept
    {
        answered();
        m_windowsAt.reset();
    }

    // Whether to ask Windows itself now, the probe having said no: asked to
    // (立即核对), not yet since the share was last read, or not for `windows`.
    bool windowsDue(Clock::time_point now, bool asked) const noexcept
    {
        return asked || !m_windowsAt || now - *m_windowsAt >= m_times.windows;
    }
    void askedWindows(Clock::time_point now) noexcept { m_windowsAt = now; }

    const RetryTimes& times() const noexcept { return m_times; }

private:
    RetryTimes m_times;
    std::chrono::milliseconds m_next;
    std::chrono::milliseconds m_last {}; // what away() said last; 0: nothing since answered()
    std::optional<Clock::time_point> m_windowsAt;
};

} // namespace ws::share

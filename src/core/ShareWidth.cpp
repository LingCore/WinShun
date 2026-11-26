#include "ShareWidth.h"

#include <algorithm>

namespace ws::share {

namespace {

// Twice as wide is kept when half as much again gets read, and half as wide
// unless a third less does. Where requests overlap, twice as wide reads near
// twice as much; one window differs from the next by a third easily, and by
// the lower figures a server answering one after another was read two and
// four wide a quarter of the time (simulated with such noise), by these a tenth.
constexpr double kGain = 1.5;
constexpr double kLoss = 1.35;
// Windows from a trial that changed nothing to the next, twice as many each
// time: 8 s, then up to 2 minutes. Each trial of a server that answers one
// after another is a second of its other users waiting twice as long; one
// that answers side by side is found out by the trials that do change it,
// which follow one another at once.
constexpr int kHoldFirst = 16;
constexpr int kHoldMost = 240;
constexpr int kTrialWindows = 4; // the width there is and the one on trial, in turn
constexpr int kMinFolders = 8; // a window has at least as many
constexpr double kItemsAFolder = 64; // a folder of so many items is twice the work of an empty one

} // namespace

WidthTrial::WidthTrial(int most) noexcept
    : m_most(std::max(1, most))
    , m_wait(1) // a server that answers side by side is found out at once
    , m_hold(kHoldFirst)
{
}

int WidthTrial::other() noexcept
{
    if (m_base >= m_most)
        m_up = false;
    else if (m_base <= 1)
        m_up = true;
    return m_up ? std::min(m_most, m_base * 2) : std::max(1, m_base / 2);
}

void WidthTrial::settle(int base, bool changed) noexcept
{
    m_base = m_width = base;
    m_step = 0;
    if (changed) { // on in the same direction, at once
        m_hold = kHoldFirst;
        m_wait = 1;
    } else {
        m_wait = m_hold;
        m_hold = std::min(m_hold * 2, kHoldMost);
        m_up = !m_up;
    }
}

void WidthTrial::window(double work, double seconds) noexcept
{
    if (m_most <= 1 || seconds <= 0)
        return;
    if (m_step == 0) {
        if (--m_wait > 0)
            return;
        m_other = other();
        m_atBase = {work, seconds}; // this window is the first of the trial
        m_atOther = {};
        m_step = 1;
        m_width = m_other;
        return;
    }
    Sum& sum = m_width == m_base ? m_atBase : m_atOther;
    sum.work += work;
    sum.seconds += seconds;
    if (++m_step < kTrialWindows) {
        m_width = m_step % 2 ? m_other : m_base;
        return;
    }
    const double had = m_atBase.rate();
    const double tried = m_atOther.rate();
    const bool take = m_other > m_base ? tried >= kGain * had : had < kLoss * tried;
    settle(take ? m_other : m_base, take);
}

Width::Width(Policy policy, std::chrono::milliseconds window)
    : m_policy(std::move(policy))
    , m_window(window)
    , m_limit(m_policy())
    , m_trial(m_limit.most)
    , m_since(Clock::now())
{
    m_limit.most = std::max(1, m_limit.most);
    m_width = m_limit.tried ? m_trial.width() : m_limit.most;
    m_widest = m_width;
}

bool Width::enter(std::stop_token stop)
{
    std::unique_lock lock(m_mutex);
    m_place.wait(lock, stop, [&] { return m_reading < m_width; });
    if (stop.stop_requested())
        return false;
    ++m_reading;
    return true;
}

void Width::leave(std::optional<std::size_t> items)
{
    bool wider = false;
    {
        std::lock_guard lock(m_mutex);
        --m_reading;
        if (items) {
            m_work += 1.0 + static_cast<double>(*items) / kItemsAFolder;
            ++m_folders;
        }
        const auto now = Clock::now();
        if (m_folders >= kMinFolders && now - m_since >= m_window) {
            const int was = m_width;
            close(now);
            wider = m_width > was;
        }
    }
    // One place is free, for one of those waiting: all of them woken for it
    // (fifteen, with one read at a time) cost a quarter of the pass's speed.
    if (wider)
        m_place.notify_all();
    else
        m_place.notify_one();
}

void Width::close(Clock::time_point now)
{
    const double seconds = std::chrono::duration<double>(now - m_since).count();
    m_widthSeconds += m_width * seconds;
    m_seconds += seconds;
    Limit limit = m_policy();
    limit.most = std::max(1, limit.most);
    if (limit != m_limit) { // someone came to the computer, or left: from the start
        m_limit = limit;
        m_trial = WidthTrial(limit.most);
    } else if (limit.tried) {
        m_trial.window(m_work, seconds);
    }
    m_width = limit.tried ? m_trial.width() : limit.most;
    m_widest = std::max(m_widest, m_width);
    m_since = now;
    m_work = 0;
    m_folders = 0;
}

int Width::widest() const
{
    std::lock_guard lock(m_mutex);
    return m_widest;
}

double Width::mean() const
{
    std::lock_guard lock(m_mutex);
    return m_seconds > 0 ? m_widthSeconds / m_seconds : m_width;
}

} // namespace ws::share

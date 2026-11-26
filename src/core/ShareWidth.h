#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>

namespace ws::share {

// How many folders of a share are read at a time.
//
// A Samba server, which is nearly every NAS, answers all the requests of one
// PC in one process, one after another. With N folders read at a time,
// whatever else on this PC asks the same server waits behind N requests of
// ours for each of its own: a file manager opening a folder there takes N
// times as long, or "hangs". And the pass is no faster for it where the
// server's disk is what takes the time. Measured against Samba 4.21
// (docs/validation/bugs-1126): a disk answering in 10 ms, the share read in
// 19 s at 1, 8 and 16 alike, a folder opened meanwhile in 22, 109 and 184 ms
// (27 ms with nothing else going on).
//
// More at a time does pay where the server answers side by side (Windows),
// or the way there takes longer than the answer (Wi-Fi, a VPN): the requests
// overlap. Which it is cannot be asked, so it is tried: WidthTrial.

// The width to read at, from how much got read in each stretch of time (a
// "window") at the width it gave. It starts at one. Now and then it reads
// twice as wide for two windows, in turn with two at the width it has, and
// keeps the wider only if clearly more got read; likewise half as wide,
// kept unless clearly less got read. Windows taken in turn: what is being
// read changes all the time (folders in the server's memory or not), and
// would pass for an effect of the width. A trial that changes nothing is
// tried again later and later.
class WidthTrial {
public:
    explicit WidthTrial(int most) noexcept;

    int width() const noexcept { return m_width; } // to read at now
    int settled() const noexcept { return m_base; } // outside a trial
    int most() const noexcept { return m_most; }

    // A window is over: `work` got done in `seconds` at width().
    void window(double work, double seconds) noexcept;

private:
    struct Sum {
        double work = 0;
        double seconds = 0;
        double rate() const noexcept { return seconds > 0 ? work / seconds : 0; }
    };
    int other() noexcept;
    void settle(int base, bool changed) noexcept;

    int m_most = 1;
    int m_base = 1;
    int m_width = 1;
    int m_other = 1; // the width on trial
    int m_step = 0; // 0: no trial; 1..3: windows of it done
    int m_wait = 0; // windows until the next
    int m_hold = 0; // ... after one that changes nothing
    bool m_up = true; // which way the next goes
    Sum m_atBase;
    Sum m_atOther;
};

// Lets as many folders be read at a time as the width is. enter() before a
// folder is taken, leave() after it, from any number of threads
// (Crawler::Gate: before it is taken, which keeps the order of one thread).
class Width {
public:
    using Clock = std::chrono::steady_clock;

    struct Limit {
        int most = 1; // never more than this at a time
        bool tried = false; // as many of them as pay (WidthTrial); else all of them
        bool operator==(const Limit&) const = default;
    };
    // Asked at the start and after every window, on a thread of the pass:
    // who is at the computer changes while a share is read.
    using Policy = std::function<Limit()>;

    explicit Width(Policy policy, std::chrono::milliseconds window = std::chrono::milliseconds(500));

    bool enter(std::stop_token stop); // waits for a place; false when stopped instead
    // The folder is read, with so many items (or could not be: 0); nothing:
    // there was no folder to read.
    void leave(std::optional<std::size_t> items);

    // For the diagnostics.
    int widest() const;
    double mean() const; // over the time so far

private:
    void close(Clock::time_point now); // the window, under the lock

    Policy m_policy;
    Clock::duration m_window;
    mutable std::mutex m_mutex;
    std::condition_variable_any m_place;
    Limit m_limit;
    WidthTrial m_trial;
    int m_width = 1;
    int m_reading = 0;
    Clock::time_point m_since; // the window's start
    double m_work = 0;
    int m_folders = 0;
    int m_widest = 0;
    double m_widthSeconds = 0;
    double m_seconds = 0;
};

} // namespace ws::share

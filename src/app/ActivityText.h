#pragma once

#include <QString>

#include <cstddef>
#include <cstdint>

namespace ws::activity {

// Why a long job is not going at full speed right now.
enum class Pause : std::uint8_t {
    None,
    SearchBox, // the search box is open (or just closed): the disks are left to a search
    ContentSearch, // a content search reads the files itself
    Battery, // battery saver: waits
    UserPresent, // someone uses the computer: fewer readers, quieter disk reads
};

// A smoothed speed (items a second) from what was done over time. Needs a
// stretch of samples before it is believed (kMinSampleMs): a start is fast or
// slow by chance, and a guess from it would be wrong for minutes.
class RateEstimator {
public:
    static constexpr std::int64_t kMinSampleMs = 30'000;

    // `done` as of `nowMs` (any steady clock, ms). A `done` below the last (a
    // new round) starts over.
    void feed(std::int64_t nowMs, std::size_t done);
    void reset() noexcept { *this = RateEstimator(); }
    double perSecond() const noexcept { return m_rate; }
    // How long it has been watched (ms).
    std::int64_t sampleMs() const noexcept { return m_started ? m_lastMs - m_startMs : 0; }

private:
    bool m_started = false;
    std::int64_t m_startMs = 0;
    std::int64_t m_lastMs = 0;
    std::size_t m_lastDone = 0;
    double m_rate = 0; // exponentially smoothed
};

// What a long job looks like from outside.
struct Progress {
    std::size_t done = 0;
    std::size_t total = 0;
    double perSecond = 0;
    std::int64_t sampleMs = 0; // how long the speed has been watched
    Pause pause = Pause::None;
    bool finishing = false; // the last tidy-up (merging): the count no longer moves
};

// "你在搜索，先停一下" and its kind; empty for Pause::None.
QString pauseText(Pause pause);

// How long to go, said the way a person would: nothing while the speed is
// not known well enough ("" then), a range rounded to minutes, "快好了" under a
// minute, the tidy-up as that, and a pause or a slow-down as its reason. Never
// a figure to the second.
QString remainingText(const Progress& progress);

// "约 3–5 分钟" for a time to go in seconds (>= 60), widened to a range.
QString rangeText(double seconds);

// "6 分钟" for how long something took (completion notices).
QString tookText(std::int64_t ms);

} // namespace ws::activity

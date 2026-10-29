// When to take a break: the clock of the winshun.break plugin, as plain C++
// (no Win32) so tests/tst_break.cpp can drive it with made-up times.
//
// A cycle of work periods, each ended by a break; every `longEvery`-th
// break is a long one. Before a break a small notice shows for `headsUp`;
// when the break is due it waits for a pause in typing (at most
// kPendingMaxMs) so no keystroke lands under the break screen. Time away
// from the PC (`resetIdle` without input) counts as a break: the cycle
// starts again when the user is back. While a full-screen program,
// a presentation or Do not disturb is on, a due break waits until it ends.
#pragma once

#include <cstdint>

namespace brk {

enum class Kind { Short, Long };
enum class Strict { Off, LongOnly, All };
enum class Phase {
    Working, // counting towards the next break
    HeadsUp, // the notice is showing; the break starts at dueAt
    Pending, // due; waiting for a pause in typing
    OnBreak, // the break screen is up until breakEnd
    Paused, // no breaks until pausedUntil (wall clock)
};

struct Config {
    std::int64_t intervalMs = 20 * 60'000;
    std::int64_t shortMs = 20'000;
    int longEvery = 2; // every Nth break is long; 0: never
    std::int64_t longMs = 5 * 60'000;
    std::int64_t headsUpMs = 30'000;
    std::int64_t snoozeMs = 5 * 60'000;
    std::int64_t resetIdleMs = 5 * 60'000;
    Strict strict = Strict::Off;
};

// What one tick sees. `now` is a steady clock in ms, `wall` the wall clock
// (Unix ms, for a pause "until 15:30" across restarts and sleep).
struct Sense {
    std::int64_t now = 0;
    std::int64_t wall = 0;
    std::int64_t idleMs = 0; // since the last input (0 while something keeps the display on)
    bool quiet = false; // full-screen program, presentation, Do not disturb
    bool locked = false;
};

// What the windows should show, from state().
struct State {
    Phase phase = Phase::Working;
    Kind next = Kind::Short; // the next (or current) break's kind
    std::int64_t untilDueMs = 0; // Working, HeadsUp: until the break; else 0
    std::int64_t breakLeftMs = 0; // OnBreak
    std::int64_t breakLengthMs = 0; // OnBreak
    std::int64_t workedMs = 0; // since the last break
    std::int64_t pausedUntil = 0; // Paused: wall clock
    bool strict = false; // OnBreak (or about to be): no skip, no postpone
    bool canPostpone = false;
    bool waiting = false; // due, but held back by `quiet`
};

class Schedule {
public:
    static constexpr std::int64_t kPresentMs = 30'000; // input within this: at the PC
    static constexpr std::int64_t kTypingPauseMs = 1'500;
    static constexpr std::int64_t kPendingMaxMs = 15'000;

    void configure(const Config& config, std::int64_t now);
    const Config& config() const { return m_config; }

    void tick(const Sense& s);
    State state(std::int64_t now, std::int64_t wall) const;

    // The user's choices.
    bool postpone(std::int64_t now); // false when not allowed (once per break, never strict)
    bool skip(std::int64_t now, bool emergency = false); // emergency: the long Esc in strict mode
    void breakNow(Kind kind, std::int64_t now);
    void pause(std::int64_t untilWall);
    void resume(std::int64_t now);
    void restart(std::int64_t now); // a new cycle from now

    Phase phase() const { return m_phase; }

private:
    Kind nextKind() const;
    bool strictFor(Kind kind) const;
    void startWork(std::int64_t now, std::int64_t length);
    void startBreak(Kind kind, std::int64_t now);
    void finishBreak(std::int64_t now);

    Config m_config;
    Phase m_phase = Phase::Working;
    std::int64_t m_workStart = 0;
    std::int64_t m_dueAt = 0;
    std::int64_t m_pendingSince = 0;
    std::int64_t m_breakEnd = 0;
    std::int64_t m_breakLength = 0;
    std::int64_t m_pausedUntil = 0;
    Kind m_breakKind = Kind::Short;
    bool m_configured = false;
    bool m_postponed = false; // this break was put off once already
    bool m_waiting = false;
    int m_taken = 0; // breaks since the cycle started
};

} // namespace brk

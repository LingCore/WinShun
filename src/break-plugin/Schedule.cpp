#include "Schedule.h"

#include <algorithm>

namespace brk {

void Schedule::configure(const Config& config, std::int64_t now)
{
    const bool first = !m_configured;
    m_config = config;
    m_configured = true;
    if (first)
        startWork(now, m_config.intervalMs);
    else if (m_phase == Phase::Working && !m_postponed)
        m_dueAt = m_workStart + m_config.intervalMs; // a new interval counts from the same start
}

void Schedule::tick(const Sense& s)
{
    if (m_phase == Phase::Paused) {
        if (m_pausedUntil == kManualPause || s.wall < m_pausedUntil)
            return;
        startWork(s.now, m_config.intervalMs);
    }
    const bool quiet = s.quiet || s.locked;
    if (s.locked) {
        // Locked: whoever was at the PC left. A break on screen counts as
        // taken; a notice goes, and the break stays due.
        if (m_phase == Phase::OnBreak)
            finishBreak(s.now);
        else if (m_phase == Phase::HeadsUp || m_phase == Phase::Pending)
            m_phase = Phase::Working;
    }
    if (m_phase != Phase::OnBreak && s.idleMs >= m_config.resetIdleMs && !s.keptOn) {
        // Away long enough: that was the break. The cycle starts again, and
        // the next work period counts from when they are back.
        m_taken = 0;
        m_postponed = false;
        startWork(s.now, m_config.intervalMs);
        return;
    }

    switch (m_phase) {
    case Phase::Working: {
        m_waiting = false;
        const std::int64_t lead = m_config.headsUpMs;
        if (s.now < m_dueAt - lead)
            return;
        if (quiet) {
            m_waiting = s.now >= m_dueAt;
            return;
        }
        if (s.idleMs >= kPresentMs && !s.keptOn)
            return; // stepped away for a moment: the notice waits for them
        if (lead > 0) {
            // Held back (quiet, away) past the notice's start: it still
            // gets its whole time.
            m_dueAt = std::max(m_dueAt, s.now + lead);
            m_phase = Phase::HeadsUp;
        } else {
            m_phase = Phase::Pending;
            m_pendingSince = s.now;
        }
        return;
    }
    case Phase::HeadsUp:
        if (quiet)
            m_phase = Phase::Working; // back with a whole notice once it ends
        else if (s.now >= m_dueAt) {
            m_phase = Phase::Pending;
            m_pendingSince = s.now;
        }
        return;
    case Phase::Pending:
        if (quiet)
            m_phase = Phase::Working;
        else if (s.idleMs >= kTypingPauseMs || s.now - m_pendingSince >= kPendingMaxMs)
            startBreak(nextKind(), s.now);
        return;
    case Phase::OnBreak:
        if (s.now >= m_breakEnd)
            finishBreak(s.now);
        return;
    case Phase::Paused:
        return;
    }
}

State Schedule::state(std::int64_t now, std::int64_t wall) const
{
    (void)wall;
    State st;
    st.phase = m_phase;
    st.next = m_phase == Phase::OnBreak ? m_breakKind : nextKind();
    if (m_phase == Phase::Working || m_phase == Phase::HeadsUp)
        st.untilDueMs = std::max<std::int64_t>(0, m_dueAt - now);
    if (m_phase == Phase::OnBreak) {
        st.breakLeftMs = std::max<std::int64_t>(0, m_breakEnd - now);
        st.breakLengthMs = m_breakLength;
    }
    if (m_phase != Phase::Paused && m_phase != Phase::OnBreak)
        st.workedMs = std::max<std::int64_t>(0, now - m_workStart);
    st.pausedUntil = m_phase == Phase::Paused ? m_pausedUntil : 0;
    st.strict = strictFor(st.next);
    const bool asking = m_phase == Phase::HeadsUp || m_phase == Phase::Pending || m_phase == Phase::OnBreak;
    st.canPostpone = asking && !m_postponed && !st.strict;
    st.waiting = m_waiting;
    return st;
}

bool Schedule::postpone(std::int64_t now)
{
    const State st = state(now, 0);
    if (!st.canPostpone)
        return false;
    m_postponed = true;
    m_phase = Phase::Working; // the work period goes on: m_workStart stays
    m_dueAt = now + m_config.snoozeMs;
    m_waiting = false;
    return true;
}

bool Schedule::skip(std::int64_t now, bool emergency)
{
    if (m_phase != Phase::HeadsUp && m_phase != Phase::Pending && m_phase != Phase::OnBreak)
        return false;
    const Kind kind = m_phase == Phase::OnBreak ? m_breakKind : nextKind();
    if (strictFor(kind) && !emergency)
        return false;
    // A skipped break still has its place in the cycle, so the long ones
    // come when they would have.
    if (kind == Kind::Long)
        m_taken = 0;
    else
        ++m_taken;
    m_postponed = false;
    startWork(now, m_config.intervalMs);
    return true;
}

void Schedule::breakNow(Kind kind, std::int64_t now)
{
    if (m_phase != Phase::Paused)
        startBreak(kind, now);
}

void Schedule::pause(std::int64_t untilWall)
{
    m_phase = Phase::Paused;
    m_pausedUntil = untilWall;
    m_postponed = false;
    m_waiting = false;
}

void Schedule::resume(std::int64_t now)
{
    if (m_phase == Phase::Paused)
        startWork(now, m_config.intervalMs);
}

void Schedule::restart(std::int64_t now)
{
    m_taken = 0;
    m_postponed = false;
    if (m_phase != Phase::Paused)
        startWork(now, m_config.intervalMs);
}

Kind Schedule::nextKind() const
{
    return m_config.longEvery > 0 && (m_taken + 1) % m_config.longEvery == 0 ? Kind::Long : Kind::Short;
}

bool Schedule::strictFor(Kind kind) const
{
    return m_config.strict == Strict::All || (m_config.strict == Strict::LongOnly && kind == Kind::Long);
}

void Schedule::startWork(std::int64_t now, std::int64_t length)
{
    m_phase = Phase::Working;
    m_workStart = now;
    m_dueAt = now + length;
    m_waiting = false;
}

void Schedule::startBreak(Kind kind, std::int64_t now)
{
    m_breakKind = kind;
    m_breakLength = kind == Kind::Long ? m_config.longMs : m_config.shortMs;
    m_breakEnd = now + m_breakLength;
    m_phase = Phase::OnBreak;
    m_waiting = false;
}

void Schedule::finishBreak(std::int64_t now)
{
    if (m_breakKind == Kind::Long)
        m_taken = 0; // a long break (due or asked for) starts the cycle over
    else
        ++m_taken;
    m_postponed = false;
    startWork(now, m_config.intervalMs);
}

} // namespace brk

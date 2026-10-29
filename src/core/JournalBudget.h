#pragma once

#include <cstdint>

namespace ws::diagnostics {

// How many entries of one kind the diagnostics journal takes: it is small
// (1 MB a file, a week) and written as things happen (~1 ms each), so a key
// pressed all day must not fill it, nor push out what came before. GitHub
// #111 ("double Ctrl sometimes does nothing, or comes seconds late") wants
// the odd ones: each notable entry (slow, refused, ignored, dropped) is
// kept, up to `notablePerHour`; the ordinary ones as a baseline, the first
// `ordinaryAtStart` of a run and then one every `ordinaryEveryMs`. What was
// left out is counted, for the next entry taken to say. Pure logic (the
// caller passes the time, on the 64-bit tick clock), so it is unit-tested.
class JournalBudget {
public:
    struct Limits {
        int notablePerHour = 30;
        int ordinaryAtStart = 5;
        std::uint64_t ordinaryEveryMs = 10 * 60 * 1000;
    };

    JournalBudget() = default;
    explicit JournalBudget(Limits limits)
        : m_limits(limits)
    {
    }

    // Whether to write this one; if so, skipped() says how many were left
    // out before it.
    bool take(bool notable, std::uint64_t now) noexcept
    {
        constexpr std::uint64_t hour = 60 * 60 * 1000;
        if (!m_hourStarted || now - m_hourStart >= hour) {
            m_hourStarted = true;
            m_hourStart = now;
            m_notable = 0;
        }
        bool write = false;
        if (notable) {
            write = m_notable < m_limits.notablePerHour;
            if (write)
                ++m_notable;
        } else if (m_ordinary < m_limits.ordinaryAtStart) {
            ++m_ordinary;
            write = true;
        } else {
            write = !m_ordinaryWritten || now - m_ordinaryAt >= m_limits.ordinaryEveryMs;
        }
        if (write && !notable) {
            m_ordinaryWritten = true;
            m_ordinaryAt = now;
        }
        if (!write) {
            ++m_skipping;
            return false;
        }
        m_skipped = m_skipping;
        m_skipping = 0;
        return true;
    }

    // Left out before the last one taken.
    int skipped() const noexcept { return m_skipped; }

private:
    Limits m_limits;
    bool m_hourStarted = false;
    std::uint64_t m_hourStart = 0;
    int m_notable = 0; // this hour
    int m_ordinary = 0; // this run, up to ordinaryAtStart
    bool m_ordinaryWritten = false;
    std::uint64_t m_ordinaryAt = 0;
    int m_skipping = 0;
    int m_skipped = 0;
};

} // namespace ws::diagnostics

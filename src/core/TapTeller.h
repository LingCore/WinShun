#pragma once

#include <cstdint>
#include <optional>

namespace ws {

// Which double taps to tell of, by the time of the key event that made each
// (the input's 32-bit clock in milliseconds, which Raw Input gives every
// process alike). Two listeners can tell of the same tap: this process's own
// and the service's UIAccess one, during a handover and whenever one stands
// in for the other's late word (KeyListener). The second word of a tap, or
// a word of one older than the last told, is a duplicate. A tap told late
// has missed its moment: up to kFreshMs old it is told as it is, up to
// kLateMs only if nothing came since (`quiet`, asked only then), never
// later. Its listener's pages trimmed away (a memory cleaner empties every
// process's working set) or starved, a word came seconds late (GitHub #111),
// and then opened Win顺 over whatever the user had gone on to. Pure logic,
// so it is unit-tested.
class TapTeller {
public:
    static constexpr std::uint32_t kFreshMs = 1000;
    static constexpr std::uint32_t kLateMs = 3000;

    enum class Verdict : std::uint8_t { Tell, Duplicate, Stale };

    // One word of a tap, `now` on the same clock. Tell: told from here on.
    template <class Quiet>
    Verdict judge(std::uint32_t tapTime, std::uint32_t now, Quiet&& quiet)
    {
        if (toldAlready(tapTime, now))
            return Verdict::Duplicate;
        const std::uint32_t age = now - tapTime;
        if (age >= kFreshMs && (age >= kLateMs || !quiet()))
            return Verdict::Stale;
        m_told = true;
        m_lastTold = tapTime;
        return Verdict::Tell;
    }

    // That tap, or a later one, was told: however late its other word comes
    // (GitHub #111: the UIAccess listener's, starved for seconds), never
    // judged as a tap of its own and dropped as too old. Times compare by
    // their difference, which says which came first while they are less
    // than 24.8 days apart: one told longer ago than that is forgotten.
    bool toldAlready(std::uint32_t tapTime, std::uint32_t now) const noexcept
    {
        return m_told && static_cast<std::int32_t>(now - m_lastTold) >= 0
            && static_cast<std::int32_t>(tapTime - m_lastTold) <= 0;
    }

    // A tap dropped (Stale, or not taken at all): true the first time for
    // it, so that it is noted once though both listeners tell of it.
    bool firstDrop(std::uint32_t tapTime) noexcept
    {
        if (m_dropped == tapTime)
            return false;
        m_dropped = tapTime;
        return true;
    }

private:
    bool m_told = false;
    std::uint32_t m_lastTold = 0;
    std::optional<std::uint32_t> m_dropped;
};

} // namespace ws

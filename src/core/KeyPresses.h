#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ws {

// The presses of keys other than the modifiers, as KeyListener's Raw Input
// sees them, for whether Windows hid the pointer for typing (hiddenForTyping,
// GameGuard). A key's first press only, not its repeats while it is held: a
// game played with the keys repeats a held arrow or W some 30 times a
// second, and a game hiding the pointer itself between two repeats passed
// for typing (GitHub #111). Kept for kKeptMs after the newest, not by count:
// with the GUI thread busy at the hide, a few keys typed meanwhile pushed the
// press before it out of the last eight. Times on the input's 32-bit clock.
// Nothing typed is kept: the presses are times alone, and which keys are
// down is known only while they are (one whose release went unseen, until it
// is pressed again). Pure logic, so it is unit-tested; not thread-safe
// (KeyListener guards it).
class KeyPresses {
public:
    static constexpr std::uint32_t kKeptMs = 10'000;
    // As DoubleTapDetector's lostReleaseMs: longer than a keyboard waits
    // before it repeats a held key (1 s at most) or between repeats. A key
    // down again after this much quiet was let go unseen: the release went
    // to the secure desktop, or to an administrator's window (UIPI).
    static constexpr std::uint32_t kLostReleaseMs = 1500;
    // 10 s of typing at 25 presses a second; more is no one's typing, and
    // the oldest go first.
    static constexpr std::size_t kMost = 256;

    // A key went down, a repeat or a press: Raw Input does not say which.
    // True for a press.
    bool down(std::uint32_t vk, std::uint32_t time) noexcept
    {
        vk %= kKeys;
        const bool press = !m_down[vk] || time - m_lastDown[vk] > kLostReleaseMs;
        m_down[vk] = true;
        m_lastDown[vk] = time;
        if (press)
            keep(time);
        return press;
    }

    void up(std::uint32_t vk) noexcept
    {
        vk %= kKeys;
        m_down[vk] = false;
        m_lastDown[vk] = 0; // read only while down
    }

    // The last press at or before `time`; none if none is kept.
    std::optional<std::uint32_t> atOrBefore(std::uint32_t time) const noexcept
    {
        for (std::size_t i = 0; i < m_kept; ++i) { // the newest first
            const std::uint32_t at = m_presses[(m_next + kMost - 1 - i) % kMost];
            if (static_cast<std::int32_t>(time - at) >= 0)
                return at;
        }
        return std::nullopt;
    }

private:
    static constexpr std::uint32_t kKeys = 256; // the virtual-key codes

    void keep(std::uint32_t time) noexcept
    {
        while (m_kept && time - m_presses[(m_next + kMost - m_kept) % kMost] > kKeptMs)
            --m_kept; // the oldest
        m_presses[m_next] = time;
        m_next = (m_next + 1) % kMost;
        m_kept = (std::min)(m_kept + 1, kMost);
    }

    std::bitset<kKeys> m_down;
    std::array<std::uint32_t, kKeys> m_lastDown {}; // repeats too; only while down
    std::array<std::uint32_t, kMost> m_presses {}; // a ring, the newest just before m_next
    std::size_t m_next = 0;
    std::size_t m_kept = 0;
};

} // namespace ws

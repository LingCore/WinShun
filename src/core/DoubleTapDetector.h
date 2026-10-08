#pragma once

#include <cstdint>
#include <cstdlib>

namespace ws {

// Recognises "tap Ctrl twice" from raw key events. Pure logic (no Win32), so
// it is unit-tested; KeyListener feeds it events.
//
// A tap is a Ctrl press and release with no other key in between, shorter than
// maxPressMs. Two taps within maxGapMs trigger, unless the mouse moved in
// between (that is Ctrl+click multi-selection, not a double tap).
class DoubleTapDetector {
public:
    struct Config {
        std::uint32_t maxPressMs = 300;
        std::uint32_t maxGapMs = 400;
        int maxMovePx = 12;
    };

    DoubleTapDetector() = default;
    explicit DoubleTapDetector(Config config)
        : m_config(config)
    {
    }

    static constexpr bool isCtrl(std::uint32_t vk) noexcept
    {
        return vk == 0x11 /*VK_CONTROL*/ || vk == 0xA2 /*VK_LCONTROL*/ || vk == 0xA3 /*VK_RCONTROL*/;
    }

    // Both return true when the event completes a double tap.
    bool keyDown(std::uint32_t vk, std::uint32_t timeMs, int x, int y) noexcept
    {
        if (!isCtrl(vk)) {
            m_clean = false;
            m_armed = false;
            return false;
        }
        if (m_down)
            return false; // auto-repeat while held
        m_down = true;
        m_clean = true;
        m_downTime = timeMs;
        m_downX = x;
        m_downY = y;
        if (m_armed && timeMs - m_firstUpTime > m_config.maxGapMs)
            m_armed = false;
        return false;
    }

    bool keyUp(std::uint32_t vk, std::uint32_t timeMs, int x, int y) noexcept
    {
        if (!isCtrl(vk) || !m_down)
            return false;
        m_down = false;
        const bool tap = m_clean && timeMs - m_downTime <= m_config.maxPressMs && near(x, y, m_downX, m_downY);
        if (!tap) {
            m_armed = false;
            return false;
        }
        if (m_armed && near(m_downX, m_downY, m_firstX, m_firstY)) {
            m_armed = false;
            return true;
        }
        m_armed = true;
        m_firstUpTime = timeMs;
        m_firstX = m_downX;
        m_firstY = m_downY;
        return false;
    }

    void reset() noexcept { *this = DoubleTapDetector(m_config); }

private:
    bool near(int x1, int y1, int x2, int y2) const noexcept
    {
        return std::abs(x1 - x2) <= m_config.maxMovePx && std::abs(y1 - y2) <= m_config.maxMovePx;
    }

    Config m_config;
    bool m_down = false;
    bool m_clean = false;
    bool m_armed = false;
    std::uint32_t m_downTime = 0;
    std::uint32_t m_firstUpTime = 0;
    int m_downX = 0;
    int m_downY = 0;
    int m_firstX = 0;
    int m_firstY = 0;
};

} // namespace ws

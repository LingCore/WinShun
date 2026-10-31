// What the break screen's keyboard hook (Ui.cpp) keeps from the program
// underneath: plain C++, so tests/tst_break.cpp can play keys on it.
//
// While the screen is up every key is kept but the volume and media ones.
// A key that was down already as it came up (Ctrl held while typing as the
// break began) has its repeats kept but its release let through, or Windows
// would have it down after the break. Once the screen goes, the keys it took
// stay kept, repeats and all, until they are up: else the program gets Esc's
// repeats after the long Esc of a strict break, and an up without its down.
#pragma once

#include <set>

namespace brk {

class KeyGate {
public:
    void close() { m_draining = false; } // the screen is up (again): every key is kept
    void drain() { m_draining = true; } // it went: only the keys it took, until they are up
    bool draining() const { return m_draining; }
    bool empty() const { return m_held.empty(); }
    bool took(unsigned vk) const { return m_held.count(vk) > 0; }

    // True: kept from the program. `wasDown`: Windows has the key down
    // already (pressed before the screen, as its own downs are kept).
    bool key(unsigned vk, bool down, bool wasDown)
    {
        if (took(vk)) {
            if (!down)
                m_held.erase(vk);
            return true;
        }
        if (m_draining || passes(vk) || !down)
            return false; // after the screen; music; the up of a key pressed before it
        if (!wasDown)
            m_held.insert(vk);
        return true;
    }

    // Music can still be paused or turned down.
    static bool passes(unsigned vk) { return vk >= 0xAD && vk <= 0xB3; } // VK_VOLUME_MUTE .. VK_MEDIA_PLAY_PAUSE

private:
    std::set<unsigned> m_held; // went down while the screen was up
    bool m_draining = false;
};

} // namespace brk

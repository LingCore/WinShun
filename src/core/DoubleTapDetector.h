#pragma once

#include <cstdint>
#include <cstdlib>
#include <utility>

namespace ws {

// Recognises "tap Ctrl twice", or Alt or Shift, from raw key events. Pure logic (no
// Win32), so it is unit-tested; KeyListener feeds it events.
//
// A tap is a press and release of the key with no other key, mouse button
// or wheel in between, shorter than maxPressMs, and with no other modifier
// held at the press: Shift, Win, and Alt for Ctrl or Ctrl for Alt (for Shift:
// Ctrl, Alt and Win; Shift with a letter is a capital, not a tap). Two taps
// of the same key, the same one of its two, within maxGapMs trigger, unless
// the mouse moved in between. The exceptions are the key used with the
// mouse, not a double tap: Ctrl+click multi-selection (the same spot twice,
// or one row and the next), Ctrl+wheel zooming, and a touchpad pinch, which
// Windows sends as Ctrl+wheel; Alt+click and Alt+drag alike.
//
// Alt (and Shift) have one more: a tap a program sent whole (press and release together,
// SendInput) is none. Programs tap Alt to be allowed to bring a window to
// the front, AutoHotkey's WinActivate even twice ({Alt 2}). A key remapped
// to Alt goes down and up with the finger, and counts.
//
// Keys come as the left or right one (VK_LCONTROL, VK_RMENU…); VK_CONTROL,
// VK_SHIFT and VK_MENU count as the left one. AltGr is the right Alt: the
// left Ctrl Windows adds to it goes to programs' messages and hooks, not to
// Raw Input (were it there, it would only spoil the Alt's tap; and it is
// never a Ctrl tap of its own, with the right Alt right after it).
class DoubleTapDetector {
public:
    enum class Key : std::uint8_t {
        Ctrl,
        Alt,
        Shift,
    };

    enum class Side : std::uint8_t {
        Both, // either one, both taps on the same one
        Left,
        Right,
    };

    // Why a press of the key soon after a tap of it (within twice maxGapMs)
    // did not make a double tap, for the diagnostics journal (GitHub #111:
    // "sometimes double Ctrl does not open"): it came late, was held too
    // long, was spoiled (another key, a modifier held, the mouse used while
    // it was down), the pointer moved, it was the key's other one, or
    // something came between the taps (a modifier, the mouse, a program's
    // key). Typing between them is not a near miss (Ctrl, a letter, Ctrl).
    // The First… ones: the press before a tap was no tap (spoiled, held too
    // long, the pointer moved), so the tap after it had nothing to make a
    // double tap with; a press spoiled by typing (Ctrl+C) is none, whatever
    // spoiled it first (Ctrl+Shift+S). Held also: a press soon after one
    // held too long, itself held too long.
    enum class MissReason : std::uint8_t {
        None,
        Late,
        Held,
        Spoiled,
        Moved,
        OtherSide,
        Interrupted,
        FirstSpoiled,
        FirstHeld,
        FirstMoved,
    };
    // What spoiled a press, or came between the taps: another modifier held
    // (one whose release was never seen counts for a while, lostReleaseMs),
    // another key, a program's key (SendInput), a mouse button or the wheel,
    // the key's other one. Never which key.
    enum class Cause : std::uint8_t { None, Modifier, Key, ProgramKey, Mouse, BothKeys };
    struct Miss {
        MissReason reason = MissReason::None;
        // Late: from the first tap's release to this press; Held, FirstHeld: how long it was held.
        std::uint32_t ms = 0;
        Cause by = Cause::None; // Spoiled, FirstSpoiled, Interrupted
    };
    // For the journal and the key log.
    static constexpr const char* name(MissReason reason) noexcept
    {
        switch (reason) {
        case MissReason::Late:
            return "late";
        case MissReason::Held:
            return "held";
        case MissReason::Spoiled:
            return "spoiled";
        case MissReason::Moved:
            return "moved";
        case MissReason::OtherSide:
            return "other-side";
        case MissReason::Interrupted:
            return "interrupted";
        case MissReason::FirstSpoiled:
            return "first-spoiled";
        case MissReason::FirstHeld:
            return "first-held";
        case MissReason::FirstMoved:
            return "first-moved";
        case MissReason::None:
            break;
        }
        return "none";
    }
    static constexpr const char* name(Cause cause) noexcept
    {
        switch (cause) {
        case Cause::Modifier:
            return "modifier";
        case Cause::Key:
            return "key";
        case Cause::ProgramKey:
            return "program-key";
        case Cause::Mouse:
            return "mouse";
        case Cause::BothKeys:
            return "both-keys";
        case Cause::None:
            break;
        }
        return "none";
    }

    struct Config {
        std::uint32_t maxPressMs = 300;
        std::uint32_t maxGapMs = 400;
        int maxMovePx = 12;
        // Longer than a keyboard waits before it repeats a held key (1 s at
        // most) or between repeats: a press after this much quiet is a new
        // one, and the release of the last was lost.
        std::uint32_t lostReleaseMs = 1500;
        Key key = Key::Ctrl;
        Side side = Side::Both;
        // Whether a key is down now (GetAsyncKeyState), for a modifier whose
        // release may have been lost; none: as the events say.
        bool (*isDown)(std::uint32_t vk) = nullptr;
    };

    DoubleTapDetector() = default;
    explicit DoubleTapDetector(Config config)
        : m_config(config)
    {
    }

    static constexpr bool isKey(Key key, std::uint32_t vk) noexcept
    {
        switch (key) {
        case Key::Alt:
            return vk == kAlt || vk == kLeftAlt || vk == kRightAlt;
        case Key::Shift:
            return vk == kShift || vk == kLeftShift || vk == kRightShift;
        case Key::Ctrl:
            break;
        }
        return vk == kCtrl || vk == kLeftCtrl || vk == kRightCtrl;
    }
    // Whether `vk` is the key tapped here, either one of its two.
    bool isKey(std::uint32_t vk) const noexcept { return isKey(m_config.key, vk); }
    Key key() const noexcept { return m_config.key; }

    // Both return true when the event completes a double tap. `sent`: a
    // program's (SendInput), not a keyboard's.
    bool keyDown(std::uint32_t vk, std::uint32_t timeMs, int x, int y, bool sent = false) noexcept
    {
        if (const int m = modifierOf(vk); m >= 0) {
            m_modifierDown[m] = true;
            m_modifierTime[m] = timeMs; // repeats too
        }
        if (!counts(vk)) {
            const bool modifier = modifierOf(vk) >= 0;
            spoil(modifier ? Cause::Modifier : sent ? Cause::ProgramKey : Cause::Key);
            m_clean = false;
            m_armed = false;
            if (!modifier && !sent) { // typing: whatever comes next is not a second tap missed
                m_tapped = false;
                m_spoiledFirst = {};
                if (m_down)
                    m_pressCause = Cause::Key; // nor the press under way, whatever spoiled it first
            }
            return false;
        }
        vk = leftOrRight(vk);
        // The other one's, a while after a press whose release was never
        // seen (it went to the secure desktop, a hook ate it): a press of its
        // own, not both at once, or the other one's taps would all go
        // unseen until this one is pressed again.
        if (m_down && vk != m_downKey && timeMs - m_lastDownTime > m_config.lostReleaseMs)
            m_down = false;
        if (m_down && vk != m_downKey) { // both keys at once
            spoil(Cause::BothKeys);
            m_clean = false;
            m_armed = false;
            return false;
        }
        // Auto-repeat while held. Unless the release was never seen: it went
        // to the secure desktop (Ctrl+Alt+Del), or a program's hook ate it.
        const bool repeat = m_down && timeMs - m_lastDownTime <= m_config.lostReleaseMs;
        m_lastDownTime = timeMs;
        if (repeat)
            return false;
        m_down = true;
        m_downKey = vk;
        m_downSent = sent;
        // Ctrl with Alt held is part of a chord, or another program's doing:
        // a hotkey tool taps Ctrl so that letting go of Alt will not open a
        // window's menu bar (AutoHotkey's #MenuMaskKey), once per hotkey.
        // Alt with Ctrl held likewise: Ctrl+Alt+Del, Ctrl+Alt+arrows.
        m_clean = !modifierHeld(timeMs);
        m_pressCause = m_clean ? Cause::None : Cause::Modifier;
        m_downTime = timeMs;
        m_downX = x;
        m_downY = y;
        if (m_armed && (timeMs - m_firstUpTime > m_config.maxGapMs || !m_clean))
            m_armed = false;
        // Pressed again as it was let go, quicker than a finger can
        // (kQueuedGapMs): the release and this press waited together behind
        // a program's slow low-level hook, which held up the press before.
        // Windows times a key as it gets to it, so that press only seemed
        // long: it was a tap (GitHub #111: the first double Ctrl right after
        // switching apps never opened, a second one did).
        if (!m_armed && m_clean && m_spoiledFirst.miss.reason == MissReason::FirstHeld && m_spoiledFirst.key == vk
            && timeMs - m_spoiledFirst.upTime <= kQueuedGapMs && closeTo(x, y, m_spoiledFirst.x, m_spoiledFirst.y)) {
            m_armed = true;
            m_firstKey = vk;
            m_firstDownTime = m_spoiledFirst.downTime;
            m_firstUpTime = m_spoiledFirst.upTime;
            m_firstX = m_spoiledFirst.x;
            m_firstY = m_spoiledFirst.y;
            m_tapped = true;
            m_lastTapUp = m_spoiledFirst.upTime;
            m_queuedMs = m_spoiledFirst.miss.ms;
            m_spoiledFirst = {};
        }
        return false;
    }

    bool keyUp(std::uint32_t vk, std::uint32_t timeMs, int x, int y, bool sent = false) noexcept
    {
        if (const int m = modifierOf(vk); m >= 0)
            m_modifierDown[m] = false;
        if (!counts(vk) || !m_down || leftOrRight(vk) != m_downKey)
            return false;
        m_down = false;
        const bool sentWhole = sent && m_downSent && timeMs - m_downTime < kSentWholeMs;
        const std::uint32_t held = timeMs - m_downTime;
        const bool moved = !closeTo(x, y, m_downX, m_downY);
        const bool tap = m_clean && !(sentWhole && m_config.key != Key::Ctrl) && held <= m_config.maxPressMs && !moved;
        // Meant as the second of a double tap, perhaps.
        const bool second = m_tapped && m_downTime - m_lastTapUp <= 2 * m_config.maxGapMs;
        if (!tap) {
            if (!sentWhole) {
                const Miss miss = !m_clean   ? Miss {MissReason::Spoiled, 0, m_pressCause}
                    : held > m_config.maxPressMs ? Miss {MissReason::Held, held}
                                                 : Miss {MissReason::Moved};
                // Told now: meant as the second, perhaps, or held too long
                // after one held too long (a slow double tap). Else told
                // should a tap follow: meant as the first, perhaps. Either
                // way the first of one, if let go as the next was pressed.
                const bool heldAgain = miss.reason == MissReason::Held
                    && m_spoiledFirst.miss.reason == MissReason::FirstHeld
                    && m_downTime - m_spoiledFirst.upTime <= 2 * m_config.maxGapMs;
                if (second || heldAgain)
                    m_miss = miss;
                if (miss.by != Cause::Key)
                    m_spoiledFirst = SpoiledFirst {
                        first(miss), timeMs, m_downKey, m_downTime, m_downX, m_downY, second || heldAgain};
            }
            m_armed = false;
            m_tapped = false;
            return false;
        }
        // A tap of the other one starts over from this one.
        if (m_armed && m_firstKey == m_downKey && closeTo(m_downX, m_downY, m_firstX, m_firstY)) {
            m_armed = false;
            m_tapped = false;
            m_spoiledFirst = {};
            return true;
        }
        if (second) {
            const std::uint32_t gap = m_downTime - m_lastTapUp;
            m_miss = m_armed && m_firstKey != m_downKey ? Miss {MissReason::OtherSide}
                : m_armed                                ? Miss {MissReason::Moved}
                : gap > m_config.maxGapMs                ? Miss {MissReason::Late, gap}
                                                         : Miss {MissReason::Interrupted, 0, m_gapCause};
        } else if (m_spoiledFirst.miss.reason != MissReason::None && !m_spoiledFirst.told
            && m_downTime - m_spoiledFirst.upTime <= 2 * m_config.maxGapMs) {
            m_miss = m_spoiledFirst.miss;
        }
        m_spoiledFirst = {};
        m_gapCause = Cause::None;
        m_queuedMs = 0;
        m_armed = true;
        m_firstKey = m_downKey;
        m_firstDownTime = m_downTime;
        m_firstUpTime = timeMs;
        m_firstX = m_downX;
        m_firstY = m_downY;
        m_tapped = true;
        m_lastTapUp = timeMs;
        return false;
    }

    // The last near miss (see MissReason), once.
    Miss takeMiss() noexcept { return std::exchange(m_miss, Miss {}); }

    // Where the double tap keyUp just told of began: its first tap's press.
    // Pressed again or not is judged by it (seenBy): a double tap begun
    // before a window came ends some 200–300 ms later, after it (GitHub #111).
    std::uint32_t firstPressTime() const noexcept { return m_firstDownTime; }
    // How long its first press seemed, when it counted as a tap all the
    // same, let go as the second was pressed (behind a slow hook); else 0.
    std::uint32_t queuedMs() const noexcept { return m_queuedMs; }

    // A mouse button went down or is held, or the wheel turned: the key held
    // now, or just tapped, was for a click, a drag or zooming.
    void mouseUsed() noexcept
    {
        spoil(Cause::Mouse);
        m_clean = false;
        m_armed = false;
    }

    // For a debug log: whether the last press was clean so far, and whether
    // a first tap waits for its second.
    bool clean() const noexcept { return m_clean; }
    bool armed() const noexcept { return m_armed; }

    // The second tap may come as late as this after the first.
    void setMaxGap(std::uint32_t ms) noexcept { m_config.maxGapMs = ms; }
    void setSide(Side side) noexcept { m_config.side = side; }

    void reset() noexcept { *this = DoubleTapDetector(m_config); }

private:
    static constexpr std::uint32_t kCtrl = 0x11, kLeftCtrl = 0xA2, kRightCtrl = 0xA3;
    static constexpr std::uint32_t kAlt = 0x12, kLeftAlt = 0xA4, kRightAlt = 0xA5;
    static constexpr std::uint32_t kShift = 0x10, kLeftShift = 0xA0, kRightShift = 0xA1;
    // A program's press and release, one right after the other: on the same
    // tick of the input's clock, or the next (it moves in steps of about
    // 16 ms). A finger holds a key down longer, a key remapped to Alt too.
    static constexpr std::uint32_t kSentWholeMs = 20;
    // Let go and pressed again on the same tick of that clock, or the next:
    // no finger is that quick, the two came together.
    static constexpr std::uint32_t kQueuedGapMs = 16;

    static constexpr std::uint32_t leftOrRight(std::uint32_t vk) noexcept
    {
        return vk == kCtrl ? kLeftCtrl : vk == kAlt ? kLeftAlt : vk == kShift ? kLeftShift : vk;
    }

    // 0 Shift, 1 Ctrl, 2 Alt, 3 Win; -1 any other key.
    static constexpr int modifierOf(std::uint32_t vk) noexcept
    {
        switch (vk) {
        case kShift:
        case kLeftShift:
        case kRightShift:
            return 0;
        case kCtrl:
        case kLeftCtrl:
        case kRightCtrl:
            return 1;
        case kAlt:
        case kLeftAlt:
        case kRightAlt:
            return 2;
        case 0x5B: // VK_LWIN
        case 0x5C: // VK_RWIN
            return 3;
        default:
            return -1;
        }
    }

    // The keys this listens to; any other is "another key".
    bool counts(std::uint32_t vk) const noexcept
    {
        const Key key = m_config.key;
        switch (m_config.side) {
        case Side::Left:
            return key == Key::Alt ? vk == kAlt || vk == kLeftAlt
                : key == Key::Shift ? vk == kShift || vk == kLeftShift
                                    : vk == kCtrl || vk == kLeftCtrl;
        case Side::Right:
            return vk == (key == Key::Alt ? kRightAlt : key == Key::Shift ? kRightShift : kRightCtrl);
        case Side::Both:
            break;
        }
        return isKey(vk);
    }

    // The other modifiers, as the events say, in their order: by the time
    // an event is looked at, a program that sent a whole sequence may have
    // let go of Alt already. A release seen by no one (Win+L, Ctrl+Alt+Del:
    // it went to the secure desktop) is noticed once the key has been quiet
    // a while and is up.
    bool modifierHeld(std::uint32_t timeMs) noexcept
    {
        static constexpr std::uint32_t kQueryKey[] {kShift, kCtrl, kAlt, 0x5B};
        const int own = m_config.key == Key::Alt ? 2 : m_config.key == Key::Shift ? 0 : 1;
        bool held = false;
        for (int m = 0; m < 4; ++m) {
            if (m == own || !m_modifierDown[m])
                continue;
            if (timeMs - m_modifierTime[m] <= m_config.lostReleaseMs || !m_config.isDown
                || m_config.isDown(kQueryKey[m]) || (m == 3 && m_config.isDown(0x5C)))
                held = true;
            else
                m_modifierDown[m] = false;
        }
        return held;
    }

    bool closeTo(int x1, int y1, int x2, int y2) const noexcept
    {
        return std::abs(x1 - x2) <= m_config.maxMovePx && std::abs(y1 - y2) <= m_config.maxMovePx;
    }

    // The first thing that spoiled the press under way, or came between a
    // tap and the next press: what a near miss says it was.
    void spoil(Cause cause) noexcept
    {
        if (m_down) {
            if (m_clean)
                m_pressCause = cause;
        } else if (m_armed) {
            m_gapCause = cause;
        }
    }

    static constexpr Miss first(Miss miss) noexcept
    {
        miss.reason = miss.reason == MissReason::Spoiled ? MissReason::FirstSpoiled
            : miss.reason == MissReason::Held            ? MissReason::FirstHeld
                                                         : MissReason::FirstMoved;
        return miss;
    }

    Config m_config;
    bool m_down = false;
    bool m_downSent = false;
    bool m_clean = false;
    bool m_armed = false;
    bool m_modifierDown[4] {};
    std::uint32_t m_modifierTime[4] {}; // the last press or repeat
    std::uint32_t m_downKey = 0; // the left or the right one
    std::uint32_t m_firstKey = 0;
    std::uint32_t m_firstDownTime = 0; // the armed tap's press
    std::uint32_t m_downTime = 0;
    std::uint32_t m_lastDownTime = 0; // repeats included
    std::uint32_t m_firstUpTime = 0;
    // The last tap of the key, armed or not, with no typing since: for a
    // near miss (Miss).
    bool m_tapped = false;
    std::uint32_t m_lastTapUp = 0;
    Miss m_miss;
    Cause m_pressCause = Cause::None;
    Cause m_gapCause = Cause::None;
    // A press that was no tap (and not for typing), with no typing since: a
    // tap after it may have been meant as the second.
    struct SpoiledFirst {
        Miss miss;
        std::uint32_t upTime = 0;
        std::uint32_t key = 0; // the left or the right one
        std::uint32_t downTime = 0;
        int x = 0;
        int y = 0;
        bool told = false; // as a near miss of its own
    };
    SpoiledFirst m_spoiledFirst;
    std::uint32_t m_queuedMs = 0; // the armed tap's, queuedMs()
    int m_downX = 0;
    int m_downY = 0;
    int m_firstX = 0;
    int m_firstY = 0;
};

} // namespace ws

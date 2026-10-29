#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace ws::hotkeysteps {

// The clipboard's hotkey held after it opened the window, as Ditto's "Move
// selection on open hot key" (GitHub #107, Settings::clipboardHoldToStep):
// its key pressed again while its modifiers stay down moves the selection one
// row older; letting go of the whole hotkey pastes that row, as Enter would.
// Held a second (kHoldMs) without a step, the first row. Esc while held:
// nothing pasted. A press and a quick release opens the window as ever.
//
// Two halves, both pure: Watch decides on the hook's thread, key by key and
// at once (HoldWatch), and tells the GUI thread what it saw (Note); Hold
// takes those notes and the hotkey's own presses (WM_HOTKEY) in the order
// they came on the GUI thread and says what each comes to (App). A press is
// a step only if the hook said so just before its WM_HOTKEY: the note is
// posted inside the hook, before Windows matches the hotkey, so the two
// arrive in order however late the GUI thread gets to them (measured: 246
// of 246 presses, bursts and a GUI thread stalled 250 ms included). No clock
// is compared for that: a WM_HOTKEY's time is up to a tick (16 ms) later.

// RegisterHotKey's modifier bits (MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN).
inline constexpr unsigned kAlt = 0x1;
inline constexpr unsigned kCtrl = 0x2;
inline constexpr unsigned kShift = 0x4;
inline constexpr unsigned kWin = 0x8;
inline constexpr unsigned kModifiers = kAlt | kCtrl | kShift | kWin;

// The opening press handled later than this after it was made: whether its
// keys stayed down in between is not known (GetAsyncKeyState cannot tell a
// key let go and pressed again), so no hold.
inline constexpr std::uint64_t kArmWithinMs = 50;
// The whole hotkey held this long from the opening press with no step:
// letting go pastes the first row (Ditto waits 20 ticks of 50 ms).
inline constexpr std::uint64_t kHoldMs = 1000;
// All the keys up, as GetAsyncKeyState has it, and no word from the hook
// for this long: Windows dropped the hook (timed out, the secure desktop).
// The hold ends without a paste.
inline constexpr std::uint64_t kSilentMs = 150;
// How often the GUI thread looks at the keys itself (Hold::poll).
inline constexpr std::uint64_t kPollMs = 50;
// A watch done with stays this long at most, for the release of an Esc it
// took (HoldWatch::drain); held longer, that release is the program's.
inline constexpr std::uint64_t kDrainCapMs = 2000;
// A press of a key the hook has as down, this long after its last press or
// repeat, is a press: its release went by unseen (the secure desktop, a
// hook before this one). Repeats come within the keyboard delay (≤ 1 s).
inline constexpr std::uint32_t kRepeatWithinMs = 1500;

inline constexpr unsigned kVkEscape = 0x1B;

// Which modifier a key is, either side (the generic code as the left side).
constexpr unsigned modifierOf(unsigned vk) noexcept
{
    switch (vk) {
    case 0x10: // VK_SHIFT
    case 0xA0: // VK_LSHIFT
    case 0xA1: // VK_RSHIFT
        return kShift;
    case 0x11: // VK_CONTROL
    case 0xA2:
    case 0xA3:
        return kCtrl;
    case 0x12: // VK_MENU
    case 0xA4:
    case 0xA5:
        return kAlt;
    case 0x5B: // VK_LWIN
    case 0x5C:
        return kWin;
    default:
        return 0;
    }
}

// The row a step goes to from the one the hold is on (`from`; -1: its entry
// is gone from the list), of `count` rows: the next older one, the last row
// staying put; -1 with no rows.
constexpr int stepRow(int from, int count) noexcept
{
    if (count <= 0)
        return -1;
    return from < 0 ? 0 : (from + 1 < count ? from + 1 : count - 1);
}

// The eight sides of the four modifiers; a modifier is held while either
// of its sides is down (left Ctrl let go, right Ctrl still down: held).
class Sides {
public:
    void set(unsigned vk, bool down) noexcept
    {
        const unsigned bit = sideBit(vk);
        m_down = down ? (m_down | bit) : (m_down & ~bit);
    }
    // The modifiers (k... bits) with a side down.
    unsigned held() const noexcept
    {
        unsigned modifiers = 0;
        for (const auto& [modifier, sides] : kPairs) {
            if (m_down & sides)
                modifiers |= modifier;
        }
        return modifiers;
    }

private:
    static constexpr unsigned sideBit(unsigned vk) noexcept
    {
        switch (vk) {
        case 0x10:
        case 0xA0:
            return 1u << 0;
        case 0xA1:
            return 1u << 1;
        case 0x11:
        case 0xA2:
            return 1u << 2;
        case 0xA3:
            return 1u << 3;
        case 0x12:
        case 0xA4:
            return 1u << 4;
        case 0xA5:
            return 1u << 5;
        case 0x5B:
            return 1u << 6;
        case 0x5C:
            return 1u << 7;
        default:
            return 0;
        }
    }
    static constexpr std::array<std::pair<unsigned, unsigned>, 4> kPairs {
        {{kShift, 0x03}, {kCtrl, 0x0C}, {kAlt, 0x30}, {kWin, 0xC0}}};
    unsigned m_down = 0;
};

// What the hook saw, for the GUI thread (in the order of the keys).
enum class Note : std::uint8_t {
    None,
    Step, // the hotkey's key pressed again, its modifiers down since the opening press
    Fresh, // ... pressed again after one of them was let go
    Lifted, // the whole hotkey no longer down: its key or one of its modifiers let go (once)
    AllUp, // the key and its modifiers all up (once): the hold is over
    Escape, // Esc pressed while it was held (taken from the program)
    Other, // another key pressed: the hold means nothing any more (once)
};

// The hook's side (HoldWatch's thread): every key event in the system, in
// order, from the opening press of the hotkey (`mask`, `vk`) on.
class Watch {
public:
    struct Answer {
        bool take = false; // kept from the program and the hooks before this one
        Note note = Note::None;
        Note then = Note::None; // a second one, after `note` (Lifted, then AllUp)
    };

    // `sides`: the modifiers' sides down as the hook went in
    // (GetAsyncKeyState); `keyDown`: the hotkey's key too.
    Watch(unsigned mask, unsigned vk, Sides sides, bool keyDown, std::uint32_t now) noexcept
        : m_mask(mask & kModifiers)
        , m_vk(vk)
        , m_sides(sides)
        , m_keyDown(keyDown)
        , m_keyAt(now)
    {
    }

    // Whether it is a hold at all: the modifiers all still down as it began.
    bool holding() const noexcept { return m_mask != 0 && (m_sides.held() & m_mask) == m_mask; }
    // As it begins: Lifted if the key was let go already (a quick press),
    // which is then the whole hotkey no longer down.
    Note start() noexcept { return m_keyDown ? Note::None : lift(); }
    // Nothing more to tell: the GUI thread is done with the hold. The
    // release of an Esc taken is still taken.
    void stop() noexcept { m_done = true; }
    bool holdsTaken() const noexcept { return m_escTaken; }

    Answer key(unsigned vk, bool down, std::uint32_t time) noexcept
    {
        Answer answer;
        // 0xFF: the extra part of an escaped key sequence; 0xE8: unassigned,
        // what programs (AutoHotkey) send to mask a Win or Alt. Not keys.
        if (vk == 0 || vk == 0xFF || vk == 0xE8)
            return answer;
        if (const unsigned modifier = modifierOf(vk)) {
            m_sides.set(vk, down);
            if (!down && !m_done && (m_mask & modifier) && !(m_sides.held() & modifier)) {
                m_broken = true;
                answer.note = lift();
                answer.then = finish();
                if (answer.note == Note::None)
                    std::swap(answer.note, answer.then);
            }
            return answer;
        }
        if (vk == kVkEscape) {
            if (!down) {
                answer.take = std::exchange(m_escTaken, false);
                return answer;
            }
            if (m_escTaken) { // its repeat
                answer.take = true;
                return answer;
            }
            if (m_done)
                return answer;
            // Ctrl+Esc, Alt+Esc, Win+Esc are Windows' (Start, the next
            // window, Magnifier): taken, they mean the window's Esc.
            m_escTaken = true;
            answer.take = true;
            answer.note = Note::Escape;
            return answer;
        }
        if (vk == m_vk) {
            if (!down) {
                m_keyDown = false;
                if (!m_done) {
                    answer.note = lift();
                    answer.then = finish();
                    if (answer.note == Note::None)
                        std::swap(answer.note, answer.then);
                }
                return answer;
            }
            const bool repeat = m_keyDown && time - m_keyAt < kRepeatWithinMs;
            m_keyDown = true;
            m_keyAt = time;
            if (m_done)
                return answer;
            if (repeat) {
                // Held: with the modifiers, Windows keeps the repeats for the
                // hotkey (MOD_NOREPEAT, nothing reaches the program); with
                // some let go they would type into it (`) or be another
                // chord (Ctrl+V of Ctrl+Shift+V). Not while it is held.
                answer.take = true;
                return answer;
            }
            // Exactly the hotkey's modifiers, or it is another chord.
            if (m_sides.held() == m_mask)
                answer.note = m_broken ? Note::Fresh : Note::Step;
            return answer;
        }
        if (down && !m_done) {
            m_done = true;
            answer.note = Note::Other;
        }
        return answer;
    }

private:
    Note lift() noexcept { return std::exchange(m_lifted, true) ? Note::None : Note::Lifted; }
    Note finish() noexcept
    {
        if (m_done || m_keyDown || (m_sides.held() & m_mask))
            return Note::None;
        m_done = true;
        return Note::AllUp;
    }

    unsigned m_mask;
    unsigned m_vk;
    Sides m_sides;
    bool m_keyDown;
    std::uint32_t m_keyAt; // its last press or repeat (the hook's clock)
    bool m_broken = false; // one of the modifiers was let go
    bool m_lifted = false;
    bool m_done = false;
    bool m_escTaken = false; // its release is taken too
};

// The GUI thread's side of one hold (App), on the 64-bit tick clock.
class Hold {
public:
    explicit Hold(std::uint64_t openedAt) noexcept
        : m_openedAt(openedAt)
    {
    }

    enum class Press {
        Normal, // what pressing it again does without this (close it, or raise it)
        Step, // one row older; the hint shows
    };
    // The hotkey's WM_HOTKEY: a step only if the hook told of it just now.
    Press press() noexcept
    {
        if (std::exchange(m_stepNoted, false) && !m_over && !m_cancelled) {
            ++m_steps;
            return Press::Step;
        }
        return Press::Normal;
    }

    enum class Outcome {
        None,
        Paste, // the row stepped to (or the first) goes in, as Enter
        Cancel, // nothing pasted; the window stays as it is
        Close, // as the window's Esc: nothing was waiting to be pasted
        End, // the hold is over, nothing pasted, the window stays
    };
    // `at`: the key's time (the hook's, on this clock).
    Outcome note(Note note, std::uint64_t at) noexcept
    {
        if (m_over)
            return Outcome::None;
        m_stepNoted = note == Note::Step;
        switch (note) {
        case Note::None:
        case Note::Step:
        case Note::Fresh:
            return Outcome::None;
        case Note::Lifted:
            if (!m_liftedAt)
                m_liftedAt = at;
            return Outcome::None;
        case Note::AllUp:
            if (!m_liftedAt)
                m_liftedAt = at;
            m_over = true;
            return !m_cancelled && pending(at) ? Outcome::Paste : Outcome::End;
        case Note::Escape:
            if (!m_cancelled && pending(at)) {
                m_cancelled = true; // the next one closes it
                return Outcome::Cancel;
            }
            m_over = true;
            return Outcome::Close;
        case Note::Other:
            m_over = true;
            return Outcome::End;
        }
        return Outcome::None;
    }

    // The GUI thread's own look at the keys every kPollMs (GetAsyncKeyState),
    // for what the hook may never tell: how much of the hotkey is down.
    enum class Held {
        None,
        Part,
        Whole, // its key and all its modifiers
    };
    enum class Poll {
        None,
        Hint, // held a second, no step: letting go pastes the first entry (once)
        Silent, // all of it up kSilentMs, no word from the hook: Windows dropped it (timed out, the secure desktop)
    };
    Poll poll(Held held, std::uint64_t now) noexcept
    {
        if (m_over)
            return Poll::None;
        if (held != Held::None)
            m_upSince.reset();
        else if (!m_upSince)
            m_upSince = now;
        else if (now >= *m_upSince + kSilentMs)
            return Poll::Silent;
        if (held == Held::Whole && m_steps == 0 && !m_hinted && hint(now)) {
            m_hinted = true;
            return Poll::Hint;
        }
        return Poll::None;
    }

    // Letting go would paste: a step made, or held a second.
    bool hint(std::uint64_t now) const noexcept { return !m_over && !m_cancelled && pending(now); }
    bool cancelled() const noexcept { return m_cancelled; }
    int steps() const noexcept { return m_steps; }
    // How long the whole hotkey was down from the opening press, by `now`.
    std::uint64_t heldFor(std::uint64_t now) const noexcept
    {
        const std::uint64_t until = m_liftedAt.value_or(now);
        return until > m_openedAt ? until - m_openedAt : 0;
    }

private:
    bool pending(std::uint64_t now) const noexcept { return m_steps > 0 || heldFor(now) >= kHoldMs; }

    std::uint64_t m_openedAt;
    std::optional<std::uint64_t> m_liftedAt;
    std::optional<std::uint64_t> m_upSince; // poll: all of it up since
    int m_steps = 0;
    bool m_stepNoted = false;
    bool m_cancelled = false;
    bool m_over = false;
    bool m_hinted = false; // poll: Hint told
};

} // namespace ws::hotkeysteps

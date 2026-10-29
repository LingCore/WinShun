#pragma once

#include "OwnInput.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ws::pastekeys {

// The keys that paste into another program: Ctrl+V, or Shift+Insert in the
// terminals that take no Ctrl+V (mintty, PuTTY). They go some 80–90 ms after
// the keys that asked for the paste are let go of (the window goes, the
// clipboard settles), and the user may be pressing modifiers again by then:
// letting go of the clipboard's hotkey pastes, pressing it again opens the
// next (GitHub #107).
//
// Unwatched, with the paste's own modifier held already: the key alone (ours
// pressed and let go of would let go of the hand's too). A key cannot be
// pressed back blindly: had the hand let go of it meanwhile, it would stay
// down. Another modifier held stays in the chord.
//
// Watched by a hook of Win顺's own as they go, in order with the user's keys
// (HoldWatch, the paste a hold ends with; Guard), the modifiers stay as the
// hand has them:
// - another one held (Alt, Win, Shift; Ctrl for Shift+Insert) is lifted for
//   the paste and pressed back after, else the program would get Ctrl+Alt+V
//   (Ctrl+Shift+V the clipboard's hotkey again). The masking key first: let
//   go of alone, an Alt opens the menu bar, a Windows key Start, a Shift
//   switches the input method. Let go of by the hand meanwhile, it is not
//   pressed back;
// - the paste's own is pressed even if held (on the side held); its let-go
//   goes only if the hand has it up by then (pressed meanwhile: it stays
//   down), and a let-go of the hand's while ours is down waits for ours,
//   after the key.
enum class Keys {
    CtrlV,
    ShiftInsert,
};

inline constexpr std::uint16_t kShift = 0x10; // VK_SHIFT
inline constexpr std::uint16_t kControl = 0x11; // VK_CONTROL
inline constexpr std::uint16_t kMenu = 0x12; // VK_MENU
inline constexpr std::uint16_t kInsert = 0x2D; // VK_INSERT
inline constexpr std::uint16_t kV = 0x56;

// The eight sides of the four modifiers, as the bits of `held`: left and
// right Shift, Ctrl, Alt, then the Windows keys (VK_LSHIFT … VK_RWIN).
inline constexpr std::array<std::uint16_t, 8> kSides {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0x5B, 0x5C};
inline constexpr unsigned kAllSides = 0xFF;
inline constexpr unsigned kExtendedSides = 0xE8; // the right Ctrl and Alt, the Windows keys

struct Key {
    std::uint16_t vk = 0; // a virtual-key code
    bool up = false;
    bool extended = false; // Insert (not the number pad's 0), the right Ctrl and Alt, the Windows keys
    // A modifier lifted or pressed back, the masking key: kOwnMask, which the
    // double-tap listeners pass over (the hand's Alt is still one press).
    // The paste's own keys: kOwnInput.
    bool mask = false;
    bool restores = false; // puts its side back as the hand has it: goes only if the hand still has it so (Guard)

    friend constexpr bool operator==(const Key&, const Key&) = default;
};

// The side of a modifier (VK_LCONTROL for VK_CONTROL, VK_RMENU for an
// extended VK_MENU); other keys as they are. Hooks are told the side; a
// program may send the plain modifier. The right Shift is no extended key:
// its scan code tells.
constexpr std::uint16_t sided(std::uint16_t vk, bool extended, std::uint16_t scan = 0) noexcept
{
    switch (vk) {
    case kShift:
        return scan == 0x36 ? 0xA1 : 0xA0;
    case kControl:
        return extended ? 0xA3 : 0xA2;
    case kMenu:
        return extended ? 0xA5 : 0xA4;
    default:
        return vk;
    }
}

// The bit of a side (sided) in `held`; 0 for any other key.
constexpr unsigned sideBit(std::uint16_t side) noexcept
{
    for (std::size_t i = 0; i < kSides.size(); ++i) {
        if (kSides[i] == side)
            return 1u << i;
    }
    return 0;
}

// The sides of the paste's own modifier.
constexpr unsigned ownSides(Keys keys) noexcept
{
    return keys == Keys::ShiftInsert ? 0x03u : 0x0Cu;
}

// The keys to send; `held`: the sides down right before; `watched`: they
// pass Guard's hook.
inline std::vector<Key> sequence(Keys keys, unsigned held, bool watched)
{
    const bool insert = keys == Keys::ShiftInsert;
    const unsigned own = ownSides(keys);
    const unsigned lifted = watched ? held & kAllSides & ~own : 0;
    const bool press = watched || !(held & own);
    const auto side = [](std::size_t i, bool up, bool restores) {
        return Key {.vk = kSides[i], .up = up, .extended = (kExtendedSides >> i & 1u) != 0, .mask = true, .restores = restores};
    };
    std::vector<Key> out;
    if (lifted) {
        out.push_back({.vk = kMaskKey, .mask = true});
        out.push_back({.vk = kMaskKey, .up = true, .mask = true});
    }
    for (std::size_t i = 0; i < kSides.size(); ++i) {
        if (lifted >> i & 1u)
            out.push_back(side(i, true, false));
    }
    // Ours on the side the hand holds (the left one if neither), which it
    // stands for while the key goes (Guard).
    Key modifier {.vk = insert ? kShift : kControl};
    if ((held & own) == (insert ? 0x02u : 0x08u))
        modifier = insert ? Key {.vk = 0xA1} : Key {.vk = 0xA3, .extended = true};
    const std::uint16_t key = insert ? kInsert : kV;
    if (press)
        out.push_back(modifier);
    out.push_back({.vk = key, .extended = insert});
    out.push_back({.vk = key, .up = true, .extended = insert});
    if (press) {
        modifier.up = true;
        modifier.restores = true;
        out.push_back(modifier);
    }
    for (std::size_t i = kSides.size(); i-- > 0;) {
        if (lifted >> i & 1u)
            out.push_back(side(i, false, true));
    }
    return out;
}

// The paste keys as they reach the hook, in order with the user's keys (the
// hooks are called one key at a time); sides as `sided` gives them.
class Guard {
public:
    // `user`: the sides down as the hook went in.
    explicit Guard(unsigned user = 0) noexcept
        : m_user(user)
    {
    }

    // A key of the user's that the hooks before this one passed on. True: it
    // is kept from the program, a let-go of a side the paste holds down (its
    // own let-go, after its key, goes for both).
    bool user(std::uint16_t side, bool up) noexcept
    {
        const unsigned bit = sideBit(side);
        m_user = up ? (m_user & ~bit) : (m_user | bit);
        return up && (m_holding & bit) != 0;
    }

    // One of Win顺's own; `keys`: the paste's as they went. False: kept from
    // everyone, a key that would put its side back as the hand no longer has
    // it. Keys not the paste's next (KeyRouter's mask) pass as they are.
    bool own(std::span<const Key> keys, std::uint16_t side, bool up) noexcept
    {
        if (m_next >= keys.size())
            return true;
        const Key& key = keys[m_next];
        if (sided(key.vk, key.extended) != side || key.up != up)
            return true;
        ++m_next;
        const unsigned bit = sideBit(side);
        m_judged = key.restores ? bit : 0;
        m_judgedUp = up;
        if (!key.restores) {
            if (!up && !key.mask)
                m_holding |= bit; // the paste's own modifier, until its let-go
            return true;
        }
        m_holding &= ~bit;
        return agrees();
    }

    // The last paste key own() let go on, once the hooks before this one are
    // through with it: the hand's keys can come in meanwhile (input sent
    // while the hook waits on theirs). False: kept from the program after
    // all, it would no longer put its side as the hand has it.
    bool agrees() const noexcept
    {
        return !m_judged || m_judgedUp != ((m_user & m_judged) != 0);
    }

    // How many of the paste's keys reached the hook.
    std::size_t passed() const noexcept { return m_next; }
    // Those still to come lost (fewer went): nothing of the hand's kept back.
    void finish() noexcept { m_holding = 0; }

private:
    unsigned m_user;
    unsigned m_holding = 0; // pressed by the paste, not let go of yet
    unsigned m_judged = 0; // the side the last paste key puts back (own)
    bool m_judgedUp = false;
    std::size_t m_next = 0;
};

} // namespace ws::pastekeys

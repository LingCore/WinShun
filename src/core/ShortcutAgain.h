#pragma once

#include <cstdint>
#include <optional>

namespace ws {

// What a press of a shortcut that opens Win顺 (the custom key, a double tap)
// does, given where the window stands. Pure logic, so it is unit-tested.
// Raise: up already, but not yet there to be seen when the key was pressed
// (seenBy): to the front as it is, neither placed again nor closed.
enum class ShortcutAction { Show, Hide, ExpandPanel, Raise };

// A message's time (GetMessageTime, a Raw Input key's: 32 bits of
// milliseconds, wrapping after 49.7 days) on the 64-bit clock it is the low
// half of (GetTickCount64), given that clock now. The message was made less
// than 49.7 days ago.
constexpr std::uint64_t tickTime(std::uint32_t messageTime, std::uint64_t now) noexcept
{
    return now - static_cast<std::uint32_t>(static_cast<std::uint32_t>(now) - messageTime);
}

// Whether a window was there to be seen when a key that toggles it was
// pressed (both on the 64-bit tick clock): revealed, on the screen and not
// cloaked waiting for its first frame, at least kSeenBeforeMs before. A
// press that waited behind a slow opening, made again because nothing came,
// is not "pressed again" (GitHub #111: the second double Ctrl closed the
// launcher the first one had just opened; the user never saw it). A double
// tap counts from its first press (DoubleTapDetector::firstPressTime): one
// begun just before the window came ends some 200–300 ms later, after it.
// Nobody reacts faster than kSeenBeforeMs to a window appearing.
inline constexpr std::uint64_t kSeenBeforeMs = 200;

constexpr bool seenBy(std::optional<std::uint64_t> revealedAt, std::uint64_t pressedAt) noexcept
{
    return revealedAt && pressedAt >= *revealedAt && pressedAt - *revealedAt >= kSeenBeforeMs;
}

// shown: up, and not on its way out; active: has the keyboard focus;
// expandOnAgain: the setting "when pressed again" says the search panel;
// seen: up before the press (seenBy).
inline ShortcutAction shortcutAction(bool shown, bool active, bool panel, bool expandOnAgain, bool seen = true)
{
    if (shown && !seen)
        return ShortcutAction::Raise; // asked for again while still coming: it comes, and stays
    if (!shown || !active)
        return ShortcutAction::Show; // behind another window: to the front, as it is
    return expandOnAgain && !panel ? ShortcutAction::ExpandPanel : ShortcutAction::Hide;
}

} // namespace ws

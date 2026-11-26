#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

namespace ws {

// Whether a double Ctrl should leave the window in front alone. Games use
// Ctrl to crouch, and players press it twice in a row; opening Win顺 then
// takes the keyboard from the game, and one in exclusive full screen drops
// out of it. Pure logic (the facts come from platform/Foreground), so it is
// unit-tested.
struct ForegroundFacts {
    QString program; // "TheFinals.exe"; empty for Win顺's own windows and the desktop
    bool coversMonitor = false; // all of the monitor it is on, and not just maximized
    bool exclusiveFullScreen = false; // Direct3D's own full screen
    bool cursorHidden = false; // by the program (not by Windows, for touch)
    bool cursorConfined = false; // kept to less than the whole desktop (ClipCursor)
    bool cursorCaptured = false; // a window of the program has the mouse (SetCapture)
    // Hidden by Windows on a key press, its "hide pointer while typing"
    // (hiddenForTyping), and not moved since; it comes back as soon as the
    // mouse moves.
    bool cursorHiddenByTyping = false;
};

// Windows' "hide pointer while typing" (SPI_GETMOUSEVANISH) hides it on a
// key press in whatever window has the focus; a program hides it with
// ShowCursor(FALSE) or SetCursor(nullptr). Both look the same afterwards
// (GetCursorInfo: not showing, no cursor) and Windows tells both alike
// (EVENT_OBJECT_HIDE for OBJID_CURSOR on the program's thread, measured
// 2026-10-27), but Windows' comes as the program handles a key press: on
// the press's tick of the clock or the next (0–15 ms, measured). So a hide
// counts as Windows' when a key other than a modifier was pressed in this
// foreground window shortly before it, with the setting on. Not when the
// hide came first in this window (a game hides it as it comes to the front,
// often right after Alt+Tab, whose keys went down before the switch, or
// Win+1, on the very tick of it), nor after the mouse: a game that hides it
// at a click. Times on the 32-bit tick clock, `keyAt` the last press at or
// before `hideAt`: a key's first, not its repeats while held (KeyPresses).
// A game played with the keys, hiding the pointer itself while one was
// held, passed for typing on the repeat just before (GitHub #111).
//
// kTypingHideMs leaves a program a few frames behind with the key (a busy
// page or editor, an input method that takes it first); a game that hides
// the pointer itself passes only on a key pressed anew within it.
inline constexpr std::uint32_t kTypingHideMs = 100;

constexpr bool hiddenForTyping(bool vanishOn, std::uint32_t hideAt, std::optional<std::uint32_t> keyAt,
    std::uint32_t foregroundAt) noexcept
{
    if (!vanishOn || !keyAt)
        return false;
    const auto sinceKey = static_cast<std::int32_t>(hideAt - *keyAt);
    const auto keyInWindow = static_cast<std::int32_t>(*keyAt - foregroundAt);
    return keyInWindow > 0 && sinceKey >= 0 && static_cast<std::uint32_t>(sinceKey) <= kTypingHideMs;
}

struct GameGuardOptions {
    bool games = true; // not while a game is played
    bool fullScreen = false; // not over anything full screen: videos, slide shows, F11 pages
    QStringList programs; // not over these program files, "TheFinals.exe"
};

enum class IgnoreReason { None, Listed, FullScreen, ExclusiveFullScreen, MouseTaken };

// A real application covering its monitor. The shell and our own windows
// have no program in ForegroundFacts and must remain usable (GitHub #121).
inline bool fullScreenShortcutBlocked(const ForegroundFacts& facts, bool enabled) noexcept
{
    return enabled && !facts.program.isEmpty() && facts.coversMonitor;
}

inline IgnoreReason doubleCtrlIgnoreReason(const ForegroundFacts& facts, const GameGuardOptions& options)
{
    if (facts.program.isEmpty())
        return IgnoreReason::None;
    if (options.programs.contains(facts.program, Qt::CaseInsensitive))
        return IgnoreReason::Listed;
    if (fullScreenShortcutBlocked(facts, options.fullScreen))
        return IgnoreReason::FullScreen;
    if (!options.games)
        return IgnoreReason::None;
    if (facts.exclusiveFullScreen)
        return IgnoreReason::ExclusiveFullScreen;
    // Turning the view with the mouse: the cursor hidden, and kept in the
    // window (confined or captured), or hidden by a program that fills the
    // screen. Whether it is a game does not matter, that the mouse is taken
    // does. A full-screen video that has hidden the cursor counts too;
    // moving the mouse brings it back. Not one Windows hid on a key press
    // ("hide pointer while typing"): typing in a page or an editor at full
    // screen, double Ctrl did nothing (GitHub #111).
    if (facts.cursorHidden
        && (facts.cursorConfined || facts.cursorCaptured || (facts.coversMonitor && !facts.cursorHiddenByTyping)))
        return IgnoreReason::MouseTaken;
    return IgnoreReason::None;
}

// The clipboard opens over the program in front without taking its focus,
// and goes as a menu when another program comes to the front. A game in
// exclusive full screen cannot have a window over it. What Windows does with
// one shown there without the focus differs: Windows 11 takes the game out of
// full screen, Windows 10 22H2 leaves it, the panel up unseen with the game's
// keys. So over one the panel opens with the focus, which takes the game out
// of full screen everywhere (GitHub #452). The shell and Win顺's own windows
// have no program in ForegroundFacts.
inline bool panelTakesFocus(const ForegroundFacts& over) noexcept
{
    return over.exclusiveFullScreen && !over.program.isEmpty();
}

// Where the panel did open without the focus over a full-screen window (one
// covering its monitor that Direct3D's state did not tell), that window may
// still leave full screen as the panel shows and minimize itself, and whatever
// was behind it comes to the front. That is the panel's own doing, not its
// user going elsewhere: it stays, with the focus.
//
// Told from a switch of programs (Alt+Tab, Win+D out of a full-screen game
// minimize it too) by the keys: none pressed since it opened. And soon: the
// change of display mode on the way out takes a second or two at most.
struct PanelFrontMove {
    bool overFullScreen = false; // it opened over a window covering its monitor, or in Direct3D's full screen
    bool targetMinimized = false; // that window, as the front moved
    bool keyPressed = false; // a key other than a modifier since it opened
    std::uint64_t shownMs = 0; // since it opened
};

inline constexpr std::uint64_t kFullScreenExitMs = 3000;

constexpr bool leftFullScreenForPanel(const PanelFrontMove& move) noexcept
{
    return move.overFullScreen && move.targetMinimized && !move.keyPressed && move.shownMs <= kFullScreenExitMs;
}

} // namespace ws

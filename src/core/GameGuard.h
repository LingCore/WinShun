#pragma once

#include <QString>
#include <QStringList>

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
};

struct GameGuardOptions {
    bool games = true; // not while a game is played
    bool fullScreen = false; // not over anything full screen: videos, slide shows, F11 pages
    QStringList programs; // not over these program files, "TheFinals.exe"
};

enum class IgnoreReason { None, Listed, FullScreen, ExclusiveFullScreen, MouseTaken };

inline IgnoreReason doubleCtrlIgnoreReason(const ForegroundFacts& facts, const GameGuardOptions& options)
{
    if (facts.program.isEmpty())
        return IgnoreReason::None;
    if (options.programs.contains(facts.program, Qt::CaseInsensitive))
        return IgnoreReason::Listed;
    if (options.fullScreen && facts.coversMonitor)
        return IgnoreReason::FullScreen;
    if (!options.games)
        return IgnoreReason::None;
    if (facts.exclusiveFullScreen)
        return IgnoreReason::ExclusiveFullScreen;
    // Turning the view with the mouse: the cursor hidden, and kept in the
    // window or under a game that fills the screen. Whether it is a game
    // does not matter, that the mouse is taken does. A full-screen video
    // that has hidden the cursor counts too; moving the mouse brings it back.
    if (facts.cursorHidden && (facts.cursorConfined || facts.coversMonitor))
        return IgnoreReason::MouseTaken;
    return IgnoreReason::None;
}

} // namespace ws

#pragma once

// A quiet word, once, that a double tap cannot open Win顺 over a program that
// runs as administrator. Windows (UIPI) gives the keys typed while such a
// window is in front to no Raw Input of a process with normal rights, so a
// double Ctrl (Alt, Shift) there does nothing and says nothing; only the
// index service's UIAccess listener hears it (docs/pitfalls.md, GitHub #31),
// or Win顺 run as administrator itself. A hotkey with a modifier still comes
// (RegisterHotKey, measured 2026-10-22). What is said, and when: here, pure,
// for tst_admintaphint; App gathers the facts (App::adminTapFront).

#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

namespace ws::admintap {

// SECURITY_MANDATORY_HIGH_RID: a program run as administrator. Not the
// system's (0x4000: dwm.exe's "Ghost" of a window that stopped responding),
// nor one whose level cannot be read.
inline constexpr std::uint32_t kAdministratorLevel = 0x3000;
// Programs told of, at most: after these the user has heard it.
inline constexpr int kMostPrograms = 3;

// Who hears the double tap over an administrator's window.
enum class Listener : std::uint8_t {
    Hears, // the service's UIAccess listener tells of that key, or Win顺 runs as administrator
    Old, // the service's listener answers, but is from before that key (double Alt or Shift)
    Connecting, // the service runs (or is starting) and its listener has not answered yet
    Off, // the service is installed but stopped or disabled
    None, // no service on this PC
};

struct Facts {
    bool doubleTap = false; // Launcher/DoubleCtrl
    Listener listener = Listener::None;
    bool settled = false; // the key listener has had time for the service listener's word
    bool trayShown = false; // a notification has somewhere to show
    std::optional<std::uint32_t> level; // of the window in front (SECURITY_MANDATORY_*_RID)
    QString program; // its program file, "regedit.exe"; empty when it cannot be told
    bool ignoredThere = false; // a double tap does nothing there anyway (GameGuard: listed, full screen, a game)
    bool shownThisRun = false; // one told of already since Win顺 started
    QStringList shown; // the programs told of before (Launcher/AdminTapHinted)
};

enum class Verdict : std::uint8_t {
    Show,
    Off, // no double tap
    Heard, // someone hears it there
    NotAdministrator, // the window in front is not an administrator's program
    Unknown, // which program it is cannot be told
    Ignored, // a double tap would do nothing there anyway
    Wait, // the service's listener may still answer: asked again at the next change of the front
    ThisRun, // one already since Win顺 started
    Again, // this program was told of before
    Enough, // kMostPrograms told of
    NoTray, // nowhere to show it: asked again later
};

inline bool told(const QStringList& shown, const QString& program)
{
    return shown.contains(program, Qt::CaseInsensitive);
}

inline Verdict decide(const Facts& f)
{
    if (!f.doubleTap)
        return Verdict::Off;
    if (f.listener == Listener::Hears)
        return Verdict::Heard;
    if (f.level != kAdministratorLevel)
        return Verdict::NotAdministrator;
    if (f.program.isEmpty())
        return Verdict::Unknown;
    if (f.ignoredThere)
        return Verdict::Ignored;
    if (told(f.shown, f.program))
        return Verdict::Again;
    if (f.shown.size() >= kMostPrograms)
        return Verdict::Enough;
    if (f.shownThisRun)
        return Verdict::ThisRun;
    if (f.listener == Listener::Connecting && !f.settled)
        return Verdict::Wait;
    if (!f.trayShown)
        return Verdict::NoTray;
    return Verdict::Show;
}

// What would make the double tap work there too, besides a hotkey: the
// service installed, or installed again over an old one (the 索引方式 row's
// button does both). Only for an account that may (canInstall: an
// administrator's, with WinShunService.exe beside Win顺); none for a
// service that is there and stopped, disabled or still connecting.
enum class Fix : std::uint8_t { None, Install, Update };

inline Fix fixFor(Listener listener, bool canInstall)
{
    if (!canInstall)
        return Fix::None;
    if (listener == Listener::None)
        return Fix::Install;
    if (listener == Listener::Old)
        return Fix::Update;
    return Fix::None;
}

// The words and the click: the hotkey named when one works over such a
// window (registered, with a modifier), else where to set one; the service
// when it would help. A click opens the settings at the row that helps most:
// the service's (it makes the double tap itself work), else the hotkey's;
// with the hotkey named and nothing to install, nothing.
enum class Click : std::uint8_t { None, Service, Hotkey };

struct Advice {
    bool hotkey = false; // name the user's hotkey
    Fix fix = Fix::None;
    Click click = Click::None;
};

inline Advice advise(bool hotkeyWorks, Fix fix)
{
    Advice a;
    a.hotkey = hotkeyWorks;
    a.fix = fix;
    a.click = fix != Fix::None ? Click::Service : !hotkeyWorks ? Click::Hotkey : Click::None;
    return a;
}

// The list kept, with `program` told of now (lower case: compared so).
inline QStringList remember(QStringList shown, const QString& program)
{
    if (!program.isEmpty() && !told(shown, program))
        shown.push_back(program.toLower());
    return shown;
}

// For the log and the diagnostics journal.
inline const char* name(Verdict v)
{
    switch (v) {
    case Verdict::Show: return "show";
    case Verdict::Off: return "off";
    case Verdict::Heard: return "heard";
    case Verdict::NotAdministrator: return "not-administrator";
    case Verdict::Unknown: return "unknown-program";
    case Verdict::Ignored: return "ignored-there";
    case Verdict::Wait: return "wait";
    case Verdict::ThisRun: return "this-run";
    case Verdict::Again: return "again";
    case Verdict::Enough: return "enough";
    case Verdict::NoTray: return "no-tray";
    }
    return "?";
}

inline const char* name(Listener l)
{
    switch (l) {
    case Listener::Hears: return "hears";
    case Listener::Old: return "old";
    case Listener::Connecting: return "connecting";
    case Listener::Off: return "off";
    case Listener::None: return "none";
    }
    return "?";
}

inline const char* name(Fix f)
{
    switch (f) {
    case Fix::None: return "none";
    case Fix::Install: return "install";
    case Fix::Update: return "update";
    }
    return "?";
}

} // namespace ws::admintap

#pragma once

// A quiet word, once, that Win顺 cannot take a file dialog of a program run
// as administrator anywhere (GitHub #216). Windows (UIPI) keeps a process
// with normal rights from driving such a dialog, so Ctrl+G, going by itself
// and the bar's picks do nothing there but copy the path; only the index
// service's helper can (ServiceDialogNav, #104), for the copy of Win顺 that
// installed it, or Win顺 run as administrator itself. What is said, and
// when: here, pure, for tst_admindialoghint; App gathers the facts
// (App::adminDialogFront), as for the double tap (AdminTapHint.h).

#include <QString>
#include <QStringList>

#include <cstdint>

namespace ws::admindialog {

// Programs told of, at most: after these the user has heard it.
inline constexpr int kMostPrograms = 3;

// Who can drive the dialog.
enum class Reach : std::uint8_t {
    Service, // the service's helper does it for this copy
    OtherCopy, // a service answers, for another copy of Win顺 only
    NoService, // none answers
};

struct Facts {
    bool following = false; // Ctrl+G, going by itself or the bar is on
    Reach reach = Reach::NoService;
    bool serviceInstalled = false; // installed, whether it answers or not
    bool canInstall = false; // the button under 索引方式 is there (a portable copy on an administrator's account)
    bool administrator = false; // the account can run Win顺 as administrator
    QString program; // the dialog's program file, "setup.exe"; empty when it cannot be told
    bool shownThisRun = false; // one told of already since Win顺 started
    bool trayShown = false; // a notification has somewhere to show
    QStringList shown; // the programs told of before (Launcher/AdminDialogHinted)
};

enum class Verdict : std::uint8_t {
    Show,
    Off, // nothing follows dialogs
    Reached, // the service drives it
    Starting, // installed, not answering yet (starting, stopped, an old one): nothing sure to say
    Nothing, // the account can neither install the service nor run as administrator
    Unknown, // which program it is cannot be told
    Again, // this program was told of before
    Enough, // kMostPrograms told of
    ThisRun, // one already since Win顺 started
    NoTray, // nowhere to show it
};

// What would help, which the words say and a click opens.
enum class Fix : std::uint8_t {
    Install, // the service, from the settings (portable)
    Administrator, // Win顺 run as administrator: an installed copy without its service
    OtherCopy, // the service is another copy's: run this one as administrator
};

inline bool told(const QStringList& shown, const QString& program)
{
    return shown.contains(program, Qt::CaseInsensitive);
}

inline Verdict decide(const Facts& f)
{
    if (!f.following)
        return Verdict::Off;
    if (f.reach == Reach::Service)
        return Verdict::Reached;
    if (f.reach == Reach::NoService && f.serviceInstalled)
        return Verdict::Starting;
    if (!f.canInstall && !f.administrator)
        return Verdict::Nothing;
    if (f.program.isEmpty())
        return Verdict::Unknown;
    if (told(f.shown, f.program))
        return Verdict::Again;
    if (f.shown.size() >= kMostPrograms)
        return Verdict::Enough;
    if (f.shownThisRun)
        return Verdict::ThisRun;
    if (!f.trayShown)
        return Verdict::NoTray;
    return Verdict::Show;
}

inline Fix fixFor(const Facts& f)
{
    if (f.reach == Reach::OtherCopy)
        return Fix::OtherCopy;
    return f.canInstall ? Fix::Install : Fix::Administrator;
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
    case Verdict::Reached: return "reached";
    case Verdict::Starting: return "service-starting";
    case Verdict::Nothing: return "nothing-to-suggest";
    case Verdict::Unknown: return "unknown-program";
    case Verdict::Again: return "told-before";
    case Verdict::Enough: return "enough-told";
    case Verdict::ThisRun: return "told-this-run";
    case Verdict::NoTray: return "no-tray";
    }
    return "?";
}

inline const char* name(Fix fix)
{
    switch (fix) {
    case Fix::Install: return "install";
    case Fix::Administrator: return "administrator";
    case Fix::OtherCopy: return "other-copy";
    }
    return "?";
}

} // namespace ws::admindialog

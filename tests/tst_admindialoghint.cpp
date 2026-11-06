#include "AdminDialogHint.h"

#include <QtTest>

using namespace ws::admindialog;
using namespace Qt::StringLiterals;

// The word that Win顺 cannot take a file dialog of a program run as
// administrator anywhere (GitHub #216): shown once per program, a few at
// most, one a run, only when something would help (AdminDialogHint.h).
class AdminDialogHintTest : public QObject {
    Q_OBJECT

    // Everything for a word: a portable copy on an administrator's account,
    // no service, Ctrl+G on, an elevated setup's dialog, nothing told before.
    static Facts showing()
    {
        Facts f;
        f.following = true;
        f.reach = Reach::NoService;
        f.canInstall = true;
        f.administrator = true;
        f.program = u"setup.exe"_s;
        f.trayShown = true;
        return f;
    }

private slots:
    void showsWhenNothingDrivesIt()
    {
        QCOMPARE(decide(showing()), Verdict::Show);
        QCOMPARE(fixFor(showing()), Fix::Install);
        // The service is another copy's: told so, whatever this one could install.
        Facts f = showing();
        f.reach = Reach::OtherCopy;
        f.serviceInstalled = true;
        QCOMPARE(decide(f), Verdict::Show);
        QCOMPARE(fixFor(f), Fix::OtherCopy);
        // An installed copy without its service: run as administrator.
        f = showing();
        f.canInstall = false;
        QCOMPARE(decide(f), Verdict::Show);
        QCOMPARE(fixFor(f), Fix::Administrator);
    }

    void quietWhenItWorksOrNothingHelps()
    {
        Facts f = showing();
        f.following = false;
        QCOMPARE(decide(f), Verdict::Off);
        f = showing();
        f.reach = Reach::Service;
        QCOMPARE(decide(f), Verdict::Reached);
        // Installed, not answering yet: it may in a moment.
        f = showing();
        f.serviceInstalled = true;
        QCOMPARE(decide(f), Verdict::Starting);
        // Neither the service nor administrator's rights for this account.
        f = showing();
        f.canInstall = false;
        f.administrator = false;
        QCOMPARE(decide(f), Verdict::Nothing);
        f = showing();
        f.program.clear();
        QCOMPARE(decide(f), Verdict::Unknown);
        f = showing();
        f.trayShown = false;
        QCOMPARE(decide(f), Verdict::NoTray);
    }

    void oncePerProgramAFewAtMost()
    {
        Facts f = showing();
        f.shown = remember({}, u"SETUP.exe"_s);
        QCOMPARE(f.shown, QStringList {u"setup.exe"_s});
        QCOMPARE(decide(f), Verdict::Again);
        f.shown = {u"a.exe"_s, u"b.exe"_s, u"c.exe"_s};
        QCOMPARE(decide(f), Verdict::Enough);
        f.shown = {u"a.exe"_s};
        f.shownThisRun = true;
        QCOMPARE(decide(f), Verdict::ThisRun);
        // Remembered once, in lower case.
        QCOMPARE(remember({u"setup.exe"_s}, u"Setup.EXE"_s).size(), 1);
        QCOMPARE(remember({}, QString()).size(), 0);
    }
};

QTEST_APPLESS_MAIN(AdminDialogHintTest)
#include "tst_admindialoghint.moc"

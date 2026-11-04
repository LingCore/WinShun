#include "AdminTapHint.h"

#include <QtTest>

using namespace ws::admintap;
using namespace Qt::StringLiterals;

// The word that double Ctrl cannot be heard over an administrator's window:
// shown once per program, a few programs at most, one a run, and only when
// nobody hears the tap there (AdminTapHint.h).
class AdminTapHintTest : public QObject {
    Q_OBJECT

    // Everything for a word: regedit run as administrator in front, no
    // service, nothing told before.
    static Facts showing()
    {
        Facts f;
        f.doubleTap = true;
        f.listener = Listener::None;
        f.settled = true;
        f.trayShown = true;
        f.level = kAdministratorLevel;
        f.elevated = true;
        f.program = u"regedit.exe"_s;
        return f;
    }

private slots:
    void showsWhenNobodyHears()
    {
        QCOMPARE(decide(showing()), Verdict::Show);
        Facts f = showing();
        f.listener = Listener::Off; // installed, stopped
        QCOMPARE(decide(f), Verdict::Show);
        f.listener = Listener::Old; // an old listener, double Shift
        QCOMPARE(decide(f), Verdict::Show);
        // Not waited for once it has had its time.
        f.listener = Listener::Connecting;
        QCOMPARE(decide(f), Verdict::Show);
    }

    void eachConditionAlone_data()
    {
        QTest::addColumn<int>("which");
        QTest::addColumn<int>("verdict");
        QTest::newRow("double tap off") << 0 << int(Verdict::Off);
        QTest::newRow("service listener hears it") << 1 << int(Verdict::Heard);
        QTest::newRow("normal window") << 2 << int(Verdict::NotAdministrator);
        QTest::newRow("system window (dwm Ghost)") << 3 << int(Verdict::NotAdministrator);
        QTest::newRow("level unknown") << 4 << int(Verdict::NotAdministrator);
        QTest::newRow("program unknown") << 5 << int(Verdict::Unknown);
        QTest::newRow("ignored there (game, listed)") << 6 << int(Verdict::Ignored);
        QTest::newRow("told of before") << 7 << int(Verdict::Again);
        QTest::newRow("told of before, other case") << 8 << int(Verdict::Again);
        QTest::newRow("enough programs") << 9 << int(Verdict::Enough);
        QTest::newRow("one this run") << 10 << int(Verdict::ThisRun);
        QTest::newRow("service still connecting") << 11 << int(Verdict::Wait);
        QTest::newRow("no tray icon") << 12 << int(Verdict::NoTray);
        QTest::newRow("high but not elevated (UIAccess: Magnifier)") << 13 << int(Verdict::NotAdministrator);
    }
    void eachConditionAlone()
    {
        QFETCH(int, which);
        QFETCH(int, verdict);
        Facts f = showing();
        switch (which) {
        case 0: f.doubleTap = false; break;
        case 1: f.listener = Listener::Hears; break;
        case 2: f.level = 0x2000; break;
        case 3: f.level = 0x4000; break;
        case 4: f.level.reset(); break;
        case 5: f.program.clear(); break;
        case 6: f.ignoredThere = true; break;
        case 7: f.shown = {u"regedit.exe"_s}; break;
        case 8: f.shown = {u"cmd.exe"_s, u"REGEDIT.EXE"_s}; break;
        case 9: f.shown = {u"a.exe"_s, u"b.exe"_s, u"c.exe"_s}; break;
        case 10: f.shownThisRun = true; break;
        case 11: f.listener = Listener::Connecting; f.settled = false; break;
        case 12: f.trayShown = false; break;
        case 13: f.elevated = false; break;
        }
        QCOMPARE(int(decide(f)), verdict);
    }

    // Win顺 run as administrator hears every key: App says Hears then,
    // whatever the window in front.
    void heardWinsOverEverything()
    {
        Facts f = showing();
        f.listener = Listener::Hears;
        f.level = 0x3000;
        f.shown.clear();
        QCOMPARE(decide(f), Verdict::Heard);
    }

    void remembersOncePerProgram()
    {
        QStringList shown = remember({}, u"Regedit.exe"_s);
        QCOMPARE(shown, QStringList {u"regedit.exe"_s});
        shown = remember(shown, u"REGEDIT.EXE"_s);
        QCOMPARE(shown.size(), 1);
        shown = remember(shown, u"cmd.exe"_s);
        QCOMPARE(shown, (QStringList {u"regedit.exe"_s, u"cmd.exe"_s}));
        QCOMPARE(remember(shown, QString()), shown);
        Facts f = showing();
        f.program = u"cmd.exe"_s;
        f.shown = shown;
        QCOMPARE(decide(f), Verdict::Again);
        f.program = u"taskmgr.exe"_s;
        QCOMPARE(decide(f), Verdict::Show);
        f.shown = remember(f.shown, f.program);
        f.program = u"wechat.exe"_s;
        QCOMPARE(decide(f), Verdict::Enough);
    }

    void titleKeepsWhy()
    {
        QCOMPARE(shortTitle(u"注册表编辑器"_s), u"注册表编辑器"_s);
        const QString longName(40, u'x');
        const QString cut = shortTitle(longName);
        QCOMPARE(cut.size(), kMostTitleChars);
        QVERIFY(cut.endsWith(u'\u2026'));
        // Never half a surrogate pair at the cut.
        QString pairs;
        for (int i = 0; i < 20; ++i)
            pairs += QString::fromUcs4(U"\U0001F600", 1);
        QVERIFY(!shortTitle(pairs).chopped(1).back().isHighSurrogate());
    }

    void fixOnlyWhereItHelps()
    {
        QCOMPARE(fixFor(Listener::None, true), Fix::Install);
        QCOMPARE(fixFor(Listener::Old, true), Fix::Update);
        QCOMPARE(fixFor(Listener::Off, true), Fix::None);
        QCOMPARE(fixFor(Listener::Connecting, true), Fix::None);
        QCOMPARE(fixFor(Listener::Hears, true), Fix::None);
        // No administrator's account, or no WinShunService.exe beside it (Scoop, a bare copy).
        QCOMPARE(fixFor(Listener::None, false), Fix::None);
        QCOMPARE(fixFor(Listener::Old, false), Fix::None);
    }

    void adviceAndClick()
    {
        Advice a = advise(true, Fix::Install);
        QVERIFY(a.hotkey);
        QCOMPARE(a.click, Click::Service);
        a = advise(true, Fix::None);
        QVERIFY(a.hotkey);
        QCOMPARE(a.click, Click::None);
        a = advise(false, Fix::Update);
        QVERIFY(!a.hotkey);
        QCOMPARE(a.fix, Fix::Update);
        QCOMPARE(a.click, Click::Service);
        a = advise(false, Fix::None);
        QVERIFY(!a.hotkey);
        QCOMPARE(a.click, Click::Hotkey);
    }
};

QTEST_APPLESS_MAIN(AdminTapHintTest)
#include "tst_admintaphint.moc"

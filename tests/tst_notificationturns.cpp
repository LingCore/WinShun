#include "NotificationTurns.h"

#include <QtTest>

using namespace ws;
using namespace Qt::StringLiterals;

constexpr std::uint64_t kNone = 0; // ownUpUntil: none of Win顺's own is up

// GitHub #113: a plugin's notification waits while one of Win顺's own is up,
// and never for long whatever Windows tells of the ends; a click right after
// a plugin's replaced another owner's runs nothing.
class NotificationTurnsTest : public QObject {
    Q_OBJECT

private slots:
    void ownIsUpUntilItGoes()
    {
        NotificationTurns t;
        QCOMPARE(t.ownUpUntil(0), kNone);
        t.shown(QString(), 1000);
        QCOMPARE(t.ownUpUntil(2000), 1000 + NotificationTurns::kUpMs);
        // Its end not told (quiet hours, Focus): gone all the same.
        QCOMPARE(t.ownUpUntil(1000 + NotificationTurns::kUpMs), kNone);
        // Told: gone at once.
        t.shown(QString(), 20000);
        QVERIFY(t.ended(26000));
        QCOMPARE(t.ownUpUntil(26001), kNone);
        // A plugin's up: nothing to wait for.
        t.shown(u"test.fake"_s, 30000);
        QCOMPARE(t.ownUpUntil(30001), kNone);
    }

    // Win顺's own, then another of its own that replaced it without the
    // first one's end, then one end: the newest's. A plugin's coming then is
    // not held for a ghost.
    void replacedWithoutItsEnd()
    {
        NotificationTurns t;
        t.shown(QString(), 0);
        t.shown(QString(), 2000);
        // A plugin's in between waits, until the newest goes at the latest.
        QCOMPARE(t.ownUpUntil(4000), 2000 + NotificationTurns::kUpMs);
        QVERIFY(t.ended(8000)); // the newest's timeout, 6 seconds on
        QCOMPARE(t.ownUpUntil(8001), kNone);
    }

    // The replaced one tells its end right then: the newest stays up.
    void replacedTellsItsEnd()
    {
        NotificationTurns t;
        t.shown(QString(), 0);
        t.shown(QString(), 2000);
        QVERIFY(!t.ended(2100));
        QCOMPARE(t.ownUpUntil(2200), 2000 + NotificationTurns::kUpMs);
        QVERIFY(t.ended(8000));
        QCOMPARE(t.ownUpUntil(8001), kNone);
    }

    void clickRightAfterAReplacement()
    {
        NotificationTurns t;
        t.shown(u"a.one"_s, 0);
        t.shown(u"b.two"_s, 1000);
        QVERIFY(t.clickUnclear(1500)); // may be on a.one's, going
        QVERIFY(!t.clickUnclear(1000 + NotificationTurns::kOverlapMs)); // b.two's alone by now
        // The same owner's: whichever, its own.
        NotificationTurns same;
        same.shown(u"a.one"_s, 0);
        same.shown(u"a.one"_s, 1000);
        QVERIFY(!same.clickUnclear(1200));
        // Win顺's own the newest: its harmless action runs.
        NotificationTurns own;
        own.shown(u"a.one"_s, 0);
        own.shown(QString(), 1000);
        QVERIFY(!own.clickUnclear(1200));
        // A plugin's after Win顺's own (held back, shown once it went): clear.
        NotificationTurns after;
        after.shown(QString(), 0);
        QVERIFY(after.ended(6000));
        after.shown(u"a.one"_s, 6100);
        QVERIFY(!after.clickUnclear(6200));
        // One whose end never came, long gone: not "replaced".
        NotificationTurns old;
        old.shown(u"a.one"_s, 0);
        old.shown(u"b.two"_s, NotificationTurns::kStaysMostMs + 1000);
        QVERIFY(!old.clickUnclear(NotificationTurns::kStaysMostMs + 1100));
        // Win顺's own kept up under the mouse past kUpMs: the plugin's that
        // was waiting replaces it, and a click right then is unclear.
        NotificationTurns hovered;
        hovered.shown(QString(), 0);
        QCOMPARE(hovered.ownUpUntil(NotificationTurns::kUpMs), kNone);
        hovered.shown(u"a.one"_s, NotificationTurns::kUpMs + 50);
        QVERIFY(hovered.clickUnclear(NotificationTurns::kUpMs + 400));
    }

    // The replaced one tells its end at once, but is still seen going: a
    // click then stays unclear, and the newest is still up.
    void endOfTheReplacedKeepsTheClickUnclear()
    {
        NotificationTurns t;
        t.shown(u"a.one"_s, 0);
        t.shown(u"b.two"_s, 1000);
        QVERIFY(!t.ended(1020));
        QVERIFY(t.clickUnclear(1400));
        // A second end that soon: there was one replaced, so it is the newest's.
        QVERIFY(t.ended(1100));
        QVERIFY(!t.clickUnclear(1200));
    }

    // Three of Win顺's own one right after another: the two replaced tell
    // their ends, the newest stays up until its own.
    void severalReplacedInARow()
    {
        NotificationTurns t;
        t.shown(QString(), 0);
        t.shown(QString(), 500);
        t.shown(QString(), 900);
        QVERIFY(!t.ended(950));
        QVERIFY(!t.ended(1000));
        QCOMPARE(t.ownUpUntil(1100), 900 + NotificationTurns::kUpMs);
        QVERIFY(t.ended(6900));
        QCOMPARE(t.ownUpUntil(6901), kNone);
        // Plugins a, b, a in a row: a click on the newest a may be on b's.
        NotificationTurns mixed;
        mixed.shown(u"b.two"_s, 0);
        mixed.shown(u"a.one"_s, 300);
        mixed.shown(u"a.one"_s, 600);
        QVERIFY(mixed.clickUnclear(800));
    }

    void clearedForgetsAll()
    {
        NotificationTurns t;
        t.shown(u"a.one"_s, 0);
        t.shown(QString(), 500);
        t.clear();
        QCOMPARE(t.ownUpUntil(600), kNone);
        QVERIFY(!t.clickUnclear(600));
        QVERIFY(t.ended(700)); // nothing up: an end is nobody's, harmless
    }
};

QTEST_GUILESS_MAIN(NotificationTurnsTest)
#include "tst_notificationturns.moc"

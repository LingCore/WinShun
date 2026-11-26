#include "PinnedStacking.h"

#include <QTest>

using namespace ws;
using namespace std::chrono_literals;

// Whether the launcher is over every other window: a menu when not pinned;
// pinned, as set, but never over what was just opened from it.
class PinnedStackingTest : public QObject {
    Q_OBJECT

private slots:
    void notPinnedIsAlwaysOnTop()
    {
        PinnedStacking stacking;
        QVERIFY(stacking.onTop(false, true));
        QVERIFY(stacking.onTop(false, false));
        stacking.opened(PinnedStacking::Clock::now()); // it is hidden for that; unpinned later, say
        QVERIFY(stacking.onTop(false, true));
        QVERIFY(stacking.onTop(false, false));
    }

    void pinnedFollowsTheSetting()
    {
        const PinnedStacking stacking;
        QVERIFY(stacking.onTop(true, true));
        QVERIFY(!stacking.onTop(true, false));
        QVERIFY(!stacking.standsBack());
    }

    void standsBackForWhatItOpened()
    {
        PinnedStacking stacking;
        const auto opened = PinnedStacking::Clock::now();
        stacking.opened(opened);
        QVERIFY(stacking.standsBack());
        QVERIFY(!stacking.onTop(true, true));
        QVERIFY(!stacking.onTop(true, false));
        // The front lost to what it opened, a moment or some seconds later (a
        // program starting): still standing back, nothing to apply again.
        QVERIFY(!stacking.lostFront(opened + 300ms));
        QVERIFY(!stacking.lostFront(opened + PinnedStacking::kStandsBackFor));
        QVERIFY(!stacking.onTop(true, true));
    }

    void onTopAgainOnceInFront()
    {
        PinnedStacking stacking;
        stacking.opened(PinnedStacking::Clock::now());
        stacking.inFront();
        QVERIFY(!stacking.standsBack());
        QVERIFY(stacking.onTop(true, true));
        QVERIFY(!stacking.onTop(true, false)); // the setting off stays off
    }

    // Nothing came to the front for what it opened (a command that shows
    // nothing): long after, a front lost is a click elsewhere.
    void aClickElsewhereLongAfterIsNotWhatItOpened()
    {
        PinnedStacking stacking;
        const auto opened = PinnedStacking::Clock::now();
        stacking.opened(opened);
        QVERIFY(stacking.lostFront(opened + PinnedStacking::kStandsBackFor + 1ms));
        QVERIFY(!stacking.standsBack());
        QVERIFY(stacking.onTop(true, true));
        QVERIFY(!stacking.lostFront(opened + 1h)); // told once
    }

    void theLatestOpeningCounts()
    {
        PinnedStacking stacking;
        const auto first = PinnedStacking::Clock::now();
        stacking.opened(first);
        stacking.opened(first + 1min); // the next result
        QVERIFY(!stacking.lostFront(first + 1min + 2s));
        QVERIFY(stacking.standsBack());
    }

    void aFrontLostWithNothingOpenedChangesNothing()
    {
        PinnedStacking stacking;
        QVERIFY(!stacking.lostFront(PinnedStacking::Clock::now()));
        QVERIFY(stacking.onTop(true, true));
    }
};

QTEST_APPLESS_MAIN(PinnedStackingTest)
#include "tst_pinnedstacking.moc"

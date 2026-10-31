// The clock of the break-reminder plugin (src/break-plugin/Schedule): when
// the notice shows, when the break starts and ends, which ones are long,
// what counts as being away, and what postpone, skip and strict allow.
// Driven with made-up times; one tick a second unless a test says so. And
// what its keyboard hook keeps from the program underneath (Keys.h).

#include "Keys.h"
#include "Schedule.h"

#include <QTest>

using namespace brk;

namespace {

constexpr std::int64_t kSec = 1000;
constexpr std::int64_t kMin = 60 * kSec;

Config config()
{
    Config c;
    c.intervalMs = 20 * kMin;
    c.shortMs = 20 * kSec;
    c.longEvery = 2;
    c.longMs = 5 * kMin;
    c.headsUpMs = 30 * kSec;
    c.snoozeMs = 5 * kMin;
    c.resetIdleMs = 5 * kMin;
    return c;
}

// A user at the PC who typed `idle` ms ago, ticked from `from` to `to`.
struct Run {
    Schedule s;
    std::int64_t now = 0;
    std::int64_t idle = 0;
    bool quiet = false;
    bool locked = false;
    bool keptOn = false;

    explicit Run(Config c = config()) { s.configure(c, 0); }
    void step(std::int64_t ms)
    {
        const std::int64_t end = now + ms;
        while (now < end) {
            now = std::min(end, now + kSec);
            s.tick({now, 1'700'000'000'000 + now, idle, quiet, locked, keptOn});
        }
    }
    // Ticks until the phase is `p` (or `limit` passes); the time it took.
    std::int64_t until(Phase p, std::int64_t limit = 3 * 60 * kMin)
    {
        const std::int64_t start = now;
        while (s.phase() != p && now - start < limit)
            step(kSec);
        return now - start;
    }
    State state() const { return s.state(now, 1'700'000'000'000 + now); }
};

} // namespace

class TestBreak : public QObject {
    Q_OBJECT

private slots:
    void noticeThenBreakThenWork()
    {
        Run r;
        QCOMPARE(r.until(Phase::HeadsUp), 20 * kMin - 30 * kSec);
        QCOMPARE(r.state().next, Kind::Short);
        QVERIFY(r.state().canPostpone);
        r.idle = 2 * kSec; // not typing: the break starts the tick after it is due
        QCOMPARE(r.until(Phase::OnBreak), 31 * kSec);
        QCOMPARE(r.state().breakLengthMs, 20 * kSec);
        QCOMPARE(r.until(Phase::Working), 20 * kSec);
        QCOMPARE(r.state().untilDueMs, 20 * kMin);
    }

    void everySecondBreakIsLong()
    {
        Run r;
        r.idle = 2 * kSec;
        QList<Kind> kinds;
        for (int i = 0; i < 4; ++i) {
            r.until(Phase::OnBreak);
            kinds << r.state().next;
            r.until(Phase::Working);
        }
        QCOMPARE(kinds, (QList<Kind> {Kind::Short, Kind::Long, Kind::Short, Kind::Long}));
    }

    void noLongBreaksWithZero()
    {
        Config c = config();
        c.longEvery = 0;
        Run r(c);
        r.idle = 2 * kSec;
        for (int i = 0; i < 3; ++i) {
            r.until(Phase::OnBreak);
            QCOMPARE(r.state().next, Kind::Short);
            r.until(Phase::Working);
        }
    }

    void waitsForAPauseInTyping()
    {
        Run r;
        r.until(Phase::Pending);
        r.idle = 0; // still typing
        r.step(10 * kSec);
        QCOMPARE(r.s.phase(), Phase::Pending);
        r.idle = 2 * kSec;
        r.step(kSec);
        QCOMPARE(r.s.phase(), Phase::OnBreak);

        Run typing; // never stops: the break starts after 15 seconds anyway
        typing.until(Phase::Pending);
        QCOMPARE(typing.until(Phase::OnBreak), Schedule::kPendingMaxMs);
    }

    void awayCountsAsABreak()
    {
        Run r;
        r.idle = 2 * kSec;
        r.until(Phase::OnBreak);
        r.until(Phase::Working);
        QCOMPARE(r.state().next, Kind::Long); // one short break taken
        r.idle = 0;
        r.step(15 * kMin);
        r.idle = 5 * kMin; // left the PC
        r.step(kSec);
        QCOMPARE(r.state().workedMs, 0);
        r.step(10 * kMin); // still away: the count waits for them
        r.idle = 0;
        r.step(kSec);
        QCOMPARE(r.state().untilDueMs, 20 * kMin - kSec);
        QCOMPARE(r.state().next, Kind::Short); // the cycle started over
    }

    void briefAbsenceDelaysTheNotice()
    {
        Run r;
        r.step(19 * kMin);
        r.idle = 60 * kSec; // away for a minute or so, not long enough to count
        r.step(2 * kMin); // past the notice and the due time
        QCOMPARE(r.s.phase(), Phase::Working);
        r.idle = 0;
        r.step(kSec);
        QCOMPARE(r.s.phase(), Phase::HeadsUp);
        QCOMPARE(r.state().untilDueMs, 30 * kSec); // the whole notice
    }

    void quietHoldsTheBreakBack()
    {
        Run r;
        r.quiet = true;
        r.step(25 * kMin);
        QCOMPARE(r.s.phase(), Phase::Working);
        QVERIFY(r.state().waiting);
        r.quiet = false;
        r.step(kSec);
        QCOMPARE(r.s.phase(), Phase::HeadsUp);
        QCOMPARE(r.state().untilDueMs, 30 * kSec);
        r.quiet = true; // a full-screen program again during the notice
        r.step(kSec);
        QCOMPARE(r.s.phase(), Phase::Working);
    }

    void postponeOnce()
    {
        Run r;
        r.until(Phase::HeadsUp);
        QVERIFY(r.s.postpone(r.now));
        QCOMPARE(r.state().untilDueMs, 5 * kMin);
        r.until(Phase::HeadsUp);
        QVERIFY(!r.state().canPostpone);
        QVERIFY(!r.s.postpone(r.now));
        r.idle = 2 * kSec;
        r.until(Phase::OnBreak);
        QVERIFY(!r.s.postpone(r.now));
        r.until(Phase::Working);
        r.until(Phase::HeadsUp);
        QVERIFY(r.state().canPostpone); // a new break, a new postpone
    }

    void strictBreaks()
    {
        Config c = config();
        c.strict = Strict::LongOnly;
        Run r(c);
        r.idle = 2 * kSec;
        r.until(Phase::OnBreak);
        QVERIFY(!r.state().strict); // the short one
        QVERIFY(r.s.skip(r.now));
        r.until(Phase::HeadsUp);
        QCOMPARE(r.state().next, Kind::Long);
        QVERIFY(r.state().strict);
        QVERIFY(!r.state().canPostpone);
        QVERIFY(!r.s.skip(r.now));
        r.until(Phase::OnBreak);
        QVERIFY(!r.s.skip(r.now));
        QVERIFY(!r.s.postpone(r.now));
        QVERIFY(r.s.skip(r.now, true)); // the long Esc
        QCOMPARE(r.s.phase(), Phase::Working);
        QCOMPARE(r.state().next, Kind::Short); // the skipped long one still ended the cycle
    }

    void lockingEndsTheBreak()
    {
        Run r;
        r.idle = 2 * kSec;
        r.until(Phase::OnBreak);
        r.locked = true;
        r.step(kSec);
        QCOMPARE(r.s.phase(), Phase::Working);
        for (int i = 0; i < 30 * 60; ++i) { // locked through the next due time: no notice, ever
            r.step(kSec);
            QCOMPARE(r.s.phase(), Phase::Working);
        }
        QVERIFY(r.state().waiting);
        r.locked = false;
        r.step(kSec);
        QCOMPARE(r.s.phase(), Phase::HeadsUp); // back: the notice, whole
        QCOMPARE(r.state().untilDueMs, 30 * kSec);
    }

    // A video or a call keeps the display on: whoever watches is there,
    // touching nothing or not; a pause in typing is still a pause.
    void displayKeptOnIsSomeoneThere()
    {
        Run r;
        r.keptOn = true;
        r.idle = 10 * kMin; // a film: no input for ten minutes
        r.step(19 * kMin + 30 * kSec);
        QCOMPARE(r.s.phase(), Phase::HeadsUp); // not away: the notice, the count went on
        QCOMPARE(r.until(Phase::OnBreak), 31 * kSec); // not typing: at once when due
        r.until(Phase::Working);

        Run typing; // typing beside a video: waits for the pause as ever
        typing.keptOn = true;
        typing.until(Phase::Pending);
        typing.step(5 * kSec);
        QCOMPARE(typing.s.phase(), Phase::Pending);
        typing.idle = 2 * kSec;
        typing.step(kSec);
        QCOMPARE(typing.s.phase(), Phase::OnBreak);
    }

    // Locked or asleep through a pause (Win顺 restarts the count then), or
    // a break asked for: the pause goes on.
    void pauseOutlivesARestart()
    {
        Run r;
        r.step(10 * kMin);
        r.s.pause(1'700'000'000'000 + r.now + 60 * kMin);
        r.s.restart(r.now);
        QCOMPARE(r.s.phase(), Phase::Paused);
        r.s.breakNow(Kind::Short, r.now);
        QCOMPARE(r.s.phase(), Phase::Paused);
        r.step(30 * kMin);
        QCOMPARE(r.s.phase(), Phase::Paused);
        r.step(31 * kMin);
        QCOMPARE(r.s.phase(), Phase::Working);
        QCOMPARE(r.state().next, Kind::Short);
    }

    // The keyboard hook (Keys.h): what reaches the program underneath.
    void keysOnTheBreakScreen()
    {
        constexpr unsigned kA = 'A', kB = 'B', kCtrl = 0xA2, kEsc = 0x1B, kVolumeUp = 0xAF;
        KeyGate g;
        g.close();
        // Pressed on the screen: kept, its repeats and its up too.
        QVERIFY(g.key(kA, true, false));
        QVERIFY(g.key(kA, true, false));
        QVERIFY(g.key(kA, false, false));
        // Volume and media keys go through.
        QVERIFY(!g.key(kVolumeUp, true, false));
        QVERIFY(!g.key(kVolumeUp, false, false));
        // Ctrl held since before the screen: its repeats are kept, its up
        // goes through (else Windows has it down after the break).
        QVERIFY(g.key(kCtrl, true, true));
        QVERIFY(g.key(kCtrl, true, true));
        QVERIFY(!g.key(kCtrl, false, true));
        QVERIFY(g.empty());
        // Esc held as the screen goes (the long Esc of a strict break): its
        // repeats and its up are still kept, other keys go through.
        QVERIFY(g.key(kEsc, true, false));
        QVERIFY(g.took(kEsc));
        g.drain();
        QVERIFY(g.key(kEsc, true, false));
        QVERIFY(!g.key(kB, true, false));
        QVERIFY(!g.key(kB, false, false));
        QVERIFY(!g.empty());
        QVERIFY(g.key(kEsc, false, false));
        QVERIFY(g.empty());
        // Up again: the next screen keeps keys again.
        g.close();
        QVERIFY(g.key(kB, true, false));
    }

    void pauseAndResume()
    {
        Run r;
        r.step(10 * kMin);
        const std::int64_t wall = 1'700'000'000'000 + r.now;
        r.s.pause(wall + 60 * kMin);
        r.step(30 * kMin);
        QCOMPARE(r.s.phase(), Phase::Paused);
        r.step(31 * kMin);
        QCOMPARE(r.s.phase(), Phase::Working); // ended by itself
        QVERIFY(r.state().untilDueMs >= 18 * kMin);

        r.s.pause(1'700'000'000'000 + r.now + 60 * kMin);
        r.s.resume(r.now);
        QCOMPARE(r.s.phase(), Phase::Working);
    }

    void breakNowAndRestart()
    {
        Run r;
        r.s.breakNow(Kind::Long, r.now);
        QCOMPARE(r.state().breakLengthMs, 5 * kMin);
        r.until(Phase::Working);
        QCOMPARE(r.state().next, Kind::Short);
        r.step(12 * kMin);
        r.s.restart(r.now);
        QCOMPARE(r.state().untilDueMs, 20 * kMin);
    }

    void newIntervalCountsFromTheSameStart()
    {
        Run r;
        r.step(10 * kMin);
        Config c = config();
        c.intervalMs = 15 * kMin;
        r.s.configure(c, r.now);
        QCOMPARE(r.state().untilDueMs, 5 * kMin);
    }

    void noNoticeWhenZero()
    {
        Config c = config();
        c.headsUpMs = 0;
        Run r(c);
        QCOMPARE(r.until(Phase::Pending), 20 * kMin);
    }
};

QTEST_GUILESS_MAIN(TestBreak)
#include "tst_break.moc"

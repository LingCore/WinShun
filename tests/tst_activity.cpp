#include "ActivityText.h"
#include "HealthHistory.h"

#include <algorithm>

#include <QtTest>

using namespace ws::activity;

// The words for "what is it doing, and when is it done": nothing said while
// the speed is not known well enough, a range rounded to minutes, "快好了"
// under a minute, the tidy-up, and the reason when it waits or goes slowly.
class ActivityTest : public QObject {
    Q_OBJECT

private slots:
    void tooFewSamplesSaysNothing()
    {
        Progress p {1000, 10000, 20.0, 29'000, Pause::None, false};
        QCOMPARE(remainingText(p), QString());
        p.sampleMs = RateEstimator::kMinSampleMs;
        QVERIFY(!remainingText(p).isEmpty());
        p.perSecond = 0; // no speed yet
        QCOMPARE(remainingText(p), QString());
    }

    void rangeInMinutes()
    {
        // 9000 left at 30 a second: 300 s, 5 min: a range around it, not a figure.
        Progress p {1000, 10000, 30.0, 60'000, Pause::None, false};
        QCOMPARE(remainingText(p), QStringLiteral("约 4–6 分钟"));
        // About three to five minutes for 240 s.
        p.total = 1000 + 240 * 30;
        QCOMPARE(remainingText(p), QStringLiteral("约 3–5 分钟"));
        // Long: hours.
        p.total = 1000 + 3 * 3600 * 30;
        QCOMPARE(remainingText(p), QStringLiteral("约 2–4 小时"));
    }

    void underAMinuteIsAlmostDone()
    {
        Progress p {9000, 10000, 30.0, 60'000, Pause::None, false}; // 33 s
        QCOMPARE(remainingText(p), QStringLiteral("快好了"));
        p.done = p.total; // nothing left to read
        QCOMPARE(remainingText(p), QString());
    }

    void tidyingUpIsSaidWhateverTheSpeed()
    {
        Progress p {10000, 10000, 0, 0, Pause::None, true};
        QCOMPARE(remainingText(p), QStringLiteral("正在整理，快好了"));
        p.pause = Pause::SearchBox; // the tidy-up is what is told
        QCOMPARE(remainingText(p), QStringLiteral("正在整理，快好了"));
    }

    void pausesSayWhy()
    {
        Progress p {1000, 10000, 30.0, 120'000, Pause::SearchBox, false};
        QCOMPARE(remainingText(p), QStringLiteral("你在搜索，先停一下")); // and no time to go: it does not know when
        p.pause = Pause::ContentSearch;
        QCOMPARE(remainingText(p), QStringLiteral("你在搜索，先停一下"));
        p.pause = Pause::Battery;
        QCOMPARE(remainingText(p), QStringLiteral("在用电池，先暂停"));
        // Slowed: the time to go with the reason, or the reason alone before the speed is known.
        p.pause = Pause::UserPresent;
        QCOMPARE(remainingText(p), QStringLiteral("约 4–6 分钟（硬盘上慢慢读，不卡你）"));
        p.sampleMs = 1000;
        QCOMPARE(remainingText(p), QStringLiteral("硬盘上慢慢读，不卡你"));
    }

    void presenceAloneIsNotAReason()
    {
        // Someone at the computer on a fast disk is not a pause at all: nothing about it is said.
        Progress p {1000, 10000, 30.0, 120'000, Pause::None, false};
        QCOMPARE(remainingText(p), QStringLiteral("约 4–6 分钟"));
        QVERIFY(!remainingText(p).contains(QStringLiteral("你在用电脑")));
        QVERIFY(pauseText(Pause::None).isEmpty());
    }

    void fileIndexNoticeNeedsTwoMinutes()
    {
        std::int64_t started = 1000;
        QVERIFY(!takeNotice(started, 1000 + kNoticeAfterMs - 1)); // a minute and 59 s: not told
        QCOMPARE(started, qint64(0));
        started = 1000;
        QVERIFY(takeNotice(started, 1000 + kNoticeAfterMs)); // first build or a rebuild over two minutes: told
        QCOMPARE(started, qint64(0));
        QVERIFY(!takeNotice(started, 1000 + 10 * kNoticeAfterMs)); // the same job is not told twice
        started = 0; // never began
        QVERIFY(!takeNotice(started, 99 * kNoticeAfterMs));
    }

    void contentNoticeOnlyForBigFinishedRounds()
    {
        const std::int64_t late = 1000 + 3 * kNoticeAfterMs;
        std::int64_t started = 1000;
        QVERIFY(takeContentNotice(started, late, 5000, 5000)); // a big round, long, finished
        QVERIFY(!takeContentNotice(started, late, 5000, 5000)); // once
        started = 1000;
        QVERIFY(!takeContentNotice(started, late, 40, 40)); // a daily small round, however long
        QCOMPARE(started, qint64(0));
        started = 1000;
        QVERIFY(takeContentNotice(started, late, kBigRound, kBigRound)); // exactly 1000 files counts as big
        started = 1000;
        QVERIFY(!takeContentNotice(started, 1000 + kNoticeAfterMs - 1, 5000, 5000)); // big but quick
        started = 1000;
        QVERIFY(!takeContentNotice(started, late, 2000, 5000)); // cut short: not over, not told ...
        QCOMPARE(started, qint64(1000)); // ... and the job is remembered for the next pass
        QVERIFY(takeContentNotice(started, late + 1, 5000, 5000)); // told when it finally ends
    }

    void tookIsWholeMinutes()
    {
        QCOMPARE(tookText(2 * 60'000 + 5000), QStringLiteral("2 分钟"));
        QCOMPARE(tookText(6 * 60'000 + 31'000), QStringLiteral("7 分钟"));
        QCOMPARE(tookText(10'000), QStringLiteral("1 分钟"));
        QCOMPARE(tookText(120 * 60'000), QStringLiteral("2.0 小时"));
    }

    void estimatorSmoothsAndRestarts()
    {
        RateEstimator r;
        r.feed(0, 0);
        QCOMPARE(r.sampleMs(), qint64(0));
        r.feed(10'000, 1000); // 100 a second
        QCOMPARE(r.perSecond(), 100.0);
        r.feed(20'000, 1000); // a stall: the figure falls, not to zero at once
        QVERIFY(r.perSecond() > 50 && r.perSecond() < 100);
        QCOMPARE(r.sampleMs(), qint64(20'000));
        r.feed(21'000, 1000); // a stretch too short to count
        QCOMPARE(r.sampleMs(), qint64(20'000));
        r.feed(30'000, 5); // a new round (fewer done): starts over
        QCOMPARE(r.sampleMs(), qint64(0));
        QCOMPARE(r.perSecond(), 0.0);
    }

    // ---- the 运行状况 page's readings (HealthHistory) ----

    static ws::health::Sample reading(std::int64_t atMs, std::uint32_t otherMb, std::uint16_t cpu = 0)
    {
        ws::health::Sample s {};
        s.atMs = atMs;
        s.partKb[ws::health::Other] = otherMb * 1024;
        s.cpu = cpu;
        return s;
    }

    void ringDropsTheOldest()
    {
        ws::health::Ring ring(3);
        for (int i = 1; i <= 5; ++i)
            ring.push(reading(i, i));
        QCOMPARE(ring.size(), std::size_t(3));
        QCOMPARE(ring.at(0).atMs, std::int64_t(3));
        QCOMPARE(ring.back().atMs, std::int64_t(5));
    }

    void seriesKeepsThePeak()
    {
        // A day in 10-second readings, one minute at 700 MB and 80 % in the middle:
        // drawn in 100 buckets of 14 minutes, the peak is still there.
        ws::health::History history;
        const std::int64_t day = 24 * 3600 * 1000LL;
        for (std::int64_t t = 0; t < day; t += ws::health::History::kCoarseMs) {
            const bool spike = t >= day / 2 && t < day / 2 + 60'000;
            history.addCoarse(reading(t, spike ? 700 : 20, spike ? 8000 : 10));
        }
        const ws::health::Series s = history.series(0, day, 100);
        QCOMPARE(s.size(), std::size_t(100));
        QCOMPARE(s.peakTotalMb, 700.0);
        QCOMPARE(s.peakCpu, 80.0);
        const auto at = std::max_element(s.totalMb.begin(), s.totalMb.end()) - s.totalMb.begin();
        QVERIFY(s.cpuMean[at] < 10); // the mean thins it; the max keeps it
        QCOMPARE(s.cpuMax[at], 80.0);
        QVERIFY(std::none_of(s.breakBefore.begin() + 1, s.breakBefore.end(), [](std::uint8_t b) { return b != 0; }));
    }

    void seriesBreaksAtAHole()
    {
        // Asleep for an hour: the line stops and starts again.
        ws::health::History history;
        for (std::int64_t t = 0; t < 600'000; t += 10'000)
            history.addCoarse(reading(t, 20));
        for (std::int64_t t = 4'200'000; t < 4'800'000; t += 10'000)
            history.addCoarse(reading(t, 20));
        const ws::health::Series s = history.series(0, 4'800'000, 480);
        int breaks = 0;
        for (std::uint8_t b : s.breakBefore)
            breaks += b;
        QCOMPARE(breaks, 2); // the first point, and after the hole
    }

    void fineReadingsTakeOver()
    {
        // Where the page's 1-second readings begin, the 10-second ones give way.
        ws::health::History history;
        for (std::int64_t t = 0; t <= 600'000; t += 10'000)
            history.addCoarse(reading(t, 20));
        for (std::int64_t t = 300'000; t <= 600'000; t += 1'000)
            history.addFine(reading(t, t == 450'000 ? 300 : 30));
        const ws::health::Series s = history.series(0, 600'000, 600);
        QCOMPARE(s.peakTotalMb, 300.0);
        QCOMPARE(s.totalMb.front(), 20.0);
        QCOMPARE(s.totalMb.back(), 30.0);
        QCOMPARE(s.size(), std::size_t(30 + 300)); // the reading at the very end shares the last bucket
        history.clearFine();
        QCOMPARE(history.series(0, 600'000, 600).peakTotalMb, 20.0);
        QVERIFY(history.bytes() < 400 * 1024);
    }

    void eventsFromFacts()
    {
        using namespace ws::health;
        Events events;
        EventTracker tracker(events);
        Facts f;
        f.loading = true;
        tracker.observe(f, 1000, 100 * 1024);
        tracker.observe(f, 3000, 250 * 1024); // its peak
        f.loading = false;
        f.saving = true; // a short save: not kept
        tracker.observe(f, 4000, 120 * 1024);
        f.saving = false;
        f.contentReading = true; // a round that reads nothing: not kept
        tracker.observe(f, 4500, 120 * 1024);
        f.contentReading = false;
        tracker.observe(f, 6000, 120 * 1024);
        f.contentReading = true; // one that reads: kept, with how many
        f.contentDone = 10;
        tracker.observe(f, 7000, 300 * 1024);
        f.contentDone = 0; // starts over at its end
        f.biggestDocumentKb = Events::kBigDocumentKb + 1024;
        tracker.observe(f, 8000, 500 * 1024);
        f.contentReading = false;
        f.biggestDocumentKb = 0;
        tracker.observe(f, 9000, 200 * 1024);
        const std::vector<Event> all = events.recent(10);
        QCOMPARE(all.size(), std::size_t(3));
        QCOMPARE(all[2].kind, EventKind::IndexLoad);
        QCOMPARE(all[2].peakKb, std::uint32_t(250 * 1024));
        QCOMPARE(all[2].endMs, std::int64_t(4000));
        QCOMPARE(all[1].kind, EventKind::ContentRound);
        QCOMPARE(all[1].count, std::uint64_t(10));
        QCOMPARE(all[1].peakKb, std::uint32_t(500 * 1024));
        QCOMPARE(all[0].kind, EventKind::BigDocument);
        QCOMPARE(all[0].count, std::uint64_t(Events::kBigDocumentKb + 1024));
        QCOMPARE(events.between(8500, 8600).size(), std::size_t(2));
        events.instant(EventKind::AutoTrim, 9500, 30 * 1024, 170);
        QVERIFY(events.recent(1).front().instant());
        QCOMPARE(events.recent(1).front().freedMb, std::uint32_t(170));
    }
};
QTEST_GUILESS_MAIN(ActivityTest)
#include "tst_activity.moc"

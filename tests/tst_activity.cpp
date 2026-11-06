#include "ActivityText.h"
#include "HealthHistory.h"
#include "platform/ProcessUsage.h"

#include <cstring>
#include <memory>

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

    // ---- the 运行状况 page's readings (HealthHistory, memory::map) ----

    void memoryMapTellsTheParts()
    {
        // A 32 MB block on the heap, every page touched, given as the file
        // index's: all of it is found there, none of it in the rest.
        constexpr std::size_t kSize = 32u << 20;
        auto block = std::make_unique<char[]>(kSize);
        std::memset(block.get(), 1, kSize);
        const ws::memory::Map with = ws::memory::map({{block.get(), kSize}}, true);
        const ws::memory::Map without = ws::memory::map({}, false);
        QVERIFY(with.fileIndex >= kSize - 8192 && with.fileIndex <= kSize + 8192);
        QCOMPARE(without.fileIndex, std::uint64_t(0));
        QVERIFY(without.other >= with.other + kSize - 8192);
        QVERIFY(with.stacks > 0); // this thread's
        QVERIFY(with.ownProgram + with.qt + with.system > 0); // the test's own pages, Qt's
        const std::uint64_t parts = with.fileIndex + with.ownProgram + with.qt + with.graphics + with.system + with.stacks + with.other;
        QCOMPARE(parts, with.total);
        QVERIFY(with.heapHandedOut >= kSize);
        QCOMPARE(without.heaps, 0); // not asked
        QVERIFY(with.heaps > 0);
        QVERIFY(with.heapSpare <= with.other);
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
        events.instant(EventKind::AutoTrim, 9500, 30 * 1024, 170);
        QVERIFY(events.recent(1).front().instant());
        QCOMPARE(events.recent(1).front().freedMb, std::uint32_t(170));
    }
};
QTEST_GUILESS_MAIN(ActivityTest)
#include "tst_activity.moc"

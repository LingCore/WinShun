#include "ActivityText.h"

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
        QCOMPARE(remainingText(p), QStringLiteral("约 4–6 分钟（你在用电脑，放慢了）"));
        p.sampleMs = 1000;
        QCOMPARE(remainingText(p), QStringLiteral("你在用电脑，放慢了"));
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
};

QTEST_GUILESS_MAIN(ActivityTest)
#include "tst_activity.moc"

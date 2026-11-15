#include "platform/UiStall.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

using namespace ws;
using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

struct Noted {
    QString event;
    QJsonObject details;
};

// What the watch wrote down, from its own thread.
class Notes {
public:
    UiStallWatch::Sink sink()
    {
        return [this](const QString& event, QJsonObject details) {
            const std::lock_guard lock(m_mutex);
            m_notes.push_back({event, std::move(details)});
        };
    }
    std::vector<Noted> all()
    {
        const std::lock_guard lock(m_mutex);
        return m_notes;
    }
    int count(const QString& event)
    {
        const std::lock_guard lock(m_mutex);
        return static_cast<int>(std::ranges::count(m_notes, event, &Noted::event));
    }
    QByteArray text()
    {
        QByteArray text;
        for (const Noted& note : all())
            text += note.event.toUtf8() + ' ' + QJsonDocument(note.details).toJson(QJsonDocument::Compact) + '\n';
        return text;
    }

private:
    std::mutex m_mutex;
    std::vector<Noted> m_notes;
};

constexpr UiStallWatch::Options kQuick {.every = 40ms, .late = 250ms, .again = 300ms, .samples = 3, .stalls = 20,
    .onlyWhileUsed = false};

// The interface's thread computing without end, as a loop gone wrong does.
Q_NEVER_INLINE void computeFor(int ms)
{
    QElapsedTimer timer;
    timer.start();
    volatile quint64 sum = 0;
    while (timer.elapsed() < ms) {
        for (int i = 0; i < 10'000; ++i)
            sum = sum + static_cast<quint64>(i);
    }
}

// Waiting on something that does not come, as a read from a dead share does.
Q_NEVER_INLINE void waitFor(int ms)
{
    ::Sleep(static_cast<DWORD>(ms));
}

bool hasFrameOf(const QJsonObject& details, QLatin1StringView module)
{
    for (const QJsonValue frame : details.value(u"frames"_s).toArray()) {
        if (frame.toString().startsWith(module, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

// Another thread of the program's, held up in a call of its own until told to go.
Q_NEVER_INLINE void heldUp(const std::atomic<bool>& go)
{
    while (!go.load())
        ::Sleep(20);
}

} // namespace

class UiStallTest : public QObject {
    Q_OBJECT

private slots:
    // Answering as it should: nothing is written.
    void quietWhileItAnswers()
    {
        Notes notes;
        UiStallWatch watch(notes.sink(), kQuick);
        QVERIFY(watch.isActive());
        QTest::qWait(1000);
        QVERIFY2(notes.all().empty(), notes.text().constData());
    }

    // Computing: said so, with the calls it is in, this test's own among them.
    void aLoopIsSeenAsBusy()
    {
        Notes notes;
        UiStallWatch watch(notes.sink(), kQuick);
        computeFor(900);
        QTRY_COMPARE_WITH_TIMEOUT(notes.count(u"ui-stall-end"_s), 1, 2000);
        const auto all = notes.all();
        QVERIFY2(all.size() >= 2, notes.text().constData());
        const QJsonObject first = all.front().details;
        QCOMPARE(all.front().event, u"ui-stall"_s);
        QCOMPARE(first.value(u"stall"_s).toInt(), 1);
        QCOMPARE(first.value(u"sample"_s).toInt(), 1);
        QVERIFY2(first.value(u"ms"_s).toInteger() >= 200 && first.value(u"ms"_s).toInteger() < 800, notes.text().constData());
        QVERIFY2(first.value(u"busy"_s).toBool(), notes.text().constData());
        QVERIFY2(first.value(u"ui_cpu_ms"_s).toInteger() >= 100, notes.text().constData());
        QVERIFY2(first.value(u"process_cpu_ms"_s).toInteger() >= first.value(u"ui_cpu_ms"_s).toInteger(), notes.text().constData());
#if defined(_M_X64)
        QVERIFY2(first.value(u"frames"_s).toArray().size() >= 3, notes.text().constData());
        QVERIFY2(hasFrameOf(first, "tst_uistall.exe+"_L1), notes.text().constData());
        // No folders: a module is named by its file alone.
        for (const QJsonValue frame : first.value(u"frames"_s).toArray())
            QVERIFY2(!frame.toString().contains(u'\\') && !frame.toString().contains(u'/'), notes.text().constData());
#endif
        const QJsonObject end = all.back().details;
        QCOMPARE(all.back().event, u"ui-stall-end"_s);
        QCOMPARE(end.value(u"stall"_s).toInt(), 1);
        QVERIFY2(end.value(u"ms"_s).toInteger() >= 600, notes.text().constData());
    }

    // Waiting: not busy, and the wait itself (in Windows' own modules) is on top.
    void aWaitIsSeenAsWaiting()
    {
        Notes notes;
        UiStallWatch watch(notes.sink(), kQuick);
        waitFor(900);
        QTRY_COMPARE_WITH_TIMEOUT(notes.count(u"ui-stall-end"_s), 1, 2000);
        const QJsonObject first = notes.all().front().details;
        QVERIFY2(!first.value(u"busy"_s).toBool(), notes.text().constData());
        QVERIFY2(first.value(u"ui_cpu_ms"_s).toInteger() < 100, notes.text().constData());
#if defined(_M_X64)
        const QString top = first.value(u"frames"_s).toArray().first().toString();
        QVERIFY2(top.startsWith(u"ntdll.dll+", Qt::CaseInsensitive) || top.startsWith(u"win32u.dll+", Qt::CaseInsensitive),
            notes.text().constData());
        QVERIFY2(hasFrameOf(first, "tst_uistall.exe+"_L1), notes.text().constData());
        // In Windows' own modules the exported call is named: which wait it is.
        QVERIFY2(top.contains(u" NtDelayExecution+"_s), notes.text().constData());
        // Never for the program's own: its offsets go with its symbols.
        for (const QJsonValue frame : first.value(u"frames"_s).toArray()) {
            if (frame.toString().startsWith(u"tst_uistall.exe+"_s, Qt::CaseInsensitive))
                QVERIFY2(!frame.toString().contains(u' '), notes.text().constData());
        }
        QCOMPARE(notes.count(u"ui-stall-threads"_s), 0); // not asked for
#endif
    }

    // Asked for the other threads: theirs at a stall's first look, the ones in the same calls once.
    void theOtherThreadsAreLookedAt()
    {
        std::atomic<bool> go = false;
        std::vector<std::jthread> held;
        for (int i = 0; i < 3; ++i)
            held.emplace_back([&go] { heldUp(go); });
        Notes notes;
        UiStallWatch::Options options = kQuick;
        options.others = true;
        {
            UiStallWatch watch(notes.sink(), options);
            waitFor(700); // looks at 250 and 550 ms
            QTRY_COMPARE_WITH_TIMEOUT(notes.count(u"ui-stall-end"_s), 1, 2000);
        }
        go = true;
        const auto all = notes.all();
        QVERIFY2(all.size() >= 4, notes.text().constData());
        QCOMPARE(all[0].event, u"ui-stall"_s);
#if defined(_M_X64)
        // Right after the first look, in parts small enough for the journal; not after the second.
        QCOMPARE(all[1].event, u"ui-stall-threads"_s);
        int heldUpThreads = 0;
        int different = 0;
        int parts = 0;
        for (const Noted& note : all) {
            if (note.event != u"ui-stall-threads"_s)
                continue;
            QCOMPARE(note.details.value(u"stall"_s).toInt(), 1);
            QCOMPARE(note.details.value(u"part"_s).toInt(), ++parts);
            QVERIFY2(QJsonDocument(note.details).toJson(QJsonDocument::Compact).size() < 7000, notes.text().constData());
            for (const QJsonValue thread : note.details.value(u"threads"_s).toArray()) {
                const QJsonObject one = thread.toObject();
                QVERIFY2(!one.value(u"frames"_s).toArray().isEmpty(), notes.text().constData());
                ++different;
                if (hasFrameOf(one, "tst_uistall.exe+"_L1))
                    heldUpThreads += one.value(u"count"_s).toInt(1);
            }
        }
        QVERIFY(parts >= 1);
        QCOMPARE(all[1].details.value(u"of"_s).toInt(), parts);
        QCOMPARE(all[static_cast<std::size_t>(parts) + 1].event, u"ui-stall"_s); // the second look
        // Sleeping or between two sleeps, in one stack or two: all three are there.
        QCOMPARE(heldUpThreads, 3);
        QVERIFY2(different < 3 + 20, notes.text().constData()); // the pools' threads at rest, once each kind
        QVERIFY2(hasFrameOf(all[0].details, "tst_uistall.exe+"_L1), notes.text().constData()); // the watched one's as before
#endif
    }

    // A long one: looked at again while it lasts, three times at most, then its whole length.
    void aLongOneIsLookedAtAgain()
    {
        Notes notes;
        UiStallWatch watch(notes.sink(), kQuick);
        waitFor(1700); // looks at 250, 550, 850 ms; no fourth at 1150 and 1450
        QTRY_COMPARE_WITH_TIMEOUT(notes.count(u"ui-stall-end"_s), 1, 2000);
        const auto all = notes.all();
        QCOMPARE(notes.count(u"ui-stall"_s), 3);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(all[static_cast<std::size_t>(i)].details.value(u"sample"_s).toInt(), i + 1);
        QVERIFY(all[2].details.value(u"ms"_s).toInteger() > all[0].details.value(u"ms"_s).toInteger());
        QCOMPARE(all.back().details.value(u"samples"_s).toInt(), 3);
        QVERIFY2(all.back().details.value(u"ms"_s).toInteger() >= 1500, notes.text().constData());
    }

    // One after another: each its own number; past the limit nothing more is written.
    void onlySoManyAreWritten()
    {
        Notes notes;
        UiStallWatch::Options options = kQuick;
        options.stalls = 2;
        UiStallWatch watch(notes.sink(), options);
        for (int i = 0; i < 3; ++i) {
            waitFor(450);
            QTest::qWait(300); // answering in between
        }
        QCOMPARE(notes.count(u"ui-stall"_s), 2);
        QCOMPARE(notes.count(u"ui-stall-end"_s), 2);
        const auto all = notes.all();
        QCOMPARE(all.front().details.value(u"stall"_s).toInt(), 1);
        QCOMPARE(all.back().details.value(u"stall"_s).toInt(), 2);
    }

    // Stuck as it is closed: gone at once, not after the wait it was in.
    void goesAtOnce()
    {
        Notes notes;
        UiStallWatch::Options options = kQuick;
        options.again = 60s;
        QElapsedTimer timer;
        {
            UiStallWatch watch(notes.sink(), options);
            waitFor(500); // one look; the watch now waits a minute for the answer
            QCOMPARE(notes.count(u"ui-stall"_s), 1);
            timer.start();
        }
        QVERIFY(timer.elapsed() < 1000);
        QCOMPARE(notes.count(u"ui-stall-end"_s), 0);
    }
};

QTEST_GUILESS_MAIN(UiStallTest)

#include "tst_uistall.moc"

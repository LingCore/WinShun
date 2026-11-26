#include "DiagnosticLog.h"
#include "TriggerJournal.h"
#include "platform/IdleTrim.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>

#include <atomic>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

// Allocate and dirty enough private memory to exceed IdleTrim's 32 MB threshold.
struct DirtyMemory {
    explicit DirtyMemory(std::size_t megabytes)
        : bytes(megabytes * 1024 * 1024)
        , data(static_cast<char*>(::VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
    {
        for (std::size_t i = 0; data && i < bytes; i += 4096)
            data[i] = static_cast<char>(i & 0xFF);
    }
    ~DirtyMemory()
    {
        if (data)
            ::VirtualFree(data, 0, MEM_RELEASE);
    }
    DirtyMemory(const DirtyMemory&) = delete;
    DirtyMemory& operator=(const DirtyMemory&) = delete;

    const std::size_t bytes;
    char* const data;
};

// The newest entry of this kind in the diagnostics journal; empty: none.
QJsonObject lastJournalled(QStringView event)
{
    QJsonObject found;
    const auto log = ws::diagnostics::current();
    if (!log)
        return found;
    for (const auto& file : log->snapshot()) {
        for (const QByteArray& line : file.bytes.split('\n')) {
            const QJsonObject entry = QJsonDocument::fromJson(line).object();
            if (entry.value(u"event"_s).toString() == event)
                found = entry.value(u"details"_s).toObject();
        }
    }
    return found;
}

} // namespace

class IdleTrimAndTriggerTest : public QObject {
    Q_OBJECT

private slots:
    void idleTrimCanBeCancelledAndDestroyedSafely()
    {
        const DirtyMemory memory(40);
        const DirtyMemory kept(1);
        QVERIFY(memory.data != nullptr);
        QVERIFY(kept.data != nullptr);
        const ws::PageSet keepSet({{reinterpret_cast<std::uintptr_t>(kept.data), kept.bytes}});
        std::atomic<bool> inUse {false};
        {
            ws::IdleTrim trim([]() -> const char* { return nullptr; }, [&] { return inUse.load(); },
                [&] { return keepSet; });
            trim.setAfterUse(30);
            trim.used(); // arm the timer with the shortened 30 ms interval
            QTRY_VERIFY_WITH_TIMEOUT(trim.trimmed(), 2000);
            QTRY_VERIFY_WITH_TIMEOUT(trim.lastTrim().atMs > 0, 2000);
            // Calling used() cancels any in-flight trim and resets the timer cleanly.
            trim.used();
            // Leaving scope destroys IdleTrim and joins any active std::jthread cleanly.
        }
        // Destroy right after a trim triggers to verify destructor stop+join safety
        // on the cancellable PageSet::release(stop_token) path.
        {
            const DirtyMemory more(40);
            ws::IdleTrim trim([]() -> const char* { return nullptr; }, [] { return false; },
                [&] { return keepSet; });
            trim.setAfterUse(10);
            trim.used();
            QTest::qWait(25);
        }
        QVERIFY(true);
    }

    // In use, nothing is given back by itself; freed() gives back all the same
    // (the settings' pages put away with the window open, GitHub #467).
    void freedGivesBackWhileInUse()
    {
        const DirtyMemory memory(40);
        QVERIFY(memory.data != nullptr);
        ws::IdleTrim trim([]() -> const char* { return nullptr; }, [] { return true; });
        trim.setAfterUse(20);
        trim.used();
        QTest::qWait(200); // several checks over: in use each time
        QVERIFY(!trim.trimmed());
        QCOMPARE(trim.lastTrim().atMs, qint64 {0});
        trim.freed();
        QTRY_VERIFY_WITH_TIMEOUT(trim.lastTrim().atMs > 0, 2000);
        QVERIFY(trim.trimmed());
        QVERIFY(trim.lastTrim().megabytes >= 30);
    }

    // What the diagnostics package says of it (summary.json "idle_trim").
    void statusTellsWhatHoldsTheMemory()
    {
        const DirtyMemory memory(40);
        const char* busy = "reading content";
        bool up = true;
        ws::IdleTrim trim([&] { return busy; }, [&] { return up; });
        QJsonObject status = trim.status();
        QCOMPARE(status.value(u"waiting_for").toString(), QStringLiteral("reading content"));
        QCOMPARE(status.value(u"window_up").toBool(), true);
        QCOMPARE(status.value(u"held_after_resume").toBool(), false);
        QCOMPARE(status.value(u"last_trim_at").toString(), QString());

        trim.resumed();
        QCOMPARE(trim.status().value(u"held_after_resume").toBool(), true);

        busy = nullptr;
        up = false;
        trim.setAfterUse(30);
        trim.used(); // in use again: no longer held
        QTRY_VERIFY_WITH_TIMEOUT(trim.lastTrim().atMs > 0, 2000);
        status = trim.status();
        QCOMPARE(status.value(u"waiting_for").toString(), QString());
        QCOMPARE(status.value(u"window_up").toBool(), false);
        QCOMPARE(status.value(u"held_after_resume").toBool(), false);
        QVERIFY(status.value(u"last_trim_at").toString().endsWith(u'Z'));
        QVERIFY(status.value(u"last_trim_mb").toInt() >= 40);
    }

    // One journal entry a giving back, written when it is over: what there
    // was, what was kept and what stayed (GitHub #459: "returned 42 MB" on
    // the page, and nothing to say that 600 MB were still there).
    void journalTellsWhatStayed()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        ws::diagnostics::initialize(temp.path());
        {
            const DirtyMemory memory(40);
            const DirtyMemory kept(1);
            const ws::PageSet keepSet({{reinterpret_cast<std::uintptr_t>(kept.data), kept.bytes}});
            ws::IdleTrim trim([]() -> const char* { return nullptr; }, [] { return false; }, [&] { return keepSet; });
            trim.setAfterUse(30);
            trim.used();
            QTRY_VERIFY_WITH_TIMEOUT(lastJournalled(u"idle-trim").contains(u"stayed_mb"_s), 3000);
            const QJsonObject entry = lastJournalled(u"idle-trim");
            const qint64 before = entry.value(entry.contains(u"private_mb"_s) ? u"private_mb"_s : u"working_set_mb"_s).toInteger(-1);
            QVERIFY(before >= 40);
            QCOMPARE(entry.value(u"keep_mb"_s).toInteger(-1), qint64 {1});
            QVERIFY(entry.value(u"stayed_mb"_s).toInteger(-1) <= before - 39); // all but the megabyte kept went
            QVERIFY(!entry.contains(u"stopped"_s));
            QCOMPARE(trim.status().value(u"kept_after_trim_mb"_s).toInteger(-1), entry.value(u"stayed_mb"_s).toInteger(-2));
        }
        ws::diagnostics::initialize({});
    }

    void triggerJournalRecordsPhaseTimingsAndMemory()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        ws::diagnostics::initialize(temp.path());

        ws::TriggerJournal journal;
        const std::uint64_t t0 = ::GetTickCount64();
        ws::TriggerJournal::Context ctx;
        ctx.helper = true;
        ctx.trimmed = true;
        ctx.hotMb = 42;
        ctx.idleSec = 1800;
        ctx.rawInput = "ok";

        journal.begin(ws::TriggerSource::Hotkey, t0, ctx);
        journal.decided("show", ws::TriggerWindow::Launcher);
        journal.front(ws::win::Front::Direct, 3.5);
        QTest::qWait(15);
        journal.showDone();
        QTest::qWait(15);
        journal.armed(ws::TriggerWindow::Launcher);
        QTest::qWait(15);
        journal.seen(ws::TriggerWindow::Launcher, true);

        const auto log = ws::diagnostics::current();
        QVERIFY(log != nullptr);
        const auto files = log->snapshot();
        QVERIFY(!files.empty());

        QJsonObject found;
        for (const auto& file : files) {
            for (const QByteArray& line : file.bytes.split('\n')) {
                if (line.trimmed().isEmpty())
                    continue;
                const QJsonObject obj = QJsonDocument::fromJson(line).object();
                if (obj.value(u"event"_s).toString() == u"trigger")
                    found = obj.value(u"details"_s).toObject();
            }
        }
        QVERIFY2(!found.isEmpty(), "trigger event was not written to diagnostics log");
        QCOMPARE(found.value(u"idle_s"_s).toInteger(-1), qint64 {1800});
        QCOMPARE(found.value(u"hot_mb"_s).toInteger(-1), qint64 {42});
        QVERIFY(found.contains(u"working_mb"_s));
        QVERIFY(found.value(u"working_mb"_s).toDouble() > 0.0);
        QVERIFY(found.contains(u"ws_delta_mb"_s));
        QVERIFY(found.contains(u"show_ms"_s));
        QVERIFY(found.contains(u"arm_ms"_s));
        QVERIFY(found.contains(u"frame_ms"_s));
        QVERIFY(found.value(u"arm_ms"_s).toInteger(-1) >= found.value(u"show_ms"_s).toInteger(0));
        QVERIFY(found.value(u"seen_ms"_s).toInteger(-1) >= found.value(u"arm_ms"_s).toInteger(0));
        QCOMPARE(found.value(u"timeout"_s).toBool(false), true);

        ws::diagnostics::initialize({});
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    IdleTrimAndTriggerTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_idletrim.moc"

// The PrtScn takeover's registry value and what keeps it taken (GitHub #322,
// src/capture/PrintScreen, src/app/platform/RegistryWatch). Everything here
// runs on a key of its own under HKCU\Software\WinShunTest: the real
// Control Panel\Keyboard value, and so the user's PrtScn key, is never written.
#include "PrintScreen.h"
#include "RegistryWatch.h"

#include <QScopeGuard>
#include <QTest>

#include <string>

using namespace ws;
using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

const std::wstring kTestParent = L"Software\\WinShunTest";
const std::wstring kTestKey = kTestParent + L"\\PrintScreen-" + std::to_wstring(::GetCurrentProcessId());

// The whole throwaway key, gone (a test that failed halfway leaves nothing).
void removeTestKey()
{
    ::RegDeleteTreeW(HKEY_CURRENT_USER, kTestKey.c_str());
    ::RegDeleteKeyW(HKEY_CURRENT_USER, kTestParent.c_str()); // only while no other run's key is in it
}

const std::optional<DWORD> kNone; // the value is absent

// What a watcher saw: how many times it was called, and the value then.
struct Seen {
    int calls = 0;
    std::optional<DWORD> last;
};

} // namespace

class TestPrintScreen : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { removeTestKey(); }
    void cleanupTestCase() { removeTestKey(); }

    void encodeAndDecodeRoundTrip()
    {
        for (const std::optional<DWORD>& value : {std::optional<DWORD> {}, std::optional<DWORD> {0}, std::optional<DWORD> {1},
                 std::optional<DWORD> {0xFFFFFFFFu}}) {
            std::optional<DWORD> back = DWORD {7};
            QVERIFY(printscreen::decode(printscreen::encode(value), &back));
            QCOMPARE(back, value);
        }
        QCOMPARE(printscreen::encode(std::nullopt), u"absent"_s);
        QCOMPARE(printscreen::encode(DWORD {0}), u"0"_s);
        std::optional<DWORD> value = DWORD {7};
        for (const QString& bad : {u""_s, u"yes"_s, u"-1"_s, u"1.5"_s, u"Absent"_s}) {
            QVERIFY2(!printscreen::decode(bad, &value), qPrintable(bad));
            QCOMPARE(value, std::optional<DWORD> {7}); // untouched on failure
        }
    }

    void snippingToolOwnsAnythingButZero()
    {
        QVERIFY(printscreen::snippingToolOwns(std::nullopt)); // absent: Windows 11's default is the Snipping Tool's
        QVERIFY(printscreen::snippingToolOwns(DWORD {1}));
        QVERIFY(printscreen::snippingToolOwns(DWORD {2}));
        QVERIFY(!printscreen::snippingToolOwns(DWORD {0}));
    }

    void valueReadAndWrittenOnAnotherKey()
    {
        const auto cleanup = qScopeGuard(removeTestKey);
        const std::optional<DWORD> real = printscreen::read(); // must be the same at the end
        QCOMPARE(printscreen::read(kTestKey.c_str()), kNone); // no key, no value

        QVERIFY(printscreen::write(DWORD {1}, kTestKey.c_str())); // makes the key
        QCOMPARE(printscreen::read(kTestKey.c_str()), std::optional<DWORD> {1});
        QVERIFY(printscreen::write(DWORD {0}, kTestKey.c_str()));
        QCOMPARE(printscreen::read(kTestKey.c_str()), std::optional<DWORD> {0});
        QVERIFY(printscreen::write(std::nullopt, kTestKey.c_str())); // deleted
        QCOMPARE(printscreen::read(kTestKey.c_str()), kNone);
        QVERIFY(printscreen::write(std::nullopt, kTestKey.c_str())); // already gone is no failure

        QCOMPARE(printscreen::read(), real);
    }

    void watchSeesAnotherWriterAndArmsAgain()
    {
        const auto cleanup = qScopeGuard(removeTestKey);
        QVERIFY(printscreen::write(DWORD {0}, kTestKey.c_str()));
        Seen seen;
        RegistryWatch watch(kTestKey.c_str(), [&] {
            ++seen.calls;
            seen.last = printscreen::read(kTestKey.c_str());
        });
        QVERIFY(watch.active());
        QCOMPARE(seen.calls, 0); // nothing yet: the watch starts quiet

        QVERIFY(printscreen::write(DWORD {1}, kTestKey.c_str())); // "Windows puts it back"
        QTRY_VERIFY_WITH_TIMEOUT(seen.calls >= 1, 5000);
        QCOMPARE(seen.last, std::optional<DWORD> {1});

        int calls = seen.calls;
        QVERIFY(printscreen::write(DWORD {2}, kTestKey.c_str())); // again: armed again after the first
        QTRY_VERIFY_WITH_TIMEOUT(seen.calls > calls, 5000);
        QCOMPARE(seen.last, std::optional<DWORD> {2});

        calls = seen.calls;
        QVERIFY(printscreen::write(std::nullopt, kTestKey.c_str())); // deleted is a change too
        QTRY_VERIFY_WITH_TIMEOUT(seen.calls > calls, 5000);
        QCOMPARE(seen.last, kNone);
    }

    void watchGoneSeesNothing()
    {
        const auto cleanup = qScopeGuard(removeTestKey);
        QVERIFY(printscreen::write(DWORD {0}, kTestKey.c_str()));
        Seen seen;
        auto watch = std::make_unique<RegistryWatch>(kTestKey.c_str(), [&] { ++seen.calls; });
        QVERIFY(watch->active());
        watch.reset(); // as CaptureApp::shutdown() does before it puts the old value back

        QVERIFY(printscreen::write(DWORD {1}, kTestKey.c_str()));
        QTest::qWait(300);
        QCOMPARE(seen.calls, 0);
    }

    // CaptureApp::printScreenChanged() on the test key: taken again at once, our own
    // write is a no-op, and the 4th time within the window is left alone.
    void watchTakesTheKeyBackUntilItIsATugOfWar()
    {
        const auto cleanup = qScopeGuard(removeTestKey);
        QVERIFY(printscreen::write(DWORD {0}, kTestKey.c_str()));
        printscreen::RetakeLimit limit;
        int retakes = 0;
        int refused = 0;
        RegistryWatch watch(kTestKey.c_str(), [&] {
            if (!printscreen::snippingToolOwns(printscreen::read(kTestKey.c_str())))
                return; // ours (0) after a retake of ours
            if (limit.allow(printscreen::RetakeLimit::Clock::now())) {
                ++retakes;
                printscreen::write(DWORD {0}, kTestKey.c_str());
            } else
                ++refused;
        });
        QVERIFY(watch.active());

        for (int round = 1; round <= 3; ++round) {
            QVERIFY(printscreen::write(DWORD {1}, kTestKey.c_str()));
            QTRY_COMPARE_WITH_TIMEOUT(retakes, round, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(printscreen::read(kTestKey.c_str()), std::optional<DWORD> {0}, 5000);
        }
        QCOMPARE(refused, 0);

        QVERIFY(printscreen::write(DWORD {1}, kTestKey.c_str())); // the 4th within a minute
        QTRY_COMPARE_WITH_TIMEOUT(refused, 1, 5000);
        QCOMPARE(retakes, 3);
        QCOMPARE(printscreen::read(kTestKey.c_str()), std::optional<DWORD> {1}); // left as it is
    }

    void retakeLimitRefusesTheFourthWithinTheWindow()
    {
        using Clock = printscreen::RetakeLimit::Clock;
        const Clock::time_point t0 = Clock::now();
        printscreen::RetakeLimit limit;
        QVERIFY(limit.allow(t0));
        QVERIFY(limit.allow(t0 + 1s));
        QVERIFY(limit.allow(t0 + 2s));
        QVERIFY(!limit.allow(t0 + 3s)); // the 4th within 60 s
        QVERIFY(!limit.allow(t0 + 59s)); // still: the first is 59 s old; refusals are not counted
        QVERIFY(limit.allow(t0 + 60s)); // the first one is out of the window
        QVERIFY(!limit.allow(t0 + 60s)); // and that one counted: 1 s, 2 s, 60 s
        QVERIFY(limit.allow(t0 + 61s)); // 1 s is out
        QVERIFY(!limit.allow(t0 + 61s));
    }

    void retakeLimitAllowsASlowTugOfWar()
    {
        using Clock = printscreen::RetakeLimit::Clock;
        const Clock::time_point t0 = Clock::now();
        printscreen::RetakeLimit limit;
        for (int i = 0; i < 20; ++i)
            QVERIFY(limit.allow(t0 + i * 30s)); // two in any minute, never more
    }

    void retakeLimitStartsOverWhenCleared()
    {
        using Clock = printscreen::RetakeLimit::Clock;
        const Clock::time_point t0 = Clock::now();
        printscreen::RetakeLimit limit;
        for (int i = 0; i < 3; ++i)
            QVERIFY(limit.allow(t0));
        QVERIFY(!limit.allow(t0));
        limit.clear(); // CaptureApp::apply()
        QVERIFY(limit.allow(t0));
    }
};

QTEST_GUILESS_MAIN(TestPrintScreen)
#include "tst_printscreen.moc"

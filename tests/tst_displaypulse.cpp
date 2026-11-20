// The screen's and the desktop's beat beside the capture (DisplayPulse.h,
// GitHub #325), on this PC's real monitor and compositor: a beat is about one
// refresh (4–17 ms here), so "small" is under 100 ms; this thread standing
// still (as the capture's does in a stall) must not show as a gap of theirs.
#include "DisplayPulse.h"

#include <QElapsedTimer>
#include <QTest>

#include <chrono>
#include <memory>
#include <thread>

#include <windows.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using ws::capture::DisplayPulse;

namespace {

constexpr int kSmallMs = 100;

QString primaryMonitor()
{
    MONITORINFOEXW info {};
    info.cbSize = sizeof info;
    ::GetMonitorInfoW(::MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info);
    return QString::fromWCharArray(info.szDevice);
}

} // namespace

class DisplayPulseTest : public QObject {
    Q_OBJECT

private slots:
    void nothingIsMeasuredUnarmed()
    {
        DisplayPulse pulse;
        DisplayPulse::Gaps gaps = pulse.take();
        QCOMPARE(gaps.vblankMs, -1);
        QCOMPARE(gaps.dwmMs, -1);
        pulse.arm(primaryMonitor());
        std::this_thread::sleep_for(50ms);
        pulse.disarm();
        gaps = pulse.take();
        QCOMPARE(gaps.vblankMs, -1);
        QCOMPARE(gaps.dwmMs, -1);
    }

    void bothBeatAboutARefreshApart()
    {
        DisplayPulse pulse;
        pulse.arm(primaryMonitor());
        for (int round = 0; round < 5; ++round) {
            std::this_thread::sleep_for(60ms); // more than a tick of the capture's, ever
            const DisplayPulse::Gaps gaps = pulse.take();
            qInfo() << "vblank" << gaps.vblankMs << "ms, dwm" << gaps.dwmMs << "ms";
            QVERIFY2(gaps.vblankMs >= 0 && gaps.vblankMs < kSmallMs, qPrintable(QString::number(gaps.vblankMs)));
            QVERIFY2(gaps.dwmMs >= 0 && gaps.dwmMs < kSmallMs, qPrintable(QString::number(gaps.dwmMs)));
            QVERIFY(gaps.vblankError.isEmpty());
        }
    }

    // The capture's own thread stuck for a second, the screen and desktop not.
    void thisThreadStandingStillIsNoGapOfTheirs()
    {
        DisplayPulse pulse;
        pulse.arm(primaryMonitor());
        std::this_thread::sleep_for(50ms);
        pulse.take();
        std::this_thread::sleep_for(1000ms);
        const DisplayPulse::Gaps gaps = pulse.take();
        qInfo() << "after 1 s: vblank" << gaps.vblankMs << "ms, dwm" << gaps.dwmMs << "ms";
        QVERIFY(gaps.vblankMs >= 0 && gaps.vblankMs < kSmallMs);
        QVERIFY(gaps.dwmMs >= 0 && gaps.dwmMs < kSmallMs);
    }

    // A monitor that can't be followed: the error kept, the vertical blanks
    // not measured after the retries, the compositions still.
    void aMonitorNotFollowedSaysSo()
    {
        DisplayPulse pulse;
        pulse.arm(u"\\\\.\\DISPLAY99"_s);
        std::this_thread::sleep_for(1500ms); // past 20 retries 50 ms apart
        const DisplayPulse::Gaps gaps = pulse.take();
        qInfo() << "vblank" << gaps.vblankMs << gaps.vblankError << "dwm" << gaps.dwmMs;
        QCOMPARE(gaps.vblankMs, -1);
        QVERIFY(gaps.vblankError.startsWith(u"0x"_s));
        QVERIFY(gaps.dwmMs >= 0 && gaps.dwmMs < kSmallMs);
        // Armed again on a real one: followed from then on.
        pulse.arm(primaryMonitor());
        std::this_thread::sleep_for(100ms);
        const DisplayPulse::Gaps again = pulse.take();
        QVERIFY(again.vblankMs >= 0 && again.vblankMs < kSmallMs);
        QVERIFY(again.vblankError.isEmpty());
    }

    // Moved to another monitor while armed.
    void armedAgainOnAnotherMonitor()
    {
        QStringList names;
        ::EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM out) -> BOOL {
            MONITORINFOEXW info {};
            info.cbSize = sizeof info;
            if (::GetMonitorInfoW(monitor, &info))
                reinterpret_cast<QStringList*>(out)->append(QString::fromWCharArray(info.szDevice));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&names));
        DisplayPulse pulse;
        for (const QString& name : names) {
            pulse.arm(name);
            std::this_thread::sleep_for(100ms);
            const DisplayPulse::Gaps gaps = pulse.take();
            qInfo() << name << "vblank" << gaps.vblankMs << "ms";
            QVERIFY2(gaps.vblankMs >= 0 && gaps.vblankMs < kSmallMs, qPrintable(name));
        }
    }

    // Gone while its threads wait in the driver: at once, and they end by themselves.
    void goesAtOnce()
    {
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 20; ++i) {
            auto pulse = std::make_unique<DisplayPulse>();
            pulse->arm(primaryMonitor());
            std::this_thread::sleep_for(5ms);
        }
        QVERIFY(timer.elapsed() < 1000);
    }
};

QTEST_GUILESS_MAIN(DisplayPulseTest)
#include "tst_displaypulse.moc"

#include "FrameReveal.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSignalBlocker>
#include <QTest>

#include <thread>

using namespace std::chrono_literals;

namespace {

void prepare(QQuickWindow& window)
{
    window.setFlags(Qt::Tool | Qt::FramelessWindowHint);
    window.setGeometry(12000, 12000, 320, 80);
    window.create();
}

// Control render/GUI ordering without relying on a machine's frame timing.
// The window and the callback delivery are real; these signals stand in
// for the render thread reaching each boundary while the GUI is busy.
void frame(QQuickWindow& window, bool synchronize = true)
{
    std::thread render([&window, synchronize] {
        if (synchronize)
            emit window.afterSynchronizing();
        emit window.frameSwapped();
    });
    render.join();
}

} // namespace

class FrameRevealTest : public QObject {
    Q_OBJECT

private slots:
    void waitsForResultsAndGui()
    {
        QQuickWindow window;
        prepare(window);
        int revealed = 0;
        ws::FrameReveal reveal;
        reveal.setWindow(&window, 150ms, [&] { ++revealed; });
        reveal.begin();
        window.show();
        frame(window); // a frame before the results are ready
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(reveal.pending());
        QCOMPARE(revealed, 0);
        reveal.arm();
        frame(window);
        QVERIFY(reveal.pending()); // the GUI has not revealed it yet
        QCOMPARE(revealed, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(!reveal.pending());
        QCOMPARE(revealed, 1);
    }

    void queuedOldFrameDoesNotRevealOrCancelNewFallback()
    {
        QQuickWindow window;
        prepare(window);
        int revealed = 0;
        ws::FrameReveal reveal;
        reveal.setWindow(&window, 60ms, [&] { ++revealed; });
        reveal.begin();
        reveal.arm();
        window.show();
        frame(window); // posted for the first opening, GUI still busy
        window.hide();
        reveal.begin();
        reveal.arm();
        window.show();
        const QSignalBlocker noNewFrame(&window);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(reveal.pending());
        QCOMPARE(revealed, 0);
        QTRY_COMPARE_WITH_TIMEOUT(revealed, 1, 1000); // the second opening's fallback still runs
        QVERIFY(!reveal.pending());
    }

    void inFlightOldFrameDoesNotRevealNewOpening()
    {
        QQuickWindow window;
        prepare(window);
        int revealed = 0;
        ws::FrameReveal reveal;
        reveal.setWindow(&window, 150ms, [&] { ++revealed; });
        reveal.begin();
        reveal.arm();
        window.show();
        std::thread sync([&window] { emit window.afterSynchronizing(); });
        sync.join();
        window.hide();
        reveal.begin();
        reveal.arm();
        window.show();
        frame(window, false); // the previous frame reaches the screen only now
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(reveal.pending());
        QCOMPARE(revealed, 0);
        frame(window);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(!reveal.pending());
        QCOMPARE(revealed, 1);
    }

    void hideCancelsQueuedFrameAndFallback()
    {
        QQuickWindow window;
        prepare(window);
        int revealed = 0;
        ws::FrameReveal reveal;
        reveal.setWindow(&window, 60ms, [&] { ++revealed; });
        reveal.begin();
        reveal.arm();
        window.show();
        frame(window);
        window.hide();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QTest::qWait(100);
        QVERIFY(!reveal.pending());
        QCOMPARE(revealed, 0);
    }

    void fallbackDoesNotWaitForeverForResults()
    {
        QQuickWindow window;
        prepare(window);
        int revealed = 0;
        ws::FrameReveal reveal;
        reveal.setWindow(&window, 60ms, [&] { ++revealed; });
        const QSignalBlocker noFrame(&window);
        reveal.begin();
        window.show(); // no results and no frame signal
        QTRY_COMPARE_WITH_TIMEOUT(revealed, 1, 1000);
        QVERIFY(!reveal.pending());
        QVERIFY(reveal.timedOut());
    }

    void timedOutReflectsRealFrameVsFallback()
    {
        QQuickWindow window;
        prepare(window);
        int revealed = 0;
        bool lastTimedOut = true;
        ws::FrameReveal reveal;
        reveal.setWindow(&window, 150ms, [&] {
            ++revealed;
            lastTimedOut = reveal.timedOut();
        });
        reveal.begin();
        reveal.arm();
        window.show();
        frame(window);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(revealed, 1);
        QVERIFY(!lastTimedOut);
        QVERIFY(!reveal.timedOut());
    }

    void armGivesFullFrameBudgetAfterResultsWait()
    {
        QQuickWindow window;
        prepare(window);
        int revealed = 0;
        bool lastTimedOut = false;
        ws::FrameReveal reveal;
        reveal.setWindow(&window, 120ms, [&] {
            ++revealed;
            lastTimedOut = reveal.timedOut();
        });
        const QSignalBlocker noFrame(&window);
        reveal.begin();
        window.show();
        QTest::qWait(80);
        QCOMPARE(revealed, 0);
        // Arm near the initial deadline: render thread still gets a full 120 ms budget.
        reveal.arm();
        QTest::qWait(60);
        QCOMPARE(revealed, 0);
        QTRY_COMPARE_WITH_TIMEOUT(revealed, 1, 1000);
        QVERIFY(lastTimedOut);
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    FrameRevealTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_framereveal.moc"

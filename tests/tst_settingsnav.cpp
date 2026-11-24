#include "SettingsNavigation.h"
#include "WindowFrame.h"

#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <windows.h>

#include <memory>

using namespace Qt::StringLiterals;
using namespace ws;

class SettingsNavTest : public QObject {
    Q_OBJECT

private slots:
    void historyPushBackForwardAndScroll()
    {
        SettingsHistory history;
        QCOMPARE(history.count(), 0);
        QVERIFY(!history.canBack());
        QVERIFY(!history.canForward());

        history.reset(u"general"_s, 0, {}, {}, 0);
        QCOMPARE(history.count(), 1);
        QCOMPARE(history.currentIndex(), 0);
        QVERIFY(!history.canBack());
        QVERIFY(!history.canForward());

        // Scroll on general, then navigate to keys › tab 1, then plugins detail.
        history.updateScroll(180);
        QVERIFY(history.push(u"keys"_s, 1, {}, {}, 0));
        history.updateScroll(95);
        QVERIFY(history.push(u"plugins"_s, 0, u"winshun.capture"_s, {}, 40));

        QCOMPARE(history.count(), 3);
        QCOMPARE(history.currentIndex(), 2);
        QVERIFY(history.canBack());
        QVERIFY(!history.canForward());
        QCOMPARE(history.backEntry().value(u"pageId"_s).toString(), u"keys"_s);
        QCOMPARE(history.backEntry().value(u"tab"_s).toInt(), 1);

        // Consecutive push to the same place updates scrollY without duplicating.
        QVERIFY(!history.push(u"plugins"_s, 0, u"winshun.capture"_s, {}, 210));
        QCOMPARE(history.count(), 3);
        QCOMPARE(history.currentEntry().value(u"scrollY"_s).toDouble(), 210.0);

        // Step back twice: restores exact page, tab, and scrollY.
        const QVariantMap step1 = history.back();
        QCOMPARE(step1.value(u"pageId"_s).toString(), u"keys"_s);
        QCOMPARE(step1.value(u"tab"_s).toInt(), 1);
        QCOMPARE(step1.value(u"scrollY"_s).toDouble(), 95.0);
        QVERIFY(history.canBack());
        QVERIFY(history.canForward());

        const QVariantMap step2 = history.back();
        QCOMPARE(step2.value(u"pageId"_s).toString(), u"general"_s);
        QCOMPARE(step2.value(u"scrollY"_s).toDouble(), 180.0);
        QVERIFY(!history.canBack());
        QVERIFY(history.canForward());
        QVERIFY(history.back().isEmpty());

        // Step forward once, then branch to a new page: forward stack must truncate.
        const QVariantMap fwd = history.forward();
        QCOMPARE(fwd.value(u"pageId"_s).toString(), u"keys"_s);
        QCOMPARE(history.forwardEntries().size(), 1);

        QVERIFY(history.push(u"appearance"_s, 0, {}, {}, 0));
        QCOMPARE(history.count(), 3);
        QCOMPARE(history.currentIndex(), 2);
        QVERIFY(!history.canForward());
        QCOMPARE(history.backEntry().value(u"pageId"_s).toString(), u"keys"_s);
    }

    void historySearchAndPluginDetailAndJumpTo()
    {
        SettingsHistory history;
        history.reset(u"general"_s, 0, {}, {}, 120);
        QVERIFY(history.push(u"general"_s, 0, {}, u"代理"_s, 30));
        QVERIFY(history.push(u"general"_s, 0, {}, {}, 400)); // search hit on same page
        QVERIFY(history.push(u"plugins"_s, 0, {}, {}, 60));
        QVERIFY(history.push(u"plugins"_s, 0, u"winshun.web"_s, {}, 0));

        QCOMPARE(history.count(), 5);
        // Back entries are ordered newest first.
        const QVariantList back = history.backEntries();
        QCOMPARE(back.size(), 4);
        QCOMPARE(back[0].toMap().value(u"pageId"_s).toString(), u"plugins"_s);
        QCOMPARE(back[0].toMap().value(u"pluginId"_s).toString(), u""_s);
        QCOMPARE(back[2].toMap().value(u"query"_s).toString(), u"代理"_s);
        QCOMPARE(back[3].toMap().value(u"historyIndex"_s).toInt(), 0);

        // Jump straight back to the search entry (index 1).
        const QVariantMap jumped = history.jumpTo(1);
        QCOMPARE(jumped.value(u"query"_s).toString(), u"代理"_s);
        QCOMPARE(jumped.value(u"scrollY"_s).toDouble(), 30.0);
        QCOMPARE(history.currentIndex(), 1);

        // Forward entries are ordered nearest first.
        const QVariantList forward = history.forwardEntries();
        QCOMPARE(forward.size(), 3);
        QCOMPARE(forward[0].toMap().value(u"historyIndex"_s).toInt(), 2);
        QCOMPARE(forward[2].toMap().value(u"pluginId"_s).toString(), u"winshun.web"_s);

        // Restoring guard blocks accidental pushes and scroll overwrites.
        history.setRestoring(true);
        QVERIFY(!history.push(u"health"_s, 0));
        history.updateScroll(999);
        history.replaceCurrent(u"health"_s, 0);
        history.setRestoring(false);
        QCOMPARE(history.currentIndex(), 1);
        QCOMPARE(history.currentEntry().value(u"query"_s).toString(), u"代理"_s);
        QCOMPARE(history.currentEntry().value(u"scrollY"_s).toDouble(), 30.0);

        // replaceCurrent works when not restoring.
        history.replaceCurrent(u"ai"_s, 0, {}, {}, 15);
        QCOMPARE(history.currentEntry().value(u"pageId"_s).toString(), u"ai"_s);
        QCOMPARE(history.currentEntry().value(u"query"_s).toString(), u""_s);
    }

    void historyCapacityAndDropdownLimits()
    {
        SettingsHistory history;
        for (int i = 0; i < SettingsHistory::kMaxEntries + 10; ++i)
            QVERIFY(history.push(u"page-%1"_s.arg(i), 0));

        QCOMPARE(history.count(), SettingsHistory::kMaxEntries);
        QCOMPARE(history.currentIndex(), SettingsHistory::kMaxEntries - 1);
        QCOMPARE(history.currentEntry().value(u"pageId"_s).toString(),
            u"page-%1"_s.arg(SettingsHistory::kMaxEntries + 9));
        QCOMPARE(history.backEntries().size(), SettingsHistory::kDropdownLimit);

        // Jump to start and check forward dropdown cap.
        const QVariantMap first = history.jumpTo(0);
        QCOMPARE(first.value(u"pageId"_s).toString(), u"page-10"_s);
        QCOMPARE(history.forwardEntries().size(), SettingsHistory::kDropdownLimit);
    }

    void edgeGateBurstSettleAndStandardTrigger()
    {
        qint64 now = 1000;
        EdgeScrollGate gate;
        gate.setClock([&] { return now; });
        gate.setCanPrevious(true);
        gate.setCanNext(true);

        QCOMPARE(gate.sensitivity(), u"standard"_s);
        QCOMPARE(gate.threshold(), 360);
        QCOMPARE(gate.settleMs(), 340);
        QCOMPARE(gate.minArmMs(), 280);

        QSignalSpy armedSpy(&gate, &EdgeScrollGate::armedDirectionChanged);
        QSignalSpy triggeredSpy(&gate, &EdgeScrollGate::triggered);

        // Scroll arrives at the bottom boundary.
        gate.noteBoundaryReached();

        // Fast wheel burst hitting the bottom (every 80 ms) is silently absorbed.
        for (int i = 0; i < 6; ++i) {
            now += 80;
            QVERIFY(!gate.overscroll(1, 120));
            QVERIFY(!gate.armed());
            QCOMPARE(gate.direction(), 0);
        }
        QCOMPARE(armedSpy.count(), 0);
        QCOMPARE(triggeredSpy.count(), 0);

        // After settling past settleMs (340 ms), notch 1 arms at 1/3 progress.
        now += 360;
        QVERIFY(!gate.overscroll(1, 120));
        QVERIFY(gate.armed());
        QVERIFY(!gate.readyToSwitch());
        QCOMPARE(gate.direction(), 1);
        QVERIFY(qAbs(gate.progress() - 1.0 / 3.0) < 0.001);
        QCOMPARE(armedSpy.count(), 1);
        QCOMPARE(armedSpy.last().at(0).toInt(), 1);

        // Notch 2 advances progress to 2/3.
        now += 150;
        QVERIFY(!gate.overscroll(1, 120));
        QVERIFY(gate.armed());
        QVERIFY(!gate.readyToSwitch());
        QVERIFY(qAbs(gate.progress() - 2.0 / 3.0) < 0.001);

        // Notch 3 past minArmMs (300 ms >= 280 ms) triggers the switch and clears armed state.
        now += 150;
        QVERIFY(gate.overscroll(1, 120));
        QVERIFY(!gate.armed());
        QVERIFY(!gate.readyToSwitch());
        QCOMPARE(gate.direction(), 0);
        QCOMPARE(triggeredSpy.count(), 1);
        QCOMPARE(triggeredSpy.last().at(0).toInt(), 1);

        // Trailing same-direction ticks right after switching are consumed and do not double-skip.
        now += 50;
        QVERIFY(gate.shouldConsumeWheel(1));
        QVERIFY(!gate.overscroll(1, 120));
        QCOMPARE(triggeredSpy.count(), 1);

        // Opposite direction immediately releases the trailing lock.
        now += 20;
        QVERIFY(!gate.shouldConsumeWheel(-1));
    }

    void edgeGateFastFlickHeldUntilMinArmAndClamped()
    {
        qint64 now = 2000;
        EdgeScrollGate gate;
        gate.setClock([&] { return now; });
        gate.setCanPrevious(true);
        gate.setCanNext(true);

        QSignalSpy triggeredSpy(&gate, &EdgeScrollGate::triggered);

        // A single coalesced giant delta (480) is clamped to maxTickDelta (120) so it never skips the cue.
        QVERIFY(!gate.overscroll(1, 480));
        QVERIFY(gate.armed());
        QVERIFY(!gate.readyToSwitch());
        QVERIFY(qAbs(gate.progress() - 120.0 / 360.0) < 0.001);

        // Rapid follow-up ticks within minArmMs (280 ms) charge the cue to full (readyToSwitch),
        // giving clear visual feedback without flipping accidentally.
        now += 40;
        QVERIFY(!gate.overscroll(1, 120));
        now += 40;
        QVERIFY(!gate.overscroll(1, 120));
        QVERIFY(gate.armed());
        QVERIFY(gate.readyToSwitch());
        QCOMPARE(gate.progress(), 1.0);
        QCOMPARE(triggeredSpy.count(), 0);

        now += 40; // still only 120 ms since armed (< 280 ms)
        QVERIFY(!gate.overscroll(1, 120));
        QVERIFY(gate.readyToSwitch());
        QCOMPARE(triggeredSpy.count(), 0);

        // Once minArmMs has elapsed, one deliberate push completes the switch.
        now += 180; // 300 ms since armed >= 280 ms
        QVERIFY(gate.overscroll(1, 120));
        QCOMPARE(triggeredSpy.count(), 1);
        QVERIFY(!gate.armed());
        QVERIFY(!gate.readyToSwitch());
    }

    void edgeGateSensitivityPresetsAndOff()
    {
        qint64 now = 3000;
        EdgeScrollGate gate;
        gate.setClock([&] { return now; });
        gate.setCanPrevious(true);
        gate.setCanNext(true);

        // Light preset: 2 notches (240) with shorter settle & minArmMs (140 ms).
        gate.setSensitivity(u"light"_s);
        QCOMPARE(gate.sensitivity(), u"light"_s);
        QCOMPARE(gate.threshold(), 240);
        QCOMPARE(gate.minArmMs(), 140);
        QVERIFY(!gate.overscroll(1, 120));
        QCOMPARE(gate.progress(), 0.5);
        now += 150;
        QVERIFY(gate.overscroll(1, 120));

        // Steady preset: 5 notches (600) with longer settle & minArmMs (420 ms).
        now += 1000;
        gate.setSensitivity(u"steady"_s);
        QCOMPARE(gate.sensitivity(), u"steady"_s);
        QCOMPARE(gate.threshold(), 600);
        QCOMPARE(gate.minArmMs(), 420);
        for (int i = 0; i < 4; ++i) {
            now += 120;
            QVERIFY(!gate.overscroll(1, 120));
            QVERIFY(gate.armed());
        }
        now += 120; // 480 ms since first arm >= 420 ms, accumulated = 600
        QVERIFY(gate.overscroll(1, 120));

        // Off mode: clears state, ignores overscroll and does not lock trailing wheels.
        now += 1000;
        QVERIFY(!gate.overscroll(1, 120));
        QVERIFY(gate.armed());
        gate.setSensitivity(u"off"_s);
        QCOMPARE(gate.sensitivity(), u"off"_s);
        QVERIFY(!gate.armed());
        QVERIFY(!gate.overscroll(1, 120));
        QVERIFY(!gate.armed());
        gate.noteSwitched(1);
        QVERIFY(!gate.shouldConsumeWheel(1));

        // Invalid sensitivity value is ignored.
        gate.setSensitivity(u"turbo"_s);
        QCOMPARE(gate.sensitivity(), u"off"_s);
    }

    void edgeGateHighResReversalDisarmAndBounds()
    {
        qint64 now = 5000;
        EdgeScrollGate gate;
        gate.setClock([&] { return now; });
        gate.setSensitivity(u"light"_s);
        gate.setCanPrevious(false);
        gate.setCanNext(true);

        QSignalSpy triggeredSpy(&gate, &EdgeScrollGate::triggered);

        // No previous view: upward overscroll never arms.
        QVERIFY(!gate.overscroll(-1, 120));
        QVERIFY(!gate.armed());

        // High-resolution small deltas accumulate smoothly (30 * 8 = 240 over 140+ ms).
        for (int i = 0; i < 7; ++i) {
            now += 25;
            QVERIFY(!gate.overscroll(1, 30));
            QVERIFY(gate.armed());
        }
        QVERIFY(qAbs(gate.progress() - 210.0 / 240.0) < 0.001);
        now += 25;
        QVERIFY(gate.overscroll(1, 30));
        QCOMPARE(triggeredSpy.count(), 1);

        // Advance past switchCooldownMs and enable both directions.
        gate.setCanPrevious(true);
        now += 500;

        // Arm downward, then reverse upward: cancels downward cleanly instead of switching.
        QVERIFY(!gate.overscroll(1, 120));
        QCOMPARE(gate.direction(), 1);
        now += 50;
        QVERIFY(!gate.overscroll(-1, 120));
        QCOMPARE(gate.direction(), 0);
        QVERIFY(!gate.armed());

        // Arm upward, wait past disarmMs: stale progress resets instead of completing on the next notch.
        now += 50;
        QVERIFY(!gate.overscroll(-1, 120));
        QCOMPARE(gate.progress(), 0.5);
        now += gate.disarmMs() + 100;
        QVERIFY(!gate.overscroll(-1, 120)); // starts fresh at 0.5, does not trigger
        QCOMPARE(gate.progress(), 0.5);
        QCOMPARE(triggeredSpy.count(), 1);

        // Disabled gate clears armed state immediately.
        gate.setEnabled(false);
        QVERIFY(!gate.armed());
        QVERIFY(!gate.overscroll(-1, 240));
    }

    void windowFrameBrowserNavigation()
    {
        QQuickWindow window;
        window.resize(400, 300);
        WindowFrame frame(true);
        frame.setWindow(&window);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QSignalSpy backSpy(&frame, &WindowFrame::navigateBack);
        QSignalSpy forwardSpy(&frame, &WindowFrame::navigateForward);

        const HWND hwnd = reinterpret_cast<HWND>(window.winId());

        // Disabled by default: neither Win32 messages nor Qt events fire signals.
        QVERIFY(!frame.browserNavigation());
        ::SendMessageW(hwnd, WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON1), MAKELPARAM(40, 80));
        ::SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), MAKELPARAM(40, 80));
        ::SendMessageW(hwnd, WM_APPCOMMAND, reinterpret_cast<WPARAM>(hwnd),
            MAKELPARAM(0, APPCOMMAND_BROWSER_BACKWARD));
        QCoreApplication::processEvents();
        QCOMPARE(backSpy.count(), 0);

        // Opt in: native client XButton1/XButton2 fire once each.
        frame.setBrowserNavigation(true);
        QCOMPARE(::SendMessageW(hwnd, WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON1), MAKELPARAM(40, 80)), LRESULT(TRUE));
        QCOMPARE(::SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), MAKELPARAM(40, 80)), LRESULT(TRUE));
        QCoreApplication::processEvents();
        QCOMPARE(backSpy.count(), 1);
        QCOMPARE(forwardSpy.count(), 0);

        // Rapid double-click (WM_XBUTTONDBLCLK + WM_XBUTTONUP) also fires the second click.
        QCOMPARE(::SendMessageW(hwnd, WM_XBUTTONDBLCLK, MAKEWPARAM(0, XBUTTON1), MAKELPARAM(40, 80)), LRESULT(TRUE));
        QCOMPARE(::SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), MAKELPARAM(40, 80)), LRESULT(TRUE));
        QCoreApplication::processEvents();
        QCOMPARE(backSpy.count(), 2);

        // Non-client title bar XButton2 fires forward.
        QCOMPARE(::SendMessageW(hwnd, WM_NCXBUTTONDOWN, MAKEWPARAM(HTCAPTION, XBUTTON2), MAKELPARAM(100, 10)), LRESULT(TRUE));
        QCOMPARE(::SendMessageW(hwnd, WM_NCXBUTTONUP, MAKEWPARAM(HTCAPTION, XBUTTON2), MAKELPARAM(100, 10)), LRESULT(TRUE));
        QCoreApplication::processEvents();
        QCOMPARE(forwardSpy.count(), 1);

        // Stray XButtonUp without prior press does not spuriously fire.
        QCOMPARE(::SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), MAKELPARAM(40, 80)), LRESULT(TRUE));
        QCoreApplication::processEvents();
        QCOMPARE(backSpy.count(), 2);

        // WM_APPCOMMAND (touchpad back/forward gesture or media keys).
        QCOMPARE(::SendMessageW(hwnd, WM_APPCOMMAND, reinterpret_cast<WPARAM>(hwnd),
            MAKELPARAM(0, APPCOMMAND_BROWSER_BACKWARD)), LRESULT(TRUE));
        QCOMPARE(::SendMessageW(hwnd, WM_APPCOMMAND, reinterpret_cast<WPARAM>(hwnd),
            MAKELPARAM(0, APPCOMMAND_BROWSER_FORWARD)), LRESULT(TRUE));
        QCoreApplication::processEvents();
        QCOMPARE(backSpy.count(), 3);
        QCOMPARE(forwardSpy.count(), 2);

        // Synthetic Qt BackButton / ForwardButton events also work.
        QMouseEvent backPress(QEvent::MouseButtonPress, {40, 80}, window.mapToGlobal(QPointF(40, 80)),
            Qt::BackButton, Qt::BackButton, Qt::NoModifier);
        QMouseEvent backRelease(QEvent::MouseButtonRelease, {40, 80}, window.mapToGlobal(QPointF(40, 80)),
            Qt::BackButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &backPress);
        QCoreApplication::sendEvent(&window, &backRelease);
        QCOMPARE(backSpy.count(), 4);

        QMouseEvent fwdRelease(QEvent::MouseButtonRelease, {40, 80}, window.mapToGlobal(QPointF(40, 80)),
            Qt::ForwardButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &fwdRelease);
        QCOMPARE(forwardSpy.count(), 3);
    }

    void listWheelScrollOverscrollIntegration()
    {
        qint64 now = 1000;
        EdgeScrollGate gate;
        gate.setClock([&] { return now; });
        gate.setCanPrevious(true);
        gate.setCanNext(true);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(u"gate"_s, &gate);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            Window {
                width: 480; height: 320; visible: true
                Flickable {
                    id: flick
                    objectName: "flick"
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: 600
                    boundsBehavior: Flickable.StopAtBounds
                    ListWheelScroll {
                        id: wheel
                        objectName: "wheel"
                        view: flick
                        animate: false
                        consumeWheel: (dir) => gate.shouldConsumeWheel(dir)
                        onBoundaryReached: gate.noteBoundaryReached()
                        onOverscrolled: (dir, delta) => gate.overscroll(dir, delta)
                    }
                }
            }
        )", QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR) + u"/overscroll-test.qml"_s));

        std::unique_ptr<QQuickWindow> window(qobject_cast<QQuickWindow*>(component.create()));
        QVERIFY2(window, qPrintable(component.errorString()));
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));

        QQuickItem* flick = window->findChild<QQuickItem*>(u"flick"_s);
        QVERIFY(flick);
        QSignalSpy triggeredSpy(&gate, &EdgeScrollGate::triggered);

        const auto sendWheel = [&](int angleDelta, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            const QPointF pt(200, 160);
            QWheelEvent event(pt, window->mapToGlobal(pt), {}, QPoint(0, angleDelta),
                Qt::NoButton, mods, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window.get(), &event);
        };

        // Scroll all the way to the bottom in one burst: hits bottom without triggering.
        const qreal maxY = 600 - 320;
        for (int i = 0; i < 8; ++i) {
            now += 40;
            sendWheel(-120);
        }
        QCOMPARE(flick->property("contentY").toDouble(), maxY);
        QVERIFY(!gate.armed());
        QCOMPARE(triggeredSpy.count(), 0);

        // Ctrl+wheel at bottom must never arm overscroll.
        now += 400;
        sendWheel(-120, Qt::ControlModifier);
        QVERIFY(!gate.armed());

        // Plain notch 1 after settling arms; 3 deliberate notches over minArmMs trigger +1.
        sendWheel(-120);
        QVERIFY(gate.armed());
        QCOMPARE(gate.direction(), 1);
        now += 150;
        sendWheel(-120);
        QVERIFY(gate.armed());
        QCOMPARE(triggeredSpy.count(), 0);
        now += 150;
        sendWheel(-120);
        QCOMPARE(triggeredSpy.count(), 1);
        QCOMPARE(triggeredSpy.last().at(0).toInt(), 1);

        // Simulate landing at top of the next view; a trailing downward notch is consumed.
        flick->setProperty("contentY", 0);
        now += 50;
        sendWheel(-120);
        QCOMPARE(flick->property("contentY").toDouble(), 0.0);

        // Short (non-scrollable) page: after cooldown, 3 deliberate upward notches trigger -1.
        flick->setProperty("contentHeight", 200);
        now += 600;
        sendWheel(120);
        QVERIFY(gate.armed());
        QCOMPARE(gate.direction(), -1);
        now += 150;
        sendWheel(120);
        now += 150;
        sendWheel(120);
        QCOMPARE(triggeredSpy.count(), 2);
        QCOMPARE(triggeredSpy.last().at(0).toInt(), -1);
    }

    void keyboardBackForwardShortcuts()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            Window {
                id: win
                width: 400; height: 260; visible: true
                property int backs: 0
                property int forwards: 0
                Shortcut {
                    sequences: ["Alt+Left", "Back"]
                    onActivated: win.backs++
                }
                Shortcut {
                    sequences: ["Alt+Right", "Forward"]
                    onActivated: win.forwards++
                }
                TextInput {
                    id: field
                    objectName: "field"
                    width: 200; height: 32
                    text: "hello"
                    focus: true
                }
            }
        )", QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR) + u"/shortcut-test.qml"_s));

        std::unique_ptr<QQuickWindow> window(qobject_cast<QQuickWindow*>(component.create()));
        QVERIFY2(window, qPrintable(component.errorString()));
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window.get()));

        // Works while a TextInput has focus without hijacking Backspace.
        QQuickItem* field = window->findChild<QQuickItem*>(u"field"_s);
        QVERIFY(field);
        QTest::keyClick(window.get(), Qt::Key_Backspace);
        QCOMPARE(window->property("backs").toInt(), 0);
        QCOMPARE(field->property("text").toString(), u"hell"_s);

        QTest::keyClick(window.get(), Qt::Key_Left, Qt::AltModifier);
        QCOMPARE(window->property("backs").toInt(), 1);
        QTest::keyClick(window.get(), Qt::Key_Right, Qt::AltModifier);
        QCOMPARE(window->property("forwards").toInt(), 1);

        // Works after clearing focus (SettingsWindow.clearFocus pattern).
        field->setFocus(false);
        QTest::keyClick(window.get(), Qt::Key_Back);
        QCOMPARE(window->property("backs").toInt(), 2);
        QTest::keyClick(window.get(), Qt::Key_Forward);
        QCOMPARE(window->property("forwards").toInt(), 2);
    }
};

QTEST_MAIN(SettingsNavTest)
#include "tst_settingsnav.moc"

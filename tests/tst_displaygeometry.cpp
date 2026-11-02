#include "Placement.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <windows.h>

#include <cmath>

using namespace Qt::StringLiterals;

class DisplayGeometryTest : public QObject {
    Q_OBJECT
private slots:
    void centeringKeepsSizeAndSavesEachMode()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString statePath = temporary.filePath(u"state.ini"_s);
        for (QScreen* screen : QGuiApplication::screens()) {
            QWindow window;
            window.setFlags(Qt::FramelessWindowHint);
            window.setScreen(screen);
            window.resize(640, 300);
            window.create();
            ws::Placement placement(statePath, u"Launcher"_s);
            placement.setWindow(&window);
            placement.setRoomNeeded(240);
            placement.placeOn(screen);
            const QRect area = screen->availableGeometry();
            for (const bool panel : {false, true}) {
                if (panel) placement.enterPanel(screen);
                else placement.attach(QRect(area.topLeft() + QPoint(40, 20), QSize(200, 40)), screen);
                const QSize size = window.size();
                placement.centerOnScreen();
                const QPoint target(area.x() + (area.width() - size.width()) / 2,
                    area.y() + (area.height() - size.height()) / 2);
                QVERIFY((window.position() - target).manhattanLength() <= 2);
                QCOMPARE(window.size(), size);
                QVERIFY(!placement.atTaskbar());
                QVERIFY(!placement.above());
                const QPoint centered = window.position();
                if (panel) {
                    placement.leavePanel();
                    placement.enterPanel(screen);
                } else {
                    window.setPosition(area.topLeft());
                    placement.placeOn(screen);
                }
                QVERIFY((window.position() - centered).manhattanLength() <= 2);
                QSettings state(statePath, QSettings::IniFormat);
                // Native pixel alignment can move a half-pixel centre by one pixel.
                QVERIFY(std::abs(state.value(panel ? u"Panel/CenterX"_s : u"Launcher/CenterX"_s).toReal() - 0.5)
                    * area.width() <= 1);
                QVERIFY(state.value(panel ? u"Panel/Top"_s : u"Launcher/Top"_s).toReal() > 0);
            }
        }
    }

    void fileManagerAvoidanceIsTemporary()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        auto* screen = QGuiApplication::primaryScreen();
        const QRect area = screen->availableGeometry();
        if (area.width() < 1200 || area.height() < 500)
            QSKIP("This layout needs room for a 760-pixel launcher beside its own fixture");
        QWindow source;
        source.setFlags(Qt::FramelessWindowHint);
        source.setGeometry(area.x() + 4, area.y() + 4, area.width() / 3, area.height() - 8);
        source.create();
        QWindow window;
        window.resize(760, 400);
        window.create();
        const QString statePath = temporary.filePath(u"state.ini"_s);
        ws::Placement placement(statePath, u"Launcher"_s);
        placement.setRoomNeeded(240);
        placement.setRoomWanted(400);
        placement.rememberWidth(760, 520);
        placement.setWindow(&window);
        QCoreApplication::processEvents();
        placement.placeOn(screen);
        const QPoint home = window.position();
        RECT native {};
        QVERIFY(::GetWindowRect(reinterpret_cast<HWND>(source.winId()), &native));
        placement.avoidNative(QRect(native.left, native.top, native.right - native.left, native.bottom - native.top), screen);
        QCoreApplication::processEvents();
        QCOMPARE(window.width(), 760);
        QVERIFY(!window.geometry().intersects(source.geometry()));
        QVERIFY(area.contains(window.geometry()));
        placement.setRoomWanted(450);
        window.resize(760, 450);
        QCoreApplication::processEvents();
        QVERIFY(!window.geometry().intersects(source.geometry()));
        QVERIFY(area.contains(window.geometry()));
        QVERIFY(!QSettings(statePath, QSettings::IniFormat).contains(u"Launcher/CenterX"_s));
        placement.placeOn(screen);
        QCOMPARE(window.position(), home);
    }

    void userDropAfterDpiChangeKeepsItsNewPosition()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QWindow window;
        window.resize(760, 400);
        window.create();
        const QString statePath = temporary.filePath(u"state.ini"_s);
        ws::Placement placement(statePath, u"Launcher"_s);
        placement.setContentSize({760, 400});
        placement.rememberWidth(760, 520);
        placement.setWindow(&window);
        QCoreApplication::processEvents();
        const QPoint dropped = window.position() + QPoint(40, 40);
        const auto handle = reinterpret_cast<HWND>(window.winId());
        ::SendMessageW(handle, WM_ENTERSIZEMOVE, 0, 0);
        window.setPosition(dropped);
        QEvent changed(QEvent::DevicePixelRatioChange);
        QCoreApplication::sendEvent(&window, &changed);
        QCoreApplication::processEvents(); // the display change waits for the user's drag
        ::SendMessageW(handle, WM_EXITSIZEMOVE, 0, 0);
        QTest::qWait(260); // queued position saving and any glide must have finished
        QCOMPARE(window.position(), dropped);
        QCOMPARE(window.size(), QSize(760, 400));
        QVERIFY(QSettings(statePath, QSettings::IniFormat).contains(u"Launcher/CenterX"_s));
    }

    void nativeResizeNeverBecomesPreferredSize()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString statePath = temporary.filePath(u"state.ini"_s);
        QWindow window;
        window.resize(760, 400);
        window.create(); // hidden native window; the user's desktop stays as it was
        ws::Placement placement(statePath, u"Launcher"_s);
        placement.setContentSize({760, 400});
        placement.rememberWidth(760, 520);
        placement.setWindow(&window);
        QCoreApplication::processEvents();
        const auto reconnect = [&] {
            const qreal dpr = window.devicePixelRatio();
            ::SetWindowPos(reinterpret_cast<HWND>(window.winId()), nullptr, 0, 0,
                qRound(480 * dpr), qRound(280 * dpr), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            QEvent event(QEvent::DevicePixelRatioChange);
            QCoreApplication::sendEvent(&window, &event);
        };
        for (int i = 0; i < 8; ++i) {
            reconnect();
            QTRY_COMPARE(window.size(), QSize(760, 400));
            QCOMPARE(placement.boxWidth(), 760);
        }
        QVERIFY(!QSettings(statePath, QSettings::IniFormat).contains(u"Launcher/Width"_s));
        placement.enterPanel(window.screen());
        const QSize panelSize = window.size();
        for (int i = 0; i < 8; ++i) {
            reconnect();
            QTRY_COMPARE(window.size(), panelSize);
        }
        placement.leavePanel();
        placement.enterPanel(window.screen());
        QCOMPARE(window.size(), panelSize);
        QVERIFY(!QSettings(statePath, QSettings::IniFormat).contains(u"Panel/Width"_s));
    }
};

QTEST_MAIN(DisplayGeometryTest)
#include "tst_displaygeometry.moc"

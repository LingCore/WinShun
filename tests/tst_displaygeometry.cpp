#include "Placement.h"
#include "SavedWindowSize.h"
#include "WindowAvoidance.h"
#include "platform/WindowEffects.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QScopeGuard>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <windows.h>

#include <cmath>
#include <memory>

using namespace Qt::StringLiterals;

namespace {

// A window as ours (frameless: all of it client area), at the settings
// window's size; hidden, so the user's desktop stays as it was.
std::unique_ptr<QWindow> settingsLikeWindow()
{
    auto window = std::make_unique<QWindow>();
    window->setFlags(Qt::Window | Qt::FramelessWindowHint);
    window->resize(920, 720);
    window->setMinimumSize({760, 420});
    window->create();
    return window;
}

void nativeResize(QWindow& window, QSize logical)
{
    const qreal dpr = window.devicePixelRatio();
    ::SetWindowPos(reinterpret_cast<HWND>(window.winId()), nullptr, 0, 0, qRound(logical.width() * dpr),
        qRound(logical.height() * dpr), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// The system's sizing loop: an edge dragged sends WM_SIZING, a move does not.
void sizingLoop(QWindow& window, QSize to, bool edge)
{
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    ::SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    nativeResize(window, to);
    if (edge) {
        RECT rect {};
        ::GetWindowRect(hwnd, &rect);
        ::SendMessageW(hwnd, WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&rect));
    }
    ::SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
}

} // namespace

class DisplayGeometryTest : public QObject {
    Q_OBJECT
private slots:
    void windowMayCloseBeforeQueuedMoveIsSaved()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        auto window = std::make_unique<QWindow>();
        window->resize(640, 300);
        window->create();
        ws::Placement placement(temporary.filePath(u"closed.ini"_s), u"Launcher"_s);
        placement.setWindow(window.get());
        const HWND hwnd = reinterpret_cast<HWND>(window->winId());
        ::SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
        ::SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
        window.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    }

    void draggedPositionSurvivesPresetsAndRestart()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString statePath = temporary.filePath(u"dragged.ini"_s);
        auto* screen = QGuiApplication::primaryScreen();
        const QRect area = screen->availableGeometry();
        for (const bool panel : {false, true}) {
            QWindow window;
            window.resize(640, 300);
            window.create();
            ws::Placement placement(statePath, u"Launcher"_s);
            placement.setWindow(&window);
            placement.placeOn(screen);
            if (panel) placement.enterPanel(screen);
            const HWND hwnd = reinterpret_cast<HWND>(window.winId());
            ::SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
            window.setPosition(area.topLeft() + QPoint(120, 96));
            ::SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
            QTest::qWait(300);
            const QPoint dragged = window.position();
            const QString group = panel ? u"Panel"_s : u"Launcher"_s;
            QSettings state(statePath, QSettings::IniFormat);
            QVERIFY(state.contains(group + u"/DraggedCenterX"_s));
            placement.centerOnScreen();
            if (!panel) placement.moveHome();
            QTest::qWait(300);
            placement.restoreDraggedPosition();
            QTest::qWait(300);
            QVERIFY((window.position() - dragged).manhattanLength() <= 2);

            QWindow reopened;
            reopened.resize(window.size());
            reopened.create();
            ws::Placement restored(statePath, u"Launcher"_s);
            restored.setWindow(&reopened);
            restored.placeOn(screen);
            if (panel) restored.enterPanel(screen);
            restored.centerOnScreen();
            restored.restoreDraggedPosition();
            QTest::qWait(300);
            QVERIFY((reopened.position() - dragged).manhattanLength() <= 2);
        }
    }

    void homeDetachesTemporaryPlacement()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        auto* screen = QGuiApplication::primaryScreen();
        const QRect area = screen->availableGeometry();
        QWindow window;
        window.setFlags(Qt::FramelessWindowHint);
        window.resize(640, 300);
        window.create();
        ws::Placement placement(temporary.filePath(u"home.ini"_s), u"Launcher"_s);
        placement.setWindow(&window);
        placement.setRoomNeeded(300);
        placement.placeOn(screen);
        const QPoint home = window.position();
        const QSize size = window.size();
        placement.attach(QRect(area.topLeft() + QPoint(20, 30), QSize(200, 40)), screen);
        QVERIFY(window.position() != home);
        placement.moveHome();
        QTest::qWait(350);
        QVERIFY((window.position() - home).manhattanLength() <= 2);
        QCOMPARE(window.size(), size);
        QVERIFY(!placement.above());
        QWindow source;
        source.setGeometry(area.x() + 20, area.y() + 30, 400, 500);
        source.create();
        RECT native {};
        QVERIFY(::GetWindowRect(reinterpret_cast<HWND>(source.winId()), &native));
        placement.avoidNative(QRect(native.left, native.top, native.right - native.left, native.bottom - native.top), screen);
        QVERIFY(placement.avoiding());
        placement.moveHome();
        QTest::qWait(350);
        QVERIFY(!placement.avoiding());
        QVERIFY((window.position() - home).manhattanLength() <= 2);
    }

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

    // Typed in File Explorer (GitHub #203): a window the launcher cannot keep
    // clear of puts it in the corner of its file list, within the window, and
    // more rows wanted later keep it there; one it can keep clear of, beside
    // it as before (GitHub #141), its list given or not.
    void explorerTooLargeToKeepClearOf()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        auto* screen = QGuiApplication::primaryScreen();
        const QRect area = screen->availableGeometry();
        if (area.width() < 1400 || area.height() < 700)
            QSKIP("This layout needs room for a 760-pixel launcher in its own fixture's file list");
        const auto fixture = [](const QRect& at) {
            auto window = std::make_unique<QWindow>();
            window->setFlags(Qt::FramelessWindowHint);
            window->setGeometry(at);
            window->create(); // hidden: the user's desktop stays as it was
            return window;
        };
        const auto native = [](const QWindow& window) {
            RECT r {};
            ::GetWindowRect(reinterpret_cast<HWND>(window.winId()), &r);
            return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
        };
        // Centred, three quarters of the screen; its list right of the
        // navigation pane, above the status bar.
        const auto centred = fixture({area.x() + area.width() / 8, area.y() + area.height() / 8, area.width() * 3 / 4,
            area.height() * 3 / 4});
        const QRect source = centred->geometry();
        const auto list = fixture({source.x() + source.width() / 5, source.y() + 80, source.width() - source.width() / 5,
            source.height() - 110});
        QWindow window;
        window.resize(760, 400);
        window.create();
        ws::Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
        placement.setRoomNeeded(240);
        placement.setRoomWanted(400);
        placement.rememberWidth(760, 520);
        // As Main.qml sizes the window by Placement.boxWidth.
        QObject::connect(&placement, &ws::Placement::boxWidthChanged, &window, [&] {
            if (placement.boxWidth() > 0) window.resize(placement.boxWidth(), window.height());
        });
        placement.setWindow(&window);
        QCoreApplication::processEvents();
        placement.avoidNative(native(*centred), screen, false, {}, native(*centred), native(*list));
        QCoreApplication::processEvents();
        const QRect in = list->geometry();
        // File Explorer's list (`orInto`): right in its corner since GitHub #234.
        constexpr int gap = ws::windowavoidance::kExplorerListGap;
        const auto inCorner = [&] {
            const QRect box = window.geometry();
            return source.contains(box) && std::abs(in.right() - gap - box.right()) <= 2 && box.bottom() <= in.bottom() - gap + 2
                && box.bottom() >= in.bottom() - gap - 2;
        };
        // Over a file list it opens compact (GitHub #82), in the list's corner.
        QCOMPARE(window.width(), 460);
        QVERIFY2(inCorner(), qPrintable(u"box %1,%2 %3x%4"_s.arg(window.x()).arg(window.y()).arg(window.width()).arg(window.height())));
        placement.setRoomWanted(450);
        window.resize(window.width(), 450);
        QCoreApplication::processEvents();
        QVERIFY(inCorner());
        // The left third: beside it.
        const auto leftThird = fixture({area.x() + 4, area.y() + 4, area.width() / 3, area.height() - 8});
        const auto leftList = fixture({area.x() + 100, area.y() + 100, area.width() / 3 - 100, area.height() - 140});
        placement.setRoomWanted(400);
        window.resize(window.width(), 400);
        placement.avoidNative(native(*leftThird), screen, false, {}, native(*leftThird), native(*leftList));
        QCoreApplication::processEvents();
        QVERIFY(!window.geometry().intersects(leftThird->geometry()));
        QVERIFY(area.contains(window.geometry()));
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

    // GitHub #401: the settings window's size, between runs.
    void savedSizeFitsTheWorkArea()
    {
        using ws::SavedWindowSize;
        const QSize least(760, 420);
        // 2048 × 1152 at 125 %: as wide as wanted, as tall as fits.
        QCOMPARE(SavedWindowSize::fit({1400, 1000}, least, {1638, 870}), QSize(1400, 830));
        QCOMPARE(SavedWindowSize::fit({1400, 1000}, least, {2560, 1392}), QSize(1400, 1000));
        QCOMPARE(SavedWindowSize::fit({920, 720}, least, {800, 560}), QSize(760, 520));
        QCOMPARE(SavedWindowSize::fit({500, 300}, least, {2560, 1392}), least); // saved under the least
        QCOMPARE(SavedWindowSize::fit({920, 720}, least, {700, 400}), QSize(700, 400)); // a screen smaller than that
    }

    void draggedSizeOutlivesARestart()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString statePath = temporary.filePath(u"state.ini"_s);
        const auto saved = [&] {
            const QSettings state(statePath, QSettings::IniFormat);
            return state.contains(u"Settings/Width"_s)
                ? QSize(state.value(u"Settings/Width"_s).toInt(), state.value(u"Settings/Height"_s).toInt()) : QSize();
        };
        {
            const auto window = settingsLikeWindow();
            ws::SavedWindowSize size(statePath, u"Settings"_s);
            size.setWindow(window.get());
            QCOMPARE(size.wanted(), QSize(920, 720)); // nothing saved: as made
            sizingLoop(*window, {1100, 900}, false); // snapped at the end of a move: not its user's size
            QCOMPARE(saved(), QSize());
            sizingLoop(*window, {1400, 1000}, true);
            QCOMPARE(saved(), QSize(1400, 1000));
            QCOMPARE(size.wanted(), QSize(1400, 1000));
            // Opened cut short by a smaller screen, then only widened: the height wished stays.
            nativeResize(*window, {1400, 830});
            sizingLoop(*window, {1500, 830}, true);
            QCOMPARE(saved(), QSize(1500, 1000));
        }
        {
            const auto window = settingsLikeWindow();
            ws::SavedWindowSize size(statePath, u"Settings"_s);
            size.setWindow(window.get());
            QCOMPARE(size.wanted(), QSize(1500, 1000)); // the next run
            QScreen* screen = QGuiApplication::primaryScreen();
            size.placeOn(screen);
            const QSize expected = ws::SavedWindowSize::fit({1500, 1000}, {760, 420}, screen->availableGeometry().size());
            QTRY_COMPARE(window->size(), expected);
            QVERIFY(screen->availableGeometry().contains(window->geometry()));
            // A double click on an edge: as made again, and nothing saved.
            ::SendMessageW(reinterpret_cast<HWND>(window->winId()), WM_NCLBUTTONDBLCLK, HTRIGHT, 0);
            QCOMPARE(size.wanted(), QSize(920, 720));
            QCOMPARE(saved(), QSize());
            QTRY_COMPARE(window->size(), ws::SavedWindowSize::fit({920, 720}, {760, 420}, screen->availableGeometry().size()));
        }
        // Junk in the file: as made.
        QSettings(statePath, QSettings::IniFormat).setValue(u"Settings/Width"_s, u"wide"_s);
        QSettings(statePath, QSettings::IniFormat).setValue(u"Settings/Height"_s, 99999);
        const auto window = settingsLikeWindow();
        ws::SavedWindowSize size(statePath, u"Settings"_s);
        size.setWindow(window.get());
        QCOMPARE(size.wanted(), QSize(920, 720));
    }

    void maximizedComesBackMaximized()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString statePath = temporary.filePath(u"state.ini"_s);
        const auto maximized = [&] { return QSettings(statePath, QSettings::IniFormat).value(u"Settings/Maximized"_s).toBool(); };
        // Shown as Windows shows them, cloaked: nothing comes up on the screen.
        const auto made = [] {
            auto window = settingsLikeWindow();
            ws::win::setCloaked(window.get(), true);
            return window;
        };
        QScreen* screen = QGuiApplication::primaryScreen();
        {
            const auto window = made();
            ws::SavedWindowSize size(statePath, u"Settings"_s);
            size.setWindow(window.get());
            size.placeOn(screen);
            window->show();
            QVERIFY(!zoomed(*window)); // nothing saved: as placed
            window->setWindowStates(Qt::WindowMaximized); // its title bar's button
            QTRY_VERIFY(maximized());
            window->setWindowStates(Qt::WindowMinimized | Qt::WindowMaximized); // minimized from there: still so
            QTRY_VERIFY(::IsIconic(reinterpret_cast<HWND>(window->winId())));
            QVERIFY(maximized());
            window->hide();
            QVERIFY(maximized());
        }
        {
            const auto window = made();
            ws::SavedWindowSize size(statePath, u"Settings"_s);
            size.setWindow(window.get());
            QVERIFY(size.maximizedWanted());
            size.placeOn(screen);
            // Resized while still hidden (its frame restyled, say): Qt calls
            // that restored, and the window opened at its normal size (#401).
            const QSize physical = window->size() * window->devicePixelRatio();
            ::SendMessageW(reinterpret_cast<HWND>(window->winId()), WM_SIZE, SIZE_RESTORED,
                MAKELPARAM(physical.width(), physical.height()));
            QCoreApplication::processEvents();
            QVERIFY(maximized());
            window->show();
            QVERIFY(zoomed(*window));
            QTRY_VERIFY(window->windowStates() & Qt::WindowMaximized);
            QVERIFY(maximized());
            // Restored: the size it was placed at, and maximized no more.
            window->setWindowStates(Qt::WindowNoState);
            QTRY_VERIFY(!zoomed(*window));
            QTRY_COMPARE(window->size(), ws::SavedWindowSize::fit({920, 720}, {760, 420}, screen->availableGeometry().size()));
            QTRY_VERIFY(!QSettings(statePath, QSettings::IniFormat).contains(u"Settings/Maximized"_s));
            window->hide();
        }
    }

    // 外观 › 记住窗口大小 off: nothing read or saved, what was saved is
    // forgotten, and what its user gives the window lasts while it runs. On
    // again: that is saved at once.
    void rememberingCanBeTurnedOff()
    {
        using ws::SavedWindowSize;
        const auto restore = qScopeGuard([] { SavedWindowSize::setRemembering(true); });
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString statePath = temporary.filePath(u"state.ini"_s);
        const auto keys = [&] { return QSettings(statePath, QSettings::IniFormat).allKeys(); };
        const QStringList others {u"Other/Kept"_s}; // not its own: left alone
        {
            QSettings state(statePath, QSettings::IniFormat);
            state.setValue(u"Settings/Width"_s, 1400);
            state.setValue(u"Settings/Height"_s, 1000);
            state.setValue(u"Settings/Maximized"_s, true);
            state.setValue(others.first(), 1);
        }
        SavedWindowSize::setRemembering(false);
        QVERIFY(!SavedWindowSize::remembering());
        const auto window = settingsLikeWindow();
        ws::win::setCloaked(window.get(), true);
        SavedWindowSize size(statePath, u"Settings"_s);
        size.setWindow(window.get());
        QCOMPARE(size.wanted(), QSize(920, 720)); // as made, each run
        QVERIFY(!size.maximizedWanted());
        QCOMPARE(keys(), others); // left from when it was on: gone
        size.placeOn(QGuiApplication::primaryScreen());
        window->show();
        QVERIFY(!zoomed(*window));
        sizingLoop(*window, {1000, 800}, true);
        QCOMPARE(size.wanted(), QSize(1000, 800)); // for this run
        window->setWindowStates(Qt::WindowMaximized);
        QTRY_VERIFY(size.maximizedWanted());
        QCOMPARE(keys(), others); // nothing saved
        SavedWindowSize::setRemembering(true);
        {
            const QSettings state(statePath, QSettings::IniFormat);
            QCOMPARE(state.value(u"Settings/Width"_s).toInt(), 1000);
            QCOMPARE(state.value(u"Settings/Height"_s).toInt(), 800);
            QVERIFY(state.value(u"Settings/Maximized"_s).toBool());
        }
        // Off while it is up: forgotten, and it keeps what it has for this run.
        SavedWindowSize::setRemembering(false);
        QCOMPARE(keys(), others);
        window->setWindowStates(Qt::WindowNoState);
        QTRY_VERIFY(!zoomed(*window));
        QTRY_COMPARE(window->size(), QSize(1000, 800));
        QCOMPARE(size.wanted(), QSize(1000, 800));
        QCOMPARE(keys(), others);
        window->hide();
    }

private:
    static bool zoomed(QWindow& window) { return ::IsZoomed(reinterpret_cast<HWND>(window.winId())) != FALSE; }
};

QTEST_MAIN(DisplayGeometryTest)
#include "tst_displaygeometry.moc"

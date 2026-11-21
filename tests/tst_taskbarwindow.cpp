// The launcher pinned as a window of its own (GitHub #418): its styles to
// the shell and back, on real windows.
#include "platform/WindowEffects.h"

#include <QGuiApplication>
#include <QTest>
#include <QWindow>

#include <windows.h>
#include <dwmapi.h>

namespace {

struct Styles {
    LONG_PTR style = 0;
    LONG_PTR exStyle = 0;
};

Styles stylesOf(const QWindow& window)
{
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    return {::GetWindowLongPtrW(hwnd, GWL_STYLE), ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE)};
}

bool shellWindow(const Styles& s)
{
    return (s.exStyle & WS_EX_APPWINDOW) && !(s.exStyle & WS_EX_TOOLWINDOW) && (s.style & WS_MINIMIZEBOX)
        && (s.style & WS_SYSMENU);
}

} // namespace

class TaskbarWindowTest : public QObject {
    Q_OBJECT

private slots:
    // As the launcher's: a tool window, topmost, no frame of Qt's.
    void toolWindowAndBack()
    {
        QWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        window.setTitle(QStringLiteral("WinShun taskbar window test"));
        window.setGeometry(200, 200, 320, 160);
        window.create();
        const HWND hwnd = reinterpret_cast<HWND>(window.winId());
        const Styles made = stylesOf(window);
        QVERIFY(made.exStyle & WS_EX_TOOLWINDOW);
        QVERIFY(!(made.style & (WS_MINIMIZEBOX | WS_SYSMENU)));

        ws::win::setTaskbarWindow(&window, true); // before it is shown, as the launcher's next opening
        QVERIFY(shellWindow(stylesOf(window)));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY2(shellWindow(stylesOf(window)), "Qt's show set its own styles again");

        // Minimized as the taskbar's button and Win+D do it, and back.
        ::ShowWindow(hwnd, SW_MINIMIZE);
        QVERIFY(::IsIconic(hwnd));
        QTRY_VERIFY(window.windowStates() & Qt::WindowMinimized);
        ws::win::bringToFront(&window);
        QVERIFY(!::IsIconic(hwnd));
        QVERIFY(::IsWindowVisible(hwnd));
        QTRY_VERIFY(!(window.windowStates() & Qt::WindowMinimized));

        window.hide();
        window.show(); // hidden and shown again: still the shell's
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(shellWindow(stylesOf(window)));

        ws::win::setTaskbarWindow(&window, false); // unpinned while on the screen
        const Styles back = stylesOf(window);
        QCOMPARE(back.exStyle, made.exStyle);
        QCOMPARE(back.style & ~WS_VISIBLE, made.style & ~WS_VISIBLE);
        ws::win::setTaskbarWindow(&window, false); // nothing to change
        QCOMPARE(stylesOf(window).exStyle, made.exStyle);
        window.hide();
    }

    // Standalone plugin windows (GesturesWindow, JsonWindow, OcrSearchWindow,
    // GitHub #419): styleFramelessWindow gives them the native DWM border,
    // shadow and corner style that WindowEdge relies on under Windows 11.
    void framelessStandaloneWindowGetsNativeFrame()
    {
        QWindow window;
        window.setFlags(Qt::Window | Qt::FramelessWindowHint);
        window.setTitle(QStringLiteral("WinShun standalone frame test"));
        window.setGeometry(220, 220, 480, 320);
        window.create();
        const HWND hwnd = reinterpret_cast<HWND>(window.winId());
        QVERIFY(::GetPropW(hwnd, L"WinShun.CornerStyle") == nullptr);

        ws::win::styleFramelessWindow(&window, false);
        QVERIFY(stylesOf(window).style & WS_CAPTION);
        QVERIFY(::GetPropW(hwnd, L"WinShun.CornerStyle") != nullptr);
        if (ws::win::nativeWindowCorners()) {
            int corner = 0;
            QCOMPARE(::DwmGetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner), S_OK);
            QCOMPARE(corner, 2 /* DWMWCP_ROUND */);
        }
        ws::win::setDarkFrame(&window, true);
        QVERIFY(ws::win::isDarkFrame(&window));
        ws::win::setDarkFrame(&window, false);
        QVERIFY(!ws::win::isDarkFrame(&window));
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    TaskbarWindowTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_taskbarwindow.moc"

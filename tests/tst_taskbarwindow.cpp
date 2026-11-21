// The launcher pinned as a window of its own (GitHub #418): its styles to
// the shell and back, on real windows.
#include "platform/WindowEffects.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QTest>
#include <QWindow>

#include <windows.h>
#include <dwmapi.h>

#include <vector>

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

HICON windowIconOf(HWND hwnd, WPARAM slot)
{
    return reinterpret_cast<HICON>(::SendMessageW(hwnd, WM_GETICON, slot, 0));
}

QSize iconBitmapSize(HICON icon)
{
    if (!icon)
        return {};
    ICONINFO info {};
    if (!::GetIconInfo(icon, &info))
        return {};
    BITMAP bm {};
    const bool ok = info.hbmColor && ::GetObjectW(info.hbmColor, sizeof bm, &bm) == sizeof bm;
    if (info.hbmColor)
        ::DeleteObject(info.hbmColor);
    if (info.hbmMask)
        ::DeleteObject(info.hbmMask);
    return ok ? QSize(bm.bmWidth, bm.bmHeight) : QSize();
}

bool iconHasVisiblePixels(HICON icon)
{
    if (!icon)
        return false;
    ICONINFO info {};
    if (!::GetIconInfo(icon, &info))
        return false;
    bool visible = false;
    BITMAP bm {};
    if (info.hbmColor && ::GetObjectW(info.hbmColor, sizeof bm, &bm) == sizeof bm && bm.bmWidth > 0 && bm.bmHeight > 0) {
        BITMAPINFO bi {};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB};
        std::vector<std::uint32_t> pixels(static_cast<size_t>(bm.bmWidth) * static_cast<size_t>(bm.bmHeight));
        const HDC dc = ::GetDC(nullptr);
        if (::GetDIBits(dc, info.hbmColor, 0, bm.bmHeight, pixels.data(), &bi, DIB_RGB_COLORS)) {
            for (const std::uint32_t px : pixels) {
                if (px != 0) {
                    visible = true;
                    break;
                }
            }
        }
        ::ReleaseDC(nullptr, dc);
    }
    if (info.hbmColor)
        ::DeleteObject(info.hbmColor);
    if (info.hbmMask)
        ::DeleteObject(info.hbmMask);
    return visible;
}

void verifyDpiMatchedAppIcon(HWND hwnd)
{
    const UINT dpi = ::GetDpiForWindow(hwnd) ? ::GetDpiForWindow(hwnd) : ::GetDpiForSystem();
    const QSize expectedBig(::GetSystemMetricsForDpi(SM_CXICON, dpi), ::GetSystemMetricsForDpi(SM_CYICON, dpi));
    const QSize expectedSmall(::GetSystemMetricsForDpi(SM_CXSMICON, dpi), ::GetSystemMetricsForDpi(SM_CYSMICON, dpi));

    const HICON bigIcon = windowIconOf(hwnd, ICON_BIG);
    const HICON smallIcon = windowIconOf(hwnd, ICON_SMALL);
    const HICON small2Icon = windowIconOf(hwnd, ICON_SMALL2);
    QVERIFY2(bigIcon != nullptr, "ICON_BIG was not set on HWND");
    QVERIFY2(smallIcon != nullptr, "ICON_SMALL was not set on HWND");
    QCOMPARE(small2Icon, smallIcon);
    QCOMPARE(iconBitmapSize(bigIcon), expectedBig);
    QCOMPARE(iconBitmapSize(smallIcon), expectedSmall);
    QVERIFY(iconHasVisiblePixels(bigIcon));
    QVERIFY(iconHasVisiblePixels(smallIcon));
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
        // Reproduce the root cause: Qt's tool-window class carries no icon.
        QCOMPARE(windowIconOf(hwnd, ICON_BIG), HICON(nullptr));
        QCOMPARE(windowIconOf(hwnd, ICON_SMALL), HICON(nullptr));
        QCOMPARE(::GetClassLongPtrW(hwnd, GCLP_HICON), LONG_PTR(0));
        QCOMPARE(::GetClassLongPtrW(hwnd, GCLP_HICONSM), LONG_PTR(0));

        ws::win::setTaskbarWindow(&window, true); // before it is shown, as the launcher's next opening
        QVERIFY(shellWindow(stylesOf(window)));
        verifyDpiMatchedAppIcon(hwnd);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY2(shellWindow(stylesOf(window)), "Qt's show set its own styles again");
        verifyDpiMatchedAppIcon(hwnd);

        // Minimized as the taskbar's button and Win+D do it, and back.
        ::ShowWindow(hwnd, SW_MINIMIZE);
        QVERIFY(::IsIconic(hwnd));
        QTRY_VERIFY(window.windowStates() & Qt::WindowMinimized);
        ws::win::bringToFront(&window);
        QVERIFY(!::IsIconic(hwnd));
        QVERIFY(::IsWindowVisible(hwnd));
        QTRY_VERIFY(!(window.windowStates() & Qt::WindowMinimized));
        verifyDpiMatchedAppIcon(hwnd);

        window.hide();
        window.show(); // hidden and shown again: still the shell's
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(shellWindow(stylesOf(window)));
        verifyDpiMatchedAppIcon(hwnd);

        // Native HWND recreated while pinned: both taskbar styles and app icon return.
        window.destroy();
        window.create();
        const HWND recreated = reinterpret_cast<HWND>(window.winId());
        QVERIFY(recreated != nullptr);
        QVERIFY(shellWindow(stylesOf(window)));
        verifyDpiMatchedAppIcon(recreated);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(shellWindow(stylesOf(window)));
        verifyDpiMatchedAppIcon(recreated);

        ws::win::setTaskbarWindow(&window, false); // unpinned while on the screen
        const Styles back = stylesOf(window);
        QCOMPARE(back.exStyle, made.exStyle);
        QCOMPARE(back.style & ~WS_VISIBLE, made.style & ~WS_VISIBLE);
        ws::win::setTaskbarWindow(&window, false); // nothing to change
        QCOMPARE(stylesOf(window).exStyle, made.exStyle);
        window.hide();

        // Native HWND recreated after unpinning: stays a tool window.
        window.destroy();
        window.create();
        QVERIFY(stylesOf(window).exStyle & WS_EX_TOOLWINDOW);
        QVERIFY(!(stylesOf(window).exStyle & WS_EX_APPWINDOW));
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
        verifyDpiMatchedAppIcon(hwnd);
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

    // Reopening a standalone plugin window after QWindow::close() (or any
    // Qt handle recreation) destroys the old HWND and creates a new one.
    // styleFramelessWindow, shadow mode, backdrop and dark frame must all
    // re-apply automatically to the new HWND.
    void framelessWindowRestylesAfterNativeHandleRecreated()
    {
        QWindow window;
        window.setFlags(Qt::Window | Qt::FramelessWindowHint);
        window.setTitle(QStringLiteral("WinShun handle recreate test"));
        window.setGeometry(240, 240, 480, 320);
        window.create();
        ws::win::styleFramelessWindow(&window, false);
        ws::win::setDarkFrame(&window, true);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const HWND first = reinterpret_cast<HWND>(window.winId());

        // Close destroys the native HWND (as OcrSearchWindow / JsonWindow do).
        window.close();
        QVERIFY(!window.handle());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const HWND second = reinterpret_cast<HWND>(window.winId());
        QVERIFY(second != nullptr);
        QVERIFY(stylesOf(window).style & WS_CAPTION);
        QVERIFY(::GetPropW(second, L"WinShun.CornerStyle") != nullptr);
        verifyDpiMatchedAppIcon(second);
        if (ws::win::nativeWindowCorners()) {
            int corner = 0;
            QCOMPARE(::DwmGetWindowAttribute(second, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner), S_OK);
            QCOMPARE(corner, 2 /* DWMWCP_ROUND */);
        }
        if (ws::win::backdropSupported()) {
            int backdrop = 0;
            QCOMPARE(::DwmGetWindowAttribute(second, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof backdrop), S_OK);
            QCOMPARE(backdrop, 1 /* DWMSBT_NONE */);
        }
        QVERIFY(ws::win::isDarkFrame(&window));
        window.hide();

        // Explicit destroy + create must also preserve shadow and backdrop settings.
        ws::win::setWindowShadow(&window, ws::win::WindowShadow::None);
        window.destroy();
        window.create();
        const HWND third = reinterpret_cast<HWND>(window.winId());
        QVERIFY(third != nullptr);
        QVERIFY(!(stylesOf(window).style & WS_CAPTION));
        QVERIFY(::GetPropW(third, L"WinShun.Shadow") != nullptr);
        if (ws::win::nativeWindowCorners()) {
            int corner = 0;
            QCOMPARE(::DwmGetWindowAttribute(third, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner), S_OK);
            QCOMPARE(corner, 1 /* DWMWCP_DONOTROUND */);
        }

        ws::win::setWindowShadow(&window, ws::win::WindowShadow::Standard);
        if (ws::win::backdropSupported()) {
            ws::win::styleFramelessWindow(&window, true);
            window.destroy();
            window.create();
            const HWND fourth = reinterpret_cast<HWND>(window.winId());
            QVERIFY(::GetPropW(fourth, L"WinShun.Backdrop") != nullptr);
            int backdrop = 0;
            QCOMPARE(::DwmGetWindowAttribute(fourth, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof backdrop), S_OK);
            QCOMPARE(backdrop, 2 /* DWMSBT_MAINWINDOW */);
        }
        Q_UNUSED(first);
    }

    // The common live flow: the launcher is already visible as a tool window
    // and the user clicks the pin button (or presses Ctrl+T).
    void pinnedWhileVisibleGetsAppIcon()
    {
        QWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        window.setTitle(QStringLiteral("WinShun live pin icon test"));
        window.setGeometry(260, 260, 320, 160);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const HWND hwnd = reinterpret_cast<HWND>(window.winId());
        QCOMPARE(windowIconOf(hwnd, ICON_BIG), HICON(nullptr));
        QCOMPARE(windowIconOf(hwnd, ICON_SMALL), HICON(nullptr));

        ws::win::setTaskbarWindow(&window, true);
        QVERIFY(shellWindow(stylesOf(window)));
        verifyDpiMatchedAppIcon(hwnd);

        ws::win::setTaskbarWindow(&window, false);
        QVERIFY(stylesOf(window).exStyle & WS_EX_TOOLWINDOW);
        window.hide();
    }

    // Matches the real launcher lifecycle: styleFramelessWindow first, optional
    // Light/None search shadow (which clears WS_CAPTION), then pinned while visible.
    void framelessToolWindowWithAllShadowsGetsAppIcon()
    {
        QWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        window.setTitle(QStringLiteral("WinShun frameless pin icon test"));
        window.setGeometry(260, 260, 320, 160);
        window.create();
        const HWND hwnd = reinterpret_cast<HWND>(window.winId());

        ws::win::styleFramelessWindow(&window, false);
        verifyDpiMatchedAppIcon(hwnd);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        for (const auto shadow : {ws::win::WindowShadow::Standard, ws::win::WindowShadow::Light, ws::win::WindowShadow::None}) {
            ws::win::setWindowShadow(&window, shadow);
            ws::win::setTaskbarWindow(&window, true);
            QVERIFY(shellWindow(stylesOf(window)));
            verifyDpiMatchedAppIcon(hwnd);
            ws::win::setTaskbarWindow(&window, false);
            QVERIFY(stylesOf(window).exStyle & WS_EX_TOOLWINDOW);
        }
        window.hide();
    }

    // Explicit QWindow::setIcon() is respected, DPI-change events refresh safely,
    // and repeated calls reuse cached HICONs without leaking USER objects.
    void explicitWindowIconPreservedAndNoHandleLeak()
    {
        QWindow custom;
        custom.setFlags(Qt::Window | Qt::FramelessWindowHint);
        custom.setTitle(QStringLiteral("WinShun explicit icon test"));
        custom.setGeometry(280, 280, 320, 160);
        QImage img(32, 32, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::red);
        custom.setIcon(QIcon(QPixmap::fromImage(img)));
        custom.create();
        const HWND customHwnd = reinterpret_cast<HWND>(custom.winId());
        const HICON beforeBig = windowIconOf(customHwnd, ICON_BIG);
        const HICON beforeSmall = windowIconOf(customHwnd, ICON_SMALL);

        ws::win::setAppIcon(&custom);
        ws::win::styleFramelessWindow(&custom, false);
        ws::win::setTaskbarWindow(&custom, true);
        QCOMPARE(windowIconOf(customHwnd, ICON_BIG), beforeBig);
        QCOMPARE(windowIconOf(customHwnd, ICON_SMALL), beforeSmall);

        // Cached HICON reuse across repeated calls and DPI-change notifications.
        QWindow appWindow;
        appWindow.setFlags(Qt::Tool | Qt::FramelessWindowHint);
        appWindow.setGeometry(300, 300, 320, 160);
        appWindow.create();
        ws::win::setAppIcon(&appWindow);
        const HWND appHwnd = reinterpret_cast<HWND>(appWindow.winId());
        verifyDpiMatchedAppIcon(appHwnd);

        const DWORD userObjectsBefore = ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS);
        for (int i = 0; i < 50; ++i) {
            ws::win::setAppIcon(&appWindow);
            QEvent dprChanged(QEvent::DevicePixelRatioChange);
            QCoreApplication::sendEvent(&appWindow, &dprChanged);
        }
        const DWORD userObjectsAfter = ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS);
        QCOMPARE(userObjectsAfter, userObjectsBefore);
        verifyDpiMatchedAppIcon(appHwnd);
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    TaskbarWindowTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_taskbarwindow.moc"

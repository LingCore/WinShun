#include "SearchShadowProbe.h"

#include "App.h"
#include "Launcher.h"
#include "Placement.h"
#include "SystemTheme.h"
#include "platform/WindowEffects.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>

#include <cmath>
#include <cstdio>
#include <functional>

#include <dwmapi.h>
#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws::preview {

// --probe-runtime <folder> --search-shadow-check
// The production App beside the Win顺 that runs; its search box opened as its
// hotkey opens it, once for each 搜索框阴影 choice (standard, light, none, and
// standard again), in front of a plain magenta window of our own, so that what
// changes around it is its shadow alone. Measured on the screen: how much
// darker the magenta is in a band below the box and one to its right (0–255),
// whether its top-left corner pixel is the magenta (a rounded corner) or the
// box's own, and whether the window has a caption and WindowEdge.qml draws
// the edge. Standard: the full shadow, rounded, DWM's edge. Light: a shadow,
// smaller than standard's, still rounded, DWM's edge. None: no shadow, a
// square corner, WindowEdge's edge. Each time also grown into the panel,
// which keeps the standard look whatever is set (its sizing frame has DWM's
// full shadow: App::applySearchShadow). Into search-shadow.json, with a picture
// of each (the window and the magenta only). Windows 11 with round corners: on
// Windows 10 or the 直角 setting, light has no shadow either (WindowEffects.h).
// Nothing is typed and no input is sent; each opening waits until the
// keyboard and mouse were left alone for three seconds, as the box takes the
// keys while it is up.
struct SearchShadowProbe {
    static constexpr COLORREF kBacking = RGB(200, 40, 200);
    static constexpr int kPad = 80; // what is captured around the box

    static void pump(int ms)
    {
        QElapsedTimer timer;
        timer.start();
        do
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        while (timer.elapsed() < ms);
    }

    static void waitForQuietInput()
    {
        QElapsedTimer waited;
        waited.start();
        for (;;) {
            LASTINPUTINFO last {};
            last.cbSize = sizeof last;
            if (!::GetLastInputInfo(&last) || ::GetTickCount() - last.dwTime >= 3000 || waited.elapsed() > 120'000)
                return;
            pump(200);
        }
    }

    static QImage screenArea(const QRect& area)
    {
        const HDC screen = ::GetDC(nullptr);
        const HDC memory = ::CreateCompatibleDC(screen);
        const HBITMAP bitmap = ::CreateCompatibleBitmap(screen, area.width(), area.height());
        const HGDIOBJ old = ::SelectObject(memory, bitmap);
        ::BitBlt(memory, 0, 0, area.width(), area.height(), screen, area.x(), area.y(), SRCCOPY | CAPTUREBLT);
        ::SelectObject(memory, old);
        QImage image = QImage::fromHBITMAP(bitmap).convertToFormat(QImage::Format_RGB32);
        ::DeleteObject(bitmap);
        ::DeleteDC(memory);
        ::ReleaseDC(nullptr, screen);
        return image;
    }

    // How much darker than the backing, over its red and blue (its green is low).
    static double darkening(const QImage& image, const QRect& band)
    {
        double sum = 0;
        int n = 0;
        for (int y = band.top(); y <= band.bottom(); ++y)
            for (int x = band.left(); x <= band.right(); ++x) {
                const QRgb p = image.pixel(x, y);
                sum += (GetRValue(kBacking) - qRed(p) + GetBValue(kBacking) - qBlue(p)) / 2.0;
                ++n;
            }
        return n ? std::round(sum / n * 10) / 10 : -1;
    }

    static bool magenta(QRgb p) { return qRed(p) - qGreen(p) > 60 && qBlue(p) - qGreen(p) > 60; }

    static QString hex(QRgb p) { return QColor(p).name(); }

    static HWND makeBacking()
    {
        static const ATOM atom = [] {
            WNDCLASSW type {};
            type.lpfnWndProc = ::DefWindowProcW;
            type.hInstance = ::GetModuleHandleW(nullptr);
            type.hbrBackground = ::CreateSolidBrush(kBacking);
            type.lpszClassName = L"WinShun.ShadowBacking";
            return ::RegisterClassW(&type);
        }();
        return atom ? ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WinShun.ShadowBacking", L"", WS_POPUP,
                          0, 0, 10, 10, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr)
                    : nullptr;
    }

    // The window as it is now, in front of the backing: its shadow, corner and edge.
    static QJsonObject measure(QQuickWindow* window, const QString& shadow, const QString& where, HWND backing,
        const QDir& output, int index, const std::function<void(bool, const char*)>& check)
    {
        const auto hwnd = reinterpret_cast<HWND>(window->winId());
        RECT r {};
        ::GetWindowRect(hwnd, &r);
        const QRect box(r.left, r.top, r.right - r.left, r.bottom - r.top);
        ::SetWindowPos(backing, hwnd, box.x() - kPad - 40, box.y() - kPad - 40, box.width() + 2 * kPad + 80,
            box.height() + 2 * kPad + 80, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::RedrawWindow(backing, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        pump(400);
        ::DwmFlush();
        ::DwmFlush();
        const QImage shot = screenArea(box.adjusted(-kPad, -kPad, kPad, kPad));
        shot.save(output.filePath(u"search-shadow-%1-%2-%3.png"_s.arg(index).arg(shadow, where)));
        const int w = box.width(), h = box.height();
        const double below = darkening(shot, QRect(kPad + w / 4, kPad + h + 2, w / 2, 24));
        const double right = darkening(shot, QRect(kPad + w + 2, kPad + h / 4, 24, std::max(1, h / 2)));
        const double beyond = darkening(shot, QRect(kPad + w + 66, kPad, 10, std::max(1, h))); // beyond any shadow
        const QRgb corner = shot.pixel(kPad, kPad);
        const QRgb edge = shot.pixel(kPad, kPad + h / 2);
        const QRgb inside = shot.pixel(kPad + 6, kPad + h / 2);
        const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
        const bool caption = (style & WS_CAPTION) == WS_CAPTION;
        QObject* edgeItem = window->findChild<QObject*>(u"windowEdge"_s);
        const bool drawnEdge = edgeItem && edgeItem->property("visible").toBool();
        const bool rounded = magenta(corner);
        check(edgeItem, "no WindowEdge in the search box");
        check(beyond < 1.5, "the backing is not plain beyond the shadow (something else on the screen?)");
        check(!magenta(inside), "the box shows the magenta through it");
        const bool win11 = win::nativeWindowCorners();
        // The panel has a sizing frame, and with it DWM's full shadow: it keeps the standard look (App::applySearchShadow).
        if (shadow == u"standard" || where == u"panel") {
            check(caption, "no caption");
            check(below > 4 && right > 2, "no full shadow");
            check(drawnEdge == !win11, "WindowEdge drawn under DWM's edge (or missing on Windows 10)");
            check(rounded == (win11 && win::roundedCorners()), "corners not as the 圆角 setting");
        } else {
            check(!caption, "still a caption");
            if (shadow == u"none" || !win11 || !win::roundedCorners()) {
                check(below < 1.5 && right < 1.5, "a shadow left");
                check(!rounded, "a rounded corner");
                check(drawnEdge, "no edge drawn");
                check(edge != inside, "no edge line on the screen");
            } else {
                check(below > 1.5, "no shadow at all");
                check(rounded, "not rounded");
                check(!drawnEdge, "WindowEdge drawn under DWM's edge");
            }
        }
        std::fprintf(stderr, "search shadow (%s, %s): caption %d, sizing frame %d, darker below %.1f, right %.1f (beyond %.1f), corner %s %s, edge %s, WindowEdge %s\n",
            qPrintable(shadow), qPrintable(where), caption, bool(style & WS_THICKFRAME), below, right, beyond, qPrintable(hex(corner)),
            rounded ? "(rounded)" : "(square)", qPrintable(hex(edge)), drawnEdge ? "drawn" : "not drawn");
        ::ShowWindow(backing, SW_HIDE);
        return {{u"where"_s, where}, {u"caption"_s, caption}, {u"sizing_frame"_s, bool(style & WS_THICKFRAME)},
            {u"below"_s, below}, {u"right"_s, right}, {u"beyond"_s, beyond}, {u"rounded_corner"_s, rounded},
            {u"corner"_s, hex(corner)}, {u"edge"_s, hex(edge)}, {u"inside"_s, hex(inside)}, {u"window_edge_drawn"_s, drawnEdge},
            {u"size"_s, u"%1x%2"_s.arg(w).arg(h)}};
    }

    // The search box, then the panel it opens into (resizable: a sizing frame).
    static QJsonObject round(App& app, const QString& shadow, HWND backing, const QDir& output, int index, bool& ok)
    {
        const auto check = [&](bool condition, const char* what) {
            if (!condition) {
                ok = false;
                std::fprintf(stderr, "search shadow (%s): FAILED: %s\n", qPrintable(shadow), what);
            }
        };
        app.m_settings.searchShadow = shadow;
        app.applyAppearance();
        waitForQuietInput();
        pump(300);
        const std::uint64_t pressedAt = ::GetTickCount64();
        app.beginTrigger(TriggerSource::Hotkey, pressedAt);
        app.toggleLauncher(true, pressedAt);
        QQuickWindow* window = app.m_window;
        check(window, "no search box");
        if (!window)
            return {};
        QElapsedTimer limit;
        limit.start();
        while ((!app.m_launcherRevealedAt || app.m_placement->sliding()) && limit.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        pump(300);
        QJsonObject report {{u"shadow"_s, shadow}, {u"box"_s, measure(window, shadow, u"box"_s, backing, output, index, check)},
            {u"native_border"_s, win::nativeBorder(static_cast<win::WindowShadow>(SystemTheme::searchShadow()))}};
        app.setPanel(true);
        pump(900);
        check(app.m_launcher->panel(), "no panel");
        report.insert(u"panel"_s, measure(window, shadow, u"panel"_s, backing, output, index, check));
        app.setPanel(false);
        pump(900);
        report.insert(u"box_again"_s, measure(window, shadow, u"box-again"_s, backing, output, index, check));
        app.hideLauncher();
        pump(600);
        return report;
    }

    static bool run(App& app, const QDir& output)
    {
        pump(1500); // started, its windows drawn ahead (prewarm)
        const HWND backing = makeBacking();
        if (!backing) {
            std::fprintf(stderr, "search shadow: FAILED: no backing window\n");
            return false;
        }
        bool ok = true;
        QJsonArray rounds;
        int index = 0;
        double standardBelow = -1;
        for (const QString& shadow : {u"standard"_s, u"light"_s, u"none"_s, u"standard"_s}) {
            const QJsonObject row = round(app, shadow, backing, output, ++index, ok);
            rounds.append(row);
            const double below = row.value(u"box"_s).toObject().value(u"below"_s).toDouble();
            if (shadow == u"standard" && standardBelow < 0)
                standardBelow = below;
            if (shadow == u"light" && win::nativeWindowCorners() && win::roundedCorners() && below >= standardBelow * 0.85) {
                ok = false;
                std::fprintf(stderr, "search shadow (light): FAILED: not lighter than standard\n");
            }
        }
        ::DestroyWindow(backing);
        QFile file(output.filePath(u"search-shadow.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly)
            && file.write(QJsonDocument(QJsonObject {{u"passed"_s, ok}, {u"rounds"_s, rounds}}).toJson()) > 0;
        std::fprintf(stderr, "search shadow: %s\n", ok ? "passed" : "FAILED");
        return ok && saved;
    }
};

bool checkSearchShadow(App& app, const QStringList&, const QDir& output)
{
    return SearchShadowProbe::run(app, output);
}

} // namespace ws::preview

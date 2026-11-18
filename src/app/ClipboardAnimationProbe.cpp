#include "ClipboardAnimationProbe.h"

#include "App.h"
#include "Placement.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>

#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

#include <dwmapi.h>
#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws::preview {

// --probe-runtime <folder> --clipboard-animation-check
// The production App beside the Win顺 that runs; its clipboard window opened
// as its hotkey opens it (beginTrigger, then the toggle) and closed, with
// 弹出动画 on and then off (GitHub #377). Every change of the window as the
// screen gets it (cloaked, its opacity, where it is) is written down from the
// opening until it has settled, and after the closing the picture that fades
// out in its place (win::fadeOutCopy) until it is gone. On: its first frame
// seen is faded and off its place toward the box it drops from, and it ends
// whole at its place; on closing a picture fades for about 120 ms, and the
// closing itself (the picture taken) is quick. Off: whole and at its place
// from its first frame, gone at once. Into clipboard-animation.json. The
// window opens at the pointer; nothing is typed and no input is sent; each
// opening waits until the keyboard and mouse were left alone for three
// seconds, as the clipboard takes the keys while it is up.
struct ClipboardAnimationProbe {
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

    struct Look {
        qint64 at = 0;
        bool shown = false; // visible and not cloaked
        double opacity = 1;
        QRect rect;
        bool operator==(const Look&) const = default;
    };

    static Look look(QQuickWindow* window, qint64 at)
    {
        const auto hwnd = reinterpret_cast<HWND>(window->winId());
        DWORD cloaked = 0;
        ::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
        RECT r {};
        ::GetWindowRect(hwnd, &r);
        return {at, ::IsWindowVisible(hwnd) && !cloaked, std::round(window->opacity() * 1000) / 1000,
            QRect(r.left, r.top, r.right - r.left, r.bottom - r.top)};
    }

    static HWND fadingCopy()
    {
        HWND found = nullptr;
        ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
            wchar_t name[64] {};
            if (::IsWindowVisible(window) && ::GetClassNameW(window, name, 64) && std::wstring_view(name) == L"WinShun.FadingCopy") {
                *reinterpret_cast<HWND*>(data) = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&found));
        return found;
    }

    // The screen there as one sees it, layered windows included (CAPTUREBLT).
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

    // Mean difference per colour channel, 0–255.
    static double difference(const QImage& a, const QImage& b)
    {
        if (a.size() != b.size() || a.isNull())
            return 255;
        double sum = 0;
        for (int y = 0; y < a.height(); ++y) {
            const auto* p = reinterpret_cast<const QRgb*>(a.constScanLine(y));
            const auto* q = reinterpret_cast<const QRgb*>(b.constScanLine(y));
            for (int x = 0; x < a.width(); ++x)
                sum += std::abs(qRed(p[x]) - qRed(q[x])) + std::abs(qGreen(p[x]) - qGreen(q[x])) + std::abs(qBlue(p[x]) - qBlue(q[x]));
        }
        return sum / (3.0 * a.width() * a.height());
    }

    static QJsonObject json(const Look& l, const QRect& rest)
    {
        return {{u"ms"_s, l.at}, {u"shown"_s, l.shown}, {u"opacity"_s, l.opacity},
            {u"dx"_s, l.rect.x() - rest.x()}, {u"dy"_s, l.rect.y() - rest.y()}};
    }

    static QJsonObject round(App& app, bool animated, bool& ok)
    {
        const auto check = [&](bool condition, const char* what) {
            if (!condition) {
                ok = false;
                std::fprintf(stderr, "clipboard animation (%s): FAILED: %s\n", animated ? "on" : "off", what);
            }
        };
        app.m_settings.openAnimation = animated;
        app.m_settings.clipboardOpenAt = u"pointer"_s; // by the pointer, not by another program's caret
        if (app.m_clipPlacement)
            app.m_clipPlacement->setAnimated(animated);
        waitForQuietInput();
        pump(500);

        const std::uint64_t pressedAt = ::GetTickCount64();
        app.beginTrigger(TriggerSource::ClipboardHotkey, pressedAt);
        app.toggleClipboard(pressedAt);
        QQuickWindow* window = app.m_clipWindow;
        check(window, "no clipboard window");
        if (!window)
            return {};
        // Every change until a moment after it settled (or 1.5 s).
        std::vector<Look> opening;
        QElapsedTimer clock;
        clock.start();
        qint64 settledAt = -1;
        while (clock.elapsed() < 1500) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            const Look now = look(window, clock.elapsed());
            if (opening.empty() || now.shown != opening.back().shown || now.opacity != opening.back().opacity
                || now.rect != opening.back().rect)
                opening.push_back(now);
            const bool settled = now.shown && now.opacity == 1 && !app.m_clipPlacement->sliding();
            if (settled && settledAt < 0)
                settledAt = now.at;
            if (!settled)
                settledAt = -1;
            if (settledAt >= 0 && now.at - settledAt > 150)
                break;
        }
        const Look rest = opening.back();
        const auto first = std::ranges::find_if(opening, [](const Look& l) { return l.shown; });
        check(first != opening.end(), "never seen");
        check(rest.shown && rest.opacity == 1, "not whole at its place in the end");
        QJsonArray steps;
        for (const Look& l : opening)
            steps.append(json(l, rest.rect));
        const bool above = app.m_clipPlacement->above();
        QJsonObject report {{u"animated"_s, animated}, {u"above_its_anchor"_s, above}, {u"opening"_s, steps},
            {u"layered"_s, bool(::GetWindowLongW(reinterpret_cast<HWND>(window->winId()), GWL_EXSTYLE) & WS_EX_LAYERED)}};
        if (first != opening.end()) {
            const Look& seen = *first;
            const int dy = seen.rect.y() - rest.rect.y();
            const qint64 inMs = rest.at - seen.at;
            report.insert(u"first_seen"_s, json(seen, rest.rect));
            report.insert(u"in_ms"_s, inMs);
            if (animated) {
                check(seen.opacity < 0.6, "its first frame seen is not faded");
                // From the box (here the pointer) it drops from: below it, it comes down a little.
                check(above ? dy > 0 : dy < 0, "its first frame seen is not off its place toward its anchor");
                check(inMs >= 100 && inMs <= 400, "it came in too soon or too slowly");
            } else {
                check(seen.opacity == 1 && seen.rect == rest.rect, "its first frame seen is not whole at its place");
            }
        }

        // Closed as Esc closes it. The picture left in its place is the
        // clipboard as it was, not what lies behind it (the window is layered):
        // the screen there just before, and once Windows has drawn the closing
        // (the picture still whole, nothing run since).
        const QImage up = screenArea(rest.rect);
        QElapsedTimer closing;
        closing.start();
        app.hideClipboard(false);
        const double closeMs = std::round(closing.nsecsElapsed() / 100'000.0) / 10;
        const HWND copy = fadingCopy();
        ::DwmFlush();
        ::DwmFlush();
        const double changed = std::round(difference(up, screenArea(rest.rect)) * 10) / 10;
        report.insert(u"closing_picture_difference"_s, changed);
        qint64 copyGoneAt = -1;
        clock.restart();
        while (clock.elapsed() < 800) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            if (copy && copyGoneAt < 0 && !::IsWindow(copy))
                copyGoneAt = clock.elapsed();
        }
        report.insert(u"close_call_ms"_s, closeMs);
        report.insert(u"fading_copy"_s, copy != nullptr);
        report.insert(u"fading_copy_gone_after_ms"_s, copyGoneAt);
        check(!window->isVisible(), "still up after closing");
        check(!fadingCopy(), "a picture left on the screen");
        if (animated) {
            check(copy, "no picture faded out in its place");
            check(copyGoneAt >= 80 && copyGoneAt <= 400, "the picture went too soon or stayed too long");
            check(closeMs < 50, "closing took too long (the picture taken)");
            check(changed < 4, "the picture fading out is not the clipboard as it was");
        } else {
            check(!copy, "a picture faded out with 弹出动画 off");
        }
        std::fprintf(stderr, "clipboard animation (%s): first seen at opacity %.2f, %d px off; in %lld ms; closed in %.1f ms, picture %s %lld ms, screen changed by %.1f on closing\n",
            animated ? "on" : "off", first != opening.end() ? first->opacity : -1.0,
            first != opening.end() ? first->rect.y() - rest.rect.y() : 0, static_cast<long long>(report.value(u"in_ms"_s).toInteger()),
            closeMs, copy ? "gone after" : "none", static_cast<long long>(copyGoneAt), changed);
        return report;
    }

    static bool run(App& app, const QDir& output)
    {
        pump(1500); // started, its windows drawn ahead (prewarm)
        bool ok = true;
        QJsonArray rounds;
        for (const bool animated : {true, false, true})
            rounds.append(round(app, animated, ok));
        QFile file(output.filePath(u"clipboard-animation.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(QJsonObject {{u"passed"_s, ok}, {u"rounds"_s, rounds}}).toJson()) > 0;
        std::fprintf(stderr, "clipboard animation: %s\n", ok ? "passed" : "FAILED");
        return ok && saved;
    }
};

bool checkClipboardAnimation(App& app, const QStringList&, const QDir& output)
{
    return ClipboardAnimationProbe::run(app, output);
}

} // namespace ws::preview

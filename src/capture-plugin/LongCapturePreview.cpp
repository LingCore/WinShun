// --preview-long-capture (CapturePreview.h). Its window takes Enter and Esc
// while it runs; the exit code counts the checks that failed. Its auto
// target is a separate generated fixture.

#include "CapturePreview.h"

#include "Capture.h"
#include "CaptureSession.h"
#include "LongCapture.h"
#include "ScrollStitcher.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QMouseEvent>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QThread>
#include <QTranslator>

#include <dwmapi.h>

#include <algorithm>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::captureplugin::preview {

// Real Capture -> selection -> LongCapture -> WGC -> pixels. The fixture
// contains only generated images; it starts no App, clipboard or user profile.
int runLongCapture(const QStringList& args)
{
    using namespace capture;
    const int flag = args.indexOf(u"--preview-long-capture"_s);
    const QDir out(args.value(flag + 1));
    QDir().mkpath(out.path());
    const auto pump = [](int ms) {
        QElapsedTimer clock;
        clock.start();
        do {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(5);
        } while (clock.elapsed() < ms);
    };
    int failed = 0;
    const auto check = [&](bool good, const char* message) {
        if (!good) {
            ++failed;
            qWarning("long capture: %s", message);
        }
    };
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/capture_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    const double dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
    constexpr int width = 600, header = 40, viewport = 360, footer = 40, bodyHeight = 2880;
    const int pixels = qRound(width * dpr);
    const auto image = [&](int logicalHeight, QColor background) {
        QImage result(pixels, qRound(logicalHeight * dpr), QImage::Format_RGB32);
        result.fill(background);
        return result;
    };
    QImage head = image(header, QColor("#e7edf5"));
    QImage body = image(bodyHeight, Qt::white);
    QImage foot = image(footer, QColor("#dae2ec"));
    {
        QPainter painter(&head);
        painter.setFont(QFont(u"Segoe UI"_s, -1, QFont::DemiBold));
        QFont font = painter.font();
        font.setPixelSize(qRound(16 * dpr));
        painter.setFont(font);
        painter.drawText(QPointF(16 * dpr, 26 * dpr), u"WinShun · scrolling capture verification"_s);
    }
    {
        QPainter painter(&body);
        QFont font(u"Segoe UI"_s);
        font.setPixelSize(qRound(16 * dpr));
        painter.setFont(font);
        for (int row = 0; row < 80; ++row) {
            const int y = qRound(row * 36 * dpr);
            painter.fillRect(0, y, pixels, qRound(36 * dpr), row % 2 ? QColor("#f4f7fb") : Qt::white);
            painter.setPen(QColor("#1c293a"));
            painter.drawText(QPointF(16 * dpr, (row * 36 + 24) * dpr),
                u"%1  中文滚动验证 · item %2 · %3"_s.arg(row + 1, 3, 10, QChar(u'0')).arg(row * 37 + 11).arg(row * row + 17));
            painter.fillRect(qRound((450 + row % 7 * 13) * dpr), y + qRound(9 * dpr),
                qRound(24 * dpr), qRound(18 * dpr), QColor::fromHsv(row * 31 % 360, 130, 170));
        }
    }
    {
        QPainter painter(&foot);
        painter.fillRect(qRound(16 * dpr), qRound(14 * dpr), qRound(200 * dpr), qRound(4 * dpr), QColor("#8392a5"));
    }
    // --sideways: the same page turned a quarter (transposed), scrolled
    // left and right; the bars stand at the left and the right.
    const bool sideways = args.contains(u"--sideways"_s);
    const auto shown = [&](const QImage& picture) { return sideways ? capture::transposed(picture) : picture; };
    check(shown(head).save(out.filePath(u"header.png"_s)), "save header");
    check(shown(body).save(out.filePath(u"body.png"_s)), "save body");
    check(shown(foot).save(out.filePath(u"footer.png"_s)), "save footer");
    QQmlApplicationEngine engine;
    engine.addImportPath(QDir(qEnvironmentVariable("WS_APP_DIR")).filePath(u"qml"_s));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        Window {
            id: fixture
            width: 1060; height: 720; x: 80; y: 60
            title: "WinShun long capture test"
            flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
            color: "#f8f9fb"
            required property url headerSource
            required property url bodySource
            required property url footerSource
            required property bool sideways
            required property bool wheelTurns // sideways, the up-and-down wheel scrolls it too
            property real scrollY: 0 // along the page: down, or (sideways) right
            Text { x: 20; y: 16; text: "长截图验证 · 在下方区域滚动，Enter 完成"; color: "#1c293a" }
            Item {
                objectName: "longCaptureRegion"
                // Multiples of four keep the fixture's texture origin on
                // device pixels at 100/125/150/175/200 percent scaling.
                x: 20; y: 48; width: fixture.sideways ? 440 : 600; height: fixture.sideways ? 600 : 440
                Image {
                    width: fixture.sideways ? 40 : 600; height: fixture.sideways ? 600 : 40
                    source: fixture.headerSource; sourceSize.width: width * Screen.devicePixelRatio
                }
                Flickable {
                    id: view
                    x: fixture.sideways ? 40 : 0; y: fixture.sideways ? 0 : 40
                    width: fixture.sideways ? 360 : 600; height: fixture.sideways ? 600 : 360
                    contentWidth: fixture.sideways ? 2880 : 600; contentHeight: fixture.sideways ? 600 : 2880
                    contentX: fixture.sideways ? fixture.scrollY : 0; contentY: fixture.sideways ? 0 : fixture.scrollY
                    interactive: false; clip: true
                    Image {
                        width: fixture.sideways ? 2880 : 600; height: fixture.sideways ? 600 : 2880
                        source: fixture.bodySource; sourceSize.width: width * Screen.devicePixelRatio
                    }
                    // Sideways: the horizontal wheel, and with wheelTurns the other one too.
                    WheelHandler {
                        enabled: !fixture.sideways || fixture.wheelTurns
                        onWheel: (event) => {
                            fixture.scrollY = Math.max(0, Math.min(2520, fixture.scrollY - event.angleDelta.y / 120 * 72))
                            event.accepted = true
                        }
                    }
                    WheelHandler {
                        orientation: Qt.Horizontal
                        enabled: fixture.sideways
                        onWheel: (event) => {
                            fixture.scrollY = Math.max(0, Math.min(2520, fixture.scrollY - event.angleDelta.x / 120 * 72))
                            event.accepted = true
                        }
                    }
                }
                Image {
                    x: fixture.sideways ? 400 : 0; y: fixture.sideways ? 0 : 400
                    width: fixture.sideways ? 40 : 600; height: fixture.sideways ? 600 : 40
                    source: fixture.footerSource; sourceSize.width: width * Screen.devicePixelRatio
                }
            }
        }
    )", QUrl(u"qrc:/long-capture-fixture.qml"_s));
    std::unique_ptr<QQuickWindow> fixture(qobject_cast<QQuickWindow*>(component.createWithInitialProperties({
        {u"headerSource"_s, QUrl::fromLocalFile(out.filePath(u"header.png"_s))},
        {u"bodySource"_s, QUrl::fromLocalFile(out.filePath(u"body.png"_s))},
        {u"footerSource"_s, QUrl::fromLocalFile(out.filePath(u"footer.png"_s))},
        {u"sideways"_s, sideways},
        {u"wheelTurns"_s, args.contains(u"--wheel-turns"_s)}})));
    if (!fixture) { qWarning().noquote() << component.errorString(); return 3; }
    const bool up = args.contains(u"--up"_s);
    const bool both = args.contains(u"--both"_s);
    const bool interactive = args.contains(u"--interactive"_s);
    const int start = up ? bodyHeight - viewport : both ? 720 : 0;
    fixture->setProperty("scrollY", start);
    if (args.contains(u"--edge"_s)) fixture->setPosition(-20, -48);
    fixture->show();
    fixture->requestActivate();
    pump(400);
    if (args.contains(u"--fixture-only"_s))
        return QGuiApplication::exec();
    QProcess externalFixture;
    const bool automatic = args.contains(u"--auto"_s);
    if (automatic) {
        // ScrollInput deliberately ignores our own process. Its target must
        // be a second, generated fixture process for a real auto-scroll test.
        fixture->hide();
        QStringList childArgs = args; // Preview receives wideArgs without argv[0].
        childArgs.removeAll(u"--auto"_s);
        childArgs[childArgs.indexOf(u"--preview-long-capture"_s) + 1] = out.filePath(u"auto-fixture"_s);
        childArgs << u"--fixture-only"_s;
        externalFixture.start(QCoreApplication::applicationFilePath(), childArgs);
        if (!externalFixture.waitForStarted(3000)) return 3;
        pump(800);
    }
    auto* item = fixture->findChild<QQuickItem*>(u"longCaptureRegion"_s);
    POINT origin {0, 0};
    ::ClientToScreen(reinterpret_cast<HWND>(fixture->winId()), &origin);
    const QRect region(origin.x + qRound(item->x() * dpr), origin.y + qRound(item->y() * dpr),
        qRound(item->width() * dpr), qRound(item->height() * dpr));
    const auto isExternalFixture = [&] {
        DWORD pid = 0;
        const POINT point {region.center().x(), region.center().y()};
        ::GetWindowThreadProcessId(::WindowFromPoint(point), &pid);
        return externalFixture.state() == QProcess::Running && pid == externalFixture.processId();
    };
    if (automatic && !isExternalFixture()) {
        qWarning("long capture: generated target window is not under the selection; no scrolling sent");
        externalFixture.terminate();
        externalFixture.waitForFinished(3000);
        return 3;
    }
    QImage result;
    bool ended = false;
    bool copied = false;
    bool edited = false;
    bool precedingCapture = false;
    QString precedingOrigin;
    Capture::Callbacks callbacks;
    callbacks.longCapture = [&](const QImage& picture) { result = picture; ended = true; edited = true; };
    callbacks.copy = [&](const QImage& picture, const QByteArray&, const QString& origin) {
        if (precedingCapture) {
            check(!origin.isEmpty(), "regular capture must supply its own clipboard identity");
            precedingOrigin = origin;
            return;
        }
        check(origin.isEmpty(), "long capture must not inherit the preceding screenshot's identity");
        result = picture;
        ended = true;
        copied = true;
    };
    callbacks.record = [&](const QImage& picture, const QByteArray&) { result = picture; ended = true; };
    callbacks.saveFolder = [&] { return out.absolutePath(); };
    bool fileSaved = false;
    callbacks.saved = [&](const QString&, const QString& error) { check(error.isEmpty(), "save exported image"); fileSaved = true; };
    PinManager pins({});
    callbacks.pins = [&] { return &pins; };
    callbacks.copyText = [&](const QString& text) {
        QFile diagnostics(out.filePath(u"diagnostics.txt"_s));
        if (diagnostics.open(QIODevice::WriteOnly)) diagnostics.write(text.toUtf8());
    };
    Capture capture(&engine, std::move(callbacks));
    capture.trigger();
    pump(300);
    CaptureSession* session = nullptr;
    CaptureScreen* screen = nullptr;
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        auto* candidate = window->property("screenModel").value<CaptureScreen*>();
        if (candidate && candidate->rect().contains(region)) {
            screen = candidate;
            session = window->property("session").value<CaptureSession*>();
            break;
        }
    }
    if (!session || !screen) { qWarning("long capture: no capture overlay"); return 3; }
    session->pointerPress(screen->index(), region.topLeft() - screen->origin(), Qt::LeftButton);
    session->pointerMove(screen->index(), region.bottomRight() - screen->origin());
    session->pointerRelease(screen->index(), region.bottomRight() - screen->origin(), Qt::LeftButton);
    if (args.contains(u"--preceding-capture"_s)) {
        precedingCapture = true;
        session->act(static_cast<int>(CaptureSession::Action::Copy));
        check(!precedingOrigin.isEmpty() && capture.lastClipboardId() == precedingOrigin,
            "remember the regular capture before starting a long capture");
        precedingCapture = false;
        pump(400);
        capture.trigger();
        pump(300);
        // Overlays are reused; their screen/session objects stay alive.
        session->pointerPress(screen->index(), region.topLeft() - screen->origin(), Qt::LeftButton);
        session->pointerMove(screen->index(), region.bottomRight() - screen->origin());
        session->pointerRelease(screen->index(), region.bottomRight() - screen->origin(), Qt::LeftButton);
    }
    session->act(static_cast<int>(CaptureSession::Action::Long));
    LongCapture* controller = nullptr;
    QQuickWindow* panel = nullptr;
    for (int i = 0; i < 300 && !controller; ++i) {
        pump(10);
        for (QWindow* window : QGuiApplication::topLevelWindows()) {
            controller = window->property("capture").value<LongCapture*>();
            if (controller) { panel = qobject_cast<QQuickWindow*>(window); break; }
        }
    }
    if (!controller || !panel) { qWarning("long capture: no long capture panel"); return 3; }
    check(!controller->autoScroll(), "manual scrolling must be the default");
    check(!controller->horizontal(), "up and down must be the default");
    // --turn: 横向 chosen on the panel; without it, sideways is for the engine to see.
    const QSize controlSize = panel->size();
    for (const auto* name : {"longCapturePin", "longCaptureSave", "longCaptureDone", "longCaptureCancel", "longCaptureCopy"}) {
        auto* button = panel->findChild<QQuickItem*>(QString::fromLatin1(name));
        check(button && button->isVisible() && button->isEnabled(), "output buttons must remain visible during capture");
    }
    auto* preview = panel->findChild<QQuickWindow*>(u"longCapturePreview"_s);
    check(preview, "preview must have its own window");
    // --preview-over (GitHub #378): the preview opened by hand over the region,
    // as when the selection fills the screen. Left out of the frames all the
    // same (the picture is held against the page at the end); dragged off it
    // later (dragPreviewOff).
    const bool previewOver = preview && args.contains(u"--preview-over"_s);
    const auto previewRect = [&] {
        RECT r {};
        ::GetWindowRect(reinterpret_cast<HWND>(preview->winId()), &r);
        return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    };
    const auto previewLeftOut = [&] {
        DWORD affinity = WDA_NONE;
        return ::GetWindowDisplayAffinity(reinterpret_cast<HWND>(preview->winId()), &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE;
    };
    if (previewOver) {
        panel->setProperty("previewVisible", false);
        pump(100);
        // Its top left a little inside the region (logical px on its screen, as LongCapture::placePanel).
        QScreen* on = fixture->screen();
        MONITORINFO monitor {sizeof monitor};
        ::GetMonitorInfoW(::MonitorFromPoint({region.center().x(), region.center().y()}, MONITOR_DEFAULTTONEAREST), &monitor);
        const QPoint inside = region.topLeft() + QPoint(qRound(40 * dpr), qRound(60 * dpr));
        preview->setPosition(on->geometry().topLeft() + (inside - QPoint(monitor.rcMonitor.left, monitor.rcMonitor.top)) / dpr);
        // Left out as it is told to show (LongCapture's handler ran just before
        // this one), before Windows has it up: nothing else may come to set it
        // right later, as a move after showing did here and not on the
        // reporter's screen.
        bool leftOutAsShown = false;
        const auto shown = QObject::connect(preview, &QWindow::visibleChanged, preview, [&](bool visible) {
            if (visible)
                leftOutAsShown = previewLeftOut();
        });
        panel->setProperty("previewVisible", true); // as the 预览 button does
        QObject::disconnect(shown);
        pump(300);
        check(preview->isVisible() && previewRect().intersects(region), "the preview must be shown over the region for this check");
        check(leftOutAsShown, "a preview opened over the region must be left out of the frames as it shows");
        check(previewLeftOut(), "a preview shown over the region must stay left out of the frames");
    }
    // A real drag (SendInput, from another thread: the window's move loop
    // holds this one until the button is let go) by the preview's heading,
    // off the region; the pointer put back after.
    const auto dragPreviewOff = [&] {
        const HWND hwnd = reinterpret_cast<HWND>(preview->winId());
        const QRect before = previewRect();
        const POINT from {before.left() + qRound(24 * dpr), before.top() + qRound(14 * dpr)};
        if (::GetAncestor(::WindowFromPoint(from), GA_ROOT) != hwnd) {
            check(false, "the preview's heading must be on top where it is dragged by");
            return;
        }
        MONITORINFO monitor {sizeof monitor};
        ::GetMonitorInfoW(::MonitorFromPoint(from, MONITOR_DEFAULTTONEAREST), &monitor);
        // Past the drag threshold slowly; the window moves from where the pointer is then.
        const int start = qRound(32 * dpr);
        // Right of the region and its shadow's reach (CaptureExclusion's, by the window's DPI), as far as the monitor allows.
        const int reach = ::MulDiv(64, int(::GetDpiForWindow(hwnd)), 96);
        const int wanted = region.right() + reach + start + 16 - before.left();
        const int dx = std::min(wanted, int(monitor.rcWork.right) - before.right() - 1);
        POINT was {};
        ::GetCursorPos(&was);
        std::thread hand([from, dx, start] {
            const auto at = [](int x, int y) { ::SetCursorPos(x, y); ::Sleep(15); };
            at(from.x, from.y);
            INPUT press {};
            press.type = INPUT_MOUSE;
            press.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            ::SendInput(1, &press, sizeof press);
            ::Sleep(60);
            for (int step = 1; step <= 16; ++step)
                at(from.x + start * step / 16, from.y);
            ::Sleep(150);
            for (int step = 1; step <= 20; ++step)
                at(from.x + start + (dx - start) * step / 20, from.y);
            ::Sleep(120);
            INPUT release {};
            release.type = INPUT_MOUSE;
            release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            ::SendInput(1, &release, sizeof release);
        });
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 1500)
            pump(10);
        hand.join();
        pump(200);
        ::SetCursorPos(was.x, was.y);
        const QRect after = previewRect();
        qInfo("long capture: preview dragged by %d px (wanted %d), from %d,%d to %d,%d; region %d,%d %dx%d; left out %d",
            after.left() - before.left(), dx, before.left(), before.top(), after.left(), after.top(), region.x(), region.y(),
            region.width(), region.height(), int(previewLeftOut()));
        // It trails the pointer by at most the slow start.
        const int moved = after.left() - before.left();
        check(moved >= dx - start - 2 && moved <= dx + 2 && std::abs(after.top() - before.top()) <= 2,
            "the preview must follow a drag by its heading");
        check(previewLeftOut() == after.adjusted(-reach, -reach, reach, reach).intersects(region),
            "the preview must be left out of the frames only while over the region");
    };
    const auto clickAction = [&](const char* name) {
        auto* button = panel->findChild<QQuickItem*>(QString::fromLatin1(name));
        if (!button) { check(false, "missing action button"); return; }
        const QPointF point = button->mapToScene(QPointF(button->width() / 2, button->height() / 2));
        const QPointF global = panel->mapToGlobal(point.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress, point, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(panel, &press);
        QCoreApplication::sendEvent(panel, &release);
    };
    const auto interfacePicture = [&] {
        // A developer-only photograph of this generated fixture and its real
        // chrome. The border strips lie outside the recorded region and the
        // panel is left out of the frames while it covers the region; clear
        // that for the picture, so the stitched pixels stay unchanged.
        struct Chrome { HWND window; DWORD affinity; };
        struct Search { HWND fixture; std::vector<Chrome> found; } search {reinterpret_cast<HWND>(fixture->winId()), {}};
        ::EnumWindows([](HWND window, LPARAM data) -> BOOL {
            auto* search = reinterpret_cast<Search*>(data);
            DWORD pid = 0, affinity = WDA_NONE;
            wchar_t name[64] {};
            ::GetWindowThreadProcessId(window, &pid);
            if (pid != ::GetCurrentProcessId() || window == search->fixture || !::IsWindowVisible(window)
                || !::GetClassNameW(window, name, 64))
                return TRUE;
            const std::wstring_view type(name);
            if (type == L"WinShunLongCaptureEdge" || type.find(L"QWindow") != std::wstring_view::npos) {
                ::GetWindowDisplayAffinity(window, &affinity);
                search->found.push_back({window, affinity});
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        const std::vector<Chrome>& chrome = search.found;
        check(chrome.size() >= 5, "selection border and controls must be visible native windows");
        for (const auto& entry : chrome)
            if (entry.affinity == WDA_EXCLUDEFROMCAPTURE) ::SetWindowDisplayAffinity(entry.window, WDA_NONE);
        ::DwmFlush();
        const QImage desktop = fixture->screen()->grabWindow(0).toImage();
        for (const auto& entry : chrome)
            if (entry.affinity == WDA_EXCLUDEFROMCAPTURE) ::SetWindowDisplayAffinity(entry.window, entry.affinity);
        const QRect picture(origin.x, origin.y, qRound(fixture->width() * dpr), qRound(fixture->height() * dpr));
        check(desktop.copy(picture).save(out.filePath(u"interface.png"_s)), "save actual selection and controls");
    };
    if (sideways && args.contains(u"--turn"_s)) {
        // A real click (SendInput): in the heading's row, where the panel is
        // dragged by, a made-up mouse event reaches no button.
        pump(400);
        auto* button = panel->findChild<QQuickItem*>(u"longCaptureDirection"_s);
        check(button && button->isVisible(), "the direction button must be shown");
        if (button) {
            const QPointF at = button->mapToScene(QPointF(button->width() / 2, button->height() / 2)) * panel->devicePixelRatio();
            POINT screenAt {LONG(at.x()), LONG(at.y())};
            ::ClientToScreen(reinterpret_cast<HWND>(panel->winId()), &screenAt);
            POINT was {};
            ::GetCursorPos(&was);
            ::SetCursorPos(screenAt.x, screenAt.y);
            pump(100);
            INPUT clicks[2] {};
            clicks[0].type = clicks[1].type = INPUT_MOUSE;
            clicks[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            clicks[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            ::SendInput(2, clicks, sizeof(INPUT));
            pump(300);
            ::SetCursorPos(was.x, was.y);
        }
        check(controller->horizontal(), "the direction button must turn it sideways");
    }
    int nearest = start, farthest = start;
    const auto move = [&](int position, int wait = 150) {
        fixture->setProperty("scrollY", position);
        nearest = std::min(nearest, position);
        farthest = std::max(farthest, position);
        pump(wait);
    };
    if (automatic) {
        if (!isExternalFixture()) {
            controller->cancel();
            externalFixture.terminate();
            externalFixture.waitForFinished(3000);
            return 3;
        }
        // --mouse-in: the pointer over the region, so SendInput's wheel is
        // tried too (it is put back afterwards).
        POINT was {};
        const bool mouseIn = args.contains(u"--mouse-in"_s);
        if (mouseIn) {
            ::GetCursorPos(&was);
            ::SetCursorPos(region.center().x(), region.center().y());
        }
        controller->setAutoScroll(true);
        QElapsedTimer clock;
        clock.start();
        qint64 told = 0;
        while (controller->running() && clock.elapsed() < 20'000) {
            pump(10);
            if (clock.elapsed() - told >= 500) { // what it did, for when it does not end
                controller->copyDiagnostics();
                told = clock.elapsed();
            }
        }
        if (mouseIn)
            ::SetCursorPos(was.x, was.y);
        check(!controller->running(), "auto-scroll must stop at the end");
        if (controller->running()) controller->finish();
        farthest = bodyHeight - viewport;
        externalFixture.terminate();
        externalFixture.waitForFinished(3000);
    } else if (interactive) {
        QElapsedTimer clock;
        clock.start();
        while (controller->running() && clock.elapsed() < 120'000) {
            const int position = qRound(fixture->property("scrollY").toReal());
            nearest = std::min(nearest, position);
            farthest = std::max(farthest, position);
            pump(10);
        }
        if (controller->running()) controller->cancel();
    } else {
        // Move during the old initial settling period, then finish on a
        // frame that has only just been presented to the compositor.
        pump(100);
        if (up) {
            for (int y = start - 120; y >= 0; y -= 120) move(y);
        } else if (both) {
            for (int y : {600, 480, 600, 720, 840, 960, 1080, 960, 840, 720, 600, 480, 360, 240, 120, 0}) move(y);
        } else {
            for (int y = 120; y <= 1440; y += 120) move(y);
        }
        panel->grabWindow().save(out.filePath(u"panel.png"_s));
        if (preview && preview->isVisible()) preview->grabWindow().save(out.filePath(u"preview.png"_s));
        interfacePicture();
        if (previewOver)
            dragPreviewOff();
        check(panel->size() == controlSize, "status changes must not move or resize the action strip");
        controller->copyDiagnostics();
        if (args.contains(u"--cancel"_s)) clickAction("longCaptureCancel");
        else {
            if (!up && !both) move(1560, 20);
            if (args.contains(u"--copy"_s)) clickAction("longCaptureCopy");
            else if (args.contains(u"--save"_s)) clickAction("longCaptureSave");
            else if (args.contains(u"--pin"_s)) clickAction("longCapturePin");
            else clickAction("longCaptureDone");
        }
    }
    for (int i = 0; i < 500 && controller->running(); ++i) pump(10);
    check(!controller->running(), "capture must finish and release its stream");
    check(!panel->isVisible() && !controller->hasPreview(), "panel and preview must be released");
    check(!preview || !preview->isVisible(), "preview window must close with the controls");
    check(copied == args.contains(u"--copy"_s), "copy button must use the copy callback");
    check(edited == (!args.contains(u"--copy"_s) && !args.contains(u"--save"_s)
        && !args.contains(u"--pin"_s) && !args.contains(u"--cancel"_s)), "only Done opens the editor");
    if (args.contains(u"--save"_s)) {
        for (int i = 0; i < 300 && !fileSaved; ++i) pump(10);
        check(fileSaved, "save must complete before the test ends");
    }
    if (args.contains(u"--pin"_s)) {
        check(pins.count() == 1, "pin action must create one pin");
        pins.closeAll();
    }
    if (args.contains(u"--cancel"_s)) {
        check(!ended && result.isNull(), "cancel must not export a picture");
        qInfo("long capture: cancelled, %d failures", failed);
    }
    else {
        check(ended && !result.isNull(), "capture must export a picture");
        const int from = qRound(nearest * dpr);
        const int rows = qRound((farthest - nearest + viewport) * dpr);
        QImage expected(pixels, head.height() + rows + foot.height(), QImage::Format_RGB32);
        QPainter painter(&expected);
        painter.drawImage(0, 0, head);
        painter.drawImage(0, head.height(), body.copy(0, from, pixels, rows));
        painter.drawImage(0, head.height() + rows, foot);
        painter.end();
        if (sideways) {
            expected = capture::transposed(expected);
            check(controller->horizontal(), "the capture must have gone sideways (the button, or seen)");
        }
        expected.save(out.filePath(u"expected.png"_s));
        result.save(out.filePath(u"result.png"_s));
        // Image texture sampling can round a channel by one even at 1:1
        // (also present in the unstitched WGC frame). No position tolerance.
        int difference = result.size() == expected.size() ? 0 : 255;
        if (!difference) {
            for (int y = 0; y < result.height(); ++y) {
                const auto* actual = reinterpret_cast<const QRgb*>(result.constScanLine(y));
                const auto* wanted = reinterpret_cast<const QRgb*>(expected.constScanLine(y));
                for (int x = 0; x < result.width(); ++x)
                    difference = std::max({difference, std::abs(qRed(actual[x]) - qRed(wanted[x])),
                        std::abs(qGreen(actual[x]) - qGreen(wanted[x])), std::abs(qBlue(actual[x]) - qBlue(wanted[x]))});
            }
        }
        check(difference <= 1, "WGC output must match the page geometry and both fixed bars within one colour level");
        qInfo("long capture: maximum channel difference %d", difference);
        qInfo("long capture: %d x %d, expected %d x %d, %d failures", result.width(), result.height(), expected.width(), expected.height(), failed);
    }
    return failed;
}

} // namespace ws::captureplugin::preview

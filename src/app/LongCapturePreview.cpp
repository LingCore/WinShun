#include "Preview.h"

#include "Capture.h"
#include "CaptureSession.h"
#include "LongCapture.h"

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

using namespace Qt::StringLiterals;

namespace ws::preview {

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
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/winshun_en.qm"_s))
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
    check(head.save(out.filePath(u"header.png"_s)), "save header");
    check(body.save(out.filePath(u"body.png"_s)), "save body");
    check(foot.save(out.filePath(u"footer.png"_s)), "save footer");
    QQmlApplicationEngine engine;
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
            property alias scrollY: view.contentY
            Text { x: 20; y: 16; text: "长截图验证 · 在下方区域滚动，Enter 完成"; color: "#1c293a" }
            Item {
                objectName: "longCaptureRegion"
                // Multiples of four keep the fixture's texture origin on
                // device pixels at 100/125/150/175/200 percent scaling.
                x: 20; y: 48; width: 600; height: 440
                Image { width: 600; height: 40; source: fixture.headerSource; sourceSize.width: 600 * Screen.devicePixelRatio }
                Flickable {
                    id: view
                    x: 0; y: 40; width: 600; height: 360
                    contentHeight: 2880; clip: true; boundsBehavior: Flickable.StopAtBounds
                    Image { width: 600; height: 2880; source: fixture.bodySource; sourceSize.width: 600 * Screen.devicePixelRatio }
                    WheelHandler {
                        onWheel: (event) => {
                            view.contentY = Math.max(0, Math.min(2520, view.contentY - event.angleDelta.y / 120 * 72))
                            event.accepted = true
                        }
                    }
                }
                Image { y: 400; width: 600; height: 40; source: fixture.footerSource; sourceSize.width: 600 * Screen.devicePixelRatio }
            }
        }
    )", QUrl(u"qrc:/long-capture-fixture.qml"_s));
    std::unique_ptr<QQuickWindow> fixture(qobject_cast<QQuickWindow*>(component.createWithInitialProperties({
        {u"headerSource"_s, QUrl::fromLocalFile(out.filePath(u"header.png"_s))},
        {u"bodySource"_s, QUrl::fromLocalFile(out.filePath(u"body.png"_s))},
        {u"footerSource"_s, QUrl::fromLocalFile(out.filePath(u"footer.png"_s))}})));
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
    Capture::Callbacks callbacks;
    callbacks.longCapture = [&](const QImage& picture) { result = picture; ended = true; edited = true; };
    callbacks.copy = [&](const QImage& picture, const QByteArray&) { result = picture; ended = true; copied = true; };
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
    const QSize controlSize = panel->size();
    for (const auto* name : {"longCapturePin", "longCaptureSave", "longCaptureDone", "longCaptureCancel", "longCaptureCopy"}) {
        auto* button = panel->findChild<QQuickItem*>(QString::fromLatin1(name));
        check(button && button->isVisible() && button->isEnabled(), "output buttons must remain visible during capture");
    }
    auto* preview = panel->findChild<QQuickWindow*>(u"longCapturePreview"_s);
    check(preview, "preview must have its own window");
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
        // chrome. Temporarily include only our excluded windows; all lie
        // outside the recorded region, so the stitched pixels stay unchanged.
        struct Chrome { HWND window; DWORD affinity; };
        std::vector<Chrome> chrome;
        ::EnumWindows([](HWND window, LPARAM data) -> BOOL {
            DWORD pid = 0, affinity = 0;
            ::GetWindowThreadProcessId(window, &pid);
            if (pid == ::GetCurrentProcessId() && ::IsWindowVisible(window)
                && ::GetWindowDisplayAffinity(window, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE)
                reinterpret_cast<std::vector<Chrome>*>(data)->push_back({window, affinity});
            return TRUE;
        }, reinterpret_cast<LPARAM>(&chrome));
        check(chrome.size() >= 5, "selection border and controls must be visible native windows");
        for (const auto& entry : chrome) ::SetWindowDisplayAffinity(entry.window, WDA_NONE);
        ::DwmFlush();
        const QImage desktop = fixture->screen()->grabWindow(0).toImage();
        for (const auto& entry : chrome) ::SetWindowDisplayAffinity(entry.window, entry.affinity);
        const QRect picture(origin.x, origin.y, qRound(fixture->width() * dpr), qRound(fixture->height() * dpr));
        check(desktop.copy(picture).save(out.filePath(u"interface.png"_s)), "save actual selection and controls");
    };
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
        controller->setAutoScroll(true);
        QElapsedTimer clock;
        clock.start();
        while (controller->running() && clock.elapsed() < 20'000) {
            pump(10);
        }
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

} // namespace ws::preview

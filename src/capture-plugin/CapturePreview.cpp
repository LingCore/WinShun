// The screen capture's previews (CapturePreview.h): what WinShun.exe's
// Preview.cpp had of the capture before it became a plugin (GitHub #138).

#include "CapturePreview.h"
#include "CaptureSettings.h"

#include "AnnotationModel.h"
#include "Capture.h"
#include "CaptureSession.h"
#include "CaptureOcr.h"
#include "WindowFrame.h"
#include "platform/OcrFiles.h"
#include "RawClicks.h"
#include "Recorder.h"
#include "RecordingPointer.h"
#include "SystemTheme.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>
#include <QTimer>
#include <QTemporaryDir>
#include <QTranslator>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

using namespace Qt::StringLiterals;

namespace ws::captureplugin::preview {

namespace {

constexpr auto kCaptureToolsFlag = u"--preview-capture-tools";
constexpr auto kSettingsFlag = u"--preview-capture-settings";
constexpr auto kRecordingFlag = u"--preview-recording";
constexpr auto kLongCaptureFlag = u"--preview-long-capture";
constexpr auto kOcrFlag = u"--preview-ocr";

int g_warnings = 0;
bool g_edges = false; // --edges (or --tree): every grab also writes <png>.edges.txt
QtMessageHandler g_previous = nullptr;
QString g_logPath; // the run is elevated, its console cannot be read

void say(const QString& line)
{
    std::fprintf(stderr, "%s\n", qUtf8Printable(line));
    if (g_logPath.isEmpty())
        return;
    QFile file(g_logPath);
    if (file.open(QIODevice::Append | QIODevice::Text))
        file.write((line + u'\n').toUtf8());
}

void countWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        ++g_warnings;
    if (g_previous)
        g_previous(type, context, message);
    say((type == QtDebugMsg || type == QtInfoMsg ? u""_s : u"WARNING: "_s) + message);
}

QString valueAfter(const QStringList& args, QStringView flag, const QString& fallback = {})
{
    const qsizetype at = args.indexOf(flag);
    return at >= 0 && at + 1 < args.size() ? args[at + 1] : fallback;
}

void pump(int ms)
{
    QElapsedTimer timer;
    timer.start();
    do
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    while (timer.elapsed() < ms);
}

// A window off the screen gets no resize or expose from Windows, so its
// content item would stay 0x0 (and every `parent.width` with it): sized
// here, more than once, as the height follows the width through the
// wrapping. Items are laid out (polished) only for a frame, and a frame
// comes only with a grab here: a grab per pass, thrown away, until the
// height the page asks for is the window's.
void settle(QQuickWindow* window, int passes = 4)
{
    for (int pass = 0; pass < passes; ++pass) {
        window->contentItem()->setSize(QSizeF(window->width(), window->height()));
        pump(200);
        (void)window->grabWindow();
        if (window->contentItem()->height() == window->height() && pass > 0)
            break;
    }
}

// Every visible Rectangle with an edge that is not on a whole device pixel
// (what a 1 px line, a border or a child inset by a border looks like at 150 %),
// in device pixels: "<png>.edges.txt". Thin ones (a separator) and ones with a
// border or a radius are the ones that show.
void writeEdges(QQuickWindow* window, const QString& path)
{
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    const qreal dpr = window->effectiveDevicePixelRatio();
    const auto fraction = [](qreal v) { return std::abs(v - std::round(v)); };
    std::function<void(QQuickItem*, int, const QString&)> walk = [&](QQuickItem* item, int depth, const QString& trail) {
        if (!item->isVisible() || item->opacity() <= 0 || depth > 40)
            return;
        const QString name = QString::fromLatin1(item->metaObject()->className()).section(u'_', 0, 0);
        const QString here = trail + u"/"_s + (item->objectName().isEmpty() ? name : item->objectName());
        if (name == u"QQuickRectangle" && item->width() > 0 && item->height() > 0) {
            const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            const qreal l = r.left() * dpr, t = r.top() * dpr, rt = r.right() * dpr, b = r.bottom() * dpr;
            qreal bw = 0;
            if (QObject* pen = item->property("border").value<QObject*>())
                bw = pen->property("width").toReal();
            const qreal alpha = item->property("color").value<QColor>().alphaF();
            const bool edge = fraction(l) > 0.02 || fraction(t) > 0.02 || fraction(rt) > 0.02 || fraction(b) > 0.02;
            if (edge && (alpha > 0.02 || bw > 0)) {
                out.write(u"l=%1 t=%2 r=%3 b=%4 | %5x%6 dev | border %7 radius %8 alpha %9 | %10 (qml y=%11 h=%12)"_s
                              .arg(l, 0, 'f', 2).arg(t, 0, 'f', 2).arg(rt, 0, 'f', 2).arg(b, 0, 'f', 2)
                              .arg(rt - l, 0, 'f', 2).arg(b - t, 0, 'f', 2).arg(bw, 0, 'f', 2)
                              .arg(item->property("radius").toReal()).arg(alpha, 0, 'f', 2).arg(here)
                              .arg(item->y(), 0, 'f', 3).arg(item->height(), 0, 'f', 3)
                              .toUtf8() + '\n');
            }
        }
        for (QQuickItem* child : item->childItems())
            walk(child, depth + 1, here);
    };
    walk(window->contentItem(), 0, QString());
}

bool saveGrab(QQuickWindow* window, const QString& path)
{
    const QImage image = window->grabWindow();
    if (g_edges)
        writeEdges(window, path + u".edges.txt"_s);
    const bool saved = !image.isNull() && image.save(path, "PNG");
    say(u"%1 %2 %3x%4"_s.arg(saved ? u"saved"_s : u"NOT saved"_s, path).arg(image.width()).arg(image.height()));
    return saved;
}

// Off the screen, never activated: the software renderer draws what is
// shown, and the grab takes the frame.
void showOffScreen(QQuickWindow* window)
{
    window->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    window->setProperty("_q_showWithoutActivating", true);
    window->setMaximumHeight(32000); // taller than the screen, for a whole page at a time
    // Win32 window coordinates are limited to 32767 physical pixels. Keep
    // the offscreen position below that even at 200% and larger scales.
    const int offscreen = static_cast<int>(std::floor(24000 / window->devicePixelRatio()));
    window->setPosition(offscreen, offscreen);
    window->show();
}

int finish(QQuickWindow* window, bool saved)
{
    window->hide();
    delete window;
    say(u"%1 warning(s)"_s.arg(g_warnings));
    return saved ? std::min(g_warnings, 100) : 4;
}

int runSettings(const QStringList& args)
{
    const QDir out(valueAfter(args, kSettingsFlag));
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 4;
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/capture_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    CaptureSettings editor(temporary.filePath(u"capture.ini"_s));
    WindowFrame frame(false);
    QQmlApplicationEngine engine;
    engine.addImportPath(QDir(valueAfter(args, u"--app-dir")).filePath(u"qml"_s));
    QQmlComponent component(&engine, u"WinShun"_s, u"CaptureSettingsWindow"_s);
    auto* window = qobject_cast<QQuickWindow*>(component.createWithInitialProperties({
        {u"editor"_s, QVariant::fromValue(&editor)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    if (!window) { say(component.errorString()); return 3; }
    frame.setWindow(window);
    editor.setWindow(window);
    window->resize(560, 640);
    showOffScreen(window);
    settle(window);
    QObject* toggle = window->findChild<QObject*>(u"capturePauseFullScreen"_s);
    bool ok = toggle != nullptr;
    if (toggle) {
        for (const bool enabled : {true, false}) {
            QMetaObject::invokeMethod(toggle, "toggled", Q_ARG(bool, enabled));
            settle(window);
            ok = ok && editor.capturePauseInFullScreen() == enabled
                && CaptureOptions::load(temporary.filePath(u"capture.ini"_s)).pauseInFullScreen == enabled
                && toggle->property("checked").toBool() == enabled;
        }
    }
    if (!ok)
        qWarning("capture settings: full-screen toggle did not update the editor and isolated file");
    if (args.contains(u"--pin-image"_s)) {
        if (QObject* options = window->findChild<QObject*>(u"captureOptions"_s))
            options->setProperty("currentGroup", 3);
        settle(window);
        auto* row = window->findChild<QQuickItem*>(u"capturePinImageHotkey"_s);
        ok = ok && row;
        if (row) {
            for (QQuickItem* item = row->parentItem(); item; item = item->parentItem()) {
                if (item->metaObject()->indexOfProperty("contentY") < 0)
                    continue;
                const qreal y = row->mapToItem(item, QPointF()).y() + item->property("contentY").toReal() - 24;
                const qreal limit = (std::max)(qreal(0), item->property("contentHeight").toReal() - item->height());
                item->setProperty("contentY", std::clamp(y, qreal(0), limit));
                break;
            }
            settle(window);
            const QRectF rect = row->mapRectToItem(window->contentItem(), row->boundingRect());
            ok = ok && row->isVisible() && QRectF(QPointF(), window->contentItem()->size()).contains(rect);
            say(u"pin image shortcut: %1; %2; visible=%3"_s.arg(row->property("title").toString(),
                row->property("words").toString()).arg(ok));
        }
    }
    const bool saved = saveGrab(window, out.filePath(u"capture-settings.png"_s));
    return finish(window, ok && saved);
}

int runOcr(const QStringList& args)
{
    const QDir out(valueAfter(args, kOcrFlag));
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/capture_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    ocr::setComponentsRoot(out.filePath(u"components"_s));
    capture::CaptureOcr::setProgramFolder(out.filePath(u"no-bundled-copy"_s));
    capture::CaptureOcr reader;
    WindowFrame frame(false);
    QQmlApplicationEngine engine;
    engine.addImportPath(QDir(valueAfter(args, u"--app-dir")).filePath(u"qml"_s));
    QQmlComponent component(&engine, u"WinShun"_s, u"OcrPanel"_s);
    auto* window = qobject_cast<QQuickWindow*>(component.createWithInitialProperties({
        {u"ocr"_s, QVariant::fromValue(&reader)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    if (!window) { say(component.errorString()); return 3; }
    frame.setWindow(window);
    QImage image(600, 80, QImage::Format_RGB32);
    image.fill(Qt::white);
    reader.read(image, 1.0);
    showOffScreen(window);
    settle(window);
    QObject* button = window->findChild<QObject*>(u"setupEnhancedOcr"_s);
    bool requested = false;
    QObject::connect(&reader, &capture::CaptureOcr::setupRequested, &reader, [&] { requested = true; });
    if (button)
        QMetaObject::invokeMethod(button, "clicked");
    const bool saved = reader.needsPackage() && !reader.busy() && requested
        && saveGrab(window, out.filePath(u"ocr-missing.png"_s));
    return finish(window, saved);
}

int runToolbarDrag(const QStringList& args)
{
    using namespace capture;
    const QDir out(valueAfter(args, kCaptureToolsFlag));
    QDir().mkpath(out.path());
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/capture_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    const qreal dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
    const QSize logical(1120, 620);
    const QSize pixels(qRound(logical.width() * dpr), qRound(logical.height() * dpr));
    CaptureScreen screen(0);
    screen.setGeometry(QRect(QPoint(), pixels), dpr, logical);
    QImage image(pixels, QImage::Format_RGB32);
    image.fill(QColor(248, 249, 251));
    {
        QPainter painter(&image);
        painter.scale(dpr, dpr);
        painter.setPen(QColor(35, 40, 45));
        painter.setFont(QFont(QGuiApplication::font().family(), 14));
        painter.drawText(40, 55, u"截图工具条 · 按住左侧三个圆点拖动 / Drag the three dots"_s);
        painter.drawText(180, 190, u"移动工具条不会移动选区，也不会在图片上留下标注。"_s);
    }
    screen.setImage(image);
    CaptureSession session;
    const auto select = [&](QRect logicalRect) {
        session.begin({&screen}, {});
        const QPoint from(qRound(logicalRect.x() * dpr), qRound(logicalRect.y() * dpr));
        const QPoint to(qRound((logicalRect.x() + logicalRect.width()) * dpr) - 1,
                        qRound((logicalRect.y() + logicalRect.height()) * dpr) - 1);
        session.pointerPress(0, from, Qt::LeftButton);
        session.pointerMove(0, to);
        session.pointerRelease(0, to, Qt::LeftButton);
    };
    select(QRect(160, 130, 760, 230));
    QQmlApplicationEngine engine;
    QQmlComponent component(&engine, u"WinShun"_s, u"CaptureOverlay"_s);
    auto* window = qobject_cast<QQuickWindow*>(component.createWithInitialProperties({
        {u"session"_s, QVariant::fromValue(&session)}, {u"screenModel"_s, QVariant::fromValue(&screen)}}));
    if (!window) { say(component.errorString()); return 3; }
    window->resize(logical);
    window->setTitle(u"WinShun toolbar drag test"_s);
    window->setFlags(Qt::Window | Qt::WindowStaysOnTopHint);
    if (args.contains(u"--interactive"_s)) {
        window->setPosition(40, 40);
        window->show();
    } else {
        showOffScreen(window);
    }
    settle(window);
    auto* toolbar = window->findChild<QQuickItem*>(u"captureToolbar"_s);
    auto* bar = window->findChild<QQuickItem*>(u"captureToolbarBar"_s);
    auto* grip = window->findChild<QQuickItem*>(u"captureToolbarGrip"_s);
    if (!toolbar || !bar || !grip) return finish(window, false);
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { say(u"FAIL: "_s + QString::fromLatin1(message)); ok = false; }
    };
    const auto grab = [&](QStringView name) {
        settle(window);
        ok = saveGrab(window, out.filePath(name.toString() + u".png"_s)) && ok;
    };
    grab(u"toolbar-before");
    if (args.contains(u"--interactive"_s)) {
        struct PointerTrace : QObject {
            std::function<void(QMouseEvent*)> report;
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease
                    || (event->type() == QEvent::MouseMove && static_cast<QMouseEvent*>(event)->buttons() != Qt::NoButton))
                    report(static_cast<QMouseEvent*>(event));
                return false;
            }
        } trace;
        trace.report = [&](QMouseEvent* event) {
            const QPointF at = grip->mapToScene(QPointF());
            say(u"native mouse %1: %2,%3; grip %4,%5 %6x%7; toolbar %8,%9"_s.arg(int(event->type()))
                .arg(event->position().x()).arg(event->position().y()).arg(at.x()).arg(at.y())
                .arg(grip->width()).arg(grip->height()).arg(toolbar->x()).arg(toolbar->y()));
        };
        window->installEventFilter(&trace);
        QEventLoop loop;
        QObject::connect(&session, &CaptureSession::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(180000, &loop, &QEventLoop::quit);
        loop.exec();
        grab(u"toolbar-after");
        return finish(window, ok);
    }
    const auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPointF global = window->mapToGlobal(at.toPoint());
        QMouseEvent event(type, at, global, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
        QCoreApplication::processEvents();
    };
    const auto drag = [&](QPointF delta, int steps = 12) {
        const QPointF from = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2));
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= steps; ++i)
            send(QEvent::MouseMove, from + delta * (double(i) / steps), Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, from + delta, Qt::LeftButton, Qt::NoButton);
        settle(window);
    };
    const QRect selection = screen.selection();
    const QPointF original = toolbar->position();
    const QString side = toolbar->property("side").toString();
    drag(QPointF(90, -90));
    check(toolbar->property("manuallyPlaced").toBool(), "grip drag did not move the toolbar");
    check(toolbar->x() > original.x() + 30 && toolbar->y() < original.y() - 30, "toolbar did not follow both drag axes");
    check(screen.selection() == selection && !session.annotation()->hasItems(), "drag reached the selection or annotation canvas");
    check(toolbar->property("side").toString() == side, "drag flipped the toolbar rows");
    const QPointF beforeFast = toolbar->position();
    drag(QPointF(-40, 20), 1);
    check(toolbar->x() < beforeFast.x() - 30 && toolbar->y() > beforeFast.y() + 10,
        "a drag delivered as one move did not reposition the toolbar");
    grab(u"toolbar-moved");
    const QPointF moved = toolbar->position();
    const QPointF firstTool = bar->mapToScene(QPointF(grip->x() + grip->width() + 4 + 16, bar->height() / 2));
    send(QEvent::MouseButtonPress, firstTool, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, firstTool, Qt::LeftButton, Qt::NoButton);
    settle(window);
    check(session.annotation()->tool() == 0 && session.annotation()->panelVisible(), "annotation button stopped working after a drag");
    check(toolbar->position() == moved, "opening the style panel moved the toolbar");
    grab(u"toolbar-with-style");
    grip->forceActiveFocus(Qt::TabFocusReason);
    const QPointF beforeKey = toolbar->position();
    QKeyEvent moveKey(QEvent::KeyPress, Qt::Key_Up, Qt::ShiftModifier);
    QCoreApplication::sendEvent(window, &moveKey);
    check(std::abs(toolbar->y() - beforeKey.y() + 10) <= 1 / dpr && screen.selection() == selection,
        "focused grip arrow keys moved the selection instead of the toolbar");
    QKeyEvent hideKey(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &hideKey);
    check(session.toolbarHidden(), "focused grip swallowed the toolbar shortcut");
    QKeyEvent showKey(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &showKey);
    check(!session.toolbarHidden(), "hiding the focused grip lost keyboard focus");
    drag(QPointF(-2000, -2000));
    check(toolbar->x() == 0 && toolbar->y() == 0, "top-left screen bounds failed");
    drag(QPointF(3000, 2000));
    check(toolbar->x() >= 0 && toolbar->y() >= 0 && toolbar->x() + toolbar->width() <= logical.width() + 1 / dpr
        && toolbar->y() + toolbar->height() <= logical.height() + 1 / dpr, "toolbar or style row left the screen");
    check(screen.selection() == selection && !session.annotation()->hasItems(), "edge drag changed the screenshot");
    grab(u"toolbar-edge");
    session.annotation()->setTool(-1);
    const QPointF kept = toolbar->position();
    session.keyPressed(Qt::Key_Left, Qt::NoModifier);
    settle(window);
    check(toolbar->position() == kept, "adjusting the selection lost manual placement");
    session.keyPressed(Qt::Key_H, Qt::NoModifier);
    session.keyPressed(Qt::Key_H, Qt::NoModifier);
    check(toolbar->position() == kept, "hiding the toolbar lost manual placement");
    select(QRect(160, 450, 760, 150));
    settle(window);
    check(!toolbar->property("manuallyPlaced").toBool(), "new selection did not reset automatic placement");
    check(toolbar->property("side").toString() == u"above", "bottom selection did not place the toolbar above");
    drag(QPointF(40, -70));
    check(toolbar->property("manuallyPlaced").toBool() && toolbar->property("side").toString() == u"above", "dragging above the selection failed");
    grab(u"toolbar-above");
    say(u"toolbar drag checks: %1"_s.arg(ok ? u"passed"_s : u"failed"_s));
    return finish(window, ok);
}

int runCaptureTools(const QStringList& args)
{
    if (args.contains(u"--toolbar-drag"_s)) return runToolbarDrag(args);
    using namespace capture;
    using namespace capture::annot;
    const QDir out(valueAfter(args, kCaptureToolsFlag));
    QDir().mkpath(out.path());
    QImage base(900, 400, QImage::Format_RGB32);
    base.fill(Qt::white);
    {
        QPainter painter(&base);
        painter.setPen(QColor(35, 40, 45));
        QFont font = QGuiApplication::font();
        font.setPixelSize(24);
        painter.setFont(font);
        painter.drawText(40, 48, u"截图标注 · 连续折线、贝塞尔曲线与荧光笔"_s);
        font.setPixelSize(18);
        painter.setFont(font);
        painter.drawText(40, 98, u"矩形荧光笔覆盖一行文字，黑色正文仍然清晰可读。"_s);
    }
    AnnotationModel model;
    model.begin(base, 1, StyleMemory{});
    model.setSelection(base.rect());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(u"previewAnnotation"_s, &model);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import WinShun
        Window {
            id: window
            width: 940; height: 530
            color: Theme.background
            Item {
                x: 20; y: 20; width: 900; height: 400
                AnnotationCanvas { anchors.fill: parent; model: previewAnnotation; sparse: false }
                AnnotateObjects { anchors.fill: parent; annot: previewAnnotation; keyTarget: parent }
            }
            AnnotateStylePanel { x: 20; y: 450; annot: previewAnnotation }
        }
    )", QUrl(u"qrc:/capture-tools-preview.qml"_s));
    auto* window = qobject_cast<QQuickWindow*>(component.create());
    if (!window) {
        say(component.errorString());
        return 3;
    }
    showOffScreen(window);
    const auto tool = [&](Kind kind) {
        for (qsizetype i = 0; i < tools().size(); ++i) {
            if (tools()[i].kind == kind)
                model.setTool(int(i));
        }
    };
    const auto press = [&](QPoint point) {
        if (!model.pressFirst(point, {}))
            model.pressSecond(point, {});
    };
    const auto drag = [&](QPoint from, QPoint to) {
        press(from);
        model.move(to, {});
        model.release(to);
    };
    bool saved = true;
    const auto grab = [&](QStringView name) {
        settle(window);
        pump(150);
        saved = saveGrab(window, out.filePath(name.toString() + u".png"_s)) && saved;
    };
    tool(Kind::Arrow);
    model.setPanel({{u"lineMode"_s, u"bezier"_s}});
    drag({410, 180}, {760, 180});
    grab(u"bezier");
    tool(Kind::Line);
    model.setPanel({{u"lineMode"_s, u"polyline"_s}});
    for (QPoint p : {QPoint(60, 200), QPoint(200, 200), QPoint(200, 300), QPoint(300, 300)}) {
        press(p);
        model.release(p);
    }
    model.finishPath();
    grab(u"polyline");
    tool(Kind::Marker);
    model.setPanel({{u"markerMode"_s, u"rectangle"_s}, {u"color"_s, u"#FFFF00"_s}});
    drag({36, 75}, {755, 108});
    grab(u"marker-rectangle");
    model.setPanel({{u"markerMode"_s, u"line"_s}});
    drag({410, 330}, {760, 330});
    grab(u"marker-line");
    return finish(window, saved);
}

// A real Capture -> selection -> Recorder -> WGC -> MP4 run, without an
// App, global hotkeys, index service, clipboard or the user's settings.
int runRecording(const QStringList& args)
{
    using namespace capture;
    QLocale::setDefault(args.contains(u"--english"_s) ? QLocale(QLocale::English, QLocale::UnitedStates)
                                                   : QLocale(QLocale::Chinese, QLocale::China));
    const QDir out(valueAfter(args, kRecordingFlag));
    QDir().mkpath(out.path());
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/capture_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    QQmlApplicationEngine engine;
    QQmlComponent fixtureComponent(&engine);
    fixtureComponent.setData(R"(
        import QtQuick
        import WinShun
        Window {
            width: 800; height: 460; x: 80; y: 80
            title: "WinShun recording test"
            flags: Qt.Window | Qt.WindowStaysOnTopHint
            color: "#f8f9fb"
            Column {
                x: 24; y: 20; spacing: 14
                Repeater {
                    model: 12
                    Text {
                        required property int index
                        text: "区域录屏测试 · 中文小字 ABC 0123456789 · C++20 / MP4"
                        color: "#1c212a"; font.pixelSize: 15
                    }
                }
            }
            Rectangle {
                width: 64; height: 28; y: 220; color: "#286ec8"
                SequentialAnimation on x {
                    loops: Animation.Infinite
                    NumberAnimation { from: 20; to: 710; duration: 2500 }
                    NumberAnimation { from: 710; to: 20; duration: 2500 }
                }
            }
        }
    )", QUrl(u"qrc:/recording-fixture.qml"_s));
    std::unique_ptr<QQuickWindow> fixture(qobject_cast<QQuickWindow*>(fixtureComponent.create()));
    if (!fixture) { say(fixtureComponent.errorString()); return 3; }
    fixture->show();
    settle(fixture.get());
    const HWND hwnd = reinterpret_cast<HWND>(fixture->winId());
    RECT client {};
    ::GetClientRect(hwnd, &client);
    POINT at {0, 0};
    ::ClientToScreen(hwnd, &at);
    const QRect region(at.x, at.y, client.right, client.bottom);
    const auto quality = args.contains(u"--small-file"_s) ? Recorder::SmallFile
        : args.contains(u"--high-quality"_s) ? Recorder::HighQuality : Recorder::Standard;
    Recorder::Preferences preferences {out.absolutePath(), args.contains(u"--system-audio"_s), false, quality,
        !args.contains(u"--no-cursor"_s), !args.contains(u"--no-countdown"_s), args.contains(u"--clicks"_s)};
    std::unique_ptr<RawClicks> clicks;
    Capture::Callbacks callbacks;
    callbacks.recording.preferences = [&] { return preferences; };
    callbacks.recording.remember = [&](const Recorder::Preferences& value) { preferences = value; };
    callbacks.recording.clicks = [&](std::shared_ptr<recording::ClickStream> input) {
        clicks.reset();
        if (!input) return true;
        clicks = std::make_unique<RawClicks>(std::move(input));
        return clicks->active();
    };
    callbacks.accent = [] { return SystemTheme::accentColor(); };
    Capture capture(&engine, std::move(callbacks));
    capture.trigger();
    pump(500);
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
    if (!session || !screen) { say(u"no capture overlay for recording region"_s); return 3; }
    const QPoint first = region.topLeft() - screen->origin();
    const QPoint last = region.bottomRight() - screen->origin();
    session->pointerPress(screen->index(), first, Qt::LeftButton);
    session->pointerMove(screen->index(), last);
    session->pointerRelease(screen->index(), last, Qt::LeftButton);
    session->act(static_cast<int>(CaptureSession::Action::Record));
    pump(500);
    auto* recorder = capture.findChild<Recorder*>();
    QQuickWindow* panel = nullptr;
    for (QWindow* window : QGuiApplication::topLevelWindows())
        if (window->property("recorder").value<Recorder*>() == recorder)
            panel = qobject_cast<QQuickWindow*>(window);
    if (!recorder || !panel) { say(u"no recording panel"_s); return 3; }
    settle(panel);
    bool saved = saveGrab(panel, out.filePath(u"ready.png"_s)) && saveGrab(fixture.get(), out.filePath(u"source.png"_s));
    if (!args.contains(u"--auto"_s)) {
        QTimer::singleShot(10 * 60 * 1000, qApp, &QCoreApplication::quit);
        return QCoreApplication::exec();
    }
    RECT panelBounds {};
    if (::GetWindowRect(reinterpret_cast<HWND>(panel->winId()), &panelBounds)) {
        const QRect bounds(panelBounds.left, panelBounds.top, panelBounds.right - panelBounds.left, panelBounds.bottom - panelBounds.top);
        recording::ClickStream ignored(bounds.adjusted(-20, -20, 20, 20), quintptr(panel->winId()));
        ignored.push(bounds.center(), false, 1);
        saved = ignored.take().count == 0 && saved;
        ignored.push(bounds.topLeft() - QPoint(10, 10), false, 2);
        saved = ignored.take().count == 1 && saved;
    } else {
        saved = false;
    }
    // Resolve the current QML item each time: this exercises its hit area and
    // binding, including the pause button's change to Resume, not just slots.
    const auto click = [&](const char* name) {
        auto* item = panel->findChild<QQuickItem*>(QString::fromLatin1(name));
        if (!item || !item->isVisible() || !item->isEnabled()) {
            say(u"recording button unavailable: %1"_s.arg(QString::fromLatin1(name)));
            saved = false;
            return;
        }
        const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        const QPointF global = panel->mapToGlobal(at.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(panel, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(panel, &release);
        pump(20);
    };
    saved = panel->height() <= 60 && !panel->property("optionsShown").toBool() && saved;
    say(u"compact ready: %1 x %2"_s.arg(panel->width()).arg(panel->height()));
    const auto panelInsideWorkArea = [&] {
        MONITORINFO monitor {sizeof monitor};
        RECT bounds {};
        const auto handle = reinterpret_cast<HWND>(panel->winId());
        if (!::GetMonitorInfoW(::MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor)
            || !::GetWindowRect(handle, &bounds)) return false;
        const auto& work = monitor.rcWork;
        return bounds.left >= work.left && bounds.top >= work.top && bounds.right <= work.right && bounds.bottom <= work.bottom;
    };
    {
        MONITORINFO monitor {sizeof monitor};
        RECT bounds {};
        const auto handle = reinterpret_cast<HWND>(panel->winId());
        if (::GetMonitorInfoW(::MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor) && ::GetWindowRect(handle, &bounds))
            ::SetWindowPos(handle, nullptr, monitor.rcWork.right - (bounds.right - bounds.left) - 4,
                monitor.rcWork.bottom - (bounds.bottom - bounds.top) - 4, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    const auto key = [&](int code) {
        QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
        QCoreApplication::sendEvent(panel, &press);
        QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier);
        QCoreApplication::sendEvent(panel, &release);
        pump(20);
    };
    auto* optionsButton = panel->findChild<QQuickItem*>(u"recordingOptions"_s);
    if (optionsButton) {
        optionsButton->forceActiveFocus(Qt::TabFocusReason);
        key(Qt::Key_Space);
        settle(panel);
        saved = panel->property("optionsShown").toBool() && saved;
        const bool inside = panelInsideWorkArea();
        say(u"options expanded beside screen edge stay in work area: %1"_s.arg(inside));
        saved = inside && saved;
        saved = saveGrab(panel, out.filePath(u"keyboard-options.png"_s)) && saved;
        auto* divider = panel->findChild<QQuickItem*>(u"recordingOptionsDivider"_s);
        auto* options = panel->findChild<QQuickItem*>(u"recordingOptionsContent"_s);
        if (divider && options) {
            const qreal toolbarBottom = optionsButton->mapToScene(QPointF(0, optionsButton->height())).y();
            const qreal dividerTop = divider->mapToScene(QPointF()).y();
            const qreal optionsTop = options->mapToScene(QPointF()).y();
            const qreal dpr = panel->devicePixelRatio();
            const int gapBefore = qRound((dividerTop - toolbarBottom) * dpr);
            const int gapAfter = qRound((optionsTop - dividerTop - divider->height()) * dpr);
            const bool compact = panel->height() <= 200 && gapBefore >= qRound(8 * dpr) && gapAfter >= qRound(8 * dpr);
            say(u"compact options with spacing on both sides of divider: %1 (%2 x %3; native gaps %4/%5 px)"_s
                .arg(compact).arg(panel->width()).arg(panel->height()).arg(gapBefore).arg(gapAfter));
            saved = compact && saved;
        } else saved = false;
        auto* loader = panel->findChild<QQuickItem*>(u"recordingOptionsLoader"_s);
        auto* viewport = panel->findChild<QQuickItem*>(u"recordingOptionsViewport"_s);
        auto* folderButton = panel->findChild<QQuickItem*>(u"recordingChooseFolder"_s);
        if (loader && viewport && folderButton) {
            const QVariant originalLimit = panel->property("maximumOptionsHeight");
            panel->setProperty("maximumOptionsHeight", 96); // constrain the real viewport, preserving the loader's height binding
            settle(panel);
            folderButton->forceActiveFocus(Qt::TabFocusReason);
            pump(20);
            const auto box = folderButton->mapRectToItem(viewport, folderButton->boundingRect());
            const bool visibleFocus = box.top() >= 0 && box.bottom() <= viewport->height() + 1 && viewport->property("contentY").toReal() > 0;
            say(u"keyboard focus scrolls the final option into a short viewport: %1"_s.arg(visibleFocus));
            saved = visibleFocus && saveGrab(panel, out.filePath(u"options-short-height.png"_s)) && saved;
            panel->setProperty("maximumOptionsHeight", originalLimit);
        } else saved = false;
        key(Qt::Key_Escape);
        settle(panel);
        const bool escaped = !panel->property("optionsShown").toBool() && panel->isVisible();
        say(u"keyboard Space opens options, Escape closes only options: %1"_s.arg(escaped));
        saved = escaped && panel->activeFocusItem() == optionsButton && saved;
    } else saved = false;
    const bool originalAudio = recorder->systemAudio();
    click("recordingSystemAudio");
    saved = recorder->systemAudio() != originalAudio && saved;
    click("recordingSystemAudio");
    saved = recorder->systemAudio() == originalAudio && saved;
    click("recordingGif");
    saved = recorder->format() == Recorder::Gif && saved;
    click("recordingGif"); // the selected radio must remain selected
    saved = panel->findChild<QQuickItem*>(u"recordingGif"_s)->property("checked").toBool() && saved;
    saved = !panel->findChild<QQuickItem*>(u"recordingSystemAudio"_s)->isEnabled() && saved;
    settle(panel);
    saved = saveGrab(panel, out.filePath(u"ready-gif.png"_s)) && saved;
    if (!args.contains(u"--gif"_s)) click("recordingMp4");
    saved = (recorder->format() == (args.contains(u"--gif"_s) ? Recorder::Gif : Recorder::Mp4)) && saved;
    click("recordingOptions");
    settle(panel);
    saved = panel->property("optionsShown").toBool() && saved;
    saved = saveGrab(panel, out.filePath(u"options.png"_s)) && saved;
    click("recordingOptions");
    settle(panel);
    saved = !panel->property("optionsShown").toBool() && panel->height() <= 60 && saved;
    click("recordingOptions");
    settle(panel);
    const bool originalCursor = recorder->cursor(), originalCountdown = recorder->countdown(), originalClicks = recorder->clickHighlights();
    click("recordingCursor");
    saved = recorder->cursor() != originalCursor && saved;
    click("recordingCursor");
    click("recordingCountdown");
    saved = recorder->countdown() != originalCountdown && saved;
    click("recordingCountdown");
    click("recordingClicks");
    const bool originalMouse = recorder->mouseButtons();
    click("recordingMouseButtons");
    saved = recorder->mouseButtons() != originalMouse && saved;
    click("recordingMouseButtons");
    click("recordingKeysAll");
    saved = recorder->keys() == Recorder::KeysAll && saved;
    settle(panel);
    saved = saveGrab(panel, out.filePath(u"options-all-keys.png"_s)) && saved;
    click("recordingKeysShortcuts");
    saved = recorder->keys() == Recorder::KeysShortcuts && saved;
    click("recordingKeysOff");
    saved = recorder->clickHighlights() != originalClicks && saved;
    click("recordingClicks");
    click("recordingSmallFile");
    saved = recorder->quality() == Recorder::SmallFile && saved;
    click("recordingHighQuality");
    saved = recorder->quality() == Recorder::HighQuality && saved;
    click("recordingHighQuality");
    saved = panel->findChild<QQuickItem*>(u"recordingHighQuality"_s)->property("checked").toBool() && saved;
    click(quality == Recorder::SmallFile ? "recordingSmallFile" : quality == Recorder::HighQuality ? "recordingHighQuality" : "recordingStandard");
    saved = recorder->quality() == quality && recorder->cursor() == originalCursor && recorder->countdown() == originalCountdown
        && recorder->clickHighlights() == originalClicks && saved;
    click("recordingOptions");
    settle(panel);
    saved = !panel->property("optionsShown").toBool() && panel->height() <= 60 && saved;
    if (args.contains(u"--ui-only"_s)) {
        recorder->close();
        return saved ? 0 : 4;
    }
    if (args.contains(u"--failure-ui"_s)) {
        const QString blockedPath = out.filePath(u"not-a-folder"_s);
        QFile blocked(blockedPath);
        saved = blocked.open(QIODevice::WriteOnly) && saved;
        blocked.close();
        preferences.folder = blockedPath;
        preferences.countdown = false;
        recorder->close();
        recorder->prepare(region, ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), fixture->devicePixelRatio());
        click("recordingStart");
        settle(panel);
        saved = recorder->stage() == Recorder::Ready && !recorder->error().isEmpty()
            && panel->property("optionsShown").toBool() && saveGrab(panel, out.filePath(u"validation-error.png"_s)) && saved;
        preferences.folder = out.absolutePath();
        recorder->close();
        recorder->prepare(QRect(region.topLeft(), QSize(8, 8)), ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), fixture->devicePixelRatio());
        click("recordingStart");
        QElapsedTimer failureWait;
        failureWait.start();
        while (recorder->stage() != Recorder::Failed && failureWait.elapsed() < 10000) pump(50);
        settle(panel);
        saved = recorder->stage() == Recorder::Failed && saveGrab(panel, out.filePath(u"failed.png"_s)) && saved;
        for (const auto* name : {"recordingPlay", "recordingCopy", "recordingReveal"}) {
            auto* action = panel->findChild<QQuickItem*>(QString::fromLatin1(name));
            saved = action && !action->isEnabled() && saved;
        }
        click("recordingAgain");
        settle(panel);
        saved = recorder->stage() == Recorder::Ready && panel->property("optionsShown").toBool()
            && saveGrab(panel, out.filePath(u"retry.png"_s)) && saved;
        recorder->close();
        return saved ? 0 : 4;
    }
    if (originalCountdown) {
        click("recordingStart");
        saved = recorder->stage() == Recorder::Counting && saved;
        settle(panel);
        saved = saveGrab(panel, out.filePath(u"counting.png"_s)) && saved;
        click("recordingClose");
        saved = !recorder->active() && !panel->isVisible() && saved;
        recorder->prepare(region, ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), fixture->devicePixelRatio());
        settle(panel);
    }
    QEventLoop loop;
    bool started = false;
    bool passed = false;
    int completed = 0;
    QString previousClip;
    qint64 previousBytes = 0;
    QObject::connect(recorder, &Recorder::changed, &loop, [&] {
        if (recorder->stage() == Recorder::Recording && !started) {
            started = true;
            if (completed) {
                QTimer::singleShot(700, &loop, [&] { click("recordingStart"); });
                return;
            }
            QTimer::singleShot(300, &loop, [&] { saved = saveGrab(panel, out.filePath(u"recording.png"_s)) && saved; });
            QTimer::singleShot(1500, &loop, [&] {
                click("recordingPause");
                saved = recorder->stage() == Recorder::Paused && saved;
            });
            QTimer::singleShot(1800, &loop, [&] { saved = saveGrab(panel, out.filePath(u"paused.png"_s)) && saved; });
            QTimer::singleShot(2600, &loop, [&] {
                click("recordingPause");
                saved = recorder->stage() == Recorder::Recording && saved;
            });
            QTimer::singleShot(4200, &loop, [&] { click("recordingStart"); });
        }
        if (recorder->stage() == Recorder::Done || recorder->stage() == Recorder::Failed) {
            QTimer::singleShot(200, &loop, [&] {
                saved = saveGrab(panel, out.filePath(completed ? u"completed-second.png"_s : u"completed.png"_s)) && saved;
                say(u"recording: %1; %2; %3; %4"_s.arg(recorder->status(), recorder->timeText(), recorder->sizeText(), recorder->error()));
                passed = recorder->stage() == Recorder::Done;
                if (passed && !completed) {
                    previousClip = out.filePath(recorder->fileName());
                    previousBytes = QFileInfo(previousClip).size();
                    ++completed;
                    started = false;
                    click("recordingAgain");
                    saved = recorder->stage() == Recorder::Ready && recorder->quality() == quality
                        && recorder->cursor() == originalCursor && recorder->countdown() == originalCountdown
                        && recorder->clickHighlights() == originalClicks && saved;
                    settle(panel);
                    saved = saveGrab(panel, out.filePath(u"again.png"_s)) && saved;
                    if (recorder->countdown()) {
                        click("recordingOptions");
                        settle(panel);
                        click("recordingCountdown");
                        click("recordingOptions");
                    }
                    click("recordingStart"); // a second real recording, with immediate start
                    saved = (recorder->stage() == Recorder::Starting || recorder->stage() == Recorder::Recording) && saved;
                    return;
                }
                saved = passed && QFileInfo(previousClip).size() == previousBytes && previousBytes > 0
                    && previousClip != out.filePath(recorder->fileName()) && saved;
                loop.quit();
            });
        }
    });
    QTimer::singleShot(25000, &loop, &QEventLoop::quit);
    click("recordingStart");
    if (args.contains(u"--skip-countdown"_s) && recorder->stage() == Recorder::Counting) {
        settle(panel);
        click("recordingStart");
    }
    loop.exec();
    passed = passed && completed == 1 && recorder->stage() == Recorder::Done;
    recorder->close();
    return saved && passed ? 0 : 4;
}

} // namespace

bool wanted(const QStringList& args)
{
    return args.contains(kCaptureToolsFlag) || args.contains(kRecordingFlag) || args.contains(kLongCaptureFlag) || args.contains(kOcrFlag) || args.contains(kSettingsFlag);
}

void note(const QString& line)
{
    say(line);
}

void prepare(const QStringList& args)
{
    const QString folder = valueAfter(args, args.contains(kSettingsFlag) ? kSettingsFlag : args.contains(kOcrFlag) ? kOcrFlag : args.contains(kLongCaptureFlag) ? kLongCaptureFlag
                                           : args.contains(kRecordingFlag) ? kRecordingFlag : kCaptureToolsFlag);
    if (!folder.isEmpty()) {
        QDir().mkpath(folder);
        g_logPath = QDir(folder).filePath(u"preview.log"_s);
        QFile::remove(g_logPath);
    }
    say(u"preparing"_s);
    // The scale asked for, exactly: not on top of the screen's own.
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    qputenv("QT_SCALE_FACTOR", valueAfter(args, u"--scale", u"1"_s).toLatin1());
    // --renderer d3d11: the windows as the desktop shows them (for counting edge pixels), not the software renderer's.
    if (valueAfter(args, u"--renderer") == u"d3d11") {
        qputenv("QSG_RHI_BACKEND", "d3d11");
        qunsetenv("QT_QUICK_BACKEND");
    } else {
        qputenv("QT_QUICK_BACKEND", "software"); // renders without a window on screen
    }
    qputenv("QT_QPA_PLATFORM", "windows:fontengine=freetype");
}

int run(const QStringList& args)
{
    const bool recordingMode = args.contains(kRecordingFlag);
    const bool longCaptureMode = args.contains(kLongCaptureFlag);
    const bool ocrMode = args.contains(kOcrFlag);
    const bool settingsMode = args.contains(kSettingsFlag);
    if (valueAfter(args, settingsMode ? kSettingsFlag : ocrMode ? kOcrFlag : longCaptureMode ? kLongCaptureFlag : recordingMode ? kRecordingFlag : kCaptureToolsFlag).isEmpty()) {
        std::fprintf(stderr, "usage: WinShunCapture --preview-capture-tools <folder> [--toolbar-drag] [--scale 1.5] [--dark] [--renderer d3d11]\n"
                             "       WinShunCapture --preview-recording <folder> --renderer d3d11 --auto [--gif] [--clicks]\n"
                             "       WinShunCapture --preview-long-capture <folder> --renderer d3d11 [--auto]\n"
                             "       (with --app-dir <Win顺's folder>, or WS_APP_DIR)\n");
        return 2;
    }
    g_previous = qInstallMessageHandler(countWarnings);
    g_edges = args.contains(u"--edges"_s);
    QGuiApplication::styleHints()->setColorScheme(args.contains(u"--dark"_s) ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    SystemTheme::setBackdropAvailable(false); // plain colours, as a window without Mica
    if (settingsMode)
        return runSettings(args);
    if (ocrMode)
        return runOcr(args);
    if (longCaptureMode)
        return runLongCapture(args) + g_warnings;
    return recordingMode ? runRecording(args) : runCaptureTools(args);
}

} // namespace ws::captureplugin::preview

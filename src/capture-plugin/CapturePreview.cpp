// The screen capture's previews (CapturePreview.h): what WinShun.exe's
// Preview.cpp had of the capture before it became a plugin (GitHub #138).

#include "CapturePreview.h"
#include "CaptureSettings.h"

#include "AnnotationModel.h"
#include "Capture.h"
#include "CaptureSession.h"
#include "CaptureOcr.h"
#include "CaptureEditor.h"
#include "WindowFrame.h"
#include "platform/OcrFiles.h"
#include "RawClicks.h"
#include "Presenter.h"
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
#include <QJsonDocument>
#include <QJsonObject>
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
#include <QSurfaceFormat>
#include <QTimer>
#include <QTemporaryDir>
#include <QTranslator>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <numbers>

using namespace Qt::StringLiterals;

namespace ws::captureplugin::preview {

namespace {

constexpr auto kCaptureToolsFlag = u"--preview-capture-tools";
constexpr auto kSettingsFlag = u"--preview-capture-settings";
constexpr auto kRecordingFlag = u"--preview-recording";
constexpr auto kLongCaptureFlag = u"--preview-long-capture";
constexpr auto kOcrFlag = u"--preview-ocr";
constexpr auto kPresenterFlag = u"--preview-presenter";

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
    // 截图时显示快捷键提示: on by default, off and on again through the switch.
    if (QObject* hints = window->findChild<QObject*>(u"captureKeyHintsSwitch"_s)) {
        bool switched = editor.captureKeyHints() && hints->property("checked").toBool();
        for (const bool enabled : {false, true}) {
            QMetaObject::invokeMethod(hints, "toggled", Q_ARG(bool, enabled));
            settle(window);
            switched = switched && editor.captureKeyHints() == enabled
                && CaptureOptions::load(temporary.filePath(u"capture.ini"_s)).keyHints == enabled
                && hints->property("checked").toBool() == enabled;
        }
        say(u"captureKeyHintsSwitch: %1"_s.arg(switched ? u"ok"_s : u"WRONG"_s));
        ok = ok && switched;
    } else {
        say(u"captureKeyHintsSwitch: not found"_s);
        ok = false;
    }
    // The choices drawn as segmented tabs (ScopeTabs): each one reaches the
    // editor and the isolated file, and the tabs show it as current; each
    // ends on its default again, for the pictures.
    const auto choose = [&](const char* name, int index, const char* property, const QVariant& expected,
                            const std::function<QVariant(const CaptureOptions&)>& stored) {
        QObject* tabs = window->findChild<QObject*>(QString::fromLatin1(name));
        if (!tabs)
            return false;
        QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, index));
        settle(window);
        const bool chosen = editor.property(property) == expected
            && stored(CaptureOptions::load(temporary.filePath(u"capture.ini"_s))) == expected
            && tabs->property("current").toInt() == index;
        say(u"%1 -> %2: %3"_s.arg(QString::fromLatin1(name)).arg(index).arg(chosen ? u"ok"_s : u"WRONG"_s));
        return chosen;
    };
    bool choices = choose("captureDelayTabs", 0, "captureDelay", 3, [](const CaptureOptions& o) { return QVariant(o.delay); })
        && choose("captureDelayTabs", 1, "captureDelay", 5, [](const CaptureOptions& o) { return QVariant(o.delay); })
        && choose("captureHistoryTabs", 2, "captureHistoryCount", 30, [](const CaptureOptions& o) { return QVariant(o.historyCount); })
        && choose("captureHistoryTabs", 1, "captureHistoryCount", 10, [](const CaptureOptions& o) { return QVariant(o.historyCount); })
        && choose("capturePinClickTabs", 2, "capturePinDoubleClick", u"collapse"_s, [](const CaptureOptions& o) { return QVariant(o.pinDoubleClick); })
        && choose("capturePinClickTabs", 0, "capturePinDoubleClick", u"reset"_s, [](const CaptureOptions& o) { return QVariant(o.pinDoubleClick); });
    // The toolbar's buttons: 重做 (the second of the first row) off, then on again.
    // In a Repeater's row: found through the items, not the objects' parents.
    const std::function<QQuickItem*(QQuickItem*, const QString&)> visual = [&](QQuickItem* item, const QString& name) -> QQuickItem* {
        if (item->objectName() == name)
            return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = visual(child, name))
                return found;
        }
        return nullptr;
    };
    if (QObject* tabs = visual(window->contentItem(), u"captureToolbarTabs0"_s)) {
        const QString before = editor.captureToolbarHidden();
        QMetaObject::invokeMethod(tabs, "toggled", Q_ARG(int, 1), Q_ARG(bool, false));
        settle(window);
        const bool hidden = editor.captureToolbarHidden().split(u',').contains(u"redo"_s)
            && CaptureOptions::load(temporary.filePath(u"capture.ini"_s)).toolbarHidden.split(u',').contains(u"redo"_s)
            && !tabs->property("checkedIndices").toList().contains(1);
        QMetaObject::invokeMethod(tabs, "toggled", Q_ARG(int, 1), Q_ARG(bool, true));
        settle(window);
        const bool shown = editor.captureToolbarHidden() == before && tabs->property("checkedIndices").toList().contains(1);
        say(u"captureToolbarTabs0 redo off/on: %1"_s.arg(hidden && shown ? u"ok"_s : u"WRONG"_s));
        choices = choices && hidden && shown;
    } else {
        choices = false;
    }
    if (QObject* tabs = visual(window->contentItem(), u"captureToolbarTabs4"_s)) {
        QObject* options = window->findChild<QObject*>(u"captureOptions"_s);
        const int previousGroup = options ? options->property("currentGroup").toInt() : 0;
        if (options)
            options->setProperty("currentGroup", 6);
        const QString before = editor.captureToolbarHidden();
        QMetaObject::invokeMethod(tabs, "toggled", Q_ARG(int, 0), Q_ARG(bool, false));
        settle(window);
        const bool hidden = editor.captureToolbarHidden().split(u',').contains(u"help"_s)
            && CaptureOptions::load(temporary.filePath(u"capture.ini"_s)).toolbarHidden.split(u',').contains(u"help"_s)
            && tabs->property("checkedIndices").toList().isEmpty();
        const bool saved = saveGrab(window, out.filePath(u"capture-toolbar-help-hidden.png"_s));
        QMetaObject::invokeMethod(tabs, "toggled", Q_ARG(int, 0), Q_ARG(bool, true));
        settle(window);
        const bool shown = editor.captureToolbarHidden() == before && tabs->property("checkedIndices").toList().contains(0);
        say(u"captureToolbarTabs4 help off/on: %1"_s.arg(hidden && shown ? u"ok"_s : u"WRONG"_s));
        choices = choices && hidden && shown && saved;
        if (options)
            options->setProperty("currentGroup", previousGroup);
    } else {
        choices = false;
    }
    if (!choices)
        qWarning("capture settings: a segmented choice did not update the editor and isolated file");
    ok = ok && choices;
    // 按键 (#187): keys pressed in the key boxes. A bare letter for 保存 is
    // refused, with why under the row, and nothing stored; Ctrl+Shift+S is
    // taken and stored; an arrow for 关闭贴图 is refused. Pictured so (the
    // 恢复默认 and the words), then put back.
    {
        using capture::KeyAction;
        QObject* options = window->findChild<QObject*>(u"captureOptions"_s);
        QQuickItem* save = visual(window->contentItem(), u"captureKeySave"_s);
        QQuickItem* close = visual(window->contentItem(), u"captureKeyPinClose"_s);
        const auto rowOf = [](QQuickItem* item) {
            while (item && item->metaObject()->indexOfProperty("keyId") < 0)
                item = item->parentItem();
            return item;
        };
        const auto press = [&](QQuickItem* box, int key, Qt::KeyboardModifiers mods) {
            QMetaObject::invokeMethod(box, "keyPressed", Q_ARG(int, key), Q_ARG(int, int(mods)), Q_ARG(int, 0));
            settle(window);
            return rowOf(box)->property("error").toString();
        };
        const auto stored = [&](KeyAction action) {
            return CaptureOptions::load(temporary.filePath(u"capture.ini"_s)).keys.key(action);
        };
        bool keys = options && save && close && rowOf(save) && rowOf(close);
        if (keys) {
            const int previousGroup = options->property("currentGroup").toInt();
            options->setProperty("currentGroup", 1);
            settle(window);
            const QString refused = press(save, Qt::Key_P, Qt::NoModifier);
            keys = !refused.isEmpty() && stored(KeyAction::Save) == QKeyCombination(Qt::ControlModifier, Qt::Key_S);
            const QString taken = press(save, Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier);
            keys = keys && taken.isEmpty() && stored(KeyAction::Save) == QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_S)
                && editor.options().keys.text(KeyAction::Save) == u"Ctrl+Shift+S"_s;
            const QString arrow = press(close, Qt::Key_Left, Qt::NoModifier);
            keys = keys && !arrow.isEmpty() && stored(KeyAction::PinClose) == QKeyCombination(Qt::Key_Escape);
            say(u"key boxes: refused \"%1\", \"%2\"; taken: %3"_s.arg(refused, arrow).arg(keys ? u"ok"_s : u"WRONG"_s));
            const int width = (std::max)(560, window->minimumWidth());
            auto* flick = window->findChild<QQuickItem*>(u"captureSettingsFlick"_s);
            window->resize(width, 640);
            settle(window);
            if (flick)
                window->resize(width, static_cast<int>(std::ceil(window->height() - flick->height() + flick->property("contentHeight").toReal())));
            settle(window);
            keys = saveGrab(window, out.filePath(u"capture-keys-states.png"_s)) && keys;
            editor.resetCaptureKey(u"Save"_s);
            press(close, Qt::Key_Escape, Qt::NoModifier);
            settle(window);
            keys = keys && stored(KeyAction::Save) == QKeyCombination(Qt::ControlModifier, Qt::Key_S)
                && rowOf(close)->property("error").toString().isEmpty();
            options->setProperty("currentGroup", previousGroup);
            window->resize(560, 640);
            settle(window);
        }
        if (!keys)
            qWarning("capture settings: the key boxes did not take, refuse or store keys as they should");
        ok = ok && keys;
    }
    if (args.contains(u"--pin-image"_s)) {
        if (QObject* options = window->findChild<QObject*>(u"captureOptions"_s))
            options->setProperty("currentGroup", 4);
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
                row->property("description").toString()).arg(ok));
        }
    }
    // --groups: every tab, each in a window tall enough for all of it, as
    // capture-settings-<n>.png, in a window as wide as it opens (720) or
    // --width (560 is its narrowest).
    if (args.contains(u"--groups"_s)) {
        const int asked = valueAfter(args, u"--width", u"720"_s).toInt();
        QObject* options = window->findChild<QObject*>(u"captureOptions"_s);
        auto* flick = window->findChild<QQuickItem*>(u"captureSettingsFlick"_s);
        bool all = options && flick;
        const int count = options ? options->property("groups").toList().size() : 0;
        for (int group = 0; all && group < count; ++group) {
            options->setProperty("currentGroup", group);
            const int width = (std::max)(asked, window->minimumWidth()); // as the desktop keeps it
            window->resize(width, 640);
            settle(window);
            const qreal chrome = window->height() - flick->height();
            window->resize(width, static_cast<int>(std::ceil(chrome + flick->property("contentHeight").toReal())));
            settle(window);
            all = saveGrab(window, out.filePath(u"capture-settings-%1.png"_s.arg(group))) && all;
        }
        return finish(window, ok && all);
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

// Paced input in a visible production overlay. Unlike the snapshot preview,
// this lets the normal event loop and D3D11 presentation run between samples.
int runBrushPerformance(QQuickWindow* window, capture::CaptureSession& session, const QDir& out,
    const std::function<void(QEvent::Type, QPointF, Qt::MouseButton, Qt::MouseButtons)>& send, bool busy)
{
    using namespace capture;
    auto* model = session.annotation();
    const qreal dpr = window->devicePixelRatio();
    const auto tool = [&](annot::Kind kind) {
        for (qsizetype i = 0; i < annot::tools().size(); ++i) {
            if (annot::tools()[i].kind == kind) model->setTool(int(i));
        }
    };
    if (busy) {
        const auto box = [&](QPoint from, QPoint to) {
            from = QPoint(qRound(from.x() * dpr), qRound(from.y() * dpr));
            to = QPoint(qRound(to.x() * dpr), qRound(to.y() * dpr));
            if (!model->pressFirst(from, {})) model->pressSecond(from, {});
            model->move(to, {});
            model->release(to);
        };
        tool(annot::Kind::Rect);
        for (int i = 0; i < 50; ++i) {
            const QPoint at(180 + (i % 10) * 70, 150 + (i / 10) * 65);
            box(at, at + QPoint(42, 28));
        }
        tool(annot::Kind::Redact);
        for (int i = 0; i < 5; ++i)
            box(QPoint(200 + i * 130, 425), QPoint(290 + i * 130, 470));
        tool(annot::Kind::Pen);
    }
    settle(window);
    const auto rest = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    struct Usage { double cpuMs = 0; qint64 privateBytes = 0; qint64 workingBytes = 0; bool valid = false; };
    const auto usage = [] {
        Usage u;
        FILETIME created {}, exited {}, kernel {}, user {};
        PROCESS_MEMORY_COUNTERS_EX memory {};
        memory.cb = sizeof(memory);
        u.valid = ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)
            && ::K32GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
        const auto ticks = [](FILETIME value) { return (quint64(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
        u.cpuMs = (ticks(kernel) + ticks(user)) / 10000.0;
        u.privateBytes = qint64(memory.PrivateUsage);
        u.workingBytes = qint64(memory.WorkingSetSize);
        return u;
    };
    rest(700); // settle toolbar feedback before measuring idle CPU
    const Usage idleStart = usage();
    QElapsedTimer idleClock;
    idleClock.start();
    rest(1500);
    const Usage before = usage();
    const double idleCpu = (before.cpuMs - idleStart.cpuMs) / idleClock.elapsed() * 100;
    Usage peak = before;
    QTimer memoryTimer;
    memoryTimer.setInterval(100);
    QObject::connect(&memoryTimer, &QTimer::timeout, &memoryTimer, [&] {
        const Usage u = usage();
        peak.privateBytes = std::max(peak.privateBytes, u.privateBytes);
        peak.workingBytes = std::max(peak.workingBytes, u.workingBytes);
    });
    QElapsedTimer clock;
    clock.start();
    std::mutex frameMutex;
    QList<qint64> frames;
    bool recordingFrames = true;
    frames.reserve(5000);
    const auto connection = QObject::connect(window, &QQuickWindow::frameSwapped, window, [&] {
        const qint64 now = clock.nsecsElapsed();
        const std::lock_guard guard(frameMutex);
        if (recordingFrames) frames.append(now);
    }, Qt::DirectConnection);
    QEventLoop loop;
    QTimer input;
    input.setTimerType(Qt::PreciseTimer);
    input.setInterval(2); // up to 500 Hz; actual delivered samples are recorded
    int samples = 0;
    QPointF last(520, 310);
    send(QEvent::MouseButtonPress, last, Qt::LeftButton, Qt::LeftButton);
    QObject::connect(&input, &QTimer::timeout, &loop, [&] {
        const double t = clock.nsecsElapsed() / 1e9;
        if (t >= 12.0) { loop.quit(); return; }
        last = QPointF(520 + 300 * std::sin(t * std::numbers::pi / 4),
            310 + 105 * std::sin(t * std::numbers::pi / 2) + 1.5 * std::sin(t * 150 * std::numbers::pi));
        send(QEvent::MouseMove, last, Qt::NoButton, Qt::LeftButton);
        ++samples;
    });
    memoryTimer.start();
    input.start();
    loop.exec();
    input.stop();
    memoryTimer.stop();
    {
        const std::lock_guard guard(frameMutex);
        recordingFrames = false; // an in-flight render callback can no longer append
    }
    QObject::disconnect(connection);
    const double elapsed = clock.nsecsElapsed() / 1e6;
    const Usage after = usage();
    send(QEvent::MouseButtonRelease, last, Qt::LeftButton, Qt::NoButton);
    QList<double> intervals;
    {
        const std::lock_guard guard(frameMutex);
        for (qsizetype i = 1; i < frames.size(); ++i)
            intervals.append((frames[i] - frames[i - 1]) / 1e6);
    }
    std::sort(intervals.begin(), intervals.end());
    const auto quantile = [&](qreal q) { return intervals.isEmpty() ? 0.0 : intervals[qsizetype(q * (intervals.size() - 1))]; };
    const int points = model->document().items.isEmpty() ? 0 : int(model->document().items.last().points.size());
    QJsonObject result {{u"durationMs"_s, elapsed}, {u"samples"_s, samples}, {u"storedPoints"_s, points},
        {u"busy"_s, busy}, {u"dpr"_s, dpr}, {u"refreshHz"_s, window->screen()->refreshRate()},
        {u"frames"_s, frames.size()}, {u"frameP50Ms"_s, quantile(0.5)}, {u"frameP95Ms"_s, quantile(0.95)},
        {u"frameP99Ms"_s, quantile(0.99)}, {u"frameMaxMs"_s, quantile(1)},
        {u"framesOver33Ms"_s, std::count_if(intervals.cbegin(), intervals.cend(), [](double v) { return v > 33.34; })},
        {u"cpuOneCorePercent"_s, (after.cpuMs - before.cpuMs) / elapsed * 100},
        {u"idleCpuOneCorePercent"_s, idleCpu}, {u"privateBeforeMiB"_s, before.privateBytes / 1048576.0},
        {u"privatePeakMiB"_s, std::max(peak.privateBytes, after.privateBytes) / 1048576.0},
        {u"workingPeakMiB"_s, std::max(peak.workingBytes, after.workingBytes) / 1048576.0},
        {u"compositorCacheMiB"_s, model->compositor().cacheBytes() / 1048576.0}};
    rest(700);
    const Usage idleAfterStart = usage();
    idleClock.restart();
    rest(1500);
    result[u"idleAfterCpuOneCorePercent"_s] = (usage().cpuMs - idleAfterStart.cpuMs) / idleClock.elapsed() * 100;
    QFile metrics(out.filePath(u"performance.json"_s));
    const bool measured = before.valid && after.valid && intervals.size() >= 120 && samples >= 1000;
    const bool saved = metrics.open(QIODevice::WriteOnly) && metrics.write(QJsonDocument(result).toJson()) > 0;
    say(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    model->setTool(0);
    send(QEvent::MouseMove, QPointF(30, 30), Qt::NoButton, Qt::NoButton);
    settle(window);
    const bool picture = saveGrab(window, out.filePath(u"performance-overlay.png"_s));
    return finish(window, measured && saved && picture);
}

int runToolbarDrag(const QStringList& args)
{
    using namespace capture;
    const bool performance = args.contains(u"--brush-performance"_s);
    const bool brush = args.contains(u"--brush"_s) || performance;
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
        painter.drawText(40, 55, brush ? u"截图画笔 · 实时防抖 / Stabilized freehand"_s
                                     : u"截图工具条 · 按住左侧三个圆点拖动 / Drag the three dots"_s);
        if (!brush)
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
    select(QRect(160, 130, 760, brush ? 370 : 230));
    QQmlApplicationEngine engine;
    QQmlComponent component(&engine, u"WinShun"_s, u"CaptureOverlay"_s);
    auto* window = qobject_cast<QQuickWindow*>(component.createWithInitialProperties({
        {u"session"_s, QVariant::fromValue(&session)}, {u"screenModel"_s, QVariant::fromValue(&screen)}}));
    if (!window) { say(component.errorString()); return 3; }
    window->resize(logical);
    window->setTitle(brush ? u"WinShun brush test"_s : u"WinShun toolbar drag test"_s);
    window->setFlags(Qt::Window | Qt::WindowStaysOnTopHint);
    if (args.contains(u"--interactive"_s) || performance) {
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
    if (brush) {
        for (qsizetype i = 0; i < annot::tools().size(); ++i) {
            if (annot::tools()[i].kind == annot::Kind::Pen)
                session.annotation()->setTool(int(i));
        }
        session.annotation()->setPanel({{u"color"_s, u"#8E24AA"_s}, {u"width"_s, 4}});
    }
    grab(u"toolbar-before");
    // Two cards: the common row next to the selection (on top while the toolbar lies below), the other
    // beyond it; the box is both and the gap; left edges together; the second no wider than the first.
    auto* row1 = window->findChild<QQuickItem*>(u"captureToolbarRow1"_s);
    auto* row2 = window->findChild<QQuickItem*>(u"captureToolbarRow2"_s);
    auto* help = window->findChild<QQuickItem*>(u"captureToolbarHelp"_s);
    check(help && help->isVisible(), "the screenshot toolbar help button is missing");
    if (help) {
        QQuickItem* helpRow = help->parentItem();
        const qreal before = helpRow->width();
        bar->setProperty("keysOpen", true);
        settle(window);
        check(bar->property("keysOpen").toBool(), "the full key list did not open");
        session.setHiddenButtons({u"help"_s});
        settle(window);
        check(!help->isVisible() && !bar->property("keysOpen").toBool(), "hiding help left its button or list visible");
        check(helpRow->width() < before, "hiding help kept its separator or space");
        grab(u"toolbar-help-hidden");
        session.setHiddenButtons({});
        settle(window);
        check(help->isVisible() && std::abs(helpRow->width() - before) < 1 / dpr, "restoring help did not restore the toolbar width");
    }
    const auto checkRows = [&](bool above) {
        if (!row1 || !row2 || !row2->isVisible()) { check(false, "the overlay toolbar is not two rows"); return; }
        check(row1->x() == row2->x() && row2->width() <= row1->width(), "rows: left edges or widths");
        check(bar->height() == row1->height() + 4 + row2->height() && bar->width() == row1->width(), "rows: box is not both cards");
        check(above ? row2->y() + row2->height() + 4 == row1->y() : row1->y() + row1->height() + 4 == row2->y(),
            above ? "rows: the common row is not next to the selection (above)" : "rows: the common row is not next to the selection (below)");
        check(row1->mapToScene(QPointF()).y() * 1 == std::round(row1->mapToScene(QPointF()).y() * dpr) / dpr, "rows: first card not on a device pixel");
    };
    checkRows(false);
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
        QTimer::singleShot(brush ? 45000 : 180000, &loop, &QEventLoop::quit);
        loop.exec();
        if (brush) {
            check(session.annotation()->itemCount() >= 2, "native mouse input did not create two strokes");
            ok = session.annotation()->composed().save(out.filePath(u"brush-native-export.png"_s)) && ok;
            say(u"native mouse brush objects: %1"_s.arg(session.annotation()->itemCount()));
        }
        grab(brush ? u"brush-native" : u"toolbar-after");
        return finish(window, ok);
    }
    const auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPointF global = window->mapToGlobal(at.toPoint());
        QMouseEvent event(type, at, global, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
        if (!performance) QCoreApplication::processEvents();
    };
    if (brush) {
        if (performance)
            return runBrushPerformance(window, session, out, send, args.contains(u"--busy"_s));
        // Real Qt mouse events -> production CaptureOverlay MouseArea ->
        // CaptureSession -> AnnotationModel -> canvas, using a synthetic base.
        auto* model = session.annotation();
        QList<QList<QPointF>> rawStrokes;
        QList<qint64> eventTimes;
        for (int row = 0; row < 3; ++row) {
            const QPointF from(210, 200 + row * 105);
            QList<QPointF> raw {annot::pixelCentre(from * dpr)};
            send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= 620; ++i) {
                const qreal curve = row == 0 ? i * 0.06 : row == 1 ? 26 * std::sin(i / 95.0) : 0;
                const QPointF at = from + QPointF(i, curve + ((i / 2) % 2 ? 2 : -2));
                raw.append(annot::pixelCentre(at * dpr));
                QElapsedTimer timer;
                timer.start();
                send(QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton);
                eventTimes.append(timer.nsecsElapsed());
            }
            const QPointF end = from + QPointF(621, row == 0 ? 621 * 0.06 : row == 1 ? 26 * std::sin(621 / 95.0) : 0);
            raw.append(annot::pixelCentre(end * dpr));
            rawStrokes.append(raw);
            send(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
            const auto draft = model->document().items;
            send(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
            check(model->document().items == draft, "releasing changed the live stroke");
        }
        check(model->itemCount() == 3, "QML pointer routing did not create three strokes");
        model->setTool(0); // let go of the pen's selection outline
        send(QEvent::MouseMove, QPointF(30, 30), Qt::NoButton, Qt::NoButton);
        grab(u"brush-overlay");
        const QImage exported = model->composed();
        ok = exported.save(out.filePath(u"brush-export.png"_s)) && ok;
        // Same events through the previous brush's release simplification,
        // with both images drawn by the production compositor.
        annot::Document previous = model->document();
        for (qsizetype i = 0; i < previous.items.size() && i < rawStrokes.size(); ++i)
            previous.items[i].points = annot::simplifyStroke(rawStrokes[i]);
        annot::Compositor oldCompositor;
        const QImage oldImage = oldCompositor.exportImage(previous, model->base(), model->sampleBounds());
        const int caption = qRound(50 * dpr);
        QImage comparison(QSize(exported.width() * 2, exported.height() + caption), QImage::Format_RGB32);
        comparison.fill(Qt::white);
        {
            QPainter painter(&comparison);
            QFont font = QGuiApplication::font();
            font.setPixelSize(qRound(20 * dpr));
            painter.setFont(font);
            painter.setPen(QColor(35, 40, 45));
            painter.drawText(qRound(20 * dpr), qRound(32 * dpr), u"改进前 · 同一段输入"_s);
            painter.drawText(exported.width() + qRound(20 * dpr), qRound(32 * dpr), u"改进后 · 实时防抖"_s);
            painter.drawImage(0, caption, oldImage);
            painter.drawImage(exported.width(), caption, exported);
        }
        ok = comparison.save(out.filePath(u"brush-comparison.png"_s)) && ok;
        std::sort(eventTimes.begin(), eventTimes.end());
        say(u"QML move event handling p95: %1 ms (%2 events; not a display frame-rate measurement)"_s
            .arg(eventTimes[eventTimes.size() * 95 / 100] / 1e6, 0, 'f', 3).arg(eventTimes.size()));
        const QImage visible = window->grabWindow();
        const QRect area(qRound(190 * dpr), qRound(170 * dpr), qRound(660 * dpr), qRound(300 * dpr));
        QImage expected;
        annot::Compositor fresh;
        fresh.render(model->document(), model->base(), model->sampleBounds(), area, &expected);
        const QImage got = visible.copy(area).convertToFormat(QImage::Format_RGB32);
        check(got == expected, "visible brush pixels differ from the export compositor");
        say(u"brush QML input, release stability and visible/export pixel checks: %1"_s.arg(ok ? u"passed"_s : u"failed"_s));
        return finish(window, ok);
    }
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
    const QPointF firstTool = grip->mapToScene(QPointF(grip->width() + 4 + 16, grip->height() / 2));
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
    checkRows(true);
    grab(u"toolbar-above");
    {
        // The editing window's toolbar: the same item without a session, one card with everything.
        QQmlComponent barComponent(&engine, u"WinShun"_s, u"AnnotateToolbar"_s);
        QQuickWindow editorWindow;
        editorWindow.resize(QSize(1000, 80));
        editorWindow.setColor(QColor(args.contains(u"--dark"_s) ? QColor(32, 32, 32) : QColor(243, 243, 243)));
        auto* editorBar = qobject_cast<QQuickItem*>(barComponent.createWithInitialProperties({
            {u"annot"_s, QVariant::fromValue(session.annotation())}, {u"editor"_s, true}, {u"dpr"_s, dpr}}));
        if (!editorBar) { say(barComponent.errorString()); return finish(window, false); }
        editorBar->setParentItem(editorWindow.contentItem());
        editorBar->setPosition(QPointF(12, 12));
        showOffScreen(&editorWindow);
        settle(&editorWindow);
        auto* editorRow2 = editorBar->findChild<QQuickItem*>(u"captureToolbarRow2"_s);
        check(editorRow2 && !editorRow2->isVisible(), "the editor toolbar has a second row");
        check(editorBar->height() == 44, "the editor toolbar is not one 44 px card");
        ok = saveGrab(&editorWindow, out.filePath(u"toolbar-editor.png"_s)) && ok;
        delete editorBar;
    }
    say(u"toolbar drag checks: %1"_s.arg(ok ? u"passed"_s : u"failed"_s));
    return finish(window, ok);
}

int runSelectionKeys(const QStringList& args)
{
    using namespace capture;
    const QDir out(valueAfter(args, kCaptureToolsFlag));
    QDir().mkpath(out.path());
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/capture_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    QQmlApplicationEngine engine;
    QQmlComponent fixtureComponent(&engine);
    fixtureComponent.setData(R"(
        import QtQuick
        Window {
            property int generation: 0
            color: generation % 2 ? "#e6ecff" : "#f4f6f8"
            flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
            Text {
                x: 80; y: 48
                text: "截图精调验证 / Capture keyboard verification · " + generation
                color: "#253047"; font.pixelSize: 24
            }
        }
    )", QUrl(u"qrc:/selection-keys-fixture.qml"_s));
    std::unique_ptr<QQuickWindow> fixture(qobject_cast<QQuickWindow*>(fixtureComponent.create()));
    if (!fixture) { say(fixtureComponent.errorString()); return 3; }
    fixture->showFullScreen();
    settle(fixture.get());
    QImage copied;
    int copies = 0;
    Capture::Callbacks callbacks;
    callbacks.copy = [&](const QImage& image, const QByteArray&, const QString&) { copied = image; ++copies; };
    Capture capture(&engine, std::move(callbacks));
    QQuickWindow* overlay = nullptr;
    CaptureSession* session = nullptr;
    CaptureScreen* screen = nullptr;
    bool ok = true;
    const auto check = [&](bool passed, const char* what) {
        say(u"%1: %2"_s.arg(passed ? u"PASS"_s : u"FAIL"_s, QString::fromUtf8(what)));
        ok = passed && ok;
    };
    const auto waitFor = [&](const std::function<bool()>& ready) {
        QElapsedTimer clock;
        clock.start();
        while (!ready() && clock.elapsed() < 8000)
            pump(20);
        return ready();
    };
    const auto begin = [&] {
        capture.trigger();
        return waitFor([&] {
            for (QWindow* window : QGuiApplication::topLevelWindows()) {
                auto* model = window->property("screenModel").value<CaptureScreen*>();
                auto* state = window->property("session").value<CaptureSession*>();
                if (model && state && model->hasImage() && state->active() && window->isVisible()
                    && window->isActive() && window->screen() == fixture->screen()) {
                    overlay = qobject_cast<QQuickWindow*>(window);
                    session = state;
                    screen = model;
                    return overlay && overlay->activeFocusItem();
                }
            }
            return false;
        });
    };
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent press(QEvent::KeyPress, code, modifiers);
        QCoreApplication::sendEvent(overlay, &press);
        QKeyEvent release(QEvent::KeyRelease, code, modifiers);
        QCoreApplication::sendEvent(overlay, &release);
        pump(20);
    };
    const auto mouse = [&](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPointF logical(at.x() / screen->dpr(), at.y() / screen->dpr());
        QMouseEvent event(type, logical, overlay->mapToGlobal(logical.toPoint()), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(overlay, &event);
        pump(20);
    };
    // The production Capture controller freezes the real fixture with Grabber.
    // Copy is intercepted here, so these runs never write the user's clipboard.
    for (int i = 0; i < 12; ++i) {
        fixture->setProperty("generation", i);
        settle(fixture.get());
        if (!begin()) { check(false, "real capture overlay appeared"); return 4; }
        if (i == 0) {
            key(Qt::Key_R);
            check(!session->hasSelection() && !screen->notice().isEmpty(), "R with no history gives feedback");
        }
        const QRect area(180 + i * 4, 200, 320, 160);
        mouse(QEvent::MouseButtonPress, area.topLeft(), Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, area.bottomRight(), Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, area.bottomRight(), Qt::LeftButton, Qt::NoButton);
        check(session->selectionRect() == area, "real QML mouse routing selected exact physical pixels");
        const QImage expected = screen->image().copy(area);
        key(Qt::Key_Return);
        check(waitFor([&] { return !capture.busy(); }) && copies == i + 1 && copied == expected,
            "Enter exported the frozen pixels through Capture");
    }
    const QRgb previousColor = copied.pixel(0, 0);
    fixture->setProperty("generation", 12);
    settle(fixture.get());
    if (!begin()) { check(false, "capture reopened for region recall"); return 4; }
    check(session->selectionHistoryCount() == 10, "only ten unique completed regions retained");
    for (int i = 0; i < 10; ++i) {
        key(Qt::Key_R);
        check(session->selectionRect() == QRect(180 + (11 - i) * 4, 200, 320, 160), "R cycles regions newest first");
    }
    key(Qt::Key_R);
    check(session->recalledSelection() == 0, "R wraps to the newest region");
    key(Qt::Key_R, Qt::ShiftModifier);
    check(session->recalledSelection() == 9, "Shift+R cycles in reverse");
    key(Qt::Key_R);
    const QRect recalled = session->selectionRect();
    QList<QPoint> pointerRequests;
    const QMetaObject::Connection pointerWatch = QObject::connect(session, &CaptureSession::cursorMoveRequested, session,
        [&](const QPoint& global) { pointerRequests.append(global); });
    key(Qt::Key_D);
    key(Qt::Key_S, Qt::ShiftModifier);
    QObject::disconnect(pointerWatch);
    check(pointerRequests.size() == 2 && session->selectionRect() == recalled && session->annotation()->tool() < 0,
        "WASD requested pointer moves and left the area alone (distances are covered by tst_capture)");
    key(Qt::Key_Left);
    key(Qt::Key_Up);
    check(session->selectionRect() == recalled.translated(-1, -1), "arrow keys moved the area by one physical pixel");
    key(Qt::Key_Right, Qt::ShiftModifier);
    key(Qt::Key_Down, Qt::ShiftModifier);
    const QRect adjusted(recalled.topLeft() - QPoint(1, 1), recalled.size() + QSize(1, 1));
    check(session->selectionRect() == adjusted && session->annotation()->tool() < 0,
        "Shift+arrows resized by one pixel without selecting a tool or starting a countdown");
    const QImage expected = screen->image().copy(adjusted);
    check(expected.pixel(0, 0) != previousColor, "R uses the newly frozen picture");
    settle(overlay);
    check(saveGrab(overlay, out.filePath(u"selection-keyboard.png"_s)), "saved actual overlay for visual inspection");
    key(Qt::Key_Return);
    check(waitFor([&] { return !capture.busy(); }) && copies == 13 && copied == expected,
        "recalled and adjusted region exports exact current pixels");
    check(copied.save(out.filePath(u"selection-export.png"_s)), "saved exported pixels");
    if (!begin()) { check(false, "capture reopened for text input"); return 4; }
    key(Qt::Key_R);
    const QRect beforeText = session->selectionRect();
    key(Qt::Key_1);
    check(session->annotation()->tool() == 0, "1 still selects the rectangle tool");
    key(Qt::Key_1);
    key(Qt::Key_3);
    check(session->annotation()->tool() == 2, "3 still selects the arrow tool");
    key(Qt::Key_3);
    key(Qt::Key_T);
    mouse(QEvent::MouseButtonPress, beforeText.center(), Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, beforeText.center(), Qt::LeftButton, Qt::NoButton);
    check(session->annotation()->editingText(), "text annotation accepts keyboard focus");
    for (const QChar letter : u"wasdr"_s) {
        QKeyEvent press(QEvent::KeyPress, letter.toUpper().unicode(), Qt::NoModifier, QString(letter));
        QCoreApplication::sendEvent(overlay, &press);
        QKeyEvent release(QEvent::KeyRelease, letter.toUpper().unicode(), Qt::NoModifier);
        QCoreApplication::sendEvent(overlay, &release);
        pump(20);
    }
    check(session->annotation()->editing().value(u"text"_s).toString() == u"wasdr"
        && session->selectionRect() == beforeText, "typing WASDR edits text without moving or restoring the area");
    key(Qt::Key_Return, Qt::ControlModifier);
    check(copies == 13 && session->hasAnnotations(), "Ctrl+Enter commits text without copying the screenshot");
    key(Qt::Key_R);
    check(session->hasAnnotations() && session->selectionRect() == beforeText, "R protects existing annotations");
    capture.trigger();
    check(waitFor([&] { return !capture.busy(); }), "cancel closes only the isolated capture");
    fixture->hide();
    say(u"selection keyboard checks: %1; %2 warning(s)"_s.arg(ok ? u"passed"_s : u"failed"_s).arg(g_warnings));
    return ok && g_warnings == 0 ? 0 : 4;
}

// --key-hints: the shortcut hints (CaptureKeyHints.qml) in the real overlay
// over a made-up desktop, through the overlay's own mouse routing, on the
// real clock (KeyHintPolicy): none at first, then by themselves; a press
// takes them away; with the pointer near, on its button (不再显示, 一直显示
// and ?), after a selection and a pause, with a tool, a recalled region, a
// selection over the left corner and one over both; a delayed capture;
// keyhints-*.png.
int runKeyHints(const QStringList& args)
{
    using namespace capture;
    const QDir out(valueAfter(args, kCaptureToolsFlag));
    QDir().mkpath(out.path());
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/capture_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    const qreal dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
    const QSize logical(1280, 800);
    const QSize pixels(qRound(logical.width() * dpr), qRound(logical.height() * dpr));
    const auto phys = [dpr](qreal x, qreal y) { return QPoint(qRound(x * dpr), qRound(y * dpr)); };
    CaptureScreen screen(0);
    screen.setGeometry(QRect(QPoint(), pixels), dpr, logical);
    QImage image(pixels, QImage::Format_RGB32);
    {
        QPainter painter(&image);
        painter.scale(dpr, dpr);
        QLinearGradient wallpaper(0, 0, logical.width(), logical.height());
        wallpaper.setColorAt(0, QColor(58, 96, 150));
        wallpaper.setColorAt(1, QColor(150, 186, 214));
        painter.fillRect(QRect(QPoint(), logical), wallpaper);
        const QRect window(330, 120, 640, 420);
        painter.fillRect(window, QColor(249, 249, 249));
        painter.fillRect(QRect(window.topLeft(), QSize(window.width(), 36)), QColor(238, 238, 238));
        painter.setPen(QColor(30, 30, 30));
        QFont font(QGuiApplication::font().family());
        font.setPixelSize(14);
        painter.setFont(font);
        painter.drawText(window.x() + 14, window.y() + 23, u"文档 - 记事本"_s);
        font.setPixelSize(16);
        painter.setFont(font);
        for (int line = 0; line < 9; ++line)
            painter.drawText(window.x() + 24, window.y() + 76 + line * 34,
                             u"第 %1 行：截图时屏幕左下角列出此刻能用的快捷键。"_s.arg(line + 1));
        painter.fillRect(QRect(0, logical.height() - 48, logical.width(), 48), QColor(32, 32, 32));
        for (int icon = 0; icon < 6; ++icon)
            painter.fillRect(QRect(470 + icon * 56, logical.height() - 36, 24, 24), QColor(90 + icon * 20, 140, 210));
    }
    screen.setImage(image);
    CaptureSession session;
    session.setOcrAvailable(true);
    CaptureSession::Windows windows;
    windows.tops = {QRect(phys(330, 120), phys(970, 540) - QPoint(1, 1))};
    session.begin({&screen}, windows);
    session.setHistoryAvailable(true);
    for (int i = 0; i < 3; ++i)
        session.rememberSelection(QRect(phys(400 + i * 20, 200), phys(800 + i * 20, 460) - QPoint(1, 1)));
    QList<bool> kept;
    QObject::connect(&session, &CaptureSession::keyHintsKept, &session, [&](bool on) { kept.append(on); });

    QQmlApplicationEngine engine;
    QQmlComponent component(&engine, u"WinShun"_s, u"CaptureOverlay"_s);
    auto* window = qobject_cast<QQuickWindow*>(component.createWithInitialProperties({
        {u"session"_s, QVariant::fromValue(&session)}, {u"screenModel"_s, QVariant::fromValue(&screen)}}));
    if (!window) { say(component.errorString()); return 3; }
    window->resize(logical);
    showOffScreen(window);
    settle(window);
    auto* hints = window->findChild<QQuickItem*>(u"captureKeyHints"_s);
    auto* card = window->findChild<QQuickItem*>(u"captureKeyHintsCard"_s);
    auto* keep = window->findChild<QQuickItem*>(u"captureKeyHintsKeep"_s);
    auto* recall = window->findChild<QQuickItem*>(u"captureRecallIndex"_s);
    if (!hints || !card || !keep || !recall) { say(u"FAIL: key hint items not found"_s); return finish(window, false); }
    bool ok = true;
    const auto check = [&](bool passed, const QString& what) {
        say(u"%1: %2"_s.arg(passed ? u"PASS"_s : u"FAIL"_s, what));
        ok = passed && ok;
    };
    const auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, window->mapToGlobal(at.toPoint()), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
        QCoreApplication::processEvents();
    };
    const auto move = [&](QPointF at) { send(QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton); };
    const auto drag = [&](QPointF from, QPointF to) {
        move(from);
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 6; ++i)
            send(QEvent::MouseMove, from + (to - from) * (i / 6.0), Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    };
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent press(QEvent::KeyPress, code, modifiers);
        QCoreApplication::sendEvent(window, &press);
        QKeyEvent release(QEvent::KeyRelease, code, modifiers);
        QCoreApplication::sendEvent(window, &release);
        QCoreApplication::processEvents();
    };
    const auto opacity = [&] { return card->opacity(); };
    const auto rows = [&] { return session.keyHints().join(u','); };
    const auto grab = [&](QStringView name) {
        settle(window);
        ok = saveGrab(window, out.filePath(u"keyhints-"_s + name.toString() + u".png"_s)) && ok;
    };
    const auto click = [&](QQuickItem* item, Qt::MouseButton button = Qt::LeftButton) {
        const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        move(at);
        send(QEvent::MouseButtonPress, at, button, button);
        send(QEvent::MouseButtonRelease, at, button, Qt::NoButton);
    };

    constexpr int startMs = static_cast<int>(KeyHintPolicy::kStartMs);
    constexpr int idleMs = static_cast<int>(KeyHintPolicy::kIdleMs);

    // Rule 1: a capture begins with none; moving about leaves the wait be.
    session.begin({&screen}, windows); // afresh, now that the overlay is up
    move(QPointF(640, 330));
    check(!session.keyHintsShown() && opacity() == 0, u"none as the capture begins"_s);
    for (int i = 0; i < 4; ++i) {
        pump(startMs / 4 + 20);
        move(QPointF(640 + i * 30, 330 + i * 10));
    }
    check(session.keyHintsShown(), u"shown by themselves after %1 ms with no button pressed"_s.arg(startMs));
    move(QPointF(640, 330));
    grab(u"choosing");
    check(rows() == u"wheel,wasd,recall,history,ocr,quit", u"choosing rows: "_s + rows());
    check(opacity() == 1 && hints->property("corner").toInt() == 0, u"shown in the bottom-left corner"_s);
    const QRectF cardRect = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
    say(u"card %1,%2 %3x%4 logical"_s.arg(cardRect.x()).arg(cardRect.y()).arg(cardRect.width()).arg(cardRect.height()));
    key(Qt::Key_W);
    check(session.keyHintsShown(), u"a key while choosing (W) leaves them up"_s);

    move(QPointF(cardRect.right() + 20, cardRect.top() + 30));
    grab(u"pointer-near");
    check(std::abs(opacity() - 0.15) < 0.01, u"the pointer near: see-through (%1)"_s.arg(opacity()));
    move(keep->mapToScene(QPointF(keep->width() / 2, keep->height() / 2)));
    grab(u"button-hover");
    check(opacity() == 1, u"on its button: in full again"_s);

    // 不再显示: the setting off, the capture as it was; ? shows it again, 一直显示 turns it on.
    click(keep, Qt::RightButton);
    check(session.active() && !session.hasSelection() && kept.isEmpty() && session.keyHintsShown(),
          u"a right click on the button is not Esc, nor a press that hides them"_s);
    click(keep);
    check(kept == QList<bool>{false} && !session.keyHintsShown() && !session.hasSelection()
              && session.phaseName() == u"hover" && session.active(),
          u"不再显示 turned the setting off without selecting or ending the capture"_s);
    check(!screen.notice().isEmpty(), u"it says how to have them back: "_s + screen.notice());
    settle(window);
    check(opacity() == 0, u"gone after 不再显示"_s);
    move(QPointF(640, 330));
    key(Qt::Key_Question, Qt::ShiftModifier);
    check(session.keyHintsShown(), u"? (after the click: the overlay still has the keys) shows them for this capture"_s);
    grab(u"temporary");
    check(opacity() == 1, u"shown by ? with the setting off"_s);
    click(keep);
    check(kept == (QList<bool>{false, true}) && session.keyHintsEnabled() && session.keyHintsShown(),
          u"一直显示 turned the setting on and they stay"_s);
    key(Qt::Key_Slash);
    check(!session.keyHintsShown(), u"/ hides them"_s);
    key(Qt::Key_F1);
    check(session.keyHintsShown(), u"F1 shows them"_s);
    settle(window);

    // Rule 2: the left button down on the picture takes even a shown-by-hand panel away, quickly.
    move(QPointF(380, 180));
    send(QEvent::MouseButtonPress, QPointF(380, 180), Qt::LeftButton, Qt::LeftButton);
    check(!session.keyHintsShown(), u"a left press hides them at once"_s);
    pump(60);
    const qreal at60 = opacity();
    pump(80);
    say(u"fade out: %1 after ~60 ms, %2 after ~140 ms"_s.arg(at60).arg(opacity()));
    check(opacity() == 0, u"gone within ~140 ms of the press"_s);
    for (int i = 1; i <= 6; ++i)
        send(QEvent::MouseMove, QPointF(380, 180) + (QPointF(900, 470) - QPointF(380, 180)) * (i / 6.0), Qt::NoButton,
             Qt::LeftButton);
    send(QEvent::MouseButtonRelease, QPointF(900, 470), Qt::LeftButton, Qt::NoButton);
    move(QPointF(640, 330));
    check(session.hasSelection() && !session.keyHintsShown(), u"a selection made: none come back at once"_s);

    // Rule 3: a pause on the selection brings its rows, once; any input takes them away.
    pump(idleMs + 300);
    check(session.keyHintsShown(), u"after %1 ms without input: the selection's rows"_s.arg(idleMs));
    grab(u"selected");
    check(rows() == u"copy,save,pin,arrows,ocr,long", u"selected rows: "_s + rows());
    check(opacity() == 1 && hints->property("corner").toInt() == 0, u"still bottom-left with a selection in the middle"_s);
    move(QPointF(650, 335));
    check(!session.keyHintsShown(), u"a move after the pause hides them"_s);
    pump(idleMs + 300);
    check(!session.keyHintsShown(), u"and they do not come back in this capture"_s);

    // Rows of a tool (shown by F1, which a tool chosen leaves up).
    key(Qt::Key_F1);
    session.annotation()->setTool(0); // 矩形
    check(session.keyHintsShown(), u"F1's show stays as a tool is chosen"_s);
    grab(u"tool");
    check(rows() == u"shift,width,undo,delete,toolOff,hide", u"tool rows: "_s + rows());
    session.annotation()->setTool(6); // 文字 (T): no Shift
    check(rows() == u"width,undo,delete,toolOff,hide", u"text tool rows: "_s + rows());
    // A text being typed: none, and / is a character there.
    send(QEvent::MouseButtonPress, QPointF(620, 300), Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, QPointF(620, 300), Qt::LeftButton, Qt::NoButton);
    settle(window);
    check(session.annotation()->editingText() && rows().isEmpty() && opacity() == 0, u"none while a text is typed"_s);
    const bool shownBefore = session.keyHintsShown();
    check(!session.key(Qt::Key_Slash, Qt::NoModifier) && session.keyHintsShown() == shownBefore,
          u"/ while typing is not the hints' key"_s);
    key(Qt::Key_Escape); // the empty text closes
    session.annotation()->setTool(-1);

    key(Qt::Key_Escape);
    key(Qt::Key_R);
    check(!session.keyHintsShown() && recall->isVisible(), u"R: no hints by themselves, the index below"_s);
    key(Qt::Key_F1);
    grab(u"recalled");
    check(rows().endsWith(u",recalled"_s) && !recall->isVisible(), u"recalled region told in the panel, no bar below"_s);

    // Over the left corner: to the right one; over both: gone.
    key(Qt::Key_Escape);
    drag(QPointF(4, 300), QPointF(560, 790));
    key(Qt::Key_F1);
    move(QPointF(300, 500));
    grab(u"moved");
    check(hints->property("corner").toInt() == 1 && opacity() == 1, u"selection over the left corner: moved right"_s);
    key(Qt::Key_Escape);
    drag(QPointF(2, 2), QPointF(1276, 796));
    key(Qt::Key_F1);
    move(QPointF(640, 330));
    grab(u"blocked");
    check(opacity() == 0, u"selection over both corners: out of the way"_s);
    key(Qt::Key_Escape);

    // While dragging: none.
    move(QPointF(300, 250));
    send(QEvent::MouseButtonPress, QPointF(300, 250), Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, QPointF(500, 400), Qt::NoButton, Qt::LeftButton);
    settle(window);
    check(rows().isEmpty() && opacity() == 0, u"none while dragging"_s);
    send(QEvent::MouseButtonRelease, QPointF(500, 400), Qt::LeftButton, Qt::NoButton);

    // Rule 7: a capture begun by the delay's countdown shows none by itself.
    key(Qt::Key_Escape);
    session.begin({&screen}, windows);
    session.quietKeyHints();
    move(QPointF(640, 330));
    pump(startMs + 300);
    check(!session.keyHintsShown() && opacity() == 0, u"a delayed capture: none by themselves"_s);

    // Off in the settings, in a new capture: none by themselves; a recalled region still says which.
    key(Qt::Key_Escape);
    session.begin({&screen}, windows);
    session.setKeyHints(false);
    key(Qt::Key_R);
    move(QPointF(640, 330));
    pump(idleMs + 300);
    grab(u"off-recalled");
    check(opacity() == 0 && recall->isVisible(), u"off: only the recalled region's index, at the bottom"_s);
    say(u"key hint checks: %1"_s.arg(ok ? u"passed"_s : u"failed"_s));
    return finish(window, ok);
}

// --preview-capture-tools <dir> --edit-window: the editing window (EditWindow,
// 编辑 on a pin or a picture) over a plain picture, off the screen, in the
// 界面风格 asked for: its title bar, toolbar and canvas (edit-window.png).
int runEditWindow(const QStringList& args)
{
    using namespace capture;
    const QDir out(valueAfter(args, kCaptureToolsFlag));
    QDir().mkpath(out.path());
    QQmlApplicationEngine engine;
    CaptureEditor editor;
    CaptureEditor::Hooks hooks;
    hooks.window.show = [](QQuickWindow* window) { showOffScreen(window); };
    hooks.window.nativeRect = [](QQuickWindow* window) {
        const qreal dpr = window->devicePixelRatio();
        return QRect(QPoint(qRound(window->x() * dpr), qRound(window->y() * dpr)),
            QSize(qRound(window->width() * dpr), qRound(window->height() * dpr)));
    };
    editor.setHooks(std::move(hooks));
    WindowFrame frame(true); // its own title bar, as CaptureApp makes it
    QQmlComponent component(&engine, u"WinShun"_s, u"EditWindow"_s);
    auto* window = qobject_cast<QQuickWindow*>(component.createWithInitialProperties({
        {u"editor"_s, QVariant::fromValue(&editor)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    if (!window) { say(component.errorString()); return 3; }
    frame.setWindow(window);
    editor.attach(window);
    QImage picture(720, 420, QImage::Format_RGB32);
    picture.fill(QColor(248, 249, 251));
    {
        QPainter painter(&picture);
        painter.setPen(QColor(35, 40, 45));
        QFont font = QGuiApplication::font();
        font.setPixelSize(22);
        painter.setFont(font);
        painter.drawText(32, 52, u"贴图编辑 · 界面风格预览"_s);
        painter.fillRect(QRect(32, 90, 300, 180), QColor(79, 157, 255));
        painter.fillRect(QRect(360, 90, 300, 180), QColor(63, 191, 127));
    }
    bool ok = editor.openImage(picture);
    if (!ok)
        say(u"edit window: the picture did not open"_s);
    if (!window->isVisible())
        showOffScreen(window);
    settle(window);
    pump(400);
    settle(window, 2);
    ok = saveGrab(window, out.filePath(u"edit-window.png"_s)) && ok;
    say(u"edit window: %1"_s.arg(ok ? u"passed"_s : u"failed"_s));
    editor.close();
    return finish(window, ok);
}

int runCaptureTools(const QStringList& args)
{
    if (args.contains(u"--edit-window"_s))
        return runEditWindow(args);
    if (args.contains(u"--key-hints"_s))
        return runKeyHints(args);
    if (args.contains(u"--save-options"_s))
        return runCaptureSave(args) + g_warnings;
    if (args.contains(u"--selection-keys"_s))
        return runSelectionKeys(args);
    if (args.contains(u"--toolbar-drag"_s) || args.contains(u"--brush"_s) || args.contains(u"--brush-performance"_s))
        return runToolbarDrag(args);
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
    const int screenIndex = valueAfter(args, u"--screen-index", u"0"_s).toInt();
    const auto screens = QGuiApplication::screens();
    if (screenIndex < 0 || screenIndex >= screens.size()) { say(u"recording test screen is unavailable"_s); return 3; }
    fixture->setScreen(screens[screenIndex]);
    fixture->setPosition(screens[screenIndex]->geometry().topLeft() + QPoint(80, 80));
    const bool fullScreenRecording = args.contains(u"--full-screen"_s);
    // A full-screen recording must contain only this test's generated content.
    if (fullScreenRecording) {
        fixture->setFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        fixture->showFullScreen();
    } else fixture->show();
    settle(fixture.get());
    const HWND hwnd = reinterpret_cast<HWND>(fixture->winId());
    RECT client {};
    ::GetClientRect(hwnd, &client);
    POINT at {0, 0};
    ::ClientToScreen(hwnd, &at);
    const QRect fixtureRegion(at.x, at.y, client.right, client.bottom);
    const QRect region = fullScreenRecording
        ? QRect(fixtureRegion.topLeft() + QPoint(40, 40), QSize(std::min(800, int(client.right) - 80), std::min(460, int(client.bottom) - 80)))
        : fixtureRegion;
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
    const int compactWidth = valueAfter(args, u"--compact-width").toInt();
    if (compactWidth > 0) {
        panel->setWidth(compactWidth);
        settle(panel);
    }
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
    if (compactWidth > 0) {
        qreal right = 0;
        bool usable = true;
        for (const auto* name : {"recordingArea", "recordingFullScreen", "recordingMp4", "recordingGif",
                 "recordingSystemAudio", "recordingMicrophone", "recordingOptions", "recordingStart", "recordingClose"}) {
            auto* item = panel->findChild<QQuickItem*>(QString::fromLatin1(name));
            if (!item) { usable = false; break; }
            const auto box = item->mapRectToScene(item->boundingRect());
            usable = usable && box.left() >= right && box.right() <= panel->width() - 10 && box.width() >= 24;
            right = box.right();
        }
        say(u"recording controls fit compact width %1 without overlap: %2"_s.arg(panel->width()).arg(usable));
        saved = usable && saved;
    }
    MONITORINFO recordingMonitor {sizeof recordingMonitor};
    const auto monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!::GetMonitorInfoW(monitor, &recordingMonitor)) return 3;
    const QRect wholeScreen(recordingMonitor.rcMonitor.left, recordingMonitor.rcMonitor.top,
        recordingMonitor.rcMonitor.right - recordingMonitor.rcMonitor.left, recordingMonitor.rcMonitor.bottom - recordingMonitor.rcMonitor.top);
    const auto recordingChoicesMatch = [&](bool fullScreen) {
        auto* area = panel->findChild<QQuickItem*>(u"recordingArea"_s);
        auto* full = panel->findChild<QQuickItem*>(u"recordingFullScreen"_s);
        return area && full && area->property("checked").toBool() == !fullScreen && full->property("checked").toBool() == fullScreen;
    };
    const auto initialFormat = recorder->format();
    const auto initialQuality = recorder->quality();
    const bool initialSystemAudio = recorder->systemAudio(), initialMicrophone = recorder->microphone();
    click("recordingFullScreen");
    click("recordingFullScreen"); // clicking the active choice keeps it selected
    settle(panel);
    bool areaPassed = recorder->fullScreen() && recorder->region() == wholeScreen && recordingChoicesMatch(true);
    say(u"full-screen uses source monitor (physical %1,%2 %3x%4): %5"_s
        .arg(wholeScreen.x()).arg(wholeScreen.y()).arg(wholeScreen.width()).arg(wholeScreen.height()).arg(areaPassed));
    saved = areaPassed && saveGrab(panel, out.filePath(u"ready-full-screen.png"_s)) && saved;
    click("recordingArea");
    areaPassed = !recorder->fullScreen() && recorder->region() == region && recordingChoicesMatch(false);
    say(u"area choice restores original selection: %1"_s.arg(areaPassed));
    saved = areaPassed && saved;
    auto* fullScreenButton = panel->findChild<QQuickItem*>(u"recordingFullScreen"_s);
    if (!fullScreenButton) return 3;
    fullScreenButton->forceActiveFocus(Qt::TabFocusReason);
    key(Qt::Key_Space);
    areaPassed = recorder->fullScreen() && recorder->region() == wholeScreen && recordingChoicesMatch(true);
    saved = areaPassed && saved;
    click("recordingArea");
    areaPassed = areaPassed && recorder->region() == region && recorder->format() == initialFormat
        && recorder->quality() == initialQuality && recorder->systemAudio() == initialSystemAudio && recorder->microphone() == initialMicrophone;
    say(u"keyboard selects full-screen; switching preserves recording preferences: %1"_s.arg(areaPassed));
    saved = areaPassed && saved;
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
    if (fullScreenRecording) click("recordingFullScreen");
    if (originalCountdown) {
        click("recordingStart");
        saved = recorder->stage() == Recorder::Counting && saved;
        const QRect frozen = recorder->region();
        recorder->setFullScreen(!recorder->fullScreen());
        saved = recorder->region() == frozen && recorder->fullScreen() == fullScreenRecording && saved;
        settle(panel);
        saved = saveGrab(panel, out.filePath(u"counting.png"_s)) && saved;
        click("recordingClose");
        saved = !recorder->active() && !panel->isVisible() && saved;
        recorder->prepare(region, ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), fixture->devicePixelRatio());
        if (fullScreenRecording) click("recordingFullScreen");
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
            const QRect expectedRegion = fullScreenRecording ? wholeScreen : region;
            const bool expectedMode = recorder->fullScreen();
            recorder->setFullScreen(!expectedMode);
            const bool frozen = recorder->region() == expectedRegion && recorder->fullScreen() == expectedMode;
            say(u"recording range is locked after start: %1"_s.arg(frozen));
            saved = frozen && saved;
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
                    const bool sameArea = recorder->fullScreen() == fullScreenRecording
                        && recorder->region() == (fullScreenRecording ? wholeScreen : region) && recordingChoicesMatch(fullScreenRecording);
                    say(u"another take keeps recording mode and range: %1"_s.arg(sameArea));
                    saved = sameArea && saved;
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

// --preview-presenter [--laser]: the presenter's white board (GitHub #189)
// for the README, with no desktop in it: the production Presenter, ink window
// (PresenterOverlay.qml) and toolbar (PresenterToolbar.qml) off the screen,
// with made-up hooks in place of PresenterHost's (one monitor 1000 x 625
// logical px, no foreground, nothing remembered on disk). The ink comes through
// the ink window's own mouse routing; the toolbar is drawn over the board
// where PresenterHost first puts it; presenter-board.png. --laser: the
// laser pointer held at the end, its trail drawn.
int runPresenter(const QStringList& args)
{
    using namespace capture;
    const QDir out(valueAfter(args, kPresenterFlag));
    QDir().mkpath(out.path());
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/capture_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    const qreal dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
    const QSize logical(1000, 625);
    constexpr int toolbarTop = 12; // PresenterHost's kToolbarTop
    QVariantMap store {{QString::fromLatin1(Presenter::kRecordHintKey), true}, {QString::fromLatin1(Presenter::kThroughHintKey), true}};
    Presenter presenter;
    Presenter::Hooks hooks;
    hooks.monitors = [&] { return QList<Presenter::Monitor> {{QSize(qRound(logical.width() * dpr), qRound(logical.height() * dpr)), dpr}}; };
    hooks.screenAtPointer = [] { return 0; };
    hooks.foreground = [] { return quintptr(0); };
    hooks.setForeground = [](quintptr) { return false; };
    hooks.beneathPointer = [] { return quintptr(0); };
    hooks.load = [&](const QString& key) { return store.value(key); };
    hooks.save = [&](const QString& key, const QVariant& value) { store.insert(key, value); };
    presenter.setHooks(std::move(hooks));
    presenter.open();
    presenter.toggleBoard(int(presenter::Board::White));
    PresenterScreen* screen = presenter.screen(0);
    if (!screen) { say(u"FAIL: no presenter screen"_s); return 4; }

    QQmlApplicationEngine engine;
    const auto make = [&](const char* type, const QVariantMap& properties) {
        QQmlComponent component(&engine, u"WinShun"_s, QString::fromLatin1(type));
        auto* window = qobject_cast<QQuickWindow*>(component.createWithInitialProperties(properties));
        if (!window) {
            say(component.errorString());
            return window;
        }
        QSurfaceFormat format = window->format();
        format.setAlphaBufferSize(8); // as PresenterHost: drawn on nothing
        window->setFormat(format);
        return window;
    };
    QQuickWindow* ink = make("PresenterOverlay", {{u"presenter"_s, QVariant::fromValue(&presenter)}, {u"screenModel"_s, QVariant::fromValue(screen)}});
    QQuickWindow* toolbar = make("PresenterToolbar", {{u"presenter"_s, QVariant::fromValue(&presenter)}});
    if (!ink || !toolbar) return 3;
    ink->resize(logical);
    showOffScreen(ink);
    showOffScreen(toolbar);
    settle(ink);

    const auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, ink->mapToGlobal(at.toPoint()), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(ink, &event);
        QCoreApplication::processEvents();
    };
    // `stepMs` between the moves: the laser's trail fades with time (kLifeMs).
    const auto stroke = [&](const QList<QPointF>& points, bool release = true, int stepMs = 0) {
        send(QEvent::MouseButtonPress, points.first(), Qt::LeftButton, Qt::LeftButton);
        for (qsizetype i = 1; i < points.size(); ++i) {
            const QPointF from = points[i - 1], to = points[i];
            for (int step = 1; step <= 6; ++step) {
                send(QEvent::MouseMove, from + (to - from) * (step / 6.0), Qt::NoButton, Qt::LeftButton);
                if (stepMs)
                    pump(stepMs);
            }
        }
        if (release)
            send(QEvent::MouseButtonRelease, points.last(), Qt::LeftButton, Qt::NoButton);
    };
    const auto tool = [&](annot::Kind kind, const QVariantMap& panel) {
        presenter.setTool(int(kind));
        presenter.model()->setPanel(panel);
    };
    // The text and number boxes ask for the keys (requestActivate): a
    // warning each, as the windows here never take the focus.
    const auto text = [&](QPointF at, const QString& words) {
        stroke({at});
        presenter.model()->commitText(words);
    };
    const auto number = [&](QPointF at) {
        stroke({at});
        if (presenter.model()->editingText())
            presenter.model()->commitText({});
        if (presenter.model()->editingNumber())
            presenter.model()->closeNumberEdit();
    };

    // A plan on the board: a title, three boxes with arrows and numbers,
    // the last one ringed and ticked, a highlighted line.
    tool(annot::Kind::Text, {{u"color"_s, u"#212121"_s}, {u"size"_s, 32}, {u"bold"_s, true}});
    text({80, 96}, u"新版上线计划"_s);
    tool(annot::Kind::Rect, {{u"color"_s, u"#1E88E5"_s}, {u"width"_s, 4}});
    const QList<qreal> lefts {90, 395, 700};
    for (const qreal x : lefts)
        stroke({{x, 220}, {x + 210, 340}});
    tool(annot::Kind::Text, {{u"color"_s, u"#212121"_s}, {u"size"_s, 24}, {u"bold"_s, false}});
    const QStringList words {u"整理需求"_s, u"开发测试"_s, u"发布上线"_s};
    for (qsizetype i = 0; i < lefts.size(); ++i)
        text({lefts[i] + 57, 262}, words[i]);
    tool(annot::Kind::Arrow, {{u"color"_s, u"#FB8C00"_s}, {u"width"_s, 4}});
    stroke({{308, 280}, {387, 280}});
    stroke({{613, 280}, {692, 280}});
    tool(annot::Kind::Number, {{u"color"_s, u"#E53935"_s}, {u"size"_s, 18}});
    for (const qreal x : lefts)
        number({x, 220});
    tool(annot::Kind::Ellipse, {{u"color"_s, u"#E53935"_s}, {u"width"_s, 4}});
    stroke({{672, 196}, {938, 366}});
    tool(annot::Kind::Marker, {{u"color"_s, u"#FDD835"_s}, {u"width"_s, 20}});
    stroke({{100, 452}, {404, 452}});
    tool(annot::Kind::Text, {{u"color"_s, u"#212121"_s}, {u"size"_s, 24}, {u"bold"_s, false}});
    text({96, 436}, u"周五前完成测试，下周一发布"_s);
    tool(annot::Kind::Pen, {{u"color"_s, u"#43A047"_s}, {u"width"_s, 6}});
    stroke({{820, 410}, {846, 440}, {900, 384}});
    const bool laser = args.contains(u"--laser"_s); // the laser held, its trail after it
    presenter.setLaser(laser);
    settle(ink);
    settle(toolbar);
    if (laser)
        stroke({{600, 560}, {680, 520}, {760, 545}, {840, 505}}, false, 12); // held: the trail shows

    bool ok = true;
    const auto check = [&](bool passed, const QString& what) {
        say(u"%1: %2"_s.arg(passed ? u"PASS"_s : u"FAIL"_s, what));
        ok = passed && ok;
    };
    check(presenter.board() == int(presenter::Board::White) && screen->boardModel(), u"the white board is open"_s);
    check(screen->boardModel() && screen->boardModel()->itemCount() >= 14 && screen->desk()->itemCount() == 0,
          u"the ink is on the board, not the desktop: %1 item(s)"_s.arg(screen->boardModel() ? screen->boardModel()->itemCount() : 0));
    const QImage board = ink->grabWindow();
    QImage bar = toolbar->grabWindow();
    check(!board.isNull() && !bar.isNull(), u"both windows drawn"_s);
    say(u"toolbar %1x%2 logical"_s.arg(toolbar->width()).arg(toolbar->height()));
    QImage picture = board.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    {
        QPainter painter(&picture);
        bar = bar.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        // Both grabs are at the windows' scale: logical px here.
        painter.drawImage(QPoint((logical.width() - toolbar->width()) / 2, toolbarTop), bar);
    }
    const QString file = out.filePath(u"presenter-board.png"_s);
    const bool saved = picture.save(file, "PNG");
    say(u"%1 %2 %3x%4"_s.arg(saved ? u"saved"_s : u"NOT saved"_s, file).arg(picture.width()).arg(picture.height()));
    if (laser)
        send(QEvent::MouseButtonRelease, {840, 505}, Qt::LeftButton, Qt::NoButton);
    presenter.close();
    toolbar->hide();
    delete toolbar;
    say(u"presenter checks: %1"_s.arg(ok ? u"passed"_s : u"failed"_s));
    return finish(ink, ok && saved);
}

} // namespace

bool wanted(const QStringList& args)
{
    return args.contains(kCaptureToolsFlag) || args.contains(kRecordingFlag) || args.contains(kLongCaptureFlag) || args.contains(kOcrFlag) || args.contains(kSettingsFlag)
        || args.contains(kPresenterFlag);
}

void note(const QString& line)
{
    say(line);
}

void prepare(const QStringList& args)
{
    const QString folder = valueAfter(args, args.contains(kPresenterFlag) ? kPresenterFlag : args.contains(kSettingsFlag) ? kSettingsFlag : args.contains(kOcrFlag) ? kOcrFlag : args.contains(kLongCaptureFlag) ? kLongCaptureFlag
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
    const bool presenterMode = args.contains(kPresenterFlag);
    if (valueAfter(args, presenterMode ? kPresenterFlag : settingsMode ? kSettingsFlag : ocrMode ? kOcrFlag : longCaptureMode ? kLongCaptureFlag : recordingMode ? kRecordingFlag : kCaptureToolsFlag).isEmpty()) {
        std::fprintf(stderr, "usage: WinShunCapture --preview-capture-tools <folder> [--toolbar-drag] [--scale 1.5] [--dark] [--renderer d3d11]\n"
                             "       WinShunCapture --preview-recording <folder> --renderer d3d11 --auto [--gif] [--clicks]\n"
                             "       WinShunCapture --preview-long-capture <folder> --renderer d3d11 [--auto]\n"
                             "       WinShunCapture --preview-presenter <folder> [--laser] [--scale 2] [--renderer d3d11]\n"
                             "       (with --app-dir <Win顺's folder>, or WS_APP_DIR)\n");
        return 2;
    }
    g_previous = qInstallMessageHandler(countWarnings);
    g_edges = args.contains(u"--edges"_s);
    QGuiApplication::styleHints()->setColorScheme(args.contains(u"--dark"_s) ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    SystemTheme::setBackdropAvailable(false); // plain colours, as a window without Mica
    SystemTheme::setUiStyle(valueAfter(args, u"--style", u"fluent"_s)); // --style fluent|brand|tactile
    if (presenterMode)
        return runPresenter(args);
    if (settingsMode)
        return runSettings(args);
    if (ocrMode)
        return runOcr(args);
    if (longCaptureMode)
        return runLongCapture(args) + g_warnings;
    return recordingMode ? runRecording(args) : runCaptureTools(args);
}

} // namespace ws::captureplugin::preview

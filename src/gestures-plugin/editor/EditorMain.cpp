// WinShunGesturesEditor.dll: the settings window of the mouse gestures plugin
// (winshun.gestures). `WinShunGestures.exe --editor` (EditorStub.cpp, no Qt)
// finds Win顺's Qt and calls winshunGesturesEditorMain; the plugin has started
// it with its stdin and stdout as pipes (EditorHost.h, EditorLink.h) and the
// window shows what it is sent and sends what is changed.
//
// For checking without a desktop or a plugin:
//   WinShunGestures.exe --editor --preview <png> [--page 0-4] [--scale 1.5] [--dark] [--locale en] [--style fluent]
//       [--width W --height H] [--picker menu|keys|open|url|type] [--program photoshop.exe] [--buttons x1]
//       --app-dir <a Win顺's folder, or one with Qt> [--font <ttf>]
// renders the window offscreen with sample rules and writes the PNG.
//   --selftest: the window is made without activating it, and the settings it is sent and what it
//   changes are checked (lines to stderr; the saves go to stdout as in use).
#include "EditorLink.h"
#include "GestureEditor.h"
#include "FontFile.h"
#include "Phosphor.h"
#include "QmlRuntime.h"
#include "SystemTheme.h"
#include "WindowFrame.h"
#include "platform/WindowEffects.h"

#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>
#include <QTimer>
#include <QTranslator>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cmath>
#include <numbers>
#include <cstdio>
#include <memory>

using namespace Qt::StringLiterals;
using namespace ws::gestureseditor;

namespace {

int g_warnings = 0;

// Qt's own stderr logging writes the ANSI code page; the plugin host reads UTF-8.
void logToStderrAsUtf8(QtMsgType type, const QMessageLogContext& context, const QString& text)
{
    // A window made off the screen for a picture gets its native handle again when shown: told, not a fault.
    if ((type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) && !text.contains(u"new native handle"_s))
        ++g_warnings;
    const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    if (!err || err == INVALID_HANDLE_VALUE)
        return;
    const QByteArray line = qFormatLogMessage(type, context, text).toUtf8() + '\n';
    DWORD put = 0;
    ::WriteFile(err, line.constData(), static_cast<DWORD>(line.size()), &put, nullptr);
}

void say(const QString& line)
{
    const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    if (!err || err == INVALID_HANDLE_VALUE)
        return;
    const QByteArray bytes = line.toUtf8() + '\n';
    DWORD put = 0;
    ::WriteFile(err, bytes.constData(), static_cast<DWORD>(bytes.size()), &put, nullptr);
}

// The program's icon in the title bar: Win顺's, read from its WinShun.exe.
class AppIcon : public QQuickImageProvider {
public:
    explicit AppIcon(QString path)
        : QQuickImageProvider(Image)
        , m_path(std::move(path))
    {
    }
    QImage requestImage(const QString&, QSize* size, const QSize& requested) override
    {
        const int side = requested.isValid() ? std::max(requested.width(), requested.height()) : 32;
        HICON icon = nullptr;
        ::SHDefExtractIconW(reinterpret_cast<LPCWSTR>(m_path.utf16()), 0, 0, &icon, nullptr, MAKELONG(side, 0));
        QImage image;
        if (icon) {
            image = QImage::fromHICON(icon);
            ::DestroyIcon(icon);
        }
        if (image.isNull()) { // none (a preview without WinShun.exe): nothing drawn, not a failure
            image = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
        }
        if (size)
            *size = image.size();
        return image;
    }

private:
    QString m_path;
};

void pump(int ms)
{
    QElapsedTimer timer;
    timer.start();
    do
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    while (timer.elapsed() < ms);
}

// A window off the screen gets no resize or expose from Windows: sized here, a few times (the height
// follows the width through the wrapping), a grab per pass for the frame the layout needs.
void settle(QQuickWindow* window, int passes = 4)
{
    for (int pass = 0; pass < passes; ++pass) {
        window->contentItem()->setSize(QSizeF(window->width(), window->height()));
        pump(200);
        (void)window->grabWindow();
    }
}

// Off the screen, never activated: the software renderer draws what is shown, and the grab takes the frame.
void showOffScreen(QQuickWindow* window)
{
    const int offscreen = static_cast<int>(std::floor(24000 / window->devicePixelRatio()));
    window->setPosition(offscreen, offscreen);
    window->show();
}

// A mouse event for the window, from the position in it: for the preview's drawn gestures (nothing reaches the desktop).
void sendMouse(QQuickWindow* window, QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, at, window->mapToGlobal(at), button, buttons, modifiers);
    QCoreApplication::sendEvent(window, &event);
}

QQuickItem* findItem(QQuickItem* item, const QString& name)
{
    if (item->objectName() == name)
        return item;
    for (QQuickItem* child : item->childItems()) {
        if (QQuickItem* found = findItem(child, name))
            return found;
    }
    return nullptr;
}

// The points of a gesture drawn from `from`: each direction 44 logical pixels on, or a full circle.
QList<QPointF> gesturePoints(const QString& gesture, const QPointF& from)
{
    QList<QPointF> points {from};
    if (gesture == u"cw" || gesture == u"ccw") {
        const double sign = gesture == u"cw" ? 1 : -1;
        const QPointF centre = from + QPointF(0, 40);
        for (int degrees = 0; degrees <= 380; degrees += 12) {
            const double angle = (-90 + sign * degrees) * std::numbers::pi / 180;
            points.append(centre + QPointF(40 * std::cos(angle), 40 * std::sin(angle)));
        }
        return points;
    }
    QPointF at = from;
    for (const QChar c : gesture) {
        const QPointF step = c == u'L' ? QPointF(-4, 0) : c == u'R' ? QPointF(4, 0) : c == u'U' ? QPointF(0, -4) : QPointF(0, 4);
        for (int i = 0; i < 11; ++i) {
            at += step;
            points.append(at);
        }
    }
    return points;
}

} // namespace

extern "C" __declspec(dllexport) int winshunGesturesEditorMain(const wchar_t* host)
{
    const QString appDir = QString::fromWCharArray(host);
    qInstallMessageHandler(logToStderrAsUtf8);
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    QStringList arguments;
    int count = 0;
    if (LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i)
            arguments.append(QString::fromWCharArray(wide[i]));
        ::LocalFree(wide);
    }
    const auto option = [&](const QString& name, const QString& fallback = {}) {
        const int index = arguments.indexOf(name);
        return index >= 0 && index + 1 < arguments.size() ? arguments[index + 1] : fallback;
    };
    const bool preview = arguments.contains(u"--preview"_s);
    const bool selftest = arguments.contains(u"--selftest"_s);
    const bool dark = arguments.contains(u"--dark"_s);

    // The plugin's first line (the rules, the settings, the look) before Qt is up: its look says how Qt draws.
    QByteArray pending;
    std::optional<QJsonObject> first;
    if (!preview) {
        first = EditorLink::readFirst(&pending);
        if (!first || first->value(u"type"_s).toString() != u"init") {
            say(u"WinShunGestures --editor: the first line was not init"_s);
            return 2;
        }
    }
    QJsonObject appearance = first ? first->value(u"appearance"_s).toObject() : QJsonObject {};
    if (preview) {
        appearance = {{u"themeSetting"_s, dark ? u"dark"_s : u"light"_s}, {u"locale"_s, option(u"--locale"_s, u"zh"_s)},
            {u"uiStyle"_s, option(u"--style"_s, u"fluent"_s)}};
    }
    const bool software = preview || appearance.value(u"renderer"_s).toString() == u"software";
    if (preview) {
        // The scale asked for, exactly: not on top of the screen's own. Rendered by software: no window on screen.
        qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
        qputenv("QT_SCALE_FACTOR", option(u"--scale"_s, u"1"_s).toLatin1());
        qputenv("QT_QUICK_BACKEND", "software");
        qputenv("QT_QPA_PLATFORM", "windows:fontengine=freetype");
    } else {
        qputenv("QT_QPA_PLATFORM", software ? "windows:fontengine=gdi" : "windows:fontengine=freetype");
    }
    ws::qmlruntime::prepare();
    QCoreApplication::setLibraryPaths({appDir});
    QCoreApplication::setOrganizationName(u"WinShun"_s);
    QCoreApplication::setApplicationName(u"WinShunGestures"_s);
    int argc = 1;
    char name[] = "WinShunGestures";
    char* argv[] = {name, nullptr};
    QGuiApplication app(argc, argv);
    qunsetenv("QT_QPA_PLATFORM");
    QGuiApplication::setApplicationDisplayName(u"Win顺"_s);
    QGuiApplication::setQuitOnLastWindowClosed(true);
    if (!preview)
        QQuickWindow::setGraphicsApi(software ? QSGRendererInterface::Software : QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    QImageReader::setAllocationLimit(64);
    ws::SystemTheme::setBackdropAvailable(false);
    ws::phosphor::loadFonts();
    // Win顺's UI font, read as data (FreeType opens files through the C runtime, which a path outside the ANSI code page defeats).
    const QString fontFile = option(u"--font"_s, QDir(appDir).filePath(u"fonts/AlibabaPuHuiTi-3-55-Regular.ttf"_s));
    const auto families = QFontDatabase::applicationFontFamilies(ws::addFontFromFile(fontFile));
    if (!families.isEmpty()) {
        QFont font(families.first());
        font.setHintingPreference(QFont::PreferVerticalHinting);
        QGuiApplication::setFont(font);
    } else {
        qWarning().noquote() << "UI font: nothing loaded from" << QDir::toNativeSeparators(fontFile);
    }

    QTranslator translator;
    GestureEditor editor;
    QQmlApplicationEngine engine;
    engine.addImportPath(appDir + u"/qml"_s);
    engine.addImageProvider(u"gesturesappicon"_s, new AppIcon(QDir(appDir).filePath(u"WinShun.exe"_s)));
    ws::WindowFrame::setIcon(u"image://gesturesappicon/app"_s);
    QQuickWindow* window = nullptr;
    // Win顺's look, as it says it (init, then appearance as it changes).
    const auto applyAppearance = [&](const QJsonObject& look) {
        const QString locale = look.value(u"locale"_s).toString(u"zh"_s);
        qputenv("WS_LOCALE", locale.toUtf8());
        const QString theme = look.value(u"themeSetting"_s).toString();
        QGuiApplication::styleHints()->setColorScheme(theme == u"dark" ? Qt::ColorScheme::Dark
                : theme == u"light"                                    ? Qt::ColorScheme::Light
                                                                       : Qt::ColorScheme::Unknown);
        ws::SystemTheme::setRoundedCorners(look.value(u"corners"_s).toString(u"round"_s) == u"round");
        ws::SystemTheme::setUiSize(look.value(u"uiSize"_s).toString(u"standard"_s));
        ws::SystemTheme::setUiStyle(look.value(u"uiStyle"_s).toString());
        ws::SystemTheme::setGlassAppearance(look.value(u"glassTransparency"_s).toInt(50), look.value(u"glassClarity"_s).toInt(50));
        ws::SystemTheme::setHoverTips(look.value(u"hoverTips"_s).toBool(true));
        QCoreApplication::removeTranslator(&translator);
        const bool english = locale == u"en";
        if (english && translator.load(u":/i18n/gestures_en.qm"_s))
            QCoreApplication::installTranslator(&translator);
        QLocale::setDefault(english ? QLocale(QLocale::English, QLocale::UnitedStates) : QLocale(QLocale::Chinese, QLocale::China));
        QGuiApplication::setApplicationDisplayName(english ? u"WinShun"_s : u"Win顺"_s);
        editor.setChinese(!english);
        engine.retranslate();
        if (window && !preview)
            ws::win::setDarkFrame(window, QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    };
    applyAppearance(appearance);

    if (first) {
        editor.applyState(*first, true);
    } else {
        editor.useSample();
        // --buttons x1 (or right,middle): other trigger buttons enabled than the sample's.
        if (const QString buttons = option(u"--buttons"_s); !buttons.isEmpty())
            editor.applyState({{u"buttons"_s, buttons}}, false);
    }

    ws::WindowFrame frame(!preview);
    const auto makeWindow = [&](bool offscreen = false) {
        QQmlComponent component(&engine, QUrl(u"qrc:/qt/qml/WinShun/GesturesWindow.qml"_s));
        QObject* object = component.createWithInitialProperties({{u"editor"_s, QVariant::fromValue(&editor)}, {u"frame"_s, QVariant::fromValue(&frame)}});
        window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << component.errorString();
            delete object;
            return false;
        }
        if (offscreen) {
            // Off the screen, never activated; set before the frame hooks the window (new flags make a new native handle).
            window->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
            window->setProperty("_q_showWithoutActivating", true);
        }
        window->create();
        frame.setWindow(window);
        if (!offscreen) {
            // DWM corners, native 1px border (Windows 11) and drop shadow (#419).
            ws::win::styleFramelessWindow(window, false);
            ws::win::setDarkFrame(window, QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
        }
        editor.setWindow(window);
        return true;
    };
    // On the screen with the pointer, in its middle.
    const auto place = [&] {
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        const QRect area = screen->availableGeometry();
        const QSize size = window->size().boundedTo(area.size() - QSize(32, 32)).expandedTo(window->minimumSize());
        window->setScreen(screen);
        window->setGeometry(QRect(area.center() - QPoint(size.width() / 2, size.height() / 2), size));
    };
    const auto showWindow = [&] {
        if (!window && !makeWindow())
            return false;
        if (!window->isVisible())
            place();
        if (window->windowStates().testFlag(Qt::WindowMinimized))
            window->showNormal();
        window->show();
        window->raise();
        window->requestActivate();
        ::SetForegroundWindow(reinterpret_cast<HWND>(window->winId()));
        return true;
    };

    std::unique_ptr<EditorLink> link;
    if (!preview) {
        link = std::make_unique<EditorLink>(pending);
        QObject::connect(link.get(), &EditorLink::closed, &app, &QCoreApplication::quit);
        QObject::connect(link.get(), &EditorLink::received, &app, [&](const QJsonObject& message) {
            const QString type = message.value(u"type"_s).toString();
            if (type == u"settings") {
                editor.applyState(message, false);
                if (selftest) {
                    say(u"selftest: state trail=%1 fullScreen=%2 paused=%3 buttons=%4 excluded=%5 rules=%6"_s.arg(editor.trail())
                            .arg(editor.fullScreen()).arg(editor.paused()).arg(editor.buttons().join(u','), editor.excluded().join(u','),
                                QString::fromStdString(gestures::rulesJson(editor.rules()->saved()))));
                }
            } else if (type == u"appearance") {
                applyAppearance(message.value(u"appearance"_s).toObject());
            } else if (type == u"show") {
                if (selftest)
                    say(u"selftest: show"_s); // not brought up in front of whoever is working
                else
                    showWindow();
            } else if (type == u"quit") {
                QCoreApplication::quit();
            }
        });
        QObject::connect(&editor, &GestureEditor::saveRequested, link.get(), [&](const QJsonObject& values) {
            QJsonObject line = values;
            line.insert(u"type"_s, u"save"_s);
            link->send(line);
        });
        QObject::connect(&editor, &GestureEditor::pauseRequested, link.get(), [&](bool paused) {
            link->send({{u"type"_s, u"pause"_s}, {u"paused"_s, paused}});
        });
        QObject::connect(&editor, &GestureEditor::recordingRequested, link.get(), [&](bool recording) {
            link->send({{u"type"_s, u"recording"_s}, {u"recording"_s, recording}});
        });
    }

    int result = 0;
    if (preview) {
        if (!makeWindow(true))
            return 3;
        window->setProperty("page", option(u"--page"_s, u"0"_s).toInt());
        if (const QString program = option(u"--program"_s); !program.isEmpty())
            window->setProperty("program", program);
        const int width = option(u"--width"_s).toInt();
        const int height = option(u"--height"_s).toInt();
        if (width > 0 || height > 0) {
            window->resize(qMax(window->minimumWidth(), width > 0 ? width : window->width()),
                qMax(window->minimumHeight(), height > 0 ? height : window->height()));
        }
        showOffScreen(window);
        settle(window);
        if (const QString picker = option(u"--picker"_s); !picker.isEmpty()) {
            QMetaObject::invokeMethod(window, "previewPicker", Q_ARG(QString, picker));
            settle(window, 3);
        }
        const QString path = option(u"--preview"_s);
        // --draw DR (or D+l, r:+l, x1:U, shift:DR): a gesture drawn or chorded on the pad
        // (a picture of it mid-gesture too); --then builtin:close | keys:F5 | cancel.
        if (const QString drawn = option(u"--draw"_s); !drawn.isEmpty()) {
            QQuickItem* pad = findItem(window->contentItem(), u"padArea"_s);
            if (!pad) {
                say(u"no pad on this page"_s);
                return 5;
            }
            const auto qtButtonOf = [](gestures::MouseButton b) {
                switch (b) {
                case gestures::MouseButton::Middle: return Qt::MiddleButton;
                case gestures::MouseButton::X1: return Qt::BackButton;
                case gestures::MouseButton::X2: return Qt::ForwardButton;
                case gestures::MouseButton::Left: return Qt::LeftButton;
                default: return Qt::RightButton;
                }
            };
            const auto parsed = gestures::parseGestureTrigger(drawn.toStdString());
            const Qt::MouseButton trig = parsed ? qtButtonOf(parsed->trigger) : Qt::RightButton;
            const QString stroke = parsed ? QString::fromStdString(parsed->stroke) : drawn;
            const bool hasChord = parsed && parsed->chord != gestures::MouseButton::Any;
            const Qt::MouseButton chordBtn = hasChord ? qtButtonOf(parsed->chord) : Qt::NoButton;
            const gestures::Modifiers held = parsed ? parsed->modifiers : gestures::Modifiers {0};
            const Qt::KeyboardModifiers keys = (held & gestures::kModCtrl ? Qt::ControlModifier : Qt::NoModifier)
                | (held & gestures::kModAlt ? Qt::AltModifier : Qt::NoModifier) | (held & gestures::kModShift ? Qt::ShiftModifier : Qt::NoModifier);
            const QPointF start = pad->mapToScene(QPointF(pad->width() / 2 - 20, pad->height() / 2 + (stroke.startsWith(u'U') ? 30 : -30)));
            const QList<QPointF> points = stroke.isEmpty() ? QList<QPointF> {start} : gesturePoints(stroke, start);
            sendMouse(window, QEvent::MouseButtonPress, points.first(), trig, trig, keys);
            for (qsizetype i = 1; i < points.size(); ++i) {
                sendMouse(window, QEvent::MouseMove, points[i], Qt::NoButton, trig, keys);
                if (!hasChord && i == points.size() * 3 / 4) {
                    settle(window, 2);
                    window->grabWindow().save(path + u".drag.png"_s, "PNG");
                }
            }
            if (hasChord) {
                sendMouse(window, QEvent::MouseButtonPress, points.last(), chordBtn, trig | chordBtn, keys);
                settle(window, 2);
                window->grabWindow().save(path + u".drag.png"_s, "PNG");
                sendMouse(window, QEvent::MouseButtonRelease, points.last(), chordBtn, trig, keys);
            }
            sendMouse(window, QEvent::MouseButtonRelease, points.last(), trig, Qt::NoButton, keys);
            settle(window, 3);
            say(u"drawn %1: %2 row(s)"_s.arg(drawn).arg(editor.rules()->rowCount()));
        }
        if (const QString then = option(u"--then"_s); !then.isEmpty()) {
            if (then == u"cancel")
                QMetaObject::invokeMethod(window, "previewClose");
            else if (then.startsWith(u"stage:"))
                QMetaObject::invokeMethod(window, "previewStage", Q_ARG(QString, then.mid(6)));
            else
                QMetaObject::invokeMethod(window, "previewChoose", Q_ARG(QString, then.section(u':', 0, 0)), Q_ARG(QString, then.section(u':', 1)));
            settle(window, 3);
            say(u"rules now: %1"_s.arg(QString::fromStdString(gestures::rulesJson(editor.rules()->saved()))));
        }
        // --key Ctrl+Shift+T: that key pressed in the window (a recording picker takes it).
        if (const QString keys = option(u"--key"_s); !keys.isEmpty()) {
            const QKeyCombination combination = QKeySequence::fromString(keys)[0];
            QKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers());
            QCoreApplication::sendEvent(window, &press);
            QKeyEvent release(QEvent::KeyRelease, combination.key(), combination.keyboardModifiers());
            QCoreApplication::sendEvent(window, &release);
            settle(window, 2);
            say(u"rules now: %1"_s.arg(QString::fromStdString(gestures::rulesJson(editor.rules()->saved()))));
        }
        // --click <objectName>: a click on the first such item.
        if (const QString click = option(u"--click"_s); !click.isEmpty()) {
            if (QQuickItem* item = findItem(window->contentItem(), click)) {
                const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
                sendMouse(window, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
                sendMouse(window, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
                sendMouse(window, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
                pump(300);
                settle(window, 2);
            } else {
                say(u"no item named %1"_s.arg(click));
            }
        }
        // --hover <objectName>: the pointer over the first such item (for the hover-only parts).
        if (const QString hover = option(u"--hover"_s); !hover.isEmpty()) {
            if (QQuickItem* item = findItem(window->contentItem(), hover)) {
                const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
                sendMouse(window, QEvent::MouseMove, at + QPointF(-3, -3), Qt::NoButton, Qt::NoButton);
                sendMouse(window, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
                pump(400);
                settle(window, 2);
            } else {
                say(u"no item named %1"_s.arg(hover));
            }
        }
        const QImage image = window->grabWindow();
        const bool saved = !image.isNull() && image.save(path, "PNG");
        say(u"%1 %2 %3x%4 (dpr %5), %6 warning(s)"_s.arg(saved ? u"saved"_s : u"NOT saved"_s, path).arg(image.width()).arg(image.height())
                .arg(window->devicePixelRatio()).arg(g_warnings));
        window->hide();
        delete window;
        return saved ? (g_warnings > 0 ? 10 : 0) : 4;
    }
    if (selftest) {
        if (!makeWindow(true))
            return 3;
        // --size 2560x1440: a big window, for what the graphics of one cost (the plugin's memory limit).
        const QStringList size = option(u"--size"_s).split(u'x');
        if (size.size() == 2)
            window->resize(size[0].toInt(), size[1].toInt());
        showOffScreen(window);
        // Something to change, then the saves it makes (to stdout, as in use).
        QTimer::singleShot(700, &app, [&] {
            say(u"selftest: changing"_s);
            editor.setAction(0, u"builtin"_s, u"refresh"_s, QString());
            editor.toggleButton(u"x1"_s);
            editor.setTrail(false);
            editor.setFullScreen(true);
            editor.addExcluded(u"Notepad.EXE"_s);
            editor.setPaused(true);
            const int row = editor.addOrFind(u"DL"_s, 0, false, QString());
            // Not kept until it has an action: the line after this one holds it.
            editor.setAction(row, u"keys"_s, u"Ctrl+Shift+T"_s, QString());
            const int chordRow = editor.addOrFind(u"D+l"_s, 0, false, QString());
            editor.setAction(chordRow, u"builtin"_s, u"reopentab"_s, QString());
        });
        QTimer::singleShot(1700, &app, [&] {
            say(u"selftest: changing again"_s);
            editor.setAction(1, u"keys"_s, u"F5"_s, QString());
        });
        // Every page drawn, as a window in use would have: for the memory the plugin's manifest allows it.
        QTimer::singleShot(2300, &app, [&] {
            for (int page = 0; page < 5; ++page) {
                window->setProperty("page", page);
                pump(250);
                (void)window->grabWindow();
            }
            window->setProperty("page", 0);
            say(u"selftest: rendered"_s);
        });
    } else if (!showWindow()) {
        return 3;
    }
    result = app.exec();
    delete window;
    return result;
}

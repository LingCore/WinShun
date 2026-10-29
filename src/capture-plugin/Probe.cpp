// Stage 0 of winshun.capture (GitHub #138): what the plugin needs, tried in
// the plugin's own process as the runner starts it -- Win顺's Qt found, a QML
// window shown, the screen read (Windows.Graphics.Capture, BitBlt), a hotkey
// registered, a low-level keyboard hook set, the clipboard opened (not
// written: that would cost the user a Win+V entry). Answers any query with
// one row per check; `wsbench --plugin <folder> --query "capprobe" --app-dir
// <a deployed Win顺>` prints them.

#include "Grabber.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>

#include <windows.h>

#include <memory>
#include <string>
#include <thread>

using namespace Qt::StringLiterals;

namespace {

void send(const QJsonObject& message)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    DWORD written = 0;
    ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), line.constData(), static_cast<DWORD>(line.size()), &written, nullptr);
}

void respond(const QJsonValue& id, const QJsonObject& result)
{
    send({{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, id}, {u"result"_s, result}});
}

QString moduleFolder(const wchar_t* name)
{
    wchar_t path[MAX_PATH * 2] {};
    const HMODULE module = ::GetModuleHandleW(name);
    if (!module || !::GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path))))
        return u"(not loaded)"_s;
    return QString::fromWCharArray(path);
}

LRESULT CALLBACK keyboardHook(int code, WPARAM w, LPARAM l)
{
    return ::CallNextHookEx(nullptr, code, w, l);
}

QJsonObject row(const QString& id, bool ok, const QString& text)
{
    return {{u"id"_s, id}, {u"title"_s, (ok ? u"ok  "_s : u"FAIL  "_s) + text}, {u"copy"_s, text}};
}

QJsonArray checks(QQmlEngine& engine)
{
    QJsonArray rows;
    rows.append(row(u"qt"_s, true, u"Qt %1, %2; platform %3"_s.arg(QString::fromLatin1(qVersion()), moduleFolder(L"Qt6Core.dll"),
                                       QGuiApplication::platformName())));

    // A window of QML, as the overlay and the pins will be: shown, and drawn once.
    QElapsedTimer t;
    t.start();
    QQmlComponent component(&engine);
    component.setData(R"(import QtQuick
Window {
    x: 40; y: 40; width: 220; height: 80; visible: true; color: "#2e7d5b"
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    Text { anchors.centerIn: parent; text: "winshun.capture probe"; color: "white" }
})"_ba, QUrl::fromLocalFile(qEnvironmentVariable("WS_PLUGIN_DIR") + u"/Probe.qml"_s)); // a local URL: a remote one loads its imports later
    std::unique_ptr<QObject> object(component.create());
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    bool drawn = false;
    if (window) {
        QEventLoop loop;
        QObject::connect(window, &QQuickWindow::frameSwapped, &loop, [&] {
            drawn = true;
            loop.quit();
        });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    rows.append(row(u"qml"_s, drawn,
        window ? u"QML window drawn in %1 ms (QtQuick from %2)"_s.arg(t.elapsed()).arg(moduleFolder(L"Qt6Quick.dll"))
               : u"no QML window: %1"_s.arg(component.errorString().trimmed())));
    object.reset();

    // The screen, every monitor.
    t.restart();
    ws::capture::Grabber grabber;
    grabber.refresh();
    QString report;
    const std::vector<ws::capture::Grabber::Shot> shots = grabber.grab(&report);
    QStringList pictures;
    bool allPixels = !shots.empty();
    for (const auto& shot : shots) {
        pictures.push_back(u"%1x%2 %3"_s.arg(shot.image.width()).arg(shot.image.height()).arg(shot.wgc ? u"WGC"_s : u"BitBlt"_s));
        // All black or all one colour would be a desktop the job keeps from us.
        const QImage small = shot.image.scaled(32, 32);
        bool varied = false;
        for (int y = 0; y < small.height() && !varied; ++y) {
            for (int x = 0; x < small.width(); ++x) {
                if (small.pixel(x, y) != small.pixel(0, 0)) {
                    varied = true;
                    break;
                }
            }
        }
        allPixels = allPixels && !shot.image.isNull() && varied;
    }
    rows.append(row(u"grab"_s, allPixels, u"%1 monitor(s) in %2 ms: %3 %4"_s.arg(shots.size()).arg(t.elapsed()).arg(pictures.join(u", "_s), report.trimmed())));

    // A hotkey (Ctrl+Alt+Shift+F12, let go at once) and a keyboard hook.
    const bool hotkey = ::RegisterHotKey(nullptr, 0x7F01, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
    const DWORD hotkeyError = hotkey ? 0 : ::GetLastError();
    if (hotkey)
        ::UnregisterHotKey(nullptr, 0x7F01);
    rows.append(row(u"hotkey"_s, hotkey, hotkey ? u"RegisterHotKey works"_s : u"RegisterHotKey failed, error %1"_s.arg(hotkeyError)));
    HHOOK hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, ::GetModuleHandleW(nullptr), 0);
    const DWORD hookError = hook ? 0 : ::GetLastError();
    if (hook)
        ::UnhookWindowsHookEx(hook);
    rows.append(row(u"hook"_s, hook != nullptr, hook ? u"WH_KEYBOARD_LL set"_s : u"WH_KEYBOARD_LL failed, error %1"_s.arg(hookError)));

    // The clipboard: opened and read, never written.
    const bool opened = ::OpenClipboard(nullptr);
    const DWORD openError = opened ? 0 : ::GetLastError();
    UINT formats = 0;
    if (opened) {
        for (UINT f = ::EnumClipboardFormats(0); f; f = ::EnumClipboardFormats(f))
            ++formats;
        ::CloseClipboard();
    }
    rows.append(row(u"clipboard"_s, opened && formats > 0,
        opened ? u"clipboard opened, %1 format(s) on it"_s.arg(formats) : u"OpenClipboard failed, error %1"_s.arg(openError)));

    // Where it may write: its data folder.
    const QString dataDir = qEnvironmentVariable("WS_DATA_DIR");
    rows.append(row(u"data"_s, !dataDir.isEmpty(), u"WS_DATA_DIR %1"_s.arg(dataDir)));
    return rows;
}

} // namespace

extern "C" __declspec(dllexport) int winshunCaptureMain(const wchar_t* appDirName)
{
    const QString appDir = QString::fromWCharArray(appDirName);
    QCoreApplication::setLibraryPaths({appDir}); // platforms\qwindows.dll and the rest, Win顺's
    int argc = 1;
    char name[] = "WinShunCapture";
    char* argv[] = {name, nullptr};
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QQmlEngine engine;
    engine.addImportPath(appDir + u"/qml"_s);

    // The protocol's lines, read on a thread of their own and handled here.
    std::thread([&app] {
        std::string pending;
        char buffer[65536];
        DWORD got = 0;
        while (::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof buffer, &got, nullptr) && got > 0) {
            pending.append(buffer, got);
            for (std::size_t nl = pending.find('\n'); nl != std::string::npos; nl = pending.find('\n')) {
                const QByteArray line(pending.data(), static_cast<qsizetype>(nl));
                pending.erase(0, nl + 1);
                QMetaObject::invokeMethod(&app, [line] {
                    const QJsonObject message = QJsonDocument::fromJson(line).object();
                    const QString method = message.value(u"method"_s).toString();
                    const QJsonValue id = message.value(u"id"_s);
                    if (id.isUndefined())
                        return; // a notification: cancel, settingsChanged
                    if (method == u"initialize")
                        respond(id, {{u"protocolVersion"_s, u"1"_s}, {u"name"_s, u"winshun.capture probe"_s}});
                    else if (method == u"query")
                        respond(id, {{u"items"_s, checks(*qobject_cast<QQmlEngine*>(qApp->property("probeEngine").value<QObject*>()))}});
                    else if (method == u"shutdown") {
                        respond(id, {});
                        qApp->quit();
                    } else
                        respond(id, {{u"effects"_s, QJsonArray()}});
                });
            }
        }
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit); // Win顺 has gone
    }).detach();
    app.setProperty("probeEngine", QVariant::fromValue(static_cast<QObject*>(&engine)));
    return app.exec();
}

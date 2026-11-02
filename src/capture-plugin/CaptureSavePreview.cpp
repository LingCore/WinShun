// Isolated native save-dialog and split-button regression, over generated pixels.
#include "CapturePreview.h"
#include "Capture.h"

#include <QDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QTranslator>

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::captureplugin::preview {
namespace {

std::vector<HWND> ownDialogs()
{
    std::vector<HWND> result;
    ::EnumWindows([](HWND window, LPARAM data) -> BOOL {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(window, &pid);
        wchar_t name[64] {};
        ::GetClassNameW(window, name, 64);
        if (pid == ::GetCurrentProcessId() && ::IsWindowVisible(window) && ::wcscmp(name, L"#32770") == 0)
            reinterpret_cast<std::vector<HWND>*>(data)->push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

HWND fileNameEdit(HWND dialog)
{
    // Same native hierarchy as platform/FileDialog.cpp's fileNameBox;
    // FloatNotifySink excludes the address and search fields on modern Windows.
    const HWND view = ::FindWindowExW(dialog, nullptr, L"DUIViewWndClassName", nullptr);
    const HWND content = view ? ::FindWindowExW(view, nullptr, L"DirectUIHWND", nullptr) : nullptr;
    for (HWND sink = content ? ::FindWindowExW(content, nullptr, L"FloatNotifySink", nullptr) : nullptr;
         sink; sink = ::FindWindowExW(content, sink, L"FloatNotifySink", nullptr)) {
        const HWND combo = ::FindWindowExW(sink, nullptr, L"ComboBox", nullptr);
        if (const HWND edit = combo ? ::FindWindowExW(combo, nullptr, L"Edit", nullptr) : nullptr)
            return edit;
    }
    return ::GetDlgItem(dialog, 0x480);
}

bool samePixels(const QImage& a, const QImage& b)
{
    return !a.isNull() && a.convertToFormat(QImage::Format_RGBA8888) == b.convertToFormat(QImage::Format_RGBA8888);
}

} // namespace

int runCaptureSave(const QStringList& args)
{
    using namespace capture;
    const QDir out(args.value(args.indexOf(u"--preview-capture-tools"_s) + 1));
    const QString oldFolder = out.filePath(u"old-path"_s);
    const QString chosenFolder = out.filePath(u"chosen-path"_s);
    QDir().mkpath(oldFolder);
    QDir().mkpath(chosenFolder);
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/capture_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    const auto pump = [](int ms) {
        QElapsedTimer clock;
        clock.start();
        do {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(5);
        } while (clock.elapsed() < ms);
    };
    const auto waitFor = [&](const std::function<bool()>& ready) {
        QElapsedTimer clock;
        clock.start();
        while (!ready() && clock.elapsed() < 10000)
            pump(20);
        return ready();
    };
    bool ok = true;
    const auto check = [&](bool passed, const char* what) {
        note(u"%1: %2"_s.arg(passed ? u"PASS"_s : u"FAIL"_s, QString::fromUtf8(what)));
        ok = passed && ok;
    };
    QQmlApplicationEngine engine;
    QQmlComponent fixtureComponent(&engine);
    fixtureComponent.setData(R"(
        import QtQuick
        Window {
            property bool alternate: false
            color: alternate ? "#c9e6db" : "#e8edf7"
            flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
            Text { x: 60; y: 50; text: "截图保存验证 / Save screenshot"; color: "#243044"; font.pixelSize: 24 }
        }
    )", QUrl(u"qrc:/capture-save-fixture.qml"_s));
    std::unique_ptr<QQuickWindow> fixture(qobject_cast<QQuickWindow*>(fixtureComponent.create()));
    if (!fixture) { note(fixtureComponent.errorString()); return 3; }
    fixture->showFullScreen();
    pump(200);
    QStringList savedPaths;
    int recorded = 0;
    int copied = 0;
    Capture::Callbacks callbacks;
    callbacks.saveFolder = [&] { return oldFolder; };
    callbacks.saved = [&](const QString& path, const QString& error) {
        check(error.isEmpty(), "asynchronous PNG save succeeded");
        savedPaths.append(path);
    };
    callbacks.record = [&](const QImage&, const QByteArray&) { ++recorded; };
    callbacks.copy = [&](const QImage&, const QByteArray&, const QString&) { ++copied; };
    Capture capture(&engine, std::move(callbacks));
    CaptureSession* session = nullptr;
    CaptureScreen* screen = nullptr;
    QQuickWindow* overlay = nullptr;
    const auto begin = [&] {
        capture.trigger();
        return waitFor([&] {
            for (QWindow* window : QGuiApplication::topLevelWindows()) {
                auto* model = window->property("screenModel").value<CaptureScreen*>();
                auto* state = window->property("session").value<CaptureSession*>();
                auto* quick = qobject_cast<QQuickWindow*>(window);
                if (model && state && quick && state->active() && model->hasImage()
                    && quick->isActive() && quick->activeFocusItem()) {
                    session = state;
                    screen = model;
                    overlay = quick;
                    return true;
                }
            }
            return false;
        });
    };
    const auto key = [&](int code, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QKeyEvent press(QEvent::KeyPress, code, mods);
        QCoreApplication::sendEvent(overlay, &press);
        QKeyEvent release(QEvent::KeyRelease, code, mods);
        QCoreApplication::sendEvent(overlay, &release);
        pump(40);
    };
    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, overlay->mapToGlobal(point.toPoint()), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(overlay, &event);
        pump(20);
    };
    const auto dragPixels = [&](QPoint first, QPoint last) {
        const QPointF a(first.x() / screen->dpr(), first.y() / screen->dpr());
        const QPointF b(last.x() / screen->dpr(), last.y() / screen->dpr());
        mouse(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
    };
    const auto click = [&](QQuickItem* item) {
        if (!item) { check(false, "requested QML save control exists"); return; }
        const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    };
    const auto select = [&] { dragPixels({160, 220}, {579, 459}); };
    bool pickerArmed = false;
    bool cancelPicker = true;
    bool pickerSeen = false;
    bool pickerHandled = false;
    bool overwriteSeen = false;
    std::vector<HWND> observedDialogs;
    HWND nativePicker = nullptr;
    QString destination;
    QElapsedTimer pickerClock;
    QTimer driver;
    driver.setInterval(50);
    QObject::connect(&driver, &QTimer::timeout, &driver, [&] {
        if (!pickerArmed)
            return;
        if (pickerClock.elapsed() > 8000) {
            pickerArmed = false;
            check(false, "native dialog automation timed out");
            for (HWND dialog : ownDialogs()) {
                ::PostMessageW(dialog, TDM_CLICK_BUTTON, IDNO, 0);
                ::PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
                ::PostMessageW(dialog, WM_COMMAND, IDNO, 0);
            }
            return;
        }
        const std::wstring title = Capture::tr("保存截图").toStdWString();
        for (HWND dialog : ownDialogs()) {
            if (std::ranges::find(observedDialogs, dialog) == observedDialogs.end()) {
                observedDialogs.push_back(dialog);
                wchar_t caption[256] {};
                ::GetWindowTextW(dialog, caption, 256);
                note(u"native dialog: %1; hwnd=%2; owner=%3; yes=%4; ok=%5"_s.arg(QString::fromWCharArray(caption))
                    .arg(reinterpret_cast<quintptr>(dialog)).arg(reinterpret_cast<quintptr>(::GetWindow(dialog, GW_OWNER)))
                    .arg(reinterpret_cast<quintptr>(::GetDlgItem(dialog, IDYES))).arg(reinterpret_cast<quintptr>(::GetDlgItem(dialog, IDOK))));
            }
            wchar_t caption[256] {};
            ::GetWindowTextW(dialog, caption, 256);
            if (nativePicker && ::GetWindow(dialog, GW_OWNER) == nativePicker && QFileInfo::exists(destination)
                && (::wcscmp(caption, L"确认另存为") == 0 || ::wcscmp(caption, L"Confirm Save As") == 0)) {
                overwriteSeen = true;
                // Current Windows uses a TaskDialog with no child HWND for Yes.
                ::PostMessageW(dialog, ::GetDlgItem(dialog, IDYES) ? WM_COMMAND : TDM_CLICK_BUTTON, IDYES, 0);
                continue;
            }
            if (title != caption)
                continue;
            if (!pickerSeen) {
                pickerSeen = true;
                nativePicker = dialog;
                check(::GetWindow(dialog, GW_OWNER) == reinterpret_cast<HWND>(overlay->winId()), "native picker belongs to the capture overlay");
                capture.trigger();
                session->act(static_cast<int>(CaptureSession::Action::Save));
                check(session->active(), "repeated save/hotkey does not close the capture or open another picker");
            }
            if (pickerHandled)
                continue;
            if (pickerClock.elapsed() < 500)
                continue; // Wait for the dialog's controls and first frame, not just its HWND.
            if (cancelPicker || pickerClock.elapsed() > 8000) {
                pickerHandled = true;
                fixture->screen()->grabWindow(reinterpret_cast<WId>(dialog)).save(out.filePath(u"native-save-dialog.png"_s));
                ::PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
            } else if (const HWND edit = fileNameEdit(dialog)) {
                pickerHandled = true;
                const std::wstring path = QDir::toNativeSeparators(destination).toStdWString();
                ::SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path.c_str()));
                note(u"native filename field located and set"_s);
                ::PostMessageW(dialog, WM_COMMAND, IDOK, 0);
            }
        }
    });
    driver.start();
    const auto arm = [&](bool cancel, const QString& path = {}) {
        cancelPicker = cancel;
        destination = path;
        pickerSeen = pickerHandled = overwriteSeen = false;
        observedDialogs.clear();
        nativePicker = nullptr;
        pickerClock.restart();
        pickerArmed = true;
    };
    if (!begin()) { check(false, "real capture is focused"); return 4; }
    select();
    key(Qt::Key_1);
    dragPixels({220, 280}, {380, 350});
    key(Qt::Key_1);
    const QRect selected = session->selectionRect();
    const QImage expected = session->annotation()->composed();
    check(session->hasAnnotations(), "save includes an actual annotation");
    arm(true);
    key(Qt::Key_S, Qt::ControlModifier);
    pickerArmed = false;
    check(pickerSeen && pickerHandled && session->active() && session->selectionRect() == selected
        && session->hasAnnotations() && session->selectionHistoryCount() == 0 && savedPaths.isEmpty() && recorded == 0,
        "Ctrl+S opens native Save As; Cancel keeps selection and annotations without recording or saving");
    const QString chosen = chosenFolder + u"/自选截图.png"_s;
    arm(false, chosen);
    click(overlay->findChild<QQuickItem*>(u"captureSaveButton"_s));
    pickerArmed = false;
    check(pickerSeen && waitFor([&] { return !capture.busy() && savedPaths.size() == 1; }), "save button opens Save As and completes");
    check(!savedPaths.isEmpty() && QFileInfo(savedPaths.last()).absoluteFilePath() == QFileInfo(chosen).absoluteFilePath()
        && samePixels(QImage(chosen), expected), "chosen Unicode PNG contains exact annotated pixels");
    check(QDir(oldFolder).entryList(QDir::Files).isEmpty(), "Save As leaves the configured old folder unchanged");
    fixture->setProperty("alternate", true);
    pump(100);
    if (!begin()) { check(false, "capture reopens for overwrite"); return 4; }
    select();
    const QImage replacement = screen->image().copy(session->selectionRect());
    arm(false, chosen);
    key(Qt::Key_S, Qt::ControlModifier);
    pickerArmed = false;
    check(pickerSeen && overwriteSeen && waitFor([&] { return !capture.busy() && savedPaths.size() == 2; })
        && samePixels(QImage(chosen), replacement), "native overwrite confirmation saves new pixels to the chosen file");
    if (!begin()) { check(false, "capture reopens for the dropdown"); return 4; }
    select();
    auto* arrow = overlay->findChild<QQuickItem*>(u"captureSaveMenuButton"_s);
    QObject* menu = overlay->findChild<QObject*>(u"captureSaveMenu"_s);
    auto* previous = overlay->findChild<QQuickItem*>(u"captureSavePreviousItem"_s);
    if (!arrow || !menu || !previous) { check(false, "split save menu exists"); return 4; }
    click(arrow);
    check(menu->property("visible").toBool(), "arrow opens the menu");
    pump(100);
    check(overlay->grabWindow().save(out.filePath(u"save-menu.png"_s)), "saved actual dropdown screenshot");
    click(arrow);
    check(!menu->property("visible").toBool(), "second arrow click closes the menu");
    click(arrow);
    key(Qt::Key_Escape);
    check(!menu->property("visible").toBool() && session->hasSelection(), "Escape closes only the menu");
    const QRect beforeNudge = session->selectionRect();
    key(Qt::Key_W);
    check(session->selectionRect() == beforeNudge.translated(0, -1), "WASD still works after closing the menu");
    key(Qt::Key_S);
    click(arrow);
    QQuickItem* copyButton = nullptr;
    for (QQuickItem* item : overlay->findChildren<QQuickItem*>())
        if (item->property("primary").toBool()) { copyButton = item; break; }
    click(copyButton);
    check(!menu->property("visible").toBool() && session->active() && copied == 0, "outside click dismisses the menu without activating Copy");
    const QImage quickExpected = screen->image().copy(session->selectionRect());
    arm(true); // A mistaken Save As is cancelled and fails the no-picker assertion.
    click(arrow);
    click(previous);
    pickerArmed = false;
    check(!pickerSeen && waitFor([&] { return !capture.busy() && savedPaths.size() == 3; }), "Save to previous folder saves directly without a picker");
    check(QDir(oldFolder).entryList({u"*.png"_s}, QDir::Files).size() == 1
        && samePixels(QImage(savedPaths.value(2)), quickExpected), "old-folder PNG contains the exact selected pixels");
    check(recorded == 3 && copied == 0, "only confirmed saves enter history; clipboard remains untouched");
    driver.stop();
    fixture->hide();
    note(u"capture save checks: %1"_s.arg(ok ? u"passed"_s : u"failed"_s));
    return ok ? 0 : 4;
}

} // namespace ws::captureplugin::preview

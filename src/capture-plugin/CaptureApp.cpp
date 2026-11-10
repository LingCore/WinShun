#include "CaptureApp.h"

#include "PresenterHost.h"
#include "platform/OcrFiles.h"

#include "Protocol.h"

#include "Capture.h"
#include "CaptureEditor.h"
#include "CaptureOcr.h"
#include "Geometry.h"
#include "HotkeyText.h"
#include "IdleTrim.h"
#include "IdleGraphics.h"
#include "ImageOrigin.h"
#include "Keycaps.h"
#include "PictureFile.h"
#include "PinManager.h"
#include "PinImage.h"
#include "PinWindow.h"
#include "PrintScreen.h"
#include "RawClicks.h"
#include "RecordingPointer.h"
#include "SavedFile.h"
#include "SystemTheme.h"
#include "TrayMenu.h"
#include "WindowFrame.h"
#include "platform/WindowEffects.h"
#include "platform/Foreground.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QLocale>
#include <QMimeData>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>
#include <QTranslator>

#include <dwmapi.h>
#include <objbase.h>

#include <algorithm>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws::captureplugin {

namespace {

constexpr wchar_t kWindowClass[] = L"WinShun.Capture.Messages";
enum Hotkey : int { CaptureKey = 1, PrintScreenKey, PinsKey, PinsRestoreKey, LastPinKey, PresentKey, OcrKey, ProbeKey = 0x0BFF };

bool isDarkMode()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

// Before create(): see-through where the window's background is, for the
// Mica behind it (App's prepareBackdrop: the alpha stays in the format).
void prepareBackdrop(QQuickWindow* window)
{
    if (!SystemTheme::backdropAvailable())
        return;
    const auto keepAlpha = [window] {
        QSurfaceFormat format = window->requestedFormat();
        if (format.alphaBufferSize() < 8) {
            format.setAlphaBufferSize(8);
            window->setFormat(format);
        }
    };
    keepAlpha();
    QObject::connect(window, &QQuickWindow::colorChanged, window, keepAlpha);
}

// The window's client area on the screen, physical: where the editing
// window's canvas is measured from (its title bar is its own).
QRect clientRectOf(QQuickWindow* window)
{
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    RECT client {};
    POINT origin {0, 0};
    if (!::GetClientRect(hwnd, &client) || !::ClientToScreen(hwnd, &origin))
        return {};
    return QRect(origin.x, origin.y, client.right, client.bottom);
}

void moveClientTo(QQuickWindow* window, QPoint topLeft)
{
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    RECT outer {};
    const QRect client = clientRectOf(window);
    if (client.isEmpty() || !::GetWindowRect(hwnd, &outer))
        return;
    const QPoint shift = client.topLeft() - QPoint(outer.left, outer.top);
    ::SetWindowPos(hwnd, nullptr, topLeft.x() - shift.x(), topLeft.y() - shift.y(), 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// A history picture: a long capture is far above Qt's default limit of 256 MB.
QImage readPicture(const QString& path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    reader.setAllocationLimit(2048);
    QImage image = reader.read();
    if (image.isNull()) // not a PNG (Win顺's folder has no Qt image plugins): Windows' own codecs
        image = decodePictureFile(path, 0);
    return image;
}

// A picture for Win顺 to take into the clipboard history: its pixels as they
// are, behind a small header (no PNG to encode here and decode there). Marked
// temporary, so Windows keeps it in memory: Win顺 reads and deletes it at once.
bool writePicture(const QString& path, const QImage& image)
{
    const HANDLE file = ::CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const qint32 header[6] {0x474D4957 /* "WIMG" */, 1, pixels.width(), pixels.height(), static_cast<qint32>(pixels.bytesPerLine()),
        static_cast<qint32>(pixels.format())};
    DWORD written = 0;
    bool ok = ::WriteFile(file, header, sizeof header, &written, nullptr) && written == sizeof header;
    const uchar* bits = pixels.constBits();
    qsizetype left = pixels.sizeInBytes();
    while (ok && left > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<qsizetype>(left, 64 << 20));
        ok = ::WriteFile(file, bits, chunk, &written, nullptr) && written == chunk;
        bits += chunk;
        left -= chunk;
    }
    ::CloseHandle(file);
    if (!ok)
        QFile::remove(path);
    return ok;
}

QString hotkeyProblem(const QString& hotkey)
{
    UINT modifiers = 0;
    UINT vk = 0;
    return hotkeys::parse(hotkey, &modifiers, &vk)
        ? QCoreApplication::translate("ws::captureplugin::CaptureApp", "“%1” 已被其他程序或系统占用，请换一个。").arg(hotkey)
        : QCoreApplication::translate("ws::captureplugin::CaptureApp", "无法识别“%1”，请重新设置。").arg(hotkey);
}

QJsonObject menuItem(const QString& id, const QString& title, bool checked = false, bool enabled = true)
{
    QJsonObject item {{u"id"_s, id}, {u"title"_s, title}};
    if (checked)
        item.insert(u"checked"_s, true);
    if (!enabled)
        item.insert(u"enabled"_s, false);
    return item;
}

QJsonObject separator()
{
    return {{u"separator"_s, true}};
}

QString withKey(const QString& text, const QString& hotkey)
{
    return hotkey.isEmpty() ? text : text + u'\t' + hotkey;
}

} // namespace

CaptureApp::CaptureApp(Protocol& protocol, QString appDir, QString dataDir, QObject* parent)
    : QObject(parent)
    , m_protocol(protocol)
    , m_appDir(std::move(appDir))
    , m_dataDir(std::move(dataDir))
    , m_settings(m_dataDir + u"/capture.ini"_s)
{
    m_qml.addImportPath(m_appDir + u"/qml"_s); // QtQuick and the rest: Win顺's
    // This process's own Theme works out the look the pins are drawn by (the
    // same Look files as Win顺's, so an older Win顺 makes no difference).
    SystemTheme::onNativeLookChanged(this, [this] {
        if (m_pins)
            m_pins->restyle();
    });
    capture::CaptureOcr::setProgramFolder(m_appDir); // onnxruntime.dll and ocr\: Win顺's
    QString componentRoot = qEnvironmentVariable("WS_COMPONENTS_DIR");
    if (componentRoot.isEmpty())
        componentRoot = QFileInfo(m_dataDir).fileName() == u"winshun.capture.data"
            ? QDir(m_dataDir).absoluteFilePath(u"../../components"_s) : m_dataDir + u"/components"_s;
    ocr::setComponentsRoot(componentRoot);
    QDir().mkpath(componentRoot);
    m_ocrWatcher.addPath(componentRoot);
    connect(&m_ocrWatcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (m_captureOcr) {
            m_captureOcr->retry();
            if (m_resumeOcr && !m_captureOcr->needsPackage() && m_ocrWindow) {
                m_resumeOcr = false;
                m_ocrWindow->show();
                m_ocrWindow->raise();
                win::bringToFront(m_ocrWindow);
            }
        }
    });
    QDir(m_dataDir).mkpath(u"out"_s);

    WNDCLASSW type {};
    type.lpfnWndProc = &CaptureApp::windowProc;
    type.hInstance = ::GetModuleHandleW(nullptr);
    type.lpszClassName = kWindowClass;
    ::RegisterClassW(&type);
    m_window = ::CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, nullptr);
    ::SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    m_idleTrim = std::make_unique<IdleTrim>([this]() -> const char* {
        // A recording's hooks and media thread, a delayed or long screenshot:
        // their pages are touched all the time (see App).
        return m_capture && m_capture->busy() ? "capturing or recording" : nullptr;
    }, [] { return false; });

    m_applyLater.setSingleShot(true);
    m_applyLater.setInterval(0);
    connect(&m_applyLater, &QTimer::timeout, this, &CaptureApp::apply);
    connect(&m_settings, &CaptureSettings::changed, &m_applyLater, qOverload<>(&QTimer::start));
    connect(&m_settings, &CaptureSettings::recordingHotkeyChanged, this, &CaptureApp::applyHotkeys);
    // A key for the capture or a pin that a global hotkey holds would never
    // reach them: asked by taking it for a moment (the plugin's own are let
    // go of while a key box records, and compared by name first).
    m_settings.setHotkeyProbe([this](unsigned modifiers, unsigned vk) {
        if (!::RegisterHotKey(m_window, ProbeKey, modifiers | MOD_NOREPEAT, vk))
            return ::GetLastError() != ERROR_HOTKEY_ALREADY_REGISTERED; // only a key held is refused
        ::UnregisterHotKey(m_window, ProbeKey);
        return true;
    });
    connect(&m_protocol, &Protocol::requested, this, &CaptureApp::requested);
    connect(&m_protocol, &Protocol::notified, this, &CaptureApp::notified);
    connect(&m_protocol, &Protocol::closed, this, [this] {
        shutdown();
        QCoreApplication::quit();
    });
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        const bool dark = isDarkMode();
        win::setMenuTheme(dark);
        for (QQuickWindow* window : {m_editWindow.data(), m_ocrWindow.data(), m_bubbleWindow.data(), m_settingsWindow.data()}) {
            if (window)
                win::setDarkFrame(window, dark);
        }
    });
}

CaptureApp::~CaptureApp()
{
    m_clicks.reset();
    m_capture.reset(); // its overlays, before the QML engine they were made with
    m_pins.reset();
    if (m_editWindow) {
        if (m_editor) // its destruction hides it: the controller must not answer that
            QObject::disconnect(m_editWindow.data(), nullptr, m_editor.get(), nullptr);
        delete m_editWindow.data();
    }
    m_editor.reset();
    m_editFrame.reset();
    closeCaptureOcr(true);
    delete m_bubbleWindow.data();
    m_bubbleFrame.reset();
    delete m_settingsWindow.data();
    if (m_window)
        ::DestroyWindow(m_window);
}

void CaptureApp::start(const QJsonObject& appearance, bool hostHotkeys)
{
    m_hostHotkeys = hostHotkeys;
    applyAppearance(appearance);
    m_started = true;
    apply();
}

// These windows are kept between openings: QWindow::close would destroy
// the HWND and detach their WindowFrame.
bool CaptureApp::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Close && (watched == m_ocrWindow || watched == m_settingsWindow)) {
        event->ignore();
        if (watched == m_ocrWindow)
            closeCaptureOcr();
        else
            m_settingsWindow->hide();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

// ---- the protocol ---------------------------------------------------------------

void CaptureApp::requested(const QJsonValue& id, const QString& method, const QJsonObject& params)
{
    if (method == u"query") {
        // "cap": what the tray menu's 截图 offers, from the search box.
        const QString text = params.value(u"text"_s).toString().trimmed();
        QJsonArray items;
        if (text == u"__selftest") { // whether each window's QML loads on this Win顺's Qt (made hidden, then let go of)
            for (const QString& type : {u"EditWindow"_s, u"OcrPanel"_s, u"OcrBubble"_s, u"CaptureSettingsWindow"_s, u"LongCapturePanel"_s, u"RecordingPanel"_s,
                     u"PresenterOverlay"_s, u"PresenterToolbar"_s}) {
                QQmlComponent component(&m_qml, u"WinShun"_s, type);
                const QString problem = component.isError() ? component.errorString().trimmed() : QString();
                items.append(QJsonObject {{u"id"_s, type}, {u"title"_s, (problem.isEmpty() ? u"ok  "_s : u"FAIL  "_s) + type},
                    {u"subtitle"_s, problem.isEmpty() ? u"Qt %1"_s.arg(QString::fromLatin1(qVersion())) : problem}, {u"valid"_s, false}});
            }
            m_protocol.respond(id, QJsonObject {{u"items"_s, items}});
            return;
        }
        // aliases: other words that find the item (the title still does), space separated, either language.
        const auto add = [&](const QString& itemId, const QString& title, const QString& subtitle, const QString& glyph, const QString& aliases = {}) {
            if (!text.isEmpty() && !title.contains(text, Qt::CaseInsensitive) && !aliases.contains(text, Qt::CaseInsensitive))
                return;
            items.append(QJsonObject {{u"id"_s, itemId}, {u"title"_s, title}, {u"subtitle"_s, subtitle},
                {u"icon"_s, u"glyph:U+"_s + glyph}});
        };
        const QString key = !m_settings.options().hotkey.isEmpty() ? m_settings.options().hotkey
            : m_settings.options().printScreen                         ? u"PrtScn"_s
                                                                       : QString();
        add(u"start"_s, tr("开始截图"), key.isEmpty() ? tr("框选窗口或区域") : tr("快捷键 %1").arg(key), u"E722"_s,
            u"截图 截屏 截取 screenshot capture shot snip"_s);
        add(u"ocr"_s, tr("识别屏幕文字"),
            m_settings.options().ocrHotkey.isEmpty() ? tr("框选一块屏幕，识别出的文字直接复制") : tr("快捷键 %1").arg(m_settings.options().ocrHotkey),
            u"E8D2"_s, u"取字 识别 文字 提取文字 复制文字 ocr text recognize extract"_s);
        add(u"delay"_s, tr("%1 秒后截图").arg(m_settings.options().delay), tr("先打开要截的菜单或提示"), u"E916"_s, u"延时 延迟 定时 delay timer later"_s);
        add(u"last"_s, tr("贴图"), tr("优先剪贴板图片，否则贴回最近截图并保留标注"), u"E718"_s, u"贴 钉 贴图 pin paste sticker"_s);
        add(u"present"_s, presenterOpen() ? tr("结束演示") : tr("演示 / 白板"),
            m_settings.options().presentHotkey.isEmpty() ? tr("在屏幕上画、激光笔、白板黑板") : tr("快捷键 %1").arg(m_settings.options().presentHotkey),
            u"E70F"_s, u"演示 白板 黑板 激光笔 画笔 present presenter whiteboard blackboard draw pen laser"_s);
        add(u"settings"_s, tr("截图设置"), tr("快捷键、鼠标、贴图、打码和工具条"), u"E713"_s, u"设置 选项 配置 快捷键 settings options preferences config hotkey"_s);
        m_protocol.respond(id, QJsonObject {{u"items"_s, items}});
    } else if (method == u"action") {
        const QString item = params.value(u"item"_s).toObject().value(u"id"_s).toString();
        m_protocol.respond(id, QJsonObject {{u"effects"_s, QJsonArray {QJsonObject {{u"type"_s, u"hide"_s}}}}});
        // Once the launcher is gone from the screen: not in the picture.
        QTimer::singleShot(200ms, this, [this, item] {
            if (item == u"settings")
                showSettings();
            else if (m_capture && item == u"delay")
                m_capture->startDelayed(m_settings.options().delay);
            else if (item == u"last")
                pinCurrentImage();
            else if (item == u"present")
                togglePresenter();
            else if (item == u"ocr")
                startOcr();
            else if (m_capture)
                m_capture->trigger();
        });
    } else if (method == u"trayMenu") {
        m_protocol.respond(id, QJsonObject {{u"items"_s, trayMenu()}});
    } else if (method == u"picture") { // "pictures" in plugin.json
        const QString action = params.value(u"action"_s).toString();
        const QString file = params.value(u"file"_s).toString();
        if (action == u"pin")
            pinPicture(file, params.value(u"screenshot"_s).toBool());
        else if (action == u"edit")
            editPicture(params.value(u"clipId"_s).toInteger(-1), file, params.value(u"project"_s).toString());
        m_protocol.respond(id, QJsonObject {});
    } else if (method == u"showSettings") {
        showSettings();
        m_protocol.respond(id, QJsonObject {});
    } else if (method == u"shutdown") {
        shutdown();
        m_protocol.respond(id, QJsonObject {});
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    } else {
        m_protocol.respondError(id, -32601, u"Unknown method: "_s + method);
    }
}

void CaptureApp::notified(const QString& method, const QJsonObject& params)
{
    if (method == u"hotkey" && m_hostHotkeys && !m_settings.recordingHotkey()) {
        const QString key = hotkeys::normalized(params.value(u"key"_s).toString());
        const auto& o = m_settings.options();
        const std::pair<int, QString> shortcuts[] {{CaptureKey, o.hotkey}, {PinsKey, o.pinsHotkey},
            {PinsRestoreKey, o.pinsRestoreHotkey}, {LastPinKey, o.lastPinHotkey}, {PresentKey, o.presentHotkey},
            {PrintScreenKey, o.printScreen ? u"PrintScreen"_s : QString()}, {OcrKey, o.ocrHotkey}};
        for (const auto& [id, shortcut] : shortcuts) {
            if (!key.isEmpty() && key == hotkeys::normalized(shortcut)) {
                hotkey(id);
                break;
            }
        }
    } else if (method == u"menu")
        menuChosen(params.value(u"id"_s).toString());
    else if (method == u"appearance")
        applyAppearance(params);
}

// On the way out: PrtScn goes back to the Snipping Tool (taken again at the
// next start), the hotkeys and the pins go with the process.
void CaptureApp::shutdown()
{
    ++m_hotkeyRevision;
    if (m_hostHotkeys)
        m_protocol.tool(u"register_hotkeys"_s, {{u"keys"_s, QJsonArray()}});
    if (m_presenter)
        m_presenter->presenter().close();
    for (int id = CaptureKey; id <= OcrKey; ++id)
        ::UnregisterHotKey(m_window, id);
    const QString before = m_settings.options().printScreenBefore;
    std::optional<DWORD> value;
    if (!before.isEmpty() && (!printscreen::decode(before, &value) || printscreen::write(value)))
        m_settings.update([](CaptureOptions& o) { o.printScreenBefore.clear(); });
    m_applyLater.stop();
}

// ---- what App did -------------------------------------------------------------------

void CaptureApp::apply()
{
    if (!m_started)
        return;
    const CaptureOptions& o = m_settings.options();
    if (!m_capture) {
        capture::Capture::Callbacks callbacks;
        callbacks.copy = [this](const QImage& image, const QByteArray& project, const QString& origin) {
            copyImage(image, project, true, true, origin);
        };
        // Ctrl+S and F3: in the screenshot history too, the clipboard left alone.
        callbacks.record = [this](const QImage& image, const QByteArray& project) { copyImage(image, project, false, true); };
        callbacks.annotateStyles = [this] { return m_settings.options().annotateStyles; };
        callbacks.annotateStylesChanged = [this](const QString& json) {
            if (json != m_settings.options().annotateStyles)
                m_settings.update([&](CaptureOptions& next) { next.annotateStyles = json; });
        };
        callbacks.copyColor = [this](const QString& hex) { copyText(hex); };
        callbacks.saveFolder = [this] { return m_settings.options().saveFolder; };
        callbacks.saved = [this](const QString& path, const QString& error) { notifySaved(path, error); };
        callbacks.ended = [this] { m_idleTrim->poke(); };
        callbacks.ocr = [this](const QImage& image, double dpr, const QRect& where) { showCaptureOcr(image, dpr, where); };
        callbacks.ocrThen = [this](const QImage& image, double dpr, const QRect& where, capture::CaptureSession::Action then) {
            using Action = capture::CaptureSession::Action;
            captureOcrThen(image, dpr, where,
                then == Action::OcrCopy ? OcrThen::Copy : then == Action::OcrSearch ? OcrThen::Search : OcrThen::Web);
        };
        callbacks.readText = [this](const QImage& image, double dpr, const QRect& where) { readScreenText(image, dpr, where); };
        // Not in the frozen picture: hidden, and one composition waited for, so
        // the grab right after does not still find it on the screen.
        callbacks.starting = [this] {
            if (closeOcrBubble())
                ::DwmFlush();
        };
        callbacks.pins = [this] { return &pins(); };
        callbacks.smart = smartHooks();
        // 长截图: straight into the editing window (Enter copies, Ctrl+S saves, F3 pins).
        callbacks.longCapture = [this](const QImage& picture) {
            capture::CaptureEditor* editor = this->editor();
            if (!editor || !editor->openImage(picture))
                notify(tr("长截图"), tr("没能打开编辑窗口。"));
        };
        callbacks.copyText = [this](const QString& text) { copyText(text); };
        callbacks.qr = [this](const QStringList& codes, const QRect& where) { showCaptureOcr(QImage(), 1.0, where, &codes); };
        callbacks.colorFormatChanged = [this](const QString& name) {
            if (name != m_settings.options().colorFormat)
                m_settings.update([&](CaptureOptions& next) { next.colorFormat = name; });
        };
        callbacks.keyHintsKept = [this](bool on) { m_settings.setCaptureKeyHints(on); }; // the panel's 不再显示 / 一直显示
        callbacks.recording.preferences = [this] {
            const CaptureOptions& p = m_settings.options();
            return capture::Recorder::Preferences {p.recordingFolder, p.recordingSystemAudio, p.recordingMicrophone,
                static_cast<capture::Recorder::Quality>(p.recordingQuality), p.recordingCursor, p.recordingCountdown,
                p.recordingClicks, static_cast<capture::Recorder::Format>(p.recordingFormat),
                static_cast<capture::Recorder::KeyDisplay>(capture::keycaps::modeFromName(p.recordingKeys)),
                p.recordingMouseButtons};
        };
        callbacks.recording.remember = [this](const capture::Recorder::Preferences& prefs) {
            m_settings.update([&](CaptureOptions& next) {
                next.recordingFolder = prefs.folder;
                next.recordingSystemAudio = prefs.systemAudio;
                next.recordingMicrophone = prefs.microphone;
                next.recordingQuality = prefs.quality;
                next.recordingCursor = prefs.cursor;
                next.recordingCountdown = prefs.countdown;
                next.recordingClicks = prefs.clickHighlights;
                next.recordingFormat = prefs.format;
                next.recordingKeys = capture::keycaps::modeName(static_cast<capture::keycaps::Mode>(prefs.keys));
                next.recordingMouseButtons = prefs.mouseButtons;
            });
        };
        callbacks.recording.finished = [this] { m_idleTrim->poke(); };
        callbacks.recording.clicks = [this](std::shared_ptr<capture::recording::ClickStream> input) {
            m_clicks.reset();
            if (!input)
                return true;
            m_clicks = std::make_unique<RawClicks>(std::move(input));
            if (!m_clicks->active())
                m_clicks.reset();
            return m_clicks != nullptr;
        };
        // The screenshot's hotkey and PrtScn (taken over) end a recording
        // (Capture::trigger): their keys are not shown in it (GitHub #98).
        callbacks.recording.ownKeys = [this] {
            std::vector<capture::keycaps::Hotkey> keys;
            UINT modifiers = 0;
            UINT vk = 0;
            if (hotkeys::parse(m_settings.options().hotkey, &modifiers, &vk))
                keys.push_back(capture::keycaps::fromRegistered(modifiers, vk));
            if (m_settings.options().printScreen)
                keys.push_back({0, VK_SNAPSHOT});
            return keys;
        };
        callbacks.accent = [this] {
            ensureTheme();
            return SystemTheme::nativeColor("accentFill", SystemTheme::accentColor());
        };
        callbacks.panelRadius = [this] {
            ensureTheme();
            return SystemTheme::nativeReal("menuRadius", win::roundedCorners() ? 8 : 0);
        };
        m_capture = std::make_unique<capture::Capture>(&m_qml, std::move(callbacks));
    }
    m_capture->setCopyOnSelect(o.copyOnSelect);
    m_capture->setMagnifier(o.magnifier);
    m_capture->setKeyHints(o.keyHints);
    m_capture->setPinOnCopy(o.pinOnCopy);
    m_capture->setMouseActions({o.mouseDoubleLeft, o.mouseRight, o.mouseMiddle, o.mouseBack, o.mouseForward});
    m_capture->setHiddenButtons(o.toolbarHidden.split(u',', Qt::SkipEmptyParts));
    m_capture->setIncludeCursor(o.cursor);
    m_capture->setHdrFile(o.hdrFile);
    m_capture->setControlDetection(o.controlsUia);
    m_capture->setDelay(o.delay);
    m_capture->setHistory(QDir(m_dataDir).filePath(u"capture-history"_s), o.historyCount);
    m_capture->setColorFormat(capture::colorFormatFromName(o.colorFormat));
    // The pin and save keys and the pin window's (#187): open pins and an open editor too.
    m_capture->setKeys(o.keys);
    // A save folder chosen: Save goes straight into it, as in 0.16 (#194).
    m_capture->setSaveDirect(!o.saveFolder.isEmpty());
    if (m_pins) {
        m_pins->setDoubleClick(capture::pinDoubleClickFromName(o.pinDoubleClick));
        m_pins->setKeys(o.keys);
    }
    if (m_editor) {
        m_editor->setKeys(o.keys);
        m_editor->setSaveDirect(!o.saveFolder.isEmpty());
    }

    // PrtScn: taken from the Snipping Tool while it is wanted, the value
    // found then kept in PrintScreenBefore and put back after.
    QString before = o.printScreenBefore;
    QString note;
    if (o.printScreen && before.isEmpty()) {
        const std::optional<DWORD> now = printscreen::read();
        if (printscreen::write(DWORD {0}))
            before = printscreen::encode(now);
        else
            note = tr("没能改 PrtScn 的设置，它还会打开截图工具。");
    } else if (!o.printScreen && !before.isEmpty()) {
        std::optional<DWORD> value;
        if (!printscreen::decode(before, &value))
            before.clear(); // unreadable: nothing to put back
        else if (printscreen::write(value))
            before.clear();
        else
            note = tr("没能把 PrtScn 的设置改回去。");
    }
    if (before != o.printScreenBefore) {
        QSignalBlocker quiet(&m_settings); // saved, not applied again
        m_settings.update([&](CaptureOptions& next) { next.printScreenBefore = before; });
    }
    if (!note.isEmpty() && !(m_settingsWindow && m_settingsWindow->isVisible()))
        notify(tr("截图"), note);
    CaptureSettings::Problems problems;
    problems.note = note;
    m_settings.setProblems(problems); // the hotkeys' come next
    applyHotkeys();
}

void CaptureApp::applyHotkeys()
{
    const quint64 revision = ++m_hotkeyRevision;
    for (int id = CaptureKey; id <= OcrKey; ++id)
        ::UnregisterHotKey(m_window, id);
    if (m_settings.recordingHotkey()) {
        if (m_hostHotkeys)
            m_protocol.tool(u"register_hotkeys"_s, {{u"keys"_s, QJsonArray()}});
        return; // the keys being pressed must reach the settings window (Win顺 is told nothing: a moment)
    }
    QStringList wanted;
    bool failed = false;
    CaptureSettings::Problems problems;
    if (m_capture) {
        const CaptureOptions& o = m_settings.options();
        const auto take = [this, &wanted, &failed](int id, const QString& hotkey) -> QString {
            UINT modifiers = 0;
            UINT vk = 0;
            if (hotkey.isEmpty())
                return {};
            const bool valid = hotkeys::parse(hotkey, &modifiers, &vk);
            if (valid)
                wanted.append(hotkey);
            if (valid && (m_hostHotkeys || ::RegisterHotKey(m_window, id, modifiers, vk)))
                return {};
            failed = true;
            return hotkeyProblem(hotkey);
        };
        if (o.printScreen)
            take(PrintScreenKey, u"PrintScreen"_s);
        problems.note = m_settings.captureNote();
        problems.hotkey = take(CaptureKey, o.hotkey);
        problems.pinsHotkey = take(PinsKey, o.pinsHotkey);
        problems.pinsRestoreHotkey = take(PinsRestoreKey, o.pinsRestoreHotkey);
        problems.lastPinHotkey = take(LastPinKey, o.lastPinHotkey);
        problems.presentHotkey = take(PresentKey, o.presentHotkey);
        // The same key as one of the others: that one keeps it, this says so.
        problems.ocrHotkey = m_settings.ocrHotkeyClash();
        if (problems.ocrHotkey.isEmpty())
            problems.ocrHotkey = take(OcrKey, o.ocrHotkey);
        if (m_presenter)
            m_presenter->setHotkeyText(o.presentHotkey);
        m_settings.setProblems(problems); // shown next to each shortcut
    }
    // Win顺 leaves these to the plugin (winshun/hotkeys): Win+Shift+S, its
    // taskbar search's second key, and its quick paste's digits. One it held
    // is free a moment after it is told: tried once more then, and the
    // problems said after that.
    if (wanted != m_reportedHotkeys) {
        m_reportedHotkeys = wanted;
        m_protocol.notify(u"winshun/hotkeys"_s, {{u"keys"_s, QJsonArray::fromStringList(wanted)}});
        if (failed) {
            QTimer::singleShot(300, this, &CaptureApp::applyHotkeys);
            return;
        }
    }
    if (m_hostHotkeys) {
        m_protocol.tool(u"register_hotkeys"_s, {{u"keys"_s, QJsonArray::fromStringList(wanted)}},
            [this, revision, problems](const QJsonValue& result, const QString& error) mutable {
                if (revision != m_hotkeyRevision)
                    return; // a recording, edit or shutdown overtook this reply
                const auto errors = result.toObject().value(u"errors"_s).toObject();
                const auto& o = m_settings.options();
                // A key set twice: the later row is the one without it, as
                // when the plugin registers them itself.
                QStringList taken;
                if (o.printScreen)
                    taken.append(hotkeys::normalized(u"PrintScreen"_s));
                const auto problem = [&](const QString& key) {
                    const QString normal = hotkeys::normalized(key);
                    const bool again = !normal.isEmpty() && taken.contains(normal);
                    taken.append(normal);
                    if (errors.value(key).toInt() == ERROR_INVALID_PARAMETER) // parses, but not a shortcut Win顺 takes
                        return QCoreApplication::translate("ws::captureplugin::CaptureApp", "无法识别“%1”，请重新设置。").arg(key);
                    return key.isEmpty() || (!normal.isEmpty() && !again && error.isEmpty() && !errors.contains(key))
                        ? QString()
                        : hotkeyProblem(key);
                };
                problems.hotkey = problem(o.hotkey);
                problems.pinsHotkey = problem(o.pinsHotkey);
                problems.pinsRestoreHotkey = problem(o.pinsRestoreHotkey);
                problems.lastPinHotkey = problem(o.lastPinHotkey);
                problems.presentHotkey = problem(o.presentHotkey);
                if (const QString clash = m_settings.ocrHotkeyClash(); !clash.isEmpty())
                    problems.ocrHotkey = clash;
                else
                    problems.ocrHotkey = problem(o.ocrHotkey);
                if (o.printScreen && (!error.isEmpty() || errors.contains(u"PrintScreen"_s)))
                    problems.note = hotkeyProblem(u"PrintScreen"_s);
                m_settings.setProblems(problems);
                reportHotkeyProblems(problems);
            });
        return;
    }
    reportHotkeyProblems(problems);
}

void CaptureApp::reportHotkeyProblems(const CaptureSettings::Problems& problems)
{
    if (!m_capture)
        return;
    if (!(m_settingsWindow && m_settingsWindow->isVisible())) {
        for (const QString& error : {problems.hotkey, problems.pinsHotkey, problems.pinsRestoreHotkey, problems.lastPinHotkey,
                 problems.presentHotkey, problems.ocrHotkey}) {
            if (!error.isEmpty()) {
                notify(tr("快捷键不可用"), error);
                break;
            }
        }
    }
}

void CaptureApp::ensureTheme()
{
    if (m_themeMade)
        return;
    m_themeMade = true;
    m_qml.singletonInstance<QObject*>(u"WinShun"_s, u"Theme"_s);
}

// Win顺's look, as it says it (initialize, then appearance as it changes).
void CaptureApp::applyAppearance(const QJsonObject& a)
{
    if (a.isEmpty())
        return;
    SystemTheme::setRoundedCorners(a.value(u"corners"_s).toString(u"round"_s) == u"round");
    SystemTheme::setUiSize(a.value(u"uiSize"_s).toString(u"standard"_s));
    SystemTheme::setUiStyle(a.value(u"uiStyle"_s).toString());
    SystemTheme::setGlassAppearance(a.value(u"glassTransparency"_s).toInt(50), a.value(u"glassClarity"_s).toInt(50));
    SystemTheme::setBackdropOn(a.value(u"transparency"_s).toString(u"on"_s) == u"on");
    const QString theme = a.value(u"themeSetting"_s).toString();
    QGuiApplication::styleHints()->setColorScheme(theme == u"dark" ? Qt::ColorScheme::Dark
            : theme == u"light"                                     ? Qt::ColorScheme::Light
                                                                    : Qt::ColorScheme::Unknown);
    win::setMenuTheme(isDarkMode());
    static QTranslator translator;
    static QString language;
    const QString locale = a.value(u"locale"_s).toString(u"zh"_s);
    if (locale != language) {
        language = locale;
        QCoreApplication::removeTranslator(&translator);
        if (locale == u"en") {
            if (translator.load(u":/i18n/capture_en.qm"_s))
                QCoreApplication::installTranslator(&translator);
            else
                qWarning() << "No English translation in the resources";
        }
        QLocale::setDefault(locale == u"en" ? QLocale(QLocale::English, QLocale::UnitedStates)
                                            : QLocale(QLocale::Chinese, QLocale::China));
        QGuiApplication::setApplicationDisplayName(locale == u"en" ? u"WinShun"_s : u"Win顺"_s);
        m_qml.retranslate();
    }
    for (QQuickWindow* window : {m_ocrWindow.data(), m_bubbleWindow.data(), m_settingsWindow.data()}) {
        if (window)
            win::styleFramelessWindow(window, SystemTheme::backdropAvailable());
    }
}

capture::CaptureOcr& CaptureApp::captureOcr()
{
    if (!m_captureOcr) {
        m_captureOcr = std::make_unique<capture::CaptureOcr>();
        connect(m_captureOcr.get(), &capture::CaptureOcr::setupRequested, this, [this] {
            m_resumeOcr = m_ocrWindow && m_ocrWindow->isVisible();
            if (m_resumeOcr)
                m_ocrWindow->hide(); // the topmost OCR window must not cover the installer in Settings
            m_protocol.tool(u"show_ocr_setup"_s, {});
        });
        connect(m_captureOcr.get(), &capture::CaptureOcr::copyRequested, this, [this](const QString& text) {
            if (!text.isEmpty())
                copyText(text);
        });
        connect(m_captureOcr.get(), &capture::CaptureOcr::searchRequested, this, [this](const QString& query) { searchText(query, false); });
        connect(m_captureOcr.get(), &capture::CaptureOcr::webSearchRequested, this, [this](const QString& query) { searchText(query, true); });
        connect(m_captureOcr.get(), &capture::CaptureOcr::closeRequested, this, [this] { closeCaptureOcr(); });
        connect(m_captureOcr.get(), &capture::CaptureOcr::openLinkRequested, this, [this](const QString& url) {
            // Only ever from a click on 打开链接; opened by Win顺 (no programs started
            // here). open_item takes files and folders only (GitHub #225).
            m_protocol.tool(u"open_url"_s, {{u"url"_s, url}});
        });
    }
    return *m_captureOcr;
}

// 智能打码 (SmartRedact.h): the scans go to the OCR window's reader; the rules
// are read from the settings at the moment they are used, plus the names of
// this computer and its user, which are never written anywhere.
capture::SmartHooks CaptureApp::smartHooks()
{
    capture::SmartHooks hooks;
    hooks.scan = [this](const QImage& image, const QRect& region, double dpr, std::function<void(capture::RedactScan)> done) {
        captureOcr().scan(image, region, dpr, std::move(done));
    };
    hooks.cancel = [this] {
        if (m_captureOcr)
            m_captureOcr->cancelScan();
    };
    hooks.rules = [this] {
        const CaptureOptions& o = m_settings.options();
        capture::RedactRules rules;
        rules.ipMac = o.smartIpMac;
        rules.allText = o.smartAllText;
        rules.words = CaptureOptions::normalizedSmartWords(o.smartWords);
        for (const QString& name : {qEnvironmentVariable("USERNAME"), qEnvironmentVariable("COMPUTERNAME"), QDir::home().dirName()}) {
            if (!name.trimmed().isEmpty() && !rules.names.contains(name, Qt::CaseInsensitive))
                rules.names.append(name.trimmed());
        }
        return rules;
    };
    hooks.prescan = [this] { return m_settings.options().smartPrescan && capture::CaptureOcr::available(); };
    return hooks;
}

void CaptureApp::prepareWindow(QQuickWindow* window, bool backdrop)
{
    capture::releaseIdleGraphics(window);
    if (backdrop)
        prepareBackdrop(window);
    window->create();
    win::styleFramelessWindow(window, backdrop && SystemTheme::backdropAvailable());
    win::setDarkFrame(window, isDarkMode());
    win::keepDarkFrameOnShow(window);
}

// 识别文字 (CaptureOcr, OcrPanel.qml): one window, made the first time,
// beside the picture, taking the focus (the text can be edited).
void CaptureApp::showCaptureOcr(const QImage& image, double dpr, const QRect& where, const QStringList* texts)
{
    m_resumeOcr = false;
    if (!m_ocrWindow) {
        captureOcr();
        m_ocrFrame = std::make_unique<WindowFrame>(false);
        QQmlComponent component(&m_qml, u"WinShun"_s, u"OcrPanel"_s);
        QObject* object = component.createWithInitialProperties({
            {u"ocr"_s, QVariant::fromValue(m_captureOcr.get())},
            {u"frame"_s, QVariant::fromValue(m_ocrFrame.get())},
        });
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << "Screen capture: no OCR window:" << component.errorString();
            delete object;
            return;
        }
        prepareWindow(window, true);
        m_ocrFrame->setWindow(window);
        window->installEventFilter(this);
        m_ocrWindow = window;
    }
    placeNear(m_ocrWindow, where);
    if (texts)
        captureOcr().showTexts(*texts, tr("二维码"), tr("选区里没有找到二维码。"));
    else
        captureOcr().read(image, dpr);
    m_ocrWindow->show();
    m_ocrWindow->raise();
    win::bringToFront(m_ocrWindow);
    m_ocrWindow->requestActivate();
}

// Beside the picture, on its monitor (physical pixels there, logical for Qt).
void CaptureApp::placeNear(QQuickWindow* window, const QRect& where)
{
    const RECT anchor {where.left(), where.top(), where.left() + where.width(), where.top() + where.height()};
    const HMONITOR monitor = ::MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info {sizeof info};
    QScreen* screen = nullptr;
    for (QScreen* s : QGuiApplication::screens()) {
        const auto* native = s->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor)
            screen = s;
    }
    if (screen && ::GetMonitorInfoW(monitor, &info)) {
        const qreal ratio = screen->devicePixelRatio();
        const QRect work(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top);
        const QSize box(qRound(window->width() * ratio), qRound(window->height() * ratio));
        const QPoint at = capture::placeBeside(where, box, work, qRound(8 * ratio));
        const QPoint monitorOrigin(info.rcMonitor.left, info.rcMonitor.top);
        window->setScreen(screen);
        window->setPosition(screen->geometry().topLeft() + (at - monitorOrigin) / ratio);
    }
}

void CaptureApp::closeCaptureOcr(bool destroy)
{
    m_resumeOcr = false;
    if (m_captureOcr)
        m_captureOcr->discardPending();
    if (m_ocrWindow) {
        if (destroy)
            delete m_ocrWindow.data(); // before its view-model
        else
            m_ocrWindow->hide();
    }
    if (destroy) {
        m_ocrFrame.reset();
        m_quickOcr.reset();
        m_captureOcr.reset(); // its thread, and the reader if still loaded
    }
}

// The mouse buttons' 识别文字并复制 / 搜索 / 网页搜索: read on a reader of its
// own, so the OCR window is left alone. Without the models the window opens
// instead, which says so.
void CaptureApp::captureOcrThen(const QImage& image, double dpr, const QRect& where, OcrThen then)
{
    if (!capture::CaptureOcr::available()) {
        showCaptureOcr(image, dpr, where);
        return;
    }
    if (!m_quickOcr) {
        m_quickOcr = std::make_unique<capture::CaptureOcr>();
        connect(m_quickOcr.get(), &capture::CaptureOcr::changed, this, [this] { quickOcrDone(); });
    }
    m_quickOcrThen = then;
    m_quickOcr->read(image, dpr);
}

void CaptureApp::quickOcrDone()
{
    if (!m_quickOcr || m_quickOcr->busy())
        return;
    const QString text = m_quickOcr->text();
    const QString message = m_quickOcr->message();
    const OcrThen then = m_quickOcrThen;
    m_quickOcr.release()->deleteLater(); // its thread and reader go with it
    m_idleTrim->poke();
    if (then == OcrThen::Bubble) {
        // Said in the card, not a toast: a toast stays in the Action Center, the words too.
        if (text.trimmed().isEmpty()) {
            showOcrBubble(u"none"_s, {}, message);
        } else {
            copyText(text);
            showOcrBubble(u"copied"_s, text, {});
        }
        return;
    }
    if (text.trimmed().isEmpty()) {
        notify(tr("识别文字"), message.isEmpty() ? tr("图片里没有找到文字。") : message);
        return;
    }
    switch (then) {
    case OcrThen::Copy:
        copyText(text);
        notify(tr("识别文字"), tr("已复制识别的文字")); // not the words: a toast stays in the Action Center
        break;
    case OcrThen::Search:
        searchText(capture::CaptureOcr::oneLine(text), false);
        break;
    case OcrThen::Web:
        searchText(capture::CaptureOcr::oneLine(text), true);
        break;
    case OcrThen::Bubble:
        break;
    }
}

// 识别屏幕文字 (#198): the capture with only the shade, the region's words
// copied at once (readScreenText); held back in full screen as the
// screenshot's hotkey is, when the user asked for that (the hotkey only).
void CaptureApp::startOcr()
{
    if (m_capture)
        m_capture->triggerOcr();
}

// Without the reader the OCR window opens instead and says how to get it (as
// Shift+C does), going on with this region once it is there.
void CaptureApp::readScreenText(const QImage& image, double dpr, const QRect& where)
{
    if (!capture::CaptureOcr::available()) {
        showCaptureOcr(image, dpr, where);
        return;
    }
    m_quickOcrWhere = where;
    showOcrBubble(u"reading"_s, {}, {});
    captureOcrThen(image, dpr, where, OcrThen::Bubble);
}

// OcrBubble.qml: made the first time and kept; shown without taking the
// focus (as Win顺's SettingsNotice), so Ctrl+V still goes to the window in front.
void CaptureApp::showOcrBubble(const QString& phase, const QString& words, const QString& detail)
{
    if (!m_bubbleWindow) {
        m_bubbleFrame = std::make_unique<WindowFrame>(false);
        QQmlComponent component(&m_qml, u"WinShun"_s, u"OcrBubble"_s);
        QObject* object = component.createWithInitialProperties({
            {u"frame"_s, QVariant::fromValue(m_bubbleFrame.get())},
            {u"host"_s, QVariant::fromValue<QObject*>(this)},
        });
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << "Screen capture: no OCR card:" << component.errorString();
            delete object;
            m_bubbleFrame.reset();
            if (phase != u"reading") // said the old way
                notify(tr("识别文字"), phase == u"copied" ? tr("已复制识别的文字") : tr("图片里没有找到文字。"));
            return;
        }
        prepareWindow(window, true);
        m_bubbleFrame->setWindow(window);
        m_bubbleFrame->setNoActivate(true);
        m_bubbleWindow = window;
        // Its size follows the words a frame later (the Column lays out on
        // polish): placed again then, so it stays on the work area and off the region.
        const auto place = [this, window] {
            if (window->isVisible())
                placeNear(window, m_quickOcrWhere);
        };
        connect(window, &QWindow::heightChanged, window, place);
        connect(window, &QWindow::widthChanged, window, place);
    }
    QQuickWindow* window = m_bubbleWindow;
    QMetaObject::invokeMethod(window, "present", Q_ARG(QVariant, phase), Q_ARG(QVariant, words), Q_ARG(QVariant, detail));
    placeNear(window, m_quickOcrWhere);
    window->setProperty("_q_showWithoutActivating", true);
    window->show();
    window->setProperty("_q_showWithoutActivating", QVariant());
    ::SetWindowPos(reinterpret_cast<HWND>(window->winId()), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool CaptureApp::closeOcrBubble()
{
    if (!m_bubbleWindow || !m_bubbleWindow->isVisible())
        return false;
    QMetaObject::invokeMethod(m_bubbleWindow.data(), "dismiss");
    return true;
}

void CaptureApp::editOcrText(const QString& text)
{
    if (text.isEmpty())
        return;
    showCaptureOcr(QImage(), 1.0, m_quickOcrWhere);
    captureOcr().showText(text);
}

void CaptureApp::notifySaved(const QString& path, const QString& error)
{
    if (!error.isEmpty()) {
        notify(tr("截图没能保存"), error);
        return;
    }
    // Not the file's name: a toast stays in the Action Center.
    const saved::Target target = saved::target(path);
    const QString folder = target.show == saved::Show::File ? QFileInfo(target.path).absolutePath() : target.path;
    // A click selects the file in its folder, as in 0.16 (#194).
    notify(tr("截图已保存"), tr("点击打开所在文件夹"), QDir::toNativeSeparators(folder),
        target.show == saved::Show::File ? QDir::toNativeSeparators(target.path) : QString());
}

capture::PinManager& CaptureApp::pins()
{
    if (!m_pins) {
        capture::PinManager::Callbacks callbacks;
        callbacks.copy = [this](const QImage& image, bool screenshot) { copyImage(image, {}, true, screenshot); };
        callbacks.save = [this](const QImage& image, const QString& path) {
            capture::saveImage(m_pinSaver, this, image, path,
                [this](const QString& saved, const QString& error) { notifySaved(saved, error); }, m_settings.options().saveFolder);
        };
        callbacks.ocr = [this](const QImage& image, double dpr, const QRect& where) { showCaptureOcr(image, dpr, where); };
        callbacks.edit = [this](capture::PinWindow* pin) { editPin(pin); };
        callbacks.closed = [this] { m_idleTrim->poke(); }; // its pixels can go back to Windows
        callbacks.dark = [] { return isDarkMode(); };
        // The 界面风格's (Theme's nativeLook), as the launcher's.
        callbacks.accent = [this] {
            ensureTheme();
            return SystemTheme::nativeColor("accentFill", SystemTheme::accentColor());
        };
        callbacks.edge = [this] {
            ensureTheme();
            return SystemTheme::nativeColor("windowEdge", QColor());
        };
        callbacks.pillRadius = [this] {
            ensureTheme();
            return SystemTheme::nativeReal("controlRadius", win::roundedCorners() ? 4 : 0);
        };
        callbacks.popupMenu = [this](HWND owner, const std::vector<shell::MenuItem>& items, POINT at) {
            QObject* theme = m_qml.singletonInstance<QObject*>(u"WinShun"_s, u"Theme"_s);
            return traymenu::popup(owner, items, at, theme);
        };
        callbacks.copyText = [this](const QString& text) {
            if (!text.isEmpty())
                copyText(text);
        };
        callbacks.ocrAvailable = capture::CaptureOcr::available;
        callbacks.readLines = [this](const QImage& image, double dpr, std::function<void(QList<capture::RedactLine>)> done) {
            captureOcr().scan(image, dpr, std::move(done));
        };
        m_pins = std::make_unique<capture::PinManager>(std::move(callbacks));
        m_pins->setDoubleClick(capture::pinDoubleClickFromName(m_settings.options().pinDoubleClick));
        m_pins->setKeys(m_settings.options().keys);
    }
    return *m_pins;
}

// The presenter (#189, PresenterHost): made the first time it is asked for.
PresenterHost& CaptureApp::presenter()
{
    if (!m_presenter) {
        m_presenter = std::make_unique<PresenterHost>(m_qml, m_dataDir + u"/presenter.ini"_s);
        m_presenter->setHotkeyText(m_settings.options().presentHotkey);
    }
    return *m_presenter;
}

bool CaptureApp::presenterOpen() const
{
    return m_presenter && m_presenter->isOpen();
}

void CaptureApp::togglePresenter()
{
    if (presenterOpen())
        m_presenter->presenter().close();
    else
        presenter().presenter().open();
}

capture::CaptureEditor* CaptureApp::editor()
{
    if (m_editor)
        return m_editor.get();
    auto editor = std::make_unique<capture::CaptureEditor>();
    capture::CaptureEditor::Hooks hooks;
    hooks.project = [this](qint64 id) {
        const auto it = m_history.find(id);
        return it == m_history.end() ? QByteArray() : it->second.project;
    };
    hooks.picture = [this](qint64 id) {
        const auto it = m_history.find(id);
        return it == m_history.end() ? QImage() : readPicture(it->second.file);
    };
    hooks.exists = [this](qint64 id) { return m_history.contains(id); };
    // Changed: a new entry and the clipboard, the original left as it is.
    hooks.copy = [this](const QImage& image, const QByteArray& project) { copyImage(image, project, true, true); };
    // Not changed: the original again, which moves it to the top.
    hooks.copyOriginal = [this](qint64 id) { m_protocol.tool(u"clipboard_copy_entry"_s, {{u"id"_s, id}}); };
    hooks.record = [this](const QImage& image, const QByteArray& project) { copyImage(image, project, false, true); };
    hooks.save = [this](const QImage& image, const QString& path) {
        capture::saveImage(m_pinSaver, this, image, path,
            [this](const QString& saved, const QString& error) { notifySaved(saved, error); }, m_settings.options().saveFolder);
    };
    hooks.chooseSavePath = capture::chooseScreenshotSavePath;
    hooks.pin = [this](const QImage& image, const QByteArray& project, QPoint origin) { pins().pin(image, origin, true, project); };
    hooks.ocr = [this](const QImage& image, double dpr, const QRect& where) { showCaptureOcr(image, dpr, where); };
    hooks.smart = smartHooks();
    hooks.styles = [this] { return m_settings.options().annotateStyles; };
    hooks.stylesChanged = [this](const QString& json) {
        if (json != m_settings.options().annotateStyles)
            m_settings.update([&](CaptureOptions& o) { o.annotateStyles = json; });
    };
    // The picture, the document and the tiles are let go of: the pages go back to Windows.
    hooks.closed = [this] {
        m_history.clear();
        m_idleTrim->poke();
    };
    hooks.window.show = [](QQuickWindow* window) {
        window->showNormal();
        window->raise();
        win::bringToFront(window);
        window->requestActivate();
    };
    hooks.window.cloak = [](QQuickWindow* window, bool cloaked) { win::setCloaked(window, cloaked); };
    hooks.window.nativeRect = [](QQuickWindow* window) { return clientRectOf(window); };
    hooks.window.moveTo = [](QQuickWindow* window, QPoint topLeft) { moveClientTo(window, topLeft); };
    editor->setHooks(std::move(hooks));
    editor->setOcrAvailable(capture::CaptureOcr::available());

    m_editFrame = std::make_unique<WindowFrame>(true); // its own title bar
    QQmlComponent component(&m_qml, u"WinShun"_s, u"EditWindow"_s);
    QObject* object = component.createWithInitialProperties({
        {u"editor"_s, QVariant::fromValue(editor.get())},
        {u"frame"_s, QVariant::fromValue(m_editFrame.get())},
    });
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        qWarning().noquote() << "Screen capture: no editing window:" << component.errorString();
        delete object;
        m_editFrame.reset();
        return nullptr;
    }
    prepareWindow(window, false);
    m_editFrame->setWindow(window);
    editor->attach(window);
    m_editWindow = window;
    editor->setKeys(m_settings.options().keys);
    editor->setSaveDirect(!m_settings.options().saveFolder.isEmpty());
    m_editor = std::move(editor);
    return m_editor.get();
}

// 编辑 on a picture of Win顺's clipboard history (or its screenshot history):
// the picture's file, and its annotations if it has them, handed over by
// Win顺 (the project in a file of this plugin's folder, deleted once read).
void CaptureApp::editPicture(qint64 clipId, const QString& file, const QString& projectFile)
{
    HistoryPicture picture {file, {}};
    if (!projectFile.isEmpty()) {
        QFile project(projectFile);
        if (project.open(QIODevice::ReadOnly))
            picture.project = project.readAll();
        project.close();
        if (QDir::cleanPath(QFileInfo(projectFile).absolutePath()).startsWith(QDir::cleanPath(m_dataDir), Qt::CaseInsensitive))
            QFile::remove(projectFile);
    }
    m_history[clipId] = std::move(picture);
    capture::CaptureEditor* editor = this->editor();
    if (editor && !editor->openHistory(clipId))
        notify(tr("没能编辑这张图片"), tr("它已不在历史里，或读不出来。"));
}

void CaptureApp::editPin(capture::PinWindow* pin)
{
    if (!pin || !m_pins || !m_pins->contains(pin))
        return;
    capture::CaptureEditor* editor = this->editor();
    if (!editor)
        return;
    // The pin is lent for as long as the window is open; it may be closed meanwhile.
    capture::CaptureEditor::PinTarget target;
    target.image = pin->image();
    target.project = pin->project();
    target.picture = pin->pictureRect();
    target.zoom = pin->zoom();
    const auto alive = [this, pin] { return m_pins && m_pins->contains(pin); };
    target.alive = alive;
    target.setShown = [alive, pin](bool shown) {
        if (alive())
            pin->setShown(shown);
    };
    target.replace = [alive, pin](const QImage& image, const QByteArray& project) {
        if (alive())
            pin->setPicture(image, project);
    };
    if (!editor->openPin(std::move(target)))
        notify(tr("没能编辑这张图片"), tr("读不出这张图。"));
}

// 贴到屏幕 on a picture file's row (the launcher) or a picture in the
// clipboard history: read by Windows Imaging Component on a thread of its
// own, pinned round the pointer within the work area of its monitor; larger
// than 90 % of that area, shown fitted, its own pixels kept for a zoom.
void CaptureApp::pinPicture(const QString& path, bool screenshot)
{
    if (path.isEmpty())
        return;
    const auto placement = pinimage::cursorPlacement();
    if (!placement)
        return;
    QPointer<CaptureApp> self(this);
    std::thread([self, path, placement = *placement, screenshot] {
        auto prepared = pinimage::prepare(pinimage::readFile(path), placement);
        QMetaObject::invokeMethod(qApp, [self, prepared = std::move(prepared), screenshot] {
            if (!self)
                return;
            if (prepared.image.isNull()) {
                self->notify(tr("没能贴到屏幕"), tr("读不出这张图片。"));
                return;
            }
            self->pins().pinFitted(prepared.image, prepared.origin, prepared.anchor, screenshot);
        }, Qt::QueuedConnection);
    }).detach();
}

void CaptureApp::pinCurrentImage()
{
    if (m_pinPending)
        return;
    const auto placement = pinimage::cursorPlacement();
    if (!placement)
        return;
    const pinimage::LastCapture last {m_capture ? m_capture->lastClipboardId() : QString(),
        m_capture ? m_capture->lastClipboardSequence() : 0};
    m_pinPending = true;
    QPointer<CaptureApp> self(this);
    std::thread([self, last, placement = *placement] {
        auto picture = pinimage::readClipboard(last);
        auto prepared = pinimage::prepare(std::move(picture.image), placement);
        QMetaObject::invokeMethod(qApp, [self, last, status = picture.status, prepared = std::move(prepared)] {
            if (!self)
                return;
            self->m_pinPending = false;
            if (self->m_capture && self->m_capture->lastClipboardId() != last.id) {
                self->pinCurrentImage(); // includes an own copy that arrived during this read
                return;
            }
            using Status = pinimage::ClipboardStatus;
            switch (status) {
            case Status::Image:
                if (!prepared.image.isNull())
                    self->pins().pinFitted(prepared.image, prepared.origin, prepared.anchor, false);
                else
                    self->notify(tr("贴图"), tr("剪贴板里的图片读不出来，请重新复制。"));
                break;
            case Status::LastCapture:
            case Status::Empty:
                if (!self->m_capture || !self->m_capture->hasLast())
                    self->notify(tr("贴图"), tr("剪贴板里没有可用图片，也没有最近截图。请先复制图片或截一张。"));
                else
                    self->m_capture->pinLast();
                break;
            case Status::Busy:
                self->notify(tr("贴图"), tr("剪贴板正被别的程序占用，请再试一次。"));
                break;
            case Status::Unreadable:
                self->notify(tr("贴图"), tr("剪贴板里的图片读不出来，请重新复制。"));
                break;
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void CaptureApp::showSettings()
{
    if (!m_settingsWindow) {
        m_settingsFrame = std::make_unique<WindowFrame>(true); // its own title bar
        QQmlComponent component(&m_qml, u"WinShun"_s, u"CaptureSettingsWindow"_s);
        QObject* object = component.createWithInitialProperties({
            {u"editor"_s, QVariant::fromValue(&m_settings)},
            {u"frame"_s, QVariant::fromValue(m_settingsFrame.get())},
        });
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << "Screen capture: no settings window:" << component.errorString();
            delete object;
            m_settingsFrame.reset();
            return;
        }
        prepareWindow(window, true);
        m_settingsFrame->setWindow(window);
        m_settings.setWindow(window);
        window->installEventFilter(this);
        m_settingsWindow = window;
    }
    m_settingsWindow->showNormal();
    m_settingsWindow->raise();
    win::bringToFront(m_settingsWindow);
    m_settingsWindow->requestActivate();
}

// The 截图 submenu of Win顺's tray menu, asked for as it opens (trayMenu);
// what is chosen comes back as menu {id}.
QJsonArray CaptureApp::trayMenu() const
{
    const CaptureOptions& o = m_settings.options();
    const QString key = !o.hotkey.isEmpty() ? o.hotkey : o.printScreen ? u"PrtScn"_s : QString();
    const int pinCount = m_pins ? m_pins->count() : 0;
    QJsonArray items;
    if (m_capture) {
        items.append(menuItem(u"capture"_s, withKey(tr("开始截图"), key)));
        items.append(menuItem(u"ocr"_s, withKey(tr("识别屏幕文字"), o.ocrHotkey)));
        QJsonObject delay {{u"title"_s, tr("延时截图")},
            {u"children"_s, QJsonArray {menuItem(u"delay3"_s, tr("3 秒"), o.delay == 3), menuItem(u"delay5"_s, tr("5 秒"), o.delay == 5),
                                 menuItem(u"delay10"_s, tr("10 秒"), o.delay == 10)}}};
        items.append(delay);
        items.append(menuItem(u"pinLast"_s, withKey(tr("贴图"), o.lastPinHotkey)));
        items.append(menuItem(u"present"_s, withKey(presenterOpen() ? tr("结束演示") : tr("演示 / 白板"), o.presentHotkey)));
        items.append(menuItem(u"restoreDraft"_s, tr("找回刚放弃的标注"), false, m_capture->hasDraft()));
        items.append(separator());
    }
    items.append(menuItem(u"clickThrough"_s, withKey(tr("取消所有贴图的穿透（单个：按住 Ctrl 点它）"), o.pinsRestoreHotkey), false,
        m_pins && m_pins->anyClickThrough()));
    items.append(menuItem(u"hidePins"_s,
        withKey(m_pins && m_pins->allHidden() ? tr("显示全部贴图") : tr("隐藏全部贴图"), o.pinsHotkey), false, pinCount > 0));
    items.append(menuItem(u"pinsTopmost"_s, tr("全部置顶"), m_pins && m_pins->allTopmost(), pinCount > 0));
    items.append(menuItem(u"closePins"_s, tr("关闭全部贴图"), false, pinCount > 0));
    items.append(separator());
    items.append(menuItem(u"settings"_s, tr("截图设置…")));
    return items;
}

void CaptureApp::menuChosen(const QString& id)
{
    if (id == u"capture") {
        // Once the menu is gone from the screen: not in the picture.
        QTimer::singleShot(200ms, this, [this] {
            if (m_capture)
                m_capture->trigger();
        });
    } else if (id == u"ocr") {
        QTimer::singleShot(200ms, this, &CaptureApp::startOcr); // once the menu is gone from the screen
    } else if (id == u"pinLast") {
        pinCurrentImage();
    } else if (id == u"present") {
        QTimer::singleShot(200ms, this, &CaptureApp::togglePresenter); // once the menu is gone
    } else if (id == u"restoreDraft" && m_capture) {
        m_capture->restoreDraft();
    } else if (id == u"clickThrough" && m_pins) {
        m_pins->clearClickThrough();
    } else if (id == u"closePins" && m_pins) {
        m_pins->closeAll();
    } else if (id == u"hidePins" && m_pins) {
        m_pins->setAllHidden(!m_pins->allHidden());
    } else if (id == u"pinsTopmost" && m_pins) {
        m_pins->setAllTopmost(!m_pins->allTopmost());
    } else if (id == u"delay3" || id == u"delay5" || id == u"delay10") {
        const int seconds = id == u"delay3" ? 3 : id == u"delay5" ? 5 : 10;
        m_settings.setCaptureDelay(seconds); // remembered (D in the overlay uses it)
        // Once the menu is gone from the screen: the countdown is a window of ours.
        QTimer::singleShot(200ms, this, [this, seconds] {
            if (m_capture)
                m_capture->startDelayed(seconds);
        });
    } else if (id == u"settings") {
        showSettings();
    }
}

// ---- Win顺's side ---------------------------------------------------------------

void CaptureApp::copyImage(const QImage& image, const QByteArray& project, bool copy, bool screenshot, const QString& imageOrigin)
{
    if (image.isNull())
        return;
    const QString base = QDir(m_dataDir).filePath(u"out/%1-%2"_s.arg(::GetCurrentProcessId()).arg(m_nextOut++));
    QPointer<CaptureApp> self(this);
    std::thread([self, image, project, copy, screenshot, base, imageOrigin] {
        const QString file = base + u".wimg"_s;
        bool ok = writePicture(file, image);
        QString projectFile;
        if (ok && !project.isEmpty()) {
            projectFile = base + u".wsproj"_s;
            QFile out(projectFile);
            ok = out.open(QIODevice::WriteOnly) && out.write(project) == project.size();
        }
        QMetaObject::invokeMethod(qApp, [self, image, copy, screenshot, file, projectFile, ok, imageOrigin] {
            if (!self)
                return;
            const auto fallback = [image, copy, file, projectFile, imageOrigin] {
                QFile::remove(file);
                if (!projectFile.isEmpty())
                    QFile::remove(projectFile);
                if (copy) {
                    auto* data = new QMimeData;
                    data->setImageData(image);
                    if (!imageOrigin.isEmpty())
                        data->setData(QString::fromLatin1(kImageOriginMime), imageOrigin.toUtf8() + '\0');
                    QGuiApplication::clipboard()->setMimeData(data);
                }
            };
            if (!ok) {
                qWarning() << "Screen capture: could not write the picture for Win顺" << file;
                fallback();
                return;
            }
            QJsonObject arguments {{u"file"_s, QDir::toNativeSeparators(file)}, {u"copy"_s, copy}, {u"screenshot"_s, screenshot}};
            if (!imageOrigin.isEmpty())
                arguments.insert(u"origin"_s, imageOrigin);
            if (!projectFile.isEmpty())
                arguments.insert(u"project"_s, QDir::toNativeSeparators(projectFile));
            self->m_protocol.tool(u"clipboard_add_image"_s, arguments, [fallback](const QJsonValue&, const QString& error) {
                if (!error.isEmpty())
                    fallback();
            });
        }, Qt::QueuedConnection);
    }).detach();
}

void CaptureApp::copyText(const QString& text)
{
    if (text.isEmpty())
        return;
    m_protocol.tool(u"clipboard_copy"_s, {{u"text"_s, text}}, [text](const QJsonValue&, const QString& error) {
        if (!error.isEmpty())
            QGuiApplication::clipboard()->setText(text);
    });
}

void CaptureApp::notify(const QString& title, const QString& text, const QString& folder, const QString& file)
{
    QJsonObject params {{u"toast"_s, true}, {u"title"_s, title}, {u"text"_s, text}};
    // A click goes to one place, the first the host knows: reveal, else open.
    QJsonArray click;
    if (!file.isEmpty())
        click.append(QJsonObject {{u"type"_s, u"reveal"_s}, {u"path"_s, file}});
    if (!folder.isEmpty())
        click.append(QJsonObject {{u"type"_s, u"open"_s}, {u"path"_s, folder}});
    if (!click.isEmpty())
        params.insert(u"click"_s, click);
    m_protocol.notify(u"winshun/notify"_s, params);
}

void CaptureApp::searchText(const QString& text, bool web)
{
    if (!text.trimmed().isEmpty())
        m_protocol.tool(web ? u"web_search"_s : u"show_launcher"_s, {{u"text"_s, text}});
}

// ---- the message window -----------------------------------------------------------

LRESULT CALLBACK CaptureApp::windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<CaptureApp*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && message == WM_HOTKEY) {
        self->hotkey(static_cast<int>(w));
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, w, l);
}

void CaptureApp::hotkey(int id)
{
    switch (id) {
    case CaptureKey:
    case PrintScreenKey:
        if (m_capture && !m_capture->busy() && m_settings.options().pauseInFullScreen
            && fullScreenShortcutBlocked(foreground::facts(), true))
            break;
        if (m_capture)
            m_capture->trigger();
        break;
    case OcrKey: // held back in full screen as the screenshot's
        if (m_capture && !m_capture->busy() && m_settings.options().pauseInFullScreen
            && fullScreenShortcutBlocked(foreground::facts(), true))
            break;
        startOcr();
        break;
    case LastPinKey:
        pinCurrentImage();
        break;
    case PresentKey: // never held back in full screen: a slide show is what it is for
        presenter().hotkey();
        break;
    case PinsKey:
        if (m_pins)
            m_pins->setAllHidden(!m_pins->allHidden());
        break;
    case PinsRestoreKey:
        if (m_pins)
            m_pins->clearClickThrough();
        break;
    default:
        break;
    }
}

} // namespace ws::captureplugin

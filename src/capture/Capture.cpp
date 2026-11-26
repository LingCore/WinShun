#include "Capture.h"

#include "CaptureOcr.h"
#include "DiagnosticLog.h"
#include "PictureWrite.h"
#include "QrCode.h"
#include "platform/Foreground.h"
#include "platform/KeyRouter.h" // kOwnInput
#include "platform/Shell.h"
#include "platform/WindowEffects.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QUuid>

#include <dwmapi.h>
#include <psapi.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws::capture {

namespace {

// The Start menu needs some 200 ms to give up the foreground after Esc
// (stage 0); past this the overlays come anyway.
constexpr qint64 kFlyoutWaitMs = 300;
// Then its closing animation: grabbed until it is over, at most this long
// (the overlays wait for it; without a frame the hover lands behind it).
constexpr qint64 kFlyoutSettleMs = 500;
// Hiding waits for a frame drawn without the pictures (their textures go
// with it); one that does not come does not hold the windows up.
constexpr auto kHideTimeout = 200ms;
// A Store app in full screen: the hotkey's keys up first (Ctrl of Ctrl+1
// down would make Win+Shift+Enter another chord), at most this long; then out
// of full screen, measured in 3 ms (the Media Player), given this long.
constexpr qint64 kKeysUpWaitMs = 1500;
constexpr qint64 kFullScreenWaitMs = 1000;
// The overlays gone: the app has the foreground back and the keys are up
// (Ctrl of Ctrl+C) within this, or it is left out of full screen. Then the
// switch waits a little more: sent the moment the app has the foreground
// again, it is not taken (measured: 3 of 3 then, 100 ms later it is, in
// some 20 ms); not back after the check time, it is sent once more.
constexpr qint64 kPutBackWaitMs = 1500;
constexpr qint64 kPutBackSettleMs = 150;
constexpr qint64 kPutBackCheckMs = 400;
// ZBID_DESKTOP: the band of the Z order every desktop program's windows are
// in, topmost or not.
constexpr DWORD kDesktopBand = 1;

HMONITOR monitorOf(QScreen* screen)
{
    const auto* native = screen ? screen->nativeInterface<QNativeInterface::QWindowsScreen>() : nullptr;
    return native ? native->handle() : nullptr;
}

QRect monitorRect(HMONITOR monitor)
{
    MONITORINFO info {sizeof info};
    if (!monitor || !::GetMonitorInfoW(monitor, &info))
        return {};
    const RECT& r = info.rcMonitor;
    return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

// The monitor without its taskbar (and other docked bars), physical.
QRect workRect(HMONITOR monitor)
{
    MONITORINFO info {sizeof info};
    if (!monitor || !::GetMonitorInfoW(monitor, &info))
        return {};
    const RECT& r = info.rcWork;
    return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

// Windows' "Screenshots" folder (Pictures\Screenshots, or where the user
// moved it), made when missing.
QString screenshotsFolder()
{
    PWSTR path = nullptr;
    QString folder;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Screenshots, KF_FLAG_CREATE, nullptr, &path)))
        folder = QString::fromWCharArray(path);
    ::CoTaskMemFree(path);
    if (folder.isEmpty()) {
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &path)))
            folder = QDir(QString::fromWCharArray(path)).filePath(u"Screenshots"_s);
        ::CoTaskMemFree(path);
        if (!folder.isEmpty())
            QDir().mkpath(folder);
    }
    return QDir::toNativeSeparators(folder);
}

// Which capture this is in the run, for the diagnostics.
bool g_firstCapture = true;
QElapsedTimer g_lastCapture; // when the one before ended

QString outcomeName(CaptureSession::Action action)
{
    switch (action) {
    case CaptureSession::Action::Copy: return u"copied"_s;
    case CaptureSession::Action::CopyPin: return u"copied_pinned"_s;
    case CaptureSession::Action::Save:
    case CaptureSession::Action::SavePrevious:
    case CaptureSession::Action::SaveAs: return u"saved"_s;
    case CaptureSession::Action::Pin: return u"pinned"_s;
    case CaptureSession::Action::Ocr:
    case CaptureSession::Action::OcrCopy:
    case CaptureSession::Action::OcrSearch:
    case CaptureSession::Action::OcrWeb:
    case CaptureSession::Action::OcrTranslate: return u"ocr"_s;
    case CaptureSession::Action::Cancel: return u"cancelled"_s;
    case CaptureSession::Action::Long: return u"long"_s;
    case CaptureSession::Action::Record: return u"record"_s;
    case CaptureSession::Action::Qr: return u"qr"_s;
    case CaptureSession::Action::Delay: return u"delay"_s;
    }
    return u"other"_s;
}

QString sizeText(const QSize& size)
{
    return size.isEmpty() ? QString() : u"%1x%2"_s.arg(size.width()).arg(size.height());
}

// Page faults of the process so far: a capture after the working set was
// emptied (IdleTrim) takes thousands, each waited for.
quint64 pageFaults()
{
    PROCESS_MEMORY_COUNTERS counters {sizeof counters};
    return ::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof counters) ? counters.PageFaultCount : 0;
}

void sendEscape()
{
    INPUT keys[2] {};
    keys[0].type = keys[1].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[1].ki.wVk = VK_ESCAPE;
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[0].ki.dwExtraInfo = keys[1].ki.dwExtraInfo = kOwnInput; // TypeToSearch lets it through to the Start menu
    ::SendInput(2, keys, sizeof keys[0]);
}

// The window's band of the Z order (user32's GetWindowBand, Windows 8 on,
// not documented); 0: not known.
DWORD windowBand(HWND window)
{
    using GetWindowBand = BOOL(WINAPI*)(HWND, DWORD*);
    static const auto get = reinterpret_cast<GetWindowBand>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetWindowBand")));
    DWORD band = 0;
    return get && get(window, &band) ? band : 0;
}

// A Store app in full screen (a video in the Media Player, F11): its frame
// goes into a band over the desktop's (8), where no desktop program's window
// comes, topmost or not, nor takes the foreground from it. The overlay stayed
// under it, and the keys went to the app: Esc took it out of full screen, and
// then showed the capture still there, out of the keys' reach. PixPin's
// overlay does not come over it either (measured).
bool fullScreenStoreApp(HWND window)
{
    wchar_t name[32] {};
    if (!window || !::GetClassNameW(window, name, static_cast<int>(std::size(name)))
        || std::wstring_view(name) != L"ApplicationFrameWindow")
        return false;
    if (const DWORD band = windowBand(window); band == 0 || band == kDesktopBand)
        return false;
    RECT frame {};
    MONITORINFO monitor {sizeof monitor};
    return ::GetWindowRect(window, &frame)
        && ::GetMonitorInfoW(::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)
        && frame.left <= monitor.rcMonitor.left && frame.top <= monitor.rcMonitor.top
        && frame.right >= monitor.rcMonitor.right && frame.bottom >= monitor.rcMonitor.bottom;
}

bool modifiersDown()
{
    for (const int key : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN}) {
        if (::GetAsyncKeyState(key) & 0x8000)
            return true;
    }
    return false;
}

// Win+Shift+Enter: Windows' own switch for the full screen of the Store app
// in front, both ways (out in 3 ms, measured).
void sendFullScreenSwitch()
{
    constexpr WORD chord[] {VK_LWIN, VK_SHIFT, VK_RETURN};
    constexpr std::size_t n = std::size(chord);
    INPUT keys[2 * n] {};
    for (std::size_t i = 0; i < n; ++i) {
        INPUT& down = keys[i];
        INPUT& up = keys[2 * n - 1 - i];
        down.type = up.type = INPUT_KEYBOARD;
        down.ki.wVk = up.ki.wVk = chord[i];
        up.ki.dwFlags = KEYEVENTF_KEYUP;
        down.ki.dwExtraInfo = up.ki.dwExtraInfo = kOwnInput;
    }
    ::SendInput(static_cast<UINT>(std::size(keys)), keys, sizeof keys[0]);
}

// What leaves nothing of ours to be seen once the overlays are gone: the
// app goes back into full screen after these. Not after a pin, the words
// read, a recording, a long screenshot: they would be under it.
bool putsBack(CaptureSession::Action action)
{
    using Action = CaptureSession::Action;
    switch (action) {
    case Action::Copy:
    case Action::Save:
    case Action::SaveAs:
    case Action::SavePrevious:
    case Action::Cancel:
        return true;
    default:
        return false;
    }
}

// Actions that open a Band-1 window (pin, OCR/QR panel, long capture,
// recording, or quick-pick bubbles) which would be buried behind a Band-8
// full-screen Store app if it stayed in full screen. Copy, direct Save,
// Cancel, OcrCopy and Delay leave the full-screen player completely untouched.
bool needsDesktopBand(CaptureSession::Action action, bool hasQuickPurpose)
{
    if (action == CaptureSession::Action::Cancel)
        return false;
    if (hasQuickPurpose)
        return true;
    using Action = CaptureSession::Action;
    switch (action) {
    case Action::Pin:
    case Action::CopyPin:
    case Action::Ocr:
    case Action::OcrSearch:
    case Action::OcrWeb:
    case Action::OcrTranslate:
    case Action::Qr:
    case Action::Long:
    case Action::Record:
        return true;
    default:
        return false;
    }
}

const char* closeReasonName(capsurface::CloseReason reason)
{
    switch (reason) {
    case capsurface::CloseReason::Requested: return "requested";
    case capsurface::CloseReason::Deactivated: return "deactivated";
    case capsurface::CloseReason::FullScreenGone: return "full-screen-gone";
    case capsurface::CloseReason::Error: return "error";
    }
    return "other";
}

} // namespace

Capture::Capture(QQmlEngine* engine, Callbacks callbacks, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_callbacks(std::move(callbacks))
{
    m_session.setOcrAvailable(CaptureOcr::available());
    // The redaction's scans (the pre-scan among them) timed for the diagnostics.
    if (auto scan = m_callbacks.smart.scan) {
        m_callbacks.smart.scan = [this, scan](const QImage& image, const QRect& region, double dpr, std::function<void(RedactScan)> done) {
            const int ticket = ++m_scanTicket;
            m_scanRunning = true;
            QElapsedTimer began;
            began.start();
            scan(image, region, dpr, [self = QPointer<Capture>(this), ticket, began, done = std::move(done)](RedactScan result) {
                if (self) {
                    if (ticket == self->m_scanTicket)
                        self->m_scanRunning = false;
                    if (self->m_prescanMs < 0)
                        self->m_prescanMs = began.elapsed();
                }
                done(std::move(result));
            });
        };
    }
    if (auto cancel = m_callbacks.smart.cancel) {
        m_callbacks.smart.cancel = [this, cancel] {
            m_scanRunning = false;
            cancel();
        };
    }
    m_session.setSmartHooks(m_callbacks.smart);
    m_watchTimer.setTimerType(Qt::PreciseTimer);
    m_watchTimer.setInterval(diag::kTickMs);
    connect(&m_watchTimer, &QTimer::timeout, this, &Capture::watchTick);
    m_draftTimer.setSingleShot(true);
    m_draftTimer.setInterval(kDraftMs);
    connect(&m_draftTimer, &QTimer::timeout, this, [this] { m_draft.reset(); });

    connect(&m_session, &CaptureSession::finished, this, &Capture::finish);
    connect(&m_session, &CaptureSession::historyStep, this, &Capture::stepHistory);
    connect(&m_session, &CaptureSession::colorCopied, this, [this](const QString& color) {
        if (m_callbacks.copyColor)
            m_callbacks.copyColor(color);
    });
    connect(&m_session, &CaptureSession::colorFormatChanged, this, [this](ColorFormat format) {
        if (m_callbacks.colorFormatChanged)
            m_callbacks.colorFormatChanged(colorFormatName(format));
    });
    connect(&m_session, &CaptureSession::sizePresetsEdited, this, [this] {
        if (m_callbacks.sizePresetsChanged)
            m_callbacks.sizePresetsChanged(sizePresetsText(m_session.sizePresets()));
    });
    connect(&m_session, &CaptureSession::keyHintsKept, this, [this](bool on) {
        if (m_callbacks.keyHintsKept)
            m_callbacks.keyHintsKept(on);
    });
    connect(&m_session, &CaptureSession::windowsOnlyChanged, this, [this](bool on) {
        if (m_callbacks.windowsOnlyChanged)
            m_callbacks.windowsOnlyChanged(on);
    });
    m_countTimer.setSingleShot(true);
    m_countTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_countTimer, &QTimer::timeout, this, &Capture::tickCountdown);
    connect(&m_session, &CaptureSession::cursorMoveRequested, this,
        [](const QPoint& global) { ::SetCursorPos(global.x(), global.y()); });

    m_flyoutWait.setInterval(10ms);
    connect(&m_flyoutWait, &QTimer::timeout, this, [this] {
        if (::GetForegroundWindow() == m_flyout && m_clock.elapsed() - m_listMs < kFlyoutWaitMs)
            return;
        m_flyoutWait.stop();
        m_flyoutMs = m_clock.elapsed() - m_listMs;
        findFlyout();
        m_flyout = nullptr;
        show();
    });
    m_fullScreenWait.setInterval(10ms);
    connect(&m_fullScreenWait, &QTimer::timeout, this, &Capture::fullScreenTick);
    m_putBackWait.setInterval(10ms);
    connect(&m_putBackWait, &QTimer::timeout, this, &Capture::putBackTick);
    m_exitFullScreenWait.setInterval(10ms);
    connect(&m_exitFullScreenWait, &QTimer::timeout, this, &Capture::exitFullScreenTick);
    m_surface = std::make_unique<CaptureSurfaceBridge>(this);
    connect(m_surface.get(), &CaptureSurfaceBridge::ready, this, &Capture::onSurfaceReady);
    connect(m_surface.get(), &CaptureSurfaceBridge::failed, this, &Capture::onSurfaceFailed);
    connect(m_surface.get(), &CaptureSurfaceBridge::closedExternally, this, &Capture::onSurfaceClosedExternally);
    m_hideTimeout.setSingleShot(true);
    m_hideTimeout.setInterval(kHideTimeout);
    connect(&m_hideTimeout, &QTimer::timeout, this, &Capture::hideAll);

    m_rebuild.setSingleShot(true);
    m_rebuild.setInterval(300ms);
    connect(&m_rebuild, &QTimer::timeout, this, [this] {
        if (m_state == State::Showing || m_state == State::Waiting)
            m_session.act(static_cast<int>(CaptureSession::Action::Cancel));
        if (m_recorder && m_recorder->active())
            m_recorder->finish();
        if (m_long && m_long->running())
            m_long->finish(); // its region may be gone; the grabber waits for its frames to stop
        if (busy()) {
            m_rebuild.start(); // once the capture is over
            return;
        }
        destroyOverlays();
        m_grabber.refresh();
        buildOverlays();
    });
    connect(qApp, &QGuiApplication::screenAdded, this, &Capture::screensChanged);
    connect(qApp, &QGuiApplication::screenRemoved, this, &Capture::screensChanged);
    buildOverlays();
}

Capture::~Capture()
{
    stopSurface();
    m_session.end();
    destroyOverlays();
}

void Capture::screensChanged()
{
    m_rebuild.start();
}

void Capture::buildOverlays()
{
    const QList<QScreen*> screens = QGuiApplication::screens();
    // The overlays present by copying, not by flipping (read when a window's
    // graphics are made, so set until they all are: warmed()). A flip swap
    // chain that covers a monitor, opaque and topmost, is what Windows takes
    // out of composition and hands to the display directly (independent
    // flip, an overlay plane). The switch is where drivers go wrong: the
    // screen black for a moment as the overlay comes, the mouse pointer (a
    // plane of its own) gone or broken over it. Copied, the overlay is
    // always composed with the desktop like any window, pointer included.
    // It draws only when something changes, so the copy costs nothing seen.
    if (!screens.isEmpty() && !qEnvironmentVariableIsSet("QT_D3D_NO_FLIP")
        && !qEnvironmentVariableIsSet("WINSHUN_CAPTURE_FLIP")) {
        qputenv("QT_D3D_NO_FLIP", "1");
        m_legacySwapSet = true;
    }
    // A picture of each monitor for the hidden first show: the grab, the
    // texture's upload and the swap chain at the monitor's size are then
    // behind the first real capture (150-650 ms measured for it, 60 after).
    m_warmClock.start();
    const std::vector<Grabber::Shot> shots = m_grabber.grab();
    for (int i = 0; i < screens.size(); ++i) {
        QScreen* screen = screens[i];
        // A monitor changing size or scale changes everything about its overlay.
        connect(screen, &QScreen::geometryChanged, this, &Capture::screensChanged, Qt::UniqueConnection);
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, &Capture::screensChanged, Qt::UniqueConnection);
        Overlay overlay;
        overlay.screen = screen;
        overlay.monitor = monitorOf(screen);
        overlay.model = new CaptureScreen(i, this);
        QQmlComponent component(m_engine, u"WinShun"_s, u"CaptureOverlay"_s);
        QObject* object = component.createWithInitialProperties({
            {u"session"_s, QVariant::fromValue(&m_session)},
            {u"screenModel"_s, QVariant::fromValue(overlay.model)},
        });
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << "Screen capture: no overlay window:" << component.errorString();
            delete object;
            delete overlay.model;
            continue;
        }
        overlay.window = window;
        if (m_callbacks.windowCreated)
            m_callbacks.windowCreated(window, u"capture-overlay-%1"_s.arg(i));
        window->setScreen(screen);
        window->setGeometry(screen->geometry());
        CaptureScreen* model = overlay.model;
        const auto follow = [model, window, monitor = overlay.monitor] {
            model->setGeometry(monitorRect(monitor), window->devicePixelRatio(), window->size());
        };
        connect(window, &QWindow::widthChanged, model, follow);
        connect(window, &QWindow::heightChanged, model, follow);
        follow();
        connect(window, &QQuickWindow::frameSwapped, this, [this, window] { frameDrawn(window); }, Qt::QueuedConnection);
        m_graphicsLoss.watch(window);
        window->create();
        // Its graphics set up now, out of sight: a first show costs 130–170 ms,
        // later ones 3–6 ms (stage 0).
        overlay.warming = 1;
        overlay.cloaked = true;
        win::setCloaked(window, true);
        if (const auto shot = std::ranges::find(shots, overlay.monitor, &Grabber::Shot::monitor);
            shot != shots.end() && !shot->image.isNull()) {
            const QRect& rect = shot->rect;
            ::SetWindowPos(reinterpret_cast<HWND>(window->winId()), nullptr, rect.x(), rect.y(), rect.width(), rect.height(),
                SWP_NOACTIVATE | SWP_NOZORDER);
            model->setGeometry(rect, window->devicePixelRatio(), window->size());
            model->setImage(shot->image);
            overlay.warming = 2;
        }
        window->show();
        m_overlays.push_back(overlay);
    }
    if (m_overlays.empty())
        warmed();
}

void Capture::warmed()
{
    if (std::exchange(m_legacySwapSet, false))
        qunsetenv("QT_D3D_NO_FLIP"); // the plugin's other windows flip as before
    if (m_warmClock.isValid()) {
        qInfo().noquote() << u"Screen capture: %1 overlay(s) warmed in %2 ms"_s.arg(m_overlays.size()).arg(m_warmClock.elapsed());
        m_warmClock.invalidate();
    }
}

void Capture::destroyOverlays()
{
    for (Overlay& overlay : m_overlays) {
        delete overlay.window;
        delete overlay.model;
    }
    m_overlays.clear();
}

void Capture::trigger()
{
    if (m_recorder && m_recorder->active()) {
        m_recorder->finish();
        return;
    }
    if (m_countdown) { // the hotkey again: the wait is over, nothing is taken
        cancelCountdown();
        return;
    }
    if (m_long && m_long->running()) {
        m_long->finish();
        return;
    }
    switch (m_state) {
    case State::Idle:
        start();
        break;
    case State::Waiting:
    case State::Showing:
        m_session.act(static_cast<int>(CaptureSession::Action::Cancel));
        break;
    case State::ChoosingSave:
    case State::Closing:
        break;
    }
}

void Capture::triggerOcr(OcrPurpose purpose)
{
    triggerQuick(purpose == OcrPurpose::Translate ? QuickPurpose::Translate : QuickPurpose::ReadText);
}

void Capture::triggerPin()
{
    triggerQuick(QuickPurpose::Pin);
}

void Capture::triggerQuick(QuickPurpose purpose)
{
    if (purpose == QuickPurpose::None)
        return;
    if ((m_recorder && m_recorder->active()) || m_countdown || (m_long && m_long->running()))
        return;
    if (m_state == State::Idle) {
        start(purpose);
        return;
    }
    if (m_state != State::Waiting && m_state != State::Showing)
        return;
    if (m_quickPurpose == purpose) {
        m_session.act(static_cast<int>(CaptureSession::Action::Cancel)); // what the key opened, it closes
        return;
    }
    // Pressing the capture-and-pin hotkey while a regular capture already has a selection pins it directly.
    if (purpose == QuickPurpose::Pin && m_quickPurpose == QuickPurpose::None && m_session.hasSelection()) {
        m_session.act(static_cast<int>(CaptureSession::Action::Pin));
        return;
    }
    if (m_quickPurpose != QuickPurpose::None || (purpose == QuickPurpose::Pin && !m_session.hasSelection())) {
        m_quickPurpose = purpose; // switch the active picking purpose seamlessly
        m_session.setQuickMode(purpose == QuickPurpose::Pin ? CaptureSession::QuickMode::Pin : CaptureSession::QuickMode::Ocr);
        if (m_state == State::Showing)
            m_session.notify(quickNotice());
    }
}

QString Capture::quickNotice() const
{
    switch (m_quickPurpose) {
    case QuickPurpose::Translate:
        return tr("截图翻译：拖选区域，或单击窗口 · Esc 取消");
    case QuickPurpose::ReadText:
        return tr("识别文字：拖选区域，或单击窗口 · Esc 取消");
    case QuickPurpose::Pin:
        return tr("截图并贴图：拖选区域，或单击窗口 · Esc 取消");
    case QuickPurpose::None:
        break;
    }
    return {};
}

void Capture::start(QuickPurpose purpose)
{
    const bool quiet = std::exchange(m_quietStart, false);
    if (m_overlays.empty())
        return;
    if (m_callbacks.starting)
        m_callbacks.starting();
    m_quickPurpose = purpose;
    const bool ocrOnly = purpose == QuickPurpose::ReadText || purpose == QuickPurpose::Translate;
    m_clock.start();
    m_faultsBefore = pageFaults();
    m_reported = false;
    m_flyoutMs = -1;
    if (!ocrOnly) { // reading words takes no picture: the draft keeps its time
        m_draft.reset(); // a new capture ends the draft's time
        m_draftTimer.stop();
    }
    m_session.setStyles(annot::StyleMemory::fromJson(m_callbacks.annotateStyles ? m_callbacks.annotateStyles() : QString()));
    // Read before anything of ours shows: the Start menu, search or a shell
    // flyout in front stays in the picture (wanted), and is closed after.
    const HWND foreground = ::GetForegroundWindow();
    const bool flyout = win::isShellFlyout(foreground);
    // A Store app in full screen stays in the picture too: normally covered
    // directly by a Band-2 UIAccess DWM mirror surface (m_surface) without
    // changing the player's state, falling back to Win+Shift+Enter only when
    // the service is unavailable.
    const bool fullScreen = !flyout && fullScreenStoreApp(foreground);
    m_fullScreenWait.stop();
    m_putBackWait.stop(); // a capture right after one that put an app back: this one's
    m_exitFullScreenWait.stop();
    stopSurface();
    m_putBack = false;
    m_exitFullScreenOnHide = false;
    m_fullScreen = nullptr;
    m_fullScreenSent = false;
    m_fullScreenMs = -1;
    m_shots = m_grabber.grab(&m_grabReport, m_hdrFile);
    if (m_includeCursor)
        drawCursorInShots();
    m_grabMs = m_clock.elapsed();
    m_wgcShots = static_cast<int>(std::ranges::count_if(m_shots, [](const Grabber::Shot& shot) { return shot.wgc; }));
    m_hdrShots = static_cast<int>(std::ranges::count_if(m_shots, [](const Grabber::Shot& shot) { return shot.hdr; }));
    m_prescanMs = -1;
    m_scanRunning = false;
    std::vector<HWND> overlayWindows;
    for (const Overlay& overlay : m_overlays)
        if (overlay.window)
            overlayWindows.push_back(reinterpret_cast<HWND>(overlay.window->winId()));
    m_snapshot = WindowSnapshot::take(overlayWindows);
    m_listMs = m_clock.elapsed();

    std::vector<CaptureScreen*> screens;
    for (Overlay& overlay : m_overlays) {
        const auto shot = std::ranges::find(m_shots, overlay.monitor, &Grabber::Shot::monitor);
        if (shot == m_shots.end() || !overlay.window) {
            overlay.model->setImage({});
            continue;
        }
        overlay.model->setGeometry(shot->rect, overlay.window->devicePixelRatio(), overlay.window->size());
        overlay.model->setWorkArea(workRect(overlay.monitor)); // the taskbar may have moved since the last capture
        overlay.model->setImage(shot->image);
        screens.push_back(overlay.model);
    }
    if (screens.empty()) {
        qWarning() << "Screen capture: no monitor matched the overlays; made again";
        m_shots.clear();
        m_snapshot.clear();
        screensChanged();
        return;
    }
    // The models keep their overlay's index: the session addresses them by it.
    std::vector<CaptureScreen*> byIndex(m_overlays.size(), nullptr);
    for (CaptureScreen* s : screens)
        byIndex[static_cast<std::size_t>(s->index())] = s;
    std::erase(byIndex, nullptr);
    CaptureSession::Windows windows;
    windows.tops = m_snapshot.windows();
    std::size_t extra = 0;
    if (flyout) {
        // The Start menu takes the taskbar into its band of the Z order:
        // EnumWindows lists neither then (the menu is found once it closed,
        // findFlyout). The taskbar goes over the windows listed.
        for (const wchar_t* name : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}) {
            for (HWND bar = ::FindWindowW(name, nullptr); bar; bar = ::FindWindowExW(nullptr, bar, name, nullptr)) {
                RECT r {};
                if (::IsWindowVisible(bar) && ::GetWindowRect(bar, &r) && !::IsRectEmpty(&r)) {
                    windows.tops.insert(windows.tops.begin(), QRect(r.left, r.top, r.right - r.left, r.bottom - r.top));
                    ++extra;
                }
            }
        }
    }
    // Nor does EnumWindows list a Store app's frame in full screen (measured):
    // the hover found the windows under it, not in the picture. Over them all.
    if (RECT r {}; fullScreen && ::GetWindowRect(foreground, &r) && !::IsRectEmpty(&r)) {
        windows.tops.insert(windows.tops.begin(), QRect(r.left, r.top, r.right - r.left, r.bottom - r.top));
        ++extra;
    }
    windows.children = [this, extra](std::size_t i) {
        return i < extra ? std::vector<QRect> {} : m_snapshot.children(i - extra);
    };
    if (m_uiaEnabled) {
        windows.probe = [this, extra](std::size_t i, QPoint point) {
            if (i >= extra)
                askUia(i - extra, point);
        };
    }
    forgetHistory();
    m_liveWindows = windows;
    m_session.setQuickMode(purpose == QuickPurpose::Pin ? CaptureSession::QuickMode::Pin
        : ocrOnly                                       ? CaptureSession::QuickMode::Ocr
                                                        : CaptureSession::QuickMode::None);
    m_session.begin(byIndex, std::move(windows));
    m_beginMs = m_clock.elapsed();
    if (quiet)
        m_session.quietKeyHints(); // a delayed capture is for a passing moment (KeyHintPolicy rule 7)

    if (flyout) {
        sendEscape();
        m_flyout = foreground;
        m_flyoutMonitor = ::MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
        m_state = State::Waiting;
        m_flyoutWait.start();
        return;
    }
    if (fullScreen) {
        m_fullScreen = foreground;
        m_fullScreenClock.start();
        if (m_callbacks.openCaptureSurface && m_callbacks.closeCaptureSurface && m_surface) {
            POINT cursor {};
            ::GetCursorPos(&cursor);
            const HMONITOR fsMon = ::MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
            std::vector<CaptureSurfaceBridge::Target> targets;
            int cursorTarget = -1;
            int fsTarget = -1;
            for (const Overlay& overlay : m_overlays) {
                if (!overlay.window || overlay.model->image().isNull())
                    continue;
                const int idx = static_cast<int>(targets.size());
                const QRect rect = overlay.model->rect();
                if (rect.contains(cursor.x, cursor.y))
                    cursorTarget = idx;
                if (overlay.monitor == fsMon)
                    fsTarget = idx;
                targets.push_back({overlay.window, rect, overlay.model->dpr()});
            }
            const int activeIndex = cursorTarget >= 0 ? cursorTarget : (fsTarget >= 0 ? fsTarget : 0);
            if (!targets.empty()
                && m_surface->start(foreground, std::move(targets), activeIndex, m_callbacks.openCaptureSurface,
                    m_callbacks.closeCaptureSurface)) {
                m_usingSurface = true;
                show();
                return;
            }
        }
        m_state = State::Waiting;
        m_fullScreenWait.start();
        return;
    }
    show();
}

void Capture::fullScreenTick()
{
    if (m_state != State::Waiting || !m_fullScreen) {
        m_fullScreenWait.stop();
        return;
    }
    if (!m_fullScreenSent) {
        if (modifiersDown() && m_fullScreenClock.elapsed() < kKeysUpWaitMs)
            return;
        sendFullScreenSwitch();
        m_fullScreenSent = true;
        m_fullScreenClock.restart();
        return;
    }
    // Closed meanwhile: nothing over the overlays either.
    const DWORD band = ::IsWindow(m_fullScreen) ? windowBand(m_fullScreen) : kDesktopBand;
    if (band != kDesktopBand && m_fullScreenClock.elapsed() < kFullScreenWaitMs)
        return;
    m_fullScreenWait.stop();
    m_fullScreenMs = m_fullScreenClock.elapsed();
    const QString program = foreground::programOf(m_fullScreen);
    diagnostics::record(u"capture-full-screen-app"_s, {{u"program"_s, program}, {u"band"_s, static_cast<qint64>(band)},
        {u"left_full_screen"_s, band == kDesktopBand}, {u"ms"_s, m_fullScreenMs}});
    if (band == kDesktopBand) {
        show();
        return;
    }
    // Still over every window of ours: overlays under it would show to no one
    // and take no key. Ended, and said why.
    qWarning().noquote() << u"Screen capture: %1 stayed in full screen (band %2) %3 ms after Win+Shift+Enter; the capture ended"_s
                                .arg(program)
                                .arg(band)
                                .arg(m_fullScreenMs);
    m_fullScreen = nullptr;
    m_session.act(static_cast<int>(CaptureSession::Action::Cancel));
    if (m_callbacks.cannotShow)
        m_callbacks.cannotShow(tr("全屏的应用挡住了截图，请先退出全屏再截图"));
}

void Capture::putBackTick()
{
    const HWND app = m_fullScreen;
    const auto done = [this] {
        m_putBackWait.stop();
        m_fullScreen = nullptr;
    };
    if (m_state != State::Idle || !app || !::IsWindow(app)) {
        done();
        return;
    }
    const qint64 now = m_fullScreenClock.elapsed();
    if (m_putBackSent > 0) {
        if (windowBand(app) != kDesktopBand) {
            qInfo().noquote() << u"Screen capture: %1 back in full screen (%2 ms after the overlays went, %3 switch(es))"_s
                                     .arg(foreground::programOf(app))
                                     .arg(now)
                                     .arg(m_putBackSent);
            done();
            return;
        }
        if (now - m_putBackSentMs < kPutBackCheckMs)
            return;
        if (m_putBackSent >= 2) {
            qWarning().noquote() << u"Screen capture: %1 did not go back into full screen after %2 switches"_s.arg(
                foreground::programOf(app)).arg(m_putBackSent);
            done();
            return;
        }
    }
    if (::GetForegroundWindow() != app || modifiersDown()) {
        m_putBackReadyMs = -1;
        if (now >= kPutBackWaitMs) {
            const HWND front = ::GetForegroundWindow();
            qInfo().noquote() << u"Screen capture: %1 left out of full screen (%2 in front%3)"_s.arg(
                foreground::programOf(app), front ? foreground::programOf(front) : u"no window"_s,
                modifiersDown() ? u", keys down"_s : QString());
            done();
        }
        return;
    }
    if (m_putBackReadyMs < 0)
        m_putBackReadyMs = now;
    if (now - m_putBackReadyMs < kPutBackSettleMs)
        return;
    sendFullScreenSwitch();
    ++m_putBackSent;
    m_putBackSentMs = now;
}

void Capture::exitFullScreenTick()
{
    const HWND app = m_fullScreen;
    const auto done = [this] {
        m_exitFullScreenWait.stop();
        m_fullScreen = nullptr;
    };
    if (m_state != State::Idle || !app || !::IsWindow(app) || windowBand(app) == kDesktopBand) {
        done();
        return;
    }
    if (m_fullScreenClock.elapsed() >= kFullScreenWaitMs) {
        done();
        return;
    }
    if (::GetForegroundWindow() != app || modifiersDown())
        return;
    sendFullScreenSwitch();
    done();
}

void Capture::stopSurface()
{
    m_usingSurface = false;
    if (m_surface)
        m_surface->stop();
}

void Capture::onSurfaceReady(const std::vector<HWND>&)
{
    if (m_state != State::Showing || !m_usingSurface)
        return;
    m_fullScreenMs = m_fullScreenClock.elapsed();
    const QString program = m_fullScreen ? foreground::programOf(m_fullScreen) : QString();
    diagnostics::record(u"capture-full-screen-app"_s, {{u"program"_s, program},
        {u"band"_s, static_cast<qint64>(m_fullScreen ? windowBand(m_fullScreen) : 0)},
        {u"mode"_s, u"surface"_s}, {u"left_full_screen"_s, false}, {u"ms"_s, m_fullScreenMs}});
    startWatch();
    m_reported = true;
    m_upClock.start();
    m_upFaults = static_cast<qint64>(pageFaults() - m_faultsBefore);
    qInfo().noquote() << u"Screen capture: overlay up %1 ms after the hotkey (grab %2 ms: %3; %4 windows listed in %5 ms; session begun at %7 ms, windows shown at %8 ms; %9 page faults; over full-screen %10 via UIAccess surface in %6 ms)"_s
                             .arg(m_clock.elapsed())
                             .arg(m_grabMs)
                             .arg(m_grabReport)
                             .arg(m_snapshot.windows().size())
                             .arg(m_listMs - m_grabMs)
                             .arg(m_fullScreenMs)
                             .arg(m_beginMs)
                             .arg(m_showMs)
                             .arg(m_upFaults)
                             .arg(program);
    if (POINT at {}; ::GetCursorPos(&at) && pointerOverOverlay(at))
        ::SetCursorPos(at.x, at.y);
    warmed();
    afterUp();
}

void Capture::onSurfaceFailed(const QString& reason, unsigned long error)
{
    if (!m_usingSurface)
        return;
    stopSurface();
    if (m_state != State::Showing && m_state != State::Waiting)
        return;
    const QString program = m_fullScreen ? foreground::programOf(m_fullScreen) : QString();
    qInfo().noquote() << u"Screen capture: UIAccess surface unavailable over %1 (%2, error %3); falling back to Win+Shift+Enter"_s
                             .arg(program, reason)
                             .arg(error);
    diagnostics::record(u"capture-full-screen-app"_s, {{u"program"_s, program},
        {u"band"_s, static_cast<qint64>(m_fullScreen ? windowBand(m_fullScreen) : 0)},
        {u"mode"_s, u"surface-fallback"_s}, {u"reason"_s, reason}, {u"error"_s, static_cast<qint64>(error)}});
    // Hide Band-1 overlays cleanly before the Win+Shift+Enter fallback path
    // takes the player out of full screen and calls show() afresh.
    for (Overlay& overlay : m_overlays) {
        if (overlay.window && overlay.window->isVisible())
            overlay.window->hide();
        if (overlay.window && overlay.cloaked)
            win::setCloaked(overlay.window, false);
        overlay.cloaked = false;
    }
    m_state = State::Waiting;
    m_fullScreenSent = false;
    m_fullScreenClock.restart();
    m_fullScreenWait.start();
}

void Capture::onSurfaceClosedExternally(capsurface::CloseReason reason)
{
    if (!m_usingSurface)
        return;
    stopSurface();
    if (m_state != State::Showing && m_state != State::Waiting)
        return;
    qInfo().noquote() << u"Screen capture: UIAccess surface closed (%1); ending capture"_s.arg(
        QString::fromLatin1(closeReasonName(reason)));
    m_session.act(static_cast<int>(CaptureSession::Action::Cancel));
}

void Capture::show()
{
    m_state = State::Showing;
    POINT cursor {};
    ::GetCursorPos(&cursor);
    QQuickWindow* under = nullptr;
    int underIndex = -1;
    QPoint underAt;
    for (Overlay& overlay : m_overlays) {
        if (!overlay.window || overlay.model->image().isNull())
            continue;
        const QRect rect = overlay.model->rect();
        // Exactly the monitor, in physical pixels: Qt's logical geometry
        // would reach a pixel into the next monitor at 150% (1707 logical
        // is 2561 pixels).
        ::SetWindowPos(reinterpret_cast<HWND>(overlay.window->winId()), HWND_TOPMOST, rect.x(), rect.y(), rect.width(),
            rect.height(), SWP_NOACTIVATE);
        overlay.model->setGeometry(rect, overlay.window->devicePixelRatio(), overlay.window->size());
        overlay.cloaked = true;
        overlay.warming = 0;
        win::setCloaked(overlay.window, true); // until its first frame with the picture
        if (m_usingSurface)
            overlay.window->setProperty("_q_showWithoutActivating", true);
        overlay.window->show();
        if (m_usingSurface)
            overlay.window->setProperty("_q_showWithoutActivating", QVariant());
        if (rect.contains(cursor.x, cursor.y)) {
            under = overlay.window;
            underIndex = overlay.model->index();
            underAt = QPoint(cursor.x, cursor.y) - rect.topLeft();
        }
    }
    if (!under) {
        for (const Overlay& overlay : m_overlays) {
            if (overlay.window && overlay.window->isVisible()) {
                under = overlay.window;
                break;
            }
        }
    }
    if (under && !m_usingSurface) {
        win::bringToFront(under); // the keys: Esc, Enter, the arrows
        under->requestActivate();
    }
    m_showMs = m_clock.elapsed();
    if (underIndex >= 0)
        m_session.pointerMove(underIndex, underAt); // the hover and the magnifier before the mouse moves
    if (m_quickPurpose != QuickPurpose::None) // where the pointer is, at the top: what this capture is for
        m_session.notify(quickNotice());
    if (!m_usingSurface)
        startWatch();
    // Nothing more here: the first frame is drawn when this returns to the
    // event loop, and what is done before that is time with no overlay.
}

void Capture::afterUp()
{
    m_pointerFacts = diag::pointerFacts(); // the registry, the system's cursors
    // The capture before, read meanwhile: , shows it at once (regular capture only).
    const bool regular = m_quickPurpose == QuickPurpose::None;
    if (m_history.enabled() && regular) {
        m_historyIds = m_history.ids(); // a folder listed
        prefetchHistory(0);
    }
    m_session.setHistoryAvailable(m_history.enabled() && regular && !m_historyIds.isEmpty()); // the key hints' , .
    if (m_callbacks.shown)
        m_callbacks.shown();
}

void Capture::startWatch()
{
    m_stalls.reset();
    m_ticks = 0;
    m_graphicsLoss.begin();
    {
        // The monitor the selection is being made on, where the pointer is.
        POINT at {};
        ::GetCursorPos(&at);
        MONITORINFOEXW monitor {};
        monitor.cbSize = sizeof monitor;
        ::GetMonitorInfoW(::MonitorFromPoint(at, MONITOR_DEFAULTTOPRIMARY), &monitor);
        m_pulse.arm(QString::fromWCharArray(monitor.szDevice));
        m_pulseLongest = {};
    }
    m_upClock.invalidate();
    m_cursorShown = {};
    m_cursorDragging = {};
    // As early as it can be: a cold stall has come 12 ms after the overlay was up.
    m_shownClock.start();
    m_tickClock.start();
    m_watchTimer.start();
}

namespace {

HWND rootAt(POINT at)
{
    const HWND hit = ::WindowFromPoint(at);
    return hit ? ::GetAncestor(hit, GA_ROOT) : nullptr;
}

} // namespace

bool Capture::isOverlay(HWND root) const
{
    if (!root)
        return false;
    if (m_surface && m_surface->isSurfaceHwnd(root))
        return true;
    return std::ranges::any_of(m_overlays, [root](const Overlay& o) {
        return o.window && reinterpret_cast<HWND>(o.window->winId()) == root;
    });
}

bool Capture::pointerOverOverlay(POINT at) const
{
    return isOverlay(rootAt(at));
}

void Capture::watchTick()
{
    const qint64 interval = m_tickClock.restart();
    ++m_ticks;
    const qint64 sinceShow = m_shownClock.elapsed();
    const qint64 sinceUp = m_upClock.isValid() ? m_upClock.elapsed() : -1;
    const QString phase = m_session.phaseName();
    const QRect selected = m_session.hasSelection() ? m_session.selectionRect() : QRect();
    const DisplayPulse::Gaps gaps = takePulse();
    if (const auto stall = m_stalls.note(interval, sinceShow, phase); stall && stall->record) {
        QJsonObject details {{u"stall_ms"_s, stall->ms}, {u"ms_since_overlay_up"_s, sinceUp},
            {u"ms_since_show"_s, sinceShow}, {u"ms_since_release"_s, m_session.msSinceRelease()}, {u"phase"_s, phase},
            {u"selection"_s, sizeText(selected.size())}, {u"prescan_running"_s, m_scanRunning},
            {u"first_since_start"_s, g_firstCapture}, {u"graphics_lost"_s, m_graphicsLoss.since()},
            {u"vblank_gap_ms"_s, gaps.vblankMs}, {u"dwm_gap_ms"_s, gaps.dwmMs}};
        if (!gaps.vblankError.isEmpty())
            details[u"vblank_error"_s] = gaps.vblankError;
        diagnostics::record(u"capture-stall"_s, details);
    }
    CURSORINFO info {sizeof info};
    if (!::GetCursorInfo(&info))
        return;
    const bool noCursor = !info.hCursor;
    if (const HWND root = rootAt(info.ptScreenPos); !isOverlay(root)) {
        // Once the overlay is up, another program's window here is over it,
        // and that program sets the pointer there: one of its own topmost
        // windows, a tooltip, an input method's bar. Our other windows (the
        // save picker) are not counted.
        DWORD pid = 0;
        if (sinceUp < 0 || !root || !::GetWindowThreadProcessId(root, &pid) || pid == ::GetCurrentProcessId())
            return;
        wchar_t name[256] {};
        ::GetClassNameW(root, name, static_cast<int>(std::size(name)));
        const QString windowClass = QString::fromWCharArray(name);
        if (m_stalls.noteCovered(u"%1|%2"_s.arg(pid).arg(windowClass))) {
            const bool topmost = ::GetWindowLongPtrW(root, GWL_EXSTYLE) & WS_EX_TOPMOST;
            diagnostics::record(u"capture-pointer-covered"_s, {{u"program"_s, foreground::programOf(root)},
                {u"window_class"_s, windowClass}, {u"topmost"_s, topmost}, {u"no_cursor"_s, noCursor},
                {u"flags"_s, static_cast<qint64>(info.flags)}, {u"ms_since_overlay_up"_s, sinceUp}, {u"phase"_s, phase}});
        }
        return;
    }
    if (!diag::cursorHidden(info.flags, info.hCursor))
        m_stalls.noteShown();
    else if (m_stalls.noteHidden()) {
        diagnostics::record(u"capture-cursor-hidden"_s, {{u"flags"_s, static_cast<qint64>(info.flags)},
            {u"no_cursor"_s, noCursor}, {u"ms_since_overlay_up"_s, sinceUp}, {u"ms_since_show"_s, sinceShow},
            {u"phase"_s, phase}});
    }
    // Not at the first tick: the overlay has had a chance to set its cursor.
    if (m_ticks >= 2 && m_cursorShown.isEmpty())
        m_cursorShown = diag::cursorFacts(info.hCursor, info.flags);
    if (phase == u"dragging"_s && m_cursorDragging.isEmpty())
        m_cursorDragging = diag::cursorFacts(info.hCursor, info.flags);
}

DisplayPulse::Gaps Capture::takePulse()
{
    DisplayPulse::Gaps gaps = m_pulse.take();
    m_pulseLongest.vblankMs = std::max(m_pulseLongest.vblankMs, gaps.vblankMs);
    m_pulseLongest.dwmMs = std::max(m_pulseLongest.dwmMs, gaps.dwmMs);
    if (!gaps.vblankError.isEmpty())
        m_pulseLongest.vblankError = gaps.vblankError;
    return gaps;
}

void Capture::recordCapture(CaptureSession::Action action, const QRect& rect)
{
    m_watchTimer.stop();
    takePulse();
    m_pulse.disarm();
    QJsonArray dprs;
    QJsonArray refresh; // with the graphics card and how the overlays are presented: what a black flash or a broken pointer depends on
    for (const Overlay& overlay : m_overlays) {
        dprs.append(overlay.model->dpr());
        refresh.append(overlay.screen ? qRound(overlay.screen->refreshRate()) : 0);
    }
    QJsonArray cards;
    DISPLAY_DEVICEW device {};
    device.cb = sizeof device;
    for (DWORD i = 0; ::EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        const QString name = QString::fromWCharArray(device.DeviceString);
        if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && !cards.contains(name))
            cards.append(name);
    }
    const bool software = QQuickWindow::graphicsApi() == QSGRendererInterface::Software;
    const QRect selected = !rect.isEmpty() ? rect : m_session.hasSelection() ? m_session.selectionRect() : QRect();
    QJsonObject details {{u"overlay_up_ms"_s, m_reported && m_upClock.isValid() ? m_clock.elapsed() - m_upClock.elapsed() : -1},
        {u"grab_ms"_s, m_grabMs}, {u"begun_ms"_s, m_beginMs}, {u"shown_ms"_s, m_showMs},
        {u"page_faults"_s, m_reported ? m_upFaults : -1}, {u"refresh_hz"_s, refresh}, {u"graphics"_s, cards},
        {u"presented"_s, software ? u"software"_s : qEnvironmentVariableIsSet("WINSHUN_CAPTURE_FLIP") ? u"flip"_s : u"copy"_s},
        {u"monitors"_s, static_cast<int>(m_overlays.size())}, {u"dpr"_s, dprs},
        {u"wgc"_s, m_wgcShots}, {u"hdr"_s, m_hdrShots}, {u"magnifier"_s, m_magnifierOn},
        {u"windows_only"_s, m_session.windowsOnly()}, // 只检测窗口 (#360): why no control is picked
        {u"smart_prescan"_s, m_callbacks.smart.prescan && m_callbacks.smart.prescan()},
        {u"first_since_start"_s, g_firstCapture}, {u"seconds_since_start"_s, diag::secondsSinceProcessStart()},
        {u"seconds_since_last_capture"_s, g_lastCapture.isValid() ? g_lastCapture.elapsed() / 1000 : -1},
        {u"session_ms"_s, m_upClock.isValid() ? m_upClock.elapsed() : -1}, {u"outcome"_s, outcomeName(action)},
        {u"selection"_s, sizeText(selected.size())}, {u"stalls"_s, m_stalls.summary()}, {u"pointer"_s, m_pointerFacts},
        {u"cursor_shown"_s, m_cursorShown}};
    if (!m_allGestureFrames.empty())
        details[u"gesture_frames"_s] = diag::frameStats(m_allGestureFrames);
    if (m_prescanMs >= 0)
        details[u"prescan_ms"_s] = m_prescanMs;
    if (const int lost = m_graphicsLoss.since(); lost > 0)
        details[u"graphics_lost"_s] = lost;
    details[u"vblank_gap_max_ms"_s] = m_pulseLongest.vblankMs; // about a refresh, or -1: not measured
    details[u"dwm_gap_max_ms"_s] = m_pulseLongest.dwmMs;
    if (!m_pulseLongest.vblankError.isEmpty())
        details[u"vblank_error"_s] = m_pulseLongest.vblankError;
    if (!m_cursorDragging.isEmpty())
        details[u"cursor_dragging"_s] = m_cursorDragging;
    diagnostics::record(u"capture"_s, details);
    g_firstCapture = false;
    g_lastCapture.start();
}

void Capture::frameDrawn(QQuickWindow* window)
{
    if (m_state == State::Showing && m_session.annotation()->gestureActive()) {
        if (m_gestureFrameOpen)
            m_gestureFrames.push_back(m_frameClock.nsecsElapsed());
        m_frameClock.restart();
        m_gestureFrameOpen = true;
    } else {
        // A gesture just ended: how smooth it was, from the time between its frames.
        if (m_gestureFrameOpen && m_gestureFrames.size() >= 8) {
            std::vector<qint64> frames = m_gestureFrames;
            std::ranges::sort(frames);
            const auto at = [&frames](double q) { return frames[static_cast<std::size_t>(q * (frames.size() - 1))] / 1e6; };
            qInfo().noquote() << u"Screen capture: gesture of %1 frames, between them p50 %2 ms, p95 %3 ms, max %4 ms (%5 items)"_s
                                     .arg(frames.size())
                                     .arg(at(0.5), 0, 'f', 1)
                                     .arg(at(0.95), 0, 'f', 1)
                                     .arg(at(1.0), 0, 'f', 1)
                                     .arg(m_session.annotation()->itemCount());
        }
        if (m_gestureFrameOpen)
            m_allGestureFrames.insert(m_allGestureFrames.end(), m_gestureFrames.begin(), m_gestureFrames.end());
        m_gestureFrames.clear();
        m_gestureFrameOpen = false;
    }
    const auto overlay = std::ranges::find(m_overlays, window, [](const Overlay& o) { return o.window.data(); });
    if (overlay == m_overlays.end() || !overlay->cloaked)
        return;
    if (overlay->warming > 0) {
        if (--overlay->warming > 0) { // drawn with the picture: once more without, its texture gone
            overlay->model->setImage({});
            window->update();
            return;
        }
        overlay->model->setImage({});
        overlay->cloaked = false;
        window->hide();
        win::setCloaked(window, false);
        if (std::ranges::none_of(m_overlays, [](const Overlay& o) { return o.warming > 0; }))
            warmed();
        return;
    }
    if (m_state == State::Closing) {
        if (overlay->closingFrames > 0 && --overlay->closingFrames > 0)
            window->update();
        else if (std::ranges::none_of(m_overlays, [](const Overlay& o) { return o.closingFrames > 0; }))
            hideAll();
        return;
    }
    overlay->cloaked = false;
    win::setCloaked(window, false);
    if (!m_reported && std::ranges::none_of(m_overlays, [](const Overlay& o) { return o.window && o.window->isVisible() && o.cloaked; })) {
        if (m_usingSurface) {
            // Band-1 overlays have rendered and uncloaked behind the Band-8 app;
            // the Band-2 UIAccess surface can now bind its DWM thumbnails and reveal.
            if (m_surface)
                m_surface->setOverlaysReady();
            return;
        }
        m_reported = true;
        m_upClock.start();
        m_upFaults = static_cast<qint64>(pageFaults() - m_faultsBefore);
        qInfo().noquote() << u"Screen capture: overlay up %1 ms after the hotkey (grab %2 ms: %3; %4 windows listed in %5 ms; session begun at %7 ms, windows shown at %8 ms; %9 page faults%6)"_s
                                 .arg(m_clock.elapsed())
                                 .arg(m_grabMs)
                                 .arg(m_grabReport)
                                 .arg(m_snapshot.windows().size())
                                 .arg(m_listMs - m_grabMs)
                                 .arg(m_flyoutMs >= 0 ? u"; waited %1 ms for the Start menu to close"_s.arg(m_flyoutMs)
                                         : m_fullScreen && m_fullScreenMs >= 0
                                         ? u"; %1 out of full screen in %2 ms"_s.arg(foreground::programOf(m_fullScreen)).arg(m_fullScreenMs)
                                         : QString())
                                 .arg(m_beginMs)
                                 .arg(m_showMs)
                                 .arg(m_upFaults);
        // show() brought it to the front for the keys. Not in front now,
        // with no word from bringToFront: something took the foreground
        // back. Say what, so a real case leaves a trace.
        const HWND front = ::GetForegroundWindow();
        const auto isOverlay = [front](const Overlay& o) { return o.window && reinterpret_cast<HWND>(o.window->winId()) == front; };
        if (m_state == State::Showing && std::ranges::none_of(m_overlays, isOverlay)) {
            wchar_t name[256] {};
            if (front)
                ::GetClassNameW(front, name, static_cast<int>(std::size(name)));
            qWarning().noquote() << u"Screen capture: overlay up, but %1 (%2) is in front"_s.arg(
                front ? QString::fromWCharArray(name) : u"no window"_s, front ? foreground::programOf(front) : QString());
        }
        // The overlay's pointer, now. Qt sets a window's cursor when the mouse
        // enters it, and Windows tells of that when a window shows under a
        // still mouse; ours showed cloaked, which is no window to the mouse,
        // and uncloaking tells nothing. Until the mouse next moved the
        // pointer stayed what the program underneath had made it: its
        // arrow or I-beam, or nothing at all where it hides the pointer
        // while typing (measured: the arrow in 16 captures of 17 with the
        // mouse still). Set to where it is, the mouse "moves" there.
        if (POINT at {}; m_state == State::Showing && ::GetCursorPos(&at) && pointerOverOverlay(at))
            ::SetCursorPos(at.x, at.y);
        warmed(); // a capture asked for before the hidden first shows were over
        if (m_state == State::Showing)
            afterUp();
    }
}

void Capture::finish(CaptureSession::Action action, int screen, const QRect& rect)
{
    if (m_state != State::Showing && m_state != State::Waiting)
        return;
    const QuickPurpose quickPurpose = std::exchange(m_quickPurpose, QuickPurpose::None);
    const bool ocrOnly = quickPurpose == QuickPurpose::ReadText || quickPurpose == QuickPurpose::Translate;
    QString savePath;
    if (action == CaptureSession::Action::Save) {
        // The picker over the monitor of the toolbar (one over several monitors has it on one, #409).
        const int home = m_session.homeOf(screen, rect);
        const auto selected = std::ranges::find(m_overlays, home, [](const Overlay& o) { return o.model->index(); });
        if (selected == m_overlays.end() || !selected->window)
            return;
        if (m_usingSurface) {
            // IFileSaveDialog is a Band-1 modal dialog: it would open hidden
            // behind the Band-8 player unless we release the Band-2 mirror and
            // take the player out of full screen first.
            stopSurface();
            if (m_fullScreen && ::IsWindow(m_fullScreen) && windowBand(m_fullScreen) != kDesktopBand) {
                QElapsedTimer wait;
                wait.start();
                while (wait.elapsed() < 400 && (::GetForegroundWindow() != m_fullScreen || modifiersDown()))
                    ::Sleep(10);
                if (::GetForegroundWindow() == m_fullScreen && !modifiersDown()) {
                    sendFullScreenSwitch();
                    m_fullScreenSent = true;
                    wait.restart();
                    while (wait.elapsed() < 500 && windowBand(m_fullScreen) != kDesktopBand)
                        ::Sleep(10);
                }
            }
            win::bringToFront(selected->window);
            selected->window->requestActivate();
        }
        const State before = std::exchange(m_state, State::ChoosingSave);
        // Show() disables its owner. Disable the other overlays too, so a
        // second monitor cannot change the selection while the picker is open.
        std::vector<HWND> disabled;
        for (const Overlay& overlay : m_overlays) {
            if (!overlay.window || overlay.window == selected->window)
                continue;
            const HWND hwnd = reinterpret_cast<HWND>(overlay.window->winId());
            if (::IsWindowEnabled(hwnd)) {
                ::EnableWindow(hwnd, FALSE);
                disabled.push_back(hwnd);
            }
        }
        savePath = chooseScreenshotSavePath(selected->window);
        for (HWND hwnd : disabled)
            if (::IsWindow(hwnd))
                ::EnableWindow(hwnd, TRUE);
        m_state = before;
        if (savePath.isEmpty()) {
            selected->window->requestActivate();
            return;
        }
        m_session.rememberSelection(m_session.virtualRect(screen, rect));
    }
    const bool wasSurface = m_usingSurface;
    // Close the Band-2 mirror BEFORE clearing overlay images so the DWM
    // thumbnail never flashes a cleared frame over the full-screen video.
    stopSurface();
    m_flyoutWait.stop();
    m_flyout = nullptr;
    m_fullScreenWait.stop();
    m_exitFullScreenWait.stop();
    m_putBack = m_fullScreen && m_fullScreenSent && putsBack(action);
    m_exitFullScreenOnHide = wasSurface && m_fullScreen && !m_fullScreenSent
        && needsDesktopBand(action, quickPurpose != QuickPurpose::None);
    if (!m_putBack && !m_exitFullScreenOnHide)
        m_fullScreen = nullptr;
    QImage picture;
    QByteArray project;
    std::optional<QList<RedactLine>> prescanned;
    std::shared_ptr<HdrPixels> hdr; // saved from an HDR monitor: the picture as it showed it
    QPoint origin; // the picture's top left on the screen, physical
    double dpr = 1.0;
    AnnotationModel* annotation = m_session.annotation();
    if (action == CaptureSession::Action::Long || action == CaptureSession::Action::Record) {
        // The selection scrolls once the overlays are gone (hideAll); no picture now.
        for (const Overlay& overlay : m_overlays) {
            if (overlay.model->index() != screen)
                continue;
            const QRect region(overlay.model->rect().topLeft() + rect.topLeft(), rect.size());
            if (action == CaptureSession::Action::Record)
                m_recordRegion = LongRegion {region, overlay.monitor, overlay.model->dpr()};
            else
                m_longRegion = LongRegion {region, overlay.monitor, overlay.model->dpr()};
            // The window the region shows goes in front now, while the overlay has the
            // foreground: when the overlay goes, Windows gives it to the window before it,
            // which need not be this one, and the keys and the scrolling are for this one.
            if (const HWND under = m_snapshot.windowAt(region.center()); under && !::SetForegroundWindow(under))
                qWarning() << "Long screenshot: the window under the region did not come to the front";
        }
    } else if (action != CaptureSession::Action::Cancel && action != CaptureSession::Action::Delay) {
        if (screen == CaptureSession::kDesktop) {
            // Over several monitors (#409): from their pictures put together, as
            // the overlays show them; no HDR file (each monitor has its own).
            picture = m_session.desktopPicture().copy(rect);
            origin = m_session.virtualRect(screen, rect).topLeft();
            const int home = m_session.homeOf(screen, rect);
            for (const Overlay& overlay : m_overlays) {
                if (overlay.model->index() == home)
                    dpr = overlay.model->dpr();
            }
        }
        for (const Overlay& overlay : m_overlays) {
            if (overlay.model->index() == screen) {
                picture = overlay.model->image().copy(rect);
                origin = overlay.model->rect().topLeft() + rect.topLeft();
                dpr = overlay.model->dpr();
                const auto shot = std::ranges::find(m_shots, overlay.monitor, &Grabber::Shot::monitor);
                if ((action == CaptureSession::Action::Save || action == CaptureSession::Action::SavePrevious)
                    && m_historyAt < 0 && shot != m_shots.end() && shot->hdrPixels) {
                    if (HdrPixels region = shot->hdrPixels->copy(rect); !region.isNull())
                        hdr = std::make_shared<HdrPixels>(std::move(region));
                }
            }
        }
        // Annotated: the picture is the composition, and the project goes with
        // it. Not annotated: the plain copy above, as fast as before; reuse any
        // completed smart-redaction prescan so pinning needs no duplicate OCR.
        if (!picture.isNull() && m_session.hasAnnotations() && m_session.annotationScreen() == screen
            && annotation->selectionRect() == rect) {
            const QImage composed = annotation->composed();
            if (composed.size() == picture.size()) {
                if (hdr)
                    paintSdrOver(*hdr, picture, composed); // the annotations in the HDR file too
                picture = composed;
                const annot::Project packed = annotation->project();
                project = annot::writeProject(packed.doc, packed.base);
            }
        } else if (!picture.isNull() && !m_session.hasAnnotations() && m_session.annotationScreen() == screen) {
            prescanned = annotation->prescannedLines(rect);
        }
    } else {
        // Ended on a capture brought back from the history: that one stays in
        // the history, and the draft is what was drawn on the live screens.
        if (m_historyAt >= 0)
            restoreLive();
        if (m_session.hasAnnotations()) {
            // Ended with annotations on the screen (the hotkey, Esc, D): kept for a
            // while, to be found again from the tray menu.
            if (const QRect where = m_session.virtualRect(m_session.annotationScreen(), annotation->selectionRect());
                !where.isNull()) {
                annotation->commitPending();
                m_draft = Draft {annotation->project(), where.topLeft()};
                m_draftTimer.start();
                qInfo().noquote() << u"Screen capture: annotations kept as a draft at %1,%2 (%3x%4)"_s.arg(m_draft->origin.x())
                                         .arg(m_draft->origin.y())
                                         .arg(m_draft->project.base.width())
                                         .arg(m_draft->project.base.height());
            }
        }
    }
    // Taken: into the history, as the overlays show it (a capture brought
    // back and taken again replaces itself there).
    QString captureHistoryId;
    if (!picture.isNull() && m_history.enabled() && !ocrOnly) {
        HistoryShot shot;
        shot.id = QString::number(QDateTime::currentMSecsSinceEpoch());
        for (const Overlay& overlay : m_overlays) {
            if (!overlay.model->hasImage())
                continue;
            if (overlay.model->index() == screen)
                shot.screen = static_cast<int>(shot.screens.size());
            shot.screens.push_back({overlay.model->rect(), overlay.model->dpr(), overlay.model->image()});
        }
        shot.selection = rect;
        shot.spans = screen == CaptureSession::kDesktop; // over several monitors (#409): on their picture together
        if (m_session.hasAnnotations() && m_session.annotationScreen() == screen)
            shot.document = QJsonDocument(annot::toJson(annotation->committedDocument())).toJson(QJsonDocument::Compact);
        if (shot.screen >= 0 || (shot.spans && shot.screens.size() > 1)) {
            captureHistoryId = shot.id;
            m_history.add(std::move(shot), m_historyAt >= 0 ? m_historyShown : QString());
        }
    }
    forgetHistory();
    recordCapture(action, rect);
    m_allGestureFrames.clear();
    m_gestureFrames.clear();
    m_gestureFrameOpen = false;
    m_delayAfter = action == CaptureSession::Action::Delay;
    m_uia.reset(); // a walk still under way finishes by itself
    m_uiaGate = {};
    m_uiaLogged = 0;
    m_uiaChromiumSkipped.clear();
    if (m_session.stylesChanged() && m_callbacks.annotateStylesChanged)
        m_callbacks.annotateStylesChanged(m_session.styles().toJson());
    m_state = State::Closing;
    m_session.end();
    // Cloaked at once (gone from the screen), drawn once more without the
    // pictures so their textures go, then hidden (hideAll).
    for (Overlay& overlay : m_overlays) {
        overlay.model->setImage({});
        if (overlay.window && overlay.window->isVisible()) {
            overlay.cloaked = true;
            win::setCloaked(overlay.window, true);
            // Two frames: one may have been under way with the picture still in it.
            overlay.closingFrames = 2;
            overlay.window->update();
        }
    }
    m_shots.clear();
    m_snapshot.clear();
    m_hideTimeout.start();
    if (std::ranges::none_of(m_overlays, [](const Overlay& o) { return o.closingFrames > 0; }))
        hideAll();

    if (picture.isNull())
        return;
    if (ocrOnly) {
        // Its words only: not the last capture (贴图 pins that), no copy of the picture.
        const QRect where(origin, picture.size());
        if (quickPurpose == QuickPurpose::Translate) {
            // 截图翻译 (#356): what the toolbar's 翻译 does, without the toolbar.
            if (m_callbacks.ocrThen)
                m_callbacks.ocrThen(picture, dpr, where, CaptureSession::Action::OcrTranslate);
        } else if (m_callbacks.readText) {
            m_callbacks.readText(picture, dpr, where);
        }
        return;
    }
    m_last = picture;
    m_lastOrigin = origin;
    m_lastProject = project;
    m_lastClipboardSequence = ::GetClipboardSequenceNumber();
    m_lastClipboardId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PinWindow* pinned = nullptr;
    const Callbacks::Read read = prescanned; // the pin takes its own below
    switch (action) {
    case CaptureSession::Action::CopyPin:
        if (PinManager* pins = m_callbacks.pins ? m_callbacks.pins() : nullptr)
            pinned = pins->pin(picture, origin, true, project, std::move(prescanned));
        [[fallthrough]]; // copying records the picture once, including its editable project
    case CaptureSession::Action::Copy:
        if (m_callbacks.copy)
            m_callbacks.copy(picture, project, m_lastClipboardId, read);
        break;
    case CaptureSession::Action::Save:
    case CaptureSession::Action::SavePrevious:
        save(picture, savePath, std::move(hdr));
        if (m_callbacks.record)
            m_callbacks.record(picture, project, read);
        break;
    case CaptureSession::Action::Pin:
        if (PinManager* pins = m_callbacks.pins ? m_callbacks.pins() : nullptr)
            pinned = pins->pin(picture, origin, true, project, std::move(prescanned));
        if (m_callbacks.record)
            m_callbacks.record(picture, project, read);
        break;
    case CaptureSession::Action::Ocr:
        if (m_callbacks.ocr)
            m_callbacks.ocr(picture, dpr, QRect(origin, picture.size()));
        break;
    case CaptureSession::Action::OcrCopy:
    case CaptureSession::Action::OcrSearch:
    case CaptureSession::Action::OcrWeb:
    case CaptureSession::Action::OcrTranslate:
        if (m_callbacks.ocrThen)
            m_callbacks.ocrThen(picture, dpr, QRect(origin, picture.size()), action);
        break;
    case CaptureSession::Action::Qr: {
        // Read off the GUI thread (a whole screen takes a few hundred ms); the
        // answer comes back through the application object, which outlives this.
        const QPointer<Capture> self(this);
        const QRect where(origin, picture.size());
        std::thread([self, picture, where] {
            QStringList codes = readQrCodes(picture);
            QMetaObject::invokeMethod(qApp, [self, where, codes = std::move(codes)] {
                if (self && self->m_callbacks.qr)
                    self->m_callbacks.qr(codes, where);
            }, Qt::QueuedConnection);
        }).detach();
        break;
    }
    case CaptureSession::Action::Cancel:
    case CaptureSession::Action::Delay:
    case CaptureSession::Action::Long:
    case CaptureSession::Action::Record:
        break;
    }
    if (m_callbacks.captured) {
        m_callbacks.captured(CapturedShot {
            .image = picture,
            .project = project,
            .origin = origin,
            .captureHistoryId = captureHistoryId,
            .clipboardOriginId = m_lastClipboardId,
            .clipboardSeq = m_lastClipboardSequence,
            .pinned = pinned,
        });
    }
}

std::vector<QRect> Capture::shownMonitors() const
{
    std::vector<QRect> monitors;
    for (const Overlay& overlay : m_overlays) {
        if (overlay.model->hasImage())
            monitors.push_back(overlay.model->rect());
    }
    return monitors;
}

void Capture::prefetchHistory(int at)
{
    if (at == m_prefetchAt || at < 0 || at >= m_historyIds.size())
        return;
    if (m_prefetch.valid()) // let go of off this thread: a future of std::async waits as it goes
        std::thread([f = std::move(m_prefetch)]() mutable { f.wait(); }).detach();
    m_prefetchAt = at;
    m_prefetch = std::async(std::launch::async, &CaptureHistory::read, m_history.folder(), m_historyIds[at]);
}

void Capture::forgetHistory()
{
    if (m_prefetch.valid())
        std::thread([f = std::move(m_prefetch)]() mutable { f.wait(); }).detach();
    m_prefetchAt = -1;
    m_historyAt = -1;
    m_historyIds.clear();
    m_historyShown.clear();
    m_liveWindows = {};
    m_liveKept = {};
}

void Capture::stepHistory(int step)
{
    if (m_state != State::Showing)
        return;
    if (!m_history.enabled()) {
        m_session.notify(tr("没有保存之前的画面，可在设置 › 截图里打开"));
        return;
    }
    const std::vector<QRect> monitors = shownMonitors();
    for (int at = m_historyAt + step;; at += step) {
        if (at < 0) {
            if (m_historyAt < 0)
                m_session.notify(tr("已是当前画面，按 , 看之前的"));
            else
                showLive();
            return;
        }
        if (at >= m_historyIds.size()) {
            m_session.notify(m_historyIds.isEmpty() ? tr("还没有之前的画面") : tr("没有更早的画面了"));
            return;
        }
        std::optional<HistoryShot> shot;
        if (at == m_prefetchAt && m_prefetch.valid()) {
            shot = m_prefetch.get();
            m_prefetchAt = -1;
        } else {
            shot = CaptureHistory::read(m_history.folder(), m_historyIds[at]);
        }
        if (!shot || !shot->fits(monitors))
            continue; // taken with other monitors (or unreadable): passed over
        if (m_historyAt < 0)
            m_liveKept = m_session.kept();
        m_historyAt = at;
        showHistoryShot(*shot);
        prefetchHistory(at + step >= 0 ? at + step : at + 1); // the next one this way
        return;
    }
}

void Capture::showHistoryShot(const HistoryShot& shot)
{
    int selectionScreen = shot.spans ? CaptureSession::kDesktop : -1;
    for (const Overlay& overlay : m_overlays) {
        if (!overlay.model->hasImage())
            continue;
        const auto match = std::ranges::find(shot.screens, overlay.model->rect(), &HistoryShot::Screen::rect);
        overlay.model->setImage(match->image);
        if (match - shot.screens.begin() == shot.screen)
            selectionScreen = overlay.model->index();
    }
    std::optional<annot::Document> doc;
    if (!shot.document.isEmpty())
        doc = annot::fromJson(QJsonDocument::fromJson(shot.document).object(), nullptr);
    m_historyShown = shot.id;
    m_session.replaceShot({}, selectionScreen, shot.selection, doc);
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(shot.id.toLongLong());
    m_session.notify(tr("之前的画面 %1/%2 · %3 · , 更早  . 更近")
                         .arg(m_historyAt + 1)
                         .arg(m_historyIds.size())
                         .arg(QLocale().toString(when, when.date() == QDate::currentDate() ? u"HH:mm"_s : u"M/d HH:mm"_s)));
    qInfo().noquote() << u"Screen capture: history %1 of %2 shown"_s.arg(m_historyAt + 1).arg(m_historyIds.size());
}

void Capture::showLive()
{
    restoreLive();
    m_session.notify(tr("回到当前画面"));
    prefetchHistory(0);
}

void Capture::restoreLive()
{
    for (const Overlay& overlay : m_overlays) {
        if (!overlay.model->hasImage())
            continue;
        if (const auto shot = std::ranges::find(m_shots, overlay.monitor, &Grabber::Shot::monitor); shot != m_shots.end())
            overlay.model->setImage(shot->image);
    }
    m_historyAt = -1;
    m_historyShown.clear();
    const CaptureSession::Kept kept = std::exchange(m_liveKept, {});
    m_session.replaceShot(m_liveWindows, kept.screen, kept.rect, kept.doc);
}

void Capture::pinLast()
{
    if (m_last.isNull())
        return;
    if (PinManager* pins = m_callbacks.pins ? m_callbacks.pins() : nullptr)
        pins->pin(m_last, m_lastOrigin, true, m_lastProject);
}

void Capture::restoreDraft()
{
    if (!m_draft)
        return;
    const Draft draft = std::move(*m_draft);
    m_draft.reset();
    m_draftTimer.stop();
    annot::Compositor compositor;
    const QImage picture = compositor.exportImage(draft.project.doc, draft.project.base,
        QRect(QPoint(0, 0), draft.project.base.size()));
    if (picture.isNull())
        return;
    const QByteArray project = annot::writeProject(draft.project.doc, draft.project.base);
    PinWindow* pinned = nullptr;
    if (PinManager* pins = m_callbacks.pins ? m_callbacks.pins() : nullptr)
        pinned = pins->pin(picture, draft.origin, true, project);
    m_last = picture;
    m_lastOrigin = draft.origin;
    m_lastProject = project;
    m_lastClipboardSequence = ::GetClipboardSequenceNumber();
    m_lastClipboardId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (m_callbacks.captured) {
        m_callbacks.captured(CapturedShot {
            .image = picture,
            .project = project,
            .origin = draft.origin,
            .captureHistoryId = {},
            .clipboardOriginId = m_lastClipboardId,
            .clipboardSeq = m_lastClipboardSequence,
            .pinned = pinned,
        });
    }
}

void Capture::findFlyout()
{
    if (!m_flyout || !m_flyoutMonitor)
        return;
    QElapsedTimer clock;
    clock.start();
    const auto before = std::ranges::find(m_shots, m_flyoutMonitor, &Grabber::Shot::monitor);
    if (before == m_shots.end() || ::GetForegroundWindow() == m_flyout)
        return; // still open after the wait: nothing to compare with
    MONITORINFO info {sizeof info};
    if (!::GetMonitorInfoW(m_flyoutMonitor, &info))
        return;
    const QRect work = QRect(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
        info.rcWork.bottom - info.rcWork.top).translated(-before->rect.topLeft());
    const int least = static_cast<int>(std::lround(48 * ::GetDpiForSystem() / 96.0));
    // The monitor again, now without the menu (~11 ms with WGC kept warm):
    // what changed is where the menu was. Only the work area: the taskbar's
    // Start button changes too. The menu is not gone when the focus is: it
    // plays its closing animation first (measured: a first grab found 34x100
    // of it), so grabs follow until the region found stays the same.
    QRect region;
    int grabs = 0;
    for (QRect last; clock.elapsed() < kFlyoutSettleMs; last = region) {
        std::vector<Grabber::Shot> again = m_grabber.grab();
        ++grabs;
        const auto after = std::ranges::find(again, m_flyoutMonitor, &Grabber::Shot::monitor);
        if (after == again.end())
            return;
        region = changedRegion(before->image, after->image, work, 4, 0.3);
        if (region == last && region.width() >= least && region.height() >= least)
            break;
        ::Sleep(15);
    }
    qInfo().noquote() << u"Screen capture: the Start menu was at %1,%2 %3x%4 (found in %5 ms, %6 grabs)"_s.arg(region.x())
                             .arg(region.y())
                             .arg(region.width())
                             .arg(region.height())
                             .arg(clock.elapsed())
                             .arg(grabs);
    if (region.width() >= least && region.height() >= least)
        m_session.setFlyout(region.translated(before->rect.topLeft()));
}

void Capture::hideAll()
{
    if (m_state != State::Closing)
        return;
    m_hideTimeout.stop();
    for (Overlay& overlay : m_overlays) {
        overlay.closingFrames = 0;
        if (overlay.window && overlay.window->isVisible())
            overlay.window->hide();
        if (overlay.window && overlay.cloaked)
            win::setCloaked(overlay.window, false);
        overlay.cloaked = false;
    }
    m_state = State::Idle;
    m_pulse.disarm(); // (recordCapture did, for every way out through finish)
    if (std::exchange(m_putBack, false)) {
        m_fullScreenClock.restart();
        m_putBackSent = 0;
        m_putBackReadyMs = -1;
        m_putBackWait.start(); // once Windows has given the app the foreground back
    } else if (std::exchange(m_exitFullScreenOnHide, false)) {
        m_fullScreenClock.restart();
        m_exitFullScreenWait.start(); // a Band-1 pin/OCR/long/recording window needs Band 8 cleared
    }
    if (m_callbacks.ended)
        m_callbacks.ended();
    if (std::exchange(m_delayAfter, false))
        startDelayed(); // D in the overlay: the wait begins now it is gone
    if (const auto region = std::exchange(m_recordRegion, std::nullopt))
        recorder().prepare(region->rect, region->monitor, region->dpr);
    if (const std::optional<LongRegion> region = std::exchange(m_longRegion, std::nullopt)) {
        if (m_callbacks.longStarting)
            m_callbacks.longStarting();
        longCapture().start(region->rect, region->monitor, region->dpr);
    }
}

Recorder& Capture::recorder()
{
    if (!m_recorder)
        m_recorder = std::make_unique<Recorder>(m_engine, m_callbacks.recording, this);
    return *m_recorder;
}

LongCapture& Capture::longCapture()
{
    if (!m_long) {
        LongCapture::Hooks hooks;
        hooks.done = [this](const QImage& picture, LongCapture::Output output, QPoint origin) {
            switch (output) {
            case LongCapture::Output::Edit:
                if (m_callbacks.longCapture)
                    m_callbacks.longCapture(picture);
                break;
            case LongCapture::Output::Copy:
                if (m_callbacks.copy)
                    m_callbacks.copy(picture, {}, {}, {}); // a long picture is not the previous regular capture
                break;
            case LongCapture::Output::Save:
                save(picture);
                if (m_callbacks.record)
                    m_callbacks.record(picture, {}, {});
                break;
            case LongCapture::Output::Pin:
                // Taller than the screen as a rule: shown fitted, its top
                // left where the scrolled region's was.
                if (PinManager* pins = m_callbacks.pins ? m_callbacks.pins() : nullptr)
                    pins->pinFitted(picture, origin, origin, true);
                if (m_callbacks.record)
                    m_callbacks.record(picture, {}, {});
                break;
            }
        };
        hooks.copyText = [this](const QString& text) {
            if (m_callbacks.copyText)
                m_callbacks.copyText(text);
        };
        hooks.ended = [this] {
            if (m_callbacks.ended)
                m_callbacks.ended();
        };
        m_long = std::make_unique<LongCapture>(m_engine, m_grabber, std::move(hooks));
    }
    return *m_long;
}

void Capture::drawCursorInShots()
{
    CURSORINFO info {sizeof info};
    if (!::GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING) || !info.hCursor)
        return;
    for (Grabber::Shot& shot : m_shots) {
        if (shot.rect.contains(info.ptScreenPos.x, info.ptScreenPos.y)) {
            const QImage before = shot.hdrPixels ? shot.image.copy() : QImage(); // the HDR pixels get it too
            const bool drawn = drawCursor(shot.image, QPoint(info.ptScreenPos.x, info.ptScreenPos.y) - shot.rect.topLeft(), info.hCursor);
            if (drawn && shot.hdrPixels)
                paintSdrOver(*shot.hdrPixels, before, shot.image);
            qInfo().noquote() << u"Screen capture: mouse cursor %1 (handle %2) at %3,%4"_s.arg(drawn ? u"drawn"_s : u"not drawn"_s)
                                     .arg(reinterpret_cast<quintptr>(info.hCursor))
                                     .arg(info.ptScreenPos.x)
                                     .arg(info.ptScreenPos.y);
        }
    }
}

void Capture::askUia(std::size_t window, QPoint point)
{
    const HWND hwnd = m_snapshot.handle(window);
    if (!hwnd || !m_uiaGate.allowed(reinterpret_cast<quintptr>(hwnd)))
        return; // its answers were slow or empty: the child windows are all it has
    // Not a Chromium window, nor one with Chromium inside (WebView2, CEF, Qt WebEngine): asking it
    // makes it turn its accessibility mode on (VS Code: "optimized for a screen reader"). Its child
    // windows stand. Logged once per window, so a test can tell the skip from a slow answer.
    if (m_snapshot.hostsChromium(window)) {
        if (m_uiaChromiumSkipped.insert(reinterpret_cast<quintptr>(hwnd)).second)
            qInfo().noquote() << u"Screen capture: UI Automation skipped for a Chromium window (class %1): its child windows only"_s.arg(m_snapshot.windowClass(window));
        return;
    }
    if (!wantsUia(m_snapshot.windowClass(window), m_snapshot.childWindows(window)))
        return;
    if (!m_uia) {
        m_uia = std::make_unique<UiaProbe>(this, [this](const UiaProbe::Answer& answer) { uiaAnswered(answer); });
    }
    m_uia->ask(hwnd, point);
}

void Capture::uiaAnswered(const UiaProbe::Answer& answer)
{
    if (m_state != State::Showing)
        return;
    const int window = m_snapshot.indexOf(reinterpret_cast<HWND>(answer.window));
    if (window < 0)
        return;
    const std::vector<QRect> controls = uiaControls(answer.chain, m_snapshot.windows()[static_cast<std::size_t>(window)]);
    m_uiaGate.record(answer.window, answer.elapsedMs, static_cast<int>(controls.size()));
    if (m_uiaLogged++ < 40 || answer.elapsedMs > UiaGate::kBudgetMs) {
        QStringList where; // the controls' rectangles, outermost first (virtual desktop), for a test to read
        for (const QRect& r : controls)
            where.append(u"%1,%2 %3x%4"_s.arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()));
        qInfo().noquote() << u"Screen capture: UI Automation gave %1 controls in %2 ms%3%4"_s.arg(controls.size())
                                 .arg(answer.elapsedMs)
                                 .arg(answer.elapsedMs > UiaGate::kBudgetMs ? u" (over the budget: this window's child windows only)"_s : QString())
                                 .arg(where.isEmpty() ? QString() : u": "_s + where.join(u" | "_s));
    }
    if (!answer.ok || answer.elapsedMs > UiaGate::kBudgetMs)
        return; // too slow to wait for: the Win32 child windows stand
    if (m_snapshot.addControls(static_cast<std::size_t>(window), controls))
        m_session.refreshHover();
}

void Capture::startDelayed(int seconds)
{
    if (m_state != State::Idle || m_countdown || m_overlays.empty() || (m_long && m_long->running()))
        return;
    if (seconds > 0)
        m_delay = normalizedDelay(seconds);
    POINT cursor {};
    ::GetCursorPos(&cursor);
    MONITORINFO info {sizeof info};
    if (!::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info))
        return;
    const QRect work(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top);
    m_countdown = std::make_unique<CountdownWindow>(work, m_callbacks.accent ? m_callbacks.accent() : QColor(),
        m_callbacks.panelRadius ? m_callbacks.panelRadius() : 8.0, [this] { QMetaObject::invokeMethod(this, [this] { cancelCountdown(); }, Qt::QueuedConnection); });
    if (!m_countdown->hwnd()) {
        m_countdown.reset();
        return;
    }
    m_countTotal = m_delay;
    m_countClock.start();
    m_countdown->setSeconds(countdownSeconds(0, m_countTotal));
    m_countTimer.start(100);
    qInfo() << "Screen capture: delayed capture," << m_countTotal << "s";
}

void Capture::tickCountdown()
{
    if (!m_countdown)
        return;
    const qint64 left = countdownRemaining(m_countClock.elapsed(), m_countTotal);
    if (left <= 0) {
        fireCountdown();
        return;
    }
    m_countdown->setSeconds(countdownSeconds(m_countClock.elapsed(), m_countTotal));
    m_countTimer.start(static_cast<int>(std::min<qint64>(left, 100)));
}

void Capture::fireCountdown()
{
    m_countTimer.stop();
    m_countdown.reset(); // out of the picture: gone from the screen once DWM has composed twice
    ::DwmFlush();
    ::DwmFlush();
    qInfo() << "Screen capture: the countdown is over after" << m_countClock.elapsed() << "ms";
    m_quietStart = true;
    start();
}

void Capture::cancelCountdown()
{
    if (!m_countdown)
        return;
    m_countTimer.stop();
    m_countdown.reset();
    qInfo() << "Screen capture: delayed capture cancelled";
}

void Capture::save(const QImage& image, const QString& path, std::shared_ptr<const HdrPixels> hdr)
{
    saveImage(m_saver, this, image, path, m_callbacks.saved, m_callbacks.saveFolder ? m_callbacks.saveFolder() : QString(),
        std::move(hdr));
}

QString chooseScreenshotSavePath(QQuickWindow* owner)
{
    return shell::pickSaveFile(owner ? reinterpret_cast<HWND>(owner->winId()) : nullptr,
        Capture::tr("保存截图"), screenshotFileName(QDateTime::currentDateTime(), {}), Capture::tr("PNG 图片"), u"png"_s);
}

void saveImage(std::jthread& thread, QObject* context, const QImage& image, const QString& path,
    std::function<void(const QString& path, const QString& error)> done, const QString& preferredFolder,
    std::shared_ptr<const HdrPixels> hdr)
{
    if (thread.joinable())
        thread.join(); // the last one, done by now
    const QString folder = path.isEmpty() ? (preferredFolder.isEmpty() ? screenshotsFolder() : preferredFolder) : QString();
    // The HDR file's name: the picture's, .jxr.
    const auto hdrPath = [](const QString& picture) {
        const QFileInfo info(picture);
        return QDir::toNativeSeparators(info.dir().filePath(info.completeBaseName() + u".jxr"_s));
    };
    // Encoding a whole screen takes a moment: not on the GUI thread.
    thread = std::jthread([context, done = std::move(done), image, folder, chosen = path, hdr = std::move(hdr), hdrPath] {
        QString path = chosen;
        QString error;
        try {
            if (!path.isEmpty()) { // 另存为… or direct save-back (#427)
                error = writeImageFile(image, path);
            } else if (folder.isEmpty()) {
                error = Capture::tr("找不到“屏幕截图”文件夹");
            } else {
                const QDir dir(folder);
                // A name neither file has yet.
                path = QDir::toNativeSeparators(dir.filePath(screenshotFileName(QDateTime::currentDateTime(), [&](const QString& name) {
                    return QFileInfo::exists(dir.filePath(name)) || (hdr && QFileInfo::exists(hdrPath(dir.filePath(name))));
                })));
                error = writeImageFile(image, path);
            }
            if (hdr && error.isEmpty() && !writeJxr(hdrPath(path), *hdr))
                error = Capture::tr("截图已保存，但 HDR 图片没能保存到 %1").arg(hdrPath(path));
        } catch (...) {
            error = Capture::tr("无法保存到 %1").arg(path);
        }
        QMetaObject::invokeMethod(context, [done, path, error] {
            if (done)
                done(path, error);
        }, Qt::QueuedConnection);
    });
}

} // namespace ws::capture

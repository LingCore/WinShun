#include "Capture.h"

#include "CaptureOcr.h"
#include "DiagnosticLog.h"
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
    case CaptureSession::Action::OcrWeb: return u"ocr"_s;
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
    connect(&m_session, &CaptureSession::keyHintsKept, this, [this](bool on) {
        if (m_callbacks.keyHintsKept)
            m_callbacks.keyHintsKept(on);
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

void Capture::triggerOcr()
{
    if ((m_recorder && m_recorder->active()) || m_countdown || (m_long && m_long->running()))
        return;
    if (m_state == State::Idle)
        start(true);
    else if (m_ocrOnly && (m_state == State::Waiting || m_state == State::Showing))
        m_session.act(static_cast<int>(CaptureSession::Action::Cancel)); // what the key opened, it closes
}

void Capture::start(bool ocrOnly)
{
    const bool quiet = std::exchange(m_quietStart, false);
    if (m_overlays.empty())
        return;
    if (m_callbacks.starting)
        m_callbacks.starting();
    m_ocrOnly = ocrOnly;
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
    m_session.setOcrOnly(ocrOnly);
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
    show();
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
        overlay.window->show();
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
    if (under) {
        win::bringToFront(under); // the keys: Esc, Enter, the arrows
        under->requestActivate();
    }
    m_showMs = m_clock.elapsed();
    if (underIndex >= 0)
        m_session.pointerMove(underIndex, underAt); // the hover and the magnifier before the mouse moves
    if (m_ocrOnly) // where the pointer is, at the top: what this capture is for
        m_session.notify(tr("识别文字：拖选区域，或单击窗口 · Esc 取消"));
    startWatch();
    // Nothing more here: the first frame is drawn when this returns to the
    // event loop, and what is done before that is time with no overlay.
}

void Capture::afterUp()
{
    m_pointerFacts = diag::pointerFacts(); // the registry, the system's cursors
    // The capture before, read meanwhile: , shows it at once (not for reading words).
    if (m_history.enabled() && !m_ocrOnly) {
        m_historyIds = m_history.ids(); // a folder listed
        prefetchHistory(0);
    }
    m_session.setHistoryAvailable(m_history.enabled() && !m_ocrOnly && !m_historyIds.isEmpty()); // the key hints' , .
    if (m_callbacks.shown)
        m_callbacks.shown();
}

void Capture::startWatch()
{
    m_stalls.reset();
    m_ticks = 0;
    m_upClock.invalidate();
    m_cursorShown = {};
    m_cursorDragging = {};
    // As early as it can be: a cold stall has come 12 ms after the overlay was up.
    m_shownClock.start();
    m_tickClock.start();
    m_watchTimer.start();
}

bool Capture::pointerOverOverlay(POINT at) const
{
    const HWND hit = ::WindowFromPoint(at);
    const HWND root = hit ? ::GetAncestor(hit, GA_ROOT) : nullptr;
    return root && std::ranges::any_of(m_overlays, [root](const Overlay& o) {
        return o.window && reinterpret_cast<HWND>(o.window->winId()) == root;
    });
}

void Capture::watchTick()
{
    const qint64 interval = m_tickClock.restart();
    ++m_ticks;
    const qint64 sinceShow = m_shownClock.elapsed();
    const qint64 sinceUp = m_upClock.isValid() ? m_upClock.elapsed() : -1;
    const QString phase = m_session.phaseName();
    const QRect selected = m_session.hasSelection() ? m_session.selectionRect() : QRect();
    if (const auto stall = m_stalls.note(interval, sinceShow, phase); stall && stall->record) {
        diagnostics::record(u"capture-stall"_s, {{u"stall_ms"_s, stall->ms}, {u"ms_since_overlay_up"_s, sinceUp},
            {u"ms_since_show"_s, sinceShow}, {u"ms_since_release"_s, m_session.msSinceRelease()}, {u"phase"_s, phase},
            {u"selection"_s, sizeText(selected.size())}, {u"prescan_running"_s, m_scanRunning},
            {u"first_since_start"_s, g_firstCapture}});
    }
    CURSORINFO info {sizeof info};
    if (!::GetCursorInfo(&info) || !pointerOverOverlay(info.ptScreenPos))
        return;
    if (diag::cursorHidden(info.flags) && m_stalls.noteHidden()) {
        diagnostics::record(u"capture-cursor-hidden"_s, {{u"flags"_s, static_cast<qint64>(info.flags)},
            {u"ms_since_overlay_up"_s, sinceUp}, {u"ms_since_show"_s, sinceShow}, {u"phase"_s, phase}});
    }
    // Not at the first tick: the overlay has had a chance to set its cursor.
    if (m_ticks >= 2 && m_cursorShown.isEmpty())
        m_cursorShown = diag::cursorFacts(info.hCursor, info.flags);
    if (phase == u"dragging"_s && m_cursorDragging.isEmpty())
        m_cursorDragging = diag::cursorFacts(info.hCursor, info.flags);
}

void Capture::recordCapture(CaptureSession::Action action, const QRect& rect)
{
    m_watchTimer.stop();
    QJsonArray dprs;
    for (const Overlay& overlay : m_overlays)
        dprs.append(overlay.model->dpr());
    const QRect selected = !rect.isEmpty() ? rect : m_session.hasSelection() ? m_session.selectionRect() : QRect();
    QJsonObject details {{u"overlay_up_ms"_s, m_reported && m_upClock.isValid() ? m_clock.elapsed() - m_upClock.elapsed() : -1},
        {u"grab_ms"_s, m_grabMs}, {u"begun_ms"_s, m_beginMs}, {u"shown_ms"_s, m_showMs},
        {u"page_faults"_s, m_reported ? m_upFaults : -1}, {u"monitors"_s, static_cast<int>(m_overlays.size())}, {u"dpr"_s, dprs},
        {u"wgc"_s, m_wgcShots}, {u"hdr"_s, m_hdrShots}, {u"magnifier"_s, m_magnifierOn},
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
        m_reported = true;
        m_upClock.start();
        m_upFaults = static_cast<qint64>(pageFaults() - m_faultsBefore);
        qInfo().noquote() << u"Screen capture: overlay up %1 ms after the hotkey (grab %2 ms: %3; %4 windows listed in %5 ms; session begun at %7 ms, windows shown at %8 ms; %9 page faults%6)"_s
                                 .arg(m_clock.elapsed())
                                 .arg(m_grabMs)
                                 .arg(m_grabReport)
                                 .arg(m_snapshot.windows().size())
                                 .arg(m_listMs - m_grabMs)
                                 .arg(m_flyoutMs >= 0 ? u"; waited %1 ms for the Start menu to close"_s.arg(m_flyoutMs) : QString())
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
    const bool ocrOnly = std::exchange(m_ocrOnly, false); // triggerOcr's: only OcrCopy or Cancel come here
    QString savePath;
    if (action == CaptureSession::Action::Save) {
        const auto selected = std::ranges::find(m_overlays, screen, [](const Overlay& o) { return o.model->index(); });
        if (selected == m_overlays.end() || !selected->window)
            return;
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
        m_session.rememberSelection(rect.translated(selected->model->origin()));
    }
    m_flyoutWait.stop();
    m_flyout = nullptr;
    QImage picture;
    QByteArray project;
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
        // it. Not annotated: the plain copy above, as fast as before.
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
        }
    } else {
        // Ended on a capture brought back from the history: that one stays in
        // the history, and the draft is what was drawn on the live screens.
        if (m_historyAt >= 0)
            restoreLive();
        if (m_session.hasAnnotations()) {
            // Ended with annotations on the screen (the hotkey, Esc, D): kept for a
            // while, to be found again from the tray menu.
            const int draftScreen = m_session.annotationScreen();
            for (const Overlay& overlay : m_overlays) {
                if (overlay.model->index() == draftScreen) {
                    annotation->commitPending();
                    m_draft = Draft {annotation->project(), overlay.model->rect().topLeft() + annotation->selectionRect().topLeft()};
                    m_draftTimer.start();
                    qInfo().noquote() << u"Screen capture: annotations kept as a draft at %1,%2 (%3x%4)"_s.arg(m_draft->origin.x())
                                             .arg(m_draft->origin.y())
                                             .arg(m_draft->project.base.width())
                                             .arg(m_draft->project.base.height());
                }
            }
        }
    }
    // Taken: into the history, as the overlays show it (a capture brought
    // back and taken again replaces itself there).
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
        if (m_session.hasAnnotations() && m_session.annotationScreen() == screen)
            shot.document = QJsonDocument(annot::toJson(annotation->committedDocument())).toJson(QJsonDocument::Compact);
        if (shot.screen >= 0)
            m_history.add(std::move(shot), m_historyAt >= 0 ? m_historyShown : QString());
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
        if (m_callbacks.readText)
            m_callbacks.readText(picture, dpr, QRect(origin, picture.size()));
        return;
    }
    m_last = picture;
    m_lastOrigin = origin;
    m_lastProject = project;
    m_lastClipboardSequence = ::GetClipboardSequenceNumber();
    m_lastClipboardId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    switch (action) {
    case CaptureSession::Action::CopyPin:
        if (PinManager* pins = m_callbacks.pins ? m_callbacks.pins() : nullptr)
            pins->pin(picture, origin, true, project);
        [[fallthrough]]; // copying records the picture once, including its editable project
    case CaptureSession::Action::Copy:
        if (m_callbacks.copy)
            m_callbacks.copy(picture, project, m_lastClipboardId);
        break;
    case CaptureSession::Action::Save:
    case CaptureSession::Action::SavePrevious:
        save(picture, savePath, std::move(hdr));
        if (m_callbacks.record)
            m_callbacks.record(picture, project);
        break;
    case CaptureSession::Action::Pin:
        if (PinManager* pins = m_callbacks.pins ? m_callbacks.pins() : nullptr)
            pins->pin(picture, origin, true, project);
        if (m_callbacks.record)
            m_callbacks.record(picture, project);
        break;
    case CaptureSession::Action::Ocr:
        if (m_callbacks.ocr)
            m_callbacks.ocr(picture, dpr, QRect(origin, picture.size()));
        break;
    case CaptureSession::Action::OcrCopy:
    case CaptureSession::Action::OcrSearch:
    case CaptureSession::Action::OcrWeb:
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
    int selectionScreen = -1;
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
    if (PinManager* pins = m_callbacks.pins ? m_callbacks.pins() : nullptr)
        pins->pin(picture, draft.origin, true, project);
    m_last = picture;
    m_lastOrigin = draft.origin;
    m_lastProject = project;
    m_lastClipboardSequence = ::GetClipboardSequenceNumber();
    m_lastClipboardId = QUuid::createUuid().toString(QUuid::WithoutBraces);
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
                    m_callbacks.copy(picture, {}, {}); // a long picture is not the previous regular capture
                break;
            case LongCapture::Output::Save:
                save(picture);
                if (m_callbacks.record)
                    m_callbacks.record(picture, {});
                break;
            case LongCapture::Output::Pin:
                // Taller than the screen as a rule: shown fitted, its top
                // left where the scrolled region's was.
                if (PinManager* pins = m_callbacks.pins ? m_callbacks.pins() : nullptr)
                    pins->pinFitted(picture, origin, origin, true);
                if (m_callbacks.record)
                    m_callbacks.record(picture, {});
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
    // PNG encoding of a whole screen takes a moment: not on the GUI thread.
    thread = std::jthread([context, done = std::move(done), image, folder, chosen = path, hdr = std::move(hdr), hdrPath] {
        QString path = chosen;
        QString error;
        if (!path.isEmpty()) { // 另存为…
            if (!image.save(path, "PNG"))
                error = Capture::tr("无法保存到 %1").arg(path);
        } else if (folder.isEmpty()) {
            error = Capture::tr("找不到“屏幕截图”文件夹");
        } else {
            const QDir dir(folder);
            // A name neither file has yet.
            path = QDir::toNativeSeparators(dir.filePath(screenshotFileName(QDateTime::currentDateTime(), [&](const QString& name) {
                return QFileInfo::exists(dir.filePath(name)) || (hdr && QFileInfo::exists(hdrPath(dir.filePath(name))));
            })));
            if (!image.save(path, "PNG"))
                error = Capture::tr("无法保存到 %1").arg(path);
        }
        if (hdr && error.isEmpty() && !writeJxr(hdrPath(path), *hdr))
            error = Capture::tr("截图已保存，但 HDR 图片没能保存到 %1").arg(hdrPath(path));
        QMetaObject::invokeMethod(context, [done, path, error] {
            if (done)
                done(path, error);
        }, Qt::QueuedConnection);
    });
}

} // namespace ws::capture

#include "LongCapture.h"
#include "IdleGraphics.h"

#include "CaptureExclusion.h"
#include "CaptureSession.h" // kLongMinimum
#include "DiagnosticLog.h"
#include "Geometry.h" // placeBeside
#include "Grabber.h"
#include "ScrollAutoEngine.h"
#include "ScrollDiagnostics.h"
#include "ScrollGrabber.h"
#include "ScrollInput.h"
#include "ScrollStitcher.h"
#include "platform/Integrity.h"
#include "platform/WindowEffects.h"

#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonObject>
#include <QMetaEnum>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QSysInfo>

#include <dwmapi.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <optional>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws::capture {

namespace {

constexpr int kManualWaitMs = 40; // responsive manual polling / stop checks
constexpr int kFinishDrainMs = 220; // drain any final compositor frame on Finish
constexpr int kFinishQuietMs = 60;
constexpr int kLostShownMs = 450; // by hand: "lost" only when it lasts (a fling's frames come apart)
constexpr int kWatchdogMs = 5000;
constexpr int kPreviewEveryMs = 60; // ~16 fps live preview while actively growing
constexpr double kOverlayLogical = 16; // the overlay scroll bar's strip on the right, left out of the comparing
constexpr double kUserCursorMoveLogical = 24; // cursor travel indicating the user took back the mouse
constexpr int kHotkeyEsc = 0x5701;
constexpr int kHotkeyEnter = 0x5702;
constexpr int kChunk = 3000; // characters of the diagnostics a log entry (entries are cut at 8 KB)
constexpr auto kCopiedShown = 1500ms;
const wchar_t* const kBorderClass = L"WinShunLongCaptureEdge";

QRect toRect(const RECT& r)
{
    return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

QScreen* screenOf(HMONITOR monitor)
{
    for (QScreen* s : QGuiApplication::screens()) {
        const auto* native = s->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor)
            return s;
    }
    return nullptr;
}

ScrollSpeed toScrollSpeed(LongCapture::Speed s)
{
    switch (s) {
    case LongCapture::Speed::Slow:
        return ScrollSpeed::Slow;
    case LongCapture::Speed::Fast:
        return ScrollSpeed::Fast;
    case LongCapture::Speed::Normal:
        break;
    }
    return ScrollSpeed::Normal;
}

LongCapture::Hint hintFromLadder(ScrollLadder::Hint h, bool fallbackAuto)
{
    switch (h) {
    case ScrollLadder::Hint::Elevated:
        return LongCapture::Hint::Elevated;
    case ScrollLadder::Hint::NothingWorks:
        return LongCapture::Hint::NothingWorks;
    case ScrollLadder::Hint::MouseOutside:
        return LongCapture::Hint::MouseOutside;
    case ScrollLadder::Hint::None:
        break;
    }
    return fallbackAuto ? LongCapture::Hint::Auto : LongCapture::Hint::Manual;
}

} // namespace

// ---- the border: four layered strips just outside the region ----------------

class LongCapture::Border {
public:
    explicit Border(LongCapture* owner)
    {
        static const bool registered = [] {
            WNDCLASSEXW wc {sizeof wc};
            wc.lpfnWndProc = &Border::proc;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = kBorderClass;
            return ::RegisterClassExW(&wc) != 0;
        }();
        if (!registered)
            return;
        for (HWND& strip : m_strips) {
            strip = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                kBorderClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
            if (strip)
                ::SetWindowLongPtrW(strip, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
        }
    }

    ~Border()
    {
        for (HWND strip : m_strips)
            if (strip)
                ::DestroyWindow(strip);
    }

    Border(const Border&) = delete;
    Border& operator=(const Border&) = delete;

    // The window the temporary hotkeys go to.
    HWND keys() const { return m_strips[0]; }

    // Round `region` (physical): 1 logical px of `edge`, then 1 px of half-white outside it.
    void show(const QRect& region, const QRect& screen, double dpr, QColor edge)
    {
        const int g = std::max(1, qRound(dpr));
        const int t = g + 1;
        const auto strips = longCaptureBorder(region, screen, t);
        const uint32_t grey = 0xFF000000u | (uint32_t(edge.red()) << 16) | (uint32_t(edge.green()) << 8) | uint32_t(edge.blue());
        const uint32_t white = 0x99999999u; // premultiplied white at 0.6
        const int w = strips[0].width();
        paint(m_strips[0], strips[0], [&](int x, int y) { return y == 0 || x == 0 || x == w - 1 ? white : grey; });
        paint(m_strips[1], strips[1],
            [&](int x, int y) { return y == t - 1 || x == 0 || x == w - 1 ? white : grey; });
        paint(m_strips[2], strips[2], [&](int x, int) { return x == 0 ? white : grey; });
        paint(m_strips[3], strips[3],
            [&](int x, int) { return x == t - 1 ? white : grey; });
    }

    void hide()
    {
        for (HWND strip : m_strips)
            if (strip)
                ::ShowWindow(strip, SW_HIDE);
    }

    // Outside the region, except where it meets the screen's edge: there
    // longCaptureBorder keeps a strip on the screen, inside the selection,
    // and it would be stitched in (a maximized window: a line down the
    // picture). Such a strip is left out of the frames; it has no shadow.
    void exclude(const QRect& region, bool running)
    {
        for (HWND strip : m_strips)
            if (strip && (!running || ::IsWindowVisible(strip)))
                excludeFromCapture(strip, region, running, 0);
    }

private:
    template<typename Pixel>
    static void paint(HWND strip, const QRect& r, Pixel pixel)
    {
        if (!strip || r.isEmpty())
            return;
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = r.width();
        info.bmiHeader.biHeight = -r.height(); // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        const HDC screen = ::GetDC(nullptr);
        const HDC memory = ::CreateCompatibleDC(screen);
        void* bits = nullptr;
        const HBITMAP section = ::CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (section && bits) {
            auto* p = static_cast<uint32_t*>(bits);
            for (int y = 0; y < r.height(); ++y)
                for (int x = 0; x < r.width(); ++x)
                    p[size_t(y) * size_t(r.width()) + size_t(x)] = pixel(x, y);
            const HGDIOBJ old = ::SelectObject(memory, section);
            POINT at {r.x(), r.y()};
            SIZE extent {r.width(), r.height()};
            POINT from {0, 0};
            BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            ::UpdateLayeredWindow(strip, screen, &at, &extent, memory, &from, 0, &blend, ULW_ALPHA);
            ::SetWindowPos(strip, HWND_TOPMOST, r.x(), r.y(), r.width(), r.height(), SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ::SelectObject(memory, old);
        }
        if (section)
            ::DeleteObject(section);
        ::DeleteDC(memory);
        ::ReleaseDC(nullptr, screen);
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_HOTKEY) {
            auto* owner = reinterpret_cast<LongCapture*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (owner && wParam == kHotkeyEsc)
                owner->cancel();
            else if (owner && wParam == kHotkeyEnter)
                owner->finish();
            return 0;
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    std::array<HWND, 4> m_strips {};
};

// ---- what the engine thread and the GUI share ----------------------------------

struct LongCapture::Run {
    QRect region; // physical px, virtual desktop
    double dpr = 1;
    QString dump; // WINSHUN_SCROLL_DUMP: every frame stitched and what came of it, there
    std::atomic<int> stop {0}; // 1 finish, 2 cancel
    std::atomic<bool> autoScroll {false}; // the panel's switch; the engine turns it off on manual fallback
    std::atomic<int> speed {int(Speed::Normal)};
    std::atomic<bool> horizontal {false}; // the panel's 横向
    std::atomic<bool> turned {false}; // the panel turned it: the engine starts again
    std::mutex mutex; // guards diag, ownRects, ownRectsSeq
    ScrollDiagnostics diag;
    std::vector<QRect> ownRects; // visible screen rects of panel & preview
    quint64 ownRectsSeq = 0;
    Output output = Output::Edit; // GUI thread only, used once the worker has finished
};

struct LongCapture::Update {
    Status status = Status::Waiting;
    int height = 0;
    bool partial = false;
    bool canAuto = true;
    bool horizontal = false;
    Hint hint = Hint::None;
    QImage preview; // null: as it was
};

LongCapture::LongCapture(QQmlEngine* engine, Grabber& grabber, Hooks hooks, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_grabber(grabber)
    , m_hooks(std::move(hooks))
{
    m_copiedTimer.setSingleShot(true);
    m_copiedTimer.setInterval(kCopiedShown);
    connect(&m_copiedTimer, &QTimer::timeout, this, [this] {
        if (m_hint == Hint::Copied) {
            m_hint = m_hintBefore;
            emit changed();
        }
    });
}

LongCapture::~LongCapture()
{
    if (m_run) {
        m_run->stop = 2;
        if (m_thread.joinable())
            m_thread.join();
        wrapUp();
        m_run.reset();
    }
    delete m_panel.data();
}

QSize LongCapture::previewBox() const
{
    return m_horizontal ? QSize(kPreviewWideWidth, kPreviewWideHeight)
                        : QSize(kPreviewWidth, kPreviewHeight);
}

void LongCapture::setAutoScroll(bool on)
{
    m_autoScrollChosen = on;
    if (on == m_autoScroll)
        return;
    m_autoScroll = on;
    if (m_run)
        m_run->autoScroll = on;
    emit changed();
}

void LongCapture::setAutoScrollSpeed(Speed speed)
{
    if (speed == m_speed)
        return;
    m_speed = speed;
    if (m_run)
        m_run->speed = int(speed);
    emit changed();
}

void LongCapture::cycleAutoScrollSpeed()
{
    switch (m_speed) {
    case Speed::Normal:
        setAutoScrollSpeed(Speed::Fast);
        break;
    case Speed::Fast:
        setAutoScrollSpeed(Speed::Slow);
        break;
    case Speed::Slow:
        setAutoScrollSpeed(Speed::Normal);
        break;
    }
}

void LongCapture::setHorizontal(bool on)
{
    m_horizontalChosen = on;
    if (on == m_horizontal)
        return;
    qInfo().noquote() << u"Long screenshot: %1 chosen"_s.arg(on ? u"sideways"_s : u"up and down"_s);
    m_horizontal = on;
    if (m_run) {
        m_run->horizontal = on;
        m_run->turned = true;
    }
    emit changed();
}

QString LongCapture::hint() const
{
    switch (m_hint) {
    case Hint::Auto:
        return tr("正在自动滚动，移动鼠标可暂停。Enter 完成，Esc 取消");
    case Hint::Manual:
        return m_horizontal ? tr("在区域内向左或向右滚动（Shift+滚轮），滚完按 Enter 或点“完成”")
                            : tr("在区域内向上或向下滚动，滚完按 Enter 或点“完成”");
    case Hint::MouseOutside:
        return tr("已暂停：把鼠标移回要滚动的区域继续，或点“完成”");
    case Hint::Elevated:
        // Not Enter: with an administrator's window in front, Windows gives
        // our hotkeys only with Ctrl, Alt, Shift or Win (tested 2026-10-22),
        // and the Enter goes to that program.
        return tr("这个程序以管理员身份运行，请手动滚动，滚完点“完成”");
    case Hint::NothingWorks:
        return tr("自动滚动不起作用，请自己滚动");
    case Hint::Reconnect:
        return tr("没接上：慢慢往回滚一点，接上后再继续");
    case Hint::NoFrames:
        return tr("5 秒没有新画面");
    case Hint::AtLimit:
        return tr("到上限了（3 万像素）");
    case Hint::Copied:
        return tr("诊断信息已复制");
    case Hint::None:
        break;
    }
    return {};
}

QQuickWindow* LongCapture::panel()
{
    if (m_panel)
        return m_panel;
    QQmlComponent component(m_engine, u"WinShun"_s, u"LongCapturePanel"_s);
    QObject* object = component.createWithInitialProperties({{u"capture"_s, QVariant::fromValue(this)}});
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        qWarning().noquote() << "Long screenshot: no panel window:" << component.errorString();
        delete object;
        return nullptr;
    }
    releaseIdleGraphics(window);
    window->create();
    win::styleFramelessWindow(window, false);
    win::disableTransitions(window); // no fade-out for the engine's last frames to catch
    // Left out of the frames only while it is in them (updateExclusion).
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    m_panel = window;
    m_previewWindow = window->findChild<QQuickWindow*>(u"longCapturePreview"_s);
    for (QQuickWindow* shown : {m_panel.data(), m_previewWindow.data()}) {
        if (!shown)
            continue;
        for (auto changed : {&QWindow::xChanged, &QWindow::yChanged, &QWindow::widthChanged, &QWindow::heightChanged})
            connect(shown, changed, this, &LongCapture::updateExclusion);
        connect(shown, &QWindow::visibleChanged, this, &LongCapture::updateExclusion);
    }
    if (m_previewWindow) {
        releaseIdleGraphics(m_previewWindow);
        m_previewWindow->create();
        win::styleFramelessWindow(m_previewWindow, false);
        win::disableTransitions(m_previewWindow);
        const HWND preview = reinterpret_cast<HWND>(m_previewWindow->winId());
        ::SetWindowLongPtrW(preview, GWL_EXSTYLE, ::GetWindowLongPtrW(preview, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    }
    return window;
}

// The panel, the preview and the border are left out of the frames while the
// capture runs and they lie over its region; otherwise they show in a
// screenshot. Running until ended(): after 完成 the engine still takes frames
// briefly, so a window hidden meanwhile keeps its setting until then.
// After that the panel and the preview keep theirs, hidden: cleared on both
// once both were hidden (the panel owns the preview), DWM left the preview's
// edge and shadow on the screen, an empty frame over whatever lay below
// (2026-11-19). A hidden window is in no picture; each start, move and
// showing in a run sets it anew.
void LongCapture::updateExclusion()
{
    const bool running = m_run != nullptr;
    const QRect region = running ? m_run->region : QRect();
    std::vector<QRect> rects;
    if (running) {
        for (QQuickWindow* window : {m_panel.data(), m_previewWindow.data()}) {
            if (!window)
                continue;
            const HWND hwnd = reinterpret_cast<HWND>(window->winId());
            excludeFromCapture(hwnd, region, true);
            RECT r {};
            if (window->isVisible() && ::GetWindowRect(hwnd, &r))
                rects.push_back(toRect(r));
        }
        const std::lock_guard lock(m_run->mutex);
        m_run->ownRects = std::move(rects);
        ++m_run->ownRectsSeq;
    }
    if (m_border)
        m_border->exclude(region, running);
}

// Keep the controls by the original selection, as the capture toolbar was.
// The optional preview takes separate room beside it and never displaces
// the actions. Both stay on the selected monitor's work area.
void LongCapture::placePanel(const QRect& region, HMONITOR monitor)
{
    MONITORINFO info {sizeof info};
    QScreen* screen = screenOf(monitor);
    if (!m_panel || !screen || !::GetMonitorInfoW(monitor, &info))
        return;
    m_panel->setScreen(screen);
    const qreal ratio = screen->devicePixelRatio();
    const QRect work = toRect(info.rcWork);
    const QSize box(qRound(m_panel->width() * ratio), qRound(m_panel->height() * ratio));
    const int gap = qRound(8 * ratio);
    const int border = std::max(1, qRound(ratio)) + 1;
    const QRect bounds = work.adjusted(gap, gap, -gap, -gap);
    const QRect anchor = region.adjusted(-border, -border, border, border);
    const QPoint at = placeToolbar(anchor, box, bounds, gap).topLeft;
    const auto place = [&](QQuickWindow* window, QPoint position) {
        window->setScreen(screen);
        window->setPosition(screen->geometry().topLeft() + (position - toRect(info.rcMonitor).topLeft()) / ratio);
    };
    place(m_panel, at);
    if (m_previewWindow) {
        const QSize previewBox(qRound(m_previewWindow->width() * ratio), qRound(m_previewWindow->height() * ratio));
        const QPoint previewAt = placeBeside(anchor, previewBox, bounds, gap);
        const QRect previewRect(previewAt, previewBox);
        // When the selection fills the screen, show the actions first. The
        // preview can still be opened explicitly instead of covering content.
        m_panel->setProperty("previewVisible", !previewRect.intersects(anchor) && !previewRect.intersects(QRect(at, box)));
        place(m_previewWindow, previewAt);
    }
}

void LongCapture::hotkeys(bool on)
{
    const HWND keys = m_border ? m_border->keys() : nullptr;
    if (!keys)
        return;
    if (on && !m_hotkeys) {
        // Only while it runs. A key another program holds leaves the panel's buttons.
        const bool esc = ::RegisterHotKey(keys, kHotkeyEsc, MOD_NOREPEAT, VK_ESCAPE);
        const bool enter = ::RegisterHotKey(keys, kHotkeyEnter, MOD_NOREPEAT, VK_RETURN);
        if (!esc || !enter)
            qWarning().noquote() << u"Long screenshot: Esc %1, Enter %2"_s.arg(esc ? u"registered"_s : u"held by another program"_s,
                enter ? u"registered"_s : u"held by another program"_s);
        m_hotkeys = true;
    } else if (!on && m_hotkeys) {
        ::UnregisterHotKey(keys, kHotkeyEsc);
        ::UnregisterHotKey(keys, kHotkeyEnter);
        m_hotkeys = false;
    }
}

void LongCapture::start(const QRect& wanted, HMONITOR monitor, double dpr)
{
    if (m_run)
        return;
    MONITORINFO info {sizeof info};
    if (!::GetMonitorInfoW(monitor, &info))
        return;
    const QRect region = wanted.intersected(toRect(info.rcMonitor));
    if (region.width() < CaptureSession::kLongMinimum || region.height() < CaptureSession::kLongMinimum) {
        qInfo().noquote() << u"Long screenshot: the region %1x%2 is too small"_s.arg(region.width()).arg(region.height());
        return;
    }
    QQuickWindow* window = panel();
    if (!window)
        return;
    m_run = std::make_unique<Run>();
    m_run->region = region;
    m_run->dpr = dpr;
    if (const QString dump = qEnvironmentVariable("WINSHUN_SCROLL_DUMP"); !dump.isEmpty())
        m_run->dump = QDir(dump).filePath(QDateTime::currentDateTime().toString(u"yyyyMMdd-HHmmss"_s)); // a folder a run
    m_autoScroll = m_autoScrollChosen;
    m_run->autoScroll = m_autoScroll;
    m_run->speed = int(m_speed);
    m_horizontal = m_horizontalChosen;
    m_run->horizontal = m_horizontal;
    m_status = Status::Waiting;
    m_height = m_horizontal ? region.width() : region.height();
    m_hint = m_autoScroll ? Hint::Auto : Hint::Manual;
    m_partial = false;
    m_canAuto = true;
    m_preview = QImage();
    emit changed();
    emit previewChanged();

    if (!m_border)
        m_border = std::make_unique<Border>(this);
    m_border->show(region, toRect(info.rcMonitor), dpr, window->property("edgeColor").value<QColor>());
    placePanel(region, monitor);
    window->show();
    updateExclusion();
    // Explicitly raise the control strip without taking the target's focus.
    ::SetWindowPos(reinterpret_cast<HWND>(window->winId()), HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    hotkeys(true);
    qInfo().noquote() << u"Long screenshot: %1x%2 at %3,%4, auto-scroll %5"_s.arg(region.width())
                             .arg(region.height())
                             .arg(region.x())
                             .arg(region.y())
                             .arg(m_autoScroll ? u"on"_s : u"off"_s)
                         + (m_horizontal ? u", sideways"_s : QString());
    Run* run = m_run.get();
    m_thread = std::jthread([this, run] {
        try {
            engine(*run);
        } catch (const std::exception& e) {
            qWarning().noquote() << "Long screenshot: worker exception:" << e.what();
            QMetaObject::invokeMethod(this, [this] { ended(QImage(), u"failed"_s); }, Qt::QueuedConnection);
        } catch (...) {
            qWarning().noquote() << "Long screenshot: unknown worker exception";
            QMetaObject::invokeMethod(this, [this] { ended(QImage(), u"failed"_s); }, Qt::QueuedConnection);
        }
    });
}

// WINSHUN_SCROLL_DUMP: the panel as it was at the end (no screenshot shows it).
void LongCapture::dumpPanel()
{
    if (m_run && !m_run->dump.isEmpty() && m_panel && m_panel->isVisible() && QDir().mkpath(m_run->dump)) {
        m_panel->grabWindow().save(QDir(m_run->dump).filePath(u"panel.png"_s), "PNG");
        if (m_previewWindow && m_previewWindow->isVisible())
            m_previewWindow->grabWindow().save(QDir(m_run->dump).filePath(u"preview.png"_s), "PNG");
    }
}

void LongCapture::finish()
{
    if (!m_run || m_run->stop)
        return;
    dumpPanel();
    m_run->stop = 1;
    // Gone at once; the engine ends its step and hands the picture over (ended()).
    hotkeys(false);
    m_border->hide();
    if (m_panel)
        m_panel->hide();
}

void LongCapture::exportTo(Output output)
{
    if (!m_run || m_run->stop || output < Output::Edit || output > Output::Pin)
        return;
    m_run->output = output;
    finish();
}

void LongCapture::cancel()
{
    if (!m_run || m_run->stop)
        return;
    dumpPanel();
    m_run->stop = 2;
    hotkeys(false);
    m_border->hide();
    if (m_panel)
        m_panel->hide();
}

void LongCapture::copyDiagnostics()
{
    if (!m_run)
        return;
    QString text;
    {
        const std::lock_guard lock(m_run->mutex);
        text = m_run->diag.text();
    }
    if (m_hooks.copyText)
        m_hooks.copyText(text);
    if (m_hint != Hint::Copied)
        m_hintBefore = m_hint;
    m_hint = Hint::Copied;
    m_copiedTimer.start();
    emit changed();
}

void LongCapture::apply(const Update& u)
{
    if (!m_run)
        return;
    m_status = u.status;
    m_height = u.height;
    m_partial = u.partial;
    m_canAuto = u.canAuto;
    m_horizontal = u.horizontal; // as the engine has it: turned by the panel, or by how it was scrolled
    m_autoScroll = m_run->autoScroll; // the engine turns it off when lost or yielded
    if (u.hint != (m_hint == Hint::Copied ? m_hintBefore : m_hint))
        qInfo().noquote() << "Long screenshot:" << QMetaEnum::fromType<Hint>().valueToKey(int(u.hint));
    if (m_hint == Hint::Copied)
        m_hintBefore = u.hint;
    else
        m_hint = u.hint;
    emit changed();
    if (!u.preview.isNull()) {
        m_preview = u.preview;
        emit previewChanged();
    }
}

void LongCapture::wrapUp()
{
    hotkeys(false);
    if (m_border)
        m_border->hide();
    if (m_panel)
        m_panel->hide();
    m_copiedTimer.stop();
    m_preview = QImage();
    emit previewChanged();
    // Both windows release their scene graphs after the idle interval.
    if (!m_run)
        return;
    QString text;
    {
        const std::lock_guard lock(m_run->mutex);
        text = m_run->diag.text();
    }
    // Into the diagnostics log too (the exported bundle has it), in parts under its 8 KB an entry.
    const int parts = int((text.size() + kChunk - 1) / kChunk);
    for (int i = 0; i < parts; ++i)
        diagnostics::record(u"long-capture"_s, {{u"part"_s, i + 1}, {u"of"_s, parts}, {u"text"_s, text.mid(qsizetype(i) * kChunk, kChunk)}});
}

void LongCapture::ended(const QImage& picture, const QString& how)
{
    if (!m_run)
        return;
    if (m_thread.joinable())
        m_thread.join();
    dumpPanel(); // when it ended by itself
    const Output output = m_run->output;
    const QPoint origin = m_run->region.topLeft();
    wrapUp();
    m_run.reset();
    updateExclusion(); // the border's strips; the panel and the preview keep theirs, hidden
    qInfo().noquote() << u"Long screenshot: %1, %2x%3"_s.arg(how).arg(picture.width()).arg(picture.height());
    emit changed();
    if (!picture.isNull() && m_hooks.done)
        m_hooks.done(picture, output, origin);
    if (m_hooks.ended)
        m_hooks.ended();
}

// ---- the engine thread ----------------------------------------------------------

void LongCapture::engine(Run& r)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED); // UI Automation
    QString how;
    QImage picture;
    {
        QElapsedTimer clock;
        clock.start();
        const auto log = [&](const QString& line) {
            const std::lock_guard lock(r.mutex);
            r.diag.event(clock.elapsed(), line);
        };
        const auto pause = [&](int ms) {
            for (const qint64 from = clock.elapsed(); !r.stop && clock.elapsed() - from < ms;)
                ::Sleep(15);
        };
        // The overlays are hidden: one more composition and they are out of what WGC gives.
        ::DwmFlush();
        const int ignoreRight = qRound(kOverlayLogical * r.dpr);
        const int cursorMoveThreshold = std::max(16, qRound(kUserCursorMoveLogical * r.dpr));
        bool sideways = r.horizontal;
        ScrollGrabber grabber(m_grabber, r.region, ignoreRight, sideways);
        auto input = std::make_unique<ScrollInput>(r.region, sideways);
        const bool elevated = input->content() && integrity::higherThanOurs(input->content());
        const bool pageScrollWheel = ScrollInput::pageScrollWheel();
        quint64 ownRectsSeq = 0;
        const auto syncOwnRects = [&] {
            std::vector<QRect> rects;
            bool changed = false;
            {
                const std::lock_guard lock(r.mutex);
                if (r.ownRectsSeq != ownRectsSeq) {
                    ownRectsSeq = r.ownRectsSeq;
                    rects = r.ownRects;
                    changed = true;
                }
            }
            if (changed)
                input->setOwnRects(std::move(rects));
        };
        syncOwnRects();

        {
            ScrollDiagnostics::Facts facts;
            facts.windows = QSysInfo::kernelVersion();
            facts.version = QStringLiteral(WINSHUN_VERSION);
            facts.program = input->program();
            facts.elevated = elevated;
            facts.region = r.region.size();
            facts.scale = r.dpr;
            facts.hdr = grabber.hdr();
            facts.ignoreRight = ignoreRight;
            facts.automatic = r.autoScroll;
            const std::lock_guard lock(r.mutex);
            r.diag.begin(facts);
        }
        log(grabber.wgc() ? u"frames: Windows.Graphics.Capture"_s : u"frames: BitBlt (Windows.Graphics.Capture not available)"_s);
        if (input->prefersSendWheel())
            log(u"target prefers SendInput wheel (WinUI/InputSite)"_s);

        const auto makeStitcher = [&](bool turned) {
            return std::make_unique<ScrollStitcher>(r.region.size(), ScrollStitcher::Limits {}, ScrollStitcher::Options {ignoreRight, turned});
        };
        auto stitcher = makeStitcher(sideways);

        const auto makeAutoConfig = [&](bool turned) {
            ScrollAutoEngine::Config cfg;
            cfg.elevated = elevated;
            cfg.prefersSendWheel = input->prefersSendWheel();
            cfg.pageScrollWheel = pageScrollWheel;
            cfg.horizontal = turned;
            cfg.speed = toScrollSpeed(Speed(r.speed.load()));
            return cfg;
        };
        ScrollAutoEngine autoEngine(makeAutoConfig(sideways));

        if (elevated) {
            // UIPI: the posted wheel is refused (the diagnostics keep the
            // error) and SendInput's dropped; the user scrolls.
            ScrollResult tried = input->scroll(ScrollMethod::PostWheel, 1);
            for (int i = 0; i < 40 && tried.code == ScrollKeysHeld && !r.stop; ++i) {
                pause(50); // the Ctrl of Ctrl+L
                tried = input->scroll(ScrollMethod::PostWheel, 1);
            }
            {
                const std::lock_guard lock(r.mutex);
                r.diag.scrolled(clock.elapsed(), tried, 1);
            }
            log(u"the program runs as administrator: scrolling by hand"_s);
        }

        Status shown = Status::Waiting;
        Hint told = elevated ? Hint::Elevated : (r.autoScroll ? Hint::Auto : Hint::Manual);
        qint64 lastPreview = -kPreviewEveryMs;
        int previewFrames = -1;
        Update posted;
        bool everPosted = false;
        const auto currentPreviewBox = [&] {
            const QSize logical = sideways ? QSize(kPreviewWideWidth, kPreviewWideHeight)
                                           : QSize(kPreviewWidth, kPreviewHeight);
            return QSize(qRound(logical.width() * r.dpr), qRound(logical.height() * r.dpr));
        };
        const auto publish = [&] {
            Update u;
            u.status = shown;
            u.height = stitcher->height();
            u.partial = stitcher->partial();
            u.canAuto = !elevated && autoEngine.canAutoScroll();
            u.horizontal = sideways;
            u.hint = told;
            const qint64 now = clock.elapsed();
            if (stitcher->frames() != previewFrames && now - lastPreview >= kPreviewEveryMs) {
                u.preview = stitcher->preview(currentPreviewBox());
                previewFrames = stitcher->frames();
                lastPreview = now;
            }
            const bool same = everPosted && u.status == posted.status && u.height == posted.height && u.partial == posted.partial
                && u.canAuto == posted.canAuto && u.horizontal == posted.horizontal && u.hint == posted.hint;
            if (same && u.preview.isNull())
                return;
            posted = u;
            posted.preview = QImage();
            everPosted = true;
            QMetaObject::invokeMethod(this, [this, u] { apply(u); }, Qt::QueuedConnection);
        };

        int dumped = 0;
        QDir dumpDir;
        if (!r.dump.isEmpty() && QDir().mkpath(r.dump)) {
            dumpDir = QDir(r.dump);
            QFile steps(dumpDir.filePath(u"steps.txt"_s));
            if (steps.open(QIODevice::WriteOnly | QIODevice::Text)) {
                steps.write(u"# region %1x%2, ignore right %3 px, scale %4\n"_s.arg(r.region.width())
                                .arg(r.region.height())
                                .arg(ignoreRight)
                                .arg(r.dpr)
                                .toUtf8());
            }
            log(u"dumping the frames"_s);
        } else {
            r.dump.clear();
        }

        QImage base; // the last frame that fitted on
        int behindTravel = 0;
        int recoverStreak = 0;
        const auto stitch = [&](const QImage& frame, std::optional<int> expect, bool guess) {
            QElapsedTimer took;
            took.start();
            const ScrollStitcher::Step step = stitcher->add(frame, expect, guess);
            {
                const std::lock_guard lock(r.mutex);
                r.diag.stitched(clock.elapsed(), step, int(took.elapsed()));
            }
            if (!r.dump.isEmpty()) {
                ++dumped;
                frame.save(dumpDir.filePath(u"frame_%1.png"_s.arg(dumped, 4, 10, QChar(u'0'))), "PNG");
                QFile steps(dumpDir.filePath(u"steps.txt"_s));
                if (steps.open(QIODevice::Append | QIODevice::Text)) {
                    steps.write(u"%1 %2 dy %3 score %4 votes %5%6 height %7 bands %8/%9 expect %10\n"_s.arg(dumped, 4, 10, QChar(u'0'))
                                    .arg(stateName(step.state))
                                    .arg(step.dy)
                                    .arg(step.score, 0, 'f', 3)
                                    .arg(step.votes)
                                    .arg(step.predicted ? u" guessed"_s : QString())
                                    .arg(step.height)
                                    .arg(step.top)
                                    .arg(step.bottom)
                                    .arg(expect.value_or(0))
                                    .toUtf8());
                }
            }
            if (step.state != ScrollStitcher::State::Lost && !step.predicted)
                base = frame;
            switch (step.state) {
            case ScrollStitcher::State::Appended:
                behindTravel = 0;
                recoverStreak = 0;
                if (stitcher->frames() > 1)
                    shown = Status::Stitching;
                break;
            case ScrollStitcher::State::Behind:
                behindTravel += std::abs(step.dy);
                if (shown == Status::Lost) {
                    if (std::abs(step.dy) >= 4 || ++recoverStreak >= 2) {
                        shown = behindTravel >= 12
                            ? Status::Behind
                            : (stitcher->height() > stitcher->frameSize().height() ? Status::Stitching : Status::Waiting);
                        recoverStreak = 0;
                    }
                } else if (shown != Status::Stitching || behindTravel >= 12) {
                    shown = Status::Behind;
                }
                break;
            case ScrollStitcher::State::Full:
                behindTravel = 0;
                recoverStreak = 0;
                shown = Status::Full;
                break;
            case ScrollStitcher::State::Lost:
                recoverStreak = 0;
                break;
            case ScrollStitcher::State::Waiting:
                break;
            case ScrollStitcher::State::Unchanged:
                if (shown == Status::Lost && ++recoverStreak >= 2) {
                    shown = stitcher->height() > stitcher->frameSize().height() ? Status::Stitching : Status::Waiting;
                    recoverStreak = 0;
                }
                break;
            }
            return step;
        };

        const auto extended = [&] { return stitcher->height() > stitcher->frameSize().height(); };
        const auto currentZone = [&] {
            const int h = stitcher->frameSize().height();
            return std::max(32, stitcher->frames() > 1 ? h - stitcher->fixedTop() - stitcher->fixedBottom() : h * 4 / 5);
        };
        const auto otherWay = [&](const QImage& frame) {
            if (base.isNull() || extended())
                return false;
            ScrollStitcher trial(r.region.size(), {}, {ignoreRight, !sideways});
            trial.add(base);
            return trial.add(frame).state == ScrollStitcher::State::Appended;
        };

        int manualDirection = 1;
        const auto turn = [&](bool turned, QImage from, bool resetTriedSideways) {
            input->releaseParkedCursor(true);
            sideways = turned;
            r.horizontal = turned;
            grabber.setOrientation(turned);
            stitcher = makeStitcher(turned);
            input = std::make_unique<ScrollInput>(r.region, turned);
            ownRectsSeq = 0;
            syncOwnRects();
            autoEngine.reconfigure(makeAutoConfig(turned), resetTriedSideways);
            manualDirection = 1;
            behindTravel = 0;
            recoverStreak = 0;
            base = QImage();
            shown = Status::Waiting;
            previewFrames = -1;
            log(turned ? u"direction: sideways"_s : u"direction: up and down"_s);
            if (!from.isNull())
                stitch(from, std::nullopt, false);
        };

        // Take the first viewport right away so neither manual nor auto loses
        // the start or stalls behind animations/caret blinking.
        QImage first;
        while (r.stop != 2 && first.isNull() && clock.elapsed() < 3000)
            first = grabber.changed(40);

        if (first.isNull()) {
            how = r.stop == 2 ? u"cancelled"_s : r.stop == 1 ? u"finished"_s : u"no frame came"_s;
            log(u"no first frame"_s);
        } else {
            stitch(first, std::nullopt, false);
            bool autoActive = false;
            if (r.autoScroll && !elevated) {
                autoEngine.begin(clock.elapsed(), manualDirection);
                autoActive = autoEngine.active();
            }
            publish();

            qint64 lostSince = -1;
            bool watchdog = false;
            std::optional<std::pair<ScrollMethod, int>> lastTransientScroll;
            for (;;) {
                if (r.stop) {
                    how = r.stop == 1 ? u"finished"_s : u"cancelled"_s;
                    break;
                }
                if (stitcher->full()) {
                    how = u"at the limit"_s;
                    break;
                }

                syncOwnRects();
                autoEngine.setSpeed(toScrollSpeed(Speed(r.speed.load())));

                if (r.turned.exchange(false) && r.horizontal != sideways) {
                    turn(r.horizontal, grabber.last(), true);
                    lostSince = -1;
                    watchdog = false;
                    if (r.autoScroll && !elevated) {
                        autoEngine.begin(clock.elapsed(), 1);
                        autoActive = autoEngine.active();
                    } else {
                        autoActive = false;
                    }
                    told = elevated ? Hint::Elevated : (autoActive ? Hint::Auto : Hint::Manual);
                    publish();
                    continue;
                }

                const bool wantAuto = r.autoScroll.load();
                if (wantAuto && !autoActive && !elevated) {
                    autoEngine.begin(clock.elapsed(), manualDirection);
                    autoActive = autoEngine.active();
                    lostSince = -1;
                    watchdog = false;
                    told = autoActive ? Hint::Auto : hintFromLadder(autoEngine.hint(), false);
                    log(u"auto-scroll started (direction %1)"_s.arg(manualDirection < 0 ? u"reverse"_s : u"forward"_s));
                    publish();
                } else if (!wantAuto && autoActive) {
                    autoEngine.stop();
                    autoActive = false;
                    input->releaseParkedCursor(true);
                    told = elevated ? Hint::Elevated : Hint::Manual;
                    log(u"auto-scroll paused by user"_s);
                    publish();
                }

                if (!autoActive) {
                    input->releaseParkedCursor(true);
                    const QImage frame = grabber.changed(kManualWaitMs);
                    if (frame.isNull()) {
                        const qint64 quiet = grabber.elapsed() - std::max<qint64>(0, grabber.lastFrameMs());
                        if (!watchdog && quiet >= kWatchdogMs) {
                            log(u"no frame for 5 s (by hand: nothing moved)"_s);
                            watchdog = true;
                        }
                    } else {
                        watchdog = false;
                        ScrollStitcher::Step step = stitch(frame, std::nullopt, false);
                        if (step.state == ScrollStitcher::State::Lost && otherWay(frame)) {
                            log(u"scrolled the other way"_s);
                            turn(!sideways, base, false);
                            step = stitch(frame, std::nullopt, false);
                        }
                        if (step.state == ScrollStitcher::State::Appended || step.state == ScrollStitcher::State::Behind) {
                            if (std::abs(step.dy) >= 8)
                                manualDirection = step.dy < 0 ? -1 : 1;
                        }
                        if (step.state != ScrollStitcher::State::Lost) {
                            if (shown != Status::Lost)
                                lostSince = -1;
                        } else if (lostSince < 0) {
                            lostSince = clock.elapsed();
                        }
                    }
                    const bool lost = (lostSince >= 0 && clock.elapsed() - lostSince >= kLostShownMs) || shown == Status::Lost;
                    if (lost)
                        shown = Status::Lost;
                    told = lost                                                   ? Hint::Reconnect
                        : elevated || autoEngine.hint() == ScrollLadder::Hint::Elevated ? Hint::Elevated
                        : autoEngine.hint() == ScrollLadder::Hint::NothingWorks   ? Hint::NothingWorks
                                                                                  : Hint::Manual;
                    publish();
                    continue;
                }

                // Continuous closed-loop auto-scrolling.
                const qint64 now = clock.elapsed();
                if (input->cursorParked() && input->userMovedCursor()) {
                    input->releaseParkedCursor(false); // yield immediately; never fight the user's hand
                    const bool inside = input->cursorInRegion();
                    autoEngine.onUserMovedCursor(now, inside);
                    log(inside ? u"user moved mouse inside region: cursor yielded"_s : u"user moved mouse outside region: pausing"_s);
                }

                const int zone = currentZone();
                const ScrollAutoEngine::Command cmd = autoEngine.next(now, zone, extended());
                switch (cmd.kind) {
                case ScrollAutoEngine::CommandKind::WaitFrame: {
                    if (cmd.hint == ScrollLadder::Hint::MouseOutside)
                        told = Hint::MouseOutside;
                    else if (!watchdog)
                        told = Hint::Auto;
                    const QImage frame = grabber.changed(cmd.timeoutMs);
                    if (frame.isNull()) {
                        const qint64 quiet = grabber.elapsed() - std::max<qint64>(0, grabber.lastFrameMs());
                        if (!watchdog && quiet >= kWatchdogMs) {
                            log(u"watchdog: no frame for 5 s"_s);
                            watchdog = true;
                            told = Hint::NoFrames;
                        }
                    } else {
                        watchdog = false;
                        const int exp = autoEngine.expectedStepPx();
                        const std::optional<int> expect = exp != 0 ? std::optional<int>(exp) : std::nullopt;
                        ScrollStitcher::Step step = stitch(frame, expect, false);
                        if (step.state == ScrollStitcher::State::Lost && otherWay(frame)) {
                            log(u"it moved the other way"_s);
                            turn(!sideways, base, false);
                            step = stitch(frame, std::nullopt, false);
                            autoEngine.begin(clock.elapsed(), step.dy < 0 ? -1 : 1);
                            autoActive = autoEngine.active();
                        } else {
                            autoEngine.onFrame(clock.elapsed(), step, extended(), currentZone());
                            if (!autoEngine.active() && autoEngine.phase() == ScrollAutoEngine::Phase::Idle) {
                                // User scrolled backwards manually to take over.
                                r.autoScroll = false;
                                autoActive = false;
                                input->releaseParkedCursor(true);
                                told = Hint::Manual;
                                log(u"auto-scroll yielded after reverse scroll"_s);
                            }
                        }
                    }
                    publish();
                    break;
                }

                case ScrollAutoEngine::CommandKind::SendScroll: {
                    ScrollResult sent;
                    sent.method = cmd.method;
                    bool handledBeforeSend = false;
                    if (cmd.method == ScrollMethod::SendWheel && !input->cursorInRegion() && cmd.allowParkCursor) {
                        const ScrollCode parked = input->parkCursor(cursorMoveThreshold);
                        if (parked == ScrollOk) {
                            log(u"cursor parked in region for send wheel"_s);
                            pause(15);
                        } else if (parked == ScrollKeysHeld) {
                            sent.code = ScrollKeysHeld;
                            handledBeforeSend = true;
                        }
                    }
                    if (r.stop)
                        break;
                    if (!handledBeforeSend)
                        sent = input->scroll(cmd.method, cmd.amount, cmd.direction);
                    const qint64 sentAt = clock.elapsed();
                    const bool transient = sent.code == ScrollCursorOutside || sent.code == ScrollKeysHeld;
                    const auto key = std::make_pair(sent.method, sent.code);
                    if (!transient || lastTransientScroll != key) {
                        const std::lock_guard lock(r.mutex);
                        r.diag.scrolled(sentAt, sent, cmd.amount);
                        lastTransientScroll = transient ? std::optional(key) : std::nullopt;
                    }
                    autoEngine.onScrollSent(sentAt, sent, cmd.amount, zone);
                    if (autoEngine.hint() == ScrollLadder::Hint::MouseOutside)
                        told = Hint::MouseOutside;
                    else if (!watchdog)
                        told = Hint::Auto;
                    publish();
                    break;
                }

                case ScrollAutoEngine::CommandKind::SwitchSideways: {
                    log(u"nothing moved up or down: trying sideways"_s);
                    turn(true, grabber.last(), false);
                    autoEngine.begin(clock.elapsed(), 1);
                    autoActive = autoEngine.active();
                    told = Hint::Auto;
                    publish();
                    break;
                }

                case ScrollAutoEngine::CommandKind::FinishAtEnd: {
                    input->releaseParkedCursor(true);
                    how = u"at the end"_s;
                    goto finishedLoop;
                }

                case ScrollAutoEngine::CommandKind::YieldToManual: {
                    input->releaseParkedCursor(true);
                    r.autoScroll = false;
                    autoActive = false;
                    if (cmd.lost) {
                        shown = Status::Lost;
                        told = Hint::Reconnect;
                        lostSince = clock.elapsed() - kLostShownMs;
                        log(u"lost after recovery: auto-scroll off"_s);
                    } else {
                        told = hintFromLadder(cmd.hint, false);
                    }
                    publish();
                    break;
                }
                }
            }
        finishedLoop:;
        }

        input->releaseParkedCursor(true);

        // Finish can arrive while WGC or a smooth scroll still has the final
        // changed frame in flight. Drain it before exporting; cancellation
        // never waits for or exports these frames.
        if (r.stop == 1 && stitcher->frames() > 0 && !stitcher->full()) {
            const qint64 finishAt = clock.elapsed();
            while (r.stop != 2 && clock.elapsed() - finishAt < kFinishDrainMs) {
                const QImage last = grabber.changed(kFinishQuietMs);
                if (last.isNull())
                    break;
                stitch(last, std::nullopt, false);
                if (stitcher->full())
                    break;
            }
        }
        // The picture, then the memory back (the stream and UI Automation go with this scope).
        if (r.stop != 2 && stitcher->height() > 0)
            picture = stitcher->result();
        {
            const std::lock_guard lock(r.mutex);
            r.diag.ended(clock.elapsed(), how, stitcher->height(), stitcher->fixedTop(), stitcher->fixedBottom(), stitcher->partial());
        }
        stitcher->release();
    }
    if (SUCCEEDED(com))
        ::CoUninitialize();
    QMetaObject::invokeMethod(this, [this, picture, how] { ended(picture, how); }, Qt::QueuedConnection);
}

} // namespace ws::capture

#include "LongCapture.h"

#include "DiagnosticLog.h"
#include "Geometry.h" // placeBeside
#include "Grabber.h"
#include "ScrollDiagnostics.h"
#include "ScrollGrabber.h"
#include "ScrollInput.h"
#include "ScrollStitcher.h"
#include "CaptureSession.h" // kLongMinimum
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
#include <mutex>
#include <optional>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws::capture {

namespace {

constexpr int kQuietMs = 100; // at rest: nothing in the region changed for this long
constexpr int kSettleMs = 2500; // at most this long for a step to come to rest
constexpr int kMoveWaitFirstMs = 1000; // after a step, for the picture to change, before any way moved it
constexpr int kMoveWaitMs = 600; // once one has
constexpr int kRetryMs = 300; // a step's frame did not fit: once more, a moment later
constexpr int kLostShownMs = 300; // by hand: "lost" only when it lasts (a fling's frames come apart)
constexpr int kWatchdogMs = 5000;
constexpr int kPreviewEveryMs = 100;
constexpr double kAim = 0.5; // a step aims at half the part that scrolls
constexpr double kOverlayLogical = 16; // the overlay scroll bar's strip on the right, left out of the comparing
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

// After a step: the first change within `waitMs`, then the frames until
// none changed for kQuietMs (kSettleMs at most): the picture at rest. Null
// when nothing changed.
QImage settle(ScrollGrabber& grabber, int waitMs, const std::atomic<int>& stop)
{
    const qint64 start = grabber.elapsed();
    QImage frame;
    while (frame.isNull() && !stop && grabber.elapsed() - start < waitMs)
        frame = grabber.changed(int(std::clamp<qint64>(waitMs - (grabber.elapsed() - start), 1, 100)));
    while (!frame.isNull() && !stop && grabber.elapsed() - start < kSettleMs) {
        const QImage next = grabber.changed(kQuietMs);
        if (next.isNull())
            break;
        frame = next;
    }
    return frame;
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
            if (strip) {
                ::SetWindowLongPtrW(strip, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
                // Never in a picture of the screen (it lies outside the region besides).
                ::SetWindowDisplayAffinity(strip, WDA_EXCLUDEFROMCAPTURE);
            }
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
    QSize previewBox; // device px
    QString dump; // WINSHUN_SCROLL_DUMP: every frame stitched and what came of it, there
    std::atomic<int> stop {0}; // 1 finish, 2 cancel
    std::atomic<bool> autoScroll {false}; // the panel's switch; the engine turns it off when lost
    std::mutex mutex; // the diagnostics
    ScrollDiagnostics diag;
    Output output = Output::Edit; // GUI thread only, used once the worker has finished
};

struct LongCapture::Update {
    Status status = Status::Waiting;
    int height = 0;
    bool partial = false;
    bool canAuto = true;
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

QString LongCapture::hint() const
{
    switch (m_hint) {
    case Hint::Auto:
        return tr("正在自动滚动。Enter 完成，Esc 取消");
    case Hint::Manual:
        return tr("在区域内向上或向下滚动，滚完按 Enter 或点“完成”");
    case Hint::MouseOutside:
        return tr("把鼠标放到要滚动的区域上");
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
        return tr("到上限了（3 万像素高）");
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
    window->create();
    win::styleFramelessWindow(window, false);
    // Never in a picture of the screen, never the window with the keys.
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    ::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    m_panel = window;
    m_previewWindow = window->findChild<QQuickWindow*>(u"longCapturePreview"_s);
    if (m_previewWindow) {
        m_previewWindow->create();
        win::styleFramelessWindow(m_previewWindow, false);
        const HWND preview = reinterpret_cast<HWND>(m_previewWindow->winId());
        ::SetWindowDisplayAffinity(preview, WDA_EXCLUDEFROMCAPTURE);
        ::SetWindowLongPtrW(preview, GWL_EXSTYLE, ::GetWindowLongPtrW(preview, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    }
    return window;
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
    m_run->previewBox = QSize(qRound(kPreviewWidth * dpr), qRound(kPreviewHeight * dpr));
    if (const QString dump = qEnvironmentVariable("WINSHUN_SCROLL_DUMP"); !dump.isEmpty())
        m_run->dump = QDir(dump).filePath(QDateTime::currentDateTime().toString(u"yyyyMMdd-HHmmss"_s)); // a folder a run
    m_autoScroll = m_autoScrollChosen;
    m_run->autoScroll = m_autoScroll;
    m_status = Status::Waiting;
    m_height = region.height();
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
    // Explicitly raise the control strip without taking the target's focus.
    ::SetWindowPos(reinterpret_cast<HWND>(window->winId()), HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    hotkeys(true);
    qInfo().noquote() << u"Long screenshot: %1x%2 at %3,%4, auto-scroll %5"_s.arg(region.width())
                             .arg(region.height())
                             .arg(region.x())
                             .arg(region.y())
                             .arg(m_autoScroll ? u"on"_s : u"off"_s);
    Run* run = m_run.get();
    m_thread = std::jthread([this, run] { engine(*run); });
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
    m_autoScroll = m_run->autoScroll; // the engine turns it off when lost
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
    if (m_panel)
        m_panel->releaseResources(); // hidden: what it drew with goes (as the editing window's)
    if (m_previewWindow)
        m_previewWindow->releaseResources();
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
                ::Sleep(20);
        };
        // The overlays are hidden: one more composition and they are out of what WGC gives.
        ::DwmFlush();
        ScrollGrabber grabber(m_grabber, r.region);
        ScrollInput input(r.region);
        const bool elevated = input.content() && integrity::higherThanOurs(input.content());
        const int ignoreRight = qRound(kOverlayLogical * r.dpr);
        {
            ScrollDiagnostics::Facts facts;
            facts.windows = QSysInfo::kernelVersion();
            facts.version = QStringLiteral(WINSHUN_VERSION);
            facts.program = input.program();
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
        ScrollStitcher stitcher(r.region.size(), {}, {ignoreRight});
        ScrollLadder ladder;
        if (elevated) {
            // UIPI: the posted wheel is refused (the diagnostics keep the
            // error) and SendInput's dropped; the user scrolls.
            ScrollResult tried = input.scroll(ScrollMethod::PostWheel, 1);
            for (int i = 0; i < 40 && tried.code == ScrollKeysHeld && !r.stop; ++i) {
                pause(50); // the Ctrl of Ctrl+L
                tried = input.scroll(ScrollMethod::PostWheel, 1);
            }
            {
                const std::lock_guard lock(r.mutex);
                r.diag.scrolled(clock.elapsed(), tried, 1);
            }
            log(u"the program runs as administrator: scrolling by hand"_s);
            ladder = ScrollLadder(true);
        }

        Status shown = Status::Waiting;
        Hint told = r.autoScroll ? Hint::Auto : Hint::Manual;
        bool matched = false; // the last frame stitched fitted on (a guess needs it)
        qint64 lastPreview = -kPreviewEveryMs;
        int previewFrames = -1; // the stitcher's frames when the preview was made
        Update posted;
        bool everPosted = false;
        const auto publish = [&] {
            Update u;
            u.status = shown;
            u.height = stitcher.height();
            u.partial = stitcher.partial();
            u.canAuto = ladder.method() != ScrollMethod::Manual;
            u.hint = told;
            const qint64 now = clock.elapsed();
            if (stitcher.frames() != previewFrames && now - lastPreview >= kPreviewEveryMs) {
                u.preview = stitcher.preview(r.previewBox);
                previewFrames = stitcher.frames();
                lastPreview = now;
            }
            const bool same = everPosted && u.status == posted.status && u.height == posted.height && u.partial == posted.partial
                && u.canAuto == posted.canAuto && u.hint == posted.hint;
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
            // For wsbench --scroll-replay.
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
        const auto stitch = [&](const QImage& frame, std::optional<int> expect, bool guess) {
            QElapsedTimer took;
            took.start();
            const ScrollStitcher::Step step = stitcher.add(frame, expect, guess);
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
            matched = step.state != ScrollStitcher::State::Lost && !step.predicted; // never a guess on a guess
            switch (step.state) {
            case ScrollStitcher::State::Appended:
                if (stitcher.frames() > 1)
                    shown = Status::Stitching;
                break;
            case ScrollStitcher::State::Behind:
                shown = Status::Behind;
                break;
            case ScrollStitcher::State::Full:
                shown = Status::Full;
                break;
            case ScrollStitcher::State::Lost: // the caller decides what to show
            case ScrollStitcher::State::Waiting:
                break;
            case ScrollStitcher::State::Unchanged:
                if (shown == Status::Lost)
                    shown = stitcher.height() > r.region.height() ? Status::Stitching : Status::Waiting;
                break;
            }
            return step;
        };

        // Manual capture keeps the first viewport immediately: replacing it
        // during a settling delay loses the start when the user scrolls at
        // once. Subsequent stationary changes can refresh it in the stitcher.
        // Auto-scroll still waits for a quiet initial viewport before moving.
        QImage first;
        while (r.stop != 2 && first.isNull() && clock.elapsed() < 3000)
            first = grabber.changed(100);
        const qint64 firstAt = clock.elapsed();
        while (!first.isNull() && !r.stop && r.autoScroll && ladder.method() != ScrollMethod::Manual
            && clock.elapsed() - firstAt < kSettleMs) {
            const QImage next = grabber.changed(kQuietMs);
            if (next.isNull())
                break;
            first = next;
        }
        if (first.isNull()) {
            how = r.stop == 2 ? u"cancelled"_s : r.stop == 1 ? u"finished"_s : u"no frame came"_s;
            log(u"no first frame"_s);
        } else {
            stitch(first, std::nullopt, false);
            publish();
            qint64 lostSince = -1;
            bool watchdog = false;
            for (;;) {
                if (r.stop) {
                    how = r.stop == 1 ? u"finished"_s : u"cancelled"_s;
                    break;
                }
                if (stitcher.full()) {
                    how = u"at the limit"_s;
                    break;
                }
                const bool automatic = r.autoScroll && ladder.method() != ScrollMethod::Manual;
                if (!automatic) {
                    // By hand: every frame that changed.
                    const QImage frame = grabber.changed(100);
                    if (frame.isNull()) {
                        const qint64 quiet = grabber.elapsed() - std::max<qint64>(0, grabber.lastFrameMs());
                        if (!watchdog && quiet >= kWatchdogMs) {
                            log(u"no frame for 5 s (by hand: nothing moved)"_s);
                            watchdog = true;
                        }
                    } else {
                        watchdog = false;
                        const ScrollStitcher::Step step = stitch(frame, std::nullopt, false);
                        if (step.state != ScrollStitcher::State::Lost)
                            lostSince = -1;
                        else if (lostSince < 0)
                            lostSince = clock.elapsed();
                    }
                    const bool lost = lostSince >= 0 && clock.elapsed() - lostSince >= kLostShownMs;
                    if (lost)
                        shown = Status::Lost;
                    told = lost                                                ? Hint::Reconnect
                        : ladder.hint() == ScrollLadder::Hint::Elevated     ? Hint::Elevated
                        : ladder.hint() == ScrollLadder::Hint::NothingWorks ? Hint::NothingWorks
                                                                            : Hint::Manual;
                    publish();
                    continue;
                }

                // Auto-scroll: one step, the picture at rest, stitched.
                told = watchdog ? Hint::NoFrames : Hint::Auto;
                const int h = r.region.height();
                const int zone = stitcher.frames() > 1 ? h - stitcher.fixedTop() - stitcher.fixedBottom() : h * 4 / 5;
                const ScrollMethod method = ladder.method();
                const double amount = ladder.amount(std::max(16, int(zone * kAim)));
                const int expect = ladder.expected(amount);
                const bool anyWorked = ladder.worked(ScrollMethod::PostWheel) || ladder.worked(ScrollMethod::SendWheel)
                    || ladder.worked(ScrollMethod::Uia);
                const bool matchedBefore = matched;
                const ScrollResult sent = input.scroll(method, amount);
                {
                    const std::lock_guard lock(r.mutex);
                    r.diag.scrolled(clock.elapsed(), sent, amount);
                }
                if (sent.code != ScrollOk) {
                    ladder.refused(sent.code);
                    if (ladder.hint() == ScrollLadder::Hint::MouseOutside)
                        told = Hint::MouseOutside;
                    if (ladder.atEnd()) {
                        how = u"at the end"_s;
                        break;
                    }
                    publish();
                    if (sent.code == ScrollKeysHeld || ladder.hint() == ScrollLadder::Hint::MouseOutside)
                        pause(150);
                    continue;
                }
                const QImage still = settle(grabber, anyWorked ? kMoveWaitMs : kMoveWaitFirstMs, r.stop);
                if (r.stop)
                    continue;
                if (still.isNull()) {
                    ladder.moved(0, amount);
                    {
                        const std::lock_guard lock(r.mutex);
                        r.diag.moved(clock.elapsed(), method, 0);
                    }
                    const qint64 quiet = grabber.elapsed() - std::max<qint64>(0, grabber.lastFrameMs());
                    if (!watchdog && quiet >= kWatchdogMs) {
                        log(u"watchdog: no frame for 5 s"_s);
                        watchdog = true;
                        told = Hint::NoFrames;
                    }
                    if (ladder.atEnd()) {
                        how = u"at the end"_s;
                        break;
                    }
                    publish();
                    continue;
                }
                watchdog = false;
                told = Hint::Auto;
                const std::optional<int> expected = expect > 0 ? std::optional<int>(expect) : std::nullopt;
                ScrollStitcher::Step step = stitch(still, expected, false);
                if (step.state == ScrollStitcher::State::Lost) {
                    // Once more a moment later; then, where the frame before
                    // fitted and a step's rows are known, the guess (the
                    // result is then partly guessed).
                    const QImage again = grabber.changed(kRetryMs);
                    if (!again.isNull())
                        step = stitch(again, expected, false);
                    if (step.state == ScrollStitcher::State::Lost && expected && matchedBefore)
                        step = stitch(grabber.last(), expected, true);
                }
                switch (step.state) {
                case ScrollStitcher::State::Appended:
                case ScrollStitcher::State::Behind:
                case ScrollStitcher::State::Full:
                    ladder.moved(step.dy, amount);
                    {
                        const std::lock_guard lock(r.mutex);
                        r.diag.moved(clock.elapsed(), method, step.dy);
                    }
                    break;
                case ScrollStitcher::State::Unchanged:
                    ladder.moved(0, amount);
                    {
                        const std::lock_guard lock(r.mutex);
                        r.diag.moved(clock.elapsed(), method, 0);
                    }
                    break;
                case ScrollStitcher::State::Lost:
                    // No overlap and nothing to guess with: by hand from here.
                    r.autoScroll = false;
                    shown = Status::Lost;
                    told = Hint::Reconnect;
                    lostSince = clock.elapsed() - kLostShownMs;
                    log(u"lost: auto-scroll off"_s);
                    break;
                case ScrollStitcher::State::Waiting:
                    break;
                }
                if (ladder.atEnd()) {
                    how = u"at the end"_s;
                    break;
                }
                publish();
            }
        }
        // Finish can arrive while WGC or a smooth scroll still has the final
        // changed frame in flight. Drain it before exporting; cancellation
        // never waits for or exports these frames.
        if (r.stop == 1 && stitcher.frames() > 0 && !stitcher.full()) {
            const qint64 finishAt = clock.elapsed();
            while (r.stop != 2 && clock.elapsed() - finishAt < kMoveWaitMs) {
                const QImage last = grabber.changed(kQuietMs);
                if (last.isNull())
                    break;
                stitch(last, std::nullopt, false);
                if (stitcher.full())
                    break;
            }
        }
        // The picture, then the memory back (the stream and UI Automation go with this scope).
        if (r.stop != 2 && stitcher.height() > 0)
            picture = stitcher.result();
        {
            const std::lock_guard lock(r.mutex);
            r.diag.ended(clock.elapsed(), how, stitcher.height(), stitcher.fixedTop(), stitcher.fixedBottom(), stitcher.partial());
        }
        stitcher.release();
    }
    if (SUCCEEDED(com))
        ::CoUninitialize();
    QMetaObject::invokeMethod(this, [this, picture, how] { ended(picture, how); }, Qt::QueuedConnection);
}

} // namespace ws::capture

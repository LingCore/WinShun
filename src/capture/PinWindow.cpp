#include "PinWindow.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDebug>
#include <QPainter>

#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>
#include <cmath>
#include <cstring>
#include <thread>

namespace ws::capture {

namespace {

const wchar_t* const kClassName = L"WinShun.Pin";
constexpr UINT kEncoded = WM_APP + 1; // the PNG bytes of a collapsed pin are made (lParam: the job)
constexpr UINT_PTR kHintTimer = 1;
constexpr qint64 kHintHoldMs = 900; // the zoom or opacity stays this long after the last notch ...
constexpr qint64 kNoteHoldMs = 1800; // status notes (Tab 看原文, 没有找到文字) float outside and stay readable
constexpr qint64 kHintFadeMs = 240; // ... then fades out in this
constexpr UINT kFadeTickMs = 16; // 60 fps fade on the lightweight pill window
constexpr qint64 kPillHoldMs = 2200; // the text pick's pill: long enough to read, then fades out (GitHub #258)
constexpr qint64 kAutoReadyHoldMs = 1600;
constexpr qint64 kSelectionHintHoldMs = 1800;
constexpr qint64 kCopiedHoldMs = 1500;
// Skip automatic background OCR on tiny swatches or giant stitched pictures (> 4K*4K); manual T / Shift+C still reads them.
constexpr qint64 kAutoOcrMaxPixels = 16'000'000;

qint64 steadyNowMs()
{
    return static_cast<qint64>(::GetTickCount64());
}

// 255 through `hold` ms of `age`, then down to 0 over kHintFadeMs.
int fadedAlpha(qint64 age, qint64 hold)
{
    if (age < hold)
        return 255;
    if (age >= hold + kHintFadeMs)
        return 0;
    return static_cast<int>(255 * (hold + kHintFadeMs - age) / kHintFadeMs);
}

void registerClass()
{
    static const bool registered = [] {
        WNDCLASSEXW wc {sizeof wc};
        wc.style = CS_DBLCLKS; // a double click: back to the size it was pinned at
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) { return ::DefWindowProcW(h, m, w, l); };
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_SIZEALL); // it moves where it is dragged
        wc.lpszClassName = kClassName;
        return ::RegisterClassExW(&wc) != 0;
    }();
    Q_UNUSED(registered);
}

// A frame `thickness` pixels wide just inside `outer`.
void ring(QPainter& painter, const QRect& outer, int thickness, const QColor& color)
{
    const int t = std::min({thickness, outer.width(), outer.height()});
    painter.fillRect(QRect(outer.left(), outer.top(), outer.width(), t), color);
    painter.fillRect(QRect(outer.left(), outer.bottom() - t + 1, outer.width(), t), color);
    painter.fillRect(QRect(outer.left(), outer.top() + t, t, outer.height() - 2 * t), color);
    painter.fillRect(QRect(outer.right() - t + 1, outer.top() + t, t, outer.height() - 2 * t), color);
}

// The picture's rows into the canvas at `at`, opaque: grabbed pixels may
// carry alpha 0 (BitBlt), which a layered window would show as nothing.
void copyOpaque(const QImage& picture, QImage& canvas, QPoint at)
{
    const int w = std::min(picture.width(), canvas.width() - at.x());
    for (int y = 0; y < picture.height() && at.y() + y < canvas.height(); ++y) {
        const auto* in = reinterpret_cast<const QRgb*>(picture.constScanLine(y));
        auto* out = reinterpret_cast<QRgb*>(canvas.scanLine(at.y() + y)) + at.x();
        for (int x = 0; x < w; ++x)
            out[x] = in[x] | 0xFF000000u;
    }
}

// The key types "?" on the keyboard layout in use (Shift+/ on most, not on all).
bool typesQuestionMark(UINT virtualKey, LPARAM lParam)
{
    BYTE state[256] {};
    if (!::GetKeyboardState(state))
        return false;
    wchar_t typed[4] {};
    const UINT scan = (static_cast<UINT>(lParam) >> 16) & 0xFFu;
    // 4: the keyboard's state (a dead key waiting) is left as it was.
    const int count = ::ToUnicodeEx(virtualKey, scan, state, typed, 4, 4, ::GetKeyboardLayout(0));
    return count == 1 && typed[0] == L'?';
}

} // namespace

PinWindow::PinWindow(const QImage& image, QPoint origin, Host host, std::optional<QPoint> fitAbout)
    : m_host(std::move(host))
    , m_image(image.convertToFormat(QImage::Format_RGB32))
    , m_imageSize(image.size())
    , m_origin(origin)
{
    registerClass();
    // Per-monitor v2 whatever the thread's default: the picture must stay
    // in physical pixels, and WM_DPICHANGED must come.
    const DPI_AWARENESS_CONTEXT previous = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (fitAbout)
        fitToWorkArea(*fitAbout);
    m_hwnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"Win顺 贴图", WS_POPUP,
        m_origin.x(), m_origin.y(), 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (previous)
        ::SetThreadDpiAwarenessContext(previous);
    if (!m_hwnd) {
        qWarning() << "Screen capture: no pin window, error" << ::GetLastError();
        return;
    }
    ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ::SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PinWindow::proc));
    // No input method: a pin takes no text, and with the thread's default
    // context a letter typed at it opened the IME's candidate window. Qt
    // does the same for its windows whose focus takes no text
    // (QWindowsInputContext::setWindowsImeEnabled); the overlay's text
    // box keeps its own.
    ::ImmAssociateContextEx(m_hwnd, nullptr, 0);
    m_dpi = std::max<UINT>(96, ::GetDpiForWindow(m_hwnd));
    m_frame = pinFrame(dpr());
    render();
    ::ShowWindow(m_hwnd, SW_SHOW);
    ::SetForegroundWindow(m_hwnd); // Esc closes it at once; Win顺 is in front (the overlay was)
}

PinWindow::~PinWindow()
{
    *m_alive = false;
    m_hintPill.destroy();
    m_textPill.destroy();
    cancelPendingScan();
    if (m_encode)
        m_encode->cancelled = true;
    if (m_hwnd) {
        ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    releasePixels();
}

QSize PinWindow::fullSize() const
{
    return m_image.isNull() ? m_imageSize : m_image.size();
}

QImage PinWindow::image() const
{
    if (!m_image.isNull())
        return m_image;
    if (m_png.isEmpty())
        return {};
    QImage decoded;
    if (!decoded.loadFromData(m_png, "PNG"))
        return {};
    return decoded.convertToFormat(QImage::Format_RGB32);
}

const QImage& PinWindow::shownImage() const
{
    return m_form == PinForm::Full ? m_image : m_thumb;
}

QRect PinWindow::pictureRect() const
{
    return QRect(m_origin, m_form == PinForm::Full ? zoomedSize(fullSize(), m_zoom) : m_thumb.size());
}

QRect PinWindow::windowRect() const
{
    return pinWindowRect(pictureRect(), m_frame);
}

QRect PinWindow::workArea() const
{
    const QRect p = pictureRect();
    const RECT r {p.left(), p.top(), p.right() + 1, p.bottom() + 1};
    MONITORINFO info {sizeof info};
    if (::GetMonitorInfoW(::MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &info))
        return QRect(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top);
    return QRect(0, 0, 1920, 1080);
}

QRect PinWindow::workAreaAt(QPoint point) const
{
    const POINT at {point.x(), point.y()};
    MONITORINFO info {sizeof info};
    if (::GetMonitorInfoW(::MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &info))
        return QRect(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top);
    return QRect(0, 0, 1920, 1080);
}

QRect PinWindow::pillBounds() const
{
    const QRect p = pictureRect();
    const RECT r {p.left(), p.top(), p.right() + 1, p.bottom() + 1};
    MONITORINFO info {sizeof info};
    if (::GetMonitorInfoW(::MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &info)) {
        const QRect work(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
            info.rcWork.bottom - info.rcWork.top);
        if (work.intersects(p))
            return work;
        return QRect(info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
            info.rcMonitor.bottom - info.rcMonitor.top);
    }
    return QRect(0, 0, 1920, 1080);
}

void PinWindow::fitToWorkArea(QPoint anchor)
{
    // The anchor's monitor, not the one nearest the whole picture's top
    // left: a long picture's middle at the cursor starts thousands of pixels
    // above it, nearer a monitor stacked on top.
    const QRect work = workAreaAt(anchor);
    m_fitZoom = fitZoom(m_imageSize, work.size());
    m_zoom = m_fitZoom;
    m_origin = fitOrigin(m_origin, m_imageSize, m_zoom, anchor, work);
}

double PinWindow::minZoom() const
{
    // Shrinking stays possible down to the smallest step; a picture fitted
    // under it has the fit as its bottom.
    return (std::min)(m_fitZoom, zoomSteps().front());
}

std::optional<QRect> PinWindow::edgeTab() const
{
    if (m_form != PinForm::Edge)
        return std::nullopt;
    return pictureRect();
}

void PinWindow::releasePixels()
{
    if (m_dc) {
        ::SelectObject(m_dc, m_oldBitmap);
        ::DeleteDC(m_dc);
        m_dc = nullptr;
    }
    if (m_bitmap) {
        ::DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
    m_bits = nullptr;
    m_dibSize = {};
    m_image = {};
    m_scaled = {};
    m_thumb = {};
    m_png = {};
}

void PinWindow::render()
{
    const QImage& source = shownImage();
    if (!m_hwnd || source.isNull())
        return;
    const QRect window = windowRect();
    if (m_dibSize != window.size()) {
        if (m_dc) {
            ::SelectObject(m_dc, m_oldBitmap);
            ::DeleteObject(m_bitmap);
        } else {
            m_dc = ::CreateCompatibleDC(nullptr);
        }
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = window.width();
        info.bmiHeader.biHeight = -window.height(); // top-down, as QImage
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!m_bitmap) {
            qWarning() << "Screen capture: no pin bitmap of" << window.size();
            m_bits = nullptr;
            m_dibSize = {};
            return;
        }
        m_oldBitmap = ::SelectObject(m_dc, m_bitmap);
        m_bits = static_cast<uchar*>(bits);
        m_dibSize = window.size();
    }
    // The DIB as a QImage (BGRA in memory = ARGB32 on little endian), no copy.
    QImage canvas(m_bits, m_dibSize.width(), m_dibSize.height(), m_dibSize.width() * 4, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    const int m = m_frame.margin();
    const QRect picture(m, m, window.width() - 2 * m, window.height() - 2 * m);
    {
        QPainter painter(&canvas);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        // The shadow: fading out from the border, quadratically.
        const int s = m_frame.shadow;
        const QRect bordered = picture.adjusted(-m_frame.border, -m_frame.border, m_frame.border, m_frame.border);
        for (int i = 0; i < s; ++i) {
            const double f = static_cast<double>(s - i) / s;
            ring(painter, bordered.adjusted(-i - 1, -i - 1, i + 1, i + 1), 1, QColor(0, 0, 0, static_cast<int>(std::lround(70 * f * f))));
        }
    }
    if (m_form != PinForm::Full) {
        m_scaled = {};
        copyOpaque(m_thumb, canvas, picture.topLeft());
    } else if (std::abs(m_zoom - 1.0) < 1e-9) {
        m_scaled = {};
        copyOpaque(m_image, canvas, picture.topLeft()); // its pixels as they are
    } else {
        if (m_scaled.size() != picture.size()) {
            // Whole multiples as squares of pixels; other zooms smoothed
            // (QImage::scaled filters properly when shrinking).
            const bool whole = m_zoom > 1 && std::abs(m_zoom - std::round(m_zoom)) < 1e-9;
            m_scaled = m_image.scaled(picture.size(), Qt::IgnoreAspectRatio, whole ? Qt::FastTransformation : Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGB32);
        }
        copyOpaque(m_scaled, canvas, picture.topLeft());
    }
    if (m_form == PinForm::Full
        && (!m_picked.isEmpty() || (m_picking && !m_pickDirect && !m_pickBand.isEmpty())))
        drawTextLayer(canvas, picture);
    if (const QRect button = closeButton(); !button.isEmpty())
        drawCloseButton(canvas, button.translated(-window.topLeft()));
    paintBorder();
    syncPills();
}

QRect PinWindow::closeButton() const
{
    if (m_form != PinForm::Full || m_textMode || m_picking || m_closeCorner == Corner::None
        || (m_clickThrough && !m_reachable))
        return {};
    return closeButtonRect(pictureRect(), m_closeCorner, dpr());
}

void PinWindow::drawCloseButton(QImage& canvas, const QRect& button)
{
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    // A dark disc that reads over any picture; red under the cursor, as a window's close button.
    painter.setBrush(m_closeHot ? (m_closePressed ? QColor(0xA3, 0x23, 0x17) : QColor(0xC4, 0x2B, 0x1C))
                                : QColor(0, 0, 0, 150));
    painter.drawEllipse(QRectF(button));
    // Phosphor's X (Regular): from 56 to 200 of its 256 grid, round ends,
    // drawn in a 14 px box at a 1.5 px line.
    const QPointF c = QRectF(button).center();
    const double half = 14 * dpr() * (72.0 / 256.0);
    QPen pen(Qt::white, 1.5 * dpr(), Qt::SolidLine, Qt::RoundCap);
    painter.setPen(pen);
    painter.drawLine(QPointF(c.x() - half, c.y() - half), QPointF(c.x() + half, c.y() + half));
    painter.drawLine(QPointF(c.x() + half, c.y() - half), QPointF(c.x() - half, c.y() + half));
}

void PinWindow::updateCloseHover(QPoint cursor)
{
    if (!m_hwnd)
        return;
    Corner corner = m_closeCorner;
    if (!m_tracking) { // the cursor has just come onto the picture
        TRACKMOUSEEVENT track {sizeof track, TME_LEAVE, m_hwnd, 0};
        m_tracking = ::TrackMouseEvent(&track) != FALSE;
        corner = entryCorner(pictureRect(), cursor);
    }
    const bool changed = corner != m_closeCorner;
    m_closeCorner = corner;
    const QRect button = closeButton();
    const bool hot = !button.isEmpty() && button.contains(cursor) && (!m_dragging || m_closePressed);
    if (changed || hot != m_closeHot) {
        m_closeHot = hot;
        render();
    }
}

void PinWindow::showHint(const QString& text, qint64 holdMs, bool stay)
{
    if (!m_hwnd)
        return;
    m_hint = text;
    m_hintStays = stay && !text.isEmpty();
    m_hintHoldMs = holdMs;
    m_hintSince = text.isEmpty() ? 0 : steadyNowMs();
    m_hintAlpha = text.isEmpty() ? 0 : 255;
    scheduleHintTimer();
}

void PinWindow::clearHint()
{
    m_hint.clear();
    m_hintStays = false;
    m_hintSince = 0;
    m_hintAlpha = 0;
    m_hintPill.hide();
    scheduleHintTimer();
}

void PinWindow::showPill(PillKind kind)
{
    if (!m_hwnd)
        return;
    m_pillKind = kind;
    if (kind == PillKind::None) {
        m_pillSince = 0;
        m_pillAlpha = 0;
        m_textPill.hide();
        scheduleHintTimer();
        return;
    }
    m_pillSince = steadyNowMs();
    m_pillAlpha = 255;
    scheduleHintTimer();
}

void PinWindow::clearPill()
{
    m_pillKind = PillKind::None;
    m_pillSince = 0;
    m_pillAlpha = 0;
    m_textPill.hide();
    scheduleHintTimer();
}

void PinWindow::dismissAutoReadyPill()
{
    if (m_pillKind != PillKind::AutoReady || m_pillAlpha <= 0)
        return;
    clearPill();
}

QString PinWindow::textPillText() const
{
    switch (m_pillKind) {
    case PillKind::None:
        return {};
    case PillKind::AutoReady:
        return QCoreApplication::translate("ws::capture::PinWindow", "已识别文字，可拖选");
    case PillKind::Reading:
        return QCoreApplication::translate("ws::capture::PinWindow", "识别文字中…");
    case PillKind::PickHint:
        return QCoreApplication::translate("ws::capture::PinWindow", "拖选文字 · Esc 退出");
    case PillKind::SelectionHint:
        return m_keys.has(KeyAction::PinCopy)
            ? QCoreApplication::translate("ws::capture::PinWindow", "%1 复制文字").arg(m_keys.text(KeyAction::PinCopy))
            : QCoreApplication::translate("ws::capture::PinWindow", "右键复制选中文字");
    case PillKind::Copied:
        return m_textMode ? QCoreApplication::translate("ws::capture::PinWindow", "已复制文字 · Esc 退出")
                          : QCoreApplication::translate("ws::capture::PinWindow", "已复制选中文字");
    case PillKind::CopiedAll:
        return QCoreApplication::translate("ws::capture::PinWindow", "已复制全部文字");
    case PillKind::NoText:
        return QCoreApplication::translate("ws::capture::PinWindow", "图片里没有找到文字");
    }
    return {};
}

qint64 PinWindow::pillHoldMs() const
{
    switch (m_pillKind) {
    case PillKind::AutoReady:
    case PillKind::NoText:
        return kAutoReadyHoldMs;
    case PillKind::SelectionHint:
        return kSelectionHintHoldMs;
    case PillKind::Copied:
        return m_textMode ? kPillHoldMs : kCopiedHoldMs;
    case PillKind::CopiedAll:
        return kCopiedHoldMs;
    case PillKind::PickHint:
    case PillKind::Reading:
    case PillKind::None:
        return kPillHoldMs;
    }
    return kPillHoldMs;
}

void PinWindow::scheduleHintTimer()
{
    if (!m_hwnd)
        return;
    const qint64 now = steadyNowMs();
    std::optional<qint64> nextMs;
    const auto consider = [&](bool active, bool stays, qint64 since, qint64 hold) {
        if (!active || stays || since <= 0)
            return;
        const qint64 age = std::max<qint64>(0, now - since);
        const qint64 wait = age < hold ? std::max<qint64>(kFadeTickMs, hold - age) : static_cast<qint64>(kFadeTickMs);
        nextMs = nextMs ? std::min(*nextMs, wait) : wait;
    };
    consider(m_hintAlpha > 0 && !m_hint.isEmpty(), m_hintStays, m_hintSince, m_hintHoldMs);
    consider(m_pillAlpha > 0 && m_pillKind != PillKind::None, m_pillKind == PillKind::Reading, m_pillSince, pillHoldMs());
    if (nextMs)
        ::SetTimer(m_hwnd, kHintTimer, static_cast<UINT>(std::clamp<qint64>(*nextMs, kFadeTickMs, 60'000)), nullptr);
    else
        ::KillTimer(m_hwnd, kHintTimer);
}

void PinWindow::hintTick()
{
    const qint64 now = steadyNowMs();
    const int hint = m_hintStays ? 255 : (m_hintSince && !m_hint.isEmpty()) ? fadedAlpha(now - m_hintSince, m_hintHoldMs) : 0;
    // The pill stays while the words are still being read: it says so.
    const int pill = m_pillKind == PillKind::Reading
        ? 255
        : (m_pillSince && m_pillKind != PillKind::None) ? fadedAlpha(now - m_pillSince, pillHoldMs()) : 0;
    if (hint != m_hintAlpha) {
        m_hintAlpha = hint;
        if (m_hintAlpha <= 0) {
            m_hintSince = 0;
            m_hintPill.hide();
        } else if (m_hintPill.visible()) {
            m_hintPill.setAlpha(m_hintAlpha);
        }
    }
    if (pill != m_pillAlpha) {
        m_pillAlpha = pill;
        if (m_pillAlpha <= 0) {
            m_pillSince = 0;
            if (m_pillKind != PillKind::Reading)
                m_pillKind = PillKind::None;
            m_textPill.hide();
        } else if (m_textPill.visible()) {
            m_textPill.setAlpha(m_pillAlpha);
        }
    }
    scheduleHintTimer();
}

void PinWindow::syncPills()
{
    if (!m_hwnd || !visibleNow() || m_form != PinForm::Full) {
        m_hintPill.hide();
        m_textPill.hide();
        return;
    }
    const QRect picture = pictureRect();
    const QRect bounds = pillBounds();
    const double scale = dpr();
    const int gap = std::max(2, static_cast<int>(std::lround(6 * scale)));
    const int inset = std::max(4, static_cast<int>(std::lround(8 * scale)));
    const QRect close = closeButton();
    const PinPill::Look look {
        m_host.accent ? m_host.accent() : QColor(0x0F, 0x6C, 0xBD),
        std::max(0.0, m_host.pillRadius ? m_host.pillRadius() : 4.0),
    };

    QRect hintRect;
    if (m_hintAlpha > 0 && !m_hint.isEmpty()) {
        const QSize size = PinPill::pillSize(m_hint, scale);
        const PinPillPlace place = placePinPill(picture, m_frame.border, size, bounds, PinPillEdge::Top, gap, inset, {}, close);
        hintRect = place.rect;
        m_hintPill.show(m_hwnd, m_topmost, m_hint, place.rect, scale, look, m_hintAlpha);
    } else {
        m_hintPill.hide();
    }

    const QString textLabel = m_pillAlpha > 0 && m_pillKind != PillKind::None && !m_picking ? textPillText() : QString();
    if (!textLabel.isEmpty()) {
        const QSize size = PinPill::pillSize(textLabel, scale);
        const PinPillPlace place = placePinPill(picture, m_frame.border, size, bounds, PinPillEdge::Bottom, gap, inset, hintRect, close);
        m_textPill.show(m_hwnd, m_topmost, textLabel, place.rect, scale, look, m_pillAlpha);
    } else {
        m_textPill.hide();
    }
}

void PinWindow::drawTextLayer(QImage& canvas, const QRect& picture)
{
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QColor accent = m_host.accent ? m_host.accent() : QColor(0x0F, 0x6C, 0xBD);
    const double z = m_zoom;
    // The lines picked: a veil of the accent over them, clipped to the picture.
    QColor veil = accent;
    veil.setAlpha(110);
    for (const PickedLine& line : std::as_const(m_picked)) {
        const QRect box = QRect(picture.left() + static_cast<int>(std::floor(line.box.left() * z)),
            picture.top() + static_cast<int>(std::floor(line.box.top() * z)),
            std::max(1, static_cast<int>(std::ceil(line.box.width() * z))),
            std::max(1, static_cast<int>(std::ceil(line.box.height() * z))))
            & picture;
        if (!box.isEmpty())
            painter.fillRect(box, veil);
    }
    if (m_picking && !m_pickDirect && !m_pickBand.isEmpty()) {
        const QRect band(picture.left() + static_cast<int>(std::lround(m_pickBand.left() * z)),
            picture.top() + static_cast<int>(std::lround(m_pickBand.top() * z)),
            static_cast<int>(std::lround(m_pickBand.width() * z)), static_cast<int>(std::lround(m_pickBand.height() * z)));
        painter.setPen(QPen(accent, std::max(1, static_cast<int>(std::lround(dpr())))));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(band.adjusted(0, 0, -1, -1));
    }
}

void PinWindow::paintBorder()
{
    if (!m_bits)
        return;
    QImage canvas(m_bits, m_dibSize.width(), m_dibSize.height(), m_dibSize.width() * 4, QImage::Format_ARGB32_Premultiplied);
    const bool dark = m_host.dark && m_host.dark();
    // Theme.windowEdge whether active or not: Win顺's edges are one grey,
    // never the accent (docs/colors.md); the shadow tells the pin apart.
    // Firmer while a click-through pin is held reachable by Ctrl (#59).
    // The firmer grey has no token: the same in every 界面风格.
    const QColor edge = m_host.edge ? m_host.edge() : QColor();
    const QColor color = m_reachable ? (dark ? QColor(0xA8, 0xA8, 0xA8) : QColor(0x70, 0x70, 0x70))
        : edge.isValid()             ? QColor(edge.red(), edge.green(), edge.blue()) // opaque: Source composition
                                     : (dark ? QColor(0x6A, 0x6A, 0x6A) : QColor(0xB4, 0xB4, 0xB4));
    const int s = m_frame.shadow;
    QPainter painter(&canvas);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    ring(painter, QRect(s, s, m_dibSize.width() - 2 * s, m_dibSize.height() - 2 * s), m_frame.border, color);
    painter.end();
    present();
}

void PinWindow::present()
{
    if (!m_hwnd || !m_dc)
        return;
    const QRect window = windowRect();
    POINT to {window.x(), window.y()};
    SIZE size {window.width(), window.height()};
    POINT from {0, 0};
    BLENDFUNCTION blend {AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255 * m_opacity)), AC_SRC_ALPHA};
    if (!::UpdateLayeredWindow(m_hwnd, nullptr, &to, &size, m_dc, &from, 0, &blend, ULW_ALPHA))
        qWarning() << "Screen capture: UpdateLayeredWindow failed, error" << ::GetLastError();
}

void PinWindow::setZoom(double zoom, QPoint cursor)
{
    if (m_form != PinForm::Full)
        return;
    zoom = std::clamp(zoom, minZoom(), maxZoom(fullSize(), kZoomBudget));
    if (m_pillKind == PillKind::AutoReady)
        clearPill();
    if (std::abs(zoom - m_zoom) >= 1e-9) {
        m_origin = zoomAbout(m_origin, cursor, m_zoom, zoom);
        m_zoom = zoom;
    }
    // The percent shows at the ends too, where the notch changes nothing.
    showHint(QString::number(static_cast<int>(std::lround(zoom * 100))) + u'%', kHintHoldMs);
    render();
}

void PinWindow::moveTo(QPoint origin)
{
    if (origin == m_origin || !m_hwnd)
        return;
    m_origin = origin; // first: a WM_DPICHANGED inside the move draws it there
    const QRect window = windowRect();
    ::SetWindowPos(m_hwnd, nullptr, window.x(), window.y(), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    syncPills();
}

void PinWindow::setPicture(const QImage& image, QByteArray project)
{
    if (!m_hwnd || image.isNull())
        return;
    if (m_form != PinForm::Full)
        expand();
    invalidateLines();
    m_image = image.convertToFormat(QImage::Format_RGB32);
    m_imageSize = m_image.size();
    m_scaled = {};
    m_project = std::move(project);
    m_copiedText.clear(); // edited: a picture now, copied as one
    forgetOtherSide();
    render();
    maybeStartAutoOcr();
}

bool PinWindow::setTranslation(const QImage& translated, const QString& words, int revision)
{
    if (!m_hwnd || m_form != PinForm::Full || m_image.isNull() || revision != m_revision || translated.size() != m_imageSize)
        return false;
    cancelPendingScan();
    if (m_readState == ReadState::Reading)
        m_readState = ReadState::Unread;
    m_copyAllWhenReady = false;
    m_autoScan = false;
    resetTextInteraction();
    // Keep the original side's cached lines so Tab back to it is instant.
    m_otherLines = std::move(m_lines);
    m_otherReadState = m_readState;
    m_lines.clear();
    m_readState = ReadState::Unread;
    m_other = std::exchange(m_image, translated.convertToFormat(QImage::Format_RGB32));
    m_translatedWords = words;
    m_translationShown = true;
    m_scaled = {};
    m_project.clear(); // the annotations are in the picture now, under the new words or beside them
    render();
    maybeStartAutoOcr();
    return true;
}

void PinWindow::showOtherSide()
{
    if (!m_hwnd || m_form != PinForm::Full || m_image.isNull() || m_other.isNull())
        return;
    cancelPendingScan();
    if (m_readState == ReadState::Reading)
        m_readState = ReadState::Unread;
    m_copyAllWhenReady = false;
    m_autoScan = false;
    resetTextInteraction();
    if (!m_hintStays && m_hintAlpha > 0)
        clearHint(); // dismiss a stale "Tab 看原文" pill once the user has already flipped
    std::swap(m_image, m_other);
    std::swap(m_lines, m_otherLines);
    std::swap(m_readState, m_otherReadState);
    m_translationShown = !m_translationShown;
    m_scaled = {};
    render();
    maybeStartAutoOcr();
    refreshCursor();
}

void PinWindow::forgetOtherSide()
{
    ++m_revision;
    m_other = {};
    m_otherLines.clear();
    m_otherReadState = ReadState::Unread;
    m_translatedWords.clear();
    m_translationShown = false;
    if (m_hintStays)
        clearHint(); // any in-flight translation for the old revision is now invalidated
}

void PinWindow::note(const QString& text, bool stay)
{
    if (!m_hwnd)
        return;
    if (text.isEmpty()) { // none: the one that stayed goes
        clearHint();
        return;
    }
    showHint(text, kNoteHoldMs, stay);
    syncPills();
}

void PinWindow::setShown(bool shown)
{
    m_shown = shown;
    applyVisibility();
}

void PinWindow::restyle()
{
    if (m_textMode || !m_picked.isEmpty()) {
        render(); // the text highlight and pills
    } else {
        paintBorder();
        syncPills(); // accent fill and corner radius
    }
}

void PinWindow::setAllHidden(bool hidden)
{
    m_allHidden = hidden;
    applyVisibility();
}

void PinWindow::setGroupHidden(bool hidden)
{
    if (m_groupHidden == hidden)
        return;
    m_groupHidden = hidden;
    applyVisibility();
}

void PinWindow::applyVisibility()
{
    if (m_hwnd)
        ::ShowWindow(m_hwnd, visibleNow() ? SW_SHOWNA : SW_HIDE);
    syncPills();
}

void PinWindow::setTopmost(bool on)
{
    m_topmost = on;
    if (m_hwnd) {
        ::SetWindowPos(m_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        m_hintPill.setTopmost(m_hwnd, on);
        m_textPill.setTopmost(m_hwnd, on);
    }
}

void PinWindow::setClickThrough(bool on)
{
    m_clickThrough = on;
    const bool was = m_reachable;
    if (!on)
        m_reachable = false;
    applyClickThrough();
    if (was != m_reachable)
        paintBorder(); // the cue goes with it
}

void PinWindow::applyClickThrough()
{
    if (!m_hwnd)
        return;
    const bool through = m_clickThrough && !m_reachable;
    LONG_PTR style = ::GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    const LONG_PTR wanted = through ? style | WS_EX_TRANSPARENT : style & ~LONG_PTR(WS_EX_TRANSPARENT);
    if (wanted == style)
        return;
    ::SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, wanted);
    ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

bool PinWindow::updateReachable(bool ctrlDown, QPoint cursor, bool menuUp)
{
    if (!m_hwnd || !m_clickThrough)
        return false;
    const bool over = pictureRect().contains(cursor);
    const bool reachable = !pinLetsMouseThrough(true, ctrlDown, over, m_dragging || menuUp);
    if (reachable == m_reachable)
        return false;
    m_reachable = reachable;
    qInfo() << "Screen capture: click-through pin" << (reachable ? "reachable (Ctrl)" : "click-through again");
    applyClickThrough();
    paintBorder(); // the cue: a firmer grey while it takes the mouse
    return true;
}

void PinWindow::close()
{
    if (m_hwnd)
        ::DestroyWindow(m_hwnd); // WM_NCDESTROY lets go of the pixels and tells the owner
}

// ---- turning and flipping ------------------------------------------------------------

void PinWindow::rotate(bool clockwise)
{
    if (!m_hwnd || m_form != PinForm::Full || m_image.isNull())
        return;
    invalidateLines();
    const QRect before = pictureRect();
    m_image = turnedImage(m_image, clockwise);
    m_imageSize = m_image.size();
    m_scaled = {};
    m_origin = recenteredOrigin(before, zoomedSize(m_imageSize, m_zoom));
    m_project.clear(); // the annotations were drawn on the other way up
    forgetOtherSide();
    render();
    maybeStartAutoOcr();
}

void PinWindow::flip(bool horizontal)
{
    if (!m_hwnd || m_form != PinForm::Full || m_image.isNull())
        return;
    invalidateLines();
    m_image = flippedImage(m_image, horizontal);
    m_scaled = {};
    m_project.clear();
    forgetOtherSide();
    render();
    maybeStartAutoOcr();
}

// ---- collapsing ------------------------------------------------------------------------------

void PinWindow::collapse(PinForm to)
{
    if (!m_hwnd || to == PinForm::Full || m_form != PinForm::Full || m_image.isNull())
        return;
    // Keep already-recognized lines across collapse/expand; only stop an active read/selection.
    if (m_readState == ReadState::Reading) {
        cancelPendingScan();
        m_readState = ReadState::Unread;
        m_autoScan = false;
        m_copyAllWhenReady = false;
    }
    resetTextInteraction();
    const QRect work = workArea();
    const QRect before = pictureRect();
    QRect tab;
    QImage thumb;
    if (to == PinForm::Tile) {
        const QSize size = tileThumbSize(m_imageSize, dpr());
        thumb = m_image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
        tab = tileRect(before.topLeft(), size, work);
    } else {
        const QSize size = edgeTabSize(dpr());
        const QImage fitted = m_image.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        thumb = fitted.copy((fitted.width() - size.width()) / 2, (fitted.height() - size.height()) / 2, size.width(), size.height())
                    .convertToFormat(QImage::Format_RGB32);
        tab = edgeTabRect(before.center().x(), size, work, m_host.takenTabs ? m_host.takenTabs(this) : std::vector<QRect> {});
    }
    m_savedOrigin = m_origin;
    m_savedZoom = m_zoom;
    m_form = to;
    m_thumb = thumb;
    m_scaled = {};
    m_origin = tab.topLeft();
    m_tileAtCollapse = m_origin;
    render(); // the window shrinks to the thumbnail: the big bitmap goes
    // The picture itself: its PNG bytes made on a thread of their own; once
    // they are there the pixels are let go of (kEncoded).
    auto job = std::make_shared<EncodeJob>();
    m_encode = job;
    const HWND hwnd = m_hwnd;
    std::thread([job, image = m_image, hwnd] {
        QBuffer buffer(&job->bytes);
        buffer.open(QIODevice::WriteOnly);
        // Quality 85 is zlib level 1: fast, and the bytes a fraction of the pixels.
        if (!image.save(&buffer, "PNG", 85))
            job->bytes.clear();
        if (!job->cancelled)
            ::PostMessageW(hwnd, kEncoded, 0, reinterpret_cast<LPARAM>(job.get()));
    }).detach();
}

void PinWindow::finishEncode(const EncodeJob* job)
{
    if (!m_encode || m_encode.get() != job || m_encode->cancelled || m_form == PinForm::Full)
        return;
    std::shared_ptr<EncodeJob> done = std::move(m_encode);
    m_encode.reset();
    if (done->bytes.isEmpty())
        return; // could not be packed: the pixels stay
    m_png = std::move(done->bytes);
    m_image = {};
    if (m_host.released)
        m_host.released(this);
}

void PinWindow::expand()
{
    if (!m_hwnd || m_form == PinForm::Full)
        return;
    if (m_encode) {
        m_encode->cancelled = true;
        m_encode.reset();
    }
    QImage full = m_image.isNull() ? image() : m_image;
    if (full.isNull())
        return; // the bytes do not decode: the thumbnail stays
    const QPoint origin = m_form == PinForm::Tile ? expandedOrigin(m_savedOrigin, m_tileAtCollapse, m_origin) : m_savedOrigin;
    m_image = std::move(full);
    m_png = {};
    m_thumb = {};
    m_form = PinForm::Full;
    m_origin = origin;
    m_zoom = m_savedZoom;
    m_scaled = {};
    render();
    maybeStartAutoOcr();
    refreshCursor();
}

// ---- picking text -----------------------------------------------------------------------------

QPoint PinWindow::toImage(QPoint screen) const
{
    const QPointF p = toImageF(screen);
    return QPoint(static_cast<int>(std::lround(p.x())), static_cast<int>(std::lround(p.y())));
}

QPointF PinWindow::toImageF(QPoint screen) const
{
    return QPointF(screen - m_origin) / m_zoom;
}

bool PinWindow::ocrAvailable() const
{
    return m_host.readLines != nullptr && (!m_host.ocrAvailable || m_host.ocrAvailable());
}

bool PinWindow::canDirectSelect() const
{
    return m_autoOcr && m_form == PinForm::Full && !m_textMode && !m_clickThrough
        && m_readState == ReadState::Ready && !m_lines.isEmpty();
}

double PinWindow::textHitPadding() const
{
    return std::clamp(2.0 / std::max(0.05, m_zoom), 1.0, 4.0);
}

bool PinWindow::hitText(QPoint screen) const
{
    return hasTextAt(m_lines, toImageF(screen), textHitPadding());
}

void PinWindow::refreshCursor()
{
    if (!m_hwnd || m_form != PinForm::Full)
        return;
    POINT pt {};
    if (!::GetCursorPos(&pt) || ::WindowFromPoint(pt) != m_hwnd)
        return;
    ::SendMessageW(m_hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(m_hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void PinWindow::cancelPendingScan()
{
    if (m_scanCancel) {
        m_scanCancel->store(true, std::memory_order_release);
        m_scanCancel.reset();
    }
}

void PinWindow::resetTextInteraction()
{
    m_textMode = false;
    m_picking = false;
    m_pickDirect = false;
    m_pickDragged = false;
    m_picked.clear();
    m_pickBand = {};
    clearPill();
    if (m_hwnd && ::GetCapture() == m_hwnd)
        ::ReleaseCapture();
}

void PinWindow::invalidateLines()
{
    cancelPendingScan();
    m_readState = ReadState::Unread;
    m_autoScan = false;
    m_copyAllWhenReady = false;
    m_lines.clear();
    resetTextInteraction();
}

void PinWindow::setAutoOcr(bool on)
{
    if (m_autoOcr == on)
        return;
    m_autoOcr = on;
    if (on) {
        maybeStartAutoOcr();
    } else {
        if (m_readState == ReadState::Reading && m_autoScan && !m_textMode && !m_copyAllWhenReady) {
            cancelPendingScan();
            m_readState = ReadState::Unread;
            m_autoScan = false;
        }
        if (!m_textMode && (!m_picked.isEmpty() || m_pillKind == PillKind::AutoReady || m_pillKind == PillKind::SelectionHint)) {
            m_picking = false;
            m_pickDirect = false;
            m_pickDragged = false;
            m_picked.clear();
            if (m_pillKind == PillKind::AutoReady || m_pillKind == PillKind::SelectionHint)
                clearPill();
            if (m_hwnd && ::GetCapture() == m_hwnd)
                ::ReleaseCapture();
            render();
        }
    }
    refreshCursor();
}

void PinWindow::setRecognizedLines(QList<RedactLine> lines, bool announce)
{
    cancelPendingScan();
    m_lines = std::move(lines);
    m_readState = ReadState::Ready;
    m_autoScan = false;
    if (std::exchange(m_copyAllWhenReady, false)) {
        copyAllRecognizedText();
        return;
    }
    if (m_textMode) {
        showPill(PillKind::PickHint);
        render();
    } else if (announce && !m_lines.isEmpty() && !m_dragging && !m_picking && m_form == PinForm::Full) {
        showPill(PillKind::AutoReady);
        render();
    }
    refreshCursor();
}

void PinWindow::maybeStartAutoOcr()
{
    if (!m_autoOcr || m_readState != ReadState::Unread || m_form != PinForm::Full || !m_hwnd || m_image.isNull()
        || !ocrAvailable())
        return;
    const qint64 pixels = static_cast<qint64>(m_image.width()) * m_image.height();
    if (m_image.width() < 16 || m_image.height() < 10 || pixels > kAutoOcrMaxPixels)
        return;
    startReadLines(true);
}

void PinWindow::startReadLines(bool background)
{
    if (!m_hwnd || m_form != PinForm::Full || m_image.isNull() || !ocrAvailable())
        return;
    cancelPendingScan();
    m_readState = ReadState::Reading;
    m_autoScan = background;
    m_scanCancel = std::make_shared<std::atomic<bool>>(false);
    if (!background) {
        showPill(PillKind::Reading); // 识别文字中…, for as long as that takes
        render();
    }
    const std::weak_ptr<bool> alive = m_alive;
    const std::shared_ptr<std::atomic<bool>> cancel = m_scanCancel;
    m_host.readLines(
        m_image, dpr(),
        [this, alive, cancel](QList<RedactLine> lines) {
            const auto token = alive.lock();
            if (!token || !*token || cancel->load(std::memory_order_acquire) || m_scanCancel != cancel)
                return;
            m_scanCancel.reset();
            m_lines = std::move(lines);
            m_readState = ReadState::Ready;
            const bool wasAuto = std::exchange(m_autoScan, false);
            if (std::exchange(m_copyAllWhenReady, false)) {
                const QString all = ws::capture::pickedText(allPickedLines(m_lines));
                if (!all.isEmpty() && m_host.copyText) {
                    m_host.copyText(all);
                    showPill(PillKind::CopiedAll);
                } else {
                    showPill(PillKind::NoText);
                }
                render();
                refreshCursor();
                return;
            }
            if (m_textMode) {
                showPill(PillKind::PickHint); // 拖选文字 · Esc 退出, for a moment
                render();
                refreshCursor();
                return;
            }
            if (wasAuto && m_autoOcr && !m_lines.isEmpty() && !m_dragging && !m_picking && m_form == PinForm::Full) {
                showPill(PillKind::AutoReady);
                render();
            }
            refreshCursor();
        },
        cancel);
}

void PinWindow::beginTextPick()
{
    if (!m_hwnd || m_form != PinForm::Full || m_textMode || m_image.isNull())
        return;
    if (m_readState == ReadState::Ready) {
        m_textMode = true;
        m_picked.clear();
        showPill(PillKind::PickHint);
        render();
        refreshCursor();
        return;
    }
    if (m_readState == ReadState::Reading) {
        m_textMode = true;
        m_autoScan = false; // promote background scan to interactive
        m_picked.clear();
        showPill(PillKind::Reading);
        render();
        refreshCursor();
        return;
    }
    if (!ocrAvailable())
        return;
    m_textMode = true;
    m_picked.clear();
    startReadLines(false);
}

void PinWindow::endTextPick()
{
    if (!m_textMode)
        return;
    m_textMode = false;
    m_picking = false;
    m_pickDirect = false;
    m_pickDragged = false;
    m_picked.clear();
    m_pickBand = {};
    if (m_readState == ReadState::Reading && !m_autoOcr && !m_copyAllWhenReady) {
        cancelPendingScan();
        m_readState = ReadState::Unread;
        m_autoScan = false;
    } else if (m_readState == ReadState::Reading && m_autoOcr && !m_copyAllWhenReady) {
        m_autoScan = true; // let the in-flight scan finish quietly for direct selection
    }
    if (!m_copyAllWhenReady)
        clearPill();
    if (m_hwnd && ::GetCapture() == m_hwnd)
        ::ReleaseCapture();
    render();
    refreshCursor();
}

void PinWindow::clearSelection()
{
    if (m_picked.isEmpty() && !m_picking)
        return;
    m_picking = false;
    m_pickDirect = false;
    m_pickDragged = false;
    m_picked.clear();
    m_pickBand = {};
    if (m_pillKind == PillKind::SelectionHint || m_pillKind == PillKind::AutoReady)
        clearPill();
    if (m_hwnd && ::GetCapture() == m_hwnd)
        ::ReleaseCapture();
    render();
}

void PinWindow::selectAllText()
{
    if (!m_hwnd || m_form != PinForm::Full || m_readState != ReadState::Ready || m_lines.isEmpty())
        return;
    m_picked = allPickedLines(m_lines);
    if (!m_picked.isEmpty()) {
        showPill(PillKind::SelectionHint);
        render();
    }
}

QString PinWindow::pickedText() const
{
    return ws::capture::pickedText(m_picked);
}

bool PinWindow::copyPickedText()
{
    const QString text = pickedText();
    if (text.isEmpty() || !m_host.copyText)
        return false;
    m_host.copyText(text);
    showPill(PillKind::Copied);
    render();
    return true;
}

bool PinWindow::copyAllRecognizedText()
{
    if (!m_hwnd || m_form != PinForm::Full || m_image.isNull() || !m_host.copyText)
        return false;
    if (m_readState == ReadState::Ready) {
        const QString all = ws::capture::pickedText(allPickedLines(m_lines));
        if (!all.isEmpty()) {
            m_host.copyText(all);
            showPill(PillKind::CopiedAll);
        } else {
            showPill(PillKind::NoText);
        }
        render();
        return true;
    }
    if (m_readState == ReadState::Reading) {
        m_copyAllWhenReady = true;
        m_autoScan = false;
        showPill(PillKind::Reading);
        render();
        return true;
    }
    if (!ocrAvailable())
        return false;
    m_copyAllWhenReady = true;
    startReadLines(false);
    return true;
}

void PinWindow::showCopiedAllPill()
{
    if (!m_hwnd || m_form != PinForm::Full)
        return;
    showPill(PillKind::CopiedAll);
    syncPills();
}

void PinWindow::updatePicked(QPoint cursor)
{
    const QPoint a = toImage(m_pickStart);
    const QPoint b = toImage(cursor);
    m_pickBand = QRect(QPoint(std::min(a.x(), b.x()), std::min(a.y(), b.y())), QPoint(std::max(a.x(), b.x()), std::max(a.y(), b.y())));
    m_picked = pickedLines(m_lines, QRectF(m_pickBand));
    render();
}

void PinWindow::updatePickedFlow(QPoint cursor)
{
    m_picked = pickedLinesFlow(m_lines, toImageF(m_pickStart), toImageF(cursor));
    render();
}

// ---- messages ------------------------------------------------------------------------------

LRESULT CALLBACK PinWindow::proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<PinWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self)
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    return self->handle(message, wParam, lParam);
}

LRESULT PinWindow::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    const HWND hwnd = m_hwnd;
    const auto cursorNow = [] {
        POINT cursor {};
        ::GetCursorPos(&cursor);
        return QPoint(cursor.x, cursor.y);
    };
    const int reach = std::max(4, static_cast<int>(std::lround(10 * dpr())));
    switch (message) {
    case WM_ACTIVATE:
        if (m_host.focus) {
            // lParam: the window the focus came from (or goes to); null for another thread's.
            const auto other = reinterpret_cast<HWND>(lParam);
            DWORD process = 0;
            if (other)
                ::GetWindowThreadProcessId(other, &process);
            m_host.focus(this, LOWORD(wParam) != WA_INACTIVE, other && process == ::GetCurrentProcessId());
        }
        return 0;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (m_host.input)
            m_host.input(this, PinInput::Press);
        break; // the menu comes with the button up (WM_CONTEXTMENU), the middle button's tile too
    case WM_RBUTTONUP:
        if (m_host.input)
            m_host.input(this, PinInput::Release);
        break;
    case kEncoded:
        finishEncode(reinterpret_cast<const EncodeJob*>(lParam));
        return 0;
    case WM_SETCURSOR: {
        if (LOWORD(lParam) != HTCLIENT)
            break;
        const QPoint at = cursorNow();
        LPCWSTR shape = nullptr;
        if (m_form != PinForm::Full) {
            shape = IDC_HAND;
        } else if (m_textMode) {
            shape = m_readState == ReadState::Ready ? IDC_IBEAM : IDC_ARROW;
        } else if (m_closePressed || (m_corner == Corner::None && closeButton().contains(at))) {
            shape = IDC_ARROW;
        } else if (m_picking) {
            shape = IDC_IBEAM;
        } else {
            const Corner corner = m_corner != Corner::None ? m_corner : cornerAt(pictureRect(), at, reach);
            if (corner == Corner::TopLeft || corner == Corner::BottomRight)
                shape = IDC_SIZENWSE;
            else if (corner == Corner::TopRight || corner == Corner::BottomLeft)
                shape = IDC_SIZENESW;
            else if (!m_dragging && canDirectSelect() && ::GetKeyState(VK_MENU) >= 0 && hitText(at))
                shape = IDC_IBEAM;
        }
        if (!shape)
            break;
        ::SetCursor(::LoadCursorW(nullptr, shape));
        return TRUE;
    }
    case WM_LBUTTONDOWN: {
        if (m_host.input)
            m_host.input(this, PinInput::Press);
        dismissAutoReadyPill();
        const QPoint at = cursorNow();
        m_press = at;
        m_moved = false;
        m_clickOpened = false;
        m_swallowUp = false;
        if (m_form == PinForm::Full && m_textMode && m_readState == ReadState::Ready) {
            m_picking = true;
            m_pickDirect = false;
            m_pickDragged = true;
            m_pickStart = at;
            m_picked.clear();
            updatePicked(at);
            ::SetCapture(hwnd);
            return 0;
        }
        if (closeButton().contains(at)) { // closes on the button up, if still on it
            m_closePressed = true;
            m_closeHot = true;
            render();
            ::SetCapture(hwnd);
            return 0;
        }
        if (m_form == PinForm::Full) {
            m_corner = cornerAt(pictureRect(), at, reach);
            if (m_corner == Corner::None && canDirectSelect() && ::GetKeyState(VK_MENU) >= 0 && hitText(at)) {
                m_picking = true;
                m_pickDirect = true;
                m_pickDragged = false;
                m_pickStart = at;
                if (!m_picked.isEmpty() || m_pillKind == PillKind::SelectionHint) {
                    m_picked.clear();
                    if (m_pillKind == PillKind::SelectionHint)
                        clearPill();
                    render();
                } else {
                    syncPills();
                }
                ::SetCapture(hwnd);
                return 0;
            }
            if (!m_picked.isEmpty())
                clearSelection();
            m_resizeStart = pictureRect();
        }
        m_grab = at - m_origin;
        m_dragging = true;
        ::SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        updateCloseHover(cursorNow());
        if (m_closePressed) {
            // The button stays down on the close button; nothing moves.
        } else if (m_picking && ::GetCapture() == hwnd) {
            const QPoint at = cursorNow();
            if (m_pickDirect) {
                if (!m_pickDragged) {
                    if ((at - m_pickStart).manhattanLength() <= std::max(3, static_cast<int>(std::lround(3 * dpr()))))
                        return 0;
                    m_pickDragged = true;
                }
                updatePickedFlow(at);
            } else {
                updatePicked(at);
            }
        } else if (m_dragging && ::GetCapture() == hwnd) {
            const QPoint at = cursorNow();
            if (!m_moved && (at - m_press).manhattanLength() > std::max(3, static_cast<int>(std::lround(3 * dpr()))))
                m_moved = true;
            if (m_form == PinForm::Edge) {
                // A tab sits on the edge: a click opens it.
            } else if (m_form == PinForm::Tile) {
                if (m_moved)
                    moveTo(at - m_grab);
            } else if (m_corner != Corner::None) {
                const Resized r = resizeFromCorner(m_corner, m_resizeStart, fullSize(), at, minZoom(),
                    maxZoom(fullSize(), kZoomBudget));
                if (r.origin != m_origin || std::abs(r.zoom - m_zoom) > 1e-9) {
                    m_origin = r.origin;
                    m_zoom = r.zoom;
                    render();
                }
            } else {
                moveTo(at - m_grab);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (m_host.input)
            m_host.input(this, PinInput::Release);
        if (m_closePressed) {
            const bool on = closeButton().contains(cursorNow());
            m_closePressed = false;
            if (::GetCapture() == hwnd)
                ::ReleaseCapture();
            if (on) {
                close(); // the window is gone: nothing more of this
                return 0;
            }
            m_closeHot = false;
            if (!m_tracking) // it left while the button was down
                m_closeCorner = Corner::None;
            render();
            return 0;
        }
        if (m_picking) {
            const bool direct = m_pickDirect;
            const bool dragged = m_pickDragged;
            m_picking = false;
            m_pickDirect = false;
            m_pickDragged = false;
            m_pickBand = {};
            if (::GetCapture() == hwnd)
                ::ReleaseCapture();
            if (direct) {
                if (dragged && !m_picked.isEmpty()) {
                    showPill(PillKind::SelectionHint);
                } else if (m_pillKind == PillKind::SelectionHint) {
                    clearPill();
                }
                render();
            } else {
                const QString text = pickedText();
                if (!text.isEmpty() && m_host.copyText) {
                    m_host.copyText(text);
                    showPill(PillKind::Copied);
                } else {
                    showPill(PillKind::PickHint);
                }
                render();
            }
            return 0;
        }
        m_dragging = false;
        m_corner = Corner::None;
        if (::GetCapture() == hwnd)
            ::ReleaseCapture();
        if (m_swallowUp) { // the double click's own button up: it collapsed the pin
            m_swallowUp = false;
        } else if (m_form != PinForm::Full && !m_moved) {
            expand();
            m_clickOpened = true;
        }
        return 0;
    case WM_MBUTTONUP:
        if (m_host.input)
            m_host.input(this, PinInput::Release);
        if (m_form == PinForm::Full)
            collapse(PinForm::Tile);
        else
            expand();
        return 0;
    case WM_MOUSELEAVE:
        m_tracking = false;
        if (m_closeCorner != Corner::None && !m_closePressed) {
            m_closeCorner = Corner::None;
            m_closeHot = false;
            render();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (m_closePressed) {
            m_closePressed = false;
            m_closeHot = false;
            render();
        }
        m_dragging = false;
        m_corner = Corner::None;
        if (m_picking) {
            m_picking = false;
            m_pickDirect = false;
            m_pickDragged = false;
            m_pickBand = {};
            render();
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        // A tile double-clicked opened on the first click: that is all it does.
        if (m_form != PinForm::Full || m_clickOpened)
            return 0;
        if (m_textMode) {
            if (m_readState == ReadState::Ready) {
                if (const auto line = pickedLineAt(m_lines, toImageF(cursorNow()), textHitPadding())) {
                    m_picked = {*line};
                    if (m_host.copyText) {
                        m_host.copyText(line->text);
                        showPill(PillKind::Copied);
                    }
                    render();
                }
            }
            return 0;
        }
        if (m_doubleClick == PinDoubleClick::None) {
            if (canDirectSelect()) {
                if (const auto line = pickedLineAt(m_lines, toImageF(cursorNow()), textHitPadding())) {
                    m_picked = {*line};
                    showPill(PillKind::SelectionHint);
                    render();
                }
            }
            return 0;
        }
        switch (m_doubleClick) {
        case PinDoubleClick::Reset: setZoom(m_fitZoom, cursorNow()); break; // the size it was pinned at
        case PinDoubleClick::Close: close(); break;
        case PinDoubleClick::Collapse:
            collapse(PinForm::Tile);
            m_swallowUp = true;
            break;
        case PinDoubleClick::None: break;
        }
        return 0;
    case WM_TIMER:
        if (wParam == kHintTimer)
            hintTick();
        return 0;
    case WM_MOUSEWHEEL: {
        if (m_host.input)
            m_host.input(this, PinInput::Key);
        m_wheel += GET_WHEEL_DELTA_WPARAM(wParam);
        const int steps = m_wheel / WHEEL_DELTA;
        m_wheel -= steps * WHEEL_DELTA;
        if (steps == 0)
            return 0;
        if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
            m_opacity = nextOpacity(m_opacity, steps);
            present();
            if (m_form == PinForm::Full) {
                showHint(QCoreApplication::translate("ws::capture::PinWindow", "透明度 %1%")
                             .arg(static_cast<int>(std::lround(m_opacity * 100))),
                    kHintHoldMs);
                syncPills();
            }
        } else if (m_form == PinForm::Full) {
            const QPoint cursor(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); // screen, physical
            setZoom(nextZoom(m_zoom, steps, maxZoom(fullSize(), kZoomBudget), minZoom()), cursor);
        }
        return 0;
    }
    case WM_SYSKEYUP:
    case WM_KEYUP:
        if (wParam == VK_MENU)
            refreshCursor();
        break;
    case WM_SYSKEYDOWN: // Alt chords and F10 come here; what is not ours goes to Windows (Alt+F4)
    case WM_KEYDOWN: {
        if (wParam == VK_MENU)
            refreshCursor();
        const bool ctrl = ::GetKeyState(VK_CONTROL) < 0;
        const bool shift = ::GetKeyState(VK_SHIFT) < 0;
        const bool alt = ::GetKeyState(VK_MENU) < 0;
        const QKeyCombination pressed = CaptureKeys::fromVirtualKey(static_cast<unsigned>(wParam), ctrl, shift, alt);
        const auto action = m_keys.triggered(KeyContext::PinWindow, pressed.key(), pressed.keyboardModifiers());
        // ? or F1 shows or hides the key card (#343), unless F1 is one of the user's keys here.
        if (!action && !ctrl && !alt && (wParam == VK_F1 ? !shift : typesQuestionMark(static_cast<UINT>(wParam), lParam))) {
            if (m_host.toggleHints)
                m_host.toggleHints(this);
            return 0;
        }
        // Any other key takes the card away; Shift, Ctrl and Alt alone do not (Shift+/ is ?, Ctrl+wheel the opacity).
        if (wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_MENU) {
            dismissAutoReadyPill();
            if (m_host.input)
                m_host.input(this, PinInput::Key);
        }
        if (action) {
            switch (*action) {
            case KeyAction::PinClose:
                if (m_textMode)
                    endTextPick();
                else if (!m_picked.isEmpty())
                    clearSelection();
                else
                    close();
                break;
            case KeyAction::PinCopy:
                if (!m_picked.isEmpty() && m_host.copyText)
                    copyPickedText();
                else if (m_host.copy)
                    m_host.copy(this);
                break;
            case KeyAction::PinSave:
                if (m_host.save)
                    m_host.save(this);
                break;
            case KeyAction::PinRotateRight: rotate(true); break;
            case KeyAction::PinRotateLeft: rotate(false); break;
            case KeyAction::PinFlipHorizontal: flip(true); break;
            case KeyAction::PinFlipVertical: flip(false); break;
            case KeyAction::PinTextPick:
                if (m_textMode)
                    endTextPick();
                else if (!m_picked.isEmpty() && m_host.copyText)
                    copyPickedText();
                else
                    beginTextPick();
                break;
            case KeyAction::PinEdit: // as the menu: a whole picture only
                if (m_form == PinForm::Full && m_host.edit)
                    m_host.edit(this);
                break;
            case KeyAction::PinOcr:
                if (m_form == PinForm::Full && m_host.ocr)
                    m_host.ocr(this);
                break;
            case KeyAction::PinOcrCopy:
                if (m_form == PinForm::Full && m_host.ocrCopy)
                    m_host.ocrCopy(this);
                break;
            case KeyAction::PinCollapseEdge:
                if (m_form == PinForm::Full)
                    collapse(PinForm::Edge);
                else
                    expand();
                break;
            case KeyAction::PinCollapse:
                if (m_form == PinForm::Full)
                    collapse(PinForm::Tile);
                else
                    expand();
                break;
            default:
                break;
            }
            return 0;
        }
        // Esc exits text-pick mode or clears direct selection whatever key closes the pin
        // (unless the user gave Esc to another action above).
        if (wParam == VK_ESCAPE && !ctrl && !shift && !alt) {
            if (m_textMode) {
                endTextPick();
                return 0;
            }
            if (!m_picked.isEmpty()) {
                clearSelection();
                return 0;
            }
        }
        if (message == WM_SYSKEYDOWN)
            break;
        if (ctrl && !shift && !alt && wParam == 'A' && m_form == PinForm::Full
            && m_readState == ReadState::Ready && !m_lines.isEmpty()) {
            selectAllText();
            return 0;
        }
        if (ctrl)
            break;
        if (wParam == VK_TAB && !shift && hasOtherSide()) { // the original under its translation, and back
            showOtherSide();
            return 0;
        }
        const int step = shift ? 10 : 1;
        switch (wParam) {
        case VK_LEFT:
            moveTo(m_origin + QPoint(-step, 0));
            return 0;
        case VK_RIGHT:
            moveTo(m_origin + QPoint(step, 0));
            return 0;
        case VK_UP:
            moveTo(m_origin + QPoint(0, -step));
            return 0;
        case VK_DOWN:
            moveTo(m_origin + QPoint(0, step));
            return 0;
        case VK_OEM_PLUS:
        case VK_ADD:
            if (m_form == PinForm::Full)
                setZoom(nextZoom(m_zoom, 1, maxZoom(fullSize(), kZoomBudget), minZoom()), pictureRect().center());
            return 0;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            if (m_form == PinForm::Full)
                setZoom(nextZoom(m_zoom, -1, maxZoom(fullSize(), kZoomBudget), minZoom()), pictureRect().center());
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_CONTEXTMENU: {
        POINT at {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (at.x == -1 && at.y == -1) { // the menu key: by the picture's top left
            const QRect picture = pictureRect();
            at = {picture.x() + 8, picture.y() + 8};
        }
        if (m_host.menu)
            m_host.menu(this, at);
        return 0;
    }
    case WM_DPICHANGED:
        // Onto a monitor at another scale: the picture keeps its pixels and
        // its place, the border and shadow follow; not the system's rectangle.
        m_dpi = std::max<UINT>(96, HIWORD(wParam));
        m_frame = pinFrame(dpr());
        render();
        return 0;
    case WM_NCDESTROY: {
        m_hintPill.destroy();
        m_textPill.destroy();
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        *m_alive = false;
        cancelPendingScan();
        if (m_encode) {
            m_encode->cancelled = true;
            m_encode.reset();
        }
        if (m_host.closing)
            m_host.closing(this);
        releasePixels(); // at once: the memory goes back now
        if (m_host.closed)
            m_host.closed(this);
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace ws::capture

#include "PinWindow.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFont>
#include <QGuiApplication>
#include <QPainter>

#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace ws::capture {

namespace {

const wchar_t* const kClassName = L"WinShun.Pin";
constexpr UINT kEncoded = WM_APP + 1; // the PNG bytes of a collapsed pin are made (lParam: the job)
constexpr UINT_PTR kHintTimer = 1;
constexpr qint64 kHintHoldMs = 900; // the zoom stays this long after the last notch ...
constexpr qint64 kHintFadeMs = 240; // ... then fades out in this

void registerClass()
{
    static const bool registered = [] {
        WNDCLASSEXW wc {sizeof wc};
        wc.style = CS_DBLCLKS; // a double click: back to 100%
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

} // namespace

PinWindow::PinWindow(const QImage& image, QPoint origin, Host host)
    : m_host(std::move(host))
    , m_image(image.convertToFormat(QImage::Format_RGB32))
    , m_imageSize(image.size())
    , m_origin(origin)
{
    registerClass();
    // Per-monitor v2 whatever the thread's default: the picture must stay
    // in physical pixels, and WM_DPICHANGED must come.
    const DPI_AWARENESS_CONTEXT previous = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    m_hwnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"Win顺 贴图", WS_POPUP,
        origin.x(), origin.y(), 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
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
    if (m_text != Text::Off)
        drawTextLayer(canvas, picture);
    if (m_hintAlpha > 0 && m_form == PinForm::Full)
        drawHint(canvas, picture);
    paintBorder();
}

void PinWindow::drawHint(QImage& canvas, const QRect& picture)
{
    QFont font = QGuiApplication::font();
    font.setPixelSize(static_cast<int>(std::lround(13 * dpr())));
    const QFontMetrics metrics(font);
    const int padX = static_cast<int>(std::lround(10 * dpr()));
    const int padY = static_cast<int>(std::lround(4 * dpr()));
    const int gap = static_cast<int>(std::lround(8 * dpr()));
    const QSize size(metrics.horizontalAdvance(m_hint) + 2 * padX, metrics.height() + 2 * padY);
    if (picture.width() < size.width() + 2 * gap || picture.height() < size.height() + 2 * gap)
        return; // a pin smaller than its own label: none
    // Upper left: the lower left is the text pick's.
    const QRect pill(picture.left() + gap, picture.top() + gap, size.width(), size.height());
    QPainter painter(&canvas);
    painter.setOpacity(m_hintAlpha / 255.0);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_host.accent ? m_host.accent() : QColor(0x0F, 0x6C, 0xBD));
    painter.drawRoundedRect(pill, 4 * dpr(), 4 * dpr());
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(pill, Qt::AlignCenter, m_hint);
}

void PinWindow::showHint(const QString& text)
{
    if (!m_hwnd)
        return;
    m_hint = text;
    m_hintSince = QDateTime::currentMSecsSinceEpoch();
    m_hintAlpha = 255;
    ::SetTimer(m_hwnd, kHintTimer, 40, nullptr); // restarts the count while it runs
}

void PinWindow::hintTick()
{
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - m_hintSince;
    if (age < kHintHoldMs)
        return;
    const int alpha = age >= kHintHoldMs + kHintFadeMs
        ? 0
        : static_cast<int>(255 * (kHintHoldMs + kHintFadeMs - age) / kHintFadeMs);
    if (alpha <= 0)
        ::KillTimer(m_hwnd, kHintTimer);
    if (alpha != m_hintAlpha) {
        m_hintAlpha = std::max(0, alpha);
        render(); // at 0 the picture is as it was: at 100% its own pixels
    }
}

void PinWindow::drawTextLayer(QImage& canvas, const QRect& picture)
{
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QColor accent = m_host.accent ? m_host.accent() : QColor(0x0F, 0x6C, 0xBD);
    const double z = m_zoom;
    // The lines picked: a veil of the accent over them.
    QColor veil = accent;
    veil.setAlpha(110);
    for (const PickedLine& line : std::as_const(m_picked)) {
        const QRect box(picture.left() + static_cast<int>(std::floor(line.box.left() * z)),
            picture.top() + static_cast<int>(std::floor(line.box.top() * z)),
            std::max(1, static_cast<int>(std::ceil(line.box.width() * z))),
            std::max(1, static_cast<int>(std::ceil(line.box.height() * z))));
        painter.fillRect(box, veil);
    }
    if (m_picking && !m_pickBand.isEmpty()) {
        const QRect band(picture.left() + static_cast<int>(std::lround(m_pickBand.left() * z)),
            picture.top() + static_cast<int>(std::lround(m_pickBand.top() * z)),
            static_cast<int>(std::lround(m_pickBand.width() * z)), static_cast<int>(std::lround(m_pickBand.height() * z)));
        painter.setPen(QPen(accent, std::max(1, static_cast<int>(std::lround(dpr())))));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(band.adjusted(0, 0, -1, -1));
    }
    // What the pin is doing, in a pill at the lower left (white on the accent fill).
    if (picture.width() < 150 * dpr() || picture.height() < 44 * dpr())
        return;
    const QString text = m_text == Text::Reading
        ? QCoreApplication::translate("ws::capture::PinWindow", "识别文字中…")
        : m_picked.isEmpty() ? QCoreApplication::translate("ws::capture::PinWindow", "拖选文字 · Esc 退出")
                             : QCoreApplication::translate("ws::capture::PinWindow", "已复制文字 · Esc 退出");
    QFont font = QGuiApplication::font();
    font.setPixelSize(static_cast<int>(std::lround(13 * dpr())));
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const int padX = static_cast<int>(std::lround(10 * dpr()));
    const int padY = static_cast<int>(std::lround(4 * dpr()));
    const int gap = static_cast<int>(std::lround(8 * dpr()));
    const QSize size(metrics.horizontalAdvance(text) + 2 * padX, metrics.height() + 2 * padY);
    const QRect pill(picture.left() + gap, picture.bottom() + 1 - gap - size.height(), size.width(), size.height());
    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    const qreal radius = std::max(0.0, m_host.pillRadius ? m_host.pillRadius() : 4.0) * dpr();
    painter.drawRoundedRect(pill, radius, radius);
    painter.setPen(Qt::white);
    painter.drawText(pill, Qt::AlignCenter, text);
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
    zoom = std::clamp(zoom, zoomSteps().front(), maxZoom(fullSize(), kZoomBudget));
    // The percent shows at the ends too, where the notch changes nothing.
    showHint(QString::number(static_cast<int>(std::lround(zoom * 100))) + u'%');
    if (std::abs(zoom - m_zoom) >= 1e-9) {
        m_origin = zoomAbout(m_origin, cursor, m_zoom, zoom);
        m_zoom = zoom;
    }
    render();
}

void PinWindow::moveTo(QPoint origin)
{
    if (origin == m_origin || !m_hwnd)
        return;
    m_origin = origin; // first: a WM_DPICHANGED inside the move draws it there
    const QRect window = windowRect();
    ::SetWindowPos(m_hwnd, nullptr, window.x(), window.y(), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void PinWindow::setPicture(const QImage& image, QByteArray project)
{
    if (!m_hwnd || image.isNull())
        return;
    if (m_form != PinForm::Full)
        expand();
    endTextPick();
    m_image = image.convertToFormat(QImage::Format_RGB32);
    m_imageSize = m_image.size();
    m_scaled = {};
    m_project = std::move(project);
    render();
}

void PinWindow::setShown(bool shown)
{
    m_shown = shown;
    applyVisibility();
}

void PinWindow::restyle()
{
    if (m_text != Text::Off)
        render(); // the text pill too
    else
        paintBorder();
}

void PinWindow::setAllHidden(bool hidden)
{
    m_allHidden = hidden;
    applyVisibility();
}

void PinWindow::applyVisibility()
{
    if (m_hwnd)
        ::ShowWindow(m_hwnd, visibleNow() ? SW_SHOWNA : SW_HIDE);
}

void PinWindow::setTopmost(bool on)
{
    m_topmost = on;
    if (m_hwnd)
        ::SetWindowPos(m_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
    endTextPick();
    const QRect before = pictureRect();
    m_image = turnedImage(m_image, clockwise);
    m_imageSize = m_image.size();
    m_scaled = {};
    m_origin = recenteredOrigin(before, zoomedSize(m_imageSize, m_zoom));
    m_project.clear(); // the annotations were drawn on the other way up
    render();
}

void PinWindow::flip(bool horizontal)
{
    if (!m_hwnd || m_form != PinForm::Full || m_image.isNull())
        return;
    endTextPick();
    m_image = flippedImage(m_image, horizontal);
    m_scaled = {};
    m_project.clear();
    render();
}

// ---- collapsing ------------------------------------------------------------------------------

void PinWindow::collapse(PinForm to)
{
    if (!m_hwnd || to == PinForm::Full || m_form != PinForm::Full || m_image.isNull())
        return;
    endTextPick();
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
}

// ---- picking text -----------------------------------------------------------------------------

QPoint PinWindow::toImage(QPoint screen) const
{
    const QPointF p = QPointF(screen - m_origin) / m_zoom;
    return QPoint(static_cast<int>(std::lround(p.x())), static_cast<int>(std::lround(p.y())));
}

void PinWindow::beginTextPick()
{
    if (!m_hwnd || m_form != PinForm::Full || m_text != Text::Off || !m_host.readLines || m_image.isNull())
        return;
    m_text = Text::Reading;
    m_lines.clear();
    m_picked.clear();
    render();
    const std::weak_ptr<bool> alive = m_alive;
    m_host.readLines(m_image, dpr(), [this, alive](QList<RedactLine> lines) {
        const auto token = alive.lock();
        if (!token || !*token || m_text != Text::Reading)
            return;
        m_lines = std::move(lines);
        m_text = Text::Ready;
        render();
    });
}

void PinWindow::endTextPick()
{
    if (m_text == Text::Off)
        return;
    m_text = Text::Off;
    m_picking = false;
    m_lines.clear();
    m_picked.clear();
    m_pickBand = {};
    if (m_hwnd && ::GetCapture() == m_hwnd)
        ::ReleaseCapture();
    render();
}

QString PinWindow::pickedText() const
{
    return ws::capture::pickedText(m_picked);
}

void PinWindow::updatePicked(QPoint cursor)
{
    const QPoint a = toImage(m_pickStart);
    const QPoint b = toImage(cursor);
    m_pickBand = QRect(QPoint(std::min(a.x(), b.x()), std::min(a.y(), b.y())), QPoint(std::max(a.x(), b.x()), std::max(a.y(), b.y())));
    m_picked = pickedLines(m_lines, QRectF(m_pickBand));
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
        return 0;
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
        } else if (m_text == Text::Ready) {
            shape = IDC_IBEAM;
        } else if (m_text == Text::Off || m_corner != Corner::None) {
            const Corner corner = m_corner != Corner::None ? m_corner : cornerAt(pictureRect(), at, reach);
            if (corner == Corner::TopLeft || corner == Corner::BottomRight)
                shape = IDC_SIZENWSE;
            else if (corner == Corner::TopRight || corner == Corner::BottomLeft)
                shape = IDC_SIZENESW;
        }
        if (!shape)
            break;
        ::SetCursor(::LoadCursorW(nullptr, shape));
        return TRUE;
    }
    case WM_LBUTTONDOWN: {
        const QPoint at = cursorNow();
        m_press = at;
        m_moved = false;
        m_clickOpened = false;
        m_swallowUp = false;
        if (m_form == PinForm::Full && m_text == Text::Ready) {
            m_picking = true;
            m_pickStart = at;
            m_picked.clear();
            updatePicked(at);
            ::SetCapture(hwnd);
            return 0;
        }
        if (m_form == PinForm::Full) {
            m_corner = cornerAt(pictureRect(), at, reach);
            m_resizeStart = pictureRect();
        }
        m_grab = at - m_origin;
        m_dragging = true;
        ::SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (m_picking && ::GetCapture() == hwnd) {
            updatePicked(cursorNow());
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
                const Resized r = resizeFromCorner(m_corner, m_resizeStart, fullSize(), at, zoomSteps().front(),
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
        if (m_picking) {
            m_picking = false;
            m_pickBand = {};
            const QString text = pickedText();
            if (!text.isEmpty() && m_host.copyText)
                m_host.copyText(text);
            render();
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
        if (m_form == PinForm::Full)
            collapse(PinForm::Tile);
        else
            expand();
        return 0;
    case WM_CAPTURECHANGED:
        m_dragging = false;
        m_corner = Corner::None;
        if (m_picking) {
            m_picking = false;
            m_pickBand = {};
            render();
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        // A tile double-clicked opened on the first click: that is all it does.
        if (m_form != PinForm::Full || m_text != Text::Off || m_clickOpened)
            return 0;
        switch (m_doubleClick) {
        case PinDoubleClick::Reset: setZoom(1.0, cursorNow()); break;
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
        m_wheel += GET_WHEEL_DELTA_WPARAM(wParam);
        const int steps = m_wheel / WHEEL_DELTA;
        m_wheel -= steps * WHEEL_DELTA;
        if (steps == 0)
            return 0;
        if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
            m_opacity = nextOpacity(m_opacity, steps);
            if (m_form == PinForm::Full) {
                showHint(QCoreApplication::translate("ws::capture::PinWindow", "透明度 %1%")
                             .arg(static_cast<int>(std::lround(m_opacity * 100))));
                render();
            } else {
                present();
            }
        } else if (m_form == PinForm::Full) {
            const QPoint cursor(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); // screen, physical
            setZoom(nextZoom(m_zoom, steps, maxZoom(fullSize(), kZoomBudget)), cursor);
        }
        return 0;
    }
    case WM_SYSKEYDOWN: // Alt chords and F10 come here; what is not ours goes to Windows (Alt+F4)
    case WM_KEYDOWN: {
        const bool ctrl = ::GetKeyState(VK_CONTROL) < 0;
        const bool shift = ::GetKeyState(VK_SHIFT) < 0;
        const bool alt = ::GetKeyState(VK_MENU) < 0;
        const QKeyCombination pressed = CaptureKeys::fromVirtualKey(static_cast<unsigned>(wParam), ctrl, shift, alt);
        const auto action = m_keys.triggered(KeyContext::PinWindow, pressed.key(), pressed.keyboardModifiers());
        if (action) {
            switch (*action) {
            case KeyAction::PinClose:
                if (m_text != Text::Off)
                    endTextPick();
                else
                    close();
                break;
            case KeyAction::PinCopy:
                if (m_text == Text::Ready && !m_picked.isEmpty() && m_host.copyText)
                    m_host.copyText(pickedText());
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
                if (m_text == Text::Off)
                    beginTextPick();
                else
                    endTextPick();
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
        // Esc is the way out of picking text whatever key closes the pin (unless
        // the user gave Esc to another action above).
        if (wParam == VK_ESCAPE && !ctrl && !shift && !alt && m_text != Text::Off) {
            endTextPick();
            return 0;
        }
        if (message == WM_SYSKEYDOWN)
            break;
        if (ctrl)
            break;
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
                setZoom(nextZoom(m_zoom, 1, maxZoom(fullSize(), kZoomBudget)), pictureRect().center());
            return 0;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            if (m_form == PinForm::Full)
                setZoom(nextZoom(m_zoom, -1, maxZoom(fullSize(), kZoomBudget)), pictureRect().center());
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
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        *m_alive = false;
        if (m_encode) {
            m_encode->cancelled = true;
            m_encode.reset();
        }
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

#include "PinPill.h"

#include <QDebug>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <optional>

namespace ws::capture {

namespace {

const wchar_t* const kClassName = L"WinShun.PinPill";

int px(double logical, double dpr)
{
    return static_cast<int>(std::lround(logical * dpr));
}

int clampStart(int start, int length, int lower, int upperExclusive)
{
    return std::max(lower, std::min(start, upperExclusive - length));
}

bool clearOf(const QRect& candidate, const QRect& obstacle, int spacing)
{
    if (obstacle.isEmpty())
        return true;
    return !candidate.intersects(obstacle.adjusted(-spacing, -spacing, spacing, spacing));
}

QFont pillFont(double dpr)
{
    QFont font = QGuiApplication::font();
    font.setPixelSize(std::max(1, px(13, dpr)));
    return font;
}

} // namespace

PinPillPlace placePinPill(const QRect& picture, int border, QSize pill, const QRect& bounds, PinPillEdge preferred,
    int gap, int inset, const QRect& avoid, const QRect& closeButton)
{
    if (picture.isEmpty() || pill.isEmpty() || bounds.isEmpty())
        return {};
    const int b = std::max(0, border);
    const int g = std::max(0, gap);
    const int in = std::max(0, inset);
    const QRect anchor = picture.adjusted(-b, -b, b, b);
    const int boundsRight = bounds.left() + bounds.width();
    const int boundsBottom = bounds.top() + bounds.height();
    const int anchorRight = anchor.left() + anchor.width();
    const int anchorBottom = anchor.top() + anchor.height();

    const auto tryHorizontalBand = [&](int y) -> std::optional<QRect> {
        if (y < bounds.top() || y + pill.height() > boundsBottom)
            return std::nullopt;
        const int x0 = clampStart(anchor.left(), pill.width(), bounds.left(), boundsRight);
        const QRect primary(x0, y, pill.width(), pill.height());
        if (clearOf(primary, avoid, g))
            return primary;
        const int xRight = avoid.left() + avoid.width() + g;
        if (xRight + pill.width() <= boundsRight)
            return QRect(xRight, y, pill.width(), pill.height());
        const int xLeft = avoid.left() - g - pill.width();
        if (xLeft >= bounds.left())
            return QRect(xLeft, y, pill.width(), pill.height());
        return std::nullopt;
    };

    const auto tryVerticalBand = [&](int x) -> std::optional<QRect> {
        if (x < bounds.left() || x + pill.width() > boundsRight)
            return std::nullopt;
        const int wantedY = preferred == PinPillEdge::Top ? anchor.top() : anchorBottom - pill.height();
        const int y0 = clampStart(wantedY, pill.height(), bounds.top(), boundsBottom);
        const QRect primary(x, y0, pill.width(), pill.height());
        if (clearOf(primary, avoid, g))
            return primary;
        const int yBelow = avoid.top() + avoid.height() + g;
        const int yAbove = avoid.top() - g - pill.height();
        if (preferred == PinPillEdge::Top) {
            if (yBelow + pill.height() <= boundsBottom)
                return QRect(x, yBelow, pill.width(), pill.height());
            if (yAbove >= bounds.top())
                return QRect(x, yAbove, pill.width(), pill.height());
        } else {
            if (yAbove >= bounds.top())
                return QRect(x, yAbove, pill.width(), pill.height());
            if (yBelow + pill.height() <= boundsBottom)
                return QRect(x, yBelow, pill.width(), pill.height());
        }
        return std::nullopt;
    };

    const int aboveY = anchor.top() - g - pill.height();
    const int belowY = anchorBottom + g;
    const int rightX = anchorRight + g;
    const int leftX = anchor.left() - g - pill.width();

    if (preferred == PinPillEdge::Top) {
        if (const auto r = tryHorizontalBand(aboveY))
            return {*r, false};
        if (const auto r = tryHorizontalBand(belowY))
            return {*r, false};
    } else {
        if (const auto r = tryHorizontalBand(belowY))
            return {*r, false};
        if (const auto r = tryHorizontalBand(aboveY))
            return {*r, false};
    }
    if (const auto r = tryVerticalBand(rightX))
        return {*r, false};
    if (const auto r = tryVerticalBand(leftX))
        return {*r, false};

    // No room on any outer edge (e.g. a full-screen or oversized zoomed pin):
    // fall back inside the visible portion of the picture, clearing the close
    // button and any other active pill.
    const QRect visible = picture.intersected(bounds);
    const QRect area = visible.isEmpty() ? bounds : visible;
    const int areaBottom = area.top() + area.height();
    const int y = preferred == PinPillEdge::Top
        ? clampStart(area.top() + in, pill.height(), bounds.top(), boundsBottom)
        : clampStart(areaBottom - in - pill.height(), pill.height(), bounds.top(), boundsBottom);
    int x = clampStart(area.left() + in, pill.width(), bounds.left(), boundsRight);
    QRect inside(x, y, pill.width(), pill.height());
    if (!clearOf(inside, closeButton, in)) {
        x = clampStart(closeButton.left() + closeButton.width() + in, pill.width(), bounds.left(), boundsRight);
        inside.moveLeft(x);
    }
    if (!clearOf(inside, avoid, g)) {
        const int shifted = avoid.left() + avoid.width() + g;
        if (shifted + pill.width() <= boundsRight)
            inside.moveLeft(shifted);
    }
    return {inside, true};
}

int PinPill::shadowMargin(double dpr)
{
    return std::max(3, px(6, dpr));
}

QSize PinPill::pillSize(const QString& text, double dpr)
{
    if (text.isEmpty())
        return {};
    const QFontMetrics metrics(pillFont(dpr));
    const int padX = std::max(4, px(10, dpr));
    const int padY = std::max(2, px(4, dpr));
    return QSize(metrics.horizontalAdvance(text) + 2 * padX, metrics.height() + 2 * padY);
}

QImage PinPill::picture(const QString& text, double dpr, const Look& look)
{
    const QSize body = pillSize(text, dpr);
    if (body.isEmpty())
        return {};
    const int s = shadowMargin(dpr);
    QImage canvas(body.width() + 2 * s, body.height() + 2 * s, QImage::Format_ARGB32_Premultiplied);
    paint(canvas, text, dpr, look);
    return canvas;
}

void PinPill::paint(QImage& canvas, const QString& text, double dpr, const Look& look)
{
    const int s = shadowMargin(dpr);
    const QRect body(s, s, canvas.width() - 2 * s, canvas.height() - 2 * s);
    const qreal radius = std::clamp(std::max<qreal>(0.0, look.radius) * dpr, 0.0, body.height() / 2.0);
    const int drop = std::min(s - 1, std::max(1, px(1, dpr)));
    const int rings = std::max(1, s - drop);
    const int edge = std::max(1, px(1, dpr));
    const qreal half = edge / 2.0;

    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // Soft floating drop shadow, strictly inside the shadow margin so it never clips.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setPen(Qt::NoPen);
    for (int i = rings; i >= 1; --i) {
        const double f = static_cast<double>(rings - i + 1) / (rings + 1);
        painter.setBrush(QColor(0, 0, 0, static_cast<int>(std::lround(56 * f * f))));
        painter.drawRoundedRect(QRectF(body).adjusted(-i, -i + drop, i, i + drop), radius + i, radius + i);
    }

    // Solid accent pill body (docs/colors.md: accentFill with white text).
    const QColor fill = look.fill.isValid() ? QColor(look.fill.red(), look.fill.green(), look.fill.blue()) : QColor(0x0F, 0x6C, 0xBD);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(body), radius, radius);

    // Subtle 1px inner specular rim so dark accent pills remain distinct over dark windows.
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 36), edge));
    painter.drawRoundedRect(QRectF(body).adjusted(half, half, -half, -half),
        std::max<qreal>(0.0, radius - half), std::max<qreal>(0.0, radius - half));

    painter.setFont(pillFont(dpr));
    painter.setPen(Qt::white);
    painter.drawText(body, Qt::AlignCenter, text);
}

PinPill::~PinPill()
{
    destroy();
}

void PinPill::destroy()
{
    if (m_hwnd) {
        ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_owner = nullptr;
    m_body = {};
    m_windowRect = {};
    m_text.clear();
    m_dpr = 0.0;
    m_alpha = 0;
    releaseSurface();
}

void PinPill::releaseSurface()
{
    if (m_dc) {
        ::SelectObject(m_dc, m_oldBitmap);
        ::DeleteObject(m_bitmap);
        ::DeleteDC(m_dc);
        m_dc = nullptr;
        m_bitmap = nullptr;
        m_oldBitmap = nullptr;
    }
    m_bits = nullptr;
    m_dibSize = {};
}

bool PinPill::ensureWindow(HWND owner, bool topmost)
{
    if (m_hwnd && m_owner == owner)
        return true;
    if (m_hwnd)
        destroy();
    if (!owner)
        return false;
    static const bool registered = [] {
        WNDCLASSEXW wc {sizeof wc};
        wc.lpfnWndProc = &PinPill::proc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        return ::RegisterClassExW(&wc) != 0;
    }();
    Q_UNUSED(registered);
    const DPI_AWARENESS_CONTEXT previous = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
        | (topmost ? WS_EX_TOPMOST : 0);
    m_hwnd = ::CreateWindowExW(exStyle, kClassName, L"Win顺 贴图提示", WS_POPUP, 0, 0, 1, 1,
        owner, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (previous)
        ::SetThreadDpiAwarenessContext(previous);
    if (!m_hwnd) {
        qWarning() << "Screen capture: no pin pill window, error" << ::GetLastError();
        return false;
    }
    m_owner = owner;
    ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

bool PinPill::ensureSurface(QSize windowSize)
{
    if (m_dc && m_dibSize == windowSize)
        return true;
    if (m_dc) {
        ::SelectObject(m_dc, m_oldBitmap);
        ::DeleteObject(m_bitmap);
        m_bitmap = nullptr;
        m_oldBitmap = nullptr;
    } else {
        m_dc = ::CreateCompatibleDC(nullptr);
        if (!m_dc)
            return false;
    }
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = windowSize.width();
    info.bmiHeader.biHeight = -windowSize.height(); // top-down, as QImage
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    m_bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!m_bitmap) {
        ::DeleteDC(m_dc);
        m_dc = nullptr;
        m_bits = nullptr;
        m_dibSize = {};
        return false;
    }
    m_oldBitmap = ::SelectObject(m_dc, m_bitmap);
    m_bits = static_cast<uchar*>(bits);
    m_dibSize = windowSize;
    return true;
}

void PinPill::show(HWND owner, bool topmost, const QString& text, const QRect& body, double dpr, const Look& look, int alpha)
{
    const int clampedAlpha = std::clamp(alpha, 0, 255);
    if (!owner || text.isEmpty() || body.isEmpty() || clampedAlpha <= 0) {
        hide();
        return;
    }
    if (!ensureWindow(owner, topmost))
        return;
    const int s = shadowMargin(dpr);
    const QRect windowRect = body.adjusted(-s, -s, s, s);
    const bool needsPaint = !m_dc || m_dibSize != windowRect.size() || m_text != text
        || std::abs(m_dpr - dpr) > 1e-6 || m_look != look;
    if (needsPaint) {
        if (!ensureSurface(windowRect.size())) {
            hide();
            return;
        }
        QImage canvas(m_bits, m_dibSize.width(), m_dibSize.height(), m_dibSize.width() * 4,
            QImage::Format_ARGB32_Premultiplied);
        paint(canvas, text, dpr, look);
        m_text = text;
        m_dpr = dpr;
        m_look = look;
    }
    const bool wasVisible = visible();
    const bool moved = windowRect.topLeft() != m_windowRect.topLeft();
    const bool alphaChanged = clampedAlpha != m_alpha;
    m_body = body;
    m_windowRect = windowRect;
    if (needsPaint || alphaChanged || !wasVisible) {
        present(clampedAlpha);
    } else if (moved) {
        ::SetWindowPos(m_hwnd, nullptr, m_windowRect.x(), m_windowRect.y(), 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!wasVisible) {
        ::ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        syncZOrder(owner, topmost);
    }
}

void PinPill::setAlpha(int alpha)
{
    const int clamped = std::clamp(alpha, 0, 255);
    if (clamped <= 0) {
        hide();
        return;
    }
    if (!visible() || !m_dc || clamped == m_alpha)
        return;
    present(clamped);
}

void PinPill::setTopmost(HWND owner, bool topmost)
{
    if (!m_hwnd || !owner)
        return;
    syncZOrder(owner, topmost);
}

void PinPill::syncZOrder(HWND owner, bool topmost)
{
    if (!m_hwnd || !owner)
        return;
    if (topmost) {
        ::SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    // Place in the non-topmost band immediately above the owner without
    // jumping over unrelated windows covering the owner.
    HWND insertAfter = ::GetWindow(owner, GW_HWNDPREV);
    while (insertAfter == m_hwnd)
        insertAfter = ::GetWindow(insertAfter, GW_HWNDPREV);
    if (!insertAfter || (::GetWindowLongPtrW(insertAfter, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
        insertAfter = HWND_NOTOPMOST;
    ::SetWindowPos(m_hwnd, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void PinPill::present(int alpha)
{
    if (!m_hwnd || !m_dc)
        return;
    m_alpha = std::clamp(alpha, 0, 255);
    POINT to {m_windowRect.x(), m_windowRect.y()};
    SIZE size {m_windowRect.width(), m_windowRect.height()};
    POINT from {0, 0};
    BLENDFUNCTION blend {AC_SRC_OVER, 0, static_cast<BYTE>(m_alpha), AC_SRC_ALPHA};
    if (!::UpdateLayeredWindow(m_hwnd, nullptr, &to, &size, m_dc, &from, 0, &blend, ULW_ALPHA))
        qWarning() << "Screen capture: pin pill UpdateLayeredWindow failed, error" << ::GetLastError();
}

void PinPill::hide()
{
    m_body = {};
    m_alpha = 0;
    if (m_hwnd && ::IsWindowVisible(m_hwnd))
        ::ShowWindow(m_hwnd, SW_HIDE);
}

bool PinPill::visible() const
{
    return m_hwnd && ::IsWindowVisible(m_hwnd);
}

LRESULT CALLBACK PinPill::proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (message == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (message == WM_NCDESTROY) {
        if (auto* self = reinterpret_cast<PinPill*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->m_hwnd = nullptr;
            self->m_owner = nullptr;
            self->m_body = {};
            self->m_alpha = 0;
        }
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace ws::capture

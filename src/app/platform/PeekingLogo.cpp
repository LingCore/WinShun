#include "PeekingLogo.h"

#include <QImage>
#include <QTransform>

#include <cmath>
#include <cstring>

namespace ws::win {

namespace {

using Edge = PeekingLogo::Edge;

constexpr wchar_t kClassName[] = L"WinShun.PeekingLogo";
constexpr wchar_t kAppIconResource[] = L"IDI_ICON1"; // see app.rc
constexpr int kLargest = 256; // in app.ico

// Clockwise, in degrees: the leaf (the top right pane) points out of the
// edge, straight away from the window. Over the top edge it stays upright,
// the leaf up and to the right.
int angleFor(Edge edge)
{
    switch (edge) {
    case Edge::Top:
        return 0;
    case Edge::Bottom:
        return 135; // the leaf down
    case Edge::Left:
        return 225; // left
    case Edge::Right:
        return 45; // right
    }
    return 0;
}

// Across the logo's bitmap, `size` the logo's own: turned 45°, a diamond as
// wide as the square's diagonal.
int extentFor(int size, Edge edge)
{
    return angleFor(edge) % 90 == 0 ? size : qRound(size * std::sqrt(2.0));
}

int shownPart(int extent) // across the edge: three quarters
{
    return extent - extent / 4;
}

// Where the logo's window goes for that edge.
RECT rectFor(Edge edge, const RECT& host, const RECT& row, int extent, int inset)
{
    const int shown = shownPart(extent);
    const int x = row.left + inset;
    const int y = (row.top + row.bottom - extent) / 2;
    switch (edge) {
    case Edge::Top:
        return {x, host.top - shown, x + extent, host.top};
    case Edge::Bottom:
        return {x, host.bottom, x + extent, host.bottom + shown};
    case Edge::Left:
        return {host.left - shown, y, host.left, y + extent};
    case Edge::Right:
        return {host.right, y, host.right + shown, y + extent};
    }
    return {};
}

bool inside(const RECT& r, const RECT& area)
{
    return r.left >= area.left && r.top >= area.top && r.right <= area.right && r.bottom <= area.bottom;
}

// The icon of that size from the exe, premultiplied; Windows scales the
// nearest one should app.ico not have it.
QImage loadIcon(int size)
{
    const auto icon = static_cast<HICON>(
        ::LoadImageW(::GetModuleHandleW(nullptr), kAppIconResource, IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    if (!icon)
        return {};
    QImage image = QImage::fromHICON(icon).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    ::DestroyIcon(icon);
    if (!image.isNull() && image.width() != size)
        image = image.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return image;
}

HWND createWindow()
{
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    static const bool registered = [instance] {
        WNDCLASSEXW wc {sizeof wc};
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = kClassName;
        return ::RegisterClassExW(&wc) != 0;
    }();
    if (!registered)
        return nullptr;
    // Layered with per-pixel alpha, and transparent to the mouse as a whole:
    // a click on it goes to the window below (the dialog).
    return ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
}

} // namespace

PeekingLogo::~PeekingLogo()
{
    if (m_dc) {
        ::SelectObject(m_dc, m_dcBitmap);
        ::DeleteDC(m_dc);
    }
    if (m_bitmap)
        ::DeleteObject(m_bitmap);
    if (m_hwnd)
        ::DestroyWindow(m_hwnd);
}

bool PeekingLogo::render(int size, Edge edge)
{
    const int angle = angleFor(edge);
    const int extent = extentFor(size, edge);
    QImage turned;
    if (angle % 90 == 0) {
        // The icon drawn for this size, turned by whole quarters: pixels
        // moved, none blurred.
        turned = loadIcon(size);
        if (angle != 0 && !turned.isNull())
            turned = turned.transformed(QTransform().rotate(angle));
    } else {
        // Askew no edge can lie on the pixel grid: the largest icon turned,
        // then made small, each pixel the average of those it covers.
        const QImage large = loadIcon(kLargest);
        if (!large.isNull())
            turned = large.transformed(QTransform().rotate(angle), Qt::SmoothTransformation)
                         .scaled(extent, extent, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (turned.isNull())
        return false;
    // The part that shows; the rest is behind the window.
    const int shown = shownPart(extent);
    const QImage part = edge == Edge::Top ? turned.copy(0, 0, extent, shown)
        : edge == Edge::Bottom            ? turned.copy(0, extent - shown, extent, shown)
        : edge == Edge::Left              ? turned.copy(0, 0, shown, extent)
                                          : turned.copy(extent - shown, 0, shown, extent);
    // Premultiplied BGRA, top-down, as UpdateLayeredWindow takes it.
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = part.width();
    info.bmiHeader.biHeight = -part.height();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap)
        return false;
    const qsizetype stride = qsizetype(part.width()) * 4;
    for (int y = 0; y < part.height(); ++y)
        std::memcpy(static_cast<uchar*>(bits) + y * stride, part.constScanLine(y), size_t(stride));
    if (!m_dc) {
        m_dc = ::CreateCompatibleDC(nullptr);
        m_dcBitmap = ::SelectObject(m_dc, bitmap);
    } else {
        ::SelectObject(m_dc, bitmap);
        ::DeleteObject(m_bitmap);
    }
    m_bitmap = bitmap;
    m_size = size;
    m_edge = edge;
    return true;
}

void PeekingLogo::show(const RECT& host, const RECT& row, const RECT& work, const RECT* avoid, qreal scale,
    std::initializer_list<Edge> edges)
{
    const int size = qRound(kSize * scale);
    const int inset = qRound(kInset * scale);
    const Edge* chosen = nullptr;
    for (const bool clear : {true, false}) {
        for (const Edge& edge : edges) {
            const RECT r = rectFor(edge, host, row, extentFor(size, edge), inset);
            RECT overlap {};
            if (inside(r, work) && (!clear || !avoid || !::IntersectRect(&overlap, &r, avoid))) {
                chosen = &edge;
                break;
            }
        }
        if (chosen)
            break;
    }
    if (!chosen) {
        hide();
        return;
    }
    if (!m_hwnd && !(m_hwnd = createWindow()))
        return;
    if ((size != m_size || *chosen != m_edge || !m_bitmap) && !render(size, *chosen))
        return;
    const RECT r = rectFor(*chosen, host, row, extentFor(size, *chosen), inset);
    POINT at {r.left, r.top};
    SIZE extent {r.right - r.left, r.bottom - r.top};
    POINT from {0, 0};
    BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    ::UpdateLayeredWindow(m_hwnd, nullptr, &at, &extent, m_dc, &from, 0, &blend, ULW_ALPHA);
    if (!::IsWindowVisible(m_hwnd))
        ::ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
}

void PeekingLogo::hide()
{
    if (m_hwnd)
        ::ShowWindow(m_hwnd, SW_HIDE);
}

} // namespace ws::win

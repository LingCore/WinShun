#include "CountdownWindow.h"

#include <QDebug>
#include <QFont>
#include <QGuiApplication>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace ws::capture {

namespace {

const wchar_t* const kClassName = L"WinShun.Countdown";

void registerClass()
{
    static const bool registered = [] {
        WNDCLASSEXW wc {sizeof wc};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) { return ::DefWindowProcW(h, m, w, l); };
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = kClassName;
        return ::RegisterClassExW(&wc) != 0;
    }();
    Q_UNUSED(registered);
}

} // namespace

CountdownWindow::CountdownWindow(const QRect& work, const QColor& fill, std::function<void()> clicked)
    : m_clicked(std::move(clicked))
    , m_fill(fill.isValid() ? fill : QColor(0x0F, 0x6C, 0xBD))
    , m_work(work)
{
    registerClass();
    const DPI_AWARENESS_CONTEXT previous = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    m_hwnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClassName,
        L"Win顺 延时截图", WS_POPUP, work.right() - 10, work.top() + 10, 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr),
        nullptr);
    if (previous)
        ::SetThreadDpiAwarenessContext(previous);
    if (!m_hwnd) {
        qWarning() << "Screen capture: no countdown window, error" << ::GetLastError();
        return;
    }
    ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ::SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CountdownWindow::proc));
    m_dpr = std::max<UINT>(96, ::GetDpiForWindow(m_hwnd)) / 96.0;
}

CountdownWindow::~CountdownWindow()
{
    if (m_hwnd) {
        ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(m_hwnd);
    }
}

void CountdownWindow::setSeconds(int seconds)
{
    if (seconds == m_seconds || !m_hwnd)
        return;
    m_seconds = seconds;
    draw();
}

void CountdownWindow::draw()
{
    const int w = static_cast<int>(std::lround(172 * m_dpr));
    const int h = static_cast<int>(std::lround(64 * m_dpr));
    const int inset = static_cast<int>(std::lround(16 * m_dpr));
    m_rect = QRect(m_work.right() + 1 - inset - w, m_work.top() + inset, w, h);

    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib)
        return;
    HDC dc = ::CreateCompatibleDC(nullptr);
    HGDIOBJ old = ::SelectObject(dc, dib);
    {
        QImage canvas(static_cast<uchar*>(bits), w, h, w * 4, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_fill);
        const double radius = 8 * m_dpr;
        painter.drawRoundedRect(QRectF(0, 0, w, h), radius, radius);
        painter.setPen(Qt::white);
        QFont big = QGuiApplication::font();
        big.setPixelSize(static_cast<int>(std::lround(34 * m_dpr)));
        big.setWeight(QFont::Bold);
        painter.setFont(big);
        painter.drawText(QRectF(0, 0, 64 * m_dpr, h), Qt::AlignCenter, QString::number(std::max(0, m_seconds)));
        QFont small = QGuiApplication::font();
        small.setPixelSize(static_cast<int>(std::lround(14 * m_dpr)));
        painter.setFont(small);
        painter.drawText(QRectF(64 * m_dpr, 0, w - 72 * m_dpr, h), Qt::AlignVCenter | Qt::AlignLeft,
            QCoreApplication::translate("ws::capture::CountdownWindow", "秒后截图\n点这里取消"));
    }
    POINT to {m_rect.x(), m_rect.y()};
    SIZE size {w, h};
    POINT from {0, 0};
    BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!::UpdateLayeredWindow(m_hwnd, nullptr, &to, &size, dc, &from, 0, &blend, ULW_ALPHA))
        qWarning() << "Screen capture: countdown UpdateLayeredWindow failed, error" << ::GetLastError();
    else if (!::IsWindowVisible(m_hwnd))
        ::ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    ::SelectObject(dc, old);
    ::DeleteDC(dc);
    ::DeleteObject(dib);
}

LRESULT CALLBACK CountdownWindow::proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<CountdownWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (self && message == WM_LBUTTONUP) {
        if (self->m_clicked)
            self->m_clicked();
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace ws::capture

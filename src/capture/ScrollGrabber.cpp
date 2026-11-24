#include "ScrollGrabber.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace ws::capture {

namespace {

constexpr int kPollMs = 16; // BitBlt's frames: about the screen's

} // namespace

bool samePixels(const QImage& a, const QImage& b, const QRect& area)
{
    if (a.size() != b.size() || a.depth() != 32 || b.depth() != 32)
        return false;
    const QRect full(0, 0, a.width(), a.height());
    const QRect box = area.isEmpty() ? full : full.intersected(area);
    if (box.isEmpty())
        return true;
    const size_t offset = size_t(box.x()) * 4;
    const size_t bytes = size_t(box.width()) * 4;
    for (int y = box.top(); y <= box.bottom(); ++y)
        if (std::memcmp(a.constScanLine(y) + offset, b.constScanLine(y) + offset, bytes) != 0)
            return false;
    return true;
}

// Persistent GDI capture resources for the non-WGC fallback path (e.g. Win10
// without borderless WGC): avoids per-frame CreateDIBSection churn and omits
// CAPTUREBLT so 60 Hz polling does not flicker the mouse pointer.
struct ScrollGrabber::GdiBuffer {
    HDC screen = nullptr;
    HDC memory = nullptr;
    HBITMAP section = nullptr;
    HGDIOBJ previous = nullptr;
    const uint32_t* bits = nullptr;
    QSize size;

    explicit GdiBuffer(QSize extent)
        : size(extent)
    {
        if (extent.isEmpty())
            return;
        screen = ::GetDC(nullptr);
        if (!screen)
            return;
        memory = ::CreateCompatibleDC(screen);
        if (!memory)
            return;
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = extent.width();
        info.bmiHeader.biHeight = -extent.height(); // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* raw = nullptr;
        section = ::CreateDIBSection(memory, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
        if (!section || !raw)
            return;
        previous = ::SelectObject(memory, section);
        if (!previous || previous == HGDI_ERROR) {
            previous = nullptr;
            return;
        }
        bits = static_cast<const uint32_t*>(raw);
    }

    ~GdiBuffer()
    {
        if (memory) {
            if (previous)
                ::SelectObject(memory, previous);
            ::DeleteDC(memory);
        }
        if (section)
            ::DeleteObject(section);
        if (screen)
            ::ReleaseDC(nullptr, screen);
    }

    GdiBuffer(const GdiBuffer&) = delete;
    GdiBuffer& operator=(const GdiBuffer&) = delete;

    bool ready() const { return screen && memory && section && previous && bits; }
};

ScrollGrabber::ScrollGrabber(Grabber& grabber, const QRect& region, int ignoreRight, bool horizontal)
    : m_region(region)
    , m_ignoreRight(std::max(0, ignoreRight))
    , m_horizontal(horizontal)
{
    m_clock.start();
    m_stream = grabber.stream(region);
    if (!m_stream)
        m_gdi = std::make_unique<GdiBuffer>(region.size());
}

ScrollGrabber::~ScrollGrabber() = default;

void ScrollGrabber::setOrientation(bool horizontal)
{
    m_horizontal = horizontal;
}

QRect ScrollGrabber::compareRect() const
{
    const int w = m_region.width();
    const int h = m_region.height();
    if (m_ignoreRight <= 0)
        return QRect(0, 0, w, h);
    if (m_horizontal) {
        const int keepH = std::max(std::min(h, 32), h - m_ignoreRight);
        return QRect(0, 0, w, keepH);
    }
    const int keepW = std::max(std::min(w, 32), w - m_ignoreRight);
    return QRect(0, 0, keepW, h);
}

QImage ScrollGrabber::bitblt()
{
    if (!m_gdi || !m_gdi->ready())
        return {};
    const QRect& r = m_region;
    if (!::BitBlt(m_gdi->memory, 0, 0, r.width(), r.height(), m_gdi->screen, r.x(), r.y(), SRCCOPY))
        return {};
    ::GdiFlush();
    QImage image(r.width(), r.height(), QImage::Format_RGB32);
    if (image.isNull())
        return {};
    const int w = r.width();
    for (int y = 0; y < r.height(); ++y) {
        const uint32_t* src = m_gdi->bits + size_t(y) * size_t(w);
        auto* dst = reinterpret_cast<uint32_t*>(image.scanLine(y));
        for (int x = 0; x < w; ++x)
            dst[x] = src[x] | 0xFF000000u; // GDI leaves the alpha byte 0
    }
    return image;
}

QImage ScrollGrabber::changed(int timeoutMs)
{
    const qint64 until = m_clock.elapsed() + std::max(0, timeoutMs);
    const QRect compare = compareRect();
    for (;;) {
        const qint64 now = m_clock.elapsed();
        QImage frame;
        if (m_stream) {
            const Grabber::Stream::Frame f = m_stream->next(m_number, int(std::max<qint64>(0, until - now)));
            if (f.number <= m_number)
                return {}; // none came in time (or the stream ended)
            m_number = f.number;
            m_lastFrameMs = m_clock.elapsed();
            frame = f.image;
        } else {
            if (now < m_nextPollMs) {
                if (m_nextPollMs > until)
                    return {};
                ::Sleep(DWORD(m_nextPollMs - now));
            }
            m_nextPollMs = m_clock.elapsed() + kPollMs;
            frame = bitblt();
            if (!frame.isNull())
                m_lastFrameMs = m_clock.elapsed();
        }
        if (!frame.isNull() && !samePixels(frame, m_last, compare)) {
            m_last = frame;
            return frame;
        }
        if (m_clock.elapsed() >= until)
            return {};
    }
}

} // namespace ws::capture

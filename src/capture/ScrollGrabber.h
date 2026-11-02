#pragma once

#include "Grabber.h"

#include <QElapsedTimer>
#include <QImage>
#include <QRect>

#include <memory>

// The long screenshot's frames (docs/design-capture-roadmap.md, 4.2, 取帧):
// the region's pixels through the grabber's stream (WGC: its resident
// device and the monitor's capture item; a frame only when something on
// that monitor changed, the newest kept), or where WGC cannot, BitBlt every
// 16 ms. Both leave out windows set to WDA_EXCLUDEFROMCAPTURE (the long
// screenshot's panel and preview, while they cover the region; stage 6
// probe), and read nothing outside the region. One thread's.
namespace ws::capture {

class ScrollGrabber {
public:
    // `region`: physical pixels, virtual desktop, inside one monitor.
    ScrollGrabber(Grabber& grabber, const QRect& region);
    ~ScrollGrabber();

    ScrollGrabber(const ScrollGrabber&) = delete;
    ScrollGrabber& operator=(const ScrollGrabber&) = delete;

    bool wgc() const { return m_stream != nullptr; }
    bool hdr() const { return m_stream && m_stream->hdr(); }
    // The next frame whose pixels differ from the last one given, waiting
    // up to `timeoutMs`; a null image when none came. The first call gives
    // the first frame.
    QImage changed(int timeoutMs);
    // The last frame given.
    const QImage& last() const { return m_last; }
    // Milliseconds since the start when the last frame of any kind came
    // (changed or not); -1 none yet. For the watchdog.
    qint64 lastFrameMs() const { return m_lastFrameMs; }
    qint64 elapsed() const { return m_clock.elapsed(); }

private:
    QImage bitblt() const;

    QRect m_region;
    std::unique_ptr<Grabber::Stream> m_stream;
    quint64 m_number = 0;
    QImage m_last;
    QElapsedTimer m_clock;
    qint64 m_lastFrameMs = -1;
    qint64 m_nextPollMs = 0;
};

// Whether two frames have the same pixels (same size, 32 bits a pixel).
bool samePixels(const QImage& a, const QImage& b);

} // namespace ws::capture

#pragma once

#include <QImage>
#include <QRect>
#include <QString>

#include "Hdr.h"

#include <windows.h>

#include <memory>
#include <vector>

namespace ws::capture {

// Pictures of every monitor, as they are on the screen (docs/design-capture.md, 4.1).
//
// Windows.Graphics.Capture (WGC) first: it gets HDR monitors right (BitBlt
// returns them washed out) and, kept warm, is the faster path (stage 0:
// 11–12 ms per 2560x1600 monitor against 30–33 ms for BitBlt). Warm means
// the D3D11 device (120+ ms the first time in a process) and one capture
// item per monitor (CreateForMonitor, 22–26 ms each) made ahead, when the
// capture is turned on and again when the monitors change; each picture
// then only needs a frame pool and a session. No yellow border: an
// unpackaged program is allowed borderless capture
// (GraphicsCaptureAccess::RequestAccessAsync(Borderless) answers Allowed),
// then IsBorderRequired(false). BitBlt is the way out when WGC fails.
//
// Everything WGC runs on a thread of its own (multi-threaded apartment),
// which also does the preparing: turning the capture on never holds up the
// GUI. grab() waits for its pictures.
class Grabber {
public:
    struct Shot {
        HMONITOR monitor = nullptr;
        QRect rect; // physical pixels, virtual desktop
        QImage image; // the monitor's pixels, Format_RGB32
        bool wgc = false; // else BitBlt
        bool hdr = false; // mapped from scRGB (Hdr.h)
        // As captured, when asked for (grab's `keepHdr`) and `hdr`: for an
        // HDR file beside the picture. Shared: a 4K monitor's are 66 MB.
        std::shared_ptr<HdrPixels> hdrPixels;
    };

    Grabber();
    ~Grabber();

    Grabber(const Grabber&) = delete;
    Grabber& operator=(const Grabber&) = delete;

    // One region's pixels as they change (the long screenshot,
    // ScrollGrabber): WGC gives a frame only when something on that
    // monitor changed; the newest is kept and read back on the capture
    // thread when next() asks for it, cut to the region on the GPU, without
    // the mouse pointer. Destroying it ends it.
    class Stream {
    public:
        struct Frame {
            QImage image; // the region's pixels, Format_RGB32 (HDR mapped as grab() maps it)
            quint64 number = 0; // which of the frames WGC gave since the start
        };
        virtual ~Stream() = default;
        // The newest frame after frame `after`, waiting up to `timeoutMs` for
        // one to come; a null image when none came.
        virtual Frame next(quint64 after, int timeoutMs) = 0;
        virtual bool hdr() const = 0;
    };

    // The monitors changed: their capture items are made again.
    void refresh();
    // A picture of every monitor; blocks until they are taken. `report`
    // (may be null) tells how, for the log; `keepHdr`: an HDR monitor's
    // frame as captured too (Shot::hdrPixels).
    //
    // WINSHUN_TEST_HDR_CAPTURE set: every monitor is taken as an HDR one
    // (half floats, SDR white at 80 nits), so that an SDR screen tries the
    // HDR path; what comes out looks the same.
    std::vector<Shot> grab(QString* report = nullptr, bool keepHdr = false);
    // A stream of `region` (physical pixels, virtual desktop, inside one
    // monitor); null when WGC cannot (no device, no capture item for that
    // monitor). It is the capture thread's job while it lasts: grab() and
    // refresh() wait behind it, so it must end before either is needed.
    std::unique_ptr<Stream> stream(const QRect& region);

private:
    struct Worker;
    std::unique_ptr<Worker> m_worker;
};

} // namespace ws::capture

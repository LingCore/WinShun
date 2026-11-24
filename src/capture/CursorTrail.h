#pragma once

#include <QPointF>
#include <QRgb>

#include <deque>
#include <span>
#include <vector>

class QImage;

namespace ws::capture::recording {

// The recording's pointer trail: a line that follows the pointer and draws
// back into it when the pointer stops (the "Line" trail of macOS's MaCursor,
// made again here). Only geometry: where the pointer was over the last
// `lifetime`, in the recorded region's pixels, becomes a smoothed polyline.
// Each of its points says how young it is (life: 1 at the pointer, 0 where
// the line is about to end) and where it sits along the line (place: 0 at
// the tail, 1 at the pointer), which the drawing turns into width, opacity
// and colour (PointerOverlay).
class CursorTrail {
public:
    static constexpr qint64 lifetime = 5'000'000; // half a second, in 100 ns
    struct Point {
        QPointF at;
        float life = 0;
        float place = 0;
    };

    // The pointer at a time (the media clock, pauses left out). A move under
    // half a pixel keeps the earlier time: the line starts to shrink when
    // the pointer stops, not when it is next looked at.
    void move(QPointF tip, qint64 time);
    void clear();
    // The line at `time`, from the tail to the pointer. Empty when the
    // pointer has been still for `lifetime`, or has not moved yet.
    std::vector<Point> line(qint64 time) const;

private:
    struct Sample {
        QPointF at;
        qint64 time = 0;
    };
    std::deque<Sample> m_samples; // oldest first; the last at the pointer
};

// A trail's colour at a place along it (0 tail, 1 pointer): between the
// stops, the first at the tail.
QRgb trailColorAt(std::span<const QRgb> stops, float place);

// The neutral grey a trail takes from a black or white pointer.
std::vector<QRgb> neutralTrailColors();

// The trail's colours from the pointer's picture (premultiplied ARGB32):
// up to four of its own clear colours, the most used first chosen, lighter
// ones at the tail; the grey when it has none (Windows' black or white).
std::vector<QRgb> trailColors(const QImage& pointer);

} // namespace ws::capture::recording

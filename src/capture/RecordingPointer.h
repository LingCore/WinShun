#pragma once

#include "CursorTrail.h"

#include <QPoint>
#include <QRect>
#include <QRgb>
#include <QSize>

class QImage;

#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace ws::capture::recording {

qint64 monotonicTime(); // QPC in 100 ns, also used by the media clock

struct CursorPicture;

struct Click {
    QPoint point; // relative to the recorded region, physical pixels
    qint64 time = 0;
    bool secondary = false;
};

// The existing Raw Input listener is the producer. The media worker drains
// this bounded queue; no keyboard data or mouse movement is kept. The keys
// and buttons shown as keycaps (GitHub #98) come through KeyStream instead.
class ClickStream {
public:
    static constexpr size_t capacity = 16;
    struct Batch {
        std::array<Click, capacity> clicks {};
        size_t count = 0;
        std::span<const Click> view() const { return {clicks.data(), count}; }
    };
    explicit ClickStream(QRect region, quintptr excludedWindow = 0);
    void push(QPoint physical, bool secondary, qint64 time = 0);
    void pushInput(QPoint physical, bool secondary, quint32 messageTime);
    Batch take();
    void discardBefore(qint64 time);
private:
    const QRect m_region;
    const quintptr m_excludedWindow;
    std::mutex m_mutex;
    Batch m_pending;
    qint64 m_after = 0;
};

class ClickPulses {
public:
    static constexpr qint64 duration = 5'000'000; // half a second
    void add(Click click);
    void clear() { m_count = 0; }
    std::span<const Click> active(qint64 time);
private:
    std::array<Click, 8> m_clicks {};
    size_t m_count = 0;
};

// One keycap row (GitHub #98): from the picture (KeycapRender) to its place
// in the frame, pixel for pixel.
struct KeycapDraw {
    QRect from;
    QPoint to;
    float opacity = 1;
};

// The pointer trail (CursorTrail) as drawn: its line from the tail to the
// pointer, its width there in pixels (thinner and fainter as it ages) and
// its colours from the tail.
struct TrailDraw {
    std::span<const CursorTrail::Point> line;
    float width = 0;
    std::span<const QRgb> colors;
};

// The pointer, drawn by us over the trail (the capture leaves it out then),
// its hot spot at `at` in the region's pixels.
struct CursorDraw {
    const CursorPicture* picture = nullptr; // CursorArt.h
    QPoint at;
};

class CursorShapes;

// A recording's pointer trail on the media worker: looks at the pointer at
// every turn of the loop (60 times a second or more, for round curves) and
// gives each frame the trail and the pointer to draw over it. The trail's
// width follows Windows' pointer size, its colours the arrow's own.
class PointerTrail {
public:
    // `drawPointer`: the capture leaves the pointer out, the frames draw it.
    // `synthetic`: Recording's sample() — a set path for 250 ms, then still,
    // in one clear colour; no pointer of the desktop's.
    PointerTrail(QRect region, double scale, bool drawPointer, bool synthetic);
    ~PointerTrail();
    PointerTrail(const PointerTrail&) = delete;
    PointerTrail& operator=(const PointerTrail&) = delete;

    void look(qint64 time); // the media clock
    void clear(); // after a pause: nothing from before it
    // Valid until the next call.
    TrailDraw line(qint64 time);
    CursorDraw pointer(qint64 time);

private:
    QRect m_region;
    bool m_drawPointer;
    bool m_synthetic;
    std::unique_ptr<CursorShapes> m_shapes;
    CursorTrail m_trail;
    std::vector<CursorTrail::Point> m_line;
    std::vector<QRgb> m_colors;
    float m_width = 6;
    void* m_cursor = nullptr; // HCURSOR while the pointer shows
    QPoint m_at;
    std::vector<void*> m_unreadable; // pointers whose picture could not be read, logged once each
};

// Direct2D draws into a D3D11 texture on the media worker, from the bottom:
// the trail, the click pulses, the pointer, the keycaps. With none of them
// showing it returns the original texture, without a copy or a draw call.
class PointerOverlay {
public:
    PointerOverlay(ID3D11Device* device, ID3D11DeviceContext* context, QSize size, double scale);
    ~PointerOverlay();
    void setKeycaps(const QImage& picture); // premultiplied ARGB32; a null image for none
    ID3D11Texture2D* draw(ID3D11Texture2D* source, std::span<const Click> clicks, qint64 time,
        std::span<const KeycapDraw> keycaps = {}, const TrailDraw& trail = {}, const CursorDraw& cursor = {});
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ws::capture::recording

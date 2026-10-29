#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

class QImage;

#include <array>
#include <memory>
#include <mutex>
#include <span>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace ws::capture::recording {

qint64 monotonicTime(); // QPC in 100 ns, also used by the media clock

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

// Direct2D draws into a D3D11 texture on the media worker. With no active
// pulse and no keycap row showing it returns the original texture, without
// a copy or a draw call.
class PointerOverlay {
public:
    PointerOverlay(ID3D11Device* device, ID3D11DeviceContext* context, QSize size, double scale);
    ~PointerOverlay();
    void setKeycaps(const QImage& picture); // premultiplied ARGB32; a null image for none
    ID3D11Texture2D* draw(ID3D11Texture2D* source, std::span<const Click> clicks, qint64 time,
        std::span<const KeycapDraw> keycaps = {});
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ws::capture::recording

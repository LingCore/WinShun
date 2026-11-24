// WinRT precedes Qt's signals/slots macros.
#include <winrt/base.h>

#include "RecordingPointer.h"

#include "CursorArt.h"

#include <QImage>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace ws::capture::recording {

qint64 monotonicTime()
{
    static const qint64 frequency = [] { LARGE_INTEGER value {}; ::QueryPerformanceFrequency(&value); return value.QuadPart; }();
    LARGE_INTEGER value {};
    ::QueryPerformanceCounter(&value);
    return qint64(static_cast<long double>(value.QuadPart) * 10'000'000 / frequency);
}

ClickStream::ClickStream(QRect region, quintptr excludedWindow) : m_region(region), m_excludedWindow(excludedWindow) { }

void ClickStream::pushInput(QPoint physical, bool secondary, quint32 messageTime)
{
    const DWORD delay = ::GetTickCount() - messageTime; // shared WM_INPUT clock, including wraparound
    if (delay > 1000) return;
    push(physical, secondary, monotonicTime() - qint64(delay) * 10000);
}

void ClickStream::push(QPoint physical, bool secondary, qint64 time)
{
    if (!m_region.contains(physical)) return;
    if (m_excludedWindow) {
        const HWND ignored = reinterpret_cast<HWND>(m_excludedWindow);
        RECT rect {};
        const POINT point {physical.x(), physical.y()};
        if (::IsWindowVisible(ignored) && ::GetWindowRect(ignored, &rect) && ::PtInRect(&rect, point)) return;
    }
    if (!time) time = monotonicTime();
    const std::lock_guard lock(m_mutex);
    if (time < m_after) return;
    if (m_pending.count == capacity) {
        std::move(m_pending.clicks.begin() + 1, m_pending.clicks.end(), m_pending.clicks.begin());
        --m_pending.count;
    }
    m_pending.clicks[m_pending.count++] = {physical - m_region.topLeft(), time, secondary};
}

ClickStream::Batch ClickStream::take()
{
    const std::lock_guard lock(m_mutex);
    const auto batch = m_pending;
    m_pending.count = 0;
    return batch;
}

void ClickStream::discardBefore(qint64 time)
{
    const std::lock_guard lock(m_mutex);
    m_pending.count = 0;
    m_after = time;
}

void ClickPulses::add(Click click)
{
    if (m_count == m_clicks.size()) {
        std::move(m_clicks.begin() + 1, m_clicks.end(), m_clicks.begin());
        --m_count;
    }
    m_clicks[m_count++] = click;
}

std::span<const Click> ClickPulses::active(qint64 time)
{
    const auto end = std::remove_if(m_clicks.begin(), m_clicks.begin() + m_count,
        [time](const Click& click) { return time - click.time >= duration; });
    m_count = size_t(end - m_clicks.begin());
    return {m_clicks.data(), m_count};
}

struct PointerOverlay::Impl {
    winrt::com_ptr<ID3D11DeviceContext> graphics;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID2D1DeviceContext> painter;
    winrt::com_ptr<ID2D1Bitmap1> target;
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    winrt::com_ptr<ID2D1Bitmap1> keycaps;
    winrt::com_ptr<ID2D1StrokeStyle1> round; // the trail's pieces
    winrt::com_ptr<ID2D1StrokeStyle1> pointerEnd; // its last: flat at the hot spot
    // The trail is drawn alone first, each piece replacing what is under
    // it (newer over older: no doubled alpha where it crosses itself), then
    // laid over the frame. As large as a trail has been, at most the frame.
    winrt::com_ptr<ID2D1Bitmap1> trailLayer;
    D2D1_SIZE_U trailSize {};
    QSize frame;
    // The pointer's picture on the GPU, made again when it changes.
    quint64 cursorId = 0;
    winrt::com_ptr<ID2D1Bitmap1> cursorImage;
    winrt::com_ptr<ID2D1Bitmap1> cursorInvert;
    float scale;

    Impl(ID3D11Device* device, ID3D11DeviceContext* context, QSize size, double dpr)
        : frame(size), scale(float(std::clamp(dpr, 0.75, 4.0)))
    {
        graphics.copy_from(context);
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = UINT(size.width());
        desc.Height = UINT(size.height());
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; // drawn from, when the video is doubled
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, texture.put()));
        winrt::com_ptr<ID2D1Factory1> factory;
        winrt::check_hresult(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.put()));
        winrt::com_ptr<IDXGIDevice> dxgi;
        winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(dxgi.put())));
        winrt::com_ptr<ID2D1Device> drawing;
        winrt::check_hresult(factory->CreateDevice(dxgi.get(), drawing.put()));
        winrt::check_hresult(drawing->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, painter.put()));
        winrt::com_ptr<IDXGISurface> surface;
        winrt::check_hresult(texture->QueryInterface(IID_PPV_ARGS(surface.put())));
        const D2D1_BITMAP_PROPERTIES1 properties {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE}, 96, 96,
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, nullptr};
        winrt::check_hresult(painter->CreateBitmapFromDxgiSurface(surface.get(), &properties, target.put()));
        painter->SetTarget(target.get());
        painter->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        const D2D1_COLOR_F color {1, 0.68f, 0.15f, 1};
        winrt::check_hresult(painter->CreateSolidColorBrush(color, brush.put()));
        D2D1_STROKE_STYLE_PROPERTIES1 stroke {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0, D2D1_STROKE_TRANSFORM_TYPE_NORMAL};
        winrt::check_hresult(factory->CreateStrokeStyle(stroke, nullptr, 0, round.put()));
        stroke.endCap = D2D1_CAP_STYLE_FLAT;
        winrt::check_hresult(factory->CreateStrokeStyle(stroke, nullptr, 0, pointerEnd.put()));
    }

    // The trail into trailLayer; where it goes in the frame (empty for none).
    D2D1_RECT_F drawTrail(const TrailDraw& trail)
    {
        if (trail.line.size() < 2 || trail.width <= 0)
            return {};
        float left = FLT_MAX, top = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;
        for (const auto& point : trail.line) {
            left = std::min(left, float(point.at.x()));
            top = std::min(top, float(point.at.y()));
            right = std::max(right, float(point.at.x()));
            bottom = std::max(bottom, float(point.at.y()));
        }
        const float margin = trail.width / 2 + 2;
        const float x0 = std::max(0.0f, std::floor(left - margin)), y0 = std::max(0.0f, std::floor(top - margin));
        const float x1 = std::min(float(frame.width()), std::ceil(right + margin));
        const float y1 = std::min(float(frame.height()), std::ceil(bottom + margin));
        if (x1 <= x0 || y1 <= y0)
            return {};
        const auto width = UINT32(x1 - x0), height = UINT32(y1 - y0);
        if (!trailLayer || width > trailSize.width || height > trailSize.height) {
            const auto grow = [](UINT32 need, UINT32 had, int most) {
                return std::min<UINT32>(UINT32(std::max(most, 1)), std::max(had, (need + 255) / 256 * 256));
            };
            trailSize = {grow(width, trailSize.width, frame.width()), grow(height, trailSize.height, frame.height())};
            trailLayer = nullptr;
            const D2D1_BITMAP_PROPERTIES1 properties {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96,
                D2D1_BITMAP_OPTIONS_TARGET, nullptr};
            winrt::check_hresult(painter->CreateBitmap(trailSize, nullptr, 0, properties, trailLayer.put()));
        }
        painter->SetTarget(trailLayer.get());
        painter->BeginDraw();
        painter->Clear(D2D1_COLOR_F {0, 0, 0, 0});
        painter->SetTransform(D2D1::Matrix3x2F::Translation(-x0, -y0));
        painter->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        const size_t last = trail.line.size() - 1;
        for (size_t i = 1; i <= last; ++i) {
            const auto& from = trail.line[i - 1];
            const auto& to = trail.line[i];
            const float life = (from.life + to.life) / 2;
            const float stroke = trail.width * life;
            if (stroke < 0.3f || life < 0.01f)
                continue;
            const QRgb rgb = trailColorAt(trail.colors, (from.place + to.place) / 2);
            brush->SetColor(D2D1_COLOR_F {qRed(rgb) / 255.0f, qGreen(rgb) / 255.0f, qBlue(rgb) / 255.0f, life});
            painter->DrawLine({float(from.at.x()), float(from.at.y())}, {float(to.at.x()), float(to.at.y())}, brush.get(), stroke,
                i == last ? pointerEnd.get() : round.get());
        }
        painter->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        painter->SetTransform(D2D1::Matrix3x2F::Identity());
        winrt::check_hresult(painter->EndDraw());
        painter->SetTarget(target.get());
        return {x0, y0, x1, y1};
    }

    void loadCursor(const CursorPicture& picture)
    {
        if (picture.id == cursorId)
            return;
        cursorId = picture.id;
        cursorImage = nullptr;
        cursorInvert = nullptr;
        const D2D1_BITMAP_PROPERTIES1 colors {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96,
            D2D1_BITMAP_OPTIONS_NONE, nullptr};
        const QImage& image = picture.image;
        winrt::check_hresult(painter->CreateBitmap(D2D1::SizeU(UINT32(image.width()), UINT32(image.height())),
            image.constBits(), UINT32(image.bytesPerLine()), colors, cursorImage.put()));
        if (!picture.invert.isNull()) {
            // White, premultiplied: MASK_INVERT makes source × (1 − screen)
            // + screen × (1 − alpha). An A8 bitmap is refused here
            // (WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT).
            QImage mask(picture.invert.size(), QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < mask.height(); ++y) {
                const uchar* from = picture.invert.constScanLine(y);
                auto* to = reinterpret_cast<QRgb*>(mask.scanLine(y));
                for (int x = 0; x < mask.width(); ++x)
                    to[x] = qRgba(from[x], from[x], from[x], from[x]);
            }
            winrt::check_hresult(painter->CreateBitmap(D2D1::SizeU(UINT32(mask.width()), UINT32(mask.height())),
                mask.constBits(), UINT32(mask.bytesPerLine()), colors, cursorInvert.put()));
        }
    }
};

PointerOverlay::PointerOverlay(ID3D11Device* device, ID3D11DeviceContext* context, QSize size, double scale)
    : m_impl(std::make_unique<Impl>(device, context, size, scale)) { }
PointerOverlay::~PointerOverlay() = default;

void PointerOverlay::setKeycaps(const QImage& picture)
{
    auto& draw = *m_impl;
    draw.keycaps = nullptr;
    if (picture.isNull() || picture.format() != QImage::Format_ARGB32_Premultiplied)
        return;
    // 96 DPI, as the target: its pixels land on the frame's one to one.
    const D2D1_BITMAP_PROPERTIES1 properties {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96,
        D2D1_BITMAP_OPTIONS_NONE, nullptr};
    winrt::check_hresult(draw.painter->CreateBitmap(D2D1::SizeU(UINT32(picture.width()), UINT32(picture.height())),
        picture.constBits(), UINT32(picture.bytesPerLine()), properties, draw.keycaps.put()));
}

ID3D11Texture2D* PointerOverlay::draw(ID3D11Texture2D* source, std::span<const Click> clicks, qint64 time,
    std::span<const KeycapDraw> keycaps, const TrailDraw& trail, const CursorDraw& cursor)
{
    const bool rows = m_impl->keycaps && std::any_of(keycaps.begin(), keycaps.end(), [](const KeycapDraw& row) { return row.opacity > 0; });
    const bool pointer = cursor.picture && !cursor.picture->image.isNull();
    if (clicks.empty() && !rows && trail.line.size() < 2 && !pointer) return source;
    auto& draw = *m_impl;
    draw.graphics->CopyResource(draw.texture.get(), source); // never paint into the retained WGC frame
    const D2D1_RECT_F trailArea = draw.drawTrail(trail);
    if (pointer) draw.loadCursor(*cursor.picture);
    draw.painter->BeginDraw();
    if (trailArea.right > trailArea.left) {
        const D2D1_RECT_F from {0, 0, trailArea.right - trailArea.left, trailArea.bottom - trailArea.top};
        draw.painter->DrawBitmap(draw.trailLayer.get(), &trailArea, 1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &from, nullptr);
    }
    for (const auto& click : clicks) {
        if (time < click.time) continue;
        const float age = std::clamp(float(time - click.time) / ClickPulses::duration, 0.0f, 1.0f);
        const float opacity = (1 - age) * (1 - age);
        const D2D1_COLOR_F color = click.secondary ? D2D1_COLOR_F {0.68f, 0.40f, 0.96f, 1} : D2D1_COLOR_F {1, 0.68f, 0.15f, 1};
        draw.brush->SetColor(color);
        const float radius = (10 + 14 * age) * draw.scale;
        const D2D1_ELLIPSE circle {{float(click.point.x()), float(click.point.y())}, radius, radius};
        draw.brush->SetOpacity(opacity * 0.12f);
        draw.painter->FillEllipse(circle, draw.brush.get());
        draw.brush->SetOpacity(opacity * 0.95f);
        draw.painter->DrawEllipse(circle, draw.brush.get(), 2.5f * draw.scale);
    }
    if (pointer) {
        // As Windows draws it: its picture over the screen, then the pixels
        // it inverts (the default I-beam's) inverted.
        const auto& picture = *cursor.picture;
        const D2D1_POINT_2F at {float(cursor.at.x() - picture.hotSpot.x()), float(cursor.at.y() - picture.hotSpot.y())};
        const D2D1_RECT_F to {at.x, at.y, at.x + float(picture.image.width()), at.y + float(picture.image.height())};
        draw.painter->DrawBitmap(draw.cursorImage.get(), &to, 1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
        if (draw.cursorInvert)
            draw.painter->DrawImage(draw.cursorInvert.get(), &at, nullptr, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                D2D1_COMPOSITE_MODE_MASK_INVERT);
    }
    if (rows) {
        for (const auto& row : keycaps) {
            if (row.opacity <= 0) continue;
            const D2D1_RECT_F from {float(row.from.left()), float(row.from.top()), float(row.from.right() + 1), float(row.from.bottom() + 1)};
            const D2D1_RECT_F to {float(row.to.x()), float(row.to.y()), float(row.to.x() + row.from.width()), float(row.to.y() + row.from.height())};
            draw.painter->DrawBitmap(draw.keycaps.get(), &to, std::min(row.opacity, 1.0f), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &from, nullptr);
        }
    }
    winrt::check_hresult(draw.painter->EndDraw());
    draw.graphics->OMSetRenderTargets(0, nullptr, nullptr);
    return draw.texture.get();
}

PointerTrail::PointerTrail(QRect region, double scale, bool drawPointer, bool synthetic)
    : m_region(region), m_drawPointer(drawPointer && !synthetic), m_synthetic(synthetic),
      m_shapes(std::make_unique<CursorShapes>(scale))
{
    if (synthetic) {
        m_colors = {qRgb(220, 40, 220)}; // apart from the sample's inks and blocks
        m_width = 10;
        return;
    }
    const CursorPicture* arrow = m_shapes->arrow();
    m_colors = arrow ? trailColors(arrow->image) : neutralTrailColors();
    // 6 px for Windows' usual 32 px pointer; a larger pointer, a wider line.
    m_width = std::max(2.0f, float(m_shapes->systemSize()) * 6 / 32);
}

PointerTrail::~PointerTrail() = default;

void PointerTrail::look(qint64 time)
{
    QPointF at;
    if (m_synthetic) {
        constexpr qint64 moving = 2'500'000; // 250 ms
        const double f = double(std::min(time, moving)) / moving;
        at = QPointF(m_region.width() * (0.2 + 0.6 * f), m_region.height() * (0.25 + 0.08 * std::sin(f * 2 * 3.141592653589793)));
    } else {
        CURSORINFO info {};
        info.cbSize = sizeof info;
        POINT physical {};
        // Hidden (typing, a video full screen) or left to touch and pen:
        // no pointer, and the line ages away.
        if (!::GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING) || !info.hCursor || !::GetPhysicalCursorPos(&physical)) {
            m_cursor = nullptr;
            return;
        }
        m_cursor = info.hCursor;
        at = QPointF(physical.x - m_region.x(), physical.y - m_region.y());
    }
    m_at = QPoint(int(std::lround(at.x())), int(std::lround(at.y())));
    m_trail.move(at, time);
}

void PointerTrail::clear()
{
    m_trail.clear();
    m_line.clear();
}

TrailDraw PointerTrail::line(qint64 time)
{
    m_line = m_trail.line(time);
    return {m_line, m_width, m_colors};
}

CursorDraw PointerTrail::pointer(qint64 time)
{
    if (!m_drawPointer || !m_cursor)
        return {};
    const CursorPicture* picture = m_shapes->picture(m_cursor, time);
    if (!picture && m_unreadable.size() < 8 && std::ranges::find(m_unreadable, m_cursor) == m_unreadable.end()) {
        m_unreadable.push_back(m_cursor);
        qWarning("Recording: pointer %p could not be read (GetIconInfo); it is left out of the frames while it shows", m_cursor);
    }
    return {picture, m_at};
}

} // namespace ws::capture::recording

// WinRT precedes Qt's signals/slots macros.
#include <winrt/base.h>

#include "RecordingPointer.h"

#include <QImage>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
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
    float scale;

    Impl(ID3D11Device* device, ID3D11DeviceContext* context, QSize size, double dpr)
        : scale(float(std::clamp(dpr, 0.75, 4.0)))
    {
        graphics.copy_from(context);
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = UINT(size.width());
        desc.Height = UINT(size.height());
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
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
    std::span<const KeycapDraw> keycaps)
{
    const bool rows = m_impl->keycaps && std::any_of(keycaps.begin(), keycaps.end(), [](const KeycapDraw& row) { return row.opacity > 0; });
    if (clicks.empty() && !rows) return source;
    auto& draw = *m_impl;
    draw.graphics->CopyResource(draw.texture.get(), source); // never paint into the retained WGC frame
    draw.painter->BeginDraw();
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

} // namespace ws::capture::recording

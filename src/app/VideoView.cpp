#include "VideoView.h"

#include <QMetaObject>
#include <QMutex>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>


#include <d3d11_4.h>
#include <mfapi.h>
#include <mfmediaengine.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#undef GetCurrentTime // windows.h's old macro for GetTickCount; IMFMediaEngine has one of its own

using Microsoft::WRL::ComPtr;

namespace ws {

namespace {

// For the session: the next video plays as loud as the last one was left.
double s_volume = 1.0;
bool s_muted = false;

constexpr QSize kLargestFrame(3840, 2160);

// The picture's node: the frame's texture, replaced with each frame, its own.
class FrameNode final : public QSGSimpleTextureNode {
public:
    ~FrameNode() override { delete texture(); }
};

} // namespace

// The engine's events (on a thread of Media Foundation's) to the view, on
// its own thread; none once the view lets go of it (detach), and none of an
// engine the view has replaced (the generation).
class VideoEngineNotify final : public IMFMediaEngineNotify {
public:
    VideoEngineNotify(VideoView* view, quint64 generation)
        : m_view(view)
        , m_generation(generation)
    {
    }

    void detach()
    {
        QMutexLocker lock(&m_mutex);
        m_view = nullptr;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *out = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG left = --m_refs;
        if (left == 0)
            delete this;
        return left;
    }

    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD) override
    {
        if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) {
            ::SetEvent(reinterpret_cast<HANDLE>(param1));
            return S_OK;
        }
        QMutexLocker lock(&m_mutex);
        if (VideoView* view = m_view) {
            const quint64 generation = m_generation;
            QMetaObject::invokeMethod(view, [view, generation, event] { view->onEngineEvent(generation, event); },
                Qt::QueuedConnection);
        }
        return S_OK;
    }

private:
    ~VideoEngineNotify() = default;

    std::atomic<ULONG> m_refs {1};
    QMutex m_mutex;
    VideoView* m_view;
    const quint64 m_generation;
};

VideoView::VideoView(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    m_tick.setTimerType(Qt::PreciseTimer);
    m_tick.setInterval(8); // a new frame looked for at about twice 60 a second
    connect(&m_tick, &QTimer::timeout, this, &VideoView::tick);
}

VideoView::~VideoView()
{
    close();
}

bool VideoView::available()
{
    // Windows N editions have no Media Foundation without the Media Feature
    // Pack: mfplat.dll is loaded only when used (delay-loaded), so asked first.
    static const bool ok = [] {
        if (!::LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)
            || !::LoadLibraryExW(L"mfmediaengine.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            return false;
        return SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }();
    return ok;
}

void VideoView::setSource(const QString& source)
{
    if (source == m_source)
        return;
    close();
    m_source = source;
    setStatus(Empty);
    emit sourceChanged();
    updateActive();
}

double VideoView::volume() const
{
    return s_volume;
}

void VideoView::setVolume(double volume)
{
    volume = std::clamp(volume, 0.0, 1.0);
    if (volume == s_volume)
        return;
    s_volume = volume;
    if (m_engine)
        m_engine->SetVolume(volume);
    emit volumeChanged();
}

bool VideoView::muted() const
{
    return s_muted;
}

void VideoView::setMuted(bool muted)
{
    if (muted == s_muted)
        return;
    s_muted = muted;
    if (m_engine)
        m_engine->SetMuted(muted);
    emit mutedChanged();
}

QRectF VideoView::videoRect() const
{
    if (m_videoSize.isEmpty() || width() <= 0 || height() <= 0)
        return {};
    const qreal scale = std::min(width() / m_videoSize.width(), height() / m_videoSize.height());
    // On whole device pixels, so the frame (made at this size) is drawn texel for pixel.
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const qreal w = std::round(m_videoSize.width() * scale * dpr) / dpr;
    const qreal h = std::round(m_videoSize.height() * scale * dpr) / dpr;
    return QRectF(std::round((width() - w) / 2 * dpr) / dpr, std::round((height() - h) / 2 * dpr) / dpr, w, h);
}

void VideoView::togglePlay()
{
    if (!m_engine) {
        updateActive();
        return;
    }
    if (m_engine->IsEnded()) {
        m_engine->SetCurrentTime(0);
        m_engine->Play();
    } else if (m_engine->IsPaused()) {
        m_engine->Play();
    } else {
        m_engine->Pause();
    }
    readState();
}

void VideoView::seek(double seconds)
{
    if (!m_engine)
        return;
    seconds = std::max(0.0, m_duration > 0 ? std::min(seconds, m_duration) : seconds);
    m_seeking = true;
    m_engine->SetCurrentTime(seconds);
    if (m_position != seconds) {
        m_position = seconds;
        emit positionChanged();
    }
}

void VideoView::setStatus(Status status)
{
    if (status == m_status)
        return;
    m_status = status;
    emit statusChanged();
}

void VideoView::updateActive()
{
    const bool wanted = !m_source.isEmpty() && isVisible() && window() && window()->isVisible();
    if (wanted && !m_engine && m_status != Failed) {
        if (!open()) {
            close();
            setStatus(Failed);
        }
    } else if (!wanted && m_engine) {
        close();
        setStatus(Empty);
    }
}

bool VideoView::open()
{
    if (!available()) {
        qWarning("WinShun: video preview: Windows has no Media Foundation here");
        return false;
    }
    // A device of our own for the engine (decoding, and scaling the frames);
    // used from its threads and this one, so guarded.
    constexpr UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
        &m_device, nullptr, &m_context);
    if (FAILED(hr))
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &m_device,
            nullptr, &m_context);
    if (FAILED(hr)) {
        qWarning("WinShun: video preview: no Direct3D device (0x%08lx)", static_cast<unsigned long>(hr));
        return false;
    }
    ComPtr<ID3D11Multithread> guarded;
    if (SUCCEEDED(m_device.As(&guarded)))
        guarded->SetMultithreadProtected(TRUE);
    UINT token = 0;
    if (FAILED(hr = ::MFCreateDXGIDeviceManager(&token, &m_manager)) || FAILED(hr = m_manager->ResetDevice(m_device.Get(), token))) {
        qWarning("WinShun: video preview: no DXGI device manager (0x%08lx)", static_cast<unsigned long>(hr));
        return false;
    }
    ComPtr<IMFMediaEngineClassFactory> factory;
    if (FAILED(hr = ::CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        qWarning("WinShun: video preview: no media engine (0x%08lx)", static_cast<unsigned long>(hr));
        return false;
    }
    ++m_generation;
    m_notify = new VideoEngineNotify(this, m_generation);
    ComPtr<IMFAttributes> attributes;
    if (FAILED(hr = ::MFCreateAttributes(&attributes, 3)) || FAILED(hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, m_notify))
        || FAILED(hr = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, m_manager.Get()))
        || FAILED(hr = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM))
        || FAILED(hr = factory->CreateInstance(0, attributes.Get(), &m_engine))) {
        qWarning("WinShun: video preview: the media engine did not start (0x%08lx)", static_cast<unsigned long>(hr));
        return false;
    }
    m_engine->SetVolume(s_volume);
    m_engine->SetMuted(s_muted);
    m_engine->SetAutoPlay(TRUE);
    BSTR url = ::SysAllocString(reinterpret_cast<const wchar_t*>(m_source.utf16()));
    hr = m_engine->SetSource(url);
    ::SysFreeString(url);
    if (FAILED(hr)) {
        qWarning("WinShun: video preview: cannot open the file (0x%08lx)", static_cast<unsigned long>(hr));
        return false;
    }
    setStatus(Loading);
    m_tick.start();
    return true;
}

void VideoView::close()
{
    m_tick.stop();
    if (m_notify)
        m_notify->detach(); // no more events from this engine
    if (m_engine) {
        m_engine->Shutdown();
        m_engine.Reset();
    }
    if (m_notify) {
        m_notify->Release();
        m_notify = nullptr;
    }
    m_staging.Reset();
    m_target.Reset();
    m_targetSize = {};
    m_manager.Reset();
    m_context.Reset();
    m_device.Reset();
    m_seeking = false;
    if (!m_frame.isNull()) {
        m_frame = {};
        update();
        emit hasFrameChanged();
    }
    if (m_playing || m_ended) {
        m_playing = m_ended = false;
        emit playingChanged();
    }
    if (m_position != 0) {
        m_position = 0;
        emit positionChanged();
    }
    if (m_duration != 0) {
        m_duration = 0;
        emit durationChanged();
    }
    if (!m_videoSize.isEmpty()) {
        m_videoSize = {};
        emit videoSizeChanged();
        emit videoRectChanged();
    }
}

void VideoView::onEngineEvent(quint64 generation, DWORD event)
{
    if (generation != m_generation || !m_engine)
        return;
    switch (event) {
    case MF_MEDIA_ENGINE_EVENT_ERROR: {
        ComPtr<IMFMediaError> error;
        m_engine->GetError(&error);
        qWarning("WinShun: video preview: Windows cannot play it (media engine error %u, 0x%08lx)",
            error ? static_cast<unsigned>(error->GetErrorCode()) : 0u,
            error ? static_cast<unsigned long>(error->GetExtendedErrorCode()) : 0ul);
        close();
        setStatus(Failed);
        return;
    }
    case MF_MEDIA_ENGINE_EVENT_CANPLAY:
        setStatus(Ready);
        break;
    case MF_MEDIA_ENGINE_EVENT_SEEKED:
        m_seeking = false;
        break;
    default:
        break;
    }
    readState();
}

void VideoView::readState()
{
    if (!m_engine)
        return;
    double duration = m_engine->GetDuration();
    if (!std::isfinite(duration) || duration < 0)
        duration = 0;
    if (duration != m_duration) {
        m_duration = duration;
        emit durationChanged();
    }
    DWORD w = 0;
    DWORD h = 0;
    if (SUCCEEDED(m_engine->GetNativeVideoSize(&w, &h))) {
        const QSizeF size(w, h);
        if (size != m_videoSize) {
            m_videoSize = size;
            emit videoSizeChanged();
            emit videoRectChanged();
        }
    }
    const bool ended = m_engine->IsEnded() != FALSE;
    const bool playing = !ended && !m_engine->IsPaused();
    if (playing != m_playing || ended != m_ended) {
        m_playing = playing;
        m_ended = ended;
        emit playingChanged();
    }
    if (!m_seeking) {
        const double position = m_engine->GetCurrentTime();
        if (std::isfinite(position) && std::abs(position - m_position) > 0.001) {
            m_position = position;
            emit positionChanged();
        }
    }
}

void VideoView::tick()
{
    if (!m_engine)
        return;
    LONGLONG time = 0;
    const bool fresh = m_engine->OnVideoStreamTick(&time) == S_OK;
    // A new frame; or the same at another size (the window resized while paused).
    if (fresh || (!m_frame.isNull() && frameSize() != m_targetSize))
        transferFrame();
    if (m_playing) {
        const double position = m_engine->GetCurrentTime();
        if (!m_seeking && std::isfinite(position) && std::abs(position - m_position) >= 0.05) {
            m_position = position;
            emit positionChanged();
        }
    }
}

QSize VideoView::frameSize() const
{
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const QSizeF shown = videoRect().size() * dpr;
    return QSize(qRound(shown.width()), qRound(shown.height())).boundedTo(kLargestFrame);
}

bool VideoView::transferFrame()
{
    const QSize size = frameSize();
    if (size.width() < 2 || size.height() < 2)
        return false;
    if (size != m_targetSize) {
        m_target.Reset();
        m_staging.Reset();
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = static_cast<UINT>(size.width());
        desc.Height = static_cast<UINT>(size.height());
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_target)))
            return false;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_staging))) {
            m_target.Reset();
            return false;
        }
        m_targetSize = size;
    }
    const RECT whole {0, 0, size.width(), size.height()};
    const MFARGB black {0, 0, 0, 255};
    if (FAILED(m_engine->TransferVideoFrame(m_target.Get(), nullptr, &whole, &black)))
        return false;
    m_context->CopyResource(m_staging.Get(), m_target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    QImage frame(size, QImage::Format_RGB32);
    const auto* from = static_cast<const uchar*>(mapped.pData);
    for (int y = 0; y < size.height(); ++y)
        std::memcpy(frame.scanLine(y), from + static_cast<qsizetype>(y) * mapped.RowPitch, static_cast<size_t>(size.width()) * 4);
    m_context->Unmap(m_staging.Get(), 0);
    const bool first = m_frame.isNull();
    m_frame = std::move(frame);
    m_frameNew = true;
    update();
    if (first)
        emit hasFrameChanged();
    return true;
}

QSGNode* VideoView::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    auto* node = static_cast<FrameNode*>(old);
    if (m_frame.isNull()) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new FrameNode;
        node->setFiltering(QSGTexture::Linear);
        m_frameNew = true;
    }
    if (m_frameNew) {
        QSGTexture* before = node->texture();
        node->setTexture(window()->createTextureFromImage(m_frame));
        delete before;
        m_frameNew = false;
    }
    node->setRect(videoRect());
    return node;
}

void VideoView::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange) {
        if (m_window)
            disconnect(m_window, nullptr, this, nullptr);
        m_window = value.window;
        if (m_window)
            connect(m_window, &QWindow::visibleChanged, this, &VideoView::updateActive);
        updateActive();
    } else if (change == ItemVisibleHasChanged) {
        updateActive();
    }
}

void VideoView::geometryChange(const QRectF& now, const QRectF& before)
{
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size()) {
        emit videoRectChanged();
        update();
    }
}

} // namespace ws

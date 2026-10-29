// C++/WinRT before Qt's headers: Qt's "signals" and "slots" macros would
// rewrite what is in it (as in platform/TextRecognizer.cpp).
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include "Grabber.h"

#include "Hdr.h"

#include <QDebug>
#include <QElapsedTimer>

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_3.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

using namespace Qt::StringLiterals;

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace ws::capture {

namespace {

constexpr DWORD kFrameWaitMs = 1000; // a frame comes in 6–11 ms (stage 0); a second means WGC is stuck

QRect toRect(const RECT& r)
{
    return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

// The screen's pixels in `r` through GDI: CAPTUREBLT takes layered windows
// in too. SDR only (HDR monitors come out washed out).
QImage bitblt(const QRect& r)
{
    QImage image(r.width(), r.height(), QImage::Format_RGB32);
    const HDC screen = ::GetDC(nullptr);
    const HDC memory = ::CreateCompatibleDC(screen);
    const HBITMAP bitmap = ::CreateCompatibleBitmap(screen, r.width(), r.height());
    const HGDIOBJ old = ::SelectObject(memory, bitmap);
    const bool ok = ::BitBlt(memory, 0, 0, r.width(), r.height(), screen, r.x(), r.y(), SRCCOPY | CAPTUREBLT);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = r.width();
    info.bmiHeader.biHeight = -r.height(); // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const int lines = ok ? ::GetDIBits(memory, bitmap, 0, static_cast<UINT>(r.height()), image.bits(), &info, DIB_RGB_COLORS) : 0;
    ::SelectObject(memory, old);
    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return lines == r.height() ? image : QImage();
}

} // namespace

// A stream's state, shared by the capture thread's job, WGC's thread that
// hands frames over and the thread that asks for them.
struct StreamState {
    explicit StreamState(const QRect& r)
        : region(r)
    {
    }
    ~StreamState()
    {
        ::CloseHandle(wake);
        ::CloseHandle(stop);
    }

    const QRect region; // physical pixels, virtual desktop
    bool hdr = false; // set before `started`
    HANDLE wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr); // a frame came, or one is asked for
    HANDLE stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::promise<bool> started;
    std::promise<void> finished;

    std::mutex mutex;
    std::condition_variable read; // a frame was read back
    wgc::Direct3D11CaptureFrame pending {nullptr}; // the newest frame WGC gave, not read yet
    quint64 given = 0; // frames WGC gave
    quint64 pendingNumber = 0;
    bool wanted = false; // next() waits for a frame after `wantedAfter`
    quint64 wantedAfter = 0;
    QImage image; // the last frame read back
    quint64 imageNumber = 0;
    bool ended = false;
};

class StreamImpl : public Grabber::Stream {
public:
    StreamImpl(std::shared_ptr<StreamState> state, std::future<void> finished)
        : m_state(std::move(state))
        , m_finished(std::move(finished))
    {
    }
    ~StreamImpl() override
    {
        ::SetEvent(m_state->stop);
        m_finished.wait();
    }

    Frame next(quint64 after, int timeoutMs) override
    {
        StreamState& s = *m_state;
        std::unique_lock lock(s.mutex);
        if (s.imageNumber <= after) {
            s.wanted = true;
            s.wantedAfter = after;
            ::SetEvent(s.wake);
            s.read.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)),
                [&] { return s.ended || s.imageNumber > after; });
        }
        if (s.imageNumber <= after)
            return {};
        return {s.image, s.imageNumber};
    }

    bool hdr() const override { return m_state->hdr; }

private:
    std::shared_ptr<StreamState> m_state;
    std::future<void> m_finished;
};

struct Grabber::Worker {
    struct Monitor {
        HMONITOR handle = nullptr;
        QRect rect;
        DisplayColorMode color;
        wgc::GraphicsCaptureItem item {nullptr};
    };

    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool stopping = false;
    HANDLE quit = ::CreateEventW(nullptr, TRUE, FALSE, nullptr); // ends a stream's job too

    // Only touched on the thread:
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> context;
    wd3d::IDirect3DDevice device {nullptr};
    bool borderless = false;
    std::vector<Monitor> monitors;

    Worker()
    {
        thread = std::thread([this] { run(); });
        post([this] { prepare(); });
    }

    ~Worker()
    {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        ::SetEvent(quit);
        thread.join();
        ::CloseHandle(quit);
    }

    void post(std::function<void()> job)
    {
        {
            const std::lock_guard lock(mutex);
            jobs.push_back(std::move(job));
        }
        wake.notify_all();
    }

    void run()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (jobs.empty())
                    break; // stopping, nothing left
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            job();
        }
        // The WinRT and D3D objects go on the thread that made them.
        monitors.clear();
        device = nullptr;
        context = nullptr;
        d3d = nullptr;
        winrt::uninit_apartment();
    }

    // The device, borderless access, an item per monitor. Each part failing
    // leaves its BitBlt fallback.
    void prepare()
    {
        QElapsedTimer clock;
        clock.start();
        try {
            if (!wgc::GraphicsCaptureSession::IsSupported()) {
                qWarning() << "Screen capture: Windows.Graphics.Capture is not supported here; BitBlt is used";
                return;
            }
            if (!device) {
                winrt::check_hresult(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, context.put()));
                const auto dxgi = d3d.as<IDXGIDevice>();
                winrt::com_ptr<::IInspectable> inspectable;
                winrt::check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
                device = inspectable.as<wd3d::IDirect3DDevice>();
            }
            if (!borderless) {
                // Allowed for an unpackaged program without asking (stage 0);
                // without it the session draws a yellow border on the screen.
                const auto access = wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
                borderless = access == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed;
            }
        } catch (const winrt::hresult_error& e) {
            qWarning().noquote() << u"Screen capture: Windows.Graphics.Capture not ready (0x%1 %2); BitBlt is used"_s
                                        .arg(static_cast<quint32>(e.code().value), 8, 16, QChar(u'0'))
                                        .arg(QString::fromWCharArray(e.message().c_str()));
            device = nullptr;
            context = nullptr;
            d3d = nullptr;
        }
        enumerate();
        qInfo().noquote() << u"Screen capture: prepared in %1 ms (%2 monitor(s), WGC %3, borderless %4)"_s
                                 .arg(clock.elapsed())
                                 .arg(monitors.size())
                                 .arg(device ? u"yes"_s : u"no"_s, borderless ? u"yes"_s : u"no"_s);
    }

    void enumerate()
    {
        monitors.clear();
        ::EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR handle, HDC, LPRECT, LPARAM param) -> BOOL {
                auto* self = reinterpret_cast<Worker*>(param);
                MONITORINFOEXW info {};
                info.cbSize = sizeof info;
                if (!::GetMonitorInfoW(handle, &info))
                    return TRUE;
                Monitor m;
                m.handle = handle;
                m.rect = toRect(info.rcMonitor);
                m.color = displayColorMode(info.szDevice);
                if (!m.color.hdr && qEnvironmentVariableIsSet("WINSHUN_TEST_HDR_CAPTURE"))
                    m.color = {true, 80.0}; // see Grabber::grab
                self->monitors.push_back(std::move(m));
                return TRUE;
            },
            reinterpret_cast<LPARAM>(this));
        if (!device)
            return;
        const auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        for (Monitor& m : monitors) {
            try {
                winrt::check_hresult(interop->CreateForMonitor(m.handle, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                    winrt::put_abi(m.item)));
            } catch (const winrt::hresult_error& e) {
                qWarning().noquote() << u"Screen capture: no capture item for a monitor (0x%1)"_s.arg(
                    static_cast<quint32>(e.code().value), 8, 16, QChar(u'0'));
                m.item = nullptr;
            }
        }
    }

    // One frame of every monitor that has an item, all sessions started
    // together; a monitor whose frame does not come is left with a null image.
    struct Pending {
        wgc::Direct3D11CaptureFramePool pool {nullptr};
        wgc::GraphicsCaptureSession session {nullptr};
        winrt::event_token token;
        HANDLE arrived = nullptr;
        std::atomic<bool> taken {false};
        wgc::Direct3D11CaptureFrame frame {nullptr};
    };

    // `keep`: an HDR monitor's pixels as they are too.
    QImage readFrame(const wgc::Direct3D11CaptureFrame& frame, const Monitor& m, HdrPixels* keep)
    {
        winrt::com_ptr<ID3D11Texture2D> texture;
        const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        if (static_cast<int>(desc.Width) != m.rect.width() || static_cast<int>(desc.Height) != m.rect.height())
            return {}; // the monitor changed under its item: BitBlt this time
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(d3d->CreateTexture2D(&desc, nullptr, staging.put()));
        context->CopyResource(staging.get(), texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        QImage image;
        if (m.color.hdr) {
            image = mapScRgbToSdr(static_cast<const std::uint16_t*>(mapped.pData), static_cast<int>(desc.Width),
                static_cast<int>(desc.Height), static_cast<int>(mapped.RowPitch), m.color.sdrWhiteNits);
            if (keep) {
                *keep = HdrPixels::fromScRgb(static_cast<const std::uint16_t*>(mapped.pData), static_cast<int>(desc.Width),
                    static_cast<int>(desc.Height), static_cast<int>(mapped.RowPitch), m.color.sdrWhiteNits);
            }
        } else {
            image = QImage(static_cast<int>(desc.Width), static_cast<int>(desc.Height), QImage::Format_RGB32);
            const std::size_t row = std::size_t(desc.Width) * 4;
            for (UINT y = 0; y < desc.Height; ++y)
                std::memcpy(image.scanLine(static_cast<int>(y)),
                    static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(mapped.RowPitch) * y, row);
        }
        context->Unmap(staging.get(), 0);
        return image;
    }

    std::vector<Shot> grabAll(QString* report, bool keepHdr)
    {
        QElapsedTimer clock;
        clock.start();
        std::vector<Shot> shots(monitors.size());
        std::vector<std::unique_ptr<Pending>> pending(monitors.size());
        std::vector<HANDLE> events;
        for (std::size_t i = 0; i < monitors.size(); ++i) {
            const Monitor& m = monitors[i];
            shots[i].monitor = m.handle;
            shots[i].rect = m.rect;
            shots[i].hdr = m.color.hdr;
            if (!device || !m.item)
                continue;
            auto p = std::make_unique<Pending>();
            try {
                const auto format = m.color.hdr ? wdx::DirectXPixelFormat::R16G16B16A16Float
                                                : wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
                p->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device, format, 1, m.item.Size());
                p->session = p->pool.CreateCaptureSession(m.item);
                p->session.IsCursorCaptureEnabled(false); // the overlay shows the real one
                if (borderless)
                    p->session.IsBorderRequired(false);
                p->arrived = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                Pending* raw = p.get();
                p->token = p->pool.FrameArrived([raw](const wgc::Direct3D11CaptureFramePool& sender, const auto&) {
                    if (raw->taken.exchange(true))
                        return;
                    raw->frame = sender.TryGetNextFrame();
                    ::SetEvent(raw->arrived);
                });
                p->session.StartCapture();
                events.push_back(p->arrived);
                pending[i] = std::move(p);
            } catch (const winrt::hresult_error& e) {
                qWarning().noquote() << u"Screen capture: could not start capturing a monitor (0x%1)"_s.arg(
                    static_cast<quint32>(e.code().value), 8, 16, QChar(u'0'));
                if (p && p->arrived)
                    ::CloseHandle(p->arrived);
            }
        }
        if (!events.empty())
            ::WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), TRUE, kFrameWaitMs);
        const qint64 framesMs = clock.elapsed();
        int viaWgc = 0;
        for (std::size_t i = 0; i < monitors.size(); ++i) {
            Pending* p = pending[i].get();
            if (p) {
                try {
                    if (::WaitForSingleObject(p->arrived, 0) == WAIT_OBJECT_0 && p->frame) {
                        auto kept = keepHdr && monitors[i].color.hdr ? std::make_shared<HdrPixels>() : nullptr;
                        shots[i].image = readFrame(p->frame, monitors[i], kept.get());
                        if (kept && !kept->isNull() && !shots[i].image.isNull())
                            shots[i].hdrPixels = std::move(kept);
                    }
                } catch (const winrt::hresult_error& e) {
                    qWarning().noquote() << u"Screen capture: could not read a frame (0x%1)"_s.arg(
                        static_cast<quint32>(e.code().value), 8, 16, QChar(u'0'));
                }
                try {
                    p->pool.FrameArrived(p->token);
                    if (p->frame)
                        p->frame.Close();
                    p->session.Close();
                    p->pool.Close();
                } catch (const winrt::hresult_error&) {
                }
                ::CloseHandle(p->arrived);
            }
            if (!shots[i].image.isNull()) {
                shots[i].wgc = true;
                ++viaWgc;
                continue;
            }
            shots[i].hdr = false;
            shots[i].image = bitblt(shots[i].rect);
        }
        if (report) {
            *report = u"%1 monitor(s), %2 by WGC (frames in %3 ms), the rest by BitBlt, %4 ms in all"_s.arg(monitors.size())
                          .arg(viaWgc)
                          .arg(framesMs)
                          .arg(clock.elapsed());
        }
        // Every monitor failing through WGC: the device may be lost (a driver
        // update, a GPU reset). Made again for the next time.
        if (viaWgc == 0 && device && !monitors.empty()) {
            device = nullptr;
            context = nullptr;
            d3d = nullptr;
            post([this] { prepare(); });
        }
        return shots;
    }

    // `region` of the frame (`m`'s pixels; `region` in the virtual desktop's)
    // through `staging`, a texture of the region's size.
    QImage readRegion(const wgc::Direct3D11CaptureFrame& frame, const Monitor& m, const QRect& region, ID3D11Texture2D* staging)
    {
        winrt::com_ptr<ID3D11Texture2D> texture;
        const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        if (static_cast<int>(desc.Width) != m.rect.width() || static_cast<int>(desc.Height) != m.rect.height())
            return {}; // the monitor changed under its item
        const int left = region.x() - m.rect.x();
        const int top = region.y() - m.rect.y();
        const D3D11_BOX box {UINT(left), UINT(top), 0, UINT(left + region.width()), UINT(top + region.height()), 1};
        context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture.get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        winrt::check_hresult(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
        QImage image;
        if (m.color.hdr) {
            image = mapScRgbToSdr(static_cast<const std::uint16_t*>(mapped.pData), region.width(), region.height(),
                static_cast<int>(mapped.RowPitch), m.color.sdrWhiteNits);
        } else {
            image = QImage(region.width(), region.height(), QImage::Format_RGB32);
            const std::size_t row = std::size_t(region.width()) * 4;
            for (int y = 0; y < region.height(); ++y)
                std::memcpy(image.scanLine(y), static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(mapped.RowPitch) * std::size_t(y), row);
        }
        context->Unmap(staging, 0);
        return image;
    }

    // A stream's job: runs until it is stopped (or the grabber goes), the
    // other jobs waiting behind it.
    void streamJob(const std::shared_ptr<StreamState>& s)
    {
        const Monitor* m = nullptr;
        for (const Monitor& candidate : monitors)
            if (candidate.rect.contains(s->region))
                m = &candidate;
        if (!device || !m || !m->item || s->region.isEmpty()) {
            s->started.set_value(false);
            s->finished.set_value();
            return;
        }
        const Monitor monitor = *m; // the list may be made again after the job
        wgc::Direct3D11CaptureFramePool pool {nullptr};
        wgc::GraphicsCaptureSession session {nullptr};
        winrt::event_token token {};
        winrt::com_ptr<ID3D11Texture2D> staging;
        try {
            const bool hdr = monitor.color.hdr;
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = UINT(s->region.width());
            desc.Height = UINT(s->region.height());
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::check_hresult(d3d->CreateTexture2D(&desc, nullptr, staging.put()));
            const auto format = hdr ? wdx::DirectXPixelFormat::R16G16B16A16Float : wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
            pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device, format, 2, monitor.item.Size());
            session = pool.CreateCaptureSession(monitor.item);
            session.IsCursorCaptureEnabled(false);
            if (borderless)
                session.IsBorderRequired(false);
            // On WGC's thread; the state lives as long as the handler.
            token = pool.FrameArrived([state = s](const wgc::Direct3D11CaptureFramePool& sender, const auto&) {
                wgc::Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
                if (!frame)
                    return;
                {
                    const std::lock_guard lock(state->mutex);
                    if (state->ended) {
                        frame.Close();
                        return;
                    }
                    if (state->pending)
                        state->pending.Close(); // only the newest is kept
                    state->pending = frame;
                    state->pendingNumber = ++state->given;
                }
                ::SetEvent(state->wake);
            });
            session.StartCapture();
            s->hdr = hdr;
        } catch (const winrt::hresult_error& e) {
            qWarning().noquote() << u"Screen capture: could not stream a region (0x%1)"_s.arg(
                static_cast<quint32>(e.code().value), 8, 16, QChar(u'0'));
            try {
                if (pool)
                    pool.FrameArrived(token);
                if (session)
                    session.Close();
                if (pool)
                    pool.Close();
            } catch (const winrt::hresult_error&) {
            }
            s->started.set_value(false);
            s->finished.set_value();
            return;
        }
        s->started.set_value(true);
        const HANDLE waits[] = {s->stop, quit, s->wake};
        for (;;) {
            const DWORD woke = ::WaitForMultipleObjects(3, waits, FALSE, INFINITE);
            if (woke != WAIT_OBJECT_0 + 2)
                break; // stopped, the grabber going, or failed
            wgc::Direct3D11CaptureFrame frame {nullptr};
            quint64 number = 0;
            {
                const std::lock_guard lock(s->mutex);
                if (!s->wanted || !s->pending || s->pendingNumber <= s->wantedAfter)
                    continue;
                frame = std::exchange(s->pending, nullptr);
                number = s->pendingNumber;
            }
            QImage image;
            try {
                image = readRegion(frame, monitor, s->region, staging.get());
                frame.Close();
            } catch (const winrt::hresult_error& e) {
                qWarning().noquote() << u"Screen capture: could not read a streamed frame (0x%1)"_s.arg(
                    static_cast<quint32>(e.code().value), 8, 16, QChar(u'0'));
            }
            {
                const std::lock_guard lock(s->mutex);
                s->image = std::move(image);
                s->imageNumber = number;
                s->wanted = false;
            }
            s->read.notify_all();
        }
        try {
            pool.FrameArrived(token);
            session.Close();
            pool.Close();
        } catch (const winrt::hresult_error&) {
        }
        {
            const std::lock_guard lock(s->mutex);
            if (s->pending)
                s->pending.Close();
            s->pending = nullptr;
            s->image = QImage();
            s->ended = true;
        }
        // What the frames took goes back now: the driver keeps freed
        // textures for the next ones otherwise (two of the monitor's size).
        staging = nullptr;
        pool = nullptr;
        session = nullptr;
        context->ClearState();
        context->Flush();
        if (const auto dxgi = d3d.try_as<IDXGIDevice3>())
            dxgi->Trim();
        s->read.notify_all();
        s->finished.set_value();
    }
};

Grabber::Grabber()
    : m_worker(std::make_unique<Worker>())
{
}

Grabber::~Grabber() = default;

void Grabber::refresh()
{
    Worker* worker = m_worker.get();
    worker->post([worker] {
        if (worker->device)
            worker->enumerate();
        else
            worker->prepare();
    });
}

std::vector<Grabber::Shot> Grabber::grab(QString* report, bool keepHdr)
{
    std::promise<std::vector<Shot>> promise;
    auto future = promise.get_future();
    Worker* worker = m_worker.get();
    worker->post([worker, &promise, report, keepHdr] { promise.set_value(worker->grabAll(report, keepHdr)); });
    return future.get();
}

std::unique_ptr<Grabber::Stream> Grabber::stream(const QRect& region)
{
    auto state = std::make_shared<StreamState>(region);
    std::future<bool> started = state->started.get_future();
    std::future<void> finished = state->finished.get_future();
    Worker* worker = m_worker.get();
    worker->post([worker, state] { worker->streamJob(state); });
    if (!started.get()) {
        finished.wait();
        return nullptr;
    }
    return std::make_unique<StreamImpl>(std::move(state), std::move(finished));
}

} // namespace ws::capture

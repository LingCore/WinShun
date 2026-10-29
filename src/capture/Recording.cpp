// WinRT must precede Qt's signals/slots macros.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include "Recording.h"
#include "RecordingPointer.h"
#include "KeycapRender.h"
#include "KeyStream.h"
#include "Hdr.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QScopeGuard>
#include <QUuid>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <audioclient.h>
#include <codecapi.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace ws::capture::recording {

void Timeline::pause(qint64 now)
{
    if (m_pause < 0)
        m_pause = std::max(m_start, now);
}

void Timeline::resume(qint64 now)
{
    if (m_pause >= 0) {
        m_paused += std::max<qint64>(0, now - m_pause);
        m_pause = -1;
    }
}

qint64 Timeline::elapsed(qint64 now) const
{
    return std::max<qint64>(0, (m_pause >= 0 ? m_pause : now) - m_start - m_paused);
}

namespace {

constexpr qint64 kSecond = 10'000'000;
constexpr int kAudioRate = 48000;
constexpr int kChannels = 2;

qint64 nowTicks()
{
    return monotonicTime();
}

struct Failure {
    QString stage;
    HRESULT hr;
};

void check(HRESULT hr, const char* stage)
{
    if (FAILED(hr))
        throw Failure {QString::fromLatin1(stage), hr};
}

winrt::com_ptr<IMFMediaType> videoType(const GUID& subtype, QSize size, int fps)
{
    winrt::com_ptr<IMFMediaType> type;
    check(::MFCreateMediaType(type.put()), "create video type");
    check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "video major type");
    check(type->SetGUID(MF_MT_SUBTYPE, subtype), "video subtype");
    check(::MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, UINT32(size.width()), UINT32(size.height())), "video size");
    check(::MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, UINT32(fps), 1), "video rate");
    check(::MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "pixel aspect");
    check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "progressive video");
    check(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709), "video primaries");
    check(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709), "video transfer");
    check(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709), "video matrix");
    check(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235), "video range");
    return type;
}

class Graphics {
public:
    explicit Graphics(HMONITOR monitor)
    {
        winrt::com_ptr<IDXGIFactory1> factory;
        check(::CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "DXGI factory");
        winrt::com_ptr<IDXGIAdapter1> selected;
        for (UINT a = 0; !selected; ++a) {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(a, adapter.put()) == DXGI_ERROR_NOT_FOUND)
                break;
            for (UINT o = 0;; ++o) {
                winrt::com_ptr<IDXGIOutput> output;
                if (adapter->EnumOutputs(o, output.put()) == DXGI_ERROR_NOT_FOUND)
                    break;
                DXGI_OUTPUT_DESC desc {};
                check(output->GetDesc(&desc), "display description");
                if (desc.Monitor == monitor) {
                    selected = adapter;
                    break;
                }
            }
        }
        check(::D3D11CreateDevice(selected.get(), selected ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()), "D3D11 device");
        if (auto threading = context.try_as<ID3D10Multithread>())
            threading->SetMultithreadProtected(TRUE); // MF's encoder may use this device on its own threads
        UINT token = 0;
        check(::MFCreateDXGIDeviceManager(&token, manager.put()), "DXGI device manager");
        check(manager->ResetDevice(device.get(), token), "DXGI device binding");
    }

    winrt::com_ptr<ID3D11Texture2D> texture(QSize size, DXGI_FORMAT format, bool staging = false)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = UINT(size.width());
        desc.Height = UINT(size.height());
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
        desc.BindFlags = staging ? 0 : D3D11_BIND_RENDER_TARGET;
        desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
        winrt::com_ptr<ID3D11Texture2D> result;
        check(device->CreateTexture2D(&desc, nullptr, result.put()), "video texture");
        return result;
    }

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<IMFDXGIDeviceManager> manager;
};

class Converter {
public:
    Converter(Graphics& graphics, QSize input, QSize output, int fps)
    {
        m_device = graphics.device.as<ID3D11VideoDevice>();
        m_context = graphics.context.as<ID3D11VideoContext>();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc {};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = {UINT(fps), 1};
        desc.OutputFrameRate = desc.InputFrameRate;
        desc.InputWidth = UINT(input.width());
        desc.InputHeight = UINT(input.height());
        desc.OutputWidth = UINT(output.width());
        desc.OutputHeight = UINT(output.height());
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        check(m_device->CreateVideoProcessorEnumerator(&desc, m_enumerator.put()), "video processor enumeration");
        check(m_device->CreateVideoProcessor(m_enumerator.get(), 0, m_processor.put()), "video processor");
        const RECT source {0, 0, input.width(), input.height()};
        const RECT dest {0, 0, input.width(), input.height()}; // odd dimensions are padded, never rescaled
        m_context->VideoProcessorSetStreamSourceRect(m_processor.get(), 0, TRUE, &source);
        m_context->VideoProcessorSetStreamDestRect(m_processor.get(), 0, TRUE, &dest);
        m_context->VideoProcessorSetStreamFrameFormat(m_processor.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_context->VideoProcessorSetStreamAutoProcessingMode(m_processor.get(), 0, FALSE);
        const D3D11_VIDEO_COLOR black {0, 0, 0, 1};
        m_context->VideoProcessorSetOutputBackgroundColor(m_processor.get(), FALSE, &black);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb {};
        rgb.RGB_Range = 0; // full-range RGB
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv {};
        yuv.YCbCr_Matrix = 1; // BT.709
        yuv.Nominal_Range = 1; // studio range
        m_context->VideoProcessorSetStreamColorSpace(m_processor.get(), 0, &rgb);
        m_context->VideoProcessorSetOutputColorSpace(m_processor.get(), &yuv);
    }

    void convert(ID3D11Texture2D* source, ID3D11Texture2D* destination, UINT slice)
    {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in {};
        in.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        winrt::com_ptr<ID3D11VideoProcessorInputView> input;
        check(m_device->CreateVideoProcessorInputView(source, m_enumerator.get(), &in, input.put()), "video input view");
        D3D11_TEXTURE2D_DESC desc {};
        destination->GetDesc(&desc);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out {};
        if (desc.ArraySize > 1) {
            out.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
            out.Texture2DArray.FirstArraySlice = slice;
            out.Texture2DArray.ArraySize = 1;
        } else {
            out.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        }
        winrt::com_ptr<ID3D11VideoProcessorOutputView> output;
        check(m_device->CreateVideoProcessorOutputView(destination, m_enumerator.get(), &out, output.put()), "video output view");
        D3D11_VIDEO_PROCESSOR_STREAM stream {};
        stream.Enable = TRUE;
        stream.pInputSurface = input.get();
        check(m_context->VideoProcessorBlt(m_processor.get(), output.get(), m_number++, 1, &stream), "GPU RGB to NV12");
    }

private:
    UINT m_number = 0;
    winrt::com_ptr<ID3D11VideoDevice> m_device;
    winrt::com_ptr<ID3D11VideoContext> m_context;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> m_enumerator;
    winrt::com_ptr<ID3D11VideoProcessor> m_processor;
};

class Writer {
public:
    Writer(Graphics& graphics, const Options& options, const QString& temporary, bool hardware)
        : m_graphics(graphics), m_size((options.region.width() + 1) & ~1, (options.region.height() + 1) & ~1), m_gpu(hardware)
    {
        winrt::com_ptr<IMFAttributes> attributes;
        check(::MFCreateAttributes(attributes.put(), 4), "writer attributes");
        check(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4), "MP4 container");
        check(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware), "hardware encoding policy");
        if (hardware)
            check(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, graphics.manager.get()), "encoder GPU device");
        check(::MFCreateSinkWriterFromURL(reinterpret_cast<const wchar_t*>(temporary.utf16()), nullptr,
            attributes.get(), m_writer.put()), "open MP4");
        const auto out = videoType(MFVideoFormat_H264, m_size, options.fps);
        check(out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High), "H.264 high profile");
        // Required when negotiating the output type. Quality mode, once
        // accepted, controls the actual bitrate rather than this estimate.
        const auto rate = UINT32(std::clamp<qint64>(qint64(m_size.width()) * m_size.height() * options.fps / 12, 500'000, 24'000'000));
        check(out->SetUINT32(MF_MT_AVG_BITRATE, rate), "video bitrate");
        check(m_writer->AddStream(out.get(), &m_video), "MP4 video stream");
        const auto in = videoType(MFVideoFormat_NV12, m_size, options.fps);
        LONG stride = 0;
        check(::MFGetStrideForBitmapInfoHeader(MFVideoFormat_NV12.Data1, DWORD(m_size.width()), &stride), "NV12 row stride");
        m_stride = UINT32(stride);
        if (!hardware)
            check(in->SetUINT32(MF_MT_DEFAULT_STRIDE, m_stride), "software NV12 row stride");
        winrt::com_ptr<IMFAttributes> encoding;
        check(::MFCreateAttributes(encoding.put(), 3), "encoding parameters");
        check(encoding->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality), "quality mode");
        check(encoding->SetUINT32(CODECAPI_AVEncCommonQuality, UINT32(options.quality)), "video quality");
        check(encoding->SetUINT32(CODECAPI_AVEncMPVGOPSize, UINT32(options.fps * 2)), "keyframe interval");
        HRESULT negotiated = m_writer->SetInputMediaType(m_video, in.get(), encoding.get());
        if (FAILED(negotiated)) // a vendor may only implement bitrate-based VBR
            negotiated = m_writer->SetInputMediaType(m_video, in.get(), nullptr);
        check(negotiated, "negotiate H.264 encoder");
        if (options.systemAudio || options.microphone)
            addAudio();
        if (hardware) {
            check(::MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(m_allocator.put())), "GPU sample allocator");
            check(m_allocator->SetDirectXManager(graphics.manager.get()), "GPU sample manager");
            winrt::com_ptr<IMFAttributes> pool;
            check(::MFCreateAttributes(pool.put(), 2), "GPU pool attributes");
            check(pool->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET), "GPU pool render targets");
            check(pool->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT), "GPU pool usage");
            check(m_allocator->InitializeSampleAllocatorEx(2, 12, pool.get(), in.get()), "bounded GPU sample pool");
        } else {
            m_nv12 = graphics.texture(m_size, DXGI_FORMAT_NV12);
            m_staging = graphics.texture(m_size, DXGI_FORMAT_NV12, true);
        }
        // Inspect the selected transform; requesting hardware is not proof
        // that the sink writer actually found a hardware encoder.
        if (auto extended = m_writer.try_as<IMFSinkWriterEx>()) {
            for (DWORD i = 0; i < 8; ++i) {
                GUID category {};
                winrt::com_ptr<IMFTransform> transform;
                if (FAILED(extended->GetTransformForStream(m_video, i, &category, transform.put())))
                    break;
                winrt::com_ptr<IMFAttributes> attrs;
                if (SUCCEEDED(transform->GetAttributes(attrs.put()))) {
                    UINT32 length = 0;
                    if (SUCCEEDED(attrs->GetStringLength(MFT_FRIENDLY_NAME_Attribute, &length))) {
                        std::wstring name(length + 1, L'\0');
                        if (SUCCEEDED(attrs->GetString(MFT_FRIENDLY_NAME_Attribute, name.data(), length + 1, &length)))
                            encoder = QString::fromWCharArray(name.c_str());
                    }
                    if (SUCCEEDED(attrs->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length)))
                        actualHardware = true;
                }
            }
        }
        if (encoder.isEmpty())
            encoder = hardware ? u"Media Foundation / D3D11"_s : u"Media Foundation / software"_s;
        check(m_writer->BeginWriting(), "start encoder");
    }

    void video(Converter& converter, ID3D11Texture2D* source, qint64 time, qint64 duration)
    {
        winrt::com_ptr<IMFSample> sample;
        if (m_gpu) {
            const HRESULT hr = m_allocator->AllocateSample(sample.put());
            check(hr, "allocate bounded encoder sample");
            winrt::com_ptr<IMFMediaBuffer> buffer;
            check(sample->GetBufferByIndex(0, buffer.put()), "GPU sample buffer");
            const auto dxgi = buffer.as<IMFDXGIBuffer>();
            winrt::com_ptr<ID3D11Texture2D> target;
            UINT slice = 0;
            check(dxgi->GetResource(IID_PPV_ARGS(target.put())), "encoder texture");
            check(dxgi->GetSubresourceIndex(&slice), "encoder texture slice");
            converter.convert(source, target.get(), slice);
            check(buffer->SetCurrentLength(DWORD(m_size.width() * m_size.height() * 3 / 2)), "GPU video length");
        } else {
            converter.convert(source, m_nv12.get(), 0);
            m_graphics.context->CopyResource(m_staging.get(), m_nv12.get());
            D3D11_MAPPED_SUBRESOURCE mapped {};
            check(m_graphics.context->Map(m_staging.get(), 0, D3D11_MAP_READ, 0, &mapped), "software encoder readback");
            const auto unmap = qScopeGuard([&] { m_graphics.context->Unmap(m_staging.get(), 0); });
            const DWORD size = DWORD(m_stride * m_size.height() * 3 / 2);
            winrt::com_ptr<IMFMediaBuffer> buffer;
            check(::MFCreateMemoryBuffer(size, buffer.put()), "software video buffer");
            BYTE* bytes = nullptr;
            check(buffer->Lock(&bytes, nullptr, nullptr), "software video lock");
            std::memset(bytes, 0, size);
            for (int row = 0; row < m_size.height() * 3 / 2; ++row)
                std::memcpy(bytes + size_t(row) * m_stride, static_cast<const BYTE*>(mapped.pData) + size_t(row) * mapped.RowPitch,
                    size_t(m_size.width()));
            check(buffer->Unlock(), "software video unlock");
            check(buffer->SetCurrentLength(size), "software video length");
            check(::MFCreateSample(sample.put()), "software video sample");
            check(sample->AddBuffer(buffer.get()), "software video sample buffer");
        }
        check(sample->SetSampleTime(time), "video timestamp");
        check(sample->SetSampleDuration(duration), "video duration");
        check(m_writer->WriteSample(m_video, sample.get()), "write video");
    }

    void audio(const std::vector<qint16>& pcm, qint64 firstFrame)
    {
        if (pcm.empty())
            return;
        winrt::com_ptr<IMFSample> sample;
        winrt::com_ptr<IMFMediaBuffer> buffer;
        const DWORD length = DWORD(pcm.size() * sizeof(qint16));
        check(::MFCreateMemoryBuffer(length, buffer.put()), "audio buffer");
        BYTE* data = nullptr;
        check(buffer->Lock(&data, nullptr, nullptr), "audio buffer lock");
        std::memcpy(data, pcm.data(), length);
        buffer->Unlock();
        buffer->SetCurrentLength(length);
        check(::MFCreateSample(sample.put()), "audio sample");
        sample->AddBuffer(buffer.get());
        sample->SetSampleTime(firstFrame * kSecond / kAudioRate);
        sample->SetSampleDuration(qint64(pcm.size() / kChannels) * kSecond / kAudioRate);
        check(m_writer->WriteSample(m_audio, sample.get()), "write audio");
    }

    HRESULT finish() { return m_writer ? m_writer->Finalize() : E_UNEXPECTED; }
    bool usesGpu() const { return m_gpu; }
    QString encoder;
    bool actualHardware = false;

private:
    void addAudio()
    {
        winrt::com_ptr<IMFMediaType> out;
        check(::MFCreateMediaType(out.put()), "audio type");
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000); // 128 kbps, stereo
        out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        check(m_writer->AddStream(out.get(), &m_audio), "MP4 AAC stream");
        winrt::com_ptr<IMFMediaType> in;
        check(::MFCreateMediaType(in.put()), "PCM input type");
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
        in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioRate * kChannels * 2);
        check(m_writer->SetInputMediaType(m_audio, in.get(), nullptr), "AAC input");
    }

    Graphics& m_graphics;
    QSize m_size;
    bool m_gpu;
    UINT32 m_stride = 0;
    DWORD m_video = 0;
    DWORD m_audio = 0;
    winrt::com_ptr<IMFSinkWriter> m_writer;
    winrt::com_ptr<IMFVideoSampleAllocatorEx> m_allocator;
    winrt::com_ptr<ID3D11Texture2D> m_nv12;
    winrt::com_ptr<ID3D11Texture2D> m_staging;
};

// WASAPI converts each default endpoint to the same 48 kHz stereo PCM
// format. QPC packet timestamps place both sources on the video clock.
class Audio {
public:
    explicit Audio(bool loopback)
    {
        winrt::com_ptr<IMMDeviceEnumerator> enumerator;
        check(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(enumerator.put())), "audio devices");
        winrt::com_ptr<IMMDevice> endpoint;
        check(enumerator->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, endpoint.put()),
            loopback ? "system audio device" : "microphone device");
        check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, m_client.put_void()), "audio client");
        WAVEFORMATEX format {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = kChannels;
        format.nSamplesPerSec = kAudioRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = kChannels * 2;
        format.nAvgBytesPerSec = kAudioRate * format.nBlockAlign;
        const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
            | (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        check(m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kSecond / 5, 0, &format, nullptr), "audio capture format");
        check(m_client->GetService(IID_PPV_ARGS(m_capture.put())), "audio capture service");
    }

    ~Audio() { if (m_client) m_client->Stop(); }
    void start() { check(m_client->Start(), "start audio capture"); }

    void collect(const Timeline& clock, qint64 now, bool paused)
    {
        UINT32 available = 0;
        check(m_capture->GetNextPacketSize(&available), "audio device availability");
        while (available) {
            BYTE* bytes = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 position = 0, qpc = 0;
            check(m_capture->GetBuffer(&bytes, &frames, &flags, &position, &qpc), "audio capture packet");
            auto release = qScopeGuard([&] { m_capture->ReleaseBuffer(frames); });
            if (!paused) {
                const qint64 packetTime = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ? now - qint64(frames) * kSecond / kAudioRate : qint64(qpc);
                Block block;
                block.first = clock.elapsed(packetTime) * kAudioRate / kSecond;
                block.pcm.resize(size_t(frames) * kChannels);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && bytes)
                    std::memcpy(block.pcm.data(), bytes, block.pcm.size() * sizeof(qint16));
                m_blocks.push_back(std::move(block));
                // At most half a second, even when an encoder stalls.
                while (m_blocks.size() > 1 && m_blocks.back().first - m_blocks.front().first > kAudioRate / 2)
                    m_blocks.pop_front();
            }
            m_capture->ReleaseBuffer(frames);
            release.dismiss();
            check(m_capture->GetNextPacketSize(&available), "next audio packet");
        }
        if (paused)
            m_blocks.clear();
    }

    void mix(std::vector<int>& mixed, qint64 first)
    {
        const qint64 end = first + qint64(mixed.size() / kChannels);
        while (!m_blocks.empty() && m_blocks.front().first + qint64(m_blocks.front().pcm.size() / kChannels) <= first)
            m_blocks.pop_front();
        for (const auto& block : m_blocks) {
            const qint64 from = std::max(first, block.first);
            const qint64 to = std::min(end, block.first + qint64(block.pcm.size() / kChannels));
            for (qint64 frame = from; frame < to; ++frame)
                for (int channel = 0; channel < kChannels; ++channel)
                    mixed[size_t(frame - first) * kChannels + channel] += block.pcm[size_t(frame - block.first) * kChannels + channel];
        }
    }

private:
    struct Block { qint64 first = 0; std::vector<qint16> pcm; };
    winrt::com_ptr<IAudioClient> m_client;
    winrt::com_ptr<IAudioCaptureClient> m_capture;
    std::deque<Block> m_blocks;
};

class Source {
public:
    Source(Graphics& graphics, const Options& options)
        : m_graphics(graphics), m_region(options.region)
    {
        const RECT area {m_region.left(), m_region.top(), m_region.right() + 1, m_region.bottom() + 1};
        const HMONITOR monitor = ::MonitorFromRect(&area, MONITOR_DEFAULTTONULL);
        MONITORINFOEXW info {};
        info.cbSize = sizeof info;
        if (!monitor || !::GetMonitorInfoW(monitor, &info))
            throw Failure {u"recording display"_s, E_INVALIDARG};
        m_bounds = QRect(info.rcMonitor.left, info.rcMonitor.top,
            info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top);
        if (!m_bounds.contains(m_region))
            throw Failure {u"recording region crosses displays"_s, E_INVALIDARG};
        m_color = displayColorMode(info.szDevice);
        m_image = graphics.texture(m_region.size(), DXGI_FORMAT_B8G8R8A8_UNORM);
        if (m_color.hdr)
            m_staging = graphics.texture(m_region.size(), DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        winrt::com_ptr<IInspectable> inspectable;
        check(::CreateDirect3D11DeviceFromDXGIDevice(graphics.device.as<IDXGIDevice>().get(), inspectable.put()), "WinRT graphics device");
        auto item = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        check(item->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(m_item)), "capture display");
        const auto format = m_color.hdr ? wdx::DirectXPixelFormat::R16G16B16A16Float : wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
        m_pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(inspectable.as<wd3d::IDirect3DDevice>(), format, 2, m_item.Size());
        m_session = m_pool.CreateCaptureSession(m_item);
        m_session.IsCursorCaptureEnabled(options.cursor);
        try {
            if (wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get()
                == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed)
                m_session.IsBorderRequired(false);
        } catch (const winrt::hresult_error&) { }
        m_shared = std::make_shared<Shared>();
        m_token = m_pool.FrameArrived([state = m_shared](const auto& pool, const auto&) {
            try {
                const auto frame = pool.TryGetNextFrame();
                const std::lock_guard lock(state->mutex);
                if (state->pending) state->pending.Close();
                state->pending = frame; // one pending frame, never an accumulating queue
            } catch (const winrt::hresult_error& error) {
                const std::lock_guard lock(state->mutex);
                state->error = error.code().value;
            }
        });
        m_closed = m_item.Closed([state = m_shared](const auto&, const auto&) {
            const std::lock_guard lock(state->mutex);
            state->closed = true;
        });
        m_session.StartCapture();
    }

    ~Source()
    {
        try {
            if (m_pool) m_pool.FrameArrived(m_token);
            if (m_item) m_item.Closed(m_closed);
            if (m_session) m_session.Close();
            if (m_pool) m_pool.Close();
        } catch (const winrt::hresult_error&) { }
    }

    ID3D11Texture2D* next()
    {
        wgc::Direct3D11CaptureFrame frame {nullptr};
        {
            const std::lock_guard lock(m_shared->mutex);
            if (m_shared->closed)
                throw Failure {QCoreApplication::translate("Recording", "录制屏幕已断开或关闭"), DXGI_ERROR_ACCESS_LOST};
            check(m_shared->error, "capture frame delivery");
            frame = std::exchange(m_shared->pending, nullptr);
        }
        if (!frame)
            return m_have ? m_image.get() : nullptr;
        const auto close = qScopeGuard([&] { frame.Close(); });
        const auto size = frame.ContentSize();
        if (size.Width != m_bounds.width() || size.Height != m_bounds.height())
            throw Failure {u"display size changed"_s, DXGI_ERROR_ACCESS_LOST};
        winrt::com_ptr<ID3D11Texture2D> texture;
        check(frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()->GetInterface(
            IID_PPV_ARGS(texture.put())), "captured texture");
        const UINT x = UINT(m_region.x() - m_bounds.x()), y = UINT(m_region.y() - m_bounds.y());
        const D3D11_BOX box {x, y, 0, x + UINT(m_region.width()), y + UINT(m_region.height()), 1};
        m_graphics.context->CopySubresourceRegion(m_color.hdr ? m_staging.get() : m_image.get(), 0, 0, 0, 0, texture.get(), 0, &box);
        if (m_color.hdr) {
            // The screenshot's proven colour mapping. Only HDR takes this
            // bounded CPU path; SDR stays on the GPU through the encoder.
            D3D11_MAPPED_SUBRESOURCE mapped {};
            check(m_graphics.context->Map(m_staging.get(), 0, D3D11_MAP_READ, 0, &mapped), "HDR readback");
            const QImage image = mapScRgbToSdr(static_cast<const std::uint16_t*>(mapped.pData), m_region.width(), m_region.height(),
                int(mapped.RowPitch), m_color.sdrWhiteNits);
            m_graphics.context->Unmap(m_staging.get(), 0);
            m_graphics.context->UpdateSubresource(m_image.get(), 0, nullptr, image.constBits(), UINT(image.bytesPerLine()), 0);
        }
        m_have = true;
        return m_image.get();
    }

private:
    struct Shared {
        std::mutex mutex;
        wgc::Direct3D11CaptureFrame pending {nullptr};
        HRESULT error = S_OK;
        bool closed = false;
    };
    Graphics& m_graphics;
    QRect m_region;
    QRect m_bounds;
    DisplayColorMode m_color;
    bool m_have = false;
    std::shared_ptr<Shared> m_shared;
    winrt::event_token m_token {};
    winrt::event_token m_closed {};
    wgc::GraphicsCaptureItem m_item {nullptr};
    wgc::Direct3D11CaptureFramePool m_pool {nullptr};
    wgc::GraphicsCaptureSession m_session {nullptr};
    winrt::com_ptr<ID3D11Texture2D> m_image;
    winrt::com_ptr<ID3D11Texture2D> m_staging;
};

Result run(const Options& options, Control& control, const std::function<void(const Progress&)>& progress, int syntheticMs)
{
    Result result;
    if (control.stop) return result;
    if (options.path.isEmpty() || QFileInfo::exists(options.path) || options.region.width() < 16 || options.region.height() < 16
        || options.region.width() > 7680 || options.region.height() > 4320 || options.fps < 1 || options.fps > 60) {
        result.error = QCoreApplication::translate("Recording", "录屏路径已存在，或录制区域 / 帧率不可用");
        return result;
    }
    // A new, owned staging file beside the destination; incomplete output
    // is never mistaken for a successfully saved MP4.
    const QString temporary = options.path + u"."_s + QUuid::createUuid().toString(QUuid::Id128) + u".partial.mp4"_s;
    QFile reservation(temporary);
    if (!reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        result.error = QCoreApplication::translate("Recording", "无法写入录屏文件夹：%1").arg(reservation.errorString());
        return result;
    }
    reservation.close();
    bool finalized = false;
    std::unique_ptr<Writer> writer;
    try {
        const HMODULE platform = ::LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const HMODULE readwrite = ::LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto libraries = qScopeGuard([&] {
            if (readwrite) ::FreeLibrary(readwrite);
            if (platform) ::FreeLibrary(platform);
        });
        if (!platform || !readwrite)
            throw Failure {u"Windows media components are unavailable"_s, HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)};
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const auto apartment = qScopeGuard([] { winrt::uninit_apartment(); });
        check(::MFStartup(MF_VERSION), "Media Foundation startup");
        const auto shutdown = qScopeGuard([&] {
            if (writer && !finalized && result.frames && !control.discard)
                finalized = SUCCEEDED(writer->finish()); // preserve completed media on a recoverable error
            writer.reset();
            ::MFShutdown();
        });
        const RECT area {options.region.left(), options.region.top(), options.region.right() + 1, options.region.bottom() + 1};
        Graphics graphics(::MonitorFromRect(&area, MONITOR_DEFAULTTOPRIMARY));
        const QSize encodedSize((options.region.width() + 1) & ~1, (options.region.height() + 1) & ~1);
        Converter converter(graphics, encodedSize, encodedSize, options.fps);
        winrt::com_ptr<ID3D11Texture2D> padded;
        if (encodedSize != options.region.size())
            padded = graphics.texture(encodedSize, DXGI_FORMAT_B8G8R8A8_UNORM);
        if (!options.software) {
            try { writer = std::make_unique<Writer>(graphics, options, temporary, true); }
            catch (const Failure&) { writer.reset(); }
        }
        if (!writer)
            writer = std::make_unique<Writer>(graphics, options, temporary, false);
        result.encoder = writer->encoder;
        result.hardware = writer->actualHardware;
        std::vector<std::unique_ptr<Audio>> audio;
        if (syntheticMs < 0) {
            try {
                if (options.systemAudio) audio.push_back(std::make_unique<Audio>(true));
            } catch (const Failure& failure) {
                throw Failure {QCoreApplication::translate("Recording", "系统声音不可用，请检查播放设备，或关闭“系统声音”"), failure.hr};
            }
            try {
                if (options.microphone) audio.push_back(std::make_unique<Audio>(false));
            } catch (const Failure& failure) {
                throw Failure {QCoreApplication::translate("Recording", "麦克风不可用，请检查设备与 Windows 麦克风权限，或关闭“麦克风”"), failure.hr};
            }
        }
        std::unique_ptr<Source> source;
        winrt::com_ptr<ID3D11Texture2D> generated;
        if (syntheticMs < 0)
            source = std::make_unique<Source>(graphics, options);
        else
            generated = graphics.texture(options.region.size(), DXGI_FORMAT_B8G8R8A8_UNORM);
        const qint64 waiting = nowTicks();
        ID3D11Texture2D* image = generated.get();
        while (!image && !control.stop) {
            image = source->next();
            if (nowTicks() - waiting > 5 * kSecond)
                throw Failure {u"no first screen frame"_s, HRESULT_FROM_WIN32(WAIT_TIMEOUT)};
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        QImage sampleBase;
        if (generated) {
            sampleBase = QImage(options.region.size(), QImage::Format_RGB32);
            sampleBase.fill(QColor(248, 249, 251));
            QPainter painter(&sampleBase);
            painter.setPen(QColor(28, 33, 42));
            painter.setFont(QFont(u"Segoe UI"_s, 12));
            for (int y = 35; y < sampleBase.height(); y += 30)
                painter.drawText(20, y, QCoreApplication::translate("Recording", "WinShun  区域录屏 — clear text 0123456789    C++20 / MP4"));
        }
        std::unique_ptr<PointerOverlay> pointer;
        const bool showKeys = options.keys.mode != keycaps::Mode::Off || options.keys.mouse;
        if ((options.clickHighlights || showKeys) && image && !control.stop)
            pointer = std::make_unique<PointerOverlay>(graphics.device.get(), graphics.context.get(), options.region.size(), options.pointerScale);
        const qint64 start = nowTicks();
        Timeline clock(start);
        ClickPulses pulses;
        qint64 syntheticClick = 0;
        if (options.clickInput) options.clickInput->discardBefore(start);
        // GitHub #98's keycaps: the rows at each frame's time, drawn into a
        // picture again only when they change.
        keycaps::Model keys(options.keys);
        const int keyUnit = keycaps::unitFor(options.region.size(), options.pointerScale, options.outputScale);
        const size_t keyRows = size_t(keycaps::rowsThatFit(options.region.size(), keyUnit));
        keycaps::Picture keyPicture;
        std::vector<QPoint> keyPlaces;
        std::vector<KeycapDraw> keyDraws;
        quint64 keysDrawn = keys.revision();
        bool keysLeft = false;
        qint64 syntheticKey = showKeys && syntheticMs >= 0 ? kSecond / 10 : -1;
        if (options.keyInput) options.keyInput->discardBefore(start);
        for (const auto& input : audio) input->start();
        qint64 nextVideo = 0, nextAudio = 0, lastUpdate = -kSecond;
        const qint64 interval = kSecond / options.fps;
        const bool withAudio = options.systemAudio || options.microphone;
        const auto audioUntil = [&](qint64 target) {
            while (nextAudio < target) {
                const qint64 count = std::min<qint64>(480, target - nextAudio);
                std::vector<int> mixed(size_t(count) * kChannels);
                for (const auto& input : audio) input->mix(mixed, nextAudio);
                if (syntheticMs >= 0)
                    for (qint64 frame = 0; frame < count; ++frame)
                        mixed[size_t(frame) * kChannels] = mixed[size_t(frame) * kChannels + 1]
                            = int(5000 * std::sin(double(nextAudio + frame) * 2 * 3.141592653589793 * 440 / kAudioRate));
                std::vector<qint16> pcm(mixed.size());
                std::transform(mixed.begin(), mixed.end(), pcm.begin(), [](int v) { return qint16(std::clamp(v, -32768, 32767)); });
                writer->audio(pcm, nextAudio);
                nextAudio += count;
            }
        };
        bool paused = false;
        while (!control.stop) {
            const qint64 wall = nowTicks();
            const bool requestedPause = control.paused.load();
            if (requestedPause != paused) {
                if (requestedPause) {
                    // Finish the tail before discarding packets captured while paused.
                    for (const auto& input : audio) input->collect(clock, wall, false);
                    if (withAudio) audioUntil(clock.elapsed(wall) * kAudioRate / kSecond);
                    clock.pause(wall);
                } else {
                    for (const auto& input : audio) input->collect(clock, wall, true);
                    clock.resume(wall);
                }
                pulses.clear();
                if (options.clickInput) options.clickInput->discardBefore(wall);
                keys.clear(); // nothing pressed before a pause shows after it
                if (options.keyInput) options.keyInput->discardBefore(wall);
                paused = requestedPause;
            }
            const qint64 elapsed = clock.elapsed(wall);
            if (syntheticMs >= 0 && elapsed >= qint64(syntheticMs) * 10000)
                break;
            for (const auto& input : audio) input->collect(clock, wall, paused);
            if (options.clickInput) {
                const auto batch = options.clickInput->take();
                if (!paused)
                    for (auto click : batch.view()) {
                        click.time = clock.elapsed(click.time);
                        pulses.add(click);
                    }
            }
            if (options.keyInput) {
                const auto batch = options.keyInput->take();
                if (!paused)
                    for (auto key : batch.view()) {
                        key.time = clock.elapsed(key.time);
                        keys.feed(key);
                    }
            }
            if (syntheticKey >= 0 && elapsed >= syntheticKey && !paused) {
                keycaps::Key key;
                key.time = elapsed;
                key.vk = options.keys.mode != keycaps::Mode::Off ? std::uint16_t('S') : std::uint16_t(VK_LBUTTON);
                key.mods = options.keys.mode != keycaps::Mode::Off ? std::uint8_t(keycaps::kCtrl) : std::uint8_t(0);
                keys.feed(key);
                syntheticKey = -1;
            }
            if (pointer && options.clickHighlights && syntheticMs >= 0 && elapsed >= syntheticClick) {
                pulses.add({QPoint(options.region.width() * 3 / 4, options.region.height() / 4), elapsed, false});
                syntheticClick = elapsed + kSecond;
            }
            if (!paused && elapsed >= nextVideo) {
                if (source) image = source->next();
                if (generated) {
                    QImage picture = sampleBase.copy();
                    QPainter painter(&picture);
                    painter.fillRect(int((elapsed / 10000) % std::max(1, picture.width() - 80)), picture.height() / 2, 70, 35, QColor(40, 110, 200));
                    painter.end();
                    graphics.context->UpdateSubresource(generated.get(), 0, nullptr, picture.constBits(), UINT(picture.bytesPerLine()), 0);
                }
                // Fixed cadence keeps static recordings and pause/resume
                // duration exact. Inter-frame compression makes repeats cheap.
                if (pointer && showKeys) {
                    keys.advance(elapsed);
                    const auto& rows = keys.rows();
                    if (keyPicture.rows.empty() && !rows.empty() && syntheticMs < 0) {
                        // A new stack: out of the pointer's way.
                        POINT cursor {};
                        keysLeft = ::GetPhysicalCursorPos(&cursor)
                            && keycaps::preferLeft(QPoint(cursor.x, cursor.y) - options.region.topLeft(), options.region.size(), keyUnit);
                    }
                    if (keys.revision() != keysDrawn) {
                        keysDrawn = keys.revision();
                        keyPicture = keycaps::render(std::span(rows).last(std::min(rows.size(), keyRows)), options.keyFont, keyUnit);
                        keyPlaces = keycaps::place(keyPicture, options.region.size(), keyUnit, keysLeft);
                        pointer->setKeycaps(keyPicture.image);
                    }
                    keyDraws.clear();
                    const size_t first = rows.size() - keyPicture.rows.size();
                    for (size_t i = 0; i < keyPicture.rows.size(); ++i)
                        keyDraws.push_back({keyPicture.rows[i], keyPlaces[i], keycaps::Model::opacity(rows[first + i], elapsed)});
                }
                ID3D11Texture2D* input = image;
                if (pointer) input = pointer->draw(image, pulses.active(elapsed), elapsed, keyDraws);
                if (padded) {
                    const UINT w = UINT(options.region.width()), h = UINT(options.region.height());
                    const D3D11_BOX whole {0, 0, 0, w, h, 1};
                    graphics.context->CopySubresourceRegion(padded.get(), 0, 0, 0, 0, input, 0, &whole);
                    // NV12 processors need even rectangles. Extend the final
                    // row/column on the GPU; never stretch the selected text.
                    if (w & 1) {
                        const D3D11_BOX edge {w - 1, 0, 0, w, h, 1};
                        graphics.context->CopySubresourceRegion(padded.get(), 0, w, 0, 0, input, 0, &edge);
                    }
                    if (h & 1) {
                        const D3D11_BOX edge {0, h - 1, 0, w, h, 1};
                        graphics.context->CopySubresourceRegion(padded.get(), 0, 0, h, 0, input, 0, &edge);
                    }
                    if ((w & 1) && (h & 1)) {
                        const D3D11_BOX corner {w - 1, h - 1, 0, w, h, 1};
                        graphics.context->CopySubresourceRegion(padded.get(), 0, w, h, 0, input, 0, &corner);
                    }
                    input = padded.get();
                }
                try {
                    writer->video(converter, input, nextVideo, interval);
                } catch (const Failure&) {
                    // Transform metadata is only diagnostic: some vendors do
                    // not expose a hardware URL even on the GPU input path.
                    if (result.frames || !writer->usesGpu()) throw;
                    writer.reset();
                    writer = std::make_unique<Writer>(graphics, options, temporary, false);
                    result.encoder = writer->encoder;
                    result.hardware = writer->actualHardware;
                    writer->video(converter, input, nextVideo, interval);
                }
                ++result.frames;
                nextVideo = std::max(nextVideo + interval, elapsed / interval * interval);
            }
            if (!paused && withAudio) {
                // Give both endpoints 100 ms to deliver their timestamped packets.
                const qint64 target = std::max<qint64>(0, elapsed * kAudioRate / kSecond - kAudioRate / 10);
                audioUntil(target);
            }
            result.elapsedMs = elapsed / 10000;
            if (elapsed - lastUpdate >= kSecond / 4) {
                result.bytes = QFileInfo(temporary).size();
                if (progress) progress(result);
                lastUpdate = elapsed;
            }
            // No input polling: Raw Input delivers clicks on its own thread.
            // Wake for the next video/audio deadline, or check stop within 16 ms.
            const qint64 remaining = nextVideo - clock.elapsed(nowTicks());
            const qint64 sleep = paused ? 25 : std::clamp<qint64>(remaining / 10000, 1, withAudio ? 5 : 16);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
        }
        const qint64 end = clock.elapsed(nowTicks());
        result.elapsedMs = end / 10000;
        if (withAudio && result.frames && !control.discard) {
            if (!paused)
                for (const auto& input : audio) input->collect(clock, nowTicks(), false);
            audioUntil(end * kAudioRate / kSecond);
        }
        source.reset();
        pointer.reset();
        audio.clear();
        if (result.frames && !control.discard) check(writer->finish(), "finalize MP4");
        finalized = true;
        writer.reset();
        graphics.context->ClearState();
        graphics.context->Flush();
        if (const auto dxgi = graphics.device.try_as<IDXGIDevice3>()) dxgi->Trim();
    } catch (const Failure& failure) {
        result.error = QCoreApplication::translate("Recording", "录屏失败：%1（0x%2）").arg(failure.stage).arg(quint32(failure.hr), 8, 16, QChar(u'0'));
    } catch (const winrt::hresult_error& failure) {
        result.error = QCoreApplication::translate("Recording", "录屏失败：%1（0x%2）").arg(QString::fromWCharArray(failure.message().c_str()))
            .arg(quint32(failure.code().value), 8, 16, QChar(u'0'));
    } catch (const std::exception& failure) {
        result.error = QCoreApplication::translate("Recording", "录屏失败：%1").arg(QString::fromUtf8(failure.what()));
    }
    // Nothing pressed lives on past the recording (through a GIF's export):
    // what the hook handed over since the last frame goes, and anything later.
    if (options.keyInput) options.keyInput->discardBefore(std::numeric_limits<qint64>::max());
    writer.reset();
    if (!result.frames && control.stop) result.error.clear(); // cancelled during initialization
    if (control.discard || !result.frames) {
        QFile::remove(temporary); // only this call's UUID-named, reserved file
    } else if (finalized && QFile::rename(temporary, options.path)) {
        result.path = options.path;
        result.bytes = QFileInfo(result.path).size();
    } else {
        result.path = temporary;
        if (result.error.isEmpty()) result.error = QCoreApplication::translate("Recording", "无法完成保存，录制文件保留在：%1").arg(temporary);
    }
    return result;
}

} // namespace

Result record(const Options& options, Control& control, const std::function<void(const Progress&)>& progress)
{
    return run(options, control, progress, -1);
}

Result sample(const Options& options, int milliseconds, const std::function<void(const Progress&)>& progress)
{
    Control control;
    return sample(options, milliseconds, control, progress);
}

Result sample(const Options& options, int milliseconds, Control& control, const std::function<void(const Progress&)>& progress)
{
    return run(options, control, progress, std::max(1, milliseconds));
}

} // namespace ws::capture::recording

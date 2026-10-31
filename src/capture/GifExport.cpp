#include "GifExport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryFile>

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace ws::capture::recording {
namespace {

QString message(const char* text)
{
    return QCoreApplication::translate("GifExport", text);
}

// Lock2D returns the displayed top row even when the memory is bottom-up.
// A contiguous RGB buffer instead needs its negative stride applied first.
HRESULT imageFromSample(IMFSample* sample, IMFMediaType* type, const QSize& size, QImage& image)
{
    ComPtr<IMFMediaBuffer> buffer;
    DWORD count = 0;
    sample->GetBufferCount(&count);
    HRESULT hr = count == 1 ? sample->GetBufferByIndex(0, &buffer) : sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr))
        return hr;
    ComPtr<IMF2DBuffer> twoD;
    BYTE* top = nullptr;
    LONG stride = 0;
    bool locked2D = SUCCEEDED(buffer.As(&twoD)) && SUCCEEDED(twoD->Lock2D(&top, &stride));
    DWORD length = 0;
    if (!locked2D) {
        hr = buffer->Lock(&top, nullptr, &length);
        if (FAILED(hr))
            return hr;
        stride = LONG(::MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, UINT32(size.width() * 4)));
    }
    const auto unlock = qScopeGuard([&] {
        if (locked2D)
            twoD->Unlock2D();
        else
            buffer->Unlock();
    });
    const qint64 pitch = std::abs(qint64(stride));
    if (pitch < qint64(size.width()) * 4 || (!locked2D && pitch * size.height() > length))
        return E_UNEXPECTED;
    if (!locked2D && stride < 0)
        top += pitch * (size.height() - 1);
    image = QImage(size, QImage::Format_RGB32);
    if (image.isNull())
        return E_OUTOFMEMORY;
    for (int y = 0; y < size.height(); ++y)
        std::memcpy(image.scanLine(y), top + qint64(y) * stride, size_t(size.width()) * 4);
    return S_OK;
}

HRESULT loopForever(IWICBitmapEncoder* encoder)
{
    ComPtr<IWICMetadataQueryWriter> metadata;
    HRESULT hr = encoder->GetMetadataQueryWriter(&metadata);
    if (FAILED(hr))
        return hr;
    // WIC's application data includes the GIF sub-block's byte count.
    std::array<BYTE, 11> application {'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'};
    std::array<BYTE, 5> data {3, 1, 0, 0, 0};
    PROPVARIANT value {};
    value.vt = VT_VECTOR | VT_UI1;
    value.caub = {ULONG(application.size()), application.data()};
    hr = metadata->SetMetadataByName(L"/appext/Application", &value);
    if (FAILED(hr))
        return hr;
    value.caub = {ULONG(data.size()), data.data()};
    return metadata->SetMetadataByName(L"/appext/Data", &value);
}

QRect changedArea(const QImage& current, const QImage& previous)
{
    if (previous.isNull())
        return current.rect();
    int left = current.width(), top = current.height(), right = -1, bottom = -1;
    for (int y = 0; y < current.height(); ++y) {
        const auto* a = reinterpret_cast<const QRgb*>(current.constScanLine(y));
        const auto* b = reinterpret_cast<const QRgb*>(previous.constScanLine(y));
        int first = 0;
        while (first < current.width() && (a[first] & 0xFFFFFF) == (b[first] & 0xFFFFFF))
            ++first;
        if (first == current.width())
            continue;
        int last = current.width() - 1;
        while (last > first && (a[last] & 0xFFFFFF) == (b[last] & 0xFFFFFF))
            --last;
        left = std::min(left, first);
        right = std::max(right, last);
        top = std::min(top, y);
        bottom = y;
    }
    // An unchanged frame needs only one pixel to carry its delay. Keeping
    // each timestamp also preserves very long still sections without a
    // 16-bit GIF delay overflowing or progress appearing stuck.
    return right < left ? QRect(0, 0, 1, 1) : QRect(QPoint(left, top), QPoint(right, bottom));
}

HRESULT writeFrame(
    IWICImagingFactory* factory, IWICBitmapEncoder* encoder, const QImage& source, const QRect& area, int delay)
{
    const QImage image = area == source.rect() ? source : source.copy(area);
    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = factory->CreateBitmapFromMemory(UINT(image.width()), UINT(image.height()), GUID_WICPixelFormat32bppBGR,
        UINT(image.bytesPerLine()), UINT(image.sizeInBytes()), const_cast<BYTE*>(image.constBits()), &bitmap);
    if (FAILED(hr))
        return hr;
    ComPtr<IWICPalette> palette;
    hr = factory->CreatePalette(&palette);
    if (FAILED(hr))
        return hr;
    hr = palette->InitializeFromBitmap(bitmap.Get(), 256, FALSE);
    if (FAILED(hr))
        return hr;
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr))
        return hr;
    hr = converter->Initialize(bitmap.Get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeErrorDiffusion,
        palette.Get(), 0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
        return hr;
    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr))
        return hr;
    hr = frame->Initialize(nullptr);
    if (FAILED(hr))
        return hr;
    hr = frame->SetSize(UINT(image.width()), UINT(image.height()));
    if (FAILED(hr))
        return hr;
    WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr) || format != GUID_WICPixelFormat8bppIndexed)
        return FAILED(hr) ? hr : E_UNEXPECTED;
    hr = frame->SetPalette(palette.Get());
    if (FAILED(hr))
        return hr;
    ComPtr<IWICMetadataQueryWriter> metadata;
    hr = frame->GetMetadataQueryWriter(&metadata);
    if (FAILED(hr))
        return hr;
    PROPVARIANT value {};
    value.vt = VT_UI2;
    value.uiVal = USHORT(area.x());
    hr = metadata->SetMetadataByName(L"/imgdesc/Left", &value);
    if (FAILED(hr))
        return hr;
    value.uiVal = USHORT(area.y());
    hr = metadata->SetMetadataByName(L"/imgdesc/Top", &value);
    if (FAILED(hr))
        return hr;
    value.uiVal = USHORT(std::clamp(delay, 2, int(std::numeric_limits<USHORT>::max())));
    hr = metadata->SetMetadataByName(L"/grctlext/Delay", &value);
    if (FAILED(hr))
        return hr;
    value.vt = VT_UI1;
    value.bVal = 1; // retain previous pixels outside this opaque update rectangle
    hr = metadata->SetMetadataByName(L"/grctlext/Disposal", &value);
    if (FAILED(hr))
        return hr;
    hr = frame->WriteSource(converter.Get(), nullptr);
    if (FAILED(hr))
        return hr;
    return frame->Commit();
}

} // namespace

QSize gifSize(QSize source, int maximumEdge)
{
    const int edge = std::clamp(maximumEdge, 64, 4096);
    if (source.width() > edge || source.height() > edge)
        source.scale(edge, edge, Qt::KeepAspectRatio);
    return source;
}

Result exportGif(const QString& video, const GifOptions& options, Control& control,
    const std::function<void(const Progress&)>& progress)
{
    Result result;
    if (control.discard)
        return result;
    if (QFileInfo::exists(options.path)) {
        result.error = message("文件已存在，请选择其他保存名称。");
        return result;
    }
    QTemporaryFile temporary(QDir(options.temporaryDirectory.isEmpty() ? QFileInfo(options.path).absolutePath()
        : options.temporaryDirectory).filePath(u".winshun-gif-XXXXXX"_s));
    if (!temporary.open()) {
        result.error = message("无法创建 GIF 文件，请检查保存位置和剩余空间。");
        return result;
    }
    const QString path = temporary.fileName();
    temporary.close();
    const auto encode = [&]() -> HRESULT {
        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr))
            return hr;
        const auto uninit = qScopeGuard([] { ::CoUninitialize(); });
        hr = ::MFStartup(MF_VERSION);
        if (FAILED(hr))
            return hr;
        const auto shutdown = qScopeGuard([] { ::MFShutdown(); });
        ComPtr<IMFAttributes> attributes;
        hr = ::MFCreateAttributes(&attributes, 1);
        if (FAILED(hr))
            return hr;
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        hr = ::MFCreateSourceReaderFromURL(reinterpret_cast<const wchar_t*>(video.utf16()), attributes.Get(), &reader);
        if (FAILED(hr))
            return hr;
        reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        ComPtr<IMFMediaType> requested;
        ::MFCreateMediaType(&requested);
        if (!requested)
            return E_OUTOFMEMORY;
        requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, requested.Get());
        if (FAILED(hr))
            return hr;
        ComPtr<IMFMediaType> type;
        hr = reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
        if (FAILED(hr))
            return hr;
        UINT32 width = 0, height = 0;
        hr = ::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(hr) || !width || !height || width > 32768 || height > 32768)
            return E_UNEXPECTED;
        const QSize source {int(width), int(height)};
        const QSize size = gifSize(source, options.maximumEdge);
        QSize decodedSize = source;
        QRect display(QPoint(), source);
        qint64 duration = 0;
        PROPVARIANT value {};
        if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value))) {
            if (value.vt == VT_UI8)
                duration = qint64(value.uhVal.QuadPart);
            ::PropVariantClear(&value);
        }
        ComPtr<IWICImagingFactory> factory;
        hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr))
            return hr;
        ComPtr<IWICStream> stream;
        hr = factory->CreateStream(&stream);
        if (FAILED(hr))
            return hr;
        hr = stream->InitializeFromFilename(reinterpret_cast<const wchar_t*>(path.utf16()), GENERIC_WRITE);
        if (FAILED(hr))
            return hr;
        ComPtr<IWICBitmapEncoder> encoder;
        hr = factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder);
        if (FAILED(hr))
            return hr;
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (FAILED(hr))
            return hr;
        hr = loopForever(encoder.Get());
        if (FAILED(hr))
            return hr;
        const qint64 interval = 10000000 / std::clamp(options.fps, 5, 50);
        QImage pending;
        QRect pendingArea;
        qint64 first = -1, pendingTime = 0, next = 0, end = 0, writtenCs = 0;
        const auto append = [&](qint64 until) -> HRESULT {
            const qint64 targetCs = (until + 50000) / 100000;
            const int delay = int(std::clamp(targetCs - writtenCs, qint64(2), qint64(65535)));
            const HRESULT added = writeFrame(factory.Get(), encoder.Get(), pending, pendingArea, delay);
            if (SUCCEEDED(added)) {
                writtenCs += delay;
                ++result.frames;
                result.elapsedMs = until / 10000;
                result.bytes = QFileInfo(path).size();
                result.percent = duration > 0 ? int(std::min(qint64(99), until * 100 / duration)) : -1;
                if (progress)
                    progress(result);
            }
            return added;
        };
        while (!control.discard) {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            hr = reader->ReadSample(
                DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &timestamp, &sample);
            if (FAILED(hr))
                return hr;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                // H.264 decoders may pad 360 to 368 rows. The media type's
                // aperture describes the original picture within that buffer.
                type.Reset();
                hr = reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
                if (FAILED(hr))
                    return hr;
                UINT32 currentWidth = 0, currentHeight = 0;
                hr = ::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &currentWidth, &currentHeight);
                if (FAILED(hr) || currentWidth < width || currentHeight < height || currentWidth > width + 64
                    || currentHeight > height + 64)
                    return MF_E_INVALIDMEDIATYPE;
                decodedSize = QSize(int(currentWidth), int(currentHeight));
                MFVideoArea aperture {};
                UINT32 bytes = 0;
                if (SUCCEEDED(type->GetBlob(
                        MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture), sizeof aperture, &bytes))
                    && bytes == sizeof aperture && QSize(aperture.Area.cx, aperture.Area.cy) == source)
                    display.moveTopLeft(QPoint(aperture.OffsetX.value, aperture.OffsetY.value));
                if (!QRect(QPoint(), decodedSize).contains(display))
                    return MF_E_INVALIDMEDIATYPE;
            }
            if (sample) {
                if (first < 0)
                    first = timestamp;
                const qint64 at = std::max(qint64(0), timestamp - first);
                LONGLONG sampleDuration = interval;
                sample->GetSampleDuration(&sampleDuration);
                end = std::max(end, at + sampleDuration);
                if (pending.isNull() || at >= next) {
                    QImage current;
                    hr = imageFromSample(sample.Get(), type.Get(), decodedSize, current);
                    if (FAILED(hr))
                        return hr;
                    if (display != current.rect())
                        current = current.copy(display);
                    if (current.size() != size)
                        current = current.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    const QRect area = changedArea(current, pending);
                    if (!pending.isNull()) {
                        hr = append(at);
                        if (FAILED(hr))
                            return hr;
                    }
                    pending = std::move(current);
                    pendingArea = area;
                    pendingTime = at;
                    next = ((at / interval) + 1) * interval;
                }
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                break;
        }
        if (control.discard)
            return E_ABORT;
        if (pending.isNull())
            return MF_E_END_OF_STREAM;
        hr = append(std::max({end, duration, pendingTime + interval}));
        if (FAILED(hr))
            return hr;
        return encoder->Commit();
    };
    const HRESULT hr = encode(); // release all native file handles before renaming
    if (control.discard)
        return {};
    if (FAILED(hr)) {
        result.error = message("无法生成 GIF（错误 %1）。").arg(quint32(hr), 8, 16, QChar(u'0'));
        return result;
    }
    // QTemporaryFile keeps its native handle across close(); rename through
    // it so Qt releases that handle. QFile::rename never replaces a target.
    if (!temporary.rename(options.path)) {
        result.error = message("无法保存 GIF，请检查文件是否已存在或磁盘是否已满。");
        return result;
    }
    temporary.setAutoRemove(false);
    result.path = options.path;
    result.bytes = QFileInfo(result.path).size();
    result.percent = 100;
    return result;
}

} // namespace ws::capture::recording

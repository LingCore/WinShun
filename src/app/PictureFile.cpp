#include "PictureFile.h"

#include <QTransform>

#include <windows.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <climits>
#include <string>

using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace ws {

namespace {

// The first frame, decoded by Windows Imaging Component and turned upright
// as the camera says (EXIF orientation); at most `largest` pixels on its
// longer side. `original`: its size as stored.
QImage decodeFrame(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, QSize* original, int largest, int number,
    int* frames, qint64 maxPixels = 0)
{
    UINT count = 0;
    if (FAILED(decoder->GetFrameCount(&count)) || number < 0 || static_cast<UINT>(number) >= count || count > INT_MAX)
        return {};
    if (frames)
        *frames = static_cast<int>(count);
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(static_cast<UINT>(number), &frame)))
        return {};
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0)
        return {};
    if (maxPixels > 0 && static_cast<qint64>(width) * height > maxPixels) {
        if (original)
            *original = QSize(static_cast<int>(std::min<UINT>(width, INT_MAX)),
                static_cast<int>(std::min<UINT>(height, INT_MAX)));
        return {};
    }
    USHORT orientation = 1;
    ComPtr<IWICMetadataQueryReader> metadata;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata))) {
        PROPVARIANT value;
        ::PropVariantInit(&value);
        // The property system's name reads it from JPEG, TIFF and HEIF alike.
        if (SUCCEEDED(metadata->GetMetadataByName(L"System.Photo.Orientation", &value)) && value.vt == VT_UI2)
            orientation = value.uiVal;
        ::PropVariantClear(&value);
    }
    ComPtr<IWICBitmapSource> source = frame;
    UINT w = width;
    UINT h = height;
    if (largest > 0 && std::max(width, height) > static_cast<UINT>(largest)) {
        const double scale = static_cast<double>(largest) / std::max(width, height);
        w = std::max(1u, static_cast<UINT>(width * scale + 0.5));
        h = std::max(1u, static_cast<UINT>(height * scale + 0.5));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler))
            || FAILED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeHighQualityCubic)))
            return {};
        source = scaler;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
            0, WICBitmapPaletteTypeMedianCut)))
        return {};
    QImage image(static_cast<int>(w), static_cast<int>(h), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()
        || FAILED(converter->CopyPixels(
            nullptr, static_cast<UINT>(image.bytesPerLine()), static_cast<UINT>(image.sizeInBytes()), image.bits())))
        return {};
    if (original)
        *original = QSize(static_cast<int>(width), static_cast<int>(height));
    switch (orientation) {
    case 2:
        return image.flipped(Qt::Horizontal);
    case 3:
        return image.flipped(Qt::Horizontal | Qt::Vertical);
    case 4:
        return image.flipped(Qt::Vertical);
    case 5:
        return image.transformed(QTransform().rotate(90)).flipped(Qt::Horizontal);
    case 6:
        return image.transformed(QTransform().rotate(90));
    case 7:
        return image.transformed(QTransform().rotate(90)).flipped(Qt::Vertical);
    case 8:
        return image.transformed(QTransform().rotate(270));
    default:
        return image;
    }
}

} // namespace

QImage decodePictureFile(const QString& path, int largest, QSize* original, int number, int* frames)
{
    QString native = path;
    native.replace(u'/', u'\\');
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return {};
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(
            native.toStdWString().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return {};
    return decodeFrame(wic.Get(), decoder.Get(), original, largest, number, frames);
}

QImage decodePictureData(
    const QByteArray& bytes, int largest, QSize* original, int number, int* frames, qint64 maxPixels)
{
    if (bytes.isEmpty() || bytes.size() > UINT_MAX)
        return {};
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return {};
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream))
        || FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<char*>(bytes.constData())), static_cast<DWORD>(bytes.size()))))
        return {};
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
        return {};
    return decodeFrame(wic.Get(), decoder.Get(), original, largest, number, frames, maxPixels);
}

} // namespace ws

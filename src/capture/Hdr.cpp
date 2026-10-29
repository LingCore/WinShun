#include "Hdr.h"

#include <QDebug>
#include <QDir>
#include <QScopeGuard>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <vector>
#include <cwchar>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace ws::capture {

// Whether the monitor (by its GDI name, "\\.\DISPLAY1") shows HDR, and at
// which SDR white level Windows composes SDR content there.
DisplayColorMode displayColorMode(const wchar_t* device)
{
    DisplayColorMode mode;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return mode;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return mode;
    for (UINT32 i = 0; i < pathCount; ++i) {
        const DISPLAYCONFIG_PATH_INFO& path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || std::wcscmp(source.viewGdiDeviceName, device) != 0)
            continue;
        // Windows 11 24H2 tells HDR from wide colour gamut; before it, the
        // older call's "advanced colour" on a non-WCG monitor means HDR.
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 info2 {};
        info2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
        info2.header.size = sizeof info2;
        info2.header.adapterId = path.targetInfo.adapterId;
        info2.header.id = path.targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) {
            mode.hdr = info2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
        } else {
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info {};
            info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
            info.header.size = sizeof info;
            info.header.adapterId = path.targetInfo.adapterId;
            info.header.id = path.targetInfo.id;
            if (::DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS)
                mode.hdr = info.advancedColorEnabled && !info.wideColorEnforced;
        }
        // In thousandths of 80 nits (scRGB 1.0).
        DISPLAYCONFIG_SDR_WHITE_LEVEL white {};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof white;
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
            mode.sdrWhiteNits = white.SDRWhiteLevel / 1000.0 * 80.0;
        break;
    }
    return mode;
}

float halfToFloat(std::uint16_t h)
{
    const std::uint32_t sign = (h & 0x8000u) << 16;
    std::uint32_t exp = (h >> 10) & 0x1Fu;
    std::uint32_t mant = h & 0x3FFu;
    std::uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else { // a subnormal half: normalised
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f = 0;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

std::uint8_t encodeSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    const float v = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(v * 255.0f));
}

QImage mapScRgbToSdr(const std::uint16_t* pixels, int width, int height, int rowBytes, double sdrWhiteNits)
{
    if (!pixels || width <= 0 || height <= 0)
        return {};
    const float scale = static_cast<float>(80.0 / std::max(sdrWhiteNits, 1.0));
    const auto row = [&](int y) {
        return reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(pixels) + std::size_t(rowBytes) * y);
    };
    float brightest = 0;
    for (int y = 0; y < height; ++y) {
        const std::uint16_t* p = row(y);
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c)
                brightest = std::max(brightest, halfToFloat(p[x * 4 + c]) * scale);
        }
    }
    constexpr float knee = 0.8f;
    const bool roll = brightest > 1.0f + 1e-4f;
    // The roll-off's white point, in units of the room above the knee.
    const float wp = (brightest - knee) / (1.0f - knee);
    // The sRGB curve's pow per pixel is the cost: about 150 ms for
    // 2560x1600 in stage 0, paid only on HDR monitors.
    QImage out(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        const std::uint16_t* p = row(y);
        auto* o = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < width; ++x) {
            // Out-of-gamut (negative) components have no sRGB value: clipped.
            float r = std::max(0.0f, halfToFloat(p[x * 4]) * scale);
            float g = std::max(0.0f, halfToFloat(p[x * 4 + 1]) * scale);
            float b = std::max(0.0f, halfToFloat(p[x * 4 + 2]) * scale);
            const float m = std::max({r, g, b});
            if (roll && m > knee) {
                const float t = (m - knee) / (1.0f - knee);
                const float mapped = knee + (1.0f - knee) * (t * (1.0f + t / (wp * wp)) / (1.0f + t));
                const float f = mapped / m;
                r *= f;
                g *= f;
                b *= f;
            }
            o[x] = qRgb(encodeSrgb(r), encodeSrgb(g), encodeSrgb(b));
        }
    }
    return out;
}

std::uint16_t floatToHalf(float value)
{
    // Round to nearest even, as in Fabian Giesen's float_to_half_fast3_rtne.
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    const std::uint32_t sign = bits & 0x8000'0000u;
    bits ^= sign;
    std::uint16_t half = 0;
    if (bits >= (127u + 16) << 23) { // 65536 or more: infinity; NaN stays one
        half = bits > 0x7F80'0000u ? 0x7E00 : 0x7C00;
    } else if (bits < (127u - 14) << 23) { // a subnormal half or zero: the FPU rounds it in place
        constexpr std::uint32_t kMagicBits = ((127u - 15) + (23 - 10) + 1) << 23;
        float magic = 0;
        std::memcpy(&magic, &kMagicBits, sizeof magic);
        float f = 0;
        std::memcpy(&f, &bits, sizeof f);
        f += magic;
        std::memcpy(&bits, &f, sizeof bits);
        half = static_cast<std::uint16_t>(bits - kMagicBits);
    } else {
        const std::uint32_t odd = (bits >> 13) & 1;
        bits -= (127u - 15) << 23;
        bits += 0xFFF + odd;
        half = static_cast<std::uint16_t>(bits >> 13);
    }
    return static_cast<std::uint16_t>(half | (sign >> 16));
}

float decodeSrgb(std::uint8_t encoded)
{
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t {};
        for (int i = 0; i < 256; ++i) {
            const float v = i / 255.0f;
            t[i] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table[encoded];
}

HdrPixels HdrPixels::fromScRgb(const std::uint16_t* pixels, int width, int height, int rowBytes, double sdrWhiteNits)
{
    HdrPixels out;
    if (!pixels || width <= 0 || height <= 0)
        return out;
    out.width = width;
    out.height = height;
    out.sdrWhiteNits = sdrWhiteNits;
    out.rgba.resize(std::size_t(width) * std::size_t(height) * 4);
    const std::size_t row = std::size_t(width) * 4;
    for (int y = 0; y < height; ++y) {
        const auto* from = reinterpret_cast<const std::uint8_t*>(pixels) + std::size_t(rowBytes) * std::size_t(y);
        std::memcpy(out.rgba.data() + row * std::size_t(y), from, row * sizeof(std::uint16_t));
    }
    return out;
}

HdrPixels HdrPixels::copy(const QRect& rect) const
{
    const QRect inside = rect & QRect(0, 0, width, height);
    if (isNull() || inside != rect || rect.isEmpty())
        return {};
    return fromScRgb(rgba.data() + (std::size_t(rect.y()) * std::size_t(width) + std::size_t(rect.x())) * 4, rect.width(),
        rect.height(), width * 4 * static_cast<int>(sizeof(std::uint16_t)), sdrWhiteNits);
}

void paintSdrOver(HdrPixels& hdr, const QImage& plain, const QImage& drawn)
{
    if (hdr.isNull() || plain.size() != QSize(hdr.width, hdr.height) || drawn.size() != plain.size())
        return;
    const QImage before = plain.convertToFormat(QImage::Format_RGB32);
    const QImage after = drawn.convertToFormat(QImage::Format_RGB32);
    const float white = static_cast<float>(std::max(hdr.sdrWhiteNits, 1.0) / 80.0);
    constexpr std::uint16_t kOne = 0x3C00; // 1.0: opaque
    for (int y = 0; y < hdr.height; ++y) {
        const auto* b = reinterpret_cast<const QRgb*>(before.constScanLine(y));
        const auto* a = reinterpret_cast<const QRgb*>(after.constScanLine(y));
        std::uint16_t* p = hdr.rgba.data() + std::size_t(y) * std::size_t(hdr.width) * 4;
        for (int x = 0; x < hdr.width; ++x, p += 4) {
            if (((a[x] ^ b[x]) & 0x00FF'FFFFu) == 0)
                continue;
            p[0] = floatToHalf(decodeSrgb(static_cast<std::uint8_t>(qRed(a[x]))) * white);
            p[1] = floatToHalf(decodeSrgb(static_cast<std::uint8_t>(qGreen(a[x]))) * white);
            p[2] = floatToHalf(decodeSrgb(static_cast<std::uint8_t>(qBlue(a[x]))) * white);
            p[3] = kOne;
        }
    }
}

bool writeJxr(const QString& path, const HdrPixels& pixels)
{
    if (pixels.isNull())
        return false;
    // RPC_E_CHANGED_MODE: the thread has COM already (an STA, Qt's GUI thread), which WIC takes too.
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto uninit = qScopeGuard([init] {
        if (SUCCEEDED(init))
            ::CoUninitialize();
    });
    const QString native = QDir::toNativeSeparators(path);
    const auto* name = reinterpret_cast<const wchar_t*>(native.utf16());
    const auto encode = [&]() -> HRESULT {
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        ComPtr<IWICStream> stream;
        if (SUCCEEDED(hr))
            hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr))
            hr = stream->InitializeFromFilename(name, GENERIC_WRITE);
        ComPtr<IWICBitmapEncoder> encoder;
        if (SUCCEEDED(hr))
            hr = factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &encoder);
        if (SUCCEEDED(hr))
            hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (SUCCEEDED(hr))
            hr = encoder->CreateNewFrame(&frame, &options);
        if (SUCCEEDED(hr)) {
            PROPBAG2 option {};
            option.pstrName = const_cast<LPOLESTR>(L"Lossless");
            VARIANT value;
            ::VariantInit(&value);
            value.vt = VT_BOOL;
            value.boolVal = VARIANT_TRUE;
            hr = options->Write(1, &option, &value);
        }
        if (SUCCEEDED(hr))
            hr = frame->Initialize(options.Get());
        if (SUCCEEDED(hr))
            hr = frame->SetSize(UINT(pixels.width), UINT(pixels.height));
        WICPixelFormatGUID format = GUID_WICPixelFormat64bppRGBAHalf;
        if (SUCCEEDED(hr))
            hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && !::IsEqualGUID(format, GUID_WICPixelFormat64bppRGBAHalf))
            hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT; // it would want them converted: not as they are
        const UINT stride = UINT(pixels.width) * 4 * sizeof(std::uint16_t);
        if (SUCCEEDED(hr)) {
            hr = frame->WritePixels(UINT(pixels.height), stride, stride * UINT(pixels.height),
                reinterpret_cast<BYTE*>(const_cast<std::uint16_t*>(pixels.rgba.data())));
        }
        if (SUCCEEDED(hr))
            hr = frame->Commit();
        if (SUCCEEDED(hr))
            hr = encoder->Commit();
        return hr;
    };
    const HRESULT hr = encode();
    if (FAILED(hr)) {
        ::DeleteFileW(name); // no half a file
        qWarning().noquote() << u"Screen capture: no HDR file at %1 (0x%2)"_s.arg(native).arg(static_cast<quint32>(hr), 8, 16, QChar(u'0'));
        return false;
    }
    return true;
}

} // namespace ws::capture

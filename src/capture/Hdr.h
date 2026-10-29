#pragma once

#include <QImage>
#include <QRect>
#include <QString>

#include <cstdint>
#include <vector>

namespace ws::capture {

struct DisplayColorMode {
    bool hdr = false;
    double sdrWhiteNits = 80;
};

// GDI display device name, as returned by MONITORINFOEX.
DisplayColorMode displayColorMode(const wchar_t* device);

// An HDR monitor's frame as Windows.Graphics.Capture gives it in
// R16G16B16A16Float (scRGB: linear, sRGB primaries, 1.0 = 80 nits) turned
// into an 8-bit sRGB picture, the way an SDR screen would show it.
//
// Windows composes SDR content at the monitor's SDR white level, so a white
// SDR pixel arrives as sdrWhiteNits / 80; dividing by that puts SDR content
// back at 0..1 exactly. When nothing in the frame is brighter, the result is
// the SDR picture untouched; otherwise everything above a knee is rolled
// off with the extended Reinhard curve, whose white point is the frame's
// brightest value, so the brightest highlight just reaches white and none
// clips. All three channels are scaled by one factor, which keeps the hue.
// Written from the definitions (IEC 61966-2-1, Reinhard et al. 2002);
// measured in stage 0 (capture-probe hdr: a lifted SDR frame maps back with
// no pixel changed).
//
// `pixels`: `height` rows of `width` half-float RGBA quadruples, `rowBytes`
// apart.
QImage mapScRgbToSdr(const std::uint16_t* pixels, int width, int height, int rowBytes, double sdrWhiteNits);

float halfToFloat(std::uint16_t half);
std::uint16_t floatToHalf(float value); // rounded to the nearest
std::uint8_t encodeSrgb(float linear);
float decodeSrgb(std::uint8_t encoded);

// An HDR monitor's frame kept as captured, for an HDR file beside the
// picture (GitHub #133): scRGB half floats, RGBA, rows packed.
struct HdrPixels {
    int width = 0;
    int height = 0;
    double sdrWhiteNits = 80; // as when it was taken
    std::vector<std::uint16_t> rgba; // width * height * 4

    static HdrPixels fromScRgb(const std::uint16_t* pixels, int width, int height, int rowBytes, double sdrWhiteNits);
    HdrPixels copy(const QRect& rect) const; // `rect` inside
    bool isNull() const noexcept { return rgba.empty(); }
};

// What was drawn over the picture (annotations, the mouse pointer) put on
// the HDR pixels too: where `drawn` differs from `plain` (both 8-bit, of
// the HDR pixels' size), its colour at the SDR white level, as Windows
// shows SDR content on that monitor. The rest stays as captured.
void paintSdrOver(HdrPixels& hdr, const QImage& plain, const QImage& drawn);

// A JPEG XR file of the pixels as they are: Windows' own HDR picture (Game
// Bar's), shown in HDR by Photos; through WIC, no codec of our own.
// Lossless: a screenshot's text stays sharp. Any COM apartment.
bool writeJxr(const QString& path, const HdrPixels& pixels);

} // namespace ws::capture

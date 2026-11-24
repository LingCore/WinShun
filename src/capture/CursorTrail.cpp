#include "CursorTrail.h"

#include <QColor>
#include <QImage>

#include <algorithm>
#include <array>
#include <cmath>

namespace ws::capture::recording {

namespace {

// At 60 to 200 looks a second, half a second of moves; a stall is not a leak.
constexpr size_t kMaxSamples = 256;

float lifeAt(qint64 age)
{
    return std::clamp(1.0f - float(age) / float(CursorTrail::lifetime), 0.0f, 1.0f);
}

double toLinear(double channel)
{
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

double fromLinear(double channel)
{
    return channel <= 0.0031308 ? channel * 12.92 : 1.055 * std::pow(channel, 1 / 2.4) - 0.055;
}

double luminance(const std::array<double, 3>& rgb)
{
    return 0.2126 * toLinear(rgb[0]) + 0.7152 * toLinear(rgb[1]) + 0.0722 * toLinear(rgb[2]);
}

QRgb rgbOf(const std::array<double, 3>& rgb)
{
    const auto channel = [](double value) { return int(std::lround(std::clamp(value, 0.0, 1.0) * 255)); };
    return qRgb(channel(rgb[0]), channel(rgb[1]), channel(rgb[2]));
}

} // namespace

void CursorTrail::move(QPointF tip, qint64 time)
{
    while (!m_samples.empty() && time - m_samples.front().time >= lifetime)
        m_samples.pop_front();
    if (!m_samples.empty()) {
        const QPointF step = tip - m_samples.back().at;
        if (QPointF::dotProduct(step, step) < 0.25)
            return;
    }
    if (m_samples.size() == kMaxSamples)
        m_samples.pop_front();
    m_samples.push_back({tip, time});
}

void CursorTrail::clear()
{
    m_samples.clear();
}

std::vector<CursorTrail::Point> CursorTrail::line(qint64 time) const
{
    std::vector<Point> out;
    size_t first = 0;
    while (first < m_samples.size() && time - m_samples[first].time >= lifetime)
        ++first;
    const size_t count = m_samples.size() - first;
    if (count < 2)
        return out;
    const size_t last = count - 1;
    const auto sample = [&](size_t i) { return m_samples[first + i]; };
    const auto value = [&](size_t i) {
        return std::array<float, 2> {lifeAt(time - sample(i).time), float(i) / float(last)};
    };
    const auto mid = [](QPointF a, QPointF b) { return (a + b) / 2; };
    const auto average = [](std::array<float, 2> a, std::array<float, 2> b) {
        return std::array<float, 2> {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2};
    };
    out.reserve(count * 4);
    // Through the midpoints between the samples, each sample the control
    // point of a quadratic curve: no corner where the pointer turned.
    for (size_t i = 0; i <= last; ++i) {
        const QPointF control = sample(i).at;
        const QPointF from = i == 0 ? control : mid(sample(i - 1).at, control);
        const QPointF to = i == last ? control : mid(control, sample(i + 1).at);
        const auto start = i == 0 ? value(0) : average(value(i - 1), value(i));
        const auto end = i == last ? value(last) : average(value(i), value(i + 1));
        const double length = std::hypot(control.x() - from.x(), control.y() - from.y())
            + std::hypot(to.x() - control.x(), to.y() - control.y());
        const int steps = std::clamp(int(std::ceil(length / 4)), 1, 8);
        for (int step = i == 0 ? 0 : 1; step <= steps; ++step) {
            const double t = double(step) / steps;
            const double u = 1 - t;
            const QPointF at = u * u * from + 2 * u * t * control + t * t * to;
            if (!out.empty()) {
                const QPointF gap = at - out.back().at;
                if (QPointF::dotProduct(gap, gap) < 1e-4)
                    continue;
            }
            const float f = float(t);
            out.push_back({at, start[0] + (end[0] - start[0]) * f, start[1] + (end[1] - start[1]) * f});
        }
    }
    if (out.size() < 2)
        out.clear();
    return out;
}

QRgb trailColorAt(std::span<const QRgb> stops, float place)
{
    if (stops.empty())
        return neutralTrailColors()[1];
    const float position = std::clamp(place, 0.0f, 1.0f) * float(stops.size() - 1);
    const size_t low = std::min(size_t(position), stops.size() - 1);
    const size_t high = std::min(low + 1, stops.size() - 1);
    const float t = position - float(low);
    const auto mix = [t](int a, int b) { return int(std::lround(a + (b - a) * t)); };
    return qRgb(mix(qRed(stops[low]), qRed(stops[high])), mix(qGreen(stops[low]), qGreen(stops[high])),
        mix(qBlue(stops[low]), qBlue(stops[high])));
}

std::vector<QRgb> neutralTrailColors()
{
    return {qRgb(0xA9, 0xB1, 0xBD), qRgb(0x85, 0x8F, 0x9D), qRgb(0x5D, 0x67, 0x77)};
}

std::vector<QRgb> trailColors(const QImage& pointer)
{
    const QImage image = pointer.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    struct Bucket {
        std::array<double, 3> sum {};
        int count = 0;
    };
    std::array<Bucket, 12> buckets {}; // by hue, 30° each
    int opaque = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(row[x]) < 128)
                continue;
            ++opaque;
            const QColor color = QColor::fromRgba(qUnpremultiply(row[x]));
            if (color.hsvSaturationF() <= 0.25f || color.valueF() <= 0.2f)
                continue; // black, white, grey: the outline and the shade
            auto& bucket = buckets[size_t(std::clamp(int(color.hsvHueF() * 12), 0, 11))];
            bucket.sum[0] += color.redF();
            bucket.sum[1] += color.greenF();
            bucket.sum[2] += color.blueF();
            ++bucket.count;
        }
    }
    std::vector<const Bucket*> kept;
    for (const auto& bucket : buckets)
        if (bucket.count > 0 && bucket.count * 100 >= opaque * 3)
            kept.push_back(&bucket);
    if (kept.empty())
        return neutralTrailColors();
    std::stable_sort(kept.begin(), kept.end(), [](const Bucket* a, const Bucket* b) { return a->count > b->count; });
    if (kept.size() > 4)
        kept.resize(4);
    std::vector<std::array<double, 3>> colors;
    for (const Bucket* bucket : kept)
        colors.push_back({bucket->sum[0] / bucket->count, bucket->sum[1] / bucket->count, bucket->sum[2] / bucket->count});
    std::stable_sort(colors.begin(), colors.end(), [](const auto& a, const auto& b) { return luminance(a) > luminance(b); });
    // The pointer's end, widest and most opaque, keeps to the middle
    // lightness: seen on white pages and on dark ones.
    auto& head = colors.back();
    const double light = luminance(head);
    std::array<double, 3> linear {toLinear(head[0]), toLinear(head[1]), toLinear(head[2])};
    if (light < 0.11)
        for (double& channel : linear) channel += (1 - channel) * (0.11 - light) / (1 - light);
    else if (light > 0.29)
        for (double& channel : linear) channel *= 0.29 / light;
    head = {fromLinear(linear[0]), fromLinear(linear[1]), fromLinear(linear[2])};
    std::vector<QRgb> stops;
    for (const auto& color : colors)
        stops.push_back(rgbOf(color));
    return stops;
}

} // namespace ws::capture::recording

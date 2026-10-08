#include "AuthorMark.h"

#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPainterPath>
#include <QPainterPathStroker>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace ws {

namespace {

// Geometry, timing and colours as in MacShun's AuthorMark.swift (itself taken
// from the author's《光标与稻穗-开场动画.html》).

using V = QPointF;

constexpr double kDeg = std::numbers::pi / 180;
constexpr double kMargin = 2.0; // canvas margin around the 48-unit icon
constexpr double kRestTime = 7.9; // long after the spring has settled: the static icon

const std::array<V, 4> kS1 {V(12.5, 41), V(14, 29), V(19, 15.5), V(28.5, 11)};
const std::array<V, 4> kS2 {V(28.5, 11), V(32.92, 9.09), V(36.06, 12.15), V(36.3, 21)};
constexpr double kW0 = 2.9, kW1 = 1.0; // stalk width at the root and the tip

struct Grain {
    V p;
    double angle;
    double scale;
};
const std::array<Grain, 3> kGrains {{{V(32.25, 10.70), 14, 1.8}, {V(32.25, 10.70), -65, 1.5}, {V(36.28, 20.30), -1.6, 1.5}}};
const V kCursorPos(18.6, 24.4);
constexpr double kCursorAngle = -33.0;
constexpr double kHeadUp = -28.0; // how far the empty ear points up before the grain weighs it down

using Curve = std::array<V, 4>;

double length(V v) { return std::hypot(v.x(), v.y()); }

V bez(const Curve& q, double t)
{
    const double u = 1 - t;
    return u * u * u * q[0] + 3 * u * u * t * q[1] + 3 * u * t * t * q[2] + t * t * t * q[3];
}

V dbez(const Curve& q, double t)
{
    const double u = 1 - t;
    return 3 * u * u * (q[1] - q[0]) + 6 * u * t * (q[2] - q[1]) + 3 * t * t * (q[3] - q[2]);
}

V rotate(V p, V c, double th)
{
    const double s = std::sin(th), k = std::cos(th);
    const V d = p - c;
    return V(c.x() + d.x() * k - d.y() * s, c.y() + d.x() * s + d.y() * k);
}

// The ear turns around the top of the bend, keeping the tangent where the two curves meet.
Curve bentS2(double degrees)
{
    return {kS2[0], kS2[1], rotate(kS2[2], kS2[0], degrees * 0.55 * kDeg), rotate(kS2[3], kS2[0], degrees * kDeg)};
}

std::vector<V> polyline(const Curve& q, int n)
{
    std::vector<V> points;
    points.reserve(n + 1);
    for (int i = 0; i <= n; ++i)
        points.push_back(bez(q, double(i) / n));
    return points;
}

double length(const std::vector<V>& p, std::size_t count)
{
    double sum = 0;
    for (std::size_t i = 1; i < count; ++i)
        sum += length(p[i] - p[i - 1]);
    return sum;
}

double length(const std::vector<V>& p) { return length(p, p.size()); }

double tangentDegrees(const Curve& q, double t)
{
    const V d = dbez(q, t);
    return std::atan2(d.y(), d.x()) / kDeg;
}

// A smooth curve through the points (smooth() in the web version).
void addSmooth(QPainterPath& path, const std::vector<V>& p)
{
    const std::size_t n = p.size();
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const V p0 = p[i == 0 ? 0 : i - 1], p1 = p[i], p2 = p[i + 1], p3 = p[std::min(i + 2, n - 1)];
        path.cubicTo(p1 + (p2 - p0) / 6, p2 - (p3 - p1) / 6, p2);
    }
}

struct Outline {
    QPainterPath path;
    std::vector<V> points;
};

// The stalk's outline, tapering from root to tip (stem_path() in gen.py).
Outline stemOutline(const Curve& a, const Curve& b)
{
    std::vector<std::pair<V, V>> samples;
    for (int i = 0; i <= 6; ++i)
        samples.emplace_back(bez(a, i / 6.0), dbez(a, i / 6.0));
    for (int i = 1; i <= 5; ++i)
        samples.emplace_back(bez(b, i / 5.0), dbez(b, i / 5.0));
    std::vector<V> left, right;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto [p, d] = samples[i];
        const double f = double(i) / double(samples.size() - 1);
        const double w = (kW0 * (1 - f) + kW1 * f) / 2;
        const V n = V(-d.y(), d.x()) / length(d);
        left.push_back(p + n * w);
        right.push_back(p - n * w);
    }
    std::reverse(right.begin(), right.end());
    Outline out;
    out.path.moveTo(left[0]);
    addSmooth(out.path, left);
    out.path.lineTo(right[0]); // the tip is one unit wide: a straight end looks the same as a round one
    addSmooth(out.path, right);
    out.path.closeSubpath();
    out.points = left;
    out.points.insert(out.points.end(), right.begin(), right.end());
    return out;
}

// An SVG objectBoundingBox gradient (0,0)->(1,1) in user space: the colour
// bands run along the box's diagonal.
QLinearGradient boxGradient(const QRectF& r, const QGradientStops& stops)
{
    const double w = r.width(), h = r.height(), k = 2 * w * h / (w * w + h * h);
    QLinearGradient g(r.topLeft(), QPointF(r.left() + h * k, r.top() + w * k));
    g.setStops(stops);
    return g;
}

double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }
double seg(double t, double a, double b) { return clamp01((t - a) / (b - a)); }
double outQuad(double p) { return 1 - (1 - p) * (1 - p); }
double outCubic(double p) { return 1 - std::pow(1 - p, 3); }
double inOutCubic(double p) { return p < 0.5 ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3) / 2; }
double outExpo(double p) { return p >= 1 ? 1 : 1 - std::pow(2, -10 * p); }
double outBack(double p, double s = 1.70158)
{
    return p <= 0 ? 0 : 1 + (s + 1) * std::pow(p - 1, 3) + s * std::pow(p - 1, 2);
}
double inOutSine(double p) { return -(std::cos(std::numbers::pi * p) - 1) / 2; }

constexpr double kGrowStart = 0.5, kGrowEnd = 1.1, kPopLength = 0.28;

// The shoot grows fast at first and slows down as it bends into the ear.
double growAt(double t) { return outQuad(seg(t, kGrowStart, kGrowEnd)); }

// Everything that depends only on the geometry, computed once.
struct Model {
    QPainterPath cursor, leaf, grain, seam;
    QRectF goldBox; // the grown stalk's bounds: the gold gradient stays put while it bends

    struct Pin {
        double t; // on the ear's curve
        V offset; // from that point, in the curve's tangent frame
        double angle; // relative to the tangent
        double scale;
    };
    std::array<Pin, 3> pins;
    std::array<double, 3> pops; // when each grain appears: as the growing tip passes its root
    std::vector<double> bend; // the ear's angle per millisecond: a damped spring

    double popAt(int i, double t) const { return seg(t, pops[i], pops[i] + kPopLength); }
    double bendAt(double t) const
    {
        const auto k = static_cast<std::ptrdiff_t>(std::lround(t * 1000));
        return bend[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(k, 0, std::ssize(bend) - 1))];
    }

    std::pair<V, double> grainPose(int i, const Curve& curve) const
    {
        const Pin& pin = pins[i];
        const V p = bez(curve, pin.t);
        const double phi = tangentDegrees(curve, pin.t), r = phi * kDeg;
        return {V(p.x() + pin.offset.x() * std::cos(r) - pin.offset.y() * std::sin(r),
                    p.y() + pin.offset.x() * std::sin(r) + pin.offset.y() * std::cos(r)),
            pin.angle + phi};
    }

    Model()
    {
        cursor.moveTo(-4.2, -10.4);
        cursor.lineTo(4.2, -10.4);
        cursor.moveTo(0, -10.4);
        cursor.lineTo(0, 10.4);
        cursor.moveTo(-4.2, 10.4);
        cursor.lineTo(4.2, 10.4);

        leaf.moveTo(13.4, 38.2);
        leaf.cubicTo(18.5, 35.4, 26.5, 35.2, 32.8, 38.9);
        leaf.cubicTo(26.8, 37.4, 19.8, 37.5, 13.6, 39.9);
        leaf.closeSubpath();

        grain.moveTo(0, 0);
        grain.cubicTo(2.3, 0.4, 2.2, 4.6, 0, 6.6);
        grain.cubicTo(-2.2, 4.6, -2.3, 0.4, 0, 0);
        grain.closeSubpath();

        seam.moveTo(0.5, 1.1);
        seam.cubicTo(1.1, 2.6, 0.9, 4.4, 0.2, 5.6);

        const auto points = stemOutline(kS1, kS2).points;
        const auto [minX, maxX] = std::minmax_element(points.begin(), points.end(),
            [](V a, V b) { return a.x() < b.x(); });
        const auto [minY, maxY] = std::minmax_element(points.begin(), points.end(),
            [](V a, V b) { return a.y() < b.y(); });
        goldBox = QRectF(minX->x(), minY->y(), maxX->x() - minX->x(), maxY->y() - minY->y());

        // Pin each grain to the nearest point of the ear's curve, so it moves
        // and turns with the ear when it bends.
        for (std::size_t g = 0; g < kGrains.size(); ++g) {
            double bestT = 0, bestD = INFINITY;
            for (int k = 0; k <= 2000; ++k) {
                const double d = length(bez(kS2, k / 2000.0) - kGrains[g].p);
                if (d < bestD) {
                    bestD = d;
                    bestT = k / 2000.0;
                }
            }
            const V p = bez(kS2, bestT);
            const double phi = tangentDegrees(kS2, bestT), r = -phi * kDeg;
            const V d = kGrains[g].p - p;
            pins[g] = {bestT, V(d.x() * std::cos(r) - d.y() * std::sin(r), d.x() * std::sin(r) + d.y() * std::cos(r)),
                kGrains[g].angle - phi, kGrains[g].scale};
        }

        const auto line2 = polyline(bentS2(kHeadUp), 400);
        const double l1 = length(polyline(kS1, 400)), l2 = length(line2);
        const auto when = [](double fraction) { // the time growAt reaches it
            double lo = kGrowStart, hi = kGrowEnd;
            for (int i = 0; i < 40; ++i) {
                const double m = (lo + hi) / 2;
                (growAt(m) < fraction ? lo : hi) = m;
            }
            return lo;
        };
        for (std::size_t i = 0; i < pins.size(); ++i) {
            const auto upTo = static_cast<std::size_t>(std::lround(pins[i].t * 400)) + 1;
            pops[i] = when((l1 + length(line2, upTo)) / (l1 + l2)) + (i == 1 ? 0.08 : 0);
        }

        // The grain's weight pulls the spring's rest position down.
        const double dt = 1.0 / 1000, w = 2 * std::numbers::pi * 1.6, z = 0.32;
        double total = 0;
        for (const Grain& g : kGrains)
            total += g.scale * g.scale;
        double th = kHeadUp, v = 0;
        bend.resize(8000);
        for (std::size_t k = 0; k < bend.size(); ++k) {
            const double t = double(k) * dt;
            double m = 0;
            for (int i = 0; i < 3; ++i)
                m += kGrains[i].scale * kGrains[i].scale * outCubic(popAt(i, t));
            const double target = kHeadUp * (1 - m / total);
            v += (-w * w * (th - target) - 2 * z * w * v) * dt;
            th += v * dt;
            bend[k] = th;
        }
    }
};

const Model& model()
{
    static const Model m;
    return m;
}

QColor rgb(QRgb v, double alpha = 1)
{
    QColor c = QColor::fromRgb(v);
    c.setAlphaF(static_cast<float>(alpha));
    return c;
}

QPen roundPen(const QColor& color, double width)
{
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

// An empty image matching what `painter` draws on, pixel for pixel.
QImage layerFor(const QPainter& painter)
{
    QPaintDevice* device = painter.device();
    QSize size(device->width(), device->height());
    if (device->devType() == QInternal::Image)
        size = static_cast<QImage*>(device)->size();
    else if (device->devType() == QInternal::Pixmap)
        size = static_cast<QPixmap*>(device)->size();
    QImage layer(size, QImage::Format_ARGB32_Premultiplied);
    layer.setDevicePixelRatio(device->devicePixelRatio());
    layer.fill(Qt::transparent);
    return layer;
}

} // namespace

AuthorMark::AuthorMark(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

qreal AuthorMark::margin() { return kMargin; }
qreal AuthorMark::restTime() { return kRestTime; }

void AuthorMark::setTime(qreal time)
{
    if (qFuzzyCompare(time, m_time))
        return;
    m_time = time;
    emit timeChanged();
    update();
}

void AuthorMark::setStartInk(const QColor& color)
{
    if (color == m_startInk)
        return;
    m_startInk = color;
    emit startInkChanged();
    update();
}

void AuthorMark::paint(QPainter* painter)
{
    const Model& m = model();
    const double t = m_time;
    const double unit = std::min(width(), height()) / (48 + 2 * kMargin);
    if (unit <= 0)
        return;
    // On top of the scaling the scene graph has already set up for the texture.
    const QTransform toIcon
        = QTransform::fromTranslate(kMargin * unit, kMargin * unit).scale(unit, unit) * painter->transform();

    painter->setRenderHint(QPainter::Antialiasing);
    painter->setTransform(toIcon);

    // The badge opens up from the cursor.
    const double bp = outExpo(seg(t, 0.25, 0.65)), bph = outExpo(seg(t, 0.31, 0.71));
    const double cp = seg(t, 0.28, 0.7);
    const V cursorAt = V(24, 24) + (kCursorPos - V(24, 24)) * inOutCubic(cp);
    const double cursorAngle = kCursorAngle * outBack(cp, 1.4);
    if (bp > 0) {
        const double bw = 3 + 45 * bp, bh = 23.8 + 24.2 * bph, rx = 1.5 + 9.5 * bp;
        const QRectF rect(24 - bw / 2, 24 - bh / 2, bw, bh);
        QRadialGradient fill(QPointF(14.4, 10.56), 45.6);
        fill.setColorAt(0, rgb(0xE8412F));
        fill.setColorAt(1, rgb(0xA5171A));
        const double opacity = seg(t, 0.25, 0.37);
        painter->setOpacity(opacity);
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(rect, rx, rx);
        painter->setOpacity(opacity * 0.12);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(Qt::white, 1));
        const double inner = std::max(0.0, rx - 0.5);
        painter->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), inner, inner);
        painter->setOpacity(1);
    }

    // Leaf, stalk and grains share a layer: the cursor cuts through it and
    // the gleam sweeps over it.
    if (const double g = growAt(t); g > 0) {
        const Curve curve = bentS2(m.bendAt(t));
        QImage layer = layerFor(*painter);
        {
            QPainter art(&layer);
            art.setRenderHint(QPainter::Antialiasing);
            art.setTransform(toIcon);
            art.setPen(Qt::NoPen);

            if (const double lp = seg(t, 0.58, 0.95); lp > 0) {
                const double le = outBack(lp, 1.3);
                art.save();
                art.translate(13.5, 39);
                art.rotate(-(1 - std::min(le, 1.0)) * 38);
                art.scale(le, 0.35 + 0.65 * std::min(le, 1.0));
                art.translate(-13.5, -39);
                QLinearGradient fill(QPointF(13.4, 0), QPointF(32.8, 0));
                fill.setColorAt(0, rgb(0xE9AE3A));
                fill.setColorAt(1, rgb(0xFFD877, 0.9));
                art.setBrush(fill);
                art.drawPath(m.leaf);
                art.restore();
            }

            art.save();
            if (g < 1) { // revealed along its centre line; the round end is the growing tip
                auto line = polyline(kS1, 120);
                const auto rest = polyline(curve, 120);
                line.insert(line.end(), rest.begin() + 1, rest.end());
                const double want = length(line) * g;
                QPainterPath reveal(line[0]);
                double acc = 0;
                for (std::size_t i = 1; i < line.size(); ++i) {
                    const double step = length(line[i] - line[i - 1]);
                    if (acc + step >= want) {
                        reveal.lineTo(line[i - 1] + (line[i] - line[i - 1]) * ((want - acc) / step));
                        break;
                    }
                    acc += step;
                    reveal.lineTo(line[i]);
                }
                QPainterPathStroker stroker;
                stroker.setWidth(4.6);
                stroker.setCapStyle(Qt::RoundCap);
                stroker.setJoinStyle(Qt::RoundJoin);
                art.setClipPath(stroker.createStroke(reveal));
            }
            art.setBrush(boxGradient(m.goldBox, {{0, rgb(0xFFE08A)}, {1, rgb(0xF2B53C)}}));
            art.drawPath(stemOutline(kS1, curve).path);
            art.restore();

            const QGradientStops grainStops {{0, rgb(0xFFF0B8)}, {0.55, rgb(0xFFD460)}, {1, rgb(0xEDA92E)}};
            for (int i = 0; i < 3; ++i) {
                const double p = m.popAt(i, t);
                if (p <= 0)
                    continue;
                const auto [at, angle] = m.grainPose(i, curve);
                const double k = outBack(p, 1.2) * m.pins[i].scale;
                art.save();
                art.translate(at);
                art.rotate(angle);
                art.scale(k, k);
                art.setPen(Qt::NoPen);
                art.setBrush(boxGradient(QRectF(-1.7, 0, 3.4, 6.6), grainStops));
                art.drawPath(m.grain);
                art.setBrush(Qt::NoBrush);
                art.setPen(QPen(rgb(0xB5621A, 0.5), 0.32, Qt::SolidLine, Qt::RoundCap));
                art.drawPath(m.seam);
                art.restore();
            }

            art.save();
            art.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            art.translate(cursorAt);
            art.rotate(cursorAngle);
            art.setPen(roundPen(Qt::black, 5.6));
            art.drawPath(m.cursor);
            art.restore();

            if (const double gp = seg(t, 1.5, 2.0); gp > 0 && gp < 1) {
                const double c = -0.3 + 1.6 * inOutSine(gp), w = 0.2;
                QLinearGradient gleam(QPointF(6, 6), QPointF(42, 42));
                gleam.setStops({{clamp01(c - w), QColor(255, 255, 255, 0)},
                    {clamp01(c), QColor(255, 255, 255, 217)}, {clamp01(c + w), QColor(255, 255, 255, 0)}});
                art.setCompositionMode(QPainter::CompositionMode_SourceAtop);
                art.fillRect(QRectF(-kMargin, -kMargin, 48 + 2 * kMargin, 48 + 2 * kMargin), gleam);
            }
        }
        painter->resetTransform(); // the layer covers the device exactly
        painter->drawImage(QPointF(0, 0), layer);
        painter->setTransform(toIcon);
    }

    // The cursor fades in; as the badge opens, its colour turns from the
    // background's ink to cream.
    if (const double ca = seg(t, 0, 0.1); ca > 0) {
        const QColor end(255, 0xF6, 0xE2);
        const auto mix = [bp](float a, float b) { return a + (b - a) * static_cast<float>(bp); };
        const QColor ink = QColor::fromRgbF(mix(m_startInk.redF(), end.redF()), mix(m_startInk.greenF(), end.greenF()),
            mix(m_startInk.blueF(), end.blueF()));
        painter->setOpacity(ca);
        painter->translate(cursorAt);
        painter->rotate(cursorAngle);
        painter->setPen(roundPen(ink, 3));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(m.cursor);
    }
}

} // namespace ws

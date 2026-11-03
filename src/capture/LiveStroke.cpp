#include "LiveStroke.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ws::capture::live {

namespace {

QPointF mid(QPointF a, QPointF b)
{
    return (a + b) / 2;
}

qreal length(QPointF v)
{
    return std::hypot(v.x(), v.y());
}

QPointF unit(QPointF v)
{
    const qreal l = length(v);
    return l > 1e-9 ? v / l : QPointF();
}

QPointF perp(QPointF v)
{
    return {-v.y(), v.x()};
}

struct Ink {
    unsigned char r, g, b, a;
};

Ink premultiplied(QRgb color, qreal coverage)
{
    const qreal a = qAlpha(color) / 255.0 * coverage;
    const auto c = [a](int v) { return static_cast<unsigned char>(std::lround(v * a)); };
    return {c(qRed(color)), c(qGreen(color)), c(qBlue(color)), static_cast<unsigned char>(std::lround(255 * a))};
}

void push(Mesh& mesh, QPointF p, Ink ink, qreal sx, qreal sy)
{
    mesh.vertices.push_back(Vertex {float(p.x() * sx), float(p.y() * sy), ink.r, ink.g, ink.b, ink.a});
}

void quad(Mesh& mesh, quint32 a, quint32 b, quint32 c, quint32 d)
{
    mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
}

// A quadratic curve's points after its start.
void flatten(QList<QPointF>& out, QPointF start, QPointF control, QPointF end)
{
    for (int i = 1; i <= kCurveSteps; ++i) {
        const qreal t = qreal(i) / kCurveSteps;
        const qreal u = 1 - t;
        out.append(start * (u * u) + control * (2 * u * t) + end * (t * t));
    }
}

} // namespace

QList<QPointF> centreline(const QList<QPointF>& points, qsizetype from, qsizetype to)
{
    QList<QPointF> line;
    const qsizetype n = points.size();
    to = std::min(to, pieceCount(n));
    if (from >= to)
        return line;
    if (n == 2) {
        line = {points[0], points[1]};
        return line;
    }
    // Piece j < n - 2: the curve about point j + 1, from the middle before
    // it (the first point for the first) to the middle after it. Piece
    // n - 2: straight from the last middle to the last point.
    const auto startOf = [&](qsizetype j) { return j == 0 ? points[0] : mid(points[j], points[j + 1]); };
    line.append(startOf(from));
    for (qsizetype j = from; j < to; ++j) {
        if (j < n - 2)
            flatten(line, startOf(j), points[j + 1], mid(points[j + 1], points[j + 2]));
        else
            line.append(points[n - 1]);
    }
    return line;
}

void appendDisc(Mesh& mesh, QPointF centre, qreal radius, qreal fringe, QRgb color, qreal sx, qreal sy)
{
    const qreal inner = std::max<qreal>(0, radius - fringe / 2);
    const qreal outer = radius + fringe / 2;
    const int sides = std::clamp(int(std::ceil(2 * std::numbers::pi * outer / 3)), 8, 48);
    const Ink solid = premultiplied(color, inner > 0 ? 1.0 : radius / std::max<qreal>(fringe, 1e-3));
    const Ink none = premultiplied(color, 0);
    const auto base = quint32(mesh.vertices.size());
    push(mesh, centre, solid, sx, sy);
    for (int i = 0; i < sides; ++i) {
        const qreal a = 2 * std::numbers::pi * i / sides;
        const QPointF d(std::cos(a), std::sin(a));
        push(mesh, centre + d * inner, solid, sx, sy);
        push(mesh, centre + d * outer, none, sx, sy);
    }
    for (int i = 0; i < sides; ++i) {
        const quint32 in0 = base + 1 + 2 * quint32(i);
        const quint32 in1 = base + 1 + 2 * quint32((i + 1) % sides);
        mesh.indices.insert(mesh.indices.end(), {base, in0, in1});
        quad(mesh, in0, in0 + 1, in1 + 1, in1);
    }
}

void appendBand(Mesh& mesh, const QList<QPointF>& line, qreal width, qreal fringe, QRgb color, bool roundStart,
    bool roundEnd, qreal sx, qreal sy)
{
    // The line without points on top of each other (no direction there).
    QList<QPointF> pts;
    pts.reserve(line.size());
    for (const QPointF& p : line) {
        if (pts.isEmpty() || length(p - pts.last()) > 1e-3)
            pts.append(p);
    }
    const qreal radius = width / 2;
    if (pts.size() < 2) {
        if (!pts.isEmpty() && (roundStart || roundEnd))
            appendDisc(mesh, pts[0], radius, fringe, color, sx, sy);
        return;
    }
    const qreal inner = std::max<qreal>(0, radius - fringe / 2);
    const qreal outer = radius + fringe / 2;
    const Ink solid = premultiplied(color, inner > 0 ? 1.0 : radius / std::max<qreal>(fringe, 1e-3));
    const Ink none = premultiplied(color, 0);
    const auto base = quint32(mesh.vertices.size());
    const qsizetype m = pts.size();
    constexpr qreal kSharp = 0.866; // a turn of more than 30 degrees gets a round join
    std::vector<QPointF> joins;
    for (qsizetype k = 0; k < m; ++k) {
        const QPointF before = k > 0 ? unit(pts[k] - pts[k - 1]) : QPointF();
        const QPointF after = k + 1 < m ? unit(pts[k + 1] - pts[k]) : QPointF();
        QPointF normal;
        qreal scale = 1;
        if (k == 0) {
            normal = perp(after);
        } else if (k + 1 == m) {
            normal = perp(before);
        } else {
            const QPointF sum = before + after;
            normal = length(sum) > 1e-6 ? perp(unit(sum)) : perp(before);
            const qreal cosHalf = QPointF::dotProduct(normal, perp(after));
            scale = 1 / std::max<qreal>(0.5, cosHalf); // the miter, at most twice the width
            if (QPointF::dotProduct(before, after) < kSharp)
                joins.push_back(pts[k]);
        }
        push(mesh, pts[k] + normal * (outer * scale), none, sx, sy);
        push(mesh, pts[k] + normal * (inner * scale), solid, sx, sy);
        push(mesh, pts[k] - normal * (inner * scale), solid, sx, sy);
        push(mesh, pts[k] - normal * (outer * scale), none, sx, sy);
    }
    for (qsizetype k = 0; k + 1 < m; ++k) {
        const quint32 a = base + 4 * quint32(k);
        const quint32 b = a + 4;
        quad(mesh, a, b, b + 1, a + 1);
        quad(mesh, a + 1, b + 1, b + 2, a + 2);
        quad(mesh, a + 2, b + 2, b + 3, a + 3);
    }
    for (const QPointF& p : joins)
        appendDisc(mesh, p, radius, fringe, color, sx, sy);
    if (roundStart)
        appendDisc(mesh, pts.first(), radius, fringe, color, sx, sy);
    if (roundEnd)
        appendDisc(mesh, pts.last(), radius, fringe, color, sx, sy);
}

void appendChunk(Mesh& mesh, const QList<QPointF>& points, qsizetype chunk, qreal width, qreal fringe, QRgb color,
    qreal sx, qreal sy)
{
    const qsizetype n = points.size();
    if (n == 0)
        return;
    const bool dot = std::ranges::all_of(points, [&](QPointF p) { return length(p - points[0]) < 1e-3; });
    if (dot) {
        if (chunk == 0)
            appendDisc(mesh, points[0], width / 2, fringe, color, sx, sy);
        return;
    }
    const qsizetype pieces = pieceCount(n);
    const qsizetype from = chunk * kChunk;
    const qsizetype to = std::min(pieces, from + kChunk);
    if (from >= to)
        return;
    appendBand(mesh, centreline(points, from, to), width, fringe, color, true, to == pieces, sx, sy);
}

} // namespace ws::capture::live

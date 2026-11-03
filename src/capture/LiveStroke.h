#pragma once

#include <QList>
#include <QPointF>
#include <QRgb>

#include <vector>

namespace ws::capture::live {

// The pen stroke under the mouse drawn by the GPU (GitHub #189, docs/
// design-presenter.md 4.5): the compositor strokes the whole of a pen's path
// again at each move, so on a long stroke the tiles take more and more time
// (4K, 3000 points: p95 over 4 ms). While the stroke is drawn the canvas
// leaves it out of its tiles and shows these triangles instead; let go of,
// the tiles take it as ever. The centreline is the compositor's
// (annot::strokeCentreline: quadratic curves through the points' middles),
// flattened; the band has round ends and a 1 px fringe fading to nothing
// on each side, which stands for the compositor's antialiasing.
//
// The stroke is cut into pieces (piece j: the curve about point j + 1; the
// last one the straight end to the newest point) and the pieces into
// chunks of kChunk: a chunk whose pieces no new point changes any more is
// made once; only the newest is made again at each move.

// As QSGGeometry::ColoredPoint2D: premultiplied colour.
struct Vertex {
    float x;
    float y;
    unsigned char r, g, b, a;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<quint32> indices; // triangles
    void clear()
    {
        vertices.clear();
        indices.clear();
    }
};

inline constexpr qsizetype kChunk = 128;
inline constexpr int kCurveSteps = 3; // straight parts per curve

// The pieces of a stroke through `count` points.
constexpr qsizetype pieceCount(qsizetype count) { return count < 2 ? 0 : count - 1; }
// The pieces no later point changes: all but the last (the straight end becomes a curve).
constexpr qsizetype settledPieces(qsizetype count) { return count < 3 ? 0 : count - 2; }

// The centreline of pieces [from, to), flattened (image px).
QList<QPointF> centreline(const QList<QPointF>& points, qsizetype from, qsizetype to);

// Appends a band `width` wide along `line`, its ends round when asked, and
// a disc; positions times (sx, sy), as the canvas maps image px. `fringe`
// is in image px (one device pixel: 1 in the overlay).
void appendBand(Mesh& mesh, const QList<QPointF>& line, qreal width, qreal fringe, QRgb color, bool roundStart,
    bool roundEnd, qreal sx = 1, qreal sy = 1);
void appendDisc(Mesh& mesh, QPointF centre, qreal radius, qreal fringe, QRgb color, qreal sx = 1, qreal sy = 1);

// Chunk `chunk` of the stroke: its pieces' band, a round start (the first
// chunk's cap, else the join with the chunk before) and, when it holds the
// last piece, a round end. A stroke of one point (or all at one place) is a disc.
void appendChunk(Mesh& mesh, const QList<QPointF>& points, qsizetype chunk, qreal width, qreal fringe, QRgb color,
    qreal sx = 1, qreal sy = 1);

} // namespace ws::capture::live

#include "AnnotationRender.h"

#include <QColor>
#include <QHash>
#include <QPainter>
#include <QPainterPathStroker>
#include <QRawFont>
#include <QTransform>
#include <QVarLengthArray>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <list>
#include <optional>
#include <vector>

namespace ws::capture::annot {

namespace {

// Shapes are split at every kGrid-th pixel line of their own anchor.
constexpr int kGrid = 256;

bool same(QPointF a, QPointF b)
{
    return a.x() == b.x() && a.y() == b.y();
}

// The raster engine turns coordinates into 26.6 fixed point by rounding half
// away from zero, which is not the same at -x and x: a coordinate already on
// the 1/64 grid needs no rounding and so moves by whole pixels exactly.
qreal snap64(qreal v)
{
    return std::floor(v * 64 + 0.5) / 64;
}

// The text size above which the raster engine fills glyph outlines instead of
// drawing from its glyph cache (QPaintEngineEx::shouldDrawCachedGlyphs).
int cachedGlyphLimit()
{
    static const int limit = [] {
        const int env = qEnvironmentVariableIntValue("QT_MAX_CACHED_GLYPH_SIZE");
        return env != 0 ? env : 64;
    }();
    return limit;
}

QColor softColor(QRgb color)
{
    QColor c = QColor::fromRgb(color);
    c.setAlpha(51); // 20 %
    return c;
}

// A filled shape: polygons relative to `anchor`, corners on the 1/64 grid,
// each edge inside one kGrid x kGrid cell of that anchor.
struct Shape {
    QPoint anchor;
    Qt::FillRule rule = Qt::WindingFill;
    QList<QPolygonF> polygons;
    QRectF bounds; // of the polygons, relative to the anchor
    QPainterPath path; // the polygons as they are
    qint64 bytes = 0;
};

void appendPoint(QPolygonF& polygon, QPointF p)
{
    if (polygon.isEmpty() || !same(polygon.last(), p))
        polygon.append(p);
}

template <typename Add>
void gridCrossings(qreal from, qreal to, Add add)
{
    if (from == to)
        return;
    const qreal low = std::min(from, to);
    const qreal high = std::max(from, to);
    for (qreal g = (std::floor(low / kGrid) + 1) * kGrid; g < high; g += kGrid)
        add(g, (g - from) / (to - from));
}

// Every edge cut where it crosses a multiple of kGrid in x or y; the cut
// lies exactly on that line, its other coordinate on the 1/64 grid.
QPolygonF splitAtGrid(const QPolygonF& polygon)
{
    struct Cut {
        qreal t;
        QPointF p;
    };
    QPolygonF out;
    out.reserve(polygon.size());
    const qsizetype n = polygon.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QPointF a = polygon[i];
        const QPointF b = polygon[(i + 1) % n];
        appendPoint(out, a);
        QVarLengthArray<Cut, 16> cuts;
        gridCrossings(a.x(), b.x(), [&](qreal g, qreal t) { cuts.append({t, QPointF(g, snap64(a.y() + t * (b.y() - a.y())))}); });
        gridCrossings(a.y(), b.y(), [&](qreal g, qreal t) { cuts.append({t, QPointF(snap64(a.x() + t * (b.x() - a.x())), g)}); });
        std::sort(cuts.begin(), cuts.end(), [](const Cut& l, const Cut& r) { return l.t < r.t; });
        for (const Cut& cut : cuts)
            appendPoint(out, cut.p);
    }
    while (out.size() > 1 && same(out.first(), out.last()))
        out.removeLast();
    return out;
}

void addPolygon(QPainterPath& path, const QPolygonF& polygon)
{
    path.moveTo(polygon[0]);
    for (qsizetype i = 1; i < polygon.size(); ++i)
        path.lineTo(polygon[i]);
    path.closeSubpath();
}

std::shared_ptr<const Shape> makeShape(const QPainterPath& local, QPoint anchor)
{
    auto shape = std::make_shared<Shape>();
    shape->anchor = anchor;
    shape->rule = local.fillRule();
    // Curves are flattened at four times the scale: within 1/8 px.
    const QList<QPolygonF> flat = local.toSubpathPolygons(QTransform::fromScale(4, 4));
    qsizetype points = 0;
    for (const QPolygonF& f : flat) {
        QPolygonF polygon;
        polygon.reserve(f.size());
        for (const QPointF& p : f)
            appendPoint(polygon, QPointF(snap64(p.x() / 4), snap64(p.y() / 4)));
        while (polygon.size() > 1 && same(polygon.first(), polygon.last()))
            polygon.removeLast();
        if (polygon.size() < 3)
            continue;
        polygon = splitAtGrid(polygon);
        if (polygon.size() < 3)
            continue;
        shape->bounds |= polygon.boundingRect();
        points += polygon.size();
        shape->polygons.append(polygon);
    }
    shape->path.setFillRule(shape->rule);
    for (const QPolygonF& polygon : std::as_const(shape->polygons))
        addPolygon(shape->path, polygon);
    // The polygons and the path's elements.
    shape->bytes = qint64(sizeof(Shape)) + points * qint64(sizeof(QPointF) + 24);
    return shape;
}

// The cells of the shape's grid that hold `local` (an area relative to the
// anchor), and one more cell around them.
QRectF gridWindow(const QRect& local)
{
    const qreal x0 = (std::floor(qreal(local.x()) / kGrid) - 1) * kGrid;
    const qreal y0 = (std::floor(qreal(local.y()) / kGrid) - 1) * kGrid;
    const qreal x1 = (std::ceil(qreal(local.x() + local.width()) / kGrid) + 1) * kGrid;
    const qreal y1 = (std::ceil(qreal(local.y() + local.height()) / kGrid) + 1) * kGrid;
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

// The shape with every corner pulled into `window`. An edge outside the
// window lies in a cell outside it, so it only slides along the outside
// while its corners are pulled in: nothing inside the window changes, and
// Qt is never handed a path so far outside the device that it cuts it.
QPainterPath clampedPath(const Shape& shape, const QRectF& window)
{
    const auto clamp = [&](QPointF p) {
        return QPointF(std::clamp(p.x(), window.left(), window.right()), std::clamp(p.y(), window.top(), window.bottom()));
    };
    QPainterPath path;
    path.setFillRule(shape.rule);
    for (const QPolygonF& polygon : shape.polygons) {
        path.moveTo(clamp(polygon[0]));
        for (qsizetype i = 1; i < polygon.size(); ++i)
            path.lineTo(clamp(polygon[i]));
        path.closeSubpath();
    }
    return path;
}

QPainterPath strokeOf(const QPainterPath& centre, int width)
{
    QPainterPathStroker stroker;
    stroker.setWidth(width);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    return stroker.createStroke(centre);
}

struct Op {
    enum Type { Rects, Fill, Layer, Patch };
    Type type = Rects;
    QColor color;
    QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver;
    QList<QRect> rects; // Rects: whole pixels, filled without antialiasing
    std::shared_ptr<const Shape> shape; // Fill
    QPoint origin; // Layer: the layer's top left
    QImage layer; // Layer: premultiplied, blended at whole pixels
};

// How to draw one item, made once and kept while the item, the scale and
// its number stay the same.
struct Recipe {
    Item item;
    qreal dpr = 1;
    int number = 0;
    NumberFormat format = NumberFormat::Arabic;
    QList<Op> ops;
    QRect extent; // every pixel the ops may touch
    qint64 bytes = 0;
};

void addRects(Recipe& recipe, const QList<QRect>& rects, const QColor& color)
{
    Op op;
    op.type = Op::Rects;
    op.color = color;
    for (const QRect& r : rects) {
        if (r.isEmpty())
            continue;
        op.rects.append(r);
        recipe.extent |= r;
    }
    if (op.rects.isEmpty())
        return;
    recipe.bytes += qint64(sizeof(Op)) + op.rects.size() * qint64(sizeof(QRect));
    recipe.ops.append(op);
}

void addFill(Recipe& recipe, const QPainterPath& local, QPoint anchor, const QColor& color,
    QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver)
{
    Op op;
    op.type = Op::Fill;
    op.color = color;
    op.mode = mode;
    op.shape = makeShape(local, anchor);
    if (op.shape->polygons.isEmpty())
        return;
    recipe.extent |= op.shape->bounds.translated(QPointF(anchor)).toAlignedRect().adjusted(-1, -1, 1, 1);
    recipe.bytes += qint64(sizeof(Op)) + op.shape->bytes;
    recipe.ops.append(op);
}

// A text drawn into a layer the size of its ink, which every area blends at
// whole pixels: the same pixels in every tile and in the export. It depends
// on nothing but the words and their look, so a text moved, or another item
// showing the same words, uses the same layer (3.3, 7).
struct TextLayer {
    QString key;
    QPoint offset; // the layer's top left from the text's
    QImage image; // premultiplied; null when there is nothing to draw
};

// `stroke` > 0 makes the layer of a text's outline (docs: Style::textStroke)
// instead of the words: the same glyphs' outlines, grown by `stroke` px with
// round joins, in `color`; it goes under the words' own layer.
QString textLayerKey(const QString& text, int pixelSize, bool bold, QRgb color, int stroke = 0)
{
    return QString::number(pixelSize) + (bold ? u'b' : u'n') + QString::number(color, 16)
        + (stroke > 0 ? u's' + QString::number(stroke) : QString()) + u'|' + text;
}

using TextLayers = std::function<std::shared_ptr<const TextLayer>(const QString& text, int pixelSize, bool bold, QRgb color, int stroke)>;

std::shared_ptr<const TextLayer> makeTextLayer(const QString& text, int pixelSize, bool bold, QRgb color, int stroke = 0);

// The outline layer: the words' own pixels grown by `stroke` px all round, a
// disc as the brush (round joins), with a half pixel of antialiasing at its
// edge. It is grown from the pixels the words are drawn with, not from the
// glyph outlines: the font engine hints the drawn glyphs (vertical hinting),
// so outlines stroked by QPainterPathStroker sat up to a pixel off them and
// left a thin or lopsided ring.
std::shared_ptr<const TextLayer> makeStrokeLayer(const QString& text, int pixelSize, bool bold, QRgb color, int stroke)
{
    auto made = std::make_shared<TextLayer>();
    made->key = textLayerKey(text, pixelSize, bold, color, stroke);
    const std::shared_ptr<const TextLayer> words = makeTextLayer(text, pixelSize, bold, qRgb(255, 255, 255));
    if (!words || words->image.isNull())
        return made;
    const QImage& src = words->image;
    const int w = src.width() + 2 * stroke;
    const int h = src.height() + 2 * stroke;
    std::vector<quint8> alpha(std::size_t(w) * std::size_t(h), 0);
    struct Reach {
        int dx;
        int dy;
        int weight; // 0..255
    };
    std::vector<Reach> disc;
    for (int dy = -stroke - 1; dy <= stroke + 1; ++dy) {
        for (int dx = -stroke - 1; dx <= stroke + 1; ++dx) {
            const qreal cover = std::clamp(stroke + 0.5 - std::hypot(qreal(dx), qreal(dy)), 0.0, 1.0);
            if (cover > 0)
                disc.push_back({dx, dy, int(std::lround(cover * 255))});
        }
    }
    // A pixel the words cover fully (doubled alpha 255) with all eight neighbours the same adds nothing but
    // its own place: its disc lies inside its neighbours' (every point of it but that one is nearer to one
    // of them). So only the rim of the words spreads, and the cost follows the outline, not the area.
    const auto full = [&src](int x, int y) {
        return x >= 0 && y >= 0 && x < src.width() && y < src.height()
            && qAlpha(reinterpret_cast<const QRgb*>(src.constScanLine(y))[x]) >= 128;
    };
    for (int y = 0; y < src.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            if (qAlpha(line[x]) >= 128 && full(x - 1, y - 1) && full(x, y - 1) && full(x + 1, y - 1) && full(x - 1, y)
                && full(x + 1, y) && full(x - 1, y + 1) && full(x, y + 1) && full(x + 1, y + 1)) {
                // only its own place is left to it: the neighbours' discs reach it at a lesser weight
                alpha[std::size_t(y + stroke) * std::size_t(w) + std::size_t(x + stroke)] = 255;
                continue;
            }
            // Doubled: a pixel the word covers by half or more counts as the word,
            // so that the ring stands full against the edge, not as faint as the edge pixel.
            const int a = std::min(255, 2 * qAlpha(line[x]));
            if (a == 0)
                continue;
            for (const Reach& r : disc) {
                quint8& out = alpha[std::size_t(y + stroke + r.dy) * std::size_t(w) + std::size_t(x + stroke + r.dx)];
                const int v = (a * r.weight + 127) / 255;
                if (v > out)
                    out = quint8(v);
            }
        }
    }
    QImage layer(w, h, QImage::Format_ARGB32_Premultiplied);
    if (layer.isNull())
        return made;
    const int cr = qRed(color);
    const int cg = qGreen(color);
    const int cb = qBlue(color);
    for (int y = 0; y < h; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(layer.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int a = alpha[std::size_t(y) * std::size_t(w) + std::size_t(x)];
            line[x] = qRgba((cr * a + 127) / 255, (cg * a + 127) / 255, (cb * a + 127) / 255, a);
        }
    }
    made->offset = words->offset - QPoint(stroke, stroke);
    made->image = std::move(layer);
    return made;
}

std::shared_ptr<const TextLayer> makeTextLayer(const QString& text, int pixelSize, bool bold, QRgb color, int stroke)
{
    if (stroke > 0)
        return makeStrokeLayer(text, pixelSize, bold, color, stroke);
    auto made = std::make_shared<TextLayer>();
    made->key = textLayerKey(text, pixelSize, bold, color);
    const TextLayout layout = layoutText(text, pixelSize, bold);
    if (layout.runs.isEmpty())
        return made;
    const QRect ink = layout.metrics.ink.toAlignedRect().adjusted(-2, -2, 2, 2);
    // Font engines without a glyph cache of their own (GDI, DirectWrite)
    // draw ClearType into ARGB32 and RGB32 images whenever Windows has it
    // on, whatever QFont::NoSubpixelAntialias says (QRasterPaintEngine
    // picks the glyph format by the image's); into RGBA8888 they draw grey.
    QImage layer(ink.size(), QImage::Format_RGBA8888_Premultiplied);
    if (layer.isNull())
        return made;
    layer.fill(Qt::transparent);
    const bool outlines = pixelSize > cachedGlyphLimit();
    QPainter painter(&layer);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(QColor::fromRgb(color));
    for (const QGlyphRun& run : layout.runs) {
        if (!outlines) {
            painter.drawGlyphRun(QPointF(-ink.topLeft()), run);
            continue;
        }
        // Larger text is filled from outlines. Qt's FreeType engine shares
        // one FT_Face between all sizes of a font, and drawing from the glyph
        // cache leaves the last glyph's sub-pixel offset set on that face
        // (FT_Set_Transform); outlines are loaded with it, so the same text
        // came out a level apart on some edge pixels, depending on what small
        // text was drawn before. A new engine for the face sets the offset
        // back to zero (QFontEngineFT::init): each run is drawn with a fresh
        // copy of its font, 1/4096 px larger, which is the same 26.6 size
        // (an engine that cannot be copied leaves the font as it was).
        QRawFont font = run.rawFont();
        font.setPixelSize(font.pixelSize() + 1.0 / 4096);
        QGlyphRun fresh = run;
        if (font.isValid())
            fresh.setRawFont(font);
        painter.drawGlyphRun(QPointF(-ink.topLeft()), fresh);
    }
    painter.end();
    made->offset = ink.topLeft();
    // The same pixels, in the format tiles blend fastest.
    made->image = std::move(layer).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return made;
}

void addText(Recipe& recipe, const TextLayers& layers, const QString& text, int pixelSize, bool bold, QPoint origin,
    QRgb color, int stroke = 0)
{
    const std::shared_ptr<const TextLayer> layer = layers(text, pixelSize, bold, color, stroke);
    if (!layer || layer->image.isNull())
        return;
    Op op;
    op.type = Op::Layer;
    op.origin = origin + layer->offset;
    op.layer = layer->image; // shared: its bytes count in the text's own entry
    recipe.extent |= QRect(op.origin, op.layer.size());
    recipe.bytes += qint64(sizeof(Op));
    recipe.ops.append(op);
}

std::shared_ptr<const Recipe> buildRecipe(const Item& item, qreal dpr, int number, NumberFormat format,
    const TextLayers& layers)
{
    auto recipe = std::make_shared<Recipe>();
    recipe->item = item;
    recipe->dpr = dpr;
    recipe->number = number;
    recipe->format = format;
    const QColor color = QColor::fromRgb(item.style.color);
    const int w = std::max(1, item.style.width);
    switch (item.kind) {
    case Kind::Rect: {
        const QRect box = itemBox(item);
        if (box.isEmpty())
            break;
        if (item.style.fill == Fill::Solid || box.width() <= 2 * w || box.height() <= 2 * w) {
            addRects(*recipe, {box}, color);
            break;
        }
        if (item.style.fill == Fill::Soft)
            addRects(*recipe, {box.adjusted(w, w, -w, -w)}, softColor(item.style.color));
        // The stroke inside the box (3.4, rule 4): whole pixels, so its
        // outer edge is exactly the edge that was drawn.
        const int innerHeight = box.height() - 2 * w;
        addRects(*recipe,
            {QRect(box.x(), box.y(), box.width(), w), QRect(box.x(), box.y() + box.height() - w, box.width(), w),
                QRect(box.x(), box.y() + w, w, innerHeight), QRect(box.x() + box.width() - w, box.y() + w, w, innerHeight)},
            color);
        break;
    }
    case Kind::Ellipse: {
        const QRect box = itemBox(item);
        if (box.isEmpty())
            break;
        const QPoint anchor = box.topLeft();
        const QRectF outer(QPointF(0, 0), QSizeF(box.size()));
        if (item.style.fill == Fill::Solid || box.width() <= 2 * w || box.height() <= 2 * w) {
            QPainterPath path;
            path.addEllipse(outer);
            addFill(*recipe, path, anchor, color);
            break;
        }
        if (item.style.fill == Fill::Soft) {
            // To the stroke's middle: its edge is under the opaque ring.
            const qreal half = w / 2.0;
            QPainterPath soft;
            soft.addEllipse(outer.adjusted(half, half, -half, -half));
            addFill(*recipe, soft, anchor, softColor(item.style.color));
        }
        // The stroke inside the box: its outer edge is the ellipse drawn.
        QPainterPath ring;
        ring.setFillRule(Qt::OddEvenFill);
        ring.addEllipse(outer);
        ring.addEllipse(outer.adjusted(w, w, -w, -w));
        addFill(*recipe, ring, anchor, color);
        break;
    }
    case Kind::Arrow: {
        if (item.points.size() < 2)
            break;
        const QPoint anchor = pixelAt(item.points[0]);
        const QPainterPath path = arrowPath(translated(item, -anchor), dpr);
        addFill(*recipe, path, anchor, color);
        break;
    }
    case Kind::Line: {
        if (item.points.size() < 2)
            break;
        const QPoint anchor = pixelAt(item.points[0]);
        const QPainterPath centre = linePath(translated(item, -anchor));
        addFill(*recipe, strokeOf(centre, w), anchor, color);
        break;
    }
    case Kind::Pen:
    case Kind::Marker: {
        if (item.points.isEmpty())
            break;
        const QPoint anchor = pixelAt(item.points[0]);
        const QList<QPointF> points = alignedPoints(translated(item, -anchor));
        const bool dot = std::all_of(points.cbegin(), points.cend(), [&](QPointF p) { return same(p, points[0]); });
        QPainterPath path;
        if (item.kind == Kind::Marker && item.style.markerMode == MarkerMode::Rectangle)
            path.addRect(QRectF(itemBox(translated(item, -anchor))));
        else if (item.kind == Kind::Marker && item.style.markerMode == MarkerMode::Line)
            path = strokeOf(linePath(translated(item, -anchor)), w);
        else if (dot)
            path.addEllipse(points[0], w / 2.0, w / 2.0);
        else
            path = strokeOf(strokeCentreline(points), w);
        // A marker multiplies, one path per stroke: where the stroke crosses
        // itself it does not get darker.
        addFill(*recipe, path, anchor, color,
            item.kind == Kind::Marker ? QPainter::CompositionMode_Multiply : QPainter::CompositionMode_SourceOver);
        break;
    }
    case Kind::Text: {
        if (item.points.isEmpty() || item.text.isEmpty())
            break;
        const TextGeometry g = textGeometry(item);
        QRgb ink = item.style.color;
        if (item.style.boxed) {
            QPainterPath box;
            box.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(g.box.size())), g.radius, g.radius);
            addFill(*recipe, box, g.box.topLeft(), color);
            ink = inkOn(item.style.color);
        }
        // The outline first, under the words, which are drawn as ever.
        if (const int stroke = textStrokeWidth(item); stroke > 0) {
            addText(*recipe, layers, item.text, std::max(1, item.style.fontPx), item.style.bold, g.content.topLeft(),
                textStrokeColorOf(item), stroke);
        }
        addText(*recipe, layers, item.text, std::max(1, item.style.fontPx), item.style.bold, g.content.topLeft(), ink);
        break;
    }
    case Kind::Number: {
        if (item.points.isEmpty())
            break;
        const NumberGeometry g = numberGeometry(item, number, dpr, format);
        const QPoint anchor = pixelAt(item.points[0]);
        const QRectF disc = g.disc.translated(-QPointF(anchor));
        const qreal radius = disc.height() / 2;
        QPainterPath shape;
        shape.addRoundedRect(disc, radius, radius);
        QRgb digits = inkOn(item.style.color);
        if (g.ring > 0) {
            // A ring in the colour, the digits in the colour inside it; a
            // white one would vanish on a white page: the words' dark then.
            digits = numberWordsColor(item);
            const QColor tint = QColor::fromRgb(digits);
            const qreal inner = std::max(0.0, radius - g.ring);
            QPainterPath ring;
            ring.setFillRule(Qt::OddEvenFill);
            ring.addRoundedRect(disc, radius, radius);
            ring.addRoundedRect(disc.adjusted(g.ring, g.ring, -g.ring, -g.ring), inner, inner);
            addFill(*recipe, ring, anchor, tint);
        } else {
            addFill(*recipe, shape, anchor, color);
        }
        if (g.outline > 0) {
            const qreal o = g.outline;
            const qreal inner = std::max(0.0, radius - o);
            QPainterPath ring;
            ring.setFillRule(Qt::OddEvenFill);
            ring.addRoundedRect(disc, radius, radius);
            ring.addRoundedRect(disc.adjusted(o, o, -o, -o), inner, inner);
            addFill(*recipe, ring, anchor, QColor::fromRgba(numberOutlineColor()));
        }
        addText(*recipe, layers, g.label, std::max(1, item.style.fontPx), true, g.origin, digits);
        if (!g.wordsBox.isEmpty())
            addText(*recipe, layers, item.text, std::max(1, item.style.fontPx), false, g.wordsOrigin, numberWordsColor(item));
        break;
    }
    case Kind::Redact: {
        const QRect box = itemBox(item);
        if (box.isEmpty())
            break;
        Op op;
        op.type = Op::Patch;
        recipe->ops.append(op);
        recipe->extent |= box;
        break;
    }
    }
    recipe->bytes += qint64(sizeof(Recipe)) + item.points.size() * qint64(sizeof(QPointF)) + item.text.size() * 2;
    return recipe;
}

// The base's pixels of `area` into the target; black where the base ends.
void copyBase(const QImage& base, const QRect& area, QImage& target)
{
    const QRect inside = area & base.rect();
    if (inside != area)
        target.fill(Qt::black);
    if (inside.isEmpty())
        return;
    const QImage::Format format = base.format();
    if (format == QImage::Format_RGB32 || format == QImage::Format_ARGB32 || format == QImage::Format_ARGB32_Premultiplied) {
        for (int y = inside.top(); y <= inside.bottom(); ++y) {
            const QRgb* from = reinterpret_cast<const QRgb*>(base.constScanLine(y)) + inside.left();
            QRgb* to = reinterpret_cast<QRgb*>(target.scanLine(y - area.top())) + (inside.left() - area.left());
            if (format == QImage::Format_RGB32) {
                std::memcpy(to, from, std::size_t(inside.width()) * sizeof(QRgb));
            } else {
                // The base is opaque; RGB32 keeps its alpha byte at 0xff.
                for (int x = 0; x < inside.width(); ++x)
                    to[x] = from[x] | 0xff000000u;
            }
        }
        return;
    }
    QPainter painter(&target);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(inside.topLeft() - area.topLeft(), base, inside);
}

QImage maskOf(const QList<Item>& redactions, const QRect& sampleBounds)
{
    QImage mask(sampleBounds.size(), QImage::Format_Grayscale8);
    if (mask.isNull())
        return mask;
    mask.fill(0);
    for (const Item& item : redactions) {
        const QRect box = itemBox(item);
        const QRect r = box & sampleBounds;
        if (r.isEmpty())
            continue;
        if (isBrush(item)) {
            // Only the stroke's own pixels: a diagonal stroke leaves the
            // corners of its box readable.
            const QImage stroke = brushCoverage(item);
            for (int y = r.top(); y <= r.bottom(); ++y) {
                const uchar* from = stroke.constScanLine(y - box.top());
                uchar* to = mask.scanLine(y - sampleBounds.top());
                for (int x = r.left(); x <= r.right(); ++x) {
                    if (from[x - box.left()])
                        to[x - sampleBounds.left()] = 255;
                }
            }
            continue;
        }
        for (int y = r.top(); y <= r.bottom(); ++y)
            std::memset(mask.scanLine(y - sampleBounds.top()) + (r.left() - sampleBounds.left()), 255, std::size_t(r.width()));
    }
    return mask;
}

// splitmix64's finaliser: the noise must be the same on every computer and
// with every compiler, which std::*_distribution does not promise (3.10).
quint64 mix(quint64 z)
{
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int noise(quint64 seed, int a, int b, int amplitude)
{
    const quint64 h = mix(seed ^ mix((quint64(quint32(a)) << 32) | quint32(b)));
    return int(h % quint64(2 * amplitude + 1)) - amplitude;
}

// Blur's noise is drawn apart from the mosaic's.
constexpr quint64 kBlurSalt = 0x5851F42D4C957F2Dull;

int channel(double v, int add)
{
    return std::clamp(int(std::lround(v)) + add, 0, 255);
}

using Rgb = std::array<double, 3>;

// What a redaction may read: pixels in S, outside U and outside its own box.
// The nearest such pixels in a row or column through the box are the same
// for every point of the box on it (all the pixels between are the box's),
// so each row and column is scanned once.
class Sampler {
public:
    // `exact`: the area is not the box (a brush stroke): its box holds pixels
    // that may be read, found by scanning out from each point.
    Sampler(const QImage& base, const QRect& sampleBounds, const QImage& covered, const QRect& box, bool exact = false, int reach = 64)
        : m_exact(exact)
        , m_reach(reach)
        , m_base(base)
        , m_sample(sampleBounds & base.rect())
        , m_covered(covered)
        , m_origin(sampleBounds.topLeft())
        , m_box(box)
        , m_left(std::size_t(box.height()), kUnknown)
        , m_right(std::size_t(box.height()), kUnknown)
        , m_up(std::size_t(box.width()), kUnknown)
        , m_down(std::size_t(box.width()), kUnknown)
    {
    }

    // The colour for point (x, y) of the box, from the nearest readable
    // pixels in its row and column, weighted by 1 / distance.
    Rgb fill(int x, int y)
    {
        Rgb sum{0, 0, 0};
        double weights = 0;
        const auto add = [&](int sx, int sy, int distance) {
            const QRgb c = pixel(sx, sy);
            const double w = 1.0 / distance;
            sum[0] += w * qRed(c);
            sum[1] += w * qGreen(c);
            sum[2] += w * qBlue(c);
            weights += w;
        };
        if (m_exact) {
            // Not past m_reach (about the stroke's width): along a long stroke the nearest pixel to read may be a screen away.
            const int left = scanRow(y, x - 1, -1, m_reach);
            const int right = scanRow(y, x + 1, 1, m_reach);
            const int up = scanColumn(x, y - 1, -1, m_reach);
            const int down = scanColumn(x, y + 1, 1, m_reach);
            if (left != kNone)
                add(left, y, x - left);
            if (right != kNone)
                add(right, y, right - x);
            if (up != kNone)
                add(x, up, y - up);
            if (down != kNone)
                add(x, down, down - y);
            if (weights == 0)
                return mean();
            return {sum[0] / weights, sum[1] / weights, sum[2] / weights};
        }
        const std::size_t row = std::size_t(y - m_box.top());
        const std::size_t column = std::size_t(x - m_box.left());
        if (m_left[row] == kUnknown)
            m_left[row] = scanRow(y, std::min(m_box.left() - 1, m_sample.right()), -1);
        if (m_right[row] == kUnknown)
            m_right[row] = scanRow(y, std::max(m_box.right() + 1, m_sample.left()), 1);
        if (m_up[column] == kUnknown)
            m_up[column] = scanColumn(x, std::min(m_box.top() - 1, m_sample.bottom()), -1);
        if (m_down[column] == kUnknown)
            m_down[column] = scanColumn(x, std::max(m_box.bottom() + 1, m_sample.top()), 1);
        if (m_left[row] != kNone)
            add(m_left[row], y, x - m_left[row]);
        if (m_right[row] != kNone)
            add(m_right[row], y, m_right[row] - x);
        if (m_up[column] != kNone)
            add(x, m_up[column], y - m_up[column]);
        if (m_down[column] != kNone)
            add(x, m_down[column], m_down[column] - y);
        if (weights == 0)
            return mean();
        return {sum[0] / weights, sum[1] / weights, sum[2] / weights};
    }

private:
    static constexpr int kUnknown = std::numeric_limits<int>::min();
    static constexpr int kNone = std::numeric_limits<int>::min() + 1;

    bool readable(int x, int y) const
    {
        if (!m_sample.contains(x, y) || (!m_exact && m_box.contains(x, y)))
            return false;
        const int mx = x - m_origin.x();
        const int my = y - m_origin.y();
        if (mx >= 0 && my >= 0 && mx < m_covered.width() && my < m_covered.height())
            return m_covered.constScanLine(my)[mx] == 0;
        return true;
    }

    QRgb pixel(int x, int y) const
    {
        return reinterpret_cast<const QRgb*>(m_base.constScanLine(y))[x];
    }

    int scanRow(int y, int from, int step, int reach = std::numeric_limits<int>::max()) const
    {
        if (y < m_sample.top() || y > m_sample.bottom())
            return kNone;
        for (int x = from, n = 0; x >= m_sample.left() && x <= m_sample.right() && n < reach; x += step, ++n) {
            if (readable(x, y))
                return x;
        }
        return kNone;
    }

    int scanColumn(int x, int from, int step, int reach = std::numeric_limits<int>::max()) const
    {
        if (x < m_sample.left() || x > m_sample.right())
            return kNone;
        for (int y = from, n = 0; y >= m_sample.top() && y <= m_sample.bottom() && n < reach; y += step, ++n) {
            if (readable(x, y))
                return y;
        }
        return kNone;
    }

    // The mean of every readable pixel, for a point with none in its row or
    // column; #808080 when nothing at all may be read.
    Rgb mean()
    {
        if (m_mean)
            return *m_mean;
        quint64 sum[3] = {0, 0, 0};
        quint64 count = 0;
        for (int y = m_sample.top(); y <= m_sample.bottom(); ++y) {
            for (int x = m_sample.left(); x <= m_sample.right(); ++x) {
                if (!readable(x, y))
                    continue;
                const QRgb c = pixel(x, y);
                sum[0] += quint64(qRed(c));
                sum[1] += quint64(qGreen(c));
                sum[2] += quint64(qBlue(c));
                ++count;
            }
        }
        if (count == 0)
            m_mean = Rgb{128, 128, 128};
        else
            m_mean = Rgb{double(sum[0]) / double(count), double(sum[1]) / double(count), double(sum[2]) / double(count)};
        return *m_mean;
    }

    bool m_exact;
    int m_reach; // a brush's scans go this far, px
    const QImage& m_base;
    QRect m_sample;
    const QImage& m_covered;
    QPoint m_origin;
    QRect m_box;
    std::vector<int> m_left;
    std::vector<int> m_right;
    std::vector<int> m_up;
    std::vector<int> m_down;
    std::optional<Rgb> m_mean;
};

// Whether any pixel of `part` (image px) is in the stroke `stroke` (a brush's
// coverage, over `box`); true for no stroke.
bool inStroke(const QImage* stroke, const QRect& box, const QRect& part)
{
    if (!stroke)
        return true;
    for (int y = part.top(); y <= part.bottom(); ++y) {
        const uchar* line = stroke->constScanLine(y - box.top()) + (part.left() - box.left());
        for (int x = 0; x < part.width(); ++x) {
            if (line[x])
                return true;
        }
    }
    return false;
}

// The pixels of a brush's box that its stroke does not cover are never drawn,
// so they are not worked out either (`stroke`, null for a box).
void mosaic(QImage& patch, Sampler& sampler, const QRect& box, int cell, quint64 seed, const QImage* stroke = nullptr)
{
    int j = 0;
    for (int top = box.top(); top <= box.bottom(); top += cell, ++j) {
        const int bottom = std::min(top + cell - 1, box.bottom());
        const int cy = top + (bottom - top + 1) / 2;
        int i = 0;
        for (int left = box.left(); left <= box.right(); left += cell, ++i) {
            const int right = std::min(left + cell - 1, box.right());
            if (!inStroke(stroke, box, QRect(QPoint(left, top), QPoint(right, bottom))))
                continue;
            const int cx = left + (right - left + 1) / 2;
            const Rgb c = sampler.fill(cx, cy);
            const int n = noise(seed, i, j, 16);
            const QRgb color = qRgb(channel(c[0], n), channel(c[1], n), channel(c[2], n));
            for (int y = top; y <= bottom; ++y) {
                QRgb* line = reinterpret_cast<QRgb*>(patch.scanLine(y - box.top()));
                std::fill(line + (left - box.left()), line + (right - box.left()) + 1, color);
            }
        }
    }
}

// Every pixel from the nearest readable ones around it, no noise: the area
// looks as if it was rubbed out.
void smooth(QImage& patch, Sampler& sampler, const QRect& box, const QImage* stroke = nullptr)
{
    for (int y = box.top(); y <= box.bottom(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(patch.scanLine(y - box.top()));
        const uchar* covered = stroke ? stroke->constScanLine(y - box.top()) : nullptr;
        for (int x = box.left(); x <= box.right(); ++x) {
            if (covered && !covered[x - box.left()])
                continue;
            const Rgb c = sampler.fill(x, y);
            line[x - box.left()] = qRgb(channel(c[0], 0), channel(c[1], 0), channel(c[2], 0));
        }
    }
}

// Grid positions from `first` every `step`, the last one at `last`.
std::vector<int> gridLine(int first, int last, int step)
{
    std::vector<int> positions;
    for (int v = first; v < last; v += step)
        positions.push_back(v);
    positions.push_back(last);
    return positions;
}

// For each position from first to last: the grid segment it is in and how
// far along it.
void segments(const std::vector<int>& grid, int first, int last, std::vector<int>& index, std::vector<double>& t)
{
    index.assign(std::size_t(last - first + 1), 0);
    t.assign(index.size(), 0);
    std::size_t k = 0;
    for (int v = first; v <= last; ++v) {
        while (k + 1 < grid.size() && grid[k + 1] < v)
            ++k;
        const std::size_t next = std::min(k + 1, grid.size() - 1);
        const std::size_t at = std::size_t(v - first);
        index[at] = int(k);
        t[at] = next == k ? 0.0 : double(v - grid[k]) / double(grid[next] - grid[k]);
    }
}

void blur(QImage& patch, Sampler& sampler, const QRect& box, int cell, quint64 seed, const QImage* stroke = nullptr)
{
    const int step = std::max(4, cell / 2);
    const std::vector<int> xs = gridLine(box.left(), box.right(), step);
    const std::vector<int> ys = gridLine(box.top(), box.bottom(), step);
    std::vector<Rgb> values(xs.size() * ys.size());
    for (std::size_t iy = 0; iy < ys.size(); ++iy) {
        for (std::size_t ix = 0; ix < xs.size(); ++ix)
            values[iy * xs.size() + ix] = sampler.fill(xs[ix], ys[iy]);
    }
    std::vector<int> columnIndex;
    std::vector<double> columnT;
    std::vector<int> rowIndex;
    std::vector<double> rowT;
    segments(xs, box.left(), box.right(), columnIndex, columnT);
    segments(ys, box.top(), box.bottom(), rowIndex, rowT);
    const quint64 salted = seed ^ kBlurSalt;
    for (int y = box.top(); y <= box.bottom(); ++y) {
        const std::size_t ry = std::size_t(y - box.top());
        const std::size_t y0 = std::size_t(rowIndex[ry]);
        const std::size_t y1 = std::min(y0 + 1, ys.size() - 1);
        const double ty = rowT[ry];
        QRgb* line = reinterpret_cast<QRgb*>(patch.scanLine(int(ry)));
        const uchar* covered = stroke ? stroke->constScanLine(int(ry)) : nullptr;
        for (int x = box.left(); x <= box.right(); ++x) {
            const std::size_t rx = std::size_t(x - box.left());
            if (covered && !covered[rx])
                continue;
            const std::size_t x0 = std::size_t(columnIndex[rx]);
            const std::size_t x1 = std::min(x0 + 1, xs.size() - 1);
            const double tx = columnT[rx];
            const Rgb& a = values[y0 * xs.size() + x0];
            const Rgb& b = values[y0 * xs.size() + x1];
            const Rgb& c = values[y1 * xs.size() + x0];
            const Rgb& d = values[y1 * xs.size() + x1];
            const int n = noise(salted, int(rx), int(ry), 4);
            int out[3];
            for (int k = 0; k < 3; ++k) {
                const double top = a[std::size_t(k)] + (b[std::size_t(k)] - a[std::size_t(k)]) * tx;
                const double bottom = c[std::size_t(k)] + (d[std::size_t(k)] - c[std::size_t(k)]) * tx;
                out[k] = channel(top + (bottom - top) * ty, n);
            }
            line[rx] = qRgb(out[0], out[1], out[2]);
        }
    }
}

enum class Slot : quint8 { Recipe = 1, Patch = 2, Mask = 3, Text = 4 };

quint64 cacheKey(Slot slot, quint32 id)
{
    return (quint64(slot) << 32) | id;
}

struct PatchEntry {
    Item item;
    QRect sample;
    qint64 baseKey = 0;
    QList<Item> redactions;
    QImage image;
};

struct MaskEntry {
    QRect sample;
    QList<Item> redactions;
    QImage image;
};

} // namespace

struct Compositor::Private {
    struct Entry {
        qint64 bytes = 0;
        std::shared_ptr<const Recipe> recipe;
        std::shared_ptr<const PatchEntry> patch;
        std::shared_ptr<const MaskEntry> mask;
        std::shared_ptr<const TextLayer> text;
        std::list<quint64>::iterator order;
    };

    explicit Private(qint64 budget_)
        : budget(budget_)
    {
    }

    // The entry for `key`, now the most recently used; null when missing.
    const Entry* touch(quint64 key)
    {
        const auto it = entries.find(key);
        if (it == entries.end())
            return nullptr;
        order.splice(order.begin(), order, it->order);
        return &*it;
    }

    void put(quint64 key, Entry entry)
    {
        drop(key);
        order.push_front(key);
        entry.order = order.begin();
        used += entry.bytes;
        entries.insert(key, std::move(entry));
        // Anything over the budget goes, the oldest first; a single entry
        // larger than the budget still serves the render that made it.
        while (used > budget && !order.empty())
            drop(order.back());
    }

    void drop(quint64 key)
    {
        const auto it = entries.find(key);
        if (it == entries.end())
            return;
        used -= it->bytes;
        order.erase(it->order);
        entries.erase(it);
    }

    void dropSlot(Slot slot)
    {
        QList<quint64> keys;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            if ((it.key() >> 32) == quint64(slot))
                keys.append(it.key());
        }
        for (quint64 key : std::as_const(keys))
            drop(key);
    }

    std::shared_ptr<const Recipe> recipe(const Item& item, qreal dpr, int number, NumberFormat format)
    {
        const quint64 key = cacheKey(Slot::Recipe, item.id);
        if (const Entry* entry = touch(key)) {
            const Recipe& r = *entry->recipe;
            if (r.dpr == dpr && r.number == number && r.format == format && r.item == item)
                return entry->recipe;
        }
        std::shared_ptr<const Recipe> made = buildRecipe(item, dpr, number, format,
            [this](const QString& text, int pixelSize, bool bold, QRgb color, int stroke) {
                return textLayer(text, pixelSize, bold, color, stroke);
            });
        Entry entry;
        entry.bytes = made->bytes;
        entry.recipe = made;
        put(key, std::move(entry));
        return made;
    }

    // Kept by the words and their look, never by item: nothing an item does
    // makes it stale. Two texts whose keys hash alike take turns.
    std::shared_ptr<const TextLayer> textLayer(const QString& text, int pixelSize, bool bold, QRgb color, int stroke)
    {
        const QString name = textLayerKey(text, pixelSize, bold, color, stroke);
        const quint64 key = cacheKey(Slot::Text, quint32(qHash(name)));
        if (const Entry* entry = touch(key)) {
            if (entry->text->key == name)
                return entry->text;
        }
        std::shared_ptr<const TextLayer> made = makeTextLayer(text, pixelSize, bold, color, stroke);
        Entry entry;
        entry.bytes = qint64(sizeof(TextLayer)) + made->key.size() * 2 + made->image.sizeInBytes();
        entry.text = made;
        put(key, std::move(entry));
        return made;
    }

    std::shared_ptr<const MaskEntry> mask(const QRect& sample, const QList<Item>& redactions)
    {
        const quint64 key = cacheKey(Slot::Mask, 0);
        if (const Entry* entry = touch(key)) {
            if (entry->mask->sample == sample && entry->mask->redactions == redactions)
                return entry->mask;
        }
        auto made = std::make_shared<MaskEntry>();
        made->sample = sample;
        made->redactions = redactions;
        made->image = maskOf(redactions, sample);
        Entry entry;
        entry.bytes = qint64(sizeof(MaskEntry)) + made->image.sizeInBytes();
        entry.mask = made;
        put(key, std::move(entry));
        return made;
    }

    std::shared_ptr<const PatchEntry> patch(const Item& item, const QImage& base, const QRect& sample, const QList<Item>& redactions)
    {
        const quint64 key = cacheKey(Slot::Patch, item.id);
        if (const Entry* entry = touch(key)) {
            const PatchEntry& p = *entry->patch;
            if (p.baseKey == base.cacheKey() && p.sample == sample && p.redactions == redactions && p.item == item)
                return entry->patch;
        }
        const std::shared_ptr<const MaskEntry> covered = mask(sample, redactions);
        auto made = std::make_shared<PatchEntry>();
        made->item = item;
        made->sample = sample;
        made->baseKey = base.cacheKey();
        made->redactions = redactions;
        made->image = redactionPatch(base, sample, covered->image, item);
        ++patchesMade;
        Entry entry;
        entry.bytes = qint64(sizeof(PatchEntry)) + made->image.sizeInBytes();
        entry.patch = made;
        put(key, std::move(entry));
        return made;
    }

    qint64 budget;
    qint64 used = 0;
    qint64 patchesMade = 0;
    QHash<quint64, Entry> entries;
    std::list<quint64> order; // most recently used first
};

Compositor::Compositor(qint64 cacheBytes_)
    : d(std::make_unique<Private>(cacheBytes_))
{
}

Compositor::~Compositor() = default;

void Compositor::render(const Document& doc, const QImage& base, const QRect& sampleBounds, const QRect& area, QImage* target,
    std::optional<quint32> skip, std::optional<quint32> unworded, std::optional<quint32> draft)
{
    if (!target || area.isEmpty())
        return;
    if (target->size() != area.size() || target->format() != QImage::Format_RGB32)
        *target = QImage(area.size(), QImage::Format_RGB32);
    if (target->isNull())
        return;
    copyBase(base, area, *target);

    QPainter painter(target);
    const QTransform toTarget = QTransform::fromTranslate(-area.x(), -area.y());
    std::optional<QHash<quint32, int>> shown;
    std::optional<QList<Item>> redactions;
    for (const Item& listed : doc.items) {
        if (skip && listed.id == *skip)
            continue;
        int number = 0;
        if (listed.kind == Kind::Number) {
            if (!shown)
                shown = numbers(doc);
            number = shown->value(listed.id, 1);
        }
        // A number whose words are being typed is drawn without them.
        Item bare;
        const bool without = unworded && listed.id == *unworded && !listed.text.isEmpty();
        if (without) {
            bare = listed;
            bare.text.clear();
        }
        // A brush stroke still being drawn is shown as a mosaic whatever its mode: the real one is worked out
        // for the whole stroke once, when the mouse is let go (the redactions' cache then makes it again).
        if (draft && listed.id == *draft && isBrush(listed) && listed.style.redact != RedactMode::Solid
            && listed.style.redact != RedactMode::Mosaic) {
            bare = listed;
            bare.style.redact = RedactMode::Mosaic;
        }
        const bool drafted = bare.id == listed.id && bare.style.redact != listed.style.redact;
        const Item& item = (without || drafted) ? bare : listed;
        const std::shared_ptr<const Recipe> recipe = d->recipe(item, doc.dpr, number, formatOf(doc, item.series));
        if (!recipe->extent.intersects(area))
            continue;
        for (const Op& op : recipe->ops) {
            // Each op sets all the state it draws with: an op that does not
            // reach one area must not change how the next op draws there.
            painter.setCompositionMode(op.mode);
            painter.setRenderHint(QPainter::Antialiasing, op.type != Op::Rects);
            switch (op.type) {
            case Op::Rects:
                painter.setTransform(toTarget);
                for (const QRect& r : op.rects) {
                    if (r.intersects(area))
                        painter.fillRect(r, op.color);
                }
                break;
            case Op::Fill: {
                const Shape& shape = *op.shape;
                const QRect local = area.translated(-shape.anchor);
                if (!shape.bounds.intersects(QRectF(local)))
                    break;
                const QPoint shift = shape.anchor - area.topLeft();
                painter.setTransform(QTransform::fromTranslate(shift.x(), shift.y()));
                const QRectF window = gridWindow(local);
                if (window.contains(shape.bounds))
                    painter.fillPath(shape.path, op.color);
                else
                    painter.fillPath(clampedPath(shape, window), op.color);
                break;
            }
            case Op::Layer:
                painter.setTransform(toTarget);
                painter.drawImage(op.origin, op.layer);
                break;
            case Op::Patch: {
                if (!redactions)
                    redactions = redactionItems(doc);
                const std::shared_ptr<const PatchEntry> patch = d->patch(item, base, sampleBounds, *redactions);
                if (patch->image.isNull())
                    break;
                painter.setTransform(toTarget);
                painter.drawImage(patch->image.offset(), patch->image);
                break;
            }
            }
        }
    }
}

QImage Compositor::exportImage(const Document& doc, const QImage& base, const QRect& sampleBounds)
{
    const QRect canvas(QPoint(0, 0), doc.canvas);
    QRect area = doc.crop & canvas;
    if (area.isEmpty())
        area = canvas;
    QImage out;
    render(doc, base, sampleBounds, area, &out);
    return out;
}

void Compositor::invalidate(const Change& change)
{
    for (quint32 id : change.ids) {
        d->drop(cacheKey(Slot::Recipe, id));
        d->drop(cacheKey(Slot::Patch, id));
    }
    if (change.redactions) {
        d->dropSlot(Slot::Patch);
        d->dropSlot(Slot::Mask);
    }
}

void Compositor::clear()
{
    d->entries.clear();
    d->order.clear();
    d->used = 0;
    clearTextMetrics();
}

qint64 Compositor::cacheBytes() const
{
    return d->used;
}

qint64 Compositor::patchesMade() const
{
    return d->patchesMade;
}

QImage coverageMask(const Document& doc, const QRect& sampleBounds)
{
    return maskOf(redactionItems(doc), sampleBounds);
}

QImage redactionPatch(const QImage& base, const QRect& sampleBounds, const QImage& covered, const Item& item)
{
    const QRect box = itemBox(item);
    if (box.isEmpty())
        return {};
    const bool brush = isBrush(item);
    const QImage stroke = brush ? brushCoverage(item) : QImage(); // once for the patch
    QImage patch(box.size(), QImage::Format_RGB32);
    if (patch.isNull())
        return patch;
    patch.setOffset(box.topLeft());
    if (item.style.redact == RedactMode::Solid) {
        patch.fill(item.style.color | 0xff000000u);
    } else {
        const QImage::Format format = base.format();
        const bool direct = format == QImage::Format_RGB32 || format == QImage::Format_ARGB32
            || format == QImage::Format_ARGB32_Premultiplied;
        const QImage pixels = direct ? base : base.convertToFormat(QImage::Format_RGB32);
        const bool byteMask = covered.format() == QImage::Format_Grayscale8 || covered.format() == QImage::Format_Alpha8;
        const QImage mask = byteMask || covered.isNull() ? covered : covered.convertToFormat(QImage::Format_Grayscale8);
        // A pixel of a stroke has readable ground within about its width; scanning further only finds the stroke's own length.
        Sampler sampler(pixels, sampleBounds, mask, box, brush, std::clamp(item.style.width, 16, 64));
        const int cell = std::max(1, item.style.cell);
        const QImage* only = brush ? &stroke : nullptr;
        if (item.style.redact == RedactMode::Blur)
            blur(patch, sampler, box, cell, item.seed, only);
        else if (item.style.redact == RedactMode::Smooth)
            smooth(patch, sampler, box, only);
        else
            mosaic(patch, sampler, box, cell, item.seed, only);
    }
    if (!brush)
        return patch;
    // Only the stroke's pixels are drawn; the rest of its box is clear.
    QImage out(box.size(), QImage::Format_ARGB32_Premultiplied);
    out.setOffset(box.topLeft());
    for (int y = 0; y < box.height(); ++y) {
        const QRgb* from = reinterpret_cast<const QRgb*>(patch.constScanLine(y));
        const uchar* in = stroke.constScanLine(y);
        QRgb* to = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < box.width(); ++x)
            to[x] = in[x] ? (from[x] | 0xff000000u) : 0u;
    }
    return out;
}

}

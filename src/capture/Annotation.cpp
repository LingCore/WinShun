#include "Annotation.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainterPathStroker>
#include <QRawFont>
#include <QStringList>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <list>
#include <numbers>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::capture::annot {

namespace {

bool same(QPointF a, QPointF b)
{
    // QPointF's operator== is fuzzy; geometry here wants the exact values.
    return a.x() == b.x() && a.y() == b.y();
}

// Perceived brightness, 0..255000.
int brightness(QRgb c)
{
    return 299 * qRed(c) + 587 * qGreen(c) + 114 * qBlue(c);
}

// Yellow (#FDD835) and white are light, orange (#FB8C00) is not.
constexpr int kLight = 178500; // 0.7 of white
constexpr int kWhitish = 229500; // 0.9 of white

qreal snap64(qreal v)
{
    return std::floor(v * 64 + 0.5) / 64;
}

qreal signedArea(const QPolygonF& polygon)
{
    qreal sum = 0;
    for (qsizetype i = 0; i < polygon.size(); ++i) {
        const QPointF a = polygon[i];
        const QPointF b = polygon[(i + 1) % polygon.size()];
        sum += a.x() * b.y() - b.x() * a.y();
    }
    return sum / 2;
}

void orient(QPolygonF& polygon)
{
    if (signedArea(polygon) < 0)
        std::reverse(polygon.begin(), polygon.end());
}

qreal distanceToSegment(QPointF p, QPointF a, QPointF b)
{
    const QPointF ab = b - a;
    const qreal length2 = QPointF::dotProduct(ab, ab);
    qreal t = length2 > 0 ? QPointF::dotProduct(p - a, ab) / length2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF q = a + ab * t;
    return std::hypot(p.x() - q.x(), p.y() - q.y());
}

qreal distanceToPolyline(QPointF p, const QList<QPointF>& points)
{
    if (points.isEmpty())
        return std::numeric_limits<qreal>::infinity();
    if (points.size() == 1)
        return std::hypot(p.x() - points[0].x(), p.y() - points[0].y());
    qreal best = std::numeric_limits<qreal>::infinity();
    for (qsizetype i = 0; i + 1 < points.size(); ++i)
        best = std::min(best, distanceToSegment(p, points[i], points[i + 1]));
    return best;
}

// `to` turned around `from` to the nearest multiple of 15 degrees; the offset
// is rounded to whole pixels so that a pixel centre stays a pixel centre, and
// 0 and 90 degrees come out exactly level or upright.

int startOf(const Document& doc, quint32 series)
{
    for (const Series& s : doc.series) {
        if (s.id == series)
            return s.start;
    }
    return 1;
}

// textMetrics()' memory: plain values keyed by size, weight and text.
struct MetricsMemo {
    static constexpr std::size_t kSize = 256;
    std::list<std::pair<QString, TextMetrics>> order; // most recent first
    QHash<QString, std::list<std::pair<QString, TextMetrics>>::iterator> index;
};
thread_local MetricsMemo t_memo;

bool nearBox(QPointF p, const QRectF& box, int width, qreal tolerance, bool filled)
{
    if (!box.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(p))
        return false;
    if (filled)
        return true;
    const qreal in = width + tolerance;
    const QRectF inner = box.adjusted(in, in, -in, -in);
    return !(inner.isValid() && inner.contains(p));
}

bool nearEllipse(QPointF p, const QRectF& box, int width, qreal tolerance, bool filled)
{
    const qreal a = box.width() / 2;
    const qreal b = box.height() / 2;
    if (a <= 0 || b <= 0)
        return false;
    const qreal dx = p.x() - box.center().x();
    const qreal dy = p.y() - box.center().y();
    const auto inside = [&](qreal ra, qreal rb) { return dx * dx / (ra * ra) + dy * dy / (rb * rb) <= 1; };
    if (!inside(a + tolerance, b + tolerance))
        return false;
    if (filled)
        return true;
    const qreal ia = a - width - tolerance;
    const qreal ib = b - width - tolerance;
    return ia <= 0 || ib <= 0 || !inside(ia, ib);
}

bool hits(const Item& item, QPointF q, qreal tolerance, qreal dpr, int number, NumberFormat format)
{
    const int w = std::max(1, item.style.width);
    switch (item.kind) {
    case Kind::Rect:
        return nearBox(q, QRectF(itemBox(item)), w, tolerance, item.style.fill != Fill::None);
    case Kind::Ellipse:
        return nearEllipse(q, QRectF(itemBox(item)), w, tolerance, item.style.fill != Fill::None);
    case Kind::Redact:
        if (item.style.shape == RedactShape::Brush)
            return distanceToPolyline(q, item.points) <= w / 2.0 + tolerance;
        return QRectF(itemBox(item)).contains(q);
    case Kind::Arrow: {
        if (item.style.lineMode == LineMode::Bezier) {
            const QPainterPath shape = arrowPath(item, dpr);
            QPainterPathStroker reach;
            reach.setWidth(std::max<qreal>(0.01, tolerance * 2));
            return shape.contains(q) || reach.createStroke(shape).contains(q);
        }
        const ArrowShape shape = arrowShape(item, dpr);
        if (shape.head.isEmpty())
            return false;
        if (shape.head.containsPoint(q, Qt::OddEvenFill))
            return true;
        const QList<QPointF> points = alignedPoints(item);
        return distanceToSegment(q, points[0], points[1]) <= w / 2.0 + tolerance;
    }
    case Kind::Line:
        for (const QPolygonF& points : linePath(item).toSubpathPolygons()) {
            if (distanceToPolyline(q, points) <= w / 2.0 + tolerance)
                return true;
        }
        return false;
    case Kind::Pen:
    case Kind::Marker:
        if (item.kind == Kind::Marker && item.style.markerMode == MarkerMode::Rectangle)
            return QRectF(itemBox(item)).adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(q);
        return distanceToPolyline(q, alignedPoints(item)) <= w / 2.0 + tolerance;
    case Kind::Text: {
        if (item.text.isEmpty())
            return false;
        // The outline is part of the text: a click on it picks the text.
        const int stroke = textStrokeWidth(item);
        return QRectF(textGeometry(item).box).adjusted(-stroke, -stroke, stroke, stroke).contains(q);
    }
    case Kind::Number: {
        const NumberGeometry g = numberGeometry(item, number, dpr, format);
        return g.disc.contains(q) || (!g.wordsBox.isEmpty() && QRectF(g.wordsBox).contains(q));
    }
    }
    return false;
}

} // namespace

QPointF snapAngle(QPointF from, QPointF to)
{
    const QPointF d = to - from;
    const qreal length = std::hypot(d.x(), d.y());
    if (length == 0)
        return from;
    constexpr qreal step = std::numbers::pi / 12;
    const qreal angle = std::round(std::atan2(d.y(), d.x()) / step) * step;
    return from + QPointF(std::round(length * std::cos(angle)), std::round(length * std::sin(angle)));
}

bool isBox(Kind kind)
{
    return kind == Kind::Rect || kind == Kind::Ellipse || kind == Kind::Redact;
}

bool isLine(Kind kind)
{
    return kind == Kind::Arrow || kind == Kind::Line;
}

bool isStroke(Kind kind)
{
    return kind == Kind::Pen || kind == Kind::Marker;
}

bool isBoxItem(const Item& item)
{
    return (isBox(item.kind) && !isBrush(item))
        || (item.kind == Kind::Marker && item.style.markerMode == MarkerMode::Rectangle);
}

bool isLineItem(const Item& item)
{
    return isLine(item.kind) || (item.kind == Kind::Marker && item.style.markerMode == MarkerMode::Line);
}

QString lineModeName(LineMode mode)
{
    switch (mode) {
    case LineMode::Straight: return QStringLiteral("straight");
    case LineMode::Polyline: return QStringLiteral("polyline");
    case LineMode::Bezier: return QStringLiteral("bezier");
    }
    return {};
}

std::optional<LineMode> lineModeFromName(QStringView name)
{
    for (LineMode mode : {LineMode::Straight, LineMode::Polyline, LineMode::Bezier}) {
        if (name == lineModeName(mode))
            return mode;
    }
    return std::nullopt;
}

QString markerModeName(MarkerMode mode)
{
    switch (mode) {
    case MarkerMode::Freehand: return QStringLiteral("freehand");
    case MarkerMode::Line: return QStringLiteral("line");
    case MarkerMode::Rectangle: return QStringLiteral("rectangle");
    }
    return {};
}

std::optional<MarkerMode> markerModeFromName(QStringView name)
{
    for (MarkerMode mode : {MarkerMode::Freehand, MarkerMode::Line, MarkerMode::Rectangle}) {
        if (name == markerModeName(mode))
            return mode;
    }
    return std::nullopt;
}

bool isBrush(const Item& item)
{
    return item.kind == Kind::Redact && item.style.shape == RedactShape::Brush;
}

bool isStrokeItem(const Item& item)
{
    return item.kind == Kind::Pen || (item.kind == Kind::Marker && item.style.markerMode == MarkerMode::Freehand) || isBrush(item);
}

QString kindName(Kind kind)
{
    switch (kind) {
    case Kind::Rect:
        return u"rect"_s;
    case Kind::Ellipse:
        return u"ellipse"_s;
    case Kind::Arrow:
        return u"arrow"_s;
    case Kind::Line:
        return u"line"_s;
    case Kind::Pen:
        return u"pen"_s;
    case Kind::Marker:
        return u"marker"_s;
    case Kind::Text:
        return u"text"_s;
    case Kind::Number:
        return u"number"_s;
    case Kind::Redact:
        return u"redact"_s;
    }
    return {};
}

std::optional<Kind> kindFromName(QStringView name)
{
    for (int i = 0; i < kKindCount; ++i) {
        if (name == kindName(Kind(i)))
            return Kind(i);
    }
    return std::nullopt;
}

QString fillName(Fill fill)
{
    switch (fill) {
    case Fill::None:
        return u"none"_s;
    case Fill::Soft:
        return u"soft"_s;
    case Fill::Solid:
        return u"solid"_s;
    }
    return {};
}

std::optional<Fill> fillFromName(QStringView name)
{
    for (Fill f : {Fill::None, Fill::Soft, Fill::Solid}) {
        if (name == fillName(f))
            return f;
    }
    return std::nullopt;
}

QString redactModeName(RedactMode mode)
{
    switch (mode) {
    case RedactMode::Mosaic:
        return u"mosaic"_s;
    case RedactMode::Blur:
        return u"blur"_s;
    case RedactMode::Solid:
        return u"solid"_s;
    case RedactMode::Smooth:
        return u"smooth"_s;
    }
    return {};
}

std::optional<RedactMode> redactModeFromName(QStringView name)
{
    for (RedactMode m : {RedactMode::Mosaic, RedactMode::Blur, RedactMode::Solid, RedactMode::Smooth}) {
        if (name == redactModeName(m))
            return m;
    }
    return std::nullopt;
}

QString redactShapeName(RedactShape shape)
{
    return shape == RedactShape::Brush ? u"brush"_s : u"box"_s;
}

std::optional<RedactShape> redactShapeFromName(QStringView name)
{
    if (name == u"box")
        return RedactShape::Box;
    if (name == u"brush")
        return RedactShape::Brush;
    return std::nullopt;
}

QString numberFormatName(NumberFormat format)
{
    switch (format) {
    case NumberFormat::Arabic:
        return u"arabic"_s;
    case NumberFormat::UpperLetters:
        return u"upper"_s;
    case NumberFormat::LowerLetters:
        return u"lower"_s;
    case NumberFormat::Roman:
        return u"roman"_s;
    case NumberFormat::Circled:
        return u"circled"_s;
    }
    return u"arabic"_s;
}

std::optional<NumberFormat> numberFormatFromName(QStringView name)
{
    for (int i = 0; i < kNumberFormatCount; ++i) {
        const NumberFormat f = NumberFormat(i);
        if (name == numberFormatName(f))
            return f;
    }
    return std::nullopt;
}

QString formatNumber(int number, NumberFormat format)
{
    if (number < 1 || format == NumberFormat::Arabic || format == NumberFormat::Circled)
        return QString::number(number);
    if (format == NumberFormat::Roman) {
        if (number > 3999)
            return QString::number(number);
        static constexpr struct {
            int value;
            const char* text;
        } steps[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"}, {50, "L"}, {40, "XL"},
            {10, "X"}, {9, "IX"}, {5, "V"}, {4, "IV"}, {1, "I"}};
        QString out;
        for (const auto& step : steps) {
            while (number >= step.value) {
                out += QLatin1StringView(step.text);
                number -= step.value;
            }
        }
        return out;
    }
    // Letters count like a spreadsheet's columns: Z, then AA, AB ...
    const QChar first = format == NumberFormat::UpperLetters ? u'A' : u'a';
    QString out;
    while (number > 0) {
        --number;
        out.prepend(QChar(first.unicode() + number % 26));
        number /= 26;
    }
    return out;
}

std::optional<QRgb> parseColor(QStringView text)
{
    if (text.size() != 7 || text[0] != u'#')
        return std::nullopt;
    bool ok = false;
    const uint value = text.mid(1).toUInt(&ok, 16);
    if (!ok)
        return std::nullopt;
    return QRgb(value | 0xff000000u);
}

int redactionEnd(const Document& doc)
{
    int end = 0;
    while (end < doc.items.size() && doc.items[end].kind == Kind::Redact)
        ++end;
    return end;
}

int insertionIndex(const Document& doc, Kind kind, int at)
{
    const int end = redactionEnd(doc);
    const int low = kind == Kind::Redact ? 0 : end;
    const int high = kind == Kind::Redact ? end : int(doc.items.size());
    return at < 0 ? high : std::clamp(at, low, high);
}

int indexOf(const Document& doc, quint32 id)
{
    for (qsizetype i = 0; i < doc.items.size(); ++i) {
        if (doc.items[i].id == id)
            return int(i);
    }
    return -1;
}

const Item* findItem(const Document& doc, quint32 id)
{
    const int i = indexOf(doc, id);
    return i < 0 ? nullptr : &doc.items[i];
}

QPoint pixelAt(QPointF p)
{
    return QPoint(qFloor(p.x()), qFloor(p.y()));
}

QPointF pixelCentre(QPointF p)
{
    return QPointF(qFloor(p.x()) + 0.5, qFloor(p.y()) + 0.5);
}

QRect pixelBox(QPoint a, QPoint b)
{
    return QRect(QPoint(std::min(a.x(), b.x()), std::min(a.y(), b.y())), QPoint(std::max(a.x(), b.x()), std::max(a.y(), b.y())));
}

QRect itemBox(const Item& item)
{
    if (isBrush(item)) {
        QRect box;
        for (const QPointF& p : item.points)
            box |= QRect(pixelAt(p), QSize(1, 1));
        // Every pixel the stroke covers is within the radius of a point.
        const int m = (std::max(1, item.style.width) + 1) / 2;
        return box.isNull() ? box : box.adjusted(-m, -m, m, m);
    }
    if (item.points.size() < 2)
        return {};
    const QPointF a = item.points[0];
    const QPointF b = item.points[1];
    const int x0 = qRound(std::min(a.x(), b.x()));
    const int x1 = qRound(std::max(a.x(), b.x()));
    const int y0 = qRound(std::min(a.y(), b.y()));
    const int y1 = qRound(std::max(a.y(), b.y()));
    return QRect(x0, y0, x1 - x0, y1 - y0);
}

QImage brushCoverage(const Item& item)
{
    if (!isBrush(item))
        return {};
    const QRect box = itemBox(item);
    QImage mask(box.size(), QImage::Format_Grayscale8);
    if (mask.isNull())
        return mask;
    mask.fill(0);
    const qreal r = std::max(1, item.style.width) / 2.0;
    const qreal r2 = r * r;
    const int m = (std::max(1, item.style.width) + 1) / 2;
    // Each segment sets the pixels whose centre is within r of it, going
    // through its own bounding box only; whole numbers and halves make the
    // test the same on every computer.
    const auto mark = [&](QPointF a, QPointF b) {
        const QRect part = QRect(pixelAt(a), pixelAt(b)).normalized().adjusted(-m, -m, m, m) & box;
        for (int y = part.top(); y <= part.bottom(); ++y) {
            uchar* line = mask.scanLine(y - box.top());
            for (int x = part.left(); x <= part.right(); ++x) {
                const QPointF c(x + 0.5, y + 0.5);
                const qreal d = distanceToSegment(c, a, b);
                if (d * d <= r2)
                    line[x - box.left()] = 255;
            }
        }
    };
    if (item.points.size() == 1)
        mark(item.points[0], item.points[0]);
    for (qsizetype i = 0; i + 1 < item.points.size(); ++i)
        mark(item.points[i], item.points[i + 1]);
    return mask;
}

QList<Item> redactionItems(const Document& doc)
{
    QList<Item> items;
    for (const Item& item : doc.items) {
        if (item.kind == Kind::Redact)
            items.append(item);
    }
    return items;
}

QList<QRect> redactionBoxes(const Document& doc)
{
    QList<QRect> boxes;
    for (const Item& item : doc.items) {
        if (item.kind == Kind::Redact)
            boxes.append(itemBox(item));
    }
    return boxes;
}

QList<QPointF> boxPoints(const QRect& box)
{
    return {QPointF(box.x(), box.y()), QPointF(box.x() + box.width(), box.y() + box.height())};
}

Item translated(const Item& item, QPoint delta)
{
    Item out = item;
    for (QPointF& p : out.points)
        p += QPointF(delta);
    return out;
}

QList<QPointF> alignedPoints(const Item& item)
{
    QList<QPointF> points = item.points;
    if (points.size() < 2 || std::max(1, item.style.width) % 2 != 0)
        return points;
    bool level = true;
    bool upright = true;
    for (const QPointF& p : points) {
        level = level && p.y() == points[0].y();
        upright = upright && p.x() == points[0].x();
    }
    if (level && upright)
        return points; // a dot
    if (level) {
        for (QPointF& p : points)
            p.ry() -= 0.5;
    } else if (upright) {
        for (QPointF& p : points)
            p.rx() -= 0.5;
    }
    return points;
}

qreal arrowHeadLength(int width, qreal dpr)
{
    return std::max(4.0 * std::max(1, width), 12.0 * dpr);
}

ArrowShape arrowShape(const Item& item, qreal dpr)
{
    ArrowShape shape;
    const QList<QPointF> points = alignedPoints(item);
    if (points.size() < 2)
        return shape;
    const QPointF s = points[0];
    const QPointF e = points[1];
    const QPointF v = e - s;
    const qreal length = std::hypot(v.x(), v.y());
    if (length <= 0)
        return shape;
    const QPointF u = v / length;
    const QPointF n(-u.y(), u.x());
    const int w = std::max(1, item.style.width);
    const qreal head = std::min(arrowHeadLength(w, dpr), length);
    const qreal half = 0.45 * head;
    const QPointF root = e - u * head;
    shape.head = QPolygonF{e, root + n * half, root - n * half};
    if (length > head) {
        // The tail reaches half a pixel back, to the edge of the pixel it
        // starts on, so that a level arrow's tail is a crisp pixel edge too.
        // The shaft ends halfway into the head: no seam at the head's root.
        const QPointF tail = s - u * 0.5;
        const QPointF end = e - u * (head / 2);
        const qreal r = w / 2.0;
        shape.shaft = QPolygonF{tail + n * r, end + n * r, end - n * r, tail - n * r};
        orient(shape.shaft);
    }
    orient(shape.head);
    return shape;
}

QPainterPath linePath(const Item& item)
{
    const QList<QPointF> points = alignedPoints(item);
    QPainterPath path;
    if (points.size() < 2)
        return path;
    path.moveTo(points[0]);
    if (isLine(item.kind) && item.style.lineMode == LineMode::Bezier && points.size() == 4) {
        path.cubicTo(points[2], points[3], points[1]);
    } else {
        for (qsizetype i = 1; i < points.size(); ++i)
            path.lineTo(points[i]);
        if (item.closed)
            path.closeSubpath();
    }
    return path;
}

QPainterPath arrowPath(const Item& item, qreal dpr)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    if (item.style.lineMode != LineMode::Bezier || item.points.size() != 4) {
        const ArrowShape shape = arrowShape(item, dpr);
        if (!shape.head.isEmpty()) {
            path.addPolygon(shape.head);
            path.closeSubpath();
        }
        if (!shape.shaft.isEmpty()) {
            path.addPolygon(shape.shaft);
            path.closeSubpath();
        }
        return path;
    }
    const QPainterPath centre = linePath(item);
    const qreal length = centre.length();
    if (length <= 0)
        return path;
    const QPointF start = item.points[0], end = item.points[1];
    const QPointF c1 = item.points[2], c2 = item.points[3];
    QPointF tangent = end - c2;
    if (qFuzzyIsNull(std::hypot(tangent.x(), tangent.y())))
        tangent = end - c1;
    if (qFuzzyIsNull(std::hypot(tangent.x(), tangent.y())))
        tangent = end - start;
    const qreal magnitude = std::hypot(tangent.x(), tangent.y());
    if (magnitude <= 0)
        return path;
    const QPointF unit = tangent / magnitude;
    const QPointF normal(-unit.y(), unit.x());
    const qreal head = std::min(arrowHeadLength(item.style.width, dpr), length);
    const QPointF root = end - unit * head;
    path.addPolygon(QPolygonF{end, root + normal * (head * 0.45), root - normal * (head * 0.45)});
    path.closeSubpath();
    if (length > head) {
        // Split the cubic with de Casteljau; the shaft ends inside the head.
        const qreal t = centre.percentAtLength(length - head * 0.55);
        const auto mix = [t](QPointF a, QPointF b) { return a + (b - a) * t; };
        const QPointF a = mix(start, c1), b = mix(c1, c2), c = mix(c2, end);
        const QPointF d = mix(a, b), e = mix(b, c);
        QPainterPath shaft(start);
        shaft.cubicTo(a, d, mix(d, e));
        QPainterPathStroker stroker;
        stroker.setWidth(std::max(1, item.style.width));
        stroker.setCapStyle(Qt::RoundCap);
        path = path.united(stroker.createStroke(shaft));
    }
    return path;
}

QPainterPath strokeCentreline(const QList<QPointF>& points)
{
    QPainterPath path;
    if (points.isEmpty())
        return path;
    path.moveTo(points[0]);
    if (points.size() == 2) {
        path.lineTo(points[1]);
        return path;
    }
    for (qsizetype i = 1; i + 1 < points.size(); ++i)
        path.quadTo(points[i], (points[i] + points[i + 1]) / 2);
    if (points.size() > 1)
        path.lineTo(points.last());
    return path;
}

QList<QPointF> simplifyStroke(const QList<QPointF>& points, qreal tolerance)
{
    const qsizetype n = points.size();
    if (n < 3)
        return points;
    std::vector<bool> keep(std::size_t(n), false);
    keep.front() = true;
    keep.back() = true;
    // A stack instead of recursion: a long stroke has thousands of points.
    std::vector<std::pair<qsizetype, qsizetype>> spans{{0, n - 1}};
    while (!spans.empty()) {
        const auto [a, b] = spans.back();
        spans.pop_back();
        qreal worst = -1;
        qsizetype at = -1;
        for (qsizetype i = a + 1; i < b; ++i) {
            const qreal d = distanceToSegment(points[i], points[a], points[b]);
            if (d > worst) {
                worst = d;
                at = i;
            }
        }
        if (at >= 0 && worst > tolerance) {
            keep[std::size_t(at)] = true;
            spans.emplace_back(a, at);
            spans.emplace_back(at, b);
        }
    }
    QList<QPointF> out;
    for (qsizetype i = 0; i < n; ++i) {
        if (keep[std::size_t(i)])
            out.append(points[i]);
    }
    return out;
}

QFont annotationFont(int pixelSize, bool bold)
{
    // The name is the system's UI font of Chinese Windows, there on every
    // Windows 10 and 11; Qt falls back to another one if it is missing. The
    // bundled UI font (Alibaba PuHuiTi) is not used: a project must draw the
    // same in a unit test and on another computer, without WinShun's files.
    QFont font(u"Microsoft YaHei UI"_s);
    font.setPixelSize(std::max(1, pixelSize));
    font.setWeight(bold ? QFont::Bold : QFont::Normal);
    font.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));
    // As the UI (main.cpp): full hinting would also move glyphs sideways.
    font.setHintingPreference(QFont::PreferVerticalHinting);
    return font;
}

TextLayout layoutText(const QString& text, int pixelSize, bool bold)
{
    TextLayout out;
    TextMetrics& m = out.metrics;
    const QFont font = annotationFont(pixelSize, bold);
    const QFontMetricsF metrics(font);
    m.ascent = qCeil(metrics.ascent());
    const int descent = qCeil(metrics.descent());
    m.pitch = std::max(1, qRound(metrics.lineSpacing()));
    m.capHeight = metrics.capHeight();
    const QStringList lines = text.split(u'\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        line.remove(u'\r');
        if (line.isEmpty())
            continue;
        QTextLayout layout(line, font);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout.setTextOption(option);
        layout.beginLayout();
        QTextLine textLine = layout.createLine();
        if (textLine.isValid()) {
            textLine.setLineWidth(1e6);
            textLine.setPosition(QPointF(0, 0));
        }
        layout.endLayout();
        if (!textLine.isValid())
            continue;
        m.width = std::max({m.width, textLine.naturalTextWidth(), textLine.horizontalAdvance()});
        // QTextLayout puts the baseline at the line's own ascent, which a
        // fallback font (emoji) can make taller; every line's baseline is
        // put on a whole pixel instead. Glyph positions stay multiples of
        // 1/64 px, which the raster engine takes exactly, so a text moved by
        // whole pixels draws exactly the same pixels moved.
        const qreal shift = (m.ascent + qreal(i) * m.pitch) - textLine.ascent();
        const QList<QGlyphRun> runs = layout.glyphRuns();
        for (QGlyphRun run : runs) {
            QList<QPointF> positions = run.positions();
            const QList<quint32> glyphs = run.glyphIndexes();
            const QRawFont raw = run.rawFont();
            for (qsizetype j = 0; j < positions.size(); ++j) {
                positions[j] = QPointF(snap64(positions[j].x()), snap64(positions[j].y() + shift));
                if (j < glyphs.size())
                    m.ink |= raw.boundingRect(glyphs[j]).translated(positions[j]);
            }
            run.setPositions(positions);
            out.runs.append(run);
        }
    }
    m.size = QSize(qCeil(m.width), m.ascent + descent + int(lines.size() - 1) * m.pitch);
    return out;
}

TextMetrics textMetrics(const QString& text, int pixelSize, bool bold)
{
    QString key = QString::number(pixelSize);
    key += bold ? u'b' : u'n';
    key += text;
    MetricsMemo& memo = t_memo;
    const auto found = memo.index.constFind(key);
    if (found != memo.index.constEnd()) {
        memo.order.splice(memo.order.begin(), memo.order, *found);
        return memo.order.front().second;
    }
    const TextMetrics m = layoutText(text, pixelSize, bold).metrics;
    memo.order.emplace_front(key, m);
    memo.index.insert(key, memo.order.begin());
    if (memo.order.size() > MetricsMemo::kSize) {
        memo.index.remove(memo.order.back().first);
        memo.order.pop_back();
    }
    return m;
}

void clearTextMetrics()
{
    t_memo.order.clear();
    t_memo.index.clear();
}

QRgb inkOn(QRgb fill)
{
    return brightness(fill) >= kLight ? qRgb(0x21, 0x21, 0x21) : qRgb(0xFF, 0xFF, 0xFF);
}

QRgb numberOutlineColor()
{
    return qRgba(0x21, 0x21, 0x21, 77);
}

int textStrokeWidth(const Item& item)
{
    if (item.kind != Kind::Text || item.style.boxed)
        return 0;
    return std::clamp(item.style.textStroke, 0, kMaxTextStroke);
}

QRgb textStrokeColorOf(const Item& item)
{
    if (item.style.textStrokeColor != 0)
        return item.style.textStrokeColor | 0xff000000u;
    return inkOn(item.style.color);
}

TextGeometry textGeometry(const Item& item)
{
    TextGeometry g;
    if (item.points.isEmpty())
        return g;
    const int f = std::max(1, item.style.fontPx);
    const TextMetrics m = textMetrics(item.text, f, item.style.bold);
    const QPoint topLeft = pixelAt(item.points[0]);
    g.content = QRect(topLeft, m.size);
    g.box = g.content;
    if (item.style.boxed) {
        const int pad = qRound(0.3 * f);
        g.box = g.content.adjusted(-pad, -pad, pad, pad);
        g.radius = 0.25 * f;
    }
    g.ink = m.ink.translated(QPointF(topLeft));
    if (const int stroke = textStrokeWidth(item); stroke > 0 && !g.ink.isEmpty())
        g.ink.adjust(-stroke, -stroke, stroke, stroke);
    return g;
}

QRgb numberWordsColor(const Item& item)
{
    return brightness(item.style.color) >= kWhitish ? qRgb(0x21, 0x21, 0x21) : item.style.color | 0xff000000u;
}

NumberGeometry numberGeometry(const Item& item, int number, qreal dpr, NumberFormat format)
{
    NumberGeometry g;
    if (item.points.isEmpty())
        return g;
    const int f = std::max(1, item.style.fontPx);
    // Odd sizes around a pixel centre put the disc's box on pixel edges.
    int d = qRound(1.5 * f);
    if (d % 2 == 0)
        ++d;
    g.label = formatNumber(number, format);
    const TextMetrics m = textMetrics(g.label, f, true);
    int width = d;
    if (g.label.size() > 1) {
        width = std::max(d, qCeil(m.width + 0.6 * d));
        if (width % 2 == 0)
            ++width;
    }
    const QPointF c = pixelCentre(item.points[0]);
    g.disc = QRectF(c.x() - width / 2.0, c.y() - d / 2.0, width, d);
    // Digits stand on the baseline and are about the capitals' height.
    const int x = qFloor(c.x() - m.width / 2 + 0.5);
    const int baseline = qFloor(c.y() + m.capHeight / 2 + 0.5);
    g.origin = QPoint(x, baseline - m.ascent);
    g.ink = m.ink.translated(QPointF(g.origin));
    if (format == NumberFormat::Circled)
        g.ring = std::max(2.0, std::round(f / 9.0)); // a hollow disc: no dark outline needed, the digits carry the colour
    else if (brightness(item.style.color) >= kWhitish)
        g.outline = std::max(1, qRound(dpr));
    // The words: their first baseline on the digits'.
    const TextMetrics words = textMetrics(item.text, f, false);
    g.wordsOrigin = QPoint(qCeil(g.disc.right()) + std::max(3, qRound(0.35 * f)), baseline - words.ascent);
    if (!item.text.trimmed().isEmpty()) {
        g.wordsBox = QRect(g.wordsOrigin, words.size);
        g.wordsInk = words.ink.translated(QPointF(g.wordsOrigin));
    }
    return g;
}

QRectF bounds(const Item& item, qreal dpr, int number, NumberFormat format)
{
    const int w = std::max(1, item.style.width);
    switch (item.kind) {
    case Kind::Rect:
    case Kind::Ellipse:
    case Kind::Redact:
        return QRectF(itemBox(item));
    case Kind::Arrow: {
        const QRectF r = arrowPath(item, dpr).boundingRect();
        return r.isNull() ? QRectF() : r.adjusted(-1, -1, 1, 1);
    }
    case Kind::Line:
    case Kind::Pen:
    case Kind::Marker: {
        if (item.kind == Kind::Marker && item.style.markerMode == MarkerMode::Rectangle)
            return QRectF(itemBox(item));
        const QList<QPointF> points = alignedPoints(item);
        if (points.isEmpty())
            return {};
        if (item.kind == Kind::Line && (points.size() < 2 || same(points[0], points[1])))
            return {}; // a line of no length draws nothing
        // Round caps and joins reach half the width past the points; the
        // smoothed curve stays inside the polyline's hull.
        const qreal m = w / 2.0 + 1;
        const QRectF area = item.kind == Kind::Line ? linePath(item).boundingRect() : QPolygonF(points).boundingRect();
        return area.adjusted(-m, -m, m, m);
    }
    case Kind::Text: {
        if (item.points.isEmpty() || item.text.isEmpty())
            return {};
        const TextGeometry g = textGeometry(item);
        QRectF r = QRectF(g.box) | QRectF(g.content);
        if (!g.ink.isEmpty())
            r |= g.ink.adjusted(-2, -2, 2, 2);
        return r.adjusted(-1, -1, 1, 1);
    }
    case Kind::Number: {
        if (item.points.isEmpty())
            return {};
        const NumberGeometry g = numberGeometry(item, number, dpr, format);
        QRectF r = g.disc;
        if (!g.ink.isEmpty())
            r |= g.ink.adjusted(-2, -2, 2, 2);
        if (!g.wordsBox.isEmpty()) {
            r |= QRectF(g.wordsBox);
            if (!g.wordsInk.isEmpty())
                r |= g.wordsInk.adjusted(-2, -2, 2, 2);
        }
        return r.adjusted(-1, -1, 1, 1);
    }
    }
    return {};
}

QRectF bounds(const Document& doc, const Item& item)
{
    const int number = item.kind == Kind::Number ? numbers(doc).value(item.id, 1) : 1;
    return bounds(item, doc.dpr, number, formatOf(doc, item.series));
}

std::optional<quint32> hitTest(const Document& doc, QPointF p, qreal tolerance)
{
    const QPointF q = pixelCentre(p);
    std::optional<QHash<quint32, int>> shown;
    for (qsizetype i = doc.items.size() - 1; i >= 0; --i) {
        const Item& item = doc.items[i];
        int number = 1;
        if (item.kind == Kind::Number) {
            if (!shown)
                shown = numbers(doc);
            number = shown->value(item.id, 1);
        }
        if (hits(item, q, tolerance, doc.dpr, number, formatOf(doc, item.series)))
            return item.id;
    }
    return std::nullopt;
}

QList<QPointF> handles(const Item& item)
{
    if (isBoxItem(item)) {
        const QRect box = itemBox(item);
        const qreal x0 = box.x();
        const qreal y0 = box.y();
        const qreal x1 = box.x() + box.width();
        const qreal y1 = box.y() + box.height();
        const qreal xm = (x0 + x1) / 2;
        const qreal ym = (y0 + y1) / 2;
        return {{x0, y0}, {xm, y0}, {x1, y0}, {x1, ym}, {x1, y1}, {xm, y1}, {x0, y1}, {x0, ym}};
    }
    if (isLineItem(item) && item.points.size() >= 2)
        return item.points;
    return {};
}

Item reshaped(const Item& item, int handle, QPointF to, bool constrain)
{
    Item out = item;
    const QPoint pixel = pixelAt(to);
    if (isBoxItem(item)) {
        if (handle < int(BoxHandle::TopLeft) || handle > int(BoxHandle::Left))
            return out;
        const QRect box = itemBox(item);
        // The first and last pixel column and row the box covers.
        const int left = box.x();
        const int right = box.x() + std::max(0, box.width() - 1);
        const int top = box.y();
        const int bottom = box.y() + std::max(0, box.height() - 1);
        const auto h = BoxHandle(handle);
        const bool movesLeft = h == BoxHandle::TopLeft || h == BoxHandle::Left || h == BoxHandle::BottomLeft;
        const bool movesRight = h == BoxHandle::TopRight || h == BoxHandle::Right || h == BoxHandle::BottomRight;
        const bool movesTop = h == BoxHandle::TopLeft || h == BoxHandle::Top || h == BoxHandle::TopRight;
        const bool movesBottom = h == BoxHandle::BottomLeft || h == BoxHandle::Bottom || h == BoxHandle::BottomRight;
        const bool movesX = movesLeft || movesRight;
        const bool movesY = movesTop || movesBottom;
        const int anchorX = movesLeft ? right : left;
        const int anchorY = movesTop ? bottom : top;
        int x = pixel.x();
        int y = pixel.y();
        if (constrain && movesX && movesY) {
            const int dx = x - anchorX;
            const int dy = y - anchorY;
            const int side = std::max(std::abs(dx), std::abs(dy));
            const int sx = dx != 0 ? (dx > 0 ? 1 : -1) : (movesRight ? 1 : -1);
            const int sy = dy != 0 ? (dy > 0 ? 1 : -1) : (movesBottom ? 1 : -1);
            x = anchorX + sx * side;
            y = anchorY + sy * side;
        }
        const int x0 = movesX ? std::min(anchorX, x) : left;
        const int x1 = movesX ? std::max(anchorX, x) : right;
        const int y0 = movesY ? std::min(anchorY, y) : top;
        const int y1 = movesY ? std::max(anchorY, y) : bottom;
        out.points = boxPoints(QRect(QPoint(x0, y0), QPoint(x1, y1)));
        return out;
    }
    if (isLineItem(item)) {
        if (item.points.size() < 2 || handle < 0 || handle >= item.points.size())
            return out;
        QPointF p = pixelCentre(to);
        if (constrain)
            p = snapAngle(item.points[handle == 0 ? 1 : handle - 1], p);
        if (isLine(item.kind) && item.style.lineMode == LineMode::Bezier && item.points.size() == 4 && handle < 2)
            out.points[handle + 2] += p - out.points[handle];
        out.points[handle] = p;
        return out;
    }
    if (isStrokeItem(item) && handle == int(LineHandle::End)) {
        const QPointF p = pixelCentre(to);
        if (out.points.isEmpty()) {
            out.points = {p};
        } else if (constrain) {
            out.points = {out.points.first(), snapAngle(out.points.first(), p)};
        } else if (!same(out.points.last(), p)) {
            out.points.append(p);
        }
        return out;
    }
    return out;
}

NumberFormat formatOf(const Document& doc, quint32 series)
{
    for (const Series& s : doc.series) {
        if (s.id == series)
            return s.format;
    }
    return NumberFormat::Arabic;
}

QHash<quint32, int> numbers(const Document& doc)
{
    QList<const Item*> list;
    for (const Item& item : doc.items) {
        if (item.kind == Kind::Number)
            list.append(&item);
    }
    std::stable_sort(list.begin(), list.end(), [](const Item* a, const Item* b) {
        return a->series != b->series ? a->series < b->series : a->seq < b->seq;
    });
    QHash<quint32, int> out;
    out.reserve(list.size());
    int next = 1;
    for (qsizetype i = 0; i < list.size(); ++i) {
        const Item* item = list[i];
        if (i == 0 || item->series != list[i - 1]->series)
            next = startOf(doc, item->series);
        if (item->pinned)
            next = *item->pinned;
        out.insert(item->id, next);
        ++next;
    }
    return out;
}

}

// The annotations' pure core (src/capture/Annotation*): geometry and hit
// testing, crisp strokes on whole pixels, tiles that are exactly the whole
// picture, the command stack, numbers, redactions that cannot be undone, the
// project format with its v1 sample, the overlay turned into a project, the
// remembered styles and the tool table (docs/design-capture-roadmap.md,
// 3.14). Random cases come from fixed seeds, so a failure fails again.
#include "Annotation.h"
#include "AnnotationCommands.h"
#include "AnnotationFormat.h"
#include "AnnotationRender.h"
#include "AnnotationStyles.h"
#include "EditView.h"
#include "Geometry.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QStringList>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

using namespace ws::capture::annot;
using namespace Qt::StringLiterals;

namespace ws::capture::annot {

// QCOMPARE shows a document as its doc.json.
char* toString(const Document& doc)
{
    return qstrdup(QJsonDocument(toJson(doc)).toJson(QJsonDocument::Compact).constData());
}

} // namespace ws::capture::annot

namespace {

constexpr QRgb kWhite = 0xffffffffu;
constexpr QRgb kRed = 0xffe53935u;
constexpr QRgb kOrange = 0xfffb8c00u;
constexpr QRgb kYellow = 0xfffdd835u;
constexpr QRgb kGreen = 0xff43a047u;
constexpr QRgb kBlue = 0xff1e88e5u;
constexpr QRgb kPurple = 0xff8e24aau;
constexpr QRgb kDark = 0xff212121u;

// splitmix64: the same numbers with every compiler and library.
class Rng {
public:
    explicit Rng(quint64 seed)
        : m_state(seed)
    {
    }
    quint64 next()
    {
        quint64 z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // A whole number in [low, high].
    int range(int low, int high) { return low + int(next() % quint64(qint64(high) - low + 1)); }
    bool chance(int percent) { return range(0, 99) < percent; }

private:
    quint64 m_state;
};

// --- pictures ---------------------------------------------------------------

QImage noiseImage(QSize size, quint64 seed)
{
    Rng rng(seed);
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x)
            line[x] = 0xff000000u | QRgb(rng.next() & 0xffffffu);
    }
    return image;
}

QImage plainImage(QSize size, QRgb color)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

void blit(QImage& target, QPoint at, const QImage& piece)
{
    for (int y = 0; y < piece.height(); ++y)
        std::memcpy(target.scanLine(at.y() + y) + at.x() * 4, piece.constScanLine(y), std::size_t(piece.width()) * 4);
}

struct Diff {
    int count = 0; // -1: the sizes or formats differ
    QPoint first;
    QRgb got = 0;
    QRgb want = 0;
};

Diff compare(const QImage& got, const QImage& want)
{
    Diff d;
    if (got.size() != want.size() || got.format() != QImage::Format_RGB32 || want.format() != QImage::Format_RGB32) {
        d.count = -1;
        return d;
    }
    for (int y = 0; y < got.height(); ++y) {
        const QRgb* g = reinterpret_cast<const QRgb*>(got.constScanLine(y));
        const QRgb* w = reinterpret_cast<const QRgb*>(want.constScanLine(y));
        for (int x = 0; x < got.width(); ++x) {
            if (g[x] == w[x])
                continue;
            if (d.count == 0) {
                d.first = QPoint(x, y);
                d.got = g[x];
                d.want = w[x];
            }
            ++d.count;
        }
    }
    return d;
}

QString hex(QRgb c)
{
    return u"#%1"_s.arg(c, 8, 16, QChar(u'0'));
}

// JSON written with ' for ": moc loses the test class when a raw string
// literal holds a quote followed by '#'.
QByteArray jsonText(const char* text)
{
    QByteArray out(text);
    out.replace('\'', '"');
    return out;
}

QString describe(const Diff& d)
{
    if (d.count < 0)
        return u"sizes or formats differ"_s;
    if (d.count == 0)
        return u"the same"_s;
    return u"%1 pixels differ, the first at (%2, %3): %4, want %5"_s.arg(d.count)
        .arg(d.first.x())
        .arg(d.first.y())
        .arg(hex(d.got), hex(d.want));
}

// The first pixel that differs between a and b outside `region`.
std::optional<QPoint> changedOutside(const QImage& a, const QImage& b, const QRect& region)
{
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* la = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* lb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (la[x] != lb[x] && !region.contains(x, y))
                return QPoint(x, y);
        }
    }
    return std::nullopt;
}

// --- documents --------------------------------------------------------------

Document emptyDoc(QSize canvas, qreal dpr = 1.0)
{
    Document doc;
    doc.canvas = canvas;
    doc.crop = QRect(QPoint(0, 0), canvas);
    doc.dpr = dpr;
    return doc;
}

// Adds `item` as the editor does: the next id and seq, at the top of its
// segment, a Number in the current series.
quint32 addItem(Document& doc, Item item)
{
    item.id = doc.nextId++;
    item.seq = doc.nextSeq++;
    if (item.kind == Kind::Number && item.series == 0)
        item.series = doc.currentSeries;
    doc.items.insert(insertionIndex(doc, item.kind, -1), item);
    return item.id;
}

QList<quint32> idsOf(const Document& doc)
{
    QList<quint32> ids;
    for (const Item& item : doc.items)
        ids.append(item.id);
    return ids;
}

Item boxItem(Kind kind, const QRect& box, QRgb color = kRed, int width = 6, Fill fill = Fill::None)
{
    Item item;
    item.kind = kind;
    item.points = boxPoints(box);
    item.style.color = color;
    item.style.width = width;
    item.style.fill = fill;
    return item;
}

Item lineItem(Kind kind, QPointF a, QPointF b, QRgb color = kRed, int width = 6)
{
    Item item;
    item.kind = kind;
    item.points = {a, b};
    item.style.color = color;
    item.style.width = width;
    return item;
}

Item strokeItem(Kind kind, const QList<QPointF>& points, QRgb color = kRed, int width = 6)
{
    Item item;
    item.kind = kind;
    item.points = points;
    item.style.color = color;
    item.style.width = width;
    return item;
}

Item textItem(QPoint topLeft, const QString& text, int pixelSize = 27, bool bold = false, bool boxed = false,
    QRgb color = kRed)
{
    Item item;
    item.kind = Kind::Text;
    item.points = {QPointF(topLeft)};
    item.text = text;
    item.style.color = color;
    item.style.fontPx = pixelSize;
    item.style.bold = bold;
    item.style.boxed = boxed;
    return item;
}

Item numberItem(QPointF centre, quint32 series = 0, QRgb color = kRed, int pixelSize = 27)
{
    Item item;
    item.kind = Kind::Number;
    item.points = {centre};
    item.series = series;
    item.style.color = color;
    item.style.fontPx = pixelSize;
    return item;
}

Item redactItem(const QRect& box, RedactMode mode, int cell, quint64 seed)
{
    Item item = boxItem(Kind::Redact, box, kDark);
    item.style.redact = mode;
    item.style.cell = cell;
    item.seed = seed;
    return item;
}

const QStringList& sampleTexts()
{
    static const QStringList texts{u"Hello"_s, u"你好\nWin顺"_s, u"a"_s, u"多行\n\n第三行"_s, u"Emoji 😀 ok"_s,
        u"WWW\niii"_s, u"Ångström ß"_s, u"截图标注 123"_s};
    return texts;
}

QPointF centreIn(Rng& rng, const QRect& r)
{
    return QPointF(rng.range(r.left(), r.right()) + 0.5, rng.range(r.top(), r.bottom()) + 0.5);
}

QPoint pointIn(Rng& rng, const QRect& r)
{
    return QPoint(rng.range(r.left(), r.right()), rng.range(r.top(), r.bottom()));
}

Style randomStyle(Rng& rng)
{
    // 65 px and up are drawn as layers, the rest from the glyph cache.
    static const int fontSizes[] = {9, 12, 14, 18, 21, 27, 36, 48, 64, 65, 72, 80};
    Style s;
    s.color = palette()[rng.range(0, int(palette().size()) - 1)];
    s.width = rng.chance(25) ? rng.range(1, 3) : rng.range(1, 16);
    s.fill = Fill(rng.range(0, 2));
    s.fontPx = fontSizes[rng.range(0, int(std::size(fontSizes)) - 1)];
    s.bold = rng.chance(50);
    s.boxed = rng.chance(40);
    s.redact = RedactMode(rng.range(0, 3));
    s.cell = rng.range(3, 30);
    return s;
}

Item randomItem(Rng& rng, const QRect& region, const QList<Series>& series)
{
    Item item;
    item.kind = Kind(rng.range(0, kKindCount - 1));
    item.style = randomStyle(rng);
    const QPointF a = centreIn(rng, region);
    switch (item.kind) {
    case Kind::Rect:
    case Kind::Ellipse:
    case Kind::Redact: {
        if (item.kind == Kind::Redact && rng.chance(15)) {
            item.style.shape = RedactShape::Brush;
            for (int i = rng.range(1, 5); i > 0; --i)
                item.points.append(centreIn(rng, region));
            break;
        }
        const QPoint p = pointIn(rng, region);
        item.points = {QPointF(p), QPointF(p + QPoint(rng.range(-260, 260), rng.range(-200, 200)))};
        break;
    }
    case Kind::Arrow:
    case Kind::Line: {
        QPointF b = a + QPointF(rng.range(-400, 400), rng.range(-300, 300));
        const int shape = rng.range(0, 9);
        if (shape == 0)
            b.setY(a.y()); // level
        else if (shape == 1)
            b.setX(a.x()); // upright
        else if (shape == 2)
            b = a + QPointF(rng.range(-3, 3), rng.range(-3, 3)); // short, or of no length
        item.points = {a, b};
        break;
    }
    case Kind::Pen:
    case Kind::Marker: {
        item.points = {a};
        const bool level = rng.chance(15);
        QPointF p = a;
        for (int i = rng.chance(10) ? 0 : rng.range(1, 50); i > 0; --i) {
            p += QPointF(rng.range(-30, 30), level ? 0 : rng.range(-30, 30));
            item.points.append(p);
        }
        break;
    }
    case Kind::Text:
        item.points = {QPointF(pointIn(rng, region))};
        item.text = sampleTexts()[rng.range(0, int(sampleTexts().size()) - 1)];
        break;
    case Kind::Number:
        item.points = {a};
        item.series = series[rng.range(0, int(series.size()) - 1)].id;
        if (rng.chance(15))
            item.pinned = rng.range(1, 120);
        break;
    }
    if (item.kind == Kind::Redact)
        item.seed = rng.next();
    return item;
}

// `count` random items in `region` (the canvas and 60 px around it when
// empty), at random places of their segments.
Document randomDoc(Rng& rng, QSize canvas, int count, QRect region = {})
{
    static const qreal scales[] = {1.0, 1.25, 1.5, 1.75, 2.0};
    Document doc = emptyDoc(canvas, scales[rng.range(0, 4)]);
    if (rng.chance(50)) {
        doc.series.append(Series{2, rng.range(-3, 40)});
        doc.currentSeries = 2;
    }
    if (region.isEmpty())
        region = QRect(QPoint(0, 0), canvas).adjusted(-60, -60, 60, 60);
    for (int i = 0; i < count; ++i) {
        Item item = randomItem(rng, region, doc.series);
        item.id = doc.nextId++;
        item.seq = doc.nextSeq++;
        const int at = rng.chance(20) ? rng.range(0, int(doc.items.size())) : -1;
        doc.items.insert(insertionIndex(doc, item.kind, at), item);
    }
    return doc;
}

// --- drawing ----------------------------------------------------------------

QImage renderArea(Compositor& compositor, const Document& doc, const QImage& base, const QRect& sample, const QRect& area)
{
    QImage out;
    compositor.render(doc, base, sample, area, &out);
    return out;
}

// The area drawn in `tile` px tiles on a grid through `origin`, put together.
QImage renderTiled(Compositor& compositor, const Document& doc, const QImage& base, const QRect& sample,
    const QRect& area, QPoint origin, int tile)
{
    QImage out(area.size(), QImage::Format_RGB32);
    out.fill(0xffff00ffu);
    const int x0 = origin.x() + int(std::floor(double(area.left() - origin.x()) / tile)) * tile;
    const int y0 = origin.y() + int(std::floor(double(area.top() - origin.y()) / tile)) * tile;
    QImage piece;
    for (int y = y0; y <= area.bottom(); y += tile) {
        for (int x = x0; x <= area.right(); x += tile) {
            const QRect r = QRect(x, y, tile, tile) & area;
            if (r.isEmpty())
                continue;
            compositor.render(doc, base, sample, r, &piece);
            blit(out, r.topLeft() - area.topLeft(), piece);
        }
    }
    return out;
}

// After a failure: the items that may paint `p`, bottom to top.
QString itemsAt(const Document& doc, QPoint p)
{
    QStringList out;
    for (const Item& item : doc.items) {
        if (!bounds(doc, item).toAlignedRect().contains(p))
            continue;
        QString what = u"%1 %2"_s.arg(kindName(item.kind)).arg(item.id);
        if (item.kind == Kind::Text || item.kind == Kind::Number)
            what += u" %1px%2%3"_s.arg(item.style.fontPx).arg(item.style.bold ? u" bold"_s : QString(), item.style.boxed ? u" boxed"_s : QString());
        if (item.kind == Kind::Text)
            what += u" \""_s + item.text.left(12) + u'"';
        what += u" " + hex(item.style.color);
        out.append(what);
    }
    return out.join(u", "_s);
}

// After a tile test failed: the items that differ on their own.
QString culprits(const Document& doc, const QImage& base, const QRect& sample, const QRect& area, QPoint origin, int tile)
{
    QStringList out;
    const QHash<quint32, int> shown = numbers(doc);
    for (const Item& item : doc.items) {
        Document one = doc;
        one.items = {item};
        if (item.kind == Kind::Number)
            one.items[0].pinned = shown.value(item.id, 1);
        Compositor whole;
        Compositor tiles;
        const Diff d = compare(renderTiled(tiles, one, base, sample, area, origin, tile), renderArea(whole, one, base, sample, area));
        if (d.count != 0)
            out.append(u"%1 %2 (size %3): %4"_s.arg(kindName(item.kind)).arg(item.id).arg(item.style.fontPx).arg(describe(d)));
    }
    return out.isEmpty() ? u"no item on its own"_s : out.join(u"; "_s);
}

// For a failure's message: the items under the first pixel that differs
// (`origin`: the image's top left on the canvas).
QString firstAt(const Diff& d, const Document& doc, QPoint origin)
{
    return d.count > 0 ? u"; under it: "_s + itemsAt(doc, d.first + origin) : QString();
}

// The zip's entries as its central directory lists them: the name and the
// compression method (0 stored, 8 deflated).
QList<std::pair<QByteArray, int>> zipEntries(const QByteArray& zip)
{
    QList<std::pair<QByteArray, int>> out;
    const auto u16 = [&](qsizetype at) { return int(quint8(zip[at])) | int(quint8(zip[at + 1])) << 8; };
    const auto u32 = [&](qsizetype at) { return quint32(u16(at)) | quint32(u16(at + 2)) << 16; };
    qsizetype end = zip.size() - 22;
    while (end >= 0 && u32(end) != 0x06054b50u)
        --end;
    if (end < 0)
        return out;
    const int count = u16(end + 10);
    qsizetype at = qsizetype(u32(end + 16));
    for (int i = 0; i < count && at + 46 <= zip.size() && u32(at) == 0x02014b50u; ++i) {
        const int method = u16(at + 10);
        const int nameLength = u16(at + 28);
        out.append({zip.mid(at + 46, nameLength), method});
        at += 46 + nameLength + u16(at + 30) + u16(at + 32);
    }
    return out;
}

// --- commands ---------------------------------------------------------------

// Random commands for a document: every kind, often on the items of the
// command before (so that steps merge), batches made step by step.
class CommandMaker {
public:
    explicit CommandMaker(Rng& rng)
        : m_rng(rng)
    {
    }

    std::unique_ptr<Command> make(const Document& doc, int depth = 0)
    {
        QList<quint32> all;
        QList<quint32> texts;
        QList<quint32> numbered;
        for (const Item& item : doc.items) {
            all.append(item.id);
            if (item.kind == Kind::Text)
                texts.append(item.id);
            if (item.kind == Kind::Number)
                numbered.append(item.id);
        }
        const bool recentLive = !m_recent.isEmpty()
            && std::all_of(m_recent.cbegin(), m_recent.cend(), [&](quint32 id) { return all.contains(id); });
        const auto targets = [&](int most) {
            if (recentLive && m_rng.chance(40))
                return m_recent;
            QList<quint32> out;
            for (int k = m_rng.range(1, most); k > 0; --k) {
                const quint32 id = all[m_rng.range(0, int(all.size()) - 1)];
                if (!out.contains(id))
                    out.append(id);
            }
            m_recent = out;
            return out;
        };
        const auto one = [&](const QList<quint32>& from) { return from[m_rng.range(0, int(from.size()) - 1)]; };
        const QRect region = QRect(QPoint(0, 0), doc.canvas).adjusted(-40, -40, 40, 40);

        int choice = m_rng.range(0, 14);
        if (all.isEmpty() && choice >= 2 && choice <= 8)
            choice = 0;
        if ((choice == 6 && texts.isEmpty()) || (choice == 7 && numbered.isEmpty()))
            choice = 3;
        if (choice == 12 && depth > 0)
            choice = 0;
        switch (choice) {
        case 2: {
            QList<quint32> gone = targets(3);
            if (m_rng.chance(10))
                gone.append(doc.nextId + 1000); // not there: left alone
            return std::make_unique<RemoveItems>(gone);
        }
        case 3: {
            auto move = std::make_unique<MoveItems>(targets(3), QPoint(m_rng.range(-40, 40), m_rng.range(-40, 40)));
            move->merge = m_rng.chance(75);
            return move;
        }
        case 4: {
            const Item& item = *findItem(doc, targets(1).first());
            Item after = item;
            if (isBox(item.kind) || isLine(item.kind))
                after = reshaped(item, m_rng.range(0, isBox(item.kind) ? 7 : 1), centreIn(m_rng, region), m_rng.chance(30));
            else if (isStroke(item.kind))
                after = reshaped(item, int(LineHandle::End), centreIn(m_rng, region), m_rng.chance(20));
            else if (item.kind == Kind::Number)
                after.series = doc.series[m_rng.range(0, int(doc.series.size()) - 1)].id;
            else
                after = translated(item, QPoint(m_rng.range(-9, 9), m_rng.range(-9, 9)));
            auto reshape = std::make_unique<Reshape>(doc, after);
            reshape->merge = m_rng.chance(75);
            return reshape;
        }
        case 5: {
            const QList<quint32> ids = targets(2);
            QList<Style> styles;
            for (qsizetype k = 0; k < ids.size(); ++k)
                styles.append(randomStyle(m_rng));
            auto restyle = std::make_unique<Restyle>(doc, ids, styles);
            restyle->merge = m_rng.chance(75);
            return restyle;
        }
        case 6:
            return std::make_unique<Retext>(doc, one(texts),
                m_rng.chance(15) ? QString() : sampleTexts()[m_rng.range(0, int(sampleTexts().size()) - 1)]);
        case 7:
            return std::make_unique<Renumber>(doc, one(numbered),
                m_rng.chance(50) ? std::optional<int>(m_rng.range(-5, 120)) : std::nullopt);
        case 8:
            return std::make_unique<Reorder>(one(all), m_rng.range(-1, int(doc.items.size())));
        case 9:
            return SetSeries::newSeries(doc);
        case 10:
            return SetSeries::setStart(doc, doc.series[m_rng.range(0, int(doc.series.size()) - 1)].id, m_rng.range(-10, 99));
        case 11:
            return std::make_unique<SetCrop>(doc,
                QRect(pointIn(m_rng, QRect(QPoint(0, 0), doc.canvas)), QSize(m_rng.range(1, 200), m_rng.range(1, 200))));
        case 12: {
            // Each step is made against the document the steps before it
            // leave, as smart redaction and deleting a selection do.
            Document scratch = doc;
            std::vector<std::unique_ptr<Command>> steps;
            for (int k = m_rng.range(2, 3); k > 0; --k) {
                std::unique_ptr<Command> step = make(scratch, depth + 1);
                step->apply(scratch);
                steps.push_back(std::move(step));
            }
            return std::make_unique<Batch>(std::move(steps));
        }
        default: {
            QList<Item> items;
            quint32 id = doc.nextId;
            quint32 seq = doc.nextSeq;
            for (int k = m_rng.range(1, 3); k > 0; --k) {
                Item item = randomItem(m_rng, region, doc.series);
                item.id = id++;
                item.seq = seq++;
                items.append(item);
            }
            return std::make_unique<AddItems>(items, m_rng.chance(25) ? m_rng.range(0, int(doc.items.size())) : -1);
        }
        }
    }

private:
    Rng& m_rng;
    QList<quint32> m_recent;
};

} // namespace

class TestAnnotate : public QObject {
    Q_OBJECT

private slots:
    void pathModesRenderAcrossTileBoundaries()
    {
        const QImage base = plainImage(QSize(800, 600), kWhite);
        Document doc = emptyDoc(base.size());
        Item curve = lineItem(Kind::Arrow, {80.5, 120.5}, {700.5, 200.5});
        curve.style.lineMode = LineMode::Bezier;
        curve.points << QPointF(160.5, 500.5) << QPointF(600.5, 20.5);
        addItem(doc, curve);
        curve.kind = Kind::Line;
        addItem(doc, translated(curve, {0, 35}));
        Item polygon = lineItem(Kind::Line, {140.5, 250.5}, {400.5, 220.5});
        polygon.style.lineMode = LineMode::Polyline;
        polygon.points << QPointF(600.5, 480.5) << QPointF(100.5, 520.5);
        polygon.closed = true;
        addItem(doc, polygon);
        Item marker = boxItem(Kind::Marker, {50, 40, 620, 45});
        marker.style.markerMode = MarkerMode::Rectangle;
        marker.style.color = qRgb(255, 255, 0);
        addItem(doc, marker);
        Compositor wholeCompositor;
        const QImage whole = renderArea(wholeCompositor, doc, base, base.rect(), base.rect());
        for (int tile : {64, 127, 256}) {
            Compositor tiledCompositor;
            QCOMPARE(renderTiled(tiledCompositor, doc, base, base.rect(), base.rect(), {17, -23}, tile), whole);
        }
        const auto reopened = fromJson(toJson(doc), nullptr);
        QVERIFY(reopened);
        Compositor loadedCompositor;
        QCOMPARE(loadedCompositor.exportImage(*reopened, base, base.rect()), whole);
    }

    // --- geometry -----------------------------------------------------------

    void geometryBoxes()
    {
        // Pressing on pixel (30, 20) and dragging to (10, 5) covers both.
        QCOMPARE(pixelBox(QPoint(30, 20), QPoint(10, 5)), QRect(10, 5, 21, 16));
        QCOMPARE(pixelAt(QPointF(-0.5, 3.99)), QPoint(-1, 3));
        QCOMPARE(pixelCentre(QPointF(7.2, -0.1)), QPointF(7.5, -0.5));
        Item rect = boxItem(Kind::Rect, QRect(10, 5, 21, 16));
        QCOMPARE(rect.points, (QList<QPointF>{QPointF(10, 5), QPointF(31, 21)}));
        // Either order of the corners is the same box.
        std::swap(rect.points[0], rect.points[1]);
        QCOMPARE(itemBox(rect), QRect(10, 5, 21, 16));
        // Moved by whole pixels, edges stay edges and centres centres.
        const Item moved = translated(lineItem(Kind::Line, QPointF(1.5, 2.5), QPointF(9.5, 2.5)), QPoint(-4, 7));
        QCOMPARE(moved.points, (QList<QPointF>{QPointF(-2.5, 9.5), QPointF(5.5, 9.5)}));
        // A brush redaction's box holds the pixels of all its points and the
        // stroke round them (half the width, 3 px at 6).
        Item brush = redactItem(QRect(), RedactMode::Mosaic, 8, 1);
        brush.style.shape = RedactShape::Brush;
        brush.points = {QPointF(10.5, 10.5), QPointF(20.5, 4.5), QPointF(12.5, 30.5)};
        QCOMPARE(itemBox(brush), QRect(QPoint(7, 1), QPoint(23, 33)));

        // The redactions are the bottom segment, in the order they came.
        Document doc = emptyDoc(QSize(100, 100));
        const quint32 box = addItem(doc, boxItem(Kind::Rect, QRect(0, 0, 10, 10)));
        const quint32 r1 = addItem(doc, redactItem(QRect(5, 5, 10, 10), RedactMode::Mosaic, 8, 1));
        const quint32 text = addItem(doc, textItem(QPoint(1, 1), u"x"_s, 14));
        const quint32 r2 = addItem(doc, redactItem(QRect(50, 5, 10, 10), RedactMode::Solid, 8, 2));
        QCOMPARE(idsOf(doc), (QList<quint32>{r1, r2, box, text}));
        QCOMPARE(redactionEnd(doc), 2);
        QCOMPARE(insertionIndex(doc, Kind::Arrow, 0), 2);
        QCOMPARE(insertionIndex(doc, Kind::Redact, 4), 2);
        QCOMPARE(insertionIndex(doc, Kind::Redact, 0), 0);
        QCOMPARE(insertionIndex(doc, Kind::Number, -1), 4);
        QCOMPARE(redactionBoxes(doc), (QList<QRect>{QRect(5, 5, 10, 10), QRect(50, 5, 10, 10)}));
        QCOMPARE(indexOf(doc, text), 3);
        QCOMPARE(indexOf(doc, 99), -1);
        QVERIFY(!findItem(doc, 99));
        QCOMPARE(findItem(doc, r2)->style.redact, RedactMode::Solid);
        // The names in doc.json go both ways.
        for (int i = 0; i < kKindCount; ++i)
            QCOMPARE(kindFromName(kindName(Kind(i))).value_or(Kind::Rect), Kind(i));
        QVERIFY(!kindFromName(u"circle"));
        QCOMPARE(parseColor(u"#1E88E5").value_or(0), kBlue);
        QVERIFY(!parseColor(u"#1E88E"));
        QVERIFY(!parseColor(u"1E88E5A"));
        QVERIFY(!parseColor(u"#1E88G5"));
    }

    void geometryBounds()
    {
        // Boxes keep their strokes inside: the box is all they paint.
        const QRect box(10, 20, 100, 50);
        for (Kind kind : {Kind::Rect, Kind::Ellipse, Kind::Redact})
            QCOMPARE(bounds(boxItem(kind, box, kRed, 9), 1.0), QRectF(box));
        // Lines: the points, half the width for the round caps and 1 for antialiasing.
        QCOMPARE(bounds(lineItem(Kind::Line, QPointF(10.5, 50.5), QPointF(90.5, 50.5), kRed, 3), 1.0),
            QRectF(QPointF(8, 48), QPointF(93, 53)));
        QVERIFY(bounds(lineItem(Kind::Line, QPointF(10.5, 50.5), QPointF(10.5, 50.5), kRed, 3), 1.0).isEmpty());
        const Item pen = strokeItem(Kind::Pen, {QPointF(10.5, 10.5), QPointF(40.5, 30.5), QPointF(20.5, 60.5)}, kRed, 5);
        QCOMPARE(bounds(pen, 1.0), QRectF(QPointF(7, 7), QPointF(44, 64)));
        QCOMPARE(bounds(strokeItem(Kind::Marker, {QPointF(5.5, 5.5)}, kYellow, 20), 1.0), QRectF(QPointF(-5.5, -5.5), QPointF(16.5, 16.5)));

        // The arrow head: max(4 x width, 12 logical px), 0.9 of that wide.
        QCOMPARE(arrowHeadLength(6, 1.0), 24.0);
        QCOMPARE(arrowHeadLength(2, 1.5), 18.0);
        QCOMPARE(arrowHeadLength(2, 2.0), 24.0);
        QCOMPARE(arrowHeadLength(8, 2.0), 32.0);
        const Item arrow = lineItem(Kind::Arrow, QPointF(10.5, 100.5), QPointF(210.5, 100.5), kRed, 6);
        const ArrowShape shape = arrowShape(arrow, 1.5);
        const qreal head = arrowHeadLength(6, 1.5);
        // Even width, level: half a pixel up, onto the line between two pixel rows.
        QCOMPARE(shape.head.boundingRect(), QRectF(210.5 - head, 100 - 0.45 * head, head, 0.9 * head));
        // The shaft starts at the edge of its first pixel and ends inside the head's root, never at the tip.
        QCOMPARE(shape.shaft.boundingRect(), QRectF(10, 97, 210.5 - head / 2 - 10, 6));
        const QRectF reach = bounds(arrow, 1.5);
        QVERIFY(reach.contains(shape.head.boundingRect()));
        QVERIFY(reach.contains(shape.shaft.boundingRect()));
        // An arrow no longer than its head is all head; one of no length is nothing.
        QVERIFY(arrowShape(lineItem(Kind::Arrow, QPointF(10.5, 10.5), QPointF(20.5, 10.5)), 1.0).shaft.isEmpty());
        const Item dot = lineItem(Kind::Arrow, QPointF(10.5, 10.5), QPointF(10.5, 10.5));
        QVERIFY(arrowShape(dot, 1.0).head.isEmpty());
        QVERIFY(bounds(dot, 1.0).isEmpty());

        // Text: its coloured box, with the glyphs that may reach past it.
        Item text = textItem(QPoint(100, 100), u"Win顺\n标注"_s, 27, true, true);
        const TextGeometry g = textGeometry(text);
        QCOMPARE(g.content.topLeft(), QPoint(100, 100));
        QCOMPARE(g.box, g.content.adjusted(-8, -8, 8, 8)); // 0.3 x 27
        QCOMPARE(g.radius, 0.25 * 27);
        QVERIFY(bounds(text, 1.0).contains(QRectF(g.box)));
        QVERIFY(bounds(text, 1.0).contains(g.ink));
        text.text.clear();
        QVERIFY(bounds(text, 1.0).isEmpty());

        // A number: its disc, about 1.5 x the font size and odd, on pixel edges around its centre.
        const Item number = numberItem(QPointF(50.5, 50.5));
        const NumberGeometry n = numberGeometry(number, 7, 1.0);
        QCOMPARE(n.disc, QRectF(30, 30, 41, 41));
        QVERIFY(bounds(number, 1.0, 7).contains(n.disc));
        QVERIFY(bounds(number, 1.0, 100).width() > bounds(number, 1.0, 7).width());
    }

    void geometryBoundsCoverPaint_data()
    {
        QTest::addColumn<quint64>("seed");
        for (quint64 seed = 1; seed <= 4; ++seed)
            QTest::addRow("seed%llu", seed) << seed;
    }

    // Every pixel an item paints is inside its bounds: the canvas redraws
    // and the export crops by them.
    void geometryBoundsCoverPaint()
    {
        QFETCH(quint64, seed);
        Rng rng(seed * 65537);
        const QSize canvas(520, 380);
        const QImage base = plainImage(canvas, qRgb(0x60, 0x70, 0x80));
        const QRect sample(QPoint(0, 0), canvas);
        const Document doc = randomDoc(rng, canvas, 40);
        const QRect area = sample.adjusted(-80, -80, 80, 80);
        const QHash<quint32, int> shown = numbers(doc);
        Compositor compositor;
        Document empty = doc;
        empty.items.clear();
        const QImage plain = renderArea(compositor, empty, base, sample, area);
        for (const Item& item : doc.items) {
            Document one = doc;
            one.items = {item};
            if (item.kind == Kind::Number)
                one.items[0].pinned = shown.value(item.id, 1);
            const QImage painted = renderArea(compositor, one, base, sample, area);
            const QRect reach = bounds(one, one.items[0]).toAlignedRect().translated(-area.topLeft());
            if (const std::optional<QPoint> p = changedOutside(plain, painted, reach)) {
                QFAIL(qPrintable(u"%1 %2 paints (%3, %4), outside its bounds"_s.arg(kindName(item.kind))
                        .arg(item.id)
                        .arg(p->x() + area.left())
                        .arg(p->y() + area.top())));
            }
        }
    }

    void geometryHitTest()
    {
        Document doc = emptyDoc(QSize(400, 300));
        const quint32 redact = addItem(doc, redactItem(QRect(0, 0, 200, 150), RedactMode::Mosaic, 12, 1));
        const quint32 open = addItem(doc, boxItem(Kind::Rect, QRect(20, 20, 100, 80), kRed, 4, Fill::None));
        const quint32 filled = addItem(doc, boxItem(Kind::Ellipse, QRect(150, 100, 100, 60), kRed, 4, Fill::Solid));
        const quint32 line = addItem(doc, lineItem(Kind::Line, QPointF(10.5, 200.5), QPointF(390.5, 200.5), kRed, 6));
        const quint32 text = addItem(doc, textItem(QPoint(160, 110), u"top"_s, 18));
        const quint32 number = addItem(doc, numberItem(QPointF(300.5, 50.5)));
        const auto at = [&](QPointF p, qreal tolerance = 6) { return hitTest(doc, p, tolerance).value_or(0); };
        // The topmost: the text over the filled ellipse over the redaction.
        QCOMPARE(at(QPointF(165.2, 115.7)), text);
        QCOMPARE(at(QPointF(200.5, 150.5)), filled);
        // An open box is caught near its stroke; in its middle the redaction under it is.
        QCOMPARE(at(QPointF(21.5, 60.5)), open);
        QCOMPARE(at(QPointF(16.5, 60.5)), open); // 3.5 px out: within the tolerance
        QCOMPARE(at(QPointF(12.5, 60.5)), redact);
        QCOMPARE(at(QPointF(70.5, 60.5)), redact);
        // A line within half its width and the tolerance (even width: its centre is y = 200).
        QCOMPARE(at(QPointF(100.5, 208.5)), line);
        QCOMPARE(at(QPointF(100.5, 209.5)), 0u);
        QCOMPARE(at(QPointF(100.5, 191.5)), line);
        QCOMPARE(at(QPointF(100.5, 202.5), 0), line);
        QCOMPARE(at(QPointF(100.5, 203.5), 0), 0u);
        // A number by its disc.
        QCOMPARE(at(QPointF(318.5, 50.5)), number);
        QCOMPARE(at(QPointF(325.5, 50.5)), 0u);
        QCOMPARE(at(QPointF(390.5, 290.5)), 0u);
    }

    void geometryHandles()
    {
        const Item rect = boxItem(Kind::Rect, QRect(10, 10, 20, 10));
        QCOMPARE(handles(rect), (QList<QPointF>{{10, 10}, {20, 10}, {30, 10}, {30, 15}, {30, 20}, {20, 20}, {10, 20}, {10, 15}}));
        const auto drag = [&](BoxHandle handle, QPointF to, bool constrain) {
            return itemBox(reshaped(rect, int(handle), to, constrain));
        };
        // The corner goes to the pixel under the mouse; the other side keeps its pixels.
        QCOMPARE(drag(BoxHandle::BottomRight, QPointF(39.7, 29.2), false), QRect(10, 10, 30, 20));
        // Dragged past the other side, the box turns over.
        QCOMPARE(drag(BoxHandle::TopLeft, QPointF(45.2, 40.9), false), QRect(QPoint(29, 19), QPoint(45, 40)));
        // A side handle moves one side.
        QCOMPARE(drag(BoxHandle::Right, QPointF(50.5, 99), false), QRect(10, 10, 41, 10));
        QCOMPARE(drag(BoxHandle::Top, QPointF(0, 2.5), false), QRect(QPoint(10, 2), QPoint(29, 19)));
        // Shift on a corner: a square.
        QCOMPARE(drag(BoxHandle::BottomRight, QPointF(40.5, 25.5), true), QRect(10, 10, 31, 31));
        const QRect up = drag(BoxHandle::TopLeft, QPointF(25.5, 0.5), true);
        QCOMPARE(up, QRect(10, 0, 20, 20));
        QCOMPARE(reshaped(rect, 8, QPointF(1, 1), false), rect);

        // A line's end goes to the pixel's centre.
        const Item line = lineItem(Kind::Line, QPointF(10.5, 10.5), QPointF(50.5, 10.5));
        QCOMPARE(handles(line), line.points);
        QCOMPARE(reshaped(line, int(LineHandle::End), QPointF(100.2, 50.7), false).points[1], QPointF(100.5, 50.5));
        // Shift: steps of 15 degrees around the other end; level and 45 degrees exactly.
        QCOMPARE(reshaped(line, int(LineHandle::End), QPointF(100.5, 13.5), true).points[1], QPointF(100.5, 10.5));
        const QPointF diagonal = reshaped(line, int(LineHandle::End), QPointF(60.5, 62.5), true).points[1];
        QCOMPARE(diagonal, QPointF(61.5, 61.5));
        for (int i = 0; i < 48; ++i) {
            const qreal degrees = i * 7.5 + 1;
            const qreal radians = degrees * std::numbers::pi / 180;
            const QPointF to(10.5 + 80 * std::cos(radians), 10.5 + 80 * std::sin(radians));
            const QPointF p = reshaped(line, int(LineHandle::End), to, true).points[1];
            QCOMPARE(p.x() - std::floor(p.x()), 0.5);
            QCOMPARE(p.y() - std::floor(p.y()), 0.5);
            const qreal got = std::atan2(p.y() - 10.5, p.x() - 10.5) * 180 / std::numbers::pi;
            QVERIFY2(std::abs(got - std::round(got / 15) * 15) < 0.6, qPrintable(QString::number(got)));
        }
        QCOMPARE(reshaped(line, int(LineHandle::Start), QPointF(50.2, 90.9), true).points[0], QPointF(50.5, 90.5));

        // A pen stroke grows by the pixels it passes; Shift makes it one straight line.
        Item pen = strokeItem(Kind::Pen, {QPointF(10.5, 10.5)});
        pen = reshaped(pen, int(LineHandle::End), QPointF(12.2, 11.9), false);
        QCOMPARE(pen.points, (QList<QPointF>{QPointF(10.5, 10.5), QPointF(12.5, 11.5)}));
        QCOMPARE(reshaped(pen, int(LineHandle::End), QPointF(12.9, 11.1), false).points.size(), qsizetype(2));
        QCOMPARE(reshaped(pen, int(LineHandle::End), QPointF(80.5, 12.5), true).points,
            (QList<QPointF>{QPointF(10.5, 10.5), QPointF(80.5, 10.5)}));
        // Pens, texts and numbers have no handles.
        QVERIFY(handles(pen).isEmpty());
        const Item text = textItem(QPoint(5, 5), u"t"_s);
        QVERIFY(handles(text).isEmpty());
        QVERIFY(handles(numberItem(QPointF(5.5, 5.5))).isEmpty());
        QCOMPARE(reshaped(text, 0, QPointF(50, 50), false), text);
    }

    void geometryStrokes()
    {
        // Points within 0.75 px of the line between their neighbours go.
        QList<QPointF> straight;
        for (int i = 0; i <= 50; ++i)
            straight.append(QPointF(10.5 + i, 20.5));
        QCOMPARE(simplifyStroke(straight), (QList<QPointF>{straight.first(), straight.last()}));
        QCOMPARE(simplifyStroke({QPointF(0.5, 0.5), QPointF(10.5, 1.0), QPointF(20.5, 0.5)}).size(), qsizetype(2));
        const QList<QPointF> corner{QPointF(0.5, 0.5), QPointF(10.5, 1.5), QPointF(20.5, 0.5)};
        QCOMPARE(simplifyStroke(corner), corner);
        QList<QPointF> zigzag;
        for (int i = 0; i < 10; ++i)
            zigzag.append(QPointF(i * 10 + 0.5, (i % 2) * 10 + 0.5));
        QCOMPARE(simplifyStroke(zigzag), zigzag);
        QCOMPARE(simplifyStroke({QPointF(1.5, 1.5)}).size(), qsizetype(1));

        // The centre line runs through the first and last points and the
        // midpoints between, its corners rounded by quadratic curves.
        const QList<QPointF> points{QPointF(0.5, 0.5), QPointF(20.5, 0.5), QPointF(20.5, 20.5), QPointF(40.5, 20.5)};
        const QPainterPath path = strokeCentreline(points);
        QCOMPARE(path.elementCount(), 8);
        QCOMPARE(QPointF(path.elementAt(0)), points.first());
        QCOMPARE(QPointF(path.elementAt(3)), QPointF(20.5, 10.5));
        QCOMPARE(QPointF(path.elementAt(6)), QPointF(30.5, 20.5));
        QCOMPARE(path.currentPosition(), points.last());
        QCOMPARE(strokeCentreline({points[0], points[1]}).elementCount(), 2);
    }

    void geometryAlignment()
    {
        const auto aligned = [](QPointF a, QPointF b, int width) { return alignedPoints(lineItem(Kind::Line, a, b, kRed, width)); };
        // Level or upright with an even width: half a pixel up or left, so
        // that both long edges are pixel edges.
        QCOMPARE(aligned(QPointF(10.5, 20.5), QPointF(90.5, 20.5), 4), (QList<QPointF>{QPointF(10.5, 20), QPointF(90.5, 20)}));
        QCOMPARE(aligned(QPointF(10.5, 20.5), QPointF(10.5, 90.5), 4), (QList<QPointF>{QPointF(10, 20.5), QPointF(10, 90.5)}));
        // Odd widths are there already; slanted lines and dots are left.
        QCOMPARE(aligned(QPointF(10.5, 20.5), QPointF(90.5, 20.5), 3), (QList<QPointF>{QPointF(10.5, 20.5), QPointF(90.5, 20.5)}));
        QCOMPARE(aligned(QPointF(10.5, 20.5), QPointF(90.5, 25.5), 4), (QList<QPointF>{QPointF(10.5, 20.5), QPointF(90.5, 25.5)}));
        QCOMPARE(alignedPoints(strokeItem(Kind::Pen, {QPointF(5.5, 5.5), QPointF(5.5, 5.5)}, kRed, 4)),
            (QList<QPointF>{QPointF(5.5, 5.5), QPointF(5.5, 5.5)}));
        // A level pen stroke too.
        QCOMPARE(alignedPoints(strokeItem(Kind::Pen, {QPointF(5.5, 5.5), QPointF(9.5, 5.5), QPointF(30.5, 5.5)}, kRed, 2)),
            (QList<QPointF>{QPointF(5.5, 5), QPointF(9.5, 5), QPointF(30.5, 5)}));
    }

    // --- crisp ------------------------------------------------------------------

    void crispBoxes_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("fill");
        for (int w : {1, 2, 3, 4, 6, 7}) {
            for (Fill f : {Fill::None, Fill::Soft})
                QTest::addRow("w%d-%s", w, qPrintable(fillName(f))) << w << int(f);
        }
    }

    // On white, a box of width w has exactly w pixels of its colour on each
    // side and nothing half covered: every pixel is white, the colour or
    // the soft fill.
    void crispBoxes()
    {
        QFETCH(int, width);
        QFETCH(int, fill);
        const QImage base = plainImage(QSize(240, 200), kWhite);
        Document doc = emptyDoc(base.size(), 1.5);
        const QRect box(40, 30, 150, 120);
        addItem(doc, boxItem(Kind::Rect, box, kRed, width, Fill(fill)));
        const QImage out = Compositor().exportImage(doc, base, base.rect());
        const QRect inner = box.adjusted(width, width, -width, -width);
        const QRgb inside = Fill(fill) == Fill::Soft ? out.pixel(box.center()) : kWhite;
        if (Fill(fill) == Fill::Soft)
            QVERIFY(inside != kWhite && inside != kRed);
        int wrong = 0;
        QPoint first;
        for (int y = 0; y < out.height(); ++y) {
            for (int x = 0; x < out.width(); ++x) {
                const QRgb want = !box.contains(x, y) ? kWhite : inner.contains(x, y) ? inside : kRed;
                if (out.pixel(x, y) != want && wrong++ == 0)
                    first = QPoint(x, y);
            }
        }
        QVERIFY2(wrong == 0, qPrintable(u"%1 pixels, the first at (%2, %3)"_s.arg(wrong).arg(first.x()).arg(first.y())));
        // The runs, as the design asks for them: from outside in, on each side.
        const auto run = [&](QPoint from, QPoint step) {
            int n = 0;
            for (QPoint p = from; out.pixel(p) == kRed; p += step)
                ++n;
            return n;
        };
        const QPoint middle = box.center();
        QCOMPARE(run(QPoint(box.left(), middle.y()), QPoint(1, 0)), width);
        QCOMPARE(run(QPoint(box.right(), middle.y()), QPoint(-1, 0)), width);
        QCOMPARE(run(QPoint(middle.x(), box.top()), QPoint(0, 1)), width);
        QCOMPARE(run(QPoint(middle.x(), box.bottom()), QPoint(0, -1)), width);
    }

    void crispLines_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("upright");
        for (Kind kind : {Kind::Line, Kind::Arrow, Kind::Pen}) {
            for (int w = 1; w <= 8; ++w) {
                for (bool upright : {false, true})
                    QTest::addRow("%s-w%d-%s", qPrintable(kindName(kind)), w, upright ? "upright" : "level") << int(kind) << w << upright;
            }
        }
    }

    // Across a level or upright line (an arrow's shaft, a pen stroke):
    // exactly w pixels of its colour, white on both sides. Odd widths are
    // centred on the points' pixels, even ones on the edge above or left of
    // them (3.4, rule 5).
    void crispLines()
    {
        QFETCH(int, kind);
        QFETCH(int, width);
        QFETCH(bool, upright);
        const QImage base = plainImage(QSize(300, 300), kWhite);
        Document doc = emptyDoc(base.size(), 1.0);
        const QPointF a = upright ? QPointF(150.5, 20.5) : QPointF(20.5, 150.5);
        const QPointF b = upright ? QPointF(150.5, 280.5) : QPointF(280.5, 150.5);
        if (Kind(kind) == Kind::Pen)
            addItem(doc, strokeItem(Kind::Pen, {a, (a + b) / 2, b}, kBlue, width));
        else
            addItem(doc, lineItem(Kind(kind), a, b, kBlue, width));
        const QImage out = Compositor().exportImage(doc, base, base.rect());
        QList<int> covered;
        for (int t = 0; t < 300; ++t) {
            const QRgb p = upright ? out.pixel(t, 100) : out.pixel(100, t);
            if (p == kWhite)
                continue;
            QVERIFY2(p == kBlue, qPrintable(u"half covered at %1: %2"_s.arg(t).arg(hex(p))));
            covered.append(t);
        }
        QCOMPARE(int(covered.size()), width);
        QCOMPARE(covered.last() - covered.first() + 1, width);
        QCOMPARE(covered.first(), 150 - width / 2);
    }

    void crispText()
    {
        const QFont font = annotationFont(27, false);
        QCOMPARE(font.pixelSize(), 27);
        QCOMPARE(font.family(), u"Microsoft YaHei UI"_s);
        QVERIFY(font.styleStrategy() & QFont::NoSubpixelAntialias);
        QCOMPARE(annotationFont(0, true).pixelSize(), 1);
        QCOMPARE(annotationFont(14, true).weight(), QFont::Bold);
        // Grey antialiasing only: black words on white have no coloured fringes.
        const QImage base = plainImage(QSize(520, 260), kWhite);
        Document doc = emptyDoc(base.size(), 1.5);
        addItem(doc, textItem(QPoint(10, 10), u"Win顺 截图 Annotate 123"_s, 14, false, false, 0xff000000u));
        addItem(doc, textItem(QPoint(10, 40), u"Win顺 截图 Annotate"_s, 27, true, false, 0xff000000u));
        addItem(doc, textItem(QPoint(10, 90), u"顺 Wg"_s, 80, false, false, 0xff000000u));
        const QImage out = Compositor().exportImage(doc, base, base.rect());
        int inked = 0;
        int coloured = 0;
        for (int y = 0; y < out.height(); ++y) {
            for (int x = 0; x < out.width(); ++x) {
                const QRgb p = out.pixel(x, y);
                if (p != kWhite)
                    ++inked;
                if (qRed(p) != qGreen(p) || qGreen(p) != qBlue(p))
                    ++coloured;
            }
        }
        QVERIFY(inked > 1000);
        QCOMPARE(coloured, 0);
        // Whole-pixel baselines, glyphs on the 1/64 grid.
        const TextLayout layout = layoutText(u"Ab 你好\n第二行 xy\n😀 é"_s, 27, false);
        QVERIFY(!layout.runs.isEmpty());
        for (const QGlyphRun& run : layout.runs) {
            for (const QPointF& p : run.positions()) {
                QCOMPARE(p.y(), std::floor(p.y()));
                QCOMPARE(p.x() * 64, std::floor(p.x() * 64));
            }
        }
        // Lines a pitch apart; the remembered metrics are the layout's.
        const TextMetrics one = textMetrics(u"Ab"_s, 27, false);
        const TextMetrics three = textMetrics(u"Ab\nAb\nAb"_s, 27, false);
        QCOMPARE(three.size.height(), one.size.height() + 2 * one.pitch);
        QCOMPARE(three.size.width(), one.size.width());
        QVERIFY(one.pitch >= one.size.height() - 1);
        QVERIFY(textMetrics(u"x"_s, 27, false) == layoutText(u"x"_s, 27, false).metrics);
        clearTextMetrics();
        QVERIFY(textMetrics(u"x"_s, 27, false) == layoutText(u"x"_s, 27, false).metrics);
    }

    // --- text outline (#101) --------------------------------------------------

    static Item outlinedText(const QString& text, int stroke, QRgb color = kRed, QRgb strokeColor = 0, int pixelSize = 27)
    {
        Item item = textItem(QPoint(20, 20), text, pixelSize, false, false, color);
        item.style.textStroke = stroke;
        item.style.textStrokeColor = strokeColor;
        return item;
    }

    // The stroke's colour: the one chosen, else opposite the text's.
    void textOutlineColour()
    {
        QCOMPARE(textStrokeColorOf(outlinedText(u"a"_s, 2, kRed)), kWhite);
        QCOMPARE(textStrokeColorOf(outlinedText(u"a"_s, 2, kYellow)), kDark);
        QCOMPARE(textStrokeColorOf(outlinedText(u"a"_s, 2, kDark)), kWhite);
        QCOMPARE(textStrokeColorOf(outlinedText(u"a"_s, 2, kWhite)), kDark);
        QCOMPARE(textStrokeColorOf(outlinedText(u"a"_s, 2, kRed, kBlue)), kBlue);
        QCOMPARE(textStrokeWidth(outlinedText(u"a"_s, 2)), 2);
        QCOMPARE(textStrokeWidth(outlinedText(u"a"_s, 999)), kMaxTextStroke);
        Item boxed = outlinedText(u"a"_s, 2);
        boxed.style.boxed = true;
        QCOMPARE(textStrokeWidth(boxed), 0); // white words on a box: no outline
        Item number = outlinedText(u"a"_s, 2);
        number.kind = Kind::Number;
        QCOMPARE(textStrokeWidth(number), 0);
    }

    // The outline is painted under the words: a text with stroke 0 is the text
    // as before; with a stroke, every pixel that is full text colour is the
    // same, the ring stands round the glyphs, and nothing is painted outside the bounds.
    void textOutlineRenders_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::addColumn<int>("logical");
        QTest::addColumn<QRgb>("color");
        for (qreal dpr : {1.0, 1.5}) {
            for (int logical : {1, 2, 3})
                QTest::addRow("%d%% %dpx red", int(dpr * 100), logical) << dpr << logical << kRed;
        }
        QTest::addRow("100%% 2px dark") << 1.0 << 2 << kDark;
        QTest::addRow("150%% 3px yellow") << 1.5 << 3 << kYellow;
    }

    void textOutlineRenders()
    {
        QFETCH(qreal, dpr);
        QFETCH(int, logical);
        QFETCH(QRgb, color);
        const int stroke = toImagePx(logical, dpr);
        const int fontPx = toImagePx(24, dpr);
        const QRgb page = qRgb(0x60, 0x70, 0x80);
        const QImage base = plainImage(QSize(420, 160), page);
        const QRect all(QPoint(0, 0), base.size());
        Compositor compositor;
        const Item plain = outlinedText(u"Win顺 Hello\n截图 gWy"_s, 0, color, 0, fontPx);
        Document docPlain = emptyDoc(base.size(), dpr);
        addItem(docPlain, plain);
        const QImage a = renderArea(compositor, docPlain, base, all, all);
        // A colour with no width draws nothing: the same bytes.
        Item zero = plain;
        zero.style.textStrokeColor = kBlue;
        Document docZero = emptyDoc(base.size(), dpr);
        addItem(docZero, zero);
        QVERIFY2(compare(renderArea(compositor, docZero, base, all, all), a).count == 0, "stroke 0 changed the text");

        Item ringed = plain;
        ringed.style.textStroke = stroke;
        Document docRing = emptyDoc(base.size(), dpr);
        addItem(docRing, ringed);
        const QImage b = renderArea(compositor, docRing, base, all, all);
        const QRgb ringColour = textStrokeColorOf(ringed) | 0xff000000u;
        const QRgb ink = color | 0xff000000u;
        // 1. Full text pixels stay the text's.
        const auto away = [](QRgb p, QRgb q) { return std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q)); };
        QList<QPoint> touched; // the pixels the plain text covers by half or more
        QList<QPoint> faint; // every pixel it changes
        int full = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                if (a.pixel(x, y) == page)
                    continue;
                faint.append(QPoint(x, y));
                if (away(a.pixel(x, y), page) * 2 >= away(ink, page))
                    touched.append(QPoint(x, y));
                if (a.pixel(x, y) == ink) {
                    ++full;
                    QCOMPARE(b.pixel(x, y), ink);
                }
            }
        }
        QVERIFY(full > 200);
        // 2. The ring: page pixels at least 1.5 px inside the stroke's reach of a text pixel are (nearly) the
        //    ring's colour, none further out than stroke + 1.5 is touched, and there is a ring at all.
        int inner = 0;
        int missing = 0;
        int farEdge = 0;
        int ring = 0;
        QPoint firstMissing;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                if (a.pixel(x, y) != page)
                    continue;
                qreal nearest = 1e9;
                qreal nearestAny = 1e9;
                for (const QPoint& s : std::as_const(touched)) {
                    if (std::abs(s.x() - x) <= stroke + 3 && std::abs(s.y() - y) <= stroke + 3)
                        nearest = std::min(nearest, std::hypot(s.x() - x, s.y() - y));
                }
                for (const QPoint& s : std::as_const(faint)) {
                    if (std::abs(s.x() - x) <= stroke + 3 && std::abs(s.y() - y) <= stroke + 3)
                        nearestAny = std::min(nearestAny, std::hypot(s.x() - x, s.y() - y));
                }
                const QRgb got = b.pixel(x, y);
                ring += got != page;
                if (nearest <= stroke - 1.0) {
                    ++inner;
                    if (away(got, ringColour) > 60 && missing++ == 0)
                        firstMissing = QPoint(x, y);
                } else if (nearestAny > stroke + 1.5 && away(got, page) > 6) {
                    ++farEdge;
                }
            }
        }
        QVERIFY(ring > 300);
        if (stroke >= 3)
            QVERIFY(inner > 100);
        QVERIFY2(missing == 0, qPrintable(u"ring hole at (%1, %2), %3 of %4"_s.arg(firstMissing.x()).arg(firstMissing.y()).arg(missing).arg(inner)));
        QCOMPARE(farEdge, 0);
        // 3. Nothing outside the item's bounds, which only grew.
        const QRectF grown = bounds(docRing, docRing.items[0]);
        const QRectF was = bounds(docPlain, docPlain.items[0]);
        QVERIFY2(!changedOutside(a, b, grown.toAlignedRect()), "the outline paints outside the bounds");
        QVERIFY(grown.contains(was));
        QVERIFY(grown.width() >= was.width() + stroke);
        // 4. The export is the same pixels.
        const QImage exported = compositor.exportImage(docRing, base, all);
        QVERIFY(compare(exported.convertToFormat(QImage::Format_RGB32), b).count == 0);
        // 5. Tiles equal the whole.
        QImage tiled(base.size(), QImage::Format_RGB32);
        for (const QRect& tile : {QRect(0, 0, 200, 160), QRect(200, 0, 220, 160)}) {
            QImage piece;
            compositor.render(docRing, base, tile, tile, &piece);
            blit(tiled, tile.topLeft(), piece);
        }
        QVERIFY(compare(tiled, b).count == 0);
        const QString shots = qEnvironmentVariable("WS_TEST_SHOTS");
        if (!shots.isEmpty()) {
            const QString tag = u"%1-%2px-%3"_s.arg(int(dpr * 100)).arg(logical).arg(color, 0, 16);
            QVERIFY(a.save(u"%1/text-plain-%2.png"_s.arg(shots, tag)));
            QVERIFY(b.save(u"%1/text-outlined-%2.png"_s.arg(shots, tag)));
        }
    }

    // The outline layer of big text is built once and cached; it must not take long on the GUI thread
    // (64 px words, 3 lines, the widest outline at 300 %).
    void textOutlineSpeed()
    {
        const int stroke = toImagePx(3, 3.0);
        const QImage base = plainImage(QSize(900, 420), qRgb(0x60, 0x70, 0x80));
        const QRect all(QPoint(0, 0), base.size());
        Item item = outlinedText(u"Win顺 截图 Annotate\n第二行 描边 gWy\nThird line 123"_s, stroke, kRed, 0, 64);
        Document doc = emptyDoc(base.size(), 3.0);
        addItem(doc, item);
        Compositor compositor;
        QElapsedTimer timer;
        timer.start();
        const QImage out = renderArea(compositor, doc, base, all, all);
        const qint64 first = timer.elapsed();
        QVERIFY(!out.isNull());
        qInfo().noquote() << u"outline layer, 64 px x 3 lines, stroke %1 px: first render %2 ms"_s.arg(stroke).arg(first);
        const QString shots = qEnvironmentVariable("WS_TEST_SHOTS");
        if (!shots.isEmpty())
            QVERIFY(out.save(u"%1/speed-stroke%2.png"_s.arg(shots).arg(stroke)));
        item.style.textStroke = kMaxTextStroke;
        Document wide = emptyDoc(base.size(), 3.0);
        addItem(wide, item);
        timer.restart();
        renderArea(compositor, wide, base, all, all);
        qInfo().noquote() << u"the same at the widest allowed outline (%1 px): %2 ms"_s.arg(kMaxTextStroke).arg(timer.elapsed());
    }

    // Boxed words keep their box and take no outline.
    void textOutlineBoxedIsPlain()
    {
        const QImage base = plainImage(QSize(260, 100), qRgb(0x60, 0x70, 0x80));
        const QRect all(QPoint(0, 0), base.size());
        Item boxed = outlinedText(u"Boxed 文字"_s, 0);
        boxed.style.boxed = true;
        Document one = emptyDoc(base.size());
        addItem(one, boxed);
        boxed.style.textStroke = 3;
        Document two = emptyDoc(base.size());
        addItem(two, boxed);
        Compositor compositor;
        QVERIFY(compare(renderArea(compositor, one, base, all, all), renderArea(compositor, two, base, all, all)).count == 0);
        QCOMPARE(bounds(one, one.items[0]), bounds(two, two.items[0]));
    }

    // The outline is saved with the text; a file made before it has none.
    void textOutlineFormat()
    {
        Document doc = emptyDoc(QSize(300, 200));
        addItem(doc, outlinedText(u"one"_s, 3, kRed, kBlue));
        addItem(doc, outlinedText(u"two"_s, 2));
        addItem(doc, outlinedText(u"three"_s, 0));
        const QJsonObject json = toJson(doc);
        const QJsonArray items = json.value(u"items"_s).toArray();
        QCOMPARE(items.size(), 3);
        const auto styleOf = [&](qsizetype i) { return items[i].toObject().value(u"style"_s).toObject(); };
        QCOMPARE(styleOf(0).value(u"textStroke"_s).toInt(), 3);
        QCOMPARE(styleOf(0).value(u"textStrokeColor"_s).toString(), u"#1E88E5"_s);
        QVERIFY(!styleOf(1).contains(u"textStrokeColor"_s)); // automatic: not written
        QVERIFY(!styleOf(2).contains(u"textStroke"_s)); // none: as before
        // A colour picked and then "no outline": the colour is kept and written, as any field off its default.
        Item picked = outlinedText(u"four"_s, 0, kRed, kBlue);
        Document pickedDoc = emptyDoc(QSize(300, 200));
        addItem(pickedDoc, picked);
        const std::optional<Document> pickedBack = fromJson(toJson(pickedDoc), nullptr);
        QVERIFY(pickedBack);
        QCOMPARE(*pickedBack, pickedDoc);
        QString error;
        const std::optional<Document> back = fromJson(json, &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(*back, doc);
        QCOMPARE(back->items[0].style.textStroke, 3);
        QCOMPARE(back->items[0].style.textStrokeColor, kBlue);
        QCOMPARE(back->items[1].style.textStrokeColor, QRgb(0));
        // A text without the fields (an older file) loads with no outline; odd values are tamed.
        const auto withStyle = [&](const std::function<void(QJsonObject&)>& edit) {
            QJsonObject out = json;
            QJsonArray list = out.value(u"items"_s).toArray();
            for (qsizetype i = 0; i < list.size(); ++i) {
                QJsonObject item = list[i].toObject();
                QJsonObject style = item.value(u"style"_s).toObject();
                edit(style);
                item[u"style"_s] = style;
                list[i] = item;
            }
            out[u"items"_s] = list;
            return out;
        };
        const std::optional<Document> older = fromJson(withStyle([](QJsonObject& style) {
            style.remove(u"textStroke"_s);
            style.remove(u"textStrokeColor"_s);
        }), &error);
        QVERIFY2(older, qPrintable(error));
        for (const Item& item : older->items) {
            QCOMPARE(item.style.textStroke, 0);
            QCOMPARE(item.style.textStrokeColor, QRgb(0));
        }
        const std::optional<Document> tamed = fromJson(withStyle([](QJsonObject& style) {
            style[u"textStroke"_s] = 100000;
            style[u"textStrokeColor"_s] = u"blue"_s;
        }), &error);
        QVERIFY2(tamed, qPrintable(error));
        QCOMPARE(tamed->items[0].style.textStroke, 0);
        QCOMPARE(tamed->items[0].style.textStrokeColor, QRgb(0));
        // The v1 sample from before the outline reads as it did.
        QFile file(QStringLiteral(WINSHUN_ANNOTATE_DATA) + u"/doc-v1.json"_s);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const std::optional<Document> sample = fromJson(QJsonDocument::fromJson(file.readAll()).object(), nullptr);
        QVERIFY(sample);
        for (const Item& item : sample->items)
            QCOMPARE(item.style.textStroke, 0);
    }

    // What the text tool remembers: width and colour, at any scale.
    void textOutlineStyles()
    {
        const StyleMemory defaults;
        QCOMPARE(defaults.forTool(Kind::Text, 1.5).textStroke, 0);
        for (qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
            StyleMemory memory;
            Style s = memory.forTool(Kind::Text, dpr);
            s.textStroke = toImagePx(2, dpr);
            s.textStrokeColor = kBlue;
            memory.remember(Kind::Text, s, dpr);
            QCOMPARE(memory.tool(Kind::Text).stroke, 2);
            const Style again = memory.forTool(Kind::Text, dpr);
            QCOMPARE(again.textStroke, toImagePx(2, dpr));
            QCOMPARE(again.textStrokeColor, kBlue);
            QVERIFY(StyleMemory::fromJson(memory.toJson()) == memory);
            s.textStroke = 0;
            memory.remember(Kind::Text, s, dpr);
            QCOMPARE(memory.forTool(Kind::Text, dpr).textStroke, 0);
        }
        StyleMemory memory;
        Style s = memory.forTool(Kind::Text, 1.0);
        s.textStroke = 3;
        memory.remember(Kind::Text, s, 1.0);
        QVERIFY(memory.toJson().contains(u"\"stroke\":3"_s));
        QVERIFY(!memory.toJson().contains(u"strokeColor"_s));
        QCOMPARE(memory.forTool(Kind::Number, 1.0).textStroke, 0); // another tool never takes it
        QCOMPARE(StyleMemory::fromJson(u"{\"version\":1,\"text\":{\"stroke\":9999}}"_s).tool(Kind::Text).stroke, 0);
        QCOMPARE(StyleMemory::fromJson(u"{\"version\":1,\"text\":{\"stroke\":2,\"strokeColor\":\"#00FF00\"}}"_s).tool(Kind::Text).strokeColor, QRgb(0xff00ff00));
        QCOMPARE(strokePresets(), (QList<int>{1, 2, 3}));
    }

    // Changing the outline of a text is one undoable step; undone, the text is as it was.
    void textOutlineUndo()
    {
        Document doc = emptyDoc(QSize(300, 120), 1.5);
        const quint32 id = addItem(doc, outlinedText(u"undo me"_s, 0));
        const Document before = doc;
        const QRectF plainBounds = bounds(doc, *findItem(doc, id));
        Style style = findItem(doc, id)->style;
        style.textStroke = 3;
        style.textStrokeColor = kBlue;
        Restyle command(doc, QList<quint32>{id}, QList<Style>{style});
        Change change = command.apply(doc);
        QCOMPARE(findItem(doc, id)->style.textStroke, 3);
        QVERIFY(change.dirty.contains(bounds(doc, *findItem(doc, id)))); // the grown outline is redrawn
        QVERIFY(bounds(doc, *findItem(doc, id)).width() > plainBounds.width());
        // The ring is part of the text for a click.
        const TextGeometry g = textGeometry(*findItem(doc, id));
        const QPoint justOutside(g.content.left() - 2, g.content.center().y());
        QVERIFY(hitTest(doc, justOutside, 0).has_value());
        QVERIFY(!hitTest(doc, QPoint(g.content.left() - 4, g.content.center().y()), 0).has_value());
        change = command.revert(doc);
        QVERIFY(!hitTest(doc, justOutside, 0).has_value());
        QCOMPARE(doc, before);
        QVERIFY(change.dirty.contains(bounds(doc, *findItem(doc, id))));
        command.apply(doc);
        QCOMPARE(findItem(doc, id)->style.textStrokeColor, kBlue);
    }

    // --- tiles == whole ---------------------------------------------------------

    // Qt's FreeType engine keeps the last sub-pixel offset of its glyph cache
    // on the font face, and large text is filled from outlines loaded with
    // it. A recipe built after small text was drawn at x.25, x.5 or x.75 must
    // still draw the same pixels.
    // A picture without objects comes out of the compositor, tile by tile, as the base itself
    // (the live test of 4b found one black pixel at a tile's inner corner on a 1080 x 30000 one).
    void tilesOfAPlainPicture()
    {
        QImage base(1080, 6000, QImage::Format_RGB32);
        for (int y = 0; y < base.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(base.scanLine(y));
            for (int x = 0; x < base.width(); ++x)
                line[x] = qRgb(y & 255, ((y >> 8) & 255) * 2 + (x % 7), (x * 5 + y * 3) & 255);
        }
        Document doc;
        doc.canvas = base.size();
        doc.crop = QRect(QPoint(0, 0), base.size());
        Compositor compositor;
        int differing = 0;
        QPoint first(-1, -1);
        for (int ty = 0; ty < base.height(); ty += 256) {
            for (int tx = 0; tx < base.width(); tx += 256) {
                const QRect area = QRect(tx, ty, 256, 256) & QRect(QPoint(0, 0), base.size());
                QImage tile;
                compositor.render(doc, base, QRect(QPoint(0, 0), base.size()), area, &tile);
                for (int y = 0; y < tile.height(); ++y) {
                    // The raw words, the alpha byte too: a texture is made from the memory, not from pixel().
                    const auto* got = reinterpret_cast<const quint32*>(tile.constScanLine(y));
                    const auto* want = reinterpret_cast<const quint32*>(base.constScanLine(ty + y));
                    for (int x = 0; x < tile.width(); ++x) {
                        if (got[x] != want[tx + x]) {
                            if (!differing)
                                first = QPoint(tx + x, ty + y);
                            ++differing;
                        }
                    }
                }
            }
        }
        QVERIFY2(differing == 0, qPrintable(u"%1 pixels differ, the first at %2,%3"_s.arg(differing).arg(first.x()).arg(first.y())));
    }

    void tilesTextHistory()
    {
        Document doc = emptyDoc(QSize(640, 360));
        addItem(doc, numberItem(QPointF(80.5, 80.5), 0, kRed, 65));
        addItem(doc, textItem(QPoint(160, 30), u"Win顺 16"_s, 72));
        addItem(doc, textItem(QPoint(160, 150), u"你好 Ab"_s, 80, true, true, kYellow));
        addItem(doc, textItem(QPoint(20, 260), u"小字 small"_s, 27, true));
        const QImage base = noiseImage(doc.canvas, 11);
        const QRect sample(QPoint(0, 0), doc.canvas);
        Compositor first;
        const QImage want = first.exportImage(doc, base, sample);
        QImage scratch(200, 100, QImage::Format_ARGB32_Premultiplied);
        int px = 20;
        for (const qreal offset : {0.25, 0.5, 0.75}) {
            scratch.fill(Qt::transparent);
            {
                QPainter painter(&scratch);
                painter.setPen(QColor(Qt::white));
                for (const bool bold : {false, true}) {
                    // A size not drawn before, so its glyph is in no cache
                    // yet: drawing it leaves its offset on the face.
                    const TextLayout small = layoutText(u"W"_s, px++, bold);
                    for (const QGlyphRun& run : small.runs)
                        painter.drawGlyphRun(QPointF(10 + offset, 50), run);
                }
            }
            Compositor fresh;
            const Diff d = compare(fresh.exportImage(doc, base, sample), want);
            QVERIFY2(d.count == 0, qPrintable(u"after small text at +"_s + QString::number(offset) + u": "_s + describe(d)
                + firstAt(d, doc, QPoint())));
            first.clear();
            const Diff again = compare(first.exportImage(doc, base, sample), want);
            QVERIFY2(again.count == 0, qPrintable(u"cleared: "_s + describe(again)));
        }
    }

    void tilesRandom_data()
    {
        QTest::addColumn<quint64>("seed");
        for (quint64 seed = 1; seed <= 6; ++seed)
            QTest::addRow("seed%llu", seed) << seed;
    }

    // Random documents (all kinds, text to 80 px with Chinese and emoji,
    // numbers, redactions, objects over the canvas' edges) drawn in 256 px
    // tiles on grids that line up with nothing, and in random areas from
    // warm and cold caches: the same pixels as drawn whole.
    void tilesRandom()
    {
        QFETCH(quint64, seed);
        Rng rng(seed * 7919);
        const QSize canvas(rng.range(600, 1100), rng.range(400, 800));
        const QImage base = noiseImage(canvas, seed);
        const QRect sample(QPoint(0, 0), canvas);
        const Document doc = randomDoc(rng, canvas, 36);
        // The whole picture, with the dark around it where objects reach out.
        const QRect area = sample.adjusted(-64, -64, 64, 64);
        Compositor first;
        const QImage whole = renderArea(first, doc, base, sample, area);
        for (const QPoint origin : {QPoint(-37, -53), QPoint(0, 0), QPoint(rng.range(0, 255), rng.range(0, 255))}) {
            Compositor tiles;
            const Diff d = compare(renderTiled(tiles, doc, base, sample, area, origin, 256), whole);
            if (d.count != 0) {
                QFAIL(qPrintable(u"tiles through (%1, %2): %3%4; %5"_s.arg(origin.x())
                        .arg(origin.y())
                        .arg(describe(d), firstAt(d, doc, area.topLeft()), culprits(doc, base, sample, area, origin, 256))));
            }
        }
        Compositor warm;
        for (int i = 0; i < 40; ++i) {
            const QRect r(rng.range(area.left(), area.right()), rng.range(area.top(), area.bottom()), rng.range(1, 320), rng.range(1, 320));
            const QRect part = r & area;
            Compositor cold;
            const QImage got = renderArea(i % 2 ? warm : cold, doc, base, sample, part);
            const Diff d = compare(got, whole.copy(part.translated(-area.topLeft())));
            QVERIFY2(d.count == 0,
                qPrintable(u"area (%1, %2) %3 x %4: %5"_s.arg(part.x()).arg(part.y()).arg(part.width()).arg(part.height()).arg(describe(d))));
        }
        // The export is the same pixels.
        const Diff d = compare(first.exportImage(doc, base, sample), whole.copy(sample.translated(-area.topLeft())));
        QVERIFY2(d.count == 0, qPrintable(describe(d)));
    }

    // A large canvas with objects far larger than a tile: Qt cuts paths that
    // reach 1024 px past what it draws on, and those cuts must not show.
    void tilesLarge()
    {
        const QSize canvas(3000, 2000);
        const QImage base = noiseImage(canvas, 77);
        const QRect sample(QPoint(0, 0), canvas);
        Document doc = emptyDoc(canvas, 1.25);
        addItem(doc, redactItem(QRect(100, 1500, 1300, 300), RedactMode::Mosaic, 24, 11));
        addItem(doc, redactItem(QRect(1700, 100, 900, 700), RedactMode::Blur, 20, 12));
        addItem(doc, redactItem(QRect(2500, 1400, 400, 500), RedactMode::Solid, 20, 13));
        addItem(doc, boxItem(Kind::Rect, QRect(40, 30, 2900, 1900), kRed, 7, Fill::Soft));
        addItem(doc, boxItem(Kind::Rect, QRect(256, 512, 768, 256), kGreen, 4, Fill::None)); // on tile edges
        addItem(doc, boxItem(Kind::Ellipse, QRect(70, 60, 2830, 1850), kGreen, 9, Fill::None));
        addItem(doc, boxItem(Kind::Ellipse, QRect(-500, -300, 2000, 1400), kBlue, 13, Fill::Soft));
        addItem(doc, boxItem(Kind::Ellipse, QRect(2200, 1300, 700, 500), kPurple, 5, Fill::Solid));
        addItem(doc, lineItem(Kind::Arrow, QPointF(5.5, 5.5), QPointF(2990.5, 1990.5), kOrange, 12));
        addItem(doc, lineItem(Kind::Arrow, QPointF(2900.5, 100.5), QPointF(300.5, 100.5), kRed, 6));
        addItem(doc, lineItem(Kind::Arrow, QPointF(1200.5, 1950.5), QPointF(1200.5, 300.5), kBlue, 9));
        addItem(doc, lineItem(Kind::Line, QPointF(-900.5, 1800.5), QPointF(3900.5, 200.5), kPurple, 5));
        addItem(doc, lineItem(Kind::Line, QPointF(10.5, 512.5), QPointF(2990.5, 512.5), kDark, 3));
        QList<QPointF> spiral;
        for (int i = 0; i < 600; ++i) {
            const double t = i * 0.05;
            spiral.append(pixelCentre(QPointF(1500 + std::cos(t) * (100 + i * 1.4), 1000 + std::sin(t) * (100 + i * 1.2))));
        }
        addItem(doc, strokeItem(Kind::Pen, spiral, kPurple, 8));
        addItem(doc, strokeItem(Kind::Marker, {QPointF(200.5, 900.5), QPointF(2800.5, 1100.5), QPointF(2800.5, 900.5), QPointF(200.5, 1100.5)}, kYellow, 30));
        addItem(doc, textItem(QPoint(600, 400), u"大字 80 px 跨过瓦片\nWin顺 😀"_s, 80, true, true));
        addItem(doc, textItem(QPoint(1250, 1270), u"中等 48 px\n两行"_s, 48));
        addItem(doc, textItem(QPoint(250, 250), u"小字 14 px"_s, 14, false, true, kYellow));
        addItem(doc, textItem(QPoint(-40, 1000), u"出界的字"_s, 64, true, false, kGreen));
        for (int i = 0; i < 12; ++i)
            addItem(doc, numberItem(QPointF(255.5 + i * 211, 1890.5), 0, palette()[i % 8], 36));
        const QRect area = sample.adjusted(-40, -40, 40, 40);
        Compositor first;
        const QImage whole = renderArea(first, doc, base, sample, area);
        for (const QPoint origin : {QPoint(-37, -53), QPoint(101, 7)}) {
            Compositor tiles;
            const Diff d = compare(renderTiled(tiles, doc, base, sample, area, origin, 256), whole);
            if (d.count != 0) {
                QFAIL(qPrintable(u"tiles through (%1, %2): %3; %4"_s.arg(origin.x())
                        .arg(origin.y())
                        .arg(describe(d), culprits(doc, base, sample, area, origin, 256))));
            }
        }
    }

    void tilesCompositor()
    {
        Rng rng(31);
        const QSize canvas(500, 360);
        const QImage base = noiseImage(canvas, 2);
        const QRect all(QPoint(0, 0), canvas);
        Document doc = randomDoc(rng, canvas, 25);
        const quint32 edited = addItem(doc, textItem(QPoint(100, 100), u"正在编辑"_s, 27, false, true));
        Compositor normal;
        const QImage expected = renderArea(normal, doc, base, all, all);
        QVERIFY(normal.cacheBytes() > 0);
        // A budget too small for anything: nothing kept, the same picture.
        Compositor tiny(1);
        QCOMPARE(compare(renderArea(tiny, doc, base, all, all), expected).count, 0);
        QCOMPARE(tiny.cacheBytes(), 0);
        normal.clear();
        QCOMPARE(normal.cacheBytes(), 0);
        // The export is the crop, opaque.
        doc.crop = QRect(30, 40, 200, 100);
        const QImage cropped = normal.exportImage(doc, base, all);
        QCOMPARE(cropped.format(), QImage::Format_RGB32);
        QCOMPARE(compare(cropped, expected.copy(doc.crop)).count, 0);
        for (int y = 0; y < cropped.height(); ++y) {
            for (int x = 0; x < cropped.width(); ++x)
                QVERIFY(qAlpha(cropped.pixel(x, y)) == 255);
        }
        // A crop past the canvas is cut to it; an empty one is all of it.
        doc.crop = QRect(-50, -50, 100, 100);
        QCOMPARE(compare(normal.exportImage(doc, base, all), expected.copy(0, 0, 50, 50)).count, 0);
        doc.crop = QRect();
        QCOMPARE(compare(normal.exportImage(doc, base, all), expected).count, 0);
        // The text being edited is left out.
        Document without = doc;
        without.items.removeAt(indexOf(without, edited));
        QImage skipped;
        normal.render(doc, base, all, all, &skipped, edited);
        QCOMPARE(compare(skipped, renderArea(normal, without, base, all, all)).count, 0);
        // A text moved keeps its layer (nothing new is kept), and on a plain
        // base it is the same pixels moved.
        {
            const QImage plain = plainImage(canvas, kWhite);
            Document one = emptyDoc(canvas);
            const quint32 id = addItem(one, textItem(QPoint(40, 50), u"移动 Move 72"_s, 72, true, true, kBlue));
            addItem(one, numberItem(QPointF(60.5, 250.5), 0, kGreen, 27));
            Compositor c;
            const QImage before = renderArea(c, one, plain, all, all);
            const qint64 bytes = c.cacheBytes();
            MoveItems move({id, one.items.last().id}, QPoint(37, 21));
            c.invalidate(move.apply(one));
            const QImage after = renderArea(c, one, plain, all, all);
            QCOMPARE(c.cacheBytes(), bytes);
            const QSize kept = canvas - QSize(37, 21);
            QCOMPARE(compare(after.copy(QRect(QPoint(37, 21), kept)), before.copy(QRect(QPoint(0, 0), kept))).count, 0);
        }
        // A base in another format draws the same.
        for (QImage::Format format : {QImage::Format_ARGB32, QImage::Format_ARGB32_Premultiplied, QImage::Format_RGB888}) {
            Compositor other;
            QCOMPARE(compare(renderArea(other, doc, base.convertToFormat(format), all, all), expected).count, 0);
        }
        // A target of another size or format is made again.
        QImage target(10, 10, QImage::Format_ARGB32);
        normal.render(doc, base, all, QRect(5, 5, 64, 48), &target);
        QCOMPARE(target.size(), QSize(64, 48));
        QCOMPARE(target.format(), QImage::Format_RGB32);
        QCOMPARE(compare(target, expected.copy(5, 5, 64, 48)).count, 0);
    }

    // --- commands -----------------------------------------------------------------

    void commandsProperty_data()
    {
        QTest::addColumn<quint64>("seed");
        for (quint64 seed = 1; seed <= 6; ++seed)
            QTest::addRow("seed%llu", seed) << seed;
    }

    // Random commands, undos and redos, interleaved: after every step the
    // document is the one expected; the compositor, told only the change,
    // draws what a fresh one draws; and no pixel changed outside the
    // change's dirty region. Undoing everything gives the first document,
    // redoing everything the last.
    void commandsProperty()
    {
        QFETCH(quint64, seed);
        Rng rng(seed * 104729);
        const QSize canvas(360, 260);
        const QImage base = noiseImage(canvas, seed);
        const QRect all(QPoint(0, 0), canvas);
        Document doc = randomDoc(rng, canvas, 10);
        CommandMaker maker(rng);
        CommandStack stack;
        QList<Document> states{doc}; // states[k]: the document with k steps done
        qint64 clock = 1000;
        Compositor live;
        QImage shown = renderArea(live, doc, base, all, all);
        for (int step = 0; step < 150; ++step) {
            const int what = rng.range(0, 9);
            Change change;
            QString done;
            if (what < 6 || (!stack.canUndo() && !stack.canRedo())) {
                std::unique_ptr<Command> command = maker.make(doc);
                done = command->label;
                const int before = stack.position();
                clock += rng.chance(50) ? rng.range(0, 700) : rng.range(701, 3000);
                change = stack.push(std::move(command), doc, clock);
                if (stack.position() == before) {
                    states[before] = doc; // taken into the step before
                } else {
                    states.resize(before + 1);
                    states.append(doc);
                }
            } else if ((what < 8 && stack.canUndo()) || !stack.canRedo()) {
                done = u"undo "_s + stack.undoLabel();
                change = stack.undo(doc);
            } else {
                done = u"redo "_s + stack.redoLabel();
                change = stack.redo(doc);
            }
            const QString where = u"step %1 (%2)"_s.arg(step).arg(done);
            QVERIFY2(int(states.size()) == stack.count() + 1, qPrintable(where));
            QVERIFY2(doc == states[stack.position()], qPrintable(where));
            QVERIFY2(std::is_partitioned(doc.items.cbegin(), doc.items.cend(), [](const Item& item) { return item.kind == Kind::Redact; }),
                qPrintable(where));
            QSet<quint32> unique;
            for (const Item& item : doc.items)
                unique.insert(item.id);
            QVERIFY2(unique.size() == doc.items.size(), qPrintable(where));
            live.invalidate(change);
            const QImage now = renderArea(live, doc, base, all, all);
            Compositor fresh;
            const Diff cached = compare(now, renderArea(fresh, doc, base, all, all));
            QVERIFY2(cached.count == 0, qPrintable(where + u": "_s + describe(cached) + firstAt(cached, doc, QPoint())));
            if (const std::optional<QPoint> p = changedOutside(shown, now, change.dirty.toAlignedRect()))
                QFAIL(qPrintable(where + u": (%1, %2) changed outside the dirty region"_s.arg(p->x()).arg(p->y())));
            shown = now;
        }
        while (stack.canUndo())
            stack.undo(doc);
        QCOMPARE(doc, states.first());
        while (stack.canRedo())
            stack.redo(doc);
        QCOMPARE(doc, states.last());
    }

    void commandsMerging()
    {
        Document doc = emptyDoc(QSize(300, 200));
        const quint32 a = addItem(doc, boxItem(Kind::Rect, QRect(10, 10, 50, 40)));
        const quint32 b = addItem(doc, boxItem(Kind::Ellipse, QRect(100, 10, 50, 40)));
        const Document start = doc;
        CommandStack stack;
        const auto move = [](quint32 id, QPoint delta) { return std::make_unique<MoveItems>(QList<quint32>{id}, delta); };
        // Presses on the same item, each within 700 ms of the last, are one step.
        stack.push(move(a, QPoint(1, 0)), doc, 1000);
        stack.push(move(a, QPoint(1, 0)), doc, 1600);
        stack.push(move(a, QPoint(0, 1)), doc, 2300);
        QCOMPARE(stack.count(), 1);
        QCOMPARE(stack.undoLabel(), u"移动"_s);
        stack.push(move(a, QPoint(1, 0)), doc, 3001); // 701 ms
        QCOMPARE(stack.count(), 2);
        // Another item; a drag, which keeps its own step.
        stack.push(move(b, QPoint(1, 0)), doc, 3100);
        QCOMPARE(stack.count(), 3);
        auto drag = move(b, QPoint(5, 5));
        drag->merge = false;
        stack.push(std::move(drag), doc, 3200);
        QCOMPARE(stack.count(), 4);
        // The wheel on a width: one step.
        Style thick = findItem(doc, b)->style;
        thick.width = 9;
        stack.push(std::make_unique<Restyle>(doc, QList<quint32>{b}, QList<Style>{thick}), doc, 3300);
        thick.width = 10;
        stack.push(std::make_unique<Restyle>(doc, QList<quint32>{b}, QList<Style>{thick}), doc, 3400);
        QCOMPARE(stack.count(), 5);
        QCOMPARE(stack.undoLabel(), u"改样式"_s);
        // Reshaping the same item: one step, back to before the first.
        stack.push(std::make_unique<Reshape>(doc, reshaped(*findItem(doc, a), int(BoxHandle::Right), QPointF(80.5, 0), false)), doc, 3500);
        stack.push(std::make_unique<Reshape>(doc, reshaped(*findItem(doc, a), int(BoxHandle::Right), QPointF(90.5, 0), false)), doc, 3600);
        QCOMPARE(stack.count(), 6);
        QCOMPARE(itemBox(*findItem(doc, a)), QRect(13, 11, 78, 40));
        stack.undo(doc);
        QCOMPARE(itemBox(*findItem(doc, a)), QRect(13, 11, 50, 40));
        stack.undo(doc);
        QCOMPARE(findItem(doc, b)->style.width, 6);
        QCOMPARE(stack.redoLabel(), u"改样式"_s);
        stack.undo(doc);
        stack.undo(doc);
        QCOMPARE(itemBox(*findItem(doc, b)), QRect(100, 10, 50, 40));
        stack.undo(doc);
        QCOMPARE(itemBox(*findItem(doc, a)), QRect(12, 11, 50, 40));
        stack.undo(doc);
        QCOMPARE(doc, start);
        QVERIFY(!stack.canUndo());
        // Nothing is taken into a step brought back by redo; a push drops the steps undone.
        stack.redo(doc);
        stack.push(move(a, QPoint(1, 0)), doc, 3700);
        QCOMPARE(stack.count(), 2);
        QCOMPARE(stack.position(), 2);
        QVERIFY(!stack.canRedo());
        // A different command on the same item is a step of its own.
        stack.push(std::make_unique<Restyle>(doc, QList<quint32>{a}, QList<Style>{thick}), doc, 3800);
        QCOMPARE(stack.count(), 3);
    }

    void commandsLimit()
    {
        Document doc = emptyDoc(QSize(100, 100));
        CommandStack stack;
        for (int i = 0; i < 250; ++i) {
            Item item = boxItem(Kind::Rect, QRect(i % 50, i % 40, 10, 10));
            item.id = doc.nextId;
            item.seq = doc.nextSeq;
            stack.push(std::make_unique<AddItems>(QList<Item>{item}), doc, i * 1000);
        }
        QCOMPARE(stack.count(), CommandStack::kLimit);
        QCOMPARE(stack.position(), 200);
        QCOMPARE(stack.undoLabel(), u"添加"_s);
        int undone = 0;
        while (stack.canUndo()) {
            stack.undo(doc);
            ++undone;
        }
        QCOMPARE(undone, 200);
        // The 50 oldest steps were dropped: their items stay.
        QCOMPARE(doc.items.size(), qsizetype(50));
        QCOMPARE(doc.nextId, 51u);
        QCOMPARE(stack.undoLabel(), QString());
        stack.clear();
        QVERIFY(!stack.canUndo());
        QVERIFY(!stack.canRedo());
        QCOMPARE(stack.count(), 0);
    }

    // A gesture: the command made at the press, reverted, changed and applied
    // again at every move, pushed as applied at the release; Esc reverts it.
    void commandsGesture()
    {
        Document doc = emptyDoc(QSize(300, 200));
        addItem(doc, boxItem(Kind::Rect, QRect(10, 10, 30, 30)));
        const Document start = doc;
        Item pen = strokeItem(Kind::Pen, {QPointF(10.5, 10.5)}, kRed, 4);
        pen.id = doc.nextId;
        pen.seq = doc.nextSeq;
        auto add = std::make_unique<AddItems>(QList<Item>{pen});
        add->apply(doc);
        for (int i = 1; i < 20; ++i) {
            add->revert(doc);
            QCOMPARE(doc, start);
            add->items[0] = reshaped(add->items[0], int(LineHandle::End), QPointF(10.5 + i * 3, 10.5 + i), false);
            add->apply(doc);
        }
        add->items[0].points = simplifyStroke(add->items[0].points);
        add->revert(doc);
        add->apply(doc);
        const Document end = doc;
        QCOMPARE(findItem(end, pen.id)->points.size(), qsizetype(2)); // a straight stroke keeps its ends
        CommandStack stack;
        stack.pushApplied(std::move(add), 5000);
        stack.undo(doc);
        QCOMPARE(doc, start);
        stack.redo(doc);
        QCOMPARE(doc, end);
        // Esc in the middle of a drag.
        auto drag = std::make_unique<MoveItems>(QList<quint32>{pen.id}, QPoint(5, 5));
        drag->apply(doc);
        drag->revert(doc);
        QCOMPARE(doc, end);
    }

    void commandsChanges()
    {
        Document doc = emptyDoc(QSize(400, 300));
        const quint32 r1 = addItem(doc, redactItem(QRect(10, 10, 50, 30), RedactMode::Mosaic, 8, 1));
        const quint32 r2 = addItem(doc, redactItem(QRect(200, 100, 40, 40), RedactMode::Blur, 8, 2));
        const quint32 n1 = addItem(doc, numberItem(QPointF(100.5, 200.5)));
        const quint32 n2 = addItem(doc, numberItem(QPointF(300.5, 250.5)));
        const quint32 rect = addItem(doc, boxItem(Kind::Rect, QRect(100, 20, 60, 40)));
        // Moving a box: its old and new place.
        Change c = MoveItems({rect}, QPoint(10, 5)).apply(doc);
        QCOMPARE(c.ids, QList<quint32>{rect});
        QCOMPARE(c.dirty, QRectF(100, 20, 70, 45));
        QVERIFY(!c.numbers && !c.redactions && !c.crop);
        // Moving a redaction changes what every redaction may read: all are drawn again.
        c = MoveItems({r1}, QPoint(5, 0)).apply(doc);
        QVERIFY(c.redactions);
        QVERIFY(c.dirty.contains(QRectF(200, 100, 40, 40)));
        // Its cell size changes only its own pixels.
        Style fine = findItem(doc, r1)->style;
        fine.cell = 4;
        c = Restyle(doc, {r1}, {fine}).apply(doc);
        QVERIFY(!c.redactions);
        QCOMPARE(c.dirty, QRectF(15, 10, 50, 30));
        // Deleting the first number renumbers the second: both are in the region.
        const QRectF second = bounds(doc, *findItem(doc, n2));
        c = RemoveItems({n1}).apply(doc);
        QVERIFY(c.numbers);
        QVERIFY(c.dirty.contains(second));
        QCOMPARE(numbers(doc).value(n2), 1);
        // The crop draws nothing again.
        c = SetCrop(doc, QRect(5, 5, 100, 100)).apply(doc);
        QVERIFY(c.crop);
        QVERIFY(c.dirty.isEmpty());
        QVERIFY(c.ids.isEmpty());
        QCOMPARE(doc.crop, QRect(5, 5, 100, 100));
        // Changes add up.
        Change x;
        x.ids = {1};
        x.dirty = QRectF(0, 0, 10, 10);
        Change y;
        y.ids = {1, 2};
        y.dirty = QRectF(20, 20, 5, 5);
        y.numbers = true;
        y.crop = true;
        x.merge(y);
        QCOMPARE(x.ids, (QList<quint32>{1, 2}));
        QCOMPARE(x.dirty, QRectF(0, 0, 25, 25));
        QVERIFY(x.numbers && x.crop && !x.redactions);
        // Reordering stays in the segment and comes back.
        const Document before = doc;
        Reorder down(rect, 0);
        down.apply(doc);
        QCOMPARE(indexOf(doc, rect), redactionEnd(doc));
        down.revert(doc);
        QCOMPARE(doc, before);
        Reorder up(r2, -1);
        up.apply(doc);
        QCOMPARE(indexOf(doc, r2), redactionEnd(doc) - 1);
        up.revert(doc);
        QCOMPARE(doc, before);
        // Added at index 0, a box still lands above the redactions.
        Item extra = boxItem(Kind::Ellipse, QRect(1, 1, 5, 5));
        extra.id = doc.nextId;
        extra.seq = doc.nextSeq;
        AddItems add({extra}, 0);
        add.apply(doc);
        QCOMPARE(indexOf(doc, extra.id), redactionEnd(doc));
        QCOMPARE(doc.nextId, extra.id + 1);
        add.revert(doc);
        QCOMPARE(doc, before);
        // A batch is one step: its first step's label, undone in reverse.
        std::vector<std::unique_ptr<Command>> steps;
        steps.push_back(std::make_unique<RemoveItems>(QList<quint32>{r1, r2}));
        steps.push_back(std::make_unique<MoveItems>(QList<quint32>{rect}, QPoint(3, 3)));
        Batch batch(std::move(steps));
        QCOMPARE(batch.label, u"删除"_s);
        c = batch.apply(doc);
        QVERIFY(c.redactions);
        QCOMPARE(redactionEnd(doc), 0);
        batch.revert(doc);
        QCOMPARE(doc, before);
    }

    // --- numbers ----------------------------------------------------------------

    void numbersCounting()
    {
        Document doc = emptyDoc(QSize(600, 200));
        QList<quint32> n;
        for (int i = 0; i < 4; ++i)
            n.append(addItem(doc, numberItem(QPointF(40.5 + 60 * i, 40.5))));
        const auto shown = [&](const QList<quint32>& ids) {
            const QHash<quint32, int> all = numbers(doc);
            QList<int> out;
            for (quint32 id : ids)
                out.append(all.value(id, -999));
            return out;
        };
        QCOMPARE(shown(n), (QList<int>{1, 2, 3, 4}));
        CommandStack stack;
        qint64 clock = 0;
        const auto push = [&](std::unique_ptr<Command> command) {
            clock += 1000;
            stack.push(std::move(command), doc, clock);
        };
        // Deleting one in the middle: the ones after it close up, no gap.
        push(std::make_unique<RemoveItems>(QList<quint32>{n[1]}));
        QCOMPARE(shown({n[0], n[2], n[3]}), (QList<int>{1, 2, 3}));
        stack.undo(doc);
        QCOMPARE(shown(n), (QList<int>{1, 2, 3, 4}));
        // Where the series starts.
        push(SetSeries::setStart(doc, 1, 5));
        QCOMPARE(shown(n), (QList<int>{5, 6, 7, 8}));
        QCOMPARE(stack.undoLabel(), u"改起始值"_s);
        // A number set by hand; the ones after it count on from it.
        push(std::make_unique<RemoveItems>(QList<quint32>{n[1]}));
        push(std::make_unique<Renumber>(doc, n[2], 10));
        QCOMPARE(shown({n[0], n[2], n[3]}), (QList<int>{5, 10, 11}));
        // A new series counts from 1 and leaves the old one alone.
        push(SetSeries::newSeries(doc));
        QCOMPARE(doc.currentSeries, 2u);
        QVERIFY(doc.series.last() == (Series{2, 1}));
        QList<quint32> m;
        for (int i = 0; i < 2; ++i) {
            Item item = numberItem(QPointF(40.5 + 60 * i, 140.5), doc.currentSeries, kBlue);
            item.id = doc.nextId;
            item.seq = doc.nextSeq;
            m.append(item.id);
            push(std::make_unique<AddItems>(QList<Item>{item}));
        }
        QCOMPARE(shown(m), (QList<int>{1, 2}));
        QCOMPARE(shown({n[0], n[2], n[3]}), (QList<int>{5, 10, 11}));
        // Cleared, the hand-set number is counted again.
        push(std::make_unique<Renumber>(doc, n[2], std::nullopt));
        QCOMPARE(shown({n[0], n[2], n[3]}), (QList<int>{5, 6, 7}));
        // Undoing everything gives the start back.
        while (stack.canUndo())
            stack.undo(doc);
        QCOMPARE(shown(n), (QList<int>{1, 2, 3, 4}));
        QCOMPARE(doc.series.size(), qsizetype(1));
        QCOMPARE(doc.currentSeries, 1u);
        // A number of a series the document does not have counts from 1.
        Document lone = emptyDoc(QSize(100, 100));
        const quint32 stray = addItem(lone, numberItem(QPointF(10.5, 10.5), 42));
        QCOMPARE(numbers(lone).value(stray), 1);
    }

    void numbersLooks()
    {
        const QPointF centre(100.5, 100.5);
        const Item red = numberItem(centre, 1, kRed, 27);
        const NumberGeometry seven = numberGeometry(red, 7, 1.5);
        QCOMPARE(seven.disc.width(), seven.disc.height());
        QCOMPARE(int(seven.disc.height()) % 2, 1);
        QCOMPARE(seven.disc.center(), centre);
        QCOMPARE(seven.label, u"7"_s);
        QCOMPARE(seven.outline, 0);
        QVERIFY(seven.disc.contains(seven.ink));
        // Two digits make a capsule, still odd and centred.
        const NumberGeometry ten = numberGeometry(red, 10, 1.5);
        QVERIFY(ten.disc.width() > ten.disc.height());
        QCOMPARE(int(ten.disc.width()) % 2, 1);
        QCOMPARE(ten.disc.center(), centre);
        QVERIFY(ten.disc.contains(ten.ink));
        // A white disc gets a ring of 1 logical px, #212121 at 30 %.
        QCOMPARE(numberGeometry(numberItem(centre, 1, kWhite), 1, 1.5).outline, 2);
        QCOMPARE(numberGeometry(numberItem(centre, 1, kWhite), 1, 1.0).outline, 1);
        QCOMPARE(qAlpha(numberOutlineColor()), 77);
        // Marks on colours are white; on yellow and white they are #212121.
        for (QRgb c : {kRed, kOrange, kGreen, kBlue, kPurple, kDark})
            QCOMPARE(inkOn(c), kWhite);
        QCOMPARE(inkOn(kYellow), kDark);
        QCOMPARE(inkOn(kWhite), kDark);
    }

    // --- number formats and the words after a number (#57) ------------------------

    void numberFormatsWrite()
    {
        const auto in = [](int n, NumberFormat f) { return formatNumber(n, f); };
        for (const NumberFormat f : {NumberFormat::Arabic, NumberFormat::Circled}) {
            QCOMPARE(in(1, f), u"1"_s);
            QCOMPARE(in(25, f), u"25"_s); // past 20 too: digits in the ring
        }
        // Letters count like columns of a spreadsheet: after Z comes AA.
        QCOMPARE(in(1, NumberFormat::UpperLetters), u"A"_s);
        QCOMPARE(in(26, NumberFormat::UpperLetters), u"Z"_s);
        QCOMPARE(in(27, NumberFormat::UpperLetters), u"AA"_s);
        QCOMPARE(in(52, NumberFormat::UpperLetters), u"AZ"_s);
        QCOMPARE(in(53, NumberFormat::UpperLetters), u"BA"_s);
        QCOMPARE(in(702, NumberFormat::UpperLetters), u"ZZ"_s);
        QCOMPARE(in(703, NumberFormat::UpperLetters), u"AAA"_s);
        QCOMPARE(in(3, NumberFormat::LowerLetters), u"c"_s);
        QCOMPARE(in(28, NumberFormat::LowerLetters), u"ab"_s);
        // Roman numerals, to 3999.
        QCOMPARE(in(1, NumberFormat::Roman), u"I"_s);
        QCOMPARE(in(4, NumberFormat::Roman), u"IV"_s);
        QCOMPARE(in(9, NumberFormat::Roman), u"IX"_s);
        QCOMPARE(in(18, NumberFormat::Roman), u"XVIII"_s);
        QCOMPARE(in(40, NumberFormat::Roman), u"XL"_s);
        QCOMPARE(in(1994, NumberFormat::Roman), u"MCMXCIV"_s);
        QCOMPARE(in(3999, NumberFormat::Roman), u"MMMCMXCIX"_s);
        QCOMPARE(in(4000, NumberFormat::Roman), u"4000"_s);
        // What a format cannot write shows as digits.
        for (const NumberFormat f : {NumberFormat::UpperLetters, NumberFormat::LowerLetters, NumberFormat::Roman}) {
            QCOMPARE(in(0, f), u"0"_s);
            QCOMPARE(in(-3, f), u"-3"_s);
        }
        // The names in doc.json and in the remembered styles.
        for (int i = 0; i < kNumberFormatCount; ++i) {
            const NumberFormat f = NumberFormat(i);
            QCOMPARE(numberFormatFromName(numberFormatName(f)), std::optional<NumberFormat>(f));
        }
        QVERIFY(!numberFormatFromName(u"hex"));
    }

    void numberFormatsFollowTheSeries()
    {
        Document doc = emptyDoc(QSize(600, 200));
        QList<quint32> n;
        for (int i = 0; i < 3; ++i)
            n.append(addItem(doc, numberItem(QPointF(60.5 + 60 * i, 60.5))));
        CommandStack stack;
        QCOMPARE(formatOf(doc, 1), NumberFormat::Arabic);
        stack.push(SetSeries::setFormat(doc, 1, NumberFormat::Roman), doc, 1000);
        QCOMPARE(formatOf(doc, 1), NumberFormat::Roman);
        QCOMPARE(stack.undoLabel(), u"改序号格式"_s);
        QCOMPARE(doc.series.first().start, 1); // the start stays
        // The start changed afterwards keeps the format; a new series starts as the current one writes.
        stack.push(SetSeries::setStart(doc, 1, 4), doc, 2000);
        QCOMPARE(formatOf(doc, 1), NumberFormat::Roman);
        stack.push(SetSeries::newSeries(doc), doc, 3000);
        QCOMPARE(doc.currentSeries, 2u);
        QCOMPARE(formatOf(doc, 2), NumberFormat::Roman);
        QCOMPARE(formatOf(doc, 99), NumberFormat::Arabic); // a series the document does not have
        // A format change redraws the numbers and widens the capsule: "XVIII" is wider than "8".
        Document wide = emptyDoc(QSize(600, 200));
        Item item = numberItem(QPointF(100.5, 100.5), 1);
        const quint32 id = addItem(wide, item);
        const qreal plain = bounds(wide, *findItem(wide, id)).width();
        CommandStack s2;
        const Change change = s2.push(SetSeries::setFormat(wide, 1, NumberFormat::Roman), wide, 1);
        QVERIFY(change.numbers);
        QVERIFY(change.dirty.contains(bounds(wide, *findItem(wide, id))));
        QVERIFY(plain > 0);
        wide.series[0].start = 18;
        const NumberGeometry g = numberGeometry(*findItem(wide, id), 18, 1.0, NumberFormat::Roman);
        QCOMPARE(g.label, u"XVIII"_s);
        QVERIFY(g.disc.width() > g.disc.height());
        QVERIFY(g.disc.contains(g.ink)); // the badge fits the wider label
        QVERIFY(bounds(*findItem(wide, id), 1.0, 18, NumberFormat::Roman).contains(g.disc));
        // A ring (Circled) is hollow and still holds its digits.
        const NumberGeometry ring = numberGeometry(*findItem(wide, id), 7, 1.0, NumberFormat::Circled);
        QVERIFY(ring.ring > 0);
        QCOMPARE(numberGeometry(*findItem(wide, id), 7, 1.0).ring, 0.0);
    }

    void numberWordsStandBesideTheNumber()
    {
        Document doc = emptyDoc(QSize(600, 200));
        Item item = numberItem(QPointF(100.5, 100.5), 0, kBlue, 27);
        const quint32 plain = addItem(doc, item);
        item.points = {QPointF(100.5, 160.5)};
        item.text = u"登录 Login"_s;
        const quint32 worded = addItem(doc, item);
        const NumberGeometry none = numberGeometry(*findItem(doc, plain), 1, 1.0);
        QVERIFY(none.wordsBox.isEmpty());
        const NumberGeometry g = numberGeometry(*findItem(doc, worded), 2, 1.0);
        QVERIFY(!g.wordsBox.isEmpty());
        QVERIFY(g.wordsBox.left() > g.disc.right()); // to the right, not over the disc
        QCOMPARE(g.wordsOrigin, g.wordsBox.topLeft());
        // The words' first baseline is the digits': the same top plus the same ascent.
        QCOMPARE(g.wordsBox.top() + textMetrics(u"x"_s, 27, false).ascent, g.origin.y() + textMetrics(u"2"_s, 27, true).ascent);
        // Blank words are none.
        Item blank = *findItem(doc, worded);
        blank.text = u"  \n "_s;
        QVERIFY(numberGeometry(blank, 2, 1.0).wordsBox.isEmpty());
        // Bounds, selection and hits include the words, and only the worded one.
        const QRectF with = bounds(doc, *findItem(doc, worded));
        QVERIFY(with.contains(QRectF(g.wordsBox)));
        QVERIFY(with.right() > g.disc.right() + 20);
        QCOMPARE(hitTest(doc, QPointF(g.wordsBox.center()), 2), std::optional<quint32>(worded));
        QCOMPARE(hitTest(doc, QPointF(g.disc.center()), 2), std::optional<quint32>(worded));
        QCOMPARE(hitTest(doc, QPointF(g.wordsBox.center()) + QPointF(0, -60), 2), std::nullopt);
        QCOMPARE(hitTest(doc, QPointF(none.disc.center().x() + 90, none.disc.center().y()), 2), std::nullopt);
        // White on white is unreadable: the words turn #212121; otherwise they are the colour.
        QCOMPARE(numberWordsColor(*findItem(doc, worded)), kBlue | 0xff000000u);
        Item white = *findItem(doc, worded);
        white.style.color = kWhite;
        QCOMPARE(numberWordsColor(white), kDark);
    }

    // The screen's tiles and the export are one set of pixels, with every format and words.
    void numberFormatsAndWordsDrawTheSameInTiles()
    {
        Document doc = emptyDoc(QSize(640, 300));
        const NumberFormat formats[] {NumberFormat::Arabic, NumberFormat::UpperLetters, NumberFormat::LowerLetters,
            NumberFormat::Roman, NumberFormat::Circled};
        for (int i = 0; i < 5; ++i) {
            doc.series.append(Series{quint32(10 + i), 15 + 3 * i, formats[i]});
            Item item = numberItem(QPointF(40.5, 30.5 + 55 * i), quint32(10 + i), i == 4 ? kWhite : kGreen, 27);
            if (i != 1)
                item.text = i == 3 ? u"第三步 three"_s : u"Click here"_s;
            addItem(doc, item);
        }
        const QImage base = noiseImage(doc.canvas, 5);
        const QRect all(QPoint(0, 0), doc.canvas);
        Compositor whole;
        const QImage want = renderArea(whole, doc, base, all, all);
        for (const QRect& area : {QRect(0, 0, 256, 256), QRect(100, 20, 300, 160), QRect(37, 41, 211, 133), QRect(384, 0, 256, 256)}) {
            Compositor cold;
            const Diff d = compare(renderArea(cold, doc, base, all, area), want.copy(area));
            QVERIFY2(d.count == 0, qPrintable(describe(d)));
        }
        // Drawn without the words being typed: the number is whole, the words are not there.
        Compositor bare;
        QImage without;
        bare.render(doc, base, all, all, &without, std::nullopt, doc.items.last().id);
        QVERIFY(compare(without, want).count > 0);
        Document stripped = doc;
        stripped.items.last().text.clear();
        Compositor reference;
        QCOMPARE(compare(without, renderArea(reference, stripped, base, all, all)).count, 0);
        // The ring is hollow: the pixel in its middle is the picture's own.
        const Item& ringItem = *findItem(doc, doc.items.last().id);
        const NumberGeometry g = numberGeometry(ringItem, 27, 1.0, NumberFormat::Circled);
        QVERIFY(g.ring > 0);
        const QPoint inside = QPoint(int(g.disc.left()) + int(g.ring) + 3, int(g.disc.center().y()));
        QCOMPARE(want.pixel(inside), base.pixel(inside));
    }

    void formatNumberFormatsAndWordsRoundTrip()
    {
        Document doc = emptyDoc(QSize(300, 200), 1.5);
        doc.series = {Series{1, 1, NumberFormat::Arabic}, Series{2, 3, NumberFormat::Roman}, Series{3, 1, NumberFormat::Circled}};
        Item a = numberItem(QPointF(30.5, 30.5), 1);
        a.text = u"第一步 \"quoted\" 😀"_s;
        addItem(doc, a);
        addItem(doc, numberItem(QPointF(30.5, 90.5), 2));
        const QJsonObject json = toJson(doc);
        const QJsonArray series = json.value(u"series"_s).toArray();
        // Written only when it is not 1 2 3: a file that never used a format reads as before.
        QVERIFY(!series[0].toObject().contains(u"format"_s));
        QCOMPARE(series[1].toObject().value(u"format"_s).toString(), u"roman"_s);
        QCOMPARE(series[2].toObject().value(u"format"_s).toString(), u"circled"_s);
        QString error;
        const std::optional<Document> back = fromJson(QJsonDocument::fromJson(QJsonDocument(json).toJson()).object(), &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(*back, doc);
        QCOMPARE(back->items.first().text, a.text);
        QCOMPARE(formatOf(*back, 2), NumberFormat::Roman);
        // A format it does not know reads as digits; a file without the key as before.
        QVERIFY(fromJson(QJsonDocument::fromJson(jsonText("{'format':'winshun-annotation','version':1,'canvas':[10,10],"
                                                          "'series':[{'id':1,'start':2,'format':'klingon'},{'id':2,'start':1}]}")).object(),
            &error)->series == (QList<Series>{Series{1, 2}, Series{2, 1}}));
        // The sample from before the formats opens as it was, in digits, and writes back without the key.
        QFile file(QStringLiteral(WINSHUN_ANNOTATE_DATA) + u"/doc-v1.json"_s);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const std::optional<Document> old = fromJson(QJsonDocument::fromJson(file.readAll()).object(), &error);
        QVERIFY2(old, qPrintable(error));
        for (const Series& s : old->series)
            QCOMPARE(s.format, NumberFormat::Arabic);
        for (const QJsonValue& s : toJson(*old).value(u"series"_s).toArray())
            QVERIFY(!s.toObject().contains(u"format"_s));
        // A project cut to a selection keeps the formats and the words (projectFromOverlay).
        Document overlay = doc;
        const Project cut = projectFromOverlay(overlay, QImage(300, 200, QImage::Format_RGB32), QRect(0, 0, 300, 200));
        QCOMPARE(formatOf(cut.doc, 2), NumberFormat::Roman);
        QCOMPARE(cut.doc.items.first().text, a.text);
    }

    // --- redactions -------------------------------------------------------------

    void redactionReadsNothingUnder_data()
    {
        QTest::addColumn<int>("mode");
        for (RedactMode m : {RedactMode::Mosaic, RedactMode::Blur, RedactMode::Solid, RedactMode::Smooth})
            QTest::addRow("%s", qPrintable(redactModeName(m))) << int(m);
    }

    // Whatever is under the redactions, the picture is the same bit for bit:
    // nothing under them is ever read, so nothing can be recovered.
    void redactionReadsNothingUnder()
    {
        QFETCH(int, mode);
        const QSize size(420, 300);
        const QImage base = noiseImage(size, 5);
        const QRect all(QPoint(0, 0), size);
        Document doc = emptyDoc(size);
        addItem(doc, redactItem(QRect(60, 40, 150, 90), RedactMode(mode), 14, 0x1234));
        addItem(doc, redactItem(QRect(180, 100, 120, 120), RedactMode(mode), 9, 0x99)); // over the first
        addItem(doc, redactItem(QRect(0, 250, 420, 50), RedactMode(mode), 20, 7)); // all the way across
        addItem(doc, lineItem(Kind::Arrow, QPointF(30.5, 280.5), QPointF(300.5, 60.5), kRed, 6));
        const QImage before = Compositor().exportImage(doc, base, all);
        QImage changed = base;
        Rng rng(42);
        for (const QRect& box : redactionBoxes(doc)) {
            for (int y = box.top(); y <= box.bottom(); ++y) {
                for (int x = box.left(); x <= box.right(); ++x)
                    changed.setPixel(x, y, 0xff000000u | QRgb(rng.next() & 0xffffffu));
            }
        }
        const Diff d = compare(Compositor().exportImage(doc, changed, all), before);
        QVERIFY2(d.count == 0, qPrintable(describe(d)));
        // The pixel left of the first cells' centre row is read: a change there shows.
        if (RedactMode(mode) != RedactMode::Solid) {
            QImage next = base;
            next.setPixel(59, 47, base.pixel(59, 47) ^ 0x00ffffffu);
            QVERIFY(compare(Compositor().exportImage(doc, next, all), before).count > 0);
        }
    }

    void redactionNoise()
    {
        const QImage base = noiseImage(QSize(300, 200), 9);
        const QRect s(0, 0, 300, 200);
        for (RedactMode mode : {RedactMode::Mosaic, RedactMode::Blur}) {
            Document doc = emptyDoc(base.size());
            const quint32 id = addItem(doc, redactItem(QRect(50, 50, 120, 80), mode, 12, 0xABCDEF));
            const Item item = *findItem(doc, id);
            const QImage mask = coverageMask(doc, s);
            const QImage one = redactionPatch(base, s, mask, item);
            QCOMPARE(one.size(), QSize(120, 80));
            QCOMPARE(one.offset(), QPoint(50, 50));
            QCOMPARE(one.format(), QImage::Format_RGB32);
            // The same seed, the same pixels; another seed, others.
            QCOMPARE(compare(redactionPatch(base, s, mask, item), one).count, 0);
            Item other = item;
            other.seed ^= 1;
            QVERIFY(compare(redactionPatch(base, s, mask, other), one).count > 0);
        }
        // On plain grey the noise shows alone: the same offset on R, G and B,
        // within 16 for a mosaic cell (one for the whole cell), 4 for a
        // blurred pixel.
        const QImage grey = plainImage(QSize(300, 200), qRgb(100, 100, 100));
        Document doc = emptyDoc(grey.size());
        const quint32 m = addItem(doc, redactItem(QRect(40, 30, 130, 100), RedactMode::Mosaic, 13, 5));
        const quint32 b = addItem(doc, redactItem(QRect(180, 30, 100, 100), RedactMode::Blur, 13, 5));
        const quint32 solid = addItem(doc, redactItem(QRect(10, 150, 50, 30), RedactMode::Solid, 13, 5));
        const QImage mask = coverageMask(doc, s);
        const QImage mosaic = redactionPatch(grey, s, mask, *findItem(doc, m));
        QSet<QRgb> cells;
        for (int y = 0; y < mosaic.height(); ++y) {
            for (int x = 0; x < mosaic.width(); ++x) {
                const QRgb p = mosaic.pixel(x, y);
                QVERIFY(qRed(p) == qGreen(p) && qGreen(p) == qBlue(p));
                QVERIFY(std::abs(qRed(p) - 100) <= 16);
                QCOMPARE(p, mosaic.pixel(x - x % 13, y - y % 13));
                cells.insert(p);
            }
        }
        QVERIFY(cells.size() > 5);
        const QImage blur = redactionPatch(grey, s, mask, *findItem(doc, b));
        for (int y = 0; y < blur.height(); ++y) {
            for (int x = 0; x < blur.width(); ++x) {
                const QRgb p = blur.pixel(x, y);
                QVERIFY(qRed(p) == qGreen(p) && qGreen(p) == qBlue(p));
                QVERIFY(std::abs(qRed(p) - 100) <= 4);
            }
        }
        QCOMPARE(redactionPatch(grey, s, mask, *findItem(doc, solid)).pixel(7, 7), kDark);
        // Smooth has no cells and no noise: the seed does not matter, and it is not the mosaic.
        Item smooth = *findItem(doc, m);
        smooth.style.redact = RedactMode::Smooth;
        Item other = smooth;
        other.seed ^= 0xFFFF;
        const QImage rubbed = redactionPatch(base, s, mask, smooth);
        QCOMPARE(compare(rubbed, redactionPatch(base, s, mask, other)).count, 0);
        QVERIFY(compare(rubbed, redactionPatch(base, s, mask, *findItem(doc, m))).count > 0);
    }

    // On a plain base a redaction moved by whole pixels takes its pixels
    // along: cells and noise go by the box, not by the picture.
    void redactionMoves()
    {
        const QImage grey = plainImage(QSize(400, 300), qRgb(90, 120, 150));
        const QRect all(0, 0, 400, 300);
        for (RedactMode mode : {RedactMode::Mosaic, RedactMode::Blur, RedactMode::Smooth}) {
            Document doc = emptyDoc(grey.size());
            const QRect box(37, 41, 101, 67);
            const quint32 id = addItem(doc, redactItem(box, mode, 13, 77));
            const QImage before = Compositor().exportImage(doc, grey, all);
            MoveItems(QList<quint32>{id}, QPoint(150, 120)).apply(doc);
            const QImage after = Compositor().exportImage(doc, grey, all);
            QCOMPARE(compare(after.copy(box.translated(150, 120)), before.copy(box)).count, 0);
            QCOMPARE(compare(after.copy(box), grey.copy(box)).count, 0);
            if (mode != RedactMode::Smooth) // rubbed out on a flat ground, it is the ground
                QVERIFY(compare(before.copy(box), grey.copy(box)).count > 0);
        }
    }

    void redactionFallbacks()
    {
        const QSize size(200, 160);
        const QImage base = noiseImage(size, 3);
        const QRect s(QPoint(0, 0), size);
        // A band all the way across and a column all the way down. Where they
        // cross, a cell's centre has nothing to read left or right (the band)
        // nor up or down (the column): the band's third cell, centre
        // (100, 80), takes the mean of everything readable.
        Document doc = emptyDoc(size);
        const QRect band(0, 60, 200, 40);
        const QRect column(80, 0, 40, 160);
        const quint32 across = addItem(doc, redactItem(band, RedactMode::Mosaic, 40, 5));
        addItem(doc, redactItem(column, RedactMode::Mosaic, 40, 6));
        double sum[3] = {0, 0, 0};
        int count = 0;
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                if (band.contains(x, y) || column.contains(x, y))
                    continue;
                const QRgb c = base.pixel(x, y);
                sum[0] += qRed(c);
                sum[1] += qGreen(c);
                sum[2] += qBlue(c);
                ++count;
            }
        }
        const QImage patch = redactionPatch(base, s, coverageMask(doc, s), *findItem(doc, across));
        const QRgb c = patch.pixel(100 - band.left(), 80 - band.top());
        const int n = qRed(c) - int(std::lround(sum[0] / count));
        QCOMPARE(qGreen(c) - int(std::lround(sum[1] / count)), n);
        QCOMPARE(qBlue(c) - int(std::lround(sum[2] / count)), n);
        QVERIFY(std::abs(n) <= 16);
        // Nothing readable at all: #808080, with the noise.
        Document covered = emptyDoc(size);
        for (RedactMode mode : {RedactMode::Mosaic, RedactMode::Blur}) {
            const quint32 id = addItem(covered, redactItem(s, mode, 25, 9));
            const QImage grey = redactionPatch(base, s, coverageMask(covered, s), *findItem(covered, id));
            for (int y = 0; y < grey.height(); ++y) {
                for (int x = 0; x < grey.width(); ++x) {
                    const QRgb p = grey.pixel(x, y);
                    QVERIFY(qRed(p) == qGreen(p) && qGreen(p) == qBlue(p));
                    QVERIFY(std::abs(qRed(p) - 128) <= 16);
                }
            }
        }
    }

    // The overlay reads only its selection (S): pixels outside it never matter.
    void redactionSampleBounds()
    {
        const QImage base = noiseImage(QSize(300, 200), 4);
        const QRect s(50, 30, 200, 140);
        Document doc = emptyDoc(base.size());
        const QRect box(40, 60, 100, 50); // over the left edge of S
        const quint32 id = addItem(doc, redactItem(box, RedactMode::Blur, 10, 3));
        const QImage mask = coverageMask(doc, s);
        QCOMPARE(mask.size(), s.size());
        QCOMPARE(mask.format(), QImage::Format_Grayscale8);
        int wrong = 0;
        for (int y = 0; y < mask.height(); ++y) {
            for (int x = 0; x < mask.width(); ++x) {
                if (mask.constScanLine(y)[x] != (box.contains(x + s.left(), y + s.top()) ? 255 : 0))
                    ++wrong;
            }
        }
        QCOMPARE(wrong, 0);
        const QImage patch = redactionPatch(base, s, mask, *findItem(doc, id));
        QImage changed = base;
        for (int y = 0; y < base.height(); ++y) {
            for (int x = 0; x < base.width(); ++x) {
                if (!s.contains(x, y))
                    changed.setPixel(x, y, base.pixel(x, y) ^ 0x00ffffffu);
            }
        }
        QCOMPARE(compare(redactionPatch(changed, s, mask, *findItem(doc, id)), patch).count, 0);
    }

    // --- format -----------------------------------------------------------------

    void formatJsonRoundTrip_data()
    {
        QTest::addColumn<quint64>("seed");
        for (quint64 seed = 1; seed <= 5; ++seed)
            QTest::addRow("seed%llu", seed) << seed;
    }

    void formatJsonRoundTrip()
    {
        QFETCH(quint64, seed);
        Rng rng(seed * 92821);
        Document doc = randomDoc(rng, QSize(rng.range(10, 3000), rng.range(10, 2000)), 40);
        doc.crop = QRect(rng.range(-5, 50), rng.range(-5, 50), rng.range(1, 900), rng.range(1, 700));
        // Fields a kind does not use, set anyway, go both ways too.
        Item& any = doc.items.last();
        any.style.fontPx = 99;
        any.style.cell = 3;
        any.style.textStroke = 4;
        any.style.textStrokeColor = kBlue;
        any.style.shape = RedactShape::Brush;
        any.text = u"stray \"text\"\n第二行 😀"_s;
        any.seed = 0xFFFFFFFFFFFFFFFFull;
        any.pinned = -7;
        any.series = 9;
        const QByteArray text = QJsonDocument(toJson(doc)).toJson(QJsonDocument::Compact);
        QString error;
        const std::optional<Document> back = fromJson(QJsonDocument::fromJson(text).object(), &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(*back, doc);
        QCOMPARE(QJsonDocument(toJson(*back)).toJson(QJsonDocument::Compact), text);
    }

    void formatVersions()
    {
        QString error;
        const auto parse = [&](const char* json) {
            error.clear();
            return fromJson(QJsonDocument::fromJson(jsonText(json)).object(), &error);
        };
        QVERIFY(parse("{'format':'winshun-annotation','version':1,'canvas':[10,10]}"));
        QVERIFY(error.isEmpty());
        QVERIFY(parse("{'format':'winshun-annotation','version':2,'canvas':[10,10]}"));
        QVERIFY(!parse("{'format':'winshun-annotation','version':3,'canvas':[10,10]}"));
        QCOMPARE(error, u"这张图是新版 Win顺 存的，升级后才能编辑"_s);
        QVERIFY(!parse("{'format':'other','version':1,'canvas':[10,10]}"));
        QCOMPARE(error, u"这不是 Win顺 的标注文件。"_s);
        QVERIFY(!parse("{'version':1,'canvas':[10,10]}"));
        QCOMPARE(error, u"这不是 Win顺 的标注文件。"_s);
        for (const char* broken : {"{'format':'winshun-annotation','version':'1','canvas':[10,10]}",
                 "{'format':'winshun-annotation','version':0,'canvas':[10,10]}",
                 "{'format':'winshun-annotation','version':1.5,'canvas':[10,10]}",
                 "{'format':'winshun-annotation','version':1}",
                 "{'format':'winshun-annotation','version':1,'canvas':[0,10]}",
                 "{'format':'winshun-annotation','version':1,'canvas':[10]}"}) {
            QVERIFY2(!parse(broken), broken);
            QCOMPARE(error, u"标注文件已损坏。"_s);
        }
        QVERIFY(!fromJson(QJsonObject(), nullptr));
    }

    void formatUnknownFields()
    {
        const QByteArray json = jsonText(
            "{'format': 'winshun-annotation', 'version': 1, 'canvas': [200, 100], 'futureField': {'x': 1},"
            " 'series': [{'id': 1, 'start': 3}, {'id': 1, 'start': 9}, {'start': 4}],"
            " 'currentSeries': 7, 'nextId': 2, 'nextSeq': 1,"
            " 'items': ["
            "  {'id': 1, 'kind': 'rect', 'seq': 1, 'points': [[0, 0], [10, 10]], 'style': {'color': '#00FF00', 'glow': true}, 'sparkle': 3},"
            "  {'id': 2, 'kind': 'hologram', 'seq': 2, 'points': [[0, 0]]},"
            "  {'id': 3, 'kind': 'line', 'seq': 3, 'points': [[0.5, 0.5]]},"
            "  {'id': 1, 'kind': 'ellipse', 'seq': 4, 'points': [[0, 0], [5, 5]]},"
            "  {'kind': 'rect', 'seq': 5, 'points': [[0, 0], [5, 5]]},"
            "  {'id': 6, 'kind': 'redact', 'seq': 6, 'points': [[20, 20], [40, 40]], 'seed': 'zz'},"
            "  {'id': 7, 'kind': 'text', 'seq': 7, 'points': [[1e30, 0]], 'text': 'far'},"
            "  {'id': 8, 'kind': 'number', 'seq': 9, 'points': [[50.5, 50.5]], 'series': 1, 'pinned': 2.5}"
            " ]}");
        QString error;
        const std::optional<Document> doc = fromJson(QJsonDocument::fromJson(json).object(), &error);
        QVERIFY2(doc, qPrintable(error));
        // Kept: the rect, the redaction (moved first), the number.
        QCOMPARE(idsOf(*doc), (QList<quint32>{6, 1, 8}));
        QCOMPARE(findItem(*doc, 1)->style.color, 0xff00ff00u);
        QCOMPARE(findItem(*doc, 6)->seed, 0ull);
        QVERIFY(!findItem(*doc, 8)->pinned);
        QVERIFY(doc->series == (QList<Series>{Series{1, 3}}));
        QCOMPARE(doc->currentSeries, 1u);
        QCOMPARE(doc->nextId, 9u);
        QCOMPARE(doc->nextSeq, 10u);
        QCOMPARE(doc->crop, QRect(0, 0, 200, 100));
        QCOMPARE(doc->dpr, 1.0);
        QCOMPARE(numbers(*doc).value(8), 3);
    }

    // The v1 sample in tests/data/annotate can always be read.
    void formatFixtureV1()
    {
        QFile file(QStringLiteral(WINSHUN_ANNOTATE_DATA) + u"/doc-v1.json"_s);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        QJsonParseError parseError;
        const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QString error;
        const std::optional<Document> doc = fromJson(json.object(), &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->canvas, QSize(1282, 841));
        QCOMPARE(doc->crop, QRect(0, 0, 1282, 841));
        QCOMPARE(doc->dpr, 1.5);
        QVERIFY(doc->series == (QList<Series>{Series{1, 1}, Series{2, 5}}));
        QCOMPARE(doc->currentSeries, 2u);
        QCOMPARE(doc->nextId, 13u);
        QCOMPARE(doc->nextSeq, 13u);
        QCOMPARE(idsOf(*doc), (QList<quint32>{3, 9, 1, 2, 4, 5, 6, 7, 8, 10, 11, 12}));
        const Item& mosaic = *findItem(*doc, 3);
        QCOMPARE(mosaic.kind, Kind::Redact);
        QCOMPARE(itemBox(mosaic), QRect(100, 40, 280, 32));
        QCOMPARE(mosaic.style.redact, RedactMode::Mosaic);
        QCOMPARE(mosaic.style.cell, 18);
        QCOMPARE(mosaic.seed, 0x9f1c2a7d4e5b6c80ull);
        QCOMPARE(findItem(*doc, 9)->style.redact, RedactMode::Solid);
        QCOMPARE(findItem(*doc, 9)->style.color, kDark);
        const Item& arrow = *findItem(*doc, 1);
        QCOMPARE(arrow.kind, Kind::Arrow);
        QCOMPARE(arrow.points, (QList<QPointF>{QPointF(520.5, 300.5), QPointF(700.5, 180.5)}));
        QCOMPARE(arrow.style.width, 6);
        QCOMPARE(findItem(*doc, 4)->style.fill, Fill::Soft);
        QCOMPARE(findItem(*doc, 4)->style.color, kBlue);
        QCOMPARE(findItem(*doc, 5)->style.width, 11);
        QCOMPARE(findItem(*doc, 7)->points.size(), qsizetype(4));
        QCOMPARE(findItem(*doc, 8)->kind, Kind::Marker);
        const Item& text = *findItem(*doc, 10);
        QCOMPARE(text.text, u"你好\nWin顺"_s);
        QVERIFY(text.style.bold && text.style.boxed);
        QCOMPARE(text.style.fontPx, 27);
        QCOMPARE(findItem(*doc, 11)->pinned.value_or(0), 10);
        const QHash<quint32, int> shown = numbers(*doc);
        QCOMPARE(shown.value(2), 1);
        QCOMPARE(shown.value(11), 10);
        QCOMPARE(shown.value(12), 11);
        // Written again, it reads back the same; and it draws.
        const std::optional<Document> again = fromJson(toJson(*doc), nullptr);
        QVERIFY(again);
        QCOMPARE(*again, *doc);
        const QImage out = Compositor().exportImage(*doc, noiseImage(doc->canvas, 1), QRect(QPoint(0, 0), doc->canvas));
        QCOMPARE(out.size(), doc->canvas);
    }

    void formatProject()
    {
        Rng rng(2024);
        const QSize canvas(640, 400);
        const Document doc = randomDoc(rng, canvas, 30);
        const QImage base = noiseImage(canvas, 8);
        const QByteArray bytes = writeProject(doc, base);
        QVERIFY(!bytes.isEmpty());
        // doc.json deflated, base.png stored (PNG is compressed already).
        const QList<std::pair<QByteArray, int>> entries = zipEntries(bytes);
        QCOMPARE(entries.size(), qsizetype(2));
        QCOMPARE(entries[0].first, QByteArray("doc.json"));
        QCOMPARE(entries[0].second, 8);
        QCOMPARE(entries[1].first, QByteArray("base.png"));
        QCOMPARE(entries[1].second, 0);
        QString error;
        const std::optional<Project> project = readProject(bytes, &error);
        QVERIFY2(project, qPrintable(error));
        QCOMPARE(project->doc, doc);
        QCOMPARE(project->base.format(), QImage::Format_RGB32);
        QCOMPARE(compare(project->base, base).count, 0);
        // A picture in another format comes back as the same RGB32 pixels.
        const std::optional<Project> argb = readProject(writeProject(doc, base.convertToFormat(QImage::Format_ARGB32)), &error);
        QVERIFY(argb);
        QCOMPARE(compare(argb->base, base).count, 0);
        // Damaged: not a zip, cut short, a picture of another size.
        const QString broken = u"标注文件已损坏。"_s;
        error.clear();
        QVERIFY(!readProject(QByteArray("PK not a zip"), &error));
        QCOMPARE(error, broken);
        error.clear();
        QVERIFY(!readProject(bytes.left(bytes.size() / 2), &error));
        QCOMPARE(error, broken);
        error.clear();
        QVERIFY(!readProject(writeProject(doc, base.copy(0, 0, 10, 10)), &error));
        QCOMPARE(error, broken);
    }

    // --- overlay -> project -------------------------------------------------------

    void overlayToProject_data()
    {
        QTest::addColumn<quint64>("seed");
        for (quint64 seed = 1; seed <= 4; ++seed)
            QTest::addRow("seed%llu", seed) << seed;
    }

    // The overlay's picture of its selection is exactly the project's
    // picture once saved and opened again, redactions at the selection's
    // edges included.
    void overlayToProject()
    {
        QFETCH(quint64, seed);
        Rng rng(seed * 31337);
        const QImage screen = noiseImage(QSize(1600, 1000), seed + 100);
        const QRect selection(203, 151, 900, 600);
        Document doc = randomDoc(rng, screen.size(), 40, selection.adjusted(-140, -140, 140, 140));
        doc.crop = selection;
        addItem(doc, redactItem(QRect(selection.left() - 30, 300, 80, 60), RedactMode::Mosaic, 12, rng.next()));
        addItem(doc, redactItem(QRect(selection.right() - 40, 420, 90, 70), RedactMode::Blur, 10, rng.next()));
        addItem(doc, redactItem(QRect(500, selection.top() - 25, 120, 50), RedactMode::Mosaic, 9, rng.next()));
        addItem(doc, redactItem(QRect(700, selection.bottom() - 20, 100, 60), RedactMode::Smooth, 16, rng.next()));
        addItem(doc, redactItem(QRect(selection.left() + 1, 600, 60, 40), RedactMode::Mosaic, 7, rng.next()));
        addItem(doc, redactItem(QRect(selection.left() - 61, 200, 60, 40), RedactMode::Solid, 7, rng.next()));
        addItem(doc, numberItem(QPointF(50.5, 50.5)));
        addItem(doc, numberItem(QPointF(400.5, 400.5)));
        Compositor overlay;
        const QImage shown = overlay.exportImage(doc, screen, selection);
        QCOMPARE(shown.size(), selection.size());
        const Project project = projectFromOverlay(doc, screen, selection);
        QCOMPARE(project.doc.canvas, selection.size());
        QCOMPARE(project.doc.crop, QRect(QPoint(0, 0), selection.size()));
        QCOMPARE(project.base.size(), selection.size());
        QString error;
        const std::optional<Project> reopened = readProject(writeProject(project.doc, project.base), &error);
        QVERIFY2(reopened, qPrintable(error));
        QCOMPARE(reopened->doc, project.doc);
        Compositor editor;
        const QImage again = editor.exportImage(reopened->doc, reopened->base, QRect(QPoint(0, 0), reopened->doc.canvas));
        const Diff d = compare(again, shown);
        QVERIFY2(d.count == 0, qPrintable(describe(d) + firstAt(d, reopened->doc, QPoint())));
        // The numbers show what they showed; what lay wholly outside is gone.
        const QHash<quint32, int> before = numbers(doc);
        const QHash<quint32, int> after = numbers(reopened->doc);
        for (const Item& item : reopened->doc.items) {
            if (item.kind == Kind::Number)
                QCOMPARE(after.value(item.id), before.value(item.id));
        }
        for (const Item& item : doc.items)
            QCOMPARE(findItem(reopened->doc, item.id) != nullptr, bounds(doc, item).intersects(QRectF(selection)));
    }

    void overlayNumbers()
    {
        const QImage screen = plainImage(QSize(800, 600), kWhite);
        Document doc = emptyDoc(screen.size());
        const QRect selection(200, 100, 400, 300);
        const quint32 out1 = addItem(doc, numberItem(QPointF(50.5, 50.5)));
        const quint32 in2 = addItem(doc, numberItem(QPointF(300.5, 200.5)));
        const quint32 in3 = addItem(doc, numberItem(QPointF(400.5, 200.5)));
        const quint32 out4 = addItem(doc, numberItem(QPointF(700.5, 500.5)));
        const Project project = projectFromOverlay(doc, screen, selection);
        QVERIFY(!findItem(project.doc, out1));
        QVERIFY(!findItem(project.doc, out4));
        // The first kept number would now count from 1: it keeps its 2 by hand, the next counts on.
        QCOMPARE(findItem(project.doc, in2)->pinned.value_or(0), 2);
        QVERIFY(!findItem(project.doc, in3)->pinned);
        QCOMPARE(numbers(project.doc).value(in3), 3);
        QCOMPARE(findItem(project.doc, in2)->points[0], QPointF(100.5, 100.5));
        // A selection partly off the screen keeps what the screen has.
        const Project edge = projectFromOverlay(doc, screen, QRect(700, 500, 300, 300));
        QCOMPARE(edge.doc.canvas, QSize(100, 100));
        QCOMPARE(edge.base.size(), QSize(100, 100));
        QVERIFY(projectFromOverlay(doc, screen, QRect(900, 900, 10, 10)).base.isNull());
    }

    // --- remembered styles ------------------------------------------------------

    void stylesScales_data()
    {
        QTest::addColumn<qreal>("dpr");
        for (qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0})
            QTest::addRow("%d%%", int(dpr * 100)) << dpr;
    }

    void stylesScales()
    {
        QFETCH(qreal, dpr);
        // Every logical size comes back from its image size at this scale.
        for (int logical = 1; logical <= 500; ++logical)
            QCOMPARE(toLogicalPx(toImagePx(logical, dpr), dpr), logical);
        QCOMPARE(toImagePx(0, dpr), 1);
        if (dpr == 1.5) {
            // 3.4: 2 / 4 / 7 at 150 % are 3 / 6 / 11.
            QCOMPARE(toImagePx(2, dpr), 3);
            QCOMPARE(toImagePx(4, dpr), 6);
            QCOMPARE(toImagePx(7, dpr), 11);
        }
        // Each tool's style at this scale, remembered again, changes nothing.
        StyleMemory memory = StyleMemory::fromJson(QString::fromUtf8(jsonText(
            "{'version':1,'rect':{'color':'#1E88E5','width':7,'fill':'soft'},'text':{'size':24,'bold':true,'boxed':true},'redact':{'mode':'blur','cell':18}}")));
        const StyleMemory copy = memory;
        for (const ToolInfo& tool : tools())
            memory.remember(tool.kind, memory.forTool(tool.kind, dpr), dpr);
        QVERIFY(memory == copy);
        const Style rect = memory.forTool(Kind::Rect, dpr);
        QCOMPARE(rect.width, toImagePx(7, dpr));
        QCOMPARE(rect.color, kBlue);
        QCOMPARE(rect.fill, Fill::Soft);
        QCOMPARE(rect.fontPx, Style{}.fontPx); // not a rect's: Style's default, so doc.json leaves it out
        const Style text = memory.forTool(Kind::Text, dpr);
        QCOMPARE(text.fontPx, toImagePx(24, dpr));
        QVERIFY(text.bold && text.boxed);
        QCOMPARE(text.width, Style{}.width);
        const Style redact = memory.forTool(Kind::Redact, dpr);
        QCOMPARE(redact.redact, RedactMode::Blur);
        QCOMPARE(redact.cell, toImagePx(18, dpr));
        QCOMPARE(redact.color, kDark);
        QCOMPARE(memory.forTool(Kind::Marker, dpr).width, toImagePx(20, dpr));
    }

    void stylesJson()
    {
        const StyleMemory defaults;
        // Nothing changed: nothing but the version is written.
        QCOMPARE(defaults.toJson(), QString::fromUtf8(jsonText("{'version':1}")));
        QVERIFY(StyleMemory::fromJson(defaults.toJson()) == defaults);
        QCOMPARE(defaults.tool(Kind::Rect).color, kRed);
        QCOMPARE(defaults.tool(Kind::Rect).width, 4);
        QCOMPARE(defaults.tool(Kind::Marker).color, kYellow);
        QCOMPARE(defaults.tool(Kind::Marker).width, 20);
        QCOMPARE(defaults.tool(Kind::Redact).color, kDark);
        QCOMPARE(defaults.tool(Kind::Redact).mode, RedactMode::Mosaic);
        StyleMemory memory;
        Style s = memory.forTool(Kind::Arrow, 1.0);
        s.color = kBlue;
        s.width = 9;
        memory.remember(Kind::Arrow, s, 1.0);
        s = memory.forTool(Kind::Text, 1.0);
        s.fontPx = 32;
        s.bold = true;
        memory.remember(Kind::Text, s, 1.0);
        s = memory.forTool(Kind::Redact, 1.0);
        s.redact = RedactMode::Solid;
        s.cell = 8;
        s.color = kPurple;
        memory.remember(Kind::Redact, s, 1.0);
        const QString json = memory.toJson();
        QVERIFY(StyleMemory::fromJson(json) == memory);
        // Only the tools changed are written.
        QCOMPARE(QJsonDocument::fromJson(json.toUtf8()).object().keys(), (QStringList{u"arrow"_s, u"redact"_s, u"text"_s, u"version"_s}));
        // A tool set back to its defaults is not written again.
        memory.remember(Kind::Arrow, defaults.forTool(Kind::Arrow, 1.0), 1.0);
        QVERIFY(!QJsonDocument::fromJson(memory.toJson().toUtf8()).object().contains(u"arrow"_s));
        // Unknown tools and fields, bad values, a newer version: ignored.
        const StyleMemory odd = StyleMemory::fromJson(QString::fromUtf8(jsonText(
            "{'version':1,'circle':{'color':'#00FF00'},'rect':{'width':0,'color':'red','glow':3,'fill':'plaid'},'pen':{'width':12.5},'marker':{'width':28}}")));
        QVERIFY(odd.tool(Kind::Rect) == StyleMemory::defaults(Kind::Rect));
        QCOMPARE(odd.tool(Kind::Pen).width, StyleMemory::defaults(Kind::Pen).width);
        QCOMPARE(odd.tool(Kind::Marker).width, 28);
        QVERIFY(StyleMemory::fromJson(QString::fromUtf8(jsonText("{'version':2,'marker':{'width':28}}"))) == defaults);
        QVERIFY(StyleMemory::fromJson(u"not json"_s) == defaults);
    }

    // ---- stage 5: brush and smooth redactions ----

    static Item brushItem(const QList<QPointF>& points, int width, RedactMode mode, int cell, quint64 seed)
    {
        Item item;
        item.kind = Kind::Redact;
        item.points = points;
        item.style.shape = RedactShape::Brush;
        item.style.width = width;
        item.style.redact = mode;
        item.style.cell = cell;
        item.style.color = kDark;
        item.seed = seed;
        return item;
    }

    // The stroke's pixels are those whose centre is within half the width of
    // the line; nothing else of its box is touched.
    void brushCoverageIsThePixelsNearTheLine()
    {
        const Item item = brushItem({QPointF(20.5, 20.5), QPointF(80.5, 50.5)}, 10, RedactMode::Solid, 8, 1);
        const QRect box = itemBox(item);
        QCOMPARE(box, QRect(QPoint(15, 15), QPoint(85, 55)));
        const QImage mask = brushCoverage(item);
        QCOMPARE(mask.size(), box.size());
        int covered = 0;
        for (int y = 0; y < mask.height(); ++y) {
            for (int x = 0; x < mask.width(); ++x) {
                const QPointF c(box.x() + x + 0.5, box.y() + y + 0.5);
                const QPointF a = item.points[0], b = item.points[1];
                const QPointF ab = b - a;
                const qreal t = std::clamp(QPointF::dotProduct(c - a, ab) / QPointF::dotProduct(ab, ab), 0.0, 1.0);
                const QPointF q = a + ab * t;
                const bool inside = std::hypot(c.x() - q.x(), c.y() - q.y()) <= 5.0;
                QCOMPARE(mask.constScanLine(y)[x] == 255, inside);
                covered += inside;
            }
        }
        QVERIFY(covered > 400 && covered < 1000);
        // A diagonal stroke leaves the corners of its box readable.
        QCOMPARE(mask.constScanLine(0)[mask.width() - 1], uchar(0));
        // A single point is a disc.
        const QImage dot = brushCoverage(brushItem({QPointF(30.5, 30.5)}, 11, RedactMode::Solid, 8, 1));
        QCOMPARE(dot.width(), 13);
        QCOMPARE(dot.constScanLine(6)[6], uchar(255));
        QCOMPARE(dot.constScanLine(0)[0], uchar(0));
        // Box redactions have none.
        QVERIFY(brushCoverage(redactItem(QRect(1, 1, 5, 5), RedactMode::Mosaic, 4, 1)).isNull());
    }

    void brushReadsNothingUnder_data()
    {
        QTest::addColumn<int>("mode");
        for (RedactMode m : {RedactMode::Mosaic, RedactMode::Blur, RedactMode::Solid, RedactMode::Smooth})
            QTest::addRow("%s", qPrintable(redactModeName(m))) << int(m);
    }

    // The same promise as the box: whatever is under the stroke, the picture
    // is the same bit for bit; the rest of the picture, its box included, stays as it was.
    void brushReadsNothingUnder()
    {
        QFETCH(int, mode);
        const QSize size(420, 300);
        const QImage base = noiseImage(size, 11);
        const QRect all(QPoint(0, 0), size);
        Document doc = emptyDoc(size);
        addItem(doc, brushItem({QPointF(40.5, 40.5), QPointF(200.5, 140.5), QPointF(330.5, 60.5)}, 24, RedactMode(mode), 12, 0x77));
        addItem(doc, brushItem({QPointF(60.5, 250.5), QPointF(380.5, 270.5)}, 16, RedactMode(mode), 8, 0x78));
        addItem(doc, redactItem(QRect(300, 150, 80, 60), RedactMode(mode), 9, 0x79));
        const QImage before = Compositor().exportImage(doc, base, all);
        const QImage covered = coverageMask(doc, all);
        QImage changed = base;
        Rng rng(5);
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                if (covered.constScanLine(y)[x])
                    changed.setPixel(x, y, 0xff000000u | QRgb(rng.next() & 0xffffffu));
            }
        }
        const Diff d = compare(Compositor().exportImage(doc, changed, all), before);
        QVERIFY2(d.count == 0, qPrintable(describe(d)));
        // Outside the strokes and the box the picture is the base.
        int redrawn = 0;
        int changedOutside = 0;
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                if (covered.constScanLine(y)[x])
                    redrawn += before.pixel(x, y) != base.pixel(x, y);
                else
                    changedOutside += before.pixel(x, y) != base.pixel(x, y);
            }
        }
        QCOMPARE(changedOutside, 0);
        QVERIFY(redrawn > 1000);
        // What is next to the stroke is read: a change there shows.
        if (RedactMode(mode) != RedactMode::Solid) {
            QImage next = base;
            for (int y = 0; y < size.height(); ++y) {
                for (int x = 0; x < size.width(); ++x) {
                    if (!covered.constScanLine(y)[x])
                        next.setPixel(x, y, base.pixel(x, y) ^ 0x00ffffffu);
                }
            }
            QVERIFY(compare(Compositor().exportImage(doc, next, all), before).count > 0);
        }
    }

    // 抹平: white text on a plain dark ground goes, every pixel of the area
    // within 3 of the ground, by box and by brush.
    void smoothRubsOut_data()
    {
        QTest::addColumn<bool>("brush");
        QTest::addColumn<QRgb>("ground");
        QTest::addRow("box on black") << false << QRgb(0xff000000u);
        QTest::addRow("brush on black") << true << QRgb(0xff000000u);
        QTest::addRow("box on grey") << false << QRgb(0xff303030u);
        QTest::addRow("brush on blue") << true << QRgb(0xff102040u);
    }

    void smoothRubsOut()
    {
        QFETCH(bool, brush);
        QFETCH(QRgb, ground);
        const QSize size(360, 120);
        QImage base(size, QImage::Format_RGB32);
        base.fill(ground);
        {
            QPainter painter(&base);
            QFont font(u"Microsoft YaHei UI"_s);
            font.setPixelSize(22);
            painter.setFont(font);
            painter.setPen(Qt::white);
            painter.drawText(30, 62, u"Hello 13812345678 电话"_s);
        }
        const QRect all(QPoint(0, 0), size);
        Document doc = emptyDoc(size);
        const Item item = brush ? brushItem({QPointF(18.5, 52.5), QPointF(330.5, 52.5)}, 44, RedactMode::Smooth, 12, 3)
                                : redactItem(QRect(18, 30, 312, 44), RedactMode::Smooth, 12, 3);
        addItem(doc, item);
        const QImage out = Compositor().exportImage(doc, base, all);
        const QRect box = itemBox(doc.items[0]);
        const QImage stroke = coverageMask(doc, all);
        int worst = 0;
        int inside = 0;
        int white = 0;
        for (int y = box.top(); y <= box.bottom(); ++y) {
            for (int x = box.left(); x <= box.right(); ++x) {
                if (!stroke.constScanLine(y)[x])
                    continue;
                ++inside;
                white += qGray(base.pixel(x, y)) > 200;
                const QRgb p = out.pixel(x, y);
                worst = std::max({worst, std::abs(qRed(p) - qRed(ground)), std::abs(qGreen(p) - qGreen(ground)),
                    std::abs(qBlue(p) - qBlue(ground))});
            }
        }
        QVERIFY(inside > 5000);
        QVERIFY(white > 300); // the text really was there
        QVERIFY2(worst <= 3, qPrintable(u"worst difference %1"_s.arg(worst)));
    }

    // A long brush stroke across a big screenshot: how long a redraw takes. The
    // number is logged; the test only fails when it is absurd.
    void brushStrokeSpeed_data()
    {
        QTest::addColumn<int>("mode");
        for (RedactMode m : {RedactMode::Mosaic, RedactMode::Blur, RedactMode::Smooth})
            QTest::addRow("%s", qPrintable(redactModeName(m))) << int(m);
    }

    void brushStrokeSpeed()
    {
        QFETCH(int, mode);
        const QSize size(2560, 1600);
        const QImage base = noiseImage(size, 21);
        const QRect all(QPoint(0, 0), size);
        Document doc = emptyDoc(size, 1.5);
        QList<QPointF> zigzag;
        for (int i = 0; i < 40; ++i)
            zigzag.append(QPointF(80.5 + i * 60, i % 2 ? 1400.5 : 160.5)); // across the whole picture
        addItem(doc, brushItem(zigzag, 36, RedactMode(mode), 18, 5));
        QElapsedTimer clock;
        clock.start();
        Compositor compositor;
        const QImage out = compositor.exportImage(doc, base, all);
        const qint64 first = clock.elapsed();
        QCOMPARE(out.size(), size);
        // The same stroke one point longer: the patch is made again (what every mouse move does).
        zigzag.append(QPointF(2500.5, 800.5));
        doc.items[0].points = zigzag;
        compositor.invalidate(Change{.ids = {doc.items[0].id}, .redactions = true});
        clock.restart();
        compositor.exportImage(doc, base, all);
        qInfo().noquote() << u"brush stroke %1 over %2x%3: %4 ms, longer by a point %5 ms (box %6x%7)"_s
                                 .arg(redactModeName(RedactMode(mode)))
                                 .arg(size.width())
                                 .arg(size.height())
                                 .arg(first)
                                 .arg(clock.elapsed())
                                 .arg(itemBox(doc.items[0]).width())
                                 .arg(itemBox(doc.items[0]).height());
        QVERIFY(first < 10000 && clock.elapsed() < 10000); // sanity only: a stroke across a whole 2560 px screen
    }

    // A brush stroke being drawn is shown as a mosaic whatever its mode; once let go (no draft) it is the real
    // kind, the same pixels as a compositor that never saw the draft, and the export never has a draft.
    void brushDraftIsCheapAndTheFinalIsUnchanged_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addRow("blur") << int(RedactMode::Blur);
        QTest::addRow("smooth") << int(RedactMode::Smooth);
        QTest::addRow("solid") << int(RedactMode::Solid);
    }

    void brushDraftIsCheapAndTheFinalIsUnchanged()
    {
        QFETCH(int, mode);
        const QSize size(300, 200);
        const QImage base = noiseImage(size, 31);
        const QRect all(QPoint(0, 0), size);
        Document doc = emptyDoc(size);
        const quint32 id = addItem(doc, brushItem({QPointF(30.5, 40.5), QPointF(150.5, 120.5), QPointF(260.5, 60.5)}, 24, RedactMode(mode), 10, 9));
        Document asMosaic = doc;
        asMosaic.items[0].style.redact = RedactMode::Mosaic;
        Compositor live;
        QImage draft;
        live.render(doc, base, all, all, &draft, std::nullopt, std::nullopt, id);
        QImage mosaic;
        Compositor().render(asMosaic, base, all, all, &mosaic);
        if (RedactMode(mode) == RedactMode::Solid)
            QCOMPARE(compare(draft, Compositor().exportImage(doc, base, all)).count, 0); // solid is cheap already
        else
            QCOMPARE(compare(draft, mosaic).count, 0);
        // Let go: the same compositor now gives what a fresh one does, and so does the export.
        QImage final;
        live.render(doc, base, all, all, &final);
        const QImage fresh = Compositor().exportImage(doc, base, all);
        QCOMPARE(compare(final, fresh).count, 0);
        QCOMPARE(compare(live.exportImage(doc, base, all), fresh).count, 0);
        if (RedactMode(mode) != RedactMode::Solid)
            QVERIFY(compare(final, draft).count > 0);
    }

    // The new fields go to doc.json and back, through the project and the
    // remembered styles; older ones load as boxes.
    void brushAndSmoothRoundTrip()
    {
        Document doc = emptyDoc(QSize(300, 200), 1.5);
        addItem(doc, brushItem({QPointF(10.5, 10.5), QPointF(90.5, 40.5), QPointF(120.5, 90.5)}, 36, RedactMode::Smooth, 12, 0xABCDEF));
        addItem(doc, redactItem(QRect(5, 100, 50, 30), RedactMode::Smooth, 9, 7));
        addItem(doc, brushItem({QPointF(200.5, 150.5)}, 16, RedactMode::Blur, 18, 9));
        const QByteArray text = QJsonDocument(toJson(doc)).toJson(QJsonDocument::Compact);
        QString error;
        const std::optional<Document> back = fromJson(QJsonDocument::fromJson(text).object(), &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(*back, doc);
        QCOMPARE(back->items[0].style.shape, RedactShape::Brush);
        QCOMPARE(back->items[0].style.redact, RedactMode::Smooth);
        QCOMPARE(back->items[0].style.width, 36);
        QCOMPARE(back->items[1].style.shape, RedactShape::Box);
        const QImage base = noiseImage(doc.canvas, 4);
        const std::optional<Project> project = readProject(writeProject(doc, base), &error);
        QVERIFY2(project, qPrintable(error));
        QCOMPARE(project->doc, doc);
        // The remembered styles.
        StyleMemory memory;
        Style s = memory.forTool(Kind::Redact, 1.5);
        QCOMPARE(s.shape, RedactShape::Box);
        s.shape = RedactShape::Brush;
        s.redact = RedactMode::Smooth;
        memory.remember(Kind::Redact, s, 1.5);
        QCOMPARE(memory.tool(Kind::Redact).shape, RedactShape::Brush);
        QCOMPARE(memory.tool(Kind::Redact).width, 24);
        QCOMPARE(memory.forTool(Kind::Redact, 1.5).width, toImagePx(24, 1.5));
        QVERIFY(StyleMemory::fromJson(memory.toJson()) == memory);
        // A box's own width does not change the brush's remembered one.
        Style box = memory.forTool(Kind::Redact, 1.0);
        box.shape = RedactShape::Box;
        box.width = 6;
        StyleMemory other = memory;
        other.remember(Kind::Redact, box, 1.0);
        QCOMPARE(other.tool(Kind::Redact).width, 24);
        // Older remembered styles (no shape, no width) load as boxes.
        const StyleMemory old = StyleMemory::fromJson(QString::fromUtf8(jsonText("{'version':1,'redact':{'mode':'blur','cell':18}}")));
        QCOMPARE(old.tool(Kind::Redact).shape, RedactShape::Box);
        QCOMPARE(old.tool(Kind::Redact).width, 24);
    }

    // Smart redaction adds many boxes as one step: one Ctrl+Z takes them all.
    void manyRedactionsAreOneStep()
    {
        Document doc = emptyDoc(QSize(300, 200));
        CommandStack stack;
        QList<Item> items;
        for (int i = 0; i < 7; ++i)
            items.append(redactItem(QRect(10 + i * 30, 20, 25, 12), RedactMode::Mosaic, 6, quint64(i + 1)));
        const Change change = stack.push(std::make_unique<AddItems>(items), doc, 1000);
        QVERIFY(change.redactions);
        QCOMPARE(doc.items.size(), qsizetype(7));
        QCOMPARE(stack.count(), 1);
        stack.undo(doc);
        QCOMPARE(doc.items.size(), qsizetype(0));
        stack.redo(doc);
        QCOMPARE(doc.items.size(), qsizetype(7));
    }

    // --- tool table -------------------------------------------------------------

    // ---- the editing window (stage 4b) ----

    void fitAndZoom()
    {
        using namespace ws::capture::editview;
        // Smaller than the window: as it is. Larger: the whole of it.
        QCOMPARE(fitZoom(QSize(400, 300), QSize(1200, 800)), 1.0);
        QCOMPARE(fitZoom(QSize(2400, 1600), QSize(1200, 800)), 0.5);
        // A long picture is fitted by its width, not shown as a sliver.
        QCOMPARE(fitZoom(QSize(1080, 30000), QSize(1200, 800)), 1.0);
        QCOMPARE(fitZoom(QSize(4000, 30000), QSize(1200, 800)), 0.3);
        QCOMPARE(fitZoom(QSize(), QSize(1200, 800)), 1.0);
        QCOMPARE(clampZoom(0.01), kMinZoom);
        QCOMPARE(clampZoom(99), kMaxZoom);
        QCOMPARE(shownSize(QSize(1001, 333), 1.0), QSize(1001, 333));
        QCOMPARE(shownSize(QSize(1000, 300), 0.5), QSize(500, 150));
        // The picture sits in the middle when it fits, else it cannot leave a gap.
        QCOMPARE(clampOrigin(QPoint(5, 7), QSize(400, 300), QSize(1000, 800)), QPoint(300, 250));
        QCOMPARE(clampOrigin(QPoint(50, -5000), QSize(2000, 3000), QSize(1000, 800)), QPoint(0, -2200));
        QCOMPARE(clampOrigin(QPoint(-5000, -100), QSize(2000, 3000), QSize(1000, 800)), QPoint(-1000, -100));
        // The pixel under the mouse, and what shows.
        QCOMPARE(imagePixelAt(QPoint(300, 250), QPoint(300, 250), 1.0), QPoint(0, 0));
        QCOMPARE(imagePixelAt(QPoint(299, 249), QPoint(300, 250), 1.0), QPoint(-1, -1));
        QCOMPARE(imagePixelAt(QPoint(110, 60), QPoint(10, 10), 0.5), QPoint(200, 100));
        QCOMPARE(imagePixelAt(QPoint(110, 60), QPoint(10, 10), 4.0), QPoint(25, 12));
        QCOMPARE(visibleImage(QPoint(-100, -200), 1.0, QSize(1000, 800)), QRectF(100, 200, 1000, 800));
        QCOMPARE(visibleImage(QPoint(0, 0), 0.5, QSize(1000, 800)), QRectF(0, 0, 2000, 1600));
        // The same zoom steps as a pin's, 100 % among them, so Z can come back to it exactly.
        const std::vector<double>& steps = ws::capture::zoomSteps();
        QVERIFY(std::find(steps.begin(), steps.end(), 1.0) != steps.end());
    }

    void tileRanges()
    {
        using namespace ws::capture::editview;
        const QSize image(1080, 30000); // 5 columns x 118 rows of 256
        // The top of it at 100 %: 4 rows seen (800 / 256 -> rows 0-3) and one more.
        QRect r = tileRange(visibleImage(QPoint(0, 0), 1.0, QSize(1080, 800)), image, 256, 1);
        QCOMPARE(r, QRect(QPoint(0, 0), QPoint(4, 4)));
        // Scrolled to the middle: only the rows it shows, and a ring.
        r = tileRange(visibleImage(QPoint(0, -15000), 1.0, QSize(1080, 800)), image, 256, 1);
        QCOMPARE(r.left(), 0);
        QCOMPARE(r.right(), 4);
        QCOMPARE(r.top(), 15000 / 256 - 1);
        QCOMPARE(r.bottom(), (15000 + 800 - 1) / 256 + 1);
        QVERIFY(r.height() <= 800 / 256 + 2 + 2);
        // A view ending exactly on a tile's edge needs no tile beyond it but the ring.
        r = tileRange(QRectF(0, 0, 512, 256), QSize(2048, 2048), 256, 0);
        QCOMPARE(r, QRect(QPoint(0, 0), QPoint(1, 0)));
        // Out of the picture: nothing. Cut short at the edges.
        QVERIFY(tileRange(QRectF(-500, -500, 100, 100), image, 256, 1).isEmpty());
        QCOMPARE(tileRange(QRectF(-100, -100, 99999, 99999), QSize(300, 300), 256, 3), QRect(QPoint(0, 0), QPoint(1, 1)));
        // Nearest the middle first, at most `limit`.
        const QList<int> near = nearestTiles(QRect(QPoint(0, 0), QPoint(4, 4)), 5, 256, QPointF(640, 640), 6);
        QCOMPARE(near.size(), 6);
        QCOMPARE(near.first(), 2 * 5 + 2);
        QCOMPARE(nearestTiles(QRect(QPoint(0, 0), QPoint(4, 4)), 5, 256, QPointF(0, 0), 100).size(), 25);
        QVERIFY(nearestTiles({}, 5, 256, QPointF(), 4).isEmpty());
        QCOMPARE(deviceLength(100, 1.5), 150);
        QCOMPARE(deviceLength(33.3, 1.0), 33);
    }

    void replaceDocumentCommand()
    {
        Document before;
        before.canvas = QSize(300, 200);
        before.crop = QRect(QPoint(0, 0), before.canvas);
        before.dpr = 1.5;
        Document doc = before;
        CommandStack stack;
        Item rect;
        rect.id = doc.nextId;
        rect.seq = doc.nextSeq;
        rect.kind = Kind::Rect;
        rect.points = boxPoints(QRect(10, 10, 80, 50));
        stack.push(std::make_unique<AddItems>(QList<Item>{rect}), doc, 0);
        stack.push(std::make_unique<SetCrop>(doc, QRect(5, 5, 200, 100)), doc, 5000);
        QVERIFY(doc != before);
        // Everything given up in one step, everything redrawn; one undo takes it back.
        const Change gone = stack.push(std::make_unique<ReplaceDocument>(doc, before), doc, 10000);
        QCOMPARE(doc, before);
        QVERIFY(gone.crop && gone.numbers && gone.redactions);
        QVERIFY(gone.dirty.contains(QRectF(10, 10, 80, 50)));
        QVERIFY(gone.ids.contains(rect.id));
        stack.undo(doc);
        QCOMPARE(doc.items.size(), qsizetype(1));
        QCOMPARE(doc.crop, QRect(5, 5, 200, 100));
        stack.redo(doc);
        QCOMPARE(doc, before);
        // And back through the earlier steps to the start: the same document.
        while (stack.canUndo())
            stack.undo(doc);
        QCOMPARE(doc, before);
    }

    void styleReset()
    {
        StyleMemory memory;
        memory.remember(Kind::Arrow, [] { Style s; s.color = qRgb(0x1E, 0x88, 0xE5); s.width = 11; return s; }(), 1.5);
        QVERIFY(memory.tool(Kind::Arrow) != StyleMemory::defaults(Kind::Arrow));
        memory.reset(Kind::Arrow);
        QCOMPARE(memory.tool(Kind::Arrow), StyleMemory::defaults(Kind::Arrow));
        QVERIFY(memory == StyleMemory());
    }

    void toolsTable()
    {
        const QList<ToolInfo>& list = tools();
        QCOMPARE(list.size(), qsizetype(kKindCount));
        QString letters;
        QList<int> digits;
        QSet<int> kinds;
        for (const ToolInfo& tool : list) {
            letters.append(tool.letter);
            digits.append(tool.digit);
            kinds.insert(int(tool.kind));
            QVERIFY(!tool.name.isEmpty());
            QVERIFY(!toolName(tool.kind).isEmpty());
            QVERIFY(tool.letter.isUpper());
        }
        QCOMPARE(letters, u"REALPMTNB"_s);
        // Plain digits 1 to 9, each once: Ctrl+digit belongs to PixPin.
        QCOMPARE(digits, (QList<int>{1, 2, 3, 4, 5, 6, 7, 8, 9}));
        QCOMPARE(kinds.size(), qsizetype(kKindCount));
        for (QChar letter : letters)
            QCOMPARE(letters.count(letter), qsizetype(1));
        // C copies a colour, H hides the toolbar, I is the eyedropper, Z is undo and zoom.
        for (char16_t taken : {u'C', u'H', u'I', u'Z'})
            QVERIFY(!letters.contains(QChar(taken)));
        QCOMPARE(palette().size(), qsizetype(8));
        for (QRgb c : {kRed, kYellow, kDark, kWhite})
            QVERIFY(palette().contains(c));
        for (const ToolInfo& tool : list) {
            const QList<int> sizes = sizePresets(tool.kind);
            QVERIFY(!sizes.isEmpty());
            QVERIFY(std::is_sorted(sizes.cbegin(), sizes.cend()));
        }
        QCOMPARE(sizePresets(Kind::Rect), (QList<int>{2, 4, 7}));
    }
};

int main(int argc, char** argv)
{
    // As the app does (main.cpp): FreeType draws the glyphs unless asked
    // otherwise, so the tests see the pixels users see. Set QT_QPA_PLATFORM
    // to windows:fontengine=gdi to test the other engine.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "windows:fontengine=freetype");
    QGuiApplication app(argc, argv);
    TestAnnotate test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_annotate.moc"

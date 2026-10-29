#include "AnnotationFormat.h"

#include "Geometry.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

namespace ws::capture::annot {

namespace {

constexpr QStringView kFormat = u"winshun-annotation";
// Far beyond any real project; a damaged or hostile file stops here.
constexpr quint64 kMaxJsonBytes = 64ull << 20;
constexpr quint64 kMaxPngBytes = 1ull << 30;
constexpr int kMaxSide = 1 << 20;
constexpr double kMaxCoordinate = double(1 << 24);
constexpr int kMaxStyleValue = 10000;

QString brokenText()
{
    return QCoreApplication::translate("Annotation", "标注文件已损坏。");
}

QJsonArray pointJson(QPointF p)
{
    return QJsonArray{p.x(), p.y()};
}

QString seedText(quint64 seed)
{
    return u"%1"_s.arg(seed, 16, 16, QChar(u'0'));
}

// A whole number in [low, high].
std::optional<qint64> wholeFrom(const QJsonValue& value, qint64 low, qint64 high)
{
    if (!value.isDouble())
        return std::nullopt;
    const double d = value.toDouble();
    if (!std::isfinite(d) || d != std::floor(d) || d < double(low) || d > double(high))
        return std::nullopt;
    return qint64(d);
}

QJsonObject styleJson(Kind kind, const Style& s)
{
    const Style defaults;
    const bool redact = kind == Kind::Redact;
    const bool words = kind == Kind::Text || kind == Kind::Number;
    const bool strokes = !redact && !words;
    const bool box = kind == Kind::Rect || kind == Kind::Ellipse;
    QJsonObject o;
    if (isLine(kind) && s.lineMode != LineMode::Straight)
        o[u"lineMode"_s] = lineModeName(s.lineMode);
    if (kind == Kind::Marker && s.markerMode != MarkerMode::Freehand)
        o[u"markerMode"_s] = markerModeName(s.markerMode);
    if (!redact || s.redact == RedactMode::Solid || s.color != defaults.color)
        o[u"color"_s] = colorHex(s.color);
    if (strokes || s.width != defaults.width)
        o[u"width"_s] = s.width;
    if (box || s.fill != defaults.fill)
        o[u"fill"_s] = fillName(s.fill);
    if (words || s.fontPx != defaults.fontPx)
        o[u"fontPx"_s] = s.fontPx;
    if (kind == Kind::Text || s.bold != defaults.bold)
        o[u"bold"_s] = s.bold;
    if (kind == Kind::Text || s.boxed != defaults.boxed)
        o[u"boxed"_s] = s.boxed;
    if (s.textStroke != 0)
        o[u"textStroke"_s] = s.textStroke;
    if (s.textStrokeColor != 0)
        o[u"textStrokeColor"_s] = colorHex(s.textStrokeColor);
    if (redact || s.redact != defaults.redact)
        o[u"redact"_s] = redactModeName(s.redact);
    if (redact || s.shape != defaults.shape)
        o[u"shape"_s] = redactShapeName(s.shape);
    if (redact || s.cell != defaults.cell)
        o[u"cell"_s] = s.cell;
    return o;
}

// Missing or unusable fields keep Style's defaults.
Style styleFrom(const QJsonObject& o)
{
    Style s;
    s.lineMode = lineModeFromName(o.value(u"lineMode"_s).toString()).value_or(LineMode::Straight);
    s.markerMode = markerModeFromName(o.value(u"markerMode"_s).toString()).value_or(MarkerMode::Freehand);
    if (const auto color = parseColor(o.value(u"color"_s).toString()))
        s.color = *color;
    if (const auto width = wholeFrom(o.value(u"width"_s), 1, kMaxStyleValue))
        s.width = int(*width);
    if (const auto fill = fillFromName(o.value(u"fill"_s).toString()))
        s.fill = *fill;
    if (const auto fontPx = wholeFrom(o.value(u"fontPx"_s), 1, kMaxStyleValue))
        s.fontPx = int(*fontPx);
    if (o.value(u"bold"_s).isBool())
        s.bold = o.value(u"bold"_s).toBool();
    if (o.value(u"boxed"_s).isBool())
        s.boxed = o.value(u"boxed"_s).toBool();
    // Added later: a file without them has no outline.
    if (const auto stroke = wholeFrom(o.value(u"textStroke"_s), 0, kMaxStyleValue))
        s.textStroke = int(std::min<qint64>(*stroke, kMaxTextStroke)); // a crafted file cannot ask for a huge ring
    if (const auto color = parseColor(o.value(u"textStrokeColor"_s).toString()))
        s.textStrokeColor = *color | 0xff000000u;
    if (const auto mode = redactModeFromName(o.value(u"redact"_s).toString()))
        s.redact = *mode;
    if (const auto shape = redactShapeFromName(o.value(u"shape"_s).toString()))
        s.shape = *shape;
    if (const auto cell = wholeFrom(o.value(u"cell"_s), 1, kMaxStyleValue))
        s.cell = int(*cell);
    return s;
}

QJsonObject seriesJson(const Series& s)
{
    QJsonObject o{{u"id"_s, qint64(s.id)}, {u"start"_s, s.start}};
    if (s.format != NumberFormat::Arabic)
        o[u"format"_s] = numberFormatName(s.format);
    return o;
}

QJsonObject itemJson(const Item& item)
{
    QJsonObject o;
    o[u"id"_s] = qint64(item.id);
    o[u"kind"_s] = kindName(item.kind);
    o[u"seq"_s] = qint64(item.seq);
    QJsonArray points;
    for (const QPointF& p : item.points)
        points.append(pointJson(p));
    o[u"points"_s] = points;
    o[u"style"_s] = styleJson(item.kind, item.style);
    if (item.closed)
        o[u"closed"_s] = true;
    if (item.kind == Kind::Text || !item.text.isEmpty())
        o[u"text"_s] = item.text;
    if (item.kind == Kind::Number || item.series != 0)
        o[u"series"_s] = qint64(item.series);
    if (item.pinned)
        o[u"pinned"_s] = *item.pinned;
    if (item.kind == Kind::Redact || item.seed != 0)
        o[u"seed"_s] = seedText(item.seed);
    return o;
}

std::optional<Item> itemFrom(const QJsonObject& o)
{
    Item item;
    const auto id = wholeFrom(o.value(u"id"_s), 1, 0xFFFFFFFFll);
    const auto kind = kindFromName(o.value(u"kind"_s).toString());
    if (!id || !kind)
        return std::nullopt;
    item.id = quint32(*id);
    item.kind = *kind;
    item.seq = quint32(wholeFrom(o.value(u"seq"_s), 0, 0xFFFFFFFFll).value_or(0));
    const QJsonArray points = o.value(u"points"_s).toArray();
    item.points.reserve(points.size());
    for (const QJsonValue& value : points) {
        const QJsonArray p = value.toArray();
        if (p.size() != 2 || !p[0].isDouble() || !p[1].isDouble())
            return std::nullopt;
        const double x = p[0].toDouble();
        const double y = p[1].toDouble();
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > kMaxCoordinate || std::abs(y) > kMaxCoordinate)
            return std::nullopt;
        item.points.append(QPointF(x, y));
    }
    item.style = styleFrom(o.value(u"style"_s).toObject());
    item.closed = item.kind == Kind::Line && item.style.lineMode == LineMode::Polyline && o.value(u"closed"_s).toBool();
    if (isLine(item.kind) && item.style.lineMode == LineMode::Bezier && item.points.size() != 4)
        return std::nullopt;
    if (item.kind == Kind::Arrow && item.style.lineMode == LineMode::Polyline)
        return std::nullopt;
    item.text = o.value(u"text"_s).toString();
    item.series = quint32(wholeFrom(o.value(u"series"_s), 0, 0xFFFFFFFFll).value_or(0));
    if (const auto pinned = wholeFrom(o.value(u"pinned"_s), std::numeric_limits<int>::min(), std::numeric_limits<int>::max()))
        item.pinned = int(*pinned);
    bool ok = false;
    const quint64 seed = o.value(u"seed"_s).toString().toULongLong(&ok, 16);
    if (ok)
        item.seed = seed;
    const bool brush = item.kind == Kind::Redact && item.style.shape == RedactShape::Brush;
    const qsizetype needed = (isBox(item.kind) && !brush) || isLineItem(item) || isBoxItem(item) ? 2 : 1;
    if (item.points.size() < needed)
        return std::nullopt;
    return item;
}

// The steps up from older versions, one per format change (migrate1to2,
// then migrate2to3, ...), each with a unit test and a sample file in
// tests/data/annotate. Version 1 is the first, so there is none yet.
QJsonObject migrate(const QJsonObject& json, int from)
{
    static_assert(Document::kVersion == 2);
    QJsonObject result = json;
    if (from == 1)
        result[u"version"_s] = 2;
    return result;
}

std::optional<Document> parse(const QJsonObject& json, QString* error)
{
    Document doc;
    const QJsonArray canvas = json.value(u"canvas"_s).toArray();
    const auto width = canvas.size() == 2 ? wholeFrom(canvas[0], 1, kMaxSide) : std::nullopt;
    const auto height = canvas.size() == 2 ? wholeFrom(canvas[1], 1, kMaxSide) : std::nullopt;
    if (!width || !height) {
        if (error)
            *error = brokenText();
        return std::nullopt;
    }
    doc.canvas = QSize(int(*width), int(*height));
    doc.crop = QRect(QPoint(0, 0), doc.canvas);
    const QJsonArray crop = json.value(u"crop"_s).toArray();
    if (crop.size() == 4) {
        const auto x = wholeFrom(crop[0], -kMaxSide, kMaxSide);
        const auto y = wholeFrom(crop[1], -kMaxSide, kMaxSide);
        const auto w = wholeFrom(crop[2], 1, kMaxSide);
        const auto h = wholeFrom(crop[3], 1, kMaxSide);
        if (x && y && w && h)
            doc.crop = QRect(int(*x), int(*y), int(*w), int(*h));
    }
    const double dpr = json.value(u"dpr"_s).toDouble(1.0);
    doc.dpr = std::isfinite(dpr) && dpr > 0 && dpr <= 16 ? dpr : 1.0;

    doc.series.clear();
    for (const QJsonValue& value : json.value(u"series"_s).toArray()) {
        const QJsonObject o = value.toObject();
        const auto id = wholeFrom(o.value(u"id"_s), 1, 0xFFFFFFFFll);
        const auto start = wholeFrom(o.value(u"start"_s), std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
        if (!id || std::any_of(doc.series.cbegin(), doc.series.cend(), [&](const Series& s) { return s.id == quint32(*id); }))
            continue;
        // Optional, and written only when not 1 2 3: a project from before
        // the formats opens as it was, and one saved with them opens (with
        // digits) in a version that does not know the key.
        doc.series.append(Series{quint32(*id), int(start.value_or(1)),
            numberFormatFromName(o.value(u"format"_s).toString()).value_or(NumberFormat::Arabic)});
    }
    if (doc.series.isEmpty())
        doc.series.append(Series{});
    const auto current = wholeFrom(json.value(u"currentSeries"_s), 1, 0xFFFFFFFFll);
    const bool known = current
        && std::any_of(doc.series.cbegin(), doc.series.cend(), [&](const Series& s) { return s.id == quint32(*current); });
    doc.currentSeries = known ? quint32(*current) : doc.series.first().id;

    quint32 maxId = 0;
    quint32 maxSeq = 0;
    QSet<quint32> ids;
    for (const QJsonValue& value : json.value(u"items"_s).toArray()) {
        std::optional<Item> item = itemFrom(value.toObject());
        if (!item || ids.contains(item->id))
            continue;
        ids.insert(item->id);
        maxId = std::max(maxId, item->id);
        maxSeq = std::max(maxSeq, item->seq);
        doc.items.append(std::move(*item));
    }
    std::stable_partition(doc.items.begin(), doc.items.end(), [](const Item& item) { return item.kind == Kind::Redact; });
    const qint64 nextId = wholeFrom(json.value(u"nextId"_s), 1, 0xFFFFFFFFll).value_or(1);
    const qint64 nextSeq = wholeFrom(json.value(u"nextSeq"_s), 1, 0xFFFFFFFFll).value_or(1);
    doc.nextId = quint32(std::max<qint64>(nextId, qint64(maxId) + 1));
    doc.nextSeq = quint32(std::max<qint64>(nextSeq, qint64(maxSeq) + 1));
    return doc;
}

QByteArray extract(mz_zip_archive* zip, const char* name, quint64 limit)
{
    const int index = mz_zip_reader_locate_file(zip, name, nullptr, 0);
    if (index < 0)
        return {};
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(zip, mz_uint(index), &stat) || stat.m_uncomp_size > limit)
        return {};
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(zip, mz_uint(index), &size, 0);
    if (!data)
        return {};
    QByteArray out(static_cast<const char*>(data), qsizetype(size));
    mz_free(data);
    return out;
}

int startOf(const Document& doc, quint32 series)
{
    for (const Series& s : doc.series) {
        if (s.id == series)
            return s.start;
    }
    return 1;
}

// After items were left out: each Number whose count would now differ
// from what it showed is pinned to that, and the ones after it count on.
void keepNumbers(Document& doc, const QHash<quint32, int>& shown)
{
    QList<qsizetype> order;
    for (qsizetype i = 0; i < doc.items.size(); ++i) {
        if (doc.items[i].kind == Kind::Number)
            order.append(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](qsizetype a, qsizetype b) {
        const Item& l = doc.items[a];
        const Item& r = doc.items[b];
        return l.series != r.series ? l.series < r.series : l.seq < r.seq;
    });
    int next = 1;
    for (qsizetype k = 0; k < order.size(); ++k) {
        Item& item = doc.items[order[k]];
        if (k == 0 || item.series != doc.items[order[k - 1]].series)
            next = startOf(doc, item.series);
        if (item.pinned)
            next = *item.pinned;
        const int want = shown.value(item.id, next);
        if (next != want) {
            item.pinned = want;
            next = want;
        }
        ++next;
    }
}

} // namespace

QJsonObject toJson(const Document& doc)
{
    QJsonObject o;
    o[u"format"_s] = kFormat.toString();
    o[u"version"_s] = Document::kVersion;
    o[u"canvas"_s] = QJsonArray{doc.canvas.width(), doc.canvas.height()};
    o[u"crop"_s] = QJsonArray{doc.crop.x(), doc.crop.y(), doc.crop.width(), doc.crop.height()};
    o[u"dpr"_s] = doc.dpr;
    QJsonArray series;
    for (const Series& s : doc.series)
        series.append(seriesJson(s));
    o[u"series"_s] = series;
    o[u"currentSeries"_s] = qint64(doc.currentSeries);
    o[u"nextId"_s] = qint64(doc.nextId);
    o[u"nextSeq"_s] = qint64(doc.nextSeq);
    QJsonArray items;
    for (const Item& item : doc.items)
        items.append(itemJson(item));
    o[u"items"_s] = items;
    return o;
}

std::optional<Document> fromJson(const QJsonObject& json, QString* error)
{
    if (json.value(u"format"_s).toString() != kFormat) {
        if (error)
            *error = QCoreApplication::translate("Annotation", "这不是 Win顺 的标注文件。");
        return std::nullopt;
    }
    const auto version = wholeFrom(json.value(u"version"_s), 1, std::numeric_limits<int>::max());
    if (!version) {
        if (error)
            *error = brokenText();
        return std::nullopt;
    }
    if (*version > Document::kVersion) {
        if (error)
            *error = QCoreApplication::translate("Annotation", "这张图是新版 Win顺 存的，升级后才能编辑");
        return std::nullopt;
    }
    return parse(migrate(json, int(*version)), error);
}

QByteArray writeProject(const Document& doc, const QImage& base, const QByteArray& basePng)
{
    const QByteArray json = QJsonDocument(toJson(doc)).toJson(QJsonDocument::Compact);
    QByteArray png = basePng;
    if (png.isEmpty()) {
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !base.save(&buffer, "PNG"))
            return {};
    }
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, std::size_t(json.size() + png.size()) + 4096))
        return {};
    bool ok = mz_zip_writer_add_mem(&zip, "doc.json", json.constData(), std::size_t(json.size()), mz_uint(MZ_DEFAULT_LEVEL))
        && mz_zip_writer_add_mem(&zip, "base.png", png.constData(), std::size_t(png.size()), mz_uint(MZ_NO_COMPRESSION));
    void* data = nullptr;
    std::size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &data, &size);
    QByteArray out;
    if (ok && data)
        out = QByteArray(static_cast<const char*>(data), qsizetype(size));
    if (data)
        mz_free(data);
    mz_zip_writer_end(&zip);
    return out;
}

std::optional<Project> readProject(const QByteArray& data, QString* error)
{
    const auto broken = [&]() -> std::optional<Project> {
        if (error)
            *error = brokenText();
        return std::nullopt;
    };
    QByteArray json;
    QByteArray png;
    {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_mem(&zip, data.constData(), std::size_t(data.size()), 0))
            return broken();
        json = extract(&zip, "doc.json", kMaxJsonBytes);
        png = extract(&zip, "base.png", kMaxPngBytes);
        mz_zip_reader_end(&zip);
    }
    if (json.isEmpty() || png.isEmpty())
        return broken();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return broken();
    std::optional<Document> doc = fromJson(document.object(), error);
    if (!doc)
        return std::nullopt;
    QBuffer buffer(&png);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "PNG");
    // A long screenshot is far above Qt's default limit of 256 MB.
    reader.setAllocationLimit(2048);
    QImage base = reader.read();
    if (base.isNull() || base.size() != doc->canvas)
        return broken();
    if (base.format() != QImage::Format_RGB32)
        base = base.convertToFormat(QImage::Format_RGB32);
    return Project{std::move(*doc), std::move(base), std::move(png)};
}

Project projectFromOverlay(const Document& doc, const QImage& screen, const QRect& selection)
{
    Project project;
    const QRect taken = selection & screen.rect();
    if (taken.isEmpty())
        return project;
    project.base = screen.copy(taken);
    if (project.base.format() != QImage::Format_RGB32)
        project.base = project.base.convertToFormat(QImage::Format_RGB32);
    Document& out = project.doc;
    out.canvas = taken.size();
    out.crop = QRect(QPoint(0, 0), taken.size());
    out.dpr = doc.dpr;
    out.series = doc.series;
    out.currentSeries = doc.currentSeries;
    out.nextId = doc.nextId;
    out.nextSeq = doc.nextSeq;
    const QHash<quint32, int> shown = numbers(doc);
    const QRectF inside(taken);
    for (const Item& item : doc.items) {
        if (bounds(item, doc.dpr, shown.value(item.id, 1), formatOf(doc, item.series)).intersects(inside))
            out.items.append(translated(item, -taken.topLeft()));
    }
    keepNumbers(out, shown);
    return project;
}

}

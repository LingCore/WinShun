#include "AnnotationStyles.h"

#include "Geometry.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws::capture::annot {

namespace {

// The names as written in the source; lupdate finds them here.
const char* const kToolNames[kKindCount] = {
    QT_TRANSLATE_NOOP("Annotation", "矩形"),
    QT_TRANSLATE_NOOP("Annotation", "椭圆"),
    QT_TRANSLATE_NOOP("Annotation", "箭头"),
    QT_TRANSLATE_NOOP("Annotation", "直线"),
    QT_TRANSLATE_NOOP("Annotation", "画笔"),
    QT_TRANSLATE_NOOP("Annotation", "荧光笔"),
    QT_TRANSLATE_NOOP("Annotation", "文字"),
    QT_TRANSLATE_NOOP("Annotation", "序号"),
    QT_TRANSLATE_NOOP("Annotation", "打码"),
};

constexpr int kMaxLogical = 500;

// Which of a tool's remembered fields its kind uses.
struct Fields {
    bool color = true;
    bool width = false;
    bool fill = false;
    bool size = false;
    bool bold = false;
    bool boxed = false;
    bool outline = false; // Text: stroke and strokeColor
    bool mode = false;
    bool shape = false; // Redact: box or brush
    bool cell = false;
    bool format = false; // Number: how a series writes its numbers
};

Fields fieldsOf(Kind kind)
{
    Fields f;
    switch (kind) {
    case Kind::Rect:
    case Kind::Ellipse:
        f.width = true;
        f.fill = true;
        break;
    case Kind::Arrow:
    case Kind::Line:
    case Kind::Pen:
    case Kind::Marker:
        f.width = true;
        break;
    case Kind::Text:
        f.size = true;
        f.bold = true;
        f.boxed = true;
        f.outline = true;
        break;
    case Kind::Number:
        f.size = true;
        f.format = true;
        break;
    case Kind::Redact:
        f.mode = true;
        f.shape = true;
        f.width = true; // the brush's
        f.cell = true;
        break;
    }
    return f;
}

void readSize(const QJsonValue& value, int& out)
{
    if (!value.isDouble())
        return;
    const double d = value.toDouble();
    if (std::isfinite(d) && d == std::floor(d) && d >= 1 && d <= kMaxLogical)
        out = int(d);
}

} // namespace

const QList<ToolInfo>& tools()
{
    static const QList<ToolInfo> list = [] {
        const QChar letters[kKindCount] = {u'R', u'E', u'A', u'L', u'P', u'M', u'T', u'N', u'B'};
        QList<ToolInfo> out;
        for (int i = 0; i < kKindCount; ++i)
            out.append(ToolInfo{Kind(i), letters[i], i + 1, QString(), QString::fromUtf8(kToolNames[i])});
        return out;
    }();
    return list;
}

QString toolName(Kind kind)
{
    return QCoreApplication::translate("Annotation", kToolNames[int(kind)]);
}

const QList<QRgb>& palette()
{
    static const QList<QRgb> colors{qRgb(0xE5, 0x39, 0x35), qRgb(0xFB, 0x8C, 0x00), qRgb(0xFD, 0xD8, 0x35),
        qRgb(0x43, 0xA0, 0x47), qRgb(0x1E, 0x88, 0xE5), qRgb(0x8E, 0x24, 0xAA), qRgb(0x21, 0x21, 0x21),
        qRgb(0xFF, 0xFF, 0xFF)};
    return colors;
}

QList<int> sizePresets(Kind kind)
{
    switch (kind) {
    case Kind::Marker:
        return {14, 20, 28};
    case Kind::Text:
    case Kind::Number:
        return {14, 18, 24, 32};
    case Kind::Redact:
        return {8, 12, 18};
    default:
        return {2, 4, 7};
    }
}

QList<int> strokePresets()
{
    return {1, 2, 3};
}

QList<int> brushWidths()
{
    return {16, 24, 36};
}

int toImagePx(int logical, qreal dpr)
{
    return std::max(1, int(std::lround(logical * dpr)));
}

int toLogicalPx(int image, qreal dpr)
{
    return std::max(1, qRound(image / (dpr > 0 ? dpr : 1.0)));
}

StyleMemory::StyleMemory()
{
    for (int i = 0; i < kKindCount; ++i)
        m_tools[std::size_t(i)] = defaults(Kind(i));
}

StyleMemory::Tool StyleMemory::defaults(Kind kind)
{
    Tool t;
    t.color = qRgb(0xE5, 0x39, 0x35);
    if (kind == Kind::Marker) {
        t.color = qRgb(0xFD, 0xD8, 0x35);
        t.width = 20;
    } else if (kind == Kind::Redact) {
        t.color = qRgb(0x21, 0x21, 0x21); // the solid fill's colour
        t.width = 24; // the brush's
    }
    return t;
}

void StyleMemory::reset(Kind kind)
{
    m_tools[std::size_t(kind)] = defaults(kind);
}

NumberFormat StyleMemory::numberFormat() const
{
    return m_tools[std::size_t(Kind::Number)].format;
}

void StyleMemory::setNumberFormat(NumberFormat format)
{
    m_tools[std::size_t(Kind::Number)].format = format;
}

const StyleMemory::Tool& StyleMemory::tool(Kind kind) const
{
    return m_tools[std::size_t(kind)];
}

StyleMemory StyleMemory::fromJson(const QString& json)
{
    StyleMemory memory;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isObject())
        return memory;
    const QJsonObject root = document.object();
    // A newer layout is not guessed at: the defaults are safer.
    const QJsonValue version = root.value(u"version"_s);
    if (!version.isUndefined() && version.toInt(0) != 1)
        return memory;
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        const std::optional<Kind> kind = kindFromName(it.key());
        if (!kind || !it.value().isObject())
            continue;
        const QJsonObject o = it.value().toObject();
        Tool& t = memory.m_tools[std::size_t(*kind)];
        const Fields f = fieldsOf(*kind);
        if (isLine(*kind)) {
            t.lineMode = lineModeFromName(o.value(u"lineMode"_s).toString()).value_or(LineMode::Straight);
            if (*kind == Kind::Arrow && t.lineMode == LineMode::Polyline)
                t.lineMode = LineMode::Straight;
        }
        if (*kind == Kind::Marker)
            t.markerMode = markerModeFromName(o.value(u"markerMode"_s).toString()).value_or(MarkerMode::Freehand);
        if (f.color) {
            if (const auto color = parseColor(o.value(u"color"_s).toString()))
                t.color = *color;
        }
        if (f.width)
            readSize(o.value(u"width"_s), t.width);
        if (f.fill) {
            if (const auto fill = fillFromName(o.value(u"fill"_s).toString()))
                t.fill = *fill;
        }
        if (f.size)
            readSize(o.value(u"size"_s), t.size);
        if (f.bold && o.value(u"bold"_s).isBool())
            t.bold = o.value(u"bold"_s).toBool();
        if (f.boxed && o.value(u"boxed"_s).isBool())
            t.boxed = o.value(u"boxed"_s).toBool();
        if (f.outline) {
            const QJsonValue stroke = o.value(u"stroke"_s);
            if (stroke.isDouble() && stroke.toDouble() == 0)
                t.stroke = 0;
            else
                readSize(stroke, t.stroke);
            if (const auto color = parseColor(o.value(u"strokeColor"_s).toString()))
                t.strokeColor = *color | 0xff000000u;
        }
        if (f.mode) {
            if (const auto mode = redactModeFromName(o.value(u"mode"_s).toString()))
                t.mode = *mode;
        }
        if (f.shape) {
            if (const auto shape = redactShapeFromName(o.value(u"shape"_s).toString()))
                t.shape = *shape;
        }
        if (f.cell)
            readSize(o.value(u"cell"_s), t.cell);
        if (f.format) {
            if (const auto format = numberFormatFromName(o.value(u"format"_s).toString()))
                t.format = *format;
        }
    }
    return memory;
}

QString StyleMemory::toJson() const
{
    QJsonObject root;
    root[u"version"_s] = 1;
    for (int i = 0; i < kKindCount; ++i) {
        const Kind kind = Kind(i);
        const Tool& t = m_tools[std::size_t(i)];
        if (t == defaults(kind))
            continue;
        const Fields f = fieldsOf(kind);
        QJsonObject o;
        if (isLine(kind) && t.lineMode != LineMode::Straight)
            o[u"lineMode"_s] = lineModeName(t.lineMode);
        if (kind == Kind::Marker && t.markerMode != MarkerMode::Freehand)
            o[u"markerMode"_s] = markerModeName(t.markerMode);
        if (f.color)
            o[u"color"_s] = colorHex(t.color);
        if (f.width)
            o[u"width"_s] = t.width;
        if (f.fill)
            o[u"fill"_s] = fillName(t.fill);
        if (f.size)
            o[u"size"_s] = t.size;
        if (f.bold)
            o[u"bold"_s] = t.bold;
        if (f.boxed)
            o[u"boxed"_s] = t.boxed;
        if (f.outline && t.stroke > 0)
            o[u"stroke"_s] = t.stroke;
        if (f.outline && t.strokeColor != 0)
            o[u"strokeColor"_s] = colorHex(t.strokeColor);
        if (f.mode)
            o[u"mode"_s] = redactModeName(t.mode);
        if (f.shape)
            o[u"shape"_s] = redactShapeName(t.shape);
        if (f.cell)
            o[u"cell"_s] = t.cell;
        if (f.format)
            o[u"format"_s] = numberFormatName(t.format);
        root[kindName(kind)] = o;
    }
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

Style StyleMemory::forTool(Kind kind, qreal dpr) const
{
    const Tool& t = m_tools[std::size_t(kind)];
    const Fields f = fieldsOf(kind);
    Style s;
    if (isLine(kind))
        s.lineMode = t.lineMode;
    if (kind == Kind::Marker)
        s.markerMode = t.markerMode;
    if (f.color)
        s.color = t.color;
    if (f.width)
        s.width = toImagePx(t.width, dpr); // a box redaction is given Style's width when it is made
    if (f.fill)
        s.fill = t.fill;
    if (f.size)
        s.fontPx = toImagePx(t.size, dpr);
    if (f.bold)
        s.bold = t.bold;
    if (f.boxed)
        s.boxed = t.boxed;
    if (f.outline) {
        s.textStroke = t.stroke > 0 ? std::min(kMaxTextStroke, toImagePx(t.stroke, dpr)) : 0;
        s.textStrokeColor = t.strokeColor;
    }
    if (f.mode)
        s.redact = t.mode;
    if (f.shape)
        s.shape = t.shape;
    if (f.cell)
        s.cell = toImagePx(t.cell, dpr);
    return s;
}

void StyleMemory::remember(Kind kind, const Style& style, qreal dpr)
{
    Tool& t = m_tools[std::size_t(kind)];
    const Fields f = fieldsOf(kind);
    if (isLine(kind))
        t.lineMode = style.lineMode;
    if (kind == Kind::Marker)
        t.markerMode = style.markerMode;
    if (f.color)
        t.color = style.color | 0xff000000u;
    if (f.width && (kind != Kind::Redact || style.shape == RedactShape::Brush))
        t.width = std::min(kMaxLogical, toLogicalPx(style.width, dpr));
    if (f.fill)
        t.fill = style.fill;
    if (f.size)
        t.size = std::min(kMaxLogical, toLogicalPx(style.fontPx, dpr));
    if (f.bold)
        t.bold = style.bold;
    if (f.boxed)
        t.boxed = style.boxed;
    if (f.outline) {
        t.stroke = style.textStroke > 0 ? std::min(kMaxLogical, toLogicalPx(style.textStroke, dpr)) : 0;
        t.strokeColor = style.textStrokeColor ? (style.textStrokeColor | 0xff000000u) : 0;
    }
    if (f.mode)
        t.mode = style.redact;
    if (f.shape)
        t.shape = style.shape;
    if (f.cell)
        t.cell = std::min(kMaxLogical, toLogicalPx(style.cell, dpr));
}

}

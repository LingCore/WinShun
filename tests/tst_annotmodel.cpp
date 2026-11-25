// The overlay's annotations (src/capture/AnnotationModel, AnnotationCanvas;
// docs/design-capture-roadmap.md, 3.5-3.9): every tool drawn with the mouse
// as CaptureSession routes it, undo and redo, the keys, numbers, typing,
// the style panel and its memory, the wheel, the selection's bounds; the
// regions the model reports covering every pixel that changed; and the
// canvas's tiles grabbed from an offscreen window on the software scene
// graph, the same pixels as the compositor's.
#include "AnnotationCanvas.h"
#include "AnnotationModel.h"

#include "Annotation.h"
#include "AnnotationCommands.h"
#include "AnnotationFormat.h"
#include "AnnotationRender.h"
#include "AnnotationStyles.h"
#include "EditView.h"
#include "Geometry.h"

#include <QColor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegion>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <numbers>
#include <optional>

using namespace ws::capture;
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

constexpr QRgb kMagenta = 0xffff00ffu;

// splitmix64: the same picture with every compiler and library.
QImage noiseImage(QSize size, quint64 seed)
{
    quint64 state = seed;
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            quint64 z = (state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            line[x] = 0xff000000u | QRgb((z ^ (z >> 31)) & 0xffffffu);
        }
    }
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

// Pixels of `got` within `area` against `want` (whole pictures of one size).
Diff compare(const QImage& got, const QImage& want, QRect area = {})
{
    Diff d;
    if (got.size() != want.size() || got.format() != QImage::Format_RGB32 || want.format() != QImage::Format_RGB32) {
        d.count = -1;
        return d;
    }
    if (area.isNull())
        area = got.rect();
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const QRgb* g = reinterpret_cast<const QRgb*>(got.constScanLine(y));
        const QRgb* w = reinterpret_cast<const QRgb*>(want.constScanLine(y));
        for (int x = area.left(); x <= area.right(); ++x) {
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

QByteArray describe(const Diff& d)
{
    return u"%1 pixels differ, the first at (%2, %3): #%4, not #%5"_s.arg(d.count)
        .arg(d.first.x())
        .arg(d.first.y())
        .arg(d.got, 8, 16, QChar(u'0'))
        .arg(d.want, 8, 16, QChar(u'0'))
        .toUtf8();
}

int toolIndex(Kind kind)
{
    const QList<ToolInfo>& list = tools();
    for (qsizetype i = 0; i < list.size(); ++i) {
        if (list[i].kind == kind)
            return int(i);
    }
    return -1;
}

// As CaptureSession routes a press (3.6): the model's first questions, the
// selection's own handles (none here), then the rest.
void press(AnnotationModel& m, QPoint p, Qt::KeyboardModifiers modifiers = {})
{
    if (!m.pressFirst(p, modifiers))
        m.pressSecond(p, modifiers);
}

void drag(AnnotationModel& m, QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = {}, int steps = 6)
{
    press(m, from, modifiers);
    for (int i = 1; i <= steps; ++i)
        m.move(from + (to - from) * i / steps, modifiers);
    m.release(to);
}

// A drag through `points`, one move each.
void stroke(AnnotationModel& m, const QList<QPoint>& points)
{
    press(m, points.first());
    for (qsizetype i = 1; i < points.size(); ++i)
        m.move(points[i], {});
    m.release(points.last());
}

void click(AnnotationModel& m, QPoint p)
{
    press(m, p);
    m.release(p);
}

QList<QPoint> zigzag(QPoint from, int count, QPoint step, QPoint swing)
{
    QList<QPoint> points;
    for (int i = 0; i < count; ++i)
        points.append(from + step * i + (i % 2 ? swing : QPoint()));
    return points;
}

// A pixel on which `id` is the topmost object, as far from its handles as
// the object allows, so a press there picks it up rather than a handle.
QPoint pointOn(const AnnotationModel& m, quint32 id)
{
    const Document& doc = m.document();
    const Item* item = findItem(doc, id);
    if (!item)
        return QPoint(-1, -1);
    const QRect r = bounds(doc, *item).toAlignedRect() & QRect(QPoint(0, 0), doc.canvas);
    const QList<QPointF> marks = handles(*item);
    QPoint best(-1, -1);
    qreal farthest = -1;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        for (int x = r.left(); x <= r.right(); ++x) {
            if (hitTest(doc, QPointF(x, y), 4 * doc.dpr) != id)
                continue;
            qreal nearest = 1e9;
            for (const QPointF& h : marks)
                nearest = std::min(nearest, std::hypot(h.x() - (x + 0.5), h.y() - (y + 0.5)));
            if (nearest > farthest) {
                farthest = nearest;
                best = QPoint(x, y);
            }
        }
    }
    return best;
}

quint32 selectedId(const AnnotationModel& m)
{
    const QVariantList s = m.selection();
    return s.isEmpty() ? 0 : s[0].toMap().value(u"id"_s).toUInt();
}

const Item* lastOf(const AnnotationModel& m, Kind kind)
{
    const Item* found = nullptr;
    for (const Item& item : m.document().items) {
        if (item.kind == kind)
            found = &item;
    }
    return found;
}

QList<int> shownNumbers(const AnnotationModel& m)
{
    const QHash<quint32, int> n = numbers(m.document());
    QList<int> out;
    for (const Item& item : m.document().items) {
        if (item.kind == Kind::Number)
            out.append(n.value(item.id));
    }
    return out;
}

QList<Kind> kinds(const AnnotationModel& m)
{
    QList<Kind> out;
    for (const Item& item : m.document().items)
        out.append(item.kind);
    return out;
}

qreal degrees(const Item& line)
{
    const QPointF v = line.points[1] - line.points[0];
    return std::atan2(-v.y(), v.x()) * 180 / std::numbers::pi;
}

// A capture: a noise picture, its whole area selected unless told.
// A reader that only records what it was asked, and answers when told to.
struct FakeReader {
    int scans = 0;
    int cancels = 0;
    QImage last;
    QImage frozen;
    QRect region;
    std::function<void(QList<RedactLine>)> answer;
    bool prescan = true;
    SmartHooks hooks()
    {
        SmartHooks h;
        h.scan = [this](const QImage& image, const QRect& rect, double, std::function<void(RedactScan)> done) {
            ++scans;
            frozen = image;
            region = rect;
            last = image.copy(rect);
            answer = [done = std::move(done), gray = last.convertToFormat(QImage::Format_Grayscale8)](QList<RedactLine> lines) {
                done(RedactScan {std::move(lines), gray});
            };
        };
        h.cancel = [this] { ++cancels; };
        h.rules = [] { return RedactRules(); };
        h.prescan = [this] { return prescan; };
        return h;
    }
};

// Three phone numbers on three lines, in the scanned picture's own pixels.
QList<RedactLine> phoneLines()
{
    QList<RedactLine> lines;
    for (int i = 0; i < 3; ++i) {
        RedactLine line;
        line.text = u"13812345678"_s;
        const int top = 20 + 40 * i;
        line.quad = QPolygonF({QPointF(10, top), QPointF(120, top), QPointF(120, top + 20), QPointF(10, top + 20)});
        for (int k = 0; k < 11; ++k)
            line.chars.append({k, float((k + 0.5) / 11)});
        lines.append(line);
    }
    return lines;
}

struct Rig {
    explicit Rig(QSize size = QSize(400, 300), qreal dpr = 1.0, QRect selection = {})
        : base(noiseImage(size, 7))
    {
        model.begin(base, dpr, StyleMemory());
        model.setSelection(selection.isNull() ? QRect(QPoint(0, 0), size) : selection);
    }
    QImage base;
    AnnotationModel model;
};

// The editing window's model: a project's whole picture opened, nothing cropped.
struct EditRig {
    explicit EditRig(QSize size = QSize(400, 300), qreal dpr = 1.0)
        : base(noiseImage(size, 9))
    {
        Project project;
        project.base = base;
        project.doc.canvas = size;
        project.doc.crop = QRect(QPoint(0, 0), size);
        project.doc.dpr = dpr;
        model.begin(project, dpr, StyleMemory());
    }
    QImage base;
    AnnotationModel model;
};

// What the canvas does with changed(): a picture brought up to date only
// where the model says something changed. Checked against the whole picture
// drawn afresh by another compositor, which also catches a cache the model
// forgot to drop.
class Screen {
public:
    explicit Screen(AnnotationModel& model)
        : m_model(model)
    {
        m_connection = QObject::connect(&model, &AnnotationModel::changed, [this](const QRectF& dirty) {
            if (dirty.isEmpty())
                m_all = true;
            else
                m_pending += dirty.toAlignedRect();
        });
    }
    ~Screen() { QObject::disconnect(m_connection); }

    Diff check()
    {
        const QRect canvas(QPoint(0, 0), m_model.base().size());
        const std::optional<quint32> skip = m_model.skipped();
        if (m_all || m_image.isNull()) {
            m_model.compositor().render(m_model.document(), m_model.base(), m_model.sampleBounds(), canvas, &m_image,
                skip);
        } else {
            for (const QRect& r : m_pending) {
                const QRect area = r & canvas;
                if (area.isEmpty())
                    continue;
                QImage piece;
                m_model.compositor().render(m_model.document(), m_model.base(), m_model.sampleBounds(), area, &piece,
                    skip);
                blit(m_image, area.topLeft(), piece);
            }
        }
        m_all = false;
        m_pending = QRegion();
        Compositor fresh;
        QImage whole;
        fresh.render(m_model.document(), m_model.base(), m_model.sampleBounds(), canvas, &whole, skip);
        return compare(m_image, whole);
    }

private:
    AnnotationModel& m_model;
    QMetaObject::Connection m_connection;
    QImage m_image;
    QRegion m_pending;
    bool m_all = true;
};

#define CHECK_SCREEN(screen)                                                                                           \
    do {                                                                                                               \
        const Diff diff_ = (screen).check();                                                                           \
        QVERIFY2(diff_.count == 0, describe(diff_).constData());                                                       \
    } while (false)

} // namespace

class TestAnnotModel : public QObject {
    Q_OBJECT

private slots:
    void connectedLines()
    {
        Rig rig;
        auto& m = rig.model;
        m.setTool(toolIndex(Kind::Line));
        m.setPanel({{u"lineMode"_s, u"polyline"_s}});
        click(m, {50, 60});
        QVERIFY(m.pathActive());
        QVERIFY(m.project().doc.items.isEmpty()); // a lone point is not a saved path
        m.move({100, 80}, {});
        click(m, {100, 80});
        m.move({150, 130}, {});
        QCOMPARE(m.project().doc.items.first().points.size(), 2); // save only placed vertices
        QVERIFY(m.pathActive()); // a project snapshot does not end drawing
        click(m, {160, 120});
        QVERIFY(m.key(Qt::Key_Backspace, {}));
        click(m, {160, 90});
        QVERIFY(m.key(Qt::Key_Return, {}));
        QVERIFY(!m.pathActive());
        QCOMPARE(m.itemCount(), 1);
        const auto item = m.document().items.first();
        QCOMPARE(item.points.size(), 3);
        QCOMPARE(item.points.last(), QPointF(160.5, 90.5));
        QVERIFY(!item.closed);
        QVERIFY(hitTest(m.document(), {125, 84}, 3));
        const auto saved = fromJson(toJson(m.document()), nullptr);
        QVERIFY(saved);
        QCOMPARE(*saved, m.document());
        m.undo();
        QCOMPARE(m.itemCount(), 0);
        m.redo();
        QCOMPARE(m.document().items.first(), item);
        click(m, {200, 140});
        click(m, {270, 140});
        click(m, {270, 200});
        click(m, {200, 140});
        QVERIFY(!m.pathActive());
        QVERIFY(m.document().items.last().closed);
        QCOMPARE(m.document().items.last().points.size(), 3);
        click(m, {70, 160});
        click(m, {120, 190});
        QVERIFY(m.escape());
        QVERIFY(!m.pathActive());
        QCOMPARE(m.itemCount(), 2);
    }

    void editableBezier()
    {
        for (Kind kind : {Kind::Line, Kind::Arrow}) {
            Rig rig;
            auto& m = rig.model;
            m.setTool(toolIndex(kind));
            m.setPanel({{u"lineMode"_s, u"bezier"_s}});
            drag(m, {50, 60}, {220, 100});
            QCOMPARE(m.itemCount(), 1);
            const auto item = m.document().items.first();
            QCOMPARE(item.points.size(), 4);
            QCOMPARE(handles(item).size(), 4);
            const QPainterPath centre = linePath(item);
            QVERIFY(hitTest(m.document(), centre.pointAtPercent(0.5), 3));
            QVERIFY(!hitTest(m.document(), {120, 40}, 1));
            const QPoint control = pixelAt(item.points[2]);
            drag(m, control, control + QPoint(0, 30));
            const auto reshaped = m.document().items.first();
            QVERIFY(reshaped.points[2] != item.points[2]);
            m.undo();
            QCOMPARE(m.document().items.first(), item);
            const auto parsed = fromJson(toJson(m.document()), nullptr);
            QVERIFY(parsed);
            QCOMPARE(*parsed, m.document());
            QVERIFY(!m.composed().isNull());
        }
    }

    void markerShapes()
    {
        QImage base(320, 220, QImage::Format_RGB32);
        base.fill(Qt::white);
        base.setPixel(100, 100, qRgb(0, 0, 0));
        for (const QString mode : {u"line"_s, u"rectangle"_s}) {
            AnnotationModel m;
            m.begin(base, 1, StyleMemory{});
            m.setSelection(base.rect());
            m.setTool(toolIndex(Kind::Marker));
            m.setPanel({{u"markerMode"_s, mode}, {u"color"_s, u"#FFFF00"_s}});
            drag(m, {60, 80}, {200, 120});
            QCOMPARE(m.itemCount(), 1);
            const auto item = m.document().items.first();
            QCOMPARE(item.points.size(), 2);
            QCOMPARE(handles(item).size(), mode == u"rectangle" ? 8 : 2);
            const QImage result = m.composed();
            QVERIFY(!result.isNull());
            if (mode == u"rectangle") {
                QCOMPARE(result.pixel(100, 100), qRgb(0, 0, 0));
                QCOMPARE(result.pixel(110, 100), qRgb(255, 255, 0));
                QVERIFY(hitTest(m.document(), {150, 100}, 1));
            }
            const auto parsed = fromJson(toJson(m.document()), nullptr);
            QVERIFY(parsed);
            QCOMPARE(*parsed, m.document());
            m.undo();
            QCOMPARE(m.itemCount(), 0);
            m.redo();
            QCOMPARE(m.document().items.first(), item);
            const StyleMemory remembered = StyleMemory::fromJson(m.styles().toJson());
            QCOMPARE(remembered, m.styles());
        }
    }

    void drawEveryTool()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        const QPoint a(50, 60);
        const QPoint b(120, 100);
        const QList<ToolInfo>& list = tools();
        for (qsizetype i = 0; i < list.size(); ++i) {
            const Kind kind = list[i].kind;
            m.setTool(int(i));
            QCOMPARE(m.tool(), int(i));
            QVERIFY(m.panelVisible());
            QCOMPARE(m.panel().value(u"kind"_s).toInt(), int(kind));
            const int before = m.itemCount();
            if (kind == Kind::Text) {
                click(m, a);
                const QVariantMap e = m.editing();
                QCOMPARE(e.value(u"id"_s).toInt(), 0);
                QCOMPARE(e.value(u"x"_s).toInt(), a.x());
                QCOMPARE(e.value(u"y"_s).toInt(), a.y());
                QCOMPARE(e.value(u"clickX"_s).toInt(), -1);
                QCOMPARE(m.itemCount(), before); // in the document once committed
                m.commitText(u"标注 Text"_s);
            } else {
                drag(m, a, b);
                if (kind == Kind::Number) {
                    QVERIFY(m.editingText()); // its words are typed next
                    m.commitPending();
                }
            }
            QCOMPARE(m.itemCount(), before + 1);
            QVERIFY(!m.gestureActive());
            const Item* item = lastOf(m, kind);
            QVERIFY(item);
            Style expected = StyleMemory().forTool(kind, 1.0);
            if (kind == Kind::Redact)
                expected.width = Style {}.width; // the tool's width is the brush's; a box has Style's
            QCOMPARE(item->style, expected);
            QCOMPARE(selectedId(m), item->id); // what was drawn is selected
            QCOMPARE(m.tool(), int(i)); // and the tool stays on
            const QVariantMap selection = m.selection().value(0).toMap();
            QCOMPARE(selection.value(u"kind"_s).toInt(), int(kind));
            switch (kind) {
            case Kind::Rect:
            case Kind::Ellipse:
            case Kind::Redact:
            case Kind::Spotlight:
                QCOMPARE(item->points, boxPoints(pixelBox(a, b)));
                QCOMPARE(itemBox(*item), QRect(QPoint(50, 60), QPoint(120, 100)));
                QVERIFY(selection.value(u"box"_s).toBool());
                QCOMPARE(selection.value(u"handles"_s).toList().size(), 8);
                if (kind == Kind::Redact)
                    QVERIFY(item->seed != 0);
                break;
            case Kind::Arrow:
            case Kind::Line:
                QCOMPARE(item->points, (QList<QPointF> {QPointF(50.5, 60.5), QPointF(120.5, 100.5)}));
                QVERIFY(!selection.value(u"box"_s).toBool());
                QCOMPARE(selection.value(u"handles"_s).toList().size(), 2);
                break;
            case Kind::Pen:
            case Kind::Marker:
                QVERIFY(item->points.size() >= 2);
                QCOMPARE(item->points.first(), QPointF(50.5, 60.5));
                QCOMPARE(item->points.last(), QPointF(120.5, 100.5));
                QVERIFY(selection.value(u"handles"_s).toList().isEmpty());
                break;
            case Kind::Number:
                QCOMPARE(item->points, (QList<QPointF> {QPointF(120.5, 100.5)})); // carried by the drag
                QCOMPARE(item->series, m.document().currentSeries);
                QVERIFY(!item->pinned);
                break;
            case Kind::Text:
                QCOMPARE(item->points, (QList<QPointF> {QPointF(a)}));
                QCOMPARE(item->text, u"标注 Text"_s);
                break;
            }
        }
        QCOMPARE(m.itemCount(), int(list.size()));
        QCOMPARE(m.document().items.first().kind, Kind::Redact); // the redactions are at the bottom
        // A pen stroke keeps its path: a zigzag stays a zigzag.
        m.setTool(toolIndex(Kind::Pen));
        stroke(m, zigzag(QPoint(200, 150), 9, QPoint(10, 0), QPoint(0, 20)));
        QVERIFY(lastOf(m, Kind::Pen)->points.size() >= 9);
    }

    void freehandFiltersSmallJitterWithoutChangingOnRelease()
    {
        for (const Kind kind : {Kind::Pen, Kind::Marker}) {
            Rig rig;
            AnnotationModel& m = rig.model;
            m.setTool(toolIndex(kind));
            QList<QPointF> raw {QPointF(40.5, 100.5)};
            press(m, QPoint(40, 100));
            for (int i = 1; i <= 200; ++i) {
                const QPoint p(40 + i, 100 + ((i / 2) % 2 ? 2 : -2));
                raw.append(pixelCentre(p));
                m.move(p, {});
            }
            m.move(QPoint(241, 100), {});
            raw.append(QPointF(241.5, 100.5));
            const Item draft = m.document().items.last();
            const auto jitter = [](const QPainterPath& path) {
                qreal sum = 0;
                for (int i = 10; i <= 190; ++i) {
                    const qreal offset = path.pointAtPercent(i / 200.0).y() - 100.5;
                    sum += offset * offset;
                }
                return std::sqrt(sum / 181);
            };
            const qreal before = jitter(strokeCentreline(raw));
            const qreal after = jitter(strokeCentreline(draft.points));
            qInfo() << "freehand jitter RMS, raw/stabilized:" << before << after;
            QVERIFY2(after < before * 0.65, "small input jitter must be reduced in the live stroke");
            // Uniform arc-length samples measure visible tangent changes,
            // rather than only how far the line strays from its baseline.
            const QPainterPath curve = strokeCentreline(draft.points);
            QList<QPointF> uniform;
            for (qreal distance = 10; distance < curve.length() - 10; distance += 1)
                uniform.append(curve.pointAtPercent(curve.percentAtLength(distance)));
            qreal roughness = 0;
            for (qsizetype i = 1; i + 1 < uniform.size(); ++i) {
                const QPointF bend = uniform[i - 1] - uniform[i] * 2 + uniform[i + 1];
                roughness += QPointF::dotProduct(bend, bend);
            }
            roughness = std::sqrt(roughness / (uniform.size() - 2));
            qInfo() << "uniform curve second difference RMS:" << roughness << "stored points:" << draft.points.size();
            QVERIFY2(roughness < 0.08, "a visually smooth line must not retain dense small waves");
            QVERIFY(draft.points.size() < raw.size() / 2);
            QCOMPARE(draft.points.first(), raw.first());
            QCOMPARE(draft.points.last(), raw.last());
            m.release(QPoint(241, 100));
            QCOMPARE(m.document().items.last(), draft);
            const Document committed = m.document();
            m.undo();
            QCOMPARE(m.itemCount(), 0);
            m.redo();
            QCOMPARE(m.document(), committed);
        }
    }

    void stabilizedFreehandRepaintsAndRoundTripsAtDifferentScales()
    {
        for (const Kind kind : {Kind::Pen, Kind::Marker}) {
            for (qreal dpr : {1.0, 1.5, 2.0}) {
                for (qreal zoom : {0.5, 1.0, 2.0}) {
                    Rig rig(QSize(400, 300), dpr);
                    AnnotationModel& m = rig.model;
                    m.setView(dpr, zoom);
                    m.setTool(toolIndex(kind));
                    Screen screen(m);
                    CHECK_SCREEN(screen);
                    const QList<QPoint> points {QPoint(45, 70), QPoint(60, 70), QPoint(62, 72), QPoint(64, 70),
                        QPoint(66, 69), QPoint(67, 71), QPoint(68, 70), QPoint(90, 70), QPoint(130, 70),
                        QPoint(130, 110), QPoint(170, 135), QPoint(210, 110), QPoint(170, 90), QPoint(130, 110),
                        QPoint(250, 190), QPoint(252, 188), QPoint(254, 190), QPoint(255, 189)};
                    press(m, points.first());
                    for (qsizetype i = 1; i < points.size(); ++i) {
                        m.move(points[i], {});
                        CHECK_SCREEN(screen); // includes a live tip bending and returning to an axis
                    }
                    const Item draft = m.document().items.last();
                    m.release(points.last());
                    QCOMPARE(m.document().items.last(), draft);
                    CHECK_SCREEN(screen);
                    const Document committed = m.document();
                    QString error;
                    const auto loaded = fromJson(toJson(committed), &error);
                    QVERIFY2(loaded.has_value(), qPrintable(error));
                    QCOMPARE(*loaded, committed); // subpixel anchors survive project serialization
                    Compositor reopened;
                    const QImage want = reopened.exportImage(*loaded, rig.base, m.sampleBounds());
                    const Diff exported = compare(m.composed(), want);
                    QVERIFY2(exported.count == 0, describe(exported).constData());
                    m.undo();
                    CHECK_SCREEN(screen);
                    m.redo();
                    CHECK_SCREEN(screen);
                    QCOMPARE(m.document(), committed);
                }
            }
        }
    }

    void freehandResumesAfterShiftAndCancel()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Pen));
        Screen screen(m);
        CHECK_SCREEN(screen);
        press(m, QPoint(40, 60));
        m.move(QPoint(80, 90), {});
        m.move(QPoint(130, 65), Qt::ShiftModifier);
        QCOMPARE(m.document().items.last().points.size(), 2);
        CHECK_SCREEN(screen);
        m.move(QPoint(170, 105), {});
        CHECK_SCREEN(screen);
        m.release(QPoint(170, 105));
        QCOMPARE(m.document().items.last().points.first(), QPointF(40.5, 60.5));
        QCOMPARE(m.document().items.last().points.last(), QPointF(170.5, 105.5));
        const Document committed = m.document();
        press(m, QPoint(40, 180));
        m.move(QPoint(130, 220), {});
        m.cancelGesture();
        QCOMPARE(m.document(), committed);
        CHECK_SCREEN(screen);
        stroke(m, {QPoint(220, 180), QPoint(260, 190), QPoint(300, 210)});
        QCOMPARE(m.document().items.last().points.first(), QPointF(220.5, 180.5));
        QCOMPARE(m.document().items.last().points.last(), QPointF(300.5, 210.5));
        CHECK_SCREEN(screen);
    }

    void clicksMakeNothing()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        for (const Kind kind : {Kind::Rect, Kind::Ellipse, Kind::Arrow, Kind::Line, Kind::Redact}) {
            m.setTool(toolIndex(kind));
            click(m, QPoint(100, 100));
            drag(m, QPoint(150, 150), QPoint(151, 150)); // under the drag start
            QCOMPARE(m.itemCount(), 0);
            QVERIFY(!m.canUndo());
        }
        // A click with the number tool is a number.
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(100, 100));
        QCOMPARE(m.itemCount(), 1);
        QCOMPARE(m.document().items[0].points, (QList<QPointF> {QPointF(100.5, 100.5)}));
    }

    // GitHub #461: a click with the pen or the freehand marker is a dot of its width.
    void penClickIsADot()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Pen));
        click(m, QPoint(100, 100));
        QCOMPARE(m.itemCount(), 1);
        QCOMPARE(m.document().items[0].kind, Kind::Pen);
        QCOMPARE(m.document().items[0].points, (QList<QPointF> {QPointF(100.5, 100.5)}));
        // It is there to be seen: as wide as the pen, round its point.
        const QRectF dot = annot::bounds(m.document(), m.document().items[0]);
        QVERIFY(dot.contains(QPointF(100.5, 100.5)));
        QVERIFY(dot.width() >= m.document().items[0].style.width);
        QVERIFY(m.canUndo());
        m.undo();
        QCOMPARE(m.itemCount(), 0);
        // A hand that shook under the drag start: still one point, where it was pressed.
        drag(m, QPoint(200, 150), QPoint(201, 150));
        QCOMPARE(m.itemCount(), 1);
        QCOMPARE(m.document().items[0].points, (QList<QPointF> {QPointF(200.5, 150.5)}));
        // The freehand marker too; its line and rectangle are shapes, a click makes none.
        m.setTool(toolIndex(Kind::Marker));
        click(m, QPoint(300, 100));
        QCOMPARE(m.itemCount(), 2);
        QCOMPARE(m.document().items[1].kind, Kind::Marker);
        QCOMPARE(m.document().items[1].points.size(), 1);
    }

    // GitHub #461: the pointer shows the tip of the pen, the marker and the brush redaction.
    void brushTipFollowsTheTool()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        const QPoint at(100, 100);
        for (const Kind kind : {Kind::Rect, Kind::Ellipse, Kind::Arrow, Kind::Line, Kind::Number, Kind::Redact})  {
            m.setTool(toolIndex(kind));
            QCOMPARE(m.brushTipAt(at).width, 0); // a cross, a box redaction: no round tip
        }
        m.setTool(toolIndex(Kind::Pen));
        const AnnotationModel::BrushTip pen = m.brushTipAt(at);
        QCOMPARE(pen.width, m.styles().tool(Kind::Pen).width);
        QCOMPARE(qAlpha(pen.color), 255);
        QCOMPARE(pen.color & 0xFFFFFF, m.styles().tool(Kind::Pen).color & 0xFFFFFF);
        m.setTool(toolIndex(Kind::Marker));
        const AnnotationModel::BrushTip marker = m.brushTipAt(at);
        QCOMPARE(marker.width, m.styles().tool(Kind::Marker).width);
        QVERIFY(qAlpha(marker.color) < 255); // see-through, as its ink
        // Over what was just drawn (selected: a press moves it) there is no tip.
        m.setTool(toolIndex(Kind::Pen));
        stroke(m, {QPoint(200, 200), QPoint(240, 200), QPoint(280, 200)});
        QCOMPARE(m.brushTipAt(QPoint(240, 200)).width, 0);
        QCOMPARE(m.brushTipAt(at).width, pen.width);
    }

    void undoRedoTenSteps()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        QList<Document> docs {m.document()};
        const auto step = [&] { docs.append(m.document()); };
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(40, 40), QPoint(120, 90));
        step(); // 1
        m.setTool(toolIndex(Kind::Ellipse));
        drag(m, QPoint(150, 40), QPoint(220, 90));
        step(); // 2
        m.setTool(toolIndex(Kind::Arrow));
        drag(m, QPoint(40, 150), QPoint(140, 200));
        step(); // 3
        m.setTool(toolIndex(Kind::Pen));
        stroke(m, zigzag(QPoint(200, 150), 8, QPoint(12, 6), QPoint(0, 18)));
        step(); // 4
        const quint32 rect = lastOf(m, Kind::Rect)->id;
        const quint32 ellipse = lastOf(m, Kind::Ellipse)->id;
        const quint32 arrow = lastOf(m, Kind::Arrow)->id;
        const quint32 pen = lastOf(m, Kind::Pen)->id;
        m.setTool(-1);
        const QPoint on = pointOn(m, rect);
        drag(m, on, on + QPoint(10, 12));
        QCOMPARE(itemBox(*findItem(m.document(), rect)), QRect(QPoint(50, 52), QPoint(130, 102)));
        step(); // 5: moved by its edge
        click(m, pointOn(m, ellipse));
        QCOMPARE(selectedId(m), ellipse);
        QVERIFY(m.onHandle(QPoint(221, 91)));
        drag(m, QPoint(221, 91), QPoint(241, 101)); // the bottom right handle
        QCOMPARE(itemBox(*findItem(m.document(), ellipse)), QRect(QPoint(150, 40), QPoint(240, 100)));
        step(); // 6: reshaped
        click(m, pointOn(m, arrow));
        QCOMPARE(selectedId(m), arrow);
        m.setPanel({{u"color"_s, u"#1E88E5"_s}});
        QCOMPARE(findItem(m.document(), arrow)->style.color, qRgb(0x1E, 0x88, 0xE5));
        step(); // 7: restyled
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(330, 60));
        step(); // 8
        m.newSeries();
        step(); // 9
        m.setTool(-1);
        click(m, pointOn(m, pen));
        QCOMPARE(selectedId(m), pen);
        m.deleteSelection();
        QVERIFY(!findItem(m.document(), pen));
        step(); // 10
        QCOMPARE(docs.size(), 11);
        for (int i = 9; i >= 0; --i) {
            QVERIFY(m.canUndo());
            m.undo();
            QCOMPARE(m.document(), docs[i]);
        }
        QVERIFY(!m.canUndo());
        QVERIFY(m.selection().isEmpty()); // nothing left to select
        for (int i = 1; i <= 10; ++i) {
            QVERIFY(m.canRedo());
            m.redo();
            QCOMPARE(m.document(), docs[i]);
        }
        QVERIFY(!m.canRedo());
        QVERIFY(!m.undoLabel().isEmpty());
    }

    // ---- smart redaction (4.1): the pre-scan and the one-click redaction ----

    void smartDispatchSharesLargeFrozenPixels()
    {
        QImage base(3840, 2160, QImage::Format_RGB32);
        base.fill(Qt::white);
        QImage pending;
        QRect requested;
        SmartHooks hooks;
        hooks.prescan = [] { return false; };
        hooks.scan = [&](const QImage& image, const QRect& region, double, std::function<void(RedactScan)>) {
            pending = image; // the reader queue keeps the immutable snapshot
            requested = region;
        };
        AnnotationModel model;
        model.setSmartHooks(std::move(hooks));
        model.begin(base, 1.5, StyleMemory());
        QList<qint64> preprocessing, dispatch;
        for (int sample = 0; sample < 8; ++sample) {
            const QRect region(40, 40, 3500 - sample, 1900);
            model.setSelection(region);
            QElapsedTimer timer;
            timer.start();
            const QImage oldGray = base.copy(region).convertToFormat(QImage::Format_Grayscale8);
            preprocessing.append(timer.nsecsElapsed());
            QCOMPARE(oldGray.size(), region.size());
            timer.restart();
            model.smartRedact();
            dispatch.append(timer.nsecsElapsed());
            QCOMPARE(pending.constBits(), base.constBits());
            QCOMPARE(requested, region);
        }
        std::ranges::sort(preprocessing);
        std::ranges::sort(dispatch);
        qInfo().noquote() << u"4K smart scan boundary: old crop/gray p50 %1 ms; shared-image GUI dispatch p50 %2 ms (OCR excluded)"_s
            .arg(preprocessing[4] / 1e6, 0, 'f', 3).arg(dispatch[4] / 1e6, 0, 'f', 3);
    }

    void smartScansOnceTheSelectionHoldsStill()
    {
        FakeReader reader;
        AnnotationModel m;
        m.setSmartHooks(reader.hooks());
        m.begin(noiseImage(QSize(400, 300), 3), 1.0, StyleMemory());
        m.setSelection(QRect(10, 10, 100, 100));
        m.setSelection(QRect(10, 10, 200, 150)); // still moving
        QCOMPARE(reader.scans, 0);
        QTRY_COMPARE_WITH_TIMEOUT(reader.scans, 1, 1500);
        QCOMPARE(reader.frozen.constBits(), m.base().constBits()); // no GUI-thread crop or deep copy
        QCOMPARE(reader.region, QRect(10, 10, 200, 150));
        QCOMPARE(reader.last.size(), QSize(200, 150)); // the settled selection, nothing else
        QCOMPARE(m.smartFound(), -1); // nothing yet
        // The selection moves while it is read: that answer is for nothing.
        m.setSelection(QRect(20, 20, 160, 120));
        QVERIFY(reader.cancels >= 1);
        reader.answer(phoneLines());
        QCOMPARE(m.smartFound(), -1);
        QTRY_COMPARE_WITH_TIMEOUT(reader.scans, 2, 1500);
        QCOMPARE(reader.last.size(), QSize(160, 120));
        reader.answer(phoneLines());
        QCOMPARE(m.smartFound(), 3);
    }

    void smartRedactIsOneUndoStep()
    {
        FakeReader reader;
        AnnotationModel m;
        m.setSmartHooks(reader.hooks());
        m.begin(noiseImage(QSize(400, 300), 3), 1.0, StyleMemory());
        const QRect selection(30, 40, 200, 160);
        m.setSelection(selection);
        QTRY_COMPARE_WITH_TIMEOUT(reader.scans, 1, 1500);
        reader.answer(phoneLines());
        QCOMPARE(m.smartFound(), 3);
        QSignalSpy notices(&m, &AnnotationModel::notice);
        m.smartRedact();
        QCOMPARE(m.itemCount(), 3);
        QCOMPARE(notices.count(), 1);
        QVERIFY2(notices.first().first().toString().contains(u"3"_s), qPrintable(notices.first().first().toString()));
        // In the tool's style, inside the selection, over the numbers (moved by the selection's corner).
        for (const Item& item : m.document().items) {
            QCOMPARE(item.kind, Kind::Redact);
            QCOMPARE(item.style.shape, RedactShape::Box);
            QVERIFY(selection.contains(itemBox(item)));
            QVERIFY(item.style.cell >= int(0.3 * itemBox(item).height()));
        }
        const QRect first = itemBox(m.document().items[0]);
        QVERIFY(first.left() <= selection.left() + 10 && first.right() >= selection.left() + 120 - 4);
        QVERIFY(first.top() <= selection.top() + 20 && first.bottom() >= selection.top() + 20 + 19);
        // What was hidden is not offered again, and a second click finds nothing new.
        QCOMPARE(m.smartFound(), -1);
        notices.clear();
        m.smartRedact();
        QCOMPARE(m.itemCount(), 3);
        QCOMPARE(notices.count(), 1);
        // One Ctrl+Z takes all three, and the offer is back; Ctrl+Y puts them back.
        m.undo();
        QCOMPARE(m.itemCount(), 0);
        QCOMPARE(m.smartFound(), 3);
        m.redo();
        QCOMPARE(m.itemCount(), 3);
        QCOMPARE(m.smartFound(), -1);
        QVERIFY(!m.canRedo());
        m.undo();
        QVERIFY(!m.canUndo());
    }

    void smartRedactReadsFirstWhenTheScanIsOff()
    {
        FakeReader reader;
        reader.prescan = false;
        AnnotationModel m;
        m.setSmartHooks(reader.hooks());
        m.begin(noiseImage(QSize(400, 300), 3), 1.0, StyleMemory());
        m.setSelection(QRect(30, 40, 200, 160));
        QTest::qWait(500);
        QCOMPARE(reader.scans, 0); // nothing on its own
        QVERIFY(m.key(Qt::Key_B, Qt::ShiftModifier)); // Shift+B, not the tool
        QCOMPARE(m.tool(), -1);
        QCOMPARE(reader.scans, 1);
        QVERIFY(m.smartBusy());
        QCOMPARE(m.itemCount(), 0);
        reader.answer(phoneLines());
        QCOMPARE(m.itemCount(), 3); // hidden as soon as it is read
        QVERIFY(!m.smartBusy());
        m.undo();
        QCOMPARE(m.itemCount(), 0);
    }

    void smartNothingFound()
    {
        FakeReader reader;
        AnnotationModel m;
        m.setSmartHooks(reader.hooks());
        m.begin(noiseImage(QSize(400, 300), 3), 1.0, StyleMemory());
        m.setSelection(QRect(30, 40, 200, 160));
        QTRY_COMPARE_WITH_TIMEOUT(reader.scans, 1, 1500);
        reader.answer({});
        QCOMPARE(m.smartFound(), -1);
        QSignalSpy notices(&m, &AnnotationModel::notice);
        m.smartRedact();
        QCOMPARE(m.itemCount(), 0);
        QCOMPARE(notices.count(), 1);
        QVERIFY(!m.canUndo());
    }

    void prescannedLinesAvailableOnlyWhenUnannotatedAndMatchingRect()
    {
        FakeReader reader;
        AnnotationModel m;
        m.setSmartHooks(reader.hooks());
        m.begin(noiseImage(QSize(400, 300), 3), 1.0, StyleMemory());
        const QRect selection(30, 40, 200, 160);
        m.setSelection(selection);
        QVERIFY(!m.prescannedLines(selection).has_value()); // not ready yet
        QTRY_COMPARE_WITH_TIMEOUT(reader.scans, 1, 1500);
        reader.answer(phoneLines());
        const auto ready = m.prescannedLines(selection);
        QVERIFY(ready.has_value());
        QCOMPARE(ready->size(), phoneLines().size());
        QVERIFY(!m.prescannedLines(selection.adjusted(0, 0, -1, 0)).has_value()); // different rect
        // Once annotated or redacted, pre-redaction lines must NOT be handed off to a pin!
        m.smartRedact();
        QVERIFY(m.itemCount() > 0);
        QVERIFY(!m.prescannedLines(selection).has_value());
        m.undo();
        QVERIFY(m.prescannedLines(selection).has_value());
    }

    // ---- the redaction tool on a line of read text (GitHub #129) ----

    void textLineClickHidesAndShowsAgain()
    {
        FakeReader reader;
        AnnotationModel m;
        m.setSmartHooks(reader.hooks());
        m.begin(noiseImage(QSize(400, 300), 3), 1.0, StyleMemory());
        const QRect selection(30, 40, 200, 160);
        m.setSelection(selection);
        QTRY_COMPARE_WITH_TIMEOUT(reader.scans, 1, 1500);
        reader.answer(phoneLines());
        QSignalSpy targets(&m, &AnnotationModel::textTargetChanged);
        const QPoint onFirst = selection.topLeft() + QPoint(60, 30); // the first line: 10..120 x 20..40
        const QPoint onSecond = selection.topLeft() + QPoint(60, 70);
        const QPoint offText = selection.topLeft() + QPoint(170, 30);
        // Without the tool the pointer marks nothing.
        m.pointerAt(onFirst);
        QVERIFY(m.textTarget().isEmpty());
        m.setTool(toolIndex(Kind::Redact));
        QCOMPARE(reader.scans, 1); // read already: not again
        // Over a line: its box, a margin about the letters, inside the selection.
        const QRectF target = m.textTarget();
        QVERIFY(!target.isEmpty());
        QCOMPARE(targets.count(), 1);
        QVERIFY(!m.textTargetHidden());
        QVERIFY(target.contains(QRectF(selection.topLeft() + QPoint(10, 20), QSize(110, 20))));
        QVERIFY(QRectF(selection).contains(target));
        QVERIFY(target.height() <= 20 + 2 * 3);
        m.pointerAt(offText);
        QVERIFY(m.textTarget().isEmpty());
        m.pointerAt(QPoint(-1, -1));
        QVERIFY(m.textTarget().isEmpty());
        // A click on the line hides it: one box redaction over it, one undo step, nothing selected.
        m.pointerAt(onFirst);
        click(m, onFirst);
        QCOMPARE(m.itemCount(), 1);
        const Item made = m.document().items[0];
        QCOMPARE(made.kind, Kind::Redact);
        QCOMPARE(made.style.shape, RedactShape::Box);
        QCOMPARE(QRectF(itemBox(made)), target);
        QVERIFY(made.style.cell >= int(0.3 * itemBox(made).height()));
        QCOMPARE(m.selectedCount(), 0);
        QVERIFY(m.textTargetHidden());
        QCOMPARE(m.smartFound(), 2); // 智能打码 offers the other two only
        // The next click shows it again; undo brings each step back.
        click(m, onFirst);
        QCOMPARE(m.itemCount(), 0);
        QVERIFY(!m.textTargetHidden());
        m.undo();
        QCOMPARE(m.itemCount(), 1);
        m.undo();
        QCOMPARE(m.itemCount(), 0);
        QVERIFY(!m.canUndo());
        // Two lines, two redactions; a click off the text makes nothing.
        click(m, onFirst);
        click(m, onSecond);
        click(m, offText);
        QCOMPARE(m.itemCount(), 2);
        // A drag is the tool's own box as ever, also when it starts on a line.
        drag(m, onFirst, onFirst + QPoint(90, 60));
        QCOMPARE(m.itemCount(), 3);
        QVERIFY(itemBox(m.document().items[2]).height() > 40);
        // A box moved off its line is an object like any other: a click on the line hides the line anew.
        m.undo();
        QCOMPARE(m.itemCount(), 2);
        // With another tool a click on the text is that tool's.
        m.setTool(toolIndex(Kind::Rect));
        m.pointerAt(onFirst);
        QVERIFY(m.textTarget().isEmpty());
        click(m, selection.topLeft() + QPoint(60, 110));
        QCOMPARE(m.itemCount(), 2);
    }

    // The scan off in the settings: choosing the tool reads the text, and once more when the selection moves.
    void textLineReadsWhenTheToolIsChosen()
    {
        FakeReader reader;
        reader.prescan = false;
        AnnotationModel m;
        m.setSmartHooks(reader.hooks());
        m.begin(noiseImage(QSize(400, 300), 3), 1.0, StyleMemory());
        const QRect selection(30, 40, 200, 160);
        m.setSelection(selection);
        QTest::qWait(450);
        QCOMPARE(reader.scans, 0);
        QSignalSpy notices(&m, &AnnotationModel::notice);
        m.setTool(toolIndex(Kind::Redact));
        QCOMPARE(reader.scans, 1);
        QVERIFY(!m.smartBusy()); // not the button's scan: nothing is hidden when it is done
        // Not read yet: a click is a click of the tool, nothing made.
        const QPoint onFirst = selection.topLeft() + QPoint(60, 30);
        m.pointerAt(onFirst);
        QVERIFY(m.textTarget().isEmpty());
        click(m, onFirst);
        QCOMPARE(m.itemCount(), 0);
        reader.answer(phoneLines());
        QCOMPARE(m.itemCount(), 0);
        QCOMPARE(notices.count(), 1); // "a click on text hides it", said once
        QVERIFY(!m.textTarget().isEmpty());
        click(m, onFirst);
        QCOMPARE(m.itemCount(), 1);
        // The selection moves with the tool on: read again, the old lines are of nothing.
        m.setSelection(selection.adjusted(0, 0, 40, 0));
        QVERIFY(m.textTarget().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(reader.scans, 2, 1500);
        reader.answer(phoneLines());
        QCOMPARE(notices.count(), 1);
        QVERIFY(!m.textTarget().isEmpty());
        // No text in it: no word about clicking text.
        m.setTool(-1);
        m.setSelection(selection);
        QTest::qWait(450);
        QCOMPARE(reader.scans, 2);
    }

    // The tool's remembered style is used (not a brush), and the editing window scans the crop.
    void smartRedactUsesTheToolsStyleAndTheEditorsCrop()
    {
        FakeReader reader;
        AnnotationModel m;
        m.setSmartHooks(reader.hooks());
        Project project;
        project.base = noiseImage(QSize(400, 300), 9);
        project.doc.canvas = project.base.size();
        project.doc.crop = QRect(50, 60, 220, 180);
        project.doc.dpr = 1.0;
        StyleMemory styles;
        Style s = styles.forTool(Kind::Redact, 1.0);
        s.redact = RedactMode::Solid;
        s.shape = RedactShape::Brush;
        s.color = 0xff112233u;
        styles.remember(Kind::Redact, s, 1.0);
        m.begin(project, 1.0, styles);
        QTRY_COMPARE_WITH_TIMEOUT(reader.scans, 1, 1500);
        QCOMPARE(reader.last.size(), QSize(220, 180));
        reader.answer(phoneLines());
        m.smartRedact();
        QCOMPARE(m.itemCount(), 3);
        const Item& item = m.document().items[0];
        QCOMPARE(item.style.redact, RedactMode::Solid);
        QCOMPARE(item.style.color, 0xff112233u);
        QCOMPARE(item.style.shape, RedactShape::Box); // a brush has no place on a line
        QVERIFY(QRect(50, 60, 220, 180).contains(itemBox(item)));
        // Cropping again reads again.
        m.undo();
        QCOMPARE(m.itemCount(), 0);
    }

    void toolKeysToggle()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        QSignalSpy toolChanged(&m, &AnnotationModel::toolChanged);
        const QList<ToolInfo>& list = tools();
        int expectedChanges = 0;
        for (qsizetype i = 0; i < list.size(); ++i) {
            const int letter = list[i].letter.unicode();
            QVERIFY(m.key(letter, Qt::NoModifier));
            QCOMPARE(m.tool(), int(i));
            QVERIFY(m.key(letter, Qt::NoModifier)); // the key that opened it closes it
            QCOMPARE(m.tool(), -1);
            expectedChanges += 2;
            if (list[i].digit >= 1 && list[i].digit <= 9) {
                const int digit = Qt::Key_0 + list[i].digit;
                QVERIFY(m.key(digit, Qt::NoModifier));
                QCOMPARE(m.tool(), int(i));
                if (list[i].kind == Kind::Redact) {
                    // Shift+B is 智能打码, not the tool: the tool stays on.
                    QVERIFY(m.key(letter, Qt::ShiftModifier));
                    QCOMPARE(m.tool(), int(i));
                    QVERIFY(m.key(letter, Qt::NoModifier));
                } else {
                    QVERIFY(m.key(letter, Qt::ShiftModifier)); // its letter closes what its digit opened
                }
                QCOMPARE(m.tool(), -1);
                QVERIFY(!m.key(digit, Qt::ControlModifier)); // Ctrl + digit is PixPin's
                expectedChanges += 2;
            } else {
                QVERIFY(m.key(letter, Qt::ShiftModifier));
                QCOMPARE(m.tool(), int(i));
                QVERIFY(m.key(letter, Qt::NoModifier));
                QCOMPARE(m.tool(), -1);
                expectedChanges += 2;
                QVERIFY(!m.key(Qt::Key_0, Qt::NoModifier));
            }
            QVERIFY(!m.key(letter, Qt::ControlModifier));
            QVERIFY(!m.key(letter, Qt::AltModifier));
            QCOMPARE(m.tool(), -1);
        }
        QCOMPARE(toolChanged.count(), expectedChanges);
        // Another tool's key switches to it.
        QVERIFY(m.key(Qt::Key_R, {}));
        QVERIFY(m.key(Qt::Key_E, {}));
        QCOMPARE(m.tool(), toolIndex(Kind::Ellipse));
        QVERIFY(!m.key(Qt::Key_X, {}));
    }

    // #57: typing goes on straight into a label to the right of a number put down,
    // and the number with its label is one undo step.
    void numberTakesWordsRightAway()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(50, 50));
        QVERIFY(m.editingText());
        const QVariantMap e = m.editing();
        QVERIFY(e.value(u"number"_s).toBool());
        QVERIFY(e.value(u"x"_s).toInt() > 50 + 10); // to the right of the disc
        QVERIFY(!e.value(u"bold"_s).toBool());
        QVERIFY(!e.value(u"boxed"_s).toBool());
        QCOMPARE(e.value(u"clickX"_s).toInt(), -1); // the caret at the end
        QCOMPARE(m.itemCount(), 1);
        QVERIFY(!m.canUndo()); // not a step of its own: it joins the words'
        QVERIFY(m.skipped() == std::nullopt); // the disc stays drawn
        QCOMPARE(m.unworded(), std::optional<quint32>(m.document().items[0].id));
        m.setEditingText(u"第一步 登录"_s);
        QVERIFY(m.escape()); // the Esc ladder of the text: the first Esc ends the words
        QVERIFY(!m.editingText());
        QCOMPARE(m.document().items[0].text, u"第一步 登录"_s);
        QVERIFY(m.canUndo());
        QCOMPARE(m.undoLabel(), u"添加"_s);
        m.undo(); // number and words together
        QCOMPARE(m.itemCount(), 0);
        QVERIFY(!m.canUndo());
        m.redo();
        QCOMPARE(m.itemCount(), 1);
        QCOMPARE(m.document().items[0].text, u"第一步 登录"_s);
        // The second Esc closes the active tool (and clears selection) so the
        // user can immediately click another annotation to edit it (#415).
        QVERIFY(m.escape());
        QCOMPARE(m.tool(), -1);
        QCOMPARE(m.selectedCount(), 0);
        QVERIFY(!m.escape()); // nothing active now: Esc goes to the host
        // The words are in the project: a round trip keeps them.
        const annot::Project project = m.project();
        QCOMPARE(project.doc.items[0].text, u"第一步 登录"_s);
    }

    void numberWithoutWordsHasNone()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(50, 50));
        QVERIFY(m.editingText());
        m.setEditingText(u"   "_s); // blanks are none
        m.commitPending();
        QVERIFY(m.document().items[0].text.isEmpty());
        QCOMPARE(m.itemCount(), 1);
        QCOMPARE(m.undoLabel(), u"添加"_s);
        m.undo();
        QCOMPARE(m.itemCount(), 0);
        QVERIFY(!m.canUndo());
    }

    // The next click is the next number: it ends the words and goes on, one click a number.
    void numbersKeepGoingWithoutExtraClicks()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(50, 50));
        m.setEditingText(u"A"_s);
        click(m, QPoint(150, 50)); // ends A's words and puts the second down
        QCOMPARE(m.itemCount(), 2);
        QVERIFY(m.editingText());
        QCOMPARE(m.document().items[0].text, u"A"_s);
        click(m, QPoint(250, 50));
        QCOMPARE(m.itemCount(), 3);
        m.commitPending();
        QCOMPARE(shownNumbers(m), (QList<int> {1, 2, 3}));
        for (int i = 3; i > 0; --i) { // a step each
            QVERIFY(m.canUndo());
            m.undo();
            QCOMPARE(m.itemCount(), i - 1);
        }
        QVERIFY(!m.canUndo());
        // Typing in the box takes its own presses: inside it nothing is put down.
        m.setTool(-1);
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(50, 150));
        const QVariantMap e = m.editing();
        const QPoint inside(e.value(u"x"_s).toInt() + 3, e.value(u"y"_s).toInt() + 3);
        press(m, inside);
        m.release(inside);
        QCOMPARE(m.itemCount(), 1);
        QVERIFY(m.editingText());
    }

    void numberWordsAreEditedAgain()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(50, 50));
        m.setEditingText(u"旧"_s);
        m.commitPending();
        // A click on the number just done with (it is selected) opens its words, the caret at the end.
        click(m, QPoint(50, 50));
        QVERIFY(m.editingText());
        QCOMPARE(m.editing().value(u"text"_s).toString(), u"旧"_s);
        m.setEditingText(u"新"_s);
        m.commitPending();
        QCOMPARE(m.document().items[0].text, u"新"_s);
        QCOMPARE(m.undoLabel(), u"改文字"_s);
        m.undo();
        QCOMPARE(m.document().items[0].text, u"旧"_s);
        m.redo();
        QCOMPARE(m.document().items[0].text, u"新"_s);
        // Emptied, the words are gone and the number stays.
        click(m, QPoint(50, 50));
        m.setEditingText({});
        m.commitPending();
        QVERIFY(m.document().items[0].text.isEmpty());
        QCOMPARE(m.itemCount(), 1);
        // Without a tool, a double click on the words edits them; on the disc it renumbers.
        click(m, QPoint(50, 50));
        m.setEditingText(u"再"_s);
        m.commitPending();
        m.setTool(-1);
        const annot::NumberGeometry g = annot::numberGeometry(m.document().items[0], 1, 1.0);
        QVERIFY(m.doubleClick(g.wordsBox.center()));
        QVERIFY(m.editingText());
        m.commitPending();
        QVERIFY(m.doubleClick(QPoint(50, 50)));
        QVERIFY(!m.editingText());
        QVERIFY(m.editingNumber());
    }

    // A click on a number the number tool has not selected only selects it, so that the keys act on it;
    // the second click, or one on its words, opens the words.
    void clickSelectsANumberBeforeItOpensItsWords()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(50, 50));
        m.setEditingText(u"x"_s);
        click(m, QPoint(250, 50)); // ends the words of the first and puts the second down
        m.commitPending();
        const quint32 first = m.document().items[0].id;
        const quint32 second = m.document().items[1].id;
        QCOMPARE(selectedId(m), second);
        click(m, QPoint(50, 50)); // the first is not the selected one: selected, not opened
        QVERIFY(!m.editingText());
        QCOMPARE(selectedId(m), first);
        click(m, QPoint(50, 50)); // the same again: its words
        QVERIFY(m.editingText());
        m.commitPending();
        // A click on the words themselves opens them at once.
        click(m, QPoint(250, 50));
        QCOMPARE(selectedId(m), second);
        const annot::NumberGeometry g = annot::numberGeometry(m.document().items[0], 1, 1.0);
        click(m, g.wordsBox.center());
        QVERIFY(m.editingText());
        QCOMPARE(m.editing().value(u"id"_s).toUInt(), first);
    }

    // The panel changes the number just put down before its words are done: undo takes the steps back in order.
    void restylingANumberWhileItsWordsAreTypedUndoesInOrder()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(50, 50));
        m.setEditingText(u"words"_s);
        m.setPanel(QVariantMap {{u"color"_s, u"#1E88E5"_s}});
        m.commitPending();
        const QRgb blue = qRgb(0x1E, 0x88, 0xE5);
        QCOMPARE(m.document().items[0].style.color, blue);
        QCOMPARE(m.document().items[0].text, u"words"_s);
        int steps = 0;
        while (m.canUndo() && steps < 10) {
            m.undo();
            ++steps;
        }
        QCOMPARE(m.itemCount(), 0);
        QVERIFY(steps >= 2 && steps <= 3);
        for (int i = 0; i < steps; ++i)
            m.redo();
        QCOMPARE(m.itemCount(), 1);
        QCOMPARE(m.document().items[0].style.color, blue);
        QCOMPARE(m.document().items[0].text, u"words"_s);
    }

    void numberFormatIsRememberedAndPerSeries()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Number));
        QCOMPARE(m.panel().value(u"format"_s).toString(), u"arabic"_s);
        click(m, QPoint(50, 50));
        m.commitPending();
        click(m, QPoint(100, 50));
        m.commitPending();
        m.setPanel(QVariantMap {{u"format"_s, u"roman"_s}});
        QCOMPARE(m.panel().value(u"format"_s).toString(), u"roman"_s);
        QCOMPARE(annot::formatOf(m.document(), 1), annot::NumberFormat::Roman);
        QCOMPARE(m.styles().numberFormat(), annot::NumberFormat::Roman);
        QVERIFY(m.stylesChanged());
        QCOMPARE(m.undoLabel(), u"改序号格式"_s);
        m.undo(); // the document's change goes back; the tool keeps its choice
        QCOMPARE(annot::formatOf(m.document(), 1), annot::NumberFormat::Arabic);
        QCOMPARE(m.styles().numberFormat(), annot::NumberFormat::Roman);
        m.redo();
        // A new group writes as the one before; a format changed on one group leaves the other alone.
        // The number just put down was selected: "另起一组" lets go of it, so the panel is for the new group
        // (live test 2026-10-22: the next format button changed the old group).
        QCOMPARE(selectedId(m), m.document().items.last().id);
        m.newSeries();
        QCOMPARE(selectedId(m), 0u);
        QCOMPARE(m.panel().value(u"format"_s).toString(), u"roman"_s);
        m.setPanel(QVariantMap {{u"format"_s, u"upper"_s}});
        QCOMPARE(annot::formatOf(m.document(), 1), annot::NumberFormat::Roman);
        QCOMPARE(annot::formatOf(m.document(), 2), annot::NumberFormat::UpperLetters);
        // While the words are typed, the panel still takes it.
        click(m, QPoint(150, 50));
        QVERIFY(m.editingText());
        m.setPanel(QVariantMap {{u"format"_s, u"circled"_s}});
        QCOMPARE(annot::formatOf(m.document(), 2), annot::NumberFormat::Circled);
        m.setPanel(QVariantMap {{u"format"_s, u"nonsense"_s}}); // unknown: nothing changes
        QCOMPARE(annot::formatOf(m.document(), 2), annot::NumberFormat::Circled);
        m.commitPending();
        // A selected number's group is the one that changes.
        m.setTool(-1);
        click(m, QPoint(350, 250)); // nothing under it: nothing selected
        QCOMPARE(selectedId(m), 0u);
        m.setPanel(QVariantMap {{u"format"_s, u"lower"_s}}); // no tool, nothing selected: no panel for it
        QCOMPARE(annot::formatOf(m.document(), 2), annot::NumberFormat::Circled);
        const quint32 first = m.document().items[0].id;
        click(m, pointOn(m, first));
        QCOMPARE(selectedId(m), first);
        m.setPanel(QVariantMap {{u"format"_s, u"lower"_s}});
        QCOMPARE(annot::formatOf(m.document(), 1), annot::NumberFormat::LowerLetters);
        QCOMPARE(annot::formatOf(m.document(), 2), annot::NumberFormat::Circled);
        // The next capture starts with the last format chosen.
        AnnotationModel next;
        next.begin(rig.base, 1.0, m.styles());
        QCOMPARE(annot::formatOf(next.document(), 1), annot::NumberFormat::LowerLetters);
        // ... and it survives the styles' JSON.
        QCOMPARE(StyleMemory::fromJson(m.styles().toJson()).numberFormat(), annot::NumberFormat::LowerLetters);
    }

    void numbersCountAndRenumber()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(50, 50));
        click(m, QPoint(100, 50));
        click(m, QPoint(150, 50));
        QCOMPARE(m.itemCount(), 3);
        QCOMPARE(shownNumbers(m), (QList<int> {1, 2, 3}));
        const quint32 first = m.document().items[0].id;
        const quint32 middle = m.document().items[1].id;
        const quint32 last = m.document().items[2].id;
        // The number tool picks a number up rather than putting one on it.
        click(m, QPoint(100, 50));
        QCOMPARE(m.itemCount(), 3);
        QCOMPARE(selectedId(m), middle);
        m.deleteSelection();
        QCOMPARE(shownNumbers(m), (QList<int> {1, 2}));
        m.setStart(5);
        QCOMPARE(shownNumbers(m), (QList<int> {5, 6}));
        QCOMPARE(m.panel().value(u"start"_s).toInt(), 5);
        m.setNumber(int(first), u"10"_s);
        QCOMPARE(shownNumbers(m), (QList<int> {10, 11}));
        m.setNumber(int(first), QString()); // counted again
        QCOMPARE(shownNumbers(m), (QList<int> {5, 6}));
        m.setNumber(int(last), 20);
        QCOMPARE(shownNumbers(m), (QList<int> {5, 20}));
        m.setNumber(int(last), u"abc"_s); // not a number: nothing changes
        QCOMPARE(shownNumbers(m), (QList<int> {5, 20}));
        m.newSeries();
        QCOMPARE(m.panel().value(u"start"_s).toInt(), 1);
        click(m, QPoint(200, 120));
        QCOMPARE(shownNumbers(m), (QList<int> {5, 20, 1}));
        // A double click opens a number's own value.
        m.setTool(-1);
        click(m, QPoint(50, 50));
        press(m, QPoint(50, 50));
        QVERIFY(m.doubleClick(QPoint(50, 50)));
        m.release(QPoint(50, 50));
        const QVariantMap edit = m.numberEdit();
        QCOMPARE(edit.value(u"id"_s).toUInt(), first);
        QCOMPARE(edit.value(u"value"_s).toInt(), -1);
        QCOMPARE(edit.value(u"shown"_s).toInt(), 5);
        QCOMPARE(m.document().items[0].points[0], QPointF(50.5, 50.5)); // the double click moved nothing
        m.setNumber(int(first), 7);
        QVERIFY(m.numberEdit().isEmpty());
        QCOMPARE(shownNumbers(m), (QList<int> {7, 20, 1}));
        // Undo and redo renumber by themselves.
        m.undo();
        QCOMPARE(shownNumbers(m), (QList<int> {5, 20, 1}));
        m.redo();
        QCOMPARE(shownNumbers(m), (QList<int> {7, 20, 1}));
    }

    void shiftConstraints()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(50, 50), QPoint(130, 80), Qt::ShiftModifier);
        QCOMPARE(itemBox(*lastOf(m, Kind::Rect)), QRect(50, 50, 81, 81));
        m.setTool(toolIndex(Kind::Ellipse));
        drag(m, QPoint(300, 200), QPoint(260, 190), Qt::ShiftModifier); // up and left
        QCOMPARE(itemBox(*lastOf(m, Kind::Ellipse)), QRect(QPoint(260, 160), QPoint(300, 200)));
        m.setTool(toolIndex(Kind::Line));
        drag(m, QPoint(50, 200), QPoint(150, 210), Qt::ShiftModifier); // 5.7 degrees: level
        const Item level = *lastOf(m, Kind::Line);
        QCOMPARE(level.points[0], QPointF(50.5, 200.5));
        QCOMPARE(level.points[1].y(), 200.5);
        QVERIFY(level.points[1].x() > 140);
        drag(m, QPoint(50, 280), QPoint(150, 240), Qt::ShiftModifier); // 21.8 degrees: 15
        QVERIFY(std::abs(degrees(*lastOf(m, Kind::Line)) - 15) < 0.7);
        m.setTool(toolIndex(Kind::Arrow));
        drag(m, QPoint(200, 280), QPoint(260, 200), Qt::ShiftModifier); // 53 degrees: 60
        QVERIFY(std::abs(degrees(*lastOf(m, Kind::Arrow)) - 60) < 0.7);
        // Shift on a pen: one straight line from the press.
        m.setTool(toolIndex(Kind::Pen));
        stroke(m, {QPoint(300, 50), QPoint(320, 70), QPoint(340, 55), QPoint(380, 62)});
        QVERIFY(lastOf(m, Kind::Pen)->points.size() > 2);
        drag(m, QPoint(300, 120), QPoint(380, 128), Qt::ShiftModifier);
        const Item straight = *lastOf(m, Kind::Pen);
        QCOMPARE(straight.points.size(), 2);
        QCOMPARE(straight.points[0].y(), straight.points[1].y());
        // Shift pressed while the mouse holds still shows at once.
        m.setTool(toolIndex(Kind::Rect));
        press(m, QPoint(200, 30));
        m.move(QPoint(260, 50), {});
        QCOMPARE(itemBox(m.document().items.last()), QRect(QPoint(200, 30), QPoint(260, 50)));
        QVERIFY(!m.key(Qt::Key_Shift, Qt::ShiftModifier));
        QCOMPARE(itemBox(m.document().items.last()), QRect(QPoint(200, 30), QPoint(260, 90)));
        m.release(QPoint(260, 50));
    }

    void selectionBoundsDrawing()
    {
        Rig rig(QSize(400, 300), 1.0, QRect(100, 100, 150, 100));
        AnnotationModel& m = rig.model;
        QSignalSpy notices(&m, &AnnotationModel::notice);
        m.setTool(toolIndex(Kind::Rect));
        QVERIFY(!m.pressFirst(QPoint(20, 20), {}));
        QVERIFY(m.pressSecond(QPoint(20, 20), {})); // taken, nothing done
        QVERIFY(!m.gestureActive());
        QCOMPARE(notices.count(), 1);
        m.move(QPoint(150, 150), {});
        m.release(QPoint(150, 150));
        QCOMPARE(m.itemCount(), 0);
        QVERIFY(!m.canUndo());
        for (qsizetype i = 0; i < tools().size(); ++i) {
            m.setTool(int(i));
            drag(m, QPoint(20, 280), QPoint(150, 150));
            QVERIFY(!m.editingText());
        }
        QCOMPARE(m.itemCount(), 0);
        QCOMPARE(notices.count(), 1 + int(tools().size()));
        // Inside, the drawing stays inside: the mouse's way out is clamped.
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(200, 150), QPoint(390, 290));
        QCOMPARE(itemBox(m.document().items.last()), QRect(QPoint(200, 150), QPoint(249, 199)));
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(150, 120), QPoint(390, 140), Qt::ShiftModifier); // the square cut by the selection
        QCOMPARE(itemBox(m.document().items.last()), QRect(QPoint(150, 120), QPoint(229, 199)));
        m.setTool(toolIndex(Kind::Line));
        drag(m, QPoint(120, 150), QPoint(390, 152), Qt::ShiftModifier); // level, shortened to the edge
        QCOMPARE(lastOf(m, Kind::Line)->points, (QList<QPointF> {QPointF(120.5, 150.5), QPointF(249.5, 150.5)}));
        m.setTool(toolIndex(Kind::Pen));
        stroke(m, {QPoint(150, 150), QPoint(200, 50), QPoint(390, 120), QPoint(220, 290)});
        const QRectF inside = QRectF(m.selectionRect());
        for (const QPointF& p : lastOf(m, Kind::Pen)->points)
            QVERIFY(inside.contains(p));
        // Drawing along the edge, out of the selection, makes nothing wider.
        m.setTool(toolIndex(Kind::Ellipse));
        const int count = m.itemCount();
        drag(m, QPoint(249, 120), QPoint(330, 121));
        QCOMPARE(m.itemCount(), count);
    }

    void escapeCancels()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(40, 40), QPoint(100, 100));
        const Document before = m.document();
        // A rect being drawn: Esc throws it away.
        press(m, QPoint(150, 150));
        m.move(QPoint(200, 190), {});
        QVERIFY(m.gestureActive());
        QVERIFY(!m.gestureBounds().isNull());
        QCOMPARE(m.itemCount(), 2);
        QVERIFY(m.escape());
        QVERIFY(!m.gestureActive());
        QVERIFY(m.gestureBounds().isNull());
        QCOMPARE(m.document(), before);
        m.release(QPoint(200, 190));
        QCOMPARE(m.document(), before);
        // A move: back where it was.
        m.setTool(-1);
        const quint32 id = before.items[0].id;
        const QPoint on = pointOn(m, id);
        press(m, on);
        m.move(on + QPoint(30, 30), {});
        QVERIFY(m.document() != before);
        QVERIFY(m.escape());
        QCOMPARE(m.document(), before);
        m.release(on + QPoint(30, 30));
        QCOMPARE(m.document(), before);
        // A reshape too.
        QCOMPARE(selectedId(m), id);
        press(m, QPoint(101, 101));
        m.move(QPoint(150, 120), {});
        QVERIFY(m.document() != before);
        m.cancelGesture();
        QCOMPARE(m.document(), before);
        m.release(QPoint(150, 120));
        // A number the press put down: gone again.
        m.setTool(toolIndex(Kind::Number));
        press(m, QPoint(300, 200));
        QCOMPARE(m.itemCount(), 2);
        QVERIFY(m.escape());
        QCOMPARE(m.document(), before);
        m.release(QPoint(300, 200));
        // The stack holds the first rect only.
        m.undo();
        QCOMPARE(m.itemCount(), 0);
        QVERIFY(!m.canUndo());
        QVERIFY(m.escape()); // turns off the active Number tool (#415)
        QCOMPARE(m.tool(), -1);
        QVERIFY(!m.escape()); // nothing active: the host's Esc

        // Secondary-editing workflow (#415): draw a rect, switch to text,
        // type, Esc commits text, Esc exits text tool, click rect to re-select,
        // Esc deselects rect, Esc hands off to host.
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(40, 40), QPoint(120, 100));
        const quint32 rectId = m.document().items.last().id;
        m.setTool(toolIndex(Kind::Text));
        click(m, QPoint(160, 60));
        m.setEditingText(u"说明"_s);
        QVERIFY(m.editingText());
        QVERIFY(m.escape()); // commits text
        QVERIFY(!m.editingText());
        QCOMPARE(m.tool(), toolIndex(Kind::Text));
        QVERIFY(m.escape()); // exits text tool into pointer/select mode
        QCOMPARE(m.tool(), -1);
        QCOMPARE(m.selectedCount(), 0);
        click(m, pointOn(m, rectId)); // immediately re-selects the earlier rect
        QCOMPARE(selectedId(m), rectId);
        QVERIFY(m.escape()); // deselects the rect
        QCOMPARE(m.selectedCount(), 0);
        QVERIFY(!m.escape()); // neutral: hands off to host
    }

    void textCommitAndEdit()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Text));
        click(m, QPoint(40, 40));
        QVERIFY(m.editingText());
        QVERIFY(m.panelVisible());
        QCOMPARE(m.panel().value(u"kind"_s).toInt(), int(Kind::Text));
        m.commitText(QString()); // a new text without words: dropped
        QVERIFY(!m.editingText());
        QCOMPARE(m.itemCount(), 0);
        QVERIFY(!m.canUndo());
        click(m, QPoint(40, 40));
        m.setEditingText(u"abc"_s);
        QCOMPARE(m.editing().value(u"text"_s).toString(), u"abc"_s);
        // A press outside the box commits it and does nothing else.
        QVERIFY(m.pressFirst(QPoint(300, 250), {}));
        m.release(QPoint(300, 250));
        QVERIFY(!m.editingText());
        QCOMPARE(m.itemCount(), 1);
        const Item text = m.document().items[0];
        QCOMPARE(text.text, u"abc"_s);
        QCOMPARE(text.points[0], QPointF(40, 40));
        QCOMPARE(selectedId(m), text.id);
        // A click on it with the text tool types in it again; the canvas
        // leaves it out meanwhile.
        const QPoint on = pointOn(m, text.id);
        QVERIFY(on.x() >= 0);
        click(m, on);
        QVERIFY(m.editingText());
        QVariantMap e = m.editing();
        QCOMPARE(e.value(u"id"_s).toUInt(), text.id);
        QCOMPARE(e.value(u"text"_s).toString(), u"abc"_s);
        QCOMPARE(e.value(u"clickX"_s).toInt(), on.x() - 40);
        QCOMPARE(e.value(u"clickY"_s).toInt(), on.y() - 40);
        QCOMPARE(m.skipped(), std::optional<quint32>(text.id));
        QVERIFY(m.selection().isEmpty());
        QVERIFY(!m.key(Qt::Key_Delete, {})); // the box has the keys
        QVERIFY(!m.key(Qt::Key_Z, Qt::ControlModifier));
        m.commitText(u"xyz"_s); // retyped
        QCOMPARE(m.document().items[0].text, u"xyz"_s);
        QVERIFY(!m.skipped());
        m.undo();
        QCOMPARE(m.document().items[0].text, u"abc"_s);
        // Emptied: deleted, one step.
        click(m, on);
        QVERIFY(m.editingText());
        m.commitText(u"  "_s);
        QCOMPARE(m.itemCount(), 0);
        m.undo();
        QCOMPARE(m.itemCount(), 1);
        // Unchanged: no step (the deletion can still be redone).
        click(m, on);
        QVERIFY(m.editingText());
        m.commitText(u"abc"_s);
        QVERIFY(m.canRedo());
        QCOMPARE(m.document().items[0].text, u"abc"_s);
        // Esc, the tool and commitPending() commit; lines come as '\n'.
        click(m, QPoint(200, 150));
        m.setEditingText(u"Esc"_s);
        QVERIFY(m.escape());
        QCOMPARE(m.itemCount(), 2);
        click(m, QPoint(200, 220));
        m.setEditingText(u"tool"_s);
        m.setTool(-1);
        QCOMPARE(m.itemCount(), 3);
        m.setTool(toolIndex(Kind::Text));
        click(m, QPoint(300, 100));
        m.setEditingText(u"one"_s + QChar(QChar::ParagraphSeparator) + u"two"_s);
        m.commitPending();
        QCOMPARE(m.itemCount(), 4);
        QCOMPARE(lastOf(m, Kind::Text)->text, u"one\ntwo"_s);
        // The text being typed takes the panel: a new one keeps its style.
        click(m, QPoint(300, 200));
        m.setPanel({{u"size"_s, 32}, {u"bold"_s, true}});
        e = m.editing();
        QCOMPARE(e.value(u"fontPx"_s).toInt(), 32);
        QVERIFY(e.value(u"bold"_s).toBool());
        QCOMPARE(m.styles().tool(Kind::Text).size, 32);
        m.commitText(u"big"_s);
        QCOMPARE(lastOf(m, Kind::Text)->style.fontPx, 32);
        QVERIFY(lastOf(m, Kind::Text)->style.bold);
        // A double click on a text without a tool opens it.
        m.setTool(-1);
        click(m, on);
        press(m, on);
        QVERIFY(m.doubleClick(on));
        m.release(on);
        QVERIFY(m.editingText());
        QCOMPARE(m.editing().value(u"id"_s).toUInt(), text.id);
        QVERIFY(m.doubleClick(on)); // while typing: the box's
        m.commitPending();
    }

    void panelAndStyleMemory()
    {
        Rig rig(QSize(400, 300), 1.5);
        AnnotationModel& m = rig.model;
        QCOMPARE(m.panel().value(u"kind"_s).toInt(), -1);
        QCOMPARE(m.panel().value(u"colors"_s).toList().size(), palette().size());
        QVERIFY(!m.panelVisible());
        m.setTool(toolIndex(Kind::Rect));
        QVERIFY(m.panelVisible());
        QVariantMap p = m.panel();
        QCOMPARE(p.value(u"kind"_s).toInt(), int(Kind::Rect));
        QCOMPARE(p.value(u"width"_s).toInt(), 4);
        QCOMPARE(p.value(u"widths"_s).toList().size(), sizePresets(Kind::Rect).size());
        QCOMPARE(p.value(u"fill"_s).toString(), u"none"_s);
        QCOMPARE(p.value(u"color"_s).toString(), u"#E53935"_s);
        QVERIFY(!p.contains(u"size"_s));
        QVERIFY(!p.value(u"selected"_s).toBool());
        // The tool's style: remembered in logical px, no step to undo.
        m.setPanel({{u"width"_s, 7}});
        QCOMPARE(m.panel().value(u"width"_s).toInt(), 7);
        QCOMPARE(m.styles().tool(Kind::Rect).width, 7);
        QVERIFY(m.stylesChanged());
        QVERIFY(!m.canUndo());
        drag(m, QPoint(40, 40), QPoint(100, 100));
        const quint32 first = lastOf(m, Kind::Rect)->id;
        QCOMPARE(lastOf(m, Kind::Rect)->style.width, 11); // 7 x 1.5, rounded
        // Selected: the panel shows the object's, and a change goes to both.
        QVERIFY(m.panel().value(u"selected"_s).toBool());
        QCOMPARE(m.panel().value(u"width"_s).toInt(), 7);
        m.setPanel({{u"width"_s, 2}, {u"fill"_s, u"soft"_s}});
        QCOMPARE(findItem(m.document(), first)->style.width, 3);
        QCOMPARE(findItem(m.document(), first)->style.fill, Fill::Soft);
        QCOMPARE(m.styles().tool(Kind::Rect).width, 2);
        QCOMPARE(m.styles().tool(Kind::Rect).fill, Fill::Soft);
        QVERIFY(m.canUndo());
        // Let go of, the tool shows its memory; changed, the memory only.
        m.setTool(-1);
        click(m, QPoint(300, 250));
        QVERIFY(m.selection().isEmpty());
        m.setTool(toolIndex(Kind::Rect));
        m.setPanel({{u"width"_s, 5}});
        QCOMPARE(findItem(m.document(), first)->style.width, 3);
        // The object, selected again, shows its own style, not the tool's.
        m.setTool(-1);
        click(m, pointOn(m, first));
        QCOMPARE(selectedId(m), first);
        QCOMPARE(m.panel().value(u"width"_s).toInt(), 2);
        QCOMPARE(m.styles().tool(Kind::Rect).width, 5);
        // Fields a kind does not use are not taken.
        m.setTool(toolIndex(Kind::Arrow));
        QVERIFY(m.selection().isEmpty()); // another kind's tool lets the rect go
        m.setPanel({{u"fill"_s, u"solid"_s}, {u"size"_s, 40}});
        QVERIFY(!m.panel().contains(u"fill"_s));
        QCOMPARE(m.styles().tool(Kind::Arrow), StyleMemory::defaults(Kind::Arrow));
        m.setPanel({{u"color"_s, u"#43A047"_s}});
        QCOMPARE(m.styles().tool(Kind::Arrow).color, qRgb(0x43, 0xA0, 0x47));
        // The redaction's panel: mode and cell.
        m.setTool(toolIndex(Kind::Redact));
        p = m.panel();
        QCOMPARE(p.value(u"mode"_s).toString(), u"mosaic"_s);
        QCOMPARE(p.value(u"cell"_s).toInt(), 12);
        m.setPanel({{u"mode"_s, u"blur"_s}});
        QCOMPARE(m.styles().tool(Kind::Redact).mode, RedactMode::Blur);
        // The memory comes back from begin().
        StyleMemory styles = m.styles();
        m.end();
        m.begin(rig.base, 1.0, styles);
        QVERIFY(!m.stylesChanged());
        QCOMPARE(m.styles(), styles);
        QCOMPARE(m.itemCount(), 0);
        QCOMPARE(m.tool(), -1);
    }

    // The text outline (#101) through the panel: the tool's memory, the selected text, undo, boxed, and a
    // screen kept up to date by the dirty regions alone.
    void textOutlinePanel()
    {
        Rig rig(QSize(400, 300), 1.5);
        AnnotationModel& m = rig.model;
        Screen screen(m);
        m.setTool(toolIndex(Kind::Text));
        QVariantMap p = m.panel();
        QCOMPARE(p.value(u"stroke"_s).toInt(), 0);
        QCOMPARE(p.value(u"strokes"_s).toList().size(), strokePresets().size());
        QVERIFY(p.value(u"strokeAuto"_s).toBool());
        QCOMPARE(p.value(u"strokeColor"_s).toString(), u"#FFFFFF"_s); // red words: white outline
        click(m, QPoint(100, 100));
        m.commitText(u"Win顺 outline"_s);
        const quint32 id = lastOf(m, Kind::Text)->id;
        QCOMPARE(findItem(m.document(), id)->style.textStroke, 0);
        CHECK_SCREEN(screen);
        const QRectF plain = bounds(m.document(), *findItem(m.document(), id));
        // Selected: the panel's change goes to the text and to the tool's memory.
        m.setTool(-1);
        click(m, pointOn(m, id));
        QCOMPARE(selectedId(m), id);
        m.setPanel({{u"stroke"_s, 2}});
        QCOMPARE(findItem(m.document(), id)->style.textStroke, 3); // 2 logical px at 150 %
        QCOMPARE(m.panel().value(u"stroke"_s).toInt(), 2);
        QCOMPARE(m.styles().tool(Kind::Text).stroke, 2);
        QVERIFY(bounds(m.document(), *findItem(m.document(), id)).width() > plain.width());
        CHECK_SCREEN(screen);
        m.setPanel({{u"strokeColor"_s, u"#1E88E5"_s}});
        QCOMPARE(findItem(m.document(), id)->style.textStrokeColor, qRgb(0x1E, 0x88, 0xE5));
        QVERIFY(!m.panel().value(u"strokeAuto"_s).toBool());
        QCOMPARE(m.panel().value(u"strokeColor"_s).toString(), u"#1E88E5"_s);
        CHECK_SCREEN(screen);
        m.setPanel({{u"strokeColor"_s, u"auto"_s}});
        QCOMPARE(findItem(m.document(), id)->style.textStrokeColor, QRgb(0));
        CHECK_SCREEN(screen);
        m.setPanel({{u"stroke"_s, 3}});
        CHECK_SCREEN(screen);
        // Undo and redo (the panel's changes may merge into fewer steps): the old outline back, nothing left of the grown one.
        int steps = 0;
        while (findItem(m.document(), id)->style.textStroke != 0 && m.canUndo() && steps++ < 10) {
            m.undo();
            CHECK_SCREEN(screen);
        }
        QCOMPARE(findItem(m.document(), id)->style.textStroke, 0);
        QCOMPARE(bounds(m.document(), *findItem(m.document(), id)), plain);
        while (m.canRedo() && steps-- > 0) {
            m.redo();
            CHECK_SCREEN(screen);
        }
        QCOMPARE(findItem(m.document(), id)->style.textStroke, 5);
        // Boxed words take no outline: it stays in the style, nothing is drawn.
        m.setPanel({{u"boxed"_s, true}});
        QVERIFY(m.panel().value(u"boxed"_s).toBool());
        CHECK_SCREEN(screen);
        // Other kinds do not take it.
        m.setTool(toolIndex(Kind::Number));
        m.setPanel({{u"stroke"_s, 2}});
        QVERIFY(!m.panel().contains(u"stroke"_s));
        QCOMPARE(m.styles().tool(Kind::Number).stroke, 0);
        // The new text starts with the remembered outline.
        m.setTool(toolIndex(Kind::Text));
        click(m, QPoint(100, 220));
        m.commitText(u"next"_s);
        QCOMPARE(lastOf(m, Kind::Text)->style.textStroke, 5);
        // The memory comes back with the next capture.
        const StyleMemory styles = m.styles();
        QVERIFY(styles.toJson().contains(u"\"stroke\":3"_s));
        QVERIFY(StyleMemory::fromJson(styles.toJson()) == styles);
    }

    void wheelSteps()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        QVERIFY(!m.wheel(120, 0)); // no tool, nothing selected: the session's
        m.setTool(toolIndex(Kind::Rect));
        const auto width = [&] { return m.panel().value(u"width"_s).toInt(); };
        QCOMPARE(width(), 4);
        QVERIFY(m.wheel(60, 1000));
        QCOMPARE(width(), 4); // half a notch
        QVERIFY(m.wheel(60, 1010));
        QCOMPARE(width(), 5); // a notch: one step
        QVERIFY(m.wheel(120, 1020));
        QCOMPARE(width(), 6); // a mouse's whole notch is a step at once, no waiting (#164)
        QVERIFY(m.wheel(120, 1030));
        QCOMPARE(width(), 7);
        QVERIFY(m.wheel(-120, 1500));
        QCOMPARE(width(), 6);
        QVERIFY(m.wheel(-40, 1800));
        QVERIFY(m.wheel(80, 1810)); // turned back: counted again from there
        QCOMPARE(width(), 6);
        QVERIFY(m.wheel(60, 1820));
        QCOMPARE(width(), 7); // a touchpad's pieces: one step ...
        QVERIFY(m.wheel(120 + 30, 1830)); // (not a whole number of notches)
        QCOMPARE(width(), 7); // ... and the next one waits 60 ms
        QVERIFY(m.wheel(150, 1900));
        QCOMPARE(width(), 8);
        QVERIFY(m.wheel(480, 2500)); // a spin: its notches
        QCOMPARE(width(), 12);
        // [ and ] step the same ladder.
        QVERIFY(m.key(Qt::Key_BracketRight, {}));
        QCOMPARE(width(), 13);
        QVERIFY(m.key(Qt::Key_BracketLeft, {}));
        QCOMPARE(width(), 12);
        m.setTool(toolIndex(Kind::Number));
        const int numberSize = m.panel().value(u"size"_s).toInt();
        QVERIFY(m.wheel(120, 3000));
        QCOMPARE(m.panel().value(u"size"_s).toInt(), numberSize + 1);
        m.setTool(toolIndex(Kind::Redact));
        const int cell = m.panel().value(u"cell"_s).toInt();
        QVERIFY(m.wheel(-120, 4000));
        QCOMPARE(m.panel().value(u"cell"_s).toInt(), cell - 1);
        // The ladder's ends hold, and it has a step for every value in between.
        m.setTool(toolIndex(Kind::Line));
        for (int i = 0; i < 80; ++i)
            m.key(Qt::Key_BracketRight, {});
        QCOMPARE(width(), 64);
        QVERIFY(m.wheel(1200, 4500));
        QCOMPARE(width(), 64);
        for (int i = 0; i < 80; ++i)
            m.key(Qt::Key_BracketLeft, {});
        QCOMPARE(width(), 1);
        QVERIFY(m.wheel(-1200, 4600));
        QCOMPARE(width(), 1);
        // A selected object: its own size, one undo step for the run.
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(40, 40), QPoint(100, 100));
        const quint32 id = selectedId(m);
        const int drawn = findItem(m.document(), id)->style.width;
        m.setTool(-1);
        QVERIFY(m.wheel(120, 10000));
        QVERIFY(m.wheel(120, 10300));
        QCOMPARE(findItem(m.document(), id)->style.width, drawn + 2);
        m.undo();
        QCOMPARE(findItem(m.document(), id)->style.width, drawn);
        // Mid-drag the wheel is still the model's, and changes nothing.
        press(m, pointOn(m, id));
        QVERIFY(m.wheel(120, 20000));
        m.release(pointOn(m, id));
        QCOMPARE(findItem(m.document(), id)->style.width, drawn);
    }

    void redactionsStayFirst()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(40, 40), QPoint(100, 100));
        const quint32 rect = selectedId(m);
        m.setTool(toolIndex(Kind::Redact));
        drag(m, QPoint(60, 60), QPoint(160, 120));
        const quint32 r1 = selectedId(m);
        QCOMPARE(kinds(m), (QList<Kind> {Kind::Redact, Kind::Rect}));
        drag(m, QPoint(200, 150), QPoint(260, 200));
        const quint32 r2 = selectedId(m);
        QCOMPARE(kinds(m), (QList<Kind> {Kind::Redact, Kind::Redact, Kind::Rect}));
        QCOMPARE(m.document().items[1].id, r2);
        // Raised to the top, a redaction stays under the rect.
        m.setTool(-1);
        click(m, pointOn(m, r1));
        QCOMPARE(selectedId(m), r1);
        QVERIFY(m.panel().value(u"canRaise"_s).toBool());
        QVERIFY(!m.panel().value(u"canLower"_s).toBool());
        m.raise(true);
        QCOMPARE(m.document().items[1].id, r1);
        QCOMPARE(m.document().items[2].id, rect);
        QVERIFY(!m.panel().value(u"canRaise"_s).toBool());
        QVERIFY(m.key(Qt::Key_BracketLeft, Qt::ControlModifier)); // a layer down
        QCOMPARE(m.document().items[0].id, r1);
        // Lowered to the bottom, the rect stays over them.
        click(m, pointOn(m, rect));
        QCOMPARE(selectedId(m), rect);
        QVERIFY(!m.panel().value(u"canLower"_s).toBool());
        QVERIFY(m.key(Qt::Key_BraceLeft, Qt::ControlModifier | Qt::ShiftModifier));
        m.lower(true);
        QCOMPARE(m.document().items[2].id, rect);
        // An arrow over the rect, lowered one layer: under it, over the redactions.
        m.setTool(toolIndex(Kind::Arrow));
        drag(m, QPoint(30, 200), QPoint(150, 250));
        const quint32 arrow = selectedId(m);
        m.lower(false);
        QCOMPARE(m.document().items[2].id, arrow);
        QCOMPARE(m.document().items[3].id, rect);
        m.undo();
        QCOMPARE(m.document().items[3].id, arrow);
    }

    void keysOnTheSelection()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        QVERIFY(!m.key(Qt::Key_Left, {})); // nothing selected: the session moves the selection
        QVERIFY(!m.key(Qt::Key_Delete, {}));
        QVERIFY(!m.key(Qt::Key_BracketRight, {}));
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(40, 40), QPoint(100, 100));
        const quint32 id = selectedId(m);
        const QRect box = itemBox(*findItem(m.document(), id));
        QVERIFY(m.key(Qt::Key_Right, {}));
        QVERIFY(m.key(Qt::Key_Down, Qt::ShiftModifier));
        QVERIFY(m.key(Qt::Key_Left, Qt::KeypadModifier));
        QCOMPARE(itemBox(*findItem(m.document(), id)), box.translated(0, 10));
        QVERIFY(m.key(Qt::Key_Z, Qt::ControlModifier)); // the run was one step
        QCOMPARE(itemBox(*findItem(m.document(), id)), box);
        QVERIFY(m.key(Qt::Key_Y, Qt::ControlModifier));
        QCOMPARE(itemBox(*findItem(m.document(), id)), box.translated(0, 10));
        QVERIFY(m.key(Qt::Key_Z, Qt::ControlModifier));
        QVERIFY(m.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier));
        QCOMPARE(itemBox(*findItem(m.document(), id)), box.translated(0, 10));
        QVERIFY(m.key(Qt::Key_Delete, {}));
        QCOMPARE(m.itemCount(), 0);
        QVERIFY(m.selection().isEmpty());
        QVERIFY(m.key(Qt::Key_Z, Qt::ControlModifier));
        QCOMPARE(m.itemCount(), 1);
        // While the button is held, the keys wait.
        m.setTool(-1);
        click(m, pointOn(m, id));
        const QPoint on = pointOn(m, id);
        press(m, on);
        m.move(on + QPoint(20, 0), {});
        QVERIFY(m.key(Qt::Key_Delete, {}));
        QVERIFY(m.key(Qt::Key_R, {}));
        QCOMPARE(m.tool(), -1);
        QCOMPARE(m.itemCount(), 1);
        m.release(on + QPoint(20, 0));
        QCOMPARE(itemBox(*findItem(m.document(), id)), box.translated(20, 10));
        QVERIFY(!m.key(Qt::Key_Tab, {}));
    }

    void pointerRules()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        QVERIFY(!m.doubleClick(QPoint(200, 200))); // the bare picture, no tool: the session copies
        QCOMPARE(m.cursorAt(QPoint(200, 200)), -1);
        QVERIFY(!m.pressFirst(QPoint(200, 200), {}));
        QVERIFY(!m.pressSecond(QPoint(200, 200), {})); // the session's press
        m.setTool(toolIndex(Kind::Rect));
        QCOMPARE(m.cursorAt(QPoint(200, 200)), int(Qt::CrossCursor));
        QVERIFY(m.doubleClick(QPoint(200, 200))); // with a tool: never a copy
        drag(m, QPoint(40, 40), QPoint(100, 100));
        const quint32 id = selectedId(m);
        QVERIFY(m.onHandle(QPoint(101, 101)));
        QCOMPARE(m.cursorAt(QPoint(101, 101)), int(Qt::SizeFDiagCursor));
        QCOMPARE(m.cursorAt(QPoint(101, 40)), int(Qt::SizeBDiagCursor));
        QCOMPARE(m.cursorAt(QPoint(70, 101)), int(Qt::SizeVerCursor));
        QCOMPARE(m.cursorAt(pointOn(m, id)), int(Qt::SizeAllCursor));
        QVERIFY(!m.onHandle(QPoint(200, 200)));
        m.setTool(-1);
        QCOMPARE(m.cursorAt(pointOn(m, id)), int(Qt::SizeAllCursor));
        QVERIFY(m.doubleClick(pointOn(m, id))); // on an object: no copy
        // A press on the blank picture lets the object go and is the session's.
        QVERIFY(!m.pressFirst(QPoint(300, 250), {}));
        QVERIFY(!m.pressSecond(QPoint(300, 250), {}));
        QVERIFY(m.selection().isEmpty());
        QVERIFY(!m.panelVisible());
        QVERIFY(!m.onHandle(QPoint(101, 101)));
        // A line's ends: the cursor follows its direction.
        m.setTool(toolIndex(Kind::Line));
        drag(m, QPoint(200, 50), QPoint(300, 50));
        QCOMPARE(m.cursorAt(QPoint(300, 50)), int(Qt::SizeHorCursor));
        drag(m, QPoint(250, 100), QPoint(330, 180));
        QCOMPARE(m.cursorAt(QPoint(330, 180)), int(Qt::SizeFDiagCursor));
        // A handle drag past the opposite side turns the box over.
        m.setTool(-1);
        click(m, pointOn(m, id));
        drag(m, QPoint(101, 101), QPoint(20, 20));
        QCOMPARE(itemBox(*findItem(m.document(), id)), QRect(QPoint(19, 19), QPoint(40, 40)));
    }

    void composedAndProject()
    {
        Rig rig(QSize(400, 300), 1.0, QRect(50, 40, 200, 150));
        AnnotationModel& m = rig.model;
        QVERIFY(m.composed().isNull()); // nothing drawn
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(60, 60), QPoint(120, 120));
        m.setTool(toolIndex(Kind::Text));
        click(m, QPoint(150, 100));
        m.setEditingText(u"pending"_s);
        const Project project = m.project(); // the text under way counts, uncommitted
        QVERIFY(m.editingText());
        QCOMPARE(project.doc.items.size(), 2);
        QCOMPARE(project.base.size(), QSize(200, 150));
        QCOMPARE(project.doc.items[1].text, u"pending"_s);
        const QImage picture = m.composed(); // commits it first
        QVERIFY(!m.editingText());
        QCOMPARE(m.itemCount(), 2);
        QCOMPARE(picture.size(), QSize(200, 150));
        Compositor other;
        const Diff d = compare(picture, other.exportImage(m.document(), m.base(), m.sampleBounds()));
        QVERIFY2(d.count == 0, describe(d).constData());
        m.end();
        QVERIFY(!m.active());
        QVERIFY(m.composed().isNull());
        QCOMPARE(m.itemCount(), 0);
        QVERIFY(!m.canUndo());
        QVERIFY(!m.pressSecond(QPoint(100, 100), {}));
        QVERIFY(!m.key(Qt::Key_R, {}));
    }

    void dirtyRegionsCoverChanges()
    {
        Rig rig(QSize(500, 360), 1.25, QRect(10, 10, 470, 330));
        AnnotationModel& m = rig.model;
        Screen screen(m);
        CHECK_SCREEN(screen);
        m.setTool(toolIndex(Kind::Redact));
        drag(m, QPoint(100, 100), QPoint(200, 160));
        CHECK_SCREEN(screen);
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(40, 40), QPoint(150, 130));
        m.setPanel({{u"fill"_s, u"soft"_s}});
        CHECK_SCREEN(screen);
        // The selection moves: the redaction takes other colours.
        m.setSelection(QRect(60, 20, 400, 320));
        CHECK_SCREEN(screen);
        // Strokes, checked while they are drawn and after.
        for (const Kind kind : {Kind::Pen, Kind::Marker}) {
            m.setTool(toolIndex(kind));
            const QList<QPoint> points = zigzag(QPoint(320, 300), 14, QPoint(-11, -14), QPoint(18, 0));
            press(m, points.first());
            for (qsizetype i = 1; i < points.size(); ++i) {
                m.move(points[i], {});
                if (i % 4 == 0)
                    CHECK_SCREEN(screen);
            }
            CHECK_SCREEN(screen);
            m.release(points.last());
            CHECK_SCREEN(screen);
        }
        // Numbers: deleting the first renumbers the others.
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(400, 60));
        click(m, QPoint(440, 60));
        click(m, QPoint(420, 100));
        CHECK_SCREEN(screen);
        m.setTool(-1);
        click(m, QPoint(400, 60));
        m.deleteSelection();
        CHECK_SCREEN(screen);
        m.undo();
        CHECK_SCREEN(screen);
        // A move, a reshape, an arrow drawn and moved, all undone and redone.
        const quint32 rect = lastOf(m, Kind::Rect)->id;
        const QPoint on = pointOn(m, rect);
        drag(m, on, on + QPoint(35, 25));
        CHECK_SCREEN(screen);
        const QVariantList marks = m.selection().value(0).toMap().value(u"handles"_s).toList();
        const QVariantMap corner = marks.value(4).toMap();
        const QPoint handle(qFloor(corner.value(u"x"_s).toReal()), qFloor(corner.value(u"y"_s).toReal()));
        drag(m, handle, handle + QPoint(40, 30));
        CHECK_SCREEN(screen);
        m.setTool(toolIndex(Kind::Arrow));
        drag(m, QPoint(250, 200), QPoint(420, 300));
        CHECK_SCREEN(screen);
        m.setTool(-1);
        const quint32 arrow = lastOf(m, Kind::Arrow)->id;
        drag(m, pointOn(m, arrow), pointOn(m, arrow) + QPoint(-60, -30));
        CHECK_SCREEN(screen);
        m.lower(true);
        CHECK_SCREEN(screen);
        for (int i = 0; i < 6; ++i) {
            m.undo();
            CHECK_SCREEN(screen);
        }
        for (int i = 0; i < 6; ++i) {
            m.redo();
            CHECK_SCREEN(screen);
        }
        // Typing: the old text is left out while its box is open.
        m.setTool(toolIndex(Kind::Text));
        click(m, QPoint(200, 250));
        m.commitText(u"Win顺 截图"_s);
        CHECK_SCREEN(screen);
        const quint32 text = lastOf(m, Kind::Text)->id;
        click(m, pointOn(m, text));
        QVERIFY(m.editingText());
        CHECK_SCREEN(screen);
        m.setPanel({{u"boxed"_s, true}});
        CHECK_SCREEN(screen);
        m.commitText(u"Win顺 截图 标注"_s);
        CHECK_SCREEN(screen);
        // Styles from the panel and the wheel.
        click(m, pointOn(m, text));
        m.commitText(u"Win顺 截图 标注"_s);
        m.setTool(-1);
        click(m, pointOn(m, rect));
        m.setPanel({{u"color"_s, u"#1E88E5"_s}, {u"width"_s, 12}});
        CHECK_SCREEN(screen);
        QVERIFY(m.wheel(-120, 0));
        CHECK_SCREEN(screen);
        // The capture ends: all of it.
        m.end();
        m.begin(rig.base, 1.0, StyleMemory());
        m.setSelection(QRect(0, 0, 500, 360));
        CHECK_SCREEN(screen);
    }

    // ---- the editing window (stage 4b) ----

    void editorBasics()
    {
        EditRig rig;
        AnnotationModel& m = rig.model;
        QVERIFY(m.editor());
        QVERIFY(m.active());
        QVERIFY(!m.modified());
        QCOMPARE(m.cropRect(), QRect(0, 0, 400, 300));
        QCOMPARE(m.sampleBounds(), QRect(0, 0, 400, 300));
        // Nothing drawn: the picture itself, pixel for pixel.
        QImage out = m.composed();
        QVERIFY(!out.isNull());
        Diff d = compare(out, rig.base);
        QVERIFY2(d.count == 0, describe(d).constData());
        // Drawing is allowed anywhere on the picture, its last pixel too.
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(300, 200), QPoint(399, 299));
        QCOMPARE(m.document().items.size(), qsizetype(1));
        QVERIFY(m.modified());
        // Undone to the start it is not modified: the document is the one opened.
        m.undo();
        QVERIFY(!m.modified());
        m.redo();
        QVERIFY(m.modified());
        // The project keeps the whole picture, even when the crop is small.
        m.setTool(-1);
        const Project project = m.project();
        QCOMPARE(project.base.size(), QSize(400, 300));
        QCOMPARE(project.doc.items.size(), qsizetype(1));
        // A text under way counts as a change.
        EditRig other;
        other.model.setTool(toolIndex(Kind::Text));
        click(other.model, QPoint(50, 50));
        QVERIFY(other.model.editingText());
        other.model.setEditingText(u"hi"_s);
        QVERIFY(other.model.modified());
        // The model lets go of everything at the end.
        m.end();
        QVERIFY(!m.editor());
        QVERIFY(!m.active());
    }

    void cropByTheEdges()
    {
        EditRig rig;
        AnnotationModel& m = rig.model;
        QSignalSpy cropped(&m, &AnnotationModel::cropChanged);
        const QRect canvas(QPoint(0, 0), rig.base.size());
        const QRect start = canvas;
        // The bottom right corner pulled in: undoable, one step.
        drag(m, QPoint(399, 299), QPoint(300, 200));
        const QRect want = dragHandle(start, Handle::BottomRight, QPoint(-99, -99), canvas);
        QCOMPARE(m.cropRect(), want);
        QVERIFY(want != canvas);
        QVERIFY(cropped.count() >= 1);
        QVERIFY(m.modified());
        QVERIFY(m.canUndo());
        QCOMPARE(m.document().items.size(), qsizetype(0));
        // What is exported is the crop; the picture and S are still all of it.
        QImage out = m.composed();
        QCOMPARE(out.size(), want.size());
        QCOMPARE(m.sampleBounds(), canvas);
        Diff d = compare(out, rig.base.copy(want));
        QVERIFY2(d.count == 0, describe(d).constData());
        QCOMPARE(m.project().base.size(), canvas.size());
        QCOMPARE(m.project().doc.crop, want);
        m.undo();
        QCOMPARE(m.cropRect(), canvas);
        QVERIFY(!m.modified());
        m.redo();
        QCOMPARE(m.cropRect(), want);
        // An edge from outside, past the picture: kept inside it.
        drag(m, QPoint(want.right() + 4, 100), QPoint(2000, 100));
        QCOMPARE(m.cropRect().right(), canvas.right());
        QCOMPARE(m.cropRect().height(), want.height());
        // Esc mid-drag puts it back.
        const QRect before = m.cropRect();
        press(m, QPoint(0, 0));
        m.move(QPoint(40, 30), {});
        QCOMPARE(m.cropRect().topLeft(), QPoint(40, 30));
        QVERIFY(m.gestureActive());
        QVERIFY(m.escape());
        QCOMPARE(m.cropRect(), before);

        // Reopened from its project, the cropped parts come back by dragging out.
        m.undo(); // the outward drag
        QCOMPARE(m.cropRect(), want);
        const Project project = m.project();
        const std::optional<Project> read = readProject(writeProject(project.doc, project.base), nullptr);
        QVERIFY(read.has_value());
        AnnotationModel again;
        again.begin(*read, 1.0, StyleMemory());
        QCOMPARE(again.cropRect(), want);
        QVERIFY(!again.modified());
        drag(again, QPoint(want.right(), want.bottom()), QPoint(450, 350));
        QCOMPARE(again.cropRect(), canvas);

        // With a tool on, a press inside the crop draws, even at its edge;
        // from outside the crop its handle is taken.
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(want.right() - 1, want.bottom() - 1), QPoint(want.right() - 20, want.bottom() - 20));
        QCOMPARE(m.cropRect(), want);
        QCOMPARE(m.document().items.size(), qsizetype(1));
        m.setTool(-1);
        click(m, QPoint(20, 20)); // lets go of the rectangle, whose handle would be taken first
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(want.right() + 3, want.bottom() + 3), QPoint(want.right() + 30, want.bottom() + 30));
        QVERIFY(m.cropRect() != want);
        QCOMPARE(m.document().items.size(), qsizetype(1));
    }

    void multiSelect()
    {
        EditRig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(40, 40), QPoint(100, 90));
        const quint32 a = m.document().items.last().id;
        drag(m, QPoint(160, 40), QPoint(220, 90));
        const quint32 b = m.document().items.last().id;
        m.setTool(toolIndex(Kind::Ellipse));
        drag(m, QPoint(40, 160), QPoint(100, 220));
        const quint32 c = m.document().items.last().id;
        m.setTool(-1);
        const auto picked = [&m] {
            QList<quint32> ids;
            for (const QVariant& v : m.selection())
                ids.append(v.toMap().value(u"id"_s).toUInt());
            std::sort(ids.begin(), ids.end());
            return ids;
        };
        // Shift adds, Shift again takes away; handles only for one object.
        click(m, pointOn(m, a));
        QCOMPARE(picked(), (QList<quint32>{a}));
        QVERIFY(!m.selection()[0].toMap().value(u"handles"_s).toList().isEmpty());
        press(m, pointOn(m, b), Qt::ShiftModifier);
        m.release(pointOn(m, b));
        QCOMPARE(picked(), (QList<quint32>{a, b}));
        QCOMPARE(m.selectedCount(), 2);
        for (const QVariant& v : m.selection())
            QVERIFY(v.toMap().value(u"handles"_s).toList().isEmpty());
        QVERIFY(!m.panel().value(u"canRaise"_s).toBool());
        QVERIFY(m.panel().value(u"many"_s).toBool());
        press(m, pointOn(m, b), Qt::ShiftModifier);
        m.release(pointOn(m, b));
        QCOMPARE(picked(), (QList<quint32>{a}));
        press(m, pointOn(m, b), Qt::ShiftModifier);
        m.release(pointOn(m, b));

        // Dragging one of them moves both, as one step.
        const QPointF a0 = findItem(m.document(), a)->points[0];
        const QPointF b0 = findItem(m.document(), b)->points[0];
        const int steps = m.document().items.size();
        Q_UNUSED(steps);
        drag(m, pointOn(m, a), pointOn(m, a) + QPoint(30, 25));
        QCOMPARE(findItem(m.document(), a)->points[0], a0 + QPointF(30, 25));
        QCOMPARE(findItem(m.document(), b)->points[0], b0 + QPointF(30, 25));
        QCOMPARE(picked(), (QList<quint32>{a, b}));
        m.undo();
        QCOMPARE(findItem(m.document(), a)->points[0], a0);
        QCOMPARE(findItem(m.document(), b)->points[0], b0);

        // A box over the empty picture selects what it touches.
        QSignalSpy marqueeSpy(&m, &AnnotationModel::marqueeChanged);
        press(m, QPoint(10, 10));
        QVERIFY(m.gestureActive());
        m.move(QPoint(240, 120), {});
        QVERIFY(!m.marquee().isNull());
        QCOMPARE(m.marquee().toRect(), QRect(QPoint(10, 10), QPoint(240, 120)));
        QCOMPARE(picked(), (QList<quint32>{a, b}));
        m.release(QPoint(240, 120));
        QVERIFY(m.marquee().isNull());
        QVERIFY(marqueeSpy.count() >= 2);
        QCOMPARE(picked(), (QList<quint32>{a, b}));
        // A box with Shift keeps them; without it starts afresh; a click on nothing lets go.
        press(m, QPoint(10, 150), Qt::ShiftModifier);
        m.move(QPoint(120, 240), Qt::ShiftModifier);
        m.release(QPoint(120, 240));
        QCOMPARE(picked(), (QList<quint32>{a, b, c}));
        click(m, QPoint(300, 280));
        QCOMPARE(m.selectedCount(), 0);
        drag(m, QPoint(10, 150), QPoint(120, 240));
        QCOMPARE(picked(), (QList<quint32>{c}));
        // Esc in the middle of a box gives the old selection back.
        click(m, pointOn(m, a));
        press(m, QPoint(250, 10), Qt::ShiftModifier);
        m.move(QPoint(390, 290), Qt::ShiftModifier);
        QVERIFY(m.escape());
        QCOMPARE(picked(), (QList<quint32>{a}));

        // All of them: Ctrl+A. The panel of mixed kinds has the colour and the layer buttons only.
        QVERIFY(m.key(Qt::Key_A, Qt::ControlModifier));
        QCOMPARE(picked(), (QList<quint32>{a, b, c}));
        const QVariantMap panel = m.panel();
        QCOMPARE(panel.value(u"kind"_s).toInt(), -1);
        QVERIFY(panel.contains(u"color"_s));
        QVERIFY(!panel.contains(u"width"_s));
        QVERIFY(panel.value(u"selected"_s).toBool());
        // One colour for all: one step, and each kind's tool remembers it.
        m.setPanel({{u"color"_s, u"#1E88E5"_s}});
        for (quint32 id : {a, b, c})
            QCOMPARE(findItem(m.document(), id)->style.color, qRgb(0x1E, 0x88, 0xE5));
        QCOMPARE(m.styles().tool(Kind::Rect).color, qRgb(0x1E, 0x88, 0xE5));
        QCOMPARE(m.styles().tool(Kind::Ellipse).color, qRgb(0x1E, 0x88, 0xE5));
        m.undo();
        for (quint32 id : {a, b, c})
            QCOMPARE(findItem(m.document(), id)->style.color, qRgb(0xE5, 0x39, 0x35));
        // Delete takes them all, in one step.
        QVERIFY(m.key(Qt::Key_Delete, {}));
        QCOMPARE(m.document().items.size(), qsizetype(0));
        m.undo();
        QCOMPARE(m.document().items.size(), qsizetype(3));
    }

    void duplicateSelected()
    {
        EditRig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(40, 40), QPoint(100, 90));
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(200, 60));
        m.setTool(-1);
        const quint32 rect = m.document().items.first().id;
        click(m, QPoint(300, 280)); // lets go of the number just drawn
        QVERIFY(!m.key(Qt::Key_D, Qt::ControlModifier)); // nothing selected
        click(m, pointOn(m, rect));
        const Item before = *findItem(m.document(), rect);
        QVERIFY(m.key(Qt::Key_D, Qt::ControlModifier));
        QCOMPARE(m.document().items.size(), qsizetype(3));
        // The copy is on top, a little aside, and the one selected.
        const Item& copy = m.document().items.last();
        QVERIFY(copy.id != rect);
        QVERIFY(copy.seq > before.seq);
        QCOMPARE(copy.style, before.style);
        const QPointF offset = copy.points[0] - before.points[0];
        QVERIFY(offset.x() > 0 && offset.x() == offset.y());
        QCOMPARE(copy.points[1] - before.points[1], offset);
        QCOMPARE(m.selectedCount(), 1);
        QCOMPARE(selectedId(m), copy.id);
        QCOMPARE(*findItem(m.document(), rect), before);
        // Both together: copies of both, in layer order, selected.
        QVERIFY(m.key(Qt::Key_A, Qt::ControlModifier));
        QVERIFY(m.key(Qt::Key_D, Qt::ControlModifier));
        QCOMPARE(m.document().items.size(), qsizetype(3 + 3));
        QCOMPARE(m.selectedCount(), 3);
        QSet<quint32> ids;
        for (const Item& item : m.document().items)
            ids.insert(item.id);
        QCOMPARE(ids.size(), qsizetype(6));
        // Numbers counted on: a copy is another number, not the same twice.
        QCOMPARE(shownNumbers(m).size(), 2);
        QCOMPARE(shownNumbers(m), (QList<int>{1, 2}));
        // One step each.
        m.undo();
        QCOMPARE(m.document().items.size(), qsizetype(3));
        m.undo();
        QCOMPARE(m.document().items.size(), qsizetype(2));
    }

    void eyedropper()
    {
        EditRig rig;
        AnnotationModel& m = rig.model;
        // Nothing for the colour to go to: no eyedropper.
        QVERIFY(!m.key(Qt::Key_I, {}));
        QVERIFY(!m.picking());
        m.setTool(toolIndex(Kind::Rect));
        QSignalSpy spy(&m, &AnnotationModel::pickingChanged);
        QVERIFY(m.key(Qt::Key_I, {}));
        QVERIFY(m.picking());
        QCOMPARE(m.cursorAt(QPoint(5, 5)), int(Qt::CrossCursor));
        // Esc leaves it.
        QVERIFY(m.escape());
        QVERIFY(!m.picking());
        QVERIFY(m.key(Qt::Key_I, {}));
        QVERIFY(m.key(Qt::Key_I, {})); // I again turns it off
        QVERIFY(!m.picking());
        // The press takes the colour as drawn: the picture under it.
        QVERIFY(m.key(Qt::Key_I, {}));
        const QPoint at(123, 77);
        QVERIFY(m.pressFirst(at, {}));
        m.release(at);
        QVERIFY(!m.picking());
        QCOMPARE(m.panel().value(u"color"_s).toString().toUpper(), colorHex(rig.base.pixel(at) | 0xff000000u).toUpper());
        QCOMPARE(m.document().items.size(), qsizetype(0)); // no rectangle was begun
        QVERIFY(spy.count() >= 2);
        // And what is drawn there: a solid box shows its own colour.
        drag(m, QPoint(200, 100), QPoint(260, 160));
        m.setPanel({{u"fill"_s, u"solid"_s}, {u"color"_s, u"#43A047"_s}});
        m.setTool(-1);
        click(m, pointOn(m, lastOf(m, Kind::Rect)->id));
        QVERIFY(m.key(Qt::Key_I, {}));
        QVERIFY(m.pressFirst(QPoint(230, 130), {}));
        QCOMPARE(findItem(m.document(), lastOf(m, Kind::Rect)->id)->style.color, qRgb(0x43, 0xA0, 0x47));
        // The selected object takes it.
        QVERIFY(m.key(Qt::Key_I, {}));
        QVERIFY(m.pressFirst(QPoint(230, 130), {}));
        QCOMPARE(m.panel().value(u"color"_s).toString().toUpper(), u"#43A047"_s);
    }

    void discardingAllChanges()
    {
        EditRig rig;
        AnnotationModel& m = rig.model;
        m.discardChanges(); // nothing to give up
        QVERIFY(!m.canUndo());
        m.setTool(toolIndex(Kind::Arrow));
        drag(m, QPoint(30, 30), QPoint(150, 90));
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(200, 200));
        m.setTool(-1);
        drag(m, QPoint(399, 299), QPoint(350, 250)); // a crop
        QVERIFY(m.modified());
        const Document changed = m.document();
        m.discardChanges();
        QVERIFY(!m.modified());
        QCOMPARE(m.document().items.size(), qsizetype(0));
        QCOMPARE(m.cropRect(), QRect(0, 0, 400, 300));
        // One Ctrl+Z gets it all back, a second Esc finds nothing to give up.
        QVERIFY(m.canUndo());
        m.undo();
        QCOMPARE(m.document(), changed);
        QVERIFY(m.modified());
        m.redo();
        QVERIFY(!m.modified());
        const int before = m.canRedo();
        m.discardChanges();
        QCOMPARE(int(m.canRedo()), before);
        // The canvas redraws what was given up: the compositor's picture is the base again.
        Compositor fresh;
        QImage out;
        fresh.render(m.document(), m.base(), m.sampleBounds(), QRect(QPoint(0, 0), m.base().size()), &out);
        const Diff d = compare(out, rig.base);
        QVERIFY2(d.count == 0, describe(d).constData());
    }

    void handsFollowTheZoom()
    {
        EditRig rig(QSize(600, 400), 1.5);
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Line));
        drag(m, QPoint(100, 200), QPoint(400, 200), {}, 10);
        m.setTool(-1);
        click(m, QPoint(500, 350)); // let go
        const QPoint near(250, 200 + 14);
        // At 100 % on a 150 % screen a line is touched within 6 image px (plus half its width).
        m.setView(1.5, 1.0);
        QCOMPARE(m.cursorAt(near), -1);
        // Zoomed out to a quarter, the same screen distance is four times as many image px.
        m.setView(1.5, 0.25);
        QCOMPARE(m.cursorAt(near), int(Qt::SizeAllCursor));
        // Zoomed in to 800 %, it is a fraction of a pixel.
        m.setView(1.5, 8.0);
        QCOMPARE(m.cursorAt(QPoint(250, 200 + 8)), -1);
    }

    void limitedCanvasKeepsTheView()
    {
        const QSize size(600, 20000); // 3 columns x 79 rows of tiles
        Rig big(size);
        AnnotationModel& m = big.model;
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(40, 10100), QPoint(300, 10300));
        drag(m, QPoint(40, 100), QPoint(300, 300));
        m.setTool(-1);

        const auto expected = [&](int top) {
            QImage out;
            Compositor fresh;
            fresh.render(m.document(), m.base(), m.sampleBounds(), QRect(0, top, 600, 800), &out);
            return out;
        };
        const int ring = AnnotationCanvas::kRing;
        const auto tilesFor = [&](int top) {
            const QRect r = ws::capture::editview::tileRange(QRectF(0, top, 600, 800), size, AnnotationCanvas::kTile, ring);
            return r.width() * r.height();
        };
        // The tiles come a few milliseconds at a time: frames until it is all there.
        const auto settle = [](QQuickWindow& window) {
            QImage got;
            for (int i = 0; i < 40; ++i) {
                got = window.grabWindow().convertToFormat(QImage::Format_RGB32);
                QCoreApplication::processEvents();
            }
            return got;
        };

        QQuickWindow window;
        window.setColor(QColor::fromRgb(kMagenta));
        window.resize(QSize(600, 800));
        AnnotationCanvas canvas(window.contentItem());
        canvas.setSize(QSizeF(size));
        canvas.setLimited(true);
        canvas.setModel(&m);
        canvas.setViewport(QRectF(0, 0, 600, 800));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // The top of the picture: only a screen's worth of tiles, not 237, and the right pixels.
        QImage got = settle(window);
        Diff d = compare(got, expected(0));
        QVERIFY2(d.count == 0, describe(d).constData());
        QCOMPARE(canvas.tilesShown(), tilesFor(0));
        QVERIFY(canvas.tilesShown() < 30);

        // Far down: the ones above are let go of, those in view drawn.
        canvas.setY(-10000);
        canvas.setViewport(QRectF(0, 10000, 600, 800));
        settle(window);
        QCOMPARE(canvas.tilesShown(), tilesFor(10000));
        // A change in view is drawn again where it shows, and not where it does not.
        m.setTool(toolIndex(Kind::Ellipse));
        drag(m, QPoint(350, 10150), QPoint(500, 10250));
        drag(m, QPoint(350, 150), QPoint(500, 250));
        m.setTool(-1);
        settle(window);
        QCOMPARE(canvas.tilesShown(), tilesFor(10000));
        canvas.setY(0);
        canvas.setViewport(QRectF(0, 0, 600, 800));
        settle(window);
        QCOMPARE(canvas.tilesShown(), tilesFor(0));
        // Not limited: all of them, as in the overlay.
        canvas.setLimited(false);
        settle(window);
        QCOMPARE(canvas.tilesShown(), 3 * 79);
        m.end();
        settle(window);
        QCOMPARE(canvas.tilesShown(), 0);

        // The same picture looked at from far down on a canvas of its own: the pixels in view.
        Rig other(size);
        other.model.setTool(toolIndex(Kind::Rect));
        drag(other.model, QPoint(40, 10100), QPoint(300, 10300));
        other.model.setTool(-1);
        QQuickWindow window2;
        window2.setColor(QColor::fromRgb(kMagenta));
        window2.resize(QSize(600, 800));
        AnnotationCanvas canvas2(window2.contentItem());
        canvas2.setSize(QSizeF(size));
        canvas2.setY(-10000);
        canvas2.setLimited(true);
        canvas2.setViewport(QRectF(0, 10000, 600, 800));
        canvas2.setModel(&other.model);
        window2.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window2));
        got = settle(window2);
        QImage want;
        Compositor fresh;
        fresh.render(other.model.document(), other.model.base(), other.model.sampleBounds(), QRect(0, 10000, 600, 800), &want);
        d = compare(got, want);
        QVERIFY2(d.count == 0, describe(d).constData());
        QCOMPARE(canvas2.tilesShown(), tilesFor(10000));
    }

    // The eyedropper reads a pixel as a 1 x 1 area of the compositor; without objects that is the base's pixel.
    void onePixelAreas()
    {
        EditRig rig(QSize(1080, 1400));
        QImage base(1080, 1400, QImage::Format_RGB32);
        for (int y = 0; y < base.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(base.scanLine(y));
            for (int x = 0; x < base.width(); ++x)
                line[x] = qRgb(y & 255, ((y >> 8) & 255) * 2 + (x % 7), (x * 5 + y * 3) & 255);
        }
        Project project;
        project.base = base;
        project.doc.canvas = base.size();
        project.doc.crop = QRect(QPoint(0, 0), base.size());
        AnnotationModel model;
        model.begin(project, 1.5, StyleMemory());
        int wrong = 0;
        QPoint first;
        for (int y = 0; y < base.height(); y += 3) {
            for (int x = 0; x < base.width(); x += 5) {
                QImage out;
                model.compositor().render(model.document(), model.base(), model.sampleBounds(), QRect(x, y, 1, 1), &out);
                if (out.isNull() || (out.pixel(0, 0) | 0xff000000u) != base.pixel(x, y)) {
                    if (!wrong)
                        first = QPoint(x, y);
                    ++wrong;
                }
            }
        }
        QVERIFY2(wrong == 0, qPrintable(u"%1 one pixel areas are not the base's pixel, the first at %2,%3"_s.arg(wrong).arg(first.x()).arg(first.y())));
        QImage out;
        model.compositor().render(model.document(), model.base(), model.sampleBounds(), QRect(353, 1037, 1, 1), &out);
        QCOMPARE(out.pixel(0, 0) | 0xff000000u, base.pixel(353, 1037));
    }

    void canvasTiles()
    {
        const QSize size(600, 400); // 3 x 2 tiles, the last column and row cut short
        Rig rig(size);
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Redact));
        drag(m, QPoint(230, 30), QPoint(300, 120));
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(40, 40), QPoint(270, 200));
        m.setPanel({{u"fill"_s, u"soft"_s}});
        m.setTool(toolIndex(Kind::Arrow));
        drag(m, QPoint(300, 300), QPoint(480, 270));
        m.setTool(toolIndex(Kind::Pen));
        stroke(m, zigzag(QPoint(100, 310), 9, QPoint(12, 3), QPoint(0, 40)));
        m.setTool(toolIndex(Kind::Number));
        click(m, QPoint(560, 60));
        m.setTool(toolIndex(Kind::Text));
        click(m, QPoint(320, 150));
        m.commitText(u"Win顺 标注"_s);
        m.setTool(-1);

        QQuickWindow window;
        window.setColor(QColor::fromRgb(kMagenta));
        window.resize(size);
        AnnotationCanvas canvas(window.contentItem());
        canvas.setSize(QSizeF(size));
        canvas.setModel(&m);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto grab = [&window] { return window.grabWindow().convertToFormat(QImage::Format_RGB32); };
        const auto expected = [&m, size] {
            QImage out;
            Compositor fresh;
            fresh.render(m.document(), m.base(), m.sampleBounds(), QRect(QPoint(0, 0), size), &out, m.skipped());
            return out;
        };
        const auto tileRect = [size](int column, int row) {
            return QRect(column * 256, row * 256, 256, 256) & QRect(QPoint(0, 0), size);
        };
        const auto reached = [&m](const QRect& tile) {
            const QHash<quint32, int> n = numbers(m.document());
            for (const Item& item : m.document().items) {
                if (m.skipped() == item.id)
                    continue;
                if (bounds(item, m.dpr(), n.value(item.id, 1)).toAlignedRect().intersects(tile))
                    return true;
            }
            return false;
        };

        QImage got = grab();
        QCOMPARE(got.size(), size);
        Diff d = compare(got, expected());
        QVERIFY2(d.count == 0, describe(d).constData());
        QCOMPARE(canvas.tilesShown(), 6);

        // A stroke being drawn: the tiles it reaches are drawn again.
        m.setTool(toolIndex(Kind::Marker));
        const QList<QPoint> points = zigzag(QPoint(520, 380), 16, QPoint(-16, -14), QPoint(0, 25));
        press(m, points.first());
        for (qsizetype i = 1; i < points.size(); ++i) {
            m.move(points[i], {});
            if (i % 5 == 0) {
                d = compare(grab(), expected());
                QVERIFY2(d.count == 0, describe(d).constData());
            }
        }
        m.release(points.last());
        d = compare(grab(), expected());
        QVERIFY2(d.count == 0, describe(d).constData());
        m.undo();
        m.setTool(-1);

        // Sparse: only the tiles something reaches; elsewhere what is beneath.
        canvas.setSparse(true);
        got = grab();
        QImage want = expected();
        int reachedTiles = 0;
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 3; ++column) {
                const QRect tile = tileRect(column, row);
                if (reached(tile)) {
                    ++reachedTiles;
                    d = compare(got, want, tile);
                    QVERIFY2(d.count == 0, describe(d).constData());
                } else {
                    QImage beneath(size, QImage::Format_RGB32);
                    beneath.fill(kMagenta);
                    d = compare(got, beneath, tile);
                    QVERIFY2(d.count == 0, describe(d).constData());
                }
            }
        }
        QCOMPARE(reachedTiles, 5); // the bottom right tile has nothing
        QCOMPARE(canvas.tilesShown(), reachedTiles);

        // A text being typed is left out, and its tile with it if nothing else is there.
        const quint32 text = lastOf(m, Kind::Text)->id;
        press(m, pointOn(m, text));
        QVERIFY(m.doubleClick(pointOn(m, text)));
        m.release(pointOn(m, text));
        QVERIFY(m.editingText());
        want = expected();
        got = grab();
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 3; ++column) {
                const QRect tile = tileRect(column, row);
                if (reached(tile)) {
                    d = compare(got, want, tile);
                    QVERIFY2(d.count == 0, describe(d).constData());
                }
            }
        }
        m.commitPending();

        // Everything undone: no tile left.
        while (m.canUndo())
            m.undo();
        got = grab();
        QCOMPARE(canvas.tilesShown(), 0);
        QImage beneath(size, QImage::Format_RGB32);
        beneath.fill(kMagenta);
        d = compare(got, beneath);
        QVERIFY2(d.count == 0, describe(d).constData());
        while (m.canRedo())
            m.redo();
        canvas.setSparse(false);
        d = compare(grab(), expected());
        QVERIFY2(d.count == 0, describe(d).constData());

        // Twice the size: every image pixel a 2 x 2 block.
        window.resize(size * 2);
        canvas.setSize(QSizeF(size * 2));
        canvas.setScaleX(2);
        canvas.setScaleY(2);
        got = grab();
        want = expected();
        QCOMPARE(got.size(), size * 2);
        int off = 0;
        for (int y = 0; y < got.height(); ++y) {
            for (int x = 0; x < got.width(); ++x) {
                if ((got.pixel(x, y) | 0xff000000u) != want.pixel(x / 2, y / 2))
                    ++off;
            }
        }
        QCOMPARE(off, 0);

        // The capture ends: the canvas lets go of its tiles.
        m.end();
        got = grab();
        QCOMPARE(canvas.tilesShown(), 0);
        QCOMPARE(got.pixel(10, 10) | 0xff000000u, kMagenta);
    }

    void textLiveEditBorderDragAndUndo()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        Screen screen(m);
        m.setTool(toolIndex(Kind::Text));
        click(m, QPoint(100, 100));
        QVERIFY(m.editingText());
        const int token1 = m.editing().value(u"token"_s).toInt();
        QVERIFY(token1 > 0);
        m.setEditingText(u"你好世界"_s);

        // Inside the typing area: IBeamCursor, not on border handle, borderHot stays false.
        const QPoint inside(108, 108);
        m.pointerAt(inside);
        QCOMPARE(m.cursorAt(inside), int(Qt::IBeamCursor));
        QVERIFY(!m.onHandle(inside));
        QVERIFY(!m.editing().value(u"borderHot"_s).toBool());

        // On the live edit border ring (both inner padding and outer reach): SizeAllCursor + onHandle + borderHot.
        for (const QPoint border : {QPoint(95, 108), QPoint(91, 108), QPoint(110, 95), QPoint(110, 91)}) {
            m.pointerAt(border);
            QCOMPARE(m.cursorAt(border), int(Qt::SizeAllCursor));
            QVERIFY(m.onHandle(border));
            QVERIFY(m.editing().value(u"borderHot"_s).toBool());
        }

        // Far outside the edit frame: IBeamCursor, not on border handle, borderHot turns off.
        m.pointerAt(QPoint(300, 250));
        QCOMPARE(m.cursorAt(QPoint(300, 250)), int(Qt::IBeamCursor));
        QVERIFY(!m.onHandle(QPoint(300, 250)));
        QVERIFY(!m.editing().value(u"borderHot"_s).toBool());

        // Drag the live text box by its border without committing or resetting its edit token.
        const QPoint grab(95, 108);
        press(m, grab);
        QVERIFY(m.gestureActive());
        QCOMPARE(m.cursorAt(grab), int(Qt::SizeAllCursor));
        QVERIFY(!m.gestureBounds().isEmpty());
        m.move(grab + QPoint(45, 30), {});
        QCOMPARE(m.editing().value(u"token"_s).toInt(), token1);
        QCOMPARE(m.editing().value(u"x"_s).toInt(), 145);
        QCOMPARE(m.editing().value(u"y"_s).toInt(), 130);
        m.release(grab + QPoint(45, 30));
        QVERIFY(!m.gestureActive());
        QVERIFY(m.editingText());
        QCOMPARE(m.itemCount(), 0);
        QCOMPARE(m.editing().value(u"token"_s).toInt(), token1);
        QCOMPARE(m.editing().value(u"text"_s).toString(), u"你好世界"_s);

        // Esc mid-drag snaps the live text box back and keeps editing open.
        const QPoint grab2(140, 138);
        press(m, grab2);
        m.move(grab2 + QPoint(35, -20), {});
        QCOMPARE(m.editing().value(u"x"_s).toInt(), 180);
        QCOMPARE(m.editing().value(u"y"_s).toInt(), 110);
        QVERIFY(m.escape());
        QVERIFY(!m.gestureActive());
        QVERIFY(m.editingText());
        QCOMPARE(m.editing().value(u"x"_s).toInt(), 145);
        QCOMPARE(m.editing().value(u"y"_s).toInt(), 130);

        // Commit creates the item at the dragged position in one undo step.
        m.commitText(u"你好世界"_s);
        QVERIFY(!m.editingText());
        QCOMPARE(m.itemCount(), 1);
        const quint32 id = m.document().items[0].id;
        QCOMPARE(m.document().items[0].points[0], QPointF(145, 130));
        CHECK_SCREEN(screen);
        m.undo();
        QCOMPARE(m.itemCount(), 0);
        CHECK_SCREEN(screen);
        m.redo();
        QCOMPARE(m.itemCount(), 1);
        QCOMPARE(m.document().items[0].points[0], QPointF(145, 130));

        // Re-edit existing text and move-only via its live border: commits as MoveItems, undoable.
        click(m, QPoint(155, 138));
        QVERIFY(m.editingText());
        const int token2 = m.editing().value(u"token"_s).toInt();
        QVERIFY(token2 > token1);
        drag(m, QPoint(140, 138), QPoint(120, 153));
        QVERIFY(m.editingText());
        QCOMPARE(m.editing().value(u"token"_s).toInt(), token2);
        QCOMPARE(m.editing().value(u"x"_s).toInt(), 125);
        QCOMPARE(m.editing().value(u"y"_s).toInt(), 145);
        m.commitText(u"你好世界"_s);
        QCOMPARE(m.document().items[0].points[0], QPointF(125, 145));
        QCOMPARE(m.undoLabel(), u"移动"_s);
        CHECK_SCREEN(screen);
        m.undo();
        QCOMPARE(m.document().items[0].points[0], QPointF(145, 130));
        CHECK_SCREEN(screen);
        m.redo();
        QCOMPARE(m.document().items[0].points[0], QPointF(125, 145));

        // Re-edit existing text: BOTH retype and drag live border -> single atomic undo/redo step.
        click(m, QPoint(135, 153));
        QVERIFY(m.editingText());
        m.setEditingText(u"已更新"_s);
        drag(m, QPoint(120, 153), QPoint(130, 128));
        QVERIFY(m.editingText());
        QCOMPARE(m.editing().value(u"x"_s).toInt(), 135);
        QCOMPARE(m.editing().value(u"y"_s).toInt(), 120);
        m.commitPending();
        QCOMPARE(findItem(m.document(), id)->text, u"已更新"_s);
        QCOMPARE(findItem(m.document(), id)->points[0], QPointF(135, 120));
        CHECK_SCREEN(screen);
        m.undo();
        QCOMPARE(findItem(m.document(), id)->text, u"你好世界"_s);
        QCOMPARE(findItem(m.document(), id)->points[0], QPointF(125, 145));
        CHECK_SCREEN(screen);
        m.redo();
        QCOMPARE(findItem(m.document(), id)->text, u"已更新"_s);
        QCOMPARE(findItem(m.document(), id)->points[0], QPointF(135, 120));
        CHECK_SCREEN(screen);

        // Live measured size & zoomed view: border follows measured size and zoom factor.
        click(m, QPoint(145, 128));
        QVERIFY(m.editingText());
        m.setEditingContentSize(180, 50);
        QCOMPARE(m.cursorAt(QPoint(135 + 170, 120 + 25)), int(Qt::IBeamCursor));
        QCOMPARE(m.cursorAt(QPoint(135 + 184, 120 + 25)), int(Qt::SizeAllCursor));
        m.setView(1.5, 0.5); // factor = 3.0 -> grab reach expands in image px to stay constant on screen
        QCOMPARE(m.cursorAt(QPoint(135 - 24, 120 + 25)), int(Qt::SizeAllCursor));
        m.commitPending();
    }

    void textToolHoverAndDragCommitted()
    {
        Rig rig;
        AnnotationModel& m = rig.model;
        m.setTool(toolIndex(Kind::Text));

        // Create two committed text items; after committing B, A is unselected while Text tool stays active.
        click(m, QPoint(60, 60));
        m.commitText(u"第一段"_s);
        const quint32 a = m.document().items[0].id;
        click(m, QPoint(60, 160));
        m.commitText(u"第二段"_s);
        const quint32 b = m.document().items[1].id;
        QCOMPARE(selectedId(m), b);

        const QRect boxA = textGeometry(*findItem(m.document(), a)).box;
        const QRect boxB = textGeometry(*findItem(m.document(), b)).box;

        // Hovering UNSELECTED A (both inside and on its outer selection border) shows SizeAllCursor.
        QCOMPARE(m.cursorAt(boxA.center()), int(Qt::SizeAllCursor));
        const QPoint borderA(boxA.left() - 2, boxA.center().y());
        QCOMPARE(m.cursorAt(borderA), int(Qt::SizeAllCursor));

        // Hovering SELECTED B right on its blue selection border (2px outside box) also shows SizeAllCursor.
        const QPoint borderB(boxB.right() + 2, boxB.center().y());
        QCOMPARE(m.cursorAt(borderB), int(Qt::SizeAllCursor));

        // Dragging unselected A directly from its outer border moves A without spawning a new text box
        // and without exiting the Text tool.
        drag(m, borderA, borderA + QPoint(50, 20));
        QVERIFY(!m.editingText());
        QCOMPARE(m.itemCount(), 2);
        QCOMPARE(selectedId(m), a);
        QCOMPARE(findItem(m.document(), a)->points[0], QPointF(110, 80));
        QCOMPARE(m.tool(), toolIndex(Kind::Text));

        // Dragging B from its outer border moves B too.
        drag(m, borderB, borderB + QPoint(25, -15));
        QVERIFY(!m.editingText());
        QCOMPARE(m.itemCount(), 2);
        QCOMPARE(selectedId(m), b);
        QCOMPARE(findItem(m.document(), b)->points[0], QPointF(85, 145));

        // Clicking A without dragging still enters edit mode cleanly.
        click(m, QPoint(118, 88));
        QVERIFY(m.editingText());
        QCOMPARE(m.editing().value(u"id"_s).toUInt(), a);
        m.commitPending();
    }

    void spotlightModelAndUndo()
    {
        AnnotationModel m;
        QImage base(300, 200, QImage::Format_RGB32);
        base.fill(qRgb(210, 210, 210));
        m.begin(base, 1.0, StyleMemory());
        m.setSelection(base.rect());

        // Key G selects Spotlight tool.
        QVERIFY(m.key(Qt::Key_G, Qt::NoModifier));
        QCOMPARE(m.tool(), toolIndex(Kind::Spotlight));
        QCOMPARE(m.panel().value(u"spotlightShape"_s).toString(), u"rect"_s);
        QCOMPARE(m.panel().value(u"spotlightOpacity"_s).toInt(), 60);

        // Draw first rectangular spotlight.
        drag(m, QPoint(20, 20), QPoint(90, 80));
        QCOMPARE(m.itemCount(), 1);
        const quint32 id1 = m.document().items[0].id;
        QVERIFY(!m.panel().value(u"canRaise"_s).toBool());
        QVERIFY(!m.panel().value(u"canLower"_s).toBool());

        // Switch next spotlight shape to ellipse and draw second cutout (discontinuous highlight).
        m.setPanel({{u"spotlightShape"_s, u"ellipse"_s}});
        QCOMPARE(m.document().items[0].style.spotlightShape, SpotlightShape::Ellipse);
        // Flip first back to rect, deselect, pick ellipse for second.
        m.setPanel({{u"spotlightShape"_s, u"rect"_s}});
        m.setTool(-1);
        m.pressSecond(QPoint(5, 5), Qt::NoModifier); // click outside to deselect
        m.release(QPoint(5, 5));
        m.setTool(toolIndex(Kind::Spotlight));
        m.setPanel({{u"spotlightShape"_s, u"ellipse"_s}});
        drag(m, QPoint(160, 90), QPoint(240, 170));
        QCOMPARE(m.itemCount(), 2);
        QCOMPARE(m.document().items[0].style.spotlightShape, SpotlightShape::Rect);
        QCOMPARE(m.document().items[1].style.spotlightShape, SpotlightShape::Ellipse);

        // Changing veil opacity to 100 % updates ALL spotlights atomically and Ctrl+Z reverts both!
        m.setPanel({{u"spotlightOpacity"_s, 100}});
        QCOMPARE(m.document().items[0].style.spotlightOpacity, 100);
        QCOMPARE(m.document().items[1].style.spotlightOpacity, 100);
        QImage exported = m.composed();
        QCOMPARE(exported.pixel(50, 50), qRgb(210, 210, 210));
        QCOMPARE(exported.pixel(200, 130), qRgb(210, 210, 210));
        QCOMPARE(exported.pixel(125, 100), qRgb(0, 0, 0));

        m.undo();
        QCOMPARE(m.document().items[0].style.spotlightOpacity, 60);
        QCOMPARE(m.document().items[1].style.spotlightOpacity, 60);

        // [ and ] step veil opacity in 10 % increments.
        QVERIFY(m.key(Qt::Key_BracketRight, Qt::NoModifier));
        QCOMPARE(m.document().items[0].style.spotlightOpacity, 70);
        QCOMPARE(m.document().items[1].style.spotlightOpacity, 70);
        QVERIFY(m.key(Qt::Key_BracketLeft, Qt::NoModifier));
        QCOMPARE(m.document().items[0].style.spotlightOpacity, 60);

        // Selecting a spotlight by clicking inside its cutout works, and regular shapes stay above it.
        m.setTool(toolIndex(Kind::Rect));
        drag(m, QPoint(30, 30), QPoint(70, 60));
        QCOMPARE(m.itemCount(), 3);
        QCOMPARE(m.document().items[2].kind, Kind::Rect);
        QVERIFY(!m.panel().value(u"canLower"_s).toBool()); // cannot lower below spotlights

        m.setTool(-1);
        m.pressSecond(QPoint(80, 70), Qt::NoModifier); // inside spot1, outside the smaller rect
        m.release(QPoint(80, 70));
        QCOMPARE(selectedId(m), id1);
    }

    void qmlAnnotateObjectsLiveTextBorderDrag();
};

class StubTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent CONSTANT)
    Q_PROPERTY(QColor accentFill READ accent CONSTANT)
    Q_PROPERTY(QColor accentText READ accentText CONSTANT)
    Q_PROPERTY(QColor danger READ danger CONSTANT)
    Q_PROPERTY(QColor background READ background CONSTANT)
    Q_PROPERTY(QColor textSelection READ textSelection CONSTANT)
    Q_PROPERTY(QColor subtext READ subtext CONSTANT)
    Q_PROPERTY(QColor text READ text CONSTANT)
    Q_PROPERTY(int fontBody READ fontBody CONSTANT)
    Q_PROPERTY(int inputField READ inputField CONSTANT)
public:
    QColor accent() const { return QColor(u"#1E88E5"_s); }
    QColor accentText() const { return QColor(u"#FFFFFF"_s); }
    QColor danger() const { return QColor(u"#E53935"_s); }
    QColor background() const { return QColor(u"#FFFFFF"_s); }
    QColor textSelection() const { return QColor(u"#BBDEFB"_s); }
    QColor subtext() const { return QColor(u"#757575"_s); }
    QColor text() const { return QColor(u"#212121"_s); }
    int fontBody() const { return 14; }
    int inputField() const { return 1; }
};

class StubHook : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int part MEMBER part)
    Q_PROPERTY(bool focused MEMBER focused)
    Q_PROPERTY(bool hovered MEMBER hovered)
    Q_PROPERTY(qreal dpr MEMBER dpr)
public:
    int part = 0;
    bool focused = false;
    bool hovered = false;
    qreal dpr = 1.0;
    Q_INVOKABLE QVariant out(const QString&, const QVariant& fallback) const { return fallback; }
};

class StubHost : public QObject {
    Q_OBJECT
    Q_PROPERTY(int cursorShape READ cursorShape NOTIFY cursorChanged)
public:
    explicit StubHost(AnnotationModel* model)
        : m_model(model)
    {
        for (const auto change : {&AnnotationModel::toolChanged, &AnnotationModel::gestureChanged,
                 &AnnotationModel::editingChanged, &AnnotationModel::selectionChanged})
            connect(m_model, change, this, &StubHost::refreshCursor);
    }
    int cursorShape() const { return m_cursor; }
    Q_INVOKABLE void pressed(qreal x, qreal y, int mods)
    {
        const QPoint p(qRound(x), qRound(y));
        m_pressed = true;
        if (!m_model->pressFirst(p, Qt::KeyboardModifiers(mods)))
            m_model->pressSecond(p, Qt::KeyboardModifiers(mods));
        hoverAt(p);
    }
    Q_INVOKABLE void moved(qreal x, qreal y, int mods)
    {
        const QPoint p(qRound(x), qRound(y));
        if (m_pressed || m_model->gestureActive())
            m_model->move(p, Qt::KeyboardModifiers(mods));
        hoverAt(p);
    }
    Q_INVOKABLE void released(qreal x, qreal y)
    {
        const QPoint p(qRound(x), qRound(y));
        m_pressed = false;
        m_model->release(p);
        hoverAt(p);
    }
    Q_INVOKABLE void exited()
    {
        m_hover.reset();
        m_model->pointerAt(QPoint(-1, -1));
    }
signals:
    void cursorChanged();
private:
    void hoverAt(QPoint p)
    {
        m_hover = p;
        m_model->pointerAt(p);
        const int s = m_model->cursorAt(p);
        const int next = s >= 0 ? s : int(Qt::ArrowCursor);
        if (next != m_cursor) {
            m_cursor = next;
            emit cursorChanged();
        }
    }
    void refreshCursor()
    {
        if (const QPoint p = m_model->pointer(); p.x() >= 0)
            hoverAt(p);
        else if (m_hover)
            hoverAt(*m_hover);
    }
    AnnotationModel* m_model = nullptr;
    bool m_pressed = false;
    int m_cursor = Qt::ArrowCursor;
    std::optional<QPoint> m_hover;
};

void TestAnnotModel::qmlAnnotateObjectsLiveTextBorderDrag()
{
    static StubTheme theme;
    qmlRegisterSingletonInstance("WinShun", 1, 0, "Theme", &theme);
    qmlRegisterType<StubHook>("WinShun", 1, 0, "Hook");
    qmlRegisterUncreatableType<AnnotationModel>("WinShun", 1, 0, "AnnotationModel", u"test"_s);

    Rig rig(QSize(480, 360), 1.0, QRect(0, 0, 480, 360));
    AnnotationModel& m = rig.model;
    m.setTool(toolIndex(Kind::Text));
    StubHost host(&m);

    QStringList warnings;
    static QStringList* sink = nullptr;
    sink = &warnings;
    const QtMessageHandler prev = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
        if ((type == QtWarningMsg || type == QtCriticalMsg) && sink)
            sink->append(msg);
        Q_UNUSED(ctx);
    });
    auto restore = qScopeGuard([prev] {
        sink = nullptr;
        qInstallMessageHandler(prev);
    });

    QQmlEngine engine;
    const QUrl objectsUrl = QUrl::fromLocalFile(QStringLiteral(WINSHUN_CAPTURE_QML) + u"/AnnotateObjects.qml"_s);
    engine.rootContext()->setContextProperty(u"testModel"_s, &m);
    engine.rootContext()->setContextProperty(u"testHost"_s, &host);
    engine.rootContext()->setContextProperty(u"objectsUrl"_s, objectsUrl);

    QQmlComponent comp(&engine);
    comp.setData(R"(
        import QtQuick
        import WinShun
        Window {
            id: win
            width: 480; height: 360
            Item { id: keys; focus: true }
            MouseArea {
                id: canvasMouse
                objectName: "canvasMouse"
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                cursorShape: testHost.cursorShape
                onPositionChanged: (ev) => testHost.moved(ev.x, ev.y, ev.modifiers)
                onPressed: (ev) => testHost.pressed(ev.x, ev.y, ev.modifiers)
                onReleased: (ev) => testHost.released(ev.x, ev.y)
                onEntered: testHost.moved(mouseX, mouseY, 0)
                onExited: testHost.exited()
            }
            Loader {
                id: loader
                objectName: "objectsLoader"
                anchors.fill: parent
                Component.onCompleted: setSource(objectsUrl, {
                    annot: testModel,
                    keyTarget: keys,
                    sx: 1,
                    sy: 1,
                    dpr: 1
                })
                onLoaded: item.anchors.fill = loader
            }
        }
    )", QUrl());
    QVERIFY2(!comp.isError(), qPrintable(comp.errorString()));
    std::unique_ptr<QQuickWindow> win(qobject_cast<QQuickWindow*>(comp.create()));
    QVERIFY(win);
    win->show();
    QVERIFY(QTest::qWaitForWindowExposed(win.get()));
    QCoreApplication::processEvents();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u'\n')));
    QQuickItem* loader = win->findChild<QQuickItem*>(u"objectsLoader"_s);
    QVERIFY(loader);
    QVERIFY2(loader->property("item").value<QObject*>() != nullptr, qPrintable(warnings.join(u'\n')));

    // Click to create a new live TextEdit inside AnnotateObjects.qml.
    QTest::mouseClick(win.get(), Qt::LeftButton, Qt::NoModifier, QPoint(120, 100));
    QTRY_VERIFY(m.editingText());
    QCoreApplication::processEvents();

    // Locate the real TextEdit inside AnnotateObjects.qml (loaded via Loader's visual tree).
    QQuickItem* objects = loader->property("item").value<QQuickItem*>();
    QVERIFY(objects);
    QQuickItem* edit = nullptr;
    std::function<void(QQuickItem*)> walk = [&](QQuickItem* node) {
        if (edit || !node)
            return;
        if (QString::fromLatin1(node->metaObject()->className()).contains(u"TextEdit"_s)) {
            edit = node;
            return;
        }
        for (QQuickItem* child : node->childItems())
            walk(child);
    };
    walk(objects);
    QVERIFY(edit);
    QVERIFY(edit->property("selectByMouse").toBool());
    edit->setProperty("text", u"拖拽测试"_s);
    QCoreApplication::processEvents();
    edit->setProperty("cursorPosition", 2);
    QTRY_COMPARE(m.editing().value(u"text"_s).toString(), u"拖拽测试"_s);
    const int token = m.editing().value(u"token"_s).toInt();
    QVERIFY(token > 0);

    // Move real mouse onto the live edit frame border just outside TextEdit (left side).
    const QPoint border(114, 108);
    QTest::mouseMove(win.get(), border);
    QCoreApplication::processEvents();
    QCOMPARE(host.cursorShape(), int(Qt::SizeAllCursor));
    QVERIFY(m.editing().value(u"borderHot"_s).toBool());

    // Drag the border by (+55, +40): live TextEdit moves without losing text, caret, or token!
    QTest::mousePress(win.get(), Qt::LeftButton, Qt::NoModifier, border);
    QTest::mouseMove(win.get(), border + QPoint(25, 20));
    QTest::mouseMove(win.get(), border + QPoint(55, 40));
    QTest::mouseRelease(win.get(), Qt::LeftButton, Qt::NoModifier, border + QPoint(55, 40));
    QCoreApplication::processEvents();

    QVERIFY(m.editingText());
    QCOMPARE(m.editing().value(u"token"_s).toInt(), token);
    QCOMPARE(m.editing().value(u"x"_s).toInt(), 175);
    QCOMPARE(m.editing().value(u"y"_s).toInt(), 140);
    QCOMPARE(edit->property("text").toString(), u"拖拽测试"_s);
    QCOMPARE(edit->property("cursorPosition").toInt(), 2);

    // Start a second border drag and hit Esc mid-drag: drag cancels, TextEdit stays open.
    const QPoint border2(169, 148);
    QTest::mousePress(win.get(), Qt::LeftButton, Qt::NoModifier, border2);
    QTest::mouseMove(win.get(), border2 + QPoint(30, 30));
    QCoreApplication::processEvents();
    QCOMPARE(m.editing().value(u"x"_s).toInt(), 205);
    QTest::keyClick(win.get(), Qt::Key_Escape);
    QCoreApplication::processEvents();
    QVERIFY(m.editingText());
    QCOMPARE(m.editing().value(u"x"_s).toInt(), 175);
    QCOMPARE(m.editing().value(u"y"_s).toInt(), 140);
    QTest::mouseRelease(win.get(), Qt::LeftButton, Qt::NoModifier, border2 + QPoint(30, 30));

    // Move onto border (borderHot = true), then into TextEdit: borderHot turns off and pointer stays fresh.
    QTest::mouseMove(win.get(), border2);
    QCoreApplication::processEvents();
    QVERIFY(m.editing().value(u"borderHot"_s).toBool());
    QTest::mouseMove(win.get(), QPoint(185, 148));
    QCoreApplication::processEvents();
    QCOMPARE(m.pointer(), QPoint(185, 148));
    QVERIFY(!m.editing().value(u"borderHot"_s).toBool());
    QTest::keyClick(win.get(), Qt::Key_Escape);
    QCoreApplication::processEvents();
    QVERIFY(!m.editingText());
    QCOMPARE(m.itemCount(), 1);
    QCOMPARE(m.document().items[0].points[0], QPointF(175, 140));
    QCOMPARE(host.cursorShape(), int(Qt::SizeAllCursor));

    // Drag the committed text box directly from its outer border while Text tool is still active.
    const QPoint committedBorder(173, 148);
    QTest::mouseMove(win.get(), committedBorder);
    QCoreApplication::processEvents();
    QCOMPARE(host.cursorShape(), int(Qt::SizeAllCursor));
    QTest::mousePress(win.get(), Qt::LeftButton, Qt::NoModifier, committedBorder);
    QTest::mouseMove(win.get(), committedBorder + QPoint(-25, 20));
    QTest::mouseRelease(win.get(), Qt::LeftButton, Qt::NoModifier, committedBorder + QPoint(-25, 20));
    QCoreApplication::processEvents();
    QVERIFY(!m.editingText());
    QCOMPARE(m.document().items[0].points[0], QPointF(150, 160));

    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u'\n')));
}

int main(int argc, char** argv)
{
    // The canvas is grabbed from an offscreen window on the software scene
    // graph; offscreen reads the fonts with FreeType from a folder, which
    // on Windows is this one.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    if (!qEnvironmentVariableIsSet("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", (qEnvironmentVariable("WINDIR", u"C:\\Windows"_s) + u"\\Fonts"_s).toLocal8Bit());
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    TestAnnotModel test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_annotmodel.moc"

#include "AnnotationModel.h"
#include <QLineF>

#include "Geometry.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegion>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

using annot::Kind;

// What [ and ] and a notch of the wheel step through, logical px (3.5): the
// next of these above or below the value now, whatever the panel's presets.
constexpr std::array kWidthSteps {1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40};
constexpr std::array kSizeSteps {10, 12, 14, 16, 18, 20, 24, 28, 32, 40, 48, 64};
constexpr std::array kCellSteps {4, 6, 8, 10, 12, 16, 18, 24, 32};
constexpr int kMaxLogical = 500; // as StyleMemory keeps them
constexpr int kMaxNumber = 99999;
// A touchpad sends the wheel in small pieces: a notch is 120 of them, and
// one notch in 200 ms at most, or a flick runs through every size (3.5).
constexpr int kNotch = 120;
constexpr qint64 kNotchMs = 200;

bool usesWidth(Kind kind)
{
    return kind == Kind::Rect || kind == Kind::Ellipse || kind == Kind::Arrow || kind == Kind::Line || kind == Kind::Pen
        || kind == Kind::Marker;
}

bool usesFill(Kind kind)
{
    return kind == Kind::Rect || kind == Kind::Ellipse;
}

bool usesSize(Kind kind)
{
    return kind == Kind::Text || kind == Kind::Number;
}

template <std::size_t N>
int stepped(const std::array<int, N>& steps, int value, int direction)
{
    if (direction > 0) {
        for (int step : steps) {
            if (step > value)
                return step;
        }
        return value;
    }
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        if (*it < value)
            return *it;
    }
    return value;
}

QVariantList variantList(const QList<int>& values)
{
    QVariantList list;
    for (int v : values)
        list.append(v);
    return list;
}

// TextEdit's plain text, as layoutText() splits it: '\n' between lines.
QString plainText(QString text)
{
    text.replace(QChar::ParagraphSeparator, u'\n');
    text.replace(QChar::LineSeparator, u'\n');
    text.remove(u'\r');
    return text;
}

QPoint clampInto(QPoint p, const QRect& room)
{
    if (room.isEmpty())
        return p;
    return QPoint(std::clamp(p.x(), room.left(), room.right()), std::clamp(p.y(), room.top(), room.bottom()));
}

int logicalValue(const QVariant& value)
{
    const double v = value.toDouble();
    if (!std::isfinite(v))
        return 1;
    return static_cast<int>(std::lround(std::clamp(v, 1.0, double(kMaxLogical))));
}

// Applies the panel's `change` (logical px) to `style`, whose sizes are px
// at `scale`: the document's scale for an object, 1 for a tool's memory,
// which StyleMemory keeps in logical px. Only the fields `kind` uses are
// taken, so an arrow never gets a fill it does not draw. True when any was.
bool applyPanel(const QVariantMap& change, Kind kind, annot::Style& style, qreal scale)
{
    bool any = false;
    for (auto it = change.cbegin(); it != change.cend(); ++it) {
        const QString& key = it.key();
        const QVariant& value = it.value();
        if (key == u"color") {
            if (const std::optional<QRgb> color = annot::parseColor(value.toString())) {
                style.color = *color;
                any = true;
            }
        } else if (key == u"width" && (usesWidth(kind) || kind == Kind::Redact)) {
            style.width = annot::toImagePx(logicalValue(value), scale);
            any = true;
        } else if (key == u"fill" && usesFill(kind)) {
            if (const std::optional<annot::Fill> fill = annot::fillFromName(value.toString())) {
                style.fill = *fill;
                any = true;
            }
        } else if (key == u"size" && usesSize(kind)) {
            style.fontPx = annot::toImagePx(logicalValue(value), scale);
            any = true;
        } else if (key == u"bold" && kind == Kind::Text) {
            style.bold = value.toBool();
            any = true;
        } else if (key == u"boxed" && kind == Kind::Text) {
            style.boxed = value.toBool();
            any = true;
        } else if (key == u"stroke" && kind == Kind::Text) {
            // 0: none; else the outline's width, logical px
            const double v = value.toDouble();
            style.textStroke = std::isfinite(v) && v >= 0.5 ? annot::toImagePx(logicalValue(value), scale) : 0;
            style.textStroke = std::min(style.textStroke, annot::kMaxTextStroke);
            any = true;
        } else if (key == u"strokeColor" && kind == Kind::Text) {
            // "#RRGGBB", or anything else (auto) for the colour opposite the text's
            const std::optional<QRgb> color = annot::parseColor(value.toString());
            style.textStrokeColor = color ? (*color | 0xff000000u) : 0;
            any = true;
        } else if (key == u"mode" && kind == Kind::Redact) {
            if (const std::optional<annot::RedactMode> mode = annot::redactModeFromName(value.toString())) {
                style.redact = *mode;
                any = true;
            }
        } else if (key == u"cell" && kind == Kind::Redact) {
            style.cell = annot::toImagePx(logicalValue(value), scale);
            any = true;
        } else if (key == u"lineMode" && annot::isLine(kind)) {
            if (const auto mode = annot::lineModeFromName(value.toString()); mode && (kind == Kind::Line || *mode != annot::LineMode::Polyline)) {
                style.lineMode = *mode;
                any = true;
            }
        } else if (key == u"markerMode" && kind == Kind::Marker) {
            if (const auto mode = annot::markerModeFromName(value.toString())) {
                style.markerMode = *mode;
                any = true;
            }
        } else if (key == u"shape" && kind == Kind::Redact) {
            if (const std::optional<annot::RedactShape> shape = annot::redactShapeFromName(value.toString())) {
                style.shape = *shape;
                any = true;
            }
        }
    }
    return any;
}

// The resize cursor of a box handle (BoxHandle's order), or of a line's end
// by the line's direction.
int handleCursor(const annot::Item& item, int handle)
{
    if (annot::isLineItem(item) && item.points.size() > 2)
        return Qt::SizeAllCursor;
    if (annot::isLineItem(item) && item.points.size() >= 2) {
        const QPointF v = item.points[1] - item.points[0];
        const qreal angle = std::fmod(std::atan2(v.y(), v.x()) * 180 / std::numbers::pi + 180, 180);
        if (angle < 22.5 || angle >= 157.5)
            return Qt::SizeHorCursor;
        if (angle < 67.5)
            return Qt::SizeFDiagCursor; // y grows downwards: down and right is "\"
        if (angle < 112.5)
            return Qt::SizeVerCursor;
        return Qt::SizeBDiagCursor;
    }
    switch (annot::BoxHandle(handle)) {
    case annot::BoxHandle::TopLeft:
    case annot::BoxHandle::BottomRight:
        return Qt::SizeFDiagCursor;
    case annot::BoxHandle::TopRight:
    case annot::BoxHandle::BottomLeft:
        return Qt::SizeBDiagCursor;
    case annot::BoxHandle::Top:
    case annot::BoxHandle::Bottom:
        return Qt::SizeVerCursor;
    case annot::BoxHandle::Left:
    case annot::BoxHandle::Right:
        return Qt::SizeHorCursor;
    }
    return Qt::SizeAllCursor;
}

// The pixel a handle stands for: the box's pixel at that corner or edge, a
// line's end. Reshaping takes it plus the mouse's travel, so grabbing a
// handle a pixel off its edge does not make the box jump by that pixel.
QPoint handlePixel(const annot::Item& item, int handle)
{
    if (annot::isLineItem(item))
        return annot::pixelAt(item.points.value(handle));
    const QRect box = annot::itemBox(item);
    const auto h = annot::BoxHandle(handle);
    const bool left = h == annot::BoxHandle::TopLeft || h == annot::BoxHandle::Left || h == annot::BoxHandle::BottomLeft;
    const bool top = h == annot::BoxHandle::TopLeft || h == annot::BoxHandle::Top || h == annot::BoxHandle::TopRight;
    return QPoint(left ? box.x() : box.x() + std::max(0, box.width() - 1),
        top ? box.y() : box.y() + std::max(0, box.height() - 1));
}

// Shift's square from `anchor` (the press) towards `to`, as large as fits
// in `room`: the side is cut where the room ends, rather than letting the
// square leave the selection.
QPoint fitSquare(QPoint anchor, QPoint to, const QRect& room)
{
    const int dx = to.x() - anchor.x();
    const int dy = to.y() - anchor.y();
    const int sx = dx >= 0 ? 1 : -1; // as reshaped() turns a square that has not moved
    const int sy = dy >= 0 ? 1 : -1;
    int side = std::max(std::abs(dx), std::abs(dy));
    if (!room.isEmpty()) {
        side = std::min(side, sx > 0 ? room.right() - anchor.x() : anchor.x() - room.left());
        side = std::min(side, sy > 0 ? room.bottom() - anchor.y() : anchor.y() - room.top());
    }
    side = std::max(0, side);
    return anchor + QPoint(sx * side, sy * side);
}

// Shift's line turned to a 15 degree step around its first point may end
// outside `room`, where clamping the end would bend it: it is shortened
// along its direction instead, until it ends on a pixel inside. `start` is
// the item reshaped() made `made` from.
annot::Item fitLine(const annot::Item& start, annot::Item made, const QRect& room)
{
    if (room.isEmpty() || made.points.size() < 2 || room.contains(annot::pixelAt(made.points.last())))
        return made;
    const QPointF a = made.points.first();
    const QPointF v = made.points.last() - a;
    const qreal length = std::hypot(v.x(), v.y());
    if (length <= 0)
        return made;
    // The largest share of the line whose end is a pixel centre inside.
    const qreal left = room.left() + 0.5;
    const qreal right = room.right() + 0.5;
    const qreal top = room.top() + 0.5;
    const qreal bottom = room.bottom() + 0.5;
    qreal t = 1;
    if (v.x() > 0)
        t = std::min(t, (right - a.x()) / v.x());
    else if (v.x() < 0)
        t = std::min(t, (left - a.x()) / v.x());
    if (v.y() > 0)
        t = std::min(t, (bottom - a.y()) / v.y());
    else if (v.y() < 0)
        t = std::min(t, (top - a.y()) / v.y());
    t = std::max<qreal>(0, t);
    // Turning it to the step again rounds to whole pixels, which can reach
    // a pixel past the edge: a pixel shorter each time.
    for (int i = 0; i < 8; ++i) {
        const annot::Item fitted = annot::reshaped(start, int(annot::LineHandle::End), a + v * t, true);
        if (fitted.points.size() >= 2 && room.contains(annot::pixelAt(fitted.points.last())))
            return fitted;
        t = std::max<qreal>(0, t - 1 / length);
    }
    made.points.last() = annot::pixelCentre(QPointF(clampInto(annot::pixelAt(made.points.last()), room)));
    return made;
}

// What a stroke growing by one point changes: the centre line is the same
// up to the midpoint before its last point (strokeCentreline), so only the
// last two points, the new one and the width around them are drawn again,
// not the whole stroke at every move. None when more may have changed: a
// stroke still level or upright is drawn half a pixel off its points until
// it bends (alignedPoints), and Shift makes it a line again.
std::optional<QRectF> strokeGrowth(const annot::Item& before, const annot::Item& after)
{
    const qsizetype n = before.points.size();
    if (n < 3 || after.points.size() != n + 1 || after.style != before.style)
        return std::nullopt;
    if (!std::equal(before.points.cbegin(), before.points.cend(), after.points.cbegin()))
        return std::nullopt;
    bool level = true;
    bool upright = true;
    for (const QPointF& p : before.points) {
        level = level && p.y() == before.points[0].y();
        upright = upright && p.x() == before.points[0].x();
    }
    if (level || upright)
        return std::nullopt;
    const qreal x0 = std::min({before.points[n - 2].x(), before.points[n - 1].x(), after.points[n].x()});
    const qreal x1 = std::max({before.points[n - 2].x(), before.points[n - 1].x(), after.points[n].x()});
    const qreal y0 = std::min({before.points[n - 2].y(), before.points[n - 1].y(), after.points[n].y()});
    const qreal y1 = std::max({before.points[n - 2].y(), before.points[n - 1].y(), after.points[n].y()});
    // Round caps and joins reach half the width past the points (bounds()),
    // and a pixel more for the antialiased edge.
    const qreal m = std::max(1, before.style.width) / 2.0 + 2;
    return QRectF(QPointF(x0, y0), QPointF(x1, y1)).adjusted(-m, -m, m, m);
}

} // namespace

struct AnnotationModel::Private {
    QElapsedTimer clock;

    // The gesture under way (3.7): its command, made at the press, reverted,
    // changed and applied again at every move; in the document only while
    // `applied`.
    std::unique_ptr<annot::Command> command;
    bool applied = false;
    bool dragging = false; // moved past the drag start since the press
    QList<QPointF> pathNodes; // fixed vertices; the last point in the draft follows the mouse
    QPoint pressAt;
    QPoint last;
    Qt::KeyboardModifiers modifiers;
    annot::Item before; // Draw: the item as the press made it; Reshape: the item before
    int handle = -1; // Reshape: of the item (BoxHandle, LineHandle); Crop: a Handle of Geometry.h
    QPoint own; // Reshape: the pixel the handle stands for (handlePixel)
    // A Number just put down: in the document, not yet a step of the stack
    // while its words are typed, so that putting it down and typing them
    // are one undo step (commitWords()).
    std::unique_ptr<annot::AddItems> placing;
    bool reopenWords = false; // Move: the number pressed was the only one selected already
    QList<quint32> moveIds; // Move: what moves together
    QRect cropStart; // Crop: the crop as the press found it
    QList<quint32> marqueeBase; // Marquee: the selected ones before it (Shift keeps them)
    QRect marqueeBox; // Marquee: the pixels spanned from the press to the mouse

    int wheelSum = 0;
    bool notched = false;
    qint64 lastNotch = 0;

    // Smart redaction (4.1): the scan of the selection's text and what it found.
    struct Smart {
        SmartHooks hooks;
        QTimer* timer = nullptr;
        int ticket = 0; // the scan whose result is wanted
        bool scanning = false;
        bool ready = false; // `lines` are the text of `rect`
        bool apply = false; // asked for by the button before the scan was done: hide what it finds at once
        bool told = false; // the "may have missed some" word has been said
        QRect rect; // the pixels scanned, image px
        QList<RedactLine> lines; // in `rect`'s pixels
        QImage gray; // the picture scanned, for the boxes' edges
        std::optional<QList<RedactHit>> all; // every place the rules found, in the document's pixels; made when asked for
        std::optional<QList<RedactHit>> hits; // those not hidden yet; made again when the redactions change
    } smart;

    // What the change signals were last sent for: sync() sends each one when
    // its value is no longer this.
    struct Shown {
        int tool = -1;
        bool canUndo = false;
        bool canRedo = false;
        QString undoLabel;
        QString redoLabel;
        int itemCount = 0;
        bool panelVisible = false;
        QVariantMap panel;
        QVariantList selection;
        QVariantMap editing;
        QVariantMap numberEdit;
        bool gestureActive = false;
        QRectF gestureBounds;
        bool editor = false;
        QRect crop;
        QRectF marquee;
        bool picking = false;
        int selectedCount = 0;
        int smartFound = -1;
        bool smartBusy = false;
    } shown;
};

AnnotationModel::AnnotationModel(QObject* parent)
    : QObject(parent)
    , m_compositor(std::make_unique<annot::Compositor>())
    , d(std::make_unique<Private>())
{
    d->clock.start();
    d->shown.panel = panel();
    d->smart.timer = new QTimer(this);
    d->smart.timer->setSingleShot(true);
    d->smart.timer->setInterval(300);
    connect(d->smart.timer, &QTimer::timeout, this, [this] { smartStart(false); });
}

AnnotationModel::~AnnotationModel() = default;

// ---- the capture's side ----------------------------------------------------

void AnnotationModel::begin(const QImage& base, qreal dpr, const annot::StyleMemory& styles)
{
    reset();
    m_base = base;
    m_document.canvas = base.size();
    m_document.dpr = dpr > 0 ? dpr : 1.0;
    m_styles = styles;
    m_stylesChanged = false;
    m_document.series.first().format = m_styles.numberFormat(); // the first group writes numbers as last time
    emit changed(QRectF()); // a new picture: all of it
    sync();
}

void AnnotationModel::begin(const annot::Project& project, qreal deviceDpr, const annot::StyleMemory& styles)
{
    reset();
    m_editor = true;
    m_base = project.base;
    m_document = project.doc;
    m_document.canvas = m_base.size();
    const QRect canvas(QPoint(0, 0), m_document.canvas);
    QRect crop = m_document.crop & canvas;
    if (crop.isEmpty())
        crop = canvas;
    m_document.crop = crop;
    if (!(m_document.dpr > 0))
        m_document.dpr = deviceDpr > 0 ? deviceDpr : 1.0;
    m_loaded = m_document;
    m_basePng = project.basePng;
    m_styles = styles;
    m_stylesChanged = false;
    emit changed(QRectF()); // a new picture: all of it
    sync();
    smartSchedule();
}

void AnnotationModel::setView(qreal devicePerLogical, qreal zoom)
{
    m_devicePerLogical = devicePerLogical > 0 ? devicePerLogical : 0;
    m_zoom = zoom > 0 ? zoom : 1;
}

bool AnnotationModel::modified() const
{
    if (!active())
        return false;
    return committed() != m_loaded;
}

void AnnotationModel::discardChanges()
{
    if (!m_editor || !active())
        return;
    commitPending();
    if (m_gesture != Gesture::None)
        cancelGesture();
    if (m_document == m_loaded)
        return;
    m_picked.clear();
    m_numberEdit.reset();
    m_picking = false;
    push(std::make_unique<annot::ReplaceDocument>(m_document, m_loaded));
    sync();
}

void AnnotationModel::end()
{
    reset();
    smartInvalidate();
    m_base = QImage();
    emit changed(QRectF()); // the canvas lets go of its tiles
    sync();
}

void AnnotationModel::reset()
{
    d->command.reset();
    d->placing.reset();
    d->applied = false;
    d->dragging = false;
    d->pathNodes.clear();
    d->wheelSum = 0;
    d->notched = false;
    m_gesture = Gesture::None;
    m_document = annot::Document {};
    m_stack.clear();
    m_compositor->clear();
    m_tool = -1;
    m_picked.clear();
    m_editing.reset();
    m_numberEdit.reset();
    m_editor = false;
    m_loaded = annot::Document {};
    m_basePng.clear();
    m_picking = false;
    m_devicePerLogical = 0;
    m_zoom = 1;
    d->moveIds.clear();
    d->marqueeBase.clear();
    d->marqueeBox = QRect();
}

void AnnotationModel::setSelection(const QRect& selection)
{
    if (selection == m_document.crop)
        return;
    m_document.crop = selection;
    // Redactions take their colours from inside the selection (3.10), so
    // every one is made again; nothing else is drawn from it.
    annot::Change change;
    change.redactions = true;
    for (const QRect& box : annot::redactionBoxes(m_document))
        change.dirty |= QRectF(box);
    touched(change);
    sync();
    smartSchedule();
}

QRect AnnotationModel::sampleBounds() const
{
    // The editor's redactions read all of the picture, whatever the crop (3.10).
    if (m_editor || m_document.crop.isEmpty())
        return QRect(QPoint(0, 0), m_document.canvas);
    return m_document.crop;
}

std::optional<quint32> AnnotationModel::skipped() const
{
    if (m_editing && m_editing->id != 0 && !m_editing->words)
        return m_editing->id;
    return std::nullopt;
}

std::optional<quint32> AnnotationModel::draft() const
{
    if (m_gesture != Gesture::Draw || !d->command || !d->applied)
        return std::nullopt;
    const auto* add = static_cast<const annot::AddItems*>(d->command.get());
    if (add->items.isEmpty() || !annot::isBrush(add->items[0]))
        return std::nullopt;
    return add->items[0].id;
}

std::optional<quint32> AnnotationModel::unworded() const
{
    if (m_editing && m_editing->words)
        return m_editing->id;
    return std::nullopt;
}

QImage AnnotationModel::composed()
{
    commitPending();
    if (m_base.isNull() || (!m_editor && m_document.items.isEmpty()))
        return {};
    return m_compositor->exportImage(m_document, m_base, sampleBounds());
}

annot::Project AnnotationModel::project() const
{
    const annot::Document doc = committed();
    if (m_base.isNull())
        return {};
    // The editor keeps the whole picture, and the crop in the document, so
    // what was cropped away can be brought back.
    if (m_editor)
        return annot::Project {doc, m_base, m_basePng};
    if (doc.items.isEmpty())
        return {};
    return annot::projectFromOverlay(doc, m_base, doc.crop);
}

// The document as committing the text under way would leave it, without
// committing it (project() is const).
annot::Document AnnotationModel::committed() const
{
    annot::Document doc = m_document;
    if (pathActive()) {
        const int index = annot::indexOf(doc, d->before.id);
        if (index >= 0) {
            if (d->pathNodes.size() < 2)
                doc.items.removeAt(index);
            else
                doc.items[index].points = d->pathNodes;
        }
    }
    if (m_editing) {
        if (const std::unique_ptr<annot::Command> command = textCommand(doc, m_editing->text))
            command->apply(doc);
    }
    return doc;
}

// ---- state for QML ---------------------------------------------------------

void AnnotationModel::setTool(int tool)
{
    if (tool < -1 || tool >= int(annot::tools().size()) || tool == m_tool)
        return;
    finishPath();
    if (m_gesture != Gesture::None)
        cancelGesture();
    commitPending();
    m_numberEdit.reset();
    // An object of another kind is let go of: the panel then shows the new
    // tool's style, which is what the next drawing gets.
    m_picking = false;
    if (tool >= 0) {
        const Kind kind = annot::tools()[tool].kind;
        m_picked.removeIf([this, kind](quint32 id) {
            const annot::Item* item = find(id);
            return !item || item->kind != kind;
        });
    }
    m_tool = tool;
    d->wheelSum = 0;
    sync();
}

QVariantList AnnotationModel::toolList() const
{
    QVariantList list;
    for (const annot::ToolInfo& t : annot::tools()) {
        list.append(QVariantMap {{u"kind"_s, int(t.kind)}, {u"name"_s, annot::toolName(t.kind)},
            {u"letter"_s, QString(t.letter)}, {u"digit"_s, t.digit}, {u"glyph"_s, t.glyph}});
    }
    return list;
}

QString AnnotationModel::fontFamily() const
{
    return annot::annotationFont(18, false).family();
}

bool AnnotationModel::canUndo() const
{
    return pathActive() || m_stack.canUndo();
}

bool AnnotationModel::canRedo() const
{
    return m_stack.canRedo();
}

QString AnnotationModel::undoLabel() const
{
    return pathActive() ? tr("撤回节点") : m_stack.undoLabel();
}

QString AnnotationModel::redoLabel() const
{
    return m_stack.redoLabel();
}

bool AnnotationModel::panelVisible() const
{
    return active() && (m_tool >= 0 || !m_picked.isEmpty() || m_editing.has_value());
}

QVariantMap AnnotationModel::panel() const
{
    QVariantMap m;
    QVariantList colors;
    for (QRgb c : annot::palette())
        colors.append(colorHex(c));
    m[u"colors"_s] = colors;
    const std::optional<Target> target = styleTarget();
    if (!target) {
        m[u"kind"_s] = -1;
        m[u"selected"_s] = false;
        m[u"canRaise"_s] = false;
        m[u"canLower"_s] = false;
        return m;
    }
    const Kind kind = target->kind;
    const bool mixed = target->mixed;
    // An object's sizes through the document's scale; a tool's as it
    // remembers them, which are logical already.
    annot::StyleMemory::Tool t = m_styles.tool(kind);
    if (!target->ids.isEmpty() || target->typing) {
        const annot::Style s = targetStyle(*target);
        const qreal dpr = this->dpr();
        t.color = s.color;
        t.width = annot::toLogicalPx(s.width, dpr);
        t.fill = s.fill;
        t.size = annot::toLogicalPx(s.fontPx, dpr);
        t.bold = s.bold;
        t.boxed = s.boxed;
        t.stroke = s.textStroke > 0 ? annot::toLogicalPx(s.textStroke, dpr) : 0;
        t.strokeColor = s.textStrokeColor;
        t.mode = s.redact;
        t.shape = s.shape;
        if (s.shape == annot::RedactShape::Brush)
            t.width = annot::toLogicalPx(s.width, dpr);
        t.cell = annot::toLogicalPx(s.cell, dpr);
    }
    // Objects of different kinds share the colour and the layer buttons only.
    m[u"kind"_s] = mixed ? -1 : int(kind);
    m[u"name"_s] = target->ids.size() > 1 ? tr("%1 个对象").arg(target->ids.size()) : annot::toolName(kind);
    m[u"color"_s] = colorHex(t.color);
    m[u"many"_s] = target->ids.size() > 1;
    if (mixed) {
        m[u"selected"_s] = true;
        m[u"canRaise"_s] = false;
        m[u"canLower"_s] = false;
        return m;
    }
    const annot::MarkerMode markerMode = target->ids.isEmpty() ? t.markerMode : targetStyle(*target).markerMode;
    if (usesWidth(kind) && !(kind == Kind::Marker && markerMode == annot::MarkerMode::Rectangle)) {
        m[u"width"_s] = t.width;
        m[u"widths"_s] = variantList(annot::sizePresets(kind));
    }
    if (annot::isLine(kind))
        m[u"lineMode"_s] = annot::lineModeName(t.lineMode);
    if (kind == Kind::Marker)
        m[u"markerMode"_s] = annot::markerModeName(t.markerMode);
    if (usesFill(kind))
        m[u"fill"_s] = annot::fillName(t.fill);
    if (usesSize(kind)) {
        m[u"size"_s] = t.size;
        m[u"sizes"_s] = variantList(annot::sizePresets(kind));
    }
    if (kind == Kind::Text) {
        m[u"bold"_s] = t.bold;
        m[u"boxed"_s] = t.boxed;
        // The outline: its width (0: none), the presets, and the colour it is
        // drawn in (automatic: the opposite of the text's) with whether that is automatic.
        m[u"stroke"_s] = t.stroke;
        m[u"strokes"_s] = variantList(annot::strokePresets());
        m[u"strokeAuto"_s] = t.strokeColor == 0;
        m[u"strokeColor"_s] = colorHex(t.strokeColor != 0 ? t.strokeColor : annot::inkOn(t.color));
    }
    if (kind == Kind::Redact) {
        m[u"mode"_s] = annot::redactModeName(t.mode);
        m[u"shape"_s] = annot::redactShapeName(t.shape);
        m[u"cell"_s] = t.cell;
        m[u"cells"_s] = variantList(annot::sizePresets(kind));
        if (t.shape == annot::RedactShape::Brush) {
            m[u"width"_s] = t.width;
            m[u"widths"_s] = variantList(annot::brushWidths());
        }
    }
    if (kind == Kind::Number) {
        int start = 1;
        for (const annot::Series& s : m_document.series) {
            if (s.id == m_document.currentSeries)
                start = s.start;
        }
        m[u"start"_s] = start;
        quint32 series = m_document.currentSeries;
        if (const annot::Item* item = find(target->id()))
            series = item->series;
        m[u"format"_s] = annot::numberFormatName(annot::formatOf(m_document, series));
    }
    m[u"selected"_s] = !target->ids.isEmpty();
    const bool layered = target->ids.size() == 1 && !m_editing;
    m[u"canRaise"_s] = layered && canReorder(1);
    m[u"canLower"_s] = layered && canReorder(-1);
    return m;
}

QVariantList AnnotationModel::selection() const
{
    if (m_editing)
        return {}; // the text box stands for it
    QVariantList list;
    const bool one = m_picked.size() == 1;
    for (const quint32 id : m_picked) {
        const annot::Item* item = find(id);
        if (!item)
            continue;
        const QRectF outline = annot::bounds(m_document, *item);
        // Handles reshape one object; several are moved, restyled, deleted together.
        const QList<QPointF> points = one ? annot::handles(*item) : QList<QPointF>();
        QVariantList handles;
        for (qsizetype i = 0; i < points.size(); ++i)
            handles.append(QVariantMap {{u"x"_s, points[i].x()}, {u"y"_s, points[i].y()}, {u"index"_s, int(i)},
                {u"control"_s, annot::isLine(item->kind) && item->style.lineMode == annot::LineMode::Bezier && i >= 2}});
        list.append(QVariantMap {{u"id"_s, int(item->id)}, {u"kind"_s, int(item->kind)}, {u"x"_s, outline.x()},
            {u"y"_s, outline.y()}, {u"width"_s, outline.width()}, {u"height"_s, outline.height()},
            {u"box"_s, annot::isBoxItem(*item)}, {u"curve"_s, annot::isLine(item->kind) && item->style.lineMode == annot::LineMode::Bezier},
            {u"handles"_s, handles}});
    }
    return list;
}

QVariantMap AnnotationModel::editing() const
{
    if (!m_editing)
        return {};
    if (m_editing->words) {
        const annot::Item* item = find(m_editing->id);
        if (!item)
            return {};
        const int f = std::max(1, item->style.fontPx);
        const annot::TextMetrics metrics = annot::textMetrics(QString(), f, false);
        const QPoint at = geometryOf(*item).wordsOrigin;
        const QString color = colorHex(annot::numberWordsColor(*item));
        return QVariantMap {{u"id"_s, int(m_editing->id)}, {u"x"_s, at.x()}, {u"y"_s, at.y()}, {u"fontPx"_s, f},
            {u"bold"_s, false}, {u"boxed"_s, false}, {u"color"_s, color}, {u"fill"_s, color},
            {u"text"_s, m_editing->text}, {u"pad"_s, 0}, {u"radius"_s, 0}, {u"pitch"_s, metrics.pitch},
            {u"ascent"_s, metrics.ascent}, {u"clickX"_s, m_editing->click.x()}, {u"clickY"_s, m_editing->click.y()},
            {u"number"_s, true}};
    }
    const annot::Style style = editingStyle();
    const int f = std::max(1, style.fontPx);
    // Ascent and pitch are the font's, whatever the words.
    const annot::TextMetrics metrics = annot::textMetrics(QString(), f, style.bold);
    const int pad = style.boxed ? qRound(0.3 * f) : 0; // as textGeometry() pads the box
    const qreal radius = style.boxed ? 0.25 * f : 0;
    // The outline the words get when committed (image px; 0 when none or boxed): the edit box draws it under the glyphs.
    annot::Item outlined;
    outlined.kind = Kind::Text;
    outlined.style = style;
    const int stroke = annot::textStrokeWidth(outlined);
    const QString strokeColor = colorHex(annot::textStrokeColorOf(outlined));
    return QVariantMap {{u"id"_s, int(m_editing->id)}, {u"x"_s, m_editing->at.x()}, {u"y"_s, m_editing->at.y()},
        {u"fontPx"_s, f}, {u"bold"_s, style.bold}, {u"boxed"_s, style.boxed},
        {u"color"_s, colorHex(style.boxed ? annot::inkOn(style.color) : style.color)},
        {u"fill"_s, colorHex(style.color)}, {u"text"_s, m_editing->text}, {u"pad"_s, pad}, {u"radius"_s, radius},
        {u"pitch"_s, metrics.pitch}, {u"ascent"_s, metrics.ascent}, {u"clickX"_s, m_editing->click.x()},
        {u"clickY"_s, m_editing->click.y()}, {u"stroke"_s, stroke}, {u"strokeColor"_s, strokeColor}};
}

QVariantMap AnnotationModel::numberEdit() const
{
    const annot::Item* item = find(m_numberEdit);
    if (!item || item->kind != Kind::Number || item->points.isEmpty())
        return {};
    return QVariantMap {{u"id"_s, int(item->id)}, {u"x"_s, item->points[0].x()}, {u"y"_s, item->points[0].y()},
        {u"value"_s, item->pinned ? *item->pinned : -1},
        {u"shown"_s, annot::numbers(m_document).value(item->id, 1)}};
}

QRectF AnnotationModel::gestureBounds() const
{
    // Cropping and selecting in the editor are far from any toolbar.
    if (m_gesture == Gesture::None || m_gesture == Gesture::Crop || m_gesture == Gesture::Marquee || !d->command)
        return {};
    QRectF r;
    if (m_gesture == Gesture::Draw) {
        // Not in the document until it moved: measured on the command's copy.
        const auto* add = static_cast<const annot::AddItems*>(d->command.get());
        if (!add->items.isEmpty()) {
            const annot::Item& item = add->items[0];
            r = d->applied ? annot::bounds(m_document, item) : annot::bounds(item, dpr());
        }
    } else if (m_gesture == Gesture::Move) {
        for (const quint32 id : d->moveIds) {
            if (const annot::Item* item = find(id))
                r |= annot::bounds(m_document, *item);
        }
    } else if (const annot::Item* item = find(d->before.id)) {
        r = annot::bounds(m_document, *item);
    }
    if (r.isEmpty())
        r = QRectF(QPointF(d->last), QSizeF(1, 1));
    const qreal m = 16 * dpr();
    return r.adjusted(-m, -m, m, m);
}

QRectF AnnotationModel::marquee() const
{
    return m_gesture == Gesture::Marquee && d->dragging ? QRectF(d->marqueeBox) : QRectF();
}

// ---- helpers ---------------------------------------------------------------

qint64 AnnotationModel::now() const
{
    return d->clock.elapsed();
}

qreal AnnotationModel::factor() const
{
    const qreal device = m_devicePerLogical > 0 ? m_devicePerLogical : dpr();
    return device / std::max<qreal>(0.01, m_zoom);
}

QRect AnnotationModel::room() const
{
    return m_editor ? QRect(QPoint(0, 0), m_document.canvas) : m_document.crop;
}

std::optional<quint32> AnnotationModel::single() const
{
    return m_picked.size() == 1 ? std::optional<quint32>(m_picked.first()) : std::nullopt;
}

qreal AnnotationModel::tolerance() const
{
    return 4 * factor(); // 3.2: lines count within half their width and 4 logical px
}

int AnnotationModel::reach() const
{
    return std::max(1, static_cast<int>(std::lround(6 * factor()))); // as CaptureSession reaches the selection's handles
}

int AnnotationModel::dragStart() const
{
    // Whole image pixels are the least a mouse position can differ by.
    return std::max(m_zoom > 1 ? 1 : 2, static_cast<int>(std::lround(3 * factor()))); // as CaptureSession starts a drag
}

QPoint AnnotationModel::clampToSelection(QPoint p) const
{
    return clampInto(p, room());
}

std::optional<Kind> AnnotationModel::toolKind() const
{
    if (m_tool < 0 || m_tool >= int(annot::tools().size()))
        return std::nullopt;
    return annot::tools()[m_tool].kind;
}

const annot::Item* AnnotationModel::find(std::optional<quint32> id) const
{
    return id ? annot::findItem(m_document, *id) : nullptr;
}

std::optional<quint32> AnnotationModel::itemAt(QPoint p) const
{
    return annot::hitTest(m_document, QPointF(p), tolerance());
}

// The selected object under `p`, even where others lie over it (3.6, 4.1):
// tested alone, a Number keeping the number it shows (its disc's width).
bool AnnotationModel::onSelected(QPoint p) const
{
    if (m_picked.isEmpty() || m_editing)
        return false;
    const QHash<quint32, int> shown = annot::numbers(m_document);
    for (const quint32 id : m_picked) {
        const annot::Item* item = find(id);
        if (!item)
            continue;
        annot::Document probe;
        probe.canvas = m_document.canvas;
        probe.dpr = m_document.dpr;
        probe.items = {*item};
        if (item->kind == Kind::Number)
            probe.items[0].pinned = shown.value(item->id, 1);
        if (annot::hitTest(probe, QPointF(p), tolerance()) == item->id)
            return true;
    }
    return false;
}

int AnnotationModel::handleAt(QPoint p) const
{
    const annot::Item* item = find(single());
    if (!item || m_editing)
        return -1;
    const QList<QPointF> points = annot::handles(*item);
    const QPointF at = annot::pixelCentre(QPointF(p));
    const qreal reach = this->reach();
    int best = -1;
    qreal nearest = 0;
    for (qsizetype i = 0; i < points.size(); ++i) {
        const qreal distance = std::hypot(points[i].x() - at.x(), points[i].y() - at.y());
        if (distance <= reach && (best < 0 || distance < nearest)) {
            best = int(i);
            nearest = distance;
        }
    }
    return best;
}

// A handle of the editor's crop under `p`. With a tool on, a press inside
// the crop draws, and one on an object takes the object: the handles are
// reached from outside the crop then.
int AnnotationModel::cropHandleAt(QPoint p) const
{
    if (!m_editor || m_gesture != Gesture::None || m_editing || m_document.crop.isEmpty())
        return 0;
    const QRect crop = m_document.crop;
    if (crop.contains(p) && (m_tool >= 0 || itemAt(p)))
        return 0;
    const Handle h = ws::capture::handleAt(crop, p, reach());
    return h == Handle::None || h == Handle::Move ? 0 : int(h);
}

QRgb AnnotationModel::colorAt(QPoint p) const
{
    QImage out;
    m_compositor->render(m_document, m_base, sampleBounds(), QRect(p, QSize(1, 1)), &out, skipped());
    return out.isNull() ? qRgb(0, 0, 0) : (out.pixel(0, 0) | 0xff000000u);
}

bool AnnotationModel::onHandle(QPoint p) const
{
    return active() && m_gesture == Gesture::None && (handleAt(p) >= 0 || cropHandleAt(p) != 0);
}

std::optional<AnnotationModel::Target> AnnotationModel::styleTarget() const
{
    if (!active())
        return std::nullopt;
    Target target;
    if (m_editing) {
        if (m_editing->words && find(m_editing->id)) {
            target.kind = Kind::Number;
            target.ids = {m_editing->id};
            return target;
        }
        target.kind = Kind::Text;
        if (m_editing->id != 0 && find(m_editing->id))
            target.ids = {m_editing->id};
        else
            target.typing = true;
        return target;
    }
    for (const quint32 id : m_picked) {
        const annot::Item* item = find(id);
        if (!item)
            continue;
        if (target.ids.isEmpty())
            target.kind = item->kind;
        else if (item->kind != target.kind)
            target.mixed = true;
        target.ids.append(id);
    }
    if (!target.ids.isEmpty())
        return target;
    if (const std::optional<Kind> kind = toolKind()) {
        target.kind = *kind;
        return target;
    }
    return std::nullopt;
}

annot::Style AnnotationModel::targetStyle(const Target& target) const
{
    if (const annot::Item* item = find(target.id()))
        return item->style;
    if (target.typing && m_editing)
        return m_editing->style;
    return m_styles.forTool(target.kind, dpr());
}

annot::Style AnnotationModel::editingStyle() const
{
    if (!m_editing)
        return {};
    if (const annot::Item* item = find(m_editing->id ? std::optional<quint32>(m_editing->id) : std::nullopt))
        return item->style;
    return m_editing->style;
}

// The box of the text being typed, its padding included.
QRect AnnotationModel::editingBox() const
{
    if (!m_editing)
        return {};
    if (m_editing->words) {
        const annot::Item* item = find(m_editing->id);
        if (!item)
            return {};
        const int f = std::max(1, item->style.fontPx);
        QRect box(geometryOf(*item).wordsOrigin, annot::textMetrics(m_editing->text, f, false).size);
        box.setWidth(std::max(box.width(), std::max(1, f / 2)));
        box.setHeight(std::max(box.height(), 1));
        return box;
    }
    annot::Item probe;
    probe.kind = Kind::Text;
    probe.points = {QPointF(m_editing->at)};
    probe.style = editingStyle();
    probe.text = m_editing->text;
    QRect box = annot::textGeometry(probe).box;
    // A new text has no width yet; the caret's place still counts as inside
    // (a double click's second press lands there).
    box.setWidth(std::max(box.width(), std::max(1, probe.style.fontPx / 2)));
    box.setHeight(std::max(box.height(), 1));
    return box;
}

// The step that committing `text` is (3.8): a new text with words is added,
// a new one without is nothing; an old one without words is removed, a
// changed one retyped, an unchanged one nothing.
std::unique_ptr<annot::Command> AnnotationModel::textCommand(const annot::Document& doc, const QString& text) const
{
    if (!m_editing)
        return nullptr;
    if (m_editing->words) { // a Number's words: empty is none, not a removed item
        const annot::Item* number = annot::findItem(doc, m_editing->id);
        if (!number || text.trimmed() == number->text)
            return nullptr;
        return std::make_unique<annot::Retext>(doc, number->id, text.trimmed());
    }
    const bool words = !text.trimmed().isEmpty();
    if (m_editing->id == 0) {
        if (!words)
            return nullptr;
        annot::Item item;
        item.id = doc.nextId;
        item.seq = doc.nextSeq;
        item.kind = Kind::Text;
        item.points = {QPointF(m_editing->at)};
        item.style = m_editing->style;
        item.text = text;
        return std::make_unique<annot::AddItems>(QList<annot::Item> {item});
    }
    const annot::Item* item = annot::findItem(doc, m_editing->id);
    if (!item)
        return nullptr;
    if (!words)
        return std::make_unique<annot::RemoveItems>(QList<quint32> {item->id});
    if (text == item->text)
        return nullptr;
    return std::make_unique<annot::Retext>(doc, item->id, text);
}

void AnnotationModel::push(std::unique_ptr<annot::Command> command)
{
    // A number just put down, its words not finished: it is a step before whatever comes now
    // (a restyle from the panel), so that undo takes them back in order.
    if (d->placing)
        m_stack.pushApplied(std::move(d->placing), now());
    touched(m_stack.push(std::move(command), m_document, now()));
}

// Every change the document went through: the compositor drops what it
// kept of the items, the canvas draws the tiles again. An empty region is
// nothing to draw (the canvas reads an empty one as all of it).
void AnnotationModel::touched(const annot::Change& change)
{
    m_compositor->invalidate(change);
    if (change.redactions)
        d->smart.hits.reset(); // what is still to hide changes with what is hidden
    if (change.crop && m_editor)
        smartSchedule();
    for (const QRectF& area : change.areas()) {
        if (!area.isEmpty())
            emit changed(area);
    }
}

// After undo and redo: what was selected or renumbered may be gone.
void AnnotationModel::keepValid()
{
    m_picked.removeIf([this](quint32 id) { return !find(id); });
    if (m_numberEdit && !find(m_numberEdit))
        m_numberEdit.reset();
}

void AnnotationModel::sync()
{
    Private::Shown& s = d->shown;
    if (m_tool != s.tool) {
        s.tool = m_tool;
        emit toolChanged();
    }
    const bool undo = canUndo();
    const bool redo = canRedo();
    const QString undoText = undoLabel();
    const QString redoText = redoLabel();
    if (undo != s.canUndo || redo != s.canRedo || undoText != s.undoLabel || redoText != s.redoLabel) {
        s.canUndo = undo;
        s.canRedo = redo;
        s.undoLabel = undoText;
        s.redoLabel = redoText;
        emit historyChanged();
    }
    if (itemCount() != s.itemCount) {
        s.itemCount = itemCount();
        emit itemsChanged();
    }
    const bool visible = panelVisible();
    QVariantMap panelNow = panel();
    if (visible != s.panelVisible || panelNow != s.panel) {
        s.panelVisible = visible;
        s.panel = std::move(panelNow);
        emit panelChanged();
    }
    QVariantList selectionNow = selection();
    if (selectionNow != s.selection) {
        s.selection = std::move(selectionNow);
        emit selectionChanged();
    }
    QVariantMap editingNow = editing();
    if (editingNow != s.editing) {
        s.editing = std::move(editingNow);
        emit editingChanged();
    }
    QVariantMap numberNow = numberEdit();
    if (numberNow != s.numberEdit) {
        s.numberEdit = std::move(numberNow);
        emit numberEditChanged();
    }
    const bool gesture = gestureActive();
    const QRectF gestureRect = gestureBounds();
    if (gesture != s.gestureActive || gestureRect != s.gestureBounds) {
        s.gestureActive = gesture;
        s.gestureBounds = gestureRect;
        emit gestureChanged();
    }
    if (m_editor != s.editor) {
        s.editor = m_editor;
        emit editorChanged();
    }
    if (m_document.crop != s.crop) {
        s.crop = m_document.crop;
        emit cropChanged();
    }
    const QRectF box = marquee();
    if (box != s.marquee) {
        s.marquee = box;
        emit marqueeChanged();
    }
    if (m_picking != s.picking) {
        s.picking = m_picking;
        emit pickingChanged();
    }
    s.selectedCount = static_cast<int>(m_picked.size());
    const int found = smartFound();
    const bool busy = smartBusy();
    if (found != s.smartFound || busy != s.smartBusy) {
        s.smartFound = found;
        s.smartBusy = busy;
        emit smartChanged();
    }
}

// ---- the mouse -------------------------------------------------------------

bool AnnotationModel::pressFirst(QPoint p, Qt::KeyboardModifiers modifiers)
{
    if (!active())
        return false;
    if (pathActive()) {
        const QPoint at = clampToSelection(p);
        if (d->pathNodes.size() >= 3 && QLineF(QPointF(at), d->pathNodes.first()).length() <= reach()) {
            finishPath(true);
            return true;
        }
        QPointF vertex = annot::pixelCentre(at);
        if (modifiers & Qt::ShiftModifier)
            vertex = annot::snapAngle(d->pathNodes.last(), vertex);
        vertex = annot::pixelCentre(clampToSelection(annot::pixelAt(vertex)));
        if (vertex != d->pathNodes.last())
            d->pathNodes.append(vertex);
        d->last = annot::pixelAt(vertex);
        d->modifiers = modifiers;
        updateGesture();
        sync();
        return true;
    }
    if (m_gesture != Gesture::None)
        finishGesture(); // a release that never came
    m_numberEdit.reset();
    if (m_editing) {
        // The text box takes the presses inside it; one that reaches here
        // only commits the text, and does nothing else (3.6, step 1). One on
        // the box's padding counts as inside.
        const bool words = m_editing->words;
        if (!editingBox().contains(p))
            commitPending();
        sync();
        // A Number's words end with the press, which goes on to put the next
        // number down (or whatever it was for): one click a number.
        return !(words && !m_editing);
    }
    if (m_picking) {
        // The eyedropper: the pixel as drawn, annotations included.
        m_picking = false;
        if (QRect(QPoint(0, 0), m_document.canvas).contains(p))
            setPanel(QVariantMap {{u"color"_s, colorHex(colorAt(p))}});
        sync();
        return true;
    }
    const int handle = handleAt(p);
    if (handle >= 0) {
        startReshape(handle, p, modifiers);
        sync();
        return true;
    }
    if (const int crop = cropHandleAt(p); crop != 0) {
        startCrop(crop, p);
        sync();
        return true;
    }
    sync();
    return false;
}

bool AnnotationModel::pressSecond(QPoint p, Qt::KeyboardModifiers modifiers)
{
    if (!active())
        return false;
    if (m_gesture != Gesture::None)
        finishGesture();
    m_numberEdit.reset();
    if (m_editing) { // pressFirst() takes these; for a caller that skipped it
        commitPending();
        sync();
        return true;
    }
    d->reopenWords = false;
    if (const std::optional<Kind> kind = toolKind()) {
        // 3.6, step 4: the selected objects move, a text is edited, a number
        // picked up; anything else inside the selection is drawn on.
        if (onSelected(p)) {
            d->reopenWords = m_picked.size() == 1;
            startMove(m_picked, p, modifiers);
            sync();
            return true;
        }
        // A text under the text tool, a number under the number tool: picked
        // up, so a drag moves it; a click on the text opens it for typing
        // (at the release, finishGesture()).
        const annot::Item* under = find(itemAt(p));
        if (under && under->kind == *kind && (*kind == Kind::Text || *kind == Kind::Number)) {
            d->reopenWords = m_picked == QList<quint32> {under->id};
            m_picked = {under->id};
            startMove(m_picked, p, modifiers);
            sync();
            return true;
        }
        m_picked.clear();
        if (!room().contains(p)) {
            emit notice(m_editor ? tr("只能在图片里画") : tr("只能在选区里画"));
            sync();
            return true;
        }
        if (*kind == Kind::Text)
            beginNewText(p);
        else
            startDraw(*kind, p, modifiers);
        sync();
        return true;
    }
    // 3.6, step 5: without a tool an object is picked up; Shift adds it to
    // the selected ones, or takes it from them; a press elsewhere lets them
    // go and, in the overlay, leaves the press to the session; in the editor
    // it starts a box that selects what it touches.
    const bool shift = modifiers & Qt::ShiftModifier;
    if (const std::optional<quint32> hit = itemAt(p)) {
        if (shift) {
            if (!m_picked.removeOne(*hit))
                m_picked.append(*hit);
            sync();
            return true; // taken; nothing to drag
        }
        if (!m_picked.contains(*hit))
            m_picked = {*hit};
        startMove(m_picked, p, modifiers);
        sync();
        return true;
    }
    if (m_editor) {
        startMarquee(p, modifiers);
        sync();
        return true;
    }
    if (!m_picked.isEmpty()) {
        m_picked.clear();
        sync();
    }
    return false;
}

void AnnotationModel::startDraw(Kind kind, QPoint p, Qt::KeyboardModifiers modifiers)
{
    const QPoint at = clampToSelection(p);
    annot::Item item;
    item.id = m_document.nextId;
    item.seq = m_document.nextSeq;
    item.kind = kind;
    item.style = m_styles.forTool(kind, dpr());
    switch (kind) {
    case Kind::Rect:
    case Kind::Ellipse:
    case Kind::Redact:
        if (kind == Kind::Redact)
            item.seed = QRandomGenerator::global()->generate64();
        if (kind == Kind::Redact && !annot::isBrush(item))
            item.style.width = annot::Style {}.width; // the tool's width is the brush's
        if (annot::isBrush(item)) {
            item.points = {annot::pixelCentre(QPointF(at))};
            break;
        }
        item.points = annot::boxPoints(annot::pixelBox(at, at));
        break;
    case Kind::Arrow:
    case Kind::Line:
        item.points = {annot::pixelCentre(QPointF(at)), annot::pixelCentre(QPointF(at))};
        break;
    case Kind::Pen:
    case Kind::Marker:
        if (kind == Kind::Marker && item.style.markerMode == annot::MarkerMode::Rectangle)
            item.points = annot::boxPoints(annot::pixelBox(at, at));
        else if (kind == Kind::Marker && item.style.markerMode == annot::MarkerMode::Line)
            item.points = {annot::pixelCentre(at), annot::pixelCentre(at)};
        else
            item.points = {annot::pixelCentre(QPointF(at))};
        break;
    case Kind::Number:
        item.points = {annot::pixelCentre(QPointF(at))};
        item.series = m_document.currentSeries;
        break;
    case Kind::Text:
        return; // beginNewText()
    }
    d->before = item;
    d->command = std::make_unique<annot::AddItems>(QList<annot::Item> {item});
    d->applied = false;
    d->dragging = false;
    d->pressAt = at;
    d->last = at;
    d->modifiers = modifiers;
    m_gesture = Gesture::Draw;
    if (kind == Kind::Line && item.style.lineMode == annot::LineMode::Polyline) {
        d->pathNodes = {item.points.first()};
        touched(d->command->apply(m_document));
        d->applied = true;
        m_picked = {item.id};
        emit notice(tr("单击添加节点 · 点击起点闭合 · 双击/回车结束 · 退格撤回 · Esc 取消"));
    }
    if (kind == Kind::Number) {
        // One click puts a number down (3.9); a drag then carries it.
        touched(d->command->apply(m_document));
        d->applied = true;
    }
}

void AnnotationModel::startMove(const QList<quint32>& ids, QPoint p, Qt::KeyboardModifiers modifiers)
{
    const annot::Item* item = ids.isEmpty() ? nullptr : find(ids.first());
    if (!item)
        return;
    auto move = std::make_unique<annot::MoveItems>(ids, QPoint());
    move->merge = false; // a drag is a step of its own
    d->before = *item;
    d->moveIds = ids;
    d->command = std::move(move);
    d->applied = true; // moved by nothing: the document is as it would be
    d->dragging = false;
    d->pressAt = p;
    d->last = p;
    d->modifiers = modifiers;
    m_gesture = Gesture::Move;
}

void AnnotationModel::startCrop(int handle, QPoint p)
{
    auto crop = std::make_unique<annot::SetCrop>(m_document, m_document.crop);
    d->cropStart = m_document.crop;
    d->handle = handle;
    d->command = std::move(crop);
    d->applied = true; // cropped to itself
    d->dragging = false;
    d->pressAt = p;
    d->last = p;
    d->modifiers = {};
    m_gesture = Gesture::Crop;
}

// A box dragged over the empty picture selects the objects it touches; with
// Shift they join the ones selected already.
void AnnotationModel::startMarquee(QPoint p, Qt::KeyboardModifiers modifiers)
{
    const bool shift = modifiers & Qt::ShiftModifier;
    d->marqueeBase = shift ? m_picked : QList<quint32>();
    if (!shift)
        m_picked.clear();
    d->marqueeBox = QRect(p, QSize(1, 1));
    d->command.reset();
    d->applied = false;
    d->dragging = false;
    d->pressAt = p;
    d->last = p;
    d->modifiers = modifiers;
    m_gesture = Gesture::Marquee;
}

void AnnotationModel::startReshape(int handle, QPoint p, Qt::KeyboardModifiers modifiers)
{
    const annot::Item* item = find(single());
    if (!item)
        return;
    auto reshape = std::make_unique<annot::Reshape>(item->id, *item, *item);
    reshape->merge = false;
    d->before = *item;
    d->handle = handle;
    d->own = handlePixel(*item, handle);
    d->command = std::move(reshape);
    d->applied = true; // reshaped to itself
    d->dragging = false;
    d->pressAt = p;
    d->last = p;
    d->modifiers = modifiers;
    m_gesture = Gesture::Reshape;
}

annot::Item AnnotationModel::shapeTo(QPoint p, bool constrain) const
{
    const QRect room = this->room();
    const QPoint to = clampToSelection(p);
    const annot::Item& start = d->before;
    if (annot::isBoxItem(start)) {
        // The press's pixel and the one under the mouse, both inside (3.4):
        // the box as reshaped() spans it from its 1 x 1 start.
        const QPoint target = constrain ? fitSquare(d->pressAt, to, room) : to;
        return annot::reshaped(start, int(annot::BoxHandle::BottomRight), QPointF(target), constrain);
    }
    if (annot::isLineItem(start)) {
        annot::Item made = annot::reshaped(start, int(annot::LineHandle::End), QPointF(to), constrain);
        if (annot::isLine(start.kind) && start.style.lineMode == annot::LineMode::Bezier) {
            const QPointF delta = made.points[1] - made.points[0];
            const QPointF bend(-delta.y() / 4, delta.x() / 4);
            made.points.append(annot::pixelCentre(clampToSelection(annot::pixelAt(made.points[0] + delta / 3 + bend))));
            made.points.append(annot::pixelCentre(clampToSelection(annot::pixelAt(made.points[0] + delta * (2.0 / 3) + bend))));
        }
        return constrain ? fitLine(start, made, room) : made;
    }
    annot::Item made = start; // a Number goes where the mouse is
    made.points = {annot::pixelCentre(QPointF(to))};
    return made;
}

annot::Item AnnotationModel::strokeTo(const annot::Item& stroke, QPoint p, bool constrain) const
{
    const QPoint to = clampToSelection(p);
    const annot::Item made = annot::reshaped(stroke, int(annot::LineHandle::End), QPointF(to), constrain);
    return constrain ? fitLine(stroke, made, room()) : made;
}

void AnnotationModel::move(QPoint p, Qt::KeyboardModifiers modifiers)
{
    if (m_gesture == Gesture::None || (!d->command && m_gesture != Gesture::Marquee))
        return;
    d->last = p;
    d->modifiers = modifiers;
    if (!d->dragging && (p - d->pressAt).manhattanLength() >= dragStart())
        d->dragging = true;
    updateGesture();
    sync();
}

// The gesture's command brought to where the mouse is: reverted, changed
// and applied again, the old place and the new one drawn again (3.7).
void AnnotationModel::updateGesture()
{
    if (!d->command && m_gesture != Gesture::Marquee)
        return;
    const bool constrain = d->modifiers & Qt::ShiftModifier;
    switch (m_gesture) {
    case Gesture::Draw: {
        auto* add = static_cast<annot::AddItems*>(d->command.get());
        if (add->items.isEmpty())
            return;
        const annot::Item& current = add->items[0];
        // A stroke keeps every point from the press on; a shape is made
        // again from the press, once the mouse went past the drag start.
        annot::Item next = current;
        if (pathActive()) {
            next.points = d->pathNodes;
            QPointF preview = annot::pixelCentre(clampToSelection(d->last));
            if (constrain)
                preview = annot::pixelCentre(clampToSelection(annot::pixelAt(annot::snapAngle(next.points.last(), preview))));
            if (next.points.size() == 1 || preview != next.points.last())
                next.points.append(preview);
        } else if (annot::isStrokeItem(current))
            next = strokeTo(current, d->last, constrain);
        else if (d->dragging)
            next = shapeTo(d->last, constrain);
        const bool show = pathActive() || d->dragging || current.kind == Kind::Number;
        if (next == current && show == d->applied)
            return;
        std::optional<QRectF> grown;
        if (d->applied && show && annot::isStroke(current.kind)) // a brush moves every redaction (its change says so)
            grown = strokeGrowth(current, next);
        annot::Change change;
        if (d->applied)
            change = add->revert(m_document);
        add->items[0] = std::move(next);
        d->applied = show;
        if (show)
            change.merge(add->apply(m_document));
        if (grown) {
            change.dirty = *grown; // the stroke's recipe still goes (change.ids)
            change.parts.clear();
        }
        touched(change);
        break;
    }
    case Gesture::Crop: {
        if (!d->dragging)
            return;
        auto* crop = static_cast<annot::SetCrop*>(d->command.get());
        const QRect canvas(QPoint(0, 0), m_document.canvas);
        const QRect to = dragHandle(d->cropStart, Handle(d->handle), d->last - d->pressAt, canvas);
        if (to == crop->after)
            return;
        annot::Change change = crop->revert(m_document);
        crop->after = to;
        change.merge(crop->apply(m_document));
        touched(change);
        break;
    }
    case Gesture::Marquee: {
        const QRect box = spanRect(d->pressAt, d->last);
        d->marqueeBox = box;
        QList<quint32> picked = d->marqueeBase;
        if (d->dragging) {
            for (const annot::Item& item : std::as_const(m_document.items)) {
                if (!picked.contains(item.id) && annot::bounds(m_document, item).intersects(QRectF(box)))
                    picked.append(item.id);
            }
        }
        m_picked = picked;
        break;
    }
    case Gesture::Move: {
        if (!d->dragging)
            return;
        auto* move = static_cast<annot::MoveItems*>(d->command.get());
        const QPoint delta = d->last - d->pressAt;
        if (delta == move->delta)
            return;
        annot::Change change = move->revert(m_document);
        move->delta = delta;
        change.merge(move->apply(m_document));
        touched(change);
        break;
    }
    case Gesture::Reshape: {
        if (!d->dragging)
            return;
        auto* reshape = static_cast<annot::Reshape*>(d->command.get());
        const QRect canvas(QPoint(0, 0), m_document.canvas);
        const QPoint to = clampInto(d->own + (d->last - d->pressAt), canvas);
        annot::Item after = annot::reshaped(d->before, d->handle, QPointF(to), constrain);
        if (after == reshape->after)
            return;
        annot::Change change = reshape->revert(m_document);
        reshape->after = std::move(after);
        change.merge(reshape->apply(m_document));
        touched(change);
        break;
    }
    case Gesture::None:
        break;
    }
}

void AnnotationModel::release(QPoint p)
{
    if (pathActive())
        return;
    if (m_gesture == Gesture::None)
        return;
    if (p != d->last) {
        d->last = p;
        if (!d->dragging && (p - d->pressAt).manhattanLength() >= dragStart())
            d->dragging = true;
        updateGesture();
    }
    finishGesture();
    sync();
}

bool AnnotationModel::pathActive() const
{
    return !d->pathNodes.isEmpty();
}

bool AnnotationModel::finishPath(bool closed)
{
    if (!pathActive())
        return false;
    if (d->pathNodes.size() < 2) {
        cancelGesture();
        return true;
    }
    auto* add = static_cast<annot::AddItems*>(d->command.get());
    annot::Change change;
    if (d->applied)
        change = add->revert(m_document);
    add->items[0].points = d->pathNodes;
    add->items[0].closed = closed && d->pathNodes.size() >= 3;
    change.merge(add->apply(m_document));
    d->applied = true;
    touched(change);
    finishGesture();
    sync();
    return true;
}

void AnnotationModel::finishGesture()
{
    d->pathNodes.clear();
    std::unique_ptr<annot::Command> command = std::move(d->command);
    const Gesture gesture = std::exchange(m_gesture, Gesture::None);
    const bool applied = std::exchange(d->applied, false);
    // A shape spans from the press to the release, both inside the
    // selection: a drag along its edge, out of it, made nothing wider.
    const QPoint end = gesture == Gesture::Draw ? clampToSelection(d->last) : d->last;
    const bool shortDrag = (end - d->pressAt).manhattanLength() < dragStart();
    d->dragging = false;
    if (gesture == Gesture::Marquee)
        d->marqueeBox = QRect(); // the objects it touched stay selected
    if (!command || !applied)
        return; // nothing was drawn: a click
    switch (gesture) {
    case Gesture::Draw: {
        auto* add = static_cast<annot::AddItems*>(command.get());
        if (add->items.isEmpty())
            return;
        annot::Item& item = add->items[0];
        // A shape is its press and its release: those closer than a drag
        // are a click, not a shape (a stroke shows its path instead).
        const bool polyline = item.kind == Kind::Line && item.style.lineMode == annot::LineMode::Polyline;
        bool nothing = item.kind != Kind::Number && !annot::isStrokeItem(item) && !polyline && shortDrag;
        if (annot::isLine(item.kind))
            nothing = nothing || item.points.value(0) == item.points.value(1);
        if (nothing) {
            touched(add->revert(m_document));
            return;
        }
        if (annot::isStrokeItem(item)) {
            const QList<QPointF> simple = annot::simplifyStroke(item.points);
            if (simple != item.points) {
                annot::Change change = add->revert(m_document);
                item.points = simple;
                change.merge(add->apply(m_document));
                touched(change);
            }
        }
        const quint32 id = item.id;
        if (annot::isBrush(item)) {
            // Drawn as a mosaic meanwhile (draft()): the real kind is made once now.
            annot::Change redraw;
            redraw.ids = {id};
            redraw.redactions = true;
            redraw.dirty = QRectF(annot::itemBox(item));
            touched(redraw);
        }
        if (item.kind == Kind::Number) {
            // Typing goes on into its words; the step joins the stack when
            // they end, so the number and its words are one undo step.
            if (d->placing)
                m_stack.pushApplied(std::move(d->placing), now());
            d->placing.reset(static_cast<annot::AddItems*>(command.release()));
            m_picked = {id};
            beginWordsEdit(*find(id), QPoint(-1, -1));
            break;
        }
        m_stack.pushApplied(std::move(command), now());
        m_picked = {id}; // the tool stays; what it drew can be adjusted at once (3.6)
        break;
    }
    case Gesture::Move: {
        if (!static_cast<annot::MoveItems*>(command.get())->delta.isNull()) {
            m_stack.pushApplied(std::move(command), now());
            break;
        }
        // A click, not a drag: with the text tool on a text, it is typed in
        // (3.8), the caret where the press was.
        if (toolKind() == Kind::Text) {
            if (const annot::Item* item = find(d->before.id); item && item->kind == Kind::Text)
                beginEdit(*item, d->pressAt);
        } else if (toolKind() == Kind::Number) {
            // A click on a number selects it (keys act on it); a click on its words, or
            // a second click on it, opens its words.
            if (const annot::Item* item = find(d->before.id);
                item && item->kind == Kind::Number && (d->reopenWords || onWords(*item, d->pressAt)))
                beginWordsEdit(*item, onWords(*item, d->pressAt) ? d->pressAt : QPoint(-1, -1));
        }
        break;
    }
    case Gesture::Reshape: {
        const auto* reshape = static_cast<annot::Reshape*>(command.get());
        if (reshape->after == reshape->before)
            return;
        m_stack.pushApplied(std::move(command), now());
        break;
    }
    case Gesture::Crop: {
        const auto* crop = static_cast<annot::SetCrop*>(command.get());
        if (crop->after == crop->before)
            return;
        m_stack.pushApplied(std::move(command), now());
        break;
    }
    case Gesture::Marquee:
    case Gesture::None:
        break;
    }
}

void AnnotationModel::cancelGesture()
{
    d->pathNodes.clear();
    if (m_gesture == Gesture::None)
        return;
    std::unique_ptr<annot::Command> command = std::move(d->command);
    if (m_gesture == Gesture::Marquee) {
        m_picked = d->marqueeBase;
        d->marqueeBox = QRect();
    }
    m_gesture = Gesture::None;
    if (command && d->applied)
        touched(command->revert(m_document));
    d->applied = false;
    d->dragging = false;
    keepValid();
    sync();
}

int AnnotationModel::cursorAt(QPoint p) const
{
    if (!active())
        return -1;
    switch (m_gesture) {
    case Gesture::Move:
        return Qt::SizeAllCursor;
    case Gesture::Reshape:
        return handleCursor(d->before, d->handle);
    case Gesture::Crop:
        return cursorFor(Handle(d->handle));
    case Gesture::Marquee:
    case Gesture::Draw:
        return Qt::CrossCursor;
    case Gesture::None:
        break;
    }
    if (m_picking)
        return Qt::CrossCursor;
    if (const int handle = handleAt(p); handle >= 0)
        return handleCursor(*find(single()), handle);
    if (const int crop = cropHandleAt(p); crop != 0)
        return cursorFor(Handle(crop));
    if (const std::optional<Kind> kind = toolKind()) {
        if (onSelected(p))
            return Qt::SizeAllCursor;
        if (const annot::Item* under = find(itemAt(p))) {
            if (*kind == Kind::Text && under->kind == Kind::Text)
                return Qt::IBeamCursor;
            if (*kind == Kind::Number && under->kind == Kind::Number)
                return Qt::SizeAllCursor;
        }
        if (room().contains(p))
            return *kind == Kind::Text ? Qt::IBeamCursor : Qt::CrossCursor;
        return -1;
    }
    return itemAt(p) ? Qt::SizeAllCursor : -1;
}

bool AnnotationModel::doubleClick(QPoint p)
{
    if (finishPath())
        return true;
    if (!active())
        return false;
    if (m_editing)
        return true; // the text box's own (a word selected)
    const annot::Item* item = find(itemAt(p));
    if (item && (item->kind == Kind::Text || item->kind == Kind::Number)) {
        // The second press began a gesture that the double click replaces.
        if (m_gesture != Gesture::None)
            cancelGesture();
        item = find(itemAt(p));
        if (!item)
            return true;
        if (item->kind == Kind::Text) {
            beginEdit(*item, p);
        } else if (onWords(*item, p)) {
            beginWordsEdit(*item, p);
        } else {
            m_picked = {item->id};
            m_numberEdit = item->id;
        }
        sync();
        return true;
    }
    // A double click copies only on the bare picture without a tool (3.6):
    // on an object, or with a tool on, it is the model's and does nothing.
    return item != nullptr || m_tool >= 0;
}

bool AnnotationModel::wheel(int angleDelta, qint64 nowMs)
{
    if (!styleTarget())
        return false;
    if (angleDelta == 0 || m_gesture != Gesture::None)
        return true;
    if (d->wheelSum != 0 && (angleDelta > 0) != (d->wheelSum > 0))
        d->wheelSum = 0; // turned the other way: start again
    d->wheelSum += angleDelta;
    if (std::abs(d->wheelSum) < kNotch)
        return true;
    const int direction = d->wheelSum > 0 ? 1 : -1;
    d->wheelSum = 0;
    if (d->notched && nowMs - d->lastNotch < kNotchMs)
        return true;
    d->notched = true;
    d->lastNotch = nowMs;
    stepSize(direction);
    return true;
}

// ---- keys ------------------------------------------------------------------

bool AnnotationModel::key(int key, Qt::KeyboardModifiers modifiers)
{
    if (!active() || m_editing)
        return false; // the text box has the keys
    const Qt::KeyboardModifiers mods = modifiers & ~Qt::KeypadModifier;
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool other = mods & (Qt::AltModifier | Qt::MetaModifier);
    if (pathActive() && !other) {
        if (!ctrl && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
            finishPath();
            return true;
        }
        if ((!ctrl && (key == Qt::Key_Backspace || key == Qt::Key_Delete)) || (ctrl && key == Qt::Key_Z && !shift)) {
            undo();
            return true;
        }
    }
    if (m_gesture != Gesture::None && key == Qt::Key_Shift) {
        // Shift pressed while the mouse holds still: the square or the 15
        // degree step shows at once, not at the next move.
        d->modifiers |= Qt::ShiftModifier;
        updateGesture();
        sync();
        return false;
    }

    if (!ctrl && !other && shift && key == Qt::Key_B) {
        smartRedact(); // Shift+B: not the tool, whose key is B alone
        return true;
    }

    enum class Action { None, Tool, Undo, Redo, Delete, Nudge, Smaller, Bigger, Lower, Raise, Pick, SelectAll, Duplicate };
    Action action = Action::None;
    int toolIndex = -1;
    QPoint delta;
    bool toEnd = false;
    if (!ctrl && !other) {
        // A tool's letter or digit, alone or with Shift (3.5); never with
        // Ctrl (Ctrl + digit is PixPin's).
        for (qsizetype i = 0; i < annot::tools().size() && action == Action::None; ++i) {
            const annot::ToolInfo& t = annot::tools()[i];
            const bool digit = t.digit >= 1 && t.digit <= 9 && key == Qt::Key_0 + t.digit;
            if (key == int(t.letter.toUpper().unicode()) || digit) {
                action = Action::Tool;
                toolIndex = int(i);
            }
        }
        if (action == Action::None) {
            const int step = shift ? 10 : 1;
            switch (key) {
            case Qt::Key_I:
                action = Action::Pick;
                break;
            case Qt::Key_Delete:
            case Qt::Key_Backspace:
                action = Action::Delete;
                break;
            case Qt::Key_Left:
                action = Action::Nudge;
                delta = QPoint(-step, 0);
                break;
            case Qt::Key_Right:
                action = Action::Nudge;
                delta = QPoint(step, 0);
                break;
            case Qt::Key_Up:
                action = Action::Nudge;
                delta = QPoint(0, -step);
                break;
            case Qt::Key_Down:
                action = Action::Nudge;
                delta = QPoint(0, step);
                break;
            case Qt::Key_BracketLeft:
                action = shift ? Action::None : Action::Smaller;
                break;
            case Qt::Key_BracketRight:
                action = shift ? Action::None : Action::Bigger;
                break;
            default:
                break;
            }
        }
    } else if (ctrl && !other) {
        switch (key) {
        case Qt::Key_A:
            action = shift ? Action::None : Action::SelectAll;
            break;
        case Qt::Key_D:
            action = shift ? Action::None : Action::Duplicate;
            break;
        case Qt::Key_Z:
            action = shift ? Action::Redo : Action::Undo;
            break;
        case Qt::Key_Y:
            action = shift ? Action::None : Action::Redo;
            break;
        // Ctrl+Shift+[ comes as { on most layouts.
        case Qt::Key_BracketLeft:
        case Qt::Key_BraceLeft:
            action = Action::Lower;
            toEnd = shift || key == Qt::Key_BraceLeft;
            break;
        case Qt::Key_BracketRight:
        case Qt::Key_BraceRight:
            action = Action::Raise;
            toEnd = shift || key == Qt::Key_BraceRight;
            break;
        default:
            break;
        }
    }
    if (action == Action::None)
        return false;
    if (m_gesture != Gesture::None)
        return true; // while the button is held these wait; they would tear the gesture

    switch (action) {
    case Action::Tool:
        setTool(m_tool == toolIndex ? -1 : toolIndex); // the key that opened a tool closes it
        return true;
    case Action::Undo:
        undo();
        return true;
    case Action::Redo:
        redo();
        return true;
    case Action::Delete:
        if (m_picked.isEmpty())
            return false;
        deleteSelection();
        return true;
    case Action::Nudge:
        if (m_picked.isEmpty())
            return false; // the session nudges the selection instead
        nudge(delta);
        return true;
    case Action::Pick:
        if (!styleTarget())
            return false;
        pickColor(!m_picking);
        return true;
    case Action::SelectAll:
        if (m_document.items.isEmpty())
            return false;
        selectAll();
        return true;
    case Action::Duplicate:
        if (m_picked.isEmpty())
            return false;
        duplicateSelection();
        return true;
    case Action::Smaller:
    case Action::Bigger:
        if (!styleTarget())
            return false;
        stepSize(action == Action::Bigger ? 1 : -1);
        return true;
    case Action::Lower:
    case Action::Raise:
        if (!single())
            return false;
        reorder(action == Action::Raise ? 1 : -1, toEnd);
        return true;
    case Action::None:
        break;
    }
    return false;
}

void AnnotationModel::nudge(QPoint delta)
{
    if (m_picked.isEmpty())
        return;
    auto move = std::make_unique<annot::MoveItems>(m_picked, delta);
    move->merge = true; // a run of presses is one step (3.7)
    push(std::move(move));
    sync();
}

bool AnnotationModel::escape()
{
    if (!active())
        return false;
    if (m_numberEdit) {
        m_numberEdit.reset();
        sync();
        return true;
    }
    if (m_editing) {
        commitPending();
        return true;
    }
    if (m_gesture != Gesture::None) {
        cancelGesture();
        return true;
    }
    if (m_picking) {
        pickColor(false);
        return true;
    }
    return false;
}

void AnnotationModel::commitPending()
{
    finishPath();
    if (m_editing)
        commitText(m_editing->text);
}

// ---- from QML --------------------------------------------------------------

void AnnotationModel::beginNewText(QPoint p)
{
    Editing editing;
    editing.at = clampToSelection(p);
    editing.style = m_styles.forTool(Kind::Text, dpr());
    m_editing = editing;
    m_picked.clear();
    m_numberEdit.reset();
}

void AnnotationModel::beginEdit(const annot::Item& item, QPoint p)
{
    Editing editing;
    editing.id = item.id;
    editing.at = annot::pixelAt(item.points.value(0));
    editing.text = item.text;
    editing.click = p - editing.at;
    editing.style = item.style;
    m_editing = editing;
    m_picked.clear();
    m_numberEdit.reset();
    emit changed(annot::bounds(m_document, item)); // the canvas leaves it out now
}

annot::NumberGeometry AnnotationModel::geometryOf(const annot::Item& item) const
{
    const int number = annot::numbers(m_document).value(item.id, 1);
    return annot::numberGeometry(item, number, dpr(), annot::formatOf(m_document, item.series));
}

bool AnnotationModel::onWords(const annot::Item& item, QPoint p) const
{
    if (item.kind != Kind::Number)
        return false;
    const annot::NumberGeometry g = geometryOf(item);
    const QPointF q = annot::pixelCentre(QPointF(p));
    return !g.wordsBox.isEmpty() && QRectF(g.wordsBox).contains(q) && !g.disc.contains(q);
}

void AnnotationModel::beginWordsEdit(const annot::Item& item, QPoint click)
{
    Editing editing;
    editing.id = item.id;
    editing.words = true;
    editing.at = geometryOf(item).wordsOrigin;
    editing.text = item.text;
    editing.click = click.x() < 0 ? QPoint(-1, -1) : click - editing.at;
    editing.style = item.style;
    m_editing = editing;
    m_picked.clear();
    m_numberEdit.reset();
    emit changed(annot::bounds(m_document, item)); // the canvas leaves its words out now
}

void AnnotationModel::setEditingText(const QString& text)
{
    if (!m_editing)
        return;
    m_editing->text = plainText(text);
    // The box shows these words already: editing.text follows them without
    // a signal, so the box is not set again under the caret.
    d->shown.editing[u"text"_s] = m_editing->text;
}

void AnnotationModel::commitText(const QString& typed)
{
    if (!m_editing)
        return;
    const QString text = plainText(typed);
    if (m_editing->words) {
        commitWords(text);
        return;
    }
    const quint32 old = m_editing->id;
    QRectF area;
    if (const annot::Item* item = find(old ? std::optional<quint32>(old) : std::nullopt))
        area = annot::bounds(m_document, *item); // drawn again: it was left out
    std::unique_ptr<annot::Command> command = textCommand(m_document, text);
    quint32 id = old;
    if (command && old == 0)
        id = static_cast<annot::AddItems*>(command.get())->items[0].id;
    m_editing.reset();
    if (command) {
        annot::Change change = m_stack.push(std::move(command), m_document, now());
        change.dirty |= area;
        touched(change);
    } else if (!area.isEmpty()) {
        emit changed(area);
    }
    m_picked.clear();
    if (id != 0 && find(id))
        m_picked = {id};
    sync();
}

// The end of a Number's words: no words, or the words unchanged, change
// nothing; the number just put down joins the stack with its words, one step.
void AnnotationModel::commitWords(const QString& typed)
{
    const quint32 id = m_editing->id;
    const QString words = typed.trimmed();
    const annot::Item* item = find(id);
    QRectF area;
    if (item)
        area = annot::bounds(m_document, *item); // drawn again: it was left without its words
    m_editing.reset();
    std::unique_ptr<annot::AddItems> placing;
    if (d->placing && !d->placing->items.isEmpty() && d->placing->items[0].id == id)
        placing = std::move(d->placing);
    if (item && words != item->text) {
        if (placing) {
            annot::Retext retext(m_document, id, words);
            annot::Change change = retext.apply(m_document);
            change.dirty |= area;
            placing->items[0].text = words; // done again, it comes back with them
            touched(change);
        } else {
            push(std::make_unique<annot::Retext>(m_document, id, words));
        }
    } else if (!area.isEmpty()) {
        emit changed(area);
    }
    if (placing)
        m_stack.pushApplied(std::move(placing), now());
    m_picked.clear();
    if (find(id))
        m_picked = {id};
    sync();
}

void AnnotationModel::setPanel(const QVariantMap& change)
{
    finishPath();
    const std::optional<Target> target = styleTarget();
    if (!target || change.isEmpty())
        return;
    // How the series writes its numbers: the document's, and the tool's
    // memory, which the next capture starts with.
    if (change.contains(u"format"_s) && target->kind == Kind::Number && !target->mixed) {
        if (const std::optional<annot::NumberFormat> format = annot::numberFormatFromName(change.value(u"format"_s).toString())) {
            quint32 series = m_document.currentSeries;
            if (const annot::Item* item = find(target->id()))
                series = item->series;
            if (annot::formatOf(m_document, series) != *format)
                push(annot::SetSeries::setFormat(m_document, series, *format));
            if (m_styles.numberFormat() != *format) {
                m_styles.setNumberFormat(*format);
                m_stylesChanged = true;
            }
        }
    }
    // The objects (or the text being typed), in image px. The shape is for
    // the next redaction only: a box does not turn into a stroke.
    QVariantMap forObjects = change;
    forObjects.remove(u"shape"_s);
    forObjects.remove(u"lineMode"_s);
    forObjects.remove(u"markerMode"_s);
    QList<Kind> kinds;
    if (!target->ids.isEmpty()) {
        QList<quint32> ids;
        QList<annot::Style> styles;
        for (const quint32 id : target->ids) {
            const annot::Item* item = find(id);
            if (!item)
                continue;
            if (!kinds.contains(item->kind))
                kinds.append(item->kind);
            annot::Style style = item->style;
            if (applyPanel(forObjects, item->kind, style, dpr()) && style != item->style) {
                ids.append(id);
                styles.append(style);
            }
        }
        if (!ids.isEmpty())
            push(std::make_unique<annot::Restyle>(m_document, ids, styles));
    } else if (target->typing && m_editing) {
        applyPanel(change, Kind::Text, m_editing->style, dpr());
    }
    if (kinds.isEmpty())
        kinds.append(target->kind);
    // A change made on the panel is a preference: the tool remembers it
    // (3.5), the fields changed only, in logical px (StyleMemory at scale
    // 1 keeps them exactly).
    for (const Kind kind : std::as_const(kinds)) {
        annot::Style memory = m_styles.forTool(kind, 1.0);
        if (applyPanel(change, kind, memory, 1.0)) {
            const annot::StyleMemory before = m_styles;
            m_styles.remember(kind, memory, 1.0);
            if (!(m_styles == before))
                m_stylesChanged = true;
        }
    }
    if (change.contains(u"lineMode"_s) || change.contains(u"markerMode"_s))
        m_picked.clear();
    sync();
}

// [ and ], a notch of the wheel: the target's width, size or cell one step
// on, as a panel change, so an object and the tool's memory both take it.
void AnnotationModel::stepSize(int direction)
{
    const std::optional<Target> target = styleTarget();
    if (!target)
        return;
    const Kind kind = target->kind;
    const bool remembered = target->ids.isEmpty() && !target->typing;
    const annot::StyleMemory::Tool& memory = m_styles.tool(kind);
    const annot::Style style = targetStyle(*target);
    const qreal dpr = this->dpr();
    QString field;
    int value = 0;
    int next = 0;
    if (usesWidth(kind)) {
        field = u"width"_s;
        value = remembered ? memory.width : annot::toLogicalPx(style.width, dpr);
        next = stepped(kWidthSteps, value, direction);
    } else if (usesSize(kind)) {
        field = u"size"_s;
        value = remembered ? memory.size : annot::toLogicalPx(style.fontPx, dpr);
        next = stepped(kSizeSteps, value, direction);
    } else if (kind == Kind::Redact && (remembered ? memory.shape : style.shape) == annot::RedactShape::Brush) {
        field = u"width"_s;
        value = remembered ? memory.width : annot::toLogicalPx(style.width, dpr);
        next = stepped(kWidthSteps, value, direction);
    } else if (kind == Kind::Redact) {
        field = u"cell"_s;
        value = remembered ? memory.cell : annot::toLogicalPx(style.cell, dpr);
        next = stepped(kCellSteps, value, direction);
    }
    if (field.isEmpty() || next == value)
        return;
    setPanel(QVariantMap {{field, next}});
}

void AnnotationModel::undo()
{
    if (!active())
        return;
    if (pathActive()) {
        if (d->pathNodes.size() > 1) {
            d->pathNodes.removeLast();
            d->last = annot::pixelAt(d->pathNodes.last());
            updateGesture();
            sync();
        } else {
            cancelGesture();
        }
        return;
    }
    if (m_gesture != Gesture::None) {
        cancelGesture(); // mid-drag, a step back is that drag
        return;
    }
    commitPending();
    if (m_stack.canUndo()) {
        touched(m_stack.undo(m_document));
        keepValid();
    }
    sync();
}

void AnnotationModel::redo()
{
    if (!active())
        return;
    if (m_gesture != Gesture::None) {
        cancelGesture();
        return;
    }
    commitPending();
    if (m_stack.canRedo()) {
        touched(m_stack.redo(m_document));
        keepValid();
    }
    sync();
}

void AnnotationModel::deleteSelection()
{
    if (!active() || m_gesture != Gesture::None)
        return;
    commitPending();
    if (m_picked.isEmpty()) {
        sync();
        return;
    }
    const QList<quint32> ids = std::exchange(m_picked, {});
    if (m_numberEdit && ids.contains(*m_numberEdit))
        m_numberEdit.reset();
    push(std::make_unique<annot::RemoveItems>(ids));
    sync();
}

void AnnotationModel::raise(bool toTop)
{
    reorder(1, toTop);
}

void AnnotationModel::lower(bool toBottom)
{
    reorder(-1, toBottom);
}

// One layer up or down, or to the end, inside the object's own segment
// (redactions stay below everything else, 3.2).
bool AnnotationModel::canReorder(int direction) const
{
    const annot::Item* item = find(single());
    if (!item)
        return false;
    const int i = annot::indexOf(m_document, item->id);
    const int end = annot::redactionEnd(m_document);
    const int low = item->kind == Kind::Redact ? 0 : end;
    const int high = item->kind == Kind::Redact ? end : int(m_document.items.size());
    return direction > 0 ? i < high - 1 : i > low;
}

void AnnotationModel::reorder(int direction, bool toEnd)
{
    if (!active() || m_gesture != Gesture::None)
        return;
    commitPending();
    if (!canReorder(direction)) {
        sync();
        return;
    }
    const annot::Item* item = find(single());
    const int i = annot::indexOf(m_document, item->id);
    // Reorder's index is in the list without the item: one past the next
    // one up, the one below, or the segment's end (insertionIndex keeps it
    // inside the segment).
    const int low = item->kind == Kind::Redact ? 0 : annot::redactionEnd(m_document);
    const int to = direction > 0 ? (toEnd ? -1 : i + 1) : (toEnd ? low : i - 1);
    push(std::make_unique<annot::Reorder>(item->id, to));
    sync();
}

void AnnotationModel::resetStyle()
{
    const std::optional<Target> target = styleTarget();
    if (!active() || !target)
        return;
    static const annot::StyleMemory defaults;
    QList<Kind> kinds;
    if (!target->ids.isEmpty()) {
        QList<quint32> ids;
        QList<annot::Style> styles;
        for (const quint32 id : target->ids) {
            const annot::Item* item = find(id);
            if (!item)
                continue;
            if (!kinds.contains(item->kind))
                kinds.append(item->kind);
            annot::Style style = defaults.forTool(item->kind, dpr());
            style.lineMode = item->style.lineMode;
            style.markerMode = item->style.markerMode;
            if (style != item->style) {
                ids.append(id);
                styles.append(style);
            }
        }
        if (!ids.isEmpty())
            push(std::make_unique<annot::Restyle>(m_document, ids, styles));
    } else if (target->typing && m_editing) {
        m_editing->style = defaults.forTool(Kind::Text, dpr());
    }
    if (kinds.isEmpty())
        kinds.append(target->kind);
    for (const Kind kind : std::as_const(kinds)) {
        if (m_styles.tool(kind) == annot::StyleMemory::defaults(kind))
            continue;
        m_styles.reset(kind);
        m_stylesChanged = true;
    }
    sync();
}

void AnnotationModel::pickColor(bool on)
{
    if (!active() || on == m_picking)
        return;
    if (on && (m_gesture != Gesture::None || !styleTarget()))
        return;
    m_picking = on;
    sync();
}

void AnnotationModel::selectAll()
{
    if (!active() || m_gesture != Gesture::None)
        return;
    commitPending();
    m_picked.clear();
    for (const annot::Item& item : std::as_const(m_document.items))
        m_picked.append(item.id);
    sync();
}

// Copies of the selected objects, a little down and to the right of them
// (12 logical px), on top; the copies are selected. A number's copy counts
// on from the others, a redaction's draws its noise of its own.
void AnnotationModel::duplicateSelection()
{
    if (!active() || m_gesture != Gesture::None)
        return;
    commitPending();
    if (m_picked.isEmpty())
        return;
    const int offset = std::max(4, annot::toImagePx(12, dpr()));
    quint32 id = m_document.nextId;
    quint32 seq = m_document.nextSeq;
    QList<annot::Item> copies;
    for (const annot::Item& item : std::as_const(m_document.items)) { // in layer order
        if (!m_picked.contains(item.id))
            continue;
        annot::Item copy = annot::translated(item, QPoint(offset, offset));
        copy.id = id++;
        copy.seq = seq++;
        copy.pinned.reset();
        if (copy.kind == Kind::Redact)
            copy.seed = QRandomGenerator::global()->generate64();
        copies.append(copy);
    }
    if (copies.isEmpty())
        return;
    QList<quint32> ids;
    for (const annot::Item& copy : std::as_const(copies))
        ids.append(copy.id);
    auto add = std::make_unique<annot::AddItems>(copies);
    add->label = tr("复制");
    push(std::move(add));
    m_picked = ids;
    sync();
}

void AnnotationModel::newSeries()
{
    if (!active())
        return;
    commitPending();
    m_picked.clear(); // the panel is for the new group now, not for the number just put down in the old one
    push(annot::SetSeries::newSeries(m_document));
    sync();
}

void AnnotationModel::setStart(int start)
{
    if (!active())
        return;
    start = std::clamp(start, 0, kMaxNumber);
    for (const annot::Series& s : m_document.series) {
        if (s.id == m_document.currentSeries && s.start == start)
            return;
    }
    push(annot::SetSeries::setStart(m_document, m_document.currentSeries, start));
    sync();
}

void AnnotationModel::setNumber(int id, const QVariant& value)
{
    const annot::Item* item = find(id > 0 ? std::optional<quint32>(quint32(id)) : std::nullopt);
    if (!item || item->kind != Kind::Number)
        return;
    // Empty (or a negative number, as numberEdit's -1): counted again.
    std::optional<int> pinned;
    if (value.isValid() && !value.isNull()) {
        if (value.metaType().id() == QMetaType::QString) {
            const QString text = value.toString().trimmed();
            if (!text.isEmpty()) {
                bool ok = false;
                const int n = text.toInt(&ok);
                if (!ok)
                    return; // not a number: nothing changes
                pinned = n;
            }
        } else {
            bool ok = false;
            const double n = value.toDouble(&ok);
            if (!ok || !std::isfinite(n))
                return;
            pinned = static_cast<int>(std::lround(std::clamp(n, -1.0, double(kMaxNumber))));
        }
    }
    if (pinned && *pinned < 0)
        pinned.reset();
    if (pinned)
        pinned = std::min(*pinned, kMaxNumber);
    m_numberEdit.reset();
    if (pinned != item->pinned)
        push(std::make_unique<annot::Renumber>(m_document, item->id, pinned));
    sync();
}

void AnnotationModel::closeNumberEdit()
{
    m_numberEdit.reset();
    sync();
}

// ---- smart redaction (4.1) -------------------------------------------------

void AnnotationModel::setSmartHooks(SmartHooks hooks)
{
    d->smart.hooks = std::move(hooks);
}

// The selection (the crop, in the editor) changed: whatever was read is of
// something else now. Read again once it has held still for 300 ms, if the
// settings let the scan start on its own.
void AnnotationModel::smartSchedule()
{
    smartInvalidate();
    Private::Smart& s = d->smart;
    if (!active() || !s.hooks.scan || (s.hooks.prescan && !s.hooks.prescan()))
        return;
    s.timer->start();
}

void AnnotationModel::smartInvalidate()
{
    Private::Smart& s = d->smart;
    s.timer->stop();
    if (s.scanning && s.hooks.cancel)
        s.hooks.cancel();
    ++s.ticket; // a result still on its way is for nothing now
    s.scanning = false;
    s.ready = false;
    s.apply = false;
    s.lines.clear();
    s.gray = QImage();
    s.all.reset();
    s.hits.reset();
    s.rect = QRect();
}

void AnnotationModel::smartStart(bool apply)
{
    Private::Smart& s = d->smart;
    if (!active() || !s.hooks.scan || s.scanning)
        return;
    const QRect canvas(QPoint(0, 0), m_document.canvas);
    const QRect rect = (m_document.crop.isEmpty() ? canvas : m_document.crop) & canvas;
    if (rect.isEmpty())
        return;
    s.rect = rect;
    s.scanning = true;
    s.apply = apply;
    const int ticket = s.ticket;
    QPointer<AnnotationModel> self(this);
    s.hooks.scan(m_base, rect, dpr(), [self, ticket](RedactScan scan) {
        if (self)
            self->smartScanned(ticket, std::move(scan));
    });
    sync();
}

void AnnotationModel::smartScanned(int ticket, RedactScan scan)
{
    Private::Smart& s = d->smart;
    if (ticket != s.ticket)
        return;
    s.scanning = false;
    s.ready = true;
    s.lines = std::move(scan.lines);
    s.gray = std::move(scan.gray);
    s.all.reset();
    s.hits.reset();
    const bool apply = std::exchange(s.apply, false);
    qInfo().noquote() << u"Screen capture: smart redaction found %1 places in %2 lines"_s.arg(smartHits().size()).arg(s.lines.size());
    if (apply)
        smartRedact();
    sync();
}

const QList<RedactHit>& AnnotationModel::smartHits() const
{
    static const QList<RedactHit> none;
    Private::Smart& s = d->smart;
    if (!s.ready)
        return none;
    if (!s.all) {
        // The rules run once per scan; what is hidden since is taken off below.
        const RedactRules rules = s.hooks.rules ? s.hooks.rules() : RedactRules();
        QList<RedactHit> found;
        for (RedactHit& hit : findSensitive(s.lines, rules, {}, s.gray)) {
            hit.box = hit.box.translated(s.rect.topLeft()) & s.rect;
            if (!hit.box.isEmpty())
                found.append(hit);
        }
        s.all = std::move(found);
    }
    if (!s.hits) {
        QRegion hidden;
        for (const QRect& box : annot::redactionBoxes(m_document))
            hidden += box;
        QList<RedactHit> left;
        for (const RedactHit& hit : *s.all) {
            qint64 area = 0;
            if (!hidden.isEmpty()) {
                for (const QRect& r : hidden & QRegion(hit.box))
                    area += qint64(r.width()) * r.height();
            }
            // Hidden by 80 % or more already: not offered again.
            if (area * 10 < qint64(hit.box.width()) * hit.box.height() * 8)
                left.append(hit);
        }
        s.hits = std::move(left);
    }
    return *s.hits;
}

int AnnotationModel::smartFound() const
{
    const int n = int(smartHits().size());
    return n > 0 ? n : -1;
}

bool AnnotationModel::smartBusy() const
{
    return d->smart.scanning && d->smart.apply;
}

void AnnotationModel::smartRedact()
{
    if (!active())
        return;
    commitPending();
    Private::Smart& s = d->smart;
    if (!s.ready) {
        // Not read yet (the scan is off, or still under way): read now, and hide when done.
        if (s.scanning) {
            s.apply = true;
        } else {
            smartStart(true);
            if (!s.scanning) {
                emit notice(tr("没找到要打码的内容"));
                return;
            }
        }
        emit notice(tr("正在识别文字…"));
        sync();
        return;
    }
    const QList<RedactHit> hits = smartHits();
    if (hits.isEmpty()) {
        emit notice(tr("没找到要打码的内容"));
        return;
    }
    // In the tool's remembered style, as a box: a brush has no place on a line of text.
    annot::Style style = m_styles.forTool(Kind::Redact, dpr());
    style.shape = annot::RedactShape::Box;
    style.width = annot::Style {}.width;
    QList<annot::Item> items;
    for (const RedactHit& hit : hits) {
        annot::Item item;
        item.kind = Kind::Redact;
        item.style = style;
        // At least a third of the box's height a cell, so that big letters do not show through.
        item.style.cell = std::max(style.cell, int(std::lround(0.3 * hit.box.height())));
        item.seed = QRandomGenerator::global()->generate64();
        item.points = annot::boxPoints(hit.box);
        items.append(item);
    }
    QElapsedTimer clock;
    clock.start();
    auto add = std::make_unique<annot::AddItems>(items);
    add->label = QCoreApplication::translate("Annotation", "智能打码");
    m_picked.clear();
    push(std::move(add));
    QString word = tr("已打码 %1 处 · Ctrl+Z 撤销").arg(hits.size());
    if (!s.told) {
        s.told = true;
        word += tr(" · 自动找的可能有漏，发出去前再看一眼");
    }
    emit notice(word);
    qInfo().noquote() << u"Screen capture: smart redaction hid %1 places (%2 ms to put them on)"_s.arg(hits.size()).arg(clock.elapsed());
    sync();
}

} // namespace ws::capture

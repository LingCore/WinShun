#include "PinTools.h"

#include "Geometry.h"

#include <QTransform>

#include <algorithm>
#include <cmath>

namespace ws::capture {

QImage turnedImage(const QImage& image, bool clockwise)
{
    if (image.isNull())
        return image;
    QTransform turn;
    turn.rotate(clockwise ? 90 : -90); // Qt turns a quarter exactly
    return image.transformed(turn, Qt::FastTransformation);
}

QImage flippedImage(const QImage& image, bool horizontal)
{
    return image.mirrored(horizontal, !horizontal);
}

QPoint recenteredOrigin(const QRect& before, QSize after)
{
    return QPoint(before.x() + (before.width() - after.width()) / 2, before.y() + (before.height() - after.height()) / 2);
}

PinDoubleClick pinDoubleClickFromName(const QString& name)
{
    if (name == u"close")
        return PinDoubleClick::Close;
    if (name == u"collapse")
        return PinDoubleClick::Collapse;
    if (name == u"none")
        return PinDoubleClick::None;
    return PinDoubleClick::Reset;
}

QSize tileThumbSize(QSize image, double dpr, int box)
{
    if (image.isEmpty())
        return QSize(1, 1);
    const double limit = std::max(1.0, std::round(box * dpr));
    const double scale = std::min({1.0, limit / image.width(), limit / image.height()});
    return QSize(std::max(1, static_cast<int>(std::lround(image.width() * scale))),
        std::max(1, static_cast<int>(std::lround(image.height() * scale))));
}

QSize edgeTabSize(double dpr)
{
    return QSize(static_cast<int>(std::lround(112 * dpr)), static_cast<int>(std::lround(28 * dpr)));
}

QRect tileRect(QPoint wanted, QSize size, const QRect& work)
{
    return moveInto(QRect(wanted, size), work);
}

QRect edgeTabRect(int centerX, QSize size, const QRect& work, const std::vector<QRect>& taken, int gap)
{
    const int lowest = work.left();
    const int highest = std::max(lowest, work.right() + 1 - size.width());
    const int wanted = std::clamp(centerX - size.width() / 2, lowest, highest);
    const auto at = [&](int x) { return QRect(QPoint(x, work.top()), size); };
    // How far right the tabs in the way of a tab at `x` reach; -1 for none.
    const auto blocked = [&](int x) {
        int reach = -1;
        for (const QRect& t : taken) {
            if (t.adjusted(-gap, 0, gap, 0).intersects(at(x)))
                reach = std::max(reach, t.right() + 1 + gap);
        }
        return reach;
    };
    for (int x = wanted; x <= highest;) {
        const int next = blocked(x);
        if (next < 0)
            return at(x);
        x = next;
    }
    // No room right of the wish: from the left end.
    for (int x = lowest; x < wanted;) {
        const int next = blocked(x);
        if (next < 0)
            return at(x);
        x = next;
    }
    return at(wanted);
}

QPoint expandedOrigin(QPoint savedOrigin, QPoint tileAtCollapse, QPoint tileNow)
{
    return savedOrigin + (tileNow - tileAtCollapse);
}

Corner cornerAt(const QRect& picture, QPoint p, int reach)
{
    if (picture.isEmpty())
        return Corner::None;
    const int r = std::max(1, std::min({reach, picture.width() / 3, picture.height() / 3}));
    const auto close = [r, p](int x, int y) { return std::abs(p.x() - x) <= r && std::abs(p.y() - y) <= r; };
    const int right = picture.right() + 1; // the corner points are the outer edges' ends
    const int bottom = picture.bottom() + 1;
    if (close(picture.left(), picture.top()))
        return Corner::TopLeft;
    if (close(right, picture.top()))
        return Corner::TopRight;
    if (close(picture.left(), bottom))
        return Corner::BottomLeft;
    if (close(right, bottom))
        return Corner::BottomRight;
    return Corner::None;
}

Corner entryCorner(const QRect& picture, QPoint p)
{
    if (picture.isEmpty())
        return Corner::None;
    const bool left = 2 * (p.x() - picture.left()) < picture.width();
    const bool top = 2 * (p.y() - picture.top()) < picture.height();
    if (top)
        return left ? Corner::TopLeft : Corner::TopRight;
    return left ? Corner::BottomLeft : Corner::BottomRight;
}

QRect closeButtonRect(const QRect& picture, Corner corner, double dpr)
{
    if (corner == Corner::None || picture.isEmpty())
        return {};
    const int size = static_cast<int>(std::lround(24 * dpr));
    const int inset = static_cast<int>(std::lround(8 * dpr));
    if (picture.width() < 3 * size || picture.height() < 3 * size)
        return {};
    const bool left = corner == Corner::TopLeft || corner == Corner::BottomLeft;
    const bool top = corner == Corner::TopLeft || corner == Corner::TopRight;
    const int x = left ? picture.left() + inset : picture.left() + picture.width() - inset - size;
    const int y = top ? picture.top() + inset : picture.top() + picture.height() - inset - size;
    return {x, y, size, size};
}

Resized resizeFromCorner(Corner corner, const QRect& start, QSize image, QPoint cursor, double minZoom, double maxZoom)
{
    Resized out {start.topLeft(), 1.0};
    if (image.isEmpty() || corner == Corner::None)
        return out;
    const int right = start.right() + 1;
    const int bottom = start.bottom() + 1;
    // The corner that stays, and which way the picture grows from it.
    const bool keepLeft = corner == Corner::TopRight || corner == Corner::BottomRight;
    const bool keepTop = corner == Corner::BottomLeft || corner == Corner::BottomRight;
    const int anchorX = keepLeft ? start.left() : right;
    const int anchorY = keepTop ? start.top() : bottom;
    const double dx = keepLeft ? cursor.x() - anchorX : anchorX - cursor.x();
    const double dy = keepTop ? cursor.y() - anchorY : anchorY - cursor.y();
    const double zoom = std::clamp(std::max(dx / image.width(), dy / image.height()), minZoom, maxZoom);
    const QSize size = zoomedSize(image, zoom);
    out.zoom = zoom;
    out.origin = QPoint(keepLeft ? anchorX : anchorX - size.width(), keepTop ? anchorY : anchorY - size.height());
    return out;
}

namespace {

struct OrderedLine {
    const RedactLine* line = nullptr;
    QRectF box;
    int row = 0;
};

struct VisualRow {
    int first = 0;
    int count = 0;
    double top = 0;
    double bottom = 0;
    double centerY() const { return (top + bottom) * 0.5; }
    double height() const { return bottom - top; }
};

struct OrderedLayout {
    std::vector<OrderedLine> items;
    std::vector<VisualRow> rows;
};

OrderedLayout orderLines(const QList<RedactLine>& lines)
{
    struct Raw {
        const RedactLine* line = nullptr;
        QRectF box;
        double centerY = 0;
        double height = 0;
    };
    std::vector<Raw> raw;
    raw.reserve(static_cast<std::size_t>(lines.size()));
    for (const RedactLine& line : lines) {
        if (line.text.isEmpty() || line.quad.size() < 4)
            continue;
        const QRectF box = line.quad.boundingRect();
        if (box.width() <= 0 || box.height() <= 0)
            continue;
        raw.push_back({&line, box, box.center().y(), box.height()});
    }
    std::ranges::stable_sort(raw, [](const Raw& a, const Raw& b) { return a.centerY < b.centerY; });
    std::vector<std::vector<Raw>> grouped;
    for (Raw& r : raw) {
        if (!grouped.empty()) {
            const Raw& head = grouped.back().front();
            if (std::abs(r.centerY - head.centerY) < 0.5 * std::min(r.height, head.height)) {
                grouped.back().push_back(r);
                continue;
            }
        }
        grouped.emplace_back();
        grouped.back().push_back(r);
    }
    OrderedLayout layout;
    layout.items.reserve(raw.size());
    layout.rows.reserve(grouped.size());
    for (int rowIndex = 0; rowIndex < static_cast<int>(grouped.size()); ++rowIndex) {
        std::vector<Raw>& group = grouped[static_cast<std::size_t>(rowIndex)];
        std::ranges::stable_sort(group, [](const Raw& a, const Raw& b) { return a.box.left() < b.box.left(); });
        VisualRow row;
        row.first = static_cast<int>(layout.items.size());
        row.count = static_cast<int>(group.size());
        row.top = group.front().box.top();
        row.bottom = group.front().box.bottom();
        for (const Raw& r : group) {
            row.top = std::min(row.top, r.box.top());
            row.bottom = std::max(row.bottom, r.box.bottom());
            layout.items.push_back({r.line, r.box, rowIndex});
        }
        layout.rows.push_back(row);
    }
    return layout;
}

std::optional<PickedLine> sliceLine(const RedactLine& line, const QRectF& box, double x0, double x1)
{
    const double left = std::min(x0, x1);
    const double right = std::max(x0, x1);
    const double t0 = (left - box.left()) / box.width();
    const double t1 = (right - box.left()) / box.width();
    if (t1 <= 0.0 || t0 >= 1.0)
        return std::nullopt;
    if ((t0 <= 0.02 && t1 >= 0.98) || line.chars.isEmpty())
        return PickedLine {box, line.text};
    int first = -1;
    int last = -1;
    double leftT = 1.0;
    double rightT = 0.0;
    for (const auto& [index, t] : line.chars) {
        if (t < t0 || t > t1 || index < 0 || index >= line.text.size())
            continue;
        first = first < 0 ? index : std::min(first, index);
        last = std::max(last, index);
        leftT = std::min<double>(leftT, t);
        rightT = std::max<double>(rightT, t);
    }
    if (first < 0)
        return std::nullopt;
    if (last + 1 < line.text.size() && line.text[last].isHighSurrogate() && line.text[last + 1].isLowSurrogate())
        ++last;
    const double half = 0.5 / static_cast<double>(std::max<qsizetype>(1, line.text.size()));
    const double l = std::max(0.0, leftT - half);
    const double r = std::min(1.0, rightT + half);
    return PickedLine {QRectF(box.left() + l * box.width(), box.top(), (r - l) * box.width(), box.height()),
        line.text.mid(first, last - first + 1)};
}

double squaredDistToRect(const QRectF& rect, QPointF point)
{
    const double dx = point.x() < rect.left() ? rect.left() - point.x() : point.x() > rect.right() ? point.x() - rect.right() : 0.0;
    const double dy = point.y() < rect.top() ? rect.top() - point.y() : point.y() > rect.bottom() ? point.y() - rect.bottom() : 0.0;
    return dx * dx + dy * dy;
}

int hitIndex(const OrderedLayout& layout, QPointF point, double padding)
{
    const double pad = std::max(0.0, padding);
    int best = -1;
    double bestScore = 0.0;
    for (int i = 0; i < static_cast<int>(layout.items.size()); ++i) {
        const QRectF& box = layout.items[static_cast<std::size_t>(i)].box;
        if (!box.adjusted(-pad, -pad, pad, pad).contains(point))
            continue;
        const double dist = squaredDistToRect(box, point);
        const double centerDist = std::hypot(point.x() - box.center().x(), point.y() - box.center().y());
        const double score = dist * 1000.0 + centerDist;
        if (best < 0 || score < bestScore) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

} // namespace

QList<PickedLine> allPickedLines(const QList<RedactLine>& lines)
{
    const OrderedLayout layout = orderLines(lines);
    QList<PickedLine> picked;
    picked.reserve(static_cast<qsizetype>(layout.items.size()));
    for (const OrderedLine& item : layout.items)
        picked.append({item.box, item.line->text});
    return picked;
}

bool hasTextAt(const QList<RedactLine>& lines, QPointF point, double padding)
{
    const double pad = std::max(0.0, padding);
    for (const RedactLine& line : lines) {
        if (line.text.isEmpty() || line.quad.size() < 4)
            continue;
        const QRectF box = line.quad.boundingRect();
        if (box.width() > 0 && box.height() > 0 && box.adjusted(-pad, -pad, pad, pad).contains(point))
            return true;
    }
    return false;
}

std::optional<PickedLine> pickedLineAt(const QList<RedactLine>& lines, QPointF point, double padding)
{
    const OrderedLayout layout = orderLines(lines);
    const int idx = hitIndex(layout, point, padding);
    if (idx < 0)
        return std::nullopt;
    const OrderedLine& item = layout.items[static_cast<std::size_t>(idx)];
    return PickedLine {item.box, item.line->text};
}

QList<PickedLine> pickedLines(const QList<RedactLine>& lines, const QRectF& selection)
{
    const QRectF sel = selection.normalized();
    const OrderedLayout layout = orderLines(lines);
    QList<PickedLine> picked;
    for (const OrderedLine& item : layout.items) {
        // Touched through a good part of its height, and along its length at all.
        const double top = std::max(item.box.top(), sel.top());
        const double bottom = std::min(item.box.bottom(), sel.bottom());
        if (bottom - top < item.box.height() * 0.4 || std::min(item.box.right(), sel.right()) <= std::max(item.box.left(), sel.left()))
            continue;
        if (auto part = sliceLine(*item.line, item.box, sel.left(), sel.right()))
            picked.append(std::move(*part));
    }
    return picked;
}

QList<PickedLine> pickedLinesFlow(const QList<RedactLine>& lines, QPointF from, QPointF to)
{
    const OrderedLayout layout = orderLines(lines);
    if (layout.items.empty())
        return {};
    const auto nearestItem = [&](QPointF p) {
        if (const int hit = hitIndex(layout, p, 2.0); hit >= 0)
            return hit;
        int best = 0;
        double bestDist = squaredDistToRect(layout.items[0].box, p);
        for (int i = 1; i < static_cast<int>(layout.items.size()); ++i) {
            const double d = squaredDistToRect(layout.items[static_cast<std::size_t>(i)].box, p);
            if (d < bestDist) {
                bestDist = d;
                best = i;
            }
        }
        return best;
    };
    const int anchorIdx = nearestItem(from);
    const int anchorRow = layout.items[static_cast<std::size_t>(anchorIdx)].row;
    const double anchorX = from.x();

    // Locate the active endpoint by row first, then horizontally on that row.
    // Slight vertical drift within one row height above the first or below the
    // last row still tracks `to.x()` so horizontal drags on a single line stay
    // steady; dragging clearly past the top/bottom takes to the very start/end.
    int activeIdx = anchorIdx;
    double activeX = to.x();
    const VisualRow& firstRow = layout.rows.front();
    const VisualRow& lastRow = layout.rows.back();
    if (to.y() < firstRow.top - firstRow.height()) {
        activeIdx = 0;
        activeX = layout.items.front().box.left();
    } else if (to.y() > lastRow.bottom + lastRow.height()) {
        activeIdx = static_cast<int>(layout.items.size()) - 1;
        activeX = layout.items.back().box.right();
    } else {
        int rowIdx = 0;
        double bestY = 1e18;
        for (int r = 0; r < static_cast<int>(layout.rows.size()); ++r) {
            const VisualRow& row = layout.rows[static_cast<std::size_t>(r)];
            const double dy = to.y() < row.top ? row.top - to.y() : to.y() > row.bottom ? to.y() - row.bottom : 0.0;
            const double tie = std::abs(to.y() - row.centerY()) * 1e-4;
            if (dy + tie < bestY) {
                bestY = dy + tie;
                rowIdx = r;
            }
        }
        const VisualRow& row = layout.rows[static_cast<std::size_t>(rowIdx)];
        const bool forward = rowIdx > anchorRow || (rowIdx == anchorRow && to.x() >= from.x());
        bool resolved = false;
        for (int i = row.first; i < row.first + row.count; ++i) {
            const QRectF& box = layout.items[static_cast<std::size_t>(i)].box;
            if (to.x() >= box.left() && to.x() <= box.right()) {
                activeIdx = i;
                activeX = to.x();
                resolved = true;
                break;
            }
        }
        if (!resolved) {
            const int lastInRow = row.first + row.count - 1;
            if (to.x() < layout.items[static_cast<std::size_t>(row.first)].box.left()) {
                activeIdx = row.first;
                activeX = layout.items[static_cast<std::size_t>(row.first)].box.left();
            } else if (to.x() > layout.items[static_cast<std::size_t>(lastInRow)].box.right()) {
                activeIdx = lastInRow;
                activeX = layout.items[static_cast<std::size_t>(lastInRow)].box.right();
            } else {
                // In a horizontal gap between two pieces on the same row.
                int leftPiece = row.first;
                for (int i = row.first; i < lastInRow; ++i) {
                    if (to.x() > layout.items[static_cast<std::size_t>(i)].box.right())
                        leftPiece = i;
                }
                if (forward) {
                    activeIdx = leftPiece;
                    activeX = layout.items[static_cast<std::size_t>(leftPiece)].box.right();
                } else {
                    activeIdx = leftPiece + 1;
                    activeX = layout.items[static_cast<std::size_t>(leftPiece + 1)].box.left();
                }
            }
        }
    }

    int startIdx = anchorIdx;
    double startX = anchorX;
    int endIdx = activeIdx;
    double endX = activeX;
    if (startIdx > endIdx || (startIdx == endIdx && startX > endX)) {
        std::swap(startIdx, endIdx);
        std::swap(startX, endX);
    }

    QList<PickedLine> picked;
    if (startIdx == endIdx) {
        const OrderedLine& item = layout.items[static_cast<std::size_t>(startIdx)];
        if (auto part = sliceLine(*item.line, item.box, startX, endX))
            picked.append(std::move(*part));
        return picked;
    }

    const OrderedLine& firstItem = layout.items[static_cast<std::size_t>(startIdx)];
    const OrderedLine& lastItem = layout.items[static_cast<std::size_t>(endIdx)];
    const bool multiRow = firstItem.row != lastItem.row;
    const double spanLeft = std::min({firstItem.box.left(), lastItem.box.left(), from.x(), to.x()});
    const double spanRight = std::max({firstItem.box.right(), lastItem.box.right(), from.x(), to.x()});

    for (int i = startIdx; i <= endIdx; ++i) {
        const OrderedLine& item = layout.items[static_cast<std::size_t>(i)];
        if (i == startIdx) {
            if (auto part = sliceLine(*item.line, item.box, startX, item.box.right()))
                picked.append(std::move(*part));
        } else if (i == endIdx) {
            if (auto part = sliceLine(*item.line, item.box, item.box.left(), endX))
                picked.append(std::move(*part));
        } else {
            if (multiRow && (item.box.right() <= spanLeft || item.box.left() >= spanRight))
                continue; // another column outside the drag's horizontal span
            picked.append({item.box, item.line->text});
        }
    }
    return picked;
}

QString pickedText(const QList<PickedLine>& picked)
{
    QString out;
    double lastY = 0;
    double lastHeight = 0;
    bool any = false;
    for (const PickedLine& p : picked) {
        if (any) {
            const bool sameRow = std::abs(p.box.center().y() - lastY) < 0.5 * std::min(p.box.height(), lastHeight);
            out += sameRow ? u" " : u"\n";
        }
        out += p.text;
        lastY = p.box.center().y();
        lastHeight = p.box.height();
        any = true;
    }
    return out;
}

} // namespace ws::capture

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

struct Candidate {
    PickedLine line;
    double centerY = 0;
    double height = 0;
};

} // namespace

QList<PickedLine> pickedLines(const QList<RedactLine>& lines, const QRectF& selection)
{
    const QRectF sel = selection.normalized();
    std::vector<Candidate> found;
    for (const RedactLine& line : lines) {
        if (line.text.isEmpty() || line.quad.size() < 4)
            continue;
        const QRectF box = line.quad.boundingRect();
        if (box.width() <= 0 || box.height() <= 0)
            continue;
        // Touched through a good part of its height, and along its length at all.
        const double top = std::max(box.top(), sel.top());
        const double bottom = std::min(box.bottom(), sel.bottom());
        if (bottom - top < box.height() * 0.4 || std::min(box.right(), sel.right()) <= std::max(box.left(), sel.left()))
            continue;
        // How far along the line the selection reaches, 0 to 1.
        const double t0 = (sel.left() - box.left()) / box.width();
        const double t1 = (sel.right() - box.left()) / box.width();
        Candidate c;
        c.centerY = box.center().y();
        c.height = box.height();
        if ((t0 <= 0.02 && t1 >= 0.98) || line.chars.isEmpty()) {
            c.line = {box, line.text};
        } else {
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
                continue;
            c.line.text = line.text.mid(first, last - first + 1);
            // The characters' middles bound the box to within half a character.
            const double half = 0.5 / static_cast<double>(std::max<qsizetype>(1, line.text.size()));
            const double l = std::max(0.0, leftT - half);
            const double r = std::min(1.0, rightT + half);
            c.line.box = QRectF(box.left() + l * box.width(), box.top(), (r - l) * box.width(), box.height());
        }
        found.push_back(std::move(c));
    }
    std::ranges::stable_sort(found, [](const Candidate& a, const Candidate& b) { return a.centerY < b.centerY; });
    // Pieces side by side on one row: left to right.
    std::vector<std::vector<Candidate>> rows;
    for (Candidate& c : found) {
        if (!rows.empty()) {
            const Candidate& head = rows.back().front();
            if (std::abs(c.centerY - head.centerY) < 0.5 * std::min(c.height, head.height)) {
                rows.back().push_back(std::move(c));
                continue;
            }
        }
        rows.emplace_back();
        rows.back().push_back(std::move(c));
    }
    QList<PickedLine> picked;
    for (std::vector<Candidate>& row : rows) {
        std::ranges::stable_sort(row, [](const Candidate& a, const Candidate& b) { return a.line.box.left() < b.line.box.left(); });
        for (Candidate& c : row)
            picked.append(std::move(c.line));
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

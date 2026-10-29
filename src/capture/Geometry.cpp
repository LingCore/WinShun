#include "Geometry.h"

#include <algorithm>
#include <cmath>
#include <tuple>

using namespace Qt::StringLiterals;

namespace ws::capture {

double bufferScale(int logicalSize, double dpr)
{
    if (logicalSize <= 0 || dpr <= 0)
        return dpr > 0 ? dpr : 1.0;
    return std::round(logicalSize * dpr) / logicalSize;
}

double toLogical(double physical, int logicalSize, double dpr)
{
    return physical / bufferScale(logicalSize, dpr);
}

int toPhysical(double logical, double dpr)
{
    // Qt's logical position is the native one divided by the DPR: the
    // product is a whole number up to rounding errors.
    return static_cast<int>(std::floor(logical * dpr + 0.5));
}

QRect spanRect(QPoint a, QPoint b)
{
    const int left = std::min(a.x(), b.x());
    const int top = std::min(a.y(), b.y());
    const int right = std::max(a.x(), b.x());
    const int bottom = std::max(a.y(), b.y());
    return QRect(left, top, right - left + 1, bottom - top + 1);
}

QRect moveInto(QRect r, const QRect& bounds)
{
    r.setWidth(std::min(r.width(), bounds.width()));
    r.setHeight(std::min(r.height(), bounds.height()));
    r.moveLeft(std::clamp(r.left(), bounds.left(), bounds.left() + bounds.width() - r.width()));
    r.moveTop(std::clamp(r.top(), bounds.top(), bounds.top() + bounds.height() - r.height()));
    return r;
}

Handle handleAt(const QRect& selection, QPoint p, int reach)
{
    if (selection.isEmpty())
        return Handle::None;
    // Edges as the lines between pixels: left of the first column, right of
    // the last one.
    const int left = selection.left();
    const int right = selection.left() + selection.width();
    const int top = selection.top();
    const int bottom = selection.top() + selection.height();
    // A pixel is "at" an edge line when the line is within reach of its centre.
    const double x = p.x() + 0.5;
    const double y = p.y() + 0.5;
    const bool inX = x >= left - reach && x <= right + reach;
    const bool inY = y >= top - reach && y <= bottom + reach;
    if (!inX || !inY)
        return Handle::None;
    const bool nearLeft = std::abs(x - left) <= reach;
    const bool nearRight = std::abs(x - right) <= reach;
    const bool nearTop = std::abs(y - top) <= reach;
    const bool nearBottom = std::abs(y - bottom) <= reach;
    // A tiny selection: the nearer edge wins.
    const bool l = nearLeft && (!nearRight || std::abs(x - left) <= std::abs(x - right));
    const bool r = nearRight && !l;
    const bool t = nearTop && (!nearBottom || std::abs(y - top) <= std::abs(y - bottom));
    const bool b = nearBottom && !t;
    if (l && t)
        return Handle::TopLeft;
    if (r && t)
        return Handle::TopRight;
    if (l && b)
        return Handle::BottomLeft;
    if (r && b)
        return Handle::BottomRight;
    if (l)
        return Handle::Left;
    if (r)
        return Handle::Right;
    if (t)
        return Handle::Top;
    if (b)
        return Handle::Bottom;
    return selection.contains(p) ? Handle::Move : Handle::None;
}

QRect dragHandle(const QRect& start, Handle handle, QPoint delta, const QRect& bounds)
{
    if (handle == Handle::None)
        return start;
    if (handle == Handle::Move)
        return moveInto(start.translated(delta), bounds);
    // Edge lines (exclusive right and bottom), each kept on the screen.
    const int minX = bounds.left();
    const int maxX = bounds.left() + bounds.width();
    const int minY = bounds.top();
    const int maxY = bounds.top() + bounds.height();
    int left = start.left();
    int right = start.left() + start.width();
    int top = start.top();
    int bottom = start.top() + start.height();
    const bool movesLeft = handle == Handle::Left || handle == Handle::TopLeft || handle == Handle::BottomLeft;
    const bool movesRight = handle == Handle::Right || handle == Handle::TopRight || handle == Handle::BottomRight;
    const bool movesTop = handle == Handle::Top || handle == Handle::TopLeft || handle == Handle::TopRight;
    const bool movesBottom = handle == Handle::Bottom || handle == Handle::BottomLeft || handle == Handle::BottomRight;
    if (movesLeft)
        left = std::clamp(left + delta.x(), minX, maxX);
    if (movesRight)
        right = std::clamp(right + delta.x(), minX, maxX);
    if (movesTop)
        top = std::clamp(top + delta.y(), minY, maxY);
    if (movesBottom)
        bottom = std::clamp(bottom + delta.y(), minY, maxY);
    if (left > right)
        std::swap(left, right);
    if (top > bottom)
        std::swap(top, bottom);
    // At least one pixel, grown away from the screen's edge.
    if (right == left) {
        if (right < maxX)
            ++right;
        else
            --left;
    }
    if (bottom == top) {
        if (bottom < maxY)
            ++bottom;
        else
            --top;
    }
    return QRect(left, top, right - left, bottom - top);
}

QRect nudge(const QRect& r, int dx, int dy, bool resize, const QRect& bounds)
{
    if (r.isEmpty())
        return r;
    if (!resize)
        return moveInto(r.translated(dx, dy), bounds);
    const int maxRight = bounds.left() + bounds.width();
    const int maxBottom = bounds.top() + bounds.height();
    const int right = std::clamp(r.left() + r.width() + dx, r.left() + 1, maxRight);
    const int bottom = std::clamp(r.top() + r.height() + dy, r.top() + 1, maxBottom);
    return QRect(r.left(), r.top(), right - r.left(), bottom - r.top());
}

int cursorFor(Handle handle)
{
    switch (handle) {
    case Handle::Move:
        return Qt::SizeAllCursor;
    case Handle::Left:
    case Handle::Right:
        return Qt::SizeHorCursor;
    case Handle::Top:
    case Handle::Bottom:
        return Qt::SizeVerCursor;
    case Handle::TopLeft:
    case Handle::BottomRight:
        return Qt::SizeFDiagCursor;
    case Handle::TopRight:
    case Handle::BottomLeft:
        return Qt::SizeBDiagCursor;
    case Handle::None:
        break;
    }
    return Qt::CrossCursor;
}

namespace {

int clampStart(int start, int length, int lower, int upperExclusive)
{
    return std::max(lower, std::min(start, upperExclusive - length));
}

} // namespace

ToolbarPlace placeToolbar(const QRect& selection, QSize bar, const QRect& screen, int gap)
{
    ToolbarPlace place;
    const int screenRight = screen.left() + screen.width();
    const int screenBottom = screen.top() + screen.height();
    const int selRight = selection.left() + selection.width();
    const int selBottom = selection.top() + selection.height();
    const int x = clampStart(selRight - bar.width(), bar.width(), screen.left(), screenRight);
    int y = selBottom + gap;
    if (y + bar.height() <= screenBottom) {
        place.where = ToolbarPlace::Below;
    } else if (selection.top() - gap - bar.height() >= screen.top()) {
        place.where = ToolbarPlace::Above;
        y = selection.top() - gap - bar.height();
    } else {
        place.where = ToolbarPlace::Inside;
        y = clampStart(selBottom - gap - bar.height(), bar.height(), screen.top(), screenBottom);
    }
    place.topLeft = QPoint(x, y);
    return place;
}

std::array<QRect, 4> longCaptureBorder(const QRect& selection, const QRect& screen, int thickness)
{
    if (selection.isEmpty() || screen.isEmpty())
        return {};
    const int t = std::max(1, thickness);
    const QRect outer = selection.adjusted(-t, -t, t, t).intersected(screen);
    return {QRect(outer.left(), outer.top(), outer.width(), t),
        QRect(outer.left(), outer.bottom() + 1 - t, outer.width(), t),
        QRect(outer.left(), outer.top() + t, t, outer.height() - 2 * t),
        QRect(outer.right() + 1 - t, outer.top() + t, t, outer.height() - 2 * t)};
}

QPoint placeSizeLabel(const QRect& selection, QSize label, const QRect& screen, int gap)
{
    const int screenRight = screen.left() + screen.width();
    const int x = clampStart(selection.left(), label.width(), screen.left(), screenRight);
    int y = selection.top() - gap - label.height();
    if (y < screen.top())
        y = std::min(selection.top() + gap, screen.top() + screen.height() - label.height());
    return QPoint(x, y);
}

QPoint placeMagnifier(QPoint cursor, QSize box, const QRect& screen, int offset)
{
    const int screenRight = screen.left() + screen.width();
    const int screenBottom = screen.top() + screen.height();
    int x = cursor.x() + 1 + offset;
    if (x + box.width() > screenRight)
        x = cursor.x() - offset - box.width();
    int y = cursor.y() + 1 + offset;
    if (y + box.height() > screenBottom)
        y = cursor.y() - offset - box.height();
    return QPoint(clampStart(x, box.width(), screen.left(), screenRight),
        clampStart(y, box.height(), screen.top(), screenBottom));
}

std::vector<QRect> hitChain(const std::vector<QRect>& windows, const std::function<std::vector<QRect>(std::size_t)>& children,
    QPoint p, const QRect& screen)
{
    std::vector<QRect> chain;
    if (!screen.contains(p))
        return chain;
    const auto add = [&chain](const QRect& r) {
        if (!r.isEmpty() && (chain.empty() || chain.back() != r))
            chain.push_back(r);
    };
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const QRect window = windows[i].intersected(screen);
        if (!window.contains(p))
            continue;
        std::vector<QRect> inside;
        if (children) {
            for (const QRect& c : children(i)) {
                const QRect cut = c.intersected(window);
                if (cut.contains(p) && cut != window)
                    inside.push_back(cut);
            }
        }
        std::ranges::stable_sort(inside, [](const QRect& a, const QRect& b) {
            return qint64(a.width()) * a.height() < qint64(b.width()) * b.height();
        });
        for (const QRect& c : inside)
            add(c);
        add(window);
        break; // the topmost window there hides the rest
    }
    add(screen);
    return chain;
}

QString screenshotFileName(const QDateTime& when, const std::function<bool(const QString&)>& taken)
{
    const QString stem = u"WinShun_"_s + when.toString(u"yyyyMMdd_HHmmss"_s);
    QString name = stem + u".png"_s;
    for (int n = 1; taken && taken(name) && n < 10000; ++n)
        name = stem + u'_' + QString::number(n) + u".png"_s;
    return name;
}

QString colorHex(QRgb color)
{
    return u"#%1%2%3"_s.arg(qRed(color), 2, 16, QChar(u'0'))
        .arg(qGreen(color), 2, 16, QChar(u'0'))
        .arg(qBlue(color), 2, 16, QChar(u'0'))
        .toUpper();
}

// ---- pins ---------------------------------------------------------------------

const std::vector<double>& zoomSteps()
{
    // Tenths from 10% to 200% (a notch is 10%), then the whole multiples
    // (3x … 8x: each pixel a crisp square).
    static const std::vector<double> steps = [] {
        std::vector<double> all;
        for (int tenth = 1; tenth <= 20; ++tenth)
            all.push_back(tenth / 10.0);
        for (const double whole : {3.0, 4.0, 5.0, 6.0, 8.0})
            all.push_back(whole);
        return all;
    }();
    return steps;
}

double nextZoom(double zoom, int steps, double maxZoom)
{
    const std::vector<double>& all = zoomSteps();
    constexpr double eps = 1e-6;
    double z = zoom;
    for (; steps > 0; --steps) {
        const auto up = std::ranges::find_if(all, [z](double s) { return s > z + eps; });
        if (up == all.end() || *up > maxZoom + eps)
            break;
        z = *up;
    }
    for (; steps < 0; ++steps) {
        const auto down = std::ranges::find_if(all.rbegin(), all.rend(), [z](double s) { return s < z - eps; });
        if (down == all.rend())
            break;
        z = *down;
    }
    return z;
}

double maxZoom(QSize image, qint64 budgetPixels)
{
    const double area = std::max<qint64>(1, qint64(image.width()) * image.height());
    return std::clamp(std::sqrt(static_cast<double>(budgetPixels) / area), 1.0, 8.0);
}

QSize zoomedSize(QSize image, double zoom)
{
    if (std::abs(zoom - 1.0) < 1e-9)
        return image;
    return QSize(std::max(1, static_cast<int>(std::lround(image.width() * zoom))),
        std::max(1, static_cast<int>(std::lround(image.height() * zoom))));
}

QPoint zoomAbout(QPoint origin, QPoint cursor, double oldZoom, double newZoom)
{
    if (oldZoom <= 0 || newZoom <= 0)
        return origin;
    // The picture's point under the cursor, in its own pixels (the cursor is
    // at the middle of a screen pixel).
    const double px = (cursor.x() + 0.5 - origin.x()) / oldZoom;
    const double py = (cursor.y() + 0.5 - origin.y()) / oldZoom;
    return QPoint(static_cast<int>(std::lround(cursor.x() + 0.5 - px * newZoom)),
        static_cast<int>(std::lround(cursor.y() + 0.5 - py * newZoom)));
}

double nextOpacity(double opacity, int steps)
{
    const long tenths = std::lround(opacity * 10) + steps;
    return std::clamp(tenths, 1L, 10L) / 10.0;
}

bool pinLetsMouseThrough(bool clickThrough, bool ctrlDown, bool cursorOver, bool busy)
{
    return clickThrough && !busy && !(ctrlDown && cursorOver);
}

PinFrame pinFrame(double dpr)
{
    if (dpr <= 0)
        dpr = 1;
    PinFrame frame;
    frame.border = std::max(1, static_cast<int>(std::lround(dpr)));
    frame.shadow = std::max(2, static_cast<int>(std::lround(8 * dpr)));
    return frame;
}

QRect pinWindowRect(const QRect& picture, const PinFrame& frame)
{
    const int m = frame.margin();
    return picture.adjusted(-m, -m, m, m);
}

// ---- OCR ------------------------------------------------------------------------

double ocrUpscale(QSize image, double dpr, int maxWidth)
{
    if (image.isEmpty() || dpr >= 1.5)
        return 1.0;
    const double scale = std::min(2.0, static_cast<double>(maxWidth) / image.width());
    return scale >= 1.25 ? scale : 1.0;
}

QString ocrOneLine(const QString& text, int most)
{
    return text.simplified().left(most); // simplified(): trimmed, each run of whitespace one space
}

QPoint placeBeside(const QRect& anchor, QSize box, const QRect& area, int gap)
{
    const int areaRight = area.left() + area.width();
    const int areaBottom = area.top() + area.height();
    const int anchorRight = anchor.left() + anchor.width();
    const int anchorBottom = anchor.top() + anchor.height();
    const int x = clampStart(anchor.left(), box.width(), area.left(), areaRight);
    const int y = clampStart(anchor.top(), box.height(), area.top(), areaBottom);
    if (anchorRight + gap + box.width() <= areaRight)
        return QPoint(anchorRight + gap, y);
    if (anchor.left() - gap - box.width() >= area.left())
        return QPoint(anchor.left() - gap - box.width(), y);
    if (anchorBottom + gap + box.height() <= areaBottom)
        return QPoint(x, anchorBottom + gap);
    if (anchor.top() - gap - box.height() >= area.top())
        return QPoint(x, anchor.top() - gap - box.height());
    return QPoint(clampStart(area.left() + (area.width() - box.width()) / 2, box.width(), area.left(), areaRight),
        clampStart(area.top() + (area.height() - box.height()) / 2, box.height(), area.top(), areaBottom));
}

// ---- the Start menu ---------------------------------------------------------------

namespace {

// The longest run of indices whose count reaches `need`, gaps of up to
// `gap` bridged (the strip of desktop between the Start menu and its
// phone panel). {-1, -1} when none does.
std::pair<int, int> longestRun(const std::vector<int>& counts, double need, int gap)
{
    std::pair<int, int> best {-1, -1};
    int start = -1;
    int last = -1;
    for (int i = 0; i < static_cast<int>(counts.size()); ++i) {
        if (counts[i] < need || counts[i] == 0)
            continue;
        if (start < 0 || i - last > gap)
            start = i;
        last = i;
        if (best.first < 0 || last - start > best.second - best.first)
            best = {start, last};
    }
    return best;
}

} // namespace

QRect changedRegion(const QImage& before, const QImage& after, const QRect& area, int threshold, double density)
{
    if (before.size() != after.size() || before.format() != QImage::Format_RGB32 || after.format() != QImage::Format_RGB32)
        return {};
    const QRect box = area.intersected(before.rect());
    if (box.isEmpty())
        return {};
    const auto changed = [threshold](QRgb a, QRgb b) {
        return std::abs(qRed(a) - qRed(b)) > threshold || std::abs(qGreen(a) - qGreen(b)) > threshold
            || std::abs(qBlue(a) - qBlue(b)) > threshold;
    };
    const auto pixel = [](const QImage& image, int x, int y) {
        return reinterpret_cast<const QRgb*>(image.constScanLine(y))[x];
    };
    const int w = box.width();
    const int h = box.height();
    std::vector<unsigned char> mask(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const auto* a = reinterpret_cast<const QRgb*>(before.constScanLine(box.top() + y)) + box.left();
        const auto* b = reinterpret_cast<const QRgb*>(after.constScanLine(box.top() + y)) + box.left();
        for (int x = 0; x < w; ++x)
            mask[static_cast<std::size_t>(y) * w + x] = changed(a[x], b[x]);
    }
    constexpr int kGap = 32;
    const auto columns = [&](int y0, int y1) {
        std::vector<int> counts(w, 0);
        for (int y = y0; y <= y1; ++y)
            for (int x = 0; x < w; ++x)
                counts[x] += mask[static_cast<std::size_t>(y) * w + x];
        return counts;
    };
    const auto rows = [&](int x0, int x1) {
        std::vector<int> counts(h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = x0; x <= x1; ++x)
                counts[y] += mask[static_cast<std::size_t>(y) * w + x];
        return counts;
    };
    // Columns first, against the busiest one (a small flyout changes few
    // pixels of a column); then the rows in those columns; then the
    // columns again in those rows.
    std::vector<int> cols = columns(0, h - 1);
    const int busiest = *std::ranges::max_element(cols);
    if (busiest < 16)
        return {};
    auto [x0, x1] = longestRun(cols, density * busiest, kGap);
    auto [y0, y1] = longestRun(rows(x0, x1), density * (x1 - x0 + 1), kGap);
    if (y0 < 0)
        return {};
    std::tie(x0, x1) = longestRun(columns(y0, y1), density * (y1 - y0 + 1), kGap);
    if (x0 < 0)
        return {};
    // A flyout's shadow falls round it and darkens what is there a little:
    // lines where every changed pixel only got darker, by little, are
    // shadow and come off the edges (measured on the 25H2 Start menu: the
    // edges land on its 2 px border).
    constexpr int kShade = 48;
    const auto shadowy = [&](bool column, int at, int from, int to) {
        int changes = 0;
        int other = 0;
        for (int i = from; i <= to; ++i) {
            const int x = box.left() + (column ? at : i);
            const int y = box.top() + (column ? i : at);
            const QRgb a = pixel(before, x, y);
            const QRgb b = pixel(after, x, y);
            if (!changed(a, b))
                continue;
            ++changes;
            // Darker with the flyout than without it.
            const int dr = qRed(a) - qRed(b), dg = qGreen(a) - qGreen(b), db = qBlue(a) - qBlue(b);
            const bool darker = dr <= 0 && dg <= 0 && db <= 0 && std::min({dr, dg, db}) >= -kShade;
            if (!darker)
                ++other;
        }
        return changes == 0 || other <= (to - from + 1) * 3 / 100;
    };
    while (x0 < x1 && shadowy(true, x0, y0, y1))
        ++x0;
    while (x1 > x0 && shadowy(true, x1, y0, y1))
        --x1;
    while (y0 < y1 && shadowy(false, y0, x0, x1))
        ++y0;
    while (y1 > y0 && shadowy(false, y1, x0, x1))
        --y1;
    return QRect(box.left() + x0, box.top() + y0, x1 - x0 + 1, y1 - y0 + 1);
}

} // namespace ws::capture

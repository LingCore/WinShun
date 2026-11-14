#include "TranslateOverlay.h"

#include "Geometry.h"

#include <QFontMetrics>
#include <QLineF>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace ws::capture {

namespace {

struct Line {
    QRectF box;
    double height = 0;
    QString text;
    bool level = true;
};

struct Open {
    TextBlock block;
    Line last;
    double left = 0;
    double right = 0;
    double heights = 0;
};

bool goesOn(const Open& open, const Line& next)
{
    const Line& last = open.last;
    if (!last.level || !next.level)
        return false;
    const double h = std::max(last.height, next.height);
    if (std::abs(last.height - next.height) > 0.25 * h)
        return false;
    const double gap = next.box.top() - last.box.bottom(); // the reader's boxes are padded: under 0 between close lines
    if (gap < -0.35 * h || gap > 0.9 * h)
        return false;
    if (std::abs(next.box.left() - open.left) > 0.8 * h)
        return false;
    if (last.box.width() < 8 * h || last.box.right() < std::max(open.right, next.box.right()) - 2.5 * h)
        return false; // a label, or a line that ended before the edge: the paragraph's last
    return ocrGoesOn(last.text, next.text);
}

int distance(QRgb a, QRgb b)
{
    return std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
}

// Where the words can go: the box, and for one line the empty ground to its
// right (every pixel there the paper's, near enough), up to the next block on
// these rows or the picture's edge.
int roomToTheRight(const QImage& image, const QRect& box, QRgb paper, const QList<TextBlock>& blocks, qsizetype self)
{
    int limit = image.width() - 2;
    for (qsizetype i = 0; i < blocks.size(); ++i) {
        const QRect& other = blocks[i].box;
        if (i != self && other.left() > box.right() && other.top() <= box.bottom() && other.bottom() >= box.top())
            limit = std::min(limit, other.left() - 4);
    }
    int x = box.right() + 1;
    for (; x <= limit; ++x) {
        bool clear = true;
        for (int y = box.top(); y <= box.bottom() && clear; y += 2)
            clear = distance(image.pixel(x, y), paper) < 30;
        if (!clear)
            break;
    }
    return std::max(0, x - 1 - box.right() - 2);
}

} // namespace

QList<TextBlock> ocrBlocks(const QList<RedactLine>& read)
{
    QList<Line> lines;
    for (const RedactLine& l : read) {
        const QString text = l.text.simplified();
        if (l.quad.size() != 4 || text.isEmpty())
            continue;
        const QPointF along = l.quad[1] - l.quad[0];
        const double height = (QLineF(l.quad[0], l.quad[3]).length() + QLineF(l.quad[1], l.quad[2]).length()) / 2;
        lines.append({l.quad.boundingRect(), height, text, std::abs(along.y()) <= std::abs(along.x()) * 0.05}); // 3 degrees
    }
    std::ranges::stable_sort(lines, [](const Line& a, const Line& b) {
        return a.box.top() != b.box.top() ? a.box.top() < b.box.top() : a.box.left() < b.box.left();
    });
    std::vector<Open> open;
    for (const Line& line : std::as_const(lines)) {
        Open* into = nullptr;
        for (auto it = open.rbegin(); it != open.rend() && !into; ++it) {
            if (goesOn(*it, line))
                into = &*it;
        }
        if (!into) {
            Open o;
            o.block.box = line.box.toAlignedRect();
            o.block.text = line.text;
            o.block.lines = 1;
            o.block.level = line.level;
            o.left = line.box.left();
            o.right = line.box.right();
            o.heights = line.height;
            o.last = line;
            open.push_back(std::move(o));
            continue;
        }
        ocrGlue(into->block.text, line.text);
        into->block.box |= line.box.toAlignedRect();
        ++into->block.lines;
        into->right = std::max(into->right, line.box.right());
        into->heights += line.height;
        into->last = line;
    }
    QList<TextBlock> blocks;
    for (Open& o : open) {
        o.block.lineHeight = o.heights / o.block.lines;
        blocks.append(std::move(o.block));
    }
    return blocks;
}

BlockColors blockColors(const QImage& image, const QRect& asked)
{
    const QRect box = asked.intersected(image.rect());
    if (box.isEmpty())
        return {Qt::white, Qt::black};
    // The paper: the middle value, channel by channel, of the pixels round the box, two outside it.
    const QRect ring = box.adjusted(-2, -2, 2, 2).intersected(image.rect());
    std::array<std::vector<int>, 3> channels;
    const auto take = [&](int x, int y) {
        const QRgb c = image.pixel(x, y);
        channels[0].push_back(qRed(c));
        channels[1].push_back(qGreen(c));
        channels[2].push_back(qBlue(c));
    };
    for (int x = ring.left(); x <= ring.right(); ++x) {
        take(x, ring.top());
        take(x, ring.bottom());
    }
    for (int y = ring.top() + 1; y < ring.bottom(); ++y) {
        take(ring.left(), y);
        take(ring.right(), y);
    }
    std::array<int, 3> middle {};
    for (int i = 0; i < 3; ++i) {
        auto& values = channels[static_cast<std::size_t>(i)];
        std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2));
        middle[static_cast<std::size_t>(i)] = values[values.size() / 2];
    }
    const QRgb paper = qRgb(middle[0], middle[1], middle[2]);
    // The ink: the pixels in the box farthest from the paper (the strokes'
    // cores; their smoothed edges are in between), averaged.
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(box.width()) * box.height() / 20000.0)));
    int farthest = 0;
    for (int y = box.top(); y <= box.bottom(); y += step) {
        for (int x = box.left(); x <= box.right(); x += step)
            farthest = std::max(farthest, distance(image.pixel(x, y), paper));
    }
    if (farthest < 60) // nothing stands out: black on a light ground, white on a dark one
        return {QColor(paper), qGray(paper) >= 128 ? QColor(Qt::black) : QColor(Qt::white)};
    qint64 red = 0, green = 0, blue = 0, count = 0;
    for (int y = box.top(); y <= box.bottom(); y += step) {
        for (int x = box.left(); x <= box.right(); x += step) {
            const QRgb c = image.pixel(x, y);
            if (distance(c, paper) * 10 >= farthest * 8) {
                red += qRed(c);
                green += qGreen(c);
                blue += qBlue(c);
                ++count;
            }
        }
    }
    return {QColor(paper), QColor(static_cast<int>(red / count), static_cast<int>(green / count), static_cast<int>(blue / count))};
}

double strokeWeight(const QImage& image, const QRect& asked, const BlockColors& colors, double lineHeight)
{
    const QRect box = asked.intersected(image.rect());
    const QRgb paper = colors.paper.rgb();
    const int far = distance(colors.ink.rgb(), paper);
    if (box.isEmpty() || far < 60 || lineHeight <= 0)
        return 0;
    // A run's length with its smoothed ends counted for what they cover: a
    // stroke of one and a half pixels is not one of one or of two.
    std::vector<double> runs;
    const int step = std::max(1, box.height() / 48);
    for (int y = box.top(); y <= box.bottom(); y += step) {
        double run = 0;
        for (int x = box.left(); x <= box.right() + 1; ++x) {
            const double cover = x <= box.right() ? std::min(1.0, distance(image.pixel(x, y), paper) / static_cast<double>(far)) : 0;
            if (cover >= 0.2) {
                run += cover;
            } else if (run > 0) {
                runs.push_back(run);
                run = 0;
            }
        }
    }
    if (runs.size() < 12)
        return 0;
    std::ranges::nth_element(runs, runs.begin() + static_cast<std::ptrdiff_t>(runs.size() / 2));
    return runs[runs.size() / 2] / lineHeight;
}

bool boldWords(const QImage& image, const QRect& box, const BlockColors& colors, double lineHeight)
{
    const bool light = qGray(colors.ink.rgb()) > qGray(colors.paper.rgb());
    return strokeWeight(image, box, colors, lineHeight) >= (light ? kBoldStrokeLight : kBoldStroke);
}

Margins blockMargins(const QImage& image, const QRect& asked, const QColor& ground)
{
    const QRect box = asked.intersected(image.rect());
    if (box.isEmpty())
        return {};
    const QRgb paper = ground.rgb();
    Margins m {image.width(), image.width()};
    bool leftEnds = true;
    bool rightEnds = true;
    for (const int y : {box.top() + box.height() / 4, box.center().y(), box.bottom() - box.height() / 4}) {
        int x = box.left() - 1;
        while (x >= 0 && distance(image.pixel(x, y), paper) < 30)
            --x;
        leftEnds = leftEnds && x >= 0;
        m.left = std::min(m.left, box.left() - 1 - x);
        x = box.right() + 1;
        while (x < image.width() && distance(image.pixel(x, y), paper) < 30)
            ++x;
        rightEnds = rightEnds && x < image.width();
        m.right = std::min(m.right, x - box.right() - 1);
    }
    if (!leftEnds)
        m.left = -1;
    if (!rightEnds)
        m.right = -1;
    return m;
}

QImage translatedPicture(const QImage& source, const QList<TextBlock>& blocks, const QStringList& translations, const QFont& family)
{
    if (source.isNull() || blocks.size() != translations.size())
        return {};
    const QImage from = source.convertToFormat(QImage::Format_RGB32);
    QImage out = from;
    struct Job {
        qsizetype index;
        QRect box; // where the new words go
        QRect cover; // what is painted over
        QString text;
        BlockColors colors;
    };
    QList<Job> jobs;
    for (qsizetype i = 0; i < blocks.size(); ++i) {
        const QString text = translations[i].simplified();
        const QRect box = blocks[i].box.intersected(from.rect());
        if (!blocks[i].level || text.isEmpty() || text == blocks[i].text || box.isEmpty())
            continue;
        // The reader's box can stop short of the tails of g and y, and of a stroke's soft edge: a little more is covered.
        const int more = std::clamp(static_cast<int>(std::lround(blocks[i].lineHeight * 0.15)), 2, 6);
        const QRect cover = box.adjusted(-2, -more, 2, more).intersected(from.rect());
        jobs.append({i, box, cover, text, blockColors(from, cover)});
    }
    QPainter painter(&out);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    // Every cover first: a box touching its neighbour does not wipe the neighbour's new words.
    for (const Job& job : std::as_const(jobs))
        painter.fillRect(job.cover, job.colors.paper);
    for (const Job& job : std::as_const(jobs)) {
        const TextBlock& block = blocks[job.index];
        const int most = std::max(9, static_cast<int>(std::lround(block.lineHeight * 0.78)));
        const int least = std::max(9, static_cast<int>(std::lround(most * 0.6)));
        QFont font = family;
        font.setBold(boldWords(from, job.box, job.colors, block.lineHeight));
        QRect area = job.box;
        const bool wraps = block.lines > 1 || job.box.height() > 2.2 * block.lineHeight;
        const Margins margins = wraps ? Margins() : blockMargins(from, job.cover, job.colors.paper);
        int flags = (margins.centred() ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignVCenter;
        if (wraps)
            flags |= Qt::TextWordWrap;
        const auto fits = [&](int size) {
            font.setPixelSize(size);
            const QFontMetrics metrics(font);
            if (flags & Qt::TextWordWrap)
                return metrics.boundingRect(QRect(0, 0, area.width(), 100000), flags, job.text).height() <= area.height() + 2;
            return metrics.horizontalAdvance(job.text) <= area.width();
        };
        int size = most;
        while (size > least && !fits(size))
            --size;
        if (!fits(size) && !(flags & Qt::TextWordWrap)) {
            // One line, too long even small: on over the empty ground to its right (both ways for one in
            // the middle of its button, short of the edges), and as large as that lets it be.
            if (margins.centred()) {
                const int more = std::max(0, std::min(margins.left, margins.right) - 2 - 4);
                area.adjust(-more, 0, more, 0);
            } else {
                area.setWidth(area.width() + roomToTheRight(from, job.box, job.colors.paper.rgb(), blocks, job.index));
            }
            size = most;
            while (size > least && !fits(size))
                --size;
        }
        font.setPixelSize(size);
        painter.setFont(font);
        painter.setPen(job.colors.ink);
        painter.setClipRect(area);
        const QString shown = (flags & Qt::TextWordWrap) ? job.text : QFontMetrics(font).elidedText(job.text, Qt::ElideRight, area.width());
        painter.drawText(area, flags, shown);
    }
    return out;
}

} // namespace ws::capture

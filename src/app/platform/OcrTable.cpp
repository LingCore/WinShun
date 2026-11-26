#include "OcrTable.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr int kMostSpan = 64; // a cell across more rows or columns than this is the model's slip

// Written without spaces between words: Chinese, Japanese, their punctuation
// and full-width forms.
bool spaceless(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 0x2E80 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFF00 && u <= 0xFFEF) || c.isSurrogate();
}

// Words of a line cut out for one cell, and where they stand.
struct Fragment {
    QRectF box;
    QString text;
};

// A cell's words from its fragments: those side by side on a line left to
// right, the lines top to bottom. A space where there is a gap or Latin
// writing; none between Chinese words, on a line or from one line to the next
// (a sentence wrapped in its cell).
QString wordsOf(std::vector<Fragment> fragments)
{
    std::ranges::sort(fragments, {}, [](const Fragment& f) { return f.box.center().y(); });
    std::vector<std::vector<const Fragment*>> lines;
    for (const Fragment& f : fragments) {
        bool placed = false;
        if (!lines.empty()) {
            const Fragment& last = *lines.back().back();
            const double overlap = std::min(last.box.bottom(), f.box.bottom()) - std::max(last.box.top(), f.box.top());
            placed = overlap >= 0.5 * std::min(last.box.height(), f.box.height());
        }
        if (placed)
            lines.back().push_back(&f);
        else
            lines.push_back({&f});
    }
    QString text;
    for (auto& line : lines) {
        std::ranges::sort(line, {}, [](const Fragment* f) { return f->box.left(); });
        const Fragment* before = nullptr;
        for (const Fragment* f : line) {
            if (f->text.isEmpty())
                continue;
            if (!text.isEmpty()) {
                const bool chinese = spaceless(text.back()) && spaceless(f->text.front());
                const bool apart = before && f->box.left() - before->box.right() >= std::min(f->box.height(), before->box.height());
                if (!chinese || apart)
                    text += u' ';
            }
            text += f->text;
            before = f;
        }
    }
    return text;
}

// How far apart two boxes are, 0 when they touch; up and down counts three
// times: a line belongs to the cell beside it rather than the one above.
double apart(const QRectF& a, const QRectF& b)
{
    const double dx = std::max({0.0, a.left() - b.right(), b.left() - a.right()});
    const double dy = std::max({0.0, a.top() - b.bottom(), b.top() - a.bottom()});
    return std::hypot(dx, 3 * dy);
}

QString escaped(const QString& text)
{
    return text.toHtmlEscaped();
}

// Digits a spreadsheet would change when pasted as a number: a leading zero
// lost, a long number rounded and shown with an exponent, "3-4" and "1/2"
// turned into dates, "1E5" into 100000.
bool keptAsText(const QString& text)
{
    static const QRegularExpression digits(u"^\\d+$"_s);
    static const QRegularExpression changed(u"^(\\d{1,2}[-/]\\d{1,2}|\\d+[Ee]\\d+)$"_s);
    const QString t = text.trimmed();
    if (digits.match(t).hasMatch())
        return t.size() > 11 || (t.size() > 1 && t.startsWith(u'0'));
    return changed.match(t).hasMatch();
}

} // namespace

bool OcrTable::standsAsTable() const
{
    QList<int> inColumn;
    int wideRows = 0;
    for (const QStringList& row : rows) {
        int inRow = 0;
        if (inColumn.size() < row.size())
            inColumn.resize(row.size());
        for (qsizetype c = 0; c < row.size(); ++c) {
            if (!row[c].trimmed().isEmpty()) {
                ++inRow;
                ++inColumn[c];
            }
        }
        wideRows += inRow >= 2 ? 1 : 0;
    }
    return wideRows >= 2 && std::ranges::count_if(inColumn, [](int n) { return n >= 2; }) >= 2;
}

QString OcrTable::tabbed() const
{
    QStringList lines;
    for (const QStringList& row : rows) {
        QStringList cells;
        for (QString cell : row)
            cells.append(cell.replace(u'\t', u' ').replace(u'\r', u' ').replace(u'\n', u' '));
        lines.append(cells.join(u'\t'));
    }
    return lines.join(u'\n');
}

QString OcrTable::html() const
{
    // What a merged cell covers but its top left.
    const auto key = [](int row, int column) { return (static_cast<qint64>(row) << 32) | static_cast<quint32>(column); };
    QHash<qint64, Span> starts;
    QSet<qint64> covered;
    for (const Span& s : spans) {
        starts.insert(key(s.row, s.column), s);
        for (int r = s.row; r < s.row + s.rows; ++r) {
            for (int c = s.column; c < s.column + s.columns; ++c) {
                if (r != s.row || c != s.column)
                    covered.insert(key(r, c));
            }
        }
    }
    QString out = u"<table>"_s;
    for (int r = 0; r < rows.size(); ++r) {
        out += u"<tr>"_s;
        for (int c = 0; c < rows[r].size(); ++c) {
            if (covered.contains(key(r, c)))
                continue;
            out += u"<td"_s;
            if (const auto it = starts.constFind(key(r, c)); it != starts.constEnd()) {
                if (it->rows > 1)
                    out += u" rowspan=\"%1\""_s.arg(it->rows);
                if (it->columns > 1)
                    out += u" colspan=\"%1\""_s.arg(it->columns);
            }
            if (keptAsText(rows[r][c]))
                out += u" style='mso-number-format:\"\\@\"'"_s;
            out += u'>' + escaped(rows[r][c]) + u"</td>"_s;
        }
        out += u"</tr>"_s;
    }
    return out + u"</table>"_s;
}

namespace {

// A table's ruling in a screenshot: drawn lines and the edges of filled
// cells are where the picture changes from one row of pixels to the next (or
// one column to the next) along a long straight stretch, the same on either
// side all the way. The tops and feet of a row of digits line up too, but
// are ink and paper in turn.
constexpr int kEdge = 10; // a change of this much in brightness (0…255)
constexpr int kEdgeSame = 6; // either side stays within this of what it was where the stretch began
constexpr int kEdgeGap = 3; // a stretch may be broken this far: where another line crosses it

struct Run {
    int at = 0; // the row (a line across) or column (a line down) it is on
    int from = 0;
    int to = 0; // past its end
};

// The stretches of at least `least` pixels. `across`: lines across the
// picture, found between each row of pixels and the next; else lines down it.
std::vector<Run> runsOf(const std::vector<uchar>& luma, int w, int h, bool across, int least)
{
    std::vector<Run> runs;
    const int lines = (across ? h : w) - 1;
    const int length = across ? w : h;
    for (int line = 0; line < lines; ++line) {
        int from = -1;
        int last = -1; // the last pixel of the stretch
        int one = 0, other = 0; // either side where it began
        for (int along = 0; along <= length; ++along) {
            int a = 0, b = 0;
            if (along < length) {
                const std::size_t at = across ? static_cast<std::size_t>(line) * w + along : static_cast<std::size_t>(along) * w + line;
                a = luma[at];
                b = luma[at + (across ? static_cast<std::size_t>(w) : 1u)];
            }
            const bool edge = along < length && std::abs(a - b) >= kEdge;
            if (from >= 0 && edge && std::abs(a - one) <= kEdgeSame && std::abs(b - other) <= kEdgeSame) {
                last = along;
                continue;
            }
            if (from >= 0 && (along == length || along - last > kEdgeGap)) {
                if (last + 1 - from >= least)
                    runs.push_back({line, from, last + 1});
                from = -1;
            }
            if (from < 0 && edge) { // (a pixel that broke a stretch may begin the next)
                from = last = along;
                one = a;
                other = b;
            }
        }
    }
    return runs;
}

// A ruled line: where it is, and the stretches of it that are drawn.
struct Rule {
    double at = 0;
    std::vector<std::pair<int, int>> drawn; // sorted, apart
};

// The runs as lines: those no further than `near` apart are one (a line's
// two edges, a double line, a fill's edge on a line).
std::vector<Rule> rulesOf(std::vector<Run> runs, double near)
{
    std::ranges::sort(runs, {}, &Run::at);
    std::vector<Rule> rules;
    std::size_t first = 0;
    const auto close = [&](std::size_t end) {
        Rule rule;
        double sum = 0, weight = 0;
        std::vector<std::pair<int, int>> spans;
        for (std::size_t i = first; i < end; ++i) {
            sum += (runs[i].at + 0.5) * (runs[i].to - runs[i].from);
            weight += runs[i].to - runs[i].from;
            spans.emplace_back(runs[i].from, runs[i].to);
        }
        rule.at = sum / weight;
        std::ranges::sort(spans);
        for (const auto& span : spans) {
            if (!rule.drawn.empty() && span.first <= rule.drawn.back().second + kEdgeGap)
                rule.drawn.back().second = std::max(rule.drawn.back().second, span.second);
            else
                rule.drawn.push_back(span);
        }
        rules.push_back(std::move(rule));
    };
    for (std::size_t i = 1; i <= runs.size(); ++i) {
        if (i == runs.size() || runs[i].at - runs[first].at > near) { // from the first of them: no chain of near ones
            close(i);
            first = i;
        }
    }
    return rules;
}

// How much of from…to the line is drawn along.
double drawnAlong(const Rule& rule, double from, double to)
{
    double sum = 0;
    for (const auto& [a, b] : rule.drawn)
        sum += std::max(0.0, std::min<double>(b, to) - std::max<double>(a, from));
    return sum;
}

bool nearAny(const std::vector<Rule>& rules, double at, double tolerance)
{
    return std::ranges::any_of(rules, [&](const Rule& r) { return std::abs(r.at - at) <= tolerance; });
}

} // namespace

namespace table {

Structure structureOf(const QStringList& words, const std::vector<QRectF>& boxes)
{
    Structure s;
    std::vector<std::vector<bool>> taken; // [row][column]: a cell is there
    int row = -1;
    int lastRow = -1; // the last row a cell begins in
    std::size_t nextBox = 0;
    bool open = false;
    Cell pending;
    const auto begin = [&] {
        Cell cell;
        if (nextBox < boxes.size())
            cell.box = boxes[nextBox];
        ++nextBox;
        return cell;
    };
    const auto place = [&](Cell cell) {
        if (row < 0)
            row = 0; // a cell before any row began
        cell.rows = std::clamp(cell.rows, 1, kMostSpan);
        cell.columns = std::clamp(cell.columns, 1, kMostSpan);
        if (taken.size() < static_cast<std::size_t>(row + cell.rows))
            taken.resize(static_cast<std::size_t>(row + cell.rows));
        int column = 0;
        while (static_cast<std::size_t>(column) < taken[static_cast<std::size_t>(row)].size() && taken[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)])
            ++column;
        cell.row = row;
        cell.column = column;
        for (int r = row; r < row + cell.rows; ++r) {
            auto& line = taken[static_cast<std::size_t>(r)];
            if (line.size() < static_cast<std::size_t>(column + cell.columns))
                line.resize(static_cast<std::size_t>(column + cell.columns), false);
            std::fill(line.begin() + column, line.begin() + column + cell.columns, true);
        }
        lastRow = std::max(lastRow, row);
        s.columns = std::max(s.columns, column + cell.columns);
        s.cells.push_back(cell);
    };
    const auto number = [](const QString& word) {
        const qsizetype from = word.indexOf(u'"');
        const qsizetype to = word.lastIndexOf(u'"');
        return from >= 0 && to > from ? word.mid(from + 1, to - from - 1).toInt() : 1;
    };
    for (const QString& word : words) {
        if (word == u"<tr>") {
            if (open) {
                place(pending);
                open = false;
            }
            ++row;
        } else if (word == u"<td></td>") {
            place(begin());
        } else if (word == u"<td") {
            if (open)
                place(pending);
            pending = begin();
            open = true;
        } else if (open && word.startsWith(u" colspan")) {
            pending.columns = number(word);
        } else if (open && word.startsWith(u" rowspan")) {
            pending.rows = number(word);
        } else if (open && word == u"</td>") {
            place(pending);
            open = false;
        }
    }
    if (open)
        place(pending);
    // No rows below the last one a cell begins in: a cell said to reach
    // further down ends there.
    s.rows = lastRow + 1;
    for (Cell& cell : s.cells)
        cell.rows = std::min(cell.rows, s.rows - cell.row);
    return s;
}

Structure ruledStructure(const QImage& picture, double textHeight)
{
    const int w = picture.width();
    const int h = picture.height();
    if (w < 16 || h < 16 || textHeight <= 0)
        return {};
    const QImage rgb = picture.format() == QImage::Format_RGB888 ? picture : picture.convertToFormat(QImage::Format_RGB888);
    std::vector<uchar> luma(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const uchar* line = rgb.constScanLine(y);
        uchar* out = luma.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x)
            out[x] = static_cast<uchar>((line[3 * x] * 77 + line[3 * x + 1] * 150 + line[3 * x + 2] * 29) >> 8);
    }
    // A line across is at least two characters long. A line down may be as
    // short as a row is high (a heading's row, filled another colour, has
    // its own stretch of each line): hardly taller than a character's stroke,
    // but ending on a line across at both ends, which a stroke does not.
    const double near = std::max(3.0, 0.3 * textHeight); // closer lines have no room for words between them
    const double snap = std::max(3.0, 0.12 * textHeight);
    std::vector<Rule> across = rulesOf(runsOf(luma, w, h, true, std::max(24, static_cast<int>(2 * textHeight))), near);
    if (across.size() < 3)
        return {};

    std::vector<Run> standing = runsOf(luma, w, h, false, std::max(8, static_cast<int>(0.9 * textHeight)));
    // (Or at the picture's edge: a table selected inside its outer border.)
    std::erase_if(standing, [&](const Run& run) {
        return !(run.from <= snap || nearAny(across, run.from, snap)) || !(run.to >= h - snap || nearAny(across, run.to, snap));
    });
    std::vector<Rule> down = rulesOf(std::move(standing), near);
    if (down.size() < 2)
        return {};
    // The lines across reaching out past the first and the last line down: a
    // table without its outer borders has its first and last column there.
    double left = down.front().at, right = down.back().at;
    for (const Rule& rule : across) {
        left = std::min<double>(left, rule.drawn.front().first);
        right = std::max<double>(right, rule.drawn.back().second);
    }
    // The same up and down: lines down reaching past the first and the last
    // line across have the first and the last row there.
    double top = across.front().at, bottom = across.back().at;
    for (const Rule& rule : down) {
        top = std::min<double>(top, rule.drawn.front().first);
        bottom = std::max<double>(bottom, rule.drawn.back().second);
    }
    const auto outer = [](double at, int length) { // a line not drawn, all along
        Rule rule;
        rule.at = at;
        rule.drawn.emplace_back(0, length);
        return rule;
    };
    if (down.front().at - left > 2 * textHeight)
        down.insert(down.begin(), outer(left, h));
    if (right - down.back().at > 2 * textHeight)
        down.push_back(outer(right, h));
    if (across.front().at - top > 0.8 * textHeight)
        across.insert(across.begin(), outer(top, w));
    if (bottom - across.back().at > 0.8 * textHeight)
        across.push_back(outer(bottom, w));
    // A line across is the table's when a stretch of it reaches from one line
    // down to another: a link's underline and the edges of a coloured label
    // in a cell end inside it.
    const double reach = std::max(4.0, 0.25 * textHeight);
    std::erase_if(across, [&](const Rule& rule) {
        return std::ranges::none_of(rule.drawn, [&](const std::pair<int, int>& stretch) {
            return nearAny(down, stretch.first, reach) && nearAny(down, stretch.second, reach);
        });
    });
    const int rows = static_cast<int>(across.size()) - 1;
    const int columns = static_cast<int>(down.size()) - 1;
    if (rows < 2 || columns < 2 || rows * columns > 20000)
        return {};

    // The squares between the lines; two with no line drawn between them are one cell.
    std::vector<int> parent(static_cast<std::size_t>(rows) * columns);
    std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&](int i) {
        while (parent[static_cast<std::size_t>(i)] != i)
            i = parent[static_cast<std::size_t>(i)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(i)])];
        return i;
    };
    for (int r = 0; r < rows; ++r) {
        const double top = across[static_cast<std::size_t>(r)].at, bottom = across[static_cast<std::size_t>(r) + 1].at;
        for (int c = 0; c < columns; ++c) {
            const double from = down[static_cast<std::size_t>(c)].at, to = down[static_cast<std::size_t>(c) + 1].at;
            if (c + 1 < columns && drawnAlong(down[static_cast<std::size_t>(c) + 1], top, bottom) < 0.5 * (bottom - top))
                parent[static_cast<std::size_t>(root(r * columns + c + 1))] = root(r * columns + c);
            if (r + 1 < rows && drawnAlong(across[static_cast<std::size_t>(r) + 1], from, to) < 0.5 * (to - from))
                parent[static_cast<std::size_t>(root((r + 1) * columns + c))] = root(r * columns + c);
        }
    }
    struct Extent {
        int top = std::numeric_limits<int>::max(), left = std::numeric_limits<int>::max(), bottom = -1, right = -1, squares = 0;
    };
    std::vector<Extent> extents(parent.size());
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            Extent& e = extents[static_cast<std::size_t>(root(r * columns + c))];
            e.top = std::min(e.top, r);
            e.left = std::min(e.left, c);
            e.bottom = std::max(e.bottom, r);
            e.right = std::max(e.right, c);
            ++e.squares;
        }
    }
    Structure s;
    s.rows = rows;
    s.columns = columns;
    s.sure = 1;
    for (const Extent& e : extents) {
        if (e.squares == 0)
            continue;
        if (e.squares != (e.bottom - e.top + 1) * (e.right - e.left + 1))
            return {}; // not a rectangle: a line was missed, the ruling cannot be trusted
        s.cells.push_back({e.top, e.left, e.bottom - e.top + 1, e.right - e.left + 1,
            QRectF(QPointF(down[static_cast<std::size_t>(e.left)].at, across[static_cast<std::size_t>(e.top)].at),
                QPointF(down[static_cast<std::size_t>(e.right) + 1].at, across[static_cast<std::size_t>(e.bottom) + 1].at))});
    }
    std::ranges::sort(s.cells, [](const Cell& a, const Cell& b) { return a.row != b.row ? a.row < b.row : a.column < b.column; });
    return s;
}

OcrTable filled(const Structure& structure, const QList<OcrPiece>& pieces, int* tornLines)
{
    OcrTable out;
    if (structure.cells.empty() || structure.rows <= 0 || structure.columns <= 0)
        return out;
    const std::vector<Cell>& cells = structure.cells;
    std::vector<std::vector<Fragment>> inCell(cells.size());
    struct Placed {
        QRectF box;
        int firstRow = 0;
        int lastRow = 0; // of its cell
    };
    std::vector<Placed> placed;
    // Words above and below all the cells (a title, a note under the table)
    // are no cell's: they get rows of their own, in the first column.
    QRectF all;
    for (const Cell& cell : cells)
        all |= cell.box;
    std::vector<Fragment> above, below;
    for (const OcrPiece& piece : pieces) {
        const QString whole = piece.text.trimmed();
        const QRectF box = piece.quad.boundingRect();
        if (whole.isEmpty() || box.isEmpty())
            continue;
        // The cell it covers most of, else the nearest.
        std::size_t best = 0;
        double bestArea = 0;
        double nearest = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const QRectF both = box.intersected(cells[i].box);
            const double area = both.isEmpty() ? 0 : both.width() * both.height();
            if (area > bestArea) {
                bestArea = area;
                best = i;
            } else if (bestArea <= 0) {
                if (const double d = apart(box, cells[i].box); d < nearest) {
                    nearest = d;
                    best = i;
                }
            }
        }
        if (bestArea <= 0 && (box.center().y() < all.top() || box.center().y() > all.bottom())) {
            (box.center().y() < all.top() ? above : below).push_back({box, whole});
            continue;
        }
        placed.push_back({box, cells[best].row, cells[best].row + cells[best].rows - 1});
        // Across several cells side by side (at least a character's width
        // into each, and a good part of the line): cut between them.
        std::vector<std::size_t> across;
        if (bestArea > 0 && !piece.chars.isEmpty()) {
            for (std::size_t i = 0; i < cells.size(); ++i) {
                const QRectF both = box.intersected(cells[i].box);
                if (!both.isEmpty() && both.height() >= 0.3 * box.height() && both.width() >= box.height()
                    && both.width() >= 0.15 * box.width())
                    across.push_back(i);
            }
            std::ranges::sort(across, {}, [&](std::size_t i) { return cells[i].box.left(); });
            // Side by side only: cells one above the other are one cell's lines to this piece.
            for (std::size_t k = 1; k < across.size();) {
                if (cells[across[k]].box.left() < cells[across[k - 1]].box.center().x())
                    across.erase(across.begin() + static_cast<std::ptrdiff_t>(k));
                else
                    ++k;
            }
        }
        if (across.size() < 2) {
            inCell[best].push_back({box, whole});
            continue;
        }
        std::vector<double> cuts; // between one cell and the next
        for (std::size_t k = 1; k < across.size(); ++k)
            cuts.push_back((cells[across[k - 1]].box.right() + cells[across[k]].box.left()) / 2);
        const auto partAt = [&](double x) {
            return static_cast<std::size_t>(std::ranges::upper_bound(cuts, x) - cuts.begin());
        };
        std::vector<QString> texts(across.size());
        std::size_t part = 0;
        qsizetype next = 0; // into piece.chars
        for (qsizetype i = 0; i < piece.text.size(); ++i) {
            while (next < piece.chars.size() && piece.chars[next].first <= i) {
                if (piece.chars[next].first == i && piece.chars[next].second >= 0)
                    part = std::max(part, partAt(box.left() + piece.chars[next].second * box.width())); // never back to the left
                ++next;
            }
            texts[part] += piece.text[i];
        }
        for (std::size_t k = 0; k < across.size(); ++k) {
            const double left = k == 0 ? box.left() : cuts[k - 1];
            const double right = k + 1 == across.size() ? box.right() : cuts[k];
            if (const QString text = texts[k].trimmed(); !text.isEmpty())
                inCell[across[k]].push_back({QRectF(QPointF(left, box.top()), QPointF(right, box.bottom())), text});
        }
    }

    if (tornLines) {
        // Words side by side (overlapping by half their height) are a line of
        // writing: the rows their cells take must have one in common.
        *tornLines = 0;
        std::ranges::sort(placed, {}, [](const Placed& p) { return p.box.center().y(); });
        for (std::size_t i = 0; i < placed.size();) {
            int first = placed[i].firstRow, last = placed[i].lastRow;
            double top = placed[i].box.top(), bottom = placed[i].box.bottom();
            std::size_t next = i + 1;
            for (; next < placed.size(); ++next) {
                const QRectF& box = placed[next].box;
                if (std::min(bottom, box.bottom()) - std::max(top, box.top()) < 0.5 * std::min(bottom - top, box.height()))
                    break;
                first = std::max(first, placed[next].firstRow);
                last = std::min(last, placed[next].lastRow);
            }
            *tornLines += first > last ? 1 : 0;
            i = next;
        }
    }

    out.rows.reserve(structure.rows);
    for (int r = 0; r < structure.rows; ++r)
        out.rows.append(QStringList(structure.columns, QString()));
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const Cell& cell = cells[i];
        if (cell.row >= structure.rows || cell.column >= structure.columns)
            continue;
        QString& text = out.rows[cell.row][cell.column];
        const QString words = wordsOf(inCell[i]);
        text = text.isEmpty() ? words : words.isEmpty() ? text : text + u' ' + words;
        const int rows = std::min(cell.rows, structure.rows - cell.row);
        const int columns = std::min(cell.columns, structure.columns - cell.column);
        if (rows > 1 || columns > 1)
            out.spans.append({cell.row, cell.column, rows, columns});
    }

    // Empty rows and columns around the words are left out (the model now and
    // then begins with a row of nothing): what is pasted starts with a cell
    // that has something in it. Those between the words stay.
    const auto rowEmpty = [&](int r) { return std::ranges::all_of(out.rows[r], &QString::isEmpty); };
    const auto columnEmpty = [&](int c) { return std::ranges::all_of(out.rows, [c](const QStringList& row) { return row[c].isEmpty(); }); };
    int top = 0, bottom = structure.rows - 1, left = 0, right = structure.columns - 1;
    while (top <= bottom && rowEmpty(top))
        ++top;
    if (top > bottom)
        return {};
    while (rowEmpty(bottom))
        --bottom;
    while (columnEmpty(left))
        ++left;
    while (columnEmpty(right))
        --right;
    if (top > 0 || left > 0 || bottom < structure.rows - 1 || right < structure.columns - 1) {
        out.rows = out.rows.mid(top, bottom - top + 1);
        for (QStringList& row : out.rows)
            row = row.mid(left, right - left + 1);
        QList<OcrTable::Span> kept;
        for (OcrTable::Span s : std::as_const(out.spans)) {
            const int fromRow = std::max(s.row, top), toRow = std::min(s.row + s.rows - 1, bottom);
            const int fromColumn = std::max(s.column, left), toColumn = std::min(s.column + s.columns - 1, right);
            // Its words are in its first cell: a merged cell that began in what
            // was left out had none, and is no longer one.
            if (s.row < top || s.column < left || (toRow == fromRow && toColumn == fromColumn))
                continue;
            kept.append({fromRow - top, fromColumn - left, toRow - fromRow + 1, toColumn - fromColumn + 1});
        }
        out.spans = std::move(kept);
    }
    // The words above and below, a line of writing a row.
    const auto linesOf = [](std::vector<Fragment> fragments) {
        QStringList lines;
        std::ranges::sort(fragments, {}, [](const Fragment& f) { return f.box.center().y(); });
        for (std::size_t i = 0; i < fragments.size();) {
            std::vector<Fragment> line {fragments[i]};
            std::size_t next = i + 1;
            for (; next < fragments.size(); ++next) {
                const QRectF& a = line.front().box;
                const QRectF& b = fragments[next].box;
                if (std::min(a.bottom(), b.bottom()) - std::max(a.top(), b.top()) < 0.5 * std::min(a.height(), b.height()))
                    break;
                line.push_back(fragments[next]);
            }
            lines.append(wordsOf(std::move(line)));
            i = next;
        }
        return lines;
    };
    const QStringList before = linesOf(std::move(above));
    const QStringList after = linesOf(std::move(below));
    const qsizetype width = out.rows.first().size();
    const auto row = [width](const QString& text) {
        QStringList cells(width, QString());
        cells[0] = text;
        return cells;
    };
    for (qsizetype i = before.size() - 1; i >= 0; --i)
        out.rows.prepend(row(before[i]));
    for (OcrTable::Span& span : out.spans)
        span.row += static_cast<int>(before.size());
    for (const QString& text : after)
        out.rows.append(row(text));
    return out;
}

} // namespace table

} // namespace ws

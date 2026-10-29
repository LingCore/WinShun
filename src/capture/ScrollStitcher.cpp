#include "ScrollStitcher.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace ws::capture {

namespace {

constexpr int kBlockRows = 256; // the canvas's rows a block
constexpr int kSegments = 16; // segments a row
constexpr double kMinScore = 0.6;
constexpr int kMinVotes = 24;
constexpr double kLostScore = 0.5; // after two frames lost in a row: a little less asked
constexpr int kLostVotes = 16;
// A segment agrees when its pixels differ by at most this much luminance on
// average: text drawn again (WinUI) differs by up to 8 here and there.
constexpr int kTolerance = 3;
// Segments of one brightness and texture more often than this in a frame
// (a table's grid, a plain margin) propose no shift; they are still compared.
constexpr int kMaxBucket = 48;
constexpr int kCandidates = 8;
constexpr int kMinOverlap = 16; // rows two frames must share
constexpr double kTieScore = 0.015; // scores this close are a tie
// A shift that fits far better than any other is taken with less: where the
// frames overlap, a picture may have faded in or loaded (it changed in place).
constexpr double kClearScore = 0.35;
constexpr int kClearVotes = 160;
constexpr double kClearMargin = 2; // times the best other shift's score (3 px or more off)
// Nothing fits and the bars are not known yet: unchanged only if this much stayed.
constexpr double kInPlaceUnknown = 0.8;

inline int luma(uint32_t p)
{
    return int((((p >> 16) & 0xFF) * 77 + ((p >> 8) & 0xFF) * 150 + (p & 0xFF) * 29) >> 8);
}

// How much edge a segment has, as a small bucket: 0 is flat (nothing to
// compare by), then about the log2 of the mean step between neighbours.
inline uint8_t activityBucket(int sum, int pixels)
{
    if (pixels <= 1)
        return 0;
    const int perPixel4 = sum * 4 / (pixels - 1); // quarter levels a pixel
    if (perPixel4 < 4)
        return 0;
    uint8_t b = 1;
    for (int v = perPixel4 >> 3; v > 0 && b < 15; v >>= 1)
        ++b;
    return b;
}

} // namespace

// What a frame is compared by.
struct ScrollStitcher::Analysis {
    QImage image; // Format_RGB32 or a 32-bit format like it
    int w = 0;
    int h = 0;
    int width = 0; // compared (w less the ignored strip)
    std::array<int, kSegments + 1> seg {}; // segment k is x seg[k] .. seg[k + 1] - 1
    std::vector<uint8_t> lum; // width x h
    std::vector<uint8_t> mean; // h x kSegments
    std::vector<uint8_t> act; // h x kSegments, 0 = flat
    std::unordered_map<uint32_t, std::vector<int>> index; // (segment, brightness, texture) -> rows

    const uint8_t* lumRow(int y) const { return lum.data() + size_t(y) * size_t(width); }
    uint8_t meanAt(int y, int k) const { return mean[size_t(y) * kSegments + size_t(k)]; }
    uint8_t actAt(int y, int k) const { return act[size_t(y) * kSegments + size_t(k)]; }
    static uint32_t key(int k, int meanQ, int actB) { return uint32_t(k) << 16 | uint32_t(meanQ) << 8 | uint32_t(actB); }

    // Whether segment k of row ya (this frame) and row yb of `other` agree.
    bool agrees(int ya, const Analysis& other, int yb, int k) const
    {
        const uint8_t* a = lumRow(ya) + seg[size_t(k)];
        const uint8_t* b = other.lumRow(yb) + seg[size_t(k)];
        const int n = seg[size_t(k) + 1] - seg[size_t(k)];
        int sad = 0;
        for (int x = 0; x < n; ++x)
            sad += std::abs(int(a[x]) - int(b[x]));
        return sad <= kTolerance * n;
    }
};

struct ScrollStitcher::Match {
    bool unchanged = false;
    bool found = false;
    int dy = 0;
    double score = 0;
    int votes = 0;
    int top = 0; // this pair's fixed bands
    int bottom = 0;
    std::array<char, kSegments> fixed {}; // this pair's fixed segment columns
};

namespace {

std::unique_ptr<ScrollStitcher::Analysis> analyze(const QImage& frame, int ignoreRight)
{
    auto a = std::make_unique<ScrollStitcher::Analysis>();
    const QImage::Format f = frame.format();
    a->image = (f == QImage::Format_RGB32 || f == QImage::Format_ARGB32 || f == QImage::Format_ARGB32_Premultiplied)
        ? frame
        : frame.convertToFormat(QImage::Format_RGB32);
    a->w = frame.width();
    a->h = frame.height();
    a->width = std::clamp(a->w - std::max(0, ignoreRight), std::min(a->w, kSegments * 2), a->w);
    for (int k = 0; k <= kSegments; ++k)
        a->seg[size_t(k)] = k * a->width / kSegments;
    a->lum.resize(size_t(a->width) * size_t(a->h));
    a->mean.resize(size_t(a->h) * kSegments);
    a->act.resize(size_t(a->h) * kSegments);
    for (int y = 0; y < a->h; ++y) {
        const auto* src = reinterpret_cast<const uint32_t*>(a->image.constScanLine(y));
        uint8_t* l = a->lum.data() + size_t(y) * size_t(a->width);
        for (int x = 0; x < a->width; ++x)
            l[x] = uint8_t(luma(src[x]));
        for (int k = 0; k < kSegments; ++k) {
            const int x0 = a->seg[size_t(k)];
            const int x1 = a->seg[size_t(k) + 1];
            int sum = l[x0];
            int edges = 0;
            for (int x = x0 + 1; x < x1; ++x) {
                sum += l[x];
                edges += std::abs(int(l[x]) - int(l[x - 1]));
            }
            const int n = std::max(1, x1 - x0);
            const uint8_t m = uint8_t(sum / n);
            const uint8_t b = activityBucket(edges, n);
            a->mean[size_t(y) * kSegments + size_t(k)] = m;
            a->act[size_t(y) * kSegments + size_t(k)] = b;
            if (b > 0)
                a->index[ScrollStitcher::Analysis::key(k, m >> 2, b)].push_back(y);
        }
    }
    return a;
}

} // namespace

ScrollStitcher::ScrollStitcher(QSize frame, Limits limits, Options options)
    : m_size(frame)
    , m_limits(limits)
    , m_options(options)
{
}

ScrollStitcher::~ScrollStitcher()
{
    release();
}

void ScrollStitcher::release()
{
    for (uint32_t* block : m_blocks)
        if (block)
            ::VirtualFree(block, 0, MEM_RELEASE);
    m_blocks.clear();
    m_blocks.shrink_to_fit();
    m_firstBlock = 0;
    m_prev.reset();
    m_far = QImage();
    m_prevOffset = m_farOffset = m_farRows = m_farFrom = 0;
    m_nearOffset = 0;
    m_previewTop = false;
    m_top = m_bottom = 0;
    m_bandsKnown = false;
    m_velocity = m_lost = m_frames = 0;
    m_partial = m_full = false;
}

int ScrollStitcher::limitHeight() const
{
    const qint64 byPixels = m_size.width() > 0 ? m_limits.maxPixels / m_size.width() : 0;
    return int(std::max<qint64>(0, std::min<qint64>(m_limits.maxHeight, byPixels)));
}

uint32_t* ScrollStitcher::row(int y)
{
    const int number = y >= 0 ? y / kBlockRows : (y + 1) / kBlockRows - 1;
    if (m_blocks.empty())
        m_firstBlock = number;
    while (number < m_firstBlock) {
        m_blocks.push_front(nullptr);
        --m_firstBlock;
    }
    const size_t block = size_t(number - m_firstBlock);
    if (block >= m_blocks.size())
        m_blocks.resize(block + 1, nullptr);
    if (!m_blocks[block]) {
        const size_t bytes = size_t(m_size.width()) * kBlockRows * sizeof(uint32_t);
        m_blocks[block] = static_cast<uint32_t*>(::VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!m_blocks[block])
            return nullptr;
    }
    return m_blocks[block] + size_t(y - number * kBlockRows) * size_t(m_size.width());
}

const uint32_t* ScrollStitcher::row(int y) const
{
    const int number = y >= 0 ? y / kBlockRows : (y + 1) / kBlockRows - 1;
    if (number < m_firstBlock)
        return nullptr;
    const size_t block = size_t(number - m_firstBlock);
    if (block >= m_blocks.size() || !m_blocks[block])
        return nullptr;
    return m_blocks[block] + size_t(y - number * kBlockRows) * size_t(m_size.width());
}

bool ScrollStitcher::write(const QImage& frame, int offset, int from, int to)
{
    const int limit = m_nearOffset + limitHeight();
    bool ok = true;
    if (offset + from < m_nearOffset) {
        from = m_nearOffset - offset;
        ok = false;
    }
    if (offset + to > limit) {
        to = limit - offset;
        ok = false;
    }
    const size_t bytes = size_t(m_size.width()) * sizeof(uint32_t);
    for (int y = std::max(0, from); y < to; ++y) {
        uint32_t* dst = row(offset + y);
        if (!dst)
            return false; // out of memory: as at the limit
        std::memcpy(dst, frame.constScanLine(y), bytes);
    }
    return ok;
}

ScrollStitcher::Match ScrollStitcher::match(const Analysis& a, const Analysis& b, std::optional<int> expectDy)
{
    Match m;
    const int h = b.h;
    // 1. What is where it was: per segment, then per row.
    std::vector<char> same(size_t(h) * kSegments);
    std::vector<char> rowSame(static_cast<size_t>(h));
    int rowsSame = 0;
    for (int y = 0; y < h; ++y) {
        bool all = true;
        for (int k = 0; k < kSegments; ++k) {
            const bool s = b.agrees(y, a, y, k);
            same[size_t(y) * kSegments + size_t(k)] = s;
            all = all && s;
        }
        rowSame[size_t(y)] = all;
        rowsSame += all;
    }
    if (rowsSame >= h - h / 100) {
        m.unchanged = true;
        return m;
    }
    // When no shift fits: nothing moved if most of what scrolls is where it
    // was, and what changed changed in place (a picture that loaded, a
    // fade). Bars and side bars stay when the page scrolls too: once the
    // bands are known only the rows between them count, and before that
    // more must have stayed.
    const auto notFound = [&] {
        const int from = m_bandsKnown ? m_top : 0;
        const int to = m_bandsKnown ? h - m_bottom : h;
        int informative = 0;
        int stayed = 0;
        for (int y = from; y < to; ++y) {
            for (int k = 0; k < kSegments; ++k) {
                if (m.fixed[size_t(k)] || (b.actAt(y, k) == 0 && a.actAt(y, k) == 0))
                    continue;
                ++informative;
                stayed += same[size_t(y) * kSegments + size_t(k)];
            }
        }
        const double need = m_bandsKnown ? kMinScore : kInPlaceUnknown;
        m.unchanged = informative >= kMinVotes && stayed >= need * informative;
        return m;
    };
    // 2. This pair's fixed bands: the rows at either end that stayed.
    int top = 0;
    while (top < h && rowSame[size_t(top)])
        ++top;
    int bottom = 0;
    while (bottom < h - top && rowSame[size_t(h - 1 - bottom)])
        ++bottom;
    if (h - top - bottom < h / 4)
        top = bottom = 0; // most of it stayed: a small change, not bars
    const int zoneFrom = top;
    const int zoneTo = h - bottom;
    const int zone = zoneTo - zoneFrom;
    // 3. Segment columns that stayed while the rest changed (side bars).
    // More than half of them staying is a page that repeats itself (rows
    // alike, moved by a multiple of their height), not side bars.
    int fixedCount = 0;
    for (int k = 0; k < kSegments; ++k) {
        int rows = 0;
        int kept = 0;
        for (int y = zoneFrom; y < zoneTo; ++y) {
            if (b.actAt(y, k) == 0)
                continue;
            ++rows;
            kept += same[size_t(y) * kSegments + size_t(k)];
        }
        m.fixed[size_t(k)] = rows >= 16 && kept * 10 >= rows * 9;
        fixedCount += m.fixed[size_t(k)];
    }
    if (fixedCount > kSegments / 2)
        m.fixed.fill(0);
    m.top = top;
    m.bottom = bottom;
    // 4. Candidate shifts: the rows of `a` with a segment like each of `b`'s.
    const int range = zone - kMinOverlap;
    if (range <= 0)
        return notFound();
    std::vector<int> votes(size_t(2 * range + 1), 0);
    for (int y = zoneFrom; y < zoneTo; ++y) {
        for (int k = 0; k < kSegments; ++k) {
            const uint8_t act = b.actAt(y, k);
            if (act == 0 || m.fixed[size_t(k)])
                continue;
            const int q = b.meanAt(y, k) >> 2;
            for (int dq = -1; dq <= 1; ++dq) {
                if (q + dq < 0 || q + dq > 63)
                    continue;
                const auto it = a.index.find(Analysis::key(k, q + dq, act));
                if (it == a.index.end() || int(it->second.size()) > kMaxBucket)
                    continue;
                for (int ya : it->second) {
                    const int dy = ya - y;
                    if (dy == 0 || dy < -range || dy > range || ya < zoneFrom || ya >= zoneTo)
                        continue;
                    ++votes[size_t(dy + range)];
                }
            }
        }
    }
    std::vector<int> candidates;
    {
        // The most votes, and the most for the rows they share: where rows look alike (a
        // file list), every multiple of a row's height gets votes from what all rows have,
        // the more the more rows overlap; the true shift gets them from all of fewer rows
        // (live test 2026-10-22: Explorer moved 774 of 1106 px, never a candidate).
        std::vector<std::pair<double, int>> ranked; // (votes, dy)
        std::vector<std::pair<double, int>> dense; // (votes a shared row, dy)
        for (int i = 0; i < int(votes.size()); ++i) {
            if (votes[size_t(i)] == 0)
                continue;
            const int dy = i - range;
            ranked.push_back({double(votes[size_t(i)]), dy});
            // Only shifts that share a fifth: in a few rows, what every row has
            // makes any shift look dense (live test 2026-10-22: Edge "moved back"
            // 1131 of 1151 px, sharing 20 rows, and the picture lost 6880 px).
            if (const int shared = zone - std::abs(dy); shared * 5 >= zone)
                dense.push_back({double(votes[size_t(i)]) / shared, dy});
        }
        for (auto* list : {&ranked, &dense}) {
            const size_t n = std::min<size_t>(kCandidates, list->size());
            std::partial_sort(list->begin(), list->begin() + ptrdiff_t(n), list->end(), [](const auto& l, const auto& r) { return l.first > r.first; });
            for (size_t i = 0; i < n; ++i)
                candidates.push_back((*list)[i].second);
        }
    }
    candidates.push_back(0);
    if (m_velocity != 0 && std::abs(m_velocity) <= range)
        candidates.push_back(m_velocity);
    if (expectDy && std::abs(*expectDy) <= range)
        candidates.push_back(*expectDy);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    // 5. Each candidate checked segment by segment over the rows both share.
    const double minScore = m_lost >= 2 ? kLostScore : kMinScore;
    const int minVotes = m_lost >= 2 ? kLostVotes : kMinVotes;
    struct Checked {
        int dy;
        double score;
        int votes;
    };
    std::vector<Checked> valid;
    std::vector<Checked> checked;
    for (int dy : candidates) {
        int compared = 0;
        int agreed = 0;
        for (int y = std::max(zoneFrom, zoneFrom - dy); y < std::min(zoneTo, zoneTo - dy); ++y) {
            const int ya = y + dy;
            for (int k = 0; k < kSegments; ++k) {
                if (m.fixed[size_t(k)] || (b.actAt(y, k) == 0 && a.actAt(ya, k) == 0))
                    continue;
                ++compared;
                agreed += b.agrees(y, a, ya, k);
            }
        }
        if (compared == 0)
            continue;
        const double score = double(agreed) / compared;
        checked.push_back({dy, score, agreed});
        if (score >= minScore && agreed >= minVotes)
            valid.push_back({dy, score, agreed});
    }
    if (valid.empty() && !checked.empty()) {
        const auto leader = std::max_element(checked.begin(), checked.end(), [](const Checked& l, const Checked& r) { return l.score < r.score; });
        double other = 0;
        for (const Checked& c : checked)
            if (std::abs(c.dy - leader->dy) >= 3)
                other = std::max(other, c.score);
        if (leader->dy != 0 && leader->score >= kClearScore && leader->votes >= kClearVotes && leader->score >= kClearMargin * other)
            valid.push_back(*leader);
    }
    if (valid.empty())
        return notFound();
    // 6. The best score; a tie goes to the shift nearest the expected one,
    // else the last one (a repeating page fits several), else to the most
    // agreeing segments.
    const int expect = expectDy && *expectDy != 0 ? *expectDy : m_velocity;
    double best = 0;
    for (const Checked& c : valid)
        best = std::max(best, c.score);
    const Checked* pick = nullptr;
    for (const Checked& c : valid) {
        if (c.score < best - kTieScore)
            continue;
        if (!pick) {
            pick = &c;
            continue;
        }
        const bool nearer = expect != 0 && std::abs(c.dy - expect) < std::abs(pick->dy - expect);
        const bool sameDistance = expect == 0 || std::abs(c.dy - expect) == std::abs(pick->dy - expect);
        if (nearer || (sameDistance && c.votes > pick->votes))
            pick = &c;
    }
    if (pick->dy == 0) {
        m.unchanged = true;
        return m;
    }
    m.found = true;
    m.dy = pick->dy;
    m.score = pick->score;
    m.votes = pick->votes;
    return m;
}

// Whether `gone` rows at the end of `end` (the frame the result ends with)
// were the page pulled past its end, now that `now` shows it moved back
// down by them: plain wherever they differ from what `now` has in their
// place (what stayed, a floating button or a side bar, is left out).
bool ScrollStitcher::bounced(const Analysis& end, const Analysis& now, int gone, bool top) const
{
    const int h = end.h;
    if (gone <= 0 || gone > (h - m_top - m_bottom) / 2)
        return false;
    const int first = top ? m_top : h - m_bottom - gone;
    for (int y = first; y < first + gone; ++y)
        for (int k = 0; k < kSegments; ++k)
            if (end.actAt(y, k) != 0 && !now.agrees(y, end, y, k))
                return false;
    return true;
}

ScrollStitcher::Step ScrollStitcher::add(const QImage& frame, std::optional<int> expectDy, bool guess)
{
    Step step;
    const auto finish = [&](State state) {
        step.state = state;
        step.top = m_top;
        step.bottom = m_bottom;
        step.height = height();
        return step;
    };
    if (frame.size() != m_size || frame.depth() != 32)
        return finish(State::Lost);
    if (m_full)
        return finish(State::Full);
    ++m_frames;
    auto b = analyze(frame, m_options.ignoreRight);
    const int h = m_size.height();
    if (!m_prev) {
        // The first frame is the start of everything, all of it.
        const bool ok = write(b->image, 0, 0, h);
        m_far = b->image;
        m_farOffset = 0;
        m_farFrom = 0;
        m_farRows = ok ? h : std::max(0, limitHeight());
        m_full = !ok;
        m_prev = std::move(b);
        m_prevOffset = 0;
        return finish(m_full ? State::Full : State::Appended);
    }
    Match m = match(*m_prev, *b, expectDy);
    const bool atEnd = m_prevOffset == m_farOffset; // the frame before is the one the result ends with
    if (m.unchanged) {
        if (atEnd) {
            // The end's newest look: a picture that loaded, a fade that ended.
            write(b->image, m_farOffset, m_farFrom, h - m_bottom);
            m_far = b->image;
        } else if (m_prevOffset == m_nearOffset) {
            const int to = h - m_bottom - (h - m_top - m_bottom) / 6;
            write(b->image, m_nearOffset, 0, to);
            m_farFrom = std::max(m_farFrom, std::min(h - m_bottom, m_nearOffset + to - m_farOffset));
        }
        m_prev = std::move(b);
        m_lost = 0;
        return finish(State::Unchanged);
    }
    if (!m.found) {
        if (!guess || !expectDy || *expectDy == 0) {
            ++m_lost;
            return finish(State::Lost);
        }
        m.dy = *expectDy;
        step.predicted = true;
        m_partial = true;
    }
    m_lost = 0;
    step.dy = m.dy;
    step.score = m.score;
    step.votes = m.votes;
    m_velocity = m.dy;
    if (!m_bandsKnown) {
        m_top = m.top;
        m_bottom = m.bottom;
        m_bandsKnown = true;
    } else if (!step.predicted) {
        m_top = std::min(m_top, m.top);
        m_bottom = std::min(m_bottom, m.bottom);
    }
    const int offset = m_prevOffset + m.dy;
    if (offset < m_nearOffset) {
        const int begin = m_nearOffset + m_top;
        const int safeTo = h - m_bottom - (h - m_top - m_bottom) / 6;
        const int to = std::clamp(begin - offset, safeTo, h - m_bottom);
        m_nearOffset = std::max(offset, m_farOffset + m_farRows - limitHeight());
        const bool ok = write(b->image, offset, 0, to);
        // The new content can replace the old top frame's header in the
        // overlap. Keep only the untouched suffix of the bottommost frame.
        m_farFrom = std::max(m_farFrom, std::min(h - m_bottom, offset + to - m_farOffset));
        m_prev = std::move(b);
        m_prevOffset = offset;
        m_previewTop = true;
        if (!ok || height() >= limitHeight()) {
            m_full = true;
            return finish(State::Full);
        }
        return finish(State::Appended);
    }
    if (offset <= m_farOffset) {
        // Behind: nothing is added. Right after the end, moving back down
        // by rows that were the page pulled past its end (a bounce) makes
        // this frame the end.
        if (atEnd && !step.predicted && bounced(*m_prev, *b, m_farOffset - offset)) {
            m_far = b->image;
            m_farOffset = offset;
            m_farFrom = std::max(m_top, h / 6);
            m_farRows = std::min(h, m_nearOffset + limitHeight() - offset);
        } else if (m_prevOffset == m_nearOffset && !step.predicted && bounced(*m_prev, *b, offset - m_nearOffset, true)) {
            // The symmetric bounce at the top must remove its blank rows
            // and restore the header from the frame now at rest.
            m_nearOffset = offset;
            const int to = h - m_bottom - (h - m_top - m_bottom) / 6;
            write(b->image, offset, 0, to);
            m_farFrom = std::max(m_farFrom, std::min(h - m_bottom, offset + to - m_farOffset));
        }
        m_prev = std::move(b);
        m_prevOffset = offset;
        return finish(State::Behind);
    }
    m_prev = std::move(b);
    m_prevOffset = offset;
    // Ahead of the end: this frame's rows go on the canvas, from below the
    // top band (and the top sixth, where a bar that sticks only once the
    // page scrolls would be), or from where the canvas ends when the step
    // was longer than that leaves.
    const int canvasEnd = m_farOffset + h - m_bottom;
    const int from = std::clamp(canvasEnd - offset, m_top, std::max(m_top, h / 6));
    const bool ok = write(m_prev->image, offset, from, h - m_bottom);
    m_far = m_prev->image;
    m_farOffset = offset;
    m_farFrom = from;
    m_farRows = std::min(h, m_nearOffset + limitHeight() - offset);
    m_previewTop = false;
    if (!ok || height() >= limitHeight()) {
        m_full = true;
        return finish(State::Full);
    }
    return finish(State::Appended);
}

int ScrollStitcher::height() const
{
    if (m_far.isNull())
        return 0;
    return m_farOffset + std::max(0, m_farRows) - m_nearOffset;
}

QImage ScrollStitcher::result() const
{
    const int total = height();
    if (total <= 0)
        return {};
    QImage out(m_size.width(), total, QImage::Format_RGB32);
    if (out.isNull())
        return {};
    const size_t bytes = size_t(m_size.width()) * sizeof(uint32_t);
    const int canvasRows = std::min(total, m_farOffset + m_farFrom - m_nearOffset);
    for (int y = 0; y < canvasRows; ++y) {
        const uint32_t* src = row(m_nearOffset + y);
        if (src)
            std::memcpy(out.scanLine(y), src, bytes);
        else
            std::memset(out.scanLine(y), 0, bytes);
    }
    for (int y = canvasRows; y < total; ++y)
        std::memcpy(out.scanLine(y), m_far.constScanLine(m_nearOffset + y - m_farOffset), bytes);
    return out;
}

QImage ScrollStitcher::preview(QSize box) const
{
    const int total = height();
    if (total <= 0 || box.isEmpty())
        return {};
    const double scale = std::min(1.0, double(box.width()) / m_size.width());
    const int rows = std::min(total, int(box.height() / scale) + 1);
    QImage part(m_size.width(), rows, QImage::Format_RGB32);
    const size_t bytes = size_t(m_size.width()) * sizeof(uint32_t);
    const int first = m_nearOffset + (m_previewTop ? 0 : total - rows);
    const int canvasRows = m_farOffset + m_farFrom;
    for (int i = 0; i < rows; ++i) {
        const int y = first + i;
        const uint32_t* src = y < canvasRows ? row(y) : reinterpret_cast<const uint32_t*>(m_far.constScanLine(y - m_farOffset));
        if (src)
            std::memcpy(part.scanLine(i), src, bytes);
        else
            std::memset(part.scanLine(i), 0, bytes);
    }
    const int w = std::max(1, int(m_size.width() * scale + 0.5));
    const int scaledRows = std::max(1, int(rows * scale + 0.5));
    const int hh = std::min(box.height(), scaledRows);
    return part.scaled(w, scaledRows, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).copy(0, m_previewTop ? 0 : scaledRows - hh, w, hh);
}

QString stateName(ScrollStitcher::State state)
{
    switch (state) {
    case ScrollStitcher::State::Waiting:
        return QStringLiteral("waiting");
    case ScrollStitcher::State::Appended:
        return QStringLiteral("appended");
    case ScrollStitcher::State::Unchanged:
        return QStringLiteral("unchanged");
    case ScrollStitcher::State::Behind:
        return QStringLiteral("behind");
    case ScrollStitcher::State::Lost:
        return QStringLiteral("lost");
    case ScrollStitcher::State::Full:
        return QStringLiteral("full");
    }
    return {};
}

} // namespace ws::capture

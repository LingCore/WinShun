#include "ScrollStitcher.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <span>

namespace ws::capture {

namespace {

constexpr int kBlockRows = 256; // the canvas's rows a block
constexpr int kSegments = 16; // segments a row
constexpr int kMeanBins = 64; // mean luminance quantized by >> 2
constexpr int kActBins = 16; // 0 = flat, 1..15 = edge activity buckets
constexpr int kBuckets = kSegments * kMeanBins * kActBins; // 16,384 flat CSR buckets
constexpr double kMinScore = 0.6;
constexpr int kMinVotes = 24;
constexpr double kLostScore = 0.5; // after two frames lost in a row: a little less asked
constexpr int kLostVotes = 16;
// A segment agrees when its pixels differ by at most this much luminance on
// average: text drawn again (WinUI) differs by up to 8 here and there.
constexpr int kTolerance = 3;
// Subpixel smooth-scroll fallback requires stronger consensus and separation
// from competing shifts so unrelated frames still report Lost.
constexpr double kSubpixelScore = 0.72;
constexpr int kSubpixelVotes = 36;
constexpr double kSubpixelMargin = 1.5;
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
// Sparse dark-mode glyphs or thin icons in a wide segment can have a low
// mean gradient yet a sharp local edge; promote those to bucket 1.
inline uint8_t activityBucket(int sum, int maxEdge, int pixels)
{
    if (pixels <= 1)
        return 0;
    const int perPixel4 = sum * 4 / (pixels - 1); // quarter levels a pixel
    if (perPixel4 < 4)
        return (maxEdge >= 12 && sum >= 16) ? 1 : 0;
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
    // Flat CSR index: (segment, brightness, texture) -> contiguous slice of rows.
    std::array<uint32_t, kBuckets + 1> bucketOffset {};
    std::vector<int> bucketRows;

    const uint8_t* lumRow(int y) const { return lum.data() + size_t(y) * size_t(width); }
    uint8_t meanAt(int y, int k) const { return mean[size_t(y) * kSegments + size_t(k)]; }
    uint8_t actAt(int y, int k) const { return act[size_t(y) * kSegments + size_t(k)]; }
    static constexpr uint32_t bucketId(int k, int meanQ, int actB)
    {
        return (uint32_t(k) << 10) | (uint32_t(meanQ) << 4) | uint32_t(actB);
    }

    std::span<const int> rowsIn(int k, int meanQ, int actB) const
    {
        const uint32_t id = bucketId(k, meanQ, actB);
        const uint32_t begin = bucketOffset[id];
        const uint32_t end = bucketOffset[id + 1];
        return {bucketRows.data() + begin, end - begin};
    }

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

    // Symmetric Birchfield-Tomasi vertical-envelope agreement: allows pixels
    // produced by fractional-pixel smooth scrolling in either direction
    // (crisp->blurred, blurred->crisp, or fractional->fractional).
    bool agreesSubpixel(int ya, const Analysis& other, int yb, int k) const
    {
        const int x0 = seg[size_t(k)];
        const int n = seg[size_t(k) + 1] - x0;
        const uint8_t* a0 = lumRow(std::max(0, ya - 1)) + x0;
        const uint8_t* a1 = lumRow(ya) + x0;
        const uint8_t* a2 = lumRow(std::min(h - 1, ya + 1)) + x0;
        const uint8_t* b0 = other.lumRow(std::max(0, yb - 1)) + x0;
        const uint8_t* b1 = other.lumRow(yb) + x0;
        const uint8_t* b2 = other.lumRow(std::min(other.h - 1, yb + 1)) + x0;
        int dist = 0;
        for (int x = 0; x < n; ++x) {
            const int va = a1[x];
            const int vb = b1[x];
            const int blo = std::min({int(b0[x]), vb, int(b2[x])});
            const int bhi = std::max({int(b0[x]), vb, int(b2[x])});
            const int da = va < blo ? blo - va : (va > bhi ? va - bhi : 0);
            const int alo = std::min({int(a0[x]), va, int(a2[x])});
            const int ahi = std::max({int(a0[x]), va, int(a2[x])});
            const int db = vb < alo ? alo - vb : (vb > ahi ? vb - ahi : 0);
            dist += std::min(da, db);
        }
        return dist <= kTolerance * n;
    }

    int segmentSad(int ya, const Analysis& other, int yb, int k) const
    {
        const uint8_t* a = lumRow(ya) + seg[size_t(k)];
        const uint8_t* b = other.lumRow(yb) + seg[size_t(k)];
        const int n = seg[size_t(k) + 1] - seg[size_t(k)];
        int sad = 0;
        for (int x = 0; x < n; ++x)
            sad += std::abs(int(a[x]) - int(b[x]));
        return sad;
    }
};

struct ScrollStitcher::Match {
    bool unchanged = false;
    bool found = false;
    bool bandsMeasured = false;
    int dy = 0;
    double score = 0;
    int votes = 0;
    int top = 0; // this pair's fixed bands
    int bottom = 0;
    std::array<char, kSegments> fixed {}; // this pair's fixed segment columns
};

namespace {

bool hasBarContent(const ScrollStitcher::Analysis& a, int y0, int y1)
{
    std::array<bool, kSegments> cols {};
    int distinctCols = 0;
    for (int y = std::max(0, y0); y < std::min(a.h, y1); ++y) {
        for (int k = 0; k < kSegments; ++k) {
            if (!cols[size_t(k)] && a.actAt(y, k) > 0) {
                cols[size_t(k)] = true;
                if (++distinctCols >= 3)
                    return true;
            }
        }
    }
    return false;
}

std::shared_ptr<ScrollStitcher::Analysis> analyze(const QImage& frame, int ignoreRight)
{
    auto a = std::make_shared<ScrollStitcher::Analysis>();
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

    std::array<uint32_t, kBuckets> counts {};
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
            int maxEdge = 0;
            for (int x = x0 + 1; x < x1; ++x) {
                sum += l[x];
                const int d = std::abs(int(l[x]) - int(l[x - 1]));
                edges += d;
                if (d > maxEdge)
                    maxEdge = d;
            }
            const int n = std::max(1, x1 - x0);
            const uint8_t m = uint8_t(sum / n);
            const uint8_t b = activityBucket(edges, maxEdge, n);
            a->mean[size_t(y) * kSegments + size_t(k)] = m;
            a->act[size_t(y) * kSegments + size_t(k)] = b;
            if (b > 0)
                ++counts[ScrollStitcher::Analysis::bucketId(k, m >> 2, b)];
        }
    }

    uint32_t total = 0;
    for (int id = 0; id < kBuckets; ++id) {
        const uint32_t c = counts[size_t(id)];
        a->bucketOffset[size_t(id)] = total;
        counts[size_t(id)] = total; // reuse as write cursor
        total += c;
    }
    a->bucketOffset[kBuckets] = total;
    a->bucketRows.resize(total);

    for (int y = 0; y < a->h; ++y) {
        for (int k = 0; k < kSegments; ++k) {
            const uint8_t b = a->actAt(y, k);
            if (b > 0) {
                const uint32_t id = ScrollStitcher::Analysis::bucketId(k, a->meanAt(y, k) >> 2, b);
                a->bucketRows[counts[id]++] = y;
            }
        }
    }
    return a;
}

} // namespace

QImage transposed(const QImage& image)
{
    const QImage in = image.depth() == 32 ? image : image.convertToFormat(QImage::Format_RGB32);
    QImage out(in.height(), in.width(), in.format());
    if (out.isNull())
        return out;
    const int w = in.width();
    const int h = in.height();
    uchar* bits = out.bits();
    const qsizetype stride = out.bytesPerLine();
    constexpr int kTile = 64; // a square at a time: both sides stay in the cache
    for (int y0 = 0; y0 < h; y0 += kTile) {
        for (int x0 = 0; x0 < w; x0 += kTile) {
            const int y1 = std::min(h, y0 + kTile);
            const int x1 = std::min(w, x0 + kTile);
            for (int y = y0; y < y1; ++y) {
                const auto* src = reinterpret_cast<const uint32_t*>(in.constScanLine(y));
                for (int x = x0; x < x1; ++x)
                    reinterpret_cast<uint32_t*>(bits + x * stride)[y] = src[x];
            }
        }
    }
    return out;
}

ScrollStitcher::ScrollStitcher(QSize frame, Limits limits, Options options)
    : m_size(options.horizontal ? frame.transposed() : frame)
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
    m_sigs.clear();
    m_sigs.shrink_to_fit();
    m_sigStart = 0;
    m_anchor.reset();
    m_anchorOffset = 0;
    m_prev.reset();
    m_nearAnalysis.reset();
    m_farAnalysis.reset();
    m_far = QImage();
    m_prevOffset = m_farOffset = m_farRows = m_farFrom = 0;
    m_nearOffset = 0;
    m_previewTop = false;
    m_top = m_bottom = 0;
    m_topCandidate = m_topStable = m_topTravel = 0;
    m_bottomCandidate = m_bottomStable = m_bottomTravel = 0;
    m_bandsKnown = false;
    m_fixedCols.fill(0);
    m_fixedColsKnown = false;
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

const uint32_t* ScrollStitcher::canvasRow(int y) const
{
    if (m_far.isNull() || y < m_nearOffset || y >= m_farOffset + m_farRows)
        return nullptr;
    if (y < m_farOffset + m_farFrom)
        return row(y);
    const int fy = y - m_farOffset;
    if (fy < 0 || fy >= m_far.height())
        return nullptr;
    return reinterpret_cast<const uint32_t*>(m_far.constScanLine(fy));
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

void ScrollStitcher::writeSigs(const Analysis& frame, int offset, int from, int to)
{
    const int limit = m_nearOffset + limitHeight();
    from = std::max(0, from);
    to = std::min(frame.h, to);
    if (offset + from < m_nearOffset)
        from = m_nearOffset - offset;
    if (offset + to > limit)
        to = limit - offset;
    if (from >= to)
        return;
    const int first = offset + from;
    const int last = offset + to; // exclusive
    if (m_sigs.empty()) {
        m_sigStart = first;
        m_sigs.resize(size_t(last - first));
    } else {
        if (first < m_sigStart) {
            m_sigs.insert(m_sigs.begin(), size_t(m_sigStart - first), RowSig {});
            m_sigStart = first;
        }
        if (last > m_sigStart + int(m_sigs.size()))
            m_sigs.resize(size_t(last - m_sigStart));
    }
    for (int y = from; y < to; ++y) {
        RowSig& dst = m_sigs[size_t(offset + y - m_sigStart)];
        std::memcpy(dst.mean.data(), frame.mean.data() + size_t(y) * kSegments, kSegments);
        std::memcpy(dst.act.data(), frame.act.data() + size_t(y) * kSegments, kSegments);
    }
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
    // 2. This pair's fixed bands: the rows at either end that stayed.
    int top = 0;
    while (top < h && rowSame[size_t(top)])
        ++top;
    int bottom = 0;
    while (bottom < h - top && rowSame[size_t(h - 1 - bottom)])
        ++bottom;
    const int rawTop = top;
    const int rawBottom = bottom;
    if (h - top - bottom < h / 4)
        top = bottom = 0; // most of it stayed: a small change, not bars
    const int zoneFrom = top;
    const int zoneTo = h - bottom;
    const int zone = zoneTo - zoneFrom;
    // 3. Segment columns that stayed while the rest changed (side bars).
    // Only edge-contiguous columns (left edge / right edge, skipping empty
    // columns) count as side bars so interior static content around an
    // animating GIF/video is never discarded. More than half staying is a
    // repeating page, not side bars.
    std::array<char, kSegments> colFixed {};
    std::array<char, kSegments> colEmpty {};
    for (int k = 0; k < kSegments; ++k) {
        int rows = 0;
        int kept = 0;
        for (int y = zoneFrom; y < zoneTo; ++y) {
            if (b.actAt(y, k) == 0)
                continue;
            ++rows;
            kept += same[size_t(y) * kSegments + size_t(k)];
        }
        colEmpty[size_t(k)] = rows < 16;
        colFixed[size_t(k)] = rows >= 16 && kept * 10 >= rows * 9;
    }
    int fixedCount = 0;
    for (int k = 0; k < kSegments && (colFixed[size_t(k)] || colEmpty[size_t(k)]); ++k) {
        if (colFixed[size_t(k)] && !m.fixed[size_t(k)]) {
            m.fixed[size_t(k)] = 1;
            ++fixedCount;
        }
    }
    for (int k = kSegments - 1; k >= 0 && (colFixed[size_t(k)] || colEmpty[size_t(k)]); --k) {
        if (colFixed[size_t(k)] && !m.fixed[size_t(k)]) {
            m.fixed[size_t(k)] = 1;
            ++fixedCount;
        }
    }
    if (fixedCount > kSegments / 2)
        m.fixed.fill(0);
    m.top = top;
    m.bottom = bottom;
    m.bandsMeasured = true;

    // When no shift fits: nothing moved if scrolling content stayed where it
    // was and only an in-place region (animated GIF/sticker, video, hover card,
    // loading picture) changed. Row-level consensus prevents a single dense
    // animating element from outvoting static text lines around it, while still
    // reporting Lost when the page jumped past overlap behind sticky/side bars.
    const auto notFound = [&] {
        if (m_bandsKnown) {
            const int from = m_top;
            const int to = h - m_bottom;
            const auto& fixedCols = m_fixedColsKnown ? m_fixedCols : m.fixed;
            int informative = 0;
            int stayed = 0;
            int infoRows = 0;
            int stayedRows = 0;
            for (int y = from; y < to; ++y) {
                int rInfo = 0;
                int rStayed = 0;
                for (int k = 0; k < kSegments; ++k) {
                    if (fixedCols[size_t(k)] || (b.actAt(y, k) == 0 && a.actAt(y, k) == 0))
                        continue;
                    ++rInfo;
                    rStayed += same[size_t(y) * kSegments + size_t(k)];
                }
                if (rInfo > 0) {
                    informative += rInfo;
                    stayed += rStayed;
                    ++infoRows;
                    if (rStayed * 4 >= rInfo * 3)
                        ++stayedRows;
                }
            }
            m.unchanged = (informative >= kMinVotes && stayed >= kMinScore * informative)
                || (stayed >= 64 && stayedRows >= 24 && (stayed >= 0.20 * informative || stayedRows * 4 >= infoRows));
            return m;
        }

        int informative = 0;
        int stayed = 0;
        for (int y = 0; y < h; ++y) {
            for (int k = 0; k < kSegments; ++k) {
                if (m.fixed[size_t(k)] || (b.actAt(y, k) == 0 && a.actAt(y, k) == 0))
                    continue;
                ++informative;
                stayed += same[size_t(y) * kSegments + size_t(k)];
            }
        }
        if (informative >= kMinVotes && stayed >= kInPlaceUnknown * informative) {
            m.unchanged = true;
            return m;
        }
        // Before fixed bands are known, rows beyond a plausible thin header/footer
        // (h/4 at each end) that stayed identical prove the page did not scroll.
        const int capTop = std::min(rawTop, h / 4);
        const int capBottom = std::min(rawBottom, h / 4);
        int intInfo = 0;
        int intStayed = 0;
        int intInfoRows = 0;
        int intStayedRows = 0;
        for (int y = capTop; y < h - capBottom; ++y) {
            const bool fullRowBand = y < rawTop || y >= h - rawBottom;
            int rInfo = 0;
            int rStayed = 0;
            for (int k = 0; k < kSegments; ++k) {
                if ((!fullRowBand && m.fixed[size_t(k)]) || (b.actAt(y, k) == 0 && a.actAt(y, k) == 0))
                    continue;
                ++rInfo;
                rStayed += same[size_t(y) * kSegments + size_t(k)];
            }
            if (rInfo > 0) {
                intInfo += rInfo;
                intStayed += rStayed;
                ++intInfoRows;
                if (rStayed * 4 >= rInfo * 3)
                    ++intStayedRows;
            }
        }
        if (intStayed >= 64 && intStayedRows >= 24
            && (intStayed >= 0.25 * intInfo || intStayedRows * 3 >= intInfoRows)) {
            m.unchanged = true;
            return m;
        }
        if (rawTop + rawBottom >= h / 2) {
            int barInfo = 0;
            int barRows = 0;
            for (int y = 0; y < h; ++y) {
                if (y >= rawTop && y < h - rawBottom)
                    continue;
                int rInfo = 0;
                for (int k = 0; k < kSegments; ++k)
                    if (b.actAt(y, k) != 0 || a.actAt(y, k) != 0)
                        ++rInfo;
                if (rInfo > 0) {
                    barInfo += rInfo;
                    ++barRows;
                }
            }
            if (barInfo >= 64 && barRows >= 24) {
                m.unchanged = true;
                return m;
            }
        }
        return m;
    };
    // 4. Candidate shifts: the rows of `a` with a segment like each of `b`'s.
    const int range = zone - kMinOverlap;
    if (range <= 0)
        return notFound();
    std::vector<int> votes(size_t(2 * range + 1), 0);
    const auto voteBucket = [&](int k, int meanQ, int actB, int y) {
        const std::span<const int> rows = a.rowsIn(k, meanQ, actB);
        if (rows.empty() || int(rows.size()) > kMaxBucket)
            return;
        for (int ya : rows) {
            const int dy = ya - y;
            if (dy == 0 || dy < -range || dy > range || ya < zoneFrom || ya >= zoneTo)
                continue;
            ++votes[size_t(dy + range)];
        }
    };
    for (int y = zoneFrom; y < zoneTo; ++y) {
        for (int k = 0; k < kSegments; ++k) {
            const uint8_t act = b.actAt(y, k);
            if (act == 0 || m.fixed[size_t(k)])
                continue;
            const int q = b.meanAt(y, k) >> 2;
            for (int dq = -1; dq <= 1; ++dq) {
                if (q + dq >= 0 && q + dq < kMeanBins)
                    voteBucket(k, q + dq, act, y);
            }
            // Also tolerate a +/-1 activity bucket boundary shift from subpixel text redrawing.
            if (act > 1)
                voteBucket(k, q, act - 1, y);
            if (act + 1 < kActBins)
                voteBucket(k, q, act + 1, y);
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
        int dy = 0;
        double score = 0;
        int votes = 0;
        double rowScore = 0;
        int matchedRows = 0;
    };
    std::vector<Checked> valid;
    std::vector<Checked> checked;
    for (int dy : candidates) {
        int compared = 0;
        int agreed = 0;
        int infoRows = 0;
        int matchedRows = 0;
        for (int y = std::max(zoneFrom, zoneFrom - dy); y < std::min(zoneTo, zoneTo - dy); ++y) {
            const int ya = y + dy;
            int rComp = 0;
            int rAgree = 0;
            for (int k = 0; k < kSegments; ++k) {
                if (m.fixed[size_t(k)] || (b.actAt(y, k) == 0 && a.actAt(ya, k) == 0))
                    continue;
                ++rComp;
                rAgree += b.agrees(y, a, ya, k);
            }
            if (rComp > 0) {
                compared += rComp;
                agreed += rAgree;
                ++infoRows;
                if (rAgree * 4 >= rComp * 3)
                    ++matchedRows;
            }
        }
        if (compared == 0)
            continue;
        const double score = double(agreed) / compared;
        const double rowScore = infoRows > 0 ? double(matchedRows) / infoRows : 0.0;
        checked.push_back({dy, score, agreed, rowScore, matchedRows});
        if (score >= minScore && agreed >= minVotes)
            valid.push_back({dy, score, agreed, rowScore, matchedRows});
    }
    if (valid.empty() && !checked.empty()) {
        const auto leader = std::max_element(checked.begin(), checked.end(), [](const Checked& l, const Checked& r) { return l.score < r.score; });
        double other = 0;
        for (const Checked& c : checked)
            if (std::abs(c.dy - leader->dy) >= 3)
                other = std::max(other, c.score);
        if (leader->dy != 0 && leader->score >= kClearScore && leader->votes >= kClearVotes && leader->score >= kClearMargin * other) {
            valid.push_back(*leader);
        } else {
            // Large animated GIF/video in a sparse chat/feed window can dominate
            // raw segment count while surrounding text rows still move rigidly.
            const Checked* rowLeader = nullptr;
            for (const Checked& c : checked) {
                if (c.dy == 0 || c.votes < kClearVotes || c.matchedRows < 32)
                    continue;
                if (!rowLeader || c.rowScore > rowLeader->rowScore
                    || (c.rowScore == rowLeader->rowScore && c.score > rowLeader->score))
                    rowLeader = &c;
            }
            if (rowLeader && rowLeader->score >= 0.22 && rowLeader->rowScore >= 0.35) {
                double otherScore = 0;
                double otherRowScore = 0;
                for (const Checked& c : checked) {
                    if (std::abs(c.dy - rowLeader->dy) >= 3) {
                        otherScore = std::max(otherScore, c.score);
                        otherRowScore = std::max(otherRowScore, c.rowScore);
                    }
                }
                if (rowLeader->score >= 2.5 * otherScore && rowLeader->rowScore >= 2.5 * otherRowScore)
                    valid.push_back(*rowLeader);
            }
        }
    }
    // 5b. Subpixel fallback for smooth-scrolling / fractional-DPI frames where
    // every text row sits at a fractional vertical phase between adjacent rows.
    // Include dy == 0 and allow refinement back to 0 so stationary frames with
    // in-place animation never oscillate as fake +/-1 px shifts.
    if (valid.empty() && !checked.empty()) {
        std::vector<int> subCandidates;
        subCandidates.reserve(12);
        subCandidates.push_back(0);
        if (m_velocity != 0 && std::abs(m_velocity) <= range)
            subCandidates.push_back(m_velocity);
        if (expectDy && std::abs(*expectDy) <= range)
            subCandidates.push_back(*expectDy);
        std::vector<size_t> order(checked.size());
        for (size_t i = 0; i < checked.size(); ++i)
            order[i] = i;
        const size_t topScore = std::min<size_t>(6, order.size());
        std::partial_sort(order.begin(), order.begin() + ptrdiff_t(topScore), order.end(),
            [&](size_t l, size_t r) { return checked[l].score > checked[r].score; });
        for (size_t i = 0; i < topScore; ++i)
            subCandidates.push_back(checked[order[i]].dy);
        const size_t topRow = std::min<size_t>(2, order.size());
        std::partial_sort(order.begin(), order.begin() + ptrdiff_t(topRow), order.end(),
            [&](size_t l, size_t r) { return checked[l].rowScore > checked[r].rowScore; });
        for (size_t i = 0; i < topRow; ++i)
            subCandidates.push_back(checked[order[i]].dy);
        std::sort(subCandidates.begin(), subCandidates.end());
        subCandidates.erase(std::unique(subCandidates.begin(), subCandidates.end()), subCandidates.end());

        std::vector<Checked> subChecked;
        subChecked.reserve(subCandidates.size());
        for (int dy : subCandidates) {
            int compared = 0;
            int agreed = 0;
            for (int y = std::max(zoneFrom, zoneFrom - dy); y < std::min(zoneTo, zoneTo - dy); ++y) {
                const int ya = y + dy;
                for (int k = 0; k < kSegments; ++k) {
                    if (m.fixed[size_t(k)] || (b.actAt(y, k) == 0 && a.actAt(ya, k) == 0))
                        continue;
                    ++compared;
                    agreed += b.agreesSubpixel(y, a, ya, k);
                }
            }
            if (compared > 0)
                subChecked.push_back({dy, double(agreed) / compared, agreed});
        }
        if (!subChecked.empty()) {
            const auto meanSadFor = [&](int cand) -> double {
                int64_t sadSum = 0;
                int64_t pxSum = 0;
                for (int y = std::max(zoneFrom, zoneFrom - cand); y < std::min(zoneTo, zoneTo - cand); ++y) {
                    const int ya = y + cand;
                    for (int k = 0; k < kSegments; ++k) {
                        if (m.fixed[size_t(k)] || (b.actAt(y, k) == 0 && a.actAt(ya, k) == 0))
                            continue;
                        sadSum += b.segmentSad(y, a, ya, k);
                        pxSum += b.seg[size_t(k) + 1] - b.seg[size_t(k)];
                    }
                }
                return pxSum > 0 ? double(sadSum) / double(pxSum) : 1e18;
            };
            const auto refineIntDy = [&](int dy) {
                int bestDy = dy;
                double bestMeanSad = 1e18;
                const auto tryCand = [&](int cand) {
                    if (cand < -range || cand > range)
                        return;
                    const double sad = meanSadFor(cand);
                    if (sad < bestMeanSad) {
                        bestMeanSad = sad;
                        bestDy = cand;
                    }
                };
                for (int d = -1; d <= 1; ++d)
                    tryCand(dy + d);
                if (std::abs(dy) <= 2)
                    tryCand(0);
                return bestDy;
            };
            const auto leader = std::max_element(subChecked.begin(), subChecked.end(),
                [](const Checked& l, const Checked& r) { return l.score < r.score; });
            double other = 0;
            double zeroScore = 0;
            for (const Checked& c : subChecked) {
                if (c.dy == 0)
                    zeroScore = c.score;
                if (std::abs(c.dy - leader->dy) >= 3)
                    other = std::max(other, c.score);
            }
            const int needVotes = std::max(minVotes, kSubpixelVotes);
            if (leader->dy == 0) {
                if (leader->score >= kSubpixelScore && leader->votes >= needVotes) {
                    m.unchanged = true;
                    return m;
                }
            } else if (leader->score >= 0.88 && leader->votes >= std::max(needVotes, 64)) {
                // High-consensus subpixel match (including repeating tables where
                // multiple shifts score >= 0.88): pass all near-tied shifts to the
                // velocity / expected-shift tie-breaker in step 6.
                for (const Checked& c : subChecked) {
                    if (c.score >= leader->score - kTieScore && c.votes >= needVotes)
                        valid.push_back({c.dy == 0 ? 0 : refineIntDy(c.dy), c.score, c.votes});
                }
            } else if (leader->score >= kSubpixelScore && leader->votes >= needVotes
                && leader->score >= kSubpixelMargin * other
                && (std::abs(leader->dy) > 2 || zeroScore < leader->score - kTieScore || meanSadFor(leader->dy) < meanSadFor(0))) {
                valid.push_back({refineIntDy(leader->dy), leader->score, leader->votes});
            }
        }
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

std::optional<ScrollStitcher::RelocHit> ScrollStitcher::relocalize(const Analysis& b) const
{
    const int h = b.h;
    const int y0 = m_bandsKnown ? m_top : 0;
    const int y1 = m_bandsKnown ? h - m_bottom : h;
    if (y1 - y0 < kMinOverlap || m_sigs.empty() || m_far.isNull() || m_farOffset <= m_nearOffset)
        return std::nullopt;
    const int span = m_farOffset - m_nearOffset + 1;
    if (span <= 1)
        return std::nullopt;

    std::vector<int> votes(size_t(span), 0);
    const int ycStart = std::max(m_nearOffset + y0, m_sigStart);
    const int ycEnd = std::min(m_farOffset + y1, m_sigStart + int(m_sigs.size()));
    const auto voteBin = [&](int k, int meanQ, int actB, int yc) {
        const std::span<const int> rows = b.rowsIn(k, meanQ, actB);
        if (rows.empty() || int(rows.size()) > kMaxBucket)
            return;
        for (int yb : rows) {
            if (yb < y0 || yb >= y1)
                continue;
            const int off = yc - yb;
            if (off >= m_nearOffset && off <= m_farOffset)
                ++votes[size_t(off - m_nearOffset)];
        }
    };
    for (int yc = ycStart; yc < ycEnd; yc += 2) {
        const RowSig& sig = m_sigs[size_t(yc - m_sigStart)];
        for (int k = 0; k < kSegments; ++k) {
            if (m_fixedColsKnown && m_fixedCols[size_t(k)])
                continue;
            const uint8_t act = sig.act[size_t(k)];
            if (act == 0)
                continue;
            const int q = sig.mean[size_t(k)] >> 2;
            for (int dq = -1; dq <= 1; ++dq) {
                if (q + dq >= 0 && q + dq < kMeanBins)
                    voteBin(k, q + dq, act, yc);
            }
        }
    }

    // Gather up to 6 well-separated vote peaks.
    struct Peak {
        int votes = 0;
        int offset = 0;
    };
    std::vector<Peak> peaks;
    peaks.reserve(6);
    for (int i = 0; i < span; ++i) {
        const int v = votes[size_t(i)];
        if (v < 8)
            continue;
        const int off = m_nearOffset + i;
        bool merged = false;
        for (Peak& p : peaks) {
            if (std::abs(p.offset - off) < 8) {
                if (v > p.votes) {
                    p.votes = v;
                    p.offset = off;
                }
                merged = true;
                break;
            }
        }
        if (merged)
            continue;
        if (peaks.size() < 6) {
            peaks.push_back({v, off});
        } else {
            auto weakest = std::min_element(peaks.begin(), peaks.end(),
                [](const Peak& l, const Peak& r) { return l.votes < r.votes; });
            if (v > weakest->votes)
                *weakest = {v, off};
        }
    }
    if (peaks.empty())
        return std::nullopt;

    struct Eval {
        int offset = 0;
        double score = 0;
        int agreed = 0;
        double rowScore = 0;
        int matchedRows = 0;
    };
    const auto evalOffset = [&](int off) -> Eval {
        Eval e;
        e.offset = off;
        int compared = 0;
        int infoRows = 0;
        for (int y = y0; y < y1; ++y) {
            const int cy = off + y;
            const uint32_t* cRow = canvasRow(cy);
            if (!cRow)
                continue;
            const RowSig* sig = (cy >= m_sigStart && cy < m_sigStart + int(m_sigs.size()))
                ? &m_sigs[size_t(cy - m_sigStart)]
                : nullptr;
            const uint8_t* bLum = b.lumRow(y);
            int rComp = 0;
            int rAgree = 0;
            for (int k = 0; k < kSegments; ++k) {
                if (m_fixedColsKnown && m_fixedCols[size_t(k)])
                    continue;
                if (b.actAt(y, k) == 0 && (!sig || sig->act[size_t(k)] == 0))
                    continue;
                const int x0 = b.seg[size_t(k)];
                const int x1 = b.seg[size_t(k) + 1];
                const int n = x1 - x0;
                int sad = 0;
                for (int x = x0; x < x1; ++x)
                    sad += std::abs(int(bLum[x]) - luma(cRow[x]));
                ++rComp;
                if (sad <= kTolerance * n)
                    ++rAgree;
            }
            if (rComp > 0) {
                compared += rComp;
                e.agreed += rAgree;
                ++infoRows;
                if (rAgree * 4 >= rComp * 3)
                    ++e.matchedRows;
            }
        }
        if (compared > 0)
            e.score = double(e.agreed) / compared;
        if (infoRows > 0)
            e.rowScore = double(e.matchedRows) / infoRows;
        return e;
    };

    std::vector<Eval> evals;
    evals.reserve(peaks.size());
    for (const Peak& p : peaks) {
        Eval bestPeak;
        for (int d = -1; d <= 1; ++d) {
            const int cand = std::clamp(p.offset + d, m_nearOffset, m_farOffset);
            const Eval e = evalOffset(cand);
            if (e.score > bestPeak.score || (e.score == bestPeak.score && e.agreed > bestPeak.agreed))
                bestPeak = e;
        }
        evals.push_back(bestPeak);
    }

    const auto bestIt = std::max_element(evals.begin(), evals.end(),
        [](const Eval& l, const Eval& r) { return l.score < r.score; });
    Eval best = *bestIt;
    Eval runnerUp;
    for (const Eval& e : evals) {
        if (std::abs(e.offset - best.offset) >= 16) {
            if (e.score > runnerUp.score)
                runnerUp = e;
        }
    }

    if (best.agreed < std::max(64, kMinVotes * 2) || best.matchedRows < 24)
        return std::nullopt;

    // Repeating canvas tie-breaker: if multiple distant matches are near-exact,
    // pick the one closest to m_prevOffset if it is within 2 viewports.
    if (best.score >= 0.88 && runnerUp.score >= best.score - 0.05) {
        const Eval* nearest = nullptr;
        for (const Eval& e : evals) {
            if (e.score < best.score - 0.05 || e.agreed < std::max(64, kMinVotes * 2))
                continue;
            if (!nearest || std::abs(e.offset - m_prevOffset) < std::abs(nearest->offset - m_prevOffset))
                nearest = &e;
        }
        if (nearest && std::abs(nearest->offset - m_prevOffset) <= 2 * h)
            return RelocHit {nearest->offset, nearest->score, nearest->agreed};
        return std::nullopt;
    }

    if (best.score >= 0.78 && (best.score >= runnerUp.score + 0.06 || runnerUp.score < 0.70))
        return RelocHit {best.offset, best.score, best.agreed};

    // Relocalize even when a large animated GIF/video plays in the viewport.
    if (best.score >= 0.30 && best.rowScore >= 0.45 && best.agreed >= kClearVotes
        && best.score >= 2.5 * runnerUp.score && best.rowScore >= 2.5 * runnerUp.rowScore)
        return RelocHit {best.offset, best.score, best.agreed};

    return std::nullopt;
}

ScrollStitcher::Step ScrollStitcher::add(const QImage& given, std::optional<int> expectDy, bool guess)
{
    // Sideways, only a frame the size of the region (turned here) is taken.
    const QImage frame = !m_options.horizontal ? given : given.size() == m_size.transposed() ? transposed(given) : QImage();
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
        writeSigs(*b, 0, 0, h);
        m_far = b->image;
        m_farOffset = 0;
        m_farFrom = 0;
        m_farRows = ok ? h : std::max(0, limitHeight());
        m_full = !ok;
        m_anchor = b;
        m_anchorOffset = 0;
        m_nearAnalysis = b;
        m_farAnalysis = b;
        m_prev = std::move(b);
        m_prevOffset = 0;
        return finish(m_full ? State::Full : State::Appended);
    }
    const int zone = std::max(1, h - m_top - m_bottom);
    const int stride = std::clamp(zone / 3, 48, 320);
    Match m;
    bool promoteAnchor = false;
    bool jumpedAnchor = false;
    const bool tryAnchor = m_anchor && m_anchor != m_prev
        && std::abs(m_prevOffset - m_anchorOffset) <= stride;
    if (tryAnchor) {
        std::optional<int> anchorExpect;
        if (expectDy && *expectDy != 0)
            anchorExpect = (m_prevOffset - m_anchorOffset) + *expectDy;
        else if (m_velocity != 0)
            anchorExpect = (m_prevOffset - m_anchorOffset) + m_velocity;
        Match am = match(*m_anchor, *b, anchorExpect);
        // Accept an anchor match only while it stays within strong overlap
        // (<= stride) and high consensus; larger jumps fall back to m_prev
        // where overlap is wider (e.g. repetitive file lists jumping far).
        const bool anchorReliable = am.unchanged
            || (am.found && std::abs(am.dy) <= stride && am.score >= 0.80);
        if (anchorReliable) {
            const int targetOffset = m_anchorOffset + (am.found ? am.dy : 0);
            if (targetOffset == m_prevOffset) {
                m.unchanged = true;
            } else {
                m = am;
                m.unchanged = false;
                m.found = true;
                m.dy = targetOffset - m_prevOffset;
                if (am.unchanged) {
                    m.score = 1.0;
                    m.votes = zone;
                }
            }
        }
    }
    if (!m.found && !m.unchanged) {
        m = match(*m_prev, *b, expectDy);
        promoteAnchor = tryAnchor; // only promote early when the existing anchor could no longer match
    }
    if (!m.found && !m.unchanged) {
        const auto tryRef = [&](const std::shared_ptr<const Analysis>& ref, int refOffset) {
            if (!ref || ref == m_prev || ref == m_anchor)
                return false;
            Match rm = match(*ref, *b, std::nullopt);
            if (!rm.unchanged && !(rm.found && rm.score >= 0.72))
                return false;
            const int targetOffset = refOffset + (rm.found ? rm.dy : 0);
            if (targetOffset == m_prevOffset) {
                m.unchanged = true;
            } else {
                m = rm;
                m.unchanged = false;
                m.found = true;
                m.dy = targetOffset - m_prevOffset;
                if (rm.unchanged) {
                    m.score = 1.0;
                    m.votes = zone;
                }
            }
            promoteAnchor = true;
            jumpedAnchor = true;
            return true;
        };
        if (!tryRef(m_farAnalysis, m_farOffset))
            tryRef(m_nearAnalysis, m_nearOffset);
    }
    if (!m.found && !m.unchanged && height() > h) {
        if (const auto hit = relocalize(*b)) {
            const int prevOff = m_prevOffset;
            m_prev = std::move(b);
            m_prevOffset = hit->offset;
            m_anchor = m_prev;
            m_anchorOffset = hit->offset;
            m_lost = 0;
            m_velocity = 0;
            step.dy = hit->offset - prevOff;
            step.score = hit->score;
            step.votes = hit->votes;
            return finish(State::Behind);
        }
    }
    const bool atEnd = m_prevOffset == m_farOffset; // the frame before is the one the result ends with
    if (m.unchanged) {
        if (atEnd) {
            // The end's newest look: a picture that loaded, a fade that ended.
            write(b->image, m_farOffset, m_farFrom, h - m_bottom);
            writeSigs(*b, m_farOffset, m_farFrom, h);
            m_far = b->image;
            m_farAnalysis = b;
        } else if (m_prevOffset == m_nearOffset) {
            const int to = h - m_bottom - (h - m_top - m_bottom) / 6;
            write(b->image, m_nearOffset, 0, to);
            writeSigs(*b, m_nearOffset, 0, to);
            m_farFrom = std::max(m_farFrom, std::min(h - m_bottom, m_nearOffset + to - m_farOffset));
            m_nearAnalysis = b;
        }
        if (m_prevOffset == m_anchorOffset || promoteAnchor) {
            m_anchor = b;
            m_anchorOffset = m_prevOffset;
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
        promoteAnchor = true;
    }
    m_lost = 0;
    step.dy = m.dy;
    step.score = m.score;
    step.votes = m.votes;
    m_velocity = jumpedAnchor ? 0 : m.dy;
    if (m.bandsMeasured && !step.predicted && !jumpedAnchor) {
        if (!m_bandsKnown) {
            m_top = m.top;
            m_bottom = m.bottom;
            m_bandsKnown = true;
        } else {
            if (m.top < m_top) {
                m_top = m.top;
                m_topStable = 0;
                m_topTravel = 0;
            } else if (std::abs(m.dy) >= 8 && m.top > m_top && m.top <= h / 3 && hasBarContent(*b, m_top, m.top)) {
                if (m_topStable > 0 && std::abs(m.top - m_topCandidate) <= 2) {
                    ++m_topStable;
                    m_topTravel += std::abs(m.dy);
                } else {
                    m_topCandidate = m.top;
                    m_topStable = 1;
                    m_topTravel = std::abs(m.dy);
                }
                if (m_topStable >= 3 && m_topTravel >= 48) {
                    m_top = std::min(m_topCandidate, m.top);
                    m_topStable = 0;
                    m_topTravel = 0;
                }
            } else if (std::abs(m.dy) >= 8) {
                m_topStable = 0;
                m_topTravel = 0;
            }

            if (m.bottom < m_bottom) {
                m_bottom = m.bottom;
                m_bottomStable = 0;
                m_bottomTravel = 0;
            } else if (std::abs(m.dy) >= 8 && m.bottom > m_bottom && m.bottom <= h / 3 && hasBarContent(*b, h - m.bottom, h - m_bottom)) {
                if (m_bottomStable > 0 && std::abs(m.bottom - m_bottomCandidate) <= 2) {
                    ++m_bottomStable;
                    m_bottomTravel += std::abs(m.dy);
                } else {
                    m_bottomCandidate = m.bottom;
                    m_bottomStable = 1;
                    m_bottomTravel = std::abs(m.dy);
                }
                if (m_bottomStable >= 3 && m_bottomTravel >= 48) {
                    m_bottom = std::min(m_bottomCandidate, m.bottom);
                    m_bottomStable = 0;
                    m_bottomTravel = 0;
                }
            } else if (std::abs(m.dy) >= 8) {
                m_bottomStable = 0;
                m_bottomTravel = 0;
            }
        }
        if (std::abs(m.dy) >= 12) {
            if (!m_fixedColsKnown) {
                m_fixedCols = m.fixed;
                m_fixedColsKnown = true;
            } else {
                for (int k = 0; k < kSegments; ++k)
                    m_fixedCols[size_t(k)] = m_fixedCols[size_t(k)] && m.fixed[size_t(k)];
            }
        }
    }
    const int offset = m_prevOffset + m.dy;
    const auto updateAnchor = [&](bool force) {
        if (force || promoteAnchor || !m_anchor || std::abs(offset - m_anchorOffset) >= stride) {
            m_anchor = m_prev;
            m_anchorOffset = offset;
        }
    };
    if (offset < m_nearOffset) {
        const int begin = m_nearOffset + m_top;
        const int safeTo = h - m_bottom - (h - m_top - m_bottom) / 6;
        const int to = std::clamp(begin - offset, safeTo, h - m_bottom);
        m_nearOffset = std::max(offset, m_farOffset + m_farRows - limitHeight());
        const bool ok = write(b->image, offset, 0, to);
        writeSigs(*b, offset, 0, to);
        // The new content can replace the old top frame's header in the
        // overlap. Keep only the untouched suffix of the bottommost frame.
        m_farFrom = std::max(m_farFrom, std::min(h - m_bottom, offset + to - m_farOffset));
        m_prev = std::move(b);
        m_prevOffset = offset;
        m_nearAnalysis = m_prev;
        updateAnchor(false);
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
        bool trimmedBounce = false;
        if (atEnd && !step.predicted && bounced(*m_prev, *b, m_farOffset - offset)) {
            m_far = b->image;
            m_farOffset = offset;
            m_farFrom = std::max(m_top, h / 6);
            m_farRows = std::min(h, m_nearOffset + limitHeight() - offset);
            writeSigs(*b, offset, m_farFrom, h);
            m_farAnalysis = b;
            trimmedBounce = true;
        } else if (m_prevOffset == m_nearOffset && !step.predicted && bounced(*m_prev, *b, offset - m_nearOffset, true)) {
            // The symmetric bounce at the top must remove its blank rows
            // and restore the header from the frame now at rest.
            m_nearOffset = offset;
            const int to = h - m_bottom - (h - m_top - m_bottom) / 6;
            write(b->image, offset, 0, to);
            writeSigs(*b, offset, 0, to);
            m_farFrom = std::max(m_farFrom, std::min(h - m_bottom, offset + to - m_farOffset));
            m_nearAnalysis = b;
            trimmedBounce = true;
        }
        m_prev = std::move(b);
        m_prevOffset = offset;
        updateAnchor(trimmedBounce);
        return finish(State::Behind);
    }
    m_prev = std::move(b);
    m_prevOffset = offset;
    updateAnchor(false);
    // Ahead of the end: this frame's rows go on the canvas, from below the
    // top band (and the top quarter, where a bar that sticks only once the
    // page scrolls would be), or from where the canvas ends when the step
    // was longer than that leaves.
    const int canvasEnd = m_farOffset + h - m_bottom;
    const int pairTop = m.bandsMeasured && m.top <= h / 3 ? m.top : 0;
    const int protectTop = std::max({m_top, pairTop, h / 4});
    const int from = std::clamp(canvasEnd - offset, m_top, protectTop);
    const bool ok = write(m_prev->image, offset, from, h - m_bottom);
    writeSigs(*m_prev, offset, from, h);
    m_far = m_prev->image;
    m_farOffset = offset;
    m_farFrom = from;
    m_farRows = std::min(h, m_nearOffset + limitHeight() - offset);
    m_farAnalysis = m_prev;
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
    return m_options.horizontal ? transposed(straight()) : straight();
}

QImage ScrollStitcher::straight() const
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
    return m_options.horizontal ? transposed(strip(box.transposed())) : strip(box);
}

QImage ScrollStitcher::strip(QSize box) const
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

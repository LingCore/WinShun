#include "ContentIndex.h"

#include "ContentScanner.h"
#include "Documents.h"
#include "TextUtil.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <numeric>
#include <thread>
#include <tuple>

namespace ws {

namespace {

constexpr unsigned kIdBits = 28; // m_memory packs key << kIdBits | content
constexpr std::uint64_t kIdMask = (std::uint64_t {1} << kIdBits) - 1;
constexpr std::size_t kMaxIds = std::size_t {1} << kIdBits; // of documents, and of contents
constexpr std::size_t kFlushPairs = std::size_t {1} << 20; // 8 MB of grams in memory make a segment
constexpr std::size_t kMaxMemoryPairs = kFlushPairs * 4; // ... unless segments cannot be written
constexpr std::size_t kMaxSegments = 64; // more (whatever their levels) are merged into one
constexpr std::size_t kOrderTail = 1024;
// 2: ASCII trigrams; 3: dense documents; 4: contents shared by documents, segment files v4;
// 5: trigrams of any ASCII characters, spaces and punctuation too; 6: documents' texts;
// 7: texts shared by documents, segment files v5 (contents ordered by bisection)
constexpr std::uint32_t kStateVersion = 7;
// The text file is rewritten once texts no document has take more than
// this, and more than the others.
constexpr std::uint64_t kTextGarbage = 32u << 20;

// m_docState: a base state and flags.
constexpr std::uint8_t kLive = 0; // its grams are in the index
constexpr std::uint8_t kEmpty = 1; // nothing to find in it: empty, too large, unreadable
constexpr std::uint8_t kDead = 2; // replaced by a newer document, or its file is gone
constexpr std::uint8_t kBase = 3;
constexpr std::uint8_t kDirty = 0x04; // written to since it was read: read it again
constexpr std::uint8_t kUnsure = 0x08; // may have changed unseen: compare its size and time

bool isDead(std::uint8_t state) noexcept
{
    return (state & kBase) == kDead;
}

// Whether a search can go by the document instead of reading the file.
bool isCurrent(std::uint8_t state) noexcept
{
    return !isDead(state) && (state & (kDirty | kUnsure)) == 0;
}

// 96 bits of the SHA-256 of some bytes.
std::optional<grams::Fingerprint> printOf(std::span<const std::byte> bytes)
{
    std::array<UCHAR, 32> digest {};
    // The bytes are not written to: the parameter is not const only for C's sake.
    auto* const input = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data()));
    if (bytes.size() > std::numeric_limits<ULONG>::max()
        || !BCRYPT_SUCCESS(::BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, input, static_cast<ULONG>(bytes.size()),
            digest.data(), static_cast<ULONG>(digest.size()))))
        return std::nullopt;
    grams::Fingerprint out;
    std::memcpy(&out.high, digest.data(), sizeof out.high);
    std::memcpy(&out.low, digest.data() + sizeof out.high, sizeof out.low);
    return out;
}

// ---- segment files ----------------------------------------------------------------
//
//   Header
//   Print[printCount]     the fingerprints of the segment's contents, ascending
//   ContentId[listedCount] the contents the table lists (see below), most
//                         listed first; none: the table lists contents themselves
//   postings              by key; zero-padded to a multiple of 8 bytes
//   Block[blockCount + 1] every kBlockKeys-th key, where its posting and its
//                         table entry begin; the last is a sentinel
//   table                 per key, as LEB128: its distance from the key before
//                         (not for the first of a block), how many contents
//                         have it, then either those contents (up to kInline,
//                         by their places in the list above, ascending: the
//                         first's place, then for each the places skipped
//                         since the one before) or the size of its posting
//
// A posting holds the contents that have the key, in binary interpolative
// coding (Moffat and Stuiver): the middle one, in as few bits as the values
// left for it allow, then each half the same way. Contents of similar files
// get numbers close together (see merge), so the numbers come in clusters and
// runs, which this coding writes in next to nothing. The table comes last so
// that a merge can write the postings as it goes.
//
// The keys few contents have are mostly pairs of Chinese characters, most of
// them in a few long documents: by how often the table lists them, those
// take a byte or two each; by their numbers, which follow what the contents
// have in common, near three (4.7 MB more of 94, wsbench --content-index).
constexpr char kSegmentMagic[8] = {'Q', 'F', 'G', 'R', 'A', 'M', 'S', '\0'};
constexpr std::uint32_t kSegmentVersion = 5;

struct SegmentHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t keyCount;
    std::uint32_t contentBegin;
    std::uint32_t contentEnd;
    std::uint32_t level; // 0: written from memory; n + 1: merged from segments of level n
    std::uint32_t printCount;
    std::uint32_t listedCount;
    std::uint32_t reserved;
    std::uint64_t postingBytes; // without the padding
    std::uint64_t tableBytes;
};
static_assert(sizeof(SegmentHeader) == 56);

// A content of the segment, by the fingerprint of its grams: a file read
// later with the same grams gets the same content.
struct Print {
    std::uint64_t high;
    std::uint32_t low;
    std::uint32_t content;

    grams::Fingerprint fingerprint() const noexcept { return {high, low}; }
};
static_assert(sizeof(Print) == 16);

struct SegmentBlock {
    grams::Key key; // its first
    std::uint32_t posting; // offset of the first's posting
    std::uint32_t entry; // offset of the first's table entry
};
static_assert(sizeof(SegmentBlock) == 16);

constexpr std::uint32_t kTopLevel = 0xFFFF; // merged from every segment
constexpr std::size_t kMergeFactor = 8; // this many segments of a level in a row make one of the next level
constexpr std::uint32_t kBlockKeys = 64;
constexpr std::uint64_t kInline = 4; // lists this short go in the table

constexpr std::uint64_t aligned8(std::uint64_t n) noexcept
{
    return (n + 7) & ~std::uint64_t {7};
}

constexpr std::uint64_t blocksFor(std::uint64_t keys) noexcept
{
    return (keys + kBlockKeys - 1) / kBlockKeys;
}

void encode(std::vector<std::uint8_t>& out, std::uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

// LEB128 numbers from a range of bytes. Anything malformed reads as 0 and
// leaves it failed and at the end.
class NumberReader {
public:
    NumberReader(const std::uint8_t* begin, const std::uint8_t* end) noexcept
        : m_p(begin)
        , m_end(end)
    {
    }
    std::uint64_t next() noexcept
    {
        std::uint64_t v = 0;
        for (unsigned shift = 0; shift < 64 && m_p < m_end; shift += 7) {
            const std::uint8_t b = *m_p++;
            v |= std::uint64_t {b & 0x7Fu} << shift;
            if (!(b & 0x80))
                return v;
        }
        m_ok = false;
        m_p = m_end;
        return 0;
    }
    bool ok() const noexcept { return m_ok; }
    bool atEnd() const noexcept { return m_p >= m_end; }
    const std::uint8_t* position() const noexcept { return m_p; }

private:
    const std::uint8_t* m_p;
    const std::uint8_t* m_end;
    bool m_ok = true;
};

// Bits, least significant first.
class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) noexcept
        : m_out(out)
    {
    }
    void put(std::uint64_t value, unsigned bits) // up to 32
    {
        m_bits |= value << m_count;
        for (m_count += bits; m_count >= 8; m_count -= 8) {
            m_out.push_back(static_cast<std::uint8_t>(m_bits));
            m_bits >>= 8;
        }
    }
    void flush()
    {
        if (m_count > 0)
            m_out.push_back(static_cast<std::uint8_t>(m_bits));
        m_bits = 0;
        m_count = 0;
    }

private:
    std::vector<std::uint8_t>& m_out;
    std::uint64_t m_bits = 0;
    unsigned m_count = 0;
};

// Reads zeros past the end.
class BitReader {
public:
    BitReader(const std::uint8_t* begin, const std::uint8_t* end) noexcept
        : m_p(begin)
        , m_end(end)
    {
    }
    std::uint64_t get(unsigned bits) noexcept // up to 32
    {
        for (; m_count < bits; m_count += 8)
            m_bits |= std::uint64_t {m_p < m_end ? *m_p++ : std::uint8_t {0}} << m_count;
        const std::uint64_t v = m_bits & ((std::uint64_t {1} << bits) - 1);
        m_bits >>= bits;
        m_count -= bits;
        return v;
    }

private:
    const std::uint8_t* m_p;
    const std::uint8_t* m_end;
    std::uint64_t m_bits = 0;
    unsigned m_count = 0;
};

// A number below `range` (2 or more) in a minimal binary code: with c bits
// enough for every value, the first 2^c - range values take c - 1 bits.
void putBelow(BitWriter& w, std::uint64_t x, std::uint64_t range)
{
    const auto c = static_cast<unsigned>(std::bit_width(range - 1));
    const std::uint64_t shorter = (std::uint64_t {1} << c) - range;
    if (x < shorter) {
        w.put(x, c - 1);
        return;
    }
    // The others: c - 1 bits that are not a shorter code, and one more.
    const std::uint64_t rest = x - shorter;
    w.put(shorter + (rest >> 1), c - 1);
    w.put(rest & 1, 1);
}

// Always below `range`, whatever the bits.
std::uint64_t getBelow(BitReader& r, std::uint64_t range) noexcept
{
    const auto c = static_cast<unsigned>(std::bit_width(range - 1));
    const std::uint64_t shorter = (std::uint64_t {1} << c) - range;
    const std::uint64_t first = r.get(c - 1);
    if (first < shorter)
        return first;
    return shorter + ((first - shorter) << 1 | r.get(1));
}

// The n ascending numbers of v, all in [low, high] (n <= high - low + 1).
void putInterpolative(BitWriter& w, const std::uint32_t* v, std::size_t n, std::uint64_t low, std::uint64_t high)
{
    if (n == 0 || high - low + 1 == n)
        return; // nothing, or every value: nothing to write
    const std::size_t m = n / 2;
    const std::uint64_t from = low + m; // m numbers below it, n - 1 - m above
    const std::uint64_t to = high - (n - 1 - m);
    if (to > from)
        putBelow(w, v[m] - from, to - from + 1);
    if (m > 0)
        putInterpolative(w, v, m, low, v[m] - 1);
    putInterpolative(w, v + m + 1, n - 1 - m, std::uint64_t {v[m]} + 1, high);
}

// Ascending numbers in [low, high], whatever the bits.
void getInterpolative(BitReader& r, std::uint32_t* v, std::size_t n, std::uint64_t low, std::uint64_t high) noexcept
{
    if (n == 0)
        return;
    if (high - low + 1 == n) {
        for (std::size_t i = 0; i < n; ++i)
            v[i] = static_cast<std::uint32_t>(low + i);
        return;
    }
    const std::size_t m = n / 2;
    const std::uint64_t from = low + m;
    const std::uint64_t to = high - (n - 1 - m);
    const std::uint64_t x = to > from ? from + getBelow(r, to - from + 1) : from;
    v[m] = static_cast<std::uint32_t>(x);
    if (m > 0)
        getInterpolative(r, v, m, low, x - 1);
    getInterpolative(r, v + m + 1, n - 1 - m, x + 1, high);
}

// Keys with their contents, encoded as a segment holds them: what follows a
// key in the table (how many contents, then the contents when they are few,
// else the size of their posting), and the posting. Where in the file they
// go does not change them, so a merge encodes stretches of keys on several
// threads, and the writer puts them one after the other.
struct EncodedKeys {
    static constexpr std::uint32_t kNoPlace = 0xFFFF'FFFFu;

    std::vector<grams::Key> keys;
    std::vector<std::uint8_t> entries;
    std::vector<std::uint8_t> postings;
    std::vector<std::uint32_t> entryEnds; // by key, in `entries`
    std::vector<std::uint32_t> postingEnds; // by key, in `postings`
    bool ok = true; // false: a content the table lists had no place

    // Contents ascending and distinct, in [begin, end); none: the key is left
    // out. `places`: the segment's list of the contents the table lists, as
    // each content's place in it (by content - begin; kNoPlace: not in it);
    // empty when the segment has none.
    void add(grams::Key key, std::span<const std::uint32_t> contents, std::uint32_t begin, std::uint32_t end,
        std::span<const std::uint32_t> places = {})
    {
        if (contents.empty())
            return;
        encode(entries, contents.size());
        if (contents.size() <= kInline) {
            std::array<std::uint32_t, kInline> listed {};
            for (std::size_t i = 0; i < contents.size(); ++i) {
                listed[i] = places.empty() ? contents[i] - begin : places[contents[i] - begin];
                ok = ok && listed[i] != kNoPlace;
            }
            std::sort(listed.begin(), listed.begin() + static_cast<std::ptrdiff_t>(contents.size()));
            std::uint64_t next = 0;
            for (std::size_t i = 0; i < contents.size(); ++i) {
                encode(entries, listed[i] - next);
                next = std::uint64_t {listed[i]} + 1;
            }
        } else {
            const std::size_t start = postings.size();
            BitWriter bits(postings);
            putInterpolative(bits, contents.data(), contents.size(), begin, std::uint64_t {end} - 1);
            bits.flush();
            encode(entries, postings.size() - start);
        }
        keys.push_back(key);
        entryEnds.push_back(static_cast<std::uint32_t>(entries.size()));
        postingEnds.push_back(static_cast<std::uint32_t>(postings.size()));
    }
    void clear() noexcept
    {
        keys.clear();
        entries.clear();
        postings.clear();
        entryEnds.clear();
        postingEnds.clear();
        ok = true;
    }
};

// Writes a segment file as its postings come: only the table stays in memory.
// Nothing is left behind unless finish() succeeds.
class SegmentWriter {
public:
    // The contents are in [contentBegin, contentEnd); `prints` are theirs,
    // ascending. `listed`: the contents the table lists, by place (see
    // EncodedKeys::add); none: the table lists contents themselves.
    SegmentWriter(const QString& path, std::uint32_t contentBegin, std::uint32_t contentEnd, std::uint32_t level,
        const std::vector<Print>& prints, std::span<const std::uint32_t> listed = {})
        : m_file(path)
        , m_contentBegin(contentBegin)
        , m_contentEnd(contentEnd)
        , m_level(level)
        , m_printCount(prints.size())
        , m_listedCount(listed.size())
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        const SegmentHeader placeholder {};
        m_ok = m_file.open(QIODevice::WriteOnly) && put(&placeholder, sizeof placeholder)
            && put(prints.data(), prints.size() * sizeof(Print)) && put(listed.data(), listed.size_bytes());
    }

    // Keys ascending; contents ascending (others are dropped). For a segment
    // whose table lists contents themselves.
    void add(grams::Key key, std::span<const std::uint32_t> contents)
    {
        m_contents.clear();
        for (const std::uint32_t c : contents) {
            if (c >= m_contentBegin && c < m_contentEnd && (m_contents.empty() || c > m_contents.back()))
                m_contents.push_back(c);
        }
        m_one.clear();
        m_one.add(key, m_contents, m_contentBegin, m_contentEnd);
        add(m_one);
    }

    // Keys encoded for this segment's contents, after those added before.
    void add(const EncodedKeys& encoded)
    {
        for (std::size_t i = 0; i < encoded.keys.size(); ++i) {
            const grams::Key key = encoded.keys[i];
            const std::uint64_t at = offset();
            if (!m_ok || !encoded.ok || at > std::numeric_limits<std::uint32_t>::max()
                || m_table.size() > std::numeric_limits<std::uint32_t>::max() || (m_keys > 0 && key <= m_lastKey)) {
                m_ok = false;
                return;
            }
            if (m_keys % kBlockKeys == 0)
                m_blocks.push_back({key, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(m_table.size())});
            else
                encode(m_table, key - m_lastKey);
            const std::size_t entry = i > 0 ? encoded.entryEnds[i - 1] : 0;
            m_table.insert(m_table.end(), encoded.entries.begin() + static_cast<std::ptrdiff_t>(entry),
                encoded.entries.begin() + encoded.entryEnds[i]);
            const std::size_t posting = i > 0 ? encoded.postingEnds[i - 1] : 0;
            m_buffer.insert(m_buffer.end(), encoded.postings.begin() + static_cast<std::ptrdiff_t>(posting),
                encoded.postings.begin() + encoded.postingEnds[i]);
            m_lastKey = key;
            ++m_keys;
            if (m_buffer.size() >= kBufferBytes)
                drain();
        }
    }

    bool finish()
    {
        drain();
        const std::uint64_t postingBytes = m_written;
        if (!m_ok || postingBytes > std::numeric_limits<std::uint32_t>::max()
            || m_table.size() > std::numeric_limits<std::uint32_t>::max() || m_keys > std::numeric_limits<std::uint32_t>::max())
            return false;
        SegmentHeader header {};
        std::memcpy(header.magic, kSegmentMagic, sizeof header.magic);
        header.version = kSegmentVersion;
        header.keyCount = static_cast<std::uint32_t>(m_keys);
        header.contentBegin = m_contentBegin;
        header.contentEnd = m_contentEnd;
        header.level = m_level;
        header.printCount = static_cast<std::uint32_t>(m_printCount);
        header.listedCount = static_cast<std::uint32_t>(m_listedCount);
        header.postingBytes = postingBytes;
        header.tableBytes = m_table.size();
        const SegmentBlock sentinel {std::numeric_limits<grams::Key>::max(), static_cast<std::uint32_t>(postingBytes),
            static_cast<std::uint32_t>(m_table.size())};
        const std::uint64_t end
            = sizeof header + m_printCount * sizeof(Print) + m_listedCount * sizeof(std::uint32_t) + postingBytes;
        constexpr char zeros[8] = {};
        return put(zeros, static_cast<std::size_t>(aligned8(end) - end))
            && put(m_blocks.data(), m_blocks.size() * sizeof(SegmentBlock)) && put(&sentinel, sizeof sentinel)
            && put(m_table.data(), m_table.size()) && m_file.seek(0) && put(&header, sizeof header) && m_file.commit();
    }

private:
    static constexpr std::size_t kBufferBytes = 1 << 20;

    std::uint64_t offset() const noexcept { return m_written + m_buffer.size(); }
    bool put(const void* data, std::size_t size)
    {
        return m_file.write(static_cast<const char*>(data), static_cast<qint64>(size)) == static_cast<qint64>(size);
    }
    void drain()
    {
        m_ok = m_ok && put(m_buffer.data(), m_buffer.size());
        m_written += m_buffer.size();
        m_buffer.clear();
    }

    QSaveFile m_file;
    std::uint32_t m_contentBegin;
    std::uint32_t m_contentEnd;
    std::uint32_t m_level;
    std::size_t m_printCount;
    std::size_t m_listedCount;
    bool m_ok = false;
    std::uint64_t m_written = 0; // posting bytes in the file
    std::vector<std::uint8_t> m_buffer; // posting bytes still to write
    std::vector<std::uint32_t> m_contents; // scratch: a key's contents, checked
    EncodedKeys m_one; // scratch: that key
    std::vector<SegmentBlock> m_blocks;
    std::vector<std::uint8_t> m_table;
    std::uint64_t m_keys = 0;
    grams::Key m_lastKey = 0;
};

// Calls f(part, worker) for each part in [0, parts), on `threads` threads
// (this one among them), worker being which of them (0 to threads - 1).
template <typename F> void inParallel(int threads, std::size_t parts, F&& f)
{
    std::atomic<std::size_t> next {0};
    const auto work = [&](int worker) {
        for (std::size_t part = next++; part < parts; part = next++)
            f(part, worker);
    };
    std::vector<std::jthread> helpers;
    for (int w = 1; w < threads && static_cast<std::size_t>(w) < parts; ++w)
        helpers.emplace_back(work, w);
    work(0);
}

// Sorts pairs (key << kIdBits | content) by key alone, stably, through
// `scratch` (as large): a radix sort, a quarter of std::sort's time. Returns
// where they ended up, the one or the other.
std::span<std::uint64_t> sortByKey(std::span<std::uint64_t> pairs, std::span<std::uint64_t> scratch)
{
    constexpr unsigned kDigitBits = 12;
    constexpr unsigned kDigits = (64 - kIdBits) / kDigitBits;
    constexpr std::uint64_t kDigitMask = (std::uint64_t {1} << kDigitBits) - 1;
    static_assert((64 - kIdBits) % kDigitBits == 0);
    // Every digit counted in one go, then the pairs moved once per digit.
    std::vector<std::array<std::uint32_t, kDigitMask + 1>> count(kDigits);
    for (const std::uint64_t p : pairs) {
        for (unsigned d = 0; d < kDigits; ++d)
            ++count[d][(p >> (kIdBits + d * kDigitBits)) & kDigitMask];
    }
    for (unsigned d = 0; d < kDigits; ++d) {
        if (std::ranges::find(count[d], pairs.size()) != count[d].end())
            continue; // the same digit for all: nothing moves
        std::uint32_t at = 0;
        for (std::uint32_t& c : count[d])
            at += std::exchange(c, at);
        const unsigned shift = kIdBits + d * kDigitBits;
        for (const std::uint64_t p : pairs)
            scratch[count[d][(p >> shift) & kDigitMask]++] = p;
        std::swap(pairs, scratch);
    }
    return pairs;
}

// Pairs sorted by key, their contents ascending, into `out`. Memory's come
// content by content, each content's keys ascending: sorted by key alone,
// and stably, they are sorted. On several threads they are first cut by key
// into stretches (at keys from a sample), which are then sorted each on its
// own, in the cache. `ends` gets where the stretches end. The pairs are only
// read: searches read them meanwhile (flush). `out` and `scratch` keep their
// memory for the next time: fresh memory costs a page fault every 4 KB, more
// than sorting into it.
void sortPairs(std::span<const std::uint64_t> pairs, int threads, std::vector<std::uint64_t>& out,
    std::vector<std::uint64_t>& scratch, std::vector<std::size_t>* ends = nullptr)
{
    const std::size_t n = pairs.size();
    out.resize(n);
    scratch.resize(n);
    const auto byContent = [](std::uint64_t a, std::uint64_t b) { return (a & kIdMask) < (b & kIdMask); };
    if (n < 4096 || !std::is_sorted(pairs.begin(), pairs.end(), byContent)) {
        std::copy(pairs.begin(), pairs.end(), out.begin());
        std::sort(out.begin(), out.end());
        if (ends)
            *ends = {n};
        return;
    }
    threads = std::max(threads, 1);
    std::vector<grams::Key> splitters; // the first key of each stretch but the first
    if (threads > 1 && n >= (std::size_t {1} << 16)) {
        const std::size_t parts = std::min<std::size_t>(static_cast<std::size_t>(threads) * 4, 255);
        std::vector<grams::Key> sample;
        for (std::size_t i = 0; i < n; i += std::max<std::size_t>(n / (parts * 64), 1))
            sample.push_back(pairs[i] >> kIdBits);
        std::sort(sample.begin(), sample.end());
        for (std::size_t k = 1; k < parts; ++k) {
            const grams::Key key = sample[k * sample.size() / parts];
            if (key > 0 && (splitters.empty() || key > splitters.back()))
                splitters.push_back(key);
        }
    }
    const std::size_t stretches = splitters.size() + 1;
    std::vector<std::size_t> start(stretches + 1, 0); // of each stretch in `out`
    if (stretches == 1)
        std::copy(pairs.begin(), pairs.end(), out.begin());
    if (stretches > 1) {
        // Into the stretches, each share of the pairs after those before it:
        // stably.
        const auto shares = static_cast<std::size_t>(threads);
        const std::size_t each = (n + shares - 1) / shares;
        std::vector<std::uint8_t> stretchOf(n);
        std::vector<std::vector<std::size_t>> at(shares, std::vector<std::size_t>(stretches, 0));
        inParallel(threads, shares, [&](std::size_t share, int) {
            for (std::size_t i = share * each; i < std::min(n, (share + 1) * each); ++i) {
                const auto b = static_cast<std::size_t>(
                    std::upper_bound(splitters.begin(), splitters.end(), pairs[i] >> kIdBits) - splitters.begin());
                stretchOf[i] = static_cast<std::uint8_t>(b);
                ++at[share][b];
            }
        });
        std::size_t next = 0;
        for (std::size_t b = 0; b < stretches; ++b) {
            start[b] = next;
            for (std::size_t share = 0; share < shares; ++share)
                next += std::exchange(at[share][b], next);
        }
        inParallel(threads, shares, [&](std::size_t share, int) {
            for (std::size_t i = share * each; i < std::min(n, (share + 1) * each); ++i)
                out[at[share][stretchOf[i]]++] = pairs[i];
        });
    }
    start[stretches] = n;
    inParallel(threads, stretches, [&](std::size_t b, int) {
        const std::span<std::uint64_t> stretch(out.data() + start[b], start[b + 1] - start[b]);
        const std::span<std::uint64_t> sorted
            = sortByKey(stretch, std::span(scratch.data() + start[b], stretch.size()));
        if (sorted.data() != stretch.data())
            std::copy(sorted.begin(), sorted.end(), stretch.begin());
    });
    if (ends)
        ends->assign(start.begin() + 1, start.end());
}

// What orders contents for a merge (see merge): splitmix64's finalizer.
std::uint64_t scramble(std::uint64_t x) noexcept
{
    x ^= x >> 30;
    x *= 0xBF58'476D'1CE4'E5B9ull;
    x ^= x >> 27;
    x *= 0x94D0'49BB'1331'11EBull;
    x ^= x >> 31;
    return x;
}

// Orders documents so that those with terms in common come together, which
// is what makes postings small (see merge). Recursive graph bisection
// (Dhulipala et al., "Compressing graphs and indexes with recursive graph
// bisection", KDD 2016): the documents are cut in two halves, and swapped
// between them while that brings each term's documents together, going by
// how many bits the gaps between them would take; then each half is cut the
// same way. Document d's terms are terms[termStart[d], termStart[d + 1]),
// below termCount. The order is the same on any number of threads.
class Bisection {
public:
    Bisection(std::span<const std::uint64_t> termStart, std::span<const std::uint32_t> terms, std::uint32_t termCount)
        : m_termStart(termStart)
        , m_terms(terms)
        , m_termCount(termCount)
        , m_log2(termStart.size() + 1)
    {
        for (std::size_t i = 1; i < m_log2.size(); ++i)
            m_log2[i] = static_cast<float>(std::log2(static_cast<double>(i)));
    }

    // `docs` (below termStart.size() - 1) in a first order; leaves in the new one.
    void order(std::span<std::uint32_t> docs, int threads) const
    {
        // Down to a few dozen documents: below that, swaps hardly change
        // the sizes (as in the paper).
        const int depth = std::max(1, static_cast<int>(std::bit_width(docs.size())) - 6);
        Scratch scratch;
        cut(docs, depth, std::max(threads, 1), scratch);
    }

private:
    static constexpr int kRounds = 20; // of swaps per cut, at most
    static constexpr std::size_t kParallel = 8192; // documents in a cut worth more than one thread

    struct Scratch {
        std::vector<std::int32_t> left, right; // by term: its documents in each half
        std::vector<float> toRight, toLeft; // by term: what moving one of them over saves
        std::vector<std::uint32_t> touched; // the terms of the documents being cut
        std::vector<std::pair<float, std::uint32_t>> gainsLeft, gainsRight; // by document
    };

    std::span<const std::uint32_t> termsOf(std::uint32_t doc) const noexcept
    {
        return m_terms.subspan(m_termStart[doc], m_termStart[doc + 1] - m_termStart[doc]);
    }

    void cut(std::span<std::uint32_t> docs, int depth, int threads, Scratch& s) const
    {
        if (depth == 0 || docs.size() < 2)
            return;
        if (s.left.empty()) {
            s.left.assign(m_termCount, 0);
            s.right.assign(m_termCount, 0);
            s.toRight.assign(m_termCount, 0);
            s.toLeft.assign(m_termCount, 0);
        }
        const std::size_t half = docs.size() / 2;
        s.touched.clear();
        for (std::size_t i = 0; i < docs.size(); ++i) {
            std::vector<std::int32_t>& side = i < half ? s.left : s.right;
            for (const std::uint32_t t : termsOf(docs[i])) {
                if (s.left[t] == 0 && s.right[t] == 0)
                    s.touched.push_back(t);
                ++side[t];
            }
        }
        // The bits a term's gaps take, about, with a documents on the left
        // and b on the right: its documents spread evenly over each half.
        const float logLeft = m_log2[half];
        const float logRight = m_log2[docs.size() - half];
        const auto bits = [&](std::int32_t a, std::int32_t b) {
            return static_cast<float>(a) * (logLeft - m_log2[static_cast<std::size_t>(a) + 1])
                + static_cast<float>(b) * (logRight - m_log2[static_cast<std::size_t>(b) + 1]);
        };
        s.gainsLeft.resize(half);
        s.gainsRight.resize(docs.size() - half);
        const auto gains = [&](std::size_t from, std::size_t to) {
            for (std::size_t i = from; i < to; ++i) {
                const std::vector<float>& saves = i < half ? s.toRight : s.toLeft;
                float gain = 0;
                for (const std::uint32_t t : termsOf(docs[i]))
                    gain += saves[t];
                (i < half ? s.gainsLeft[i] : s.gainsRight[i - half]) = {gain, docs[i]};
            }
        };
        const auto byGain = [](const std::pair<float, std::uint32_t>& a, const std::pair<float, std::uint32_t>& b) {
            return a.first > b.first || (a.first == b.first && a.second < b.second);
        };
        for (int round = 0; round < kRounds; ++round) {
            for (const std::uint32_t t : s.touched) {
                const std::int32_t a = s.left[t];
                const std::int32_t b = s.right[t];
                const float now = bits(a, b);
                s.toRight[t] = a > 0 ? now - bits(a - 1, b + 1) : 0;
                s.toLeft[t] = b > 0 ? now - bits(a + 1, b - 1) : 0;
            }
            if (threads > 1 && docs.size() >= kParallel) {
                const std::size_t parts = static_cast<std::size_t>(threads) * 4;
                const std::size_t each = (docs.size() + parts - 1) / parts;
                inParallel(threads, parts,
                    [&](std::size_t part, int) { gains(part * each, std::min(docs.size(), (part + 1) * each)); });
            } else {
                gains(0, docs.size());
            }
            std::sort(s.gainsLeft.begin(), s.gainsLeft.end(), byGain);
            std::sort(s.gainsRight.begin(), s.gainsRight.end(), byGain);
            std::size_t swaps = 0;
            while (swaps < s.gainsLeft.size() && swaps < s.gainsRight.size()
                && s.gainsLeft[swaps].first + s.gainsRight[swaps].first > 0)
                ++swaps;
            if (swaps == 0)
                break;
            for (std::size_t i = 0; i < swaps; ++i) {
                for (const std::uint32_t t : termsOf(s.gainsLeft[i].second)) {
                    --s.left[t];
                    ++s.right[t];
                }
                for (const std::uint32_t t : termsOf(s.gainsRight[i].second)) {
                    ++s.left[t];
                    --s.right[t];
                }
            }
            std::size_t at = 0;
            for (std::size_t i = swaps; i < s.gainsLeft.size(); ++i)
                docs[at++] = s.gainsLeft[i].second;
            for (std::size_t i = 0; i < swaps; ++i)
                docs[at++] = s.gainsRight[i].second;
            for (std::size_t i = swaps; i < s.gainsRight.size(); ++i)
                docs[at++] = s.gainsRight[i].second;
            for (std::size_t i = 0; i < swaps; ++i)
                docs[at++] = s.gainsLeft[i].second;
        }
        for (const std::uint32_t t : s.touched)
            s.left[t] = s.right[t] = 0;
        const std::span<std::uint32_t> first = docs.first(half);
        const std::span<std::uint32_t> second = docs.subspan(half);
        if (threads > 1) {
            std::jthread other([&, first] {
                Scratch own;
                cut(first, depth - 1, threads / 2, own);
            });
            cut(second, depth - 1, threads - threads / 2, s);
        } else {
            cut(first, depth - 1, 1, s);
            cut(second, depth - 1, 1, s);
        }
    }

    std::span<const std::uint64_t> m_termStart;
    std::span<const std::uint32_t> m_terms;
    std::uint32_t m_termCount;
    std::vector<float> m_log2; // of 0 to the number of documents
};

// The code points of a string; unpaired surrogates as U+FFFD.
template <typename F> void forEachCodePoint(QStringView s, F&& f)
{
    for (qsizetype i = 0; i < s.size(); ++i) {
        const char16_t u = s[i].unicode();
        if (QChar::isHighSurrogate(u) && i + 1 < s.size() && QChar::isLowSurrogate(s[i + 1].unicode())) {
            f(static_cast<char32_t>(QChar::surrogateToUcs4(u, s[i + 1].unicode())));
            ++i;
        } else {
            f(QChar::isSurrogate(u) ? char32_t {0xFFFD} : char32_t {u});
        }
    }
}

// Up to 8 bytes, ASCII-folded and packed: a cheap key for an extension.
bool packExtension(std::string_view s, std::uint64_t& out) noexcept
{
    if (s.size() > 8)
        return false;
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < s.size(); ++i)
        v |= std::uint64_t {text::fold(s[i])} << (8 * i);
    out = v;
    return true;
}

} // namespace

// ---- grams ----------------------------------------------------------------------------

namespace grams {

namespace {

constexpr Key single(char32_t c) noexcept
{
    return Key {c} << kCharBits;
}

constexpr Key pair(char32_t a, char32_t b) noexcept
{
    return (Key {a} << kCharBits) | b;
}

constexpr unsigned kAscii = Collector::kAsciiChars;
constexpr unsigned kTrigrams = kAscii * kAscii * kAscii;

// The characters of the classes, by Collector::asciiClass - 1: ' ' to '@',
// then '[' to '~' (capital letters fold to small ones). Every punctuation
// mark is a class of its own: with them all as one, a line of code leaves
// ten times as many files to read (wsbench --content-index), for an index
// a fifth smaller.
constexpr std::array<char, kAscii> kAsciiText = [] {
    std::array<char, kAscii> text {};
    std::size_t n = 0;
    for (char c = ' '; c <= '~'; ++c) {
        if (c < 'A' || c > 'Z')
            text[n++] = c;
    }
    return text;
}();

constexpr std::array<std::uint8_t, 128> kAsciiClasses = [] {
    std::array<std::uint8_t, 128> classes {};
    for (unsigned i = 0; i < kAscii; ++i)
        classes[static_cast<unsigned char>(kAsciiText[i])] = static_cast<std::uint8_t>(i + 1);
    for (char c = 'A'; c <= 'Z'; ++c)
        classes[static_cast<unsigned char>(c)] = classes[static_cast<unsigned char>(c - 'A' + 'a')];
    for (const char c : {'\t', '\n', '\v', '\f', '\r'})
        classes[static_cast<unsigned char>(c)] = Collector::kSpace;
    return classes;
}();
static_assert(kAsciiText[Collector::kSpace - 1] == ' ' && kAsciiText.back() == '~');

// Three classes (1..kAsciiChars) in a row.
constexpr Key trigram(unsigned a, unsigned b, unsigned c) noexcept
{
    return (Key {static_cast<unsigned char>(kAsciiText[a - 1])} << kCharBits)
        | (Key {static_cast<unsigned char>(kAsciiText[b - 1])} << 8) | static_cast<unsigned char>(kAsciiText[c - 1]);
}

} // namespace

unsigned Collector::asciiClass(char32_t c) noexcept
{
    return c < 128 ? kAsciiClasses[c] : 0;
}

bool isIndexed(char32_t c) noexcept
{
    return (c >= 0x3040 && c <= 0x9FFF) // kana, bopomofo, CJK ideographs with extension A
        || (c >= 0xAC00 && c <= 0xD7AF) // Hangul syllables
        || (c >= 0xF900 && c <= 0xFAFF) // CJK compatibility ideographs
        || (c >= 0x20000 && c <= 0x323AF); // CJK extensions B to H
}

std::vector<Key> ofPhrase(QStringView phrase)
{
    std::vector<Key> keys;
    std::vector<char32_t> run; // CJK characters in a row
    std::vector<unsigned> ascii; // ASCII characters in a row, as Collector::asciiClass, whitespace runs as one
    const auto endRun = [&] {
        if (run.size() == 1)
            keys.push_back(single(run[0]));
        for (std::size_t i = 1; i < run.size(); ++i)
            keys.push_back(pair(run[i - 1], run[i]));
        run.clear();
    };
    const auto endAscii = [&] {
        for (std::size_t i = 2; i < ascii.size(); ++i)
            keys.push_back(trigram(ascii[i - 2], ascii[i - 1], ascii[i]));
        ascii.clear();
    };
    forEachCodePoint(phrase, [&](char32_t c) {
        if (isIndexed(c))
            run.push_back(c);
        else
            endRun();
        const unsigned a = Collector::asciiClass(c);
        if (a == 0)
            endAscii();
        else if (a != Collector::kSpace || ascii.empty() || ascii.back() != Collector::kSpace)
            ascii.push_back(a);
    });
    endRun();
    endAscii();
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

bool decides(QStringView phrase)
{
    std::size_t count = 0;
    bool indexed = true;
    forEachCodePoint(phrase, [&](char32_t c) {
        ++count;
        indexed = indexed && isIndexed(c);
    });
    return indexed && (count == 1 || count == 2);
}

std::optional<Fingerprint> fingerprint(std::span<const Key> keys)
{
    return printOf(std::as_bytes(keys));
}

Collector::Collector(TextEncoding encoding, unsigned ansiCodePage)
    : m_encoding(encoding)
    , m_codePage(ansiCodePage)
    , m_trigrams((kTrigrams + 63) / 64, 0)
{
    if (encoding == TextEncoding::Ansi) {
        CPINFO info {};
        m_doubleByte = ::GetCPInfo(ansiCodePage, &info) && info.MaxCharSize > 1;
    }
}

void Collector::feed(const char* data, std::size_t size)
{
    switch (m_encoding) {
    case TextEncoding::Utf8:
        feedUtf8(data, size);
        break;
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE:
        feedUtf16(data, size);
        break;
    case TextEncoding::Ansi:
        if (m_doubleByte) {
            feedAnsi(data, size);
        } else {
            // A single-byte code page: ASCII below 0x80, no CJK characters.
            const auto* b = reinterpret_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < size;) {
                if (b[i] < 0x80) {
                    i += asciiRun(b + i, size - i);
                } else {
                    endRun();
                    ++i;
                }
            }
        }
        break;
    }
}

// Most of what is read is ASCII, and most of the time taking grams out of a
// file goes here. The state stays in locals: in members, it went to and from
// memory around every store to m_trigrams (the compiler cannot tell them
// apart), an eighth of the time (wsbench --content-index --grams-only).
std::size_t Collector::asciiRun(const unsigned char* b, std::size_t n) noexcept
{
    std::uint64_t* const trigrams = m_trigrams.data();
    unsigned char1 = m_char1;
    unsigned char2 = m_char2;
    std::size_t i = 0;
    for (; i < n; ++i) {
        const unsigned char c = b[i];
        if (c >= 0x80)
            break;
        const unsigned w = kAsciiClasses[c];
        if (w == 0) {
            char1 = char2 = 0; // a control character
            continue;
        }
        if (w == kSpace && char1 == kSpace)
            continue; // a run of whitespace is one space
        if (char2 != 0) {
            const unsigned t = ((char2 - 1) * kAsciiChars + (char1 - 1)) * kAsciiChars + (w - 1);
            trigrams[t >> 6] |= std::uint64_t {1} << (t & 63);
        }
        char2 = char1;
        char1 = w;
    }
    m_char1 = char1;
    m_char2 = char2;
    if (i > 0)
        m_previous = 0;
    return i;
}

// A decoder that keeps its state across chunks: m_carry holds the lead byte
// and the continuation bytes seen so far. Malformed bytes count as one
// non-indexed character each, which is where QString::fromUtf8 (what the
// scanner decodes with) puts its U+FFFD too.
void Collector::feedUtf8(const char* p, std::size_t n)
{
    const auto* b = reinterpret_cast<const unsigned char*>(p);
    const auto lengthOf = [](unsigned char lead) -> std::size_t { return lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4; };
    // A whole character, or not one (an overlong form, a surrogate).
    const auto character = [&](char32_t cp, std::size_t length) {
        const char32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            m_previous = 0;
            endRun();
        } else {
            add(cp);
        }
    };
    std::size_t i = 0;
    while (i < n) {
        if (m_carry.empty()) {
            if (b[i] < 0x80) {
                i += asciiRun(b + i, n - i);
                continue;
            }
            const unsigned char c = b[i];
            if (c < 0xC2 || c > 0xF4) {
                ++i;
                m_previous = 0; // a byte that cannot start a character
                endRun();
                continue;
            }
            const std::size_t length = lengthOf(c);
            if (i + length > n) {
                ++i;
                m_carry.push_back(static_cast<char>(c)); // the rest in the next chunk
                continue;
            }
            // The whole character is here, nearly always: decoded in place.
            char32_t cp = c & (length == 2 ? 0x1F : length == 3 ? 0x0F : 0x07);
            std::size_t k = 1;
            for (; k < length && (b[i + k] & 0xC0) == 0x80; ++k)
                cp = (cp << 6) | (b[i + k] & 0x3F);
            i += k;
            if (k < length) { // cut short: the byte at i starts something new
                m_previous = 0;
                endRun();
                continue;
            }
            character(cp, length);
            continue;
        }
        const unsigned char c = b[i];
        if ((c & 0xC0) != 0x80) {
            m_carry.clear(); // cut short: the byte starts something new
            m_previous = 0;
            endRun();
            continue;
        }
        ++i;
        m_carry.push_back(static_cast<char>(c));
        const auto lead = static_cast<unsigned char>(m_carry[0]);
        const std::size_t length = lengthOf(lead);
        if (m_carry.size() < length)
            continue;
        char32_t cp = lead & (length == 2 ? 0x1F : length == 3 ? 0x0F : 0x07);
        for (std::size_t k = 1; k < length; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(m_carry[k]) & 0x3F);
        m_carry.clear();
        character(cp, length);
    }
}

void Collector::feedUtf16(const char* p, std::size_t n)
{
    const bool big = m_encoding == TextEncoding::Utf16BE;
    const auto unitOf = [big](unsigned char x, unsigned char y) {
        return big ? static_cast<char16_t>((x << 8) | y) : static_cast<char16_t>((y << 8) | x);
    };
    std::size_t i = 0;
    if (!m_carry.empty() && n > 0) {
        unit(unitOf(static_cast<unsigned char>(m_carry[0]), static_cast<unsigned char>(p[0])));
        m_carry.clear();
        i = 1;
    }
    for (; i + 1 < n; i += 2)
        unit(unitOf(static_cast<unsigned char>(p[i]), static_cast<unsigned char>(p[i + 1])));
    if (i < n)
        m_carry.assign(1, p[i]);
}

void Collector::feedAnsi(const char* p, std::size_t n)
{
    // Whole characters only: a lead byte at the end waits for the next chunk.
    std::string joined;
    if (!m_carry.empty()) {
        joined = std::exchange(m_carry, {});
        joined.append(p, n);
        p = joined.data();
        n = joined.size();
    }
    std::size_t end = 0;
    while (end < n) {
        if (::IsDBCSLeadByteEx(m_codePage, static_cast<BYTE>(p[end]))) {
            if (end + 1 >= n)
                break;
            end += 2;
        } else {
            ++end;
        }
    }
    m_carry.assign(p + end, n - end);
    if (end == 0)
        return;
    const int wide = ::MultiByteToWideChar(m_codePage, 0, p, static_cast<int>(end), nullptr, 0);
    if (wide <= 0)
        return;
    m_wide.resize(static_cast<std::size_t>(wide));
    ::MultiByteToWideChar(m_codePage, 0, p, static_cast<int>(end), reinterpret_cast<wchar_t*>(m_wide.data()), wide);
    for (const char16_t u : m_wide)
        unit(u);
}

void Collector::unit(char16_t u) noexcept
{
    if (m_highSurrogate) {
        const char16_t high = std::exchange(m_highSurrogate, char16_t {0});
        if (QChar::isLowSurrogate(u)) {
            add(QChar::surrogateToUcs4(high, u));
            return;
        }
        m_previous = 0; // unpaired
    }
    if (u < 0x80) {
        ascii(static_cast<unsigned char>(u));
    } else if (QChar::isHighSurrogate(u)) {
        m_highSurrogate = u;
        endRun();
    } else if (QChar::isLowSurrogate(u)) {
        m_previous = 0;
        endRun();
    } else {
        add(u);
    }
}

// A character that is not ASCII.
void Collector::add(char32_t c)
{
    endRun();
    if (!isIndexed(c)) {
        m_previous = 0;
        return;
    }
    m_keys.push_back(single(c));
    if (m_previous)
        m_keys.push_back(pair(m_previous, c));
    m_previous = c;
    if (m_keys.size() >= m_compactAt) {
        std::sort(m_keys.begin(), m_keys.end());
        m_keys.erase(std::unique(m_keys.begin(), m_keys.end()), m_keys.end());
        m_compactAt = std::max<std::size_t>(m_keys.size() * 2, 1 << 16);
    }
}

std::vector<Key> Collector::finish()
{
    std::sort(m_keys.begin(), m_keys.end());
    m_keys.erase(std::unique(m_keys.begin(), m_keys.end()), m_keys.end());
    std::size_t count = m_keys.size();
    for (const std::uint64_t bits : m_trigrams)
        count += static_cast<std::size_t>(std::popcount(bits));
    // Trigrams first: their keys are below every CJK key.
    std::vector<Key> keys;
    keys.reserve(count);
    for (std::size_t w = 0; w < m_trigrams.size(); ++w) {
        for (std::uint64_t bits = m_trigrams[w]; bits != 0; bits &= bits - 1) {
            const auto t = static_cast<unsigned>(w * 64 + static_cast<unsigned>(std::countr_zero(bits)));
            keys.push_back(trigram(t / (kAscii * kAscii) + 1, t / kAscii % kAscii + 1, t % kAscii + 1));
        }
    }
    keys.insert(keys.end(), m_keys.begin(), m_keys.end());
    m_keys = {};
    return keys;
}

} // namespace grams

// ---- ContentSizeLimits ----------------------------------------------------------------

namespace {

// Sorted, for binary search.
constexpr std::string_view kCodeExtensions[] {"asm", "bash", "bat", "c", "c++", "cc", "cjs", "clj", "cljs", "cmake",
    "cmd", "cpp", "cs", "css", "cts", "cxx", "dart", "elm", "erl", "ex", "exs", "fish", "fs", "go", "gradle", "groovy",
    "h", "h++", "hh", "hpp", "hrl", "hs", "hxx", "inl", "ipp", "java", "jl", "js", "jsx", "kt", "kts", "less", "lua",
    "m", "mjs", "mm", "mts", "nim", "php", "pl", "pm", "ps1", "psm1", "py", "pyi", "pyw", "qml", "r", "rb", "rs", "s",
    "sass", "scala", "scss", "sh", "sv", "svelte", "swift", "tpp", "ts", "tsx", "v", "vb", "vhd", "vhdl", "vue", "zig",
    "zsh"};
constexpr std::string_view kDataExtensions[] {"htm", "html", "json", "jsonl", "ndjson", "plist", "resx", "sql", "svg",
    "wsdl", "xaml", "xhtml", "xml", "xsd"};
static_assert(std::ranges::is_sorted(kCodeExtensions) && std::ranges::is_sorted(kDataExtensions));

} // namespace

ContentSizeLimits::Kind ContentSizeLimits::kindOf(std::string_view extension) noexcept
{
    if (extension.empty() || extension.size() > 8)
        return Text;
    std::array<char, 8> folded {};
    for (std::size_t i = 0; i < extension.size(); ++i)
        folded[i] = static_cast<char>(text::fold(extension[i]));
    const std::string_view ext(folded.data(), extension.size());
    if (std::ranges::binary_search(kCodeExtensions, ext))
        return Code;
    if (std::ranges::binary_search(kDataExtensions, ext))
        return Data;
    if (isDocumentExtension(ext))
        return Document;
    return Text;
}

std::int64_t ContentSizeLimits::of(std::wstring_view path) const noexcept
{
    const std::size_t dot = path.find_last_of(L".\\/");
    std::array<char, 8> ext {};
    std::size_t n = 0;
    if (dot != std::wstring_view::npos && path[dot] == L'.') {
        for (std::size_t i = dot + 1; i < path.size() && n <= ext.size(); ++i, ++n) {
            if (n == ext.size() || path[i] >= 0x80)
                return bytes[Text]; // no extension of a kind listed
            ext[n] = static_cast<char>(path[i]);
        }
    }
    return bytes[kindOf({ext.data(), n})];
}

// ---- ExtensionFilter ------------------------------------------------------------------

ExtensionFilter::ExtensionFilter(const QStringList& extensions)
{
    for (QString ext : extensions) {
        ext = ext.trimmed();
        while (ext.startsWith(u'.') || ext.startsWith(u'*'))
            ext.remove(0, 1);
        if (ext.isEmpty())
            continue;
        std::string folded = text::foldAscii(wtf8::fromUtf16(wtf8::view(ext)));
        std::uint64_t packed = 0;
        if (packExtension(folded, packed))
            m_short.push_back(packed);
        else
            m_long.push_back(std::move(folded));
    }
    std::sort(m_short.begin(), m_short.end());
    m_short.erase(std::unique(m_short.begin(), m_short.end()), m_short.end());
}

bool ExtensionFilter::matches(std::string_view name, std::size_t extLength) const noexcept
{
    if (extLength == 0 || extLength > name.size())
        return false;
    const std::string_view ext = name.substr(name.size() - extLength);
    std::uint64_t packed = 0;
    if (packExtension(ext, packed))
        return std::binary_search(m_short.begin(), m_short.end(), packed);
    return std::any_of(m_long.begin(), m_long.end(), [&](const std::string& x) { return text::equalsFolded(ext, x); });
}

// ---- ContentIndex::Segment ----------------------------------------------------------------

class ContentIndex::Segment {
public:
    static std::shared_ptr<const Segment> open(const QString& path, std::uint64_t number);
    ~Segment()
    {
        if (m_view)
            ::UnmapViewOfFile(m_view);
    }
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    std::uint64_t number() const noexcept { return m_number; }
    std::uint64_t bytes() const noexcept { return m_size; }
    std::uint64_t grams() const noexcept { return m_header.keyCount; }
    std::uint64_t postingBytes() const noexcept { return m_header.postingBytes; }
    ContentId contentBegin() const noexcept { return m_header.contentBegin; }
    ContentId contentEnd() const noexcept { return m_header.contentEnd; }
    std::uint32_t level() const noexcept { return m_header.level; }
    std::span<const Print> prints() const noexcept { return {m_prints, m_header.printCount}; }
    ContentId contentOf(const grams::Fingerprint& print) const noexcept; // kNoContent if it has none such

    // A key's table entry.
    struct Entry {
        grams::Key key = 0;
        std::uint64_t count = 0; // of the contents that have it
        const std::uint8_t* listed = nullptr; // where the table lists them (up to kInline)
        std::uint64_t offset = 0; // otherwise: of its posting
        std::uint64_t size = 0;
    };
    bool find(grams::Key key, Entry& out) const noexcept;
    void contents(const Entry& e, std::vector<ContentId>& out) const; // appends them, ascending

    // What merging its keys takes, where: for every few blocks, their first
    // key and how many contents their keys have, all told (each is decoded,
    // renumbered and encoded), and a few more for each key. Not their
    // bytes: a key most contents have takes next to none.
    void work(std::vector<std::pair<grams::Key, std::uint64_t>>& out) const
    {
        constexpr std::uint64_t kBlocksEach = 8;
        const std::uint64_t blocks = blocksFor(m_header.keyCount);
        for (std::uint64_t b = 0; b < blocks; ++b) {
            if (b % kBlocksEach == 0)
                out.emplace_back(m_blocks[b].key, 0);
            NumberReader r(m_table + m_blocks[b].entry, m_table + m_blocks[b + 1].entry);
            Entry e;
            std::uint64_t posting = 0;
            for (bool first = true; !r.atEnd(); first = false) {
                if (!first)
                    r.next(); // the key
                readEntry(r, e, posting);
                out.back().second += e.count + 8;
            }
        }
    }

    // Every key in order, from the first at or after `from` (for merging).
    class Cursor {
    public:
        explicit Cursor(const Segment& s, grams::Key from = 0) noexcept
            : m_s(&s)
            , m_reader(s.m_table, s.m_table)
        {
            const SegmentBlock* const end = s.m_blocks + blocksFor(s.m_header.keyCount);
            const SegmentBlock* block = std::upper_bound(
                s.m_blocks, end, from, [](grams::Key k, const SegmentBlock& b) { return k < b.key; });
            if (block != s.m_blocks)
                --block;
            m_index = static_cast<std::uint32_t>(block - s.m_blocks) * kBlockKeys;
            next();
            while (!m_done && m_entry.key < from)
                next();
        }
        bool done() const noexcept { return m_done; }
        grams::Key key() const noexcept { return m_entry.key; }
        void contents(std::vector<ContentId>& out) const { m_s->contents(m_entry, out); }
        void next() noexcept
        {
            if (m_index >= m_s->m_header.keyCount) {
                m_done = true;
                return;
            }
            if (m_index % kBlockKeys == 0) {
                const SegmentBlock* const block = m_s->m_blocks + m_index / kBlockKeys;
                m_reader = NumberReader(m_s->m_table + block->entry, m_s->m_table + block[1].entry);
                m_entry.key = block->key;
                m_posting = block->posting;
            } else {
                m_entry.key += m_reader.next();
            }
            readEntry(m_reader, m_entry, m_posting);
            ++m_index;
        }

    private:
        const Segment* m_s;
        NumberReader m_reader;
        std::uint32_t m_index = 0; // of the next entry
        std::uint64_t m_posting = 0; // offset of the next posting
        Entry m_entry;
        bool m_done = false;
    };

private:
    explicit Segment(std::uint64_t number)
        : m_number(number)
    {
    }
    bool validate() const noexcept;
    // What follows a key in the table; `posting` is where its posting would
    // start, and moves past it.
    static void readEntry(NumberReader& r, Entry& e, std::uint64_t& posting) noexcept;

    std::uint64_t m_number;
    win32::UniqueHandle m_file;
    win32::UniqueHandle m_mapping;
    void* m_view = nullptr;
    std::uint64_t m_size = 0;
    SegmentHeader m_header {};
    const Print* m_prints = nullptr;
    const ContentId* m_listed = nullptr; // null when the table lists contents themselves
    const std::uint8_t* m_postings = nullptr;
    const SegmentBlock* m_blocks = nullptr;
    const std::uint8_t* m_table = nullptr;
};

std::shared_ptr<const ContentIndex::Segment> ContentIndex::Segment::open(const QString& path, std::uint64_t number)
{
    std::shared_ptr<Segment> s(new Segment(number));
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    s->m_file.reset(::CreateFileW(win32::longPath(native).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size {};
    if (!s->m_file.valid() || !::GetFileSizeEx(s->m_file.get(), &size)
        || size.QuadPart < static_cast<LONGLONG>(sizeof(SegmentHeader)))
        return nullptr;
    s->m_mapping.reset(::CreateFileMappingW(s->m_file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!s->m_mapping.valid())
        return nullptr;
    s->m_view = ::MapViewOfFile(s->m_mapping.get(), FILE_MAP_READ, 0, 0, 0);
    if (!s->m_view)
        return nullptr;
    s->m_size = static_cast<std::uint64_t>(size.QuadPart);
    const auto* bytes = static_cast<const std::uint8_t*>(s->m_view);
    std::memcpy(&s->m_header, bytes, sizeof(SegmentHeader));
    const SegmentHeader& h = s->m_header;
    if (std::memcmp(h.magic, kSegmentMagic, sizeof h.magic) != 0 || h.version != kSegmentVersion)
        return nullptr;
    if (h.contentBegin > h.contentEnd || h.contentEnd > kMaxIds || h.printCount > h.contentEnd - h.contentBegin
        || h.listedCount > h.contentEnd - h.contentBegin || h.postingBytes > std::numeric_limits<std::uint32_t>::max()
        || h.tableBytes > std::numeric_limits<std::uint32_t>::max())
        return nullptr;
    const std::uint64_t listedAt = sizeof(SegmentHeader) + std::uint64_t {h.printCount} * sizeof(Print);
    const std::uint64_t postingsAt = listedAt + std::uint64_t {h.listedCount} * sizeof(ContentId);
    const std::uint64_t blocksAt = aligned8(postingsAt + h.postingBytes);
    const std::uint64_t tableAt = blocksAt + (blocksFor(h.keyCount) + 1) * sizeof(SegmentBlock);
    if (tableAt + h.tableBytes != s->m_size)
        return nullptr;
    s->m_prints = reinterpret_cast<const Print*>(bytes + sizeof(SegmentHeader));
    s->m_listed = h.listedCount > 0 ? reinterpret_cast<const ContentId*>(bytes + listedAt) : nullptr;
    s->m_postings = bytes + postingsAt;
    s->m_blocks = reinterpret_cast<const SegmentBlock*>(bytes + blocksAt);
    s->m_table = bytes + tableAt;
    if (!s->validate())
        return nullptr;
    return s;
}

// Every lookup stays inside the file, whatever it holds: the table is read
// through once here; postings decode to numbers in range, whatever their bits.
bool ContentIndex::Segment::validate() const noexcept
{
    const SegmentHeader& h = m_header;
    for (std::uint32_t j = 0; j < h.printCount; ++j) {
        const Print& p = m_prints[j];
        if (p.content < h.contentBegin || p.content >= h.contentEnd
            || (j > 0 && !(m_prints[j - 1].fingerprint() < p.fingerprint())))
            return false;
    }
    const std::uint64_t universe = h.contentEnd - h.contentBegin;
    if (m_listed) { // contents of the segment, each once
        std::vector<bool> seen(universe, false);
        for (std::uint32_t j = 0; j < h.listedCount; ++j) {
            const ContentId c = m_listed[j];
            if (c < h.contentBegin || c >= h.contentEnd || seen[c - h.contentBegin])
                return false;
            seen[c - h.contentBegin] = true;
        }
    }
    const std::uint64_t places = m_listed ? h.listedCount : universe; // what the table's lists may name
    const auto blockCount = static_cast<std::uint32_t>(blocksFor(h.keyCount));
    if (m_blocks[0].entry != 0 || m_blocks[blockCount].entry != h.tableBytes
        || m_blocks[blockCount].posting != h.postingBytes)
        return false;
    std::uint64_t offset = 0;
    for (std::uint32_t b = 0; b < blockCount; ++b) {
        const SegmentBlock* const block = m_blocks + b;
        if (block->posting != offset || block->entry > block[1].entry)
            return false;
        NumberReader r(m_table + block->entry, m_table + block[1].entry);
        grams::Key key = block->key;
        const std::uint32_t n = std::min(kBlockKeys, h.keyCount - b * kBlockKeys);
        for (std::uint32_t i = 0; i < n; ++i) {
            if (i > 0) {
                const std::uint64_t step = r.next();
                if (step == 0 || step > std::numeric_limits<grams::Key>::max() - key)
                    return false;
                key += step;
            }
            const std::uint64_t count = r.next();
            if (!r.ok() || count == 0 || count > universe)
                return false;
            if (count <= kInline) {
                std::uint64_t next = 0; // the first place the next number can stand for
                for (std::uint64_t k = 0; k < count; ++k) {
                    const std::uint64_t skipped = r.next();
                    if (!r.ok() || skipped >= places - next)
                        return false;
                    next += skipped + 1;
                }
            } else {
                const std::uint64_t size = r.next();
                if (!r.ok() || size > h.postingBytes - offset)
                    return false;
                offset += size;
            }
        }
        if (!r.atEnd() || (b + 1 < blockCount && block[1].key <= key))
            return false;
    }
    return offset == h.postingBytes;
}

void ContentIndex::Segment::readEntry(NumberReader& r, Entry& e, std::uint64_t& posting) noexcept
{
    e.count = r.next();
    e.listed = nullptr;
    e.offset = posting;
    e.size = 0;
    if (e.count <= kInline) {
        e.listed = r.position();
        for (std::uint64_t i = 0; i < e.count; ++i)
            r.next();
    } else {
        e.size = r.next();
        posting += e.size;
    }
}

ContentIndex::ContentId ContentIndex::Segment::contentOf(const grams::Fingerprint& print) const noexcept
{
    const std::span<const Print> all = prints();
    const auto it = std::lower_bound(all.begin(), all.end(), print,
        [](const Print& p, const grams::Fingerprint& f) { return p.fingerprint() < f; });
    return it != all.end() && it->fingerprint() == print ? it->content : kNoContent;
}

bool ContentIndex::Segment::find(grams::Key key, Entry& out) const noexcept
{
    const SegmentBlock* const end = m_blocks + blocksFor(m_header.keyCount);
    const SegmentBlock* block = std::upper_bound(
        m_blocks, end, key, [](grams::Key k, const SegmentBlock& b) { return k < b.key; });
    if (block == m_blocks)
        return false;
    --block;
    NumberReader r(m_table + block->entry, m_table + block[1].entry);
    grams::Key k = block->key;
    std::uint64_t posting = block->posting;
    for (bool first = true; !r.atEnd(); first = false) {
        if (!first)
            k += r.next();
        if (k > key)
            return false;
        readEntry(r, out, posting);
        if (k == key) {
            out.key = k;
            return true;
        }
    }
    return false;
}

void ContentIndex::Segment::contents(const Entry& e, std::vector<ContentId>& out) const
{
    if (e.listed) {
        NumberReader r(e.listed, m_table + m_header.tableBytes);
        const std::size_t from = out.size();
        std::uint64_t place = 0;
        for (std::uint64_t i = 0; i < e.count; ++i) {
            place += r.next();
            out.push_back(m_listed ? m_listed[place] : static_cast<ContentId>(m_header.contentBegin + place));
            ++place;
        }
        if (m_listed)
            std::sort(out.begin() + static_cast<std::ptrdiff_t>(from), out.end());
        return;
    }
    const std::size_t from = out.size();
    out.resize(from + static_cast<std::size_t>(e.count));
    BitReader bits(m_postings + e.offset, m_postings + e.offset + e.size);
    getInterpolative(bits, out.data() + from, static_cast<std::size_t>(e.count), m_header.contentBegin,
        std::uint64_t {m_header.contentEnd} - 1);
}

// ---- ContentIndex::TextFile ---------------------------------------------------------
//
// Documents' texts, one after the other, each as packText() made it. Only
// appended to; read at any offset by searches. Others may read it meanwhile
// (a copy of the index folder: IndexService::moveTo).

class ContentIndex::TextFile {
public:
    static std::shared_ptr<TextFile> open(const QString& path, std::uint64_t number, bool create)
    {
        win32::UniqueHandle file(::CreateFileW(win32::longPath(QDir::toNativeSeparators(path).toStdWString()).c_str(),
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
            create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        LARGE_INTEGER size {};
        if (!file.valid() || !::GetFileSizeEx(file.get(), &size))
            return nullptr;
        auto out = std::make_shared<TextFile>();
        out->m_file = std::move(file);
        out->m_number = number;
        out->m_size = static_cast<std::uint64_t>(size.QuadPart);
        return out;
    }

    std::uint64_t number() const noexcept { return m_number; }
    std::uint64_t size() const noexcept { return m_size.load(); }

    std::optional<std::uint64_t> append(QByteArrayView data)
    {
        std::lock_guard lock(m_appendMutex);
        const std::uint64_t at = m_size.load();
        OVERLAPPED where {};
        where.Offset = static_cast<DWORD>(at);
        where.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD written = 0;
        if (data.size() > 0x7FFF'FFFF
            || !::WriteFile(m_file.get(), data.data(), static_cast<DWORD>(data.size()), &written, &where)
            || written != static_cast<DWORD>(data.size()))
            return std::nullopt;
        m_size = at + written;
        return at;
    }

    bool read(std::uint64_t offset, std::uint32_t bytes, QByteArray& out) const
    {
        if (offset + bytes > m_size.load())
            return false;
        out.resize(static_cast<qsizetype>(bytes));
        OVERLAPPED where {};
        where.Offset = static_cast<DWORD>(offset);
        where.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        return ::ReadFile(m_file.get(), out.data(), bytes, &got, &where) && got == bytes;
    }

private:
    win32::UniqueHandle m_file;
    std::uint64_t m_number = 0;
    std::atomic<std::uint64_t> m_size {0};
    std::mutex m_appendMutex;
};

// ---- ContentIndex -------------------------------------------------------------------

ContentIndex::SearchGuard::SearchGuard(const ContentIndex* index) noexcept
    : m_index(index)
{
    if (m_index)
        m_index->m_searches.fetch_add(1);
}

ContentIndex::SearchGuard::~SearchGuard()
{
    if (m_index)
        m_index->m_searches.fetch_sub(1);
}

ContentIndex::ContentIndex(QString directory)
    : m_directory(std::move(directory))
{
}

ContentIndex::~ContentIndex() = default;

QString ContentIndex::segmentPath(const QString& directory, std::uint64_t number)
{
    return directory + u'/' + QString::number(number) + QStringLiteral(".grams");
}

QString ContentIndex::segmentPath(std::uint64_t number) const
{
    return segmentPath(m_directory, number);
}

QString ContentIndex::textPath(const QString& directory, std::uint64_t number)
{
    return directory + u'/' + QString::number(number) + QStringLiteral(".texts");
}

QString ContentIndex::textPath(std::uint64_t number) const
{
    return textPath(m_directory, number);
}

std::shared_ptr<ContentIndex::TextFile> ContentIndex::textFileForAppend()
{
    std::unique_lock lock(m_mutex);
    return textFileLocked();
}

std::shared_ptr<ContentIndex::TextFile> ContentIndex::textFileLocked()
{
    if (!m_textFile) {
        QDir().mkpath(m_directory);
        const std::uint64_t number = m_nextSegment++;
        m_textFile = TextFile::open(textPath(number), number, true);
    }
    return m_textFile;
}

std::optional<ContentIndex::TextRef> ContentIndex::textWithPrint(const grams::Fingerprint& print) const
{
    const auto it = m_textByPrint.find(print);
    if (it == m_textByPrint.end())
        return std::nullopt;
    const auto use = m_textUses.find(it->second);
    if (use == m_textUses.end())
        return std::nullopt;
    return TextRef {it->second, use->second.bytes};
}

void ContentIndex::useText(DocId doc, TextRef text, const std::optional<grams::Fingerprint>& print)
{
    m_texts[doc] = text;
    const auto [use, fresh] = m_textUses.try_emplace(text.offset, TextUse {0, text.bytes, print});
    ++use->second.docs;
    if (fresh && print)
        m_textByPrint.emplace(*print, text.offset); // the first stays when two came at once
}

void ContentIndex::dropText(DocId doc) noexcept
{
    const auto it = m_texts.find(doc);
    if (it == m_texts.end())
        return;
    const TextRef text = it->second;
    m_texts.erase(it);
    const auto use = m_textUses.find(text.offset);
    if (use != m_textUses.end() && --use->second.docs > 0)
        return;
    m_textGarbage += text.bytes;
    if (use == m_textUses.end())
        return;
    if (use->second.print) {
        const auto by = m_textByPrint.find(*use->second.print);
        if (by != m_textByPrint.end() && by->second == text.offset)
            m_textByPrint.erase(by);
    }
    m_textUses.erase(use);
}

QByteArray ContentIndex::packText(const doctext::DocText& text)
{
    const std::string bytes = doctext::serialize(text);
    return qCompress(reinterpret_cast<const uchar*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}

std::optional<doctext::DocText> ContentIndex::textOf(EntryId entry) const
{
    std::shared_ptr<TextFile> file;
    TextRef ref {};
    {
        std::shared_lock lock(m_mutex);
        const DocId d = findDoc(entry);
        if (d == kNoDoc || !isCurrent(m_docState[d]))
            return std::nullopt;
        if ((m_docState[d] & kBase) == kEmpty)
            return doctext::DocText {};
        if (!m_textFile)
            return std::nullopt;
        const auto it = m_texts.find(d);
        if (it == m_texts.end())
            return std::nullopt;
        file = m_textFile;
        ref = it->second;
    }
    QByteArray packed;
    if (!file->read(ref.offset, ref.bytes, packed))
        return std::nullopt;
    const QByteArray bytes = qUncompress(packed);
    return doctext::deserialize(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
        std::numeric_limits<std::size_t>::max());
}

std::uint32_t ContentIndex::stampOf(std::int64_t size, std::int64_t writeTime) noexcept
{
    std::uint64_t x = static_cast<std::uint64_t>(size) * 0x9E37'79B9'7F4A'7C15ull ^ static_cast<std::uint64_t>(writeTime);
    x ^= x >> 31; // splitmix64's finalizer
    x *= 0xBF58'476D'1CE4'E5B9ull;
    x ^= x >> 27;
    x *= 0x94D0'49BB'1331'11EBull;
    x ^= x >> 31;
    return static_cast<std::uint32_t>(x);
}

ContentIndex::DocId ContentIndex::findDoc(EntryId entry) const noexcept
{
    const auto search = [&](const std::vector<DocId>& order) {
        auto it = std::lower_bound(
            order.begin(), order.end(), entry, [&](DocId d, EntryId e) { return m_docEntry[d] < e; });
        for (; it != order.end() && m_docEntry[*it] == entry; ++it) {
            if (!isDead(m_docState[*it]))
                return *it;
        }
        return kNoDoc;
    };
    const DocId found = search(m_order);
    return found != kNoDoc ? found : search(m_orderTail);
}

void ContentIndex::kill(DocId doc) noexcept
{
    if (isDead(m_docState[doc]))
        return;
    m_docState[doc] = kDead;
    ++m_dead;
    ++m_deadInOrder;
    dropText(doc);
}

void ContentIndex::insertOrder(DocId doc)
{
    const EntryId entry = m_docEntry[doc];
    const auto at = std::upper_bound(
        m_orderTail.begin(), m_orderTail.end(), entry, [&](EntryId e, DocId d) { return e < m_docEntry[d]; });
    m_orderTail.insert(at, doc);
    if (m_deadInOrder * 4 > m_order.size() + m_orderTail.size() && m_deadInOrder > kOrderTail) {
        rebuildOrder();
    } else if (m_orderTail.size() > kOrderTail) {
        const std::size_t middle = m_order.size();
        m_order.insert(m_order.end(), m_orderTail.begin(), m_orderTail.end());
        std::inplace_merge(m_order.begin(), m_order.begin() + static_cast<std::ptrdiff_t>(middle), m_order.end(),
            [&](DocId a, DocId b) { return m_docEntry[a] < m_docEntry[b]; });
        m_orderTail.clear();
    }
}

void ContentIndex::rebuildOrder()
{
    m_order.clear();
    m_orderTail.clear();
    for (DocId d = 0; d < m_docEntry.size(); ++d) {
        if (!isDead(m_docState[d]))
            m_order.push_back(d);
    }
    std::stable_sort(m_order.begin(), m_order.end(), [&](DocId a, DocId b) { return m_docEntry[a] < m_docEntry[b]; });
    m_order.shrink_to_fit();
    m_deadInOrder = 0;
}

// Calls f(doc) for each document that is not dead, by entry.
template <typename F> void ContentIndex::forEachOrdered(F&& f) const
{
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < m_order.size() || j < m_orderTail.size()) {
        DocId d;
        if (j >= m_orderTail.size() || (i < m_order.size() && m_docEntry[m_order[i]] <= m_docEntry[m_orderTail[j]]))
            d = m_order[i++];
        else
            d = m_orderTail[j++];
        if (!isDead(m_docState[d]))
            f(d);
    }
}

void ContentIndex::contentsOf(grams::Key key, std::vector<ContentId>& out) const
{
    Segment::Entry e;
    for (const auto& s : m_segments) {
        if (s->find(key, e))
            s->contents(e, out);
    }
    const std::size_t from = out.size();
    const auto scan = [&](const std::vector<std::uint64_t>& pairs) {
        for (const std::uint64_t p : pairs) {
            if ((p >> kIdBits) == key)
                out.push_back(static_cast<ContentId>(p & kIdMask));
        }
    };
    if (m_writing)
        scan(*m_writing);
    scan(m_memory);
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(from), out.end());
}

ContentIndex::ContentId ContentIndex::findContent(const grams::Fingerprint& print) const noexcept
{
    if (const auto it = m_memoryContents.find(print); it != m_memoryContents.end())
        return it->second;
    for (const auto& s : m_segments) {
        if (const ContentId c = s->contentOf(print); c != kNoContent)
            return c;
    }
    return kNoContent;
}

ContentIndex::ContentId ContentIndex::memoryBegin() const noexcept
{
    return m_segments.empty() ? 0 : m_segments.back()->contentEnd();
}

std::vector<bool> ContentIndex::liveContents() const
{
    std::vector<bool> live(m_contentEnd, false);
    for (DocId d = 0; d < m_docEntry.size(); ++d) {
        if (const ContentId c = m_docContent[d]; c != kNoContent && !isDead(m_docState[d]))
            live[c] = true;
    }
    return live;
}

bool ContentIndex::knows(EntryId entry) const
{
    std::shared_lock lock(m_mutex);
    const DocId d = findDoc(entry);
    return d != kNoDoc && isCurrent(m_docState[d]);
}

ContentIndex::Lookup ContentIndex::lookup(QStringView phrase) const
{
    Lookup out;
    const std::vector<grams::Key> keys = grams::ofPhrase(phrase);
    out.usable = !keys.empty();
    out.decisive = out.usable && grams::decides(phrase);

    std::shared_lock lock(m_mutex);
    // Rarest first: intersecting with it keeps the lists short.
    std::vector<std::pair<std::uint64_t, grams::Key>> order;
    for (const grams::Key key : keys) {
        std::uint64_t count = 0;
        Segment::Entry e;
        for (const auto& s : m_segments) {
            if (s->find(key, e))
                count += e.count;
        }
        order.emplace_back(count, key);
    }
    std::sort(order.begin(), order.end());
    std::vector<ContentId> matched;
    std::vector<ContentId> next;
    for (std::size_t k = 0; k < order.size(); ++k) {
        next.clear();
        contentsOf(order[k].second, next);
        if (k == 0) {
            matched.swap(next);
        } else {
            std::size_t kept = 0;
            std::size_t j = 0;
            for (const ContentId c : matched) {
                while (j < next.size() && next[j] < c)
                    ++j;
                if (j < next.size() && next[j] == c)
                    matched[kept++] = c;
            }
            matched.resize(kept);
        }
        if (matched.empty())
            break;
    }

    std::vector<std::uint64_t> bits((m_contentEnd + std::size_t {63}) / 64, 0);
    for (const ContentId c : matched) {
        if (c < m_contentEnd)
            bits[c >> 6] |= std::uint64_t {1} << (c & 63);
    }
    out.known.reserve(m_order.size() + m_orderTail.size());
    forEachOrdered([&](DocId d) {
        const EntryId entry = m_docEntry[d];
        if (!isCurrent(m_docState[d]) || entry >= Lookup::kMatch)
            return;
        const ContentId c = m_docContent[d];
        const bool match = c != kNoContent && ((bits[c >> 6] >> (c & 63)) & 1);
        out.known.push_back(entry | (match ? Lookup::kMatch : 0));
    });
    return out;
}

void ContentIndex::markChanged(std::span<const EntryId> entries)
{
    if (entries.empty())
        return;
    std::function<void()> listener;
    {
        std::unique_lock lock(m_mutex);
        const std::uint64_t sequence = m_changeSequence.fetch_add(1) + 1;
        bool any = false;
        for (const EntryId e : entries) {
            if (m_reads > 0)
                m_changedWhileReading[e] = sequence;
            const DocId d = findDoc(e);
            if (d == kNoDoc || (m_docState[d] & kDirty))
                continue;
            m_docState[d] |= kDirty;
            any = true;
        }
        if (!any)
            return;
        m_changed = true;
        listener = m_listener;
    }
    if (listener)
        listener();
}

void ContentIndex::markUnsure(const std::function<bool(EntryId)>& which)
{
    std::unique_lock lock(m_mutex);
    for (DocId d = 0; d < m_docEntry.size(); ++d) {
        if (!isDead(m_docState[d]) && which(m_docEntry[d])) {
            m_docState[d] |= kUnsure;
            m_changed = true;
        }
    }
}

void ContentIndex::markEmptyChanged()
{
    std::unique_lock lock(m_mutex);
    for (std::uint8_t& state : m_docState) {
        if ((state & kBase) == kEmpty) {
            state |= kDirty;
            m_changed = true;
        }
    }
}

void ContentIndex::remap(const FileIndex::Renumber& renumber)
{
    std::unique_lock lock(m_mutex);
    for (DocId d = 0; d < m_docEntry.size(); ++d) {
        if (!isDead(m_docState[d])) {
            m_docEntry[d] = renumber(m_docEntry[d]);
            if (m_docEntry[d] == kNoEntry)
                kill(d);
        } else {
            m_docEntry[d] = kNoEntry;
        }
    }
    rebuildOrder();
    std::unordered_map<EntryId, std::uint64_t> changed;
    for (const auto& [entry, sequence] : m_changedWhileReading) {
        if (const EntryId e = renumber(entry); e != kNoEntry)
            changed[e] = sequence;
    }
    m_changedWhileReading.swap(changed);
    m_changed = true;
}

void ContentIndex::resetLocked()
{
    ++m_generation; // a flush running now leaves its segment out
    for (const auto& s : m_segments)
        m_obsolete.push_back(s->number());
    m_segments.clear();
    m_memory = decltype(m_memory)();
    m_writing.reset();
    m_flushBuffers.reset();
    m_memoryContents.clear();
    m_contentEnd = 0;
    m_docEntry = decltype(m_docEntry)();
    m_docState = decltype(m_docState)();
    m_docStamp = decltype(m_docStamp)();
    m_docContent = decltype(m_docContent)();
    m_order = decltype(m_order)();
    m_orderTail.clear();
    m_dead = 0;
    m_deadInOrder = 0;
    if (m_textFile)
        m_obsoleteTexts.push_back(m_textFile->number());
    m_textFile.reset();
    m_texts = decltype(m_texts)();
    m_textUses = decltype(m_textUses)();
    m_textByPrint.clear();
    m_textGarbage = 0;
}

void ContentIndex::clear()
{
    std::unique_lock lock(m_mutex);
    if (m_docEntry.empty() && m_segments.empty())
        return;
    resetLocked();
    m_changed = true;
}

void ContentIndex::setChangeListener(std::function<void()> listener)
{
    std::unique_lock lock(m_mutex);
    m_listener = std::move(listener);
}

std::vector<ContentIndex::DocInfo> ContentIndex::documents() const
{
    std::shared_lock lock(m_mutex);
    std::vector<DocInfo> out;
    out.reserve(m_order.size() + m_orderTail.size());
    forEachOrdered([&](DocId d) {
        const std::uint8_t state = m_docState[d];
        const Need need = (state & kDirty) ? Need::Read : (state & kUnsure) ? Need::Check : Need::None;
        out.push_back({m_docEntry[d], need, m_docStamp[d]});
    });
    return out;
}

void ContentIndex::retire(std::span<const EntryId> entries)
{
    if (entries.empty())
        return;
    std::unique_lock lock(m_mutex);
    for (const EntryId e : entries) {
        if (const DocId d = findDoc(e); d != kNoDoc)
            kill(d);
    }
    m_changed = true;
}

void ContentIndex::confirm(EntryId entry)
{
    std::unique_lock lock(m_mutex);
    if (const DocId d = findDoc(entry); d != kNoDoc && (m_docState[d] & kUnsure)) {
        m_docState[d] &= static_cast<std::uint8_t>(~kUnsure);
        m_changed = true;
    }
}

void ContentIndex::beginReads()
{
    std::unique_lock lock(m_mutex);
    ++m_reads;
}

void ContentIndex::endReads()
{
    std::unique_lock lock(m_mutex);
    if (--m_reads == 0)
        m_changedWhileReading = decltype(m_changedWhileReading)();
}

bool ContentIndex::add(EntryId entry, std::span<const grams::Key> keys, bool empty, std::uint32_t stamp,
    std::uint64_t since, QByteArrayView text)
{
    const bool hasGrams = !empty && !keys.empty();
    const bool hasText = !text.isEmpty() && text.size() <= 0x7FFF'FFFF;
    const auto textBytes = static_cast<std::uint32_t>(hasText ? text.size() : 0);
    // Before taking the lock: the grams of a large file take a while to hash.
    const std::optional<grams::Fingerprint> print = hasGrams ? grams::fingerprint(keys) : std::nullopt;
    const std::optional<grams::Fingerprint> textPrint
        = hasText ? printOf(std::as_bytes(std::span(text.data(), textBytes))) : std::nullopt;
    // Looked up while others may look up too: the content with these grams
    // (a copy's, say), and whether a document has this text. The content
    // took four fifths of the time the lock was held alone, and 16 threads
    // adding files waited for it longer than they hashed and added
    // (wsbench --content-index, without an antivirus).
    ContentId known = kNoContent;
    std::uint64_t generation = 0;
    bool textKnown = false;
    if (print || textPrint) {
        std::shared_lock lock(m_mutex);
        known = print ? findContent(*print) : kNoContent;
        generation = m_generation;
        textKnown = textPrint && textWithPrint(*textPrint).has_value();
    }
    // The text goes to the file first (outside the lock: searches read
    // meanwhile), unless a document has it already.
    std::shared_ptr<TextFile> textFile;
    std::optional<std::uint64_t> textAt;
    if (hasText && !textKnown) {
        textFile = textFileForAppend();
        if (textFile)
            textAt = textFile->append(text);
    }
    std::unique_lock lock(m_mutex);
    if (hasGrams) {
        // Room in memory first, as these may let go of the lock: what is
        // looked up below is looked up after.
        if (!m_memory.empty() && m_memory.size() + keys.size() > kFlushPairs)
            flush(lock);
        // Grams come faster than a segment is written (on 16 threads,
        // without an antivirus): past another segment's worth, wait for it.
        m_flushed.wait(lock, [&] { return !m_flushing || m_memory.size() + keys.size() <= kFlushPairs; });
    }
    // The document's text: one a document has (found before, or come
    // meanwhile), else the one appended; appended again, under the lock,
    // if the other went meanwhile or the file was replaced (compactTexts,
    // clear).
    std::optional<TextRef> textRef = textPrint ? textWithPrint(*textPrint) : std::nullopt;
    if (textAt && textFile == m_textFile) {
        if (textRef)
            m_textGarbage += textBytes;
        else
            textRef = TextRef {*textAt, textBytes};
    }
    if (hasText && !textRef) {
        textFile = textFileLocked();
        textAt = textFile ? textFile->append(text) : std::nullopt;
        if (textAt)
            textRef = TextRef {*textAt, textBytes};
    }
    const auto refuse = [&] {
        if (textRef && !m_textUses.contains(textRef->offset))
            m_textGarbage += textRef->bytes; // appended for this document alone
        return false;
    };
    if (m_docEntry.size() >= kMaxIds)
        return refuse();
    // As looked up before, unless the contents were numbered anew meanwhile
    // (merge); one added meanwhile is in memory. (One added and written to a
    // segment meanwhile is missed: the grams get a content of their own.)
    ContentId content = known;
    if (print && generation != m_generation) {
        content = findContent(*print);
    } else if (print && content == kNoContent) {
        if (const auto it = m_memoryContents.find(*print); it != m_memoryContents.end())
            content = it->second;
    }
    if (hasGrams && content == kNoContent) {
        const std::size_t inMemory = m_memory.size() + (m_writing ? m_writing->size() : 0);
        if ((inMemory > 0 && inMemory + keys.size() > kMaxMemoryPairs) || m_contentEnd >= kMaxIds)
            return refuse(); // segments cannot be written
        content = m_contentEnd++;
        if (print)
            m_memoryContents.emplace(*print, content);
        for (const grams::Key key : keys)
            m_memory.push_back((key << kIdBits) | content);
    }
    const DocId doc = addDocLocked(entry, empty ? kEmpty : kLive, stamp, since, content);
    if (textRef)
        useText(doc, *textRef, textPrint);
    replaceLocked(entry, doc);
    if (m_memory.size() >= kFlushPairs)
        flush(lock); // last: nothing is looked up after
    return true;
}

bool ContentIndex::addCopy(
    EntryId entry, EntryId original, std::uint32_t originalStamp, std::uint32_t stamp, std::uint64_t since)
{
    std::unique_lock lock(m_mutex);
    const DocId from = findDoc(original);
    if (from == kNoDoc || !isCurrent(m_docState[from]) || m_docStamp[from] != originalStamp
        || m_docEntry.size() >= kMaxIds)
        return false;
    const DocId doc = addDocLocked(entry, m_docState[from] & kBase, stamp, since, m_docContent[from]);
    if (const auto it = m_texts.find(from); it != m_texts.end()) {
        const auto use = m_textUses.find(it->second.offset);
        useText(doc, it->second, use != m_textUses.end() ? use->second.print : std::nullopt);
    }
    replaceLocked(entry, doc);
    return true;
}

ContentIndex::DocId ContentIndex::addDocLocked(
    EntryId entry, std::uint8_t state, std::uint32_t stamp, std::uint64_t since, ContentId content)
{
    if (const auto it = m_changedWhileReading.find(entry); it != m_changedWhileReading.end() && it->second > since)
        state |= kDirty; // written to while it was being read
    const auto doc = static_cast<DocId>(m_docEntry.size());
    m_docEntry.push_back(entry);
    m_docState.push_back(state);
    m_docStamp.push_back(stamp);
    m_docContent.push_back(content);
    m_changed = true;
    return doc;
}

// After the new document has its text: the old one's is often the same, and
// would be counted as left behind.
void ContentIndex::replaceLocked(EntryId entry, DocId doc)
{
    if (const DocId old = findDoc(entry); old != kNoDoc) // not `doc`: that one is not in the order yet
        kill(old);
    insertOrder(doc);
}

// What writing a segment uses, kept for the next one: fresh memory costs a
// page fault every 4 KB, and memory handed back in many pieces (the encoded
// stretches) stays with the process.
struct ContentIndex::FlushBuffers {
    std::vector<std::uint64_t> memory; // m_memory's next
    std::vector<std::uint64_t> sorted;
    std::vector<std::uint64_t> scratch;
    std::vector<EncodedKeys> encoded;
};

// The grams in memory into a segment of their own: sorted, encoded and
// written without the lock. Memory hands its pairs over as they are, and
// fills anew; until the segment takes their place, searches find them there
// (m_writing), and copies of their files too. With the lock held throughout,
// every thread with a file to add waited: 12 s of a first run's 75, the 16
// threads idle (wsbench --service --content); copying them under the lock,
// 2 ms a segment, for all of them. One at a time; memory takes another
// segment's worth meanwhile (add() waits past that).
bool ContentIndex::flush(std::unique_lock<std::shared_mutex>& lock)
{
    if (m_memory.empty() || m_flushing)
        return true;
    m_flushing = true;
    auto writing = std::make_shared<std::vector<std::uint64_t>>();
    writing->swap(m_memory);
    std::unique_ptr<FlushBuffers> buffers
        = m_flushBuffers ? std::move(m_flushBuffers) : std::make_unique<FlushBuffers>();
    m_memory.swap(buffers->memory); // what the segment before had
    m_memory.reserve(writing->size());
    m_writing = writing;
    std::vector<Print> prints; // ascending, as the map has them
    prints.reserve(m_memoryContents.size());
    for (const auto& [print, content] : m_memoryContents)
        prints.push_back({print.high, print.low, content});
    const ContentId begin = memoryBegin();
    const ContentId end = m_contentEnd;
    const std::uint64_t number = m_nextSegment++;
    const QString path = segmentPath(number);
    const std::uint64_t generation = m_generation;
    lock.unlock();

    // On a few threads: sorted by stretches of keys (sortPairs), each
    // stretch encoded, written in order (as merge does). On one, a segment
    // took 42 ms, and the 16 threads reading files spent a third of their
    // time waiting for room (wsbench --content-index, without an antivirus).
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 4, 1, 8);
    std::vector<std::size_t> ends;
    sortPairs(*writing, threads, buffers->sorted, buffers->scratch, &ends);
    const std::vector<std::uint64_t>& pairs = buffers->sorted;
    std::vector<EncodedKeys>& encoded = buffers->encoded;
    encoded.resize(ends.size());
    for (EncodedKeys& e : encoded)
        e.clear();
    inParallel(threads, encoded.size(), [&](std::size_t part, int) {
        std::vector<ContentId> contents;
        for (std::size_t i = part > 0 ? ends[part - 1] : 0; i < ends[part];) {
            const grams::Key key = pairs[i] >> kIdBits;
            contents.clear();
            for (; i < ends[part] && (pairs[i] >> kIdBits) == key; ++i)
                contents.push_back(static_cast<ContentId>(pairs[i] & kIdMask));
            encoded[part].add(key, contents, begin, end);
        }
    });
    SegmentWriter writer(path, begin, end, 0, prints);
    for (const EncodedKeys& e : encoded)
        writer.add(e);
    auto segment = writer.finish() ? Segment::open(path, number) : nullptr;

    lock.lock();
    m_flushing = false;
    m_flushed.notify_all();
    if (generation != m_generation) { // cleared, or merged (with these pairs), meanwhile
        m_obsolete.push_back(number);
        return false;
    }
    m_writing.reset();
    if (!segment) { // the pairs go back to memory, before those that came since
        m_memory.insert(m_memory.begin(), writing->begin(), writing->end());
        m_obsolete.push_back(number);
        return false;
    }
    if (writing.use_count() == 1) { // the next memory, empty
        buffers->memory.swap(*writing);
        buffers->memory.clear();
    }
    m_flushBuffers = std::move(buffers);
    m_segments.push_back(std::move(segment));
    std::erase_if(m_memoryContents, [&](const auto& c) { return c.second < end; });
    return true;
}

// How many segments at the end are due to be merged: kMergeFactor of the
// same level in a row. Every segment is rewritten about once per level, and
// there are at most kMergeFactor - 1 of each level.
std::size_t ContentIndex::dueTailLocked() const noexcept
{
    if (m_segments.size() < kMergeFactor)
        return 0;
    const std::uint32_t level = m_segments.back()->level();
    if (level == kTopLevel)
        return 0;
    for (std::size_t k = 2; k <= kMergeFactor; ++k) {
        if (m_segments[m_segments.size() - k]->level() != level)
            return 0;
    }
    return kMergeFactor;
}

bool ContentIndex::mergeAllDueLocked() const noexcept
{
    return m_segments.size() > kMaxSegments || (m_dead > 4096 && m_dead * 4 > m_docEntry.size());
}

bool ContentIndex::needsMerge() const
{
    std::shared_lock lock(m_mutex);
    return mergeAllDueLocked() || dueTailLocked() > 0;
}

template <typename F>
void ContentIndex::forEachKey(
    const Segments& segments, std::span<const std::uint64_t> memory, grams::Key from, grams::Key to, F&& f)
{
    std::vector<Segment::Cursor> cursors;
    cursors.reserve(segments.size());
    for (const auto& s : segments)
        cursors.emplace_back(*s, from);
    // The cursors with keys left, the one with the smallest key on top; of
    // those with the same key, the first segment's: popped in that order,
    // they give their contents in ascending order.
    const auto later = [&](std::uint32_t a, std::uint32_t b) {
        const grams::Key ka = cursors[a].key();
        const grams::Key kb = cursors[b].key();
        return ka != kb ? ka > kb : a > b;
    };
    std::vector<std::uint32_t> heap;
    for (std::uint32_t i = 0; i < cursors.size(); ++i) {
        if (!cursors[i].done() && cursors[i].key() < to)
            heap.push_back(i);
    }
    std::make_heap(heap.begin(), heap.end(), later);
    std::size_t m = static_cast<std::size_t>(
        std::lower_bound(memory.begin(), memory.end(), from << kIdBits) - memory.begin());
    std::vector<ContentId> contents;
    for (;;) {
        grams::Key key = heap.empty() ? to : cursors[heap.front()].key();
        if (m < memory.size())
            key = std::min(key, memory[m] >> kIdBits);
        if (key >= to)
            break;
        contents.clear();
        while (!heap.empty() && cursors[heap.front()].key() == key) {
            std::pop_heap(heap.begin(), heap.end(), later);
            Segment::Cursor& c = cursors[heap.back()];
            c.contents(contents);
            c.next();
            if (c.done() || c.key() >= to)
                heap.pop_back();
            else
                std::push_heap(heap.begin(), heap.end(), later);
        }
        for (; m < memory.size() && (memory[m] >> kIdBits) == key; ++m)
            contents.push_back(static_cast<ContentId>(memory[m] & kIdMask));
        f(key, contents);
    }
}

std::vector<grams::Key> ContentIndex::splitKeys(const Segments& segments, std::span<const std::uint64_t> memory,
    std::size_t parts, int threads, std::uint64_t& total)
{
    std::vector<grams::Key> out;
    // Where the work is, from every segment (on the threads: their tables
    // are read through) and from memory, by key.
    std::vector<std::vector<std::pair<grams::Key, std::uint64_t>>> of(segments.size() + 1);
    inParallel(threads, segments.size(), [&](std::size_t s, int) { segments[s]->work(of[s]); });
    constexpr std::size_t kPairsEach = 512;
    for (std::size_t i = 0; i < memory.size(); i += kPairsEach)
        of.back().emplace_back(memory[i] >> kIdBits, std::min(kPairsEach, memory.size() - i));
    std::vector<std::pair<grams::Key, std::uint64_t>> work;
    for (const auto& w : of)
        work.insert(work.end(), w.begin(), w.end());
    std::sort(work.begin(), work.end());
    total = 0;
    for (const auto& w : work)
        total += w.second;
    std::uint64_t sum = 0;
    for (const auto& [key, amount] : work) {
        if (out.size() + 1 >= parts)
            break;
        if (sum >= total / parts * (out.size() + 1) && key > 0 && (out.empty() || key > out.back()))
            out.push_back(key);
        sum += amount;
    }
    return out;
}

bool ContentIndex::needsFullMerge() const
{
    std::shared_lock lock(m_mutex);
    return mergeAllDueLocked();
}

bool ContentIndex::mergeTails()
{
    for (;;) {
        std::size_t count = 0;
        {
            std::shared_lock lock(m_mutex);
            count = dueTailLocked();
        }
        if (count == 0)
            return true;
        if (!mergeTail(count))
            return false;
    }
}

bool ContentIndex::mergeDue(int threads)
{
    if (needsFullMerge())
        return merge(threads);
    return mergeTails();
}

// The last `count` segments into one of the next level, all their contents
// kept: documents may be added meanwhile, and one may take a content that no
// document had when this began (a copy of a file that was deleted). merge()
// drops those, and numbers the contents anew.
bool ContentIndex::mergeTail(std::size_t count)
{
    Segments segments;
    std::uint64_t number = 0;
    QString path;
    {
        std::unique_lock lock(m_mutex);
        if (count < 2 || count > m_segments.size())
            return false;
        segments.assign(m_segments.end() - static_cast<std::ptrdiff_t>(count), m_segments.end());
        number = m_nextSegment++;
        path = segmentPath(number);
    }
    std::uint32_t level = 0;
    for (const auto& s : segments)
        level = std::max(level, s->level() + 1);
    level = std::min(level, kTopLevel - 1);

    std::vector<Print> prints;
    for (const auto& s : segments)
        prints.insert(prints.end(), s->prints().begin(), s->prints().end());
    std::sort(prints.begin(), prints.end(),
        [](const Print& a, const Print& b) { return a.fingerprint() < b.fingerprint(); });
    SegmentWriter writer(path, segments.front()->contentBegin(), segments.back()->contentEnd(), level, prints);
    forEachKey(segments, {}, 0, std::numeric_limits<grams::Key>::max(),
        [&](grams::Key key, const std::vector<ContentId>& contents) { writer.add(key, contents); });
    auto merged = writer.finish() ? Segment::open(path, number) : nullptr;

    std::unique_lock lock(m_mutex);
    // The same segments must still be there, side by side: clear() may have
    // dropped them meanwhile. New ones may have come after them.
    const auto at = std::find(m_segments.begin(), m_segments.end(), segments.front());
    if (!merged || static_cast<std::size_t>(m_segments.end() - at) < count
        || !std::equal(segments.begin(), segments.end(), at)) {
        m_obsolete.push_back(number);
        return false;
    }
    for (const auto& s : segments)
        m_obsolete.push_back(s->number());
    const auto next = m_segments.erase(at, at + static_cast<std::ptrdiff_t>(count));
    m_segments.insert(next, std::move(merged));
    m_changed = true;
    return true;
}

// Everything into one segment, without the dead documents and the contents no
// document has, and the contents numbered anew so that those of similar files
// (versions of a file, files of a kind) end up side by side: a gram's
// contents then come in runs and clusters, which postings write in next to
// nothing. First by the smallest scrambled key among their grams (a MinHash),
// then by how many grams they have: contents that have most grams in common
// most likely share their smallest one. Then by a recursive graph bisection
// (Bisection) over a sample of the grams many contents have: postings of 59
// MB rather than 79 (of 330 000 files), for half a second on 16 threads and
// some 50 MB meanwhile; over all of them, 57 MB for 18 s and 800 MB.
//
// On one thread it took 13 s for 330 000 files, most of it decoding and
// encoding postings, so stretches of keys are merged on `threads` threads
// (1 to 1.5 s on 8 to 16: wsbench --service --content) and written one after
// the other as they come: the file is the same as one thread writes.
bool ContentIndex::merge(int threads)
{
    // Read from what is there now, without the lock: segments never change,
    // and documents are only added between merges (by the same caller).
    Segments segments;
    std::vector<std::uint64_t> memory;
    std::vector<Print> prints;
    std::vector<bool> live;
    std::size_t docCount = 0;
    ContentId contentEnd = 0;
    std::uint64_t number = 0;
    QString path;
    {
        std::unique_lock lock(m_mutex);
        // A flush running now would take memory's grams into a segment
        // this does not know: the merge would come to nothing.
        m_flushed.wait(lock, [&] { return !m_flushing; });
        segments = m_segments;
        memory = m_memory;
        for (const auto& [print, content] : m_memoryContents)
            prints.push_back({print.high, print.low, content});
        live = liveContents();
        docCount = m_docEntry.size();
        contentEnd = m_contentEnd;
        number = m_nextSegment++;
        path = segmentPath(number);
    }
    for (const auto& s : segments)
        prints.insert(prints.end(), s->prints().begin(), s->prints().end());
    {
        std::vector<std::uint64_t> sorted;
        std::vector<std::uint64_t> scratch;
        sortPairs(memory, std::max(threads, 1), sorted, scratch);
        memory.swap(sorted);
    }

    threads = std::max(threads, 1);
    // Several stretches per thread: some take longer than others.
    std::uint64_t work = 0;
    const std::vector<grams::Key> cuts
        = splitKeys(segments, memory, threads > 1 ? std::size_t(threads) * 8 : 1, threads, work);
    const std::size_t parts = cuts.size() + 1;
    const auto from = [&](std::size_t part) { return part == 0 ? grams::Key {0} : cuts[part - 1]; };
    const auto to = [&](std::size_t part) {
        return part < cuts.size() ? cuts[part] : std::numeric_limits<grams::Key>::max();
    };
    // The grams the bisection goes by: one in `sampling` (by a hash of the
    // key) of those that `common` contents have or more, some 6 million
    // pairs all told. Twice as many took twice as long, for postings 2%
    // smaller; a quarter as many, 5% larger.
    constexpr std::uint64_t kBisectionWork = std::uint64_t {8} << 20;
    constexpr std::uint64_t kSampleSeed = 0x9E37'79B9'7F4A'7C15ull;
    const std::uint64_t sampling = std::bit_ceil(std::max<std::uint64_t>(work / kBisectionWork, 1));
    const std::uint64_t common = std::clamp<std::uint64_t>(contentEnd / 2048, kInline + 1, 64);

    std::vector<ContentId> newId(contentEnd, kNoContent);
    std::vector<ContentId> listed; // the contents the table lists, by their new numbers, most listed first
    ContentId kept = 0;
    {
        // Each thread its own minimums and counts, added up after: no two
        // threads write the same memory. As many threads as that leaves
        // within 64 MB.
        struct Counts {
            std::vector<std::uint64_t> smallest;
            std::vector<std::uint32_t> size; // grams
            std::vector<std::uint32_t> listings; // keys few enough contents have to be listed in the table
        };
        const int counting = static_cast<int>(
            std::clamp<std::uint64_t>((std::uint64_t {64} << 20) / ((std::uint64_t {contentEnd} + 1) * 16), 1,
                static_cast<std::uint64_t>(threads)));
        std::vector<Counts> countsOf(static_cast<std::size_t>(counting));
        // By stretch, the contents of each sampled gram in turn.
        std::vector<std::vector<ContentId>> sampled(parts);
        std::vector<std::vector<std::size_t>> sampledEnds(parts);
        inParallel(counting, parts, [&](std::size_t part, int worker) {
            Counts& counts = countsOf[static_cast<std::size_t>(worker)];
            if (counts.smallest.empty()) {
                counts.smallest.assign(contentEnd, std::numeric_limits<std::uint64_t>::max());
                counts.size.assign(contentEnd, 0);
                counts.listings.assign(contentEnd, 0);
            }
            forEachKey(
                segments, memory, from(part), to(part), [&](grams::Key key, const std::vector<ContentId>& contents) {
                    const std::uint64_t scrambled = scramble(key);
                    std::uint64_t alive = 0;
                    for (const ContentId c : contents) {
                        if (c < contentEnd && live[c]) {
                            counts.smallest[c] = std::min(counts.smallest[c], scrambled);
                            ++counts.size[c];
                            ++alive;
                        }
                    }
                    const bool few = alive <= kInline;
                    if (!few && (alive < common || (scramble(key ^ kSampleSeed) & (sampling - 1)) != 0))
                        return;
                    for (const ContentId c : contents) {
                        if (c >= contentEnd || !live[c])
                            continue;
                        if (few)
                            ++counts.listings[c];
                        else
                            sampled[part].push_back(c);
                    }
                    if (!few)
                        sampledEnds[part].push_back(sampled[part].size());
                });
        });
        Counts all {std::vector<std::uint64_t>(contentEnd, std::numeric_limits<std::uint64_t>::max()),
            std::vector<std::uint32_t>(contentEnd, 0), std::vector<std::uint32_t>(contentEnd, 0)};
        for (Counts& counts : countsOf) {
            if (counts.smallest.empty())
                continue; // took no stretch
            for (ContentId c = 0; c < contentEnd; ++c) {
                all.smallest[c] = std::min(all.smallest[c], counts.smallest[c]);
                all.size[c] += counts.size[c];
                all.listings[c] += counts.listings[c];
            }
            counts = {};
        }
        std::vector<ContentId> order;
        for (ContentId c = 0; c < contentEnd; ++c) {
            if (all.size[c] > 0)
                order.push_back(c);
        }
        std::sort(order.begin(), order.end(), [&](ContentId a, ContentId b) {
            return std::tie(all.smallest[a], all.size[a], a) < std::tie(all.smallest[b], all.size[b], b);
        });
        kept = static_cast<ContentId>(order.size());
        for (ContentId place = 0; place < kept; ++place)
            newId[order[place]] = place;

        // The bisection, of the places in that order, by the sampled grams.
        std::uint32_t termCount = 0;
        for (const auto& ends : sampledEnds)
            termCount += static_cast<std::uint32_t>(ends.size());
        if (termCount > 0) {
            std::vector<std::uint64_t> termStart(std::size_t {kept} + 1, 0);
            for (const auto& contents : sampled) {
                for (const ContentId c : contents)
                    ++termStart[std::size_t {newId[c]} + 1];
            }
            for (ContentId place = 0; place < kept; ++place)
                termStart[place + 1] += termStart[place];
            std::vector<std::uint32_t> terms(termStart[kept]);
            std::vector<std::uint64_t> fill(termStart.begin(), termStart.end() - 1);
            std::uint32_t term = 0;
            for (std::size_t part = 0; part < parts; ++part) {
                std::size_t i = 0;
                for (const std::size_t end : sampledEnds[part]) {
                    for (; i < end; ++i)
                        terms[fill[newId[sampled[part][i]]]++] = term;
                    ++term;
                }
                sampled[part] = {};
                sampledEnds[part] = {};
            }
            fill = {};
            std::vector<std::uint32_t> places(kept);
            std::iota(places.begin(), places.end(), 0u);
            Bisection(termStart, terms, termCount).order(places, threads);
            for (ContentId id = 0; id < kept; ++id)
                newId[order[places[id]]] = id;
        }

        for (ContentId c = 0; c < contentEnd; ++c) {
            if (newId[c] != kNoContent && all.listings[c] > 0)
                listed.push_back(c);
        }
        std::sort(listed.begin(), listed.end(), [&](ContentId a, ContentId b) {
            return all.listings[a] > all.listings[b] || (all.listings[a] == all.listings[b] && newId[a] < newId[b]);
        });
        for (ContentId& c : listed)
            c = newId[c];
    }
    std::vector<std::uint32_t> placeOf(kept, EncodedKeys::kNoPlace); // in `listed`, by new number
    for (std::size_t place = 0; place < listed.size(); ++place)
        placeOf[listed[place]] = static_cast<std::uint32_t>(place);
    std::vector<Print> keptPrints;
    for (const Print& p : prints) {
        if (p.content < contentEnd && newId[p.content] != kNoContent)
            keptPrints.push_back({p.high, p.low, newId[p.content]});
    }
    std::sort(keptPrints.begin(), keptPrints.end(),
        [](const Print& a, const Print& b) { return a.fingerprint() < b.fingerprint(); });
    prints = {};

    // A key's contents numbered anew, ascending again: sorted when they are
    // few, through a bitmap of all contents when they are many.
    const auto renumber = [&](const std::vector<ContentId>& contents, std::vector<ContentId>& mapped,
                              std::vector<std::uint64_t>& bits) {
        mapped.clear();
        if (contents.size() * 64 < kept) {
            for (const ContentId c : contents) {
                if (c < contentEnd && newId[c] != kNoContent)
                    mapped.push_back(newId[c]);
            }
            std::sort(mapped.begin(), mapped.end());
            return;
        }
        bits.assign((std::size_t {kept} + 63) / 64, 0);
        for (const ContentId c : contents) {
            if (c < contentEnd && newId[c] != kNoContent)
                bits[newId[c] >> 6] |= std::uint64_t {1} << (newId[c] & 63);
        }
        for (std::size_t w = 0; w < bits.size(); ++w) {
            for (std::uint64_t b = bits[w]; b != 0; b &= b - 1)
                mapped.push_back(static_cast<ContentId>(w * 64 + static_cast<std::size_t>(std::countr_zero(b))));
        }
    };

    // The stretches are encoded on `threads` threads, a few ahead of this
    // one, which writes them in order.
    SegmentWriter writer(path, 0, kept, kTopLevel, keptPrints, listed);
    {
        std::vector<EncodedKeys> encoded(parts);
        std::mutex readyMutex;
        std::condition_variable readyChanged;
        std::vector<char> ready(parts, 0); // under readyMutex
        std::size_t written = 0; // stretches written, under readyMutex
        const std::size_t ahead = static_cast<std::size_t>(threads) * 2;
        std::atomic<std::size_t> next {0};
        const auto encode = [&] {
            std::vector<ContentId> mapped;
            std::vector<std::uint64_t> bits;
            for (std::size_t part = next++; part < parts; part = next++) {
                {
                    std::unique_lock lock(readyMutex);
                    readyChanged.wait(lock, [&] { return part < written + ahead; });
                }
                EncodedKeys& out = encoded[part];
                forEachKey(segments, memory, from(part), to(part),
                    [&](grams::Key key, const std::vector<ContentId>& contents) {
                        renumber(contents, mapped, bits);
                        out.add(key, mapped, 0, kept, placeOf);
                    });
                {
                    std::lock_guard lock(readyMutex);
                    ready[part] = 1;
                }
                readyChanged.notify_all();
            }
        };
        std::vector<std::jthread> encoders;
        for (std::size_t t = 0; t < std::min(static_cast<std::size_t>(threads), parts); ++t)
            encoders.emplace_back(encode);
        for (std::size_t part = 0; part < parts; ++part) {
            {
                std::unique_lock lock(readyMutex);
                readyChanged.wait(lock, [&] { return ready[part] != 0; });
            }
            writer.add(encoded[part]);
            encoded[part] = {};
            {
                std::lock_guard lock(readyMutex);
                ++written;
            }
            readyChanged.notify_all();
        }
    } // joins
    auto merged = writer.finish() ? Segment::open(path, number) : nullptr;

    std::unique_lock lock(m_mutex);
    const std::size_t inMemory = m_memory.size() + (m_writing ? m_writing->size() : 0); // what it took, unless more came
    if (!merged || m_docEntry.size() != docCount || inMemory != memory.size() || m_contentEnd != contentEnd) {
        m_obsolete.push_back(number);
        return false;
    }
    // Documents dead by now are dropped too; the others had a live content
    // when the merge began, which they keep.
    std::vector<EntryId> entries;
    std::vector<std::uint8_t> states;
    std::vector<std::uint32_t> stamps;
    std::vector<ContentId> contents;
    std::unordered_map<DocId, TextRef> texts;
    for (DocId d = 0; d < docCount; ++d) {
        if (isDead(m_docState[d]))
            continue;
        std::uint8_t state = m_docState[d];
        ContentId content = m_docContent[d];
        if (content != kNoContent) {
            content = content < contentEnd ? newId[content] : kNoContent;
            if (content == kNoContent)
                state |= kDirty; // cannot happen; were it to, the file is read again
        }
        if (const auto it = m_texts.find(d); it != m_texts.end())
            texts.emplace(static_cast<DocId>(entries.size()), it->second);
        entries.push_back(m_docEntry[d]);
        states.push_back(state);
        stamps.push_back(m_docStamp[d]);
        contents.push_back(content);
    }
    for (const auto& s : m_segments)
        m_obsolete.push_back(s->number());
    // A flush started meanwhile (by a search's file) wrote contents this one
    // has, by their old numbers: it leaves its segment out.
    ++m_generation;
    m_segments = {std::move(merged)};
    m_memory = decltype(m_memory)();
    m_writing.reset();
    m_flushBuffers.reset(); // till files are read again
    m_memoryContents.clear();
    m_contentEnd = kept;
    m_texts.swap(texts);
    m_docEntry.swap(entries);
    m_docState.swap(states);
    m_docStamp.swap(stamps);
    m_docContent.swap(contents);
    m_dead = 0;
    rebuildOrder();
    m_changed = true;
    return true;
}

bool ContentIndex::needsTextCompaction() const
{
    std::shared_lock lock(m_mutex);
    if (!m_textFile)
        return false;
    const std::uint64_t live = m_textFile->size() - std::min(m_textFile->size(), m_textGarbage);
    return m_textGarbage > kTextGarbage && m_textGarbage > live;
}

// By the indexer, between its reads; a search may add documents meanwhile.
// Each text once, however many documents have it.
bool ContentIndex::compactTexts()
{
    std::vector<TextRef> texts;
    std::shared_ptr<TextFile> old;
    std::uint64_t number = 0;
    {
        std::unique_lock lock(m_mutex);
        if (!m_textFile)
            return true;
        for (const auto& [offset, use] : m_textUses)
            texts.push_back({offset, use.bytes});
        old = m_textFile;
        number = m_nextSegment++;
    }
    std::sort(texts.begin(), texts.end(), [](const TextRef& a, const TextRef& b) { return a.offset < b.offset; });
    auto fresh = TextFile::open(textPath(number), number, true);
    std::unordered_map<std::uint64_t, std::uint64_t> moved; // offsets, old to new
    QByteArray buffer;
    bool ok = fresh != nullptr;
    for (const TextRef& ref : texts) {
        if (!ok)
            break;
        if (!old->read(ref.offset, ref.bytes, buffer))
            continue; // lost: the documents' files are read again when a search needs them
        const auto at = fresh->append(buffer);
        ok = at.has_value();
        if (ok)
            moved.emplace(ref.offset, *at);
    }
    std::unique_lock lock(m_mutex);
    if (!ok || m_textFile != old) {
        fresh.reset();
        m_obsoleteTexts.push_back(number);
        return false;
    }
    for (const auto& [offset, use] : m_textUses) {
        if (moved.contains(offset) || !old->read(offset, use.bytes, buffer))
            continue;
        if (const auto at = fresh->append(buffer)) // came meanwhile (from a search)
            moved.emplace(offset, *at);
    }
    std::unordered_map<DocId, TextRef> next;
    std::unordered_map<std::uint64_t, TextUse> uses;
    std::map<grams::Fingerprint, std::uint64_t> byPrint;
    for (const auto& [doc, ref] : m_texts) {
        const auto to = moved.find(ref.offset);
        if (to == moved.end()) {
            m_docState[doc] |= kDirty; // its text could not be read: the file is read again
            continue;
        }
        next.emplace(doc, TextRef {to->second, ref.bytes});
        const auto was = m_textUses.find(ref.offset);
        const auto [use, first] = uses.try_emplace(
            to->second, TextUse {0, ref.bytes, was != m_textUses.end() ? was->second.print : std::nullopt});
        ++use->second.docs;
        if (first && use->second.print)
            byPrint.emplace(*use->second.print, to->second);
    }
    m_texts.swap(next);
    m_textUses.swap(uses);
    m_textByPrint.swap(byPrint);
    m_obsoleteTexts.push_back(old->number());
    m_textFile = std::move(fresh);
    m_textGarbage = 0;
    m_changed = true;
    return true;
}

// ---- persistence -----------------------------------------------------------------------

namespace {

class Blob {
public:
    template <typename T> void put(const T& v) { bytes(&v, sizeof v); }
    void bytes(const void* p, std::size_t n)
    {
        const auto* c = static_cast<const char*>(p);
        m_data.insert(m_data.end(), c, c + n);
    }
    std::vector<char> take() { return std::move(m_data); }

private:
    std::vector<char> m_data;
};

class BlobReader {
public:
    explicit BlobReader(std::span<const char> data)
        : m_data(data)
    {
    }
    template <typename T> T get()
    {
        T v {};
        if (m_data.size() - m_pos < sizeof(T)) {
            m_ok = false;
            return v;
        }
        std::memcpy(&v, m_data.data() + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return v;
    }
    template <typename T> bool array(std::vector<T>& out, std::size_t n)
    {
        if ((m_data.size() - m_pos) / sizeof(T) < n) {
            m_ok = false;
            return false;
        }
        out.resize(n);
        std::memcpy(out.data(), m_data.data() + m_pos, n * sizeof(T));
        m_pos += n * sizeof(T);
        return true;
    }
    bool ok() const noexcept { return m_ok; }
    bool atEnd() const noexcept { return m_pos == m_data.size(); }

private:
    std::span<const char> m_data;
    std::size_t m_pos = 0;
    bool m_ok = true;
};

} // namespace

// Layout: version:u32 nextSegment:u64 contentEnd:u32 segmentCount:u32 number:u64[]
//   docCount:u32 entry:u32[] state:u8[] stamp:u32[] content:u32[] memoryCount:u32 pair:u64[]
//   textFile:u64 (0: none) textGarbage:u64 textCount:u32 (doc:u32 offset:u64 bytes:u32)[]
//   printCount:u32 (offset:u64 high:u64 low:u32)[]   the texts' fingerprints (textWithPrint)
std::vector<char> ContentIndex::serialize(const std::vector<EntryId>& newIds, std::vector<std::uint64_t>& segments) const
{
    std::shared_lock lock(m_mutex);
    Blob b;
    b.put(kStateVersion);
    b.put(m_nextSegment);
    b.put(m_contentEnd);
    b.put(static_cast<std::uint32_t>(m_segments.size()));
    segments.clear();
    for (const auto& s : m_segments) {
        b.put(s->number());
        segments.push_back(s->number());
    }
    const auto count = static_cast<std::uint32_t>(m_docEntry.size());
    b.put(count);
    std::vector<EntryId> entries(count, kNoEntry);
    std::vector<std::uint8_t> states(m_docState);
    std::vector<ContentId> contents(m_docContent);
    for (DocId d = 0; d < count; ++d) {
        const EntryId e = m_docEntry[d];
        if (!isDead(states[d]) && e < newIds.size() && newIds[e] != kNoEntry) {
            entries[d] = newIds[e];
        } else {
            states[d] = kDead;
            contents[d] = kNoContent;
        }
    }
    b.bytes(entries.data(), entries.size() * sizeof(EntryId));
    b.bytes(states.data(), states.size());
    b.bytes(m_docStamp.data(), m_docStamp.size() * sizeof(std::uint32_t));
    b.bytes(contents.data(), contents.size() * sizeof(ContentId));
    const std::size_t writing = m_writing ? m_writing->size() : 0; // the older ones
    b.put(static_cast<std::uint32_t>(writing + m_memory.size()));
    if (m_writing)
        b.bytes(m_writing->data(), writing * sizeof(std::uint64_t));
    b.bytes(m_memory.data(), m_memory.size() * sizeof(std::uint64_t));
    b.put(m_textFile ? m_textFile->number() : std::uint64_t {0});
    b.put(m_textGarbage);
    b.put(static_cast<std::uint32_t>(m_texts.size()));
    for (const auto& [doc, ref] : m_texts) {
        b.put(doc);
        b.put(ref.offset);
        b.put(ref.bytes);
    }
    b.put(static_cast<std::uint32_t>(m_textByPrint.size()));
    for (const auto& [print, offset] : m_textByPrint) {
        b.put(offset);
        b.put(print.high);
        b.put(print.low);
    }
    if (m_textFile)
        segments.push_back(m_textFile->number()); // kept by saved() as the segments are
    return b.take();
}

bool ContentIndex::restore(std::span<const char> data, std::size_t entryCount)
{
    std::unique_lock lock(m_mutex);
    m_flushed.wait(lock, [&] { return !m_flushing; }); // segment numbers start again from the state's
    resetLocked();
    m_obsolete.clear(); // whatever is on disk and unused goes below
    const auto parse = [&] {
        if (data.empty())
            return false;
        BlobReader r(data);
        if (r.get<std::uint32_t>() != kStateVersion)
            return false;
        m_nextSegment = std::max<std::uint64_t>(r.get<std::uint64_t>(), 1);
        const auto contentEnd = r.get<ContentId>();
        const auto segmentCount = r.get<std::uint32_t>();
        if (!r.ok() || segmentCount > 1024)
            return false;
        ContentId contentsBefore = 0;
        for (std::uint32_t i = 0; i < segmentCount; ++i) {
            const auto number = r.get<std::uint64_t>();
            if (!r.ok() || number >= m_nextSegment)
                return false;
            auto segment = Segment::open(segmentPath(number), number);
            if (!segment || segment->contentBegin() < contentsBefore || segment->contentEnd() > contentEnd)
                return false;
            contentsBefore = segment->contentEnd();
            m_segments.push_back(std::move(segment));
        }
        // Every content came with a document, and they go together.
        const auto count = r.get<std::uint32_t>();
        if (!r.ok() || count > kMaxIds || contentEnd > count || !r.array(m_docEntry, count)
            || !r.array(m_docState, count) || !r.array(m_docStamp, count) || !r.array(m_docContent, count))
            return false;
        for (DocId d = 0; d < count; ++d) {
            const std::uint8_t state = m_docState[d];
            if ((state & kBase) > kDead || (state & ~(kBase | kDirty | kUnsure)))
                return false;
            if (isDead(state)) {
                m_docEntry[d] = kNoEntry;
                m_docContent[d] = kNoContent;
                ++m_dead;
            } else if (m_docEntry[d] >= entryCount
                || (m_docContent[d] != kNoContent && m_docContent[d] >= contentEnd)) {
                return false;
            }
        }
        const auto pairs = r.get<std::uint32_t>();
        if (!r.ok() || !r.array(m_memory, pairs))
            return false;
        for (const std::uint64_t p : m_memory) {
            if ((p & kIdMask) >= contentEnd || (p & kIdMask) < contentsBefore)
                return false;
        }
        m_contentEnd = contentEnd;
        // The fingerprints of the contents in memory, from their grams.
        std::vector<std::pair<ContentId, grams::Key>> byContent;
        byContent.reserve(m_memory.size());
        for (const std::uint64_t p : m_memory)
            byContent.emplace_back(static_cast<ContentId>(p & kIdMask), p >> kIdBits);
        std::sort(byContent.begin(), byContent.end());
        std::vector<grams::Key> keys;
        for (std::size_t i = 0; i < byContent.size();) {
            const ContentId content = byContent[i].first;
            keys.clear();
            for (; i < byContent.size() && byContent[i].first == content; ++i)
                keys.push_back(byContent[i].second);
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
            if (const auto print = grams::fingerprint(keys))
                m_memoryContents.emplace(*print, content);
        }
        const auto textNumber = r.get<std::uint64_t>();
        m_textGarbage = r.get<std::uint64_t>();
        const auto texts = r.get<std::uint32_t>();
        if (!r.ok() || texts > count || textNumber >= m_nextSegment)
            return false;
        if (textNumber != 0)
            m_textFile = TextFile::open(textPath(textNumber), textNumber, false);
        for (std::uint32_t i = 0; i < texts; ++i) {
            const auto doc = r.get<DocId>();
            const auto offset = r.get<std::uint64_t>();
            const auto bytes = r.get<std::uint32_t>();
            if (!r.ok() || doc >= count)
                return false;
            if (isDead(m_docState[doc]))
                continue;
            const auto [use, first] = m_textUses.try_emplace(offset, TextUse {0, bytes, std::nullopt});
            if (!m_textFile || offset + bytes > m_textFile->size() || use->second.bytes != bytes) {
                if (first)
                    m_textUses.erase(use);
                m_docState[doc] |= kDirty; // its text is lost (the file was cut short): read it again
                continue;
            }
            ++use->second.docs;
            m_texts.emplace(doc, TextRef {offset, bytes});
        }
        const auto prints = r.get<std::uint32_t>();
        if (!r.ok() || prints > texts)
            return false;
        for (std::uint32_t i = 0; i < prints; ++i) {
            const auto offset = r.get<std::uint64_t>();
            const grams::Fingerprint print {r.get<std::uint64_t>(), r.get<std::uint32_t>()};
            if (!r.ok())
                return false;
            if (const auto use = m_textUses.find(offset); use != m_textUses.end() && !use->second.print) {
                use->second.print = print;
                m_textByPrint.emplace(print, offset);
            }
        }
        return r.atEnd();
    };
    const bool ok = parse();
    if (!ok)
        resetLocked();
    rebuildOrder();

    // Segment files nothing refers to: left by a crash, or by a state that
    // could not be restored (one of an older version, say).
    std::vector<std::uint64_t> used;
    for (const auto& s : m_segments)
        used.push_back(s->number());
    m_obsolete.clear();
    m_obsoleteTexts.clear();
    const QDir dir(m_directory);
    for (const QString& name : dir.entryList({QStringLiteral("*.grams"), QStringLiteral("*.texts")}, QDir::Files)) {
        bool numeric = false;
        const std::uint64_t number = QStringView(name).chopped(6).toULongLong(&numeric);
        const bool text = name.endsWith(u".texts");
        if (numeric)
            m_nextSegment = std::max(m_nextSegment, number + 1); // never reuse a name still on disk
        const bool inUse = numeric
            && (text ? m_textFile && m_textFile->number() == number
                     : std::find(used.begin(), used.end(), number) != used.end());
        if (!inUse && !QFile::remove(dir.filePath(name)) && numeric)
            (text ? m_obsoleteTexts : m_obsolete).push_back(number);
    }
    m_changed = !ok;
    return ok;
}

bool ContentIndex::relocate(QString directory)
{
    std::unique_lock lock(m_mutex);
    m_flushed.wait(lock, [&] { return !m_flushing; }); // its segment goes where the others are
    std::vector<std::shared_ptr<const Segment>> reopened;
    reopened.reserve(m_segments.size());
    for (const auto& s : m_segments) {
        auto segment = Segment::open(segmentPath(directory, s->number()), s->number());
        if (!segment)
            return false;
        reopened.push_back(std::move(segment));
    }
    std::shared_ptr<TextFile> textFile;
    if (m_textFile) {
        textFile = TextFile::open(textPath(directory, m_textFile->number()), m_textFile->number(), false);
        if (!textFile || textFile->size() != m_textFile->size())
            return false;
    }
    m_segments.swap(reopened); // searches hold the lock: the old ones are closed here
    m_textFile = std::move(textFile); // ... or once the last search reading from it is done
    m_directory = std::move(directory);
    return true;
}

std::vector<QString> ContentIndex::segmentPaths() const
{
    std::shared_lock lock(m_mutex);
    std::vector<QString> paths;
    paths.reserve(m_segments.size());
    for (const auto& s : m_segments)
        paths.push_back(segmentPath(s->number()));
    return paths;
}

void ContentIndex::saved(const std::vector<std::uint64_t>& saved)
{
    std::unique_lock lock(m_mutex);
    std::erase_if(m_obsolete, [&](std::uint64_t number) {
        if (std::find(saved.begin(), saved.end(), number) != saved.end())
            return false; // that snapshot still refers to it (merged away since)
        const QString path = segmentPath(number);
        return !QFile::exists(path) || QFile::remove(path);
    });
    std::erase_if(m_obsoleteTexts, [&](std::uint64_t number) {
        if (std::find(saved.begin(), saved.end(), number) != saved.end())
            return false;
        const QString path = textPath(number);
        return !QFile::exists(path) || QFile::remove(path);
    });
}

std::uint64_t ContentIndex::obsoleteBytes() const
{
    std::vector<QString> paths;
    {
        std::shared_lock lock(m_mutex);
        for (const std::uint64_t number : m_obsolete)
            paths.push_back(segmentPath(number));
        for (const std::uint64_t number : m_obsoleteTexts)
            paths.push_back(textPath(number));
    }
    std::uint64_t bytes = 0;
    for (const QString& path : paths)
        bytes += static_cast<std::uint64_t>(std::max<qint64>(QFileInfo(path).size(), 0)); // 0 when it is gone
    return bytes;
}

ContentIndex::Stats ContentIndex::stats() const
{
    std::shared_lock lock(m_mutex);
    Stats s;
    s.documents = m_docEntry.size() - m_dead;
    const std::vector<bool> live = liveContents();
    s.contents = static_cast<std::size_t>(std::count(live.begin(), live.end(), true));
    for (const std::uint8_t state : m_docState)
        s.pending += !isDead(state) && (state & (kDirty | kUnsure)) ? 1 : 0;
    s.segments = m_segments.size();
    for (const auto& segment : m_segments) {
        s.segmentBytes += segment->bytes();
        s.grams += segment->grams();
        s.postingBytes += segment->postingBytes();
    }
    s.memoryPairs = m_memory.size() + (m_writing ? m_writing->size() : 0);
    s.texts = m_texts.size();
    s.distinctTexts = m_textUses.size();
    s.textBytes = m_textFile ? m_textFile->size() : 0;
    return s;
}

} // namespace ws

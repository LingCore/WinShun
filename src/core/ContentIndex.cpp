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
#include <cstring>
#include <limits>
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
// 5: trigrams of any ASCII characters, spaces and punctuation too; 6: documents' texts
constexpr std::uint32_t kStateVersion = 6;
// The text file is rewritten once texts of dead documents take more than
// this, and more than the live ones.
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

// ---- segment files ----------------------------------------------------------------
//
//   Header
//   Print[printCount]     the fingerprints of the segment's contents, ascending
//   postings              by key; zero-padded to a multiple of 8 bytes
//   Block[blockCount + 1] every kBlockKeys-th key, where its posting and its
//                         table entry begin; the last is a sentinel
//   table                 per key, as LEB128: its distance from the key before
//                         (not for the first of a block), how many contents
//                         have it, then either those contents (up to kInline:
//                         the first's distance from contentBegin, then for
//                         each the contents skipped since the one before) or
//                         the size of its posting
//
// A posting holds the contents that have the key, in binary interpolative
// coding (Moffat and Stuiver): the middle one, in as few bits as the values
// left for it allow, then each half the same way. Contents of similar files
// get numbers close together (see merge), so the numbers come in clusters and
// runs, which this coding writes in next to nothing. The table comes last so
// that a merge can write the postings as it goes.
constexpr char kSegmentMagic[8] = {'Q', 'F', 'G', 'R', 'A', 'M', 'S', '\0'};
constexpr std::uint32_t kSegmentVersion = 4;

struct SegmentHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t keyCount;
    std::uint32_t contentBegin;
    std::uint32_t contentEnd;
    std::uint32_t level; // 0: written from memory; n + 1: merged from segments of level n
    std::uint32_t printCount;
    std::uint64_t postingBytes; // without the padding
    std::uint64_t tableBytes;
};
static_assert(sizeof(SegmentHeader) == 48);

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

// Writes a segment file as its postings come: only the table stays in memory.
// Nothing is left behind unless finish() succeeds.
class SegmentWriter {
public:
    // The contents are in [contentBegin, contentEnd); `prints` are theirs, ascending.
    SegmentWriter(const QString& path, std::uint32_t contentBegin, std::uint32_t contentEnd, std::uint32_t level,
        const std::vector<Print>& prints)
        : m_file(path)
        , m_contentBegin(contentBegin)
        , m_contentEnd(contentEnd)
        , m_level(level)
        , m_printCount(prints.size())
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        const SegmentHeader placeholder {};
        m_ok = m_file.open(QIODevice::WriteOnly) && put(&placeholder, sizeof placeholder)
            && put(prints.data(), prints.size() * sizeof(Print));
    }

    // Keys ascending; contents ascending (others are dropped).
    void add(grams::Key key, std::span<const std::uint32_t> contents)
    {
        const std::uint64_t at = offset();
        if (!m_ok || at > std::numeric_limits<std::uint32_t>::max()
            || m_table.size() > std::numeric_limits<std::uint32_t>::max() || (m_keys > 0 && key <= m_lastKey)) {
            m_ok = false;
            return;
        }
        m_contents.clear();
        for (const std::uint32_t c : contents) {
            if (c >= m_contentBegin && c < m_contentEnd && (m_contents.empty() || c > m_contents.back()))
                m_contents.push_back(c);
        }
        if (m_contents.empty())
            return;

        if (m_keys % kBlockKeys == 0)
            m_blocks.push_back({key, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(m_table.size())});
        else
            encode(m_table, key - m_lastKey);
        encode(m_table, m_contents.size());
        if (m_contents.size() <= kInline) {
            std::uint64_t next = m_contentBegin;
            for (const std::uint32_t c : m_contents) {
                encode(m_table, c - next);
                next = std::uint64_t {c} + 1;
            }
        } else {
            const std::size_t start = m_buffer.size();
            BitWriter bits(m_buffer);
            putInterpolative(bits, m_contents.data(), m_contents.size(), m_contentBegin, m_contentEnd - 1);
            bits.flush();
            encode(m_table, m_buffer.size() - start);
        }
        m_lastKey = key;
        ++m_keys;
        if (m_buffer.size() >= kBufferBytes)
            drain();
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
        header.postingBytes = postingBytes;
        header.tableBytes = m_table.size();
        const SegmentBlock sentinel {std::numeric_limits<grams::Key>::max(), static_cast<std::uint32_t>(postingBytes),
            static_cast<std::uint32_t>(m_table.size())};
        const std::uint64_t end = sizeof header + m_printCount * sizeof(Print) + postingBytes;
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
    bool m_ok = false;
    std::uint64_t m_written = 0; // posting bytes in the file
    std::vector<std::uint8_t> m_buffer; // posting bytes still to write
    std::vector<std::uint32_t> m_contents; // scratch: a key's contents, checked
    std::vector<SegmentBlock> m_blocks;
    std::vector<std::uint8_t> m_table;
    std::uint64_t m_keys = 0;
    grams::Key m_lastKey = 0;
};

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
    std::array<UCHAR, 32> digest {};
    // The keys are not written to: the parameter is not const only for C's sake.
    auto* const input = reinterpret_cast<PUCHAR>(const_cast<Key*>(keys.data()));
    if (keys.size_bytes() > std::numeric_limits<ULONG>::max()
        || !BCRYPT_SUCCESS(::BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, input,
            static_cast<ULONG>(keys.size_bytes()), digest.data(), static_cast<ULONG>(digest.size()))))
        return std::nullopt;
    Fingerprint out;
    std::memcpy(&out.high, digest.data(), sizeof out.high);
    std::memcpy(&out.low, digest.data() + sizeof out.high, sizeof out.low);
    return out;
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
            for (std::size_t i = 0; i < size; ++i) {
                const auto c = static_cast<unsigned char>(data[i]);
                if (c < 0x80)
                    ascii(c);
                else
                    endRun();
            }
        }
        break;
    }
}

// A decoder that keeps its state across chunks: m_carry holds the lead byte
// and the continuation bytes seen so far. Malformed bytes count as one
// non-indexed character each, which is where QString::fromUtf8 (what the
// scanner decodes with) puts its U+FFFD too.
void Collector::feedUtf8(const char* p, std::size_t n)
{
    const auto* b = reinterpret_cast<const unsigned char*>(p);
    std::size_t i = 0;
    while (i < n) {
        if (m_carry.empty()) {
            const unsigned char c = b[i++];
            if (c < 0x80) {
                ascii(c);
            } else if (c < 0xC2 || c > 0xF4) {
                m_previous = 0; // a byte that cannot start a character
                endRun();
            } else {
                m_carry.push_back(static_cast<char>(c));
            }
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
        const std::size_t length = lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        if (m_carry.size() < length)
            continue;
        char32_t cp = lead & (length == 2 ? 0x1F : length == 3 ? 0x0F : 0x07);
        for (std::size_t k = 1; k < length; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(m_carry[k]) & 0x3F);
        m_carry.clear();
        const char32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            m_previous = 0;
            endRun();
        } else {
            add(cp);
        }
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

    // Every key in order (for merging).
    class Cursor {
    public:
        explicit Cursor(const Segment& s) noexcept
            : m_s(&s)
            , m_reader(s.m_table, s.m_table)
        {
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
        || h.postingBytes > std::numeric_limits<std::uint32_t>::max()
        || h.tableBytes > std::numeric_limits<std::uint32_t>::max())
        return nullptr;
    const std::uint64_t postingsAt = sizeof(SegmentHeader) + std::uint64_t {h.printCount} * sizeof(Print);
    const std::uint64_t blocksAt = aligned8(postingsAt + h.postingBytes);
    const std::uint64_t tableAt = blocksAt + (blocksFor(h.keyCount) + 1) * sizeof(SegmentBlock);
    if (tableAt + h.tableBytes != s->m_size)
        return nullptr;
    s->m_prints = reinterpret_cast<const Print*>(bytes + sizeof(SegmentHeader));
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
    const auto blockCount = static_cast<std::uint32_t>(blocksFor(h.keyCount));
    if (m_blocks[0].entry != 0 || m_blocks[blockCount].entry != h.tableBytes
        || m_blocks[blockCount].posting != h.postingBytes)
        return false;
    const std::uint64_t universe = h.contentEnd - h.contentBegin;
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
                std::uint64_t next = h.contentBegin; // the first content the next number can stand for
                for (std::uint64_t k = 0; k < count; ++k) {
                    const std::uint64_t skipped = r.next();
                    if (!r.ok() || skipped >= h.contentEnd - next)
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
        std::uint64_t next = m_header.contentBegin;
        for (std::uint64_t i = 0; i < e.count; ++i) {
            next += r.next();
            out.push_back(static_cast<ContentId>(next++));
        }
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
    if (!m_textFile) {
        QDir().mkpath(m_directory);
        const std::uint64_t number = m_nextSegment++;
        m_textFile = TextFile::open(textPath(number), number, true);
    }
    return m_textFile;
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
    if (const auto it = m_texts.find(doc); it != m_texts.end()) {
        m_textGarbage += it->second.bytes;
        m_texts.erase(it);
    }
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
    for (const std::uint64_t p : m_memory) {
        if ((p >> kIdBits) == key)
            out.push_back(static_cast<ContentId>(p & kIdMask));
    }
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
    for (const auto& s : m_segments)
        m_obsolete.push_back(s->number());
    m_segments.clear();
    m_memory = decltype(m_memory)();
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
    // Before taking the lock: the grams of a large file take a while to hash.
    const std::optional<grams::Fingerprint> print = hasGrams ? grams::fingerprint(keys) : std::nullopt;
    // The text goes to the file first (outside the lock: searches read meanwhile).
    std::shared_ptr<TextFile> textFile;
    std::optional<std::uint64_t> textAt;
    if (!text.isEmpty() && text.size() <= 0x7FFF'FFFF) {
        textFile = textFileForAppend();
        if (textFile)
            textAt = textFile->append(text);
    }
    std::unique_lock lock(m_mutex);
    if (textAt && textFile != m_textFile) { // replaced meanwhile (compactTexts, clear)
        textFile = m_textFile;
        textAt = textFile ? textFile->append(text) : std::nullopt;
    }
    const auto refuse = [&] {
        if (textAt)
            m_textGarbage += static_cast<std::uint64_t>(text.size());
        return false;
    };
    if (m_docEntry.size() >= kMaxIds)
        return refuse();
    ContentId content = print ? findContent(*print) : kNoContent; // a copy's, say
    if (hasGrams && content == kNoContent) {
        if (!m_memory.empty() && m_memory.size() + keys.size() > kFlushPairs)
            flushLocked();
        if ((!m_memory.empty() && m_memory.size() + keys.size() > kMaxMemoryPairs) || m_contentEnd >= kMaxIds)
            return refuse(); // segments cannot be written
        content = m_contentEnd++;
        if (print)
            m_memoryContents.emplace(*print, content);
        for (const grams::Key key : keys)
            m_memory.push_back((key << kIdBits) | content);
    }
    if (const DocId old = findDoc(entry); old != kNoDoc)
        kill(old);
    const auto doc = static_cast<DocId>(m_docEntry.size());
    std::uint8_t state = empty ? kEmpty : kLive;
    if (const auto it = m_changedWhileReading.find(entry); it != m_changedWhileReading.end() && it->second > since)
        state |= kDirty; // written to while it was being read
    m_docEntry.push_back(entry);
    m_docState.push_back(state);
    m_docStamp.push_back(stamp);
    m_docContent.push_back(content);
    insertOrder(doc);
    if (textAt)
        m_texts[doc] = {*textAt, static_cast<std::uint32_t>(text.size())};
    m_changed = true;
    if (m_memory.size() >= kFlushPairs)
        flushLocked();
    return true;
}

bool ContentIndex::flushLocked()
{
    if (m_memory.empty())
        return true;
    std::sort(m_memory.begin(), m_memory.end());
    std::vector<Print> prints; // ascending, as the map has them
    prints.reserve(m_memoryContents.size());
    for (const auto& [print, content] : m_memoryContents)
        prints.push_back({print.high, print.low, content});
    const std::uint64_t number = m_nextSegment++;
    SegmentWriter writer(segmentPath(number), memoryBegin(), m_contentEnd, 0, prints);
    std::vector<ContentId> contents;
    for (std::size_t i = 0; i < m_memory.size();) {
        const grams::Key key = m_memory[i] >> kIdBits;
        contents.clear();
        for (; i < m_memory.size() && (m_memory[i] >> kIdBits) == key; ++i)
            contents.push_back(static_cast<ContentId>(m_memory[i] & kIdMask));
        writer.add(key, contents);
    }
    if (!writer.finish())
        return false;
    auto segment = Segment::open(segmentPath(number), number);
    if (!segment) {
        m_obsolete.push_back(number);
        return false;
    }
    m_segments.push_back(std::move(segment));
    m_memory = decltype(m_memory)();
    m_memoryContents.clear();
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

bool ContentIndex::mergeDue()
{
    for (;;) {
        std::size_t count = 0;
        {
            std::shared_lock lock(m_mutex);
            if (mergeAllDueLocked())
                count = std::numeric_limits<std::size_t>::max();
            else
                count = dueTailLocked();
        }
        if (count == 0)
            return true;
        if (count == std::numeric_limits<std::size_t>::max())
            return merge();
        if (!mergeTail(count))
            return false;
    }
}

// The last `count` segments into one of the next level. Contents keep their
// numbers (merge() renumbers them); those no document has any more are dropped.
bool ContentIndex::mergeTail(std::size_t count)
{
    std::vector<std::shared_ptr<const Segment>> segments;
    std::vector<bool> live;
    std::size_t docCount = 0;
    std::uint64_t number = 0;
    QString path;
    {
        std::unique_lock lock(m_mutex);
        if (count < 2 || count > m_segments.size())
            return false;
        segments.assign(m_segments.end() - static_cast<std::ptrdiff_t>(count), m_segments.end());
        live = liveContents();
        docCount = m_docEntry.size();
        number = m_nextSegment++;
        path = segmentPath(number);
    }
    std::uint32_t level = 0;
    for (const auto& s : segments)
        level = std::max(level, s->level() + 1);
    level = std::min(level, kTopLevel - 1);
    const auto kept = [&](ContentId c) { return c < live.size() && live[c]; };

    std::vector<Print> prints;
    for (const auto& s : segments) {
        for (const Print& p : s->prints()) {
            if (kept(p.content))
                prints.push_back(p);
        }
    }
    std::sort(prints.begin(), prints.end(),
        [](const Print& a, const Print& b) { return a.fingerprint() < b.fingerprint(); });
    SegmentWriter writer(path, segments.front()->contentBegin(), segments.back()->contentEnd(), level, prints);
    std::vector<Segment::Cursor> cursors;
    for (const auto& s : segments)
        cursors.emplace_back(*s);
    std::vector<ContentId> contents;
    for (;;) {
        grams::Key key = std::numeric_limits<grams::Key>::max();
        bool any = false;
        for (const Segment::Cursor& c : cursors) {
            if (!c.done()) {
                key = std::min(key, c.key());
                any = true;
            }
        }
        if (!any)
            break;
        contents.clear();
        for (Segment::Cursor& c : cursors) {
            if (!c.done() && c.key() == key) {
                c.contents(contents); // ascending: the segments hold ascending ranges
                c.next();
            }
        }
        std::erase_if(contents, [&](ContentId c) { return !kept(c); });
        writer.add(key, contents);
    }
    auto merged = writer.finish() ? Segment::open(path, number) : nullptr;

    std::unique_lock lock(m_mutex);
    // The same segments must still be there, side by side (clear() may have
    // dropped them meanwhile), and no document added: it could have a content
    // dropped here.
    const auto at = std::find(m_segments.begin(), m_segments.end(), segments.front());
    if (!merged || m_docEntry.size() != docCount || static_cast<std::size_t>(m_segments.end() - at) < count
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
// document has, and the contents numbered anew: by the smallest scrambled key
// among their grams (a MinHash), then by how many grams they have. Contents
// that have most grams in common most likely share their smallest one, so
// those of similar files (versions of a file, files of a kind) end up side by
// side, and a gram's contents come in runs, which postings write in next to
// nothing.
bool ContentIndex::merge()
{
    // Read from what is there now, without the lock: segments never change,
    // and documents are only added between merges (by the same caller).
    std::vector<std::shared_ptr<const Segment>> segments;
    std::vector<std::uint64_t> memory;
    std::vector<Print> prints;
    std::vector<bool> live;
    std::size_t docCount = 0;
    ContentId contentEnd = 0;
    std::uint64_t number = 0;
    QString path;
    {
        std::unique_lock lock(m_mutex);
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
    std::sort(memory.begin(), memory.end());

    // Calls f(key, contents) for every key of every segment and of memory, in
    // order. Segments hold ascending ranges of contents, memory the newest:
    // concatenated, each key's contents stay sorted.
    const auto forEachKey = [&](auto&& f) {
        std::vector<Segment::Cursor> cursors;
        for (const auto& s : segments)
            cursors.emplace_back(*s);
        std::size_t m = 0;
        std::vector<ContentId> contents;
        for (;;) {
            grams::Key key = std::numeric_limits<grams::Key>::max();
            bool any = false;
            for (const Segment::Cursor& c : cursors) {
                if (!c.done()) {
                    key = std::min(key, c.key());
                    any = true;
                }
            }
            if (m < memory.size()) {
                key = std::min(key, memory[m] >> kIdBits);
                any = true;
            }
            if (!any)
                break;
            contents.clear();
            for (Segment::Cursor& c : cursors) {
                if (!c.done() && c.key() == key) {
                    c.contents(contents);
                    c.next();
                }
            }
            for (; m < memory.size() && (memory[m] >> kIdBits) == key; ++m)
                contents.push_back(static_cast<ContentId>(memory[m] & kIdMask));
            f(key, contents);
        }
    };

    std::vector<ContentId> newId(contentEnd, kNoContent);
    ContentId kept = 0;
    {
        std::vector<std::uint64_t> smallest(contentEnd, std::numeric_limits<std::uint64_t>::max());
        std::vector<std::uint32_t> size(contentEnd, 0);
        forEachKey([&](grams::Key key, const std::vector<ContentId>& contents) {
            const std::uint64_t scrambled = scramble(key);
            for (const ContentId c : contents) {
                if (c < contentEnd && live[c]) {
                    smallest[c] = std::min(smallest[c], scrambled);
                    ++size[c];
                }
            }
        });
        std::vector<ContentId> order;
        for (ContentId c = 0; c < contentEnd; ++c) {
            if (size[c] > 0)
                order.push_back(c);
        }
        std::sort(order.begin(), order.end(), [&](ContentId a, ContentId b) {
            return std::tie(smallest[a], size[a], a) < std::tie(smallest[b], size[b], b);
        });
        for (const ContentId c : order)
            newId[c] = kept++;
    }
    std::vector<Print> keptPrints;
    for (const Print& p : prints) {
        if (p.content < contentEnd && newId[p.content] != kNoContent)
            keptPrints.push_back({p.high, p.low, newId[p.content]});
    }
    std::sort(keptPrints.begin(), keptPrints.end(),
        [](const Print& a, const Print& b) { return a.fingerprint() < b.fingerprint(); });
    prints = {};

    SegmentWriter writer(path, 0, kept, kTopLevel, keptPrints);
    std::vector<ContentId> mapped;
    forEachKey([&](grams::Key key, const std::vector<ContentId>& contents) {
        mapped.clear();
        for (const ContentId c : contents) {
            if (c < contentEnd && newId[c] != kNoContent)
                mapped.push_back(newId[c]);
        }
        std::sort(mapped.begin(), mapped.end());
        writer.add(key, mapped);
    });
    auto merged = writer.finish() ? Segment::open(path, number) : nullptr;

    std::unique_lock lock(m_mutex);
    if (!merged || m_docEntry.size() != docCount || m_memory.size() != memory.size() || m_contentEnd != contentEnd) {
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
    m_segments = {std::move(merged)};
    m_memory = decltype(m_memory)();
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
bool ContentIndex::compactTexts()
{
    std::unordered_map<DocId, TextRef> texts;
    std::shared_ptr<TextFile> old;
    std::uint64_t number = 0;
    {
        std::unique_lock lock(m_mutex);
        if (!m_textFile)
            return true;
        texts = m_texts;
        old = m_textFile;
        number = m_nextSegment++;
    }
    auto fresh = TextFile::open(textPath(number), number, true);
    std::unordered_map<DocId, TextRef> moved;
    QByteArray buffer;
    bool ok = fresh != nullptr;
    for (const auto& [doc, ref] : texts) {
        if (!ok)
            break;
        if (!old->read(ref.offset, ref.bytes, buffer))
            continue; // lost: the document's file is read again when a search needs it
        const auto at = fresh->append(buffer);
        ok = at.has_value();
        if (ok)
            moved.emplace(doc, TextRef {*at, ref.bytes});
    }
    std::unique_lock lock(m_mutex);
    if (!ok || m_textFile != old) {
        fresh.reset();
        m_obsoleteTexts.push_back(number);
        return false;
    }
    std::unordered_map<DocId, TextRef> next;
    for (const auto& [doc, ref] : m_texts) {
        std::optional<TextRef> now;
        if (const auto was = texts.find(doc); was != texts.end() && was->second == ref) {
            if (const auto it = moved.find(doc); it != moved.end())
                now = it->second;
        } else if (old->read(ref.offset, ref.bytes, buffer)) { // came meanwhile (from a search)
            if (const auto at = fresh->append(buffer))
                now = TextRef {*at, ref.bytes};
        }
        if (now)
            next.emplace(doc, *now);
        else
            m_docState[doc] |= kDirty; // its text could not be read: the file is read again
    }
    m_texts.swap(next);
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
    b.put(static_cast<std::uint32_t>(m_memory.size()));
    b.bytes(m_memory.data(), m_memory.size() * sizeof(std::uint64_t));
    b.put(m_textFile ? m_textFile->number() : std::uint64_t {0});
    b.put(m_textGarbage);
    b.put(static_cast<std::uint32_t>(m_texts.size()));
    for (const auto& [doc, ref] : m_texts) {
        b.put(doc);
        b.put(ref.offset);
        b.put(ref.bytes);
    }
    if (m_textFile)
        segments.push_back(m_textFile->number()); // kept by saved() as the segments are
    return b.take();
}

bool ContentIndex::restore(std::span<const char> data, std::size_t entryCount)
{
    std::unique_lock lock(m_mutex);
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
            if (!m_textFile || offset + bytes > m_textFile->size()) {
                m_docState[doc] |= kDirty; // its text is lost (the file was cut short): read it again
                continue;
            }
            m_texts.emplace(doc, TextRef {offset, bytes});
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
    s.memoryPairs = m_memory.size();
    s.texts = m_texts.size();
    s.textBytes = m_textFile ? m_textFile->size() : 0;
    return s;
}

} // namespace ws

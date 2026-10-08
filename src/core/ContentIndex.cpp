#include "ContentIndex.h"

#include "ContentScanner.h"
#include "TextUtil.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace ws {

namespace {

constexpr unsigned kDocBits = 28; // m_memory packs key << kDocBits | doc
constexpr std::uint64_t kDocMask = (std::uint64_t {1} << kDocBits) - 1;
constexpr std::size_t kMaxDocs = std::size_t {1} << kDocBits;
constexpr std::uint64_t kCharMask = (std::uint64_t {1} << grams::kCharBits) - 1;
constexpr std::size_t kFlushPairs = std::size_t {1} << 20; // 8 MB of grams in memory make a segment
constexpr std::size_t kMaxMemoryPairs = kFlushPairs * 4; // ... unless segments cannot be written
constexpr std::size_t kMaxSegments = 64; // more (whatever their levels) are merged into one
constexpr std::size_t kOrderTail = 1024;
constexpr std::uint32_t kStateVersion = 3; // 2: ASCII trigrams; 3: dense documents, segment files v3

// m_docState: a base state and flags.
constexpr std::uint8_t kLive = 0; // its grams are in the index
constexpr std::uint8_t kEmpty = 1; // nothing to find in it: empty, too large, unreadable
constexpr std::uint8_t kDead = 2; // replaced by a newer document, or its file is gone
constexpr std::uint8_t kBase = 3;
constexpr std::uint8_t kDirty = 0x04; // written to since it was read: read it again
constexpr std::uint8_t kUnsure = 0x08; // may have changed unseen: compare its size and time
constexpr std::uint8_t kDense = 0x10; // has most trigrams: segments keep those as bitmaps

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
//   DocId[denseCount]     the dense documents (kDense), ascending
//   postings              by key; zero-padded to a multiple of 8 bytes
//   Block[blockCount + 1] every kBlockKeys-th key, where its posting and its
//                         table entry begin; the last is a sentinel
//   table                 per key, as LEB128: its distance from the key before
//                         (not for the first of a block), then size << 2 | coding
//
// A posting lists the documents that have the key as numbers: the first's
// distance from docBegin, then for each the documents skipped since the one
// before. Coding says how they are written; size is the posting's length in
// bytes. A trigram's posting starts with a bitmap of the dense documents that
// have it (denseCount bits, not counted in size): a dense document has most
// trigrams, and a row of bits per trigram takes less room than listing it in
// every posting. The table comes last so that a merge can write the postings
// as it goes.
constexpr char kSegmentMagic[8] = {'Q', 'F', 'G', 'R', 'A', 'M', 'S', '\0'};
constexpr std::uint32_t kSegmentVersion = 3;

struct SegmentHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t keyCount;
    std::uint32_t docBegin;
    std::uint32_t docEnd;
    std::uint32_t level; // 0: written from memory; n + 1: merged from segments of level n
    std::uint32_t denseCount;
    std::uint64_t postingBytes; // without the padding
    std::uint64_t tableBytes;
};
static_assert(sizeof(SegmentHeader) == 48);

struct SegmentBlock {
    grams::Key key; // its first
    std::uint32_t posting; // offset of the first's posting
    std::uint32_t entry; // offset of the first's table entry
};
static_assert(sizeof(SegmentBlock) == 16);

enum Coding : std::uint8_t {
    kGaps, // each number as LEB128
    kPacked, // their count as LEB128; then per kPackedRun numbers a byte w and the numbers in w bits each
    kBitmap, // a bit per document of the segment
};

constexpr std::uint32_t kTopLevel = 0xFFFF; // merged from every segment
constexpr std::size_t kMergeFactor = 8; // this many segments of a level in a row make one of the next level
constexpr std::uint32_t kBlockKeys = 64;
constexpr std::size_t kPackedRun = 128;
// A document with more distinct trigrams than this (a fifth of them all) is
// dense: a bitmap row per trigram costs it 6 KB, listing it under each more.
constexpr std::size_t kDenseTrigrams = 10000;

constexpr std::uint64_t aligned8(std::uint64_t n) noexcept
{
    return (n + 7) & ~std::uint64_t {7};
}

constexpr std::uint64_t blocksFor(std::uint64_t keys) noexcept
{
    return (keys + kBlockKeys - 1) / kBlockKeys;
}

constexpr bool isTrigram(grams::Key key) noexcept
{
    return (key >> grams::kCharBits) < 0x80;
}

void encode(std::vector<std::uint8_t>& out, std::uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

unsigned encodedSize(std::uint64_t v) noexcept
{
    unsigned n = 1;
    for (; v >= 0x80; v >>= 7)
        ++n;
    return n;
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

// Appends the documents of a posting (without its bitmap): ascending, in
// [docBegin, docEnd). Stops at anything malformed.
void decode(std::span<const std::uint8_t> posting, Coding coding, std::uint32_t docBegin, std::uint32_t docEnd,
    std::vector<std::uint32_t>& out)
{
    const std::uint64_t end = docEnd;
    std::uint64_t next = docBegin; // the first document the next number can stand for
    const auto take = [&](std::uint64_t skipped) {
        if (skipped >= end - next)
            return false;
        next += skipped;
        out.push_back(static_cast<std::uint32_t>(next++));
        return true;
    };
    switch (coding) {
    case kGaps: {
        NumberReader r(posting.data(), posting.data() + posting.size());
        while (!r.atEnd()) {
            const std::uint64_t v = r.next();
            if (!r.ok() || !take(v))
                return;
        }
        return;
    }
    case kPacked: {
        NumberReader r(posting.data(), posting.data() + posting.size());
        std::uint64_t count = r.next();
        if (!r.ok() || count > end - next)
            return;
        const std::uint8_t* p = r.position();
        const std::uint8_t* const stop = posting.data() + posting.size();
        while (count > 0) {
            if (p >= stop || *p > 32)
                return;
            const unsigned width = *p++;
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(count, kPackedRun));
            if ((n * width + 7) / 8 > static_cast<std::size_t>(stop - p))
                return;
            const std::uint64_t mask = (std::uint64_t {1} << width) - 1;
            std::uint64_t bits = 0;
            unsigned have = 0;
            for (std::size_t i = 0; i < n; ++i) {
                for (; have < width; have += 8)
                    bits |= std::uint64_t {*p++} << have;
                const std::uint64_t v = bits & mask;
                bits >>= width;
                have -= width;
                if (!take(v))
                    return;
            }
            count -= n;
        }
        return;
    }
    case kBitmap: {
        if (posting.size() != (end - next + 7) / 8)
            return;
        for (std::size_t i = 0; i < posting.size(); i += 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, posting.data() + i, std::min<std::size_t>(8, posting.size() - i));
            for (; word != 0; word &= word - 1) {
                const std::uint64_t doc = docBegin + i * 8 + static_cast<unsigned>(std::countr_zero(word));
                if (doc >= end)
                    return;
                out.push_back(static_cast<std::uint32_t>(doc));
            }
        }
        return;
    }
    }
}

// How many documents decode() finds, near enough to order postings by.
std::uint64_t countOf(std::span<const std::uint8_t> posting, Coding coding) noexcept
{
    switch (coding) {
    case kGaps:
        return static_cast<std::uint64_t>(
            std::count_if(posting.begin(), posting.end(), [](std::uint8_t b) { return b < 0x80; }));
    case kPacked: {
        NumberReader r(posting.data(), posting.data() + posting.size());
        return r.next();
    }
    case kBitmap:
        break;
    }
    std::uint64_t n = 0;
    for (const std::uint8_t b : posting)
        n += static_cast<unsigned>(std::popcount(b));
    return n;
}

// Writes a segment file as its postings come: only the table stays in memory.
// Nothing is left behind unless finish() succeeds.
class SegmentWriter {
public:
    // The documents are in [docBegin, docEnd); `dense` are the dense ones (kDense).
    SegmentWriter(const QString& path, std::uint32_t docBegin, std::uint32_t docEnd, std::uint32_t level,
        std::vector<std::uint32_t> dense)
        : m_file(path)
        , m_docBegin(docBegin)
        , m_docEnd(docEnd)
        , m_level(level)
        , m_dense(std::move(dense))
        , m_rowBytes((m_dense.size() + 7) / 8)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        const SegmentHeader placeholder {};
        m_ok = m_file.open(QIODevice::WriteOnly) && put(&placeholder, sizeof placeholder)
            && put(m_dense.data(), m_dense.size() * sizeof(std::uint32_t));
    }

    // Keys ascending; docs ascending (others are dropped).
    void add(grams::Key key, std::span<const std::uint32_t> docs)
    {
        const std::uint64_t at = offset();
        if (!m_ok || at > std::numeric_limits<std::uint32_t>::max()
            || m_table.size() > std::numeric_limits<std::uint32_t>::max() || (m_keys > 0 && key <= m_lastKey)) {
            m_ok = false;
            return;
        }
        const bool row = isTrigram(key) && !m_dense.empty();
        if (row)
            m_buffer.resize(m_buffer.size() + m_rowBytes, 0);
        std::uint8_t* const bits = m_buffer.data() + m_buffer.size() - (row ? m_rowBytes : 0);
        m_docs.clear();
        std::uint64_t next = m_docBegin;
        std::size_t j = 0;
        for (const std::uint32_t d : docs) {
            if (d < next || d >= m_docEnd)
                continue;
            next = std::uint64_t {d} + 1;
            if (row) {
                for (; j < m_dense.size() && m_dense[j] < d; ++j) {}
                if (j < m_dense.size() && m_dense[j] == d) {
                    bits[j / 8] |= static_cast<std::uint8_t>(1u << (j % 8));
                    continue;
                }
            }
            m_docs.push_back(d);
        }
        const std::size_t start = m_buffer.size();
        const Coding coding = put(m_docs);
        const std::uint64_t size = m_buffer.size() - start;

        if (m_keys % kBlockKeys == 0)
            m_blocks.push_back({key, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(m_table.size())});
        else
            encode(m_table, key - m_lastKey);
        encode(m_table, size << 2 | coding);
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
        header.docBegin = m_docBegin;
        header.docEnd = m_docEnd;
        header.level = m_level;
        header.denseCount = static_cast<std::uint32_t>(m_dense.size());
        header.postingBytes = postingBytes;
        header.tableBytes = m_table.size();
        const SegmentBlock sentinel {std::numeric_limits<grams::Key>::max(), static_cast<std::uint32_t>(postingBytes),
            static_cast<std::uint32_t>(m_table.size())};
        const std::uint64_t end = sizeof header + m_dense.size() * sizeof(std::uint32_t) + postingBytes;
        constexpr char zeros[8] = {};
        return put(zeros, static_cast<std::size_t>(aligned8(end) - end))
            && put(m_blocks.data(), m_blocks.size() * sizeof(SegmentBlock)) && put(&sentinel, sizeof sentinel)
            && put(m_table.data(), m_table.size()) && m_file.seek(0) && put(&header, sizeof header) && m_file.commit();
    }

private:
    static constexpr std::size_t kBufferBytes = 1 << 20;

    // Appends the documents in whichever coding is shortest.
    Coding put(std::span<const std::uint32_t> docs)
    {
        std::uint64_t gaps = 0;
        std::uint64_t packed = encodedSize(docs.size());
        std::uint64_t next = m_docBegin;
        for (std::size_t i = 0; i < docs.size(); i += kPackedRun) {
            const std::size_t n = std::min(kPackedRun, docs.size() - i);
            std::uint32_t any = 0;
            for (std::size_t k = i; k < i + n; ++k) {
                const auto v = static_cast<std::uint32_t>(docs[k] - next);
                next = std::uint64_t {docs[k]} + 1;
                gaps += encodedSize(v);
                any |= v;
            }
            packed += 1 + (n * static_cast<unsigned>(std::bit_width(any)) + 7) / 8;
        }
        const std::uint64_t bitmap = (std::uint64_t {m_docEnd} - m_docBegin + 7) / 8;

        next = m_docBegin;
        if (gaps <= packed && gaps <= bitmap) {
            for (const std::uint32_t d : docs) {
                encode(m_buffer, d - next);
                next = std::uint64_t {d} + 1;
            }
            return kGaps;
        }
        if (packed <= bitmap) {
            encode(m_buffer, docs.size());
            for (std::size_t i = 0; i < docs.size(); i += kPackedRun) {
                const std::size_t n = std::min(kPackedRun, docs.size() - i);
                std::uint32_t any = 0;
                std::uint64_t from = next;
                for (std::size_t k = i; k < i + n; ++k) {
                    any |= static_cast<std::uint32_t>(docs[k] - from);
                    from = std::uint64_t {docs[k]} + 1;
                }
                const auto width = static_cast<unsigned>(std::bit_width(any));
                m_buffer.push_back(static_cast<std::uint8_t>(width));
                std::uint64_t bits = 0;
                unsigned have = 0;
                for (std::size_t k = i; k < i + n; ++k) {
                    bits |= (docs[k] - next) << have;
                    next = std::uint64_t {docs[k]} + 1;
                    for (have += width; have >= 8; have -= 8) {
                        m_buffer.push_back(static_cast<std::uint8_t>(bits));
                        bits >>= 8;
                    }
                }
                if (have > 0)
                    m_buffer.push_back(static_cast<std::uint8_t>(bits));
            }
            return kPacked;
        }
        const std::size_t at = m_buffer.size();
        m_buffer.resize(at + bitmap, 0);
        for (const std::uint32_t d : docs) {
            const std::uint32_t i = d - m_docBegin;
            m_buffer[at + i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
        }
        return kBitmap;
    }

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
    std::uint32_t m_docBegin;
    std::uint32_t m_docEnd;
    std::uint32_t m_level;
    std::vector<std::uint32_t> m_dense;
    std::size_t m_rowBytes;
    bool m_ok = false;
    std::uint64_t m_written = 0; // posting bytes in the file
    std::vector<std::uint8_t> m_buffer; // posting bytes still to write
    std::vector<std::uint32_t> m_docs; // scratch: a posting's documents that are not dense
    std::vector<SegmentBlock> m_blocks;
    std::vector<std::uint8_t> m_table;
    std::uint64_t m_keys = 0;
    grams::Key m_lastKey = 0;
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

constexpr char kWordText[] = "0123456789_abcdefghijklmnopqrstuvwxyz"; // by Collector::wordClass - 1
constexpr unsigned kTrigrams = Collector::kWordChars * Collector::kWordChars * Collector::kWordChars;

// Three word classes (1..37) in a row.
constexpr Key trigram(unsigned a, unsigned b, unsigned c) noexcept
{
    return (Key {static_cast<unsigned char>(kWordText[a - 1])} << kCharBits)
        | (Key {static_cast<unsigned char>(kWordText[b - 1])} << 8) | static_cast<unsigned char>(kWordText[c - 1]);
}

} // namespace

unsigned Collector::wordClass(char32_t c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0' + 1;
    if (c == '_')
        return 11;
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 12;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 12;
    return 0;
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
    std::vector<unsigned> word; // word characters in a row, as Collector::wordClass
    const auto endRun = [&] {
        if (run.size() == 1)
            keys.push_back(single(run[0]));
        for (std::size_t i = 1; i < run.size(); ++i)
            keys.push_back(pair(run[i - 1], run[i]));
        run.clear();
    };
    const auto endWord = [&] {
        for (std::size_t i = 2; i < word.size(); ++i)
            keys.push_back(trigram(word[i - 2], word[i - 1], word[i]));
        word.clear();
    };
    forEachCodePoint(phrase, [&](char32_t c) {
        if (isIndexed(c))
            run.push_back(c);
        else
            endRun();
        if (const unsigned w = Collector::wordClass(c))
            word.push_back(w);
        else
            endWord();
    });
    endRun();
    endWord();
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
                    m_word1 = m_word2 = 0;
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
                m_word1 = m_word2 = 0;
            } else {
                m_carry.push_back(static_cast<char>(c));
            }
            continue;
        }
        const unsigned char c = b[i];
        if ((c & 0xC0) != 0x80) {
            m_carry.clear(); // cut short: the byte starts something new
            m_previous = 0;
            m_word1 = m_word2 = 0;
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
            m_word1 = m_word2 = 0;
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
        m_word1 = m_word2 = 0;
    } else if (QChar::isLowSurrogate(u)) {
        m_previous = 0;
        m_word1 = m_word2 = 0;
    } else {
        add(u);
    }
}

// A character that is not ASCII.
void Collector::add(char32_t c)
{
    m_word1 = m_word2 = 0;
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
            keys.push_back(trigram(t / (kWordChars * kWordChars) + 1, t / kWordChars % kWordChars + 1, t % kWordChars + 1));
        }
    }
    keys.insert(keys.end(), m_keys.begin(), m_keys.end());
    m_keys = {};
    return keys;
}

} // namespace grams

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
    std::uint32_t docBegin() const noexcept { return m_header.docBegin; }
    std::uint32_t docEnd() const noexcept { return m_header.docEnd; }
    std::uint32_t level() const noexcept { return m_header.level; }

    // A key's table entry.
    struct Entry {
        grams::Key key = 0;
        std::uint64_t offset = 0; // of its posting
        std::uint64_t size = 0; // of its posting, without the bitmap of dense documents
        Coding coding = kGaps;
    };
    bool find(grams::Key key, Entry& out) const noexcept;
    void docs(const Entry& e, std::vector<DocId>& out) const; // appends them, ascending
    std::uint64_t count(const Entry& e) const noexcept; // of docs(), near enough

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
        void docs(std::vector<DocId>& out) const { m_s->docs(m_entry, out); }
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
                m_offset = block->posting;
            } else {
                m_entry.key += m_reader.next();
            }
            const std::uint64_t v = m_reader.next();
            m_entry.offset = m_offset;
            m_entry.size = v >> 2;
            m_entry.coding = static_cast<Coding>(v & 3);
            m_offset += m_s->rowBytes(m_entry.key) + m_entry.size;
            ++m_index;
        }

    private:
        const Segment* m_s;
        NumberReader m_reader;
        std::uint32_t m_index = 0; // of the next entry
        std::uint64_t m_offset = 0; // of the next posting
        Entry m_entry;
        bool m_done = false;
    };

private:
    explicit Segment(std::uint64_t number)
        : m_number(number)
    {
    }
    bool validate() const noexcept;
    std::uint64_t rowBytes(grams::Key key) const noexcept { return isTrigram(key) ? m_rowBytes : 0; }

    std::uint64_t m_number;
    win32::UniqueHandle m_file;
    win32::UniqueHandle m_mapping;
    void* m_view = nullptr;
    std::uint64_t m_size = 0;
    SegmentHeader m_header {};
    std::uint64_t m_rowBytes = 0; // of a bitmap of dense documents
    const std::uint32_t* m_dense = nullptr;
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
    if (h.docBegin > h.docEnd || h.docEnd > kMaxDocs || h.denseCount > h.docEnd - h.docBegin
        || h.postingBytes > std::numeric_limits<std::uint32_t>::max()
        || h.tableBytes > std::numeric_limits<std::uint32_t>::max())
        return nullptr;
    const std::uint64_t postingsAt = sizeof(SegmentHeader) + std::uint64_t {h.denseCount} * sizeof(std::uint32_t);
    const std::uint64_t blocksAt = aligned8(postingsAt + h.postingBytes);
    const std::uint64_t tableAt = blocksAt + (blocksFor(h.keyCount) + 1) * sizeof(SegmentBlock);
    if (tableAt + h.tableBytes != s->m_size)
        return nullptr;
    s->m_rowBytes = (std::uint64_t {h.denseCount} + 7) / 8;
    s->m_dense = reinterpret_cast<const std::uint32_t*>(bytes + sizeof(SegmentHeader));
    s->m_postings = bytes + postingsAt;
    s->m_blocks = reinterpret_cast<const SegmentBlock*>(bytes + blocksAt);
    s->m_table = bytes + tableAt;
    if (!s->validate())
        return nullptr;
    return s;
}

// Every lookup stays inside the file, whatever it holds: the table is read
// through once here; postings are checked as they are decoded.
bool ContentIndex::Segment::validate() const noexcept
{
    const SegmentHeader& h = m_header;
    for (std::uint32_t j = 0; j < h.denseCount; ++j) {
        if (m_dense[j] < h.docBegin || m_dense[j] >= h.docEnd || (j > 0 && m_dense[j] <= m_dense[j - 1]))
            return false;
    }
    const auto blockCount = static_cast<std::uint32_t>(blocksFor(h.keyCount));
    if (m_blocks[0].entry != 0 || m_blocks[blockCount].entry != h.tableBytes
        || m_blocks[blockCount].posting != h.postingBytes)
        return false;
    const std::uint64_t bitmapBytes = (std::uint64_t {h.docEnd} - h.docBegin + 7) / 8;
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
            const std::uint64_t v = r.next();
            const std::uint64_t size = v >> 2;
            const auto coding = static_cast<unsigned>(v & 3);
            if (!r.ok() || coding > kBitmap || (coding == kBitmap && size != bitmapBytes)
                || rowBytes(key) + size > h.postingBytes - offset)
                return false;
            offset += rowBytes(key) + size;
        }
        if (!r.atEnd() || (b + 1 < blockCount && block[1].key <= key))
            return false;
    }
    return offset == h.postingBytes;
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
    std::uint64_t offset = block->posting;
    for (bool first = true; !r.atEnd(); first = false) {
        if (!first)
            k += r.next();
        const std::uint64_t v = r.next();
        if (k >= key) {
            if (k > key)
                return false;
            out = {k, offset, v >> 2, static_cast<Coding>(v & 3)};
            return true;
        }
        offset += rowBytes(k) + (v >> 2);
    }
    return false;
}

void ContentIndex::Segment::docs(const Entry& e, std::vector<DocId>& out) const
{
    const std::size_t from = out.size();
    const std::uint64_t row = rowBytes(e.key);
    decode({m_postings + e.offset + row, static_cast<std::size_t>(e.size)}, e.coding, docBegin(), docEnd(), out);
    if (row == 0)
        return;
    const std::size_t middle = out.size();
    const std::uint8_t* const bits = m_postings + e.offset;
    for (std::uint32_t i = 0; i < row; ++i) {
        for (unsigned b = bits[i]; b != 0; b &= b - 1) {
            const std::uint32_t j = i * 8 + static_cast<unsigned>(std::countr_zero(b));
            if (j < m_header.denseCount)
                out.push_back(m_dense[j]);
        }
    }
    if (middle > from && out.size() > middle)
        std::inplace_merge(out.begin() + static_cast<std::ptrdiff_t>(from),
            out.begin() + static_cast<std::ptrdiff_t>(middle), out.end());
}

std::uint64_t ContentIndex::Segment::count(const Entry& e) const noexcept
{
    const std::uint64_t row = rowBytes(e.key);
    std::uint64_t n = countOf({m_postings + e.offset + row, static_cast<std::size_t>(e.size)}, e.coding);
    for (std::uint64_t i = 0; i < row; ++i)
        n += static_cast<unsigned>(std::popcount(m_postings[e.offset + i]));
    return n;
}

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

QString ContentIndex::segmentPath(std::uint64_t number) const
{
    return m_directory + u'/' + QString::number(number) + QStringLiteral(".grams");
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

void ContentIndex::docsOf(grams::Key key, std::vector<DocId>& out) const
{
    Segment::Entry e;
    for (const auto& s : m_segments) {
        if (s->find(key, e))
            s->docs(e, out);
    }
    const std::size_t from = out.size();
    for (const std::uint64_t p : m_memory) {
        if ((p >> kDocBits) == key)
            out.push_back(static_cast<DocId>(p & kDocMask));
    }
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(from), out.end());
}

ContentIndex::Lookup ContentIndex::lookup(QStringView phrase) const
{
    Lookup out;
    const std::vector<grams::Key> keys = grams::ofPhrase(phrase);
    if (keys.empty())
        return out;
    out.usable = true;
    out.decisive = grams::decides(phrase);

    std::shared_lock lock(m_mutex);
    // Rarest first: intersecting with it keeps the lists short.
    std::vector<std::pair<std::uint64_t, grams::Key>> order;
    for (const grams::Key key : keys) {
        std::uint64_t count = 0;
        Segment::Entry e;
        for (const auto& s : m_segments) {
            if (s->find(key, e))
                count += s->count(e);
        }
        order.emplace_back(count, key);
    }
    std::sort(order.begin(), order.end());
    std::vector<DocId> matched;
    std::vector<DocId> next;
    for (std::size_t k = 0; k < order.size(); ++k) {
        next.clear();
        docsOf(order[k].second, next);
        if (k == 0) {
            matched.swap(next);
        } else {
            std::size_t kept = 0;
            std::size_t j = 0;
            for (const DocId d : matched) {
                while (j < next.size() && next[j] < d)
                    ++j;
                if (j < next.size() && next[j] == d)
                    matched[kept++] = d;
            }
            matched.resize(kept);
        }
        if (matched.empty())
            break;
    }

    std::vector<std::uint64_t> bits((m_docEntry.size() + 63) / 64, 0);
    for (const DocId d : matched) {
        if (d < m_docEntry.size())
            bits[d >> 6] |= std::uint64_t {1} << (d & 63);
    }
    out.known.reserve(m_order.size() + m_orderTail.size());
    forEachOrdered([&](DocId d) {
        const EntryId entry = m_docEntry[d];
        if (!isCurrent(m_docState[d]) || entry >= Lookup::kMatch)
            return;
        const bool match = (bits[d >> 6] >> (d & 63)) & 1;
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
    m_docEntry = decltype(m_docEntry)();
    m_docState = decltype(m_docState)();
    m_docStamp = decltype(m_docStamp)();
    m_order = decltype(m_order)();
    m_orderTail.clear();
    m_dead = 0;
    m_deadInOrder = 0;
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

bool ContentIndex::add(
    EntryId entry, std::span<const grams::Key> keys, bool empty, std::uint32_t stamp, std::uint64_t since)
{
    std::unique_lock lock(m_mutex);
    if (!m_memory.empty() && m_memory.size() + keys.size() > kFlushPairs)
        flushLocked();
    if ((!m_memory.empty() && m_memory.size() + keys.size() > kMaxMemoryPairs) || m_docEntry.size() >= kMaxDocs)
        return false; // segments cannot be written
    if (const DocId old = findDoc(entry); old != kNoDoc)
        kill(old);
    const auto doc = static_cast<DocId>(m_docEntry.size());
    std::uint8_t state = empty ? kEmpty : kLive;
    if (static_cast<std::size_t>(std::count_if(keys.begin(), keys.end(), isTrigram)) > kDenseTrigrams)
        state |= kDense;
    if (const auto it = m_changedWhileReading.find(entry); it != m_changedWhileReading.end() && it->second > since)
        state |= kDirty; // written to while it was being read
    m_docEntry.push_back(entry);
    m_docState.push_back(state);
    m_docStamp.push_back(stamp);
    insertOrder(doc);
    for (const grams::Key key : keys)
        m_memory.push_back((key << kDocBits) | doc);
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
    DocId first = kNoDoc;
    DocId last = 0;
    for (const std::uint64_t p : m_memory) {
        first = std::min(first, static_cast<DocId>(p & kDocMask));
        last = std::max(last, static_cast<DocId>(p & kDocMask));
    }
    std::vector<DocId> dense;
    for (DocId d = first; d <= last; ++d) {
        if (m_docState[d] & kDense)
            dense.push_back(d);
    }
    const std::uint64_t number = m_nextSegment++;
    SegmentWriter writer(segmentPath(number), first, last + 1, 0, std::move(dense));
    std::vector<DocId> docs;
    for (std::size_t i = 0; i < m_memory.size();) {
        const grams::Key key = m_memory[i] >> kDocBits;
        docs.clear();
        for (; i < m_memory.size() && (m_memory[i] >> kDocBits) == key; ++i)
            docs.push_back(static_cast<DocId>(m_memory[i] & kDocMask));
        writer.add(key, docs);
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

// The last `count` segments into one of the next level. Documents keep their
// ids (merge() renumbers them); the postings of dead ones are dropped.
bool ContentIndex::mergeTail(std::size_t count)
{
    std::vector<std::shared_ptr<const Segment>> segments;
    std::vector<std::uint8_t> states;
    std::uint64_t number = 0;
    {
        std::unique_lock lock(m_mutex);
        if (count < 2 || count > m_segments.size())
            return false;
        segments.assign(m_segments.end() - static_cast<std::ptrdiff_t>(count), m_segments.end());
        states = m_docState;
        number = m_nextSegment++;
    }
    std::uint32_t level = 0;
    for (const auto& s : segments)
        level = std::max(level, s->level() + 1);
    level = std::min(level, kTopLevel - 1);

    const DocId docBegin = segments.front()->docBegin();
    const DocId docEnd = segments.back()->docEnd();
    std::vector<DocId> dense;
    for (DocId d = docBegin; d < docEnd && d < states.size(); ++d) {
        if (states[d] & kDense)
            dense.push_back(d);
    }
    const QString path = segmentPath(number);
    SegmentWriter writer(path, docBegin, docEnd, level, std::move(dense));
    std::vector<Segment::Cursor> cursors;
    for (const auto& s : segments)
        cursors.emplace_back(*s);
    std::vector<DocId> docs;
    std::vector<DocId> kept;
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
        docs.clear();
        for (Segment::Cursor& c : cursors) {
            if (!c.done() && c.key() == key) {
                c.docs(docs); // ascending: the segments hold ascending ranges
                c.next();
            }
        }
        kept.clear();
        for (const DocId d : docs) {
            if (d < states.size() && !isDead(states[d]))
                kept.push_back(d);
        }
        if (!kept.empty())
            writer.add(key, kept);
    }
    auto merged = writer.finish() ? Segment::open(path, number) : nullptr;

    std::unique_lock lock(m_mutex);
    // The same segments must still be there, side by side (clear() may have
    // dropped them meanwhile).
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

bool ContentIndex::merge()
{
    // Read from what is there now, without the lock: segments never change,
    // and documents are only added between merges (by the same caller).
    std::vector<std::shared_ptr<const Segment>> segments;
    std::vector<std::uint64_t> memory;
    std::vector<std::uint8_t> states;
    std::uint64_t number = 0;
    {
        std::unique_lock lock(m_mutex);
        segments = m_segments;
        memory = m_memory;
        states = m_docState;
        number = m_nextSegment++;
    }
    const std::size_t docCount = states.size();
    std::vector<DocId> newId(docCount, kNoDoc);
    DocId live = 0;
    for (DocId d = 0; d < docCount; ++d) {
        if (!isDead(states[d]))
            newId[d] = live++;
    }
    std::vector<DocId> dense;
    for (DocId d = 0; d < docCount; ++d) {
        if (newId[d] != kNoDoc && (states[d] & kDense))
            dense.push_back(newId[d]);
    }
    std::sort(memory.begin(), memory.end());

    // Every key of every segment and of memory, in order. Segments hold
    // ascending ranges of documents, memory the newest: concatenated, each
    // key's documents stay sorted.
    const QString path = segmentPath(number);
    SegmentWriter writer(path, 0, live, kTopLevel, std::move(dense));
    std::vector<Segment::Cursor> cursors;
    for (const auto& s : segments)
        cursors.emplace_back(*s);
    std::size_t m = 0;
    std::vector<DocId> docs;
    std::vector<DocId> mapped;
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
            key = std::min(key, memory[m] >> kDocBits);
            any = true;
        }
        if (!any)
            break;
        docs.clear();
        for (Segment::Cursor& c : cursors) {
            if (!c.done() && c.key() == key) {
                c.docs(docs);
                c.next();
            }
        }
        for (; m < memory.size() && (memory[m] >> kDocBits) == key; ++m)
            docs.push_back(static_cast<DocId>(memory[m] & kDocMask));
        mapped.clear();
        for (const DocId d : docs) {
            if (d < docCount && newId[d] != kNoDoc)
                mapped.push_back(newId[d]);
        }
        if (!mapped.empty())
            writer.add(key, mapped);
    }
    auto merged = writer.finish() ? Segment::open(path, number) : nullptr;
    std::unique_lock lock(m_mutex);
    if (!merged || m_docEntry.size() != docCount || m_memory.size() != memory.size()) {
        m_obsolete.push_back(number);
        return false;
    }
    std::vector<EntryId> entries(live);
    std::vector<std::uint8_t> newStates(live);
    std::vector<std::uint32_t> stamps(live);
    std::size_t dead = 0;
    for (DocId d = 0; d < docCount; ++d) {
        if (const DocId n = newId[d]; n != kNoDoc) {
            entries[n] = m_docEntry[d];
            newStates[n] = m_docState[d]; // dirty or dead meanwhile: it stays so
            stamps[n] = m_docStamp[d];
            dead += isDead(newStates[n]) ? 1 : 0;
        }
    }
    for (const auto& s : m_segments)
        m_obsolete.push_back(s->number());
    m_segments = {std::move(merged)};
    m_memory = decltype(m_memory)();
    m_docEntry.swap(entries);
    m_docState.swap(newStates);
    m_docStamp.swap(stamps);
    m_dead = dead;
    rebuildOrder();
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

// Layout: version:u32 nextSegment:u64 segmentCount:u32 number:u64[]
//   docCount:u32 entry:u32[] state:u8[] stamp:u32[] memoryCount:u32 pair:u64[]
std::vector<char> ContentIndex::serialize(const std::vector<EntryId>& newIds, std::vector<std::uint64_t>& segments) const
{
    std::shared_lock lock(m_mutex);
    Blob b;
    b.put(kStateVersion);
    b.put(m_nextSegment);
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
    for (DocId d = 0; d < count; ++d) {
        const EntryId e = m_docEntry[d];
        if (!isDead(states[d]) && e < newIds.size() && newIds[e] != kNoEntry)
            entries[d] = newIds[e];
        else
            states[d] = kDead;
    }
    b.bytes(entries.data(), entries.size() * sizeof(EntryId));
    b.bytes(states.data(), states.size());
    b.bytes(m_docStamp.data(), m_docStamp.size() * sizeof(std::uint32_t));
    b.put(static_cast<std::uint32_t>(m_memory.size()));
    b.bytes(m_memory.data(), m_memory.size() * sizeof(std::uint64_t));
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
        const auto segmentCount = r.get<std::uint32_t>();
        if (!r.ok() || segmentCount > 1024)
            return false;
        std::uint32_t docsBefore = 0;
        for (std::uint32_t i = 0; i < segmentCount; ++i) {
            const auto number = r.get<std::uint64_t>();
            if (!r.ok() || number >= m_nextSegment)
                return false;
            auto segment = Segment::open(segmentPath(number), number);
            if (!segment || segment->docBegin() < docsBefore)
                return false;
            docsBefore = segment->docEnd();
            m_segments.push_back(std::move(segment));
        }
        const auto count = r.get<std::uint32_t>();
        if (!r.ok() || count > kMaxDocs || count < docsBefore || !r.array(m_docEntry, count)
            || !r.array(m_docState, count) || !r.array(m_docStamp, count))
            return false;
        for (DocId d = 0; d < count; ++d) {
            const std::uint8_t state = m_docState[d];
            if ((state & kBase) > kDead || (state & ~(kBase | kDirty | kUnsure | kDense)))
                return false;
            if (isDead(state)) {
                m_docEntry[d] = kNoEntry;
                ++m_dead;
            } else if (m_docEntry[d] >= entryCount) {
                return false;
            }
        }
        const auto pairs = r.get<std::uint32_t>();
        if (!r.ok() || !r.array(m_memory, pairs) || !r.atEnd())
            return false;
        for (const std::uint64_t p : m_memory) {
            if ((p & kDocMask) >= count || (p & kDocMask) < docsBefore)
                return false;
        }
        return true;
    };
    const bool ok = parse();
    if (!ok)
        resetLocked();
    rebuildOrder();

    // Segment files nothing refers to: left by a crash, or by a state that
    // could not be restored.
    std::vector<std::uint64_t> used;
    for (const auto& s : m_segments)
        used.push_back(s->number());
    m_obsolete.clear();
    const QDir dir(m_directory);
    for (const QString& name : dir.entryList({QStringLiteral("*.grams")}, QDir::Files)) {
        bool numeric = false;
        const std::uint64_t number = QStringView(name).chopped(6).toULongLong(&numeric);
        if (numeric)
            m_nextSegment = std::max(m_nextSegment, number + 1); // never reuse a name still on disk
        if ((!numeric || std::find(used.begin(), used.end(), number) == used.end())
            && !QFile::remove(dir.filePath(name)) && numeric)
            m_obsolete.push_back(number);
    }
    m_changed = !ok;
    return ok;
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
}

ContentIndex::Stats ContentIndex::stats() const
{
    std::shared_lock lock(m_mutex);
    Stats s;
    s.documents = m_docEntry.size() - m_dead;
    for (const std::uint8_t state : m_docState)
        s.pending += !isDead(state) && (state & (kDirty | kUnsure)) ? 1 : 0;
    s.segments = m_segments.size();
    for (const auto& segment : m_segments) {
        s.segmentBytes += segment->bytes();
        s.grams += segment->grams();
        s.postingBytes += segment->postingBytes();
    }
    s.memoryPairs = m_memory.size();
    return s;
}

} // namespace ws

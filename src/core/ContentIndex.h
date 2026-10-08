#pragma once

#include "FileIndex.h"

#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ws {

enum class TextEncoding;

// ---- grams --------------------------------------------------------------------
//
// What the content index records about a file's text ("grams"): which Chinese,
// Japanese and Korean characters it has, one by one and in adjacent pairs; and
// which runs of three ASCII letters, digits or underscores, case folded. A
// phrase can only be in a file that has all of the phrase's grams, and for a
// phrase of one or two CJK characters having them is the same as containing
// it. Other letters (accented, Cyrillic...) are not indexed.
namespace grams {

// (first << 18) | second. For CJK both are code points, below 0x40000, and
// second is 0 for a single character. For three ASCII characters first is
// the first one and second the other two (b << 8 | c): first stays below any
// CJK code point, and second is never 0.
using Key = std::uint64_t;
inline constexpr unsigned kCharBits = 18;

bool isIndexed(char32_t c) noexcept; // a CJK character

// The grams a file must have to contain `phrase`: its pairs, and the
// characters that are in no pair. Sorted, distinct; empty when the phrase has
// no indexed character.
std::vector<Key> ofPhrase(QStringView phrase);
// Whether a file that has all of ofPhrase() is sure to contain the phrase.
bool decides(QStringView phrase);

// Collects the distinct grams of a text that arrives in chunks of raw bytes
// (after the byte order mark), decoded as ContentScanner decodes it.
class Collector {
public:
    Collector(TextEncoding encoding, unsigned ansiCodePage);
    void feed(const char* data, std::size_t size);
    std::vector<Key> finish(); // sorted, distinct

    // ASCII letters, digits and '_' (folded) as 1..37, in code order; 0 for the rest.
    static constexpr unsigned kWordChars = 37;
    static unsigned wordClass(char32_t c) noexcept;

private:
    void feedUtf8(const char* p, std::size_t n);
    void feedUtf16(const char* p, std::size_t n);
    void feedAnsi(const char* p, std::size_t n);
    void unit(char16_t u) noexcept;
    void ascii(unsigned char c) noexcept
    {
        m_previous = 0;
        const unsigned w = wordClass(c);
        if (w == 0) {
            m_word1 = m_word2 = 0;
            return;
        }
        if (m_word2 != 0) {
            const unsigned t = ((m_word2 - 1) * kWordChars + (m_word1 - 1)) * kWordChars + (w - 1);
            m_trigrams[t >> 6] |= std::uint64_t {1} << (t & 63);
        }
        m_word2 = m_word1;
        m_word1 = w;
    }
    void add(char32_t c);

    TextEncoding m_encoding;
    unsigned m_codePage;
    bool m_doubleByte = false; // the ANSI code page has characters of two bytes
    std::string m_carry; // the start of a character cut off by the end of a chunk
    char16_t m_highSurrogate = 0;
    char32_t m_previous = 0; // the character before, when it is indexed
    std::vector<Key> m_keys; // CJK
    std::size_t m_compactAt = 1 << 16; // sort and drop repeats once m_keys gets this long
    unsigned m_word1 = 0; // the last two characters, when they are word characters (wordClass)
    unsigned m_word2 = 0;
    std::vector<std::uint64_t> m_trigrams; // one bit per run of three word characters
    std::u16string m_wide; // ANSI decoding scratch
};

} // namespace grams

// Extensions of the files a content search looks in ("txt", ...), matched
// against FileIndex names without allocating.
class ExtensionFilter {
public:
    ExtensionFilter() = default;
    explicit ExtensionFilter(const QStringList& extensions); // leading '.' and '*' are ignored
    bool empty() const noexcept { return m_short.empty() && m_long.empty(); }
    bool matches(std::string_view name, std::size_t extLength) const noexcept;

private:
    std::vector<std::uint64_t> m_short; // up to 8 bytes, folded and packed
    std::vector<std::string> m_long; // folded
};

// The files a content search looks in. The content indexer covers the same
// ones on volumes it can follow.
struct ContentFilter {
    ExtensionFilter extensions;
    bool includeLowPriority = false;

    bool accepts(const FileIndex& index, const Entry& e) const noexcept
    {
        constexpr std::uint8_t kNever = EntryFlag::Deleted | EntryFlag::Directory | EntryFlag::Offline;
        if ((e.flags & kNever) || e.extLength == 0)
            return false;
        if (!includeLowPriority && (e.flags & EntryFlag::LowPriority))
            return false;
        return extensions.matches(index.name(e), e.extLength);
    }
};

// A file-level inverted index of grams (see above) over the files a content
// search looks in, so a search knows which files may contain a phrase without
// opening them: on Windows, opening a file costs milliseconds (the antivirus
// scans it); reading it, next to nothing.
//
// A document is what was read from one file, tied to its FileIndex entry.
// When the change journal reports that a file was written to, its document
// turns dirty and searches read the file itself until the indexer has read it
// again (into a new document). Postings live in immutable segment files, read
// through memory mapping; the newest documents' grams stay in memory until
// there are enough of them for a segment. The table of documents is stored in
// the snapshot, so it always matches the entries it refers to.
//
// Thread safety: every member locks internally. Callers that also hold a
// FileIndex lock take that one first.
class ContentIndex {
public:
    using DocId = std::uint32_t;

    explicit ContentIndex(QString directory); // where the segment files go
    ~ContentIndex();
    ContentIndex(const ContentIndex&) = delete;
    ContentIndex& operator=(const ContentIndex&) = delete;

    // ---- searching ------------------------------------------------------------
    struct Lookup {
        static constexpr std::uint32_t kMatch = 0x8000'0000u;
        bool usable = false; // the phrase has grams to look up
        bool decisive = false; // ... and having them means containing the phrase
        // The entries whose content is known, ascending; kMatch is set on
        // those whose content has every gram of the phrase.
        std::vector<std::uint32_t> known;
    };
    Lookup lookup(QStringView phrase) const;

    // While a content search runs, the indexer waits: both would open files.
    class SearchGuard {
    public:
        explicit SearchGuard(const ContentIndex* index) noexcept;
        ~SearchGuard();
        SearchGuard(const SearchGuard&) = delete;
        SearchGuard& operator=(const SearchGuard&) = delete;

    private:
        const ContentIndex* m_index;
    };
    bool searching() const noexcept { return m_searches.load(std::memory_order_relaxed) > 0; }

    // ---- keeping up with the files ---------------------------------------------
    // These are called with the FileIndex write lock (or read lock) held, so
    // the ids are current.
    void markChanged(std::span<const EntryId> entries); // written to (the change journal says so)
    // Changes went unseen (a journal was lost): check these documents again
    // (cheaply: a file whose size and time are the same is taken as unchanged).
    void markUnsure(const std::function<bool(EntryId)>& which);
    void markEmptyChanged(); // the size limit went up: files that were too large may fit now
    void remap(const FileIndex::Renumber& renumber); // FileIndex::compact
    void clear();
    void setChangeListener(std::function<void()> listener); // a document turned dirty (journal thread)

    // ---- the indexer ------------------------------------------------------------
    enum class Need : std::uint8_t { None, Check, Read };
    struct DocInfo {
        EntryId entry;
        Need need;
        std::uint32_t stamp; // size and time of the file when it was read
    };
    // Documents that are not dead, ascending by entry.
    std::vector<DocInfo> documents() const;
    void retire(std::span<const EntryId> entries); // no longer to be indexed
    void confirm(EntryId entry); // checked: the file is unchanged

    // Reads are bracketed so that a change reported while a file is being
    // read leaves its new document dirty.
    void beginReads();
    void endReads();
    std::uint64_t changeSequence() const noexcept { return m_changeSequence.load(); }
    // What was read from the entry's file (`empty`: nothing to find in it)
    // replaces the entry's document. False when there is no room left.
    bool add(EntryId entry, std::span<const grams::Key> keys, bool empty, std::uint32_t stamp, std::uint64_t since);
    static std::uint32_t stampOf(std::int64_t size, std::int64_t writeTime) noexcept;

    // Merges are called between reads, by the indexer only.
    bool needsMerge() const;
    bool mergeDue(); // what needsMerge() asks for: small segments into larger ones, or everything
    bool merge(); // all segments into one, dropping dead documents

    // ---- persistence ------------------------------------------------------------
    // The state, with entry ids translated through `newIds` (FileIndex id ->
    // id in the snapshot). `segments` gets the segment files it refers to.
    std::vector<char> serialize(const std::vector<EntryId>& newIds, std::vector<std::uint64_t>& segments) const;
    // State as serialize() wrote it, for a FileIndex of `entryCount` entries
    // numbered like the snapshot. False (and empty) when it does not fit.
    bool restore(std::span<const char> data, std::size_t entryCount);
    // Once a snapshot that refers to `saved` is on disk: delete the segment
    // files nothing refers to any more.
    void saved(const std::vector<std::uint64_t>& saved);
    bool takeChanged() noexcept { return m_changed.exchange(false); } // anything to save since the last call

    struct Stats {
        std::size_t documents = 0; // not dead
        std::size_t pending = 0; // dirty or to be checked
        std::size_t segments = 0;
        std::uint64_t segmentBytes = 0;
        std::uint64_t grams = 0; // distinct, summed over segments
        std::uint64_t postingBytes = 0;
        std::size_t memoryPairs = 0; // grams of the newest documents, not yet in a segment
    };
    Stats stats() const;

private:
    class Segment;
    struct Order; // m_order's comparisons

    static constexpr DocId kNoDoc = 0xFFFF'FFFFu;

    QString segmentPath(std::uint64_t number) const;
    DocId findDoc(EntryId entry) const noexcept; // the entry's document that is not dead
    void kill(DocId doc) noexcept;
    void insertOrder(DocId doc);
    void rebuildOrder();
    template <typename F> void forEachOrdered(F&& f) const; // live documents by entry
    void docsOf(grams::Key key, std::vector<DocId>& out) const;
    bool flushLocked();
    std::size_t dueTailLocked() const noexcept;
    bool mergeAllDueLocked() const noexcept;
    bool mergeTail(std::size_t count);
    void resetLocked();

    QString m_directory;
    mutable std::shared_mutex m_mutex;
    // Documents, by id. A document's grams are in the segments and m_memory.
    std::vector<EntryId> m_docEntry;
    std::vector<std::uint8_t> m_docState;
    std::vector<std::uint32_t> m_docStamp;
    std::size_t m_dead = 0;
    // Document ids sorted by entry, plus the newest ones (sorted, merged in
    // now and then). Dead documents are skipped, and dropped on rebuild.
    std::vector<DocId> m_order;
    std::vector<DocId> m_orderTail;
    std::size_t m_deadInOrder = 0;

    std::vector<std::shared_ptr<const Segment>> m_segments; // doc id ranges ascending
    std::vector<std::uint64_t> m_memory; // key << kDocBits | doc, of documents newer than every segment
    std::uint64_t m_nextSegment = 1;
    std::vector<std::uint64_t> m_obsolete; // segment files no longer used, to delete once no snapshot refers to them

    std::atomic<std::uint64_t> m_changeSequence {0};
    int m_reads = 0;
    std::unordered_map<EntryId, std::uint64_t> m_changedWhileReading;
    std::function<void()> m_listener;

    mutable std::atomic<int> m_searches {0};
    std::atomic<bool> m_changed {false};
};

} // namespace ws

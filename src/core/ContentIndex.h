#pragma once

#include "DocText.h"
#include "FileIndex.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <compare>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
// which runs of three ASCII characters, letters case folded and any run of
// whitespace taken as one space. Spaces and punctuation count: a line of code
// is told apart by them ("o l", "->r", "();") as much as by its words. A
// phrase can only be in a file that has all of the phrase's grams, and for a
// phrase of one or two CJK characters having them is the same as containing
// it. Other letters (accented, Cyrillic...) are not indexed.
namespace grams {

// (first << 18) | second. For CJK both are code points, below 0x40000, and
// second is 0 for a single character. For three ASCII characters first is
// the first one and second the other two (b << 8 | c): first stays below any
// CJK code point, and second is never 0 (no control characters).
using Key = std::uint64_t;
inline constexpr unsigned kCharBits = 18;

bool isIndexed(char32_t c) noexcept; // a CJK character

// The grams a file must have to contain `phrase`: its pairs, and the
// characters that are in no pair. Sorted, distinct; empty when the phrase has
// no indexed character.
std::vector<Key> ofPhrase(QStringView phrase);
// Whether a file that has all of ofPhrase() is sure to contain the phrase.
bool decides(QStringView phrase);

// Tells sets of grams apart: 96 bits of their SHA-256. Files with the same
// one have the same grams (copies of a file, say).
struct Fingerprint {
    std::uint64_t high = 0;
    std::uint32_t low = 0;

    friend auto operator<=>(const Fingerprint&, const Fingerprint&) = default;
};
std::optional<Fingerprint> fingerprint(std::span<const Key> keys); // sorted, distinct; nullopt if hashing fails

// Collects the distinct grams of a text that arrives in chunks of raw bytes
// (after the byte order mark), decoded as ContentScanner decodes it.
class Collector {
public:
    Collector(TextEncoding encoding, unsigned ansiCodePage);
    void feed(const char* data, std::size_t size);
    std::vector<Key> finish(); // sorted, distinct

    // Printable ASCII characters, letters folded, as 1..kAsciiChars in code
    // order, whitespace as kSpace (the class of ' '); 0 for control characters.
    static constexpr unsigned kAsciiChars = 69;
    static constexpr unsigned kSpace = 1;
    static unsigned asciiClass(char32_t c) noexcept;

private:
    void feedUtf8(const char* p, std::size_t n);
    void feedUtf16(const char* p, std::size_t n);
    void feedAnsi(const char* p, std::size_t n);
    void unit(char16_t u) noexcept;
    void ascii(unsigned char c) noexcept
    {
        m_previous = 0;
        const unsigned w = asciiClass(c);
        if (w == 0) {
            endRun();
            return;
        }
        if (w == kSpace && m_char1 == kSpace)
            return; // a run of whitespace is one space
        if (m_char2 != 0) {
            const unsigned t = ((m_char2 - 1) * kAsciiChars + (m_char1 - 1)) * kAsciiChars + (w - 1);
            m_trigrams[t >> 6] |= std::uint64_t {1} << (t & 63);
        }
        m_char2 = m_char1;
        m_char1 = w;
    }
    void endRun() noexcept { m_char1 = m_char2 = 0; } // a character that is not ASCII, or a control character
    // As ascii() for each byte, up to the first that is not ASCII; how many it took.
    std::size_t asciiRun(const unsigned char* b, std::size_t n) noexcept;
    void add(char32_t c);

    TextEncoding m_encoding;
    unsigned m_codePage;
    bool m_doubleByte = false; // the ANSI code page has characters of two bytes
    std::string m_carry; // the start of a character cut off by the end of a chunk
    char16_t m_highSurrogate = 0;
    char32_t m_previous = 0; // the character before, when it is indexed
    std::vector<Key> m_keys; // CJK
    std::size_t m_compactAt = 1 << 16; // sort and drop repeats once m_keys gets this long
    unsigned m_char1 = 0; // the last two characters, when they are ASCII (asciiClass)
    unsigned m_char2 = 0;
    std::vector<std::uint64_t> m_trigrams; // one bit per run of three ASCII characters
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

// How large a file may be for a content search (and the indexer) to read it,
// by what kind of text it holds. Larger files are left out: much reading for
// little to find, and a search reads them for nearly every phrase, since they
// have nearly every gram (minified bundles, exported data of tens of MB).
// Documents are another matter: most of a large one is pictures, and the text
// read out of one is limited anyway (DocExtractor::kMaxText).
struct ContentSizeLimits {
    enum Kind : std::uint8_t {
        Text, // plain text, notes, logs, tables: what people write and keep (and the extensions not listed)
        Code, // source code and scripts: hand-written files are small, generated ones large
        Data, // data and markup: JSON, XML, SVG, web pages, SQL
        Document, // Word, Excel, PowerPoint, PDF (documentExtensions())
        kKinds
    };
    std::array<std::int64_t, kKinds> bytes {64ll << 20, 8ll << 20, 16ll << 20, 512ll << 20};

    static Kind kindOf(std::string_view extension) noexcept; // without the dot, any case
    std::int64_t of(std::wstring_view path) const noexcept; // by the file's extension
    bool operator==(const ContentSizeLimits&) const = default;
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
        const std::string_view name = index.name(e);
        if (name.starts_with("~$"))
            return false; // Office's lock file next to an open document
        return extensions.matches(name, e.extLength);
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
// again (into a new document). A document's grams are its content's:
// documents with the same grams (copies of a file, checkouts of a project)
// share one, and postings list contents. Postings live in immutable segment
// files, read through memory mapping; the newest contents' grams stay in
// memory until there are enough of them for a segment. The table of
// documents is stored in the snapshot, so it always matches the entries it
// refers to.
//
// A document read out of a Word, Excel, PowerPoint or PDF file (by the
// DocExtractor) also keeps its text, compressed, in a text file next to the
// segments: a search reads it there for the place and the snippet, rather
// than reading the file again. Documents with the same text share it. Texts
// no document has any more stay until there are more of them than of the
// others; then the file is rewritten.
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
        // The entries whose content is known, ascending; when usable, kMatch
        // is set on those whose content has every gram of the phrase.
        std::vector<std::uint32_t> known;
    };
    Lookup lookup(QStringView phrase) const;
    bool knows(EntryId entry) const; // its content is known (as Lookup::known)

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
    // replaces the entry's document, with the text of a document file, packed
    // by packText(). False when there is no room left.
    bool add(EntryId entry, std::span<const grams::Key> keys, bool empty, std::uint32_t stamp, std::uint64_t since,
        QByteArrayView text = {});
    // The entry's file has the same bytes as the original's, whose document
    // was read when its file had `originalStamp`: the entry gets a document
    // of its own with what that one has (grams, text), without reading the
    // file. False when the original has no such current document.
    bool addCopy(EntryId entry, EntryId original, std::uint32_t originalStamp, std::uint32_t stamp, std::uint64_t since);
    static std::uint32_t stampOf(std::int64_t size, std::int64_t writeTime) noexcept;

    // Merges are called by the indexer only. Small segments into larger ones
    // (mergeTails) while documents are added too; the others between reads.
    // `threads`: how many a merge of everything may take.
    bool needsMerge() const;
    bool needsFullMerge() const; // mergeDue() would merge everything
    bool mergeTails(); // the small segments due, into larger ones
    bool mergeDue(int threads = 1); // what needsMerge() asks for: mergeTails(), or everything
    bool merge(int threads = 1); // all segments into one, dropping dead documents
    bool needsTextCompaction() const;
    bool compactTexts(); // the text file again, without the texts of dead documents

    // ---- documents' text ----------------------------------------------------------
    static QByteArray packText(const doctext::DocText& text);
    // The text kept with the entry's document, if the document is current:
    // empty when there is nothing to find in the file (no text, too large,
    // not readable); none when no text was kept (or could not be read back).
    std::optional<doctext::DocText> textOf(EntryId entry) const;

    // ---- persistence ------------------------------------------------------------
    // The state, with entry ids translated through `newIds` (FileIndex id ->
    // id in the snapshot). `segments` gets the segment files it refers to.
    std::vector<char> serialize(const std::vector<EntryId>& newIds, std::vector<std::uint64_t>& segments) const;
    // State as serialize() wrote it, for a FileIndex of `entryCount` entries
    // numbered like the snapshot. False (and empty) when it does not fit.
    bool restore(std::span<const char> data, std::size_t entryCount);
    // Once a snapshot that refers to `saved` is on disk: delete the segment
    // and text files nothing refers to any more.
    void saved(const std::vector<std::uint64_t>& saved);
    std::uint64_t obsoleteBytes() const; // of the files waiting for that
    bool takeChanged() noexcept { return m_changed.exchange(false); } // anything to save since the last call
    // The segment and text files have been copied to `directory`
    // (IndexService::moveTo): they are used from there on, and nothing has
    // the old ones open once this returns (and the last search reading a
    // text is done). False, with nothing changed, when one cannot be opened
    // there. Not while the indexer reads or merges.
    bool relocate(QString directory);
    std::vector<QString> segmentPaths() const; // the segment files in use (to read them ahead: IndexService)

    struct Stats {
        std::size_t documents = 0; // not dead
        std::size_t contents = 0; // distinct, of those documents
        std::size_t pending = 0; // dirty or to be checked
        std::size_t segments = 0;
        std::uint64_t segmentBytes = 0;
        std::uint64_t grams = 0; // distinct, summed over segments
        std::uint64_t postingBytes = 0;
        std::size_t memoryPairs = 0; // grams of the newest contents, not yet in a segment
        std::size_t texts = 0; // documents with their text kept
        std::size_t distinctTexts = 0; // ... the texts themselves (documents share them)
        std::uint64_t textBytes = 0; // the text file, with what dead documents left
    };
    Stats stats() const;

private:
    class Segment;
    using ContentId = std::uint32_t;
    class TextFile;
    struct TextRef {
        std::uint64_t offset;
        std::uint32_t bytes;
        friend bool operator==(const TextRef&, const TextRef&) = default;
    };

    static constexpr DocId kNoDoc = 0xFFFF'FFFFu;
    static constexpr ContentId kNoContent = 0xFFFF'FFFFu;

    static QString segmentPath(const QString& directory, std::uint64_t number);
    QString segmentPath(std::uint64_t number) const; // in m_directory
    static QString textPath(const QString& directory, std::uint64_t number);
    QString textPath(std::uint64_t number) const; // in m_directory
    std::shared_ptr<TextFile> textFileForAppend();
    std::shared_ptr<TextFile> textFileLocked(); // as textFileForAppend, with the lock held
    std::optional<TextRef> textWithPrint(const grams::Fingerprint& print) const; // a document's, if any has it
    void useText(DocId doc, TextRef text, const std::optional<grams::Fingerprint>& print);
    void dropText(DocId doc) noexcept;
    DocId findDoc(EntryId entry) const noexcept; // the entry's document that is not dead
    DocId addDocLocked(EntryId entry, std::uint8_t state, std::uint32_t stamp, std::uint64_t since, ContentId content);
    void replaceLocked(EntryId entry, DocId doc); // the entry's document so far dies; `doc` is in the order
    void kill(DocId doc) noexcept;
    void insertOrder(DocId doc);
    void rebuildOrder();
    template <typename F> void forEachOrdered(F&& f) const; // live documents by entry
    void contentsOf(grams::Key key, std::vector<ContentId>& out) const;
    ContentId findContent(const grams::Fingerprint& print) const noexcept; // kNoContent if there is none
    ContentId memoryBegin() const noexcept; // the first content not in a segment
    std::vector<bool> liveContents() const; // those that a document not dead has
    bool flush(std::unique_lock<std::shared_mutex>& lock); // memory into a segment; unlocks meanwhile
    using Segments = std::vector<std::shared_ptr<const Segment>>;
    // Calls f(key, contents) for each key in [from, to) of `segments` and of
    // `memory` (sorted pairs), in order. Segments hold ascending ranges of
    // contents, memory the newest: concatenated, a key's contents ascend.
    template <typename F>
    static void forEachKey(const Segments& segments, std::span<const std::uint64_t> memory, grams::Key from,
        grams::Key to, F&& f);
    // Keys that cut those of `segments` and `memory` into about `parts`
    // stretches that take about as long to merge, ascending; `total` gets
    // what merging all of them takes (Segment::work: about their pairs).
    static std::vector<grams::Key> splitKeys(const Segments& segments, std::span<const std::uint64_t> memory,
        std::size_t parts, int threads, std::uint64_t& total);
    std::size_t dueTailLocked() const noexcept;
    bool mergeAllDueLocked() const noexcept;
    bool mergeTail(std::size_t count);
    void resetLocked();

    QString m_directory;
    mutable std::shared_mutex m_mutex;
    // Documents, by id.
    std::vector<EntryId> m_docEntry;
    std::vector<std::uint8_t> m_docState;
    std::vector<std::uint32_t> m_docStamp;
    std::vector<ContentId> m_docContent; // kNoContent: no grams
    std::size_t m_dead = 0;
    // Document ids sorted by entry, plus the newest ones (sorted, merged in
    // now and then). Dead documents are skipped, and dropped on rebuild.
    std::vector<DocId> m_order;
    std::vector<DocId> m_orderTail;
    std::size_t m_deadInOrder = 0;

    // Contents are numbered below m_contentEnd; their grams are in the
    // segments (ranges of content ids, ascending) and m_memory.
    std::vector<std::shared_ptr<const Segment>> m_segments;
    std::vector<std::uint64_t> m_memory; // key << kIdBits | content, of contents newer than every segment
    std::shared_ptr<const std::vector<std::uint64_t>> m_writing; // older memory, being written to a segment (flush)
    struct FlushBuffers;
    std::unique_ptr<FlushBuffers> m_flushBuffers; // kept from one segment to the next while files are read (flush)
    std::map<grams::Fingerprint, ContentId> m_memoryContents; // those contents
    bool m_flushing = false; // memory is being written to a segment (flush)
    std::condition_variable_any m_flushed; // ... no longer
    std::uint64_t m_generation = 0; // goes up when contents are dropped (resetLocked) or numbered anew (merge)
    ContentId m_contentEnd = 0;
    std::uint64_t m_nextSegment = 1;
    std::vector<std::uint64_t> m_obsolete; // segment files no longer used, to delete once no snapshot refers to them

    std::shared_ptr<TextFile> m_textFile; // appended to; null until a document brings text
    std::unordered_map<DocId, TextRef> m_texts; // in m_textFile
    // Documents with the same text share it (copies of a file: half of the
    // documents here).
    struct TextUse {
        std::uint32_t docs = 0;
        std::uint32_t bytes = 0;
        std::optional<grams::Fingerprint> print; // of its packed bytes
    };
    std::unordered_map<std::uint64_t, TextUse> m_textUses; // by offset, the texts documents have
    std::map<grams::Fingerprint, std::uint64_t> m_textByPrint; // their offsets
    std::uint64_t m_textGarbage = 0; // bytes of texts no document has any more
    std::vector<std::uint64_t> m_obsoleteTexts; // as m_obsolete

    std::atomic<std::uint64_t> m_changeSequence {0};
    int m_reads = 0;
    std::unordered_map<EntryId, std::uint64_t> m_changedWhileReading;
    std::function<void()> m_listener;

    mutable std::atomic<int> m_searches {0};
    std::atomic<bool> m_changed {false};
};

} // namespace ws

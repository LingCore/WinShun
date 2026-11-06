#pragma once

#include <QString>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ws {

using EntryId = std::uint32_t;
inline constexpr EntryId kNoEntry = 0xFFFF'FFFFu;

namespace EntryFlag {
inline constexpr std::uint8_t Directory = 0x01;
inline constexpr std::uint8_t Deleted = 0x02;
inline constexpr std::uint8_t Root = 0x04;
inline constexpr std::uint8_t LowPriority = 0x08; // under system/program/tooling folders
inline constexpr std::uint8_t Hidden = 0x10;
inline constexpr std::uint8_t Offline = 0x20; // cloud placeholder: reading would download it
inline constexpr std::uint8_t Han = 0x40; // the name has Chinese characters (tried as pinyin); set by the index
inline constexpr std::uint8_t Remote = 0x80; // on a network share (a root named "\\host\share")
inline constexpr std::uint8_t Persistent = Directory | Root | LowPriority | Hidden | Offline | Remote;
inline constexpr std::uint8_t Inherited = LowPriority | Hidden | Remote;
} // namespace EntryFlag

// One file or folder. Children form a sibling list, newest first. Taking a
// child out of it needs the one before: found by walking a small folder;
// a big one has a table of its children that knows (see ChildTable).
// Packed: there is one per file and folder, and a name search reads them
// all (unaligned reads cost nothing on x64). The name's length and that of
// its extension are kept with the name (see FileIndex::name).
#pragma pack(push, 1)
struct Entry {
    EntryId parent = kNoEntry;
    // A directory's first child or a file's packed size: files have no
    // children and directories have no size. Shared without a union, so
    // traversal can safely ask firstChild() of either kind.
    std::uint32_t childOrSize = kNoEntry;
    EntryId nextSibling = kNoEntry;
    std::uint32_t nameOffset = 0; // in the index's name storage
    std::uint8_t flags = 0;

    bool isDir() const noexcept { return flags & EntryFlag::Directory; }
    bool isDeleted() const noexcept { return flags & EntryFlag::Deleted; }
    EntryId firstChild() const noexcept { return isDir() ? childOrSize : kNoEntry; }
};
#pragma pack(pop)
static_assert(sizeof(Entry) == 17, "keep Entry compact: there is one per file and folder");

// When a file or folder was last written: seconds since 1970 (UTC), 0 when
// not known. Good until 2106.
using FileTime = std::uint32_t;

// A time to sort by or to show: one before 1980 is none. Archives, package
// managers and copies from systems that knew no time leave 1970-01-01 (Unix
// time 0) or 1980-01-01 (the DOS epoch, a day either way in local time).
inline constexpr FileTime kFirstKnownTime = 315'619'200; // 1980-01-02 00:00 UTC
constexpr FileTime knownTime(FileTime time) noexcept { return time < kFirstKnownTime ? 0 : time; }

// Later than this is a clock gone wrong (a camera's, another computer's, an
// installer's 2076), not new work: a day from now, taken once per search.
inline FileTime trustedUntil() noexcept
{
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return static_cast<FileTime>(std::clamp<std::int64_t>(now + 86'400, 0, 0xFFFF'FFFF));
}

// A time to order by: none (0, last both ways) when not known or later than
// `until`. Such a file still shows its own date; it is just not "the newest".
constexpr FileTime sortTime(FileTime time, FileTime until) noexcept
{
    const FileTime known = knownTime(time);
    return known > until ? 0 : known;
}

// A file's size, packed into 32 bits: the bits of the size as a float. Exact
// up to 16 MB, within a hundred-thousandth above (a list says "1.2 GB", a
// filter asks for "over 100 MB": neither needs the byte). Packed so, sizes
// compare as the unsigned integers they are (a sort by size never unpacks),
// and a column of them costs 4 bytes an entry. kUnknownSize (no float a size
// packs to) is a file whose size was never read: a snapshot from before
// sizes were kept, a folder.
using FileSize = std::uint32_t;
inline constexpr FileSize kUnknownSize = 0xFFFF'FFFFu;
inline FileSize packSize(std::uint64_t bytes) noexcept
{
    return std::bit_cast<FileSize>(static_cast<float>(bytes));
}
inline std::uint64_t unpackSize(FileSize packed) noexcept
{
    if (packed == kUnknownSize)
        return 0;
    const float f = std::bit_cast<float>(packed);
    // In double: a float's integer is exact there, and the half added does
    // not round it up (16777215 + 0.5f is 16777216 as a float).
    return f <= 0 ? 0 : static_cast<std::uint64_t>(static_cast<double>(f) + 0.5);
}

// Bounds an entry must fit besides its name: the search panel's filters, and
// size: / dm: typed in the query (Query.h). A bound on the time leaves out
// entries whose time is not known (knownTime); a bound on the size leaves
// out folders and files whose size is not known. `folders`: under one of
// them (sorted ids); empty: anywhere.
struct EntryFilter {
    FileTime after = 0; // written at or after this (0: no bound)
    FileTime before = 0; // ... and before this (0: none)
    FileSize minSize = 0; // packed: at least this
    FileSize maxSize = kUnknownSize; // ... and at most this (kUnknownSize: none)
    std::vector<EntryId> folders;
    // Only what lies directly in this folder, not deeper ("this level only"
    // of a folder's window); kNoEntry: no such bound. A folder that is not in
    // the index is kNoEntry - 1, which no entry has as its parent.
    EntryId directIn = kNoEntry;

    bool byTime() const noexcept { return after != 0 || before != 0; }
    bool bySize() const noexcept { return minSize != 0 || maxSize != kUnknownSize; }
    bool any() const noexcept { return byTime() || bySize() || !folders.empty() || directIn != kNoEntry; }
    // Both filters at once: the narrower bounds.
    void merge(const EntryFilter& other)
    {
        after = std::max(after, other.after);
        before = before == 0 ? other.before : other.before == 0 ? before : std::min(before, other.before);
        minSize = std::max(minSize, other.minSize);
        maxSize = std::min(maxSize, other.maxSize);
        for (const EntryId f : other.folders)
            folders.push_back(f);
        if (other.directIn != kNoEntry)
            directIn = other.directIn;
    }
    // Whether an entry with these fits (folders aside); for a folder entry
    // the size is kUnknownSize.
    bool fits(FileTime modified, FileSize size, bool isDir) const noexcept
    {
        if (byTime()) {
            const FileTime known = knownTime(modified);
            if (known == 0 || known < after || (before != 0 && known >= before))
                return false;
        }
        if (bySize() && (isDir || size == kUnknownSize || size < minSize || size > maxSize))
            return false;
        return true;
    }
    friend bool operator==(const EntryFilter&, const EntryFilter&) = default;
};

// A FILETIME's count (100-nanosecond intervals since 1601) as a FileTime.
constexpr FileTime fileTimeOf(std::int64_t ticks) noexcept
{
    constexpr std::int64_t kTicksPerSecond = 10'000'000;
    constexpr std::int64_t kSeconds1601To1970 = 11'644'473'600;
    const std::int64_t seconds = ticks / kTicksPerSecond - kSeconds1601To1970;
    return seconds <= 0 ? 0 : seconds >= 0xFFFF'FFFF ? 0xFFFF'FFFF : static_cast<FileTime>(seconds);
}

// Folder entries by NTFS file record number (open addressing). Record 0 is
// the MFT itself, never a folder, so it marks empty slots.
class RecordTable {
public:
    EntryId find(std::uint32_t record) const noexcept;
    void set(std::uint32_t record, EntryId id);
    void erase(std::uint32_t record) noexcept;
    std::size_t size() const noexcept { return m_count; }
    std::size_t capacity() const noexcept { return m_slots.capacity(); }
    template <typename F> void forEach(F&& f) const
    {
        for (const Slot& s : m_slots) {
            if (s.record != 0)
                f(s.record, s.id);
        }
    }
    // Replaces every id with map(id); entries mapped to kNoEntry are dropped.
    template <typename F> void remap(F&& map)
    {
        std::vector<Slot> old;
        old.swap(m_slots);
        m_count = 0;
        for (const Slot& s : old) {
            const EntryId id = s.record != 0 ? map(s.id) : kNoEntry;
            if (id != kNoEntry)
                set(s.record, id);
        }
    }

private:
    struct Slot {
        std::uint32_t record = 0;
        EntryId id = kNoEntry;
    };
    std::size_t home(std::uint32_t record) const noexcept;
    std::vector<Slot> m_slots;
    std::size_t m_count = 0;
};

// In-memory index of every file and folder name on the indexed volumes.
//
// Storage is chunked (entries and names) so growth never copies the whole
// index and peak memory stays close to the live size. Names are WTF-8.
//
// Thread safety: callers hold readLock() while reading and writeLock() while
// mutating. Searches take the read lock for a few milliseconds; writers
// (crawler, change watcher) take the write lock in small batches. Whoever
// reads for long takes longReadLock() instead.
class FileIndex {
public:
    static constexpr std::size_t kChunkBits = 16;
    static constexpr std::size_t kChunkSize = std::size_t {1} << kChunkBits;
    static constexpr std::size_t kNameChunkBits = 18;
    static constexpr std::size_t kNameChunkSize = std::size_t {1} << kNameChunkBits;

    FileIndex();
    ~FileIndex();
    FileIndex(const FileIndex&) = delete;
    FileIndex& operator=(const FileIndex&) = delete;

    [[nodiscard]] std::shared_lock<std::shared_mutex> readLock() const { return std::shared_lock(m_mutex); }
    // The same if it can be had at once (owns_lock()), else none: for a look that may skip a turn.
    [[nodiscard]] std::shared_lock<std::shared_mutex> tryReadLock() const { return std::shared_lock(m_mutex, std::try_to_lock); }
    [[nodiscard]] std::unique_lock<std::shared_mutex> writeLock()
    {
        const std::lock_guard gate(m_writerGate);
        return std::unique_lock(m_mutex);
    }

    // The read lock, for reading long (saving the index, a pass over all of
    // it). Writers wait at their gate meanwhile, not on the lock: Windows'
    // shared mutex lets no reader past a waiting writer, so one waiting there
    // would hold up the searches until the long read ends. Long reads go
    // through the gate side by side.
    struct LongReadLock {
        std::shared_lock<std::shared_mutex> gate;
        std::shared_lock<std::shared_mutex> lock; // let go first
    };
    [[nodiscard]] LongReadLock longReadLock() const
    {
        std::shared_lock gate(m_writerGate);
        return {std::move(gate), std::shared_lock(m_mutex)};
    }

    // ---- read API -------------------------------------------------------
    std::size_t slotCount() const noexcept { return m_count; } // includes deleted slots
    std::size_t liveCount() const noexcept { return m_live.load(std::memory_order_relaxed); }

    const Entry& entry(EntryId id) const noexcept { return m_chunks[id >> kChunkBits][id & (kChunkSize - 1)]; }
    std::string_view name(const Entry& e) const noexcept { return nameAt(e.nameOffset); }
    std::string_view name(EntryId id) const noexcept { return name(entry(id)); }
    // Bytes after the last '.' of a file's name; 0 for folders and names without one.
    std::size_t extLength(const Entry& e) const noexcept { return e.isDir() ? 0 : nameExtLength(e.nameOffset); }
    // A file's name and its extLength, from one read of the record (for passes over every file).
    std::string_view fileName(const Entry& e, std::size_t& extLength) const noexcept
    {
        const char* record = nameRecord(m_names, e.nameOffset);
        const std::string_view name = storedName(record);
        extLength = static_cast<unsigned char>(name.data()[-1]) & 0x7F;
        return name;
    }
    // Kept apart from the entries, which name searches go through: only the
    // matches have theirs looked at.
    FileTime modified(EntryId id) const noexcept { return m_times[id >> kChunkBits][id & (kChunkSize - 1)]; }
    FileTime created(EntryId id) const noexcept { return m_created[id >> kChunkBits][id & (kChunkSize - 1)]; }
    // A file's size, packed (packSize); kUnknownSize for a folder, or a file
    // whose size was never read. Stored in the entry's unused child link.
    FileSize size(EntryId id) const noexcept
    {
        const Entry& e = entry(id);
        return e.isDir() ? kUnknownSize : e.childOrSize;
    }

    // Bytes held, by part (for measuring). Caller holds a read lock.
    struct MemoryUse {
        std::size_t entries = 0;
        std::size_t times = 0;
        std::size_t sizes = 0;
        std::size_t names = 0; // name chunks allocated
        std::size_t nameBytes = 0; // ... of which used
        std::size_t folderRecords = 0;
        std::size_t childTables = 0;
        std::size_t intern = 0;
    };
    MemoryUse memoryUse() const noexcept;
    // The big blocks the memoryUse() numbers are made of (entries, times,
    // names): address and size, so how much of them is in the working set
    // right now can be asked of Windows (memory::residentBytes). The rest
    // (tables on the general heap) is not in the list. Caller holds a read lock.
    std::vector<std::pair<const void*, std::size_t>> memoryBlocks() const;

    // Brings the pages a name search reads (entries, times, names) back into
    // the working set after it was emptied (IdleTrim), so the search that
    // follows does not take them one fault at a time. Caller holds a read lock.
    void warm() const noexcept;

    std::size_t chunkCount() const noexcept { return m_chunks.size(); }
    std::span<const Entry> chunk(std::size_t index) const noexcept;
    const std::vector<EntryId>& roots() const noexcept { return m_roots; }

    // Names compare ignoring ASCII case, as NTFS does for most names.
    EntryId findChild(EntryId parent, std::string_view name, bool exactCase = false) const;
    EntryId findPath(std::wstring_view path) const; // "C:\dir\file.txt", "\\host\share\dir\file.txt"
    std::u16string path16(EntryId id) const;
    void appendPath16(EntryId id, std::u16string& out) const;
    QString path(EntryId id) const;
    std::wstring wpath(EntryId id) const;
    int depth(EntryId id) const noexcept;

    // ---- write API ------------------------------------------------------
    // Lookups for writers (caller holds writeLock()). A folder with very
    // many children gets a hash table of them on first use, so that changes
    // in it (thousands of files unpacked into one folder) cost O(1) each
    // instead of a walk through all its children. `exactCase` compares the
    // bytes; otherwise ASCII case is ignored.
    EntryId childForUpdate(EntryId parent, std::string_view name, bool exactCase);
    EntryId pathForUpdate(std::wstring_view path);

    EntryId addRoot(std::string_view name); // "C:", or a share's "\\host\share" (or a folder in it)
    EntryId add(EntryId parent, std::string_view name, std::uint8_t flags);
    // The same with a name already stored (storeName), for loading names
    // that are distinct already; a root when parent is kNoEntry.
    EntryId addStored(EntryId parent, std::uint32_t nameOffset, std::uint8_t flags);
    std::size_t remove(EntryId id); // removes the whole subtree; returns entries removed
    bool move(EntryId id, EntryId newParent, std::string_view newName);
    void setFlags(EntryId id, std::uint8_t flags);
    void setModified(EntryId id, FileTime time) noexcept { m_times[id >> kChunkBits][id & (kChunkSize - 1)] = time; }
    void setCreated(EntryId id, FileTime time) noexcept { m_created[id >> kChunkBits][id & (kChunkSize - 1)] = time; }
    void setSize(EntryId id, FileSize size) noexcept
    {
        Entry& e = mut(id);
        if (!e.isDir())
            e.childOrSize = size;
    }

    // While interning is on, identical names share one copy (only a third of
    // the names on a typical disk are unique: index.js, __init__.py, ...).
    // It is switched on for bulk work (loading, crawling) and off afterwards
    // to free the lookup table; names added later are simply stored again.
    void setInterning(bool enabled);

    // Removed items keep their slot and name until this drops them. The live
    // entries are renumbered (in their old order), so every EntryId held
    // across the call is invalid afterwards. Not while interning is on.
    // `always` also runs it with nothing removed, to drop names that were
    // stored but never used. `renumbered` is handed the old -> new mapping
    // (kNoEntry for removed items; it never reorders) for ids kept elsewhere.
    // Returns the number of slots freed.
    using Renumber = std::function<EntryId(EntryId)>;
    std::size_t compact(bool always = false, const std::function<void(const Renumber&)>& renumbered = {});
    std::uint64_t generation() const noexcept { return m_generation.load(std::memory_order_acquire); } // +1 per compact()

    // Code that holds EntryIds outside the lock for a while (a content scan,
    // the content indexer) pins them: IndexService does not compact meanwhile.
    class IdPin {
    public:
        IdPin() noexcept = default;
        explicit IdPin(const FileIndex& index) noexcept
            : m_index(&index)
        {
            m_index->m_pins.fetch_add(1);
        }
        IdPin(IdPin&& other) noexcept
            : m_index(std::exchange(other.m_index, nullptr))
        {
        }
        IdPin& operator=(IdPin&& other) noexcept
        {
            if (this != &other) {
                reset();
                m_index = std::exchange(other.m_index, nullptr);
            }
            return *this;
        }
        ~IdPin() { reset(); }
        void reset() noexcept
        {
            if (m_index)
                std::exchange(m_index, nullptr)->m_pins.fetch_sub(1);
        }

    private:
        const FileIndex* m_index = nullptr;
    };
    // Pin while holding a lock, so no compaction can come in between.
    [[nodiscard]] IdPin pinIds() const noexcept { return IdPin(*this); }
    bool idsPinned() const noexcept { return m_pins.load() > 0; }

    // ---- files changed, for the content indexer --------------------------
    // The files added, removed, renamed or moved, or whose flags changed,
    // since the last call: all a content pass has to look at besides the
    // documents it knows are due (ContentIndexer::findWork), instead of every
    // file. Recorded from the first call on. Not `complete` when that cannot
    // be told: the first call, more than kMaxChanges of them, ids renumbered
    // by compact(), a folder moved onto another root; every file is looked at
    // then. Folders are never in it: what a content pass decides is per file.
    // The caller holds a read lock (writers record under the write lock); one
    // caller at a time.
    struct Changes {
        std::vector<EntryId> files; // ascending, each once
        bool complete = false;
    };
    Changes takeChanges();
    static constexpr std::size_t kMaxChanges = std::size_t {1} << 19; // 2 MB of ids

    // Name storage, for bulk readers that collect names before adding the
    // entries (the MFT reader). Offsets stay valid until compact().
    //
    // Each name is stored as its length (one byte below 128, else two: the
    // first with its top bit set), one byte with the length of its extension
    // as a file's (an extension of 128 bytes or more counts as none) and on
    // top whether it has Chinese characters, and its bytes; chunks end with
    // zero bytes. Every record takes 3 bytes at least. Entries refer
    // to where a name starts. Names longer than kMaxName bytes are cut.
    static constexpr std::size_t kMaxName = 0x7FFF;
    std::uint32_t storeName(std::string_view name);
    std::string_view nameAt(std::uint32_t offset) const noexcept { return storedName(nameRecord(m_names, offset)); }
    std::size_t nameExtLength(std::uint32_t offset) const noexcept { return nameTraits(offset) & 0x7F; }
    bool nameHasHan(std::uint32_t offset) const noexcept { return nameTraits(offset) & 0x80; }

    // The stored names, a chunk at a time (for a pass over them all, which
    // looks at each name once however many entries have it). Calls
    // f(offset, name) for every name stored in chunk `chunk` in order, also
    // those no entry has any more. Caller holds a read lock.
    std::size_t nameChunkCount() const noexcept { return m_names.size(); }
    template <typename F> void forEachName(std::size_t chunk, F&& f) const
    {
        const char* const base = m_names[chunk].get();
        const auto start = static_cast<std::uint32_t>(chunk << kNameChunkBits);
        std::size_t at = 0;
        while (at < kNameChunkSize && base[at] != 0) {
            const std::string_view name = storedName(base + at);
            f(start + static_cast<std::uint32_t>(at), name);
            at = static_cast<std::size_t>(name.data() + name.size() - base);
        }
    }

    // ---- NTFS folder record numbers --------------------------------------
    // The change journal names a file's folder only by its record number in
    // the MFT, so folders on journaled volumes are registered here, one table
    // per volume root. Lookups ignore entries removed since.
    void setFolderRecord(EntryId root, std::uint32_t record, EntryId folder);
    EntryId folderByRecord(EntryId root, std::uint32_t record) const noexcept;
    void forgetFolderRecord(EntryId root, std::uint32_t record) noexcept;
    const RecordTable* folderRecords(EntryId root) const noexcept;

private:
    unsigned char nameTraits(std::uint32_t offset) const noexcept
    {
        const char* p = nameRecord(m_names, offset);
        return static_cast<unsigned char>(p[static_cast<unsigned char>(p[0]) < 0x80 ? 1 : 2]);
    }
    static const char* nameRecord(const std::vector<std::unique_ptr<char[]>>& names, std::uint32_t offset) noexcept
    {
        return names[offset >> kNameChunkBits].get() + (offset & (kNameChunkSize - 1));
    }
    static std::string_view storedName(const char* record) noexcept
    {
        const auto first = static_cast<unsigned char>(record[0]);
        if (first < 0x80)
            return {record + 2, first};
        return {record + 3, (std::size_t {first & 0x7Fu} << 8) | static_cast<unsigned char>(record[1])};
    }

    // A folder's children by name: open addressing over their ids, by the
    // hash of the ASCII-folded name. Each also has the child before it in
    // the sibling list, so taking one out of a big folder walks nothing.
    struct ChildTable {
        std::vector<EntryId> buckets; // kNoEntry: empty
        std::vector<EntryId> previous; // parallel to buckets
        std::size_t count = 0;
    };
    static constexpr std::size_t kNoSlot = ~std::size_t {0};

    Entry& mut(EntryId id) noexcept { return m_chunks[id >> kChunkBits][id & (kChunkSize - 1)]; }
    template <typename Lookup> EntryId walkPath(std::wstring_view path, Lookup&& lookup) const;
    std::pair<EntryId, std::size_t> shareRootOf(std::wstring_view path) const;
    EntryId tableFind(const ChildTable& table, std::string_view name, bool exactCase) const noexcept;
    std::size_t tableSlot(const ChildTable& table, EntryId child) const noexcept; // kNoSlot: not in it
    void tableInsert(ChildTable& table, EntryId child, EntryId previous);
    void tableErase(ChildTable& table, EntryId child) noexcept;
    ChildTable& buildTable(EntryId parent);
    EntryId allocate();
    std::uint32_t appendName(std::string_view name);
    void internInsert(std::uint64_t slot, std::size_t hash) noexcept;
    void link(EntryId parent, EntryId child);
    void unlink(EntryId child);
    EntryId rootOf(EntryId id) const noexcept;
    void noteChanged(EntryId file)
    {
        if (!m_tracking || m_changesLost)
            return;
        if (m_changes.size() >= kMaxChanges)
            loseChanges();
        else
            m_changes.push_back(file);
    }
    void loseChanges() noexcept
    {
        m_changesLost = true;
        std::vector<EntryId>().swap(m_changes);
    }

    mutable std::shared_mutex m_mutex;
    mutable std::shared_mutex m_writerGate; // see longReadLock()
    std::vector<std::unique_ptr<Entry[]>> m_chunks;
    std::vector<std::unique_ptr<FileTime[]>> m_times; // parallel to m_chunks
    std::vector<std::unique_ptr<FileTime[]>> m_created;
    std::vector<std::unique_ptr<char[]>> m_names;
    std::size_t m_count = 0;
    std::size_t m_nameUsed = kNameChunkSize; // forces allocation on first use
    std::atomic<std::size_t> m_live {0};
    mutable std::atomic<int> m_pins {0};
    std::atomic<std::uint64_t> m_generation {0};
    std::vector<EntryId> m_roots;
    std::vector<std::uint64_t> m_intern; // open addressing: offset<<32 | length<<16 | hash tag; 0 = empty
    std::size_t m_internCount = 0;
    std::vector<std::pair<EntryId, RecordTable>> m_folderRecords; // by volume root
    std::unordered_map<EntryId, ChildTable> m_childTables; // by folder; compact() drops them all
    // takeChanges(): written under the write lock, taken under a read lock.
    std::vector<EntryId> m_changes;
    bool m_tracking = false; // since the first takeChanges()
    bool m_changesLost = false;
    std::mutex m_changesTaking; // one caller at a time
};

std::uint8_t extensionLength(std::string_view name, bool isDir) noexcept;

} // namespace ws

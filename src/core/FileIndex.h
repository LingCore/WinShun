#pragma once

#include <QString>

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
inline constexpr std::uint8_t Persistent = Directory | Root | LowPriority | Hidden | Offline;
inline constexpr std::uint8_t Inherited = LowPriority | Hidden;
} // namespace EntryFlag

// One file or folder. Children form a sibling list, newest first. Taking a
// child out of it needs the one before: found by walking a small folder;
// a big one has a table of its children that knows (see ChildTable).
struct Entry {
    EntryId parent = kNoEntry;
    EntryId firstChild = kNoEntry;
    EntryId nextSibling = kNoEntry;
    std::uint32_t nameOffset = 0;
    std::uint16_t nameLength = 0;
    std::uint8_t flags = 0;
    std::uint8_t extLength = 0; // bytes after the last '.', 0 when there is no extension

    bool isDir() const noexcept { return flags & EntryFlag::Directory; }
    bool isDeleted() const noexcept { return flags & EntryFlag::Deleted; }
};
static_assert(sizeof(Entry) == 20, "keep Entry compact: there is one per file and folder");

// When a file or folder was last written: seconds since 1970 (UTC), 0 when
// not known. Good until 2106.
using FileTime = std::uint32_t;

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
    static constexpr std::size_t kNameChunkBits = 20;
    static constexpr std::size_t kNameChunkSize = std::size_t {1} << kNameChunkBits;

    FileIndex();
    ~FileIndex();
    FileIndex(const FileIndex&) = delete;
    FileIndex& operator=(const FileIndex&) = delete;

    [[nodiscard]] std::shared_lock<std::shared_mutex> readLock() const { return std::shared_lock(m_mutex); }
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
    std::string_view name(const Entry& e) const noexcept
    {
        return {m_names[e.nameOffset >> kNameChunkBits].get() + (e.nameOffset & (kNameChunkSize - 1)), e.nameLength};
    }
    std::string_view name(EntryId id) const noexcept { return name(entry(id)); }
    // Kept apart from the entries, which name searches go through: only the
    // matches have theirs looked at.
    FileTime modified(EntryId id) const noexcept { return m_times[id >> kChunkBits][id & (kChunkSize - 1)]; }

    std::size_t chunkCount() const noexcept { return m_chunks.size(); }
    std::span<const Entry> chunk(std::size_t index) const noexcept;
    const std::vector<EntryId>& roots() const noexcept { return m_roots; }

    // Names compare ignoring ASCII case, as NTFS does for most names.
    EntryId findChild(EntryId parent, std::string_view name) const;
    EntryId findPath(std::wstring_view path) const; // "C:\dir\file.txt"
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

    EntryId addRoot(std::string_view name); // "C:"
    EntryId add(EntryId parent, std::string_view name, std::uint8_t flags);
    // The same with a name already stored (storeName), for loading names
    // that are distinct already; a root when parent is kNoEntry.
    EntryId addStored(EntryId parent, std::uint32_t nameOffset, std::uint16_t nameLength, std::uint8_t flags);
    std::size_t remove(EntryId id); // removes the whole subtree; returns entries removed
    bool move(EntryId id, EntryId newParent, std::string_view newName);
    void setFlags(EntryId id, std::uint8_t flags);
    void setModified(EntryId id, FileTime time) noexcept { m_times[id >> kChunkBits][id & (kChunkSize - 1)] = time; }

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

    // Name storage, for bulk readers that collect names before adding the
    // entries (the MFT reader). Offsets stay valid until compact().
    std::uint32_t storeName(std::string_view name);
    std::string_view nameAt(std::uint32_t offset, std::size_t length) const noexcept
    {
        return {m_names[offset >> kNameChunkBits].get() + (offset & (kNameChunkSize - 1)), length};
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

    mutable std::shared_mutex m_mutex;
    mutable std::shared_mutex m_writerGate; // see longReadLock()
    std::vector<std::unique_ptr<Entry[]>> m_chunks;
    std::vector<std::unique_ptr<FileTime[]>> m_times; // parallel to m_chunks
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
};

std::uint8_t extensionLength(std::string_view name, bool isDir) noexcept;

} // namespace ws

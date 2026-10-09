#pragma once

#include "FileIndex.h"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ws {

// What to skip and what to rank lower while indexing.
//   *Paths: absolute folder paths without a trailing backslash.
//   *Names: a folder name matched at any depth, or a short tail such as
//           ".svn\pristine" (that folder inside any ".svn").
struct CrawlRules {
    std::vector<std::wstring> excludedPaths;
    std::vector<std::wstring> excludedNames;
    std::vector<std::wstring> lowPriorityPaths;
    std::vector<std::wstring> lowPriorityNames;

    bool operator==(const CrawlRules&) const = default;
};

struct VolumeInfo {
    std::wstring root; // "C:"
    std::uint32_t serial = 0;
    bool ntfs = false;

    bool operator==(const VolumeInfo&) const = default;
};

// How far a volume's NTFS change journal has been applied to the index.
struct JournalPosition {
    std::uint64_t journalId = 0; // 0: the volume is not followed through its journal
    std::int64_t usn = 0; // the first record not applied yet

    bool operator==(const JournalPosition&) const = default;
};

// Local volumes worth indexing (fixed disks, optionally removable ones).
// Drives in `untouched` are left out without being looked at: they may be
// in the middle of being ejected.
std::vector<VolumeInfo> listLocalVolumes(bool includeRemovable, const std::vector<std::wstring>& untouched = {});
bool driveLetterExists(std::wstring_view root); // "E:"

// Which disk a volume is on, and whether it is a hard disk.
struct VolumePlacement {
    std::uint32_t disk = ~0u; // unknown, or a volume across several disks
    bool seeks = true; // a hard disk, or not known not to be
};
VolumePlacement placementOf(std::wstring_view root); // "E:"

// `which` of `volumes` (indexes into it) in groups that are read at the same
// time: the volumes of one hard disk together, one after another (reading
// two at once would make its heads seek back and forth between them), and
// each volume of a disk that does not seek (an SSD) on its own.
std::vector<std::vector<std::size_t>> readingGroups(
    const std::vector<VolumeInfo>& volumes, const std::vector<std::size_t>& which);

// Drops the folders `rules` exclude (only its excluded paths and names are
// used) from the index, wherever they are: for rules added since it was
// built. Caller holds index.writeLock(). Returns the entries removed.
std::size_t removeExcluded(FileIndex& index, const CrawlRules& rules);

// The children of one folder, read from the disk or the MFT without holding
// the index lock. Names live in two buffers reused for every folder.
struct DirListing {
    struct Item {
        std::uint32_t offset;
        std::uint16_t length;
        std::uint8_t flags;
        bool descend;
        std::uint32_t wideOffset; // UTF-16 name, only kept for folders we descend into
        std::uint32_t wideLength;
        std::uint32_t record; // NTFS file record number, 0 when unknown
    };
    std::string names;
    std::wstring wideNames;
    std::vector<Item> items;
    std::wstring scratchPath; // a child's full path, while checking the rules
    std::vector<std::uint64_t> readBuffer; // raw directory reads

    void clear()
    {
        names.clear();
        wideNames.clear();
        items.clear();
    }
    std::string_view name(const Item& item) const { return {names.data() + item.offset, item.length}; }
    std::wstring_view wideName(const Item& item) const { return {wideNames.data() + item.wideOffset, item.wideLength}; }
};

// Walks directory trees on a small thread pool and brings a FileIndex in line
// with the disk (or with another source of folder listings, such as the MFT).
class Crawler {
public:
    struct Root {
        EntryId id = kNoEntry;
        std::wstring path; // "C:" or "C:\dir"
        std::uint8_t inheritedFlags = 0;
        EntryId volume = kNoEntry; // set: register subfolders' record numbers with this volume root
        std::uint32_t record = 0; // this folder's NTFS record number, 0 when unknown
    };

    // Reads one folder's children into `out`. Returns false when the folder
    // cannot be read: the index then keeps what it has for it.
    using Lister = std::function<bool(const Root& dir, DirListing& out)>;

    explicit Crawler(CrawlRules rules);

    // Reconciles everything below `roots` with the disk: adds new items,
    // removes vanished ones and refreshes flags, in place. On an empty index
    // this is simply a full crawl. Returns false when stopped early.
    // `list` replaces reading the disk (it must be safe to call on `threads` threads).
    bool sync(FileIndex& index, std::vector<Root> roots, int threads, bool lowPriority, std::stop_token stop,
        const Lister& list = {}) const;

    // Reads a folder from the disk, with each child's record number.
    bool listDirectory(const Root& dir, DirListing& out) const;

    // Appends one child of `dir` to `out`, applying the rules to folders:
    // excluded ones are left out, the others get their priority flags.
    // `wideName` is only needed for folders.
    void addToListing(DirListing& out, const Root& dir, std::string_view name, std::wstring_view wideName,
        std::uint8_t flags, bool descend, std::uint32_t record) const;

    bool isExcludedDir(std::wstring_view fullPath, std::wstring_view name) const;
    bool isExcludedPath(std::wstring_view fullPath) const; // the path or one of its ancestors
    std::uint8_t dirPriorityFlags(std::wstring_view fullPath, std::wstring_view name) const;

    static std::uint8_t attributeFlags(unsigned long attributes) noexcept;
    static bool shouldDescend(unsigned long attributes, unsigned long reparseTag) noexcept;

    const CrawlRules& rules() const noexcept { return m_rules; }

private:
    CrawlRules m_rules;
};

} // namespace ws

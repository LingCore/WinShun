#pragma once

#include "ChangeWatcher.h"
#include "FileIndex.h"

#include <cstdint>
#include <functional>
#include <optional>
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

// A volume on a disk that is carried around: what Windows calls removable
// (USB sticks, memory cards), and disks that say "fixed" but hang on USB or
// FireWire (external hard disks, fast USB sticks: GitHub #266). Never one
// on the disk Windows runs from (Windows To Go, a tablet's own storage).
// "包括 U 盘和移动硬盘" decides about them (Settings::includeRemovableDrives).
// For tests on a machine without one: WINSHUN_EXTERNAL=E:,F: counts those.
bool isExternalVolume(std::wstring_view root);

// Local volumes worth indexing (fixed disks, optionally external ones:
// isExternalVolume). Drives in `untouched` are left out without being looked
// at: they may be in the middle of being ejected.
// Unset: all eligible disks. An empty selection means no local disks.
bool volumeSelected(std::wstring_view root, const std::optional<std::vector<std::wstring>>& selected);
std::vector<VolumeInfo> listLocalVolumes(bool includeRemovable, const std::vector<std::wstring>& untouched = {},
    const std::optional<std::vector<std::wstring>>& selected = {});
bool driveLetterExists(std::wstring_view root); // "E:"
// A junction/symlink/mount point on the way to `path`, after `ancestor`.
// Cloud placeholders are not directory links. No link is followed while probing.
bool hasDirectoryLink(std::wstring_view path, std::wstring_view ancestor = {});

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
        FileTime modified;
        FileSize size; // packed; kUnknownSize for a folder, or when not read
        FileTime created = 0;
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

    // The same, but what is gone from the disk is only collected in
    // `deferRemovals`, not removed: a network share that drops out halfway
    // (or comes back empty for a moment) must not cost the index its items.
    // The caller removes them once it trusts the whole pass.
    struct SyncStats {
        std::size_t listed = 0; // folders read
        std::size_t failed = 0; // folders that could not be read (left as they were)
        std::size_t items = 0; // in the folders read
    };
    bool sync(FileIndex& index, std::vector<Root> roots, int threads, bool lowPriority, std::stop_token stop,
        const Lister& list, std::vector<EntryId>* deferRemovals, SyncStats* stats) const;

    // Reads a folder from the disk (or a share), with each child's record number.
    bool listDirectory(const Root& dir, DirListing& out) const;

    // Appends one child of `dir` to `out`, applying the rules to folders:
    // excluded ones are left out, the others get their priority flags.
    // `wideName` is only needed for folders.
    void addToListing(DirListing& out, const Root& dir, std::string_view name, std::wstring_view wideName,
        std::uint8_t flags, bool descend, std::uint32_t record, FileTime modified, FileSize size = kUnknownSize,
        FileTime created = 0) const;

    bool isExcludedDir(std::wstring_view fullPath, std::wstring_view name) const;
    bool isExcludedPath(std::wstring_view fullPath) const; // the path or one of its ancestors
    std::uint8_t dirPriorityFlags(std::wstring_view fullPath, std::wstring_view name) const;

    static std::uint8_t attributeFlags(unsigned long attributes) noexcept;
    static bool shouldDescend(unsigned long attributes, unsigned long reparseTag) noexcept;

    const CrawlRules& rules() const noexcept { return m_rules; }

private:
    CrawlRules m_rules;
};

// A file or folder as it is on disk now.
struct PathProbe {
    unsigned long attributes = 0xFFFF'FFFF; // INVALID_FILE_ATTRIBUTES: not there
    unsigned long reparseTag = 0;
    FileTime modified = 0;
    FileSize size = kUnknownSize; // a file's, packed
    FileTime created = 0;
};
PathProbe probePath(const std::wstring& path);

// Applies what a ChangeWatcher reported, a batch per write lock. New folders
// are walked with `list` (the disk when empty). The caller keeps compaction
// out meanwhile: new folders are walked by their entry ids.
void applyChanges(FileIndex& index, const std::vector<FsChange>& changes, const Crawler& crawler, std::stop_token stop,
    const Crawler::Lister& list = {});

} // namespace ws

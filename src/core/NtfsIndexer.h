#pragma once

#include "Crawler.h"
#include "FileIndex.h"
#include "Ntfs.h"

#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace qf {

// Every name on an NTFS volume, read from its MFT and grouped by folder: the
// listing source for a Crawler sync of the volume. Several times faster than
// walking the folders, and sees each hard link of a file under its own name.
class MftTree {
public:
    // Reads the volume's MFT. Names go straight into the index's name storage
    // (keep interning on until the sync is done); the caller holds no lock.
    bool read(std::wstring_view root, FileIndex& index, std::stop_token stop, std::wstring* error = nullptr);

    // Crawler::Lister for one folder (Root::record).
    bool list(const FileIndex& index, const Crawler& crawler, const Crawler::Root& dir, DirListing& out) const;

    std::size_t nameCount() const noexcept { return m_folders.size() + m_files.size(); }

private:
    struct Name {
        std::uint32_t parent; // record number of the folder
        std::uint32_t nameOffset; // in the index's name storage
        std::uint16_t nameLength;
        std::uint8_t flags; // EntryFlag: Directory, Hidden, Offline
    };
    struct Folder {
        Name name;
        std::uint32_t record;
    };
    std::vector<Folder> m_folders; // sorted by parent
    std::vector<Name> m_files; // sorted by parent
};

// Applies one volume's change journal records to the index, in order. Every
// step is idempotent, so replaying records the index already reflects (after
// a restart, or a read of the MFT) does no harm.
class UsnApplier {
public:
    UsnApplier(FileIndex& index, const Crawler& crawler, std::string rootName); // "C:"

    // Takes the index's write lock in small batches. Folders moved in from
    // outside the index are walked. The caller keeps compaction away meanwhile.
    void apply(std::span<const ntfs::UsnRecord> records, std::stop_token stop);

    // Where a replay has to start so nothing is lost: before the oldest
    // rename whose new name has not been seen yet, or else `next`.
    std::int64_t safePosition(std::int64_t next) const noexcept;

private:
    struct OldName {
        std::uint32_t parent; // record number of the folder
        std::string name;
        std::int64_t usn;
    };
    struct Item; // one record, decoded

    void applyOne(const ntfs::UsnRecord& record, std::vector<Crawler::Root>& walks);
    void remove(const Item& item);
    void move(const Item& item, const OldName* old, std::vector<Crawler::Root>& walks);
    void ensure(const Item& item, bool walk, std::vector<Crawler::Root>& walks);
    void add(EntryId parent, const Item& item, bool walk, std::vector<Crawler::Root>& walks);
    void updateFlags(const Item& item);
    EntryId find(EntryId parent, std::string_view name) const;
    EntryId findItem(const Item& item) const;
    std::wstring childPath(EntryId parent, std::wstring_view name) const;
    std::uint8_t flagsFor(EntryId parent, const Item& item, const std::wstring& path) const;

    FileIndex& m_index;
    const Crawler& m_crawler;
    std::string m_rootName;
    EntryId m_root = kNoEntry; // looked up per batch: compaction renumbers it
    std::unordered_map<std::uint64_t, OldName> m_oldNames; // renames waiting for their new name, by file
    std::unordered_map<std::uint64_t, std::uint32_t> m_seen; // reasons applied in each file's open session
};

} // namespace qf

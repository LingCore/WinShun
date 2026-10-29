#include "NtfsIndexer.h"

#include "TextUtil.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>

namespace ws {

namespace {

constexpr std::uint32_t kFirstUserRecord = 16; // below: NTFS metadata ($MFT, $Extend, ...) and the root
constexpr std::size_t kApplyBatch = 256; // journal records applied per write lock
constexpr std::uint32_t kNameReasons = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME
    | USN_REASON_RENAME_NEW_NAME | USN_REASON_BASIC_INFO_CHANGE | USN_REASON_HARD_LINK_CHANGE;
constexpr std::uint32_t kDataReasons = USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION;

// Attributes as stored by NTFS (in the MFT and the journal) to EntryFlags.
std::uint8_t flagsOf(std::uint32_t attributes, bool folder) noexcept
{
    // On disk, 0x40000 means "has extended attributes" (code integrity caches
    // its verdict on most .exe and .dll files that way); the API reports the
    // same bit as FILE_ATTRIBUTE_RECALL_ON_OPEN only for cloud placeholders.
    constexpr std::uint32_t kHasExtendedAttributes = 0x0004'0000;
    attributes &= ~(kHasExtendedAttributes | static_cast<std::uint32_t>(FILE_ATTRIBUTE_DIRECTORY));
    return Crawler::attributeFlags(folder ? attributes | FILE_ATTRIBUTE_DIRECTORY : attributes);
}

template <typename T> auto parentRange(const std::vector<T>& v, std::uint32_t parent, auto parentOf)
{
    const auto first = std::partition_point(v.begin(), v.end(), [&](const T& x) { return parentOf(x) < parent; });
    const auto last = std::partition_point(first, v.end(), [&](const T& x) { return parentOf(x) == parent; });
    return std::pair {first, last};
}

} // namespace

// ---- MftTree ------------------------------------------------------------------

bool MftTree::read(
    ntfs::RecordSource& reader, FileIndex& index, std::stop_token stop, std::wstring* error, const Progress& progress)
{
    m_folders.clear();
    m_files.clear();
    const auto failed = [&] {
        if (error)
            *error = reader.valid() ? L"stopped" : reader.error();
        return false;
    };
    if (!reader.valid())
        return failed();
    // Mostly one name per record; hard links add a few more. Pages of the
    // reservation that stay unused are never touched, so they cost no memory.
    const std::uint64_t records = reader.recordCount();
    m_files.reserve(static_cast<std::size_t>(records + records / 8));
    m_folders.reserve(static_cast<std::size_t>(records / 5));

    struct Collected {
        std::uint32_t record;
        std::uint32_t parent;
        std::uint32_t begin;
        std::uint16_t length;
        std::uint8_t flags;
        bool extension;
        FileTime modified;
        FileSize size;
        FileTime created;
    };
    std::vector<Collected> block;
    std::string names;
    std::vector<std::pair<std::uint32_t, Name>> extensionNames; // by base record
    struct Base {
        std::uint8_t flags;
        FileTime modified;
        FileTime created;
    };
    std::unordered_map<std::uint32_t, Base> bases; // records whose names may be in extensions
    // A file with an attribute list may keep its $DATA in an extension
    // record: its size comes from there, by base record, once all are read.
    std::unordered_map<std::uint32_t, FileSize> extensionSizes;
    std::vector<std::pair<std::uint32_t, std::size_t>> unsized; // files of such records (by base), in m_files
    ntfs::FileRecord record;
    while (reader.next()) {
        if (stop.stop_requested())
            return failed();
        block.clear();
        names.clear();
        for (std::size_t i = 0; i < reader.blockRecords(); ++i) {
            if (!reader.parse(i, record) || !record.inUse)
                continue;
            const auto number = static_cast<std::uint32_t>(reader.firstRecord() + i);
            const bool extension = record.baseRecord != 0;
            const std::uint32_t owner = extension ? record.baseRecord : number;
            if (owner < kFirstUserRecord)
                continue;
            const std::uint8_t flags = extension ? 0 : flagsOf(record.attributes, record.directory);
            const FileTime modified = extension ? 0 : fileTimeOf(record.modified);
            const FileTime created = extension ? 0 : fileTimeOf(record.created);
            const FileSize size = record.directory || !record.sizeKnown ? kUnknownSize : packSize(record.size);
            if (!extension && record.hasAttributeList)
                bases[number] = {flags, modified, created};
            if (extension && record.sizeKnown)
                extensionSizes[owner] = packSize(record.size);
            for (const auto& n : record.names) {
                const auto begin = static_cast<std::uint32_t>(names.size());
                wtf8::append(names, n.name);
                block.push_back({owner, ntfs::recordOf(n.parent), begin,
                    static_cast<std::uint16_t>(names.size() - begin), flags, extension, modified, size, created});
            }
        }
        {
            // One lock per block of records to store their names.
            auto lock = index.writeLock();
            for (const Collected& c : block) {
                const Name name {c.parent, index.storeName({names.data() + c.begin, c.length}), c.flags, c.modified, c.size, c.created};
                if (c.extension) {
                    extensionNames.emplace_back(c.record, name);
                } else if (c.flags & EntryFlag::Directory) {
                    m_folders.push_back({name, c.record});
                } else {
                    if (c.size == kUnknownSize && bases.contains(c.record))
                        unsized.emplace_back(c.record, m_files.size());
                    m_files.push_back(name);
                }
            }
        }
        if (progress)
            progress(reader.bytesDone(), reader.bytes());
    }
    if (!reader.valid())
        return failed();

    // Names kept in extension records (files with very many links) take the
    // flags and time of their base record; a $DATA kept there gives the
    // file its size.
    const auto sizeOf = [&](std::uint32_t base, FileSize known) {
        if (known != kUnknownSize)
            return known;
        const auto it = extensionSizes.find(base);
        return it == extensionSizes.end() ? kUnknownSize : it->second;
    };
    for (auto [base, name] : extensionNames) {
        const auto it = bases.find(base);
        if (it == bases.end())
            continue;
        name.flags = it->second.flags;
        name.modified = it->second.modified;
        name.created = it->second.created;
        name.size = name.flags & EntryFlag::Directory ? kUnknownSize : sizeOf(base, name.size);
        if (name.flags & EntryFlag::Directory)
            m_folders.push_back({name, base});
        else
            m_files.push_back(name);
    }
    for (const auto& [base, at] : unsized)
        m_files[at].size = sizeOf(base, kUnknownSize);
    std::sort(m_folders.begin(), m_folders.end(), [](const Folder& a, const Folder& b) { return a.name.parent < b.name.parent; });
    std::sort(m_files.begin(), m_files.end(), [](const Name& a, const Name& b) { return a.parent < b.parent; });
    return true;
}

bool MftTree::list(const FileIndex& index, const Crawler& crawler, const Crawler::Root& dir, DirListing& out) const
{
    out.clear();
    const auto [firstFolder, lastFolder] = parentRange(m_folders, dir.record, [](const Folder& f) { return f.name.parent; });
    const auto [firstFile, lastFile] = parentRange(m_files, dir.record, [](const Name& n) { return n.parent; });
    std::u16string wide;
    const auto lock = index.readLock(); // the names live in the index
    for (auto it = firstFolder; it != lastFolder; ++it) {
        const std::string_view name = index.nameAt(it->name.nameOffset);
        wide.clear();
        wtf8::decodeAppend(wide, name);
        // Junctions and other links have no children in the MFT: descending is harmless.
        crawler.addToListing(out, dir, name, wtf8::wview(wide), it->name.flags, true, it->record, it->name.modified,
            kUnknownSize, it->name.created);
    }
    for (auto it = firstFile; it != lastFile; ++it)
        crawler.addToListing(out, dir, index.nameAt(it->nameOffset), {}, it->flags, false, 0, it->modified, it->size, it->created);
    return true;
}

// ---- UsnApplier -------------------------------------------------------------

struct UsnApplier::Item {
    std::uint32_t record = 0;
    std::uint32_t parent = 0; // record number of the folder
    std::string name;
    std::wstring wideName;
    std::uint32_t attributes = 0;
    bool folder = false;
};

UsnApplier::UsnApplier(FileIndex& index, const Crawler& crawler, std::string rootName, Written written)
    : m_index(index)
    , m_crawler(crawler)
    , m_rootName(std::move(rootName))
    , m_onWritten(std::move(written))
{
}

void UsnApplier::apply(std::span<const ntfs::UsnRecord> records, std::stop_token stop)
{
    std::vector<Crawler::Root> walks;
    std::size_t i = 0;
    while (i < records.size() && !stop.stop_requested()) {
        const std::size_t end = std::min(records.size(), i + kApplyBatch);
        {
            auto lock = m_index.writeLock();
            const auto& roots = m_index.roots();
            const auto it = std::find_if(roots.begin(), roots.end(), [&](EntryId r) { return m_index.name(r) == m_rootName; });
            if (it == roots.end())
                return; // the volume is no longer indexed
            m_root = *it;
            for (; i < end; ++i) {
                applyOne(records[i], walks);
                // After: a file created or renamed by the record has its entry now.
                noteWritten(records[i]);
                noteTime(records[i]);
            }
        }
        // Told once the write lock is let go (GitHub #115): the content index
        // waits for a content search's lookup that reads it off the disk
        // (seconds, from a hard disk), and name searches would wait for this
        // lock all that time. The caller's journal lock keeps the ids as they
        // are meanwhile (no compaction).
        if (!m_written.empty()) {
            m_onWritten(m_written);
            m_written.clear();
        }
        if (!m_toRead.empty())
            readTimes();
        if (!walks.empty()) {
            // Folders moved in from outside the index: their contents are new to it.
            m_crawler.sync(m_index, std::move(walks), 1, true, stop);
            walks.clear();
        }
    }
    if (m_seen.size() > 100'000)
        m_seen.clear(); // files kept open for ages; forgetting them only costs a repeat
}

std::int64_t UsnApplier::safePosition(std::int64_t next) const noexcept
{
    for (const auto& [file, old] : m_oldNames)
        next = std::min(next, old.usn);
    return next;
}

void UsnApplier::applyOne(const ntfs::UsnRecord& r, std::vector<Crawler::Root>& walks)
{
    const std::uint32_t reason = r.reason & kNameReasons;
    const bool closed = r.reason & USN_REASON_CLOSE;
    if (reason == 0) {
        if (closed)
            m_seen.erase(r.file);
        return;
    }
    Item item;
    item.record = ntfs::recordOf(r.file);
    item.parent = ntfs::recordOf(r.parent);
    item.name = wtf8::fromUtf16(r.name);
    item.attributes = r.attributes;
    item.folder = r.attributes & FILE_ATTRIBUTE_DIRECTORY;

    if ((reason & USN_REASON_RENAME_OLD_NAME) && !(reason & USN_REASON_RENAME_NEW_NAME)) {
        // The name before a rename: kept until the record with the new name.
        if (m_oldNames.size() > 10'000)
            m_oldNames.clear();
        m_oldNames[r.file] = {item.parent, std::move(item.name), r.usn};
        return;
    }
    item.wideName.assign(wtf8::wview(r.name));

    // Records repeat every reason gathered since the file was opened, until
    // it is closed: apply each one once (renames each time they happen).
    const auto old = m_oldNames.find(r.file);
    std::uint32_t& seen = m_seen[r.file];
    const bool fresh = (reason & ~seen) != 0 || (old != m_oldNames.end() && (reason & USN_REASON_RENAME_NEW_NAME));
    seen |= reason;
    if (closed)
        m_seen.erase(r.file);
    if (!fresh)
        return;

    if (reason & USN_REASON_FILE_DELETE) {
        remove(item);
    } else if (reason & USN_REASON_RENAME_NEW_NAME) {
        move(item, old != m_oldNames.end() ? &old->second : nullptr, walks);
    } else if ((reason & USN_REASON_HARD_LINK_CHANGE) && !(reason & USN_REASON_FILE_CREATE)) {
        // A link was added or removed: the record names it, the disk tells which.
        const EntryId parent = m_index.folderByRecord(m_root, item.parent);
        if (parent != kNoEntry) {
            const std::wstring path = win32::longPath(childPath(parent, item.wideName));
            if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                ensure(item, false, walks);
            else
                remove(item);
        }
    } else if (reason & USN_REASON_FILE_CREATE) {
        ensure(item, false, walks);
    }
    if ((reason & USN_REASON_BASIC_INFO_CHANGE) && !(reason & USN_REASON_FILE_DELETE))
        updateFlags(item);
    if (old != m_oldNames.end() && (reason & (USN_REASON_RENAME_NEW_NAME | USN_REASON_FILE_DELETE)))
        m_oldNames.erase(old);
}

void UsnApplier::noteWritten(const ntfs::UsnRecord& r)
{
    if (!m_onWritten || !(r.reason & kDataReasons) || (r.reason & USN_REASON_FILE_DELETE)
        || (r.attributes & FILE_ATTRIBUTE_DIRECTORY))
        return;
    const EntryId parent = m_index.folderByRecord(m_root, ntfs::recordOf(r.parent));
    if (parent == kNoEntry)
        return; // outside the index
    const EntryId id = find(parent, wtf8::fromUtf16(r.name));
    if (id != kNoEntry && !m_index.entry(id).isDir())
        m_written.push_back(id);
}

// The journal says when each change was made: when the file was written,
// or a name added to or taken from its folder (NTFS writes the folder then).
// Times only move forward here, so that replaying records the index already
// reflects leaves the newer time read from the MFT. Times set on purpose
// (a copy or an unpacked file keeps the original's) can be anything: those
// are read from the disk once the file is closed.
void UsnApplier::noteTime(const ntfs::UsnRecord& r)
{
    const FileTime time = fileTimeOf(r.time);
    if (time == 0)
        return;
    const auto later = [&](EntryId id) {
        if (id != kNoEntry && m_index.modified(id) < time)
            m_index.setModified(id, time);
    };
    constexpr std::uint32_t kListingReasons = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE
        | USN_REASON_RENAME_OLD_NAME | USN_REASON_RENAME_NEW_NAME;
    if (r.reason & kListingReasons)
        later(m_index.folderByRecord(m_root, ntfs::recordOf(r.parent)));
    if (r.reason & USN_REASON_FILE_DELETE)
        return;
    const bool timesSet = r.reason & USN_REASON_BASIC_INFO_CHANGE;
    if (timesSet ? !(r.reason & USN_REASON_CLOSE) : !(r.reason & (USN_REASON_FILE_CREATE | kDataReasons)))
        return;
    Item item;
    item.record = ntfs::recordOf(r.file);
    item.parent = ntfs::recordOf(r.parent);
    item.name = wtf8::fromUtf16(r.name);
    item.folder = r.attributes & FILE_ATTRIBUTE_DIRECTORY;
    const EntryId id = findItem(item);
    if (id == kNoEntry || (m_index.entry(id).flags & EntryFlag::Root))
        return;
    // A file closed after being written: its size has changed too, and the
    // journal does not say to what (a stat, cheap: it was just written).
    if (timesSet || (!item.folder && (r.reason & USN_REASON_CLOSE)))
        m_toRead.emplace_back(id, m_index.wpath(id));
    else
        later(id);
}

void UsnApplier::readTimes()
{
    struct Read {
        EntryId id;
        FileTime time;
        FileSize size;
        FileTime created;
    };
    std::vector<Read> read;
    read.reserve(m_toRead.size());
    for (const auto& [id, path] : m_toRead) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (::GetFileAttributesExW(win32::longPath(path).c_str(), GetFileExInfoStandard, &data)) {
            const bool folder = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            read.push_back({id, fileTimeOf(win32::ticks(data.ftLastWriteTime)),
                folder ? kUnknownSize : packSize((static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow),
                fileTimeOf(win32::ticks(data.ftCreationTime))});
        }
    }
    m_toRead.clear();
    auto lock = m_index.writeLock();
    for (const Read& r : read) {
        if (r.id < m_index.slotCount() && !m_index.entry(r.id).isDeleted()) {
            m_index.setModified(r.id, r.time); // ids stay: the caller keeps compaction away
            m_index.setCreated(r.id, r.created);
            if (r.size != kUnknownSize)
                m_index.setSize(r.id, r.size);
        }
    }
}

EntryId UsnApplier::find(EntryId parent, std::string_view name)
{
    // The exact spelling only: the journal reports case-only renames too, and
    // case-sensitive folders may hold both "a" and "A".
    return m_index.childForUpdate(parent, name, true);
}

EntryId UsnApplier::findItem(const Item& item)
{
    if (item.folder) {
        const EntryId id = m_index.folderByRecord(m_root, item.record);
        if (id != kNoEntry && text::equalsIgnoreAsciiCase(m_index.name(id), item.name))
            return id;
    }
    const EntryId parent = m_index.folderByRecord(m_root, item.parent);
    const EntryId id = parent != kNoEntry ? find(parent, item.name) : kNoEntry;
    return id != kNoEntry && m_index.entry(id).isDir() == item.folder ? id : kNoEntry;
}

std::wstring UsnApplier::childPath(EntryId parent, std::wstring_view name) const
{
    std::wstring path = m_index.wpath(parent);
    if (!path.empty() && path.back() == L'\\')
        path.pop_back(); // a volume root: "C:\"
    path.push_back(L'\\');
    path.append(name);
    return path;
}

std::uint8_t UsnApplier::flagsFor(EntryId parent, const Item& item, const std::wstring& path) const
{
    auto flags = static_cast<std::uint8_t>(
        flagsOf(item.attributes, item.folder) | (m_index.entry(parent).flags & EntryFlag::Inherited));
    if (item.folder)
        flags |= m_crawler.dirPriorityFlags(path, item.wideName);
    return flags;
}

void UsnApplier::remove(const Item& item)
{
    const EntryId id = findItem(item);
    if (id != kNoEntry && !(m_index.entry(id).flags & EntryFlag::Root))
        m_index.remove(id);
    if (item.folder)
        m_index.forgetFolderRecord(m_root, item.record);
}

void UsnApplier::move(const Item& item, const OldName* old, std::vector<Crawler::Root>& walks)
{
    EntryId id = kNoEntry;
    if (item.folder) {
        id = m_index.folderByRecord(m_root, item.record);
    } else if (old) {
        const EntryId from = m_index.folderByRecord(m_root, old->parent);
        id = from != kNoEntry ? find(from, old->name) : kNoEntry;
        if (id != kNoEntry && m_index.entry(id).isDir())
            id = kNoEntry;
    }
    if (id != kNoEntry && (m_index.entry(id).flags & EntryFlag::Root))
        return;
    const EntryId parent = m_index.folderByRecord(m_root, item.parent);
    const std::wstring path = parent != kNoEntry && item.folder ? childPath(parent, item.wideName) : std::wstring();
    if (parent == kNoEntry || (item.folder && m_crawler.isExcludedDir(path, item.wideName))) {
        // Moved out of the index: into the recycle bin, an excluded folder, ...
        if (id != kNoEntry)
            m_index.remove(id);
        return;
    }
    if (id == kNoEntry) {
        ensure(item, true, walks); // moved in from outside the index
        return;
    }
    const EntryId existing = find(parent, item.name);
    if (existing != kNoEntry && existing != id)
        m_index.remove(existing); // the move replaced it
    if (m_index.move(id, parent, item.name))
        m_index.setFlags(id, flagsFor(parent, item, path));
}

void UsnApplier::ensure(const Item& item, bool walk, std::vector<Crawler::Root>& walks)
{
    const EntryId parent = m_index.folderByRecord(m_root, item.parent);
    if (parent == kNoEntry)
        return;
    if (item.folder) {
        const EntryId known = m_index.folderByRecord(m_root, item.record);
        if (known != kNoEntry) {
            if (m_index.entry(known).parent != parent || m_index.name(known) != item.name)
                move(item, nullptr, walks); // a rename we did not see
            return;
        }
    }
    const EntryId id = find(parent, item.name);
    if (id == kNoEntry)
        add(parent, item, walk, walks);
    else if (item.folder && m_index.entry(id).isDir())
        m_index.setFolderRecord(m_root, item.record, id);
}

void UsnApplier::add(EntryId parent, const Item& item, bool walk, std::vector<Crawler::Root>& walks)
{
    std::wstring path;
    if (item.folder) {
        path = childPath(parent, item.wideName);
        if (m_crawler.isExcludedDir(path, item.wideName))
            return;
    }
    const std::uint8_t flags = flagsFor(parent, item, path);
    const EntryId id = m_index.add(parent, item.name, flags);
    if (!item.folder)
        return;
    m_index.setFolderRecord(m_root, item.record, id);
    if (!walk)
        return; // just created: empty
    DWORD reparseTag = 0;
    if (item.attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        WIN32_FIND_DATAW fd;
        const win32::UniqueFind find(::FindFirstFileExW(
            win32::longPath(path).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0));
        if (!find.valid())
            return;
        reparseTag = fd.dwReserved0;
    }
    if (Crawler::shouldDescend(item.attributes, reparseTag))
        walks.push_back({id, std::move(path), static_cast<std::uint8_t>(flags & EntryFlag::Inherited), m_root, item.record});
}

void UsnApplier::updateFlags(const Item& item)
{
    const EntryId id = findItem(item);
    if (id == kNoEntry || (m_index.entry(id).flags & EntryFlag::Root))
        return;
    const Entry& e = m_index.entry(id);
    const auto inherited = static_cast<std::uint8_t>(m_index.entry(e.parent).flags & EntryFlag::Inherited);
    m_index.setFlags(id, static_cast<std::uint8_t>(
        flagsOf(item.attributes, item.folder) | inherited | (e.flags & EntryFlag::LowPriority)));
}

} // namespace ws

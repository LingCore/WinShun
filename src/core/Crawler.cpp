#include "Crawler.h"

#include "TextUtil.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <thread>
#include <utility>

namespace ws {

std::vector<VolumeInfo> listLocalVolumes(bool includeRemovable, const std::vector<std::wstring>& untouched)
{
    std::vector<VolumeInfo> volumes;
    const DWORD mask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i)))
            continue;
        const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (std::any_of(untouched.begin(), untouched.end(),
                [&](const std::wstring& u) { return win32::equalsIgnoreCase(u, std::wstring_view(root, 2)); }))
            continue;
        const UINT type = ::GetDriveTypeW(root);
        if (type != DRIVE_FIXED && !(includeRemovable && type == DRIVE_REMOVABLE))
            continue;
        DWORD serial = 0;
        wchar_t fileSystem[MAX_PATH + 1] = {};
        if (!::GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, fileSystem, MAX_PATH + 1))
            continue; // no media, or not ready
        volumes.push_back({std::wstring(root, 2), serial, std::wstring_view(fileSystem) == L"NTFS"});
    }
    return volumes;
}

bool driveLetterExists(std::wstring_view root)
{
    if (root.size() < 2 || root[1] != L':')
        return false;
    const auto letter = static_cast<wchar_t>(std::towupper(root[0]));
    return letter >= L'A' && letter <= L'Z' && (::GetLogicalDrives() & (1u << (letter - L'A')));
}

VolumePlacement placementOf(std::wstring_view root)
{
    VolumePlacement p;
    const std::wstring path = L"\\\\.\\" + std::wstring(root);
    const win32::UniqueHandle h(::CreateFileW(
        path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!h.valid())
        return p;
    VOLUME_DISK_EXTENTS extents {}; // room for one: a volume across disks fails with ERROR_MORE_DATA
    DWORD bytes = 0;
    if (::DeviceIoControl(h.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &extents, sizeof extents, &bytes,
            nullptr)
        && extents.NumberOfDiskExtents == 1)
        p.disk = extents.Extents[0].DiskNumber;
    STORAGE_PROPERTY_QUERY query {StorageDeviceSeekPenaltyProperty, PropertyStandardQuery, {}};
    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty {};
    if (::DeviceIoControl(h.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &penalty, sizeof penalty, &bytes,
            nullptr)
        && bytes >= sizeof penalty)
        p.seeks = penalty.IncursSeekPenalty;
    return p;
}

std::vector<std::vector<std::size_t>> readingGroups(
    const std::vector<VolumeInfo>& volumes, const std::vector<std::size_t>& which)
{
    std::vector<std::vector<std::size_t>> groups;
    std::vector<std::pair<std::uint32_t, std::size_t>> hardDisks; // disk, its group
    for (const std::size_t i : which) {
        const VolumePlacement p = placementOf(volumes[i].root);
        if (!p.seeks) {
            groups.push_back({i});
            continue;
        }
        const auto it = std::find_if(hardDisks.begin(), hardDisks.end(), [&](const auto& d) { return d.first == p.disk; });
        if (it != hardDisks.end()) {
            groups[it->second].push_back(i);
        } else {
            hardDisks.emplace_back(p.disk, groups.size());
            groups.push_back({i});
        }
    }
    return groups;
}

Crawler::Crawler(CrawlRules rules)
    : m_rules(std::move(rules))
{
}

std::uint8_t Crawler::attributeFlags(unsigned long attributes) noexcept
{
    std::uint8_t flags = 0;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        flags |= EntryFlag::Directory;
    if (attributes & FILE_ATTRIBUTE_HIDDEN)
        flags |= EntryFlag::Hidden;
    if (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))
        flags |= EntryFlag::Offline;
    return flags;
}

bool Crawler::shouldDescend(unsigned long attributes, unsigned long reparseTag) noexcept
{
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    // Junctions, symlinks and mount points would index things twice or loop.
    // Cloud-sync folders (OneDrive) are reparse points too, but not surrogates.
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(reparseTag))
        return false;
    return true;
}

namespace {

// "C:\a\.svn\pristine" ends with the tail ".svn\pristine".
bool endsWithTail(std::wstring_view path, std::wstring_view tail)
{
    return path.size() > tail.size() && path[path.size() - tail.size() - 1] == L'\\'
        && win32::equalsIgnoreCase(path.substr(path.size() - tail.size()), tail);
}

bool matchesNameRule(std::wstring_view rule, std::wstring_view fullPath, std::wstring_view name)
{
    return rule.find(L'\\') == std::wstring_view::npos ? win32::equalsIgnoreCase(rule, name)
                                                       : endsWithTail(fullPath, rule);
}

} // namespace

bool Crawler::isExcludedDir(std::wstring_view fullPath, std::wstring_view name) const
{
    for (const auto& n : m_rules.excludedNames) {
        if (matchesNameRule(n, fullPath, name))
            return true;
    }
    for (const auto& p : m_rules.excludedPaths) {
        if (win32::equalsIgnoreCase(p, fullPath))
            return true;
    }
    return false;
}

bool Crawler::isExcludedPath(std::wstring_view fullPath) const
{
    // Check every ancestor folder ("C:\a", "C:\a\b", ...) and the path itself.
    std::size_t start = fullPath.find(L'\\');
    while (start != std::wstring_view::npos) {
        const std::size_t end = fullPath.find(L'\\', start + 1);
        const std::wstring_view prefix = fullPath.substr(0, end);
        const std::wstring_view name = prefix.substr(start + 1);
        if (!name.empty() && isExcludedDir(prefix, name))
            return true;
        start = end;
    }
    return false;
}

std::uint8_t Crawler::dirPriorityFlags(std::wstring_view fullPath, std::wstring_view name) const
{
    // Dot-folders (.git, .vscode, .cache, ...) hold tool data, not documents.
    if (name.size() > 1 && name[0] == L'.')
        return EntryFlag::LowPriority;
    for (const auto& n : m_rules.lowPriorityNames) {
        if (matchesNameRule(n, fullPath, name))
            return EntryFlag::LowPriority;
    }
    for (const auto& p : m_rules.lowPriorityPaths) {
        if (win32::equalsIgnoreCase(p, fullPath))
            return EntryFlag::LowPriority;
    }
    return 0;
}

namespace {

constexpr std::size_t kDirReadBytes = 64 * 1024;

std::uint32_t recordNumber(LARGE_INTEGER fileId) noexcept
{
    // The low 48 bits of an NTFS file id; ids past 32 bits are left unknown.
    const auto record = static_cast<std::uint64_t>(fileId.QuadPart) & 0xFFFF'FFFF'FFFFull;
    return record <= 0xFFFF'FFFFull ? static_cast<std::uint32_t>(record) : 0;
}

} // namespace

void Crawler::addToListing(DirListing& out, const Root& dir, std::string_view name, std::wstring_view wideName,
    std::uint8_t flags, bool descend, std::uint32_t record) const
{
    flags |= dir.inheritedFlags;
    if (flags & EntryFlag::Directory) {
        out.scratchPath.assign(dir.path).append(1, L'\\').append(wideName);
        if (isExcludedDir(out.scratchPath, wideName))
            return;
        flags |= dirPriorityFlags(out.scratchPath, wideName);
    } else {
        descend = false;
    }
    const auto offset = static_cast<std::uint32_t>(out.names.size());
    if (name.empty())
        wtf8::append(out.names, wtf8::view(wideName));
    else
        out.names.append(name);
    const auto length = static_cast<std::uint16_t>(out.names.size() - offset);
    const auto wideOffset = static_cast<std::uint32_t>(out.wideNames.size());
    if (descend)
        out.wideNames.append(wideName);
    out.items.push_back({offset, length, flags, descend, wideOffset,
        static_cast<std::uint32_t>(out.wideNames.size() - wideOffset), record});
}

// Returns false when the directory could not be read (access denied, gone):
// the caller then leaves what the index already has untouched.
bool Crawler::listDirectory(const Root& dir, DirListing& out) const
{
    out.clear();
    std::wstring path = win32::longPath(dir.path);
    if (dir.path.size() == 2)
        path.push_back(L'\\'); // a volume root
    const win32::UniqueHandle h(::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!h.valid())
        return false;
    out.readBuffer.resize(kDirReadBytes / sizeof(std::uint64_t)); // 8-byte aligned, as the records need

    // The same directory read FindFirstFile does, plus each child's file id.
    FILE_INFO_BY_HANDLE_CLASS request = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!::GetFileInformationByHandleEx(h.get(), request, out.readBuffer.data(), kDirReadBytes))
            return ::GetLastError() == ERROR_NO_MORE_FILES;
        request = FileIdBothDirectoryInfo;
        const auto* p = reinterpret_cast<const std::byte*>(out.readBuffer.data());
        for (;;) {
            const auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(p);
            const std::wstring_view name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                // For reparse points, EaSize holds the reparse tag.
                addToListing(out, dir, {}, name, attributeFlags(info->FileAttributes),
                    shouldDescend(info->FileAttributes, info->EaSize), recordNumber(info->FileId));
            }
            if (info->NextEntryOffset == 0)
                break;
            p += info->NextEntryOffset;
        }
    }
}

namespace {

bool foldedLess(std::string_view a, std::string_view b) noexcept
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char x = text::fold(a[i]);
        const unsigned char y = text::fold(b[i]);
        if (x != y)
            return x < y;
    }
    return a.size() < b.size();
}

// The children a directory already has in the index, found by name ignoring
// ASCII case. One sorted array per walker thread, reused for every directory:
// a node-based map allocated per child and left megabytes of freed nodes in
// the heap after each walk. Names that differ only in case (case-sensitive
// folders) each keep their own entry.
class ChildMap {
public:
    void assign(const FileIndex& index, EntryId dir)
    {
        m_children.clear();
        for (EntryId c = index.entry(dir).firstChild; c != kNoEntry; c = index.entry(c).nextSibling)
            m_children.push_back({index.name(c), c});
        std::sort(m_children.begin(), m_children.end(), byName);
    }

    // Takes the child called `name`: the exact spelling if there is one,
    // otherwise one that differs only in case (renamed, case only).
    EntryId take(std::string_view name)
    {
        const auto [first, last] = std::equal_range(m_children.begin(), m_children.end(), Child {name, kNoEntry}, byName);
        auto pick = last;
        for (auto it = first; it != last; ++it) {
            if (it->id == kNoEntry)
                continue; // taken already
            if (it->name == name) {
                pick = it;
                break;
            }
            if (pick == last)
                pick = it;
        }
        return pick == last ? kNoEntry : std::exchange(pick->id, kNoEntry);
    }

    template <typename F> void forEachLeft(F&& f) const
    {
        for (const Child& c : m_children) {
            if (c.id != kNoEntry)
                f(c.id);
        }
    }

private:
    struct Child {
        std::string_view name;
        EntryId id;
    };
    static bool byName(const Child& a, const Child& b) noexcept { return foldedLess(a.name, b.name); }

    std::vector<Child> m_children;
};

void removeChildren(FileIndex& index, EntryId dir)
{
    std::vector<EntryId> children;
    for (EntryId c = index.entry(dir).firstChild; c != kNoEntry; c = index.entry(c).nextSibling)
        children.push_back(c);
    for (const EntryId c : children)
        index.remove(c);
}

// Applies one directory listing to the index. Caller holds the write lock.
void reconcile(FileIndex& index, const Crawler::Root& dir, const DirListing& listing, ChildMap& existing,
    std::vector<Crawler::Root>& subdirs)
{
    if (dir.id >= index.slotCount() || index.entry(dir.id).isDeleted())
        return;
    existing.assign(index, dir.id);

    for (const auto& item : listing.items) {
        const std::string_view name = listing.name(item);
        const bool isDir = item.flags & EntryFlag::Directory;
        EntryId id = existing.take(name);
        if (id != kNoEntry) {
            if (index.entry(id).isDir() != isDir) {
                index.remove(id); // a file became a folder or vice versa
                id = kNoEntry;
            } else {
                if (index.name(id) != name)
                    index.move(id, dir.id, name); // renamed, case only
                index.setFlags(id, item.flags);
                if (!item.descend && index.entry(id).firstChild != kNoEntry)
                    removeChildren(index, id); // turned into a junction
            }
        }
        if (id == kNoEntry)
            id = index.add(dir.id, name, item.flags);
        if (isDir && dir.volume != kNoEntry && item.record != 0)
            index.setFolderRecord(dir.volume, item.record, id);
        if (item.descend) {
            std::wstring path;
            path.reserve(dir.path.size() + 1 + item.wideLength);
            path.append(dir.path).append(1, L'\\').append(listing.wideName(item));
            subdirs.push_back({id, std::move(path), static_cast<std::uint8_t>(item.flags & EntryFlag::Inherited),
                dir.volume, item.record});
        }
    }
    existing.forEachLeft([&](EntryId id) { index.remove(id); }); // gone from disk
}

} // namespace

bool Crawler::sync(FileIndex& index, std::vector<Root> roots, int threads, bool lowPriority, std::stop_token stop,
    const Lister& list) const
{
    struct Shared {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::vector<Root> stack; // LIFO: depth-first keeps the pending set small
        int busy = 0;
    } shared;
    shared.stack = std::move(roots);

    auto worker = [&] {
        if (lowPriority)
            ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);

        DirListing listing;
        ChildMap existing;
        std::vector<Root> subdirs;
        for (;;) {
            Root task;
            {
                std::unique_lock lock(shared.mutex);
                shared.cv.wait(lock, stop, [&] { return !shared.stack.empty() || shared.busy == 0; });
                if (stop.stop_requested() || shared.stack.empty())
                    break;
                task = std::move(shared.stack.back());
                shared.stack.pop_back();
                ++shared.busy;
            }

            if (list ? list(task, listing) : listDirectory(task, listing)) {
                auto lock = index.writeLock();
                reconcile(index, task, listing, existing, subdirs);
            }

            {
                std::lock_guard lock(shared.mutex);
                for (auto& s : subdirs)
                    shared.stack.push_back(std::move(s));
                --shared.busy;
            }
            subdirs.clear();
            shared.cv.notify_all();
        }
        shared.cv.notify_all();

        if (lowPriority)
            ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    };

    {
        std::vector<std::jthread> pool;
        const int n = std::max(1, threads);
        pool.reserve(n);
        for (int i = 0; i < n; ++i)
            pool.emplace_back(worker);
    } // joins
    return !stop.stop_requested();
}

namespace {

// Whether an index name (UTF-8) is `wide`, ignoring case as NTFS does.
bool sameName(std::string_view utf8, const std::string& wideAsUtf8, std::wstring_view wide)
{
    if (text::equalsIgnoreAsciiCase(utf8, wideAsUtf8))
        return true;
    if (std::all_of(utf8.begin(), utf8.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }))
        return false; // ASCII only: the comparison above was complete
    std::u16string decoded;
    wtf8::decodeAppend(decoded, utf8);
    return win32::equalsIgnoreCase(wtf8::wview(decoded), wide);
}

} // namespace

std::size_t removeExcluded(FileIndex& index, const CrawlRules& rules)
{
    std::size_t removed = 0;
    for (const std::wstring& path : rules.excludedPaths) {
        const EntryId id = index.pathForUpdate(path);
        if (id != kNoEntry && index.entry(id).isDir() && !(index.entry(id).flags & EntryFlag::Root))
            removed += index.remove(id);
    }
    if (rules.excludedNames.empty())
        return removed;

    // Folders named like a rule's last segment; then the whole rule (a tail
    // such as ".svn\pristine") is checked on their path.
    const Crawler names(CrawlRules {{}, rules.excludedNames, {}, {}});
    struct Segment {
        std::wstring wide;
        std::string utf8;
    };
    std::vector<Segment> lastSegments;
    for (const std::wstring& rule : rules.excludedNames) {
        std::wstring last = rule.substr(rule.find_last_of(L'\\') + 1);
        std::string utf8 = wtf8::fromUtf16(wtf8::view(last));
        lastSegments.push_back({std::move(last), std::move(utf8)});
    }
    std::vector<EntryId> doomed;
    for (std::size_t c = 0; c < index.chunkCount(); ++c) {
        const auto entries = index.chunk(c);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            if (!e.isDir() || (e.flags & (EntryFlag::Deleted | EntryFlag::Root)))
                continue;
            const std::string_view name = index.name(e);
            if (std::none_of(lastSegments.begin(), lastSegments.end(),
                    [&](const Segment& s) { return sameName(name, s.utf8, s.wide); }))
                continue;
            const auto id = static_cast<EntryId>((c << FileIndex::kChunkBits) + i);
            const std::wstring path = index.wpath(id);
            if (names.isExcludedDir(path, std::wstring_view(path).substr(path.find_last_of(L'\\') + 1)))
                doomed.push_back(id);
        }
    }
    for (const EntryId id : doomed)
        removed += index.remove(id); // one inside another: removed with it already, so 0
    return removed;
}

} // namespace ws

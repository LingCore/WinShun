#include "IndexService.h"

#include "IndexFolder.h"
#include "Ntfs.h"
#include "NtfsIndexer.h"
#include "Snapshot.h"
#include "Unc.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QtGlobal>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

// A journal read completes on every record written, even ones it filters
// out: after each read, let records gather this long before the next one.
constexpr auto kJournalPause = 500ms;
constexpr std::size_t kMaxJournal = 200'000; // beyond this, just sync again
constexpr std::size_t kMinCompactSlots = 65'536; // removed items (~40 bytes each) before compacting
// A save writes the whole index (some 30 MB packed for 3 million names), and
// what changed after it is replayed from the change journals at the next
// start anyway. Longer than the 20 idle minutes after which Windows turns a
// hard disk off (by default), which saves more often would never let it be.
constexpr auto kSaveInterval = 30min;
// Sooner when merges left this much of the content index's old segment files,
// which only go once a snapshot no longer refers to them.
constexpr std::uint64_t kGarbageBytes = 32 << 20;
constexpr auto kSaveCheck = 1min;
constexpr auto kPrewarmAfterStart = 90s; // once logging on has settled
// GitHub #52: a chosen folder whose watcher lost events (or a folder above it
// came back) is walked again this much later, all such at once.
constexpr auto kFolderResync = 3s;
// ... when their watcher keeps losing events, 30 s, then 5 minutes, until
// 10 minutes go by without (LostEventsBackoff).
constexpr std::chrono::milliseconds kLostSteps[] {kFolderResync, 30s, 5min};
constexpr auto kLostCalm = 10min;
constexpr int kVerifyShare = 60; // a folder walked again no sooner than 60 times as long as its walk took
constexpr auto kVerifyWaitForAway = 2h; // ... and once the user is away, or this much later anyway

// A folder's NTFS record number, 0 when unknown.
std::uint32_t folderRecord(const std::wstring& path)
{
    const win32::UniqueHandle h(::CreateFileW(win32::longPath(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    BY_HANDLE_FILE_INFORMATION info {};
    if (!h.valid() || !::GetFileInformationByHandle(h.get(), &info))
        return 0;
    return ntfs::recordOf((std::uint64_t {info.nFileIndexHigh} << 32) | info.nFileIndexLow);
}

std::pair<std::wstring_view, std::wstring_view> splitPath(std::wstring_view path)
{
    const std::size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring_view::npos)
        return {path, {}};
    return {path.substr(0, slash), path.substr(slash + 1)};
}

std::string toUtf8(std::wstring_view s)
{
    return wtf8::fromUtf16(wtf8::view(s));
}

bool isUnder(std::wstring_view path, std::wstring_view dir)
{
    return path.size() > dir.size() && path[dir.size()] == L'\\'
        && win32::equalsIgnoreCase(path.substr(0, dir.size()), dir);
}

// "C:", as Settings::crawlRules() writes a drive's root.
bool isDriveRoot(std::wstring_view path)
{
    return path.size() == 2 && path[1] == L':';
}

int crawlThreads()
{
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 2, 8);
}

bool contains(const std::vector<std::wstring>& list, std::wstring_view s)
{
    return std::any_of(list.begin(), list.end(), [&](const std::wstring& x) { return win32::equalsIgnoreCase(x, s); });
}

// The items of `to` that `from` lacks.
std::vector<std::wstring> added(const std::vector<std::wstring>& from, const std::vector<std::wstring>& to)
{
    std::vector<std::wstring> out;
    for (const std::wstring& s : to) {
        if (!contains(from, s))
            out.push_back(s);
    }
    return out;
}

// The same disk in the same drive letter (a reformatted or swapped one is not).
bool sameVolume(const VolumeInfo& a, const VolumeInfo& b)
{
    return a.serial == b.serial && win32::equalsIgnoreCase(a.root, b.root);
}

// Removes the roots of volumes whose entries cannot be kept: drives that are
// gone, and drive letters that now hold another disk. Shares are the
// ShareIndexer's. Returns entries removed.
std::size_t dropStaleRoots(FileIndex& index, const std::vector<VolumeInfo>& volumes, const std::vector<bool>& known)
{
    std::size_t removed = 0;
    const std::vector<EntryId> roots = index.roots();
    for (const EntryId r : roots) {
        const std::string_view name = index.name(r);
        if (name.starts_with("\\\\"))
            continue;
        bool keep = false;
        for (std::size_t i = 0; i < volumes.size() && !keep; ++i)
            keep = known[i] && toUtf8(volumes[i].root) == name;
        if (!keep)
            removed += index.remove(r);
    }
    return removed;
}

// ---- GitHub #52: only the chosen folders ------------------------------------

// "C:" of "C:\a\b" (and of "C:"); empty for a share.
std::wstring_view volumeOf(std::wstring_view path)
{
    return path.size() >= 2 && path[1] == L':' ? path.substr(0, 2) : std::wstring_view {};
}

bool isRemovableDrive(std::wstring_view root)
{
    const std::wstring dir = std::wstring(root) + L'\\';
    return ::GetDriveTypeW(dir.c_str()) == DRIVE_REMOVABLE;
}

std::int64_t secondsNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Whether a chosen folder is there, as a folder. Unknown: it cannot be told
// (access denied), and what the index has of it stays.
enum class Presence { Folder, Missing, Unknown };
Presence presenceOf(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (::GetFileAttributesExW(win32::longPath(path).c_str(), GetFileExInfoStandard, &data))
        return data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? Presence::Folder : Presence::Missing;
    switch (::GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return Presence::Missing;
    default:
        return Presence::Unknown;
    }
}

// A folder as it is on disk, and its name as the disk spells it: a chosen
// folder may be written in another case, and its entries take the disk's.
struct OnDisk {
    PathProbe probe;
    std::wstring name;
};
OnDisk onDisk(const std::wstring& path)
{
    OnDisk d;
    WIN32_FIND_DATAW fd;
    const win32::UniqueFind find(
        ::FindFirstFileExW(win32::longPath(path).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0));
    if (find.valid()) {
        d.probe.attributes = fd.dwFileAttributes;
        d.probe.reparseTag = fd.dwReserved0;
        d.probe.modified = fileTimeOf(win32::ticks(fd.ftLastWriteTime));
        d.probe.created = fileTimeOf(win32::ticks(fd.ftCreationTime));
        d.name = fd.cFileName;
    }
    if (d.name.empty())
        d.name = splitPath(path).second;
    return d;
}

// The folders between a volume's root and `path`: "C:", "C:\a\b\c" -> "C:\a", "C:\a\b".
std::vector<std::wstring> foldersAbove(std::wstring_view root, std::wstring_view path)
{
    std::vector<std::wstring> out;
    for (std::size_t at = path.find(L'\\', root.size() + 1); at != std::wstring_view::npos; at = path.find(L'\\', at + 1))
        out.emplace_back(path.substr(0, at));
    return out;
}

bool containsId(const std::vector<EntryId>& ids, EntryId id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Removes what is below `root` but neither in a kept folder (`keep`) nor on
// the way to one (`through`, whose other children go). Caller holds the
// write lock. Returns the entries removed.
std::size_t pruneOutside(FileIndex& index, EntryId root, const std::vector<EntryId>& keep, const std::vector<EntryId>& through)
{
    std::size_t removed = 0;
    std::vector<EntryId> pending {root};
    std::vector<EntryId> children;
    while (!pending.empty()) {
        const EntryId dir = pending.back();
        pending.pop_back();
        children.clear();
        for (EntryId c = index.entry(dir).firstChild; c != kNoEntry; c = index.entry(c).nextSibling)
            children.push_back(c);
        for (const EntryId c : children) {
            if (containsId(keep, c))
                continue;
            if (containsId(through, c))
                pending.push_back(c);
            else
                removed += index.remove(c);
        }
    }
    return removed;
}

// The entries below `id`. Caller holds a lock.
std::size_t countBelow(const FileIndex& index, EntryId id)
{
    std::size_t count = 0;
    std::vector<EntryId> pending {id};
    while (!pending.empty()) {
        const EntryId dir = pending.back();
        pending.pop_back();
        for (EntryId c = index.entry(dir).firstChild; c != kNoEntry; c = index.entry(c).nextSibling) {
            ++count;
            if (index.entry(c).firstChild != kNoEntry)
                pending.push_back(c);
        }
    }
    return count;
}

// A chosen folder's entry, however its path is written (GitHub #52 review):
// findPath ignores ASCII case only, while the entries are named as the disk
// has them (ДОКИ for a chosen доки). Each step by its name first, then by
// the names compared as NTFS does. Caller holds a lock.
EntryId findFolder(const FileIndex& index, std::wstring_view path)
{
    if (const EntryId e = index.findPath(path); e != kNoEntry)
        return e;
    EntryId e = index.findPath(volumeOf(path));
    std::u16string decoded;
    for (std::size_t at = 2; e != kNoEntry && at < path.size();) {
        const std::size_t begin = at + 1;
        at = std::min(path.find(L'\\', begin), path.size());
        const std::wstring_view name = path.substr(begin, at - begin);
        if (name.empty())
            continue;
        EntryId next = index.findChild(e, toUtf8(name));
        for (EntryId c = index.entry(e).firstChild; next == kNoEntry && c != kNoEntry; c = index.entry(c).nextSibling) {
            decoded.clear();
            wtf8::decodeAppend(decoded, index.name(c));
            if (win32::equalsIgnoreCase(wtf8::wview(decoded), name))
                next = c;
        }
        e = next;
    }
    return e;
}

using FolderState = IndexService::FolderStatus::State;

const char* stateName(FolderState state)
{
    switch (state) {
    case FolderState::Waiting:
        return "waiting";
    case FolderState::Verifying:
        return "verifying";
    case FolderState::Ready:
        return "ready";
    case FolderState::NotFound:
        return "not_found";
    case FolderState::Unreadable:
        return "unreadable";
    case FolderState::Excluded:
        return "excluded";
    case FolderState::Offline:
        return "offline";
    case FolderState::WholeDrive:
        return "whole_drive";
    }
    return "waiting";
}

FolderState stateNamed(const QString& name)
{
    for (const FolderState s : {FolderState::Ready, FolderState::NotFound, FolderState::Unreadable,
             FolderState::Excluded, FolderState::Offline, FolderState::WholeDrive}) {
        if (name == QLatin1StringView(stateName(s)))
            return s;
    }
    return FolderState::Waiting; // "verifying" too: cut short
}

// scope.json, beside index.bin (indexfolder::scopePath): the volumes whose
// entries in that snapshot are their chosen folders' only, and how each
// folder stood at its last walk. Written after the snapshot, with its size and
// time: one an older Win顺 saved over (or none at all) is told by them.
struct ScopeFile {
    std::vector<std::wstring> scoped;
    std::vector<IndexService::FolderStatus> folders;
};

std::optional<std::pair<std::uint64_t, std::int64_t>> stampOf(const QString& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!::GetFileAttributesExW(win32::longPath(QDir::toNativeSeparators(path).toStdWString()).c_str(),
            GetFileExInfoStandard, &data))
        return std::nullopt;
    return std::pair {(std::uint64_t {data.nFileSizeHigh} << 32) | data.nFileSizeLow, win32::ticks(data.ftLastWriteTime)};
}

QString scopeFileOf(const QString& snapshotPath)
{
    return indexfolder::scopePath(QFileInfo(snapshotPath).absolutePath());
}

bool writeScopeFile(const QString& snapshotPath, const ScopeFile& file)
{
    const auto stamp = stampOf(snapshotPath);
    if (!stamp)
        return false;
    QJsonArray scoped;
    for (const std::wstring& root : file.scoped)
        scoped.append(QString::fromStdWString(root));
    QJsonArray folders;
    for (const IndexService::FolderStatus& f : file.folders) {
        folders.append(QJsonObject {{u"path"_s, QString::fromStdWString(f.path)},
            {u"state"_s, QString::fromLatin1(stateName(f.state))}, {u"items"_s, static_cast<qint64>(f.items)},
            {u"failed"_s, static_cast<qint64>(f.failed)}, {u"verified"_s, static_cast<qint64>(f.lastVerified)},
            {u"took_ms"_s, static_cast<qint64>(f.took.count())}});
    }
    // As text: a FILETIME is beyond what a JSON number holds exactly.
    const QJsonObject root {{u"version"_s, 1}, {u"index_bytes"_s, QString::number(stamp->first)},
        {u"index_written"_s, QString::number(stamp->second)}, {u"scoped"_s, scoped}, {u"folders"_s, folders}};
    const QByteArray json = QJsonDocument(root).toJson();
    QSaveFile out(scopeFileOf(snapshotPath));
    return out.open(QIODevice::WriteOnly) && out.write(json) == json.size() && out.commit();
}

// Nothing when there is none, or it was not written with this snapshot. Then
// `stale` tells which: there is one, but not this snapshot's (or not to be
// read), so the snapshot may hold volumes cut down to chosen folders it
// does not name.
std::optional<ScopeFile> readScopeFile(const QString& snapshotPath, bool& stale)
{
    stale = false;
    QFile in(scopeFileOf(snapshotPath));
    const auto stamp = stampOf(snapshotPath);
    if (!stamp || !in.exists())
        return std::nullopt;
    if (!in.open(QIODevice::ReadOnly)) {
        stale = true;
        return std::nullopt;
    }
    const QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    if (root.value(u"index_bytes"_s).toString() != QString::number(stamp->first)
        || root.value(u"index_written"_s).toString() != QString::number(stamp->second)) {
        stale = true;
        return std::nullopt;
    }
    ScopeFile file;
    for (const QJsonValue& v : root.value(u"scoped"_s).toArray())
        file.scoped.push_back(v.toString().toStdWString());
    for (const QJsonValue& v : root.value(u"folders"_s).toArray()) {
        const QJsonObject o = v.toObject();
        IndexService::FolderStatus f;
        f.path = o.value(u"path"_s).toString().toStdWString();
        f.state = stateNamed(o.value(u"state"_s).toString());
        f.items = static_cast<std::size_t>(std::max<qint64>(0, o.value(u"items"_s).toInteger()));
        f.failed = static_cast<std::size_t>(std::max<qint64>(0, o.value(u"failed"_s).toInteger()));
        f.lastVerified = o.value(u"verified"_s).toInteger();
        f.took = std::chrono::milliseconds(std::max<qint64>(0, o.value(u"took_ms"_s).toInteger()));
        file.folders.push_back(std::move(f));
    }
    return file;
}

} // namespace

VerifyNow folderVerifyDue(
    std::int64_t lastVerified, std::chrono::milliseconds took, std::chrono::hours every, std::int64_t now) noexcept
{
    if (every.count() <= 0)
        return VerifyNow::No;
    if (lastVerified <= 0)
        return VerifyNow::Yes;
    if (now + 3600 < lastVerified)
        return VerifyNow::IfAway; // the clock was put back: from now on
    const auto wait = std::max<std::chrono::seconds>(every, std::chrono::ceil<std::chrono::seconds>(took * kVerifyShare));
    const std::int64_t due = lastVerified + wait.count();
    if (now < due)
        return VerifyNow::No;
    return now >= due + std::chrono::seconds(kVerifyWaitForAway).count() ? VerifyNow::Yes : VerifyNow::IfAway;
}

std::vector<std::wstring> foldersDue(const std::vector<IndexService::FolderStatus>& statuses, std::chrono::hours every,
    std::int64_t now, const std::function<bool()>& away)
{
    using State = IndexService::FolderStatus::State;
    std::vector<std::wstring> due;
    std::optional<bool> isAway;
    for (const IndexService::FolderStatus& s : statuses) {
        if (s.state != State::Ready && s.state != State::NotFound && s.state != State::Unreadable)
            continue;
        const VerifyNow verify = folderVerifyDue(s.lastVerified, s.took, every, now);
        if (verify == VerifyNow::No)
            continue;
        if (verify == VerifyNow::IfAway) {
            if (!isAway)
                isAway = away && away();
            if (!*isAway)
                continue;
        }
        due.push_back(s.path);
    }
    return due;
}

std::chrono::milliseconds LostEventsBackoff::next(std::chrono::steady_clock::time_point now) noexcept
{
    if (!m_last || now - *m_last >= kLostCalm)
        m_streak = 0;
    m_last = now;
    const std::size_t step = std::min(static_cast<std::size_t>(m_streak), std::size(kLostSteps) - 1);
    m_streak = static_cast<int>(step) + 1;
    return kLostSteps[step];
}

ScopedChanges changesInScope(std::vector<FsChange> changes,
    const std::function<const std::vector<std::wstring>&(std::wstring_view)>& chosenOn,
    const std::function<bool(const std::wstring&)>& stillThere)
{
    using Kind = FsChange::Kind;
    ScopedChanges out;
    const auto owe = [&](const std::wstring& f) {
        if (!contains(out.owed, f))
            out.owed.push_back(f);
    };
    // Where a change is: in a chosen folder (or on a volume indexed whole),
    // at or above one (where the index has the way to it only), or neither.
    enum class Place { Inside, Above, Outside };
    const auto placeOf = [&](const FsChange& c) {
        const std::vector<std::wstring>& chosen = chosenOn(volumeOf(c.path));
        if (chosen.empty())
            return Place::Inside; // a volume indexed whole
        bool above = false;
        for (const std::wstring& f : chosen) {
            if (isUnder(c.path, f))
                return Place::Inside;
            above = above || win32::equalsIgnoreCase(c.path, f) || isUnder(f, c.path);
        }
        return above ? Place::Above : Place::Outside;
    };
    std::vector<Place> places(changes.size(), Place::Outside);
    std::vector<bool> drop(changes.size(), true);
    std::vector<bool> owes(changes.size(), true); // the chosen folders at or below it are walked again
    std::vector<bool> paired(changes.size(), false); // half of a rename in this batch
    for (std::size_t k = 0; k < changes.size(); ++k) {
        if (changes[k].kind == Kind::Overflow)
            continue;
        places[k] = placeOf(changes[k]);
        // Above a chosen folder, only what takes it away (its entries go
        // now): whether it is gone or back, its walk tells.
        drop[k] = places[k] == Place::Outside
            || (places[k] == Place::Above && changes[k].kind != Kind::Removed && changes[k].kind != Kind::RenamedFrom);
    }
    for (std::size_t k = 0; k + 1 < changes.size(); ++k) {
        if (changes[k].kind != Kind::RenamedFrom || changes[k + 1].kind != Kind::RenamedTo)
            continue;
        paired[k] = paired[k + 1] = true;
        const Place from = places[k];
        const Place to = places[k + 1];
        if (from == Place::Above && to == Place::Above) {
            // A folder above chosen ones renamed, or moved to above others:
            // its entries go along (GitHub #52 review: not removed and walked
            // again). Only as a move: should that fail, nothing comes in its
            // place (moveOnly), the walks below set it right. A new name that
            // differs in case alone changes nothing below: none walked.
            drop[k] = drop[k + 1] = false;
            changes[k + 1].moveOnly = true;
            if (win32::equalsIgnoreCase(changes[k].path, changes[k + 1].path))
                owes[k] = owes[k + 1] = false;
        } else if (from == Place::Above && to == Place::Inside) {
            // Into a chosen folder: what the index has of it is the way to
            // others, not all that is in it. Gone from there, walked in whole here.
            drop[k] = drop[k + 1] = false;
            changes[k].kind = Kind::Removed;
            changes[k + 1].kind = Kind::Added;
        } else if (drop[k] != drop[k + 1]) {
            // Half a rename leaving (or entering) one: by itself.
            if (!drop[k])
                changes[k].kind = Kind::Removed;
            else
                changes[k + 1].kind = Kind::Added;
        }
        ++k;
    }
    // Above chosen folders, said to be gone and there all the same: Windows
    // says a folder is removed when its name changes in case only (up to UP,
    // its 8.3 name goes), a batch before the rename itself. It stays, with
    // the way to them. Had it gone and another come in its place, that one's
    // Added says so (and has them walked).
    for (std::size_t k = 0; stillThere && k < changes.size(); ++k) {
        if (places[k] == Place::Above && !paired[k] && !drop[k] && stillThere(changes[k].path)) {
            drop[k] = true;
            owes[k] = false;
        }
    }
    for (std::size_t k = 0; k < changes.size(); ++k) {
        const FsChange& c = changes[k];
        const std::vector<std::wstring>& chosen = chosenOn(volumeOf(c.path));
        if (c.kind == Kind::Overflow) {
            if (chosen.empty())
                out.wholeLost = true;
            for (const std::wstring& f : chosen) {
                if (c.path.size() == 2 || win32::equalsIgnoreCase(c.path, f)) {
                    owe(f); // the volume's root, or the folder watched itself
                    out.lost = true;
                }
            }
        } else if (places[k] == Place::Above && owes[k]) {
            for (const std::wstring& f : chosen) {
                if (win32::equalsIgnoreCase(c.path, f) || isUnder(f, c.path))
                    owe(f);
            }
        }
    }
    for (std::size_t k = 0; k < changes.size(); ++k) {
        if (!drop[k])
            out.apply.push_back(std::move(changes[k]));
    }
    return out;
}

IndexService::Scope::Scope(std::optional<std::vector<std::wstring>> folders, const std::vector<std::wstring>& extra)
    : m_set(folders.has_value())
{
    for (std::wstring f : folders ? *folders : extra) {
        while (f.size() > 2 && f.back() == L'\\')
            f.pop_back();
        if (volumeOf(f).empty() || (f.size() > 2 && f[2] != L'\\'))
            continue; // a share (the ShareIndexer's), or no full path
        if (!m_set && !hasDirectoryLink(f))
            continue; // ordinary folders are already covered by whole-disk indexing
        if (f[0] >= L'a' && f[0] <= L'z')
            f[0] = static_cast<wchar_t>(f[0] - L'a' + L'A'); // as the roots are named
        const std::wstring root(volumeOf(f));
        auto v = std::find_if(m_volumes.begin(), m_volumes.end(), [&](const Volume& v) { return v.root == root; });
        if (v == m_volumes.end())
            v = m_volumes.insert(m_volumes.end(), Volume {root});
        if (f.size() == 2) {
            v->whole = true;
        } else {
            // None inside another: the outer one stands for both.
            if (std::any_of(v->folders.begin(), v->folders.end(),
                    [&](const std::wstring& o) { return win32::equalsIgnoreCase(o, f) || (isUnder(f, o) && !hasDirectoryLink(f, o)); }))
                continue;
            std::erase_if(v->folders, [&](const std::wstring& o) { return isUnder(o, f) && !hasDirectoryLink(o, f); });
            v->folders.push_back(f);
        }
        if (!contains(m_all, f))
            m_all.push_back(f);
    }
    std::erase_if(m_all, [&](const std::wstring& f) {
        const std::vector<std::wstring>& kept = on(volumeOf(f));
        return f.size() > 2 && !contains(kept, f);
    });
}

const std::vector<std::wstring>& IndexService::Scope::on(std::wstring_view root) const
{
    static const std::vector<std::wstring> none;
    for (const Volume& v : m_volumes) {
        if (win32::equalsIgnoreCase(v.root, root))
            return v.whole ? none : v.folders;
    }
    return none;
}

bool IndexService::Scope::whole(std::wstring_view root) const
{
    return std::any_of(m_volumes.begin(), m_volumes.end(),
        [&](const Volume& v) { return v.whole && win32::equalsIgnoreCase(v.root, root); });
}

bool IndexService::Scope::touches(std::wstring_view root) const
{
    return std::any_of(
        m_volumes.begin(), m_volumes.end(), [&](const Volume& v) { return win32::equalsIgnoreCase(v.root, root); });
}

bool IndexService::Scope::onRemovable() const
{
    return std::any_of(m_volumes.begin(), m_volumes.end(), [](const Volume& v) { return isRemovableDrive(v.root); });
}

struct IndexService::Followed {
    std::size_t volume = 0;
    std::int64_t usn = 0; // the next record to read
    std::shared_ptr<FileIndex> index;
    std::shared_ptr<const Crawler> crawler; // the applier's rules
    std::unique_ptr<ntfs::JournalSource> reader;
    std::unique_ptr<UsnApplier> applier;
    std::vector<ntfs::UsnRecord> records; // of the last read
    bool reading = false;
    std::chrono::steady_clock::time_point resumeAt {}; // the next read starts then
};

IndexService::IndexService(QString folder, Options options, QObject* parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_folder(QDir::cleanPath(QDir::fromNativeSeparators(folder)))
{
    m_access.store(m_options.volumeAccess ? m_options.volumeAccess : std::make_shared<ntfs::DirectVolumeAccess>());
    m_crawler.store(std::make_shared<const Crawler>(m_options.rules));
    m_scope.store(std::make_shared<const Scope>(m_options.folders, m_options.extraFolders));
    resetFolderStatuses();
    m_index.store(std::make_shared<FileIndex>());
    m_content = std::make_shared<ContentIndex>(indexfolder::contentPath(m_folder));
    m_contentShares.store(contentSharesOf(m_options.shares));
    ContentIndexer::Source source;
    source.index = [this] { return m_index.load(); };
    source.volumes = [this] { return contentVolumes(); };
    source.access = [this] { return m_access.load(); }; // the bulk reads of a first build
    // Ready once the volumes are known too: a snapshot makes the index Ready
    // before that, and a content pass then found no volume to read and slept
    // for 15 minutes (after an upgrade that rebuilds the content index).
    source.ready = [this] { return state() == State::Ready && !m_refreshing.load() && m_volumesKnown.load(); };
    m_contentIndexer = std::make_unique<ContentIndexer>(m_content, std::move(source), m_options.content);

    // Shares go in once the local index is there: loaded, or read the first time.
    ShareIndexer::Source shareSource;
    shareSource.index = [this] { return m_index.load(); };
    shareSource.ready = [this] { return state() == State::Ready && m_volumesKnown.load(); };
    ShareIndexer::Callbacks shareCallbacks;
    shareCallbacks.changed = [this] {
        if (m_shareContent)
            m_shareContent->statusesChanged(); // a share Online now, or no longer: it reads, or stops
        QMetaObject::invokeMethod(this, [this] { emit sharesChanged(); }, Qt::QueuedConnection);
    };
    shareCallbacks.dirty = [this] { m_dirty = true; };
    shareCallbacks.filesChanged = [this](const std::wstring& path) {
        if (m_shareContent)
            m_shareContent->wake(path); // names or sizes came in: that share's files may want reading
    };
    shareCallbacks.reading = [this](const std::wstring& path, bool reading) {
        setReading(path, reading);
        notifyLater();
    };
    m_shares = std::make_unique<ShareIndexer>(std::move(shareSource), m_options.shares, std::move(shareCallbacks));

    // Their contents, where they are indexed (GitHub #114): read while the
    // share is Online, on threads of their own.
    remotecontent::Indexer::Source contentSource;
    contentSource.index = [this] { return m_index.load(); };
    contentSource.indexer = m_contentIndexer.get();
    contentSource.online = [this](const std::wstring& path) {
        const std::vector<ShareIndexer::Status> statuses = m_shares->statuses();
        return std::ranges::any_of(statuses, [&](const ShareIndexer::Status& s) {
            return s.state == ShareIndexer::State::Online && unc::equal(s.path, path);
        });
    };
    remotecontent::Indexer::Callbacks contentCallbacks;
    contentCallbacks.changed = [this] {
        QMetaObject::invokeMethod(this, [this] { emit sharesChanged(); }, Qt::QueuedConnection);
    };
    contentCallbacks.dirty = [this] { m_dirty = true; };
    m_shareContent = std::make_unique<remotecontent::Indexer>(std::move(contentSource), std::move(contentCallbacks));
    m_shareContent->setShares(*m_contentShares.load());

    m_saveTimer.setInterval(kSaveCheck);
    connect(&m_saveTimer, &QTimer::timeout, this, &IndexService::saveInBackground);

    m_resyncTimer.setSingleShot(true);
    m_resyncTimer.setInterval(3s);
    connect(&m_resyncTimer, &QTimer::timeout, this, [this] {
        if (!m_stopped)
            launch(Pass::Walked); // a watcher lost events
    });

    // Drives that come, go or come back: a moment later, so the new one is
    // mounted (or the ejection done) and several changes are handled at once.
    m_volumesTimer.setSingleShot(true);
    m_volumesTimer.setInterval(1500ms);
    connect(&m_volumesTimer, &QTimer::timeout, this, [this] {
        if (!m_stopped)
            launch(Pass::Volumes);
    });

    // GitHub #52: whether chosen folders are due to be walked again.
    m_verifyTimer.setInterval(m_options.folderVerifyCheckMs);
    connect(&m_verifyTimer, &QTimer::timeout, this, &IndexService::verifyDueFolders);

    // GitHub #52: chosen folders whose watcher lost events, walked again all
    // at once (resyncFoldersSoon says when).
    m_folderResyncTimer.setSingleShot(true);
    connect(&m_folderResyncTimer, &QTimer::timeout, this, [this] {
        {
            std::lock_guard lock(m_journalMutex);
            for (std::wstring& f : m_overflowed) {
                if (!contains(m_verifyOwed, f))
                    m_verifyOwed.push_back(std::move(f));
            }
            m_overflowed.clear();
        }
        walkOwedFolders();
    });
}

IndexService::~IndexService()
{
    shutdown();
}

std::size_t IndexService::itemCount() const
{
    const auto index = m_index.load();
    return index ? index->liveCount() : 0;
}

void IndexService::start()
{
    // Set once: a pass cut short and planned again (a drive plugged in, the
    // settings edited) waits only for what is left of the time.
    m_accessDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_options.accessWaitMs);
    launch(Pass::Startup);
    m_shares->start();
    m_shareContent->start();
    m_saveTimer.start();
    m_verifyTimer.start();
    m_prewarmer = std::jthread([this](std::stop_token stop) { prewarmLoop(stop); });
    prewarmSoon(kPrewarmAfterStart);
}

void IndexService::rebuild()
{
    if (m_stopped)
        return;
    launch(Pass::Full);
    m_shares->verifyAll();
}

void IndexService::setOptions(Options options)
{
    if (m_stopped)
        return;
    if (!(options.content == m_options.content)) {
        m_options.content = options.content;
        m_contentIndexer->setOptions(std::move(options.content));
        m_shareContent->wake(); // turned on, other types or limits: shares' files to read
    }
    if (!(options.shares == m_options.shares)) {
        m_options.shares = options.shares;
        auto content = contentSharesOf(m_options.shares);
        const bool contentChanged = *content != *m_contentShares.load();
        m_contentShares.store(std::move(content));
        m_shares->setOptions(std::move(options.shares)); // the local disks carry on
        m_shareContent->setShares(*m_contentShares.load());
        if (contentChanged) // a share's contents turned on or off: read, or let go at once (not 2 minutes later)
            m_contentIndexer->volumesChanged();
    }
    m_options.folderVerifyEvery = options.folderVerifyEvery; // the next look at what is due goes by it
    const bool rulesChanged = !(options.rules == m_options.rules);
    const bool drivesChanged = options.includeRemovable != m_options.includeRemovable
        || options.selectedDrives != m_options.selectedDrives;
    const bool foldersChanged = options.folders != m_options.folders || options.extraFolders != m_options.extraFolders;
    if (!rulesChanged && !drivesChanged && !foldersChanged)
        return;
    // Stop the worker, which reads the options, before replacing them. The
    // watcher thread keeps the old rules until its current batch is done.
    stopWorker();
    if (rulesChanged) {
        std::lock_guard lock(m_journalMutex);
        if (!m_rulesBefore)
            m_rulesBefore = m_options.rules; // what the index reflects; the next run applies the difference
    }
    m_options.rules = std::move(options.rules);
    m_options.includeRemovable = options.includeRemovable;
    m_options.selectedDrives = std::move(options.selectedDrives);
    m_crawler.store(std::make_shared<const Crawler>(m_options.rules));
    if (foldersChanged) {
        m_options.folders = std::move(options.folders);
        m_options.extraFolders = std::move(options.extraFolders);
        replaceScope();
    }
    launch(Pass::Volumes);
}

void IndexService::setScope(std::optional<std::vector<std::wstring>> folders)
{
    if (m_stopped || folders == m_options.folders)
        return;
    stopWorker(); // it reads the scope; the watcher thread keeps the old one until its current batch is done
    m_options.folders = std::move(folders);
    replaceScope();
    launch(Pass::Volumes);
}

void IndexService::replaceScope()
{
    const auto before = scope();
    auto next = std::make_shared<const Scope>(m_options.folders, m_options.extraFolders);
    if (before->supplemental()) {
        std::lock_guard lock(m_journalMutex);
        for (const auto& path : before->all()) {
            if (!contains(next->all(), path) && !contains(m_removedExtra, path))
                m_removedExtra.push_back(path);
        }
    }
    m_scope.store(std::move(next));
    resetFolderStatuses();
}

bool IndexService::foldersOnly(const Scope& scope) const
{
    return scope.set() && m_access.load()->kind() == ntfs::VolumeAccess::Kind::None;
}

bool IndexService::foldersOnly() const
{
    return foldersOnly(*scope());
}

std::vector<IndexService::FolderStatus> IndexService::folderStatuses() const
{
    std::lock_guard lock(m_foldersMutex);
    return m_folderStatus;
}

void IndexService::verifyFolder(const std::wstring& path)
{
    const auto scope = this->scope();
    if (m_stopped || !(foldersOnly(*scope) || scope->supplemental()) || !contains(scope->on(volumeOf(path)), path))
        return;
    {
        // Not on a disk that is not here, or is being ejected: it is walked when that is back.
        std::lock_guard lock(m_journalMutex);
        if (!diskHere(path))
            return;
        if (!contains(m_verifyOwed, path))
            m_verifyOwed.push_back(path);
    }
    setFolderStates({{path, FolderStatus::State::Verifying}});
    walkOwedFolders();
}

bool IndexService::diskHere(std::wstring_view path) const
{
    const std::wstring_view root = volumeOf(path);
    return !contains(m_suspended, root) && std::any_of(m_volumes.begin(), m_volumes.end(), [&](const VolumeInfo& v) {
        return win32::equalsIgnoreCase(v.root, root);
    });
}

void IndexService::verifyDueFolders()
{
    // Not while a pass is under way (it may be walking them), nor before the index is built.
    const auto scope = this->scope();
    if (m_stopped || m_running.load() || state() != State::Ready || !(foldersOnly(*scope) || scope->supplemental()))
        return;
    const std::vector<std::wstring> due = foldersDue(
        folderStatuses(), m_options.folderVerifyEvery, secondsNow(), [] { return win32::userAway(); });
    bool owed = false;
    {
        std::lock_guard lock(m_journalMutex);
        for (const std::wstring& f : due) {
            if (!diskHere(f))
                continue;
            owed = true;
            if (!contains(m_verifyOwed, f))
                m_verifyOwed.push_back(f);
        }
    }
    if (owed)
        walkOwedFolders(); // all in one pass, at low priority (not a first build)
}

void IndexService::walkOwedFolders()
{
    // A pass under way takes them up once its walks are done (walkFolders),
    // or right after it (ranOut): never cut short for them.
    if (!m_stopped && !m_running.load())
        launch(Pass::Volumes);
}

void IndexService::resyncFoldersSoon(bool lost)
{
    // One walk takes all those owed by then: a timer on its way stays as it is.
    if (m_stopped || m_folderResyncTimer.isActive())
        return;
    m_folderResyncTimer.start(
        lost ? m_lostBackoff.next(std::chrono::steady_clock::now()) : std::chrono::milliseconds(kFolderResync));
}

void IndexService::resetFolderStatuses()
{
    const auto scope = this->scope();
    {
        std::lock_guard lock(m_foldersMutex);
        std::vector<FolderStatus> statuses;
        for (const std::wstring& path : scope->all()) {
            const auto it = std::find_if(m_folderStatus.begin(), m_folderStatus.end(),
                [&](const FolderStatus& s) { return win32::equalsIgnoreCase(s.path, path); });
            FolderStatus s = it != m_folderStatus.end() ? *it : FolderStatus {path};
            s.path = path;
            if (path.size() == 2)
                s.state = FolderStatus::State::WholeDrive;
            statuses.push_back(std::move(s));
        }
        if (statuses == m_folderStatus)
            return;
        m_folderStatus = std::move(statuses);
    }
    noteFoldersChanged();
}

void IndexService::setFolderStates(
    const std::vector<std::pair<std::wstring, FolderStatus::State>>& states, std::optional<std::int64_t> checked)
{
    bool changed = false;
    {
        std::lock_guard lock(m_foldersMutex);
        for (const auto& [path, state] : states) {
            for (FolderStatus& s : m_folderStatus) {
                if (!win32::equalsIgnoreCase(s.path, path))
                    continue;
                changed = changed || s.state != state || (checked && s.lastVerified != *checked);
                s.state = state;
                if (checked)
                    s.lastVerified = *checked;
            }
        }
    }
    if (changed) {
        m_dirty = true; // saved with the next snapshot (scope.json)
        noteFoldersChanged();
    }
}

void IndexService::noteFoldersChanged()
{
    QMetaObject::invokeMethod(this, [this] { emit foldersChanged(); }, Qt::QueuedConnection);
}

void IndexService::setVolumeAccess(std::shared_ptr<ntfs::VolumeAccess> access)
{
    if (m_stopped || !access)
        return;
    {
        // Under the mutex, or the worker could look (nothing yet) and then
        // block just as this is stored and told.
        const std::lock_guard lock(m_accessMutex);
        m_access.store(std::move(access));
    }
    m_accessCv.notify_all();
    // A worker waiting in awaitAccess() goes on with it, its pass intact. Any
    // other is in a pass planned without it: planned again. (The worker
    // clears the flag before it loads the access, and the store came before
    // this look: whichever way they interleave, the new access is used, or
    // the pass is relaunched.)
    if (!m_accessAwaited.load())
        launch(Pass::Volumes);
}

void IndexService::suspendVolume(const std::wstring& root)
{
    if (m_stopped)
        return;
    {
        std::lock_guard lock(m_journalMutex);
        if (contains(m_suspended, root))
            return;
        m_suspended.push_back(root);
    }
    // The worker may hold the volume's MFT or change journal open, the
    // watcher its root folder, the content indexer a file: all let go
    // before this returns. What the index service opened for us, too.
    stopWorker();
    ensureWatcher();
    m_contentIndexer->interrupt();
    m_access.load()->closeVolume(root);
    m_volumesTimer.start(); // the other volumes carry on in a moment
}

void IndexService::resumeVolume(const std::wstring& root)
{
    if (m_stopped)
        return;
    {
        std::lock_guard lock(m_journalMutex);
        const auto it = std::find_if(m_suspended.begin(), m_suspended.end(),
            [&](const std::wstring& s) { return win32::equalsIgnoreCase(s, root); });
        if (it == m_suspended.end())
            return;
        m_suspended.erase(it);
        // A walked volume went unwatched meanwhile: walk it again. NTFS ones
        // catch up through their journal.
        for (std::size_t i = 0; i < m_volumes.size(); ++i) {
            if (win32::equalsIgnoreCase(m_volumes[i].root, root) && m_journals[i].journalId == 0)
                m_rewalk.push_back(root);
        }
    }
    m_volumesTimer.start();
}

void IndexService::refreshVolumes()
{
    if (!m_stopped)
        m_volumesTimer.start();
}

std::vector<IndexService::Reading> IndexService::readingVolumes() const
{
    std::lock_guard lock(m_readingMutex);
    return m_reading;
}

void IndexService::setReading(const std::wstring& root, bool reading)
{
    std::lock_guard lock(m_readingMutex);
    std::erase_if(m_reading, [&](const Reading& r) { return r.root == root; });
    if (reading) {
        m_reading.push_back({root});
        std::sort(m_reading.begin(), m_reading.end(), [](const Reading& a, const Reading& b) { return a.root < b.root; });
    }
}

void IndexService::setReadingPercent(const std::wstring& root, int percent)
{
    std::lock_guard lock(m_readingMutex);
    for (Reading& r : m_reading) {
        if (r.root == root)
            r.percent = percent;
    }
}

std::vector<std::wstring> IndexService::volumeRoots() const
{
    std::lock_guard lock(m_journalMutex);
    std::vector<std::wstring> roots;
    for (const VolumeInfo& v : m_volumes)
        roots.push_back(v.root);
    return roots;
}

void IndexService::shutdown()
{
    if (m_stopped)
        return;
    m_stopped = true;
    m_saveTimer.stop();
    m_resyncTimer.stop();
    m_volumesTimer.stop();
    m_folderResyncTimer.stop();
    m_verifyTimer.stop();
    m_shutdown.request_stop();
    if (m_mover.joinable()) {
        m_mover.request_stop(); // a move cut short leaves the files where they were
        m_mover.join();
    }
    if (m_prewarmer.joinable()) {
        m_prewarmer.request_stop();
        m_prewarmer.join();
    }
    m_contentIndexer->stop();
    // Network shares stop where they are but are waited for only after the
    // save: a pass may be waiting on a server that went away, and at the end
    // of the session Windows gives us only seconds. Saving with them still
    // running is what the periodic save does anyway. Their contents' threads
    // wait for no server (a file being read is let go of: remotecontent::readFile).
    m_shares->requestStop();
    m_shareContent->requestStop(); // the indexer takes nothing more from them (stop() above)
    stopWorker();
    m_watcher.reset();
    if (m_pendingSave.valid())
        m_pendingSave.wait();
    if (m_content->takeChanged())
        m_dirty = true;
    if (m_dirty)
        saveSnapshot();
    m_shareContent->stop();
    m_shares->stop();
}

void IndexService::stopWorker()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}

void IndexService::launch(Pass pass)
{
    stopWorker();
    m_running = true;
    m_worker = std::jthread([this, pass](std::stop_token stop) { run(stop, pass); });
}

bool passOver(std::atomic<bool>& running, const std::function<bool()>& owed)
{
    running = false;
    return owed();
}

void IndexService::ranOut()
{
    const bool owed = passOver(m_running, [this] {
        std::lock_guard lock(m_journalMutex);
        return !m_verifyOwed.empty();
    });
    if (owed) // asked for after this pass's walks were done
        QMetaObject::invokeMethod(this, [this] { walkOwedFolders(); }, Qt::QueuedConnection);
}

void IndexService::run(std::stop_token stop, Pass pass)
{
    const auto ran = qScopeGuard([this] { ranOut(); });
    const auto crawler = this->crawler();
    const auto scope = this->scope();
    m_placed.clear();
    std::vector<VolumeInfo> previous;
    std::vector<JournalPosition> previousJournals;
    std::vector<std::wstring> suspended;
    std::vector<std::wstring> rewalk;
    {
        std::lock_guard lock(m_journalMutex);
        previous = m_volumes;
        previousJournals = m_journals;
        suspended = m_suspended;
        rewalk = m_rewalk;
    }

    // The volumes here now. Suspended ones are not looked at: they keep their
    // place and entries while their drive letter is there, and are forgotten
    // once it is gone. Removable ones with a chosen folder too: whether only
    // the chosen folders are indexed is told once the index service had its
    // chance (below), and those not wanted then go.
    const bool listedForScope = !m_options.includeRemovable && !m_options.selectedDrives
        && scope->set() && scope->onRemovable();
    std::vector<VolumeInfo> volumes = listLocalVolumes(m_options.includeRemovable || listedForScope, suspended, m_options.selectedDrives);
    std::vector<std::wstring> gone;
    for (const std::wstring& root : suspended) {
        const auto it = std::find_if(previous.begin(), previous.end(),
            [&](const VolumeInfo& v) { return win32::equalsIgnoreCase(v.root, root); });
        if (it != previous.end() && driveLetterExists(root) && volumeSelected(root, m_options.selectedDrives))
            volumes.push_back(*it);
        else
            gone.push_back(root);
    }
    // A whole drive excluded ("X:\" in the list, GitHub #17: a cloud drive
    // mounted as a local disk) is not a volume of ours: its root and entries
    // go below like those of a drive taken out, and come back when it is
    // excluded no more.
    std::erase_if(volumes, [&](const VolumeInfo& v) { return crawler->isExcludedDir(v.root, v.root); });
    std::sort(volumes.begin(), volumes.end(), [](const VolumeInfo& a, const VolumeInfo& b) { return a.root < b.root; });
    const auto isSuspended = [&](const VolumeInfo& v) { return contains(suspended, v.root) && !contains(gone, v.root); };

    // Where each volume's entries stand: in the index already (`known`), and
    // how far its change journal has been applied.
    std::vector<JournalPosition> journals(volumes.size());
    std::vector<bool> known(volumes.size(), false);
    std::optional<CrawlRules> snapshotRules;
    bool timesMissing = false; // a snapshot from before the index kept write times: every volume is read again
    bool fromSnapshot = false;
    bool scopeKnown = false; // ... with its scope.json (GitHub #52)
    bool scopeStale = false; // ... with one that does not go with it
    if (pass == Pass::Startup) {
        setState(State::Loading);
        const std::lock_guard files(m_filesMutex); // a move waits until they are read
        if (auto contents = snapshot::load(indexfolder::snapshotPath(m_folder))) {
            for (std::size_t i = 0; i < volumes.size(); ++i) {
                for (std::size_t j = 0; j < contents->volumes.size(); ++j) {
                    if (sameVolume(volumes[i], contents->volumes[j])) {
                        journals[i] = contents->journals[j];
                        known[i] = true;
                    }
                }
            }
            // Drives that are gone (or hold another disk now) leave the snapshot.
            FileIndex& loaded = *contents->index;
            m_content->restore(contents->attachment, loaded.slotCount());
            contents->attachment = decltype(contents->attachment)();
            if (!m_contentIndexer->enabled())
                m_content->clear(); // turned off: its files go once the snapshot is saved without it
            const std::size_t dropped = dropStaleRoots(loaded, volumes, known)
                + ShareIndexer::dropUnknownRoots(loaded, m_options.shares.shares);
            if (dropped > 0)
                loaded.compact(false, [&](const FileIndex::Renumber& renumber) { m_content->remap(renumber); });
            m_index.store(std::shared_ptr<FileIndex>(std::move(contents->index)));
            snapshotRules = std::move(contents->rules);
            timesMissing = !contents->times || !contents->created;
            fromSnapshot = true;
            // GitHub #52: what of it the chosen folders are, and how each stood.
            // Not known (none, or written with another snapshot): all of them
            // are walked now, and what is not theirs goes (prepareScope); with
            // whole disks, the disks walked are walked again (see below).
            std::optional<ScopeFile> scopeFile = readScopeFile(indexfolder::snapshotPath(m_folder), scopeStale);
            scopeKnown = scopeFile.has_value();
            if (scopeFile) {
                std::lock_guard lock(m_foldersMutex);
                for (FolderStatus& s : m_folderStatus) {
                    const auto it = std::find_if(scopeFile->folders.begin(), scopeFile->folders.end(),
                        [&](const FolderStatus& f) { return win32::equalsIgnoreCase(f.path, s.path); });
                    if (it == scopeFile->folders.end() || s.state == FolderStatus::State::WholeDrive)
                        continue;
                    const std::wstring path = s.path;
                    s = *it;
                    s.path = path;
                }
            }
            noteFoldersChanged();
            setState(State::Ready);
            // The volumes and their journal positions are known from here
            // on. A pass launched before this one is through (a drive
            // plugged in, the settings edited, the index service
            // connecting; see awaitAccess) plans from them: planned from
            // nothing, it would take every root for a drive that is gone.
            // (timesMissing is not kept: such a snapshot is from before
            // 0.5, and a pass cut short reads the times the next time the
            // MFT is read.) All the disks here, those without a chosen
            // folder too: whether they keep their entries is told after
            // awaitAccess (GitHub #52 review).
            std::lock_guard lock(m_journalMutex);
            m_volumes = volumes;
            m_journals = journals;
            if (snapshotRules && !(*snapshotRules == crawler->rules()) && !m_rulesBefore)
                m_rulesBefore = *snapshotRules; // the rules were edited while we were not running
            m_scopedVolumes = scopeFile ? std::move(scopeFile->scoped) : std::vector<std::wstring> {};
        } else {
            m_content->restore({}, 0); // clears out segment files nothing refers to
            setState(State::Building);
        }
    } else {
        for (std::size_t i = 0; i < volumes.size(); ++i) {
            for (std::size_t j = 0; j < previous.size() && j < previousJournals.size(); ++j) {
                if (sameVolume(volumes[i], previous[j])) {
                    journals[i] = previousJournals[j];
                    known[i] = true;
                }
            }
        }
        const auto index = m_index.load();
        std::lock_guard journalLock(m_journalMutex); // keeps compaction out
        auto lock = index->writeLock();
        dropStaleRoots(*index, volumes, known);
    }

    // With no way to the journals yet, the index service may be on its way
    // (Options::accessWaitMs): waited for, so that no volume loses its place.
    awaitAccess(stop, volumes, journals, known);
    if (stop.stop_requested())
        return; // planned again by whoever stopped us, from what is stored

    // GitHub #52: only the chosen folders, and only the disks they are on;
    // told now that the index service had its chance (with it, the disks
    // are whole). The others go, removable ones without one too unless they
    // are asked for; with whole disks, those listed only for a chosen folder.
    const bool foldersOnly = this->foldersOnly(*scope);
    {
        bool erased = false;
        for (std::size_t i = volumes.size(); i-- > 0;) {
            const std::wstring& root = volumes[i].root;
            const bool outside = foldersOnly
                ? !scope->touches(root) && !(m_options.includeRemovable && isRemovableDrive(root))
                : listedForScope && isRemovableDrive(root) && !contains(suspended, root);
            if (!outside)
                continue;
            volumes.erase(volumes.begin() + static_cast<std::ptrdiff_t>(i));
            journals.erase(journals.begin() + static_cast<std::ptrdiff_t>(i));
            known.erase(known.begin() + static_cast<std::ptrdiff_t>(i));
            erased = true;
        }
        if (erased) {
            const auto index = m_index.load();
            std::lock_guard journalLock(m_journalMutex); // keeps compaction out
            auto lock = index->writeLock();
            dropStaleRoots(*index, volumes, known);
        }
    }
    if (foldersOnly || scope->supplemental()) {
        // A chosen folder whose disk is not here keeps nothing (as a drive
        // taken out) and says so: after what scope.json said of it, and as
        // left out when its drive is (GitHub #52 review).
        std::vector<std::pair<std::wstring, FolderStatus::State>> away;
        for (const std::wstring& f : scope->all()) {
            const bool here = std::any_of(volumes.begin(), volumes.end(),
                [&](const VolumeInfo& v) { return win32::equalsIgnoreCase(v.root, volumeOf(f)); });
            if (!here)
                away.push_back({f, crawler->isExcludedPath(f) ? FolderStatus::State::Excluded : FolderStatus::State::Offline});
            else if (f.size() == 2)
                away.push_back({f, FolderStatus::State::WholeDrive});
        }
        setFolderStates(away);
        if (fromSnapshot && !scopeKnown) {
            std::lock_guard lock(m_journalMutex);
            for (const std::wstring& f : scope->all()) {
                if (f.size() > 2 && !contains(m_verifyOwed, f))
                    m_verifyOwed.push_back(f);
            }
        }
    }
    // Walked for its chosen folders alone (they are none of a volume read through its journal).
    const auto scopedOn = [&](const VolumeInfo& v) -> const std::vector<std::wstring>& {
        static const std::vector<std::wstring> none;
        return foldersOnly ? scope->on(v.root) : none;
    };

    // NTFS volumes follow their change journal from where the index left
    // off. When the journal no longer reaches back that far, the MFT is read
    // again; the journal then replays what changed during the read.
    std::vector<std::size_t> toSync;
    std::vector<std::size_t> walkLater;
    std::vector<std::size_t> scoped; // walked for their chosen folders (GitHub #52)
    std::vector<std::wstring> scopedBefore; // ... and those whose entries were only those
    {
        std::lock_guard lock(m_journalMutex);
        scopedBefore = m_scopedVolumes;
    }
    std::vector<std::optional<ntfs::JournalInfo>> journalInfo(volumes.size());
    const std::shared_ptr<ntfs::VolumeAccess> access = m_access.load();
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        if (isSuspended(volumes[i]))
            continue;
        journalInfo[i] = volumes[i].ntfs ? access->queryJournal(volumes[i].root, volumes[i].serial) : std::nullopt;
        if (const auto& journal = journalInfo[i]) {
            const JournalPosition& p = journals[i];
            const bool current = known[i] && p.journalId == journal->id && p.usn >= journal->firstUsn
                && p.usn <= journal->nextUsn;
            if (pass == Pass::Full || !current) {
                journals[i] = {journal->id, journal->nextUsn};
                toSync.push_back(i);
                if (known[i])
                    contentUnsureOf(volumes[i].root); // what changed meanwhile is not in the journal any more
            } else if (timesMissing) {
                toSync.push_back(i); // the journal then goes on from where the snapshot left off
            }
        } else {
            // Followed through its journal until now (the snapshot's position,
            // with no way to the journal any more): what changed since is in
            // no position of ours, so it is walked whatever the rescan
            // setting (which is about the disks that have no journal) says.
            const bool wasFollowed = known[i] && journals[i].journalId != 0;
            journals[i] = {};
            if (!scopedOn(volumes[i]).empty()) {
                // GitHub #52: all its chosen folders now when the index cannot
                // vouch for them, and a while after startup (as the rescan
                // setting says); new ones and those owed a walk come below.
                scoped.push_back(i);
                if (!known[i] || pass == Pass::Full || timesMissing || wasFollowed || contains(rewalk, volumes[i].root))
                    toSync.push_back(i);
                else if (pass == Pass::Startup && m_options.rescanOnStartup)
                    walkLater.push_back(i);
                continue;
            }
            // GitHub #52: whole again, after it held its chosen folders only.
            const bool wasScoped = contains(scopedBefore, volumes[i].root);
            const bool walkNow = !known[i] || pass == Pass::Full || pass == Pass::Walked || timesMissing
                || contains(rewalk, volumes[i].root) || wasScoped;
            // A scope.json that does not go with the snapshot cannot tell
            // whether it held its chosen folders only (GitHub #52 review):
            // walked as after a startup rescan.
            if (walkNow)
                toSync.push_back(i);
            else if (pass == Pass::Startup && (m_options.rescanOnStartup || wasFollowed || scopeStale))
                walkLater.push_back(i); // let login settle first
        }
    }
    bool listChanged = false;
    std::vector<std::wstring> notOwed; // GitHub #52
    {
        std::lock_guard lock(m_journalMutex);
        listChanged = previous.size() != volumes.size()
            || !std::equal(previous.begin(), previous.end(), volumes.begin(),
                [](const VolumeInfo& a, const VolumeInfo& b) { return a.root == b.root; });
        m_volumes = volumes;
        m_journals = journals;
        std::erase_if(m_suspended, [&](const std::wstring& root) { return contains(gone, root); });
        // A walk owed stays owed until it is done (syncWithDisk crosses it
        // off), even if this run is cut short; the startup rescan too.
        for (const std::size_t i : walkLater) {
            if (!contains(m_rewalk, volumes[i].root))
                m_rewalk.push_back(volumes[i].root);
        }
        std::erase_if(m_rewalk, [&](const std::wstring& root) {
            return std::none_of(volumes.begin(), volumes.end(), [&](const VolumeInfo& v) { return v.root == root; });
        });
        if (pass == Pass::Full)
            m_rulesBefore.reset(); // every volume is read with the current rules
        // GitHub #52: owed a walk only what is chosen on a volume walked for
        // it, here and not being ejected (one that comes back is walked whole).
        const auto walkable = [&](const std::wstring& f) {
            if (scope->supplemental())
                return diskHere(f) && contains(scope->on(volumeOf(f)), f);
            return std::any_of(scoped.begin(), scoped.end(), [&](std::size_t i) { return contains(scopedOn(volumes[i]), f); });
        };
        for (const std::wstring& f : m_verifyOwed) {
            if (!walkable(f))
                notOwed.push_back(f);
        }
        std::erase_if(m_verifyOwed, [&](const std::wstring& f) { return !walkable(f); });
        std::erase_if(m_overflowed, [&](const std::wstring& f) { return !walkable(f); });
    }
    if (!notOwed.empty()) {
        // Asked to be walked, and not to be now.
        {
            std::lock_guard lock(m_foldersMutex);
            for (FolderStatus& s : m_folderStatus) {
                if (s.state == FolderStatus::State::Verifying && contains(notOwed, s.path))
                    s.state = FolderStatus::State::Waiting;
            }
        }
        noteFoldersChanged();
    }
    if (!m_volumesKnown.exchange(true))
        m_contentIndexer->wake();
    if (listChanged)
        QMetaObject::invokeMethod(this, [this] { emit volumesChanged(); }, Qt::QueuedConnection);
    ensureWatcher(); // watch before walking so nothing slips through

    // GitHub #52: the volumes walked for their chosen folders, brought in line with them.
    std::vector<std::wstring> folderWalks;
    if (!scoped.empty()) {
        std::vector<std::wstring> roots;
        for (const std::size_t i : scoped)
            roots.push_back(volumes[i].root);
        folderWalks = prepareScope(stop, *scope, roots, *crawler);
        if (stop.stop_requested()) {
            // Planned again by whoever stopped us: what came in is walked then.
            std::lock_guard lock(m_journalMutex);
            for (std::wstring& f : folderWalks) {
                if (!contains(m_verifyOwed, f))
                    m_verifyOwed.push_back(std::move(f));
            }
            return;
        }
    }

    // Rules that changed since the index was built, applied in place.
    std::optional<CrawlRules> rulesBefore;
    {
        std::lock_guard lock(m_journalMutex);
        rulesBefore = m_rulesBefore;
    }
    if (rulesBefore) {
        switch (applyRuleChanges(*rulesBefore, *crawler, stop)) {
        case RuleChange::Applied:
            break;
        case RuleChange::Stopped:
            return; // applied again next time
        case RuleChange::NeedsFullRead: {
            for (std::size_t i = 0; i < volumes.size(); ++i) {
                if (isSuspended(volumes[i]) || std::find(toSync.begin(), toSync.end(), i) != toSync.end())
                    continue;
                if (journalInfo[i]) {
                    journals[i] = {journalInfo[i]->id, journalInfo[i]->nextUsn};
                    contentUnsureOf(volumes[i].root); // the journal is skipped ahead
                }
                toSync.push_back(i);
            }
            walkLater.clear(); // all in toSync now
            std::lock_guard lock(m_journalMutex);
            m_journals = journals;
            break;
        }
        }
    }

    // Catch up where the index left off (quick), then read what must be read.
    std::vector<std::unique_ptr<Followed>> followed;
    const auto startFollowing = [&](std::size_t i) {
        auto f = follow(i);
        if (!f) {
            // The journal cannot be read after all: walk and watch the volume instead.
            {
                std::lock_guard lock(m_journalMutex);
                m_journals[i] = {};
            }
            ensureWatcher();
            syncWithDisk(stop, {i});
            return;
        }
        if (!catchUp(*f, stop) && !stop.stop_requested())
            recover(f, stop);
        if (f)
            followed.push_back(std::move(f));
    };
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        if (journals[i].journalId != 0 && !isSuspended(volumes[i])
            && std::find(toSync.begin(), toSync.end(), i) == toSync.end())
            startFollowing(i);
    }
    bool owed = false; // chosen folders owed a walk (GitHub #52)
    {
        std::lock_guard lock(m_journalMutex);
        owed = !m_verifyOwed.empty() || !m_removedExtra.empty();
    }
    if ((!toSync.empty() || !folderWalks.empty() || owed || scope->supplemental()) && !syncWithDisk(stop, toSync, std::move(folderWalks)))
        return; // stopped
    for (const std::size_t i : toSync) {
        if (journals[i].journalId != 0)
            startFollowing(i); // replays the changes made while the MFT was read
    }
    followJournals(stop, followed, std::move(walkLater));
}

void IndexService::awaitAccess(std::stop_token stop, const std::vector<VolumeInfo>& volumes,
    const std::vector<JournalPosition>& journals, const std::vector<bool>& known)
{
    const auto readsJournals = [this] { return m_access.load()->kind() != ntfs::VolumeAccess::Kind::None; };
    if (readsJournals() || std::chrono::steady_clock::now() >= m_accessDeadline)
        return;
    bool atStake = false; // a volume followed so far, which a pass without the journals would walk
    for (std::size_t i = 0; i < volumes.size() && !atStake; ++i)
        atStake = known[i] && volumes[i].ntfs && journals[i].journalId != 0;
    if (!atStake)
        return;
    std::unique_lock lock(m_accessMutex);
    m_accessAwaited = true;
    m_accessCv.wait_until(lock, stop, m_accessDeadline, readsJournals);
    m_accessAwaited = false; // before the access is loaded (setVolumeAccess relies on that order)
}

IndexService::RuleChange IndexService::applyRuleChanges(
    const CrawlRules& before, const Crawler& crawler, std::stop_token stop)
{
    const CrawlRules& after = crawler.rules();
    // A folder name no longer excluded could be anywhere, and other ranking
    // rules change flags everywhere: read every volume again.
    if (before.lowPriorityPaths != after.lowPriorityPaths || before.lowPriorityNames != after.lowPriorityNames
        || !added(after.excludedNames, before.excludedNames).empty()) {
        std::lock_guard lock(m_journalMutex);
        m_rulesBefore.reset();
        return RuleChange::NeedsFullRead;
    }
    const std::vector<std::wstring> newPaths = added(before.excludedPaths, after.excludedPaths);
    const std::vector<std::wstring> newNames = added(before.excludedNames, after.excludedNames);
    const std::vector<std::wstring> freedPaths = added(after.excludedPaths, before.excludedPaths);

    // Folders excluded no more, as they are on disk (looked at before locking).
    struct Freed {
        std::wstring path;
        PathProbe probe;
        std::uint32_t record = 0;
    };
    std::vector<Freed> freed;
    const auto scope = this->scope();
    const bool foldersOnly = this->foldersOnly(*scope);
    for (const std::wstring& path : freedPaths) {
        // Still left out by another rule; or on a share, which its own
        // indexer walks again (a probe here could wait on an offline server);
        // or a whole drive, which comes back as a volume (run()).
        if (crawler.isExcludedPath(path) || unc::isUnc(path) || isDriveRoot(path))
            continue;
        // GitHub #52: on a volume walked for its chosen folders, only what is
        // in one (the folders on the way to one are there by name only).
        if (const auto& chosen = foldersOnly ? scope->on(volumeOf(path)) : std::vector<std::wstring> {};
            !chosen.empty() && std::none_of(chosen.begin(), chosen.end(), [&](const std::wstring& f) {
                return win32::equalsIgnoreCase(path, f) || isUnder(path, f);
            }))
            continue;
        Freed f {path, probePath(path)};
        if (f.probe.attributes == INVALID_FILE_ATTRIBUTES || !(f.probe.attributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        f.record = folderRecord(path);
        freed.push_back(std::move(f));
    }

    const auto index = m_index.load();
    std::lock_guard journalLock(m_journalMutex); // keeps compaction out while entry ids are held
    std::vector<Crawler::Root> walks;
    {
        auto lock = index->writeLock();

        removeExcluded(*index, CrawlRules {newPaths, newNames, {}, {}}); // what is excluded now

        // Add the folders excluded no more, to be walked below.
        for (const Freed& f : freed) {
            const auto [parentPath, name] = splitPath(f.path);
            const EntryId parent = index->pathForUpdate(parentPath);
            const std::string utf8 = toUtf8(name);
            if (parent == kNoEntry || name.empty() || index->childForUpdate(parent, utf8, false) != kNoEntry)
                continue;
            const auto inherited = static_cast<std::uint8_t>(index->entry(parent).flags & EntryFlag::Inherited);
            const auto flags = static_cast<std::uint8_t>(
                Crawler::attributeFlags(f.probe.attributes) | inherited | crawler.dirPriorityFlags(f.path, name));
            const EntryId id = index->add(parent, utf8, flags);
            // On a volume followed through its journal, the walk records the folders' numbers.
            EntryId volume = kNoEntry;
            for (std::size_t v = 0; v < m_volumes.size(); ++v) {
                if (m_journals[v].journalId != 0 && isUnder(f.path, m_volumes[v].root))
                    volume = index->findPath(m_volumes[v].root);
            }
            if (volume != kNoEntry && f.record != 0)
                index->setFolderRecord(volume, f.record, id);
            if (Crawler::shouldDescend(f.probe.attributes, f.probe.reparseTag))
                walks.push_back({id, f.path, static_cast<std::uint8_t>(flags & EntryFlag::Inherited), volume, f.record});
        }
    }
    if (!walks.empty() && !crawler.sync(*index, std::move(walks), crawlThreads(), true, stop))
        return RuleChange::Stopped;
    m_rulesBefore.reset();
    m_dirty = true;
    return RuleChange::Applied;
}

std::vector<std::wstring> IndexService::prepareScope(
    std::stop_token stop, const Scope& scope, const std::vector<std::wstring>& roots, const Crawler& crawler, bool prune,
    const std::vector<std::wstring>& renewed)
{
    using State = FolderStatus::State;
    // As they are on disk, looked at before locking: each chosen folder, and
    // the folders on the way to it (whose attributes their entries take).
    // One the index does not vouch for (new, or not read in full at its last
    // walk) is walked, even if it is there by name, on the way to another.
    // Stopped meanwhile (a disk being ejected waits for us), nothing is done.
    struct Chosen {
        std::wstring root;
        std::wstring path;
        bool excluded = false;
        bool vouched = false;
        Presence presence = Presence::Unknown;
        std::vector<std::pair<std::wstring, OnDisk>> steps; // the folders on the way, then the folder itself
    };
    const std::vector<FolderStatus> statuses = folderStatuses();
    std::vector<Chosen> chosen;
    for (const std::wstring& root : roots) {
        for (const std::wstring& path : scope.on(root)) {
            if (stop.stop_requested())
                return {};
            Chosen c {root, path};
            c.vouched = !contains(renewed, root) && std::any_of(statuses.begin(), statuses.end(), [&](const FolderStatus& s) {
                return s.state == State::Ready && win32::equalsIgnoreCase(s.path, path);
            });
            c.excluded = crawler.isExcludedPath(path); // the exclusion wins (design 2.4)
            if (!c.excluded) {
                c.presence = presenceOf(path);
                if (c.presence == Presence::Folder) {
                    std::vector<std::wstring> folders = foldersAbove(root, path);
                    folders.push_back(path);
                    for (std::wstring& folder : folders) {
                        OnDisk d = onDisk(folder);
                        c.steps.emplace_back(std::move(folder), std::move(d));
                    }
                }
            }
            chosen.push_back(std::move(c));
        }
    }

    const auto index = m_index.load();
    const std::int64_t now = secondsNow();
    std::vector<std::wstring> added;
    std::vector<std::pair<std::wstring, State>> states; // checked just now
    std::vector<std::pair<std::wstring, std::size_t>> current; // came from a volume indexed whole: as current as it was
    std::size_t removed = 0;
    {
        std::lock_guard journalLock(m_journalMutex); // keeps compaction out
        auto lock = index->writeLock();
        for (const std::wstring& root : roots) {
            const std::string rootName = toUtf8(root);
            const auto& rs = index->roots();
            const auto it = std::find_if(rs.begin(), rs.end(), [&](EntryId r) { return index->name(r) == rootName; });
            const EntryId rootId = it != rs.end() ? *it : index->addRoot(rootName);
            const bool wasWhole = prune && !contains(m_scopedVolumes, root);
            std::vector<EntryId> keep;
            std::vector<EntryId> through;
            for (const Chosen& c : chosen) {
                if (c.root != root)
                    continue;
                if (c.excluded) {
                    states.push_back({c.path, State::Excluded});
                    if (!prune) {
                        if (const EntryId old = findFolder(*index, c.path); old != kNoEntry)
                            removed += index->remove(old);
                    }
                    continue;
                }
                if (c.presence == Presence::Missing) {
                    states.push_back({c.path, State::NotFound}); // its entries go below
                    if (!prune) {
                        if (const EntryId old = findFolder(*index, c.path); old != kNoEntry)
                            removed += index->remove(old);
                    }
                    continue;
                }
                if (c.presence == Presence::Unknown) {
                    // Cannot be looked at: what the index has of it stays.
                    states.push_back({c.path, State::Unreadable});
                    if (const EntryId e = findFolder(*index, c.path); e != kNoEntry) {
                        keep.push_back(e);
                        for (EntryId p = index->entry(e).parent; p != kNoEntry && p != rootId; p = index->entry(p).parent)
                            through.push_back(p);
                    }
                    continue;
                }
                // The folders on the way, by name only, then the folder itself.
                EntryId e = rootId;
                bool made = false;
                for (std::size_t k = 0; k < c.steps.size(); ++k) {
                    const auto& [folder, d] = c.steps[k];
                    const EntryId parent = e;
                    const std::string utf8 = toUtf8(d.name);
                    e = index->childForUpdate(parent, utf8, false);
                    if (e != kNoEntry && !index->entry(e).isDir()) {
                        index->remove(e); // a file once, a folder now
                        e = kNoEntry;
                    }
                    made = e == kNoEntry;
                    if (made) {
                        const unsigned long attributes = d.probe.attributes == INVALID_FILE_ATTRIBUTES
                            ? FILE_ATTRIBUTE_DIRECTORY : d.probe.attributes | FILE_ATTRIBUTE_DIRECTORY;
                        const auto flags = static_cast<std::uint8_t>(Crawler::attributeFlags(attributes)
                            | (index->entry(parent).flags & EntryFlag::Inherited) | crawler.dirPriorityFlags(folder, d.name));
                        e = index->add(parent, utf8, flags);
                        index->setModified(e, d.probe.modified);
                        index->setCreated(e, d.probe.created);
                    }
                    if (k + 1 < c.steps.size())
                        through.push_back(e);
                }
                if (made)
                    added.push_back(c.path);
                else if (wasWhole)
                    current.push_back({c.path, countBelow(*index, e)});
                else if (!c.vouched)
                    added.push_back(c.path);
                keep.push_back(e);
                if (!contains(m_placed, c.path))
                    m_placed.push_back(c.path); // its entry is there now: see walkFolders
            }
            // What is neither in a chosen folder nor on the way to one.
            if (prune)
                removed += pruneOutside(*index, rootId, keep, through);
            if (wasWhole)
                m_scopedVolumes.push_back(root);
        }
    }
    if (removed > 0 || !added.empty())
        m_dirty = true;
    setFolderStates(states, now);
    if (!current.empty()) {
        // Walked and watched whole until now: their entries are as current as a walk's.
        {
            std::lock_guard lock(m_foldersMutex);
            for (FolderStatus& s : m_folderStatus) {
                for (const auto& [path, items] : current) {
                    if (win32::equalsIgnoreCase(s.path, path)) {
                        s.state = State::Ready;
                        s.items = items;
                        s.lastVerified = now;
                    }
                }
            }
        }
        m_dirty = true;
        noteFoldersChanged();
    }
    return added;
}

bool IndexService::walkFolders(
    std::stop_token stop, FileIndex& index, const Crawler& crawler, std::vector<std::wstring> folders, bool firstBuild)
{
    using State = FolderStatus::State;
    const auto scope = this->scope();
    // A walk again on a hard disk (or one not known not to be) goes two
    // folders at a time: more would make its heads jump about. Looked at once
    // a volume.
    std::vector<std::pair<std::wstring, int>> threadsOn;
    const auto threadsFor = [&](std::wstring_view root) {
        if (firstBuild)
            return crawlThreads();
        for (const auto& [r, n] : threadsOn) {
            if (r == root)
                return n;
        }
        const int n = placementOf(root).seeks ? 2 : crawlThreads();
        threadsOn.emplace_back(std::wstring(root), n);
        return n;
    };
    const auto oweAgain = [&](std::size_t from) {
        std::lock_guard lock(m_journalMutex);
        for (std::size_t k = from; k < folders.size(); ++k) {
            if (!contains(m_verifyOwed, folders[k]))
                m_verifyOwed.push_back(folders[k]);
        }
    };
    for (;;) {
        // And what was asked for meanwhile (a verification, verifyFolder()).
        {
            std::lock_guard lock(m_journalMutex);
            for (std::wstring& f : m_verifyOwed) {
                if (!contains(folders, f))
                    folders.push_back(std::move(f));
            }
            m_verifyOwed.clear();
        }
        std::erase_if(folders, [&](const std::wstring& f) { return !contains(scope->on(volumeOf(f)), f); });
        if (folders.empty())
            return true;
        for (std::size_t k = 0; k < folders.size(); ++k) {
            const std::wstring& path = folders[k];
            if (stop.stop_requested()) {
                oweAgain(k);
                return false;
            }
            EntryId id = kNoEntry;
            std::uint8_t inherited = 0;
            {
                const auto lock = index.readLock();
                id = findFolder(index, path);
                if (id != kNoEntry && index.entry(id).isDir())
                    inherited = static_cast<std::uint8_t>(index.entry(id).flags & EntryFlag::Inherited);
                else
                    id = kNoEntry;
            }
            if (id == kNoEntry) {
                // Not in the index: left out, gone, or come since this pass
                // looked (the next one makes its entry and walks it).
                State state = State::NotFound;
                if (crawler.isExcludedPath(path)) {
                    state = State::Excluded;
                } else if (const Presence presence = presenceOf(path); presence == Presence::Unknown) {
                    state = State::Unreadable;
                } else if (presence == Presence::Folder && contains(m_placed, path)) {
                    // Its entry made this pass, and it cannot be found as the
                    // path is written (GitHub #52 review: walked again every
                    // 3 s): told so, not looked for again and again.
                    qWarning("WinShun: the chosen folder %ls is there, but not in the index as written: not walked",
                        path.c_str());
                    state = State::Unreadable;
                } else if (presence == Presence::Folder) {
                    {
                        std::lock_guard lock(m_journalMutex);
                        if (!contains(m_overflowed, path))
                            m_overflowed.push_back(path);
                    }
                    QMetaObject::invokeMethod(this, [this] { resyncFoldersSoon(false); }, Qt::QueuedConnection);
                    setFolderStates({{path, State::Waiting}});
                    continue;
                }
                setFolderStates({{path, state}}, secondsNow());
                continue;
            }
            setFolderStates({{path, State::Verifying}});
            const auto began = std::chrono::steady_clock::now();
            Crawler::SyncStats stats;
            setReading(path, true);
            const bool done = crawler.sync(index, {{id, path, inherited}}, threadsFor(volumeOf(path)), !firstBuild, stop,
                {}, nullptr, &stats);
            setReading(path, false);
            if (!done) {
                setFolderStates({{path, State::Waiting}});
                oweAgain(k);
                return false;
            }
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
            State state = State::Ready;
            if (stats.listed == 0 && stats.failed > 0) {
                // Not read at all: gone meanwhile (its entries go), or not for this account (they stay).
                if (presenceOf(path) == Presence::Missing && driveLetterExists(volumeOf(path))) {
                    auto lock = index.writeLock();
                    if (const EntryId e = findFolder(index, path); e != kNoEntry)
                        index.remove(e);
                    state = State::NotFound;
                } else {
                    state = State::Unreadable;
                }
            }
            {
                std::lock_guard lock(m_foldersMutex);
                for (FolderStatus& s : m_folderStatus) {
                    if (!win32::equalsIgnoreCase(s.path, path))
                        continue;
                    s.state = state;
                    if (stats.listed > 0)
                        s.items = stats.items;
                    s.failed = stats.failed;
                    s.lastVerified = secondsNow();
                    s.took = took;
                }
            }
            m_dirty = true;
            noteFoldersChanged();
        }
        folders.clear();
    }
}

bool IndexService::syncWithDisk(std::stop_token stop, const std::vector<std::size_t>& which, std::vector<std::wstring> folders)
{
    const auto index = m_index.load();
    const auto crawler = this->crawler();
    const auto scope = this->scope();
    const bool foldersOnly = this->foldersOnly(*scope);
    const bool firstBuild = state() != State::Ready;
    std::vector<VolumeInfo> volumes;
    std::vector<JournalPosition> journals;
    {
        std::lock_guard lock(m_journalMutex);
        m_journaling = true;
        m_journal.clear();
        m_resyncAfter = false;
        volumes = m_volumes;
        journals = m_journals;
    }
    if (!firstBuild)
        setRefreshing(true);

    std::vector<EntryId> roots(volumes.size(), kNoEntry);
    {
        std::lock_guard journalLock(m_journalMutex);
        auto lock = index->writeLock();
        for (const std::wstring& path : m_removedExtra) {
            if (const EntryId id = findFolder(*index, path); id != kNoEntry) {
                while (index->entry(id).firstChild != kNoEntry)
                    index->remove(index->entry(id).firstChild);
                m_dirty = true;
            }
        }
        m_removedExtra.clear();
        // Drop drives that are gone (shares are not drives); add new ones.
        const std::vector<EntryId> existingRoots = index->roots();
        for (const EntryId r : existingRoots) {
            const std::string_view name = index->name(r);
            const bool keep = name.starts_with("\\\\") || std::any_of(
                volumes.begin(), volumes.end(), [&](const VolumeInfo& v) { return toUtf8(v.root) == name; });
            if (!keep)
                index->remove(r);
        }
        for (std::size_t i = 0; i < volumes.size(); ++i) {
            const std::string name = toUtf8(volumes[i].root);
            const auto& rs = index->roots();
            const auto it = std::find_if(rs.begin(), rs.end(), [&](EntryId r) { return index->name(r) == name; });
            roots[i] = it != rs.end() ? *it : index->addRoot(name);
        }
        index->setInterning(true);
    }

    // NTFS volumes are read from their MFT, the volumes of different disks at
    // the same time; the others are walked.
    std::vector<Crawler::Root> walks;
    std::vector<std::size_t> fromMft;
    for (const std::size_t i : which) {
        if (journals[i].journalId != 0) {
            fromMft.push_back(i);
        } else if (const auto& chosen = foldersOnly ? scope->on(volumes[i].root) : std::vector<std::wstring> {};
                   !chosen.empty()) {
            for (const std::wstring& f : chosen) { // GitHub #52: each on its own, below
                if (!contains(folders, f))
                    folders.push_back(f);
            }
        } else {
            walks.push_back({roots[i], volumes[i].root, 0});
        }
    }
    std::atomic<bool> readMft = false;
    std::mutex walksMutex;
    {
        std::vector<std::jthread> readers;
        for (std::vector<std::size_t>& group : readingGroups(volumes, fromMft)) {
            readers.emplace_back([&, group = std::move(group)] {
                for (const std::size_t i : group) {
                    if (stop.stop_requested())
                        return;
                    const VolumeInfo& v = volumes[i];
                    {
                        auto lock = index->writeLock();
                        index->setFolderRecord(roots[i], ntfs::kRootRecord, roots[i]);
                    }
                    setReading(v.root, true);
                    const bool read = syncFromMft(v, roots[i], *crawler, !firstBuild, stop);
                    setReading(v.root, false);
                    if (read) {
                        readMft = true;
                    } else if (!stop.stop_requested()) {
                        // Walking works too, a few times slower (on a hard disk, far
                        // slower); it records the folder numbers as well.
                        std::lock_guard lock(walksMutex);
                        walks.push_back({roots[i], v.root, 0, roots[i], ntfs::kRootRecord});
                    }
                }
            });
        }
    } // joins
    bool completed = !stop.stop_requested();
    // GitHub #52: the chosen folders first, the volumes walked whole after them.
    if (completed && foldersOnly)
        completed = walkFolders(stop, *index, *crawler, std::move(folders), firstBuild);
    if (completed && !walks.empty()) {
        std::vector<std::wstring> walked;
        for (const Crawler::Root& w : walks)
            walked.push_back(w.path);
        for (const std::wstring& root : walked)
            setReading(root, true);
        completed = crawler->sync(*index, std::move(walks), crawlThreads(), !firstBuild, stop);
        for (const std::wstring& root : walked)
            setReading(root, false);
    }

    // A disk-wide walk or MFT refresh intentionally skips directory links.
    // Explicit links are read afterwards, under their original visible paths.
    if (completed && scope->supplemental()) {
        std::vector<std::wstring> available;
        std::vector<std::wstring> renewed;
        {
            std::lock_guard lock(m_journalMutex);
            for (const auto& volume : volumes) {
                if (!contains(m_suspended, volume.root))
                    available.push_back(volume.root);
            }
        }
        for (const auto i : which)
            renewed.push_back(volumes[i].root);
        for (auto& path : prepareScope(stop, *scope, available, *crawler, false, renewed)) {
            if (!contains(folders, path))
                folders.push_back(std::move(path));
        }
        completed = !stop.stop_requested() && walkFolders(stop, *index, *crawler, std::move(folders), firstBuild);
    }

    {
        auto lock = index->writeLock();
        index->setInterning(false); // frees the lookup table
    }
    {
        std::lock_guard lock(m_journalMutex);
        applyChanges(*index, m_journal, *crawler, m_shutdown.get_token()); // replay what changed on watched volumes during the walk
        m_journal = decltype(m_journal)(); // releases the capacity, unlike `= {}`
        m_journaling = false;
        for (const std::size_t i : which) {
            const std::wstring& root = volumes[i].root;
            if (journals[i].journalId == 0) {
                // A walked volume: its walk is no longer owed, or still is.
                std::erase(m_rewalk, root);
                if (!completed)
                    m_rewalk.push_back(root);
            } else if (!completed && i < m_journals.size()) {
                m_journals[i] = {}; // read only in part: read again next time, not caught up
            }
            // GitHub #52: read whole, it holds more than chosen folders now.
            if (completed && !(foldersOnly && journals[i].journalId == 0 && !scope->on(root).empty()))
                std::erase(m_scopedVolumes, root);
        }
        if (m_resyncAfter) {
            m_resyncAfter = false;
            QMetaObject::invokeMethod(this, [this] { m_resyncTimer.start(); }, Qt::QueuedConnection);
        }
    }
    setRefreshing(false);
    if (!completed)
        return false; // stopped: a half-built index is never marked ready or saved

    // Removed items, and names the MFT read stored for excluded folders.
    compactIfWasteful(readMft.load());
    ::HeapCompact(::GetProcessHeap(), 0); // hand scratch memory back to the OS
    m_dirty = true;
    setState(State::Ready);
    // Never waits for the files, which a move holds for a while: the GUI
    // thread may be waiting for this one (stopWorker).
    saveSnapshot(false);
    return true;
}

bool IndexService::syncFromMft(
    const VolumeInfo& volume, EntryId root, const Crawler& crawler, bool lowPriority, std::stop_token stop)
{
    const auto index = m_index.load();
    if (lowPriority)
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    MftTree tree;
    std::wstring error = L"no way to the MFT";
    const std::unique_ptr<ntfs::RecordSource> source = m_access.load()->openMft(volume.root, volume.serial, lowPriority);
    const bool read = source
        && tree.read(*source, *index, stop, &error, [&](std::uint64_t done, std::uint64_t total) {
               setReadingPercent(volume.root, total > 0 ? static_cast<int>(std::min(done, total) * 100 / total) : -1);
           });
    setReadingPercent(volume.root, -1); // the index is built from it now
    if (lowPriority)
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    if (!read) {
        qWarning("WinShun: cannot read the MFT of %ls: %ls", volume.root.c_str(), error.c_str());
        return false;
    }
    // The listings are in memory: one thread does.
    return crawler.sync(*index, {{root, volume.root, 0, root, ntfs::kRootRecord}}, 1, lowPriority, stop,
        [&](const Crawler::Root& dir, DirListing& out) { return tree.list(*index, crawler, dir, out); });
}

std::unique_ptr<IndexService::Followed> IndexService::follow(std::size_t volume)
{
    VolumeInfo v;
    JournalPosition position;
    {
        std::lock_guard lock(m_journalMutex);
        if (volume >= m_volumes.size())
            return nullptr;
        v = m_volumes[volume];
        position = m_journals[volume];
    }
    auto f = std::make_unique<Followed>();
    f->volume = volume;
    f->usn = position.usn;
    f->index = m_index.load();
    f->crawler = crawler();
    f->reader = m_access.load()->openJournal(v.root, v.serial, position.journalId);
    f->applier = std::make_unique<UsnApplier>(*f->index, *f->crawler, toUtf8(v.root),
        [content = m_content](std::span<const EntryId> written) { content->markChanged(written); });
    return f->reader && f->reader->valid() ? std::move(f) : nullptr;
}

// Applies the read started on f. False when the journal lost track of the
// volume: it wrapped past our position, was deleted, or the volume is gone.
bool IndexService::readJournal(Followed& f, std::stop_token stop)
{
    f.reading = false;
    std::int64_t next = 0;
    if (!f.reader->finish(f.records, next))
        return false;
    std::lock_guard lock(m_journalMutex); // keeps compaction out
    if (!f.records.empty()) {
        f.applier->apply(f.records, stop);
        m_dirty = true;
        if (stop.stop_requested())
            return true; // applied in part: the position stays, a replay redoes it
    }
    f.usn = next;
    if (f.volume < m_journals.size())
        m_journals[f.volume].usn = f.applier->safePosition(next);
    return true;
}

bool IndexService::catchUp(Followed& f, std::stop_token stop)
{
    for (;;) {
        const std::int64_t from = f.usn;
        f.reader->start(from, false);
        if (!readJournal(f, stop))
            return false;
        if (stop.stop_requested() || f.usn == from)
            return true;
    }
}

void IndexService::recover(std::unique_ptr<Followed>& f, std::stop_token stop)
{
    // The journal lost track of the volume (it wrapped, or was deleted and
    // made anew): read the volume again, then follow the journal from there.
    const std::size_t volume = f->volume;
    f.reset();
    VolumeInfo v;
    {
        std::lock_guard lock(m_journalMutex);
        v = m_volumes[volume];
    }
    if (!driveLetterExists(v.root))
        return; // pulled out without ejecting: refreshVolumes() drops it
    const auto journal = m_access.load()->queryJournal(v.root, v.serial);
    {
        std::lock_guard lock(m_journalMutex);
        m_journals[volume] = journal ? JournalPosition {journal->id, journal->nextUsn} : JournalPosition {};
    }
    contentUnsureOf(v.root); // changes the journal lost
    if (!journal)
        ensureWatcher(); // walked and watched from now on
    if (!syncWithDisk(stop, {volume}) || !journal)
        return;
    f = follow(volume);
    if (f && !catchUp(*f, stop))
        f.reset();
}

void IndexService::followJournals(
    std::stop_token stop, std::vector<std::unique_ptr<Followed>>& followed, std::vector<std::size_t> walkLater)
{
    const win32::UniqueHandle stopEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const std::stop_callback onStop(stop, [h = stopEvent.get()] { ::SetEvent(h); });
    const auto walkAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_options.rescanDelayMs);

    while (!stop.stop_requested()) {
        std::erase(followed, nullptr);
        if (followed.empty() && walkLater.empty())
            return; // nothing to follow; the watcher thread handles walked volumes
        const auto now = std::chrono::steady_clock::now();
        auto wakeAt = std::chrono::steady_clock::time_point::max();
        if (!walkLater.empty())
            wakeAt = walkAt;
        std::vector<HANDLE> handles {stopEvent.get()};
        for (auto& f : followed) {
            if (!f->reading && f->resumeAt <= now) {
                f->reader->start(f->usn, true); // completes once there is something new
                f->reading = true;
            }
            if (f->reading) {
                handles.push_back(f->reader->event());
            } else {
                wakeAt = std::min(wakeAt, f->resumeAt);
            }
        }
        DWORD timeout = INFINITE;
        if (wakeAt != std::chrono::steady_clock::time_point::max()) {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(wakeAt - now);
            timeout = static_cast<DWORD>(std::max<std::int64_t>(left.count(), 0));
        }
        const DWORD r = ::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED)
            break;
        if (!walkLater.empty() && std::chrono::steady_clock::now() >= walkAt) {
            // Pick up what changed on walked volumes while the app was not running.
            syncWithDisk(stop, walkLater);
            walkLater.clear();
        }
        // Every volume whose read completed, not only the first: none starves.
        for (auto& f : followed) {
            if (stop.stop_requested())
                break;
            if (!f->reading || ::WaitForSingleObject(f->reader->event(), 0) != WAIT_OBJECT_0)
                continue;
            if (readJournal(*f, stop))
                f->resumeAt = std::chrono::steady_clock::now() + kJournalPause;
            else
                recover(f, stop);
        }
    }
}

void IndexService::ensureWatcher()
{
    const auto chosen = scope();
    std::vector<std::wstring> roots;
    {
        std::lock_guard lock(m_journalMutex);
        for (std::size_t i = 0; i < m_volumes.size(); ++i) {
            if ((i >= m_journals.size() || m_journals[i].journalId == 0) && !contains(m_suspended, m_volumes[i].root))
                roots.push_back(m_volumes[i].root);
        }
    }
    for (const std::wstring& path : chosen->all()) {
        if (!hasDirectoryLink(path) || crawler()->isExcludedPath(path))
            continue;
        std::lock_guard lock(m_journalMutex);
        if (diskHere(path) && !contains(roots, path))
            roots.push_back(path); // watching a volume does not follow its junction targets
    }
    // GitHub #52: a volume walked for its chosen folders whose root cannot be
    // watched (this account may not list it, as some D:\ are set up): the
    // folders themselves in its place.
    std::vector<ChangeWatcher::Fallback> instead;
    if (const auto scope = this->scope(); foldersOnly(*scope)) {
        for (const std::wstring& root : roots) {
            if (const std::vector<std::wstring>& chosen = scope->on(root); !chosen.empty())
                instead.push_back({root, chosen});
        }
    }
    if (!m_watcher) {
        if (roots.empty())
            return;
        m_watcher = std::make_unique<ChangeWatcher>(
            [this](std::vector<FsChange>&& changes) { onChanges(std::move(changes)); });
    }
    // Only when something changes, or is not watched (a root that may open
    // now, a folder made since): the rest stay watched without a gap.
    const std::vector<std::wstring> current = m_watcher->roots();
    const auto covered = [&] {
        for (const std::wstring& root : roots) {
            if (contains(current, root))
                continue;
            const auto fallback = std::find_if(
                instead.begin(), instead.end(), [&](const ChangeWatcher::Fallback& f) { return f.root == root; });
            if (fallback == instead.end()
                || !std::all_of(fallback->folders.begin(), fallback->folders.end(),
                    [&](const std::wstring& d) { return contains(current, d); }))
                return false;
        }
        return true;
    };
    if (roots != m_watchRoots || instead != m_watchInstead || !covered()) {
        m_watchRoots = roots;
        m_watchInstead = instead;
        m_watcher->setRoots(std::move(roots), std::move(instead));
    }

    // GitHub #52: which chosen folders are watched (through their volume's
    // root, or by themselves).
    const std::vector<std::wstring> watched = m_watcher->roots();
    bool changed = false;
    {
        std::lock_guard lock(m_foldersMutex);
        for (FolderStatus& s : m_folderStatus) {
            const bool watching = contains(m_watchRoots, s.path) ? contains(watched, s.path)
                : contains(watched, volumeOf(s.path)) || contains(watched, s.path);
            changed = changed || s.watching != watching;
            s.watching = watching;
        }
    }
    if (changed)
        noteFoldersChanged();
}

void IndexService::onChanges(std::vector<FsChange>&& changes)
{
    using Kind = FsChange::Kind;
    const auto crawler = this->crawler();
    const auto scope = this->scope();
    bool overflow = false; // of a volume walked whole
    std::vector<std::wstring> owed; // GitHub #52: chosen folders to walk again in a moment
    bool lost = false; // ... some because their watcher lost events
    if (!foldersOnly(*scope)) {
        overflow = std::any_of(changes.begin(), changes.end(), [](const FsChange& c) { return c.kind == Kind::Overflow; });
        if (scope->supplemental()) {
            const ScopedChanges extra = changesInScope(changes,
                [&](std::wstring_view root) -> const std::vector<std::wstring>& { return scope->on(root); },
                [](const std::wstring& path) { return presenceOf(path) == Presence::Folder; });
            owed = extra.owed;
            lost = extra.lost;
        }
    } else {
        ScopedChanges scoped = changesInScope(
            std::move(changes), [&](std::wstring_view root) -> const std::vector<std::wstring>& { return scope->on(root); },
            [](const std::wstring& path) { return presenceOf(path) == Presence::Folder; });
        changes = std::move(scoped.apply);
        owed = std::move(scoped.owed);
        lost = scoped.lost;
        overflow = scoped.wholeLost;
    }
    std::erase_if(changes, [&](const FsChange& c) { return c.kind == Kind::Overflow || crawler->isExcludedPath(c.path); });

    std::lock_guard lock(m_journalMutex);
    if (!owed.empty()) {
        for (const std::wstring& f : owed) {
            if (!contains(m_overflowed, f))
                m_overflowed.push_back(f);
        }
        QMetaObject::invokeMethod(this, [this, lost] { resyncFoldersSoon(lost); }, Qt::QueuedConnection);
    }
    if (overflow) {
        // Events were lost: only a sync can tell what changed.
        if (m_journaling)
            m_resyncAfter = true;
        else
            QMetaObject::invokeMethod(this, [this] { m_resyncTimer.start(); }, Qt::QueuedConnection);
    }
    if (changes.empty())
        return;
    if (m_journaling) {
        if (m_journal.size() + changes.size() > kMaxJournal) {
            m_journal = decltype(m_journal)();
            m_resyncAfter = true;
        } else {
            m_journal.insert(m_journal.end(), changes.begin(), changes.end());
        }
        return;
    }
    applyChanges(*m_index.load(), changes, *crawler, m_shutdown.get_token());
    m_dirty = true;
}

void IndexService::saveSnapshot(bool wait)
{
    std::unique_lock files(m_filesMutex, std::defer_lock);
    if (wait) {
        files.lock();
    } else if (!files.try_lock()) {
        m_dirty = true; // the next periodic save, or the one on exit
        return;
    }
    std::vector<std::uint64_t> segments;
    if (writeSnapshot(indexfolder::snapshotPath(m_folder), segments) == Save::Written) {
        m_lastSave = std::chrono::steady_clock::now();
        m_content->saved(segments);
    }
    m_garbageAfterSave = m_content->obsoleteBytes(); // what could not be deleted (or came meanwhile)
}

IndexService::Save IndexService::writeSnapshot(const QString& path, std::vector<std::uint64_t>& segments)
{
    if (state() != State::Ready)
        return Save::Skipped; // never persist a half-built index
    const auto index = m_index.load();
    std::vector<VolumeInfo> volumes;
    std::vector<JournalPosition> journals;
    CrawlRules rules;
    ScopeFile scope;
    {
        std::lock_guard lock(m_journalMutex);
        if (m_journaling)
            return Save::Skipped; // a sync is under way; it saves when it is done
        volumes = m_volumes;
        journals = m_journals;
        rules = m_rulesBefore ? *m_rulesBefore : crawler()->rules(); // what the index reflects
        scope.scoped = m_scopedVolumes;
    }
    m_dirty = false;
    m_content->takeChanged();
    bool saved = false;
    {
        // Streams through a 1 MB buffer into the OS file cache; searches keep
        // running meanwhile (they only need the read lock too, and changes
        // wait at the writers' gate: longReadLock). Changes applied after the
        // journal positions were taken are replayed on the next start, which
        // is harmless. The content index's documents go along, so they always
        // refer to the entries saved with them.
        const auto lock = index->longReadLock();
        saved = snapshot::save(*index, volumes, journals, rules, path,
            [&](const std::vector<EntryId>& newIds) { return m_content->serialize(newIds, segments); });
    }
    if (saved) {
        // GitHub #52: beside it, and so with it. A volume that came to hold
        // its chosen folders only while it was saved counts as such: at
        // worst it is walked whole once more than need be.
        {
            std::lock_guard lock(m_journalMutex);
            for (const std::wstring& root : m_scopedVolumes) {
                if (!contains(scope.scoped, root))
                    scope.scoped.push_back(root);
            }
        }
        scope.folders = folderStatuses();
        if (!writeScopeFile(path, scope))
            qWarning("WinShun: cannot save the chosen folders' state beside %ls", qUtf16Printable(QDir::toNativeSeparators(path)));
        return Save::Written;
    }
    m_dirty = true;
    qWarning("WinShun: cannot save the index to %ls", qUtf16Printable(QDir::toNativeSeparators(path)));
    return Save::Failed;
}

void IndexService::moveTo(const QString& folder)
{
    if (m_stopped || m_moving.exchange(true))
        return;
    if (m_mover.joinable())
        m_mover.join(); // the last move, over already
    m_mover = std::jthread([this, to = QDir::cleanPath(QDir::fromNativeSeparators(folder))](std::stop_token stop) {
        const std::error_code error = moveFiles(to, stop);
        if (error && error.value() != ERROR_CANCELLED)
            qWarning("WinShun: cannot move the index to %ls (error %d)", qUtf16Printable(QDir::toNativeSeparators(to)),
                error.value());
        m_moving = false;
        if (!error)
            prewarmSoon(5s); // another disk, maybe a hard one; and the copies may have gone around the cache
        QMetaObject::invokeMethod(
            this, [this, code = error.value()] { emit moveFinished(code); }, Qt::QueuedConnection);
    });
}

// Copies the files over, writes a fresh snapshot there, then switches to the
// copies and deletes the originals. Up to the switch the originals are left
// as they are and stay in use, so a failure (or a shutdown) before it changes
// nothing.
std::error_code IndexService::moveFiles(const QString& to, std::stop_token stop)
{
    const auto systemError = [](DWORD code) { return std::error_code(static_cast<int>(code), std::system_category()); };
    // No segment file is written or merged away meanwhile, and no text added.
    // The journal may still mark documents dirty, which touches no file.
    m_contentIndexer->pause();
    const auto resume = qScopeGuard([this] { m_contentIndexer->resume(); });
    std::unique_lock files(m_filesMutex); // nothing saved or loaded meanwhile
    const QString from = m_folder;
    if (stop.stop_requested())
        return systemError(ERROR_CANCELLED);
    if (QString::compare(from, to, Qt::CaseInsensitive) == 0)
        return {};

    int reported = -1;
    const std::error_code copied = indexfolder::copy(from, to, [&](std::uint64_t done, std::uint64_t total) {
        const int percent = total > 0 ? static_cast<int>(done * 100 / total) : 100;
        if (percent != reported) {
            reported = percent;
            QMetaObject::invokeMethod(this, [this, percent] { emit moveProgress(percent); }, Qt::QueuedConnection);
        }
        return !stop.stop_requested();
    });
    if (copied)
        return stop.stop_requested() ? systemError(ERROR_CANCELLED) : copied;

    // The copies are a whole index already: their snapshot refers to segment
    // files that are all there. One of the index as it is now replaces it,
    // unless a sync is under way (which saves once it is done).
    std::vector<std::uint64_t> segments;
    const Save saved = writeSnapshot(indexfolder::snapshotPath(to), segments);
    std::error_code error;
    if (saved == Save::Failed)
        error = systemError(ERROR_WRITE_FAULT);
    else if (stop.stop_requested())
        error = systemError(ERROR_CANCELLED);
    else if (!m_content->relocate(indexfolder::contentPath(to)))
        error = systemError(ERROR_FILE_CORRUPT);
    if (error) {
        m_dirty = true; // saved over there only: saved here again
        indexfolder::remove(to);
        return error;
    }
    m_folder = to;
    if (saved == Save::Written)
        m_content->saved(segments); // the segment files no snapshot refers to any more go
    files.unlock();
    indexfolder::remove(from); // nothing has them open now
    return {};
}

void IndexService::prewarmSoon(std::chrono::milliseconds delay)
{
    {
        const std::lock_guard lock(m_prewarmMutex);
        const auto at = std::chrono::steady_clock::now() + delay;
        m_prewarmAt = m_prewarmAt ? std::min(*m_prewarmAt, at) : at;
    }
    m_prewarmCv.notify_all();
}

void IndexService::prewarmLoop(std::stop_token stop)
{
    for (;;) {
        std::unique_lock lock(m_prewarmMutex);
        if (!m_prewarmCv.wait(lock, stop, [this] { return m_prewarmAt.has_value(); }))
            return;
        // Until it is due; an earlier request wakes this up.
        while (std::chrono::steady_clock::now() < *m_prewarmAt) {
            const auto at = *m_prewarmAt;
            m_prewarmCv.wait_until(lock, stop, at, [&] { return *m_prewarmAt != at; });
            if (stop.stop_requested())
                return;
        }
        m_prewarmAt.reset();
        lock.unlock();
        prewarm(stop);
    }
}

void IndexService::prewarm(std::stop_token stop)
{
    const auto cancelled = [&] { return stop.stop_requested() || m_moving.load(); };
    if (cancelled())
        return; // a move asks again once it is done
    if (m_contentIndexer->reading()) {
        prewarmSoon(kPrewarmAfterStart); // not beside the files being read: on a hard disk both would seek
        return;
    }
    QString folder;
    {
        const std::lock_guard files(m_filesMutex);
        folder = m_folder;
    }
    if (indexfolder::onSpinningDisk(folder) == false)
        return; // a solid-state disk reads them in no time when they are needed
    indexfolder::readIntoCache(m_content->segmentPaths(), cancelled);
}

void IndexService::saveInBackground()
{
    if (std::chrono::steady_clock::now() - m_lastSave.load() < kSaveInterval
        && m_content->obsoleteBytes() < m_garbageAfterSave + kGarbageBytes)
        return;
    if (m_content->takeChanged())
        m_dirty = true;
    if (!m_dirty || state() != State::Ready)
        return;
    if (m_pendingSave.valid() && m_pendingSave.wait_for(0s) != std::future_status::ready)
        return;
    m_pendingSave = std::async(std::launch::async, [this] {
        compactIfWasteful();
        saveSnapshot();
    });
}

void IndexService::compactIfWasteful(bool always)
{
    // The journal lock keeps out everything that holds entry ids across write
    // locks: a disk walk (journaling) and live changes, which may walk new
    // folders. Content searches and the content indexer pin the ids instead.
    std::lock_guard journalLock(m_journalMutex);
    if (m_journaling)
        return;
    always = always || m_compactOwed.exchange(false);
    const auto index = m_index.load();
    auto lock = index->writeLock();
    const std::size_t live = index->liveCount();
    // Worth a pause in searches (~30 ms per million items) only once it frees a few MB.
    if (!always && index->slotCount() - live < std::max(kMinCompactSlots, live / 16))
        return;
    if (index->idsPinned()) {
        m_compactOwed = always; // the next periodic save tries again
        return;
    }
    index->compact(always, [&](const FileIndex::Renumber& renumber) { m_content->remap(renumber); });
}

void IndexService::contentUnsureOf(const std::wstring& root)
{
    const auto index = m_index.load();
    const std::string name = toUtf8(root);
    const auto lock = index->longReadLock(); // through all the content index knows
    const auto& roots = index->roots();
    const auto it = std::find_if(roots.begin(), roots.end(), [&](EntryId r) { return index->name(r) == name; });
    if (it == roots.end())
        return;
    const EntryId volume = *it;
    m_content->markUnsure([&](EntryId e) {
        if (e >= index->slotCount())
            return false;
        while (index->entry(e).parent != kNoEntry)
            e = index->entry(e).parent;
        return e == volume;
    });
}

std::shared_ptr<const std::vector<std::wstring>> IndexService::contentSharesOf(const ShareIndexer::Options& shares)
{
    auto out = std::make_shared<std::vector<std::wstring>>();
    for (const ShareConfig& s : shares.shares) {
        if (s.content)
            out->push_back(s.path);
    }
    return out;
}

std::vector<ContentIndexer::Volume> IndexService::contentVolumes() const
{
    // The shares whose contents are indexed (GitHub #114), as configured
    // (ContentIndexer matches them to their roots with unc::equal): always
    // there, so that their documents are kept; suspended but while read and
    // followed (offline, read again, not known yet after the start). Asked
    // before the journal mutex is taken: the share indexer has one of its own.
    std::vector<ContentIndexer::Volume> shares;
    if (const auto content = m_contentShares.load(); content && !content->empty()) {
        const std::vector<ShareIndexer::Status> statuses = m_shares ? m_shares->statuses() : std::vector<ShareIndexer::Status> {};
        for (const std::wstring& path : *content) {
            const auto it = std::ranges::find_if(statuses, [&](const ShareIndexer::Status& s) { return unc::equal(s.path, path); });
            ContentIndexer::Volume v;
            v.root = toUtf8(path);
            v.suspended = it == statuses.end() || it->state != ShareIndexer::State::Online;
            v.remote = true;
            shares.push_back(std::move(v));
        }
    }
    std::lock_guard lock(m_journalMutex);
    std::vector<ContentIndexer::Volume> out;
    for (std::size_t i = 0; i < m_volumes.size() && i < m_journals.size(); ++i) {
        if (m_journals[i].journalId != 0)
            out.push_back({toUtf8(m_volumes[i].root), contains(m_suspended, m_volumes[i].root), m_volumes[i].serial});
    }
    out.insert(out.end(), std::make_move_iterator(shares.begin()), std::make_move_iterator(shares.end()));
    return out;
}

void IndexService::setState(State state)
{
    if (m_state.exchange(state) != state) {
        notifyLater();
        if (state == State::Ready)
            m_contentIndexer->wake(); // it waits for a complete index
    }
}

void IndexService::setRefreshing(bool refreshing)
{
    if (m_refreshing.exchange(refreshing) != refreshing) {
        notifyLater();
        if (!refreshing)
            m_contentIndexer->wake();
    }
}

void IndexService::notifyLater()
{
    QMetaObject::invokeMethod(this, [this] { emit stateChanged(); }, Qt::QueuedConnection);
}

} // namespace ws

#include "IndexService.h"

#include "Ntfs.h"
#include "NtfsIndexer.h"
#include "Snapshot.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QtGlobal>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <optional>

using namespace std::chrono_literals;

namespace qf {

namespace {

constexpr std::size_t kApplyBatch = 256; // changes applied per write lock
// A journal read completes on every record written, even ones it filters
// out: after each read, let records gather this long before the next one.
constexpr auto kJournalPause = 500ms;
constexpr std::size_t kMaxJournal = 200'000; // beyond this, just sync again
constexpr std::size_t kMinCompactSlots = 65'536; // removed items (~40 bytes each) before compacting

struct Probe {
    DWORD attributes = INVALID_FILE_ATTRIBUTES;
    DWORD reparseTag = 0;
};

Probe probe(const std::wstring& path)
{
    Probe p;
    WIN32_FIND_DATAW fd;
    const win32::UniqueFind find(
        ::FindFirstFileExW(win32::longPath(path).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0));
    if (find.valid()) {
        p.attributes = fd.dwFileAttributes;
        p.reparseTag = fd.dwReserved0;
    }
    return p;
}

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
// gone, and drive letters that now hold another disk. Returns entries removed.
std::size_t dropStaleRoots(FileIndex& index, const std::vector<VolumeInfo>& volumes, const std::vector<bool>& known)
{
    std::size_t removed = 0;
    const std::vector<EntryId> roots = index.roots();
    for (const EntryId r : roots) {
        const std::string_view name = index.name(r);
        bool keep = false;
        for (std::size_t i = 0; i < volumes.size() && !keep; ++i)
            keep = known[i] && toUtf8(volumes[i].root) == name;
        if (!keep)
            removed += index.remove(r);
    }
    return removed;
}

} // namespace

struct IndexService::Followed {
    std::size_t volume = 0;
    std::int64_t usn = 0; // the next record to read
    std::shared_ptr<FileIndex> index;
    std::shared_ptr<const Crawler> crawler; // the applier's rules
    std::unique_ptr<ntfs::JournalReader> reader;
    std::unique_ptr<UsnApplier> applier;
    std::vector<ntfs::UsnRecord> records; // of the last read
    bool reading = false;
    std::chrono::steady_clock::time_point resumeAt {}; // the next read starts then
};

IndexService::IndexService(Options options, QObject* parent)
    : QObject(parent)
    , m_options(std::move(options))
{
    m_crawler.store(std::make_shared<const Crawler>(m_options.rules));
    m_index.store(std::make_shared<FileIndex>());

    m_saveTimer.setInterval(10min);
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
    launch(Pass::Startup);
    m_saveTimer.start();
}

void IndexService::rebuild()
{
    if (!m_stopped)
        launch(Pass::Full);
}

void IndexService::setOptions(Options options)
{
    const bool rulesChanged = !(options.rules == m_options.rules);
    const bool drivesChanged = options.includeRemovable != m_options.includeRemovable;
    if ((!rulesChanged && !drivesChanged) || m_stopped)
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
    m_crawler.store(std::make_shared<const Crawler>(m_options.rules));
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
    // watcher its root folder: both let go before this returns.
    stopWorker();
    ensureWatcher();
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
    m_shutdown.request_stop();
    stopWorker();
    m_watcher.reset();
    if (m_pendingSave.valid())
        m_pendingSave.wait();
    if (m_dirty)
        saveSnapshot();
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
    m_worker = std::jthread([this, pass](std::stop_token stop) { run(stop, pass); });
}

void IndexService::run(std::stop_token stop, Pass pass)
{
    const auto crawler = this->crawler();
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
    // once it is gone.
    std::vector<VolumeInfo> volumes = listLocalVolumes(m_options.includeRemovable, suspended);
    std::vector<std::wstring> gone;
    for (const std::wstring& root : suspended) {
        const auto it = std::find_if(previous.begin(), previous.end(),
            [&](const VolumeInfo& v) { return win32::equalsIgnoreCase(v.root, root); });
        if (it != previous.end() && driveLetterExists(root))
            volumes.push_back(*it);
        else
            gone.push_back(root);
    }
    std::sort(volumes.begin(), volumes.end(), [](const VolumeInfo& a, const VolumeInfo& b) { return a.root < b.root; });
    const auto isSuspended = [&](const VolumeInfo& v) { return contains(suspended, v.root) && !contains(gone, v.root); };

    // Where each volume's entries stand: in the index already (`known`), and
    // how far its change journal has been applied.
    std::vector<JournalPosition> journals(volumes.size());
    std::vector<bool> known(volumes.size(), false);
    std::optional<CrawlRules> snapshotRules;
    if (pass == Pass::Startup) {
        setState(State::Loading);
        if (auto contents = snapshot::load(m_options.snapshotPath)) {
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
            if (dropStaleRoots(loaded, volumes, known) > 0)
                loaded.compact();
            m_index.store(std::shared_ptr<FileIndex>(std::move(contents->index)));
            snapshotRules = std::move(contents->rules);
            setState(State::Ready);
        } else {
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

    // NTFS volumes follow their change journal from where the index left
    // off. When the journal no longer reaches back that far, the MFT is read
    // again; the journal then replays what changed during the read.
    std::vector<std::size_t> toSync;
    std::vector<std::size_t> walkLater;
    std::vector<std::optional<ntfs::JournalInfo>> journalInfo(volumes.size());
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        if (isSuspended(volumes[i]))
            continue;
        journalInfo[i] = volumes[i].ntfs ? ntfs::queryJournal(volumes[i].root) : std::nullopt;
        if (const auto& journal = journalInfo[i]) {
            const JournalPosition& p = journals[i];
            const bool current = known[i] && p.journalId == journal->id && p.usn >= journal->firstUsn
                && p.usn <= journal->nextUsn;
            if (pass == Pass::Full || !current) {
                journals[i] = {journal->id, journal->nextUsn};
                toSync.push_back(i);
            }
        } else {
            journals[i] = {};
            const bool walkNow
                = !known[i] || pass == Pass::Full || pass == Pass::Walked || contains(rewalk, volumes[i].root);
            if (walkNow)
                toSync.push_back(i);
            else if (pass == Pass::Startup && m_options.rescanOnStartup)
                walkLater.push_back(i); // let login settle first
        }
    }
    bool listChanged = false;
    {
        std::lock_guard lock(m_journalMutex);
        listChanged = m_volumes.size() != volumes.size()
            || !std::equal(m_volumes.begin(), m_volumes.end(), volumes.begin(),
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
        if (snapshotRules && !(*snapshotRules == crawler->rules()) && !m_rulesBefore)
            m_rulesBefore = std::move(snapshotRules); // the rules were edited while we were not running
        if (pass == Pass::Full)
            m_rulesBefore.reset(); // every volume is read with the current rules
    }
    if (listChanged)
        QMetaObject::invokeMethod(this, [this] { emit volumesChanged(); }, Qt::QueuedConnection);
    ensureWatcher(); // watch before walking so nothing slips through

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
                if (journalInfo[i])
                    journals[i] = {journalInfo[i]->id, journalInfo[i]->nextUsn};
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
    if (!toSync.empty() && !syncWithDisk(stop, toSync))
        return; // stopped
    for (const std::size_t i : toSync) {
        if (journals[i].journalId != 0)
            startFollowing(i); // replays the changes made while the MFT was read
    }
    followJournals(stop, followed, std::move(walkLater));
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
        Probe probe;
        std::uint32_t record = 0;
    };
    std::vector<Freed> freed;
    for (const std::wstring& path : freedPaths) {
        if (crawler.isExcludedPath(path))
            continue; // still left out by another rule
        Freed f {path, probe(path)};
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

bool IndexService::syncWithDisk(std::stop_token stop, const std::vector<std::size_t>& which)
{
    const auto index = m_index.load();
    const auto crawler = this->crawler();
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
        auto lock = index->writeLock();
        // Drop drives that are gone; add new ones.
        const std::vector<EntryId> existingRoots = index->roots();
        for (const EntryId r : existingRoots) {
            const std::string_view name = index->name(r);
            const bool keep = std::any_of(
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

    bool completed = true;
    bool readMft = false;
    std::vector<Crawler::Root> walks;
    for (const std::size_t i : which) {
        const VolumeInfo& v = volumes[i];
        if (journals[i].journalId == 0) {
            walks.push_back({roots[i], v.root, 0});
            continue;
        }
        {
            auto lock = index->writeLock();
            index->setFolderRecord(roots[i], ntfs::kRootRecord, roots[i]);
        }
        if (syncFromMft(v, roots[i], *crawler, !firstBuild, stop)) {
            readMft = true;
            continue;
        }
        if (stop.stop_requested()) {
            completed = false;
            break;
        }
        // Walking works too, a few times slower; it records the folder numbers as well.
        walks.push_back({roots[i], v.root, 0, roots[i], ntfs::kRootRecord});
    }
    if (completed && !walks.empty())
        completed = crawler->sync(*index, std::move(walks), crawlThreads(), !firstBuild, stop);

    {
        auto lock = index->writeLock();
        index->setInterning(false); // frees the lookup table
    }
    {
        std::lock_guard lock(m_journalMutex);
        applyChanges(*index, m_journal, *crawler); // replay what changed on watched volumes during the walk
        m_journal = decltype(m_journal)(); // releases the capacity, unlike `= {}`
        m_journaling = false;
        for (const std::size_t i : which) {
            if (journals[i].journalId == 0) {
                // A walked volume: its walk is no longer owed, or still is.
                const std::wstring& root = volumes[i].root;
                std::erase(m_rewalk, root);
                if (!completed)
                    m_rewalk.push_back(root);
            } else if (!completed && i < m_journals.size()) {
                m_journals[i] = {}; // read only in part: read again next time, not caught up
            }
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
    compactIfWasteful(readMft);
    ::HeapCompact(::GetProcessHeap(), 0); // hand scratch memory back to the OS
    m_dirty = true;
    setState(State::Ready);
    saveSnapshot();
    return true;
}

bool IndexService::syncFromMft(
    const VolumeInfo& volume, EntryId root, const Crawler& crawler, bool lowPriority, std::stop_token stop)
{
    const auto index = m_index.load();
    if (lowPriority)
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    MftTree tree;
    std::wstring error;
    const bool read = tree.read(volume.root, *index, stop, &error);
    if (lowPriority)
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    if (!read) {
        qWarning("QuickFind: cannot read the MFT of %ls: %ls", volume.root.c_str(), error.c_str());
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
    f->reader = std::make_unique<ntfs::JournalReader>(v.root, position.journalId);
    f->applier = std::make_unique<UsnApplier>(*f->index, *f->crawler, toUtf8(v.root));
    return f->reader->valid() ? std::move(f) : nullptr;
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
    const auto journal = ntfs::queryJournal(v.root);
    {
        std::lock_guard lock(m_journalMutex);
        m_journals[volume] = journal ? JournalPosition {journal->id, journal->nextUsn} : JournalPosition {};
    }
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
    std::vector<std::wstring> roots;
    {
        std::lock_guard lock(m_journalMutex);
        for (std::size_t i = 0; i < m_volumes.size(); ++i) {
            if ((i >= m_journals.size() || m_journals[i].journalId == 0) && !contains(m_suspended, m_volumes[i].root))
                roots.push_back(m_volumes[i].root);
        }
    }
    if (!m_watcher) {
        if (roots.empty())
            return;
        m_watcher = std::make_unique<ChangeWatcher>(
            [this](std::vector<FsChange>&& changes) { onChanges(std::move(changes)); });
    }
    // Only the volumes that change: the others stay watched without a gap.
    if (m_watcher->roots() != roots)
        m_watcher->setRoots(std::move(roots));
}

void IndexService::onChanges(std::vector<FsChange>&& changes)
{
    const auto crawler = this->crawler();
    const bool overflow = std::any_of(
        changes.begin(), changes.end(), [](const FsChange& c) { return c.kind == FsChange::Kind::Overflow; });
    std::erase_if(changes,
        [&](const FsChange& c) { return c.kind == FsChange::Kind::Overflow || crawler->isExcludedPath(c.path); });

    std::lock_guard lock(m_journalMutex);
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
    applyChanges(*m_index.load(), changes, *crawler);
    m_dirty = true;
}

void IndexService::applyChanges(FileIndex& index, const std::vector<FsChange>& changes, const Crawler& crawler)
{
    using Kind = FsChange::Kind;
    std::size_t i = 0;
    while (i < changes.size()) {
        std::size_t end = std::min(changes.size(), i + kApplyBatch);
        if (end < changes.size() && changes[end - 1].kind == Kind::RenamedFrom)
            ++end; // keep rename pairs in one batch

        // File system probes happen before taking the lock.
        std::vector<Probe> probes(end - i);
        for (std::size_t k = i; k < end; ++k) {
            if (changes[k].kind == Kind::Added || changes[k].kind == Kind::RenamedTo)
                probes[k - i] = probe(changes[k].path);
        }

        std::vector<Crawler::Root> newDirs;
        {
            auto lock = index.writeLock();

            const auto removePath = [&](const std::wstring& path) {
                const EntryId id = index.pathForUpdate(path);
                if (id != kNoEntry && !(index.entry(id).flags & EntryFlag::Root))
                    index.remove(id);
            };

            const auto addPath = [&](const std::wstring& path, const Probe& p) {
                if (p.attributes == INVALID_FILE_ATTRIBUTES)
                    return; // already gone again
                for (const auto& dir : newDirs) {
                    if (isUnder(path, dir.path))
                        return; // the subtree walk below picks it up
                }
                const auto [parentPath, name] = splitPath(path);
                const EntryId parent = index.pathForUpdate(parentPath);
                if (parent == kNoEntry || name.empty())
                    return;
                const std::string utf8 = toUtf8(name);
                if (index.childForUpdate(parent, utf8, false) != kNoEntry)
                    return;
                auto flags = static_cast<std::uint8_t>(
                    Crawler::attributeFlags(p.attributes) | (index.entry(parent).flags & EntryFlag::Inherited));
                if (flags & EntryFlag::Directory)
                    flags |= crawler.dirPriorityFlags(path, name);
                const EntryId id = index.add(parent, utf8, flags);
                if (Crawler::shouldDescend(p.attributes, p.reparseTag))
                    newDirs.push_back({id, path, static_cast<std::uint8_t>(flags & EntryFlag::Inherited)});
            };

            const auto movePath = [&](const std::wstring& from, const std::wstring& to) {
                const EntryId id = index.pathForUpdate(from);
                if (id == kNoEntry)
                    return false;
                const auto [parentPath, name] = splitPath(to);
                const EntryId parent = index.pathForUpdate(parentPath);
                if (parent == kNoEntry || name.empty())
                    return false;
                const std::string utf8 = toUtf8(name);
                const EntryId existing = index.childForUpdate(parent, utf8, false);
                if (existing != kNoEntry && existing != id)
                    index.remove(existing); // replaced an existing item
                return index.move(id, parent, utf8);
            };

            for (std::size_t k = i; k < end; ++k) {
                const FsChange& c = changes[k];
                switch (c.kind) {
                case Kind::Removed:
                    removePath(c.path);
                    break;
                case Kind::RenamedFrom:
                    if (k + 1 < end && changes[k + 1].kind == Kind::RenamedTo
                        && movePath(c.path, changes[k + 1].path)) {
                        ++k;
                        break;
                    }
                    removePath(c.path);
                    break;
                case Kind::Added:
                case Kind::RenamedTo:
                    addPath(c.path, probes[k - i]);
                    break;
                case Kind::Overflow:
                    break;
                }
            }
        }

        if (!newDirs.empty())
            crawler.sync(index, std::move(newDirs), 1, true, m_shutdown.get_token());
        i = end;
    }
}

void IndexService::saveSnapshot()
{
    std::lock_guard saveLock(m_saveMutex);
    if (state() != State::Ready)
        return; // never persist a half-built index
    const auto index = m_index.load();
    std::vector<VolumeInfo> volumes;
    std::vector<JournalPosition> journals;
    CrawlRules rules;
    {
        std::lock_guard lock(m_journalMutex);
        if (m_journaling)
            return; // a sync is under way; it saves when it is done
        volumes = m_volumes;
        journals = m_journals;
        rules = m_rulesBefore ? *m_rulesBefore : crawler()->rules(); // what the index reflects
    }
    m_dirty = false;
    bool saved = false;
    {
        // Streams through a 1 MB buffer into the OS file cache; searches keep
        // running meanwhile (they only need the read lock too). Changes
        // applied after the journal positions were taken are replayed on the
        // next start, which is harmless.
        auto lock = index->readLock();
        saved = snapshot::save(*index, volumes, journals, rules, m_options.snapshotPath);
    }
    if (!saved) {
        m_dirty = true;
        qWarning("QuickFind: cannot save the index to %ls", qUtf16Printable(m_options.snapshotPath));
    }
}

void IndexService::saveInBackground()
{
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
    // locks: a disk walk (journaling) and live changes, which may walk new folders.
    std::lock_guard journalLock(m_journalMutex);
    if (m_journaling)
        return;
    const auto index = m_index.load();
    auto lock = index->writeLock();
    const std::size_t live = index->liveCount();
    // Worth a pause in searches (~30 ms per million items) only once it frees a few MB.
    if (always || index->slotCount() - live >= std::max(kMinCompactSlots, live / 16))
        index->compact(always);
}

void IndexService::setState(State state)
{
    if (m_state.exchange(state) != state)
        notifyLater();
}

void IndexService::setRefreshing(bool refreshing)
{
    if (m_refreshing.exchange(refreshing) != refreshing)
        notifyLater();
}

void IndexService::notifyLater()
{
    QMetaObject::invokeMethod(this, [this] { emit stateChanged(); }, Qt::QueuedConnection);
}

} // namespace qf

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
    HANDLE h
        = ::FindFirstFileExW(win32::longPath(path).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        p.attributes = fd.dwFileAttributes;
        p.reparseTag = fd.dwReserved0;
        ::FindClose(h);
    }
    return p;
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

} // namespace

struct IndexService::Followed {
    std::size_t volume = 0;
    std::int64_t usn = 0; // the next record to read
    std::unique_ptr<ntfs::JournalReader> reader;
    std::unique_ptr<UsnApplier> applier;
    std::vector<ntfs::UsnRecord> records; // of the last read
    bool reading = false;
    std::chrono::steady_clock::time_point resumeAt {}; // the next read starts then
};

IndexService::IndexService(Options options, QObject* parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_crawler(std::make_unique<Crawler>(m_options.rules))
{
    m_index.store(std::make_shared<FileIndex>());

    m_saveTimer.setInterval(10min);
    connect(&m_saveTimer, &QTimer::timeout, this, &IndexService::saveInBackground);

    m_resyncTimer.setSingleShot(true);
    m_resyncTimer.setInterval(3s);
    connect(&m_resyncTimer, &QTimer::timeout, this, [this] {
        if (!m_stopped)
            launch(Pass::Walked); // a watcher lost events
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
    const bool changed = !(options.rules == m_options.rules) || options.includeRemovable != m_options.includeRemovable;
    if (!changed || m_stopped)
        return;
    // Stop everything that reads the options before replacing them.
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
    m_watcher.reset();
    m_options.rules = std::move(options.rules);
    m_options.includeRemovable = options.includeRemovable;
    m_crawler = std::make_unique<Crawler>(m_options.rules);
    launch(Pass::Full);
}

void IndexService::shutdown()
{
    if (m_stopped)
        return;
    m_stopped = true;
    m_saveTimer.stop();
    m_resyncTimer.stop();
    m_shutdown.request_stop();
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
    m_watcher.reset();
    if (m_pendingSave.valid())
        m_pendingSave.wait();
    if (m_dirty)
        saveSnapshot();
}

void IndexService::launch(Pass pass)
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
    m_worker = std::jthread([this, pass](std::stop_token stop) { run(stop, pass); });
}

void IndexService::run(std::stop_token stop, Pass pass)
{
    const auto volumes = listLocalVolumes(m_options.includeRemovable);
    std::vector<JournalPosition> journals(volumes.size());
    bool loaded = false;
    if (pass == Pass::Startup) {
        setState(State::Loading);
        if (auto snapshot = snapshot::load(m_options.snapshotPath, volumes, &journals)) {
            m_index.store(std::shared_ptr<FileIndex>(std::move(snapshot)));
            loaded = true;
            setState(State::Ready);
        } else {
            journals.assign(volumes.size(), {});
            setState(State::Building);
        }
    } else {
        std::lock_guard lock(m_journalMutex);
        if (m_volumes == volumes)
            journals = m_journals;
    }

    // NTFS volumes follow their change journal from where the index left
    // off. When the journal no longer reaches back that far, the MFT is read
    // again; the journal then replays what changed during the read.
    std::vector<std::size_t> toSync;
    std::vector<std::size_t> walkLater;
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        const auto journal = volumes[i].ntfs ? ntfs::queryJournal(volumes[i].root) : std::nullopt;
        if (journal) {
            const JournalPosition& p = journals[i];
            const bool current = p.journalId == journal->id && p.usn >= journal->firstUsn && p.usn <= journal->nextUsn;
            if (pass == Pass::Full || !current) {
                journals[i] = {journal->id, journal->nextUsn};
                toSync.push_back(i);
            }
        } else {
            journals[i] = {};
            if (pass != Pass::Startup || !loaded)
                toSync.push_back(i);
            else if (m_options.rescanOnStartup)
                walkLater.push_back(i); // let login settle first
        }
    }
    {
        std::lock_guard lock(m_journalMutex);
        m_volumes = volumes;
        m_journals = journals;
    }
    ensureWatcher(); // watch before walking so nothing slips through

    // Catch up where the snapshot left off (quick), then read what must be read.
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
        if (journals[i].journalId != 0 && std::find(toSync.begin(), toSync.end(), i) == toSync.end())
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

bool IndexService::syncWithDisk(std::stop_token stop, const std::vector<std::size_t>& which)
{
    const auto index = m_index.load();
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
        if (syncFromMft(v, roots[i], !firstBuild, stop)) {
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
        completed = m_crawler->sync(*index, std::move(walks), crawlThreads(), !firstBuild, stop);

    {
        auto lock = index->writeLock();
        index->setInterning(false); // frees the lookup table
    }
    {
        std::lock_guard lock(m_journalMutex);
        applyChanges(*index, m_journal); // replay what changed on watched volumes during the walk
        m_journal = decltype(m_journal)(); // releases the capacity, unlike `= {}`
        m_journaling = false;
        if (!completed) {
            // A volume read only in part is read again next time, not caught up.
            for (const std::size_t i : which) {
                if (i < m_journals.size())
                    m_journals[i] = {};
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

bool IndexService::syncFromMft(const VolumeInfo& volume, EntryId root, bool lowPriority, std::stop_token stop)
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
    return m_crawler->sync(*index, {{root, volume.root, 0, root, ntfs::kRootRecord}}, 1, lowPriority, stop,
        [&](const Crawler::Root& dir, DirListing& out) { return tree.list(*index, *m_crawler, dir, out); });
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
    f->reader = std::make_unique<ntfs::JournalReader>(v.root, position.journalId);
    f->applier = std::make_unique<UsnApplier>(*m_index.load(), *m_crawler, toUtf8(v.root));
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
            if (i >= m_journals.size() || m_journals[i].journalId == 0)
                roots.push_back(m_volumes[i].root);
        }
    }
    if (m_watcher && m_watcher->roots() == roots)
        return;
    m_watcher.reset();
    if (!roots.empty()) {
        m_watcher = std::make_unique<ChangeWatcher>(
            std::move(roots), [this](std::vector<FsChange>&& changes) { onChanges(std::move(changes)); });
    }
}

void IndexService::onChanges(std::vector<FsChange>&& changes)
{
    const bool overflow = std::any_of(
        changes.begin(), changes.end(), [](const FsChange& c) { return c.kind == FsChange::Kind::Overflow; });
    std::erase_if(changes,
        [this](const FsChange& c) { return c.kind == FsChange::Kind::Overflow || m_crawler->isExcludedPath(c.path); });

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
    applyChanges(*m_index.load(), changes);
    m_dirty = true;
}

void IndexService::applyChanges(FileIndex& index, const std::vector<FsChange>& changes)
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
                const EntryId id = index.findPath(path);
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
                const EntryId parent = index.findPath(parentPath);
                if (parent == kNoEntry || name.empty())
                    return;
                const std::string utf8 = toUtf8(name);
                if (index.findChild(parent, utf8) != kNoEntry)
                    return;
                auto flags = static_cast<std::uint8_t>(
                    Crawler::attributeFlags(p.attributes) | (index.entry(parent).flags & EntryFlag::Inherited));
                if (flags & EntryFlag::Directory)
                    flags |= m_crawler->dirPriorityFlags(path, name);
                const EntryId id = index.add(parent, utf8, flags);
                if (Crawler::shouldDescend(p.attributes, p.reparseTag))
                    newDirs.push_back({id, path, static_cast<std::uint8_t>(flags & EntryFlag::Inherited)});
            };

            const auto movePath = [&](const std::wstring& from, const std::wstring& to) {
                const EntryId id = index.findPath(from);
                if (id == kNoEntry)
                    return false;
                const auto [parentPath, name] = splitPath(to);
                const EntryId parent = index.findPath(parentPath);
                if (parent == kNoEntry || name.empty())
                    return false;
                const std::string utf8 = toUtf8(name);
                const EntryId existing = index.findChild(parent, utf8);
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
            m_crawler->sync(index, std::move(newDirs), 1, true, m_shutdown.get_token());
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
    {
        std::lock_guard lock(m_journalMutex);
        if (m_journaling)
            return; // a sync is under way; it saves when it is done
        volumes = m_volumes;
        journals = m_journals;
    }
    m_dirty = false;
    bool saved = false;
    {
        // Streams through a 1 MB buffer into the OS file cache; searches keep
        // running meanwhile (they only need the read lock too). Changes
        // applied after the journal positions were taken are replayed on the
        // next start, which is harmless.
        auto lock = index->readLock();
        saved = snapshot::save(*index, volumes, journals, m_options.snapshotPath);
    }
    if (!saved)
        m_dirty = true;
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

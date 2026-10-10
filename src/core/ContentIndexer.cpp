#include "ContentIndexer.h"

#include "ContentScanner.h"
#include "Crawler.h"
#include "Documents.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <windows.h>

#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <limits>

using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr auto kRetryDelay = 1min; // after a pass was cut short
constexpr auto kLookAround = 250ms; // a pass looks at merges due, the power and the user this often
constexpr std::size_t kMergeAfter = 1024; // files read (by passes or searches) before the segments become one
constexpr int kHardDiskReads = 2; // files read from one hard disk at once: more would make its heads jump about
constexpr DWORD kAwayAfterMs = 2 * 60 * 1000;
constexpr std::size_t kChunkBytes = ContentScanner::kChunkBytes;

// How many files are read at once. Opening one costs the antivirus several
// milliseconds of processor time (measured with Defender: about 7 ms), in its
// own process and at its own priority, so this is about how many processors
// a pass takes: a quarter of them while someone uses the computer, half
// while nobody does. Here (32 logical processors) 8 read 1200 files a second
// and 16 read 2300.
int readers(bool away)
{
    const int processors = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return away ? std::clamp(processors / 2, 4, 16) : std::clamp(processors / 4, 2, 8);
}


bool batterySaverOn()
{
    SYSTEM_POWER_STATUS status {};
    return ::GetSystemPowerStatus(&status) && status.SystemStatusFlag == 1;
}

// Nobody has used the keyboard or mouse for a while, nothing runs full screen
// (a film, a game, slides) and the computer is on mains power: files may be
// read faster. Most of what a read costs is the antivirus scanning the file,
// in its own process, which the indexer's background priority does not reach.
bool userAway()
{
    LASTINPUTINFO input {sizeof input, 0};
    SYSTEM_POWER_STATUS power {};
    QUERY_USER_NOTIFICATION_STATE state {};
    return ::GetLastInputInfo(&input) && ::GetTickCount() - input.dwTime >= kAwayAfterMs
        && ::GetSystemPowerStatus(&power) && power.ACLineStatus == 1
        && SUCCEEDED(::SHQueryUserNotificationState(&state))
        && (state == QUNS_NOT_PRESENT || state == QUNS_ACCEPTS_NOTIFICATIONS || state == QUNS_QUIET_TIME);
}

// A file's size and stamp (ContentIndex::stampOf), from its attributes.
struct FileStamp {
    std::uint64_t size;
    std::uint32_t stamp;
};
std::optional<FileStamp> statOf(std::wstring_view path)
{
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!::GetFileAttributesExW(win32::longPath(path).c_str(), GetFileExInfoStandard, &data))
        return std::nullopt;
    const std::int64_t size = (static_cast<std::int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    const std::int64_t time
        = (static_cast<std::int64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return FileStamp {static_cast<std::uint64_t>(size), ContentIndex::stampOf(size, time)};
}

// Documents that are copies of others, the same bytes ("report (1).pdf", a
// folder copied as a backup): the first is read, and the others get its
// document (ContentIndex::addCopy). Here four documents in ten (2668 of
// 5875, 4.2 of 10.6 GB), and about as much of the time reading documents
// takes. Only files of a size another document has are compared, by a hash
// of all their bytes, which reading takes a fraction of the time reading
// their text out does. A copy found while its original is read waits for it.
class Copies {
public:
    struct Original {
        EntryId entry;
        std::uint32_t stamp;
    };

    // Before reading the document of `entry`, whose file has `size` bytes
    // and `stamp`: the document of the same bytes read before, if there is
    // one; otherwise it is read. Either way, done() follows.
    std::optional<Original> find(EntryId entry, std::wstring path, std::uint64_t size, std::uint32_t stamp,
        const std::function<bool()>& cancelled)
    {
        const auto doc = std::make_shared<Doc>(entry, std::move(path), stamp);
        std::vector<std::shared_ptr<Doc>> earlier;
        {
            std::lock_guard lock(m_mutex);
            std::vector<std::shared_ptr<Doc>>& same = m_bySize[size];
            earlier = same;
            same.push_back(doc);
            m_reading.emplace(entry, doc);
        }
        if (earlier.empty())
            return std::nullopt;
        const std::optional<Digest> mine = digestOf(*doc, cancelled);
        if (!mine)
            return std::nullopt;
        for (const auto& other : earlier) {
            if (digestOf(*other, cancelled) != mine)
                continue;
            // Only on those before it: none waits for one that waits for it.
            std::unique_lock lock(m_mutex);
            while (other->state == State::Reading && !cancelled())
                m_done.wait_for(lock, 100ms);
            if (other->state == State::Added)
                return Original {other->entry, other->stamp};
        }
        return std::nullopt;
    }

    // The document of `entry` is in the index (`added`), or was not read.
    void done(EntryId entry, bool added)
    {
        {
            std::lock_guard lock(m_mutex);
            const auto it = m_reading.find(entry);
            if (it == m_reading.end())
                return;
            it->second->state = added ? State::Added : State::Failed;
            m_reading.erase(it);
        }
        m_done.notify_all();
    }

private:
    using Digest = std::array<UCHAR, 32>; // SHA-256
    enum class State : std::uint8_t { Reading, Added, Failed };
    struct Doc {
        Doc(EntryId e, std::wstring p, std::uint32_t s)
            : entry(e)
            , path(std::move(p))
            , stamp(s)
        {
        }
        const EntryId entry;
        const std::wstring path;
        const std::uint32_t stamp;
        State state = State::Reading; // under m_mutex
        std::once_flag hashed;
        std::optional<Digest> digest; // none if the file could not be read, or no longer has `stamp`
    };

    // Hashed once, by whichever thread needs it first.
    static std::optional<Digest> digestOf(Doc& doc, const std::function<bool()>& cancelled)
    {
        std::call_once(doc.hashed, [&] { doc.digest = hash(doc.path, doc.stamp, cancelled); });
        return doc.digest;
    }

    static std::optional<Digest> hash(
        const std::wstring& path, std::uint32_t stamp, const std::function<bool()>& cancelled)
    {
        win32::UniqueHandle file(::CreateFileW(win32::longPath(path).c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        FILE_BASIC_INFO basic {};
        LARGE_INTEGER size {};
        if (!file.valid() || !::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof basic)
            || !::GetFileSizeEx(file.get(), &size)
            || ContentIndex::stampOf(size.QuadPart, basic.LastWriteTime.QuadPart) != stamp)
            return std::nullopt;
        FILE_IO_PRIORITY_HINT_INFO hint {};
        hint.PriorityHint = IoPriorityHintLow;
        ::SetFileInformationByHandle(file.get(), FileIoPriorityHintInfo, &hint, sizeof hint);
        BCRYPT_HASH_HANDLE hashing = nullptr;
        if (!BCRYPT_SUCCESS(::BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hashing, nullptr, 0, nullptr, 0, 0)))
            return std::nullopt;
        const struct Destroy {
            BCRYPT_HASH_HANDLE h;
            ~Destroy() { ::BCryptDestroyHash(h); }
        } destroy {hashing};
        thread_local std::vector<UCHAR> buffer;
        buffer.resize(std::size_t {1} << 20);
        for (;;) {
            DWORD got = 0;
            if (cancelled() || !::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr))
                return std::nullopt;
            if (got == 0)
                break;
            if (!BCRYPT_SUCCESS(::BCryptHashData(hashing, buffer.data(), got, 0)))
                return std::nullopt;
        }
        Digest digest {};
        if (!BCRYPT_SUCCESS(::BCryptFinishHash(hashing, digest.data(), static_cast<ULONG>(digest.size()), 0)))
            return std::nullopt;
        return digest;
    }

    std::mutex m_mutex;
    std::condition_variable m_done; // a document was read
    std::unordered_map<std::uint64_t, std::vector<std::shared_ptr<Doc>>> m_bySize; // documents found, by size
    std::unordered_map<EntryId, std::shared_ptr<Doc>> m_reading; // ... that are being read
};

// The volume root an entry is on, and whether it is in the user's profile
// folder, with a small cache by parent folder (files next to each other
// share it).
class RootFinder {
public:
    RootFinder(const FileIndex& index, EntryId profile)
        : m_index(index)
        , m_profile(profile)
    {
        m_cache.fill({kNoEntry, kNoEntry, false});
    }
    struct Place {
        EntryId root;
        bool inProfile;
    };
    Place placeOf(EntryId id)
    {
        const EntryId parent = m_index.entry(id).parent;
        if (parent == kNoEntry)
            return {id, false};
        Slot& slot = m_cache[parent & (m_cache.size() - 1)];
        if (slot.parent != parent) {
            EntryId cur = parent;
            bool inProfile = m_profile != kNoEntry && cur == m_profile;
            while (m_index.entry(cur).parent != kNoEntry) {
                cur = m_index.entry(cur).parent;
                inProfile = inProfile || (m_profile != kNoEntry && cur == m_profile);
            }
            slot = {parent, cur, inProfile};
        }
        return {slot.root, slot.inProfile};
    }

private:
    struct Slot {
        EntryId parent;
        EntryId root;
        bool inProfile;
    };
    const FileIndex& m_index;
    EntryId m_profile;
    std::array<Slot, 4096> m_cache {};
};

} // namespace

ContentIndexer::ContentIndexer(std::shared_ptr<ContentIndex> content, Source source, Options options, Timing timing)
    : m_content(std::move(content))
    , m_source(std::move(source))
    , m_timing(timing)
    , m_options(std::move(options))
{
    m_content->setChangeListener([this] { wake(); });
    m_thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

ContentIndexer::~ContentIndexer()
{
    stop();
}

void ContentIndexer::stop()
{
    if (!m_thread.joinable())
        return;
    m_content->setChangeListener({});
    m_thread.request_stop();
    m_cv.notify_all();
    m_thread.join();
}

void ContentIndexer::setOptions(Options options)
{
    {
        // Turned off, the index is cleared: no search hands a file over meanwhile.
        std::unique_lock gate(m_intakeGate, std::defer_lock);
        if (!options.enabled)
            gate.lock();
        std::lock_guard lock(m_mutex);
        if (options == m_options)
            return;
        for (std::size_t k = 0; k < ContentSizeLimits::kKinds; ++k) {
            if (options.sizeLimits.bytes[k] > m_options.sizeLimits.bytes[k]) {
                m_content->markEmptyChanged(); // files that were too large may fit now
                break;
            }
        }
        if (!options.enabled)
            m_content->clear();
        m_options = std::move(options);
        m_optionsChanged = true;
    }
    m_cv.notify_all();
}

bool ContentIndexer::enabled()
{
    std::lock_guard lock(m_mutex);
    return m_options.enabled;
}

void ContentIndexer::wake()
{
    {
        std::lock_guard lock(m_mutex);
        m_woken = true;
    }
    m_cv.notify_all();
}

void ContentIndexer::interrupt()
{
    std::unique_lock lock(m_mutex);
    if (!m_passRunning)
        return;
    m_interrupt = true;
    m_cv.notify_all();
    m_cv.wait(lock, [this] { return !m_passRunning; });
}

void ContentIndexer::pause()
{
    {
        std::unique_lock lock(m_mutex);
        m_paused = true;
        m_interrupt = true; // the next pass, after resume(), clears it
        m_cv.notify_all();
        m_cv.wait(lock, [this] { return !m_passRunning; });
    }
    // A search handing a file over finishes that; the ones after see m_paused.
    std::unique_lock gate(m_intakeGate);
}

void ContentIndexer::resume()
{
    {
        std::lock_guard lock(m_mutex);
        m_paused = false;
    }
    m_cv.notify_all();
}

bool ContentIndexer::interrupted(std::stop_token stop)
{
    if (stop.stop_requested() || m_interrupt.load())
        return true;
    std::lock_guard lock(m_mutex);
    return m_optionsChanged;
}

void ContentIndexer::run(std::stop_token stop)
{
    // Not background mode: it puts the thread's disk requests at very low
    // priority, and the antivirus's scan of each file opened goes with them.
    // Measured here, reading files in background mode: 4 threads 141 files a
    // second, 16 threads 183; without it, 683 and 2105. A lower processor
    // priority made it slower too (the scans keep the processors busy). What
    // a pass costs is held down by how many files it reads at once (readers).
    const auto started = std::chrono::steady_clock::now();
    {
        // The first pass: once the file index is complete (setState wakes
        // this), and on later runs once logging on has settled. The first
        // run has everything still to read, and someone waiting for it.
        std::unique_lock lock(m_mutex);
        while (!m_source.ready()) {
            m_woken = false;
            if (!m_cv.wait(lock, stop, [this] { return m_woken; }))
                return;
        }
        lock.unlock();
        const bool firstRun = m_content->stats().documents == 0;
        lock.lock();
        if (!firstRun)
            m_cv.wait_until(lock, stop, started + m_timing.start, [] { return false; });
    }
    if (stop.stop_requested())
        return;
    for (;;) {
        Options options;
        {
            std::unique_lock lock(m_mutex);
            if (!m_cv.wait(lock, stop, [this] { return !m_paused; }))
                return; // stopped while paused
            options = m_options;
            m_optionsChanged = false;
            m_woken = false;
            m_interrupt = false;
            m_passRunning = true;
        }
        bool finished = true;
        if (options.enabled && m_source.ready() && !batterySaverOn())
            finished = pass(options, stop);
        {
            std::lock_guard lock(m_mutex);
            m_passRunning = false;
        }
        m_cv.notify_all(); // interrupt() waits for this
        if (stop.stop_requested())
            return;

        std::unique_lock lock(m_mutex);
        if (!finished) {
            m_cv.wait_for(lock, stop, kRetryDelay, [this] { return m_optionsChanged; });
            continue;
        }
        m_cv.wait_for(lock, stop, m_timing.interval, [this] { return m_woken || m_optionsChanged; });
        if (m_woken && !m_optionsChanged) // let more changes gather
            m_cv.wait_for(lock, stop, m_timing.gather, [this] { return m_optionsChanged; });
        if (stop.stop_requested())
            return;
    }
}

// The files to read or check: candidates of a content search on a followed
// volume whose document is missing, dirty or unsure. Documents of files that
// are no longer candidates are retired on the way; those on a suspended
// volume are left as they are. In the order they are best read: files read
// before (someone is working on them), then the user's own files, the others,
// hidden ones; each kind as the index has them (by folder, mostly).
std::vector<ContentIndexer::Work> ContentIndexer::findWork(
    const FileIndex& index, const Options& options, const std::vector<Volume>& volumes)
{
    const ContentFilter filter {
        ExtensionFilter(options.documents ? options.extensions + documentExtensions() : options.extensions),
        options.includeLowPriority};
    const ExtensionFilter documents(documentExtensions()); // read as readFile() reads them
    struct Root {
        EntryId entry;
        std::size_t volume;
        bool suspended;
    };
    std::vector<Root> roots;
    for (const EntryId r : index.roots()) {
        const auto it = std::find_if(
            volumes.begin(), volumes.end(), [&](const Volume& v) { return v.root == index.name(r); });
        if (it != volumes.end())
            roots.push_back({r, static_cast<std::size_t>(it - volumes.begin()), it->suspended});
    }
    const std::vector<ContentIndex::DocInfo> docs = m_content->documents();
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_recentReads, [&](const auto& r) { return now - r.second >= m_timing.cooldown; });

    std::vector<Work> work;
    std::vector<EntryId> retired;
    RootFinder finder(index, index.findPath(win32::expandEnvironment(L"%USERPROFILE%")));
    std::size_t k = 0;
    for (std::size_t c = 0; c < index.chunkCount(); ++c) {
        const auto entries = index.chunk(c);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            if (!filter.accepts(index, e))
                continue;
            const auto id = static_cast<EntryId>((c << FileIndex::kChunkBits) + i);
            const RootFinder::Place place = finder.placeOf(id);
            const auto volume
                = std::find_if(roots.begin(), roots.end(), [&](const Root& r) { return r.entry == place.root; });
            if (volume == roots.end())
                continue;
            for (; k < docs.size() && docs[k].entry < id; ++k)
                retired.push_back(docs[k].entry);
            const bool known = k < docs.size() && docs[k].entry == id;
            if (volume->suspended) {
                k += known ? 1 : 0;
                continue;
            }
            Work w {id, false, false, 0};
            if (known) {
                const ContentIndex::DocInfo& d = docs[k++];
                if (d.need == ContentIndex::Need::Check)
                    w = {id, true, false, d.stamp};
                else if (d.need == ContentIndex::Need::Read && !m_recentReads.contains(id))
                    w = {id, false, true, 0};
                else
                    continue;
            } else if (place.inProfile && !(e.flags & EntryFlag::Hidden)) {
                w.order = 1;
            } else {
                w.order = e.flags & EntryFlag::Hidden ? 3 : 2;
            }
            w.document = documents.matches(index.name(e), e.extLength);
            w.volume = volume->volume;
            work.push_back(w);
        }
    }
    for (; k < docs.size(); ++k)
        retired.push_back(docs[k].entry);
    m_content->retire(retired);
    std::stable_sort(work.begin(), work.end(), [](const Work& a, const Work& b) { return a.order < b.order; });
    return work;
}

int ContentIndexer::documentProcesses() noexcept
{
    // Each takes a processor while it reads (at idle priority), and the
    // memory of the document it reads. Here (32 logical processors) 8 read
    // 5815 documents in 21 s, 4 in 28 s: a few large PDFs take seconds each.
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 4, 2, 8);
}

bool ContentIndexer::pass(const Options& options, std::stop_token stop)
{
    const auto index = m_source.index();
    if (!index)
        return true;
    const std::vector<Volume> volumes = m_source.volumes();
    std::vector<Work> work;
    FileIndex::IdPin pin; // the ids in `work` stay valid: no compaction until the pass is over
    {
        const auto lock = index->longReadLock(); // findWork goes through all of it
        pin = index->pinIds();
        if (index->generation() != m_generation) {
            m_recentReads.clear(); // ids from before a compaction
            m_generation = index->generation();
        }
        work = findWork(*index, options, volumes);
    }
    if (work.empty()) {
        tidy(m_taken.exchange(0)); // what searches handed over
        return true;
    }

    // The files go into queues, each with how many of its files may be read
    // at once: one queue per hard disk, the documents on the other disks, and
    // all the rest. A thread takes the next file of the first queue with
    // room, so none waits for an extractor or a hard disk while there are
    // other files to read.
    struct Queue {
        std::vector<const Work*> files;
        std::atomic<int> room = std::numeric_limits<int>::max();
        std::atomic<std::size_t> next {0};
        std::atomic<int> reading {0};
    };
    std::deque<Queue> queues; // keeps their places as more are added
    std::vector<Queue*> hardDiskQueue(volumes.size(), nullptr);
    {
        std::vector<std::pair<std::uint32_t, Queue*>> disks;
        for (std::size_t v = 0; v < volumes.size(); ++v) {
            if (volumes[v].suspended)
                continue; // not touched: it is being ejected or locked
            const VolumePlacement p = placementOf(QString::fromStdString(volumes[v].root).toStdWString());
            if (!p.seeks)
                continue;
            const auto it = std::find_if(disks.begin(), disks.end(),
                [&](const auto& d) { return p.disk != ~0u && d.first == p.disk; });
            if (it != disks.end()) {
                hardDiskQueue[v] = it->second;
                continue;
            }
            Queue& q = queues.emplace_back();
            q.room = kHardDiskReads;
            hardDiskQueue[v] = &q;
            disks.emplace_back(p.disk, &q);
        }
    }
    // Half the extractor processes while other files are read and someone
    // uses the computer (the antivirus scans documents too), all of them
    // once those are done or nobody does: documents read beside the other
    // files leave none to read at the end (wsbench --service --content).
    bool away = userAway(); // nobody at the computer (the coordinator's, from here on)
    Queue& documents = queues.emplace_back();
    documents.room = away ? documentProcesses() : std::max(1, documentProcesses() / 2);
    Queue& others = queues.emplace_back();
    for (const Work& w : work) {
        Queue* q = hardDiskQueue[w.volume];
        (q ? *q : w.document ? documents : others).files.push_back(&w);
    }
    // Two kinds of threads: readers (as many at a time as `allowed`) take
    // the files of the hard disks and the rest; a few more the documents,
    // which they wait for while an extractor process reads them, with a
    // processor of its own at idle priority.
    std::vector<Queue*> readerQueues;
    for (Queue& q : queues) {
        if (&q != &documents)
            readerQueues.push_back(&q);
    }
    const std::vector<Queue*> documentQueues {&documents};
    const int documentThreads = documents.files.empty() ? 0 : documentProcesses();
    const int readerThreads = static_cast<int>(std::min(work.size() - documents.files.size(),
        static_cast<std::size_t>(readers(true))));

    m_reading = true;
    const struct Done {
        std::atomic<bool>& flag;
        ~Done() { flag = false; }
    } done {m_reading};
    std::atomic<bool> halt {false}; // the pass is cut short
    std::atomic<bool> hold {false}; // no file is started: the index is being merged
    std::atomic<int> busy {0}; // threads between taking a file and handing it to the index
    std::atomic<int> allowed {readers(away)}; // threads that may read now (the others wait)
    std::atomic<bool> full {false};
    std::atomic<std::size_t> added {0};
    Copies copies;
    std::mutex recentMutex;
    std::mutex gateMutex;
    std::condition_variable gate; // waiting threads may go on, or one has ended
    std::condition_variable room; // a queue has room again
    const int threads = documentThreads + readerThreads;
    int running = threads; // under gateMutex
    const auto cancelled = [&] { return halt.load() || stop.stop_requested() || m_interrupt.load(); };
    const auto waitOn = [&](std::condition_variable& cv, std::chrono::milliseconds most) {
        std::unique_lock lock(gateMutex);
        cv.wait_for(lock, most);
    };

    const auto anyLeft = [](const std::vector<Queue*>& mine) {
        return std::any_of(mine.begin(), mine.end(), [](const Queue* q) { return q->next.load() < q->files.size(); });
    };
    // The next file of the first of `mine` with room for it; null when there
    // is none at the moment, and `left` false when there are no files left.
    const auto take = [](const std::vector<Queue*>& mine, Queue*& from, bool& left) -> const Work* {
        left = false;
        for (Queue* queue : mine) {
            Queue& q = *queue;
            if (q.next.load() >= q.files.size())
                continue;
            left = true;
            if (q.reading.fetch_add(1) >= q.room) {
                q.reading.fetch_sub(1);
                continue;
            }
            if (const std::size_t i = q.next.fetch_add(1); i < q.files.size()) {
                from = &q;
                return q.files[i];
            }
            q.reading.fetch_sub(1);
        }
        return nullptr;
    };
    // Reads one file into the index; false when the index has no room left.
    const auto read = [&](const Work& w, std::wstring& path) {
        if (m_content->knows(w.entry))
            return true; // a search read it meanwhile (Intake)
        {
            const auto lock = index->readLock();
            if (index->entry(w.entry).isDeleted())
                return true;
            path = index->wpath(w.entry);
        }
        if (w.checkOnly) {
            const auto stamp = stampOf(path);
            if (!stamp)
                return true; // gone or busy: the next pass sees
            if (*stamp == w.stamp) {
                m_content->confirm(w.entry);
                return true;
            }
        }
        const std::uint64_t since = m_content->changeSequence();
        const std::int64_t maxBytes = options.sizeLimits.of(path);
        // A document may be a copy of one read before.
        struct Report {
            Copies& copies;
            EntryId entry;
            bool added = false;
            ~Report() { copies.done(entry, added); }
        } report {copies, w.entry};
        const std::optional<FileStamp> file = w.document ? statOf(path) : std::nullopt;
        const std::optional<Copies::Original> original
            = file && file->size > 0 && static_cast<std::int64_t>(file->size) <= maxBytes
            ? copies.find(w.entry, path, file->size, file->stamp, cancelled)
            : std::nullopt;
        if (original && m_content->addCopy(w.entry, original->entry, original->stamp, file->stamp, since)) {
            ++m_copies;
        } else {
            const FileText text = readFile(path, maxBytes, cancelled, &m_documents);
            if (text.outcome == Outcome::Skipped)
                return true;
            if (!m_content->add(w.entry, text.keys, text.outcome == Outcome::Empty, text.stamp, since, text.text))
                return false;
        }
        report.added = true;
        ++added;
        if (w.again) {
            std::lock_guard lock(recentMutex);
            m_recentReads[w.entry] = std::chrono::steady_clock::now();
        }
        return true;
    };
    const auto worker = [&](int t) {
        const bool forDocuments = t < documentThreads;
        const std::vector<Queue*>& mine = forDocuments ? documentQueues : readerQueues;
        std::wstring path;
        while (!cancelled() && !full.load()) {
            if (hold.load() || (!forDocuments && t - documentThreads >= allowed.load())) {
                if (!anyLeft(mine))
                    break; // the threads that may read took them all
                waitOn(gate, 1s);
                continue;
            }
            if (m_content->searching()) {
                waitOn(gate, 100ms); // it opens files meanwhile
                continue;
            }
            // Announced before looking at `hold`: a merge waits for it.
            busy.fetch_add(1);
            Queue* from = nullptr;
            bool left = true;
            const Work* w = hold.load() ? nullptr : take(mine, from, left);
            if (!w) {
                busy.fetch_sub(1);
                if (!left)
                    break; // all taken
                waitOn(room, 100ms); // the files left wait for a hard disk or an extractor
                continue;
            }
            if (!read(*w, path))
                full = true;
            from->reading.fetch_sub(1);
            busy.fetch_sub(1);
            if (from->room.load() != std::numeric_limits<int>::max())
                room.notify_one();
        }
        {
            std::lock_guard lock(gateMutex);
            --running;
        }
        gate.notify_all();
        room.notify_all();
    };

    m_content->beginReads();
    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t)
            workers.emplace_back(worker, t);
        int ticks = 0;
        std::unique_lock lock(gateMutex);
        while (!gate.wait_for(lock, kLookAround, [&] { return running == 0; })) {
            lock.unlock();
            bool letGo = false;
            if (full.load() || interrupted(stop) || batterySaverOn()) {
                halt = true;
                letGo = true;
            } else {
                if (++ticks % 8 == 0) { // whether someone is at the computer, every 2 s
                    away = userAway();
                    const int now = readers(away);
                    letGo = now > allowed.exchange(now);
                }
                // Only documents left, or nobody at the computer: every
                // extractor process.
                const int processes = away || !anyLeft(readerQueues) ? documentProcesses()
                                                                     : std::max(1, documentProcesses() / 2);
                if (processes > documents.room.exchange(processes))
                    letGo = true;
                if (!m_content->searching()) {
                    if (m_content->needsFullMerge() || m_content->needsTextCompaction()) {
                        // Merging everything (or rewriting the text file)
                        // cannot take in documents added meanwhile: the files
                        // being read are finished first, no other started.
                        hold = true;
                        while (busy.load() > 0 && !cancelled())
                            std::this_thread::sleep_for(10ms);
                        if (busy.load() == 0) {
                            if (m_content->needsMerge())
                                m_content->mergeDue(readers(away));
                            if (m_content->needsTextCompaction())
                                m_content->compactTexts();
                        }
                        hold = false;
                        letGo = true;
                    } else if (m_content->needsMerge()) {
                        // Small segments into larger ones while files are
                        // read: holding the readers for it cost the first run
                        // 14 s of 75 (most of it waiting for documents being
                        // read to finish: wsbench --service --content).
                        m_content->mergeTails();
                    }
                }
            }
            if (letGo) {
                gate.notify_all();
                room.notify_all();
            }
            lock.lock();
        }
    } // joins
    m_content->endReads();
    m_documents.closeIdle(); // their memory back now rather than later
    if (full.load() || halt.load() || interrupted(stop))
        return false;
    tidy(added.load() + m_taken.exchange(0));
    return true;
}

// After many files were read (the first time, say, or by a search that the
// index could not answer): one segment again, smaller than several, and the
// grams still in memory written out. Not while a search runs: it may hand
// files over, which a merge started before them cannot take in.
void ContentIndexer::tidy(std::size_t added)
{
    const int threads = readers(userAway()); // as many as read files
    if (m_content->needsMerge())
        m_content->mergeDue(threads);
    if (m_content->needsTextCompaction())
        m_content->compactTexts();
    if (added >= kMergeAfter) {
        const ContentIndex::Stats stats = m_content->stats();
        if (stats.segments + (stats.memoryPairs > 0 ? 1 : 0) > 1
            && (m_content->searching() || !m_content->merge(threads)))
            m_taken += added; // the next pass tries again
    }
    ::HeapCompact(::GetProcessHeap(), 0); // hand back the read buffers and gram lists
}

namespace {

// Opens a file to read it for the index, at low I/O priority, and puts its
// stamp in `out`. Invalid when it is not to be read: out.outcome is Empty
// when there is nothing to find in it, Skipped when it may be read later.
win32::UniqueHandle openText(std::wstring_view path, std::int64_t maxBytes, ContentIndexer::FileText& out)
{
    win32::UniqueHandle file(::CreateFileW(win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file.valid()) {
        // No access will not change by itself; in use or gone may (and the
        // journal reports a file that is gone).
        if (::GetLastError() == ERROR_ACCESS_DENIED)
            out.outcome = ContentIndexer::Outcome::Empty;
        return {};
    }
    FILE_BASIC_INFO basic {};
    LARGE_INTEGER size {};
    if (!::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof basic)
        || !::GetFileSizeEx(file.get(), &size))
        return {};
    constexpr DWORD kNotHere = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN
        | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if (basic.FileAttributes & kNotHere)
        return {}; // never download a cloud file
    out.stamp = ContentIndex::stampOf(size.QuadPart, basic.LastWriteTime.QuadPart);
    if (size.QuadPart <= 0 || (maxBytes > 0 && size.QuadPart > maxBytes)) {
        out.outcome = ContentIndexer::Outcome::Empty; // as ContentScanner::scanFile skips it
        return {};
    }
    FILE_IO_PRIORITY_HINT_INFO hint {};
    hint.PriorityHint = IoPriorityHintLow;
    ::SetFileInformationByHandle(file.get(), FileIoPriorityHintInfo, &hint, sizeof hint);
    return file;
}

} // namespace

ContentIndexer::FileText ContentIndexer::readFile(
    std::wstring_view path, std::int64_t maxBytes, const std::function<bool()>& cancelled, DocExtractor* documents)
{
    FileText out;
    const bool document = isDocumentPath(path);
    if (document && !documents)
        return out;
    const win32::UniqueHandle file = openText(path, maxBytes, out);
    if (!file.valid())
        return out;

    if (document) {
        LARGE_INTEGER size {};
        ::GetFileSizeEx(file.get(), &size);
        DocExtractor::Result read = documents->extract(file.get(), static_cast<std::uint64_t>(size.QuadPart), cancelled);
        if (read.retry)
            return out;
        out.outcome = Outcome::Empty; // nothing to find: no text, a password, damaged, crashed the reader
        if (read.status != extractproto::Status::Ok)
            return out;
        grams::Collector collector(TextEncoding::Utf8, ContentScanner::legacyCodePage());
        collector.feed(read.text.text.data(), read.text.text.size());
        out.keys = collector.finish();
        out.text = ContentIndex::packText(read.text);
        out.document = std::move(read.text);
        out.outcome = Outcome::Indexed;
        return out;
    }

    thread_local std::vector<char> buffer;
    buffer.resize(kChunkBytes);
    // Fills the buffer (or reads to the end): detection sees the same bytes as the scanner's.
    const auto fill = [&](bool& failed) {
        std::size_t total = 0;
        while (total < kChunkBytes) {
            DWORD got = 0;
            if (!::ReadFile(file.get(), buffer.data() + total, static_cast<DWORD>(kChunkBytes - total), &got, nullptr)) {
                failed = true;
                return total;
            }
            if (got == 0)
                break;
            total += got;
        }
        return total;
    };
    bool failed = false;
    std::size_t n = fill(failed);
    if (failed)
        return out;
    std::size_t bom = 0;
    const TextEncoding encoding = ContentScanner::detect({buffer.data(), n}, &bom);
    grams::Collector collector(encoding, ContentScanner::legacyCodePage());
    collector.feed(buffer.data() + bom, n - bom);
    while (n == kChunkBytes) {
        if (cancelled && cancelled())
            return out;
        n = fill(failed);
        if (failed)
            return out;
        collector.feed(buffer.data(), n);
    }
    out.keys = collector.finish();
    out.outcome = Outcome::Indexed;
    return out;
}

ContentIndexer::Scanned ContentIndexer::scanFile(std::wstring_view path, std::int64_t maxBytes,
    const ContentScanner& scanner, const std::function<bool()>& cancelled)
{
    Scanned out;
    const win32::UniqueHandle file = openText(path, maxBytes, out.text);
    if (!file.valid())
        return out;
    // The scanner reads through this, which hands the bytes to the collector
    // too: the first chunk once it is all there (the encoding is detected
    // from it, as readFile() does), then each as it comes.
    thread_local std::string head;
    head.clear();
    std::optional<grams::Collector> collector;
    bool failed = false;
    const auto startCollector = [&] {
        std::size_t bom = 0;
        const TextEncoding encoding = ContentScanner::detect(head, &bom);
        collector.emplace(encoding, ContentScanner::legacyCodePage());
        collector->feed(head.data() + bom, head.size() - bom);
        head.clear();
    };
    const ContentScanner::ReadFn read = [&](char* buffer, std::size_t capacity) -> std::size_t {
        DWORD got = 0;
        if (!::ReadFile(file.get(), buffer, static_cast<DWORD>(std::min<std::size_t>(capacity, 1u << 30)), &got, nullptr)) {
            failed = true;
            return 0;
        }
        if (collector) {
            collector->feed(buffer, got);
        } else {
            head.append(buffer, got);
            if (head.size() >= kChunkBytes || got == 0)
                startCollector();
        }
        return got;
    };
    out.match = scanner.scan(read, kChunkBytes, cancelled);
    // What the scan left: the rest after a match (or after the first chunk,
    // when the phrase cannot be in the file's encoding).
    thread_local std::vector<char> rest;
    rest.resize(kChunkBytes);
    while (!failed && !(cancelled && cancelled()) && read(rest.data(), rest.size()) > 0) {
    }
    if (failed || (cancelled && cancelled()))
        return out; // the text Skipped: not all of it was read
    if (!collector)
        startCollector();
    out.text.keys = collector->finish();
    out.text.outcome = Outcome::Indexed;
    return out;
}

ContentIndexer::Intake::Intake(ContentIndexer* indexer)
{
    if (!indexer)
        return;
    {
        std::lock_guard lock(indexer->m_mutex);
        if (!indexer->m_options.enabled || indexer->m_paused)
            return;
    }
    if (!indexer->m_source.ready())
        return; // as the indexer: ids may not be final yet
    for (const Volume& v : indexer->m_source.volumes()) {
        if (!v.suspended)
            m_roots.push_back(QString::fromStdString(v.root).toStdWString() + L'\\');
    }
    if (m_roots.empty())
        return;
    m_indexer = indexer;
    m_indexer->m_content->beginReads();
}

ContentIndexer::Intake::~Intake()
{
    if (!m_indexer)
        return;
    m_indexer->m_content->endReads();
    if (const std::size_t taken = m_taken.load(); taken > 0) {
        m_indexer->m_taken += taken;
        m_indexer->wake(); // merges what came in, and goes on with the files left
    }
}

bool ContentIndexer::Intake::wants(std::wstring_view path) const noexcept
{
    return std::any_of(m_roots.begin(), m_roots.end(), [&](const std::wstring& root) {
        return path.size() > root.size() && win32::equalsIgnoreCase(path.substr(0, root.size()), root);
    });
}

std::uint64_t ContentIndexer::Intake::since() const noexcept
{
    return m_indexer ? m_indexer->m_content->changeSequence() : 0;
}

void ContentIndexer::Intake::take(EntryId entry, const FileText& text, std::uint64_t since)
{
    if (!m_indexer || text.outcome == Outcome::Skipped)
        return;
    std::shared_lock gate(m_indexer->m_intakeGate);
    {
        std::lock_guard lock(m_indexer->m_mutex);
        if (m_indexer->m_paused || !m_indexer->m_options.enabled)
            return;
    }
    if (m_indexer->m_content->add(entry, text.keys, text.outcome == Outcome::Empty, text.stamp, since, text.text))
        ++m_taken;
}

std::optional<std::uint32_t> ContentIndexer::stampOf(std::wstring_view path)
{
    const std::optional<FileStamp> file = statOf(path);
    return file ? std::optional(file->stamp) : std::nullopt;
}

} // namespace ws

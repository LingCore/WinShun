#include "ContentIndexer.h"

#include "ContentScanner.h"
#include "Crawler.h"
#include "Documents.h"
#include "DiagnosticLog.h"
#include "TextUtil.h"
#include "Unc.h"
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
#include <optional>
#include <QJsonArray>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr auto kRetryDelay = 1min; // after a pass was cut short
constexpr auto kLookAround = 250ms; // a pass looks at merges due, the power and the user this often
constexpr std::size_t kMergeAfter = 1024; // files read (by passes or searches) before the segments become one
constexpr int kHardDiskReads = 2; // files read from one hard disk at once: more would make its heads jump about
// ... while someone uses the computer: one, at very low I/O priority (HardDisk::quiet)
constexpr int kHardDiskReadsQuiet = 1;
constexpr auto kAfterSearchBox = std::chrono::seconds(5); // closed, before files are read again: searching again
constexpr std::size_t kChunkBytes = ContentScanner::kChunkBytes;
// The bulk way (ContentIndexer.h): from this many text files to read for the
// first time on; files written within kBulkRecent are left to the usual way
// (what was just written may not be on the disk yet: the volume is read
// unbuffered); a session's files are fetched in batches of up to
// kBulkBatchBytes, and no more than kBulkPendingBytes wait in memory for a
// thread to take their grams.
constexpr std::size_t kBulkThreshold = 2000;
constexpr FileTime kBulkRecent = 5 * 60; // seconds
constexpr std::uint64_t kBulkBatchBytes = 64ull << 20;
constexpr std::size_t kBulkPendingBytes = 256u << 20;

std::int64_t steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One slot per reader/session. Report blocked opens, hashes or document
// reads before they return, without storing paths or file contents.
class ActiveReads {
    struct Slot {
        bool active = false;
        bool document = false;
        bool hardDisk = false;
        bool bulk = false;
        std::int64_t started = 0;
        QString volume;
        QString extension;
        const char* stage = "reading";
    };
    std::vector<Slot> m_slots;
    std::mutex m_mutex;
    std::atomic<int> m_slowReported {0};
    static QJsonObject describe(const Slot& slot)
    {
        return {{u"kind"_s, slot.bulk ? u"bulk"_s : slot.document ? u"document"_s : u"text"_s},
            {u"volume"_s, slot.volume}, {u"extension"_s, slot.extension}, {u"hard_disk"_s, slot.hardDisk},
            {u"stage"_s, QString::fromLatin1(slot.stage)}, {u"elapsed_ms"_s, steadyMs() - slot.started}};
    }
public:
    explicit ActiveReads(std::size_t capacity) : m_slots(diagnostics::enabled() ? capacity : 0) {}
    class Scope {
        ActiveReads& m_owner;
        int m_slot;
    public:
        Scope(ActiveReads& owner, int slot, const std::string& volume, bool document, bool hardDisk,
            std::wstring_view path = {}, bool bulk = false)
            : m_owner(owner), m_slot(slot)
        {
            if (m_owner.m_slots.empty()) {
                m_slot = -1;
                return;
            }
            QString extension;
            const auto at = path.find_last_of(L".\\/");
            if (at != std::wstring_view::npos && path[at] == L'.')
                extension = QString::fromStdWString(std::wstring(path.substr(at + 1, 16))).toLower();
            const std::lock_guard lock(m_owner.m_mutex);
            m_owner.m_slots[m_slot] = {true, document, hardDisk, bulk, steadyMs(), QString::fromStdString(volume),
                std::move(extension), bulk ? "bulk-connect" : "checking-file"};
        }
        Scope(const Scope&) = delete;
        void stage(const char* stage)
        {
            if (m_slot >= 0) {
                const std::lock_guard lock(m_owner.m_mutex);
                m_owner.m_slots[m_slot].stage = stage;
            }
        }
        ~Scope()
        {
            if (m_slot < 0)
                return;
            Slot slot;
            {
                const std::lock_guard lock(m_owner.m_mutex);
                slot = m_owner.m_slots[m_slot];
                m_owner.m_slots[m_slot].active = false;
            }
            if (!slot.bulk && steadyMs() - slot.started >= 3000 && m_owner.m_slowReported.fetch_add(1) < 8)
                diagnostics::record(u"content-slow-read"_s, describe(slot));
        }
    };
    QJsonArray snapshot()
    {
        const std::lock_guard lock(m_mutex);
        std::vector<Slot> active;
        for (const Slot& slot : m_slots) {
            if (slot.active)
                active.push_back(slot);
        }
        std::sort(active.begin(), active.end(), [](const Slot& a, const Slot& b) { return a.started < b.started; });
        QJsonArray out;
        for (std::size_t i = 0; i < std::min<std::size_t>(8, active.size()); ++i)
            out.append(describe(active[i]));
        return out;
    }
};

// WINSHUN_BULK_VERIFY=1: every file read the bulk way is read the usual way
// too, and the grams compared (wsbench --service --content reports).
bool bulkVerify()
{
    static const bool on = [] {
        wchar_t value[8] {};
        return ::GetEnvironmentVariableW(L"WINSHUN_BULK_VERIFY", value, 8) > 0 && value[0] == L'1';
    }();
    return on;
}

// WINSHUN_CROWDED=0: crowded folders' files are not put last (for wsbench
// --service --content, to compare).
bool crowdedLast()
{
    static const bool on = [] {
        wchar_t value[8] {};
        return !(::GetEnvironmentVariableW(L"WINSHUN_CROWDED", value, 8) > 0 && value[0] == L'0');
    }();
    return on;
}

// An extension as one number, for counting: up to 8 characters packed and
// folded; a longer one by its hash.
std::uint64_t extensionKey(std::string_view ext) noexcept
{
    if (ext.size() > 8)
        return std::hash<std::string_view> {}(ext) | (1ull << 63);
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < ext.size(); ++i)
        v |= std::uint64_t {text::fold(ext[i])} << (8 * i);
    return v;
}

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
    // Before the snapshot is saved: a share's thread still reading a file
    // then (they are joined after it) must not append its text to the index.
    {
        std::unique_lock gate(m_intakeGate); // a hand-over under way finishes first
        m_stopping = true;
    }
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

ContentIndexer::Options ContentIndexer::options()
{
    std::lock_guard lock(m_mutex);
    return m_options;
}

bool ContentIndexer::mayReadRemote(bool few)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_options.enabled || m_paused || m_stopping.load())
            return false;
    }
    // Nor while the index is merged: a document added meanwhile makes a full
    // merge (ContentIndex::merge) throw its work away.
    if (m_merging.load() || batterySaverOn() || m_content->searching())
        return false;
    return few || !searchBoxHolds();
}

ContentIndexer::BulkStats ContentIndexer::bulkStats() const noexcept
{
    return {m_bulk.sessions.load(), m_bulk.sessionsFailed.load(), m_bulk.files.load(), m_bulk.bytes.load(),
        m_bulk.tooLarge.load(), m_bulk.recent.load(), m_bulk.noRecord.load(), m_bulk.notFound.load(), m_bulk.denied.load(),
        m_bulk.fallback.load(), m_bulk.verified.load(), m_bulk.verifyMismatches.load()};
}

void ContentIndexer::wake()
{
    {
        std::lock_guard lock(m_mutex);
        m_woken = true;
    }
    m_cv.notify_all();
}

void ContentIndexer::volumesChanged()
{
    {
        std::lock_guard lock(m_mutex);
        m_optionsChanged = true; // as for new options: a pass now, and one under way starts over
    }
    m_cv.notify_all();
}

QJsonObject ContentIndexer::diagnosticStatus() const
{
    const auto stats = m_content->stats();
    const auto bulk = bulkStats();
    const bool active = reading();
    const QString phase = !active ? u"idle"_s : m_merging.load() ? u"merging"_s
        : m_searchBoxOpen.load() ? u"paused-search-window"_s : searchBoxHolds() ? u"waiting-after-search-window"_s
        : m_content->searching() ? u"paused-content-search"_s : u"reading"_s;
    return {{u"phase"_s, phase}, {u"reading"_s, active},
        {u"done"_s, static_cast<qint64>(m_passDone.load())}, {u"total"_s, static_cast<qint64>(m_passTotal.load())},
        {u"elapsed_ms"_s, active ? steadyMs() - m_passStartedMs.load() : 0},
        {u"no_progress_ms"_s, active ? steadyMs() - m_lastProgressMs.load() : 0},
        {u"indexed_files"_s, static_cast<qint64>(stats.documents)}, {u"pending_files"_s, static_cast<qint64>(stats.pending)},
        {u"document_texts"_s, static_cast<qint64>(stats.texts)},
        {u"index_bytes"_s, static_cast<qint64>(stats.segmentBytes + stats.textBytes)},
        {u"bulk_sessions"_s, static_cast<qint64>(bulk.sessions)},
        {u"bulk_sessions_failed"_s, static_cast<qint64>(bulk.sessionsFailed)},
        {u"bulk_files"_s, static_cast<qint64>(bulk.files)}, {u"bulk_bytes"_s, static_cast<qint64>(bulk.bytes)},
        {u"bulk_too_large"_s, static_cast<qint64>(bulk.tooLarge)},
        {u"fallback_recent"_s, static_cast<qint64>(bulk.recent)},
        {u"fallback_no_record"_s, static_cast<qint64>(bulk.noRecord)},
        {u"fallback_not_found"_s, static_cast<qint64>(bulk.notFound)},
        {u"fallback_denied"_s, static_cast<qint64>(bulk.denied)},
        {u"fallback_special_files"_s, static_cast<qint64>(bulk.fallback)}};
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
    diagnostics::record(u"content-worker-start"_s);
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
            diagnostics::record(u"content-waiting-for-file-index"_s);
            m_woken = false;
            if (!m_cv.wait(lock, stop, [this] { return m_woken; }))
                return;
        }
        lock.unlock();
        const bool firstRun = m_content->stats().documents == 0;
        lock.lock();
        if (!firstRun) {
            diagnostics::record(u"content-startup-delay"_s, {{u"milliseconds"_s, m_timing.start.count()}});
            m_cv.wait_until(lock, stop, started + m_timing.start, [] { return false; });
        }
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
        else if (options.enabled)
            diagnostics::record(u"content-deferred"_s,
                {{u"reason"_s, batterySaverOn() ? u"battery-saver"_s : u"file-index-not-ready"_s}});
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
// hidden ones, and those in crowded folders (crowdedFolders: generated files,
// mostly, that few look for); each kind as the index has them (by folder,
// mostly).
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
        bool remote;
    };
    std::vector<Root> roots;
    for (const EntryId r : index.roots()) {
        // A share as configured, its root as the index names it ("\\NAS\Docs"
        // for "\\nas\docs", "\\wsl$\..." for "\\wsl.localhost\...": unc::equal).
        const std::string_view name = index.name(r);
        const auto it = std::find_if(volumes.begin(), volumes.end(), [&](const Volume& v) {
            if (!v.remote)
                return v.root == name;
            std::u16string a;
            std::u16string b;
            wtf8::decodeAppend(a, v.root);
            wtf8::decodeAppend(b, name);
            return unc::equal(wtf8::wview(a), wtf8::wview(b));
        });
        if (it != volumes.end() && (it->remote || options.local))
            roots.push_back({r, static_cast<std::size_t>(it - volumes.begin()), it->suspended, it->remote});
    }
    const bool anyRemote = std::ranges::any_of(roots, &Root::remote);
    const std::vector<ContentIndex::DocInfo> docs = m_content->documents();
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_recentReads, [&](const auto& r) { return now - r.second >= m_timing.cooldown; });

    std::vector<Work> work;
    std::vector<EntryId> retired;
    std::vector<EntryId> candidates; // on the volumes followed, read before or not: what makes a folder crowded
    RootFinder finder(index, index.findPath(win32::expandEnvironment(L"%USERPROFILE%")));
    std::size_t k = 0;
    for (std::size_t c = 0; c < index.chunkCount(); ++c) {
        const auto entries = index.chunk(c);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            // A remote file a search looks in (ContentVerdict::Remote): kept
            // when its share's contents are indexed, read by its own thread.
            const bool remote = (e.flags & EntryFlag::Remote) != 0;
            if (remote ? !anyRemote || !filter.acceptsRemote(index, e) : !filter.accepts(index, e))
                continue;
            const auto id = static_cast<EntryId>((c << FileIndex::kChunkBits) + i);
            const RootFinder::Place place = finder.placeOf(id);
            const auto volume
                = std::find_if(roots.begin(), roots.end(), [&](const Root& r) { return r.entry == place.root; });
            if (volume == roots.end())
                continue;
            if (!remote)
                candidates.push_back(id);
            for (; k < docs.size() && docs[k].entry < id; ++k)
                retired.push_back(docs[k].entry);
            const bool known = k < docs.size() && docs[k].entry == id;
            if (volume->suspended || volume->remote) {
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
            } else {
                w.order = orderOf(place.inProfile, (e.flags & EntryFlag::Hidden) != 0, false); // crowded: below
            }
            w.document = documents.matches(index.name(e), index.extLength(e));
            w.volume = volume->volume;
            work.push_back(w);
        }
    }
    for (; k < docs.size(); ++k)
        retired.push_back(docs[k].entry);
    m_content->retire(retired);
    // Only files read for the first time are put last (12 ms for 359248
    // candidates here): most passes have none.
    if (std::none_of(work.begin(), work.end(), [](const Work& w) { return w.order != kKnown; }))
        return work; // in order already
    const auto crowdedStarted = std::chrono::steady_clock::now();
    const std::vector<EntryId> crowded = crowdedFolders(index, candidates);
    qint64 crowdedFiles = 0;
    for (Work& w : work) {
        if (w.order != kKnown && std::binary_search(crowded.begin(), crowded.end(), index.entry(w.entry).parent)) {
            w.crowded = true;
            ++crowdedFiles;
            if (crowdedLast())
                w.order = kCrowded;
        }
    }
    diagnostics::record(u"content-crowded"_s, {{u"candidates"_s, static_cast<qint64>(candidates.size())},
        {u"folders"_s, static_cast<qint64>(crowded.size())}, {u"files"_s, crowdedFiles},
        {u"last"_s, crowdedLast()},
        {u"elapsed_us"_s, static_cast<qint64>(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - crowdedStarted).count())}});
    std::stable_sort(work.begin(), work.end(), [](const Work& a, const Work& b) { return a.order < b.order; });
    return work;
}

std::vector<EntryId> ContentIndexer::crowdedFolders(const FileIndex& index, std::span<const EntryId> candidates)
{
    // Measured 2026-10-19 on the 356787 candidates of a developer's computer
    // (wsbench --content-plan): 24499 files, 1459 MB, in folders like Qt's
    // and CMake's HTML documentation, translation output (JSON) and
    // Android's XML resources, and almost none a user would look for. Folder
    // names (bin, build, lib...) were no help: they are in source trees too.
    struct File {
        EntryId folder;
        std::uint64_t extension;
        auto operator<=>(const File&) const = default;
    };
    std::vector<File> files;
    files.reserve(candidates.size());
    for (const EntryId id : candidates) {
        const Entry& e = index.entry(id);
        std::size_t extLength = 0;
        const std::string_view name = index.fileName(e, extLength);
        files.push_back({e.parent, extensionKey(name.substr(name.size() - std::min(extLength, name.size())))});
    }
    std::sort(files.begin(), files.end());
    std::vector<EntryId> crowded;
    for (auto folder = files.begin(); folder != files.end();) {
        const auto end = std::find_if(folder, files.end(), [&](const File& f) { return f.folder != folder->folder; });
        const auto total = static_cast<std::size_t>(end - folder);
        if (total >= kCrowdedFiles) {
            std::size_t most = 0;
            for (auto run = folder; run != end;) {
                const auto runEnd = std::find_if(run, end, [&](const File& f) { return f.extension != run->extension; });
                most = std::max(most, static_cast<std::size_t>(runEnd - run));
                run = runEnd;
            }
            if (most * 100 >= total * kCrowdedShare)
                crowded.push_back(folder->folder);
        }
        folder = end;
    }
    return crowded; // sorted, as the files were
}

int ContentIndexer::documentProcesses() noexcept
{
    // Each takes a processor while it reads (at idle priority), and the
    // memory of the document it reads. Here (32 logical processors) 8 read
    // 5815 documents in 21 s, 4 in 28 s: a few large PDFs take seconds each.
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 4, 2, 8);
}

void ContentIndexer::setSearchBoxOpen(bool open) noexcept
{
    if (!open)
        m_searchBoxClosed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    m_searchBoxOpen = open;
}

bool ContentIndexer::searchBoxHolds() const noexcept
{
    if (m_searchBoxOpen.load())
        return true;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return now - std::chrono::milliseconds(m_searchBoxClosed.load()) < kAfterSearchBox;
}

bool ContentIndexer::pass(const Options& options, std::stop_token stop)
{
    const auto index = m_source.index();
    if (!index)
        return true;
    const std::vector<Volume> volumes = m_source.volumes();
    const auto gatherStarted = steadyMs();
    diagnostics::record(u"content-gather-start"_s, {{u"volumes"_s, static_cast<qint64>(volumes.size())}});
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
    diagnostics::record(u"content-gather-finished"_s,
        {{u"files"_s, static_cast<qint64>(work.size())}, {u"elapsed_ms"_s, steadyMs() - gatherStarted}});
    if (work.empty()) {
        m_merging = true; // the shares' threads wait meanwhile (mayReadRemote)
        tidy(m_taken.exchange(0)); // what searches and shares' threads handed over
        m_merging = false;
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
        bool hardDisk = false;
    };
    std::deque<Queue> queues; // keeps their places as more are added
    std::vector<Queue*> hardDiskQueue(volumes.size(), nullptr);
    {
        std::vector<std::pair<std::uint32_t, Queue*>> disks;
        for (std::size_t v = 0; v < volumes.size(); ++v) {
            if (volumes[v].suspended || volumes[v].remote)
                continue; // not touched: it is being ejected or locked; a share's own thread reads it
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
            q.hardDisk = true;
            hardDiskQueue[v] = &q;
            disks.emplace_back(p.disk, &q);
        }
    }
    // Half the extractor processes while other files are read and someone
    // uses the computer (the antivirus scans documents too), all of them
    // once those are done or nobody does: documents read beside the other
    // files leave none to read at the end (wsbench --service --content).
    bool away = win32::userAway(); // nobody at the computer (the coordinator's, from here on)
    std::atomic<bool> awayNow {away}; // the same, for the readers
    // A hard disk is all the user's while they are at the computer: one file
    // at a time, at very low I/O priority (readFile, HardDisk::quiet), so
    // Windows serves every other program's reads first. Nobody there: two.
    const auto hardDiskRoom = [](bool away) { return away ? kHardDiskReads : kHardDiskReadsQuiet; };
    for (Queue& q : queues)
        q.room = hardDiskRoom(away);
    Queue& documents = queues.emplace_back();
    documents.room = away ? documentProcesses() : std::max(1, documentProcesses() / 2);
    Queue& others = queues.emplace_back();

    // The bulk way (ContentIndexer.h): the text files to read for the first
    // time, when there are enough of them and the volumes can be read by
    // their records. Each goes to its volume's session instead of a queue;
    // what a session does not give comes back to the usual way (`late`).
    struct BulkVolume {
        std::size_t volume;
        std::vector<const Work*> files;
        std::unordered_map<EntryId, std::uint32_t> folderRecord; // the index keeps the folders' MFT record numbers
    };
    std::vector<BulkVolume> bulkVolumes;
    const std::shared_ptr<ntfs::VolumeAccess> access = m_source.access ? m_source.access() : nullptr;
    if (access && access->kind() != ntfs::VolumeAccess::Kind::None) {
        const auto lock = index->readLock();
        const auto now = static_cast<FileTime>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        std::vector<EntryId> rootOf(volumes.size(), kNoEntry);
        for (const EntryId r : index->roots()) {
            for (std::size_t v = 0; v < volumes.size(); ++v) {
                if (index->name(r) == volumes[v].root)
                    rootOf[v] = r;
            }
        }
        std::vector<std::vector<const Work*>> perVolume(volumes.size());
        std::size_t candidates = 0;
        std::size_t recent = 0;
        for (const Work& w : work) {
            if (w.checkOnly || w.again || w.document || volumes[w.volume].suspended || rootOf[w.volume] == kNoEntry)
                continue;
            // Written lately (the time is the journal's, so it is current):
            // what was written may not be on the disk yet.
            if (index->modified(w.entry) + kBulkRecent > now) {
                ++recent;
                continue;
            }
            perVolume[w.volume].push_back(&w);
            ++candidates;
        }
        if (candidates >= kBulkThreshold) {
            m_bulk.recent += recent;
            for (std::size_t v = 0; v < volumes.size(); ++v) {
                if (perVolume[v].empty())
                    continue;
                BulkVolume bv {v, std::move(perVolume[v]), {}};
                if (const RecordTable* table = index->folderRecords(rootOf[v]))
                    table->forEach([&](std::uint32_t record, EntryId folder) { bv.folderRecord[folder] = record; });
                bulkVolumes.push_back(std::move(bv));
            }
        }
    }
    std::vector<bool> inBulk(work.size(), false);
    for (const BulkVolume& bv : bulkVolumes) {
        for (const Work* w : bv.files)
            inBulk[static_cast<std::size_t>(w - work.data())] = true;
    }
    for (const Work& w : work) {
        if (inBulk[static_cast<std::size_t>(&w - work.data())])
            continue;
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

    m_passDone = 0;
    m_passTotal = work.size();
    m_passCrowdedDone = 0;
    m_passCrowded = static_cast<std::size_t>(
        std::count_if(work.begin(), work.end(), [](const Work& w) { return w.crowded; }));
    const auto doneWith = [this](const Work& w) {
        if (w.crowded)
            ++m_passCrowdedDone;
        ++m_passDone;
    };
    m_passStartedMs = steadyMs();
    m_lastProgressMs = m_passStartedMs.load();
    m_merging = false;
    m_reading = true;
    const struct Done {
        std::atomic<bool>& flag;
        std::atomic<bool>& merging;
        ~Done() { flag = false; merging = false; }
    } done {m_reading, m_merging};
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
    ActiveReads activeReads(static_cast<std::size_t>(threads) + volumes.size());
    QJsonArray diskInfo;
    std::size_t docCount = 0;
    for (const Work& file : work)
        docCount += file.document;
    for (std::size_t v = 0; v < volumes.size(); ++v)
        diskInfo.append(QJsonObject {{u"volume"_s, QString::fromStdString(volumes[v].root)},
            {u"hard_disk"_s, hardDiskQueue[v] != nullptr},
            {u"file_readers"_s, hardDiskQueue[v] ? hardDiskRoom(away) : readers(away)}});
    QJsonArray sizeLimits;
    for (const auto bytes : options.sizeLimits.bytes)
        sizeLimits.append(static_cast<qint64>(bytes));
    diagnostics::record(u"content-start"_s, {{u"total"_s, static_cast<qint64>(work.size())},
        {u"documents"_s, static_cast<qint64>(docCount)}, {u"disks"_s, diskInfo},
        {u"user_away"_s, away}, {u"reader_threads"_s, readers(away)},
        {u"document_processes"_s, documentProcesses()}, {u"include_system_folders"_s, options.includeLowPriority},
        {u"size_limits_bytes"_s, sizeLimits},
        {u"access"_s, !access || access->kind() == ntfs::VolumeAccess::Kind::None ? u"none"_s
                : access->kind() == ntfs::VolumeAccess::Kind::Service ? u"service"_s : u"direct"_s}});
    int running = threads; // under gateMutex
    const auto cancelled = [&] { return halt.load() || stop.stop_requested() || m_interrupt.load(); };
    const auto waitOn = [&](std::condition_variable& cv, std::chrono::milliseconds most) {
        std::unique_lock lock(gateMutex);
        cv.wait_for(lock, most);
    };

    // The bulk way's hand-overs: files whose bytes a session delivered, for a
    // reader thread to take the grams out of (`ready`: kBulkPendingBytes of
    // them at most), and files a session will not give, for the usual way
    // (`late`). `feeders`: sessions still at work.
    struct Ready {
        const Work* work;
        std::uint64_t since;
        std::uint32_t stamp;
        std::vector<std::byte> bytes;
    };
    std::mutex bulkMutex;
    std::condition_variable bulkRoom; // pending bytes went down (or the pass ends)
    std::deque<Ready> ready;
    std::deque<const Work*> late;
    std::size_t pendingBytes = 0; // under bulkMutex
    std::atomic<int> feeders {static_cast<int>(bulkVolumes.size())};
    const auto bulkLeft = [&] {
        if (feeders.load() > 0)
            return true;
        std::lock_guard lock(bulkMutex);
        return !ready.empty() || !late.empty();
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
    const auto read = [&](const Work& w, std::wstring& path, bool hardDisk, int slot) {
        if (m_content->knows(w.entry))
            return true; // a search read it meanwhile (Intake)
        {
            const auto lock = index->readLock();
            if (index->entry(w.entry).isDeleted())
                return true;
            path = index->wpath(w.entry);
        }
        ActiveReads::Scope activity(activeReads, slot, volumes[w.volume].root, w.document, hardDisk, path);
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
        const HardDisk disk {.on = hardDisk, .quiet = hardDisk && !awayNow.load()};
        // Quiet: the thread's own I/O at background priority too, which is
        // how the antivirus's scan of the file as it is opened goes after
        // the user's reads as well (on a solid-state disk that only made
        // reading 5-15 times slower). Not while handing over to the index,
        // whose lock others wait for.
        struct Background {
            bool on = false;
            explicit Background(bool quiet) { begin(quiet); }
            void begin(bool quiet)
            {
                if (quiet && !on)
                    on = ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            }
            void end()
            {
                if (std::exchange(on, false))
                    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            }
            ~Background() { end(); }
        } background(disk.quiet);
        const std::optional<FileStamp> file = w.document ? statOf(path) : std::nullopt;
        activity.stage("document-hash-or-copy");
        const std::optional<Copies::Original> original
            = file && file->size > 0 && static_cast<std::int64_t>(file->size) <= maxBytes
            ? copies.find(w.entry, path, file->size, file->stamp, cancelled)
            : std::nullopt;
        bool copied = false;
        if (original) {
            background.end();
            copied = m_content->addCopy(w.entry, original->entry, original->stamp, file->stamp, since);
            if (copied)
                ++m_copies;
            else
                background.begin(disk.quiet);
        }
        if (!copied) {
            activity.stage(w.document ? "document-open-prefetch-extract" : "text-open-read");
            const FileText text = readFile(path, maxBytes, cancelled, &m_documents, disk);
            background.end();
            activity.stage("add-to-content-index");
            if (text.outcome == Outcome::Skipped)
                return true;
            if (!m_content->add(
                    w.entry, text.keys, text.outcome == Outcome::Empty, text.stamp, since, text.text, text.large))
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
    // The grams of a file the bulk way delivered, into the index; false when
    // the index has no room left.
    const auto takeGrams = [&](Ready& r) {
        const Work& w = *r.work;
        if (m_content->knows(w.entry))
            return true; // a search read it meanwhile (Intake)
        const FileText text = collectText({reinterpret_cast<const char*>(r.bytes.data()), r.bytes.size()}, r.stamp);
        if (bulkVerify()) {
            // The file the usual way too: the same grams?
            std::wstring path;
            {
                const auto lock = index->readLock();
                path = index->wpath(w.entry);
            }
            const FileText again = readFile(path, options.sizeLimits.of(path), cancelled);
            ++m_bulk.verified;
            if (again.outcome != text.outcome || again.keys != text.keys || again.stamp != text.stamp) {
                ++m_bulk.verifyMismatches;
                qWarning("WinShun: the bulk read of %ls differs from the file: %zu grams (outcome %d, stamp %u) against %zu "
                         "(outcome %d, stamp %u) from %zu bytes",
                    path.c_str(), text.keys.size(), static_cast<int>(text.outcome), text.stamp, again.keys.size(),
                    static_cast<int>(again.outcome), again.stamp, r.bytes.size());
            }
        }
        if (!m_content->add(w.entry, text.keys, text.outcome == Outcome::Empty, text.stamp, r.since, {}, false))
            return false;
        ++added;
        ++m_bulk.files;
        m_bulk.bytes += r.bytes.size();
        return true;
    };
    const auto worker = [&](int t) {
        const bool forDocuments = t < documentThreads;
        const std::vector<Queue*>& mine = forDocuments ? documentQueues : readerQueues;
        std::wstring path;
        while (!cancelled() && !full.load()) {
            if (hold.load() || (!forDocuments && t - documentThreads >= allowed.load())) {
                if (!anyLeft(mine) && (forDocuments || !bulkLeft()))
                    break; // the threads that may read took them all
                waitOn(gate, 1s);
                continue;
            }
            if (m_content->searching() || searchBoxHolds()) {
                waitOn(gate, 100ms); // it opens files meanwhile, or may any moment
                continue;
            }
            // Announced before looking at `hold`: a merge waits for it.
            busy.fetch_add(1);
            if (!forDocuments && !hold.load()) {
                // The bulk way's hand-overs first: their bytes are in memory
                // already; then the files it would not give.
                std::optional<Ready> delivered;
                const Work* lateWork = nullptr;
                {
                    std::lock_guard lock(bulkMutex);
                    if (!ready.empty()) {
                        delivered = std::move(ready.front());
                        ready.pop_front();
                        pendingBytes -= delivered->bytes.size();
                    } else if (!late.empty()) {
                        lateWork = late.front();
                        late.pop_front();
                    }
                }
                if (delivered) {
                    bulkRoom.notify_all();
                    if (!takeGrams(*delivered))
                        full = true;
                    doneWith(*delivered->work);
                    busy.fetch_sub(1);
                    continue;
                }
                if (lateWork) {
                    if (!read(*lateWork, path, hardDiskQueue[lateWork->volume] != nullptr, t))
                        full = true;
                    doneWith(*lateWork);
                    busy.fetch_sub(1);
                    continue;
                }
            }
            Queue* from = nullptr;
            bool left = true;
            const Work* w = hold.load() ? nullptr : take(mine, from, left);
            if (!w) {
                busy.fetch_sub(1);
                if (!left && (forDocuments || !bulkLeft()))
                    break; // all taken
                waitOn(room, 100ms); // the files left wait for a hard disk, an extractor or a session
                continue;
            }
            if (!read(*w, path, from->hardDisk, t))
                full = true;
            doneWith(*w);
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

    // One volume's bulk session, on a thread of its own: asks for its files,
    // records the ones too large from what the scan says, hands the others
    // back to the usual way, and fetches the bytes of the rest in batches as
    // memory allows and while nobody searches. What it did not deliver by the
    // end goes the usual way too.
    const auto feed = [&](BulkVolume& bv) {
        const Volume& vol = volumes[bv.volume];
        const std::wstring root = QString::fromStdString(vol.root).toStdWString();
        const auto giveBack = [&](const std::vector<const Work*>& files) {
            if (files.empty())
                return;
            {
                std::lock_guard lock(bulkMutex);
                late.insert(late.end(), files.begin(), files.end());
            }
            room.notify_all();
        };
        const auto finish = [&] {
            --feeders;
            room.notify_all();
            bulkRoom.notify_all();
        };
        const bool quiet = hardDiskQueue[bv.volume] != nullptr && !awayNow.load();
        ActiveReads::Scope activity(activeReads, threads + static_cast<int>(bv.volume), vol.root, false,
            hardDiskQueue[bv.volume] != nullptr, {}, true);
        diagnostics::record(u"content-bulk-start"_s, {{u"volume"_s, QString::fromStdString(vol.root)},
            {u"quiet"_s, quiet}, {u"files"_s, static_cast<qint64>(bv.files.size())}});
        std::unique_ptr<ntfs::BulkSource> session = access->openBulkRead(root, vol.serial, quiet);
        if (!session || !session->valid()) {
            if (session)
                qWarning("WinShun: no bulk read of %ls: %ls", root.c_str(), session->error().c_str());
            ++m_bulk.sessionsFailed;
            giveBack(bv.files);
            finish();
            return;
        }
        ++m_bulk.sessions;
        // Taken before anything is read: a file written from here on turns
        // dirty as it is added (ContentIndex::add's `since`).
        const std::uint64_t since = m_content->changeSequence();
        std::vector<ntfs::BulkWant> wants;
        std::vector<const Work*> ticketWork; // the Work of each ticket
        wants.reserve(bv.files.size());
        ticketWork.reserve(bv.files.size());
        std::vector<const Work*> back;
        {
            const auto lock = index->readLock();
            for (const Work* w : bv.files) {
                const Entry& e = index->entry(w->entry);
                if (e.isDeleted()) {
                    doneWith(*w);
                    continue;
                }
                const auto parent = bv.folderRecord.find(e.parent);
                if (parent == bv.folderRecord.end()) {
                    ++m_bulk.noRecord;
                    back.push_back(w);
                    continue;
                }
                std::u16string name;
                wtf8::decodeAppend(name, index->name(e));
                wants.push_back({parent->second, std::move(name),
                    static_cast<std::uint64_t>(std::max<std::int64_t>(0, options.sizeLimits.of(index->wpath(w->entry))))});
                ticketWork.push_back(w);
            }
        }
        giveBack(back);
        back.clear();
        std::vector<ntfs::BulkFile> files;
        activity.stage("bulk-scan-mft");
        if (cancelled() || !session->want(wants) || !session->scan(files)) {
            if (!cancelled()) {
                qWarning("WinShun: the bulk read of %ls did not start: %ls", root.c_str(), session->error().c_str());
                ++m_bulk.sessionsFailed;
                giveBack(ticketWork);
            }
            finish();
            return;
        }
        // What the session will not give goes the usual way; a file too large
        // for its limit is recorded from the record alone, as openText would.
        std::vector<const ntfs::BulkFile*> data; // in the order given: the disk's
        std::vector<const ntfs::BulkFile*> byTicket(ticketWork.size(), nullptr);
        for (const ntfs::BulkFile& f : files) {
            if (f.ticket >= ticketWork.size())
                continue;
            byTicket[f.ticket] = &f;
            const Work* w = ticketWork[f.ticket];
            switch (f.status) {
            case ntfs::BulkStatus::Data:
                data.push_back(&f);
                break;
            case ntfs::BulkStatus::TooLarge:
                if (!m_content->knows(w->entry)) {
                    if (m_content->add(w->entry, {}, true, ContentIndex::stampOf(static_cast<std::int64_t>(f.size), f.modified),
                            since, {}, true))
                        ++added;
                    else
                        full = true;
                }
                ++m_bulk.tooLarge;
                doneWith(*w);
                break;
            case ntfs::BulkStatus::NotFound:
                ++m_bulk.notFound;
                back.push_back(w);
                break;
            case ntfs::BulkStatus::Denied:
                ++m_bulk.denied;
                back.push_back(w);
                break;
            case ntfs::BulkStatus::Fallback:
                ++m_bulk.fallback;
                back.push_back(w);
                break;
            }
        }
        giveBack(back);
        back.clear();
        // The disk's order within each kind of file, the kinds in findWork's
        // order: the user's own files first, crowded folders' last.
        std::stable_sort(data.begin(), data.end(), [&](const ntfs::BulkFile* a, const ntfs::BulkFile* b) {
            return ticketWork[a->ticket]->order < ticketWork[b->ticket]->order;
        });
        // The bytes, in batches, in that order.
        std::vector<bool> got(ticketWork.size(), false);
        std::size_t at = 0;
        while (at < data.size() && !cancelled() && !full.load()) {
            activity.stage("bulk-wait-for-readers-or-search");
            std::vector<std::uint32_t> tickets;
            std::uint64_t batchBytes = 0;
            while (at < data.size() && (tickets.empty() || batchBytes + data[at]->size <= kBulkBatchBytes)) {
                tickets.push_back(data[at]->ticket);
                batchBytes += data[at]->size;
                ++at;
            }
            {
                // Room in memory, and nobody searching (the readers wait then too).
                std::unique_lock lock(bulkMutex);
                while (!cancelled() && !full.load()
                    && (pendingBytes + batchBytes > kBulkPendingBytes || hold.load() || m_content->searching() || searchBoxHolds()))
                    bulkRoom.wait_for(lock, 100ms);
            }
            if (cancelled() || full.load())
                break;
            activity.stage("bulk-read");
            const bool fetched = session->fetch(tickets, [&](std::uint32_t ticket, std::vector<std::byte>&& bytes) {
                if (ticket >= ticketWork.size() || !byTicket[ticket] || got[ticket])
                    return;
                got[ticket] = true;
                const ntfs::BulkFile& f = *byTicket[ticket];
                std::lock_guard lock(bulkMutex);
                pendingBytes += bytes.size();
                ready.push_back({ticketWork[ticket], since,
                    ContentIndex::stampOf(static_cast<std::int64_t>(f.size), f.modified), std::move(bytes)});
                room.notify_one();
            });
            if (!fetched) {
                if (!cancelled()) {
                    qWarning("WinShun: the bulk read of %ls broke: %ls", root.c_str(), session->error().c_str());
                    ++m_bulk.sessionsFailed;
                }
                break;
            }
        }
        session.reset(); // the volume let go of
        if (!cancelled()) {
            for (const ntfs::BulkFile* f : data) {
                if (!got[f->ticket])
                    back.push_back(ticketWork[f->ticket]);
            }
            giveBack(back);
        }
        finish();
    };

    const auto report = [&](const QString& event, double perSecond = -1) {
        if (!diagnostics::enabled())
            return;
        QJsonObject status = diagnosticStatus();
        if (hold.load() && !m_merging.load())
            status[u"phase"_s] = u"waiting-for-readers-before-merge"_s;
        status[u"elapsed_ms"_s] = steadyMs() - m_passStartedMs.load();
        if (perSecond >= 0)
            status[u"recent_files_per_second"_s] = perSecond;
        status[u"average_files_per_second"_s] = static_cast<double>(m_passDone.load()) * 1000
            / std::max<std::int64_t>(1, steadyMs() - m_passStartedMs.load());
        status[u"interrupt_requested"_s] = m_interrupt.load();
        status[u"stopping"_s] = stop.stop_requested();
        status[u"capacity_exhausted"_s] = full.load();
        status[u"battery_saver"_s] = batterySaverOn();
        status[u"user_away"_s] = away;
        status[u"allowed_readers"_s] = allowed.load();
        status[u"hard_disk_readers"_s] = hardDiskRoom(away);
        status[u"busy_readers"_s] = busy.load();
        status[u"bulk_sessions_active"_s] = feeders.load();
        status[u"active_operations"_s] = activeReads.snapshot();
        diagnostics::record(event, std::move(status));
    };
    std::int64_t lastReport = steadyMs();
    std::size_t lastReportDone = 0;
    std::size_t lastDone = 0;
    QString lastPhase;
    m_content->beginReads();
    {
        std::vector<std::jthread> feederThreads; // joined after the workers: those wait for them
        for (BulkVolume& bv : bulkVolumes)
            feederThreads.emplace_back([&feed, &bv] { feed(bv); });
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t)
            workers.emplace_back(worker, t);
        int ticks = 0;
        std::unique_lock lock(gateMutex);
        while (!gate.wait_for(lock, kLookAround, [&] { return running == 0; })) {
            lock.unlock();
            const std::int64_t nowMs = steadyMs();
            const std::size_t progressDone = m_passDone.load();
            if (progressDone != lastDone) {
                m_lastProgressMs = nowMs;
                lastDone = progressDone;
            }
            const QString phase = m_searchBoxOpen.load() ? u"paused-search-window"_s
                : searchBoxHolds() ? u"waiting-after-search-window"_s
                : m_content->searching() ? u"paused-content-search"_s : u"reading"_s;
            if (phase != lastPhase || nowMs - lastReport >= m_timing.report.count()) {
                report(phase != lastPhase ? u"content-state"_s : u"content-progress"_s,
                    static_cast<double>(progressDone - lastReportDone) * 1000 / std::max<std::int64_t>(1, nowMs - lastReport));
                lastPhase = phase;
                lastReport = nowMs;
                lastReportDone = progressDone;
            }
            bool letGo = false;
            if (full.load() || interrupted(stop) || batterySaverOn()) {
                halt = true;
                letGo = true;
            } else {
                if (++ticks % 8 == 0) { // whether someone is at the computer, every 2 s
                    away = win32::userAway();
                    awayNow = away;
                    const int now = readers(away);
                    letGo = now > allowed.exchange(now);
                    for (Queue& q : queues) {
                        if (q.hardDisk && hardDiskRoom(away) > q.room.exchange(hardDiskRoom(away)))
                            letGo = true;
                    }
                }
                // Only documents left, or nobody at the computer: every
                // extractor process.
                const int processes = away || !anyLeft(readerQueues) ? documentProcesses()
                                                                     : std::max(1, documentProcesses() / 2);
                if (processes > documents.room.exchange(processes))
                    letGo = true;
                if (!m_content->searching() && !searchBoxHolds()) {
                    if (m_content->needsFullMerge() || m_content->needsTextCompaction()) {
                        // Merging everything (or rewriting the text file)
                        // cannot take in documents added meanwhile: the files
                        // being read are finished first, no other started.
                        hold = true;
                        report(u"content-waiting-to-merge"_s);
                        while (busy.load() > 0 && !cancelled()) {
                            const auto at = steadyMs();
                            if (const auto count = m_passDone.load(); count != lastDone) {
                                lastDone = count;
                                m_lastProgressMs = at;
                            }
                            if (at - lastReport >= m_timing.report.count()) {
                                report(u"content-progress"_s);
                                lastReport = at;
                            }
                            std::this_thread::sleep_for(10ms);
                        }
                        if (busy.load() == 0) {
                            m_merging = true;
                            report(u"content-merge-start"_s);
                            if (m_content->needsMerge())
                                m_content->mergeDue(readers(away));
                            if (m_content->needsTextCompaction())
                                m_content->compactTexts();
                            m_merging = false;
                            report(u"content-merge-finished"_s);
                        }
                        hold = false;
                        letGo = true;
                    } else if (m_content->needsMerge()) {
                        // Small segments into larger ones while files are
                        // read: holding the readers for it cost the first run
                        // 14 s of 75 (most of it waiting for documents being
                        // read to finish: wsbench --service --content).
                        m_merging = true;
                        report(u"content-merge-tails"_s);
                        m_content->mergeTails();
                        m_merging = false;
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
    if (full.load() || halt.load() || interrupted(stop)) {
        report(u"content-interrupted"_s);
        return false;
    }
    m_merging = true;
    report(u"content-finalizing"_s);
    tidy(added.load() + m_taken.exchange(0));
    m_merging = false;
    m_reading = false;
    report(u"content-finished"_s);
    return true;
}

// After many files were read (the first time, say, or by a search that the
// index could not answer): one segment again, smaller than several, and the
// grams still in memory written out. Not while a search runs: it may hand
// files over, which a merge started before them cannot take in.
void ContentIndexer::tidy(std::size_t added)
{
    const int threads = readers(win32::userAway()); // as many as read files
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

// Opens a file to read it for the index, at low I/O priority (very low when
// `quiet`), and puts its stamp in `out`. Invalid when it is not to be read:
// out.outcome is Empty when there is nothing to find in it, Skipped when it
// may be read later.
win32::UniqueHandle openText(std::wstring_view path, std::int64_t maxBytes, ContentIndexer::FileText& out, bool quiet)
{
    win32::UniqueHandle file(::CreateFileW(win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file.valid()) {
        // No access will not change by itself; in use or gone may (and the
        // journal reports a file that is gone). Nor will WebDAV's size limit
        // (WebClient's FileSizeLimitInBytes): too large, as below.
        out.error = ::GetLastError();
        if (out.error == ERROR_ACCESS_DENIED || out.error == ERROR_FILE_TOO_LARGE)
            out.outcome = ContentIndexer::Outcome::Empty;
        out.large = out.error == ERROR_FILE_TOO_LARGE;
        return {};
    }
    FILE_BASIC_INFO basic {};
    LARGE_INTEGER size {};
    if (!::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof basic)
        || !::GetFileSizeEx(file.get(), &size)) {
        out.error = ::GetLastError();
        return {};
    }
    constexpr DWORD kNotHere = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN
        | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if (basic.FileAttributes & kNotHere)
        return {}; // never download a cloud file
    out.stamp = ContentIndex::stampOf(size.QuadPart, basic.LastWriteTime.QuadPart);
    out.bytes = size.QuadPart;
    out.written = basic.LastWriteTime.QuadPart;
    if (size.QuadPart <= 0 || (maxBytes > 0 && size.QuadPart > maxBytes)) {
        out.outcome = ContentIndexer::Outcome::Empty; // as ContentScanner::scanFile skips it
        out.large = size.QuadPart > 0;
        return {};
    }
    // On the file object: the extractor's reads through a copy of the handle have it too.
    FILE_IO_PRIORITY_HINT_INFO hint {};
    hint.PriorityHint = quiet ? IoPriorityHintVeryLow : IoPriorityHintLow;
    ::SetFileInformationByHandle(file.get(), FileIoPriorityHintInfo, &hint, sizeof hint);
    return file;
}

} // namespace

ContentIndexer::FileText ContentIndexer::readFile(std::wstring_view path, std::int64_t maxBytes,
    const std::function<bool()>& cancelled, DocExtractor* documents, HardDisk disk)
{
    FileText out;
    const bool document = isDocumentPath(path);
    if (document && !documents)
        return out;
    const win32::UniqueHandle file = openText(path, maxBytes, out, disk.quiet);
    if (!file.valid())
        return out;

    if (document) {
        LARGE_INTEGER size {};
        ::GetFileSizeEx(file.get(), &size);
        if (disk.on) {
            // Into the file cache in one go: the extractor reads a PDF's
            // cross-reference table at the end first, then its objects all
            // over, and on a hard disk every jump is a seek.
            thread_local std::vector<char> through;
            through.resize(1 << 20);
            DWORD got = 0;
            DWORD failed = NO_ERROR;
            while (!cancelled()) {
                if (!::ReadFile(file.get(), through.data(), static_cast<DWORD>(through.size()), &got, nullptr)) {
                    failed = ::GetLastError();
                    break;
                }
                if (got == 0)
                    break;
            }
            if (cancelled())
                return out;
            if (failed != NO_ERROR && disk.remote) {
                out.error = failed; // the server went away, most likely: later
                return out;
            }
        }
        FileText read = extractDocument(file.get(), static_cast<std::uint64_t>(size.QuadPart), documents, cancelled);
        read.stamp = out.stamp;
        read.bytes = out.bytes;
        read.written = out.written;
        return read;
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

ContentIndexer::FileText ContentIndexer::extractDocument(HANDLE file, std::uint64_t size, DocExtractor* documents,
    const std::function<bool()>& cancelled)
{
    FileText out;
    if (!documents)
        return out;
    DocExtractor::Result read = documents->extract(file, size, cancelled);
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

ContentIndexer::FileText ContentIndexer::collectText(std::span<const char> bytes, std::uint32_t stamp)
{
    // As readFile() has it: the encoding from the first chunk, the grams
    // from the bytes in chunks of the same size.
    FileText out;
    out.stamp = stamp;
    if (bytes.empty()) {
        out.outcome = Outcome::Empty;
        return out;
    }
    const std::size_t head = std::min(bytes.size(), kChunkBytes);
    std::size_t bom = 0;
    const TextEncoding encoding = ContentScanner::detect({bytes.data(), head}, &bom);
    grams::Collector collector(encoding, ContentScanner::legacyCodePage());
    collector.feed(bytes.data() + bom, head - bom);
    for (std::size_t at = head; at < bytes.size(); at += kChunkBytes)
        collector.feed(bytes.data() + at, std::min(kChunkBytes, bytes.size() - at));
    out.keys = collector.finish();
    out.outcome = Outcome::Indexed;
    return out;
}

ContentIndexer::Scanned ContentIndexer::scanFile(std::wstring_view path, std::int64_t maxBytes,
    const ContentScanner& scanner, const std::function<bool()>& cancelled)
{
    Scanned out;
    const win32::UniqueHandle file = openText(path, maxBytes, out.text, false); // a search: the user waits for it
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
    bool local = true;
    {
        std::lock_guard lock(indexer->m_mutex);
        if (!indexer->m_options.enabled || indexer->m_paused || indexer->m_stopping.load())
            return;
        local = indexer->m_options.local;
    }
    if (!indexer->m_source.ready())
        return; // as the indexer: ids may not be final yet
    for (const Volume& v : indexer->m_source.volumes()) {
        if (v.suspended)
            continue;
        if (v.remote) {
            std::u16string root;
            wtf8::decodeAppend(root, v.root);
            m_remote.emplace_back(wtf8::wview(root));
        } else if (local)
            m_roots.push_back(QString::fromStdString(v.root).toStdWString() + L'\\');
    }
    if (m_roots.empty() && m_remote.empty())
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
    return std::any_of(m_roots.begin(), m_roots.end(),
               [&](const std::wstring& root) {
                   return path.size() > root.size() && win32::equalsIgnoreCase(path.substr(0, root.size()), root);
               })
        // A share as configured ("\\wsl$\..." for the index's "\\wsl.localhost\...").
        || std::any_of(m_remote.begin(), m_remote.end(),
            [&](const std::wstring& root) { return unc::contains(root, path) && !unc::equal(root, path); });
}

std::uint64_t ContentIndexer::Intake::since() const noexcept
{
    return m_indexer ? m_indexer->m_content->changeSequence() : 0;
}

bool ContentIndexer::Intake::take(EntryId entry, const FileText& text, std::uint64_t since)
{
    if (!m_indexer || text.outcome == Outcome::Skipped)
        return false;
    std::shared_lock gate(m_indexer->m_intakeGate);
    {
        std::lock_guard lock(m_indexer->m_mutex);
        if (m_indexer->m_paused || !m_indexer->m_options.enabled || m_indexer->m_stopping.load())
            return false;
    }
    if (!m_indexer->m_content->add(
            entry, text.keys, text.outcome == Outcome::Empty, text.stamp, since, text.text, text.large))
        return false;
    ++m_taken;
    return true;
}

std::optional<std::uint32_t> ContentIndexer::stampOf(std::wstring_view path)
{
    const std::optional<FileStamp> file = statOf(path);
    return file ? std::optional(file->stamp) : std::nullopt;
}

} // namespace ws

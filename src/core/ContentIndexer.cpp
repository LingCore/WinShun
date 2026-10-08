#include "ContentIndexer.h"

#include "ContentScanner.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>

using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr auto kRetryDelay = 1min; // after a pass was cut short
constexpr std::size_t kBatch = 1024; // files read between merges and checks
constexpr int kThreads = 4;
constexpr std::size_t kChunkBytes = ContentScanner::kChunkBytes;

bool batterySaverOn()
{
    SYSTEM_POWER_STATUS status {};
    return ::GetSystemPowerStatus(&status) && status.SystemStatusFlag == 1;
}

// The volume root an entry is on, with a small cache by parent folder (files
// next to each other share it).
class RootFinder {
public:
    explicit RootFinder(const FileIndex& index)
        : m_index(index)
    {
        m_cache.fill({kNoEntry, kNoEntry});
    }
    EntryId rootOf(EntryId id)
    {
        const EntryId parent = m_index.entry(id).parent;
        if (parent == kNoEntry)
            return id;
        Slot& slot = m_cache[parent & (m_cache.size() - 1)];
        if (slot.parent != parent) {
            EntryId cur = parent;
            while (m_index.entry(cur).parent != kNoEntry)
                cur = m_index.entry(cur).parent;
            slot = {parent, cur};
        }
        return slot.root;
    }

private:
    struct Slot {
        EntryId parent;
        EntryId root;
    };
    const FileIndex& m_index;
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
        std::lock_guard lock(m_mutex);
        if (options == m_options)
            return;
        if (options.maxFileBytes > m_options.maxFileBytes)
            m_content->markEmptyChanged(); // files that were too large may fit now
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

bool ContentIndexer::interrupted(std::stop_token stop)
{
    if (stop.stop_requested() || m_interrupt.load())
        return true;
    std::lock_guard lock(m_mutex);
    return m_optionsChanged;
}

void ContentIndexer::run(std::stop_token stop)
{
    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, stop, m_timing.start, [] { return false; });
    }
    if (stop.stop_requested())
        return;
    for (;;) {
        Options options;
        {
            std::lock_guard lock(m_mutex);
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
// volume are left as they are.
std::vector<ContentIndexer::Work> ContentIndexer::findWork(const FileIndex& index, const Options& options)
{
    const ContentFilter filter {ExtensionFilter(options.extensions), options.includeLowPriority};
    const std::vector<Volume> volumes = m_source.volumes();
    std::vector<std::pair<EntryId, bool>> roots; // root, suspended
    for (const EntryId r : index.roots()) {
        const auto it = std::find_if(
            volumes.begin(), volumes.end(), [&](const Volume& v) { return v.root == index.name(r); });
        if (it != volumes.end())
            roots.emplace_back(r, it->suspended);
    }
    const std::vector<ContentIndex::DocInfo> docs = m_content->documents();
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_recentReads, [&](const auto& r) { return now - r.second >= m_timing.cooldown; });

    std::vector<Work> work;
    std::vector<EntryId> retired;
    RootFinder finder(index);
    std::size_t k = 0;
    for (std::size_t c = 0; c < index.chunkCount(); ++c) {
        const auto entries = index.chunk(c);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            if (!filter.accepts(index, e))
                continue;
            const auto id = static_cast<EntryId>((c << FileIndex::kChunkBits) + i);
            const EntryId root = finder.rootOf(id);
            const auto volume = std::find_if(roots.begin(), roots.end(), [&](const auto& r) { return r.first == root; });
            if (volume == roots.end())
                continue;
            for (; k < docs.size() && docs[k].entry < id; ++k)
                retired.push_back(docs[k].entry);
            const bool known = k < docs.size() && docs[k].entry == id;
            if (volume->second) { // suspended
                k += known ? 1 : 0;
                continue;
            }
            if (known) {
                const ContentIndex::DocInfo& d = docs[k++];
                if (d.need == ContentIndex::Need::Check)
                    work.push_back({id, true, false, d.stamp});
                else if (d.need == ContentIndex::Need::Read && !m_recentReads.contains(id))
                    work.push_back({id, false, true, 0});
            } else {
                work.push_back({id, false, false, 0});
            }
        }
    }
    for (; k < docs.size(); ++k)
        retired.push_back(docs[k].entry);
    m_content->retire(retired);
    return work;
}

bool ContentIndexer::pass(const Options& options, std::stop_token stop)
{
    const auto index = m_source.index();
    if (!index)
        return true;
    std::vector<Work> work;
    FileIndex::IdPin pin; // the ids in `work` stay valid: no compaction until the pass is over
    {
        const auto lock = index->readLock();
        pin = index->pinIds();
        if (index->generation() != m_generation) {
            m_recentReads.clear(); // ids from before a compaction
            m_generation = index->generation();
        }
        work = findWork(*index, options);
    }
    if (work.empty())
        return true;

    m_reading = true;
    const struct Done {
        std::atomic<bool>& flag;
        ~Done() { flag = false; }
    } done {m_reading};
    const auto cancelled = [&] { return stop.stop_requested() || m_interrupt.load(); };
    std::atomic<std::size_t> added {0};
    for (std::size_t begin = 0; begin < work.size(); begin += kBatch) {
        if (interrupted(stop) || batterySaverOn())
            return false;
        const std::size_t end = std::min(work.size(), begin + kBatch);
        std::atomic<std::size_t> next {begin};
        std::atomic<bool> full {false};
        std::mutex recentMutex;
        m_content->beginReads();
        {
            std::vector<std::jthread> workers;
            for (int t = 0; t < kThreads; ++t) {
                workers.emplace_back([&] {
                    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
                    std::wstring path;
                    const auto halted = [&] { return stop.stop_requested() || m_interrupt.load(); };
                    while (!halted() && !full.load()) {
                        while (m_content->searching() && !halted())
                            std::this_thread::sleep_for(100ms);
                        if (halted())
                            break;
                        const std::size_t i = next.fetch_add(1);
                        if (i >= end)
                            break;
                        const Work& w = work[i];
                        {
                            const auto lock = index->readLock();
                            if (index->entry(w.entry).isDeleted())
                                continue;
                            path = index->wpath(w.entry);
                        }
                        if (w.checkOnly) {
                            const auto stamp = stampOf(path);
                            if (!stamp)
                                continue; // gone or busy: the next pass sees
                            if (*stamp == w.stamp) {
                                m_content->confirm(w.entry);
                                continue;
                            }
                        }
                        const std::uint64_t since = m_content->changeSequence();
                        const FileText text = readFile(path, options.maxFileBytes, cancelled);
                        if (text.outcome == Outcome::Skipped)
                            continue;
                        if (m_content->add(w.entry, text.keys, text.outcome == Outcome::Empty, text.stamp, since))
                            ++added;
                        else
                            full = true;
                        if (w.again) {
                            std::lock_guard lock(recentMutex);
                            m_recentReads[w.entry] = std::chrono::steady_clock::now();
                        }
                    }
                });
            }
        }
        m_content->endReads();
        if (full.load() || interrupted(stop))
            return false;
        if (m_content->needsMerge())
            m_content->mergeDue();
    }
    // After reading many files (the first time, say): one segment again,
    // smaller than several, and the grams still in memory written out.
    if (added.load() >= kBatch) {
        const ContentIndex::Stats stats = m_content->stats();
        if (stats.segments + (stats.memoryPairs > 0 ? 1 : 0) > 1)
            m_content->merge();
    }
    ::HeapCompact(::GetProcessHeap(), 0); // hand back the read buffers and gram lists
    return true;
}

ContentIndexer::FileText ContentIndexer::readFile(
    std::wstring_view path, std::int64_t maxBytes, const std::function<bool()>& cancelled)
{
    FileText out;
    const win32::UniqueHandle file(::CreateFileW(win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file.valid()) {
        // No access will not change by itself; in use or gone may (and the
        // journal reports a file that is gone).
        if (::GetLastError() == ERROR_ACCESS_DENIED)
            out.outcome = Outcome::Empty;
        return out;
    }
    FILE_BASIC_INFO basic {};
    LARGE_INTEGER size {};
    if (!::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof basic)
        || !::GetFileSizeEx(file.get(), &size))
        return out;
    constexpr DWORD kNotHere = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN
        | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if (basic.FileAttributes & kNotHere)
        return out; // never download a cloud file
    out.stamp = ContentIndex::stampOf(size.QuadPart, basic.LastWriteTime.QuadPart);
    if (size.QuadPart <= 0 || (maxBytes > 0 && size.QuadPart > maxBytes)) {
        out.outcome = Outcome::Empty; // as ContentScanner::scanFile skips it
        return out;
    }
    FILE_IO_PRIORITY_HINT_INFO hint {};
    hint.PriorityHint = IoPriorityHintLow;
    ::SetFileInformationByHandle(file.get(), FileIoPriorityHintInfo, &hint, sizeof hint);

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

std::optional<std::uint32_t> ContentIndexer::stampOf(std::wstring_view path)
{
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!::GetFileAttributesExW(win32::longPath(path).c_str(), GetFileExInfoStandard, &data))
        return std::nullopt;
    const std::int64_t size = (static_cast<std::int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    const std::int64_t time
        = (static_cast<std::int64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return ContentIndex::stampOf(size, time);
}

} // namespace ws

#include "RemoteContent.h"

#include "ContentIndex.h"
#include "Documents.h"
#include "FileText.h"
#include "ShareLister.h"
#include "Unc.h"
#include "WebDav.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QtGlobal>

#include <windows.h>

#include <objbase.h>

#include <netlistmgr.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>

namespace ws::remotecontent {

namespace {

// stampOf() of a size and time as the index keeps them.
std::uint32_t stampAsListed(FileSize size, FileTime modified)
{
    constexpr std::int64_t kTicksPerSecond = 10'000'000;
    constexpr std::int64_t kSeconds1601To1970 = 11'644'473'600;
    const std::uint64_t bytes = size == kUnknownSize ? 0 : unpackSize(size);
    const std::int64_t ticks = (std::int64_t {modified} + kSeconds1601To1970) * kTicksPerSecond;
    return ContentIndex::stampOf(static_cast<std::int64_t>(bytes), ticks);
}

} // namespace

std::uint32_t stampOf(const FileIndex& index, EntryId id)
{
    return stampAsListed(index.size(id), index.modified(id));
}

std::optional<ReadStamp> stampAfterRead(FileIndex& index, EntryId id, std::int64_t bytes, std::int64_t written, Listed listed)
{
    const FileSize size = bytes < 0 ? kUnknownSize : packSize(static_cast<std::uint64_t>(bytes));
    const FileTime time = fileTimeOf(written);
    const auto unchanged = [&] { return index.size(id) == listed.size && index.modified(id) == listed.modified; };
    {
        const auto lock = index.readLock();
        if (index.entry(id).isDeleted())
            return std::nullopt;
        if (bytes < 0 || (index.size(id) == size && index.modified(id) == time))
            return ReadStamp {stampOf(index, id), false};
        if (!unchanged())
            return ReadStamp {stampAsListed(size, time), false};
    }
    const auto lock = index.writeLock();
    if (index.entry(id).isDeleted())
        return std::nullopt;
    if (!unchanged()) // changed meanwhile, after all
        return ReadStamp {stampAsListed(size, time), false};
    index.setSize(id, size);
    index.setModified(id, time);
    return ReadStamp {stampOf(index, id), true};
}

ReadError readErrorOf(unsigned long error) noexcept
{
    switch (error) {
    case NO_ERROR:
        return ReadError::None;
    case ERROR_FILE_TOO_LARGE:
        return ReadError::TooLarge;
    case ERROR_ACCESS_DENIED:
    case ERROR_CANT_ACCESS_FILE:
        return ReadError::Denied;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_DELETE_PENDING:
        return ReadError::Gone;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return ReadError::Busy;
    case ERROR_NOT_READY: // WebClient's, for a server that went away
        return ReadError::Network;
    default:
        // What stops a pass over the share's names (the server, the connection,
        // signing in: share::classify) stops reading its files too.
        return share::affectsShare(share::classify(error, false)) ? ReadError::Network : ReadError::Other;
    }
}

ContentFilter filterOf(const ContentIndexer::Options& options)
{
    return {ExtensionFilter(options.documents ? options.extensions + documentExtensions() : options.extensions), false};
}

Plan plan(const FileIndex& index, EntryId root, const ContentFilter& filter, const ContentSizeLimits& limits,
    std::uint64_t sizeCap, std::span<const ContentIndex::DocInfo> docs)
{
    Plan p;
    if (root >= index.slotCount() || index.entry(root).isDeleted())
        return p;
    const ExtensionFilter documents(documentExtensions());
    // Written to first (someone works on them), then the rest by depth (the
    // shallow ones are a person's own, mostly), text before documents (those
    // take the extractor), hidden ones, those in crowded folders last.
    enum Order : std::uint8_t { kAgain, kText, kDocument, kHidden, kCrowded };
    struct Found {
        EntryId entry;
        Order order;
        bool document;
    };
    std::vector<Found> found;
    std::vector<EntryId> fresh; // read for the first time: what makes a folder crowded
    std::deque<EntryId> folders {root}; // breadth first
    while (!folders.empty()) {
        const EntryId folder = folders.front();
        folders.pop_front();
        for (EntryId c = index.entry(folder).firstChild; c != kNoEntry; c = index.entry(c).nextSibling) {
            const Entry& e = index.entry(c);
            if (e.isDeleted())
                continue;
            if (e.isDir()) {
                folders.push_back(c);
                continue;
            }
            std::string_view name;
            std::size_t extLength = 0;
            if (filter.classify(index, e, name, extLength) != ContentVerdict::Remote)
                continue;
            ++p.files;
            const FileSize packed = index.size(c);
            const std::uint64_t bytes = packed == kUnknownSize ? 0 : unpackSize(packed);
            p.bytes += bytes;
            const std::uint32_t stamp = stampOf(index, c);
            const auto doc = std::lower_bound(docs.begin(), docs.end(), c,
                [](const ContentIndex::DocInfo& d, EntryId id) { return d.entry < id; });
            const bool known = doc != docs.end() && doc->entry == c;
            if (known)
                ++p.indexed;
            if (known && doc->stamp == stamp && doc->need != ContentIndex::Need::Read) {
                ++p.current;
                if (doc->need == ContentIndex::Need::Check)
                    p.confirm.push_back(c);
                continue;
            }
            const bool document = documents.matches(name, extLength);
            const std::uint64_t most = limitOf(
                limits.bytes[ContentSizeLimits::kindOf(name.substr(name.size() - extLength))], sizeCap, document);
            if (most > 0 && bytes > most) {
                p.tooLarge.push_back(c);
                ++p.current;
                continue;
            }
            found.push_back({c,
                known                                ? kAgain
                    : (e.flags & EntryFlag::Hidden) ? kHidden
                    : document                      ? kDocument
                                                    : kText,
                document});
            if (!known)
                fresh.push_back(c);
        }
    }
    if (!fresh.empty()) {
        const std::vector<EntryId> crowded = ContentIndexer::crowdedFolders(index, fresh);
        for (Found& f : found) {
            if (f.order != kAgain && std::binary_search(crowded.begin(), crowded.end(), index.entry(f.entry).parent))
                f.order = kCrowded;
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.order < b.order; });
    p.read.reserve(found.size());
    for (const Found& f : found)
        p.read.push_back({f.entry, f.document, f.order == kAgain});
    return p;
}

std::uint64_t limitOf(std::int64_t kindLimit, std::uint64_t sizeCap, bool document) noexcept
{
    std::uint64_t most = document ? 0 : kMaxTextBytes;
    for (const std::uint64_t limit : {static_cast<std::uint64_t>(std::max<std::int64_t>(0, kindLimit)), sizeCap}) {
        if (limit > 0 && (most == 0 || limit < most))
            most = limit;
    }
    return most;
}

namespace {

// What a thread of its own gets of a remote file (readFile): the part that
// Windows cannot call off. Opening a WebDAV file downloads it whole (up to
// 50 MB), and a server that went away holds a read for as long as the
// redirector waits. Let go of (letGo), the thread ends by itself and
// touches nothing but this.
struct Fetch {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false; // mutex
    std::atomic<bool> letGo {false};
    ContentIndexer::FileText text; // as opened (stamp, size, time), or why not
    bool whole = false; // read to the end: the rest is the reader's
    std::string bytes; // a text file's
    win32::UniqueHandle file; // a document's, read through once, for the extractor
};

void fetchInto(Fetch& f, const std::wstring& path, std::int64_t maxBytes, bool document)
{
    ContentIndexer::FileText& t = f.text;
    win32::UniqueHandle file(::CreateFileW(win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file.valid()) {
        t.error = ::GetLastError();
        const ReadError why = readErrorOf(t.error);
        if (why == ReadError::Denied || why == ReadError::TooLarge)
            t.outcome = ContentIndexer::Outcome::Empty; // will not change by itself
        t.large = why == ReadError::TooLarge;
        return;
    }
    FILE_BASIC_INFO basic {};
    LARGE_INTEGER size {};
    if (!::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof basic)
        || !::GetFileSizeEx(file.get(), &size)) {
        t.error = ::GetLastError();
        return;
    }
    constexpr DWORD kNotHere = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN
        | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if (basic.FileAttributes & kNotHere)
        return; // never download a cloud file
    t.stamp = ContentIndex::stampOf(size.QuadPart, basic.LastWriteTime.QuadPart);
    t.bytes = size.QuadPart;
    t.written = basic.LastWriteTime.QuadPart;
    if (size.QuadPart <= 0 || (maxBytes > 0 && size.QuadPart > maxBytes)
        || (!document && static_cast<std::uint64_t>(size.QuadPart) > kMaxTextBytes)) { // grown so since it was listed
        t.outcome = ContentIndexer::Outcome::Empty;
        t.large = size.QuadPart > 0;
        return;
    }
    if (document) {
        // On the file object: the extractor's reads through a copy of the handle have it too.
        FILE_IO_PRIORITY_HINT_INFO hint {};
        hint.PriorityHint = IoPriorityHintLow;
        ::SetFileInformationByHandle(file.get(), FileIoPriorityHintInfo, &hint, sizeof hint);
        // Into the file cache in one go first (ContentIndexer::HardDisk::on):
        // over the network, each of the extractor's jumps would be a round trip.
        std::vector<char> through(std::size_t {1} << 20);
        for (;;) {
            if (f.letGo)
                return;
            DWORD got = 0;
            if (!::ReadFile(file.get(), through.data(), static_cast<DWORD>(through.size()), &got, nullptr)) {
                t.error = ::GetLastError(); // the server went away, most likely: later
                return;
            }
            if (got == 0)
                break;
        }
        f.file = std::move(file);
        f.whole = true;
        return;
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t total = 0;
    while (total < bytes.size()) {
        if (f.letGo)
            return;
        const auto want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - total, 1 << 20));
        DWORD got = 0;
        if (!::ReadFile(file.get(), bytes.data() + total, want, &got, nullptr)) {
            t.error = ::GetLastError();
            return;
        }
        if (got == 0)
            break; // shorter than it was: what there is
        total += got;
    }
    bytes.resize(total);
    f.bytes = std::move(bytes);
    f.whole = true;
}

} // namespace

Read readFile(std::wstring_view path, std::int64_t maxBytes, bool document, DocExtractor* documents,
    const std::function<bool()>& cancelled)
{
    Read out;
    if (document && !documents)
        return out; // as ContentIndexer::readFile: skipped
    // Opened and read on a thread of its own, waited for until `cancelled`:
    // then let go of, so that stopping waits for no server (GitHub #114).
    const auto f = std::make_shared<Fetch>();
    try {
        std::thread([f, file = std::wstring(path), maxBytes, document] {
            fetchInto(*f, file, maxBytes, document);
            {
                std::lock_guard lock(f->mutex);
                f->done = true;
            }
            f->cv.notify_all();
        }).detach();
    } catch (const std::system_error&) {
        return out; // no thread to be had now: later
    }
    {
        std::unique_lock lock(f->mutex);
        while (!f->cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return f->done; })) {
            if (cancelled && cancelled()) {
                f->letGo = true;
                return out;
            }
        }
    }
    ContentIndexer::FileText& t = out.text;
    t = std::move(f->text);
    if (!f->whole)
        return out;
    const std::uint32_t stamp = t.stamp;
    const std::int64_t bytes = t.bytes;
    const std::int64_t written = t.written;
    const auto opened = [&](ContentIndexer::FileText& x) {
        x.stamp = stamp;
        x.bytes = bytes;
        x.written = written;
    };
    if (document) {
        t = ContentIndexer::extractDocument(f->file.get(), static_cast<std::uint64_t>(bytes), documents, cancelled);
        opened(t);
        return out;
    }
    const std::string& data = f->bytes;
    // The grams as a search's read of it takes them (ContentIndexer::collectText).
    t = ContentIndexer::collectText(data, stamp);
    opened(t);
    if (t.outcome != ContentIndexer::Outcome::Indexed)
        return out;
    const filetext::Head head = filetext::decodeHead(data, data.size());
    if (head.status != filetext::Status::Ok) {
        t.outcome = ContentIndexer::Outcome::Empty; // binary: nothing to find
        t.keys.clear();
        return out;
    }
    doctext::DocText text;
    text.text = head.text.toStdString(); // UTF-8, "\n" line ends
    if (!text.text.empty() && text.text.back() != '\n')
        text.text.push_back('\n'); // as a document's: every line ends so
    t.text = ContentIndex::packText(text);
    return out;
}

bool meteredNetwork()
{
    if (qEnvironmentVariableIntValue("WINSHUN_METERED") == 1)
        return true;
    using Clock = std::chrono::steady_clock;
    static std::mutex mutex;
    static Clock::time_point askedAt {};
    static bool metered = false;
    std::lock_guard lock(mutex);
    if (askedAt != Clock::time_point {} && Clock::now() - askedAt < std::chrono::seconds(30))
        return metered;
    askedAt = Clock::now();
    metered = false;
    // COM for the call alone: the share threads are ours.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    INetworkCostManager* costs = nullptr;
    if (SUCCEEDED(::CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&costs)))) {
        DWORD cost = 0;
        if (SUCCEEDED(costs->GetCost(&cost, nullptr))) {
            metered = (cost
                          & (NLM_CONNECTION_COST_FIXED | NLM_CONNECTION_COST_VARIABLE | NLM_CONNECTION_COST_OVERDATALIMIT
                              | NLM_CONNECTION_COST_ROAMING))
                != 0;
        }
        costs->Release();
    }
    if (SUCCEEDED(com))
        ::CoUninitialize();
    return metered;
}

namespace {

// This computer itself: "localhost", "127.0.0.1", WebClient's "127.0.0.1@8714".
bool loopbackHost(std::wstring_view host) noexcept
{
    const std::wstring_view name = host.substr(0, host.find(L'@'));
    return win32::equalsIgnoreCase(name, L"localhost") || name.starts_with(L"127.");
}

} // namespace

ShareContent::ShareContent(std::wstring path, Clock::duration readAgain, Clock::duration failedAgain)
    : m_path(std::move(path))
    , m_webdav(webdav::isWebDavPath(m_path))
    , m_local(!unc::wslDistribution(m_path).empty() || loopbackHost(unc::host(m_path)))
    , m_readAgain(readAgain)
    , m_failedAgain(failedAgain)
{
}

std::optional<ShareContent::Clock::duration> ShareContent::holdBack(const Source& s, Plan& todo)
{
    const std::uint64_t generation = s.index->generation();
    if (m_readAgainIndex.owner_before(s.index) || s.index.owner_before(m_readAgainIndex)
        || generation != m_readAgainGeneration) {
        m_readAgainAt.clear(); // the ids of another index, or from before a compaction
        m_failed.clear();
        m_readAgainIndex = s.index;
        m_readAgainGeneration = generation;
    }
    const auto now = Clock::now();
    std::erase_if(m_readAgainAt, [&](const auto& r) { return now - r.second >= m_readAgain; });
    std::optional<Clock::duration> due;
    const auto later = [&](Clock::duration left) { due = due ? std::min(*due, left) : left; };
    std::erase_if(todo.read, [&](const Plan::File& f) {
        if (const auto it = f.again ? m_readAgainAt.find(f.entry) : m_readAgainAt.end(); it != m_readAgainAt.end()) {
            later(it->second + m_readAgain - now);
            return true;
        }
        if (const auto it = m_failed.find(f.entry); it != m_failed.end() && now < it->second.again) {
            later(it->second.again - now);
            return true;
        }
        return false;
    });
    return due;
}

bool ShareContent::failed(EntryId entry, std::uint32_t stamp)
{
    Failed& f = m_failed[entry];
    if (f.stamp != stamp)
        f = {stamp, 0, {}}; // written to since: afresh
    if (++f.times >= kTries) {
        m_failed.erase(entry);
        return true;
    }
    f.again = Clock::now() + m_failedAgain * (1 << (f.times - 1));
    return false;
}

void ShareContent::tell(const Source& source, bool now)
{
    if (m_progress == m_told || !source.progress)
        return;
    if (!now && Clock::now() - m_toldAt < std::chrono::seconds(1))
        return;
    m_told = m_progress;
    m_toldAt = Clock::now();
    source.progress(m_progress);
}

bool ShareContent::planOf(EntryId root, const Source& s, const ContentIndexer::Options& options, Plan& todo,
    FileIndex::IdPin* pin, std::wstring* rootPath)
{
    const FileIndex& index = *s.index;
    const auto lock = index.longReadLock(); // through all of the share: writers wait at their gate, not searches
    if (root >= index.slotCount() || index.entry(root).isDeleted() || index.entry(root).parent != kNoEntry)
        return false;
    std::u16string name; // wpath() would end in '\'
    wtf8::decodeAppend(name, index.name(root));
    std::wstring path(name.begin(), name.end());
    // Found before the lock was taken: renumbered since (a compaction), it is looked for again.
    if (!unc::equal(path, m_path))
        return false;
    if (pin)
        *pin = index.pinIds();
    if (rootPath)
        *rootPath = std::move(path);
    const std::vector<ContentIndex::DocInfo> docs = s.indexer->content()->documents(); // as findWork: under the lock
    todo = plan(index, root, filterOf(options), options.sizeLimits, m_webdav ? webdav::fileSizeLimit() : 0, docs);
    m_progress.files = todo.files;
    m_progress.current = todo.current;
    m_progress.indexed = todo.indexed;
    m_progress.bytes = todo.bytes;
    m_progress.known = true;
    return true;
}

void ShareContent::count(EntryId root, const Source& s)
{
    const ContentIndexer::Options options = s.indexer->options();
    Plan todo;
    if (options.enabled && planOf(root, s, options, todo, nullptr, nullptr))
        tell(s, true);
}

std::optional<ShareContent::Clock::duration> ShareContent::slice(std::stop_token stop, EntryId root, const Source& s)
{
    using namespace std::chrono_literals;
    constexpr Clock::duration kSliceTime = 20s; // then the thread looks at its other work
    constexpr Clock::duration kHeld = 5s; // the gates looked at again
    constexpr Clock::duration kLater = 60s; // metered, or the server failed
    ContentIndexer& indexer = *s.indexer;
    FileIndex& index = *s.index;
    const auto finish = [&](std::optional<Clock::duration> again) {
        m_progress.reading = false;
        tell(s, true);
        return again;
    };
    const ContentIndexer::Options options = indexer.options();
    if (!options.enabled) { // content search turned off: setOptions wakes the thread when it is on again
        m_progress = {};
        return finish(std::nullopt);
    }

    // Names keep coming on a metered connection; contents wait for another.
    // Not for WSL and this computer's own shares, which use no network.
    const bool metered = qEnvironmentVariableIntValue("WINSHUN_METERED") == 1 || (!m_local && meteredNetwork());
    // On hold with files left, as the last plan counted them: not planned
    // again for nothing every few seconds (the search box open a while, a
    // metered connection), all of the share gone through under the lock.
    if (m_progress.known) {
        const std::size_t left = m_progress.files - std::min(m_progress.current, m_progress.files);
        if (metered) {
            m_progress.metered = true;
            return finish(kLater);
        }
        if (left > 0 && !indexer.mayReadRemote(left <= kFew)) {
            m_progress.metered = false;
            m_progress.held = true;
            return finish(kHeld);
        }
    }

    // What there is to do, from the index alone. Its ids are pinned for the
    // slice: no compaction renumbers them in between.
    const std::uint64_t cap = m_webdav ? webdav::fileSizeLimit() : 0;
    Plan todo;
    FileIndex::IdPin pin;
    std::wstring rootPath;
    if (!planOf(root, s, options, todo, &pin, &rootPath))
        return finish(Clock::duration::zero());
    m_progress.metered = metered; // counted: the card says why nothing is read
    if (m_progress.metered)
        return finish(kLater);
    // Read again lately and written to since: back for them when they are due.
    std::optional<Clock::duration> due = holdBack(s, todo);
    if (todo.read.empty() && todo.tooLarge.empty() && todo.confirm.empty()) {
        m_progress.held = false;
        return finish(due);
    }
    for (const EntryId e : todo.confirm)
        indexer.content()->confirm(e); // unsure, and unchanged as listed: current

    const bool few = todo.read.size() <= kFew;
    m_progress.held = !indexer.mayReadRemote(few);
    if (m_progress.held)
        return finish(kHeld);
    ContentIndexer::Intake intake(&indexer);
    if (!intake.wants(rootPath + L"\\a"))
        return finish(30s); // not followed now: offline, read again, just turned on
    // WebClient stopped: Windows would have none of the files there (GitHub #114).
    if (m_webdav && !webdav::startClient(std::chrono::minutes(1), stop))
        return finish(kLater);
    m_progress.reading = true;
    m_progress.error = 0;
    tell(s, true);

    // A file's stamp is the index's (stampOf): its size and time as listed;
    // written to since, the file's (stampAfterRead). False: gone.
    const auto stampFor = [&](EntryId e, std::int64_t bytes, std::int64_t written, std::uint32_t& stamp, Listed listed) {
        const std::optional<ReadStamp> got = stampAfterRead(index, e, bytes, written, listed);
        if (!got)
            return false;
        stamp = got->stamp;
        if (got->wrote && s.dirty)
            s.dirty();
        return true;
    };

    // Too large as listed: recorded so, unread (a search counts them; the
    // plan counted them current already).
    const std::uint64_t listedSince = intake.since();
    for (const EntryId e : todo.tooLarge) {
        ContentIndexer::FileText t;
        t.outcome = ContentIndexer::Outcome::Empty;
        t.large = true;
        const bool known = indexer.content()->knows(e); // counted by the plan then
        if (stampFor(e, -1, 0, t.stamp, {}) && intake.take(e, t, listedSince) && !known)
            ++m_progress.indexed;
    }

    const auto started = Clock::now();
    const auto cancelled = [&] { return stop.stop_requested() || indexer.stopping(); };
    int errors = 0;
    for (const Plan::File& f : todo.read) {
        if (cancelled())
            return finish(Clock::duration::zero());
        if (!indexer.mayReadRemote(few)) {
            m_progress.held = true;
            return finish(kHeld);
        }
        if (s.online && !s.online())
            return finish(Clock::duration::zero()); // a pass began, or the server went away: the caller waits for it
        std::wstring path;
        Listed listed;
        {
            const auto lock = index.readLock();
            if (index.entry(f.entry).isDeleted())
                continue;
            path = index.wpath(f.entry);
            listed = {index.size(f.entry), index.modified(f.entry)};
        }
        const auto maxBytes = static_cast<std::int64_t>(limitOf(options.sizeLimits.of(path), cap, f.document));
        const std::uint64_t since = intake.since();
        const auto fileStarted = Clock::now();
        Read read = readFile(path, maxBytes, f.document, indexer.documents(), cancelled);
        if (cancelled())
            return finish(Clock::duration::zero());
        if (readErrorOf(read.text.error) == ReadError::Network) {
            if (++errors >= kErrorsInARow) {
                m_progress.error = read.text.error; // the share's own check decides what that means
                return finish(kLater);
            }
            continue;
        }
        errors = 0;
        if (read.text.outcome != ContentIndexer::Outcome::Skipped) {
            // Gone since (stampFor): the next listing tells.
            if (stampFor(f.entry, read.text.bytes, read.text.written, read.text.stamp, listed)
                && intake.take(f.entry, read.text, since)) {
                m_failed.erase(f.entry);
                ++m_progress.current;
                if (f.again)
                    m_readAgainAt[f.entry] = Clock::now();
                else
                    ++m_progress.indexed;
            }
        } else if (read.text.error != 0 || read.text.bytes < 0) {
            // Not read for itself (in use, a cloud file, an error of its
            // own), not for the server: later and later, then unreadable
            // (nothing to find until it is written to), not every round.
            // Opened fine (no extractor free): simply later.
            ContentIndexer::FileText none;
            none.outcome = ContentIndexer::Outcome::Empty;
            if (!failed(f.entry, stampAsListed(listed.size, listed.modified))) {
                const Clock::duration left = m_failed[f.entry].again - Clock::now(); // back for it then
                due = due ? std::min(*due, left) : left;
            } else if (stampFor(f.entry, -1, 0, none.stamp, listed) && intake.take(f.entry, none, since)) {
                ++m_progress.current;
                if (!f.again)
                    ++m_progress.indexed;
            }
        }
        tell(s, false);
        if (Clock::now() - started >= kSliceTime)
            return finish(Clock::duration::zero());
        // Half speed while someone uses the computer: a pause as long as the file took.
        if (!win32::userAway() && s.pause && !s.pause(Clock::now() - fileStarted))
            return finish(Clock::duration::zero());
    }
    return finish(due);
}

struct Indexer::Share {
    std::wstring path;
    Status status; // m_mutex
    bool woken = false; // m_mutex: wake()
    bool nudged = false; // m_mutex: statusesChanged()
    std::atomic<bool> done {false}; // its thread has ended
    std::condition_variable_any cv; // with m_mutex
    std::jthread thread;
};

namespace {

// The root entry of the share at `path`; kNoEntry: not in the index (yet).
// Read lock held.
EntryId rootOf(const FileIndex& index, const std::wstring& path)
{
    for (const EntryId r : index.roots()) {
        if (index.entry(r).isDeleted())
            continue;
        std::u16string wide;
        wtf8::decodeAppend(wide, index.name(r));
        if (unc::equal(wtf8::wview(wide), path))
            return r;
    }
    return kNoEntry;
}

} // namespace

Indexer::Indexer(Source source, Callbacks callbacks, Times times)
    : m_source(std::move(source))
    , m_callbacks(std::move(callbacks))
    , m_times(times)
{
}

Indexer::~Indexer()
{
    stop();
}

void Indexer::start()
{
    std::lock_guard lock(m_mutex);
    if (m_started || m_stopped)
        return;
    m_started = true;
    for (const std::unique_ptr<Share>& share : m_shares)
        launch(*share);
}

void Indexer::launch(Share& share)
{
    share.thread = std::jthread([this, &share](std::stop_token stop) {
        run(stop, share);
        share.done = true;
    });
}

void Indexer::setShares(const std::vector<std::wstring>& paths)
{
    bool changed = false;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopped)
            return;
        std::erase_if(m_retired, [](const std::unique_ptr<Share>& share) { return share->done.load(); }); // joined at once
        for (auto it = m_shares.begin(); it != m_shares.end();) {
            if (std::ranges::any_of(paths, [&](const std::wstring& p) { return unc::equal(p, (*it)->path); })) {
                ++it;
                continue;
            }
            (*it)->thread.request_stop(); // its slice ends after the file it is in
            m_retired.push_back(std::move(*it));
            it = m_shares.erase(it);
            changed = true;
        }
        for (const std::wstring& path : paths) {
            if (std::ranges::any_of(m_shares, [&](const std::unique_ptr<Share>& s) { return unc::equal(s->path, path); }))
                continue;
            auto share = std::make_unique<Share>();
            share->path = path;
            share->status.path = path;
            if (m_started)
                launch(*share);
            m_shares.push_back(std::move(share));
            changed = true;
        }
    }
    if (changed && m_callbacks.changed)
        m_callbacks.changed();
}

void Indexer::wake()
{
    std::lock_guard lock(m_mutex);
    for (const std::unique_ptr<Share>& share : m_shares) {
        share->woken = true;
        share->cv.notify_all();
    }
}

void Indexer::wake(const std::wstring& path)
{
    std::lock_guard lock(m_mutex);
    for (const std::unique_ptr<Share>& share : m_shares) {
        if (!unc::equal(share->path, path))
            continue;
        share->woken = true;
        share->cv.notify_all();
    }
}

void Indexer::statusesChanged()
{
    // Each thread asks about its own share (Source::online): not here, where
    // the share indexer may hold its lock while it tells.
    std::lock_guard lock(m_mutex);
    for (const std::unique_ptr<Share>& share : m_shares) {
        share->nudged = true;
        share->cv.notify_all();
    }
}

void Indexer::requestStop()
{
    std::lock_guard lock(m_mutex);
    m_stopped = true;
    for (const std::unique_ptr<Share>& share : m_shares)
        share->thread.request_stop();
}

void Indexer::stop()
{
    requestStop();
    std::vector<std::unique_ptr<Share>> shares;
    {
        std::lock_guard lock(m_mutex);
        shares = std::move(m_shares);
        m_shares.clear();
        std::ranges::move(m_retired, std::back_inserter(shares));
        m_retired.clear();
    }
    for (const std::unique_ptr<Share>& share : shares) {
        if (share->thread.joinable())
            share->thread.join();
    }
}

std::vector<Indexer::Status> Indexer::statuses() const
{
    std::lock_guard lock(m_mutex);
    std::vector<Status> out;
    out.reserve(m_shares.size());
    for (const std::unique_ptr<Share>& share : m_shares)
        out.push_back(share->status);
    return out;
}

void Indexer::setProgress(Share& share, const ShareContent::Progress& progress)
{
    {
        std::lock_guard lock(m_mutex);
        if (share.status.progress == progress)
            return;
        share.status.progress = progress;
    }
    if (m_callbacks.changed)
        m_callbacks.changed();
}

Indexer::Wake Indexer::wait(std::stop_token stop, Share& share, Clock::time_point until)
{
    std::unique_lock lock(m_mutex);
    const bool called = share.cv.wait_until(lock, stop, until, [&] { return share.woken || share.nudged; });
    if (called) // a moment for the rest of a burst of changes
        share.cv.wait_for(lock, stop, m_times.settle, [] { return false; });
    const Wake why = stop.stop_requested() ? Wake::Stop
        : share.woken                      ? Wake::Woken
        : called                           ? Wake::Nudged
                                           : Wake::Time;
    share.woken = false;
    share.nudged = false;
    return why;
}

void Indexer::run(std::stop_token stop, Share& share)
{
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ShareContent content(share.path, m_times.readAgain);
    ShareContent::Source s;
    s.indexer = m_source.indexer;
    s.pause = [&](Clock::duration d) { // stopped only: a wake is for the next slice
        std::unique_lock lock(m_mutex);
        share.cv.wait_for(lock, stop, d, [] { return false; });
        return !stop.stop_requested();
    };
    s.progress = [&](const ShareContent::Progress& p) { setProgress(share, p); };
    s.dirty = m_callbacks.dirty;
    s.online = [&] { return !m_source.online || m_source.online(share.path); };
    // What the last look went by. Nudged (statusesChanged) with all of it as
    // it was, the share has nothing new to read: another share's state
    // changed, or its own check is under way, or it is tried again while it
    // stays offline (once a minute or so, GitHub #116).
    struct Seen {
        std::weak_ptr<FileIndex> index; // a full rebuild replaces it
        EntryId root = kNoEntry;
        bool online = false;
        bool enabled = false;
        bool same(const Seen& o) const noexcept
        {
            return !index.owner_before(o.index) && !o.index.owner_before(index) && root == o.root
                && online == o.online && enabled == o.enabled;
        }
    };
    std::optional<Seen> seen;
    auto next = Clock::now(); // the next look, unless called sooner: a nudge puts it off no more than it brings it on
    Wake why = Wake::Time;
    while (!stop.stop_requested()) {
        if (why == Wake::Nudged && Clock::now() >= next)
            why = Wake::Time; // its time came meanwhile
        Seen now;
        now.online = s.online();
        now.enabled = s.indexer->enabled();
        s.index = m_source.index(); // the index of the moment
        if (s.index) {
            const auto lock = s.index->readLock();
            now.root = rootOf(*s.index, share.path);
        }
        now.index = s.index;
        if (why != Wake::Nudged || !seen || !seen->same(now)) {
            std::optional<Clock::duration> again;
            if (now.root != kNoEntry && now.online)
                again = content.slice(stop, now.root, s);
            else if (now.root != kNoEntry && !content.progress().known)
                content.count(now.root, s); // what of it is indexed, while it cannot be read (offline after a start)
            next = Clock::now() + again.value_or(m_times.lookAgain);
            seen = std::move(now);
        }
        s.index.reset();
        why = wait(stop, share, next);
        if (why == Wake::Stop)
            break;
    }
}

} // namespace ws::remotecontent

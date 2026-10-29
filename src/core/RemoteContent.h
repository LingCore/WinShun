#pragma once

#include "ContentIndex.h"
#include "ContentIndexer.h"
#include "FileIndex.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ws {
class DocExtractor;
}

// The contents of files on network shares and in WSL (GitHub #114): read
// on a thread of each share's own (Indexer), with the user's credentials,
// into the content index the local disks use, a file at a time. No change
// journal there: a document is current while its file's size and time, as
// the share's listing has them, are what they were when it was read.
namespace ws::remotecontent {

// A file's stamp (ContentIndex::stampOf) from what the index has of it: its
// size (packed: exact to 16 MB) and its time (in seconds), not the file's
// own, so that a listing that finds them unchanged leaves it current.
std::uint32_t stampOf(const FileIndex& index, EntryId id);

// The stamp for the text of `id` read through a handle that had `bytes` and
// `written` (FILETIME ticks; bytes < 0: no handle, the index's stamp). The
// file written to since it was listed, the index takes the handle's size and
// time (`wrote`), as the next listing would: when it still has what it had
// before the read (`listed`). Changed meanwhile (a listing, the share's
// watcher telling of a later write, GitHub #114), the index stands and the
// stamp is the handle's, so that the text is read again. Takes the locks
// itself; nullopt: the entry is gone.
struct Listed {
    FileSize size = kUnknownSize;
    FileTime modified = 0;
};
struct ReadStamp {
    std::uint32_t stamp = 0;
    bool wrote = false;
};
std::optional<ReadStamp> stampAfterRead(FileIndex& index, EntryId id, std::int64_t bytes, std::int64_t written, Listed listed);

// Why a read failed, and what that means for the file and the share.
enum class ReadError : std::uint8_t {
    None,
    TooLarge, // WebClient's size limit (223): not read, counted as too large
    Denied, // nothing to find for this user
    Gone, // deleted or renamed since it was listed: the next listing tells
    Busy, // open elsewhere without sharing: later
    Network, // the server or the connection: stop for now, the share's check decides
    Other,
};
ReadError readErrorOf(unsigned long error) noexcept;

// The files a content search looks in, as the indexer's options have them.
ContentFilter filterOf(const ContentIndexer::Options& options);

// What a share's thread is to do about its files' contents, found in the
// index alone (no file opened). Candidates are the files under `root` a
// content search looks in (ContentVerdict::Remote); those without a current
// document are read: none yet, a stamp the listing no longer matches (the
// file was written to), or one the index wants read again (a larger size
// limit). Ones larger than their limit as listed are recorded so unread.
struct Plan {
    struct File {
        EntryId entry;
        bool document = false; // read by the DocExtractor
        bool again = false; // read before, written to since
    };
    std::vector<File> read; // in the order to read them: written to first, then shallow, documents, hidden, crowded
    std::vector<EntryId> tooLarge;
    std::vector<EntryId> confirm; // unsure (ContentIndex::Need::Check) and unchanged: current
    std::size_t files = 0; // candidates
    std::size_t current = 0; // ... with a current document (or recorded as too large)
    std::size_t indexed = 0; // ... with a document, current or not (ContentIndex::Stats::documents counts it)
    std::uint64_t bytes = 0; // ... how large, as listed
};
// Read lock held. `docs`: ContentIndex::documents(). `sizeCap`: a limit for
// every file on top of `limits` (WebClient's; 0: none); a text file is never
// read past kMaxTextBytes either (limitOf).
Plan plan(const FileIndex& index, EntryId root, const ContentFilter& filter, const ContentSizeLimits& limits,
    std::uint64_t sizeCap, std::span<const ContentIndex::DocInfo> docs);

// A remote text file is read whole, and its text kept: one larger than this
// is recorded as too large, whatever its kind's limit (which goes up to 4 GB).
// A search reads no more of a local one either (FileText).
inline constexpr std::uint64_t kMaxTextBytes = std::uint64_t {16} << 20;

// The most of a remote file that is read, in bytes (0: no limit): the
// smallest of its kind's limit (the user's, ContentSizeLimits), `sizeCap`
// (WebClient's; 0: none) and, for a text file, kMaxTextBytes. A larger
// file is recorded as too large.
std::uint64_t limitOf(std::int64_t kindLimit, std::uint64_t sizeCap, bool document) noexcept;

// One remote file read for the index. A text file's text is kept with its
// grams (packed in FileText::text), decoded as a search decodes it, so that
// searches and their snippets need no network, nor the server at all. A
// document is read as ContentIndexer::readFile() reads one, through the
// file cache first: the extractor jumps about in it. A read the server cut
// short leaves the file for later (FileText::error). The file is opened and
// read on a thread of its own, waited for until `cancelled` says stop, and
// then let go of: Windows cannot call off an open or a read on a server
// that went away, and stopping waits for none. The process itself, though,
// ends only when that call does (WebClient gives a stalled request 60 s):
// its windows and instance mutex are gone at once, one thread stays in the
// kernel, and neither CancelSynchronousIo nor TerminateProcess shortens it
// (measured 2026-10-29).
struct Read {
    ContentIndexer::FileText text; // stamp: the opened file's (callers use stampOf())
};
Read readFile(std::wstring_view path, std::int64_t maxBytes, bool document, DocExtractor* documents,
    const std::function<bool()>& cancelled);

// Whether the connection to the network is metered (a phone's hotspot, a
// connection set so in Windows): files on shares are not read then, names
// still are. WINSHUN_METERED=1 pretends so (tests). Remembered for 30 seconds.
bool meteredNetwork();

// A share's files' contents (ShareConfig::content), read on a thread of the
// share's own (Indexer) while its names are current: a slice at a time, one
// file at a time, at half speed while someone uses the computer. A slice
// ends when all is read, the indexer's gates close (the search box, a content
// search, battery saver, paused), the share is no longer Online, the thread
// has something else to do, or the server fails three times in a row.
// Through ContentIndexer::Intake, so nothing is kept while the share is not
// followed (offline, mid-pass).
class ShareContent {
public:
    using Clock = std::chrono::steady_clock;
    struct Progress {
        std::size_t files = 0; // under the share, of the types a content search reads
        std::size_t current = 0; // ... their contents indexed (or known to be too large)
        // ... with a document in the content index, current or not: of the
        // documents ContentIndex::Stats counts, how many are this share's.
        std::size_t indexed = 0;
        std::uint64_t bytes = 0; // the files', as listed
        bool reading = false; // a slice runs
        bool metered = false; // on hold: the connection is metered
        bool held = false; // on hold: the search box, a content search, battery saver
        unsigned long error = 0; // the network error that ended the last slice; 0: none
        bool known = false; // the counts are there: a slice has looked
        bool operator==(const Progress&) const = default;
    };
    struct Source {
        std::shared_ptr<FileIndex> index;
        ContentIndexer* indexer = nullptr;
        // Waits up to the time given; false: the thread has something else to do.
        std::function<bool(Clock::duration)> pause;
        std::function<void(const Progress&)> progress; // as it goes, every second or so
        std::function<void()> dirty; // a file's size or time in the index was updated
        std::function<bool()> online; // asked before each file: false ends the slice; empty: always
    };
    // A file written to since it was read again is read again once more no
    // sooner than this (a log being written, a file being copied in: a
    // share's watcher tells of each write), as on the local disks
    // (ContentIndexer::Timing::cooldown). The first time, at once.
    static constexpr Clock::duration kReadAgain = std::chrono::minutes(10);
    // `path`: the share's (ShareConfig::path). `failedAgain`: kFailedAgain
    // (shorter in tests).
    explicit ShareContent(std::wstring path, Clock::duration readAgain = kReadAgain, Clock::duration failedAgain = kFailedAgain);
    // One slice from `root` (the share's root entry). When to come back for
    // the next; nullopt: all is read (until a pass or a change).
    std::optional<Clock::duration> slice(std::stop_token stop, EntryId root, const Source& source);
    // The counts alone, from the index (no file opened): for a share that is
    // not read now (offline), what of it is indexed.
    void count(EntryId root, const Source& source);
    const Progress& progress() const noexcept { return m_progress; }

    static constexpr std::size_t kFew = 10; // read even while the search box is open
    static constexpr int kErrorsInARow = 3;
    // A file not read for itself (in use, a cloud file, an error of its
    // own; not the server's, kErrorsInARow) is tried again after
    // kFailedAgain, then twice that, and after kTries in all it is
    // recorded as unreadable: nothing to find until it is written to. Not
    // opened every few minutes for ever, the share's count not stuck short.
    static constexpr int kTries = 3;
    static constexpr Clock::duration kFailedAgain = std::chrono::minutes(2);

private:
    // The plan for `root` (index read lock taken), the counts set from it;
    // false: `root` is not the share's (gone, or renumbered: looked for again).
    bool planOf(EntryId root, const Source& source, const ContentIndexer::Options& options, Plan& todo,
        FileIndex::IdPin* pin, std::wstring* rootPath);
    // Takes the files read again lately out of `todo.read` (kReadAgain),
    // and those that failed lately (kFailedAgain); how long until the first
    // of them is due, none: none was taken out. Ids pinned.
    std::optional<Clock::duration> holdBack(const Source& source, Plan& todo);
    // `entry` (as listed: `stamp`) could not be read once more; true: that
    // was the last try (kTries), it is to be recorded as unreadable.
    bool failed(EntryId entry, std::uint32_t stamp);
    void tell(const Source& source, bool now);

    std::wstring m_path;
    bool m_webdav = false;
    bool m_local = false; // WSL, or this computer's own share: never metered
    Clock::duration m_readAgain;
    Clock::duration m_failedAgain;
    // Files read again lately, and when: by their ids in one index (a
    // compaction renumbers them, a full rebuild replaces it).
    std::unordered_map<EntryId, Clock::time_point> m_readAgainAt;
    struct Failed {
        std::uint32_t stamp = 0; // as listed when it failed: written to since, it starts afresh
        int times = 0;
        Clock::time_point again {}; // not before
    };
    std::unordered_map<EntryId, Failed> m_failed; // by ids in the same index as m_readAgainAt
    std::weak_ptr<FileIndex> m_readAgainIndex;
    std::uint64_t m_readAgainGeneration = 0;
    Progress m_progress;
    Progress m_told;
    Clock::time_point m_toldAt {};
};

// The shares whose contents are indexed, each read (ShareContent) on a thread
// of its own while the share is Online: not while its names are read, nor
// while it is offline. Not on the share indexer's threads, which keep to the
// names: those of one share are read again in the meantime, as they would be
// while a search reads its files. A share's thread looks again when woken
// (names came in, the options changed), when the share indexer's statuses
// changed and its own share's with them (Online or no longer, another root:
// not for a check under way or a retry while it stays offline, GitHub #116),
// a settled moment later so that a burst is looked at once, and every few
// minutes anyway. Threads are asked to stop by requestStop() and joined by
// stop(), after the snapshot is saved. That waits for no server: a file
// being opened or read (which Windows cannot call off: a WebDAV download, a
// server that went away) is on a thread of its own (readFile), let go of;
// the process lingers until that call returns (see readFile).
class Indexer {
public:
    using Clock = ShareContent::Clock;
    struct Source {
        std::function<std::shared_ptr<FileIndex>()> index;
        ContentIndexer* indexer = nullptr; // outlives this
        // Whether the share at `path` is Online (ShareIndexer::State): read, and no pass running.
        std::function<bool(const std::wstring& path)> online;
    };
    struct Callbacks {
        std::function<void()> changed; // statuses() differ; any thread
        std::function<void()> dirty; // the index changed: save it in a while
    };
    struct Status {
        std::wstring path;
        ShareContent::Progress progress;
        bool operator==(const Status&) const = default;
    };
    struct Times {
        Clock::duration settle = std::chrono::seconds(3); // after a wake
        Clock::duration lookAgain = std::chrono::minutes(2); // all read: for files that came in live
        Clock::duration readAgain = ShareContent::kReadAgain;
    };

    Indexer(Source source, Callbacks callbacks, Times times = {});
    ~Indexer(); // stop()
    Indexer(const Indexer&) = delete;
    Indexer& operator=(const Indexer&) = delete;

    void start();
    // The shares whose contents are indexed (ShareConfig::path): threads
    // started for new ones, ended for those gone.
    void setShares(const std::vector<std::wstring>& paths);
    void wake(); // the options changed: every share looks again
    // Names or sizes of the share at `path` (ShareConfig::path) came in (a
    // pass, a change the server told of): that share alone looks again.
    void wake(const std::wstring& path);
    // The share indexer's statuses changed (ShareIndexer::Callbacks::changed):
    // a share looks again if its own did, else this costs it nothing.
    void statusesChanged();
    void requestStop();
    void stop();
    std::vector<Status> statuses() const;

private:
    struct Share;
    enum class Wake : std::uint8_t {
        Stop,
        Time, // the time given is up
        Woken, // wake()
        Nudged, // statusesChanged()
    };
    void launch(Share& share);
    void run(std::stop_token stop, Share& share);
    Wake wait(std::stop_token stop, Share& share, Clock::time_point until);
    void setProgress(Share& share, const ShareContent::Progress& progress);

    Source m_source;
    Callbacks m_callbacks;
    const Times m_times;
    mutable std::mutex m_mutex; // guards the members below and each Share's wake and status
    std::vector<std::unique_ptr<Share>> m_shares;
    std::vector<std::unique_ptr<Share>> m_retired; // removed, their threads ending
    bool m_started = false;
    bool m_stopped = false;
};

} // namespace ws::remotecontent

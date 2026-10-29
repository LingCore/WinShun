#pragma once

#include "ContentIndex.h"
#include "ContentScanner.h"
#include "DocExtractor.h"
#include "Ntfs.h"

#include <QStringList>
#include <QJsonObject>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ws {

// Reads files into a ContentIndex in the background: the files a content
// search looks in that have no document yet, and those whose document turned
// dirty. Only on volumes whose change journal is followed: the journal is
// what says a file was written to. Opening a file costs the antivirus a few
// milliseconds of processor time, in its own process: how many files are
// read at once is how many processors that takes, a quarter of them while
// someone uses the computer and half while nobody does (two at a time from a
// hard disk). Files in the user's folders come first, those in crowded
// folders (crowdedFolders) last, the bulk way too. Waits while a content
// search runs, since both open files; what such a search reads itself comes
// in too (Intake). Documents (Word, PDF...) are read by WinShunExtract.exe
// (DocExtractor), a few at a time beside the other files (more while nobody
// uses the computer), and their text is kept in the index; copies of one
// (the same bytes) get its document without being read. Small segments are
// merged while files are read; everything, on as many threads as read
// files, once they are all in.
//
// A first build (or a rebuild) has hundreds of thousands of text files to
// read for the first time, and the antivirus scans each one as it is
// opened. Those go the bulk way when there are enough of them (kBulk*):
// per volume, a session with the index service (or the volume itself, when
// this program may read it: ntfs::VolumeAccess::openBulkRead) that reads
// their bytes off the volume by their MFT records, in disk order within each
// kind of file (Work::order), with no
// file opened; the grams are taken out here as from a file. Files written
// lately, documents, and whatever the session will not give (compressed,
// encrypted, reparse points, not readable by this user, not found) are read
// the usual way instead; nothing is lost, only read differently.
class ContentIndexer {
public:
    struct Options {
        bool enabled = true;
        QStringList extensions;
        ContentSizeLimits sizeLimits;
        bool documents = true; // and documentExtensions()
        bool includeLowPriority = false;
        // The local disks too ([Content] Index). Off while it is on: only the
        // shares whose contents are indexed (Volume::remote; GitHub #114).
        bool local = true;

        bool operator==(const Options&) const = default;
    };
    // What it needs from the index service.
    struct Volume {
        std::string root; // "C:"
        bool suspended = false; // being ejected or locked: not touched, its documents kept
        std::uint32_t serial = 0; // VolumeInfo's, for the bulk reads (0: not checked)
        // A network share or WSL folder ("\\nas\docs") whose contents are
        // indexed: its documents are kept, and its own thread reads its
        // files (RemoteContent), not a pass. Suspended while offline.
        bool remote = false;
    };
    struct Source {
        std::function<std::shared_ptr<FileIndex>()> index;
        std::function<std::vector<Volume>()> volumes; // those followed through their change journal
        std::function<bool()> ready; // the file index is complete
        std::function<std::shared_ptr<ntfs::VolumeAccess>()> access; // for the bulk reads; may be empty: none
    };
    // How the bulk way went, all told (for the settings and wsbench).
    struct BulkStats {
        std::size_t sessions = 0; // opened
        std::size_t sessionsFailed = 0; // could not be opened, or broke (their files were read the usual way)
        std::size_t files = 0; // read in bulk
        std::uint64_t bytes = 0;
        std::size_t tooLarge = 0; // recorded as too large from the record, without a read
        std::size_t recent = 0; // written lately: kept to the usual way
        std::size_t noRecord = 0; // the folder's record is not known: the usual way
        std::size_t notFound = 0; // the session found no record (the usual way)
        std::size_t denied = 0; // this user may not read it, said the service (the usual way: ACCESS_DENIED)
        std::size_t fallback = 0; // compressed, encrypted, reparse points...: the usual way
        std::size_t verified = 0; // WINSHUN_BULK_VERIFY: read the usual way too and compared
        std::size_t verifyMismatches = 0;
    };
    BulkStats bulkStats() const noexcept;
    struct Timing {
        // From starting to the first pass, to let logging on settle; when
        // nothing has been read yet (the first run), no wait. Either way the
        // first pass waits for the file index to be complete (Source::ready).
        std::chrono::milliseconds start {std::chrono::seconds(30)};
        std::chrono::milliseconds gather {std::chrono::minutes(2)}; // after a change, wait for more
        std::chrono::milliseconds interval {std::chrono::minutes(15)}; // new files come without a change of an indexed one
        std::chrono::milliseconds cooldown {std::chrono::minutes(10)}; // a file is read at most this often
        std::chrono::milliseconds report {std::chrono::seconds(30)}; // diagnostics heartbeat; tests shorten it
    };

    ContentIndexer(std::shared_ptr<ContentIndex> content, Source source, Options options, Timing timing = {});
    ~ContentIndexer();
    ContentIndexer(const ContentIndexer&) = delete;
    ContentIndexer& operator=(const ContentIndexer&) = delete;

    void setOptions(Options options);
    void wake(); // documents turned dirty: read them again a little later
    // What Source::volumes gives changed so that a pass should look now, not
    // after the wait for more changes: a share's contents turned on or off
    // (its documents read, or let go at once; GitHub #114).
    void volumesChanged();
    bool reading() const noexcept { return m_reading.load(); } // files are being read now
    // Of the files the pass under way looks at: how many it has done, of how
    // many; and of those, the ones in crowded folders (read last).
    struct Progress {
        std::size_t done = 0;
        std::size_t total = 0;
        std::size_t crowdedDone = 0;
        std::size_t crowded = 0;
    };
    Progress progress() const noexcept
    {
        return {m_passDone.load(), m_passTotal.load(), m_passCrowdedDone.load(), m_passCrowded.load()};
    }
    // Crowded folders: those that directly hold at least kCrowdedFiles of
    // `candidates` (files a content search looks in), kCrowdedShare of them
    // of one extension. Generated files, mostly (documentation, translation
    // output, resources); their files are read last. Sorted. Measured
    // 2026-10-19 (wsbench --content-plan): these catch 24.5k files, 1.46 GB
    // of 357k candidates here, almost none a user looks for (crowdedFolders()).
    static constexpr std::size_t kCrowdedFiles = 500;
    static constexpr std::size_t kCrowdedShare = 90; // percent
    static std::vector<EntryId> crowdedFolders(const FileIndex& index, std::span<const EntryId> candidates);
    // The order a file to read for the first time gets (lower first).
    enum Order : std::uint8_t { kKnown = 0, kProfile = 1, kOther = 2, kHidden = 3, kCrowded = 4 };
    static Order orderOf(bool inProfile, bool hidden, bool crowded) noexcept
    {
        return crowded ? kCrowded : hidden ? kHidden : inProfile ? kProfile : kOther;
    }
    QJsonObject diagnosticStatus() const; // metadata only; no paths or document text
    // The search box is open: the user may search again any moment, and a
    // search reads the files it needs itself, best with the disks to itself.
    // No file is started (nor segments merged) meanwhile, nor for a few
    // seconds after it closed. Any thread.
    void setSearchBoxOpen(bool open) noexcept;
    // Documents found to be copies of others (the same bytes) and not read
    // again, all told.
    std::size_t copies() const noexcept { return m_copies.load(); }
    bool enabled();
    // Stops reading files and returns once none is open; the next pass starts
    // a little later (and leaves alone volumes that are suspended by then).
    void interrupt();
    // From when pause() returns until resume(), no file is read and no
    // segment written or merged: the content index's files are being moved
    // (IndexService::moveTo).
    void pause();
    void resume();
    void stop(); // idempotent; nothing is taken (Intake) from when it starts

    // For the threads of network shares and WSL folders, which read their
    // files' contents themselves (RemoteContent; GitHub #114).
    Options options();
    // Whether such a thread may read a file now, as a pass would: on, not
    // paused or stopping, no battery saver, no content search reading and no
    // search box open (or just closed). `few`: a handful of files written to
    // lately, read even while the search box is open.
    bool mayReadRemote(bool few);
    bool stopping() const noexcept { return m_stopping.load(); } // stop() has begun: a read under way gives up
    DocExtractor* documents() noexcept { return &m_documents; }
    const std::shared_ptr<ContentIndex>& content() const noexcept { return m_content; }

    // One file, read as ContentScanner reads it.
    enum class Outcome {
        Indexed,
        Empty, // nothing to find in it: empty, too large, not readable
        Skipped, // try again later: in use, gone, a cloud file
    };
    struct FileText {
        Outcome outcome = Outcome::Skipped;
        std::vector<grams::Key> keys;
        std::uint32_t stamp = 0; // ContentIndex::stampOf
        QByteArray text; // a document's, packed (ContentIndex::packText)
        std::optional<doctext::DocText> document; // ... and as read, for a search to look through
        bool large = false; // Empty for having more bytes than the limit (a search counts those)
        unsigned long error = 0; // why it could not be opened (Skipped; Empty for no access)
        // The file's size and last write time (FILETIME ticks) as opened; -1: not opened.
        std::int64_t bytes = -1;
        std::int64_t written = 0;
    };
    // A file on a hard disk: a document is read through once, in order,
    // before the extractor jumps about in it. `quiet` (someone uses the
    // computer): at very low I/O priority, after whatever else wants the disk.
    // `remote` (on a share: RemoteContent): a read through that fails is the
    // server's doing, not the file's; the file is left for later (Skipped,
    // FileText::error), not handed to the extractor, which would find nothing.
    struct HardDisk {
        bool on = false;
        bool quiet = false;
        bool remote = false;
    };
    // A document (isDocumentPath) goes to `documents`; without it, it is skipped.
    static FileText readFile(std::wstring_view path, std::int64_t maxBytes, const std::function<bool()>& cancelled,
        DocExtractor* documents = nullptr, HardDisk disk = {});
    // readFile()'s last step for a document: its text from the file open in
    // `file` (`size` bytes), through `documents`. The stamp, size and time
    // are left to the caller, which opened it (RemoteContent: on a thread
    // of its own, GitHub #114).
    static FileText extractDocument(HANDLE file, std::uint64_t size, DocExtractor* documents,
        const std::function<bool()>& cancelled);
    // A text file's bytes, read elsewhere (the bulk way): the same grams as
    // readFile() takes from the file, with `stamp` (ContentIndex::stampOf).
    static FileText collectText(std::span<const char> bytes, std::uint32_t stamp);
    static std::optional<std::uint32_t> stampOf(std::wstring_view path); // size and time only
    // A content search's read of a file: `scanner` looks for its phrase while
    // the whole file is read for the index (the rest of it after a match).
    struct Scanned {
        std::optional<ContentMatch> match;
        FileText text; // Skipped when the read was cut short
    };
    static Scanned scanFile(std::wstring_view path, std::int64_t maxBytes, const ContentScanner& scanner,
        const std::function<bool()>& cancelled);

    // A content search reads the files the index does not know yet (or knew
    // before they were written to) itself. What it reads is kept as if the
    // indexer had read it, so that the next search, for the same text or
    // another, need not open them again. One Intake per search, used from its
    // threads; it takes nothing while the indexer is off or paused, before
    // the file index is complete, or from a volume the indexer does not follow.
    class Intake {
    public:
        explicit Intake(ContentIndexer* indexer); // null: takes nothing
        ~Intake(); // the indexer tidies up after what was taken
        Intake(const Intake&) = delete;
        Intake& operator=(const Intake&) = delete;

        bool wants(std::wstring_view path) const noexcept; // a file on a volume it takes from
        std::uint64_t since() const noexcept; // before reading a file, for take()
        // False: not taken (Skipped, the indexer paused, off or stopping, or no room left).
        bool take(EntryId entry, const FileText& text, std::uint64_t since);

    private:
        ContentIndexer* m_indexer = nullptr;
        std::vector<std::wstring> m_roots; // "C:\"
        std::vector<std::wstring> m_remote; // "\\nas\docs" (Volume::remote; unc::contains)
        std::atomic<std::size_t> m_taken {0};
    };

private:
    struct Work {
        EntryId entry;
        bool checkOnly; // compare the size and time; read only if they changed
        bool again; // read before, changed since
        std::uint32_t stamp;
        bool document = false; // read by the DocExtractor
        std::uint8_t order = kKnown; // Order: files read before, then the user's own, others, hidden, crowded ones
        bool crowded = false; // in a crowded folder (read last but with WINSHUN_CROWDED=0)
        std::size_t volume = 0; // in the volumes findWork() was given
    };

    void run(std::stop_token stop);
    bool pass(const Options& options, std::stop_token stop); // false: cut short
    std::vector<Work> findWork(const FileIndex& index, const Options& options, const std::vector<Volume>& volumes);
    bool interrupted(std::stop_token stop);
    static int documentProcesses() noexcept; // documents read at once, at most
    void tidy(std::size_t added); // merges after `added` files were read (by passes or searches)

    std::shared_ptr<ContentIndex> m_content;
    Source m_source;
    Timing m_timing;
    // Held shared while a search hands a file over (Intake::take), alone
    // while the indexer is paused or turned off: nothing is taken then.
    std::shared_mutex m_intakeGate;
    std::atomic<std::size_t> m_taken {0}; // files searches handed over since the last tidy()
    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    Options m_options;
    bool m_optionsChanged = false;
    bool m_woken = false;
    bool m_passRunning = false;
    bool m_paused = false;
    std::atomic<bool> m_stopping {false}; // stop() has begun: nothing more is taken
    std::atomic<bool> m_interrupt {false}; // checked before each file
    std::atomic<bool> m_reading {false};
    bool searchBoxHolds() const noexcept;
    std::atomic<bool> m_searchBoxOpen {false};
    std::atomic<std::int64_t> m_searchBoxClosed {0}; // steady clock, ms
    std::atomic<std::size_t> m_passDone {0};
    std::atomic<std::size_t> m_passTotal {0};
    std::atomic<std::size_t> m_passCrowdedDone {0};
    std::atomic<std::size_t> m_passCrowded {0};
    std::atomic<std::int64_t> m_passStartedMs {0};
    std::atomic<std::int64_t> m_lastProgressMs {0};
    std::atomic<bool> m_merging {false};
    std::atomic<std::size_t> m_copies {0};
    struct BulkCounters {
        std::atomic<std::size_t> sessions {0}, sessionsFailed {0}, files {0}, tooLarge {0}, recent {0}, noRecord {0},
            notFound {0}, denied {0}, fallback {0}, verified {0}, verifyMismatches {0};
        std::atomic<std::uint64_t> bytes {0};
    } m_bulk;
    // Files read lately, and when: one that keeps changing (a log being
    // written) is read at most every Timing::cooldown; meanwhile searches
    // read it themselves.
    std::unordered_map<EntryId, std::chrono::steady_clock::time_point> m_recentReads;
    std::uint64_t m_generation = 0; // of the FileIndex the ids above are from
    DocExtractor m_documents {documentProcesses(), DocExtractor::Priority::Background};
    std::jthread m_thread; // last: started once the members above exist
};

} // namespace ws

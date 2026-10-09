#pragma once

#include "ContentIndex.h"
#include "ContentScanner.h"
#include "DocExtractor.h"

#include <QStringList>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
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
// hard disk). Files in the user's folders come first. Waits while a content
// search runs, since both open files; what such a search reads itself comes
// in too (Intake). Documents (Word, PDF...) are read by WinShunExtract.exe
// (DocExtractor), a few at a time beside the other files (more while nobody
// uses the computer), and their text is kept in the index; copies of one
// (the same bytes) get its document without being read. Small segments are
// merged while files are read; everything, on as many threads as read
// files, once they are all in.
class ContentIndexer {
public:
    struct Options {
        bool enabled = true;
        QStringList extensions;
        ContentSizeLimits sizeLimits;
        bool documents = true; // and documentExtensions()
        bool includeLowPriority = false;

        bool operator==(const Options&) const = default;
    };
    // What it needs from the index service.
    struct Volume {
        std::string root; // "C:"
        bool suspended = false; // being ejected or locked: not touched, its documents kept
    };
    struct Source {
        std::function<std::shared_ptr<FileIndex>()> index;
        std::function<std::vector<Volume>()> volumes; // those followed through their change journal
        std::function<bool()> ready; // the file index is complete
    };
    struct Timing {
        // From starting to the first pass, to let logging on settle; when
        // nothing has been read yet (the first run), no wait. Either way the
        // first pass waits for the file index to be complete (Source::ready).
        std::chrono::milliseconds start {std::chrono::seconds(30)};
        std::chrono::milliseconds gather {std::chrono::minutes(2)}; // after a change, wait for more
        std::chrono::milliseconds interval {std::chrono::minutes(15)}; // new files come without a change of an indexed one
        std::chrono::milliseconds cooldown {std::chrono::minutes(10)}; // a file is read at most this often
    };

    ContentIndexer(std::shared_ptr<ContentIndex> content, Source source, Options options, Timing timing = {});
    ~ContentIndexer();
    ContentIndexer(const ContentIndexer&) = delete;
    ContentIndexer& operator=(const ContentIndexer&) = delete;

    void setOptions(Options options);
    void wake(); // documents turned dirty: read them again a little later
    bool reading() const noexcept { return m_reading.load(); } // files are being read now
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
    void stop(); // idempotent

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
    };
    // A document (isDocumentPath) goes to `documents`; without it, it is skipped.
    static FileText readFile(std::wstring_view path, std::int64_t maxBytes, const std::function<bool()>& cancelled,
        DocExtractor* documents = nullptr);
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
        void take(EntryId entry, const FileText& text, std::uint64_t since);

    private:
        ContentIndexer* m_indexer = nullptr;
        std::vector<std::wstring> m_roots; // "C:\"
        std::atomic<std::size_t> m_taken {0};
    };

private:
    struct Work {
        EntryId entry;
        bool checkOnly; // compare the size and time; read only if they changed
        bool again; // read before, changed since
        std::uint32_t stamp;
        bool document = false; // read by the DocExtractor
        std::uint8_t order = 0; // lower first: files read before, then the user's own, others, hidden ones
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
    std::atomic<bool> m_interrupt {false}; // checked before each file
    std::atomic<bool> m_reading {false};
    std::atomic<std::size_t> m_copies {0};
    // Files read lately, and when: one that keeps changing (a log being
    // written) is read at most every Timing::cooldown; meanwhile searches
    // read it themselves.
    std::unordered_map<EntryId, std::chrono::steady_clock::time_point> m_recentReads;
    std::uint64_t m_generation = 0; // of the FileIndex the ids above are from
    DocExtractor m_documents {documentProcesses(), DocExtractor::Priority::Background};
    std::jthread m_thread; // last: started once the members above exist
};

} // namespace ws

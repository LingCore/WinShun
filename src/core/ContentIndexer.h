#pragma once

#include "ContentIndex.h"

#include <QStringList>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
// what says a file was written to. Runs at background priority (processor,
// disk and memory), a few files at a time, and waits while a content search
// runs, since both open files.
class ContentIndexer {
public:
    struct Options {
        bool enabled = true;
        QStringList extensions;
        std::int64_t maxFileBytes = 64ll << 20;
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
        std::chrono::milliseconds start {std::chrono::seconds(30)}; // let login and the first sync settle
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
    bool enabled();
    // Stops reading files and returns once none is open; the next pass starts
    // a little later (and leaves alone volumes that are suspended by then).
    void interrupt();
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
    };
    static FileText readFile(std::wstring_view path, std::int64_t maxBytes, const std::function<bool()>& cancelled);
    static std::optional<std::uint32_t> stampOf(std::wstring_view path); // size and time only

private:
    struct Work {
        EntryId entry;
        bool checkOnly; // compare the size and time; read only if they changed
        bool again; // read before, changed since
        std::uint32_t stamp;
    };

    void run(std::stop_token stop);
    bool pass(const Options& options, std::stop_token stop); // false: cut short
    std::vector<Work> findWork(const FileIndex& index, const Options& options);
    bool interrupted(std::stop_token stop);

    std::shared_ptr<ContentIndex> m_content;
    Source m_source;
    Timing m_timing;
    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    Options m_options;
    bool m_optionsChanged = false;
    bool m_woken = false;
    bool m_passRunning = false;
    std::atomic<bool> m_interrupt {false}; // checked before each file
    std::atomic<bool> m_reading {false};
    // Files read lately, and when: one that keeps changing (a log being
    // written) is read at most every Timing::cooldown; meanwhile searches
    // read it themselves.
    std::unordered_map<EntryId, std::chrono::steady_clock::time_point> m_recentReads;
    std::uint64_t m_generation = 0; // of the FileIndex the ids above are from
    std::jthread m_thread; // last: started once the members above exist
};

} // namespace ws

#pragma once

#include "DocText.h"
#include "Protocol.h"
#include "Win32Util.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace ws {

// Reads the text of documents (Word, Excel, PowerPoint, PDF) in
// WinShunExtract.exe, never in WinShun itself: WinShun runs as administrator,
// and document parsers are big programs fed untrusted files. The extractor
// processes run with a restricted token (no administrator rights, no
// privileges) at low integrity, each in a job that caps its memory, lets it
// start no process, keeps it off the clipboard and other programs' windows,
// and ends it with WinShun. They open no files: each request carries a
// read-only handle to one. A process reads one file at a time; up to
// `maxProcesses` run at once, and those left idle end after a while.
//
// A file the extractor crashes on, or takes too long with, comes back as
// Failed (and the process is replaced); one that could not be tried (no
// extractor could be started, or `cancelled` said stop) as `retry`.
class DocExtractor {
public:
    enum class Priority { Background, Normal };
    DocExtractor(int maxProcesses, Priority priority);
    ~DocExtractor();
    DocExtractor(const DocExtractor&) = delete;
    DocExtractor& operator=(const DocExtractor&) = delete;

    // Bytes of UTF-8 per document. Their text is limited rather than their
    // size (ContentSizeLimits::Document): most of a large one is pictures.
    static constexpr std::size_t kMaxText = 16u << 20;

    struct Result {
        extractproto::Status status = extractproto::Status::Failed;
        doctext::DocText text; // when Ok
        bool retry = false; // not tried: ask again later
    };
    // The text of an open file (read access). Waits for a free extractor
    // unless `cancelled` says stop.
    Result extract(HANDLE file, std::uint64_t size, const std::function<bool()>& cancelled);
    // ... of a file by its path, if it has no more than `maxBytes`. A cloud
    // file that is not on this computer is not downloaded: it comes back
    // Failed, as does one that cannot be opened.
    Result extractFile(std::wstring_view path, std::int64_t maxBytes, const std::function<bool()>& cancelled);
    void closeIdle(); // ends the extractor processes not reading now

    static std::wstring programPath(); // WinShunExtract.exe, next to this program

private:
    struct Process;
    std::unique_ptr<Process> start();
    std::unique_ptr<Process> acquire(const std::function<bool()>& cancelled);
    void release(std::unique_ptr<Process> process); // null: it was ended
    void reap(std::stop_token stop);

    const int m_max;
    const Priority m_priority;
    const unsigned m_codePage;
    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::vector<std::unique_ptr<Process>> m_idle;
    int m_running = 0; // processes, idle or reading
    std::chrono::steady_clock::time_point m_retryStartAt {}; // after starting one failed
    std::jthread m_reaper; // last: ends idle processes
};

} // namespace ws

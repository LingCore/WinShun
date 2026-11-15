#pragma once

#include "EverythingIpc.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace ws {

class FileIndex;

// Win顺 answering where programs look for Everything (GitHub #40): a window
// of Everything's class that takes its queries (core/EverythingIpc), so that
// Directory Opus, Total Commander, es.exe and the like search Win顺's index
// with Everything not installed. Only while its switch is on
// (Settings::everythingCompat).
//
// All of it is off the GUI thread: the window has a thread of its own (a
// query is taken in a moment, never waited for), the searches another, one
// at a time; a program that asks again before its answer came (typing)
// drops the search it no longer waits for. The answer is sent from the
// search's thread, for no longer than a hung program is waited for.
//
// With Everything itself running, its window is there already and it goes on
// answering: Win顺 makes none (two would be asked at random), and looks
// again now and then.
class EverythingServer {
public:
    // The index as it is now (IndexService::index), from any thread.
    using IndexSource = std::function<std::shared_ptr<FileIndex>()>;
    // What was asked and found, told on the search's thread as each answer
    // goes out: for tests. (The log never has the words asked for.)
    using Observer = std::function<void(const everything::Query&, const everything::Rows&)>;
    // `windowClass`: Everything's, unless a test has one of its own.
    explicit EverythingServer(IndexSource index, std::wstring windowClass = everything::kWindowClass, Observer observer = {});
    ~EverythingServer();
    EverythingServer(const EverythingServer&) = delete;
    EverythingServer& operator=(const EverythingServer&) = delete;

    // Whether its window is up (not while Everything's own is). For tests
    // and the log; changes on the window's thread.
    bool serving() const noexcept { return m_window.load() != nullptr; }
    HWND window() const noexcept { return m_window.load(); }

private:
    struct Job {
        everything::Query query;
        std::uint64_t turn = 0; // the asker's: an older one is dropped
    };

    void serve(std::stop_token stop);
    void search(std::stop_token stop);
    bool makeWindow();
    void enqueue(everything::Query query);
    bool superseded(const Job& job);
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    IndexSource m_index;
    Observer m_observer;
    std::wstring m_class;
    std::atomic<HWND> m_window {nullptr};
    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<Job> m_jobs;
    std::unordered_map<std::uint32_t, std::uint64_t> m_turns; // by reply window: its latest query
    std::uint64_t m_turn = 0;
    std::atomic<DWORD> m_serveThread {0};
    std::jthread m_searcher; // before m_server: it is stopped last
    std::jthread m_server;
};

} // namespace ws

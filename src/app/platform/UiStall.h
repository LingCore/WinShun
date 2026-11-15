#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace ws {

// Where the interface was when it stopped answering (GitHub #111: "stuck, a
// quarter of the processor in use, ended from Task Manager", with nothing in
// the journal to say what it was doing).
//
// A thread of its own asks the interface's thread every two seconds, with a
// message posted to a window of that thread's: answered, the interface is
// taking input. Not answered for two seconds, it is stuck, and what it is
// doing is written down at once (the process may be ended before it comes
// back): how long by then, how much of that its thread spent computing (all
// of it: a loop; none: waiting on something), what the whole process spent,
// and the calls it is in, innermost first, as "module+offset". Looked at
// again every ten seconds while it lasts, up to three times; then how long
// it lasted in all, once it answers.
//
// The calls are read without stopping the thread for longer than it takes to
// copy the top of its stack: stopped, it may hold a lock that anything else
// here would wait on for good (the heap's, the loader's), so nothing is
// allocated, looked up or logged until it runs again; the walk up the calls
// is done on the copy.
//
// No names but those of the modules (file names, no folders), of the function
// one of Windows' own modules exports nearest before a call, and of the
// program's threads; no text, no titles, no keys. An offset becomes a
// function with that version's symbols (docs/pitfalls.md has how they are
// made for a version already out).
//
// Not asked while nobody has typed or moved the mouse for a minute: nothing
// wakes the interface's thread then. A stall the PC slept through is none.
class UiStallWatch {
public:
    struct Options {
        std::chrono::milliseconds every {2000}; // asked this often
        std::chrono::milliseconds late {2000}; // not answered for this long: stuck
        std::chrono::milliseconds again {10'000}; // looked at again this often while it lasts
        int samples = 3; // looks at one stall, at most
        int stalls = 20; // written down per run; the ones after are left out
        bool onlyWhileUsed = true;
        // The other threads' calls too, at a stall's first look: for a thread
        // that may wait on another of the program's, the one that draws its
        // windows. "ui-stall-threads" {stall, part, of, threads: [{frames,
        // count, name}]} after that look's "ui-stall", the same calls once.
        bool others = false;
    };
    // "ui-stall" {stall, sample, ms, ui_cpu_ms, process_cpu_ms, busy, frames}
    // for each look, "ui-stall-end" {stall, ms, samples} when it answers
    // again. Called on the watch's thread.
    using Sink = std::function<void(const QString& event, QJsonObject details)>;

    // On the thread to watch.
    explicit UiStallWatch(Sink sink, Options options);
    explicit UiStallWatch(Sink sink)
        : UiStallWatch(std::move(sink), Options {})
    {
    }
    ~UiStallWatch();

    UiStallWatch(const UiStallWatch&) = delete;
    UiStallWatch& operator=(const UiStallWatch&) = delete;

    bool isActive() const noexcept { return m_thread.joinable(); }

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void run();
    enum class Waited { Answered, Late, Stopped };
    Waited wait(std::chrono::milliseconds limit) const;
    QJsonObject look(); // the calls the watched thread is in
    QJsonArray lookAtOthers(); // and those of the program's other threads
    struct Spent {
        qint64 threadMs = 0;
        qint64 processMs = 0;
    };
    Spent spent() const;

    const Sink m_sink;
    const Options m_options;
    HWND m_window = nullptr; // message-only, of the watched thread
    HANDLE m_watched = nullptr; // the watched thread
    HANDLE m_answered = nullptr; // set by the window as the message reaches it
    HANDLE m_stop = nullptr;
    quintptr m_stackTop = 0; // the watched thread's stack ends here
    std::vector<std::byte> m_copy; // the top of its stack, as it was
    std::jthread m_thread;
};

} // namespace ws

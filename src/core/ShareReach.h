#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Whether a server is there for what the user does with a file on it
// (GitHub #116). The probe (share::probe) is quick, but it can say no where
// Windows gets there: a NAS slow to say hello, a server Windows reaches in
// ways the probe does not try. So before the user is told a server cannot
// be reached, Windows itself is asked, as File Explorer gets there; and once
// it got there, quick checks on that server ask it rather than the probe.
namespace ws::share {

// Whether Windows' answer about a path on a share (0, or GetLastError) came
// from the server, not from the network failing on the way: the file not
// there, access denied, signing in, "not supported" (its IPC$) are all the
// server talking.
bool serverAnswered(unsigned long windowsError) noexcept;

enum class Reach : std::uint8_t {
    There, // go on (also when the server refuses this user: the action's own error says why)
    Missing, // the server answered: the file or its folder is not there
    // The server answered, its share is not there now: a NAS just started,
    // an encrypted folder still locked. Nothing is forgotten for it.
    ShareAway,
    // Windows cannot get there at all: the service it needs is off or would
    // not start (WebClient, for WebDAV).
    NoService,
    Unreachable, // neither the probe nor Windows got there in time
};

// What Windows' answer about a path on a server (askWindows) means for an
// action on it, Windows having the last word: 0 There; the file or its
// folder not there Missing; the share not there (ERROR_BAD_NET_NAME)
// ShareAway, which a NAS starting up says; ERROR_SERVICE_DISABLED and
// ERROR_SERVICE_NOT_ACTIVE NoService; the server refusing this user There
// too (the action's own error tells why); anything else Unreachable.
Reach reachOf(unsigned long windowsError) noexcept;

struct ReachSteps {
    std::function<bool()> probe; // the probe, with a time limit of its own
    // Windows asked about the path (askWindows): 0, or why not. Empty: the
    // probe alone.
    std::function<unsigned long()> windows;
};

// For an action the user asked for on a file on a server (open, show in
// folder, delete, its menu); off the UI thread. The probe and Windows are
// asked at once, on threads of their own, and Windows has the last word:
// the probe's yes only makes it worth waiting for. Windows is waited for
// `windowsWait`, twice that once the probe said yes (the server is there,
// Windows on its way); no answer by then is Unreachable. Windows takes up
// to a minute on a server gone away, and goes on alone after (what it says
// late is kept for the next time by askWindows). A step's thread can
// outlive the call: what a step holds, it must own.
Reach reach(ReachSteps steps, std::chrono::milliseconds windowsWait);

// What actionSteps() asks, for tests in place of the network; empty: the real one.
struct ReachProbes {
    std::function<bool(const std::wstring& path, std::chrono::milliseconds wait)> probe; // reachablePath, asked afresh
    std::function<unsigned long(const std::wstring& path)> windows; // askWindows
    std::function<bool()> webClientDisabled; // webdav::clientDisabled
    std::function<bool()> webClientRunning; // webdav::clientRunning
    std::function<bool(std::chrono::milliseconds wait)> startWebClient; // webdav::startClient
};
// The steps for an action on `path` on a server (reach()): the probe within
// `wait`, and Windows asked about the file as File Explorer gets there; for
// a WebDAV path through WebClient, which with the service disabled opens
// none: ERROR_SERVICE_DISABLED then, at once. Stopped, it opens none either,
// and Windows says the folder is not there: started first (GitHub #114),
// Windows not asked when it does not start, nor believed when it stopped
// meanwhile (ERROR_SERVICE_NOT_ACTIVE for both).
ReachSteps actionSteps(const std::wstring& path, std::chrono::milliseconds wait, ReachProbes with = {});

// How checkFiles() asks.
struct FileSteps {
    std::function<std::wstring(const std::wstring& file)> server; // which files share a server
    std::function<Reach(const std::wstring& file)> first; // a server's first file: the probe and Windows (reach())
    std::function<Reach(const std::wstring& file)> next; // its others, while it answers: Windows alone
};
// What is there of `files` on servers, for an action on them; off the UI
// thread. Each server on a thread of its own, all at once (servers gone
// away one after the other would add up), its files one after the other
// while it answers; within `limit` for all: a file without an answer by then
// counts as Unreachable, as do the rest of a server that did not answer.
std::map<std::wstring, Reach> checkFiles(const std::vector<std::wstring>& files, std::chrono::milliseconds limit,
    const FileSteps& steps);

// The last word on each server (by host, any case), for what the UI thread
// must decide without asking (share::lastHeard). The probe's or Windows';
// a yes from Windows outranks a later no from the probe while it is recent
// (GitHub #116: the probe can be wrong where Windows gets there).
class Hearsay {
public:
    using Clock = std::chrono::steady_clock;

    void probeSaid(std::wstring_view server, bool yes);
    void windowsSaid(std::wstring_view server, bool yes);
    // What was said within `within`, a yes from Windows first: within
    // `windowsYes` when given (it can stand longer); else nullopt.
    std::optional<bool> last(std::wstring_view server, std::chrono::milliseconds within,
        std::optional<std::chrono::milliseconds> windowsYes = std::nullopt) const;

private:
    struct Word {
        bool yes = false;
        Clock::time_point at {};
        std::optional<Clock::time_point> windowsYes; // until Windows says no
    };
    mutable std::mutex m_mutex;
    std::map<std::wstring, Word, std::less<>> m_servers; // in lower case; never removed
};

// The servers Windows itself got to (GitHub #116), so that quick checks of
// what the user does there (answersNow) take its word, asked afresh, rather
// than the probe's, until Windows does not get there.
class WindowsMemory {
public:
    using Clock = std::chrono::steady_clock;

    // What Windows said about something on `host` (0, or GetLastError).
    void said(std::wstring_view host, unsigned long error);

    // For a quick check on `host`: nullopt when Windows has not got there
    // (the probe decides); else whether it gets there now, asked with `ask`
    // on a thread of its own (one at a time for a server) and waited for at
    // most `wait`. No answer in time is a no, and the probe decides again
    // until Windows gets there after all. `fresh` false takes a yes of the
    // last 30 seconds without asking.
    std::optional<bool> quick(std::wstring_view host, bool fresh, std::chrono::milliseconds wait,
        std::function<unsigned long()> ask);

private:
    struct Server {
        bool known = false; // Windows got there, and has not failed since
        Clock::time_point yesAt {}; // when it last got there
        bool asking = false; // quick()'s ask is under way
        std::uint64_t answers = 0; // so far: a new one changes it
        unsigned long last = 0; // the last answer
    };
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        std::map<std::wstring, Server, std::less<>> servers; // by host, in lower case; never removed
    };
    static void record(State& state, const std::wstring& key, unsigned long error);

    std::shared_ptr<State> m_state = std::make_shared<State>(); // an ask's thread holds it too
};

// Windows asked one at a time for a server (askWindows): on one gone away
// each ask takes up to a minute, and a key pressed again should not start
// another. Asked while one is under way, a caller waits for it; its failure
// (the network's, not the server's word) answers for the caller too when
// that ask began after the caller asked: an older one says nothing of now
// (the server may be back). Else the caller asks in turn, and that one ask
// answers for all who waited with it.
class OneAtATime {
public:
    unsigned long ask(std::wstring_view server, const std::function<unsigned long()>& ask);

private:
    using Clock = std::chrono::steady_clock;
    struct Server {
        bool busy = false;
        std::uint64_t done = 0; // asks answered so far
        unsigned long last = 0; // the last answer
        Clock::time_point lastBegan {}; // when the ask that gave it began
    };
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<std::wstring, Server, std::less<>> m_servers; // by server, in lower case; never removed
};

} // namespace ws::share

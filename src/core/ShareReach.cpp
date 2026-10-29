#include "ShareReach.h"

#include "ShareLister.h"
#include "WebDav.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <thread>

namespace ws::share {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kReuse = std::chrono::seconds(30); // quick(fresh = false): a yes this recent stands, as probe()'s does
// WebClient started for an action (webdav::startClient); the caller's limit
// (reach()) cuts it shorter, the start going on.
constexpr auto kClientStart = std::chrono::seconds(60);

std::wstring keyOf(std::wstring_view host)
{
    std::wstring key(host);
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    return key;
}

} // namespace

bool serverAnswered(unsigned long windowsError) noexcept
{
    switch (classify(windowsError, false)) {
    case Problem::Offline:
    case Problem::Dropped:
        return false;
    default:
        // Windows giving up by its own clock, or no network provider taking the path: not the server either.
        return windowsError != ERROR_TIMEOUT && windowsError != ERROR_NO_NET_OR_BAD_PATH;
    }
}

Reach reachOf(unsigned long windowsError) noexcept
{
    switch (windowsError) {
    case NO_ERROR:
        return Reach::There;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return Reach::Missing;
    case ERROR_BAD_NET_NAME:
        return Reach::ShareAway; // ShareIndexer has it pass by itself too (Problem::NotFound)
    case ERROR_SERVICE_DISABLED:
    case ERROR_SERVICE_NOT_ACTIVE:
        return Reach::NoService;
    default:
        // The server answered, not plainly (signing in, kept from this
        // user, the file in use): the action goes on and Windows tells why,
        // as File Explorer has it. Else Windows did not get there, whatever
        // the probe says: the action would fail alike.
        return serverAnswered(windowsError) ? Reach::There : Reach::Unreachable;
    }
}

Reach reach(ReachSteps steps, std::chrono::milliseconds windowsWait)
{
    if (!steps.windows)
        return steps.probe() ? Reach::There : Reach::Unreachable;

    struct Answers {
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<bool> probe;
        std::optional<unsigned long> windows;
    };
    const auto answers = std::make_shared<Answers>();
    const auto until = Clock::now() + windowsWait; // Windows, unless the probe said yes by then
    const auto last = until + windowsWait; // and if it did
    std::thread([answers, windows = std::move(steps.windows)] {
        const unsigned long error = windows();
        std::lock_guard lock(answers->mutex);
        answers->windows = error;
        answers->cv.notify_all();
    }).detach();
    std::thread([answers, probe = std::move(steps.probe)] {
        const bool yes = probe();
        std::lock_guard lock(answers->mutex);
        answers->probe = yes;
        answers->cv.notify_all();
    }).detach();

    std::unique_lock lock(answers->mutex);
    for (;;) {
        if (answers->windows)
            return reachOf(*answers->windows);
        const auto end = answers->probe == true ? last : until;
        if (Clock::now() >= end)
            return Reach::Unreachable;
        answers->cv.wait_until(lock, end);
    }
}

ReachSteps actionSteps(const std::wstring& path, std::chrono::milliseconds wait, ReachProbes with)
{
    if (!with.probe)
        with.probe = [](const std::wstring& p, std::chrono::milliseconds w) { return reachablePath(p, true, w); };
    if (!with.windows)
        with.windows = [](const std::wstring& p) { return askWindows(p); };
    if (!with.webClientDisabled)
        with.webClientDisabled = [] { return webdav::clientDisabled(); };
    if (!with.webClientRunning)
        with.webClientRunning = [] { return webdav::clientRunning(); };
    if (!with.startWebClient)
        with.startWebClient = [](std::chrono::milliseconds w) { return webdav::startClient(w, {}, webdav::Starter::User); };
    ReachSteps s;
    s.probe = [probe = std::move(with.probe), path, wait] { return probe(path, wait); };
    s.windows = [windows = std::move(with.windows), disabled = std::move(with.webClientDisabled),
                    running = std::move(with.webClientRunning), start = std::move(with.startWebClient), path,
                    webDav = webdav::isWebDavPath(path)]() -> unsigned long {
        if (!webDav)
            return windows(path);
        if (disabled())
            return ERROR_SERVICE_DISABLED;
        // Stopped, WebClient is not started by asking about a WebDAV path:
        // Windows says at once the folder is not there, which would have the
        // file forgotten (Missing).
        if (!start(kClientStart))
            return ERROR_SERVICE_NOT_ACTIVE;
        const unsigned long error = windows(path);
        if ((error == ERROR_PATH_NOT_FOUND || error == ERROR_BAD_NETPATH) && !running())
            return ERROR_SERVICE_NOT_ACTIVE; // stopped meanwhile: that is what Windows said
        return error;
    };
    return s;
}

std::map<std::wstring, Reach> checkFiles(const std::vector<std::wstring>& files, std::chrono::milliseconds limit,
    const FileSteps& steps)
{
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        std::map<std::wstring, Reach> found;
        std::size_t asking = 0; // servers not done yet
    };
    // Each server's files, in their order.
    std::vector<std::vector<std::wstring>> servers;
    std::map<std::wstring, std::size_t> place; // a server's in `servers`
    for (const std::wstring& f : files) {
        const auto [it, added] = place.try_emplace(steps.server(f), servers.size());
        if (added)
            servers.emplace_back();
        servers[it->second].push_back(f);
    }
    const auto shared = std::make_shared<Shared>();
    shared->asking = servers.size();
    const auto until = Clock::now() + limit;
    for (std::vector<std::wstring>& theirs : servers) {
        std::thread([shared, theirs = std::move(theirs), first = steps.first, next = steps.next] {
            for (std::size_t i = 0; i < theirs.size(); ++i) {
                const Reach r = i == 0 ? first(theirs[i]) : next(theirs[i]);
                std::lock_guard lock(shared->mutex);
                shared->found[theirs[i]] = r;
                if (r == Reach::Unreachable || r == Reach::NoService) {
                    // Not got to: nor are the server's other files.
                    for (std::size_t j = i + 1; j < theirs.size(); ++j)
                        shared->found[theirs[j]] = r;
                    break;
                }
            }
            std::lock_guard lock(shared->mutex);
            --shared->asking;
            shared->cv.notify_all();
        }).detach();
    }
    std::unique_lock lock(shared->mutex);
    shared->cv.wait_until(lock, until, [&] { return shared->asking == 0; });
    std::map<std::wstring, Reach> found;
    for (const std::wstring& f : files) {
        const auto it = shared->found.find(f);
        found[f] = it != shared->found.end() ? it->second : Reach::Unreachable; // no answer in time
    }
    return found;
}

void Hearsay::probeSaid(std::wstring_view server, bool yes)
{
    std::lock_guard lock(m_mutex);
    Word& w = m_servers[keyOf(server)];
    w.yes = yes;
    w.at = Clock::now();
}

void Hearsay::windowsSaid(std::wstring_view server, bool yes)
{
    std::lock_guard lock(m_mutex);
    Word& w = m_servers[keyOf(server)];
    w.yes = yes;
    w.at = Clock::now();
    w.windowsYes = yes ? std::optional(w.at) : std::nullopt;
}

std::optional<bool> Hearsay::last(std::wstring_view server, std::chrono::milliseconds within,
    std::optional<std::chrono::milliseconds> windowsYes) const
{
    const auto now = Clock::now();
    std::lock_guard lock(m_mutex);
    const auto it = m_servers.find(keyOf(server));
    if (it == m_servers.end())
        return std::nullopt;
    const Word& w = it->second;
    if (w.windowsYes && now - *w.windowsYes < windowsYes.value_or(within))
        return true;
    if (now - w.at < within)
        return w.yes;
    return std::nullopt;
}

void WindowsMemory::record(State& state, const std::wstring& key, unsigned long error)
{
    Server& s = state.servers[key];
    s.last = error;
    ++s.answers;
    s.known = serverAnswered(error);
    if (s.known)
        s.yesAt = Clock::now();
    state.cv.notify_all();
}

void WindowsMemory::said(std::wstring_view host, unsigned long error)
{
    std::lock_guard lock(m_state->mutex);
    record(*m_state, keyOf(host), error);
}

std::optional<bool> WindowsMemory::quick(std::wstring_view host, bool fresh, std::chrono::milliseconds wait,
    std::function<unsigned long()> ask)
{
    const std::wstring key = keyOf(host);
    std::unique_lock lock(m_state->mutex);
    const auto it = m_state->servers.find(key);
    if (it == m_state->servers.end() || !it->second.known)
        return std::nullopt;
    Server& s = it->second; // a map's elements stay where they are
    if (!fresh && Clock::now() - s.yesAt < kReuse)
        return true;
    const std::uint64_t before = s.answers;
    if (!s.asking) {
        s.asking = true;
        std::thread([state = m_state, key, ask = std::move(ask)] {
            const unsigned long error = ask();
            std::lock_guard lock(state->mutex);
            state->servers[key].asking = false;
            record(*state, key, error);
        }).detach();
    }
    if (!m_state->cv.wait_for(lock, wait, [&] { return s.answers != before; })) {
        s.known = false; // no answer in time: the probe's word, until Windows gets there after all
        return false;
    }
    return serverAnswered(s.last);
}

unsigned long OneAtATime::ask(std::wstring_view server, const std::function<unsigned long()>& ask)
{
    const auto askedAt = Clock::now();
    std::unique_lock lock(m_mutex);
    Server& s = m_servers[keyOf(server)]; // a map's elements stay where they are
    while (s.busy) {
        const std::uint64_t before = s.done;
        m_cv.wait(lock, [&] { return s.done != before; });
        if (!serverAnswered(s.last) && s.lastBegan >= askedAt)
            return s.last; // the network failing, asked after this was: so it is now
    }
    s.busy = true;
    const auto began = Clock::now();
    lock.unlock();
    const unsigned long error = ask();
    lock.lock();
    s.busy = false;
    s.last = error;
    s.lastBegan = began;
    ++s.done;
    m_cv.notify_all();
    return error;
}

} // namespace ws::share

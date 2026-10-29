#include "ShareIndexer.h"

#include "DiagnosticLog.h"
#include "Unc.h"
#include "WebDav.h"
#include "Win32Util.h"
#include "Wsl.h"
#include "Wtf8.h"

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

#include <windows.h>

#include <algorithm>
#include <iterator>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr auto kStartupDelay = 15s; // after start, before shares read before are verified: let the network come up
constexpr auto kNetworkQuiet = 5s; // after the network changed, before servers are tried: DHCP, VPN routes
// The probe here, on the share's own thread: a NAS slow to say hello (2.5
// and 4 s measured through a relay, GitHub #116) is no reason to call it
// offline. The UI asks with 1.5 s (Launcher).
constexpr auto kProbeWait = 5s;
// WebDAV goes through Windows' WebClient (GitHub #114): started when it is
// stopped (webdav::startClient, 70 ms here), and its first access to a
// server waited for this long (said to take up to a minute).
constexpr auto kClientStart = 60s;
constexpr auto kWarmWait = 60s;
// A WebDAV server that misses one probe while Online is asked once more
// this much later (no later than the next poll) before it is called offline:
// one lost answer (a NAS busy, Wi-Fi waking) took it offline (GitHub #114).
constexpr auto kDavAskAgain = 15s;
constexpr auto kWslPoll = 10s; // notice a started/stopped distro without accessing it
constexpr auto kWslVerify = 30s; // Linux changes are reconciled even without notifications
constexpr auto kSuspectRetry = 30min; // a share that came back nearly empty, read again
constexpr auto kForever = std::chrono::hours(24 * 365);
constexpr int kPassShare = 60; // verified at most every this many times as long as a pass takes
constexpr std::size_t kSuspectMin = 1000; // missing items that can be a mount not ready yet
constexpr std::size_t kRemoveBatch = 256; // removals per write lock
constexpr std::size_t kMaxPending = 100'000; // live changes queued; beyond, the share is verified instead

std::string toUtf8(std::wstring_view s)
{
    return wtf8::fromUtf16(wtf8::view(s));
}

std::wstring wideName(const FileIndex& index, EntryId id)
{
    std::u16string wide;
    wtf8::decodeAppend(wide, index.name(id));
    return std::wstring(wide.begin(), wide.end());
}

FileTime now() noexcept
{
    FILETIME t;
    ::GetSystemTimeAsFileTime(&t);
    return fileTimeOf(win32::ticks(t));
}

// Entries in the subtree of `id`, itself included. Read lock held.
std::size_t subtreeSize(const FileIndex& index, EntryId id)
{
    if (id >= index.slotCount() || index.entry(id).isDeleted())
        return 0;
    std::size_t n = 0;
    std::vector<EntryId> stack {id};
    while (!stack.empty()) {
        const EntryId cur = stack.back();
        stack.pop_back();
        ++n;
        for (EntryId c = index.entry(cur).firstChild; c != kNoEntry; c = index.entry(c).nextSibling)
            stack.push_back(c);
    }
    return n;
}

bool isUnder(const FileIndex& index, EntryId id, EntryId root)
{
    for (EntryId cur = id; cur != kNoEntry; cur = index.entry(cur).parent) {
        if (cur == root)
            return true;
    }
    return false;
}

// Shares inside another share (or the same twice) are left out: their files
// would be in the index twice. A WebDAV server's root holds its folders,
// written without DavWWWRoot (webdav::rootHolds, GitHub #114).
std::vector<ShareConfig> distinct(std::vector<ShareConfig> shares)
{
    std::vector<ShareConfig> out;
    for (ShareConfig& s : shares) {
        if (s.path.empty())
            continue;
        const auto overlaps = [&](const ShareConfig& o) {
            return unc::contains(o.path, s.path) || unc::contains(s.path, o.path) || webdav::rootHolds(o.path, s.path)
                || webdav::rootHolds(s.path, o.path);
        };
        if (std::none_of(out.begin(), out.end(), overlaps))
            out.push_back(std::move(s));
    }
    return out;
}

bool unreachable(share::Problem p)
{
    return p == share::Problem::Offline || p == share::Problem::Dropped
        || p == share::Problem::WslStopped || p == share::Problem::WslNotRegistered
        || p == share::Problem::WslUnavailable;
}

// Problems that go away by themselves, tried again in minutes (Retry::away),
// not in half an hour: a NAS just restarted can answer before its shares are
// there.
bool passing(share::Problem p)
{
    return unreachable(p) || p == share::Problem::NotFound;
}

// Those, and a WebDAV root that perhaps wants signing in or failed on the
// server (share::davRefusal): it may as well be a server not quite up yet.
// Not its 403, an answer: tried again as a sign-in is (and at once after one).
bool soonAgain(std::wstring_view path, share::Problem p, unsigned long error)
{
    const share::DavRefusal refusal = share::davRefusal(path, p, error);
    return passing(p) || refusal == share::DavRefusal::MaybeSignIn || refusal == share::DavRefusal::ServerError;
}

// What only a server that answered tells of. Windows saying anything else
// where the probe said no leaves the share offline, tried again in minutes:
// a code Windows has for a NAS starting up is no reason to wait half an hour.
bool fromServer(share::Problem p)
{
    using P = share::Problem;
    return p == P::NeedsSignIn || p == P::GuestBlocked || p == P::Signing || p == P::Smb1Only || p == P::NotFound;
}

// A try of tryWindows', which stop() lets go of rather than wait for: it
// then leaves the share and the indexer be, gone by the time it answers.
struct AttemptLink {
    std::mutex mutex;
    bool letGo = false;
};

// Where a share is in the list, for the journal: no paths, no server names.
template <class List, class Item>
qint64 placeOf(const List& list, const Item* item)
{
    const auto it = std::find_if(list.begin(), list.end(), [&](const auto& p) { return p.get() == item; });
    return it == list.end() ? -1 : static_cast<qint64>(it - list.begin());
}

} // namespace

struct ShareIndexer::Share {
    ShareConfig config;
    Status status;
    std::condition_variable_any cv;
    bool wake = false;
    bool verifyRequested = false;
    bool reachRequested = false; // checkReach(): its server asked afresh, nothing read for it
    bool checkRequested = false; // the user asked (ask()): Windows asked too, status.checking until answered
    Clock::time_point checkAskedAt {}; // when
    Clock::time_point probedAt {}; // its server last asked (run), or about to be (lookedAt)
    bool acceptMissing = false;
    bool passRunning = false;
    bool removed = false;
    std::vector<FsChange> pending; // live changes, applied between passes
    std::size_t suspectMissing = 0; // what the first suspect pass did not find
    Clock::time_point suspectAt {};
    std::optional<Clock::time_point> problemSince; // for the journal
    std::atomic<std::size_t> progress {0}; // items read by the pass under way
    std::jthread thread;

    // tryWindows: the share opened through Windows on a thread of its own.
    bool attemptRunning = false;
    bool attemptWanted = false; // the share thread waits for its answer
    bool attemptDone = false; // its answer, not taken yet (a late one only when it got there)
    unsigned long attemptError = 0;
    std::jthread attempt;
    std::shared_ptr<AttemptLink> attemptLink; // the try's, let go of by stop()
};

ShareIndexer::ShareIndexer(Source source, Options options, Callbacks callbacks)
    : m_source(std::move(source))
    , m_callbacks(std::move(callbacks))
    , m_options(std::move(options))
{
    m_options.shares = distinct(std::move(m_options.shares));
    for (const ShareConfig& c : m_options.shares) {
        auto s = std::make_unique<Share>();
        s->config = c;
        s->status.path = c.path;
        s->status.name = c.name;
        m_shares.push_back(std::move(s));
    }
}

ShareIndexer::~ShareIndexer()
{
    stop();
}

void ShareIndexer::start()
{
    std::lock_guard lock(m_mutex);
    if (m_started || m_stopped)
        return;
    m_started = true;
    for (auto& s : m_shares)
        launch(*s);
}

void ShareIndexer::launch(Share& share)
{
    share.thread = std::jthread([this, &share](std::stop_token stop) { run(stop, share); });
}

void ShareIndexer::requestStop()
{
    std::lock_guard lock(m_mutex);
    m_stopped = true;
    for (auto& s : m_shares)
        s->thread.request_stop();
    for (auto& s : m_retired)
        s->thread.request_stop();
    m_watchThread.request_stop();
}

void ShareIndexer::stop()
{
    {
        std::lock_guard lock(m_mutex);
        if (m_joined)
            return;
        m_joined = true;
    }
    requestStop();
    // Joined without the lock: the threads take it on their way out.
    for (auto& s : m_shares) {
        if (s->thread.joinable())
            s->thread.join();
    }
    for (auto& s : m_retired) {
        if (s->thread.joinable())
            s->thread.join();
    }
    // A server tried through Windows cannot be called off, and is not waited
    // for: up to a minute for one gone away, the next Win顺 unable to start
    // meanwhile (GitHub #116). Let go of, the try leaves the share be. A
    // watch being dropped is waited for.
    for (const auto* shares : {&m_shares, &m_retired}) {
        for (const auto& s : *shares) {
            if (s->attemptLink) {
                std::lock_guard letGo(s->attemptLink->mutex);
                s->attemptLink->letGo = true;
            }
            if (s->attempt.joinable())
                s->attempt.detach();
        }
    }
    if (m_watchThread.joinable())
        m_watchThread.join();
    m_watcher.reset();
    m_writeWatcher.reset();
}

void ShareIndexer::setOptions(Options options)
{
    options.shares = distinct(std::move(options.shares));
    std::vector<std::unique_ptr<Share>> removed;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopped)
            return;
        const bool rulesChanged = !(options.rules == m_options.rules) || options.excludedFiles != m_options.excludedFiles;
        bool contentChanged = false;
        // Shares no longer wanted: their threads end and take their entries with them.
        for (auto it = m_shares.begin(); it != m_shares.end();) {
            const auto kept = std::find_if(options.shares.begin(), options.shares.end(),
                [&](const ShareConfig& c) { return unc::equal(c.path, (*it)->config.path); });
            if (kept == options.shares.end()) {
                (*it)->removed = true;
                (*it)->thread.request_stop();
                removed.push_back(std::move(*it));
                it = m_shares.erase(it);
                continue;
            }
            Share& s = **it;
            s.config.name = kept->name;
            s.status.name = kept->name;
            if (s.config.content != kept->content) {
                s.config.content = kept->content;
                contentChanged = true; // its watch tells of writes now, or no longer (watchLoop)
            }
            if (rulesChanged) {
                s.verifyRequested = true; // folders no longer excluded come back (newly excluded ones are gone already)
                s.wake = true;
                s.cv.notify_all();
            }
            ++it;
        }
        // New ones.
        for (const ShareConfig& c : options.shares) {
            const bool known = std::any_of(m_shares.begin(), m_shares.end(),
                [&](const auto& s) { return unc::equal(s->config.path, c.path); });
            if (known)
                continue;
            auto s = std::make_unique<Share>();
            s->config = c;
            s->status.path = c.path;
            s->status.name = c.name;
            if (m_started)
                launch(*s);
            m_shares.push_back(std::move(s));
        }
        // In the order given.
        std::stable_sort(m_shares.begin(), m_shares.end(), [&](const auto& a, const auto& b) {
            const auto at = [&](const Share& s) {
                return std::find_if(options.shares.begin(), options.shares.end(),
                           [&](const ShareConfig& c) { return unc::equal(c.path, s.config.path); })
                    - options.shares.begin();
            };
            return at(*a) < at(*b);
        });
        m_options = std::move(options);
        for (auto& r : removed)
            m_retired.push_back(std::move(r));
        if (contentChanged && m_watchThread.joinable()) {
            m_watchDirty = true;
            m_watchCv.notify_all();
        }
    }
    if (m_callbacks.changed)
        m_callbacks.changed();
}

ShareIndexer::Options ShareIndexer::options() const
{
    std::lock_guard lock(m_mutex);
    return m_options;
}

void ShareIndexer::verifyNow(const std::wstring& path)
{
    ask([&](const Share& s) { return unc::equal(s.config.path, path); });
}

void ShareIndexer::verifyAll()
{
    ask([](const Share&) { return true; });
}

// Not ask(): nothing to show as checking, nor to read (verifyRequested); a
// server just slow to answer the launcher (disks spinning up) costs a probe,
// not a pass over its shares.
void ShareIndexer::checkReach(const std::wstring& path)
{
    std::lock_guard lock(m_mutex);
    for (auto& s : m_shares) {
        if (!unc::equal(s->config.path, path))
            continue;
        s->reachRequested = true;
        s->wake = true;
        s->cv.notify_all();
    }
}

void ShareIndexer::acceptMissing(const std::wstring& path)
{
    ask([&](Share& s) {
        if (!unc::equal(s.config.path, path))
            return false;
        s.acceptMissing = true;
        return true;
    });
}

// The user asked (立即核对, signed in, rebuilt the index): read now, or
// tried now, Windows too when the probe says no (GitHub #116: before, an
// offline share was probed again and nothing showed). Shown as checking at
// once; a second click while it is joins the first.
void ShareIndexer::ask(const std::function<bool(Share&)>& which)
{
    bool changed = false;
    {
        std::lock_guard lock(m_mutex);
        for (auto& s : m_shares) {
            if (!which(*s))
                continue;
            s->verifyRequested = true;
            if (!s->status.checking) {
                s->status.checking = true;
                s->checkRequested = true;
                s->checkAskedAt = Clock::now();
                changed = true;
            }
            s->wake = true;
            s->cv.notify_all();
        }
    }
    if (changed && m_callbacks.changed)
        m_callbacks.changed();
}

void ShareIndexer::networkChanged()
{
    webdav::startAgain(); // a WebClient that would not start may now
    std::lock_guard lock(m_mutex);
    m_networkChangedAt = Clock::now();
    for (auto& s : m_shares) {
        // Offline ones try again; online ones make sure they still are.
        s->wake = true;
        s->cv.notify_all();
    }
}

// A WebDAV server stopped meanwhile is told of nowhere (no watch): asked as
// the launcher opens, its rows and card say so a moment later. At most once
// a minute (the launcher opens and closes all day long), and not while the
// network settles: networkChanged() has them ask again already.
void ShareIndexer::lookedAt()
{
    std::lock_guard lock(m_mutex);
    const auto at = Clock::now();
    if (at - m_networkChangedAt < kNetworkQuiet)
        return;
    for (auto& s : m_shares) {
        if (!webdav::isWebDavPath(s->config.path) || at - s->probedAt < m_options.lookedAtAfter)
            continue;
        s->probedAt = at; // once, however often it opens meanwhile
        s->wake = true;
        s->cv.notify_all();
    }
}

std::vector<ShareIndexer::Status> ShareIndexer::statuses() const
{
    std::lock_guard lock(m_mutex);
    std::vector<Status> out;
    for (const auto& s : m_shares) {
        out.push_back(s->status);
        if (s->status.state == State::Building)
            out.back().items = s->progress.load(std::memory_order_relaxed);
    }
    return out;
}

bool ShareIndexer::offline(std::wstring_view path) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& s : m_shares) {
        if (unc::contains(s->config.path, path))
            return s->status.state == State::Problem && unreachable(s->status.problem);
    }
    return false;
}

std::size_t ShareIndexer::dropUnknownRoots(FileIndex& index, const std::vector<ShareConfig>& shares)
{
    std::size_t removed = 0;
    const std::vector<EntryId> roots = index.roots();
    for (const EntryId r : roots) {
        if (!index.name(r).starts_with("\\\\"))
            continue;
        const std::wstring name = wideName(index, r);
        const bool wanted = std::any_of(
            shares.begin(), shares.end(), [&](const ShareConfig& c) { return unc::equal(c.path, name); });
        if (!wanted)
            removed += index.remove(r);
    }
    return removed;
}

EntryId ShareIndexer::ensureRoot(FileIndex& index, const std::wstring& path)
{
    EntryId root = kNoEntry;
    for (const EntryId r : index.roots()) {
        if (index.name(r).starts_with("\\\\") && unc::equal(wideName(index, r), path)) {
            root = r;
            break;
        }
    }
    if (root == kNoEntry)
        root = index.addRoot(toUtf8(unc::wslDistribution(path).empty() ? path : unc::normalize(path)));
    // Not kept in the snapshot for roots (FileIndex::addStored): set every time.
    index.setFlags(root, EntryFlag::Remote);
    return root;
}

void ShareIndexer::setStatus(Share& share, const std::function<void(Status&)>& change)
{
    bool changed = false;
    QString event;
    QJsonObject details;
    {
        std::lock_guard lock(m_mutex);
        Status before = share.status;
        change(share.status);
        changed = !(before == share.status);
        // The journal (diagnostics): when a share went away, and how long
        // until it was read again (GitHub #116).
        const Status& s = share.status;
        if (s.state == State::Problem && !share.problemSince) {
            share.problemSince = Clock::now();
            event = u"share-offline"_s;
            details = {{u"problem"_s, static_cast<int>(s.problem)}, {u"error"_s, static_cast<qint64>(s.error)}};
        } else if (s.state == State::Online && share.problemSince) {
            event = u"share-online"_s;
            details = {{u"minutes"_s,
                static_cast<qint64>(std::chrono::duration_cast<std::chrono::minutes>(Clock::now() - *share.problemSince).count())}};
            share.problemSince.reset();
        }
        if (!event.isEmpty())
            details.insert(u"share"_s, placeOf(m_shares, &share));
    }
    if (!event.isEmpty())
        diagnostics::record(event, details);
    if (changed && m_callbacks.changed)
        m_callbacks.changed();
}

bool ShareIndexer::wait(std::stop_token stop, Share& share, Clock::duration timeout)
{
    std::unique_lock lock(m_mutex);
    const auto until = Clock::now() + std::min<Clock::duration>(timeout, kForever);
    share.cv.wait_until(lock, stop, until, [&] { return share.wake; });
    share.wake = false;
    return !stop.stop_requested();
}

// Opens the share through Windows (share::openRoot) on a thread of its own
// and waits up to `wait` for the answer: Windows takes up to a minute to give
// up on a server gone away, and the share thread stays free meanwhile (to
// stop, to be removed). nullopt: no answer yet. The try goes on; should it
// get there after all, it wakes the share, and the next call takes that up.
std::optional<unsigned long> ShareIndexer::tryWindows(std::stop_token stop, Share& share, Clock::duration wait)
{
    std::jthread ended;
    bool start = false;
    {
        std::lock_guard lock(m_mutex);
        if (share.attemptDone) {
            share.attemptDone = false;
            return share.attemptError;
        }
        if (stop.stop_requested())
            return std::nullopt; // stopping, or removed: no try that stop() would wait for
        if (!share.attemptRunning) {
            ended = std::move(share.attempt);
            share.attemptRunning = start = true;
        }
        share.attemptWanted = true;
    }
    if (ended.joinable())
        ended.join(); // it has answered: ending as we speak
    if (start) {
        // What it uses it holds: stop() may let go of it, and the indexer go.
        auto link = std::make_shared<AttemptLink>();
        std::jthread attempt([this, &share, link, open = m_source.openRoot, path = share.config.path] {
            const unsigned long error = open ? open(path) : share::openRoot(path);
            std::lock_guard held(link->mutex);
            if (link->letGo)
                return; // stopped meanwhile: no share to tell
            std::lock_guard lock(m_mutex);
            share.attemptRunning = false;
            // Late, it counts when it got there, and no pass is telling more.
            if (share.attemptWanted || (error == 0 && !share.passRunning)) {
                share.attemptDone = true;
                share.attemptError = error;
                if (!share.attemptWanted)
                    share.wake = true; // got there after all
            }
            share.cv.notify_all();
        });
        std::lock_guard lock(m_mutex);
        share.attempt = std::move(attempt);
        share.attemptLink = std::move(link);
    }
    std::unique_lock lock(m_mutex);
    share.cv.wait_until(lock, stop, Clock::now() + wait, [&] { return share.attemptDone; });
    share.attemptWanted = false;
    if (!share.attemptDone)
        return std::nullopt;
    share.attemptDone = false;
    return share.attemptError;
}

void ShareIndexer::run(std::stop_token stop, Share& share)
{
    const std::wstring path = share.config.path;
    const std::wstring host(unc::host(path));
    const bool isWsl = !unc::wslDistribution(path).empty();
    // Plain SMB can be opened through Windows when the probe says no
    // (tryWindows); WebDAV and WSL answer for themselves.
    const bool smb = !isWsl && !webdav::isWebDavPath(path);
    const bool dav = !isWsl && !smb;
    const auto answers = [&](bool fresh) {
        return m_source.reachable ? m_source.reachable(path, fresh) : share::reachable(host, kProbeWait, fresh);
    };
    const auto availability = [&](bool fresh) {
        using P = share::Problem;
        if (!isWsl)
            return answers(fresh) ? P::None : P::Offline;
        switch (wsl::probe(path, fresh)) {
        case wsl::State::Running: return P::None;
        case wsl::State::Stopped: return P::WslStopped;
        case wsl::State::NotRegistered: return P::WslNotRegistered;
        case wsl::State::Unknown: return P::WslUnavailable;
        }
        return P::WslUnavailable;
    };
    const auto journal = [&](const QString& event, QJsonObject details) {
        {
            std::lock_guard lock(m_mutex);
            details.insert(u"share"_s, placeOf(m_shares, &share));
        }
        diagnostics::record(event, details);
    };
    const auto end = [&] {
        bool removed = false;
        {
            std::lock_guard lock(m_mutex);
            removed = share.removed;
        }
        if (!removed)
            return;
        // Taken out of the search: its entries go too.
        if (const auto index = m_source.index()) {
            auto lock = index->writeLock();
            for (const EntryId r : std::vector<EntryId>(index->roots())) {
                if (index->name(r).starts_with("\\\\") && unc::equal(wideName(*index, r), path))
                    index->remove(r);
            }
        }
        if (m_callbacks.dirty)
            m_callbacks.dirty();
        updateWatches();
    };

    // The index first: loaded from the snapshot, or the local disks read.
    while (!m_source.ready()) {
        if (!wait(stop, share, 500ms))
            return end();
    }
    const auto index = m_source.index();
    FileTime last = 0;
    std::size_t items = 0;
    {
        auto lock = index->writeLock();
        const EntryId root = ensureRoot(*index, path);
        last = index->modified(root);
        items = subtreeSize(*index, root) - 1;
    }
    setStatus(share, [&](Status& s) {
        s.lastVerified = last;
        s.items = items;
    });

    // Read before: verified once a little after start, for what changed
    // while we were not running. Asking for it (or a new share) goes first.
    auto nextPass = Clock::now() + (last != 0 ? kStartupDelay : 0s);
    share::Retry retry(options().retry);
    bool answered = true; // by the probe, the last time it was asked
    bool cutShort = false; // the last wait while away was woken before its time
    // WebDAV: one probe missed while Online (asked again, fresh, before it is
    // called offline); offline for the probe alone, read in full before
    // (back without reading it all again: the next pass when it is due).
    bool davMissed = false;
    bool askAgain = false;
    bool probeOnly = false;
    // WebDAV: Windows opens nothing while WebClient is stopped, and opening
    // does not start it (webdav::startClient). Started here whenever the
    // server answers (a moment, when it runs), for the pass and the user's
    // opens; and its first access to the server, which can be slow, made
    // here (after it started, at the start, after the network changed)
    // rather than at the user's first open. The journal has how long.
    bool warmed = false;
    bool startFailed = false; // journaled once, until it starts
    // `asked`: the user's check (立即核对), which tries to start it even
    // while a failed start keeps the share's own tries back (webdav::Starter).
    const auto warmUp = [&](bool networkChanged, bool asked) {
        const auto since = [](Clock::time_point from) {
            return static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from).count());
        };
        const auto from = Clock::now();
        const bool wasRunning = webdav::clientRunning();
        const bool running = wasRunning
            || webdav::startClient(kClientStart, stop, asked ? webdav::Starter::User : webdav::Starter::Background);
        if (stop.stop_requested())
            return;
        if (!wasRunning && (running || !startFailed))
            journal(u"webclient-start"_s, {{u"milliseconds"_s, since(from)}, {u"running"_s, running}});
        startFailed = !running;
        if (!running || (wasRunning && warmed && !networkChanged))
            return;
        warmed = true;
        const auto askedAt = Clock::now();
        const std::optional<unsigned long> opened = tryWindows(stop, share, kWarmWait);
        if (!stop.stop_requested())
            journal(u"webdav-warm"_s,
                {{u"milliseconds"_s, since(askedAt)}, {u"error"_s, opened ? static_cast<qint64>(*opened) : qint64 {-1}}});
    };
    // While away: tried again in Retry's time; a wait cut short does not make
    // the next one longer.
    const auto waitAway = [&](Clock::duration next) {
        const auto from = Clock::now();
        if (!wait(stop, share, next))
            return false;
        cutShort = Clock::now() - from < next;
        return true;
    };
    // The check the user asked for (ask()) answered: what was found goes in
    // with checking cleared, no sooner than RetryTimes::shown after the
    // click, so that a quick answer is seen to be one.
    const auto checked = [&](const std::function<void(Status&)>& found) {
        {
            std::unique_lock lock(m_mutex);
            // Not cut short by a wake: what woke it is still there after.
            share.cv.wait_until(lock, stop, share.checkAskedAt + retry.times().shown, [] { return false; });
        }
        setStatus(share, [&](Status& s) {
            if (found)
                found(s);
            s.checking = false;
        });
    };
    while (!stop.stop_requested()) {
        const bool again = std::exchange(cutShort, false);
        // Wait out a network change before trying a server.
        Clock::time_point changedAt;
        {
            std::lock_guard lock(m_mutex);
            changedAt = m_networkChangedAt;
        }
        const bool justChanged = !isWsl && Clock::now() - changedAt < kNetworkQuiet;
        if (justChanged && !wait(stop, share, changedAt + kNetworkQuiet - Clock::now()))
            break;

        bool asked = false;
        bool requested = false;
        bool reach = false;
        bool late = false;
        State was = State::Waiting;
        {
            std::lock_guard lock(m_mutex);
            asked = std::exchange(share.checkRequested, false);
            requested = share.verifyRequested;
            reach = std::exchange(share.reachRequested, false);
            late = share.attemptDone; // Windows got there after we stopped waiting
            was = share.status.state;
            share.probedAt = Clock::now(); // lookedAt: asked just now
        }
        const auto askedAt = Clock::now();
        auto problem = availability(justChanged || requested || reach || std::exchange(askAgain, false));
        if (stop.stop_requested())
            break;
        if (problem == share::Problem::None) {
            davMissed = false;
            if (!answered)
                retry.answered();
            std::lock_guard lock(m_mutex);
            share.attemptDone = false; // Windows getting there late: reading tells more
        } else if (dav && was == State::Online && !asked && !std::exchange(davMissed, true)) {
            // Once more before it is called offline (not for the user's
            // check, whose answer is wanted now): one lost answer is no
            // reason to show it offline and read it whole when it is back.
            journal(u"webdav-probe-missed"_s, {});
            askAgain = true;
            if (!wait(stop, share, std::min<Clock::duration>(kDavAskAgain, options().webDavPoll)))
                break;
            continue;
        }
        answered = problem == share::Problem::None;
        if (dav && answered) {
            warmUp(justChanged, asked);
            if (stop.stop_requested())
                break;
        }
        unsigned long error = isWsl ? ERROR_NOT_READY : ERROR_BAD_NETPATH;
        // The probe can be wrong where Windows gets there: Windows is asked
        // before the share is called offline, every so often while it is,
        // and when the user asks. It also tells why (signing in, no such share).
        bool windows = false;
        if (problem != share::Problem::None && smb
            && (was != State::Problem || late || retry.windowsDue(askedAt, asked))) {
            windows = true;
            retry.askedWindows(askedAt);
            const auto opened = tryWindows(stop, share, retry.times().answer);
            if (stop.stop_requested())
                break;
            if (opened && *opened == 0) {
                problem = share::Problem::None;
                if (was != State::Online)
                    journal(u"share-probe-missed"_s, {}); // the probe said no, Windows got there
            } else if (opened) {
                error = *opened;
                if (const auto p = share::classify(error, true); fromServer(p))
                    problem = p;
            }
        }
        if (problem != share::Problem::None) {
            davMissed = false;
            if (was != State::Problem)
                probeOnly = dav && was == State::Online; // WebDAV asks no one else (smb: Windows)
            // The probe alone said no: what Windows said last time stands.
            const bool keep = smb && !windows && was == State::Problem;
            const auto gone = [&](Status& s) {
                s.state = State::Problem;
                if (!keep) {
                    s.problem = problem;
                    s.error = error;
                }
                s.watching = false;
                if (!isWsl)
                    s.lastAttempt = now(); // not every WSL poll: a change for nothing each 10 s
            };
            if (asked) {
                journal(u"share-check"_s, {{u"problem"_s, static_cast<int>(problem)},
                    {u"error"_s, static_cast<qint64>(error)}, {u"windows"_s, windows},
                    {u"milliseconds"_s, static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - askedAt).count())}});
                checked(gone);
            } else {
                setStatus(share, gone);
            }
            updateWatches();
            share::Problem standing = problem;
            unsigned long standingError = error;
            {
                std::lock_guard lock(m_mutex);
                standing = share.status.problem;
                standingError = share.status.error;
            }
            if (isWsl ? !wait(stop, share, kWslPoll)
                      : !waitAway(soonAgain(path, standing, standingError) ? Clock::duration(retry.away(again))
                                                                           : Clock::duration(retry.times().problem)))
                break;
            continue;
        }
        if (asked)
            journal(u"share-check"_s, {{u"windows"_s, windows}, {u"read"_s, true}});

        bool due = false;
        bool building = false;
        bool back = false; // from offline for the probe alone
        {
            std::lock_guard lock(m_mutex);
            building = share.status.lastVerified == 0;
            back = share.status.state == State::Problem && probeOnly;
            due = building || share.verifyRequested || Clock::now() >= nextPass
                || (share.status.state == State::Problem && !probeOnly); // came back: read what we missed
            share.verifyRequested = false;
        }
        probeOnly = false;
        if (back && !due) {
            // Nothing was missed that a pass would find sooner than when it
            // is due (WebDAV tells of no change either way): as it was.
            setStatus(share, [](Status& s) {
                s.state = State::Online;
                s.problem = share::Problem::None;
                s.error = 0;
            });
            journal(u"share-back"_s, {{u"read"_s, false}});
        }
        if (due) {
            const auto passStart = Clock::now();
            const bool read = pass(stop, share, building);
            const auto passTime = Clock::now() - passStart;
            if (stop.stop_requested())
                break;
            if (std::exchange(asked, false))
                checked({}); // the answer: read, or why not
            const Options o = options();
            Status st;
            {
                std::lock_guard lock(m_mutex);
                st = share.status;
            }
            if (read) {
                retry.read();
                const Clock::duration every = isWsl ? Clock::duration(kWslVerify)
                    : st.watching ? o.verifyEvery : o.verifyEveryUnwatched;
                // A share that takes long to read is not read most of the time.
                nextPass = Clock::now()
                    + (every.count() > 0 ? std::max<Clock::duration>(every, passTime * kPassShare) : kForever);
            } else if (st.state == State::Suspect) {
                nextPass = Clock::now() + kSuspectRetry;
            } else if (st.state == State::Problem) {
                if (isWsl) {
                    if (!wait(stop, share, kWslPoll))
                        break;
                    continue;
                }
                if (unreachable(st.problem) && !answers(true)) {
                    answered = false;
                    continue; // gone: tried again from the top (the probe, now and then Windows)
                }
                // Still answering, but not to us (signing in, a connection it
                // drops each time, its shares not there yet): not again at once.
                if (!waitAway(soonAgain(path, st.problem, st.error) ? Clock::duration(retry.away(again))
                                                                    : Clock::duration(retry.times().problem)))
                    break;
                continue;
            }
        }
        if (asked)
            checked({}); // nothing to read after all (asking makes it due: not seen)
        updateWatches();
        applyPending(stop, share);

        // Until the next pass, or something to do (live changes, asked to).
        // WSL and WebDAV tell of nothing: asked now and then whether they are
        // still there, nothing read unless that changed (GitHub #114: a
        // WebDAV server stopped showed 在线 until the hourly pass).
        const auto untilPass = nextPass - Clock::now();
        const auto until = isWsl ? std::min<Clock::duration>(untilPass, kWslPoll)
            : dav                ? std::min<Clock::duration>(untilPass, options().webDavPoll)
                                 : untilPass;
        if (!wait(stop, share, until))
            break;
    }
    end();
}

bool ShareIndexer::pass(std::stop_token stop, Share& share, bool building)
{
    const Options o = options();
    const Crawler crawler(o.rules);
    const auto index = m_source.index();
    const std::wstring& path = share.config.path;
    EntryId root = kNoEntry;
    FileIndex::IdPin pin; // the ids held across write locks stay valid
    {
        auto lock = index->writeLock();
        root = ensureRoot(*index, path);
        pin = index->pinIds();
    }
    {
        std::lock_guard lock(m_mutex);
        share.passRunning = true;
        share.progress = 0;
        share.attemptDone = false; // what Windows said late: this pass says more
    }
    const auto done = [&] {
        std::lock_guard lock(m_mutex);
        share.passRunning = false;
    };
    setStatus(share, [&](Status& s) { s.state = building ? State::Building : State::Verifying; });
    if (m_callbacks.reading)
        m_callbacks.reading(path, true);

    std::stop_source passStop;
    const std::stop_callback forward(stop, [&] { passStop.request_stop(); });
    share::Lister lister(crawler, path, {o.excludedFiles, false, 64 * 1024, &share.progress}, passStop);
    std::vector<EntryId> gone;
    Crawler::SyncStats stats;
    const int threads = building ? o.threads : std::max(2, o.threads / 2);
    crawler.sync(*index, {{root, path, EntryFlag::Remote}}, threads, false, passStop.get_token(),
        [&](const Crawler::Root& dir, DirListing& out) { return lister(dir, out); }, &gone, &stats);
    if (m_callbacks.reading)
        m_callbacks.reading(path, false);
    if (stop.stop_requested()) {
        done();
        return false; // what it added stays; nothing removed
    }
    if (lister.firstError() != 0) {
        share::Problem problem = lister.problem();
        unsigned long error = lister.firstError();
        // Windows hangs up on an SMB 1 server (64, "no longer available")
        // unless SMB 1 was installed again: the probe knows why.
        const bool isWsl = !unc::wslDistribution(path).empty();
        if (isWsl && problem == share::Problem::NeedsSignIn)
            problem = share::Problem::Denied;
        if (!isWsl && (unreachable(problem) || problem == share::Problem::Other)
            && share::probe(unc::host(path), 1500ms, true) == share::Answer::Smb1Only) {
            problem = share::Problem::Smb1Only;
            error = ERROR_SMB1_NOT_AVAILABLE;
        }
        // An http WebDAV server that wants a password Windows will not send
        // in the clear (BasicAuthLevel) is "not there" to Windows (67, 53;
        // 50 when even OPTIONS wants it, share::Lister): asked itself, the
        // server says why (GitHub #114).
        if ((problem == share::Problem::NotFound || unreachable(problem) || problem == share::Problem::Other)
            && webdav::httpSignInBlocked(path) && webdav::wantsClearPassword(path))
            problem = share::Problem::NeedsSignIn;
        qWarning("WinShun: cannot read %ls (error %lu)", path.c_str(), error);
        setStatus(share, [&](Status& s) {
            s.state = State::Problem;
            s.problem = problem;
            s.error = error;
            s.watching = false;
            if (!isWsl)
                s.lastAttempt = now(); // WSL is tried every 10 s: nothing to show for it
        });
        done();
        return false;
    }

    // Believe what is gone? Not when it is much of the share at once.
    std::size_t missing = 0;
    std::size_t live = 0;
    {
        const auto lock = index->readLock();
        for (const EntryId id : gone)
            missing += subtreeSize(*index, id);
        live = subtreeSize(*index, root) - 1;
    }
    bool accept = false;
    {
        std::lock_guard lock(m_mutex);
        // The same finding (within a tenth) a while after the first: believed.
        // Read again sooner (by hand), it keeps the time of the first.
        const bool same = share.suspectMissing != 0 && missing * 10 >= share.suspectMissing * 9
            && missing * 10 <= share.suspectMissing * 11;
        const bool agrees = same && Clock::now() - share.suspectAt >= kSuspectRetry - 5min;
        accept = std::exchange(share.acceptMissing, false) || building || agrees
            || missing <= std::max(kSuspectMin, live / 2);
        if (accept) {
            share.suspectMissing = 0;
        } else if (!same) {
            share.suspectMissing = missing;
            share.suspectAt = Clock::now();
        }
    }
    if (!accept) {
        qWarning("WinShun: %ls has %zu of %zu items no more: not believed yet", path.c_str(), missing, live);
        setStatus(share, [&](Status& s) {
            s.state = State::Suspect;
            s.problem = share::Problem::None;
            s.error = 0;
            s.items = live; // all still in the index
            s.missing = missing;
        });
        done();
        return false;
    }

    for (std::size_t i = 0; i < gone.size(); i += kRemoveBatch) {
        auto lock = index->writeLock();
        for (std::size_t k = i; k < std::min(gone.size(), i + kRemoveBatch); ++k) {
            const EntryId id = gone[k];
            // Removed or moved away by a live change meanwhile: leave it be.
            if (id < index->slotCount() && !index->entry(id).isDeleted() && isUnder(*index, id, root))
                index->remove(id);
        }
    }
    const FileTime verified = now();
    {
        auto lock = index->writeLock();
        index->setModified(root, verified);
        live = subtreeSize(*index, root) - 1;
    }
    setStatus(share, [&](Status& s) {
        s.state = State::Online;
        s.problem = share::Problem::None;
        s.error = 0;
        s.items = live;
        s.lastVerified = verified;
        s.missing = 0;
    });
    done();
    if (m_callbacks.dirty)
        m_callbacks.dirty();
    if (m_callbacks.filesChanged)
        m_callbacks.filesChanged(path);
    return true;
}

void ShareIndexer::applyPending(std::stop_token stop, Share& share)
{
    std::vector<FsChange> changes;
    {
        std::lock_guard lock(m_mutex);
        changes.swap(share.pending);
    }
    if (changes.empty())
        return;
    const Options o = options();
    const Crawler crawler(o.rules);
    std::erase_if(changes, [&](const FsChange& c) { return crawler.isExcludedPath(c.path); });
    // Files written to alone (GitHub #114: a log being written, each second):
    // no name came or went, the count stands without going through the share.
    const bool named = std::ranges::any_of(changes, [](const FsChange& c) { return c.kind != FsChange::Kind::Modified; });
    const auto index = m_source.index();
    FileIndex::IdPin pin;
    {
        const auto lock = index->readLock();
        pin = index->pinIds();
    }
    std::stop_source passStop;
    const std::stop_callback forward(stop, [&] { passStop.request_stop(); });
    share::Lister lister(crawler, share.config.path, {o.excludedFiles}, passStop);
    applyChanges(*index, changes, crawler, passStop.get_token(),
        [&](const Crawler::Root& dir, DirListing& out) { return lister(dir, out); });
    if (named) {
        std::size_t items = 0;
        {
            const auto lock = index->readLock();
            items = subtreeSize(*index, index->findPath(share.config.path)) - 1;
        }
        setStatus(share, [&](Status& s) { s.items = items; });
    }
    if (m_callbacks.dirty)
        m_callbacks.dirty();
    if (m_callbacks.filesChanged)
        m_callbacks.filesChanged(share.config.path);
}

// The shares to watch follow their states, set by a thread of their own
// (watchLoop). Dropping a watch waits until its server gives it up: a server
// that hung holds that up for a minute (GitHub #116), and the share thread
// that called for it answered nothing meanwhile.
void ShareIndexer::updateWatches()
{
    std::lock_guard lock(m_mutex);
    if (m_stopped)
        return;
    m_watchDirty = true;
    if (!m_watchThread.joinable())
        m_watchThread = std::jthread([this](std::stop_token stop) { watchLoop(stop); });
    m_watchCv.notify_all();
}

void ShareIndexer::watchLoop(std::stop_token stop)
{
    // A watcher for names, and one for writes too (m_writeWatcher), each made
    // when first needed. Without the lock: setRoots waits for the handler,
    // which takes it.
    const auto watch = [this](std::unique_ptr<ChangeWatcher>& watcher, const std::vector<std::wstring>& roots,
                           ChangeWatcher::Tell tell) {
        if (!watcher) {
            if (roots.empty())
                return std::vector<std::wstring> {};
            watcher = std::make_unique<ChangeWatcher>([this](std::vector<FsChange>&& c) { onChanges(std::move(c)); }, tell);
        }
        if (watcher->roots() != roots)
            watcher->setRoots(roots); // a server that does not tell is not in roots() after
        return watcher->roots();
    };
    for (;;) {
        std::vector<std::wstring> roots;
        std::vector<std::wstring> writeRoots;
        {
            std::unique_lock lock(m_mutex);
            m_watchCv.wait(lock, stop, [&] { return m_watchDirty; });
            if (stop.stop_requested())
                return; // stopping: what is still watched goes with the watchers (stop())
            m_watchDirty = false; // what changes from here on is set next time round
            for (const auto& s : m_shares) {
                // Neither 9P nor WebDAV tells of changes (ReadDirectoryChangesW: error 1).
                const bool read = s->status.lastVerified != 0 && m_options.watch
                    && unc::wslDistribution(s->config.path).empty() && !webdav::isWebDavPath(s->config.path);
                if (read && (s->status.state == State::Online || s->status.state == State::Verifying))
                    (s->config.content ? writeRoots : roots).push_back(s->config.path);
            }
        }
        // Writes too where the contents are indexed: a file edited on the
        // share is read again soon, not at the next pass (GitHub #114; they
        // come within 0.2-1.8 s over SMB).
        std::vector<std::wstring> watched = watch(m_watcher, roots, ChangeWatcher::Tell::Names);
        std::ranges::move(watch(m_writeWatcher, writeRoots, ChangeWatcher::Tell::Writes), std::back_inserter(watched));
        bool changed = false;
        {
            std::lock_guard lock(m_mutex);
            for (auto& s : m_shares) {
                const bool watching = std::find(watched.begin(), watched.end(), s->config.path) != watched.end();
                if (s->status.watching != watching) {
                    s->status.watching = watching;
                    changed = true;
                }
            }
        }
        if (changed && m_callbacks.changed)
            m_callbacks.changed();
    }
}

void ShareIndexer::onChanges(std::vector<FsChange>&& changes)
{
    std::lock_guard lock(m_mutex);
    for (FsChange& c : changes) {
        const auto it = std::find_if(m_shares.begin(), m_shares.end(),
            [&](const auto& s) { return unc::contains(s->config.path, c.path); });
        if (it == m_shares.end())
            continue;
        Share& s = **it;
        // Lost events (or the server went away): only reading it again tells.
        if (c.kind == FsChange::Kind::Overflow || s.pending.size() >= kMaxPending) {
            s.verifyRequested = true;
            s.pending.clear();
        } else {
            s.pending.push_back(std::move(c));
        }
        s.wake = true;
        s.cv.notify_all();
    }
}

} // namespace ws

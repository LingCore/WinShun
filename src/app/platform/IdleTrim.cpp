#include "IdleTrim.h"

#include "DiagnosticLog.h"

#include <QDateTime>
#include <QDebug>
#include <QGuiApplication>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTimeZone>
#include <QWindow>

#include <algorithm>
#include <thread>
#include <utility>

#include <windows.h>
#include <psapi.h>
#include <realtimeapiset.h>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr int kSettleMs = 5'000; // after busy work ended (poke)
constexpr int kBusyRecheckMs = 15'000; // while the index loads, catches up or reads content
// While unused: background work brings pages back. The content indexer's
// passes (30 s after start, then every 15 minutes and after changes) look
// only at the files that changed; just a full one (the first pass, other
// options, lost track, every 24 hours) goes through the whole file index,
// ~90 MB; copies, journal changes and saves.
// A check reads one number; only a working set above kTrimAbove is emptied.
constexpr int kRecheckMs = 30'000;
constexpr SIZE_T kTrimAbove = 32 * 1024 * 1024; // private working set, over what stayed last time; ~5 MB is what idling itself touches
// More asleep than this between two checks (at most kRecheckMs apart, awake): woken up.
constexpr std::uint64_t kSleptMs = 10'000;

// Since the start, without the time asleep or hibernating (GetTickCount64 counts it).
std::uint64_t awakeMs()
{
    ULONGLONG unbiased = 0; // in 100 ns
    ::QueryUnbiasedInterruptTime(&unbiased);
    return unbiased / 10'000;
}

// Task Manager's Memory column, the private working set. Windows tells it
// from Windows 11 22H2 on (PROCESS_MEMORY_COUNTERS_EX2); before, the call
// succeeds and leaves it 0 (ProcessUsage.cpp), so nothing was ever above
// kTrimAbove: on Windows 10 Win顺 never gave its memory back (a user's memory
// report, 2026-11-17, 1809: ~250 MB all day, no "idle-trim" in a week, an
// EmptyWorkingSet took it to 1 MB). There the whole working set stands in,
// the program files' shared pages with it: read the same way before and
// after, the floor and kTrimAbove hold all the same.
struct WorkingSet {
    SIZE_T bytes = SIZE_MAX; // unknown: trim
    bool exact = true; // the private working set, not the whole one
};

WorkingSet privateWorkingSet()
{
    PROCESS_MEMORY_COUNTERS_EX2 counters {};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        counters = {}; // in case an older Windows refuses the larger size
        counters.cb = sizeof(PROCESS_MEMORY_COUNTERS);
        if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                sizeof(PROCESS_MEMORY_COUNTERS)))
            return {};
    }
    if (counters.PrivateWorkingSetSize != 0) // never really 0 when told
        return {counters.PrivateWorkingSetSize, true};
    return {counters.WorkingSetSize, false};
}

} // namespace

int IdleTrim::memoryLoad() noexcept
{
    MEMORYSTATUSEX memory {sizeof memory};
    return ::GlobalMemoryStatusEx(&memory) ? static_cast<int>(memory.dwMemoryLoad) : -1;
}

IdleTrim::IdleTrim(std::function<const char*()> busy, std::function<bool()> up, std::function<PageSet()> keep, QObject* parent)
    : QObject(parent)
    , m_busy(std::move(busy))
    , m_up(std::move(up))
    , m_keep(std::move(keep))
    , m_lastTickMs(::GetTickCount64())
    , m_lastAwakeMs(awakeMs())
{
    m_timer.setSingleShot(true);
    m_inactive.start();
    connect(&m_timer, &QTimer::timeout, this, &IdleTrim::check);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            m_heldAfterResume = false; // used: emptied a while after, as ever
            m_worker.request_stop(); // do not keep evicting pages while a window wakes up
        } else {
            m_inactive.restart();
            m_timer.start(m_afterUseMs);
        }
    });
    m_timer.start(m_afterUseMs);
}

IdleTrim::~IdleTrim()
{
    m_timer.stop();
    m_worker.request_stop();
}

bool IdleTrim::inUse() const
{
    if (m_up && m_up())
        return true;
    // Qt's application state follows the focus events only; closing the
    // focused window sets the focus window to null without ever going
    // inactive, so the state alone stays Active for good. A window of ours has
    // the focus, or the foreground window (a native dialog too) is ours.
    if (QGuiApplication::applicationState() != Qt::ApplicationActive)
        return false;
    // Minimized, a window may stay the foreground one (and Qt's focus window,
    // the application active) when Windows has no other to give it to: not in
    // use. Measured 2026-11-06: the settings minimized kept ~120 MB for good.
    const HWND foreground = ::GetForegroundWindow();
    if (foreground && ::IsIconic(foreground))
        return false;
    if (const QWindow* focus = QGuiApplication::focusWindow(); focus && focus->visibility() != QWindow::Minimized)
        return true;
    DWORD owner = 0;
    if (foreground)
        ::GetWindowThreadProcessId(foreground, &owner);
    return owner == ::GetCurrentProcessId();
}

QJsonObject IdleTrim::status() const
{
    const char* busy = m_busy ? m_busy() : nullptr;
    const LastTrim last = lastTrim();
    return {
        {u"held_after_resume"_s, m_heldAfterResume},
        {u"held_short_of_memory"_s, m_heldShortAt >= 0},
        {u"memory_load"_s, m_memoryLoad ? m_memoryLoad() : -1},
        {u"waiting_for"_s, busy ? QString::fromLatin1(busy) : QString()},
        {u"window_up"_s, m_up && m_up()}, // the clipboard, a pinned launcher, a dialog's bar
        {u"after_use_ms"_s, m_afterUseMs},
        {u"last_trim_at"_s, last.atMs ? QDateTime::fromMSecsSinceEpoch(last.atMs, QTimeZone::UTC).toString(Qt::ISODate) : QString()},
        {u"last_trim_mb"_s, last.megabytes},
        {u"kept_after_trim_mb"_s, static_cast<qint64>(m_floor.load() >> 20)},
    };
}

void IdleTrim::poke()
{
    // Not asking inUse() here: right after a window closes the foreground may
    // not have moved yet; check() asks again when the time is up.
    if (m_busy())
        return;
    startAfter(static_cast<int>(std::max<qint64>(std::min(kSettleMs, m_afterUseMs), m_afterUseMs - m_inactive.elapsed())));
}

void IdleTrim::startAfter(int ms)
{
    if (!m_timer.isActive() || m_timer.remainingTime() > ms)
        m_timer.start(ms);
}

void IdleTrim::used()
{
    m_heldAfterResume = false;
    m_worker.request_stop();
    m_inactive.restart();
    m_timer.start(m_afterUseMs);
}

void IdleTrim::resumed()
{
    m_worker.request_stop();
    takeSlept(); // told of: the next check does not find that sleep again
    if (std::exchange(m_heldAfterResume, true))
        return;
    qInfo() << "Idle trim: held after the system resumed, until Win顺 is used";
    journal({{u"held"_s, u"resumed"_s}}, true);
}

void IdleTrim::journal(QJsonObject details, bool notable)
{
    if (!m_journal.take(notable, ::GetTickCount64()))
        return;
    if (m_journal.skipped() > 0) // those left out before this one
        details.insert(u"skipped"_s, m_journal.skipped());
    diagnostics::record(u"idle-trim"_s, details);
}

// A check due while the system slept comes at once as it wakes, before
// Windows tells of the resume (GitHub #111: emptied 4.6 s before it).
bool IdleTrim::takeSlept()
{
    const std::uint64_t tick = ::GetTickCount64();
    const std::uint64_t awake = awakeMs();
    const std::uint64_t passed = tick - m_lastTickMs;
    const std::uint64_t awakeFor = std::min(passed, awake - m_lastAwakeMs);
    m_lastTickMs = tick;
    m_lastAwakeMs = awake;
    return passed - awakeFor > kSleptMs;
}

void IdleTrim::noteTrim(qint64 megabytes) noexcept
{
    m_floor = 0; // everything went
    gaveBack(megabytes);
}

void IdleTrim::gaveBack(qint64 megabytes) noexcept
{
    m_lastTrimMb = megabytes;
    m_trimmed = true; // the next opening brings the pages a search reads back (App)
    m_lastTrimAtMs = QDateTime::currentMSecsSinceEpoch();
}

void IdleTrim::check()
{
    if (takeSlept())
        resumed();
    if (inUse()) {
        m_heldAfterResume = false;
        m_timer.start(m_afterUseMs);
        return;
    }
    if (m_heldAfterResume) {
        m_timer.start(kRecheckMs);
        return;
    }
    // In the diagnostics journal, as "idle-trim": an opening right after one
    // brings the pages back (soft faults; hard ones after a hibernation, or
    // once Windows took the standby pages for others), GitHub #111.
    if (const char* busy = m_busy()) {
        if (busy != m_waitingFor) {
            qInfo() << "Idle trim: waiting," << busy;
            journal({{u"waiting"_s, QString::fromLatin1(busy)}}, false);
        }
        m_waitingFor = busy;
        m_timer.start(kBusyRecheckMs);
        return;
    }
    m_waitingFor = nullptr;
    trim();
    m_timer.start(kRecheckMs);
}

void IdleTrim::freed()
{
    trim();
}

void IdleTrim::trim()
{
    if (m_trimming.load(std::memory_order_acquire))
        return;
    const WorkingSet now = privateWorkingSet();
    if (now.bytes <= m_floor.load() + kTrimAbove)
        return;
    // Windows short of memory: held, until there is room again (rechecked
    // as ever). Told once each time it starts holding.
    const int load = m_memoryLoad ? m_memoryLoad() : -1;
    if (load >= kShortOfMemoryLoad) {
        if (m_heldShortAt < 0) {
            qInfo("Idle trim: held, Windows short of memory (%d%% in use)", load);
            journal({{u"held"_s, u"short of memory"_s}, {u"memory_load"_s, load},
                        {now.exact ? u"private_mb"_s : u"working_set_mb"_s, static_cast<qint64>(now.bytes >> 20)}},
                true);
        }
        m_heldShortAt = load;
        return;
    }
    m_heldShortAt = -1;
    const SIZE_T bytes = now.bytes;
    PageSet keep = m_keep ? m_keep() : PageSet();
    const auto megabytes = static_cast<qint64>(bytes / (1024 * 1024));
    const auto keepMb = static_cast<qint64>(keep.bytes() >> 20);
    qInfo() << "Idle trim:" << megabytes << "MB, keeping" << keepMb << "MB";
    QJsonObject details {{now.exact ? u"private_mb"_s : u"working_set_mb"_s, megabytes}};
    if (load >= 0)
        details.insert(u"memory_load"_s, load);
    if (!keep.empty())
        details.insert(u"keep_mb"_s, keepMb);
    // ~10 ms per 100 MB: not on the GUI thread, so a hotkey pressed
    // meanwhile is not held up, and stoppable if the user wakes the app
    // mid-pass.
    m_trimmed = true;
    m_trimming.store(true, std::memory_order_release);
    m_worker = std::jthread([this, bytes, keepMb, keep = std::move(keep), details = std::move(details)](std::stop_token stop) mutable {
        const auto finished = qScopeGuard([this] { m_trimming.store(false, std::memory_order_release); });
        // In the journal once it is over, with what stayed: a giving back
        // that gives little back is what "the memory stays high" looks
        // like from outside (GitHub #459). Far more than was kept: said
        // each time, not just every ten minutes.
        const auto tell = [this, &details](bool notable) {
            QMetaObject::invokeMethod(this, [this, details = std::move(details), notable] { journal(details, notable); },
                Qt::QueuedConnection);
        };
        if (!stop.stop_requested()) {
            if (keep.empty()) { // all of it, in one call
                ::SetProcessWorkingSetSizeEx(::GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
            } else if (const PageSet cold = PageSet::resident().without(keep); !stop.stop_requested()) {
                cold.release(stop);
            }
        }
        if (stop.stop_requested()) { // woken mid-trim: do not record the active working set as m_floor
            details.insert(u"stopped"_s, true);
            tell(false);
            return;
        }
        const SIZE_T after = privateWorkingSet().bytes;
        m_floor = keep.empty() || after == SIZE_MAX ? 0 : after;
        gaveBack(static_cast<qint64>((bytes > after ? bytes - after : 0) / (1024 * 1024)));
        const auto stayedMb = after == SIZE_MAX ? qint64 {-1} : static_cast<qint64>(after >> 20);
        if (stayedMb >= 0)
            details.insert(u"stayed_mb"_s, stayedMb);
        const bool much = stayedMb > keepMb + static_cast<qint64>(kTrimAbove >> 20);
        if (much)
            qInfo() << "Idle trim:" << stayedMb << "MB stayed";
        tell(much);
    });
}

} // namespace ws

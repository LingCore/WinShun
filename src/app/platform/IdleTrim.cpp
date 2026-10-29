#include "IdleTrim.h"

#include "DiagnosticLog.h"

#include <QDebug>
#include <QGuiApplication>
#include <QJsonObject>

#include <algorithm>
#include <thread>
#include <utility>

#include <windows.h>
#include <psapi.h>
#include <realtimeapiset.h>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// Since the last window lost the focus: one searches, opens what was found,
// and often searches again within seconds; emptied in between, every open
// would take the pages back.
constexpr int kAfterUseMs = 20'000;
constexpr int kSettleMs = 5'000; // after busy work ended (poke)
constexpr int kBusyRecheckMs = 15'000; // while the index loads, catches up or reads content
// While unused: background work brings pages back. The content indexer's
// passes (30 s after start, then every 15 minutes and after changes) go
// through the whole file index, ~90 MB; copies, journal changes and saves.
// A check reads one number; only a working set above kTrimAbove is emptied.
constexpr int kRecheckMs = 30'000;
constexpr SIZE_T kTrimAbove = 32 * 1024 * 1024; // private working set; ~5 MB is what idling itself touches
// More asleep than this between two checks (at most kRecheckMs apart, awake): woken up.
constexpr std::uint64_t kSleptMs = 10'000;

// Since the start, without the time asleep or hibernating (GetTickCount64 counts it).
std::uint64_t awakeMs()
{
    ULONGLONG unbiased = 0; // in 100 ns
    ::QueryUnbiasedInterruptTime(&unbiased);
    return unbiased / 10'000;
}

SIZE_T privateWorkingSet()
{
    PROCESS_MEMORY_COUNTERS_EX2 counters {};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)))
        return SIZE_MAX; // unknown: trim
    return counters.PrivateWorkingSetSize;
}

} // namespace

IdleTrim::IdleTrim(std::function<const char*()> busy, std::function<bool()> up, QObject* parent)
    : QObject(parent)
    , m_busy(std::move(busy))
    , m_up(std::move(up))
    , m_lastTickMs(::GetTickCount64())
    , m_lastAwakeMs(awakeMs())
{
    m_timer.setSingleShot(true);
    m_inactive.start();
    connect(&m_timer, &QTimer::timeout, this, &IdleTrim::check);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            m_heldAfterResume = false; // used: emptied a while after, as ever
        } else {
            m_inactive.restart();
            m_timer.start(kAfterUseMs);
        }
    });
    m_timer.start(kAfterUseMs);
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
    if (QGuiApplication::focusWindow())
        return true;
    DWORD owner = 0;
    if (const HWND foreground = ::GetForegroundWindow())
        ::GetWindowThreadProcessId(foreground, &owner);
    return owner == ::GetCurrentProcessId();
}

void IdleTrim::poke()
{
    // Not asking inUse() here: right after a window closes the foreground may
    // not have moved yet; check() asks again when the time is up.
    if (m_busy())
        return;
    startAfter(static_cast<int>(std::max<qint64>(kSettleMs, kAfterUseMs - m_inactive.elapsed())));
}

void IdleTrim::startAfter(int ms)
{
    if (!m_timer.isActive() || m_timer.remainingTime() > ms)
        m_timer.start(ms);
}

void IdleTrim::used()
{
    m_heldAfterResume = false;
    m_inactive.restart();
    m_timer.start(kAfterUseMs);
}

void IdleTrim::resumed()
{
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

void IdleTrim::check()
{
    if (takeSlept())
        resumed();
    if (inUse()) {
        m_heldAfterResume = false;
        m_timer.start(kAfterUseMs);
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
    if (const SIZE_T bytes = privateWorkingSet(); bytes > kTrimAbove) {
        const auto megabytes = static_cast<qint64>(bytes / (1024 * 1024));
        qInfo() << "Idle trim:" << megabytes << "MB";
        journal({{u"private_mb"_s, megabytes}}, false);
        // ~10 ms per 100 MB (wsbench --trim): not on the GUI thread, so a
        // hotkey pressed meanwhile is not held up.
        m_trimmed = true;
        std::thread([] {
            ::SetProcessWorkingSetSizeEx(::GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
        }).detach();
    }
    m_timer.start(kRecheckMs);
}

} // namespace ws

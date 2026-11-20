#include "DisplayPulse.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>
#include <winternl.h> // NTSTATUS, for d3dkmthk.h under WIN32_LEAN_AND_MEAN
#include <dwmapi.h>
#include <d3dkmthk.h>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int kRetries = 20; // a failing wait, again 50 ms later, within one arm
constexpr auto kRetryAfter = 50ms;

// One beat's waits: from the last beat (or the arm), the longest since a take.
struct Beat {
    Clock::time_point last;
    Clock::duration longest {};
    bool failed = false; // can't be followed in this arm

    void start(Clock::time_point now) { *this = {now, {}, false}; }
    void beat(Clock::time_point now)
    {
        longest = std::max(longest, now - last);
        last = now;
    }
    int take(Clock::time_point now)
    {
        if (failed)
            return -1;
        const auto gap = std::max(longest, now - last); // one still open: until now
        longest = {};
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(gap).count());
    }
};

} // namespace

struct DisplayPulse::State {
    std::mutex mutex;
    std::condition_variable wake;
    bool armed = false;
    bool stopping = false;
    bool started = false;
    unsigned arms = 0;
    std::wstring device;
    Beat vblank;
    Beat dwm;
    QString vblankError;
};

namespace {

void closeAdapter(D3DKMT_HANDLE& adapter)
{
    if (!adapter)
        return;
    D3DKMT_CLOSEADAPTER close {adapter};
    ::D3DKMTCloseAdapter(&close);
    adapter = 0;
}

} // namespace

// The desktop's compositions: DwmFlush returns at each, the desktop still or not.
void DisplayPulse::followCompositions(std::shared_ptr<State> state)
{
    std::unique_lock lock(state->mutex);
    for (;;) {
        state->wake.wait(lock, [&] { return state->stopping || (state->armed && !state->dwm.failed); });
        if (state->stopping)
            return;
        lock.unlock();
        const HRESULT result = ::DwmFlush();
        const auto now = Clock::now();
        lock.lock();
        if (FAILED(result))
            state->dwm.failed = true; // until the next arm
        else if (state->armed)
            state->dwm.beat(now);
    }
}

// The screen's vertical blanks, of the monitor armed with.
void DisplayPulse::followVerticalBlanks(std::shared_ptr<State> state)
{
    D3DKMT_HANDLE adapter = 0;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID source = 0;
    std::wstring opened;
    unsigned arm = 0;
    int failures = 0;
    std::unique_lock lock(state->mutex);
    for (;;) {
        state->wake.wait(lock, [&] { return state->stopping || (state->armed && !state->vblank.failed); });
        if (state->stopping)
            break;
        if (arm != state->arms) {
            arm = state->arms;
            failures = 0;
        }
        const std::wstring wanted = state->device;
        lock.unlock();
        NTSTATUS status = 0;
        if (!adapter || wanted != opened) {
            closeAdapter(adapter);
            D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open {};
            wcsncpy_s(open.DeviceName, wanted.c_str(), _TRUNCATE);
            status = ::D3DKMTOpenAdapterFromGdiDisplayName(&open);
            if (status == 0) {
                adapter = open.hAdapter;
                source = open.VidPnSourceId;
                opened = wanted;
            }
        }
        if (status == 0) {
            D3DKMT_WAITFORVERTICALBLANKEVENT wait {};
            wait.hAdapter = adapter;
            wait.VidPnSourceId = source;
            status = ::D3DKMTWaitForVerticalBlankEvent(&wait);
        }
        const auto now = Clock::now();
        lock.lock();
        if (status == 0) {
            if (state->armed)
                state->vblank.beat(now);
            continue;
        }
        // Kept as found (a card reset, a mode changed under it), the adapter
        // opened again a moment later; the gap runs on until a beat comes.
        state->vblankError = u"0x%1"_s.arg(static_cast<quint32>(status), 8, 16, QChar(u'0'));
        lock.unlock();
        closeAdapter(adapter);
        lock.lock();
        if (++failures >= kRetries)
            state->vblank.failed = true; // until the next arm
        else
            state->wake.wait_for(lock, kRetryAfter, [&] { return state->stopping; });
    }
    lock.unlock();
    closeAdapter(adapter);
}

DisplayPulse::DisplayPulse() : m_state(std::make_shared<State>()) {}

DisplayPulse::~DisplayPulse()
{
    {
        const std::lock_guard lock(m_state->mutex);
        m_state->stopping = true;
    }
    m_state->wake.notify_all();
}

void DisplayPulse::arm(const QString& gdiDeviceName)
{
    {
        const std::lock_guard lock(m_state->mutex);
        const auto now = Clock::now();
        m_state->armed = true;
        ++m_state->arms;
        m_state->device = gdiDeviceName.toStdWString();
        m_state->vblank.start(now);
        m_state->dwm.start(now);
        m_state->vblankError.clear();
        if (!m_state->started) {
            // Their own (detached, holding the state): one may be in a wait
            // of the driver's when this ends, and is not waited for.
            m_state->started = true;
            std::thread(followVerticalBlanks, m_state).detach();
            std::thread(followCompositions, m_state).detach();
        }
    }
    m_state->wake.notify_all();
}

void DisplayPulse::disarm()
{
    const std::lock_guard lock(m_state->mutex);
    m_state->armed = false;
}

DisplayPulse::Gaps DisplayPulse::take()
{
    const std::lock_guard lock(m_state->mutex);
    if (!m_state->armed)
        return {};
    const auto now = Clock::now();
    return {m_state->vblank.take(now), m_state->dwm.take(now), m_state->vblankError};
}

} // namespace ws::capture

#pragma once

#include <windows.h>

namespace ws::qos {

// Opt the calling thread out of Windows EcoQoS / background power throttling
// (Low QoS). A resident tray process spends most of its time without a
// foreground window; Windows can demote its threads to efficiency cores or
// throttled clocks, turning the first hotkey wake-up after a long idle into a
// noticeable stall. Calling this only on latency-critical interactive threads
// (GUI, input listeners, low-level hooks, render thread) keeps background
// indexers power-efficient while ensuring instant wake-up.
inline void keepCurrentThreadResponsive() noexcept
{
    THREAD_POWER_THROTTLING_STATE throttling {
        THREAD_POWER_THROTTLING_CURRENT_VERSION,
        THREAD_POWER_THROTTLING_EXECUTION_SPEED,
        0,
    };
    ::SetThreadInformation(::GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof throttling);
}

} // namespace ws::qos

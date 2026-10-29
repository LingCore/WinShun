// What the break clock needs from Windows (src/break-plugin/Schedule.h):
// how long since the last input, and whether now is a bad time.
#pragma once

#include <cstdint>

namespace brk {

// Milliseconds since the last keyboard or mouse input in this session
// (GetLastInputInfo). 0 while a program keeps the display on (a video
// playing, a call: SetThreadExecutionState / PowerSetRequest), so watching
// without touching anything is not taken for being away.
std::int64_t idleMs();

// A full-screen program (game, video, slide show), presentation mode, or
// Focus assist / Do not disturb: a break waits until it is over.
bool quietTime();

// The pieces, for the plugin's diagnostics.
bool displayKeptOn();
int notificationState(); // QUERY_USER_NOTIFICATION_STATE, -1 when unknown
int focusAssist(); // 0 off, 1 priority only, 2 alarms only, -1 unknown

} // namespace brk

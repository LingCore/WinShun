#pragma once

#include <QJsonObject>
#include <QString>

namespace ws::crashrecord {

// Where the program was when it ended at once, for the next start to put in
// the diagnostics journal ("app-crash"). A 1809 cloud PC's copy ended two
// minutes after every start for days with nothing written anywhere: Windows'
// report named only abort() in ucrtbase.dll (a user's report, 2026-11-19).
//
// abort() (std::terminate: an exception nobody caught, out of memory in a
// container) and the structured exceptions nobody handles (an access
// violation) write `file`, then Windows' own error reporting goes on as
// before. Not qFatal: Qt ends the program there without abort(), and its
// message is in the log already. In it: why, the calls of the thread it
// happened on as "module+offset" (CodeAddress), and what the program said it
// was doing (setActivity). Plain Win32 calls into buffers on the stack: the
// heap may be what failed. No paths, no text of the user's.
void install(const QString& file);

// What the program is busy with, for a crash meanwhile ("clipboard text
// recognition: a 4480 × 1440 picture"); empty when nothing in particular.
// Any thread; the last one set is kept.
void setActivity(const QString& text);
void clearActivity();

// The record a crash left in `file`, as the journal's details {reason,
// thread, activity, frames}, and the file removed; empty when there is none.
QJsonObject takeLeft(const QString& file);

} // namespace ws::crashrecord

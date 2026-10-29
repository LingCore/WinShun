#pragma once

// Ctrl+C (and Ctrl+Break, closing the window). The handler runs on a thread
// of its own: it tells Win顺 to cancel what ws asked (the session's action),
// wakes the main thread wherever it waits, and lets it end with exit code
// 130. Should the main thread not get there (blocked writing to a full pipe,
// say), the handler ends the process itself after a moment.

#include <windows.h>

#include <functional>

namespace ws::cli::interrupt {

void install();
bool requested() noexcept;

// Sleeps; false when interrupted (at once if it already was).
bool sleep(DWORD milliseconds);

// What Ctrl+C does besides: send the cancellations, end a blocked read. Run
// on the handler's thread, and again every few milliseconds until the main
// thread is done (a read begun just after the first try is ended too).
void setAction(std::function<void()> action);
void clearAction();

// The main thread is leaving: the handler need not wait.
void finished() noexcept;

} // namespace ws::cli::interrupt

#pragma once

#include <windows.h>

namespace ws::servicepaste {

// Has the index service press the paste keys in `window`, a program run as
// administrator that is in front, with what to paste on the clipboard
// already (GitHub #277). Win顺's UI runs with normal rights, and Windows
// drops the keys it sends such a window (UIPI) without saying so; the
// service's uiAccess helper may press them. It presses that one chord and
// nothing else, only for Win顺's own UI, only in the window in front
// (src/service/InputListener.cpp, pasteInto).
//
// One pipe connection, the helper started and waited for: tens of
// milliseconds, never on the GUI thread.
enum class Outcome {
    Pasted,
    NoService, // none running (or of another major version)
    NotOffered, // one runs, but older, or not for this copy of Win顺
    Failed, // it tried: `error` says why (ERROR_NOT_READY: the window was no longer in front)
};

struct Result {
    Outcome outcome = Outcome::NoService;
    DWORD error = ERROR_SUCCESS;

    explicit operator bool() const noexcept { return outcome == Outcome::Pasted; }
};

Result paste(HWND window);

// For the diagnostics journal.
constexpr const char* name(Outcome outcome) noexcept
{
    switch (outcome) {
    case Outcome::Pasted:
        return "pasted";
    case Outcome::NoService:
        return "no-service";
    case Outcome::NotOffered:
        return "not-offered";
    case Outcome::Failed:
        break;
    }
    return "failed";
}

} // namespace ws::servicepaste

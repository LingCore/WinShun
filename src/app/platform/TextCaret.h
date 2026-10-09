#pragma once

#include <QRect>

#include <windows.h>

#include <chrono>
#include <memory>
#include <optional>

namespace ws::win {

// Where the text caret, the blinking line, is in another program's window:
// the clipboard opens by it, as Windows' own does. Asked in turn, the first
// that knows wins:
// - the system caret (CreateCaret), which classic programs have, and others
//   set for magnifiers (Firefox, the console): read at once, nothing sent;
// - the caret object for accessibility (OBJID_CARET), which Chrome and the
//   programs built on it (Edge, VS Code) serve instead;
// - UI Automation's caret (TextPattern2), or the selection's end: Windows
//   Terminal, the XAML apps. Not in Chrome and Firefox, which would turn
//   their accessibility on for it and stay slower from then on.
// The last two ask the program itself, which can take long or not answer
// at all (hung): on a thread of their own, waited for up to a limit. In
// Edge that took 7 ms, in Windows Terminal 3.
class TextCaret {
public:
    TextCaret(); // starts the thread, which loads UI Automation
    ~TextCaret();

    TextCaret(const TextCaret&) = delete;
    TextCaret& operator=(const TextCaret&) = delete;

    // The caret in `window` (top level), physical pixels on the screen;
    // nothing if it has none or did not tell within `limit`. Asked while
    // that window has the keyboard: programs hide their caret without it.
    std::optional<QRect> find(HWND window, std::chrono::milliseconds limit);

private:
    struct Shared;
    std::shared_ptr<Shared> m_shared; // with the thread, which outlives us when a program hangs
};

} // namespace ws::win

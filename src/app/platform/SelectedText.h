#pragma once

// The `selected_text` tool of a plugin with the selection:read permission
// (0.41.0; the gestures plugin): the words selected in the window in front,
// read with UI Automation, through the clipboard only when asked to copy
// (below). Win顺 reads them, not
// the plugin: a plugin's job keeps it from the clipboard, and a job with any
// such limit keeps it from talking to other programs' windows too (UI
// Automation's calls time out at once, an EM_GETSEL comes back empty;
// docs/pitfalls.md).
//
// Never asked of a Chromium window (browsers, VS Code, Electron, WebView2's
// page) nor of Firefox: asked, they turn their whole accessibility on and stay
// slower, and VS Code goes into its screen reader mode (docs/pitfalls.md). Those
// answer `readable: false`.

#include "McpTools.h"

#include <QJsonObject>

#include <functional>
#include <string_view>

namespace ws::selection {

// The longest text answered, in characters.
inline constexpr int kMostCharacters = 4000;

// Decision for falling back to a clipboard copy chord when UI Automation yields
// no selected text (tested directly in tst_plugin).
struct CopyPlan {
    bool copy = false;
    bool ctrlInsert = false; // mintty / PuTTY: Ctrl+Insert copies without sending SIGINT
    bool readableIfNothing = true;
    bool operator==(const CopyPlan&) const = default;
};

// Terminal windows where an unguarded Ctrl+C would send SIGINT to the running job.
bool isTerminalClass(std::wstring_view className) noexcept;

// Whether `run()` should borrow the clipboard after UI Automation returned no text.
CopyPlan planCopyFallback(bool allowCopy, bool hasText, bool readable, bool known, bool askable,
    std::wstring_view windowClass) noexcept;

// `arguments.window` (optional): the top-level window the plugin means, as a
// number; when another window is in front, nothing is selected there. The
// answer is {text, readable}: `text` empty for nothing selected (or only
// spaces), `readable` false when the window is not asked (Chromium, Firefox)
// or could not be (an administrator's window, a program that did not answer
// within 400 ms). On a thread of its own; `done` is called once, on the
// calling thread.
//
// `arguments.copy` (0.42.0; a shortcut the user pressed, never a gesture):
// whenever UI Automation yields no selected text on a non-terminal window
// (browsers, Firefox, custom controls, or chat windows like QQ where focus stays
// on the input box while a message bubble is selected), Win顺 waits for the
// shortcut's keys to be let go, keeps what is on the clipboard, presses Ctrl+C
// (or Ctrl+Insert in mintty/PuTTY) in the window in front, reads the text that
// came, and puts what it kept back at once. Terminals with UI Automation (Windows
// Terminal, conhost) are never sent Ctrl+C so an empty selection cannot send
// SIGINT. Win顺's own clipboard history records none of it; Windows' (Win+V)
// keeps the words copied (that copy is the program's), not what is put back. A
// clipboard holding more than 64 MB is left alone (`readable` false).
void run(const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done);

} // namespace ws::selection

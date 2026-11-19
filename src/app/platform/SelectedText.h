#pragma once

// The `selected_text` tool of a plugin with the selection:read permission
// (0.41.0; the gestures plugin): the words selected in the window in front,
// read with UI Automation, never through the clipboard. Win顺 reads them, not
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

namespace ws::selection {

// The longest text answered, in characters.
inline constexpr int kMostCharacters = 4000;

// `arguments.window` (optional): the top-level window the plugin means, as a
// number; when another window is in front, nothing is selected there. The
// answer is {text, readable}: `text` empty for nothing selected (or only
// spaces), `readable` false when the window is not asked (Chromium, Firefox)
// or could not be (an administrator's window, a program that did not answer
// within 400 ms). On a thread of its own; `done` is called once, on the
// calling thread.
void run(const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done);

} // namespace ws::selection

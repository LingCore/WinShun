#pragma once

// Carrying out a plugin's declarative `open` (an item's default action, or an
// `open` effect) happens in Win顺 itself, outside the plugin's sandbox. The
// plugin design (§3.3) grants that reach with the `processes` permission: a
// plugin without it may not start programs, so Win顺 will not open one for it
// either. This is the single rule for every such open, kept pure so tst_plugin
// can walk the whole table of targets.
//
// With `processes`: anything (any URL scheme, any path, arguments and a
// working folder). Without it:
//   - URLs: only http, https and mailto (a program or a Control-Panel page
//     hides behind every other scheme: ms-settings:, file:, a custom
//     protocol handler);
//   - paths: only an existing folder, or a file that is not a program or a
//     script -- judged on the target as it will really resolve (environment
//     variables expanded, a file: URL turned into its path, trailing dots and
//     spaces dropped) by PATHEXT, an explicit list of dangerous types, and the
//     Windows association (AssocIsDangerous). A relative path, a bare name
//     (ShellExecute would look it up on PATH), an extension-less name and an
//     alternate data stream are all refused: what they resolve to cannot be
//     vouched for;
//   - no arguments and no working folder.

#include "Items.h"

#include <QString>
#include <QStringList>
#include <QStringView>

#include <functional>

namespace ws::plugin {

// Why an open was refused (OpenGate::None: it is allowed).
enum class OpenGate {
    None,
    Args, // arguments or a working folder without `processes`
    Scheme, // a URL scheme other than http/https/mailto
    Program, // a program, a script, or a path that cannot be vouched for
};

struct OpenRuling {
    OpenGate gate = OpenGate::None;
    QString target; // the target as it resolved, for the notice and the log
    bool allowed() const noexcept { return gate == OpenGate::None; }
    friend bool operator==(const OpenRuling&, const OpenRuling&) = default;
};

// What ruleOpen must learn from the system, given explicitly so the whole
// table is testable without the real environment, file system or registry.
struct OpenProbe {
    std::function<QString(const QString&)> expandEnv; // %VAR% -> value
    std::function<bool(const QString&)> isDirectory; // an existing folder
    std::function<bool(QStringView)> dangerousAssoc; // AssocIsDangerous(ext), ext like ".exe"
    QStringList pathExt; // %PATHEXT%, lower case, each with a leading dot
};

// The real, Win32-backed probe (environment, GetFileAttributes, AssocIsDangerous).
OpenProbe systemProbe();

// The rule. `open.kind == None` is allowed (nothing is carried out).
OpenRuling ruleOpen(const Open& open, bool hasProcesses, const OpenProbe& probe);

// The dangerous extensions Win顺 always refuses, on top of PATHEXT and the
// Windows association (lower case, each with a leading dot). Exposed for tests.
const QStringList& dangerousOpenExtensions();

} // namespace ws::plugin

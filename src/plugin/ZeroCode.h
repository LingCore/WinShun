#pragma once

// Zero-code commands (the plugin design, §1.1 L0): what a manifest's `run`
// answers for a query, made in Win顺 itself, no process. A `url` runs
// through WebShortcut, so %s is encoded exactly as the [WebSearch] entries
// are.

#include "Items.h"
#include "Manifest.h"

#include <QString>

namespace ws::plugin {

// The items of `command` (one with `run`) for `text`, the words after the
// keyword. `locale` picks the titles' language.
Items runZeroCode(const Command& command, const QString& text, const QString& locale);

// A path chosen for a program's %s (Run::suggest, GitHub #344): in quotes
// when it has a space and `args` does not quote %s already.
QString zeroCodePathArgument(const Run& run, const QString& path);

// `url` with %s replaced by `words`, as WebShortcut::searchUrl encodes them;
// without words, the home page (WebShortcut::homeUrl).
QString zeroCodeUrl(const Run& run, const QString& words);

} // namespace ws::plugin

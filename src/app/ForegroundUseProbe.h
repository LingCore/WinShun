#pragma once

#include <QStringList>

class QDir;
namespace ws { class App; }

namespace ws::preview {
// --probe-runtime <folder> --foreground-use-check: what the launcher holds in
// memory while it is up and typed into, with an index of a real size, and how
// long its searches take. See the .cpp.
bool checkForegroundUse(App& app, const QStringList& args, const QDir& output);
// Whether the allocation trace, once started, tells what is held: blocks of
// every size asked for and freed on several threads at once, against what
// it says of them. Run before a traced probe believes it.
bool checkAllocationTrace();
}

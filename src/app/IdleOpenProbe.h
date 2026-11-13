#pragma once

#include <QStringList>

class QDir;
namespace ws { class App; }

namespace ws::preview {
// --probe-runtime <folder> --idle-open-check: how long the launcher and the
// clipboard take to come after Win顺 sat idle (GitHub #280). See the .cpp.
bool checkIdleOpen(App& app, const QStringList& args, const QDir& output);
}

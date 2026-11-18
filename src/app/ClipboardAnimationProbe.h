#pragma once

#include <QStringList>

class QDir;
namespace ws { class App; }

namespace ws::preview {
// --probe-runtime <folder> --clipboard-animation-check: the clipboard window
// comes and goes as the launcher does, by 弹出动画 (GitHub #377). See the .cpp.
bool checkClipboardAnimation(App& app, const QStringList& args, const QDir& output);
}

#pragma once

#include <QStringList>

class QDir;
namespace ws { class App; }

namespace ws::preview {
// --probe-runtime <folder> --search-shadow-check: the search box with each
// 搜索框阴影 choice, its shadow measured on the screen (GitHub #412). See the .cpp.
bool checkSearchShadow(App& app, const QStringList& args, const QDir& output);
}

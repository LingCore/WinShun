#pragma once

#include <QStringList>

class QDir;

namespace ws {
class App;

namespace preview {

// --probe-runtime <folder> --launcher-settings-check [--renderer d3d11]:
// opens the settings window from the search box's logo button and records the
// search box's fade-out (steps, gaps, activation/backdrop changes, hover tip,
// and overlap with the settings window).
bool checkLauncherToSettings(App& app, const QStringList& args, const QDir& output);
bool checkLauncherOpen(App& app, const QStringList& args, const QDir& output);

} // namespace preview
} // namespace ws

#pragma once

#include <QStringList>

class QDir;
namespace ws { class App; }

namespace ws::preview {
// --probe-runtime <folder> --settings-motion-check [--scale 1.5 --renderer d3d11]:
// how smoothly the settings window switches tabs and pages and scrolls. See the .cpp.
bool checkSettingsMotion(App& app, const QStringList& args, const QDir& output);
// The 插件库's list kept by the Win顺 that runs, into the probe's profile
// (read-only), so that the 插件库 tab has its entries without a fetch.
void copyGalleryCacheForMotionCheck();
}

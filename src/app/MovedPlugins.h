#pragma once

// Plugins that came with an earlier Win顺 and are in the plugin library now
// (GitHub #138: 局域网传输 and 按意思找 came with 0.16.0). The installer takes
// their old folders out of <install dir>\plugins and a portable update
// leaves them behind, so who had one on would find it gone. Such a plugin
// is fetched from the library at the next start and installed into the
// user's plugins folder, as 安装 on the 插件库 tab would, without the
// questions: the user had turned it on. Its settings, keywords and the
// permissions said yes to are in [Plugin.<id>], which the new copy reads as
// the old one did; its data (<id>.data) and passwords (Credential Manager)
// are where they were.

#include "Settings.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace ws::plugin {
class Catalog;
}

namespace ws::moved {

// Of the plugins moved to the library, those turned on in `plugins` that
// `catalog` has no copy of (none of the user's, none come with Win顺).
QStringList missing(const plugin::Catalog& catalog, const QList<PluginConfig>& plugins);

// What became of one.
struct Outcome {
    QString id;
    QString name; // the library's, in `locale`; the id when it has none
    QString error; // empty: installed
    bool offline = false; // no answer at all: tried again at the next start, nothing said
};

// Fetches the list from `source` (the library's, or WINSHUN_PLUGIN_INDEX's)
// and installs each of `ids` into `userDir`, on a thread of its own; `done`
// comes on the GUI thread, once, with what became of each.
void install(const QStringList& ids, const QString& userDir, const QString& locale,
    std::function<void(const QList<Outcome>&)> done);

} // namespace ws::moved

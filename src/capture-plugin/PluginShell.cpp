// shell::open and shell::reveal for the screen capture plugin: the plugin
// starts no programs of its own (no `processes` permission, the job allows it
// one process), so Win顺 opens the files and shows them in the file manager
// the user chose, through its tools (open_item, show_in_folder). The rest of
// what the capture uses from the shell, the dialogs and menus, is
// platform/ShellDialogs.cpp, as in Win顺.

#include "PluginShell.h"

#include "Protocol.h"
#include "platform/Shell.h"

#include <QDir>
#include <QJsonObject>
#include <QPointer>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

QPointer<captureplugin::Protocol> g_protocol;

} // namespace

namespace captureplugin {

void setShellProtocol(Protocol* protocol)
{
    g_protocol = protocol;
}

} // namespace captureplugin

namespace shell {

void open(const QString& path, bool, std::function<void(bool)> done)
{
    if (!g_protocol) {
        if (done)
            done(false);
        return;
    }
    g_protocol->tool(u"open_item"_s, {{u"path"_s, QDir::toNativeSeparators(path)}},
        [done = std::move(done)](const QJsonValue& result, const QString& error) {
            if (done)
                done(error.isEmpty() && result.toObject().value(u"opened"_s).toBool(true));
        });
}

void reveal(const QString& path)
{
    if (g_protocol)
        g_protocol->tool(u"show_in_folder"_s, {{u"path"_s, QDir::toNativeSeparators(path)}});
}

void reveal(const QStringList& paths)
{
    if (!paths.isEmpty())
        reveal(paths.first());
}

} // namespace shell

} // namespace ws

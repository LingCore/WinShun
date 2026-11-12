#pragma once

#include <QString>

namespace ws {

// How an install went, for who asked without looking at the page (ws plugin,
// PluginCommands): the plugin is in place; or the page is asking the user
// first (the version there already, a package nobody reviewed, a folder of
// its name that is no plugin) and goes on when they answer; or it was not
// installed, `message` saying why as the page does.
struct PluginInstalled {
    enum class Step { Done, Asking, Failed };
    Step step = Step::Failed;
    QString id; // empty when the package could not be read
    QString version; // the one installed, or asked about
    QString message; // in the user's language
};

} // namespace ws

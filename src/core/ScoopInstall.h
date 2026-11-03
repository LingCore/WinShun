#pragma once

#include <QString>

#include <optional>

namespace ws::scoop {

// A copy of Win顺 that Scoop (https://scoop.sh) installed from the portable
// zip: <root>\apps\winshun\<version>\ with a junction <root>\apps\winshun\current
// to the newest version. Scoop starts and shims it through "current", so
// what is registered with Windows (autostart, Explorer menus, PATH, Opus) must
// name "current" too: the version folder is deleted by `scoop cleanup`, and
// `scoop update` points "current" at the next one.
struct Install {
    QString name;       // the app's folder name under apps\ ("winshun")
    QString root;       // the Scoop folder that holds apps\ (native separators)
    QString version;    // the version folder's name; empty when the path named "current" and it could not be resolved
    bool global = false; // under the global Scoop folder: updating it takes administrator rights

    QString appDir() const; // <root>\apps\<name>
    QString current() const; // <appDir>\current
};

// Whether `folder` (the program's folder, as given: "current" or a version
// folder) is a Scoop-managed install: it holds the manifest and install
// records Scoop writes (scoop-manifest.json + scoop-install.json, or
// manifest.json + install.json before Scoop 0.5) and sits at apps\<name>\<x>.
// `globalRoot`: the global Scoop folder (SCOOP_GLOBAL, else %ProgramData%\scoop).
QString defaultGlobalRoot();
std::optional<Install> detect(const QString& folder, const QString& globalRoot = defaultGlobalRoot());

// `path` (a file or folder in a Scoop install) with its version folder
// replaced by "current", when "current" is a junction to that very folder;
// else `path` as it was. Native separators.
QString stablePath(const QString& path);

// Both paths name the same file, comparing stable forms (one through the
// version folder, one through "current"), without regard to case.
bool sameCopy(const QString& a, const QString& b);

// <root>\shims for a path inside <root>\apps\<name>\...; empty for any other. Scoop
// puts a shim of each `bin` entry there (ws.exe) and that folder on the PATH.
QString shimsFolder(const QString& path);

// "scoop update winshun" (with -g when global).
QString updateCommand(const Install& install);

} // namespace ws::scoop

#pragma once

// Where the interpreters a manifest names are (the plugin design, §5.4),
// and the command line and environment each one wants.

#include "Manifest.h"

#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVersionNumber>

#include <optional>
#include <utility>

namespace ws::plugin::programs {

// The program's path; empty when it is not installed (`hint` says what
// to install, or why the python.exe found cannot run). `python` is
// python.exe itself, never a launcher that starts it as a child -- py.exe,
// a venv's python.exe, uv's trampolines: a job allowing one process
// forbids the child (docs/pitfalls.md, "uv 装的 Python").
QString find(Program program, QString* hint = nullptr);
void setOverride(Program program, const QString& path); // the settings' choice; empty: look again

struct Launch {
    QString exe;
    QStringList args;
    QString cwd;
    QList<std::pair<QString, QString>> env; // what the interpreter needs (PYTHONIOENCODING …)
};
// How to start `runtime` in `pluginDir`; nothing with `error` when the
// program is missing.
std::optional<Launch> launchFor(const Runtime& runtime, const QString& pluginDir, QString* error);

// ---- finding Python (the parts the tests check) -----------------------------

// A python.exe as it is started: the interpreter itself, and for a venv the
// venv's python.exe it stands in for. That one goes in __PYVENV_LAUNCHER__,
// as the venv's own launcher passes it, so sys.prefix and site-packages are
// the venv's in a single process.
struct Python {
    QString exe;
    QString venvLauncher;
    bool operator==(const Python&) const = default;
};

// What `path` (a python.exe from the registry, PATH or the settings) comes
// down to through venvs (pyvenv.cfg's home) and uv's trampolines; nothing,
// with `why`, when it leads to no interpreter a plugin's job can run.
// Paths stay as written: uv's links to a minor version are junctions that
// survive `uv python upgrade`, the folders behind them do not.
std::optional<Python> resolvePython(const QString& path, QString* why = nullptr);

// The `home` of a pyvenv.cfg (PEP 405); empty when it names none.
QString venvHome(QByteArrayView cfg);

// uv's trampoline (the python.exe in uv's bin folder and in its venvs).
struct UvTrampoline {
    enum class Kind : quint8 {
        Unknown = 0, // a newer uv's, or a broken one
        Script = 1, // a tool's launcher (black.exe …)
        Python = 2, // python.exe: a proxy for the interpreter
    };
    Kind kind = Kind::Unknown;
    QString target; // the interpreter it starts; a relative one is from the trampoline's folder
    bool operator==(const UvTrampoline&) const = default;
};
// From its RCDATA resources UV_TRAMPOLINE_KIND and UV_PYTHON_PATH (uv
// after 0.9.0), else the tail older ones carry; nothing when `exe` is no
// trampoline. The file is mapped as data: none of it runs.
std::optional<UvTrampoline> uvTrampoline(const QString& exe);
// The older tail, `tail` being the file's last bytes: the path (UTF-8), its
// length (u32, little-endian), then "UVPY" (or "UVSC" for a script).
std::optional<UvTrampoline> uvTrampolineTail(QByteArrayView tail);

// One PEP 514 registration: Software\Python\<company>\<tag>.
struct RegisteredPython {
    QString company; // PythonCore (python.org), Astral (uv), ContinuumAnalytics (Anaconda) …
    QString tag;
    QVersionNumber version; // SysVersion (3.12), made precise by Version or the tag when they agree (3.12.14)
    QString exe; // InstallPath's ExecutablePath (PythonCore: else python.exe in InstallPath)
};
enum class RegistryView { Native, Bits32 }; // Bits32: what 32-bit programs see of HKLM\Software
// The registrations under `root` ("HKEY_CURRENT_USER\\Software\\Python",
// or HKEY_LOCAL_MACHINE's), all companies but PyLauncher (reserved by
// PEP 514 for py.exe's own). Only read: nothing is created.
QList<RegisteredPython> registeredPythons(const QString& root, RegistryView view = RegistryView::Native);
// The order to try them in: PythonCore first, as py.exe has it (a user of
// python.org's keeps it when uv or Anaconda come along), then the others;
// the newest first in each. Python 2 is left out.
QList<RegisteredPython> ranked(QList<RegisteredPython> entries);

} // namespace ws::plugin::programs

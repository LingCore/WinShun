#pragma once

#include <QString>

namespace ws::commandlaunch {

// How a custom command's program is to be started without a window (GitHub
// #112). A console program that is started without CREATE_NO_WINDOW gets a
// console, and with Windows Terminal as the default terminal that console is
// handed to a Terminal window: SW_HIDE (what ShellExecute and PowerShell's
// -WindowStyle Hidden ask for) is only a request to a window that is already
// being made, so it still flashes. No console window is made at all when the
// process is created with CREATE_NO_WINDOW, so that is what the commands that
// have a console get; a program that has windows of its own is left alone.
enum class Kind {
    Console, // an .exe or .com of the console subsystem
    Gui, // an .exe of the Windows subsystem: shows what it shows, never hidden
    Batch, // .bat, .cmd: run by cmd.exe
    PowerShell, // .ps1: run by powershell.exe
    Other, // anything else (a document, a shortcut, a URL, an unknown program): started as ever
};

// What kind of program `path` is, by its extension and, for .exe and .com,
// the subsystem its PE header says it was built for (Other when that cannot
// be read).
Kind kindOf(const QString& path);

// The subsystem word of a PE image's header (IMAGE_SUBSYSTEM_*), 0 when the
// file is not one or cannot be read.
int peSubsystem(const QString& path);

// `program` as the file that would start, as the shell finds it: a path
// as it is, otherwise searched for in the working folder and on PATH (also
// with .exe, .com, .cmd, .bat or .ps1 added: "code" is code.cmd) and in the
// registry's App Paths (also what ShellExecute reads). Environment variables
// are expanded. Empty when there is none.
QString resolveProgram(const QString& program, const QString& directory = {});

// The programs that run the scripts: cmd.exe and powershell.exe in System32.
struct Tools {
    QString cmd;
    QString powershell;
};
Tools systemTools();

// What to give CreateProcess to run a command with no window.
struct Plan {
    QString application; // lpApplicationName: the program to start
    QString arguments; // what follows the program on its command line
    QString commandLine; // lpCommandLine: the program's path quoted, then `arguments`
    bool ok = false; // false for a Kind that is not started this way
};
// `program` is the resolved file, `arguments` the rest of the command line
// as it is to be seen by the program (the {query} text already put in).
//   Console:    "prog" args
//   Batch:      "cmd.exe" /d /s /c ""script" args"   (/s takes off exactly the outer pair)
//   PowerShell: "powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "script" args
// -NonInteractive so that a script waiting for an answer fails instead of
// waiting for ever, where nobody can see it. `visible`: the same lines for a
// window that is to be seen (a script run as administrator, whose window is
// the user's to see): without -NonInteractive. A .bat or .ps1 handed to
// ShellExecute's "runas" is not run the way it is without it (cmd.exe's
// /c without /s mangles a quoted path with quoted arguments).
Plan silentPlan(const QString& program, const QString& arguments, Kind kind, const Tools& tools, bool visible = false);

// Whether text typed after the keyword may be an argument of a batch file.
// cmd.exe reads & | < > ^ and %VARIABLES% even in an argument that is not
// quoted, and its quotes are not the ones other programs use, so a word with
// any of them is not passed to it.
bool safeForCmd(const QString& words);

// The command line the elevated helper is given (WinShun.exe --run-hidden
// <line>): the line a Plan has, run with no console window by a process that
// has none itself, since the one that asks for administrator rights shows
// none either.
inline constexpr const char16_t kHiddenSwitch[] = u"--run-hidden";
// The same helper for a script run as administrator with its window: elevated
// cmd.exe and powershell.exe start in System32 whatever folder they are asked
// for (ShellExecute's lpDirectory), the helper keeps the folder it was given.
inline constexpr const char16_t kShownSwitch[] = u"--run-shown";

} // namespace ws::commandlaunch

#pragma once

// The commands (docs/design-cli.md §3), one table: a command is a row with
// its names, help and options, and the function that runs it. A new command
// is a new row.

#include "Args.h"
#include "Context.h"
#include "Result.h"
#include "Text.h"

#include <span>
#include <string_view>

namespace ws::cli {

struct Command {
    std::wstring_view name;
    std::wstring_view alias; // another name ("search" for find), or none
    Text summary; // one line, in the list of commands
    Text usage; // "ws find <查询> [选项]"
    Text details; // what it does, a few lines
    Text examples; // one per line
    std::span<const Opt> options; // its own; the global ones always come too
    bool talks = true; // asks Win顺 (local ones: skill, help)
    Result<Exit> (*run)(Context&) = nullptr;
};

std::span<const Command> commandTable();
// By name or alias, any case; nullptr when it is no command.
const Command* commandNamed(std::wstring_view word);
const Command& defaultCommand(); // find

// The commands' runs (Find.cpp, Grep.cpp, …).
Result<Exit> runFind(Context& context);
Result<Exit> runGrep(Context& context);
Result<Exit> runRecent(Context& context);
Result<Exit> runText(Context& context);
Result<Exit> runLs(Context& context);
Result<Exit> runOpen(Context& context);
Result<Exit> runClip(Context& context);
Result<Exit> runStatus(Context& context);
Result<Exit> runSkill(Context& context);
Result<Exit> runHelp(Context& context);

// ws --version; ws with nothing on a console (help and a status line).
Result<Exit> runVersion(Context& context);
Result<Exit> runBare(Context& context);

// The help (Help.cpp): the whole of it, or one command's. In the current language.
std::wstring helpText(const Command* command);
std::wstring shortHelp();

// The query of find-like commands: the words joined, --ext added (ext:pdf,docx).
std::wstring buildQuery(const Parsed& args);
// --ext's extensions: "pdf, .docx;xlsx" → pdf, docx, xlsx.
std::vector<std::wstring> extensionList(const Parsed& args);
// --in folders, absolute; a missing one is an error (exit 6).
Result<std::vector<std::wstring>> inFolders(const Parsed& args);
// --files / --folders as Win顺's kind (`fallback` when neither).
Result<std::string> kindOf(const Parsed& args, std::string_view fallback);
// -n as a limit for Win顺: 0 is `most`.
Result<long long> limitOf(const Parsed& args, long long most);

} // namespace ws::cli

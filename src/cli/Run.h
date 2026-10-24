#pragma once

// A whole run of ws: the arguments in, the exit code out.

#include <windows.h>

#include <span>
#include <string>

namespace ws::cli {

int run(std::span<const std::wstring> args);

// Whether `codePage` can write Chinese: if not, Chinese messages would come out
// as rows of '?' (an English system locale's 437 or 1252 under PowerShell, or
// --encoding ansi there), and ws speaks English instead.
bool holdsChinese(UINT codePage);

// The note about `lines` lines with characters the code page could not write,
// and what to do about it: not --encoding utf8 under PowerShell (there ws
// follows the console's output code page, as PowerShell decodes with it, and
// UTF-8 bytes would be garbled for a cmdlet): the profile; --json where the
// command has it.
std::wstring lossyNote(std::size_t lines, UINT codePage, bool powerShell, bool hasJson);

} // namespace ws::cli

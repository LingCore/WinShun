#pragma once

// The SKILL.md built into ws.exe (resources/ws.rc), for AI agents.

#include "Result.h"

#include <string>
#include <string_view>

namespace ws::cli {

// Its text, UTF-8 as the file is; empty when this program has none (the tests'
// build of the sources).
std::string_view skillText();

// Where Claude Code looks for it: %CLAUDE_CONFIG_DIR%, else <the user's profile
// folder, from the shell>\.claude; then skills\winshun\SKILL.md. A failure (exit
// 7) when CLAUDE_CONFIG_DIR is not a full path (relative to what?) or the
// profile folder cannot be found: nothing is written to a guess such as C:\.claude.
Result<std::wstring> claudeSkillFile();

} // namespace ws::cli

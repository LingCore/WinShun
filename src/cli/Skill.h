#pragma once

// The SKILL.md built into ws.exe (resources/ws.rc), for AI agents.

#include <string>
#include <string_view>

namespace ws::cli {

// Its text, UTF-8 as the file is; empty when this program has none (the tests'
// build of the sources).
std::string_view skillText();

// Where Claude Code looks for it: %CLAUDE_CONFIG_DIR%, else
// %USERPROFILE%\.claude; then skills\winshun\SKILL.md.
std::wstring claudeSkillFile();

} // namespace ws::cli

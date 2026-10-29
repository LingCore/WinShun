#pragma once

// What ws reads of Win顺's settings file (WinShun.ini, written by QSettings):
// the connection switch and the interface language. Read on every run, so
// plain and quick: the file is scanned once, no Qt.

#include <string>
#include <string_view>

namespace ws::cli {

struct Config {
    std::wstring file; // the settings file looked at (it may not exist)
    bool found = false; // it was there and could be read
    // [Mcp] Enabled: “允许 AI 助手和命令行连接”. Off until the user turns it on.
    bool connectionOn = false;
    // [Appearance] Language: system, zh or en.
    std::wstring language = L"system";
};

// The settings file Win顺 uses for this ws: WS_SETTINGS (the tests'), else
// the portable copy's (<install>\data\WinShun\WinShun.ini when there is a
// <install>\data folder; ws.exe sits in <install>\bin), else the user's
// (%APPDATA%\WinShun\WinShun.ini).
std::wstring configFile();

Config readConfig();
Config readConfig(const std::wstring& file);

// The two values from a settings file's text (UTF-8), as QSettings would read
// them: sections and keys as written, a bool false only for "", "0" and
// "false" (any case).
Config parseConfig(std::string_view text);

} // namespace ws::cli

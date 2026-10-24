#include "Config.h"

#include "Encoding.h"
#include "Win.h"

#include <windows.h>

namespace ws::cli {

namespace {

std::string_view trimmed(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i]))
            return false;
    }
    return true;
}

// QSettings writes a string with quotes when it has to (leading spaces, a
// comma, a semicolon…); the values read here never need them, but take them off.
std::string_view unquoted(std::string_view value) noexcept
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        return value.substr(1, value.size() - 2);
    return value;
}

bool qtBool(std::string_view value) noexcept
{
    value = trimmed(unquoted(trimmed(value)));
    return !(value.empty() || value == "0" || equalsIgnoringCase(value, "false"));
}

// <folder of this program>, without the backslash at the end.
std::wstring ownFolder()
{
    std::wstring path = modulePath();
    const std::size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool isFolder(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

std::wstring configFile()
{
    if (std::wstring forced = environment(L"WS_SETTINGS"); !forced.empty())
        return forced;
    // Portable: Win顺 keeps its settings in <its folder>\data when that folder
    // is there (Settings::portableFolder); ws is in <its folder>\bin.
    std::wstring install = ownFolder();
    if (const std::size_t slash = install.find_last_of(L'\\');
        slash != std::wstring::npos && ::CompareStringOrdinal(install.c_str() + slash + 1, -1, L"bin", -1, TRUE) == CSTR_EQUAL)
        install.resize(slash);
    if (!install.empty() && isFolder(install + L"\\data"))
        return install + L"\\data\\WinShun\\WinShun.ini";
    // QSettings' user scope: the roaming application data folder. From the
    // environment, not SHGetFolderPath: no shell32.dll on every run.
    std::wstring appData = environment(L"APPDATA");
    if (appData.empty())
        appData = environment(L"USERPROFILE") + L"\\AppData\\Roaming";
    return appData + L"\\WinShun\\WinShun.ini";
}

Config readConfig()
{
    return readConfig(configFile());
}

Config readConfig(const std::wstring& file)
{
    Config config;
    std::string text;
    if (readFile(file, text, 16u << 20)) {
        config = parseConfig(text);
        config.found = true;
    }
    config.file = file;
    return config;
}

Config parseConfig(std::string_view text)
{
    Config config;
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    std::string_view section;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = trimmed(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#')
            continue;
        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            section = close == std::string_view::npos ? line.substr(1) : line.substr(1, close - 1);
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string_view key = trimmed(line.substr(0, equals));
        const std::string_view value = trimmed(line.substr(equals + 1));
        if (equalsIgnoringCase(section, "Mcp") && equalsIgnoringCase(key, "Enabled"))
            config.connectionOn = qtBool(value);
        else if (equalsIgnoringCase(section, "Appearance") && equalsIgnoringCase(key, "Language"))
            config.language = wide(trimmed(unquoted(value)));
    }
    return config;
}

} // namespace ws::cli

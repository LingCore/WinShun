// skill: the SKILL.md built into ws.exe (docs/design-cli.md §11), printed or
// installed for an AI client.

#include "Commands.h"
#include "Encoding.h"
#include "Messages.h"
#include "Skill.h"
#include "Win.h"

namespace ws::cli {

namespace {

constexpr std::wstring_view kClients[] = {L"claude"};

// Every folder on the way to `folder`, made. The parts that cannot be (a
// drive, a share's server) are there or not: the last one says.
bool makeFolders(const std::wstring& folder)
{
    for (std::size_t at = folder.find(L'\\', 1); at != std::wstring::npos; at = folder.find(L'\\', at + 1))
        ::CreateDirectoryW(folder.substr(0, at).c_str(), nullptr);
    if (::CreateDirectoryW(folder.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = ::GetFileAttributesW(folder.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    return false;
}

// The installed copy also says where this ws.exe is: a terminal opened
// before the installer put bin\ on PATH does not find `ws` yet. Before the
// first section, in the file's own line ends.
std::string installedText(std::string_view text)
{
    const std::string_view end = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    const std::string exe = utf8(modulePath());
    std::string line = "If `ws` is not found (PATH not updated in this terminal yet), run it by its full path: `\"" + exe
        + "\"` (in PowerShell: `& \"" + exe + "\"`).";
    line += end;
    line += end;
    std::string out(text);
    const std::size_t section = out.find(std::string(end) + "## ");
    if (section == std::string::npos)
        return out + std::string(end) + line;
    out.insert(section + end.size(), line);
    return out;
}

// `bytes` as `file`, all or nothing: written beside it, then put in its place
// (an earlier copy, read-only even, is ours to replace).
bool replaceFile(const std::wstring& file, std::string_view bytes)
{
    const std::wstring temporary = file + L".new";
    {
        const Handle handle(::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        if (!handle || !::WriteFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size()) {
            const DWORD error = ::GetLastError();
            ::DeleteFileW(temporary.c_str());
            ::SetLastError(error);
            return false;
        }
    }
    const DWORD attributes = ::GetFileAttributesW(file.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
        ::SetFileAttributesW(file.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    if (!::MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = ::GetLastError();
        ::DeleteFileW(temporary.c_str());
        ::SetLastError(error);
        return false;
    }
    return true;
}

} // namespace

std::string_view skillText()
{
    const HRSRC resource = ::FindResourceW(nullptr, L"SKILL", MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (!resource)
        return {};
    const HGLOBAL loaded = ::LoadResource(nullptr, resource);
    const DWORD size = ::SizeofResource(nullptr, resource);
    const void* data = loaded ? ::LockResource(loaded) : nullptr;
    return data ? std::string_view(static_cast<const char*>(data), size) : std::string_view();
}

std::wstring claudeSkillFile()
{
    std::wstring folder = environment(L"CLAUDE_CONFIG_DIR");
    if (folder.empty())
        folder = environment(L"USERPROFILE") + L"\\.claude";
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/'))
        folder.pop_back();
    return folder + L"\\skills\\winshun\\SKILL.md";
}

Result<Exit> runSkill(Context& context)
{
    const std::string_view text = skillText();
    if (text.empty())
        return Failure {Exit::Failed, std::wstring(tr(msg::kSkillMissing))};
    if (!context.args.has(Opt::Install)) {
        context.out.writeUtf8(text);
        return Exit::Found;
    }
    Result<std::wstring_view> client = choiceOption(context.args, Opt::Install, L"", kClients);
    if (!client)
        return std::move(client).failure();
    const std::wstring file = absolutePath(claudeSkillFile());
    const std::wstring folder = file.substr(0, file.find_last_of(L'\\'));
    if (!makeFolders(folder) || !replaceFile(file, installedText(text)))
        return Failure {Exit::Failed, fmt(msg::kSkillWriteFailed, file, systemMessage(::GetLastError()))};
    // The path alone on stdout: the settings page's 装 Skill reads it.
    context.out.write(file);
    context.out.newline();
    context.note(tr(msg::kSkillInstalled));
    return Exit::Found;
}

} // namespace ws::cli

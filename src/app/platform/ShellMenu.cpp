#include "ShellMenu.h"

#include "ScoopInstall.h"
#include "AutostartRun.h" // runCommandProgram, runCommandStarts: the program a command line starts
#include "ExplorerLanguage.h"

#include <QCoreApplication>
#include <QDir>

#include <windows.h>
#include <shlobj.h>

#include <span>
#include <string>
#include <string_view>

namespace ws::shellmenu {

namespace {

// Where 用 Win顺 搜索此处 goes: a folder, a folder's empty space (%V is the
// folder shown), a drive's root.
constexpr const wchar_t* kSearchHereKeys[] {
    L"Software\\Classes\\Directory\\shell\\WinShun.SearchHere",
    L"Software\\Classes\\Directory\\Background\\shell\\WinShun.SearchHere",
    L"Software\\Classes\\Drive\\shell\\WinShun.SearchHere",
};

// Where 粘贴为文件 goes: a folder's empty space, the desktop's among them (a
// drive's root shown is a folder too). Not on a folder itself: right there,
// Explorer's own 粘贴 pastes into the folder selected, not beside it.
constexpr const wchar_t* kPasteFileKeys[] {
    L"Software\\Classes\\Directory\\Background\\shell\\WinShun.PasteFile",
};

std::wstring readString(HKEY root, const std::wstring& key, const wchar_t* name)
{
    DWORD bytes = 0;
    if (::RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0)
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
        return {};
    value.resize(::wcsnlen(value.c_str(), value.size()));
    return value;
}

// Whether it was written (false when it was so already, or could not be).
bool writeString(const std::wstring& key, const wchar_t* name, const std::wstring& value)
{
    if (readString(HKEY_CURRENT_USER, key, name) == value)
        return false;
    return ::RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)))
        == ERROR_SUCCESS;
}

// Whether it was there and is now gone.
bool deleteValue(const std::wstring& key, const wchar_t* name)
{
    return ::RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), name) == ERROR_SUCCESS;
}

// The WinShunSearch.exe beside this program, and the folder it is in (a
// Scoop install's "current", not the version folder `scoop cleanup` deletes).
struct Stub {
    std::wstring folder = scoop::stablePath(QCoreApplication::applicationDirPath()).toStdWString();
    std::wstring path = folder + L"\\WinShunSearch.exe";
};

// Sets the verb at `keys` to run `arguments` with the stub (`on`), or takes
// away the verbs there that start this copy's stub or one that is gone.
void setVerbs(std::span<const wchar_t* const> keys, bool on, std::wstring_view title, std::wstring_view arguments)
{
    const Stub stub;
    if (on && ::GetFileAttributesW(stub.path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // a build without it (run from the build folder)
    const std::wstring command = L"\"" + stub.path + L"\" " + std::wstring(arguments);
    bool changed = false;
    for (const wchar_t* key : keys) {
        const std::wstring commandKey = std::wstring(key) + L"\\command";
        if (!on) {
            // Only ours: a portable copy elsewhere may have set them since.
            // A copy that is gone (a portable folder deleted) left a verb
            // that does nothing: that goes too.
            const std::wstring current = readString(HKEY_CURRENT_USER, commandKey, nullptr);
            const std::wstring program = autostart::runCommandProgram(current);
            if (!program.empty()
                && (scoop::sameCopy(QString::fromStdWString(program), QString::fromStdWString(stub.path))
                    || ::GetFileAttributesW(program.c_str()) == INVALID_FILE_ATTRIBUTES))
                changed |= ::RegDeleteTreeW(HKEY_CURRENT_USER, key) == ERROR_SUCCESS;
            continue;
        }
        changed |= writeString(key, L"MUIVerb", std::wstring(title));
        changed |= writeString(key, L"Icon", stub.folder + L"\\WinShun.exe,0");
        // Empty space has nothing selected, so "Single" hides the verb there
        // (0.8.0 wrote it everywhere; take it back off).
        if (std::wstring_view(key).find(L"\\Background\\") == std::wstring_view::npos)
            changed |= writeString(key, L"MultiSelectModel", L"Single");
        else
            changed |= deleteValue(key, L"MultiSelectModel");
        changed |= writeString(commandKey, nullptr, command);
    }
    if (changed)
        ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

} // namespace

// The titles are plain text in the language Windows is shown in (no @exe,-id
// indirection: Explorer's MUI cache would keep the old words after an update),
// written again whenever it differs, so a changed Windows language follows.
void apply(bool on)
{
    setVerbs(kSearchHereKeys, on, explorerlang::searchHereTitle(explorerlang::windowsIsEnglish()),
        L"--search-here \"%V\"");
}

void applyPasteFile(bool on)
{
    // No --en any more: the stub reads the language of Windows itself. An older
    // verb that has it differs, so it is written again without.
    setVerbs(kPasteFileKeys, on, explorerlang::pasteFileTitle(explorerlang::windowsIsEnglish()),
        L"--paste-file \"%V\"");
}

} // namespace ws::shellmenu

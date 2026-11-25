#include "ShellMenu.h"

#include "AutostartRun.h" // runCommandProgram, runCommandStarts: the program a command line starts
#include "ExplorerLanguage.h"
#include "PictureFile.h"
#include "ScoopInstall.h"

#include <QCoreApplication>
#include <QDir>

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ws::shellmenu {

namespace {

// Where 用 Win顺 搜索此处 goes: a folder, a folder's empty space (%V is the
// folder shown), a drive's root.
const std::wstring kSearchHereKeys[] {
    L"Software\\Classes\\Directory\\shell\\WinShun.SearchHere",
    L"Software\\Classes\\Directory\\Background\\shell\\WinShun.SearchHere",
    L"Software\\Classes\\Drive\\shell\\WinShun.SearchHere",
};

// Where 粘贴为文件 goes: a folder's empty space, the desktop's among them (a
// drive's root shown is a folder too). Not on a folder itself: right there,
// Explorer's own 粘贴 pastes into the folder selected, not beside it.
const std::wstring kPasteFileKeys[] {
    L"Software\\Classes\\Directory\\Background\\shell\\WinShun.PasteFile",
};

// Where 贴到屏幕 goes: each picture extension's own key under
// SystemFileAssociations, whatever program the files open with. Explorer
// lists these with the file type's own verbs at the top of the menu (打开,
// 编辑, 设置为桌面背景) and runs them for every extension, .webp, .heic and
// .avif among them. Not SystemFileAssociations\image: Windows leaves that out
// or fails with ERROR_NO_ASSOCIATION (0x80070483) for those three. Not
// *\shell with an AppliesTo filter either: that puts it below every other
// program's entry for all files, a long way down the menu.
std::vector<std::wstring> pinPictureKeys()
{
    std::vector<std::wstring> keys;
    keys.reserve(kPinnablePictureExtensions.size());
    for (const QStringView extension : kPinnablePictureExtensions) {
        keys.push_back(L"Software\\Classes\\SystemFileAssociations\\." + extension.toString().toStdWString()
            + L"\\shell\\WinShun.PinPicture");
    }
    return keys;
}

// Where earlier versions put it (0.45.0; up to 0.44.0): ours goes from there
// whether the entry is on or off, so that an updated copy never shows two.
const std::wstring kLegacyPinPictureKeys[] {
    L"Software\\Classes\\*\\shell\\WinShun.PinPicture",
    L"Software\\Classes\\SystemFileAssociations\\image\\shell\\WinShun.PinPicture",
};

// Where Win顺 命令 › goes: any file, any folder.
constexpr const wchar_t* kCommandsKeys[] {
    L"Software\\Classes\\*\\shell\\WinShun.Commands",
    L"Software\\Classes\\Directory\\shell\\WinShun.Commands",
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

// Whether a string value is there, an empty one too (readString cannot tell).
bool hasString(const std::wstring& key, const wchar_t* name)
{
    return ::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
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

// Takes away the keys above `key` that are left with nothing in them (no
// value, no key), up to Software\Classes: a verb removed leaves no empty
// "shell" or ".webp" behind.
void removeEmptyParents(std::wstring key)
{
    constexpr std::wstring_view kRoot = L"Software\\Classes";
    for (;;) {
        const auto slash = key.rfind(L'\\');
        if (slash == std::wstring::npos || slash <= kRoot.size())
            return;
        key.resize(slash);
        HKEY handle = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS)
            return;
        DWORD keys = 0, values = 0;
        const bool read = ::RegQueryInfoKeyW(handle, nullptr, nullptr, nullptr, &keys, nullptr, nullptr, &values,
                              nullptr, nullptr, nullptr, nullptr)
            == ERROR_SUCCESS;
        ::RegCloseKey(handle);
        if (!read || keys > 0 || values > 0 || ::RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()) != ERROR_SUCCESS)
            return;
    }
}

bool removeOwnedVerbs(std::span<const std::wstring> keys, const std::wstring& stubPath)
{
    bool changed = false;
    for (const std::wstring& key : keys) {
        const std::wstring commandKey = key + L"\\command";
        // Only ours: a portable copy elsewhere may have set them since.
        // A copy that is gone (a portable folder deleted) left a verb
        // that does nothing: that goes too.
        const std::wstring current = readString(HKEY_CURRENT_USER, commandKey, nullptr);
        const std::wstring program = autostart::runCommandProgram(current);
        if (!program.empty()
            && (scoop::sameCopy(QString::fromStdWString(program), QString::fromStdWString(stubPath))
                || ::GetFileAttributesW(program.c_str()) == INVALID_FILE_ATTRIBUTES)
            && ::RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()) == ERROR_SUCCESS) {
            removeEmptyParents(key);
            changed = true;
        }
    }
    return changed;
}

// Sets the verb at `keys` to run `arguments` with the stub (`on`), or takes
// away the verbs there that start this copy's stub or one that is gone.
// `each`: with several items chosen the verb stays, run once an item.
// `legacyKeys`: where it was before, taken away either way.
void setVerbs(std::span<const std::wstring> keys, bool on, std::wstring_view title, std::wstring_view arguments,
    bool each = false, std::span<const std::wstring> legacyKeys = {})
{
    const Stub stub;
    if (on && ::GetFileAttributesW(stub.path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // a build without it (run from the build folder)
    bool changed = removeOwnedVerbs(legacyKeys, stub.path);
    if (!on) {
        changed |= removeOwnedVerbs(keys, stub.path);
        if (changed)
            ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return;
    }
    const std::wstring command = L"\"" + stub.path + L"\" " + std::wstring(arguments);
    for (const std::wstring& key : keys) {
        const std::wstring commandKey = key + L"\\command";
        changed |= writeString(key, L"MUIVerb", std::wstring(title));
        changed |= writeString(key, L"Icon", stub.folder + L"\\WinShun.exe,0");
        // Empty space has nothing selected, so "Single" hides the verb there
        // (0.8.0 wrote it everywhere; take it back off).
        if (key.find(L"\\Background\\") == std::wstring::npos)
            changed |= writeString(key, L"MultiSelectModel", each ? L"Document" : L"Single");
        else
            changed |= deleteValue(key, L"MultiSelectModel");
        changed |= writeString(commandKey, nullptr, command);
    }
    if (changed)
        ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// The winshun:// protocol (GitHub #358): a web page, a note or a script that
// opens such a link runs the stub with it. Under HKCU like the verbs, so no
// administrator is needed.
constexpr const wchar_t* kProtocolKey = L"Software\\Classes\\winshun";

} // namespace

// The titles are plain text in the language Windows is shown in (no @exe,-id
// indirection: Explorer's MUI cache would keep the old words after an update),
// written again whenever it differs, so a changed Windows language follows.
void apply(bool on)
{
    setVerbs(kSearchHereKeys, on, explorerlang::searchHereTitle(explorerlang::windowsIsEnglish()),
        L"--search-here \"%V\"");
}

void applyUriProtocol(bool on)
{
    const Stub stub;
    if (on && ::GetFileAttributesW(stub.path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // a build without it (run from the build folder)
    const std::wstring key = kProtocolKey;
    const std::wstring commandKey = key + L"\\shell\\open\\command";
    bool changed = false;
    if (!on) {
        // Only ours, or a copy's that is gone (as setVerbs).
        const std::wstring current = readString(HKEY_CURRENT_USER, commandKey, nullptr);
        const std::wstring program = autostart::runCommandProgram(current);
        if (!program.empty()
            && (scoop::sameCopy(QString::fromStdWString(program), QString::fromStdWString(stub.path))
                || ::GetFileAttributesW(program.c_str()) == INVALID_FILE_ATTRIBUTES))
            changed = ::RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()) == ERROR_SUCCESS;
    } else {
        changed |= writeString(key, nullptr, L"URL:Win顺"); // what Windows shows for the protocol
        // An empty value marks it as a protocol: set by hand, as writeString takes empty for "not there".
        if (!hasString(key, L"URL Protocol"))
            changed |= ::RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"URL Protocol", REG_SZ, L"", sizeof(wchar_t))
                == ERROR_SUCCESS;
        changed |= writeString(key + L"\\DefaultIcon", nullptr, stub.folder + L"\\WinShun.exe,0");
        // %1 in quotes: the link, as it was given (the stub reads it from the raw command line).
        changed |= writeString(commandKey, nullptr, L"\"" + stub.path + L"\" --uri \"%1\"");
    }
    if (changed)
        ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void applyPasteFile(bool on)
{
    // No --en any more: the stub reads the language of Windows itself. An older
    // verb that has it differs, so it is written again without.
    setVerbs(kPasteFileKeys, on, explorerlang::pasteFileTitle(explorerlang::windowsIsEnglish()),
        L"--paste-file \"%V\"");
}

void applyPinPicture(bool on)
{
    setVerbs(pinPictureKeys(), on, explorerlang::pinPictureTitle(explorerlang::windowsIsEnglish()),
        L"--pin-picture \"%1\"", true, kLegacyPinPictureKeys);
}

void applyCommands(const QList<MenuCommand>& commands)
{
    const Stub stub;
    const bool on = !commands.isEmpty();
    if (on && ::GetFileAttributesW(stub.path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // a build without it (run from the build folder)
    const std::wstring title(explorerlang::commandsTitle(explorerlang::windowsIsEnglish()));
    // All that is written, in one line: compared with what was, so that a start
    // with the same commands writes nothing and tells Explorer nothing.
    std::wstring state = stub.path + L'|' + title;
    for (const MenuCommand& c : commands)
        state += L'|' + c.id.toStdWString() + L'=' + c.title.toStdWString();
    bool changed = false;
    for (const wchar_t* key : kCommandsKeys) {
        const std::wstring was = readString(HKEY_CURRENT_USER, key, L"WinShunState");
        if (!on) {
            // Only ours, or a copy's that is gone (as setVerbs).
            const std::wstring program = was.substr(0, was.find(L'|'));
            if (!program.empty()
                && (scoop::sameCopy(QString::fromStdWString(program), QString::fromStdWString(stub.path))
                    || ::GetFileAttributesW(program.c_str()) == INVALID_FILE_ATTRIBUTES))
                changed |= ::RegDeleteTreeW(HKEY_CURRENT_USER, key) == ERROR_SUCCESS;
            continue;
        }
        if (was == state)
            continue;
        ::RegDeleteTreeW(HKEY_CURRENT_USER, key); // the commands of before, however many
        writeString(key, L"MUIVerb", title);
        writeString(key, L"Icon", stub.folder + L"\\WinShun.exe,0");
        // A cascade: its entries are the verbs under shell. Empty, and there all the same
        // (writeString leaves out what reads as it is already, as a value not there does).
        ::RegSetKeyValueW(HKEY_CURRENT_USER, key, L"SubCommands", REG_SZ, L"", sizeof(wchar_t));
        // With several chosen each is run with the command, as Explorer's own 打开 does.
        writeString(key, L"MultiSelectModel", L"Document");
        int number = 0;
        for (const MenuCommand& c : commands) {
            wchar_t name[8]; // in the user's order: Explorer lists the verbs by their keys
            ::swprintf_s(name, L"%03d", ++number);
            const std::wstring verb = std::wstring(key) + L"\\shell\\" + name;
            writeString(verb, L"MUIVerb", c.title.toStdWString());
            writeString(verb + L"\\command", nullptr,
                L"\"" + stub.path + L"\" --run-command " + c.id.toStdWString() + L" \"%1\"");
        }
        writeString(key, L"WinShunState", state);
        changed = true;
    }
    if (changed)
        ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void applyLanSend(bool on)
{
    using Microsoft::WRL::ComPtr;
    const Stub stub;
    if (on && ::GetFileAttributesW(stub.path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // a build without it (run from the build folder)
    PWSTR known = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_SendTo, KF_FLAG_DEFAULT, nullptr, &known)))
        return;
    const std::wstring folder = known;
    ::CoTaskMemFree(known);
    const bool english = explorerlang::windowsIsEnglish();
    // Either language's: Windows' may have changed since it was written.
    for (const bool named : {true, false}) {
        const std::wstring link = folder + L'\\' + std::wstring(explorerlang::lanSendTitle(named)) + L".lnk";
        const bool wanted = on && named == english;
        const bool there = ::GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES;
        ComPtr<IShellLinkW> shortcut;
        ComPtr<IPersistFile> file;
        if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shortcut)))
            || FAILED(shortcut.As(&file)))
            return;
        wchar_t target[MAX_PATH] {};
        const bool read = there && SUCCEEDED(file->Load(link.c_str(), STGM_READ))
            && shortcut->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH) == S_OK;
        const bool ours = read && scoop::sameCopy(QString::fromWCharArray(target), QString::fromStdWString(stub.path));
        if (!wanted) {
            // Only ours, or one to a program that is gone: another copy's stays.
            if (there && (ours || (read && ::GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES)))
                ::DeleteFileW(link.c_str());
            continue;
        }
        if (ours)
            continue; // as it should be already
        ComPtr<IShellLinkW> made;
        ComPtr<IPersistFile> saved;
        const std::wstring icon = stub.folder + L"\\WinShun.exe";
        if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&made)))
            && SUCCEEDED(made->SetPath(stub.path.c_str())) && SUCCEEDED(made->SetArguments(L"--lan-send"))
            && SUCCEEDED(made->SetIconLocation(icon.c_str(), 0)) && SUCCEEDED(made.As(&saved)))
            saved->Save(link.c_str(), TRUE);
    }
}

} // namespace ws::shellmenu

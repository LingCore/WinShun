#include "RecentFolders.h"

#include <QDir>

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace Qt::StringLiterals;

namespace ws::recentfolders {

namespace {

// "C:\Users\me\file.txt" -> "C:\Users\me"; "C:\file.txt" -> "C:\".
QString parentOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(u'\\');
    return slash < 0 ? QString() : path.left(slash == 2 ? 3 : slash);
}

// The folders of what Windows lists as opened lately, newest first: the
// shortcuts in the Recent folder, which the file dialogs of all programs add
// to (what they open and save), and File Explorer too (the folders opened
// in it). Empty while Windows keeps no such list. Needs COM.
QStringList windowsRecentFolders(int max, const LocalDrives& drives)
{
    PWSTR dir = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DONT_VERIFY, nullptr, &dir)))
        return {};
    const QString recent = QString::fromWCharArray(dir);
    ::CoTaskMemFree(dir);
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))
        || FAILED(link.As(&file)))
        return {};
    // Load and GetPath read the shortcut file alone (no Resolve): a target on
    // a server that is gone costs nothing here, and folderOf looks only at
    // local drives.
    std::wstring target(32768, L'\0');
    QStringList folders;
    for (const QString& shortcut : shortcutsNewestFirst(recent, 100)) {
        if (folders.size() >= max)
            break;
        const std::wstring name = shortcut.toStdWString();
        if (FAILED(file->Load(name.c_str(), STGM_READ))
            || link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, 0) != S_OK)
            continue; // S_FALSE: no file behind it (Control Panel and the like)
        const QString folder = folderOf(QString::fromWCharArray(target.c_str()), drives);
        if (!folder.isEmpty() && !folders.contains(folder, Qt::CaseInsensitive))
            folders.append(folder);
    }
    return folders;
}

QStringList interleaved(const QStringList& a, const QStringList& b, int max)
{
    QStringList out;
    for (qsizetype i = 0; out.size() < max && (i < a.size() || i < b.size()); ++i) {
        for (const QStringList* list : {&a, &b}) {
            if (i < list->size() && out.size() < max && !out.contains(list->at(i), Qt::CaseInsensitive))
                out.append(list->at(i));
        }
    }
    return out;
}

} // namespace

std::vector<QString> shortcutsNewestFirst(const QString& folder, int max)
{
    struct Shortcut {
        std::wstring name;
        FILETIME written;
    };
    std::vector<Shortcut> found;
    const std::wstring base = QDir::toNativeSeparators(folder).toStdWString();
    WIN32_FIND_DATAW data {};
    const HANDLE find = ::FindFirstFileExW((base + L"\\*.lnk").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return {};
    do {
        // The pattern also matches "*.lnkx" and the like by the 8.3 rule: the ending checked.
        const std::wstring_view name(data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || name.size() < 5 || ::_wcsicmp(name.data() + name.size() - 4, L".lnk") != 0)
            continue;
        found.push_back({std::wstring(name), data.ftLastWriteTime});
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
    std::stable_sort(found.begin(), found.end(), [](const Shortcut& a, const Shortcut& b) { return ::CompareFileTime(&a.written, &b.written) > 0; });
    std::vector<QString> paths;
    for (const Shortcut& s : found) {
        if (static_cast<int>(paths.size()) >= std::max(0, max))
            break;
        paths.push_back(QString::fromStdWString(base + L"\\" + s.name));
    }
    return paths;
}

LocalDrives::LocalDrives()
{
    const DWORD present = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(present & (1u << i)))
            continue;
        const wchar_t root[] {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
        const UINT type = ::GetDriveTypeW(root);
        if (type != DRIVE_REMOTE && type != DRIVE_NO_ROOT_DIR && type != DRIVE_UNKNOWN)
            m_mask |= 1u << i;
    }
}

bool LocalDrives::contain(const QString& path) const
{
    if (path.size() < 3 || path[1] != u':' || path[2] != u'\\')
        return false;
    const char16_t letter = path[0].toUpper().unicode();
    return letter >= u'A' && letter <= u'Z' && (m_mask & (1u << (letter - u'A')));
}

QString folderOf(const QString& path, const LocalDrives& drives)
{
    const DWORD attributes
        = drives.contain(path) ? ::GetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16())) : INVALID_FILE_ATTRIBUTES;
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return {};
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? path : parentOf(path);
}

QStringList recent(const QStringList& history, int max)
{
    const LocalDrives drives;
    // The folders of what was opened through Win顺 (or the folders themselves).
    QStringList ours;
    for (const QString& item : history) {
        if (ours.size() >= max)
            break;
        const QString folder = folderOf(QDir::toNativeSeparators(item), drives);
        if (!folder.isEmpty() && !ours.contains(folder, Qt::CaseInsensitive))
            ours.append(folder);
    }
    // For screenshots on a profile (tools/screenshots): WINSHUN_RECENT=own
    // leaves out Windows' list, which is this user's and not the profile's.
    wchar_t only[8] {};
    if (::GetEnvironmentVariableW(L"WINSHUN_RECENT", only, 8) > 0 && std::wstring_view(only) == L"own")
        return ours;
    return interleaved(windowsRecentFolders(max, drives), ours, max);
}

} // namespace ws::recentfolders

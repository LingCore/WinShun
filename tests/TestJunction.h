#pragma once

// A junction for tests, made in-process: no `cmd /c mklink`, whose message
// comes back in the console's code page (mojibake in a UTF-8 log), and no
// elevation. Every path and target belongs to the test. A volume by its
// name (\\?\Volume{…}\) is taken as such: a folder the volume is mounted in.

#include <QDir>
#include <QString>

#include <windows.h>
#include <winioctl.h>

#include <cstddef>
#include <cstring>
#include <ranges>
#include <string>
#include <vector>

inline bool makeJunction(const QString& link, const QString& target)
{
    if (!QDir().mkpath(link))
        return false;
    const std::wstring linkPath = QDir::toNativeSeparators(link).toStdWString();
    const bool volume = target.startsWith(uR"(\\?\Volume{)");
    const std::wstring targetPath
        = volume ? target.toStdWString() : QDir::toNativeSeparators(QDir(target).absolutePath()).toStdWString();
    const std::wstring substitute = L"\\??\\" + (volume ? targetPath.substr(4) : targetPath);
    struct JunctionData {
        DWORD tag;
        WORD bytes;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
        wchar_t names[1];
    };
    const std::size_t nameBytes = (substitute.size() + 1 + targetPath.size() + 1) * sizeof(wchar_t);
    std::vector<std::byte> buffer(offsetof(JunctionData, names) + nameBytes);
    auto* data = reinterpret_cast<JunctionData*>(buffer.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->bytes = WORD(buffer.size() - 8);
    data->substituteLength = WORD(substitute.size() * sizeof(wchar_t));
    data->printOffset = WORD((substitute.size() + 1) * sizeof(wchar_t));
    data->printLength = WORD(targetPath.size() * sizeof(wchar_t));
    std::memcpy(data->names, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(reinterpret_cast<std::byte*>(data->names) + data->printOffset, targetPath.c_str(),
        (targetPath.size() + 1) * sizeof(wchar_t));
    // Shared with whoever looks at the new folder meanwhile (an index under
    // test, listing it as it is made): opened for itself alone, this failed
    // now and then with a sharing violation.
    const HANDLE handle = ::CreateFileW(linkPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    DWORD returned = 0;
    const bool made = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data, DWORD(buffer.size()), nullptr, 0, &returned, nullptr);
    ::CloseHandle(handle);
    if (!made)
        QDir().rmdir(link); // the empty folder made for it
    return made;
}

// Directory links a test made, removed as links (never followed) before the
// QTemporaryDir holding them goes: its removeRecursively() walks through
// junctions, into whatever they lead to. Declare it after the dirs.
class MadeLinks {
public:
    MadeLinks() = default;
    MadeLinks(const MadeLinks&) = delete;
    MadeLinks& operator=(const MadeLinks&) = delete;
    ~MadeLinks()
    {
        for (const std::wstring& link : m_links | std::views::reverse)
            ::RemoveDirectoryW(link.c_str());
    }
    const QString& operator<<(const QString& link)
    {
        m_links.push_back(QDir::toNativeSeparators(link).toStdWString());
        return link;
    }

private:
    std::vector<std::wstring> m_links;
};

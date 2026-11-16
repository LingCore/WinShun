#pragma once

// A junction for tests, made in-process: no `cmd /c mklink`, whose message
// comes back in the console's code page (mojibake in a UTF-8 log), and no
// elevation. Every path and target belongs to the test.

#include <QDir>
#include <QString>

#include <windows.h>
#include <winioctl.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

inline bool makeJunction(const QString& link, const QString& target)
{
    if (!QDir().mkpath(link))
        return false;
    const std::wstring linkPath = QDir::toNativeSeparators(link).toStdWString();
    const std::wstring targetPath = QDir::toNativeSeparators(QDir(target).absolutePath()).toStdWString();
    const std::wstring substitute = L"\\??\\" + targetPath;
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
    const HANDLE handle = ::CreateFileW(linkPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    DWORD returned = 0;
    const bool made = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data, DWORD(buffer.size()), nullptr, 0, &returned, nullptr);
    ::CloseHandle(handle);
    if (!made)
        QDir().rmdir(link); // the empty folder made for it
    return made;
}

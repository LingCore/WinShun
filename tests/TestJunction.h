#pragma once

// A junction for tests, made in-process: no `cmd /c mklink`, whose message
// comes back in the console's code page (mojibake in a UTF-8 log), and no
// elevation. Every path and target belongs to the test. A volume by its
// name (\\?\Volume{…}\) is taken as such: a folder the volume is mounted in.

#include <QDir>
#include <QString>
#include <QtGlobal>

#include <windows.h>
#include <winioctl.h>

#include <cstddef>
#include <cstring>
#include <ranges>
#include <string>
#include <vector>

// A whole volume a test may link to: only a scratch one made for the tests
// (a .vhd), named by WINSHUN_TEST_SCRATCH_VOLUME ("V:"). Never the disk of
// the system, of %TEMP% or of this source tree, whatever the variable says.
inline QString testScratchVolume()
{
    const QString named = qEnvironmentVariable("WINSHUN_TEST_SCRATCH_VOLUME").left(2).toUpper();
    if (named.size() != 2 || named[1] != u':' || !named[0].isLetter())
        return {};
    wchar_t windows[MAX_PATH] = {};
    ::GetWindowsDirectoryW(windows, MAX_PATH);
    for (const QString& real : {QString::fromWCharArray(windows), QDir::tempPath(), QDir::homePath(), QStringLiteral(__FILE__)}) {
        if (QDir::toNativeSeparators(real).left(2).toUpper() == named)
            return {};
    }
    return named;
}

// Whether a link may lead there: only to what the tests made themselves.
// Whatever removes a folder tree by going through its links (QTemporaryDir,
// QDir::removeRecursively, a cleanup of %TEMP%, a test killed before its
// MadeLinks ran) then removes test data and nothing else. A link to a real
// disk's root, made in a test, once emptied a user's Desktop and AppData.
inline bool testOwnsLinkTarget(const QString& target)
{
    const QString scratch = testScratchVolume();
    if (target.startsWith(uR"(\\?\Volume{)")) { // a whole volume by its name: the scratch one alone
        wchar_t name[64] = {};
        return !scratch.isEmpty() && ::GetVolumeNameForVolumeMountPointW((scratch + u'\\').toStdWString().c_str(), name, 64)
            && target.compare(QString::fromWCharArray(name), Qt::CaseInsensitive) == 0;
    }
    const QString path = QDir::cleanPath(QDir(target).absolutePath());
    if (!scratch.isEmpty() && path.startsWith(scratch, Qt::CaseInsensitive))
        return true; // the scratch volume, its root too
    for (const QString& owned : {QDir::tempPath(), qEnvironmentVariable("WINSHUN_TEST_OTHER_DISK_DIR")}) {
        const QString root = owned.isEmpty() ? QString() : QDir::cleanPath(QDir(owned).absolutePath());
        if (root.size() > 3 && path.size() > root.size() + 1 && path.startsWith(root + u'/', Qt::CaseInsensitive))
            return true; // strictly inside: never the folder of all the temporary files itself
    }
    return false;
}

// A directory link taken away as a link, whatever it leads to. For a path
// about to be made anew or removed recursively: false when it is a plain folder.
inline bool removeLinkOnly(const QString& path)
{
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD attributes = ::GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? ::RemoveDirectoryW(native.c_str()) : ::DeleteFileW(native.c_str());
}

inline bool makeJunction(const QString& link, const QString& target)
{
    if (!testOwnsLinkTarget(target)) {
        qCritical("makeJunction: refused, the target is not test data: %ls", qUtf16Printable(target));
        ::SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
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
// junctions, into whatever they lead to. Declare it after the dirs, and
// register each link before it is made. This is the second guard: the first
// is that a link only leads to test data (testOwnsLinkTarget).
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

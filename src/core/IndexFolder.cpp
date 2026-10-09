#include "IndexFolder.h"

#include "Win32Util.h"

#include <QDir>
#include <QFileInfo>

#include <windows.h>
#include <cfgmgr32.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::indexfolder {

namespace {

const QString kSnapshot = u"index.bin"_s;
const QString kContent = u"content"_s;

struct File {
    QString name; // in the folder: "index.bin", "content/12.grams", "content/13.texts"
    std::uint64_t bytes = 0;
};

std::vector<File> filesIn(const QString& folder)
{
    std::vector<File> files;
    if (const QFileInfo snapshot(snapshotPath(folder)); snapshot.isFile())
        files.push_back({kSnapshot, static_cast<std::uint64_t>(snapshot.size())});
    for (const QFileInfo& f :
        QDir(contentPath(folder)).entryInfoList({u"*.grams"_s, u"*.texts"_s}, QDir::Files | QDir::Hidden))
        files.push_back({kContent + u'/' + f.fileName(), static_cast<std::uint64_t>(f.size())});
    return files;
}

std::wstring nativePath(const QString& folder, const QString& name)
{
    return win32::longPath(QDir::toNativeSeparators(QDir::cleanPath(folder + u'/' + name)).toStdWString());
}

std::error_code lastError()
{
    return {static_cast<int>(::GetLastError()), std::system_category()};
}

struct Copying {
    const Progress& progress;
    std::uint64_t before; // bytes of the files copied before this one
    std::uint64_t total;
};

DWORD CALLBACK copyProgress(
    LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID data)
{
    const auto* c = static_cast<const Copying*>(data);
    const std::uint64_t done = c->before + static_cast<std::uint64_t>(transferred.QuadPart);
    return c->progress(done, c->total) ? PROGRESS_CONTINUE : PROGRESS_CANCEL;
}

// GUID_DEVINTERFACE_DISK and DEVPKEY_Device_InstanceId, without initguid.h.
constexpr GUID kDiskInterface {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
constexpr DEVPROPKEY kInstanceId {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};

// For queries only: no access asked for.
win32::UniqueHandle openDevice(const std::wstring& path)
{
    return win32::UniqueHandle(::CreateFileW(
        path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
}

// \\.\D: for "D:/...".
win32::UniqueHandle openVolume(const QString& folder)
{
    const QString path = QDir::cleanPath(QDir::fromNativeSeparators(folder));
    if (path.size() < 2 || path[1] != u':')
        return {};
    return openDevice(L"\\\\.\\" + path.left(2).toStdWString());
}

// How a disk is attached, as its driver says.
struct DeviceInfo {
    STORAGE_BUS_TYPE bus = BusTypeUnknown;
    bool removableMedia = false;
};

std::optional<DeviceInfo> deviceInfo(HANDLE device)
{
    alignas(STORAGE_DEVICE_DESCRIPTOR) std::byte buffer[1024] {};
    STORAGE_PROPERTY_QUERY query {StorageDeviceProperty, PropertyStandardQuery, {}};
    DWORD bytes = 0;
    if (!::DeviceIoControl(
            device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, buffer, sizeof buffer, &bytes, nullptr)
        || bytes < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawPropertiesLength))
        return std::nullopt;
    const auto* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);
    return DeviceInfo {d->BusType, d->RemovableMedia != FALSE};
}

// Whether a disk has to seek. Unasked, an NVMe disk does not.
std::optional<bool> seekPenalty(HANDLE disk)
{
    STORAGE_PROPERTY_QUERY query {StorageDeviceSeekPenaltyProperty, PropertyStandardQuery, {}};
    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty {};
    DWORD bytes = 0;
    if (::DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &penalty, sizeof penalty, &bytes,
            nullptr)
        && bytes >= sizeof penalty)
        return penalty.IncursSeekPenalty != FALSE;
    if (const auto device = deviceInfo(disk); device && device->bus == BusTypeNvme)
        return false;
    return std::nullopt;
}

// Which disk a volume or disk handle is: fails for a volume across several.
std::optional<STORAGE_DEVICE_NUMBER> deviceNumber(HANDLE device)
{
    STORAGE_DEVICE_NUMBER number {};
    DWORD bytes = 0;
    if (!::DeviceIoControl(
            device, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number, sizeof number, &bytes, nullptr))
        return std::nullopt;
    return number;
}

// The device node of that disk.
std::optional<DEVINST> diskNode(const STORAGE_DEVICE_NUMBER& wanted)
{
    GUID diskInterface = kDiskInterface;
    std::wstring list; // of the disks' interface paths, each ending in a null, then one more
    for (CONFIGRET result = CR_BUFFER_SMALL; result == CR_BUFFER_SMALL;) { // disks may come meanwhile
        ULONG size = 0;
        if (::CM_Get_Device_Interface_List_SizeW(&size, &diskInterface, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT)
            != CR_SUCCESS)
            return std::nullopt;
        list.assign(size, L'\0');
        result = ::CM_Get_Device_Interface_ListW(
            &diskInterface, nullptr, list.data(), size, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result != CR_SUCCESS && result != CR_BUFFER_SMALL)
            return std::nullopt;
    }
    for (const wchar_t* path = list.c_str(); *path; path += std::wcslen(path) + 1) {
        const win32::UniqueHandle disk = openDevice(path);
        const auto number = disk.valid() ? deviceNumber(disk.get()) : std::nullopt;
        if (!number || number->DeviceType != wanted.DeviceType || number->DeviceNumber != wanted.DeviceNumber)
            continue;
        wchar_t id[MAX_DEVICE_ID_LEN] {};
        ULONG bytes = sizeof id;
        DEVPROPTYPE type = 0;
        DEVINST node = 0;
        if (::CM_Get_Device_Interface_PropertyW(path, &kInstanceId, &type, reinterpret_cast<PBYTE>(id), &bytes, 0)
                != CR_SUCCESS
            || ::CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            return std::nullopt;
        return node;
    }
    return std::nullopt;
}

// The disk's controller, or something it hangs off, can be unplugged. The
// disk itself is not asked: see onExternalDisk().
bool removableAbove(DEVINST disk)
{
    for (DEVINST node = disk; ::CM_Get_Parent(&node, node, 0) == CR_SUCCESS;) {
        ULONG capabilities = 0;
        ULONG size = sizeof capabilities;
        if (::CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_CAPABILITIES, nullptr, &capabilities, &size, 0)
                == CR_SUCCESS
            && (capabilities & CM_DEVCAP_REMOVABLE))
            return true;
    }
    return false;
}

} // namespace

QString snapshotPath(const QString& folder)
{
    return QDir(folder).filePath(kSnapshot);
}

QString contentPath(const QString& folder)
{
    return QDir(folder).filePath(kContent);
}

bool hasIndex(const QString& folder)
{
    return QFileInfo(snapshotPath(folder)).isFile();
}

std::uint64_t size(const QString& folder)
{
    std::uint64_t bytes = 0;
    for (const File& f : filesIn(folder))
        bytes += f.bytes;
    return bytes;
}

bool onlyIndexFiles(const QString& folder)
{
    const auto all = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
    const auto isSegment = [](const QFileInfo& f) {
        return f.isFile()
            && (f.suffix().compare(u"grams", Qt::CaseInsensitive) == 0
                || f.suffix().compare(u"texts", Qt::CaseInsensitive) == 0);
    };
    return std::ranges::all_of(QDir(folder).entryInfoList(all), [&](const QFileInfo& entry) {
        if (entry.isDir())
            return entry.fileName().compare(kContent, Qt::CaseInsensitive) == 0
                && std::ranges::all_of(QDir(entry.filePath()).entryInfoList(all), isSegment);
        // index.bin.XXXXXX: one being saved when Win顺 stopped (QSaveFile)
        return entry.fileName().startsWith(kSnapshot, Qt::CaseInsensitive)
            && (entry.fileName().size() == kSnapshot.size() || entry.fileName()[kSnapshot.size()] == u'.');
    });
}

bool onExternalDisk(const QString& folder)
{
    const win32::UniqueHandle volume = openVolume(folder);
    if (!volume.valid())
        return false;
    if (const auto device = deviceInfo(volume.get())) {
        if (device->removableMedia)
            return true;
        switch (device->bus) {
        case BusTypeUsb: // over USB-A, Type-C or a dock alike
        case BusTypeSd:
        case BusTypeMmc:
        case BusType1394:
            return true;
        default:
            break;
        }
    }
    // NVMe and SATA say nothing of where they are: a Thunderbolt or USB4
    // enclosure shows in the device tree, as a controller that can be unplugged.
    const auto number = deviceNumber(volume.get());
    const auto disk = number ? diskNode(*number) : std::nullopt;
    return disk && removableAbove(*disk);
}

std::optional<bool> onSpinningDisk(const QString& folder)
{
    const win32::UniqueHandle volume = openVolume(folder);
    if (!volume.valid())
        return std::nullopt;
    // Asked of each disk under the volume: asked of a volume across several,
    // the question fails.
    alignas(VOLUME_DISK_EXTENTS) std::byte buffer[sizeof(VOLUME_DISK_EXTENTS) + 31 * sizeof(DISK_EXTENT)] {};
    DWORD bytes = 0;
    if (!::DeviceIoControl(
            volume.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buffer, sizeof buffer, &bytes, nullptr))
        return std::nullopt;
    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer);
    bool unknown = extents->NumberOfDiskExtents == 0;
    for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i) {
        const win32::UniqueHandle disk
            = openDevice(L"\\\\.\\PhysicalDrive" + std::to_wstring(extents->Extents[i].DiskNumber));
        const std::optional<bool> seeks = disk.valid() ? seekPenalty(disk.get()) : std::nullopt;
        if (seeks == true)
            return true;
        unknown = unknown || !seeks;
    }
    return unknown ? std::nullopt : std::optional(false);
}

void readIntoCache(const std::vector<QString>& files, const std::function<bool()>& cancelled)
{
    using namespace std::chrono_literals;
    constexpr DWORD kChunk = 1 << 20;
    const auto buffer = std::make_unique_for_overwrite<char[]>(kChunk);
    for (const QString& path : files) {
        const std::wstring native = win32::longPath(QDir::toNativeSeparators(path).toStdWString());
        const win32::UniqueHandle file(::CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.valid())
            continue; // merged away meanwhile
        alignas(8) FILE_IO_PRIORITY_HINT_INFO hint {IoPriorityHintLow};
        ::SetFileInformationByHandle(file.get(), FileIoPriorityHintInfo, &hint, sizeof hint);
        for (;;) {
            if (cancelled())
                return;
            const auto started = std::chrono::steady_clock::now();
            DWORD got = 0;
            if (!::ReadFile(file.get(), buffer.get(), kChunk, &got, nullptr) || got == 0)
                break;
            // Out of the cache it took no time. Off the disk: rest as long
            // (a fifth of a second at most, so that cancelling is quick).
            const auto took = std::chrono::steady_clock::now() - started;
            if (took > 2ms)
                std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(took, 200ms));
        }
    }
}

std::error_code copy(const QString& from, const QString& to, const Progress& progress)
{
    const std::vector<File> files = filesIn(from);
    std::uint64_t total = 0;
    for (const File& f : files)
        total += f.bytes;

    remove(to); // what an earlier attempt left there
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(contentPath(to).toStdWString()), error);
    if (error)
        return error;

    std::uint64_t before = 0;
    for (const File& f : files) {
        Copying copying {progress, before, total};
        BOOL cancel = FALSE;
        if (!::CopyFileExW(nativePath(from, f.name).c_str(), nativePath(to, f.name).c_str(),
                progress ? &copyProgress : nullptr, &copying, &cancel, 0)) {
            error = lastError();
            remove(to);
            return error;
        }
        before += f.bytes;
    }
    // On the disk before the originals are deleted: a power cut right after
    // must not take both. The pages stay in the file cache, for the first
    // searches from the new place.
    for (const File& f : files) {
        const win32::UniqueHandle file(::CreateFileW(nativePath(to, f.name).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.valid() || !::FlushFileBuffers(file.get())) {
            error = lastError();
            remove(to);
            return error;
        }
    }
    if (progress)
        progress(total, total);
    return {};
}

void remove(const QString& folder)
{
    for (const File& f : filesIn(folder))
        ::DeleteFileW(nativePath(folder, f.name).c_str());
    ::RemoveDirectoryW(nativePath(folder, kContent).c_str()); // only once it is empty
}

} // namespace ws::indexfolder

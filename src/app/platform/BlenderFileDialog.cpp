#include "BlenderFileDialog.h"

#include "Win32Util.h"

#include <QByteArray>
#include <QDir>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cstdint>

namespace ws::blenderdialog {
namespace {

constexpr auto kProperty = L"WinShun.Blender.FileBrowser.v1";
constexpr auto kFolderProperty = L"WinShun.Blender.Folder.v1";
constexpr std::uint32_t kMaxPacket = 65536;

bool hasClass(HWND window, const wchar_t* expected)
{
    wchar_t name[64] {};
    return ::GetClassNameW(window, name, static_cast<int>(std::size(name))) && ::wcscmp(name, expected) == 0;
}

bool transfer(HANDLE pipe, void* data, DWORD count, bool read)
{
    auto* cursor = static_cast<char*>(data);
    while (count) {
        win32::UniqueHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid())
            return false;
        OVERLAPPED operation {};
        operation.hEvent = event.get();
        DWORD done = 0;
        const BOOL immediate = read ? ::ReadFile(pipe, cursor, count, &done, &operation)
                                    : ::WriteFile(pipe, cursor, count, &done, &operation);
        if (!immediate) {
            if (::GetLastError() != ERROR_IO_PENDING)
                return false;
            if (::WaitForSingleObject(event.get(), 1800) != WAIT_OBJECT_0) {
                ::CancelIoEx(pipe, &operation);
                ::GetOverlappedResult(pipe, &operation, &done, TRUE); // before the stack OVERLAPPED goes away
                return false;
            }
            if (!::GetOverlappedResult(pipe, &operation, &done, FALSE))
                return false;
        }
        if (!done || done > count)
            return false;
        cursor += done;
        count -= done;
    }
    return true;
}

QJsonObject request(HWND window, const char* method, const std::wstring& path = {})
{
    if (!isOne(window))
        return {};
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    const std::wstring name = L"\\\\.\\pipe\\WinShun.Blender.FileDialog.v1." + std::to_wstring(pid);
    win32::UniqueHandle pipe(::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (!pipe.valid() && ::GetLastError() == ERROR_PIPE_BUSY && ::WaitNamedPipeW(name.c_str(), 150))
        pipe.reset(::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    ULONG server = 0;
    if (!pipe.valid() || !::GetNamedPipeServerProcessId(pipe.get(), &server) || server != pid)
        return {}; // a pipe with this name is not enough: it must belong to that Blender
    const auto token = reinterpret_cast<quintptr>(::GetPropW(window, kProperty));
    QJsonObject message {{QStringLiteral("window"), QString::number(reinterpret_cast<quintptr>(window))},
        {QStringLiteral("token"), static_cast<qint64>(token)}, {QStringLiteral("method"), QString::fromLatin1(method)}};
    if (!path.empty())
        message.insert(QStringLiteral("path"), QString::fromStdWString(path));
    QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (bytes.size() > kMaxPacket)
        return {};
    auto length = static_cast<std::uint32_t>(bytes.size());
    if (!transfer(pipe.get(), &length, sizeof length, false)
        || !transfer(pipe.get(), bytes.data(), length, false)
        || !transfer(pipe.get(), &length, sizeof length, true) || !length || length > kMaxPacket)
        return {};
    bytes.resize(length);
    if (!transfer(pipe.get(), bytes.data(), length, true))
        return {};
    const QJsonObject response = QJsonDocument::fromJson(bytes).object();
    if (!response.value(QStringLiteral("ok")).toBool())
        qWarning().noquote() << "Blender file-dialog" << method << response.value(QStringLiteral("error")).toString();
    return response;
}

} // namespace

bool mayBe(HWND window)
{
    if (!window || !hasClass(window, L"GHOST_WindowClass") || !::GetWindow(window, GW_OWNER))
        return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.valid())
        return false;
    wchar_t image[1024] {};
    DWORD length = static_cast<DWORD>(std::size(image));
    if (!::QueryFullProcessImageNameW(process.get(), 0, image, &length))
        return false;
    const wchar_t* leaf = ::wcsrchr(image, L'\\');
    return leaf && ::_wcsicmp(leaf + 1, L"blender.exe") == 0;
}

bool isOne(HWND window)
{
    return ::GetPropW(window, kProperty) && mayBe(window);
}

std::wstring currentFolder(HWND window)
{
    if (!isOne(window))
        return {};
    const auto atom = reinterpret_cast<quintptr>(::GetPropW(window, kFolderProperty));
    if (atom < 0xC000 || atom > 0xFFFF)
        return {};
    wchar_t folder[256] {};
    const UINT length = ::GlobalGetAtomNameW(static_cast<ATOM>(atom), folder, static_cast<int>(std::size(folder)));
    if (!length || !QDir::isAbsolutePath(QString::fromWCharArray(folder, static_cast<int>(length)))
        || atom != reinterpret_cast<quintptr>(::GetPropW(window, kFolderProperty)))
        return {};
    return {folder, length};
}

bool goTo(HWND window, const std::wstring& folder)
{
    return request(window, "navigate", folder).value(QStringLiteral("ok")).toBool();
}

bool setFileName(HWND window, const std::wstring& name)
{
    QString path = QString::fromStdWString(name);
    if (!QDir::isAbsolutePath(path)) {
        if (path.contains(QLatin1Char('/')) || path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':')))
            return false;
        const QJsonObject state = request(window, "state");
        if (!state.value(QStringLiteral("ok")).toBool())
            return false;
        path = QDir(state.value(QStringLiteral("directory")).toString()).filePath(path);
    }
    return request(window, "select_file", QDir::toNativeSeparators(path).toStdWString()).value(QStringLiteral("ok")).toBool();
}

bool accept(HWND window)
{
    return request(window, "accept").value(QStringLiteral("ok")).toBool();
}

} // namespace ws::blenderdialog

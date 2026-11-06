#pragma once

#include "Win32Util.h"

#include <QByteArray>
#include <QDeadlineTimer>
#include <QStringDecoder>
#include <QtEndian>

namespace ws::tessoa {

// Public v1 interface: Tessoa/Listary.FileAppPlugin.Tessoa/TessoaQuery.cs.
// The caller identifies tessoa.exe and runs this on its file-manager worker.
inline QString currentFolder(HWND window)
{
    DWORD pid = 0;
    if (!window || !::GetWindowThreadProcessId(window, &pid)) return {};
    const QDeadlineTimer deadline(600);
    const std::wstring name = L"\\\\.\\pipe\\tessoa.q." + std::to_wstring(pid);
    const auto connect = [&] {
        return ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    };
    win32::UniqueHandle pipe(connect());
    if (!pipe.valid() && ::GetLastError() == ERROR_PIPE_BUSY && ::WaitNamedPipeW(name.c_str(), 300))
        pipe.reset(connect());
    ULONG server = 0;
    if (!pipe.valid() || !::GetNamedPipeServerProcessId(pipe.get(), &server) || server != pid) return {};
    win32::UniqueHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) return {};
    const auto transfer = [&](char* bytes, DWORD length, bool read) {
        while (length && !deadline.hasExpired()) {
            ::ResetEvent(event.get());
            OVERLAPPED operation {};
            operation.hEvent = event.get();
            DWORD done = 0;
            const BOOL immediate = read ? ::ReadFile(pipe.get(), bytes, length, &done, &operation)
                                        : ::WriteFile(pipe.get(), bytes, length, &done, &operation);
            if (!immediate) {
                if (::GetLastError() != ERROR_IO_PENDING) return false;
                if (::WaitForSingleObject(event.get(), static_cast<DWORD>(deadline.remainingTime())) != WAIT_OBJECT_0) {
                    ::CancelIoEx(pipe.get(), &operation);
                    ::GetOverlappedResult(pipe.get(), &operation, &done, TRUE);
                    return false;
                }
                if (!::GetOverlappedResult(pipe.get(), &operation, &done, FALSE)) return false;
            }
            if (!done || done > length) return false;
            bytes += done;
            length -= done;
        }
        return length == 0;
    };
    QByteArray request = QByteArrayLiteral("v 1\nop query-cwd\nhwnd ")
        + QByteArray::number(reinterpret_cast<quintptr>(window)) + '\n';
    quint32 length = qToLittleEndian(static_cast<quint32>(request.size()));
    if (!transfer(reinterpret_cast<char*>(&length), sizeof length, false)
        || !transfer(request.data(), static_cast<DWORD>(request.size()), false)
        || !transfer(reinterpret_cast<char*>(&length), sizeof length, true)) return {};
    length = qFromLittleEndian(length);
    if (!length || length > (1u << 20)) return {};
    QByteArray response(length, Qt::Uninitialized);
    if (!transfer(response.data(), length, true)) return {};
    QStringDecoder decode(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString path = decode(response);
    if (decode.hasError() || path.contains(QChar::Null) || path.contains(u'\n') || path.contains(u'\r')) return {};
    // Only absolute filesystem paths, including UNC; never a shell URL or
    // a remembered directory when the active page has no filesystem path.
    if (!(path.size() >= 3 && path[0].isLetter() && path[1] == u':' && (path[2] == u'\\' || path[2] == u'/'))
        && !path.startsWith(QStringLiteral("\\\\"))) return {};
    DWORD currentPid = 0;
    if (!::GetWindowThreadProcessId(window, &currentPid) || currentPid != pid) return {};
    return path;
}

} // namespace ws::tessoa

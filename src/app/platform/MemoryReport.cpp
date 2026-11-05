#include "MemoryReport.h"

#include <QString>

#include <algorithm>
#include <vector>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace ws::memory {

namespace {

struct Proc {
    DWORD pid = 0;
    DWORD parent = 0;
    bool documents = false;
};

std::uint64_t createdAt(HANDLE process)
{
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!::GetProcessTimes(process, &created, &exited, &kernel, &user))
        return 0;
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

// This process and the Win顺 processes beneath it, parents first.
std::vector<Proc> family()
{
    std::vector<Proc> all;
    std::vector<QString> names;
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};
    PROCESSENTRY32W entry {sizeof entry};
    for (BOOL more = ::Process32FirstW(snapshot, &entry); more; more = ::Process32NextW(snapshot, &entry)) {
        all.push_back({entry.th32ProcessID, entry.th32ParentProcessID, false});
        names.push_back(QString::fromWCharArray(entry.szExeFile));
    }
    ::CloseHandle(snapshot);
    std::vector<Proc> out {{::GetCurrentProcessId(), 0, false}};
    for (std::size_t i = 0; i < out.size(); ++i) { // breadth first; `out` grows meanwhile
        const DWORD parent = out[i].pid;
        for (std::size_t j = 0; j < all.size(); ++j) {
            if (all[j].parent != parent || all[j].pid == parent
                || std::any_of(out.begin(), out.end(), [&](const Proc& p) { return p.pid == all[j].pid; }))
                continue;
            if (!names[j].startsWith(QStringLiteral("WinShun"), Qt::CaseInsensitive))
                continue;
            // A parent id can name a process that is gone and its number given to another.
            const HANDLE child = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, all[j].pid);
            const HANDLE up = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
            const bool fresh = child && up && createdAt(child) >= createdAt(up);
            if (child)
                ::CloseHandle(child);
            if (up)
                ::CloseHandle(up);
            if (!fresh)
                continue;
            out.push_back({all[j].pid, parent, names[j].startsWith(QStringLiteral("WinShunExtract"), Qt::CaseInsensitive)});
        }
    }
    return out;
}

bool privateBytes(HANDLE process, std::uint64_t& bytes)
{
    PROCESS_MEMORY_COUNTERS_EX2 counters {};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        return false;
    bytes = counters.PrivateWorkingSetSize;
    return true;
}

} // namespace

Use measure()
{
    Use use;
    for (const Proc& p : family()) {
        const bool self = p.pid == ::GetCurrentProcessId();
        const HANDLE h = self ? ::GetCurrentProcess() : ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
        if (!h)
            continue; // not ours to look at: left out, not guessed
        std::uint64_t bytes = 0;
        if (privateBytes(h, bytes)) {
            if (self)
                use.main = bytes;
            else if (p.documents) {
                use.documents += bytes;
                ++use.documentProcesses;
            } else {
                use.plugins += bytes;
                ++use.pluginProcesses;
            }
        }
        if (!self)
            ::CloseHandle(h);
    }
    return use;
}

qint64 trimAll()
{
    std::uint64_t given = 0;
    for (const Proc& p : family()) {
        const bool self = p.pid == ::GetCurrentProcessId();
        const HANDLE h = self ? ::GetCurrentProcess()
                              : ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA, FALSE, p.pid);
        if (!h)
            continue;
        std::uint64_t before = 0, after = 0;
        const bool known = privateBytes(h, before);
        ::SetProcessWorkingSetSizeEx(h, static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
        if (known && privateBytes(h, after) && before > after)
            given += before - after;
        if (!self)
            ::CloseHandle(h);
    }
    return static_cast<qint64>(given / (1024 * 1024));
}

} // namespace ws::memory

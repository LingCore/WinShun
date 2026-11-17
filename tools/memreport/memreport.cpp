// WinShunMemReport: one run on a user's machine collects everything needed
// to tell why Win顺's memory "does not go down" there, and packs it into one
// zip on the desktop. Plain Win32 with the C runtime linked in, no Qt: it
// works beside any Win顺 version and needs nothing installed.
//
// What it answers (tools/memreport/README.md has the file list):
// - Is it Win顺 at all? Every Win顺 process, plus the whole machine (free
//   memory, standby, commit, pool tags, top processes, disk load).
// - Which Win顺 process, which counter (private working set = Task
//   Manager's "内存", commit, full working set), and how it moves over time.
// - Why it is not handed back: the app's own journal ("idle-trim" waiting
//   reasons), the foreground window and the user's idle time per sample, the
//   disks (hard disks make the content indexer slow and "busy" for long).
// - Is it held or really used: a trim test at minute 20 (what the app does
//   itself when idle) and how fast it comes back afterwards.
// - What the memory is: region maps (private/mapped/image, resident
//   private/shared, size histogram, biggest allocations, mapped files,
//   threads with their names and CPU) at the start, before and after the
//   trim, at the end, and whenever a process grew by 256 MB.
//
// Privacy: no file contents, clipboard, search history or passwords. The
// user profile folder is written as %USERPROFILE%; settings sections that
// may hold addresses or keys are reduced to their key names.

#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <tlhelp32.h>
#include <winevt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <sddl.h>
#include <winioctl.h>
#include <conio.h>
#include <winsvc.h>

#include <miniz.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <concepts>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "wevtapi.lib")

namespace {

constexpr const wchar_t* kToolVersion = L"1.0";
constexpr int kDefaultMinutes = 30; // two content-indexer passes (every 15 min), many idle-trim rechecks (30 s)
constexpr int kDefaultTrimAt = 20;
constexpr int kDefaultIntervalMs = 2000;
constexpr ULONGLONG kTopEveryMs = 30'000;
constexpr ULONGLONG kCheckpointMs = 5 * 60'000; // the zip on the desktop is rewritten: a crash or a closed window still leaves one
constexpr ULONGLONG kLogTail = 4ull << 20;
constexpr ULONGLONG kGrowthForMap = 256ull << 20;
constexpr int kMaxGrowthMaps = 4;

// ---------------------------------------------------------------- text

std::string utf8(std::wstring_view w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(std::string_view s)
{
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring lower(std::wstring s)
{
    if (!s.empty())
        ::CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool equalsI(std::wstring_view a, std::wstring_view b)
{
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE)
        == CSTR_EQUAL;
}

bool startsWithI(std::wstring_view s, std::wstring_view prefix)
{
    return s.size() >= prefix.size() && equalsI(s.substr(0, prefix.size()), prefix);
}

bool containsI(std::wstring_view hay, std::wstring_view needle)
{
    return lower(std::wstring(hay)).find(lower(std::wstring(needle))) != std::wstring::npos;
}

std::wstring fmt(const wchar_t* format, ...)
{
    wchar_t buffer[2048];
    va_list args;
    va_start(args, format);
    const int n = _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
    va_end(args);
    return n < 0 ? std::wstring(buffer) : std::wstring(buffer, static_cast<size_t>(n));
}

std::wstring replaceAllI(std::wstring s, const std::wstring& from, const std::wstring& to)
{
    if (from.empty())
        return s;
    std::wstring low = lower(s);
    const std::wstring f = lower(from);
    const std::wstring lowTo = lower(to);
    size_t pos = 0;
    while ((pos = low.find(f, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        low.replace(pos, from.size(), lowTo);
        pos += to.size();
    }
    return s;
}

// ---------------------------------------------------------------- JSON

void quote(std::string& out, std::string_view s)
{
    out += '"';
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

class Json {
public:
    Json& o() { pre(); m_s += '{'; m_stack.push_back({'}', 0}); return *this; }
    Json& a() { pre(); m_s += '['; m_stack.push_back({']', 0}); return *this; }
    Json& end() { m_s += m_stack.back().close; m_stack.pop_back(); return *this; }
    Json& k(std::string_view key) { pre(); quote(m_s, key); m_s += ':'; m_afterKey = true; return *this; }
    Json& v(std::string_view x) { pre(); quote(m_s, x); return *this; }
    Json& v(const std::string& x) { return v(std::string_view(x)); }
    Json& v(const char* x) { return v(std::string_view(x)); }
    Json& v(std::wstring_view x) { return v(utf8(x)); }
    Json& v(const std::wstring& x) { return v(std::wstring_view(x)); }
    Json& v(const wchar_t* x) { return v(std::wstring_view(x)); }
    Json& v(bool x) { pre(); m_s += x ? "true" : "false"; return *this; }
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    Json& v(T x) { pre(); m_s += std::to_string(x); return *this; }
    Json& v(double x)
    {
        pre();
        if (!std::isfinite(x)) {
            m_s += "null";
        } else {
            char b[64];
            std::snprintf(b, sizeof b, "%.10g", x);
            m_s += b;
        }
        return *this;
    }
    Json& null() { pre(); m_s += "null"; return *this; }
    template <class T>
    Json& kv(std::string_view key, const T& x) { k(key); return v(x); }
    Json& raw(std::string_view json) { pre(); m_s += json; return *this; }
    const std::string& str() const { return m_s; }

private:
    void pre()
    {
        if (m_afterKey) {
            m_afterKey = false;
            return;
        }
        if (!m_stack.empty() && m_stack.back().count++ > 0)
            m_s += ',';
    }
    struct Level {
        char close;
        int count;
    };
    std::string m_s;
    std::vector<Level> m_stack;
    bool m_afterKey = false;
};

// ---------------------------------------------------------------- state

std::map<std::string, std::string> g_files; // the zip's contents
std::vector<std::string> g_manifest; // one JSON object per collected source file
std::vector<std::pair<std::wstring, std::wstring>> g_redactions;
std::atomic<bool> g_stop {false};
std::atomic<bool> g_closing {false};
HANDLE g_done = nullptr;
ULONGLONG g_t0 = 0;
std::wstring g_zipPath;
HANDLE g_out = nullptr;
bool g_isConsole = false;
size_t g_statusLen = 0;
std::atomic<const char*> g_stopReason {"finished"};

std::string isoUtc(const SYSTEMTIME& st)
{
    char b[40];
    std::snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
        st.wSecond, st.wMilliseconds);
    return b;
}

std::string nowUtc()
{
    SYSTEMTIME st;
    ::GetSystemTime(&st);
    return isoUtc(st);
}

std::string fileTimeIso(LONGLONG value)
{
    if (value <= 0)
        return {};
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(value);
    ft.dwHighDateTime = static_cast<DWORD>(value >> 32);
    SYSTEMTIME st;
    return ::FileTimeToSystemTime(&ft, &st) ? isoUtc(st) : std::string();
}

std::string fileTimeIso(const FILETIME& ft)
{
    return fileTimeIso(static_cast<LONGLONG>((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime));
}

double sinceStart()
{
    return static_cast<double>(::GetTickCount64() - g_t0) / 1000.0;
}

std::wstring clean(std::wstring s)
{
    for (const auto& [from, to] : g_redactions)
        s = replaceAllI(std::move(s), from, to);
    return s;
}

void addProfile(std::wstring profile)
{
    while (!profile.empty() && (profile.back() == L'\\' || profile.back() == L'/'))
        profile.pop_back();
    if (profile.size() < 4)
        return;
    for (const auto& r : g_redactions)
        if (equalsI(r.first, profile))
            return;
    std::wstring forward = profile;
    std::ranges::replace(forward, L'\\', L'/');
    g_redactions.push_back({profile, L"%USERPROFILE%"});
    g_redactions.push_back({forward, L"%USERPROFILE%"});
    const size_t slash = profile.find_last_of(L'\\');
    if (slash != std::wstring::npos && profile.size() - slash > 2) {
        const std::wstring name = profile.substr(slash + 1);
        g_redactions.push_back({L"\\Users\\" + name + L"\\", L"\\Users\\<user>\\"});
        g_redactions.push_back({L"/Users/" + name + L"/", L"/Users/<user>/"});
    }
}

void toolEvent(std::string_view name, const std::function<void(Json&)>& body = {})
{
    Json j;
    j.o().kv("t", sinceStart()).kv("time", nowUtc()).kv("event", name);
    if (body)
        body(j);
    j.end();
    g_files["tool-events.jsonl"] += j.str() + "\n";
}

void failure(std::string_view where, DWORD code = ::GetLastError())
{
    toolEvent("error", [&](Json& j) { j.kv("where", where).kv("code", static_cast<unsigned long>(code)); });
}

// ---------------------------------------------------------------- console

void writeOut(std::wstring_view text)
{
    DWORD written = 0;
    if (g_isConsole) {
        ::WriteConsoleW(g_out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else if (g_out && g_out != INVALID_HANDLE_VALUE) {
        const std::string s = utf8(text);
        ::WriteFile(g_out, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
    }
}

void endStatus()
{
    if (g_statusLen) {
        writeOut(L"\r\n");
        g_statusLen = 0;
    }
}

void say(std::wstring_view line = {})
{
    endStatus();
    writeOut(line);
    writeOut(L"\r\n");
    g_files["console.txt"] += utf8(line) + "\n";
}

void status(const std::wstring& text)
{
    if (!g_isConsole)
        return;
    std::wstring s = L"\r" + text;
    if (text.size() < g_statusLen)
        s.append(g_statusLen - text.size() + 2, L' ');
    g_statusLen = text.size();
    writeOut(s);
}

std::wstring mmss(double seconds)
{
    const int s = static_cast<int>(seconds);
    return fmt(L"%02d:%02d", s / 60, s % 60);
}

// ---------------------------------------------------------------- registry, files

std::optional<std::wstring> regString(HKEY root, const wchar_t* path, const wchar_t* name)
{
    DWORD size = 0;
    const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    if (::RegGetValueW(root, path, name, flags, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return std::nullopt;
    std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
    if (::RegGetValueW(root, path, name, flags, nullptr, value.data(), &size) != ERROR_SUCCESS)
        return std::nullopt;
    value.resize(std::wcslen(value.c_str()));
    return value;
}

std::optional<std::wstring> regMulti(HKEY root, const wchar_t* path, const wchar_t* name)
{
    DWORD size = 0;
    if (::RegGetValueW(root, path, name, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return std::nullopt;
    std::wstring value(size / sizeof(wchar_t) + 2, L'\0');
    if (::RegGetValueW(root, path, name, RRF_RT_REG_MULTI_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS)
        return std::nullopt;
    std::wstring joined;
    for (const wchar_t* p = value.c_str(); *p; p += std::wcslen(p) + 1) {
        if (!joined.empty())
            joined += L"; ";
        joined += p;
    }
    return joined;
}

std::optional<DWORD> regDword(HKEY root, const wchar_t* path, const wchar_t* name)
{
    DWORD value = 0;
    DWORD size = sizeof value;
    if (::RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

std::wstring expand(const std::wstring& s)
{
    wchar_t buffer[4096];
    const DWORD n = ::ExpandEnvironmentStringsW(s.c_str(), buffer, _countof(buffer));
    return n && n <= _countof(buffer) ? std::wstring(buffer) : s;
}

bool isDir(const std::wstring& path)
{
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

struct ReadResult {
    std::string bytes;
    ULONGLONG size = 0;
    FILETIME modified {};
    bool truncated = false;
    bool ok = false;
};

// Shared with the writer: Win顺 keeps its logs open.
ReadResult readTail(const std::wstring& path, ULONGLONG limit)
{
    ReadResult r;
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return r;
    LARGE_INTEGER size {};
    ::GetFileSizeEx(file, &size);
    ::GetFileTime(file, nullptr, nullptr, &r.modified);
    r.size = static_cast<ULONGLONG>(size.QuadPart);
    ULONGLONG start = 0;
    if (r.size > limit) {
        start = r.size - limit;
        r.truncated = true;
        LARGE_INTEGER at;
        at.QuadPart = static_cast<LONGLONG>(start);
        ::SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
    }
    r.bytes.resize(static_cast<size_t>(r.size - start));
    size_t got = 0;
    while (got < r.bytes.size()) {
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(r.bytes.size() - got, 1u << 20));
        if (!::ReadFile(file, r.bytes.data() + got, want, &n, nullptr) || n == 0)
            break;
        got += n;
    }
    r.bytes.resize(got);
    if (r.truncated) { // start at a line
        const size_t newline = r.bytes.find('\n');
        if (newline != std::string::npos && newline < 64 * 1024)
            r.bytes.erase(0, newline + 1);
    }
    ::CloseHandle(file);
    r.ok = true;
    return r;
}

// Collected again at the end: the newer entry replaces the older.
void noteSource(const std::string& packed, const std::string& json)
{
    std::erase_if(g_manifest, [&](const std::string& m) { return m.find("\"packed\":\"" + packed + "\"") != std::string::npos; });
    g_manifest.push_back(json);
}

std::string sanitizeText(const std::string& bytes)
{
    return utf8(clean(wide(bytes)));
}

void collectFile(const std::wstring& path, const std::string& packed, ULONGLONG limit = kLogTail, bool text = true)
{
    if (path.empty() || ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    ReadResult r = readTail(path, limit);
    Json j;
    j.o().kv("packed", packed).kv("source", clean(path)).kv("ok", r.ok);
    if (r.ok) {
        j.kv("size", r.size).kv("modified", fileTimeIso(r.modified)).kv("truncated", r.truncated);
        g_files[packed] = text ? sanitizeText(r.bytes) : std::move(r.bytes);
    } else {
        j.kv("error", static_cast<unsigned long>(::GetLastError()));
    }
    j.end();
    noteSource(packed, j.str());
}

// ---------------------------------------------------------------- native calls

using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using RtlGetVersionFn = LONG(NTAPI*)(PRTL_OSVERSIONINFOW);
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

template <class F>
F ntdll(const char* name)
{
    return reinterpret_cast<F>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), name));
}

constexpr LONG kInfoLengthMismatch = static_cast<LONG>(0xC0000004);
constexpr LONG kBufferOverflow = static_cast<LONG>(0x80000005);
constexpr LONG kBufferTooSmall = static_cast<LONG>(0xC0000023);

bool querySystem(ULONG infoClass, std::vector<BYTE>& buffer)
{
    static const auto query = ntdll<NtQuerySystemInformationFn>("NtQuerySystemInformation");
    if (!query)
        return false;
    if (buffer.empty())
        buffer.resize(1 << 20);
    for (int i = 0; i < 10; ++i) {
        ULONG need = 0;
        const LONG st = query(infoClass, buffer.data(), static_cast<ULONG>(buffer.size()), &need);
        if (st >= 0)
            return true;
        if (st != kInfoLengthMismatch && st != kBufferTooSmall)
            return false;
        buffer.resize(std::max<size_t>(static_cast<size_t>(need) + (256 << 10), buffer.size() * 2));
    }
    return false;
}

// SYSTEM_PROCESS_INFORMATION in full (winternl.h hides most fields).
struct SpiEntry {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
};

struct PageFileEntry {
    ULONG NextEntryOffset;
    ULONG TotalSize;
    ULONG TotalInUse;
    ULONG PeakUsage;
    UNICODE_STRING PageFileName;
};

struct PoolTag {
    union {
        UCHAR Tag[4];
        ULONG TagUlong;
    };
    ULONG PagedAllocs;
    ULONG PagedFrees;
    SIZE_T PagedUsed;
    ULONG NonPagedAllocs;
    ULONG NonPagedFrees;
    SIZE_T NonPagedUsed;
};

struct Proc {
    DWORD pid = 0;
    DWORD ppid = 0;
    DWORD session = 0;
    std::wstring name;
    LONGLONG created = 0;
    ULONG threads = 0;
    ULONG handles = 0;
    ULONG hardFaults = 0;
    ULONG faults = 0;
    ULONGLONG privWs = 0;
    ULONGLONG ws = 0;
    ULONGLONG peakWs = 0;
    ULONGLONG commit = 0;
    ULONGLONG peakCommit = 0;
    ULONGLONG virt = 0;
    ULONGLONG pagedPool = 0;
    ULONGLONG nonPagedPool = 0;
    ULONGLONG cpu = 0; // 100 ns
    ULONGLONG readBytes = 0;
    ULONGLONG writeBytes = 0;
    ULONGLONG otherBytes = 0;
    ULONGLONG readOps = 0;
    ULONGLONG writeOps = 0;
};

struct Snapshot {
    std::vector<Proc> procs;
    std::unordered_map<DWORD, size_t> index;
    const Proc* find(DWORD pid) const
    {
        const auto it = index.find(pid);
        return it == index.end() ? nullptr : &procs[it->second];
    }
};

Snapshot takeSnapshot()
{
    static std::vector<BYTE> buffer;
    Snapshot s;
    if (!querySystem(5 /* SystemProcessInformation */, buffer))
        return s;
    size_t offset = 0;
    for (;;) {
        const auto* e = reinterpret_cast<const SpiEntry*>(buffer.data() + offset);
        Proc p;
        p.pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(e->UniqueProcessId));
        p.ppid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(e->InheritedFromUniqueProcessId));
        p.session = e->SessionId;
        p.name = e->ImageName.Buffer ? std::wstring(e->ImageName.Buffer, e->ImageName.Length / sizeof(wchar_t))
                                     : (p.pid == 0 ? L"Idle" : L"");
        p.created = e->CreateTime.QuadPart;
        p.threads = e->NumberOfThreads;
        p.handles = e->HandleCount;
        p.hardFaults = e->HardFaultCount;
        p.faults = e->PageFaultCount;
        p.privWs = static_cast<ULONGLONG>(e->WorkingSetPrivateSize.QuadPart);
        p.ws = e->WorkingSetSize;
        p.peakWs = e->PeakWorkingSetSize;
        p.commit = e->PagefileUsage;
        p.peakCommit = e->PeakPagefileUsage;
        p.virt = e->VirtualSize;
        p.pagedPool = e->QuotaPagedPoolUsage;
        p.nonPagedPool = e->QuotaNonPagedPoolUsage;
        p.cpu = static_cast<ULONGLONG>(e->UserTime.QuadPart + e->KernelTime.QuadPart);
        p.readBytes = static_cast<ULONGLONG>(e->ReadTransferCount.QuadPart);
        p.writeBytes = static_cast<ULONGLONG>(e->WriteTransferCount.QuadPart);
        p.otherBytes = static_cast<ULONGLONG>(e->OtherTransferCount.QuadPart);
        p.readOps = static_cast<ULONGLONG>(e->ReadOperationCount.QuadPart);
        p.writeOps = static_cast<ULONGLONG>(e->WriteOperationCount.QuadPart);
        s.index[p.pid] = s.procs.size();
        s.procs.push_back(std::move(p));
        if (!e->NextEntryOffset)
            break;
        offset += e->NextEntryOffset;
    }
    return s;
}

bool isTool(const Proc& p)
{
    return p.pid == ::GetCurrentProcessId() || startsWithI(p.name, L"WinShunMemReport");
}

// Win顺's processes: every image named WinShun*, and what they started
// (third-party plugins live in other executables).
std::set<DWORD> familyOf(const Snapshot& s)
{
    std::set<DWORD> family;
    for (const Proc& p : s.procs)
        if (p.pid && startsWithI(p.name, L"WinShun") && !isTool(p))
            family.insert(p.pid);
    for (bool grew = true; grew;) {
        grew = false;
        for (const Proc& p : s.procs) {
            if (family.contains(p.pid) || isTool(p) || !family.contains(p.ppid))
                continue;
            const Proc* parent = s.find(p.ppid);
            if (parent && parent->created <= p.created) {
                family.insert(p.pid);
                grew = true;
            }
        }
    }
    return family;
}

// Per process, read once: path, command line, rights, version.
struct Info {
    HANDLE handle = nullptr;
    LONGLONG created = 0;
    std::wstring path;
    std::wstring commandLine;
    std::wstring version;
    std::wstring integrity = L"unknown";
    std::wstring sid;
    bool elevated = false;
};

std::unordered_map<DWORD, Info> g_info;

std::wstring fileVersion(const std::wstring& path)
{
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size)
        return {};
    std::vector<BYTE> data(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    if (!::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) || !fixed)
        return {};
    return fmt(L"%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
        HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
}

std::wstring commandLineOf(HANDLE process)
{
    static const auto query = ntdll<NtQueryInformationProcessFn>("NtQueryInformationProcess");
    if (!query)
        return {};
    std::vector<BYTE> buffer(8192);
    ULONG need = 0;
    LONG st = query(process, 60 /* ProcessCommandLineInformation */, buffer.data(), static_cast<ULONG>(buffer.size()), &need);
    if ((st == kInfoLengthMismatch || st == kBufferOverflow || st == kBufferTooSmall) && need > buffer.size()) {
        buffer.resize(need);
        st = query(process, 60, buffer.data(), static_cast<ULONG>(buffer.size()), &need);
    }
    if (st < 0)
        return {};
    const auto* text = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
    return text->Buffer ? std::wstring(text->Buffer, text->Length / sizeof(wchar_t)) : std::wstring();
}

void readToken(HANDLE process, Info& info)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &token))
        return;
    alignas(TOKEN_MANDATORY_LABEL) BYTE label[128];
    DWORD size = 0;
    if (::GetTokenInformation(token, TokenIntegrityLevel, label, sizeof label, &size)) {
        const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
        if (const UCHAR count = *::GetSidSubAuthorityCount(sid)) {
            const DWORD level = *::GetSidSubAuthority(sid, count - 1);
            info.integrity = level >= SECURITY_MANDATORY_SYSTEM_RID ? L"system"
                : level >= SECURITY_MANDATORY_HIGH_RID              ? L"high"
                : level >= SECURITY_MANDATORY_MEDIUM_RID            ? L"medium"
                : level >= SECURITY_MANDATORY_LOW_RID               ? L"low"
                                                                    : L"untrusted";
        }
    }
    TOKEN_ELEVATION elevation {};
    if (::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size))
        info.elevated = elevation.TokenIsElevated != 0;
    alignas(TOKEN_USER) BYTE user[256];
    if (::GetTokenInformation(token, TokenUser, user, sizeof user, &size)) {
        LPWSTR text = nullptr;
        if (::ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user)->User.Sid, &text)) {
            info.sid = text;
            ::LocalFree(text);
        }
    }
    ::CloseHandle(token);
}

Info& infoOf(const Proc& p)
{
    if (const auto it = g_info.find(p.pid); it != g_info.end()) {
        if (it->second.created == p.created)
            return it->second;
        if (it->second.handle)
            ::CloseHandle(it->second.handle);
        g_info.erase(it);
    }
    Info info;
    info.created = p.created;
    info.handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
    if (info.handle) {
        wchar_t path[MAX_PATH * 4];
        DWORD length = _countof(path);
        if (::QueryFullProcessImageNameW(info.handle, 0, path, &length))
            info.path.assign(path, length);
        info.commandLine = commandLineOf(info.handle);
        readToken(info.handle, info);
        if (!info.path.empty())
            info.version = fileVersion(info.path);
    }
    return g_info[p.pid] = std::move(info);
}

void forgetGone(const Snapshot& s)
{
    for (auto it = g_info.begin(); it != g_info.end();) {
        const Proc* p = s.find(it->first);
        if (!p || p->created != it->second.created) {
            if (it->second.handle)
                ::CloseHandle(it->second.handle);
            it = g_info.erase(it);
        } else {
            ++it;
        }
    }
}

void writeInfo(Json& j, const Proc& p)
{
    Info& info = infoOf(p);
    j.kv("pid", p.pid).kv("ppid", p.ppid).kv("name", p.name).kv("session", p.session).kv("created", fileTimeIso(p.created));
    j.kv("path", clean(info.path)).kv("command_line", clean(info.commandLine)).kv("version", info.version);
    j.kv("integrity", info.integrity).kv("elevated", info.elevated).kv("opened", info.handle != nullptr);
}

// The main program: WinShun.exe, the one with the most memory (a second
// WinShun.exe may be a short-lived helper or a portable copy).
const Proc* mainProcess(const Snapshot& s, const std::set<DWORD>& family)
{
    const Proc* best = nullptr;
    for (const DWORD pid : family) {
        const Proc* p = s.find(pid);
        if (p && equalsI(p->name, L"WinShun.exe") && (!best || p->privWs > best->privWs))
            best = p;
    }
    return best;
}

// ---------------------------------------------------------------- elevation

bool elevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION e {};
    DWORD size = 0;
    const bool yes = ::GetTokenInformation(token, TokenElevation, &e, sizeof e, &size) && e.TokenIsElevated;
    ::CloseHandle(token);
    return yes;
}

bool enableDebugPrivilege()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES tp {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = ::LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)
        && ::AdjustTokenPrivileges(token, FALSE, &tp, sizeof tp, nullptr, nullptr) && ::GetLastError() == ERROR_SUCCESS;
    ::CloseHandle(token);
    return ok;
}

// ---------------------------------------------------------------- PDH

class Counters {
public:
    void open()
    {
        if (::PdhOpenQueryW(nullptr, 0, &m_query) != ERROR_SUCCESS) {
            m_query = nullptr;
            m_failed.push_back("open");
            return;
        }
        add("standby_reserve_bytes", L"\\Memory\\Standby Cache Reserve Bytes");
        add("standby_normal_bytes", L"\\Memory\\Standby Cache Normal Priority Bytes");
        add("standby_core_bytes", L"\\Memory\\Standby Cache Core Bytes");
        add("modified_bytes", L"\\Memory\\Modified Page List Bytes");
        add("free_zero_bytes", L"\\Memory\\Free & Zero Page List Bytes");
        add("cache_bytes", L"\\Memory\\Cache Bytes");
        add("pool_paged_bytes", L"\\Memory\\Pool Paged Bytes");
        add("pool_nonpaged_bytes", L"\\Memory\\Pool Nonpaged Bytes");
        add("page_faults_per_s", L"\\Memory\\Page Faults/sec");
        add("page_reads_per_s", L"\\Memory\\Page Reads/sec");
        add("pages_input_per_s", L"\\Memory\\Pages Input/sec");
        add("page_writes_per_s", L"\\Memory\\Page Writes/sec");
        add("pages_output_per_s", L"\\Memory\\Pages Output/sec");
        add("pagefile_usage_pct", L"\\Paging File(_Total)\\% Usage");
        add("cpu_pct", L"\\Processor Information(_Total)\\% Processor Utility");
        add("cpu_time_pct", L"\\Processor(_Total)\\% Processor Time");
        add("disk_idle_pct", L"\\PhysicalDisk(*)\\% Idle Time", true);
        add("disk_queue", L"\\PhysicalDisk(*)\\Current Disk Queue Length", true);
        add("disk_read_bytes_per_s", L"\\PhysicalDisk(*)\\Disk Read Bytes/sec", true);
        add("disk_write_bytes_per_s", L"\\PhysicalDisk(*)\\Disk Write Bytes/sec", true);
        add("disk_transfers_per_s", L"\\PhysicalDisk(*)\\Disk Transfers/sec", true);
        add("disk_sec_per_transfer", L"\\PhysicalDisk(*)\\Avg. Disk sec/Transfer", true);
        ::PdhCollectQueryData(m_query); // rates need two
    }

    void sample(Json& j)
    {
        if (!m_query)
            return;
        if (::PdhCollectQueryData(m_query) != ERROR_SUCCESS)
            return;
        j.k("pdh").o();
        for (const Counter& c : m_counters) {
            if (!c.wildcard) {
                PDH_FMT_COUNTERVALUE value {};
                if (::PdhGetFormattedCounterValue(c.handle, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &value) == ERROR_SUCCESS
                    && (value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA))
                    j.kv(c.name, value.doubleValue);
                continue;
            }
            DWORD bytes = 0;
            DWORD count = 0;
            if (::PdhGetFormattedCounterArrayW(c.handle, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) != PDH_MORE_DATA)
                continue;
            std::vector<BYTE> buffer(bytes);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            if (::PdhGetFormattedCounterArrayW(c.handle, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) != ERROR_SUCCESS)
                continue;
            j.k(c.name).o();
            for (DWORD i = 0; i < count; ++i) {
                if (std::wcscmp(items[i].szName, L"_Total") == 0)
                    continue;
                if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                    j.kv(utf8(items[i].szName), items[i].FmtValue.doubleValue);
            }
            j.end();
        }
        j.end();
    }

    const std::vector<std::string>& failed() const { return m_failed; }

private:
    void add(const char* name, const wchar_t* path, bool wildcard = false)
    {
        PDH_HCOUNTER handle = nullptr;
        const PDH_STATUS st = ::PdhAddEnglishCounterW(m_query, path, 0, &handle);
        if (st == ERROR_SUCCESS)
            m_counters.push_back({name, handle, wildcard});
        else
            m_failed.push_back(std::string(name) + " " + std::to_string(static_cast<unsigned long>(st)));
    }
    struct Counter {
        std::string name;
        PDH_HCOUNTER handle;
        bool wildcard;
    };
    PDH_HQUERY m_query = nullptr;
    std::vector<Counter> m_counters;
    std::vector<std::string> m_failed;
};

// ---------------------------------------------------------------- places

struct Places {
    std::wstring profile;
    std::wstring localAppData;
    std::wstring roamingAppData;
    std::wstring desktop;
    std::wstring programData;
    std::wstring exeDir;
    std::wstring dataDir;
    std::wstring iniPath;
    std::wstring indexDir;
    bool portable = false;
    bool fromRunningApp = false;
};

std::wstring knownFolder(REFKNOWNFOLDERID id, HANDLE token)
{
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, token, &path)) && path)
        result = path;
    ::CoTaskMemFree(path);
    return result;
}

std::wstring parentDir(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// One value from an ini that QSettings wrote (UTF-8; "\\" for a backslash).
std::wstring iniValue(const std::string& ini, std::string_view section, std::string_view key)
{
    std::string current;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t eol = ini.find('\n', pos);
        if (eol == std::string::npos)
            eol = ini.size();
        std::string line = ini.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            current = line.substr(1, line.size() - 2);
            continue;
        }
        const size_t eq = line.find('=');
        if (current != section || eq == std::string::npos || line.substr(0, eq) != key)
            continue;
        std::string value = line.substr(eq + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        std::string unescaped;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\')
                ++i;
            unescaped += value[i];
        }
        return wide(unescaped);
    }
    return {};
}

Places locate(const Snapshot& s, const std::set<DWORD>& family)
{
    Places places;
    HANDLE token = nullptr;
    if (const Proc* app = mainProcess(s, family)) {
        Info& info = infoOf(*app);
        places.exeDir = parentDir(info.path);
        if (const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, app->pid)) {
            if (::OpenProcessToken(process, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE, &token))
                places.fromRunningApp = true;
            ::CloseHandle(process);
        }
    }
    // The running app's user (an administrator elevating this tool may be
    // another account); else this tool's.
    places.profile = knownFolder(FOLDERID_Profile, token);
    places.localAppData = knownFolder(FOLDERID_LocalAppData, token);
    places.roamingAppData = knownFolder(FOLDERID_RoamingAppData, token);
    places.desktop = knownFolder(FOLDERID_Desktop, token);
    if (places.localAppData.empty() && token) {
        places.fromRunningApp = false;
        places.profile = knownFolder(FOLDERID_Profile, nullptr);
        places.localAppData = knownFolder(FOLDERID_LocalAppData, nullptr);
        places.roamingAppData = knownFolder(FOLDERID_RoamingAppData, nullptr);
        places.desktop = knownFolder(FOLDERID_Desktop, nullptr);
    }
    if (token)
        ::CloseHandle(token);
    places.programData = knownFolder(FOLDERID_ProgramData, nullptr);
    if (!places.exeDir.empty() && isDir(places.exeDir + L"\\data")) {
        places.portable = true;
        places.dataDir = places.exeDir + L"\\data";
        places.iniPath = places.dataDir + L"\\WinShun\\WinShun.ini";
    } else {
        places.dataDir = places.localAppData + L"\\WinShun";
        places.iniPath = places.roamingAppData + L"\\WinShun\\WinShun.ini";
    }
    places.indexDir = places.dataDir;
    const ReadResult ini = readTail(places.iniPath, 4 << 20);
    if (ini.ok) {
        const std::wstring folder = iniValue(ini.bytes, "Index", "Folder");
        if (!folder.empty())
            places.indexDir = expand(folder);
    }
    return places;
}

// ---------------------------------------------------------------- settings

std::string redactedSettings(const std::string& ini)
{
    // Whole sections that may hold addresses, accounts or keys: names only.
    static const char* const kSecretSections[] = {"Proxy", "Shares", "Favorites", "Network", "Mcp", "Backup"};
    static const wchar_t* const kSecretKeys[] = {L"pass", L"token", L"secret", L"apikey", L"api_key", L"cookie", L"auth",
        L"credential", L"account", L"user"};
    std::string out;
    std::string section;
    bool secret = false;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t eol = ini.find('\n', pos);
        if (eol == std::string::npos)
            eol = ini.size();
        std::string line = ini.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            secret = section.rfind("Plugin.", 0) == 0
                || std::ranges::any_of(kSecretSections, [&](const char* s) { return section == s; });
            out += line + "\n";
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            out += line + "\n";
            continue;
        }
        const std::wstring key = wide(line.substr(0, eq));
        const std::string value = line.substr(eq + 1);
        // Counts and switches stay even there (how many shares, threads):
        // they bear on memory, and say nothing about the user.
        const bool plain = value == "true" || value == "false"
            || (!value.empty() && value.size() < 12 && std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; }));
        const bool hide = !plain && (secret || std::ranges::any_of(kSecretKeys, [&](const wchar_t* k) { return containsI(key, k); }));
        if (hide)
            out += line.substr(0, eq) + "=<removed, " + std::to_string(line.size() - eq - 1) + " characters>\n";
        else
            out += sanitizeText(line) + "\n";
    }
    return out;
}

// ---------------------------------------------------------------- listings

struct TreeTotals {
    ULONGLONG files = 0;
    ULONGLONG bytes = 0;
};

TreeTotals walk(const std::wstring& dir, const std::wstring& rel, int depth, int maxDepth, bool list, Json& j,
    size_t& listBudget, size_t& walkBudget)
{
    TreeTotals totals;
    WIN32_FIND_DATAW data;
    const HANDLE find = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return totals;
    do {
        if (walkBudget == 0)
            break;
        --walkBudget;
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..")
            continue;
        const std::wstring childRel = rel.empty() ? name : rel + L"\\" + name;
        const bool directory = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        const bool link = data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
        if (directory) {
            TreeTotals sub;
            // Clipboard items: counted, not named.
            const bool private_ = equalsI(name, L"clipboard");
            if (!link)
                sub = walk(dir + L"\\" + name, childRel, depth + 1, maxDepth, list && !private_ && depth + 1 < maxDepth, j,
                    listBudget, walkBudget);
            totals.files += sub.files;
            totals.bytes += sub.bytes;
            if (list && listBudget) {
                --listBudget;
                j.o().kv("path", clean(childRel) + L"\\").kv("files", sub.files).kv("bytes", sub.bytes).kv("link", link).kv(
                    "modified", fileTimeIso(data.ftLastWriteTime)).end();
            }
        } else {
            const ULONGLONG size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            ++totals.files;
            totals.bytes += size;
            if (list && listBudget) {
                --listBudget;
                j.o().kv("path", clean(childRel)).kv("bytes", size).kv("modified", fileTimeIso(data.ftLastWriteTime)).end();
            }
        }
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
    return totals;
}

void listFolder(Json& j, const char* key, const std::wstring& root, int maxDepth)
{
    j.k(key).o().kv("root", clean(root)).kv("exists", isDir(root));
    if (isDir(root)) {
        size_t listBudget = 4000;
        size_t walkBudget = 200'000;
        j.k("entries").a();
        const TreeTotals totals = walk(root, L"", 0, maxDepth, true, j, listBudget, walkBudget);
        j.end();
        j.kv("files", totals.files).kv("bytes", totals.bytes).kv("walk_cut", walkBudget == 0);
    }
    j.end();
}

// ---------------------------------------------------------------- disks

std::wstring g_diskSummary; // for report.txt

const char* busName(int bus)
{
    static const char* const kNames[] = {"Unknown", "SCSI", "ATAPI", "ATA", "1394", "SSA", "Fibre", "USB", "RAID", "iSCSI",
        "SAS", "SATA", "SD", "MMC", "Virtual", "FileBackedVirtual", "Spaces", "NVMe", "SCM", "UFS"};
    return bus >= 0 && bus < static_cast<int>(std::size(kNames)) ? kNames[bus] : "Other";
}

std::string trimmedAscii(const char* base, DWORD offset, DWORD limit)
{
    if (!offset || offset >= limit)
        return {};
    std::string s(base + offset, strnlen(base + offset, limit - offset));
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    while (!s.empty() && s.front() == ' ')
        s.erase(0, 1);
    return s;
}

void collectDisks(Json& j)
{
    j.k("physical_disks").a();
    for (int n = 0; n < 32; ++n) {
        const std::wstring path = fmt(L"\\\\.\\PhysicalDrive%d", n);
        const HANDLE disk = ::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (disk == INVALID_HANDLE_VALUE)
            continue;
        j.o().kv("number", n);
        DWORD returned = 0;
        STORAGE_PROPERTY_QUERY query {};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        alignas(8) char buffer[4096] {};
        std::string bus, model;
        if (::DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, buffer, sizeof buffer, &returned, nullptr)) {
            const auto* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);
            bus = busName(d->BusType);
            model = trimmedAscii(buffer, d->ProductIdOffset, returned);
            j.kv("bus", bus).kv("removable", d->RemovableMedia != 0);
            j.kv("vendor", trimmedAscii(buffer, d->VendorIdOffset, returned)).kv("model", model);
            j.kv("revision", trimmedAscii(buffer, d->ProductRevisionOffset, returned));
        }
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        DEVICE_SEEK_PENALTY_DESCRIPTOR seek {};
        const char* kind = "unknown";
        if (::DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &seek, sizeof seek, &returned, nullptr)) {
            kind = seek.IncursSeekPenalty ? "HDD" : "SSD";
            j.kv("seek_penalty", seek.IncursSeekPenalty != 0);
        }
        j.kv("kind", kind);
        g_diskSummary += fmt(L"  磁盘 %d：%s，%s，%s\r\n", n, wide(kind).c_str(), wide(bus).c_str(), wide(model).c_str());
        query.PropertyId = StorageDeviceTrimProperty;
        DEVICE_TRIM_DESCRIPTOR trim {};
        if (::DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &trim, sizeof trim, &returned, nullptr))
            j.kv("trim", trim.TrimEnabled != 0);
        GET_LENGTH_INFORMATION length {};
        if (::DeviceIoControl(disk, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof length, &returned, nullptr))
            j.kv("size_gb", static_cast<double>(length.Length.QuadPart) / (1024.0 * 1024 * 1024));
        j.end();
        ::CloseHandle(disk);
    }
    j.end();

    j.k("volumes").a();
    const DWORD drives = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i)))
            continue;
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        const std::wstring root = fmt(L"%c:\\", letter);
        const UINT type = ::GetDriveTypeW(root.c_str());
        static const char* const kTypes[] = {"unknown", "no-root", "removable", "fixed", "remote", "cdrom", "ramdisk"};
        j.o().kv("root", root).kv("type", type < 7 ? kTypes[type] : "other");
        // Network and optical drives are not touched: a dead share can
        // hang for long, a disc would spin up.
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK) {
            wchar_t fs[64] {};
            DWORD serial = 0, maxName = 0, flags = 0;
            if (::GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, &maxName, &flags, fs, _countof(fs)))
                j.kv("filesystem", fs);
            ULARGE_INTEGER freeToCaller {}, total {}, free {};
            if (::GetDiskFreeSpaceExW(root.c_str(), &freeToCaller, &total, &free))
                j.kv("size_gb", static_cast<double>(total.QuadPart) / (1024.0 * 1024 * 1024))
                    .kv("free_gb", static_cast<double>(free.QuadPart) / (1024.0 * 1024 * 1024));
            DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, clusters = 0;
            if (::GetDiskFreeSpaceW(root.c_str(), &sectorsPerCluster, &bytesPerSector, &freeClusters, &clusters))
                j.kv("cluster_bytes", sectorsPerCluster * bytesPerSector);
            const std::wstring device = fmt(L"\\\\.\\%c:", letter);
            const HANDLE volume = ::CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (volume != INVALID_HANDLE_VALUE) {
                alignas(8) BYTE buffer[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)] {};
                DWORD returned = 0;
                if (::DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buffer, sizeof buffer, &returned, nullptr)) {
                    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer);
                    j.k("disks").a();
                    for (DWORD e = 0; e < extents->NumberOfDiskExtents && e < 9; ++e)
                        j.v(static_cast<unsigned long>(extents->Extents[e].DiskNumber));
                    j.end();
                }
                ::CloseHandle(volume);
            }
        }
        j.end();
    }
    j.end();
}

// ---------------------------------------------------------------- system

void writePageFiles(Json& j)
{
    static std::vector<BYTE> buffer(64 << 10);
    j.k("pagefiles").a();
    if (querySystem(18 /* SystemPageFileInformation */, buffer)) {
        SYSTEM_INFO si;
        ::GetSystemInfo(&si);
        const ULONGLONG page = si.dwPageSize;
        size_t offset = 0;
        for (int guard = 0; guard < 32; ++guard) {
            const auto* e = reinterpret_cast<const PageFileEntry*>(buffer.data() + offset);
            if (!e->TotalSize && !e->PageFileName.Length)
                break;
            j.o().kv("name", e->PageFileName.Buffer ? std::wstring(e->PageFileName.Buffer, e->PageFileName.Length / 2) : std::wstring());
            j.kv("size_mb", e->TotalSize * page >> 20).kv("in_use_mb", e->TotalInUse * page >> 20).kv("peak_mb", e->PeakUsage * page >> 20).end();
            if (!e->NextEntryOffset)
                break;
            offset += e->NextEntryOffset;
        }
    }
    j.end();
}

void writePoolTags(Json& j, size_t top)
{
    static std::vector<BYTE> buffer(1 << 20);
    j.k("pool_tags").a();
    if (querySystem(22 /* SystemPoolTagInformation */, buffer)) {
        const ULONG count = *reinterpret_cast<const ULONG*>(buffer.data());
        const auto* tags = reinterpret_cast<const PoolTag*>(buffer.data() + sizeof(ULONG_PTR));
        std::vector<const PoolTag*> sorted;
        for (ULONG i = 0; i < count; ++i)
            sorted.push_back(&tags[i]);
        std::ranges::sort(sorted, [](const PoolTag* a, const PoolTag* b) {
            return a->PagedUsed + a->NonPagedUsed > b->PagedUsed + b->NonPagedUsed;
        });
        for (size_t i = 0; i < sorted.size() && i < top; ++i) {
            std::string tag;
            for (const UCHAR c : sorted[i]->Tag)
                tag += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
            j.o().kv("tag", tag).kv("paged_kb", sorted[i]->PagedUsed >> 10).kv("nonpaged_kb", sorted[i]->NonPagedUsed >> 10);
            j.kv("paged_allocs", sorted[i]->PagedAllocs - sorted[i]->PagedFrees).kv("nonpaged_allocs", sorted[i]->NonPagedAllocs - sorted[i]->NonPagedFrees).end();
        }
    }
    j.end();
}

void writeServices(Json& j)
{
    j.k("services").a();
    const SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (manager) {
        DWORD bytes = 0, count = 0, resume = 0;
        ::EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &bytes, &count, &resume, nullptr);
        std::vector<BYTE> buffer(bytes + 4096);
        resume = 0;
        if (::EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer.data(),
                static_cast<DWORD>(buffer.size()), &bytes, &count, &resume, nullptr)) {
            const auto* services = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
            for (DWORD i = 0; i < count; ++i) {
                const std::wstring name = services[i].lpServiceName;
                const std::wstring display = services[i].lpDisplayName ? services[i].lpDisplayName : L"";
                // Memory relevant: SysMain (prefetches into standby, heavy on
                // hard disks), Windows Search, Everything, Win顺's own.
                if (!(equalsI(name, L"SysMain") || equalsI(name, L"WSearch") || containsI(name, L"WinShun")
                        || containsI(display, L"WinShun") || containsI(name, L"Everything")))
                    continue;
                j.o().kv("name", name).kv("display", display).kv("state", static_cast<unsigned long>(services[i].ServiceStatusProcess.dwCurrentState));
                j.kv("pid", static_cast<unsigned long>(services[i].ServiceStatusProcess.dwProcessId));
                if (const SC_HANDLE service = ::OpenServiceW(manager, name.c_str(), SERVICE_QUERY_CONFIG)) {
                    DWORD need = 0;
                    ::QueryServiceConfigW(service, nullptr, 0, &need);
                    std::vector<BYTE> config(need + 64);
                    if (::QueryServiceConfigW(service, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data()), static_cast<DWORD>(config.size()), &need))
                        j.kv("start_type", static_cast<unsigned long>(reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data())->dwStartType));
                    ::CloseServiceHandle(service);
                }
                j.end();
            }
        }
        ::CloseServiceHandle(manager);
    }
    j.end();
}

std::string systemJson(const Snapshot& s, bool admin, bool debug, const std::vector<std::string>& pdhFailed)
{
    Json j;
    j.o().kv("tool_version", kToolVersion).kv("generated_at", nowUtc()).kv("administrator", admin).kv("debug_privilege", debug);
    TIME_ZONE_INFORMATION tz {};
    ::GetTimeZoneInformation(&tz);
    j.kv("utc_offset_minutes", -static_cast<long>(tz.Bias));

    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    j.k("os").o();
    j.kv("product", regString(HKEY_LOCAL_MACHINE, cv, L"ProductName").value_or(L""));
    j.kv("display_version", regString(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion").value_or(L""));
    j.kv("edition", regString(HKEY_LOCAL_MACHINE, cv, L"EditionID").value_or(L""));
    j.kv("build", regString(HKEY_LOCAL_MACHINE, cv, L"CurrentBuild").value_or(L""));
    j.kv("ubr", static_cast<unsigned long>(regDword(HKEY_LOCAL_MACHINE, cv, L"UBR").value_or(0)));
    if (const auto get = ntdll<RtlGetVersionFn>("RtlGetVersion")) {
        RTL_OSVERSIONINFOW v {sizeof v};
        if (get(&v) >= 0)
            j.kv("version", fmt(L"%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber));
    }
    j.end();

    j.k("cpu").o();
    j.kv("name", regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString").value_or(L""));
    j.kv("logical_processors", static_cast<unsigned long>(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    j.end();

    MEMORYSTATUSEX ms {sizeof ms};
    ::GlobalMemoryStatusEx(&ms);
    PERFORMANCE_INFORMATION pi {sizeof pi};
    ::GetPerformanceInfo(&pi, sizeof pi);
    ULONGLONG installedKb = 0;
    ::GetPhysicallyInstalledSystemMemory(&installedKb);
    const ULONGLONG page = pi.PageSize;
    j.k("memory").o();
    j.kv("installed_mb", installedKb >> 10).kv("usable_mb", ms.ullTotalPhys >> 20).kv("available_mb", ms.ullAvailPhys >> 20);
    j.kv("load_pct", static_cast<unsigned long>(ms.dwMemoryLoad)).kv("commit_mb", pi.CommitTotal * page >> 20);
    j.kv("commit_limit_mb", pi.CommitLimit * page >> 20).kv("commit_peak_mb", pi.CommitPeak * page >> 20);
    j.kv("system_cache_mb", pi.SystemCache * page >> 20).kv("kernel_paged_mb", pi.KernelPaged * page >> 20);
    j.kv("kernel_nonpaged_mb", pi.KernelNonpaged * page >> 20);
    for (const Proc& p : s.procs)
        if (p.name == L"Memory Compression" || p.name == L"MemCompression")
            j.kv("memory_compression_ws_mb", p.ws >> 20);
    j.end();

    const wchar_t* mm = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management";
    j.k("memory_management").o();
    j.kv("PagingFiles", regMulti(HKEY_LOCAL_MACHINE, mm, L"PagingFiles").value_or(L"<none>"));
    j.kv("ExistingPageFiles", regMulti(HKEY_LOCAL_MACHINE, mm, L"ExistingPageFiles").value_or(L"<none>"));
    for (const wchar_t* name : {L"ClearPageFileAtShutdown", L"DisablePagingExecutive", L"LargeSystemCache", L"PagingFilesLimit"})
        if (const auto v = regDword(HKEY_LOCAL_MACHINE, mm, name))
            j.kv(utf8(name), static_cast<unsigned long>(*v));
    for (const wchar_t* name : {L"EnablePrefetcher", L"EnableSuperfetch"})
        if (const auto v = regDword(HKEY_LOCAL_MACHINE, (std::wstring(mm) + L"\\PrefetchParameters").c_str(), name))
            j.kv(utf8(name), static_cast<unsigned long>(*v));
    j.end();
    writePageFiles(j);

    const ULONGLONG uptime = ::GetTickCount64();
    j.kv("uptime_hours", static_cast<double>(uptime) / 3'600'000.0);
    SYSTEM_POWER_STATUS power {};
    if (::GetSystemPowerStatus(&power))
        j.k("power").o().kv("ac", static_cast<int>(power.ACLineStatus)).kv("battery_pct", static_cast<int>(power.BatteryLifePercent)).kv("saver", static_cast<int>(power.SystemStatusFlag)).end();
    writeServices(j);
    collectDisks(j);
    writePoolTags(j, 25);

    // The process table's layout is undocumented in part: checked against
    // what the documented call says for this process.
    PROCESS_MEMORY_COUNTERS_EX self {};
    self.cb = sizeof self;
    const Snapshot fresh = takeSnapshot();
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&self), sizeof self);
    if (const Proc* me = fresh.find(::GetCurrentProcessId()))
        j.k("layout_check").o().kv("table_commit_kb", me->commit >> 10).kv("api_commit_kb", self.PagefileUsage >> 10).kv(
            "table_ws_kb", me->ws >> 10).kv("api_ws_kb", self.WorkingSetSize >> 10).end();
    j.k("pdh_failed").a();
    for (const std::string& f : pdhFailed)
        j.v(f);
    j.end();
    j.end();
    return j.str();
}

// ---------------------------------------------------------------- event logs

std::wstring renderXml(EVT_HANDLE event)
{
    DWORD used = 0, props = 0;
    ::EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &used, &props);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return {};
    std::wstring xml(used / sizeof(wchar_t) + 1, L'\0');
    if (!::EvtRender(nullptr, event, EvtRenderEventXml, static_cast<DWORD>(xml.size() * sizeof(wchar_t)), xml.data(), &used, &props))
        return {};
    xml.resize(std::wcslen(xml.c_str()));
    return xml;
}

std::wstring eventMessage(EVT_HANDLE publisher, EVT_HANDLE event)
{
    if (!publisher)
        return {};
    DWORD used = 0;
    ::EvtFormatMessage(publisher, event, 0, 0, nullptr, EvtFormatMessageEvent, 0, nullptr, &used);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || !used)
        return {};
    std::wstring text(used + 1, L'\0');
    if (!::EvtFormatMessage(publisher, event, 0, 0, nullptr, EvtFormatMessageEvent, static_cast<DWORD>(text.size()), text.data(), &used))
        return {};
    text.resize(std::wcslen(text.c_str()));
    return text;
}

int dumpEvents(const wchar_t* channel, const std::wstring& xpath, int maxKeep, int maxScan, const wchar_t* mustContain,
    std::string& out)
{
    const EVT_HANDLE query = ::EvtQuery(nullptr, channel, xpath.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!query) {
        failure("EvtQuery");
        return 0;
    }
    std::map<std::wstring, EVT_HANDLE> publishers;
    int kept = 0;
    int scanned = 0;
    EVT_HANDLE events[16];
    DWORD returned = 0;
    while (kept < maxKeep && scanned < maxScan && ::EvtNext(query, 16, events, 2000, 0, &returned)) {
        for (DWORD i = 0; i < returned; ++i) {
            ++scanned;
            if (kept < maxKeep) {
                const std::wstring xml = renderXml(events[i]);
                if (!mustContain || containsI(xml, mustContain)) {
                    std::wstring provider;
                    if (const size_t at = xml.find(L"Provider Name='"); at != std::wstring::npos) {
                        const size_t from = at + 15;
                        provider = xml.substr(from, xml.find(L'\'', from) - from);
                    }
                    auto [it, added] = publishers.try_emplace(provider, nullptr);
                    if (added && !provider.empty())
                        it->second = ::EvtOpenPublisherMetadata(nullptr, provider.c_str(), nullptr, 0, 0);
                    Json j;
                    j.o().kv("channel", channel).kv("xml", clean(xml)).kv("message", clean(eventMessage(it->second, events[i]))).end();
                    out += j.str() + "\n";
                    ++kept;
                }
            }
            ::EvtClose(events[i]);
        }
    }
    for (auto& [name, handle] : publishers)
        if (handle)
            ::EvtClose(handle);
    ::EvtClose(query);
    return kept;
}

void collectEventLogs()
{
    std::string out;
    // Sleep, wake, boot, shutdown, crashes of the system (14 days): the
    // app holds its memory after a wake until it is used (IdleTrim).
    dumpEvents(L"System",
        L"*[System[(Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] or EventID=41 or EventID=42 or EventID=107 "
        L"or EventID=506 or EventID=507 or EventID=6005 or EventID=6006 or EventID=6008 or EventID=1074) "
        L"and TimeCreated[timediff(@SystemTime) <= 1209600000]]]",
        400, 4000, nullptr, out);
    // "Windows successfully diagnosed a low virtual memory condition": names the biggest processes.
    dumpEvents(L"System",
        L"*[System[Provider[@Name='Microsoft-Windows-Resource-Exhaustion-Detector'] and TimeCreated[timediff(@SystemTime) <= 2592000000]]]",
        60, 600, nullptr, out);
    // Crashes and hangs of Win顺 (30 days).
    dumpEvents(L"Application",
        L"*[System[(EventID=1000 or EventID=1001 or EventID=1002) and TimeCreated[timediff(@SystemTime) <= 2592000000]]]",
        100, 5000, L"WinShun", out);
    g_files["system/eventlog.jsonl"] = std::move(out);
}

// ---------------------------------------------------------------- region maps

std::map<std::wstring, std::wstring> g_devices; // \Device\HarddiskVolume3 -> C:

void loadDevices()
{
    for (wchar_t d = L'A'; d <= L'Z'; ++d) {
        const wchar_t name[3] = {d, L':', 0};
        wchar_t target[1024];
        if (::QueryDosDeviceW(name, target, _countof(target)))
            g_devices[target] = name;
    }
}

std::wstring dosPath(const std::wstring& device)
{
    for (const auto& [dev, letter] : g_devices)
        if (startsWithI(device, dev) && (device.size() == dev.size() || device[dev.size()] == L'\\'))
            return letter + device.substr(dev.size());
    return device;
}

struct ModuleRange {
    ULONG_PTR base;
    ULONG_PTR size;
    std::wstring name;
};

std::vector<ModuleRange> modulesOf(HANDLE process)
{
    std::vector<ModuleRange> list;
    std::vector<HMODULE> modules(1024);
    DWORD need = 0;
    if (!::EnumProcessModulesEx(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &need, LIST_MODULES_ALL))
        return list;
    modules.resize(std::min<size_t>(need / sizeof(HMODULE), modules.size()));
    for (const HMODULE m : modules) {
        MODULEINFO mi {};
        wchar_t name[MAX_PATH];
        if (::GetModuleInformation(process, m, &mi, sizeof mi) && ::GetModuleBaseNameW(process, m, name, MAX_PATH))
            list.push_back({reinterpret_cast<ULONG_PTR>(mi.lpBaseOfDll), mi.SizeOfImage, name});
    }
    return list;
}

std::wstring symbolOf(const std::vector<ModuleRange>& modules, ULONG_PTR address)
{
    for (const ModuleRange& m : modules)
        if (address >= m.base && address < m.base + m.size)
            return fmt(L"%s+0x%llx", m.name.c_str(), static_cast<unsigned long long>(address - m.base));
    return address ? fmt(L"0x%llx", static_cast<unsigned long long>(address)) : L"";
}

bool workingSetPages(HANDLE process, std::vector<ULONG_PTR>& privatePages, std::vector<ULONG_PTR>& sharedPages)
{
    size_t entries = 1 << 16;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::vector<ULONG_PTR> buffer(entries + 1);
        auto* info = reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
        if (::QueryWorkingSet(process, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(ULONG_PTR)))) {
            const size_t count = std::min<size_t>(info->NumberOfEntries, entries);
            for (size_t i = 0; i < count; ++i) {
                const PSAPI_WORKING_SET_BLOCK& block = info->WorkingSetInfo[i];
                (block.Shared ? sharedPages : privatePages).push_back(block.VirtualPage);
            }
            std::ranges::sort(privatePages);
            std::ranges::sort(sharedPages);
            return true;
        }
        if (::GetLastError() != ERROR_BAD_LENGTH)
            return false;
        entries = static_cast<size_t>(info->NumberOfEntries) + entries / 2 + 4096;
    }
    return false;
}

size_t pagesIn(const std::vector<ULONG_PTR>& pages, ULONG_PTR first, ULONG_PTR end)
{
    return static_cast<size_t>(std::lower_bound(pages.begin(), pages.end(), end) - std::lower_bound(pages.begin(), pages.end(), first));
}

struct Totals {
    ULONGLONG reserved = 0;
    ULONGLONG committed = 0;
    ULONGLONG residentPrivate = 0;
    ULONGLONG residentShared = 0;
    size_t count = 0;
    void write(Json& j, std::string_view key) const
    {
        j.k(key).o().kv("reserved_kb", reserved >> 10).kv("committed_kb", committed >> 10).kv("resident_private_kb", residentPrivate >> 10).kv(
            "resident_shared_kb", residentShared >> 10).kv("count", count).end();
    }
};

struct Allocation {
    ULONG_PTR base = 0;
    DWORD type = 0;
    DWORD allocationProtect = 0;
    std::set<DWORD> protects;
    Totals t;
    int regions = 0;
};

std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> g_mapped; // pid -> private ws, commit at the last map
int g_growthMaps = 0;

std::wstring protectName(DWORD p)
{
    std::wstring s;
    switch (p & 0xff) {
    case PAGE_NOACCESS: s = L"NA"; break;
    case PAGE_READONLY: s = L"R"; break;
    case PAGE_READWRITE: s = L"RW"; break;
    case PAGE_WRITECOPY: s = L"WC"; break;
    case PAGE_EXECUTE: s = L"X"; break;
    case PAGE_EXECUTE_READ: s = L"RX"; break;
    case PAGE_EXECUTE_READWRITE: s = L"RWX"; break;
    case PAGE_EXECUTE_WRITECOPY: s = L"WCX"; break;
    default: s = fmt(L"0x%x", p & 0xff);
    }
    if (p & PAGE_GUARD) s += L"+guard";
    if (p & PAGE_NOCACHE) s += L"+nocache";
    if (p & PAGE_WRITECOMBINE) s += L"+writecombine";
    return s;
}

void memoryMap(const Proc& p, const std::string& label)
{
    const ULONGLONG started = ::GetTickCount64();
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, p.pid);
    if (!process) {
        const DWORD error = ::GetLastError();
        toolEvent("map-failed", [&](Json& j) { j.kv("pid", p.pid).kv("name", p.name).kv("label", label).kv("error", static_cast<unsigned long>(error)); });
        return;
    }
    SYSTEM_INFO si;
    ::GetSystemInfo(&si);
    const ULONG_PTR page = si.dwPageSize;
    std::vector<ULONG_PTR> privatePages, sharedPages;
    const bool haveWs = workingSetPages(process, privatePages, sharedPages);

    std::map<ULONG_PTR, Allocation> allocations;
    std::map<std::string, Totals> special;
    Totals byType[3]; // private, mapped, image
    MEMORY_BASIC_INFORMATION mbi {};
    ULONG_PTR address = 0;
    size_t regions = 0;
    while (::VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &mbi, sizeof mbi) == sizeof mbi) {
        const auto base = reinterpret_cast<ULONG_PTR>(mbi.BaseAddress);
        const ULONG_PTR size = mbi.RegionSize;
        ++regions;
        if (mbi.State != MEM_FREE) {
            Allocation& a = allocations[reinterpret_cast<ULONG_PTR>(mbi.AllocationBase)];
            a.base = reinterpret_cast<ULONG_PTR>(mbi.AllocationBase);
            a.type = mbi.Type;
            a.allocationProtect = mbi.AllocationProtect;
            ++a.regions;
            a.t.reserved += size;
            const int typeIndex = mbi.Type == MEM_IMAGE ? 2 : mbi.Type == MEM_MAPPED ? 1 : 0;
            byType[typeIndex].reserved += size;
            if (mbi.State == MEM_COMMIT) {
                const ULONGLONG rp = pagesIn(privatePages, base / page, (base + size) / page) * page;
                const ULONGLONG rs = pagesIn(sharedPages, base / page, (base + size) / page) * page;
                a.t.committed += size;
                a.t.residentPrivate += rp;
                a.t.residentShared += rs;
                a.protects.insert(mbi.Protect);
                byType[typeIndex].committed += size;
                byType[typeIndex].residentPrivate += rp;
                byType[typeIndex].residentShared += rs;
                const auto addSpecial = [&](const char* key) {
                    Totals& t = special[key];
                    t.committed += size;
                    t.residentPrivate += rp;
                    t.residentShared += rs;
                    ++t.count;
                };
                if (mbi.Protect & PAGE_WRITECOMBINE)
                    addSpecial("write_combine");
                if (mbi.Protect & PAGE_NOCACHE)
                    addSpecial("no_cache");
                if (mbi.Protect & PAGE_GUARD)
                    addSpecial("guard");
                if (mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                    addSpecial("private_executable");
            }
        }
        const ULONG_PTR next = base + size;
        if (next <= address)
            break;
        address = next;
    }
    for (auto& [base, a] : allocations)
        ++byType[a.type == MEM_IMAGE ? 2 : a.type == MEM_MAPPED ? 1 : 0].count;

    Json j;
    j.o().kv("label", label).kv("time", nowUtc()).kv("t", sinceStart()).kv("pid", p.pid).kv("name", p.name);
    j.kv("priv_ws_kb", p.privWs >> 10).kv("ws_kb", p.ws >> 10).kv("commit_kb", p.commit >> 10).kv("virt_mb", p.virt >> 20);
    j.kv("working_set_read", haveWs).kv("ws_private_pages", privatePages.size()).kv("ws_shared_pages", sharedPages.size()).kv("regions", regions);
    j.k("by_type").o();
    byType[0].write(j, "private");
    byType[1].write(j, "mapped");
    byType[2].write(j, "image");
    j.end();
    j.k("special").o();
    for (const auto& [key, t] : special)
        t.write(j, key);
    j.end();

    // Private allocations by size: 1.06 MB blocks are the file index's
    // entries, a few big ones heaps or buffers, many small ones stacks etc.
    struct Bucket {
        const char* name;
        ULONGLONG below;
        Totals t;
    };
    Bucket buckets[] = {{"<64K", 64ull << 10, {}}, {"64K-1M", 1ull << 20, {}}, {"1M-2M", 2ull << 20, {}}, {"2M-8M", 8ull << 20, {}},
        {"8M-32M", 32ull << 20, {}}, {"32M-128M", 128ull << 20, {}}, {"128M-1G", 1ull << 30, {}}, {">=1G", ~0ull, {}}};
    std::vector<const Allocation*> privates, mapped, images;
    for (const auto& [base, a] : allocations) {
        if (a.type == MEM_PRIVATE) {
            privates.push_back(&a);
            for (Bucket& b : buckets) {
                if (a.t.reserved < b.below) {
                    b.t.reserved += a.t.reserved;
                    b.t.committed += a.t.committed;
                    b.t.residentPrivate += a.t.residentPrivate;
                    b.t.residentShared += a.t.residentShared;
                    ++b.t.count;
                    break;
                }
            }
        } else if (a.type == MEM_MAPPED) {
            mapped.push_back(&a);
        } else {
            images.push_back(&a);
        }
    }
    j.k("private_by_size").o();
    for (const Bucket& b : buckets)
        b.t.write(j, b.name);
    j.end();

    std::ranges::sort(privates, [](const Allocation* x, const Allocation* y) { return x->t.committed > y->t.committed; });
    j.k("biggest_private").a();
    for (size_t i = 0; i < privates.size() && i < 80; ++i) {
        const Allocation& a = *privates[i];
        std::wstring protects;
        for (const DWORD pr : a.protects)
            protects += (protects.empty() ? L"" : L",") + protectName(pr);
        j.o().kv("base", fmt(L"0x%llx", static_cast<unsigned long long>(a.base))).kv("reserved_kb", a.t.reserved >> 10).kv(
            "committed_kb", a.t.committed >> 10).kv("resident_private_kb", a.t.residentPrivate >> 10).kv("regions", a.regions).kv("protect", protects).end();
    }
    j.end();

    const auto named = [&](const std::vector<const Allocation*>& list, const char* key, size_t top) {
        std::map<std::wstring, Totals> byFile;
        for (const Allocation* a : list) {
            wchar_t name[2048] {};
            std::wstring file = ::GetMappedFileNameW(process, reinterpret_cast<LPVOID>(a->base), name, _countof(name))
                ? clean(dosPath(name))
                : (a->type == MEM_MAPPED ? L"<pagefile-backed section>" : L"<unknown>");
            Totals& t = byFile[file];
            t.reserved += a->t.reserved;
            t.committed += a->t.committed;
            t.residentPrivate += a->t.residentPrivate;
            t.residentShared += a->t.residentShared;
            ++t.count;
        }
        std::vector<std::pair<std::wstring, Totals>> sorted(byFile.begin(), byFile.end());
        std::ranges::sort(sorted, [](const auto& x, const auto& y) {
            return x.second.committed + x.second.residentPrivate > y.second.committed + y.second.residentPrivate;
        });
        j.k(key).a();
        for (size_t i = 0; i < sorted.size() && i < top; ++i) {
            j.o().kv("file", sorted[i].first).kv("views", sorted[i].second.count).kv("reserved_kb", sorted[i].second.reserved >> 10).kv(
                "committed_kb", sorted[i].second.committed >> 10).kv("resident_private_kb", sorted[i].second.residentPrivate >> 10).kv(
                "resident_shared_kb", sorted[i].second.residentShared >> 10).end();
        }
        j.end();
    };
    named(mapped, "mapped_files", 80);
    named(images, "images", 80);

    // Threads: what started each (module+offset), its name, its CPU.
    const std::vector<ModuleRange> modules = modulesOf(process);
    static const auto queryThread = ntdll<NtQueryInformationThreadFn>("NtQueryInformationThread");
    static const auto describe = reinterpret_cast<GetThreadDescriptionFn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    struct ThreadRow {
        DWORD tid;
        std::wstring start;
        std::wstring description;
        ULONGLONG cpuMs;
        int priority;
    };
    std::vector<ThreadRow> threads;
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te {sizeof te};
        for (BOOL more = ::Thread32First(snap, &te); more; more = ::Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != p.pid)
                continue;
            ThreadRow row {te.th32ThreadID, {}, {}, 0, 0};
            HANDLE thread = ::OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!thread)
                thread = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (thread) {
                ULONG_PTR start = 0;
                if (queryThread && queryThread(thread, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, nullptr) >= 0)
                    row.start = symbolOf(modules, start);
                PWSTR description = nullptr;
                if (describe && SUCCEEDED(describe(thread, &description)) && description) {
                    row.description = description;
                    ::LocalFree(description);
                }
                FILETIME created, exited, kernel, user;
                if (::GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                    const auto ticks = [](const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
                    row.cpuMs = (ticks(kernel) + ticks(user)) / 10'000;
                }
                row.priority = ::GetThreadPriority(thread);
                ::CloseHandle(thread);
            }
            threads.push_back(std::move(row));
        }
        ::CloseHandle(snap);
    }
    std::ranges::sort(threads, [](const ThreadRow& x, const ThreadRow& y) { return x.cpuMs > y.cpuMs; });
    j.k("threads").a();
    for (const ThreadRow& t : threads)
        j.o().kv("tid", t.tid).kv("start", t.start).kv("name", t.description).kv("cpu_ms", t.cpuMs).kv("priority", t.priority).end();
    j.end();
    j.kv("took_ms", ::GetTickCount64() - started);
    j.end();
    ::CloseHandle(process);

    std::wstring safeName = p.name;
    std::ranges::replace_if(safeName, [](wchar_t c) { return c > 0x7e || c == L' ' || c == L'\\' || c == L'/'; }, L'_');
    g_files["memmaps/" + label + "/" + utf8(safeName) + "-" + std::to_string(p.pid) + ".json"] = j.str();
    g_mapped[p.pid] = {p.privWs, p.commit};
}

void mapAll(const Snapshot& s, const std::set<DWORD>& family, const std::string& label)
{
    for (const DWORD pid : family)
        if (const Proc* p = s.find(pid))
            memoryMap(*p, label);
    toolEvent("maps", [&](Json& j) { j.kv("label", label).kv("processes", family.size()); });
}

// ---------------------------------------------------------------- Win顺 files

void collectAppFiles(const Places& places)
{
    const std::wstring& d = places.dataDir;
    collectFile(d + L"\\diagnostics\\events.jsonl", "app/diagnostics/events.jsonl");
    collectFile(d + L"\\diagnostics\\events.1.jsonl", "app/diagnostics/events.1.jsonl");
    collectFile(d + L"\\diagnostics\\events.2.jsonl", "app/diagnostics/events.2.jsonl");
    collectFile(d + L"\\WinShun.log", "app/WinShun.log");
    collectFile(d + L"\\WinShun.old.log", "app/WinShun.old.log");
    collectFile(d + L"\\plugins.log", "app/plugins.log", 2ull << 20);
    collectFile(d + L"\\service.log", "app/service.log");
    collectFile(d + L"\\service.old.log", "app/service.old.log");
    collectFile(d + L"\\plugins\\winshun.capture.data\\diagnostics\\events.jsonl", "capture/events.jsonl");
    collectFile(d + L"\\plugins\\winshun.capture.data\\diagnostics\\events.1.jsonl", "capture/events.1.jsonl");
    collectFile(places.indexDir + L"\\scope.json", "app/scope.json");
    const std::wstring service = places.programData + L"\\WinShun";
    collectFile(service + L"\\service.log", "service/service.log");
    collectFile(service + L"\\service.old.log", "service/service.old.log");
    // The app's own export, if the user made one (it has the file index's
    // storage figures only the app knows).
    collectFile(d + L"\\diagnostics\\WinShun-diagnostics.zip", "app/WinShun-diagnostics.zip", 64ull << 20, false);
    const ReadResult ini = readTail(places.iniPath, 4 << 20);
    if (ini.ok) {
        g_files["app/WinShun.ini.redacted"] = redactedSettings(ini.bytes);
        noteSource("app/WinShun.ini.redacted", Json().o().kv("packed", "app/WinShun.ini.redacted").kv("source", clean(places.iniPath)).kv(
            "size", ini.size).kv("modified", fileTimeIso(ini.modified)).end().str());
    }
}

std::string appJson(const Places& places, const Snapshot& s, const std::set<DWORD>& family)
{
    Json j;
    j.o().kv("generated_at", nowUtc()).kv("running", !family.empty()).kv("portable", places.portable).kv("from_running_app", places.fromRunningApp);
    j.k("places").o().kv("exe_dir", clean(places.exeDir)).kv("data_dir", clean(places.dataDir)).kv("ini", clean(places.iniPath)).kv(
        "index_dir", clean(places.indexDir)).kv("program_data", clean(places.programData)).end();
    j.k("processes").a();
    for (const DWORD pid : family)
        if (const Proc* p = s.find(pid)) {
            j.o();
            writeInfo(j, *p);
            j.end();
        }
    j.end();
    listFolder(j, "data_dir_files", places.dataDir, 3);
    if (!equalsI(places.indexDir, places.dataDir))
        listFolder(j, "index_dir_files", places.indexDir, 2);
    listFolder(j, "service_dir_files", places.programData + L"\\WinShun", 2);
    if (!places.exeDir.empty())
        listFolder(j, "program_files", places.exeDir, 1);
    j.end();
    return j.str();
}

// ---------------------------------------------------------------- sampling

struct Series {
    std::wstring name;
    std::vector<float> t;
    std::vector<ULONGLONG> privKb;
    std::vector<ULONGLONG> commitKb;
    std::vector<bool> idleAndAway; // user idle > 60 s and no Win顺 window in front
    ULONGLONG firstRead = 0, lastRead = 0, firstWrite = 0, lastWrite = 0, firstCpu = 0, lastCpu = 0;
    ULONG firstHard = 0, lastHard = 0;
};

std::map<DWORD, Series> g_series;
std::vector<ULONGLONG> g_availKb;
std::vector<ULONGLONG> g_commitKb;

struct TrimResult {
    bool done = false;
    double at = 0;
    DWORD pid = 0;
    ULONGLONG beforeKb = 0;
    ULONGLONG afterKb = 0;
    std::string note;
};
TrimResult g_trim;

std::wstring g_windowsSignature;

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM param)
{
    auto* pair = reinterpret_cast<std::pair<const std::set<DWORD>*, std::vector<std::wstring>*>*>(param);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (!pair->first->contains(pid) || !::IsWindowVisible(hwnd))
        return TRUE;
    wchar_t cls[128] {}, title[128] {};
    ::GetClassNameW(hwnd, cls, _countof(cls));
    ::GetWindowTextW(hwnd, title, _countof(title));
    RECT r {};
    ::GetWindowRect(hwnd, &r);
    pair->second->push_back(fmt(L"%lu|%s|%s|%ld,%ld,%ld,%ld|%s", pid, cls, title, r.left, r.top, r.right - r.left, r.bottom - r.top,
        ::IsIconic(hwnd) ? L"minimized" : L""));
    return TRUE;
}

void noteWindows(const std::set<DWORD>& family)
{
    std::vector<std::wstring> rows;
    std::pair<const std::set<DWORD>*, std::vector<std::wstring>*> param {&family, &rows};
    ::EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&param));
    std::ranges::sort(rows);
    std::wstring signature;
    for (const std::wstring& r : rows)
        signature += r + L"\n";
    if (signature == g_windowsSignature)
        return;
    g_windowsSignature = signature;
    toolEvent("visible-windows", [&](Json& j) {
        j.k("windows").a();
        for (const std::wstring& r : rows)
            j.v(clean(r));
        j.end();
    });
}

struct Sampled {
    ULONGLONG appPrivKb = 0;
    ULONGLONG familyPrivKb = 0;
    ULONGLONG availMb = 0;
    bool foregroundOurs = false;
};

Sampled sample(const Snapshot& s, const std::set<DWORD>& family, Counters& counters)
{
    Sampled out;
    LASTINPUTINFO input {sizeof input};
    ::GetLastInputInfo(&input);
    const DWORD idleMs = ::GetTickCount() - input.dwTime;
    const HWND foreground = ::GetForegroundWindow();
    DWORD fgPid = 0;
    if (foreground)
        ::GetWindowThreadProcessId(foreground, &fgPid);
    out.foregroundOurs = family.contains(fgPid);

    MEMORYSTATUSEX ms {sizeof ms};
    ::GlobalMemoryStatusEx(&ms);
    PERFORMANCE_INFORMATION pi {sizeof pi};
    ::GetPerformanceInfo(&pi, sizeof pi);
    const ULONGLONG page = pi.PageSize;
    out.availMb = ms.ullAvailPhys >> 20;
    g_availKb.push_back(ms.ullAvailPhys >> 10);
    g_commitKb.push_back(pi.CommitTotal * page >> 10);

    const double t = sinceStart();
    Json j;
    j.o().kv("t", t).kv("time", nowUtc()).kv("idle_ms", static_cast<unsigned long>(idleMs));
    j.k("fg").o().kv("pid", fgPid);
    if (const Proc* p = s.find(fgPid))
        j.kv("name", p->name);
    j.kv("ours", out.foregroundOurs);
    if (out.foregroundOurs) {
        wchar_t cls[128] {}, title[128] {};
        ::GetClassNameW(foreground, cls, _countof(cls));
        ::GetWindowTextW(foreground, title, _countof(title));
        j.kv("class", cls).kv("title", clean(title)).kv("minimized", ::IsIconic(foreground) != 0);
    }
    j.end();
    j.k("sys").o().kv("load_pct", static_cast<unsigned long>(ms.dwMemoryLoad)).kv("avail_kb", ms.ullAvailPhys >> 10);
    j.kv("commit_kb", pi.CommitTotal * page >> 10).kv("commit_limit_kb", pi.CommitLimit * page >> 10);
    j.kv("system_cache_kb", pi.SystemCache * page >> 10).kv("kernel_paged_kb", pi.KernelPaged * page >> 10);
    j.kv("kernel_nonpaged_kb", pi.KernelNonpaged * page >> 10).kv("processes", static_cast<unsigned long>(pi.ProcessCount));
    j.end();
    counters.sample(j);
    j.k("procs").a();
    const Proc* app = mainProcess(s, family);
    const bool away = idleMs > 60'000 && !out.foregroundOurs;
    for (const DWORD pid : family) {
        const Proc* p = s.find(pid);
        if (!p)
            continue;
        Info& info = infoOf(*p);
        j.o().kv("pid", p->pid).kv("name", p->name).kv("priv_ws_kb", p->privWs >> 10).kv("ws_kb", p->ws >> 10).kv("commit_kb", p->commit >> 10);
        j.kv("peak_ws_kb", p->peakWs >> 10).kv("peak_commit_kb", p->peakCommit >> 10).kv("virt_mb", p->virt >> 20);
        j.kv("threads", p->threads).kv("handles", p->handles).kv("cpu_ms", p->cpu / 10'000);
        j.kv("read_kb", p->readBytes >> 10).kv("write_kb", p->writeBytes >> 10).kv("other_kb", p->otherBytes >> 10);
        j.kv("read_ops", p->readOps).kv("write_ops", p->writeOps).kv("hard_faults", p->hardFaults).kv("faults", p->faults);
        j.kv("paged_pool_kb", p->pagedPool >> 10).kv("nonpaged_pool_kb", p->nonPagedPool >> 10);
        if (info.handle)
            j.kv("gdi", static_cast<unsigned long>(::GetGuiResources(info.handle, GR_GDIOBJECTS)))
                .kv("user", static_cast<unsigned long>(::GetGuiResources(info.handle, GR_USEROBJECTS)));
        j.end();

        Series& series = g_series[pid];
        if (series.t.empty()) {
            series.name = p->name;
            series.firstRead = p->readBytes;
            series.firstWrite = p->writeBytes;
            series.firstCpu = p->cpu;
            series.firstHard = p->hardFaults;
        }
        series.t.push_back(static_cast<float>(t));
        series.privKb.push_back(p->privWs >> 10);
        series.commitKb.push_back(p->commit >> 10);
        series.idleAndAway.push_back(away);
        series.lastRead = p->readBytes;
        series.lastWrite = p->writeBytes;
        series.lastCpu = p->cpu;
        series.lastHard = p->hardFaults;
        out.familyPrivKb += p->privWs >> 10;
    }
    j.end();
    j.end();
    g_files["samples.jsonl"] += j.str() + "\n";
    if (app)
        out.appPrivKb = app->privWs >> 10;
    return out;
}

std::map<DWORD, std::pair<ULONGLONG, ULONG>> g_lastIo; // pid -> (bytes, hard faults) at the last top list

void writeTop(const Snapshot& s)
{
    std::vector<const Proc*> all;
    ULONGLONG totalPriv = 0;
    for (const Proc& p : s.procs) {
        all.push_back(&p);
        totalPriv += p.privWs;
    }
    Json j;
    j.o().kv("t", sinceStart()).kv("time", nowUtc()).kv("total_priv_ws_kb", totalPriv >> 10);
    const auto row = [&](const Proc* p) {
        j.o().kv("name", p->name).kv("pid", p->pid).kv("priv_ws_kb", p->privWs >> 10).kv("commit_kb", p->commit >> 10).kv("ws_kb", p->ws >> 10).end();
    };
    std::ranges::sort(all, [](const Proc* a, const Proc* b) { return a->privWs > b->privWs; });
    j.k("by_priv_ws").a();
    for (size_t i = 0; i < all.size() && i < 25; ++i)
        row(all[i]);
    j.end();
    std::ranges::sort(all, [](const Proc* a, const Proc* b) { return a->commit > b->commit; });
    j.k("by_commit").a();
    for (size_t i = 0; i < all.size() && i < 15; ++i)
        row(all[i]);
    j.end();
    // Who used the disks since the last list (hard faults: pages read back from disk).
    struct Delta {
        const Proc* p;
        ULONGLONG io;
        ULONG hard;
    };
    std::vector<Delta> deltas;
    std::map<DWORD, std::pair<ULONGLONG, ULONG>> now;
    for (const Proc& p : s.procs) {
        const ULONGLONG io = p.readBytes + p.writeBytes;
        now[p.pid] = {io, p.hardFaults};
        if (const auto it = g_lastIo.find(p.pid); it != g_lastIo.end())
            deltas.push_back({&p, io >= it->second.first ? io - it->second.first : 0, p.hardFaults >= it->second.second ? p.hardFaults - it->second.second : 0});
    }
    g_lastIo = std::move(now);
    if (!deltas.empty()) {
        std::ranges::sort(deltas, [](const Delta& a, const Delta& b) { return a.io > b.io; });
        j.k("by_io_since_last").a();
        for (size_t i = 0; i < deltas.size() && i < 10 && deltas[i].io; ++i)
            j.o().kv("name", deltas[i].p->name).kv("pid", deltas[i].p->pid).kv("io_kb", deltas[i].io >> 10).end();
        j.end();
        std::ranges::sort(deltas, [](const Delta& a, const Delta& b) { return a.hard > b.hard; });
        j.k("by_hard_faults_since_last").a();
        for (size_t i = 0; i < deltas.size() && i < 10 && deltas[i].hard; ++i)
            j.o().kv("name", deltas[i].p->name).kv("pid", deltas[i].p->pid).kv("hard_faults", deltas[i].hard).end();
        j.end();
    }
    for (const Proc& p : s.procs)
        if (p.name == L"Memory Compression" || p.name == L"MemCompression")
            j.kv("memory_compression_ws_kb", p.ws >> 10);
    writePageFiles(j);
    j.end();
    g_files["top.jsonl"] += j.str() + "\n";
}

void trimTest(const Snapshot& s, const std::set<DWORD>& family)
{
    const Proc* app = mainProcess(s, family);
    if (!app) {
        g_trim.note = "Win顺 not running";
        toolEvent("trim-skipped", [](Json& j) { j.kv("reason", "not running"); });
        return;
    }
    const HANDLE process = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, app->pid);
    const bool ok = process && ::EmptyWorkingSet(process);
    const DWORD error = ok ? 0 : ::GetLastError();
    if (process)
        ::CloseHandle(process);
    ::Sleep(1500);
    const Snapshot after = takeSnapshot();
    const Proc* now = after.find(app->pid);
    g_trim.done = ok;
    g_trim.at = sinceStart();
    g_trim.pid = app->pid;
    g_trim.beforeKb = app->privWs >> 10;
    g_trim.afterKb = now ? now->privWs >> 10 : 0;
    if (!ok)
        g_trim.note = "EmptyWorkingSet failed, error " + std::to_string(error);
    toolEvent("trim", [&](Json& j) {
        j.kv("pid", app->pid).kv("ok", ok).kv("error", static_cast<unsigned long>(error)).kv("before_priv_ws_kb", g_trim.beforeKb).kv(
            "after_priv_ws_kb", g_trim.afterKb).kv("before_ws_kb", app->ws >> 10).kv("after_ws_kb", now ? now->ws >> 10 : 0);
    });
}

// ---------------------------------------------------------------- report

ULONGLONG percentile(std::vector<ULONGLONG> v, double q)
{
    if (v.empty())
        return 0;
    std::ranges::sort(v);
    return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
}

std::wstring mb(ULONGLONG kb)
{
    return fmt(L"%.0f", static_cast<double>(kb) / 1024.0);
}

// The app's own journal: idle-trim lines (what it gave back, or why it waited).
std::wstring journalSummary()
{
    std::map<std::string, int> waiting;
    int trims = 0, held = 0;
    long maxMb = 0;
    std::vector<std::string> recent;
    for (const char* name : {"app/diagnostics/events.2.jsonl", "app/diagnostics/events.1.jsonl", "app/diagnostics/events.jsonl"}) {
        const auto it = g_files.find(name);
        if (it == g_files.end())
            continue;
        size_t pos = 0;
        const std::string& text = it->second;
        while (pos < text.size()) {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos)
                eol = text.size();
            const std::string line = text.substr(pos, eol - pos);
            pos = eol + 1;
            if (line.find("\"event\":\"idle-trim\"") == std::string::npos)
                continue;
            if (const size_t w = line.find("\"waiting\":\""); w != std::string::npos) {
                const size_t from = w + 11;
                ++waiting[line.substr(from, line.find('"', from) - from)];
            } else if (line.find("\"held\"") != std::string::npos) {
                ++held;
            } else if (const size_t m = line.find("\"private_mb\":"); m != std::string::npos) {
                ++trims;
                maxMb = std::max(maxMb, std::strtol(line.c_str() + m + 13, nullptr, 10));
            }
            recent.push_back(line);
        }
    }
    std::wstring s = fmt(L"  归还 %d 次（归还前最多 %ld MB），系统唤醒后暂缓 %d 次\r\n", trims, maxMb, held);
    for (const auto& [reason, count] : waiting)
        s += fmt(L"  等待（%s）%d 次\r\n", wide(reason).c_str(), count);
    s += L"  最近 8 条：\r\n";
    for (size_t i = recent.size() > 8 ? recent.size() - 8 : 0; i < recent.size(); ++i)
        s += L"    " + wide(recent[i]) + L"\r\n";
    return s;
}

std::string report(const Places& places, bool admin, int minutes)
{
    std::wstring r;
    r += fmt(L"Win顺 内存诊断报告（工具 %s）\r\n", kToolVersion);
    r += fmt(L"生成于 %s UTC，已运行 %s（计划 %d 分钟），结束原因：%s，管理员：%s\r\n\r\n", wide(nowUtc()).c_str(),
        mmss(sinceStart()).c_str(), minutes, wide(g_stopReason.load()).c_str(), admin ? L"是" : L"否");
    MEMORYSTATUSEX ms {sizeof ms};
    ::GlobalMemoryStatusEx(&ms);
    r += fmt(L"电脑内存 %llu MB；可用内存 最少 %s MB / 中位 %s MB；提交 最多 %s MB\r\n", ms.ullTotalPhys >> 20,
        mb(percentile(g_availKb, 0)).c_str(), mb(percentile(g_availKb, 0.5)).c_str(), mb(percentile(g_commitKb, 1)).c_str());
    r += fmt(L"数据文件夹 %s（%s）\r\n", clean(places.dataDir).c_str(), places.portable ? L"便携版" : L"安装版");
    r += g_diskSummary + L"\r\n";

    r += L"各进程专用工作集（任务管理器“内存”列），MB：最少 / 中位 / 最多 / 最后；提交 首 → 末（最多）；硬错误页；读盘；CPU 秒\r\n";
    std::vector<std::pair<DWORD, const Series*>> rows;
    for (const auto& [pid, s] : g_series)
        if (!s.privKb.empty())
            rows.push_back({pid, &s});
    std::ranges::sort(rows, [](const auto& x, const auto& y) { return percentile(x.second->privKb, 1) > percentile(y.second->privKb, 1); });
    for (const auto& [pid, series] : rows) {
        const Series& s = *series;
        r += fmt(L"  %-26s %6lu  %s / %s / %s / %s；提交 %s → %s（%s）；硬错误页 %lu；读 %s MB；CPU %.1f\r\n", s.name.c_str(), pid,
            mb(percentile(s.privKb, 0)).c_str(), mb(percentile(s.privKb, 0.5)).c_str(), mb(percentile(s.privKb, 1)).c_str(),
            mb(s.privKb.back()).c_str(), mb(s.commitKb.front()).c_str(), mb(s.commitKb.back()).c_str(), mb(percentile(s.commitKb, 1)).c_str(),
            s.lastHard - s.firstHard, mb((s.lastRead - s.firstRead) >> 10).c_str(), static_cast<double>(s.lastCpu - s.firstCpu) / 1e7);
        if (!equalsI(s.name, L"WinShun.exe"))
            continue;
        size_t away = 0, awayHigh = 0;
        for (size_t i = 0; i < s.privKb.size(); ++i) {
            if (!s.idleAndAway[i])
                continue;
            ++away;
            if (s.privKb[i] > 100 * 1024)
                ++awayHigh;
        }
        if (away)
            r += fmt(L"      用户离开（空闲 > 60 秒且前台不是 Win顺）的 %zu 次采样里，%zu 次超过 100 MB\r\n", away, awayHigh);
    }

    r += L"\r\n归还测试（工具让主程序归还一次内存，相当于它闲置时自己做的事）：\r\n";
    if (g_trim.done || !g_trim.note.empty()) {
        r += fmt(L"  %s：%s MB → %s MB %s\r\n", mmss(g_trim.at).c_str(), mb(g_trim.beforeKb).c_str(), mb(g_trim.afterKb).c_str(),
            wide(g_trim.note).c_str());
        if (const auto it = g_series.find(g_trim.pid); it != g_series.end()) {
            for (const int after : {60, 300, 600}) {
                const Series& s = it->second;
                for (size_t i = 0; i < s.t.size(); ++i)
                    if (s.t[i] >= g_trim.at + after) {
                        r += fmt(L"  %d 秒后 %s MB\r\n", after, mb(s.privKb[i]).c_str());
                        break;
                    }
            }
        }
    } else {
        r += L"  未进行（提前结束或未到时间）\r\n";
    }
    r += L"\r\nWin顺 自己的闲置归还记录（events.jsonl 的 idle-trim）：\r\n";
    r += journalSummary();
    return utf8(r);
}

const char* kReadme = R"(Win顺 内存诊断包 / WinShun memory report

report.txt            一眼能看的摘要（各进程内存、归还测试、Win顺 自己的归还记录）
samples.jsonl         每 2 秒一行：Win顺 各进程的内存/提交/读写/硬错误页/句柄/CPU，
                      用户空闲时间、前台窗口、电脑可用内存/提交/备用列表/磁盘忙碌
top.jsonl             每 30 秒：全机内存最大的进程、读写盘和硬错误页最多的进程、分页文件
memmaps/<时刻>/       各 Win顺 进程的内存区域图：私有/映射/映像，驻留的私有/共享，
                      按大小分布、最大的分配、映射的文件、线程（启动位置、名称、CPU）
                      00-start、10-min、20-before-trim、21-after-trim、99-end；growth-N = 进程涨了 256 MB
tool-events.jsonl     工具自己的事件：进程出现/退出、可见窗口变化、归还测试结果、错误
system.json           系统、CPU、内存、分页文件、磁盘（机械/固态、总线、型号）、分卷、服务、内核池标记
system/eventlog.jsonl 睡眠/唤醒/开关机、内存不足（Resource-Exhaustion）、Win顺 崩溃/无响应
winshun.json          Win顺 进程（路径、命令行、版本、权限）、数据文件夹/索引文件夹/服务文件夹的文件列表
app/                  Win顺 的诊断日志（events*.jsonl）、WinShun.log、plugins.log、服务日志、设置（已去掉敏感项）
service/              后台服务日志
console.txt           工具窗口里显示过的文字

不含：文件内容、剪贴板内容、搜索记录、密码。用户文件夹路径写作 %USERPROFILE%。
)";

bool writeZip(const Places& places, bool admin, int minutes, bool final)
{
    std::map<std::string, std::string> extra;
    extra["README.txt"] = kReadme;
    extra["report.txt"] = report(places, admin, minutes);
    Json m;
    m.o().kv("tool_version", kToolVersion).kv("final", final).kv("written_at", nowUtc()).kv("ran_seconds", sinceStart()).kv("stop_reason", g_stopReason.load());
    m.k("sources").a();
    for (const std::string& entry : g_manifest)
        m.raw(entry);
    m.end().end();
    extra["manifest.json"] = m.str();

    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 1 << 20))
        return false;
    bool ok = true;
    const auto add = [&](const std::string& name, const std::string& data) {
        const bool stored = name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0;
        if (ok && !mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), stored ? MZ_NO_COMPRESSION : MZ_BEST_SPEED))
            ok = false;
    };
    for (const auto& [name, data] : extra)
        add(name, data);
    for (const auto& [name, data] : g_files)
        add(name, data);
    void* buffer = nullptr;
    size_t bytes = 0;
    if (ok)
        ok = mz_zip_writer_finalize_heap_archive(&zip, &buffer, &bytes) != 0;
    mz_zip_writer_end(&zip);
    if (!ok) {
        mz_free(buffer);
        return false;
    }
    const std::wstring temp = g_zipPath + L".partial";
    const HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = ::WriteFile(file, buffer, static_cast<DWORD>(bytes), &written, nullptr) && written == bytes;
        ::CloseHandle(file);
        ok = ok && ::MoveFileExW(temp.c_str(), g_zipPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok)
            ::DeleteFileW(temp.c_str());
    } else {
        ok = false;
    }
    mz_free(buffer);
    return ok;
}

// ---------------------------------------------------------------- main

BOOL WINAPI onConsoleEvent(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_stopReason = "stopped by Ctrl+C";
        g_stop = true;
        return TRUE;
    }
    // The window is closing: Windows waits ~5 s for this handler.
    g_stopReason = "window closed";
    g_closing = true;
    g_stop = true;
    if (g_done)
        ::WaitForSingleObject(g_done, 4500);
    return TRUE;
}

std::wstring quoteArg(const std::wstring& a)
{
    return a.find_first_of(L" \t\"") == std::wstring::npos ? a : L"\"" + a + L"\"";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    int minutes = kDefaultMinutes;
    int trimAt = -1;
    int intervalMs = kDefaultIntervalMs;
    bool noTrim = false;
    bool noElevate = false;
    bool noWait = false;
    std::wstring outDir;
    std::wstring passOn;
    for (int i = 1; i < argc; ++i)
        if (std::wstring_view(argv[i]) != L"--no-elevate")
            passOn += L" " + quoteArg(argv[i]);
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == L"--minutes" && hasValue)
            minutes = std::clamp(_wtoi(argv[++i]), 1, 24 * 60);
        else if (a == L"--trim-at" && hasValue)
            trimAt = _wtoi(argv[++i]);
        else if (a == L"--interval-ms" && hasValue)
            intervalMs = std::clamp(_wtoi(argv[++i]), 500, 60'000);
        else if (a == L"--out" && hasValue)
            outDir = argv[++i];
        else if (a == L"--no-trim")
            noTrim = true;
        else if (a == L"--no-elevate")
            noElevate = true;
        else if (a == L"--no-wait") // automated runs: no key press, no Explorer
            noWait = true;
    }
    if (trimAt < 0)
        trimAt = std::min(kDefaultTrimAt, minutes * 2 / 3);
    if (minutes < 3 || trimAt <= 0 || trimAt >= minutes)
        noTrim = true;

    g_out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    g_isConsole = ::GetConsoleMode(g_out, &mode) != 0;
    ::SetConsoleTitleW(L"Win顺 内存诊断");
    // A click in the window would freeze output (and the sampling) in
    // "quick edit" selection mode.
    const HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    if (DWORD inMode = 0; ::GetConsoleMode(in, &inMode))
        ::SetConsoleMode(in, (inMode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);

    const bool admin = elevated();
    if (!admin && !noElevate) {
        say(L"Win顺 内存诊断：需要管理员权限才能看到后台服务和完整的内存情况，请在弹出的窗口里点“是”。");
        wchar_t self[MAX_PATH * 4];
        ::GetModuleFileNameW(nullptr, self, _countof(self));
        const std::wstring parameters = passOn + L" --no-elevate";
        SHELLEXECUTEINFOW sei {sizeof sei};
        sei.lpVerb = L"runas";
        sei.lpFile = self;
        sei.lpParameters = parameters.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (::ShellExecuteExW(&sei))
            return 0;
        say(L"没有拿到管理员权限，部分信息收集不到，继续以普通权限运行。");
    }

    g_t0 = ::GetTickCount64();
    g_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::SetConsoleCtrlHandler(onConsoleEvent, TRUE);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool debug = admin && enableDebugPrivilege();
    toolEvent("start", [&](Json& j) {
        j.kv("version", kToolVersion).kv("minutes", minutes).kv("trim_at", noTrim ? -1 : trimAt).kv("interval_ms", intervalMs).kv("administrator", admin).kv("debug_privilege", debug);
    });

    say();
    say(fmt(L"Win顺 内存诊断工具 %s", kToolVersion));
    say(fmt(L"会记录 Win顺 和这台电脑的内存、磁盘情况，共 %d 分钟，结束后在桌面生成一个压缩包，请把它发给我们。", minutes));
    say(L"· 不收集文件内容、剪贴板内容、搜索记录和密码。");
    say(L"· 请照常使用电脑，不要关闭这个窗口（可以最小化）。中途离开电脑也没关系。");
    if (!noTrim)
        say(fmt(L"· 第 %d 分钟会让 Win顺 归还一次内存（和它闲置时自己做的一样），之后第一次呼出可能稍慢。", trimAt));
    say(L"· 想提前结束就按 Ctrl+C，已经记录的内容会照样打包。");
    say();

    loadDevices();
    Snapshot snap = takeSnapshot();
    std::set<DWORD> family = familyOf(snap);
    if (!mainProcess(snap, family))
        say(L"没有找到正在运行的 Win顺。请先启动 Win顺，本工具会一直记录，Win顺 启动后自动包括进来。");
    Places places = locate(snap, family);
    addProfile(places.profile);
    addProfile(knownFolder(FOLDERID_Profile, nullptr));

    SYSTEMTIME local;
    ::GetLocalTime(&local);
    std::wstring folder = !outDir.empty() ? outDir : places.desktop;
    if (folder.empty() || !isDir(folder))
        folder = knownFolder(FOLDERID_Desktop, nullptr);
    if (folder.empty() || !isDir(folder)) {
        wchar_t temp[MAX_PATH];
        ::GetTempPathW(MAX_PATH, temp);
        folder = temp;
    }
    while (!folder.empty() && folder.back() == L'\\')
        folder.pop_back();
    g_zipPath = folder + fmt(L"\\WinShun-memory-report-%04u%02u%02u-%02u%02u.zip", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);

    say(L"正在收集电脑和 Win顺 的基本信息……");
    Counters counters;
    counters.open();
    g_files["system.json"] = systemJson(snap, admin, debug, counters.failed());
    g_files["winshun.json"] = appJson(places, snap, family);
    collectAppFiles(places);
    collectEventLogs();
    for (const DWORD pid : family)
        if (const Proc* p = snap.find(pid))
            toolEvent("process-seen", [&](Json& j) { writeInfo(j, *p); });
    say(L"正在记录各进程的内存分布……");
    mapAll(snap, family, "00-start");
    writeTop(snap);
    writeZip(places, admin, minutes, false);
    say(fmt(L"开始记录。压缩包会保存在：%s", g_zipPath.c_str()));

    const ULONGLONG endAt = g_t0 + static_cast<ULONGLONG>(minutes) * 60'000;
    const ULONGLONG midAt = g_t0 + static_cast<ULONGLONG>(std::max(1, minutes / 3)) * 60'000;
    const ULONGLONG trimDue = g_t0 + static_cast<ULONGLONG>(trimAt) * 60'000;
    ULONGLONG nextSample = ::GetTickCount64();
    ULONGLONG nextTop = nextSample + kTopEveryMs;
    ULONGLONG nextCheckpoint = nextSample + kCheckpointMs;
    ULONGLONG lastSampleAt = 0;
    ULONGLONG afterTrimMapAt = 0;
    bool midDone = minutes < 3;
    bool trimDone = noTrim;
    bool afterTrimDone = noTrim;
    std::set<DWORD> known = family;
    bool appMapped = mainProcess(snap, family) != nullptr;

    while (!g_stop) {
        const ULONGLONG now = ::GetTickCount64();
        if (now >= endAt)
            break;
        if (now < nextSample) {
            ::Sleep(100);
            continue;
        }
        if (lastSampleAt && now - lastSampleAt > static_cast<ULONGLONG>(intervalMs) * 5)
            toolEvent("gap", [&](Json& j) { j.kv("ms", now - lastSampleAt); }); // slept, or the tool was held up
        lastSampleAt = now;
        nextSample += static_cast<ULONGLONG>(intervalMs);
        if (nextSample <= now)
            nextSample = now + static_cast<ULONGLONG>(intervalMs);

        snap = takeSnapshot();
        family = familyOf(snap);
        for (const DWORD pid : family)
            if (!known.contains(pid))
                if (const Proc* p = snap.find(pid))
                    toolEvent("process-seen", [&](Json& j) { writeInfo(j, *p); });
        for (const DWORD pid : known)
            if (!family.contains(pid))
                toolEvent("process-gone", [&](Json& j) { j.kv("pid", pid); });
        known = family;
        forgetGone(snap);
        noteWindows(family);
        const Sampled sampled = sample(snap, family, counters);

        const Proc* app = mainProcess(snap, family);
        if (app && !appMapped) { // started after this tool
            appMapped = true;
            places = locate(snap, family);
            addProfile(places.profile);
            memoryMap(*app, "first-seen");
        }
        // A process grew a lot: what it grew by.
        for (const DWORD pid : family) {
            const Proc* p = snap.find(pid);
            const auto it = g_mapped.find(pid);
            if (!p || it == g_mapped.end() || g_growthMaps >= kMaxGrowthMaps)
                continue;
            if (p->privWs > it->second.first + kGrowthForMap || p->commit > it->second.second + kGrowthForMap) {
                ++g_growthMaps;
                memoryMap(*p, "growth-" + std::to_string(g_growthMaps));
            }
        }
        if (!midDone && now >= midAt) {
            midDone = true;
            mapAll(snap, family, utf8(fmt(L"%02d-min", std::max(1, minutes / 3))));
        }
        // The trim waits while a Win顺 window is in front (up to 3 minutes).
        if (!trimDone && now >= trimDue && (!sampled.foregroundOurs || now >= trimDue + 180'000)) {
            trimDone = true;
            mapAll(snap, family, utf8(fmt(L"%02d-before-trim", trimAt)));
            say(L"正在让 Win顺 归还一次内存……");
            trimTest(snap, family);
            afterTrimMapAt = ::GetTickCount64() + 30'000;
        }
        if (!afterTrimDone && afterTrimMapAt && now >= afterTrimMapAt) {
            afterTrimDone = true;
            mapAll(snap, family, utf8(fmt(L"%02d-after-trim", trimAt + 1)));
        }
        if (now >= nextTop) {
            nextTop += kTopEveryMs;
            writeTop(snap);
        }
        if (now >= nextCheckpoint) {
            nextCheckpoint += kCheckpointMs;
            writeZip(places, admin, minutes, false);
        }
        status(fmt(L"已记录 %s / %s · Win顺 主程序 %s MB，全部进程 %s MB · 电脑可用内存 %.1f GB", mmss(sinceStart()).c_str(),
            mmss(minutes * 60.0).c_str(), mb(sampled.appPrivKb).c_str(), mb(sampled.familyPrivKb).c_str(),
            static_cast<double>(sampled.availMb) / 1024.0));
    }

    if (!g_closing) {
        say(L"正在整理最后的数据……");
        snap = takeSnapshot();
        family = familyOf(snap);
        mapAll(snap, family, "99-end");
        writeTop(snap);
        // Re-read: the logs now include this run's minutes.
        places = locate(snap, family);
        collectAppFiles(places);
        g_files["winshun-end.json"] = appJson(places, snap, family);
        Json pools;
        pools.o().kv("time", nowUtc());
        writePoolTags(pools, 25);
        pools.end();
        g_files["system/pool-tags-end.json"] = pools.str();
    }
    toolEvent("stop", [](Json& j) { j.kv("reason", g_stopReason.load()); });
    const bool written = writeZip(places, admin, minutes, true);
    ::SetEvent(g_done);
    if (g_closing)
        return 0;
    say();
    if (noWait) {
        say(written ? L"完成：" + g_zipPath : L"压缩包写入失败");
        return written ? 0 : 1;
    }
    if (written) {
        say(L"完成！请把这个文件发给我们：");
        say(L"  " + g_zipPath);
        const std::wstring select = L"/select,\"" + g_zipPath + L"\"";
        ::ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        say(fmt(L"压缩包写入失败（错误 %lu）。请截图这个窗口发给我们。", ::GetLastError()));
    }
    say(L"按任意键关闭这个窗口。");
    if (g_isConsole)
        (void)_getwch();
    return written ? 0 : 1;
}

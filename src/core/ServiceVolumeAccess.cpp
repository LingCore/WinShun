#include "ServiceVolumeAccess.h"

#include "ServiceLocation.h"
#include "VolumeProtocol.h"

#include <QtGlobal>

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ws::ntfs {

namespace {

using namespace volproto;

constexpr DWORD kConnectWaitMs = 2000; // for an instance of the pipe, when every one is taken
constexpr DWORD kReplyMs = 10'000; // for an answer the service gives at once
constexpr DWORD kOpenMs = 60'000; // for MftInfo: the service reads record 0 and the bitmap first
constexpr DWORD kBlockMs = 120'000; // for the next MftBlock (a 4 MB read at low priority on a busy hard disk)
constexpr DWORD kCloseMs = 2000; // for Closed
std::atomic<long long> g_reconnectMs {30'000};

std::wstring describe(const wchar_t* what, DWORD error)
{
    return std::wstring(what) + L" (error " + std::to_wstring(error) + L")";
}

// Connects to the service and shakes hands, within `timeoutMs` for each
// step (a healthy service answers in microseconds; the GUI thread asks with
// a short one). Nothing when it is not there, or refuses us; `error` says which.
std::optional<Channel> connectSession(const std::wstring& pipeName, std::wstring& error, std::wstring* version = nullptr,
    DWORD timeoutMs = kReplyMs, std::uint32_t* capabilities = nullptr, std::uint16_t* minor = nullptr)
{
    const std::wstring path = fullPipePath(pipeName);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const ULONGLONG deadline = ::GetTickCount64() + std::min(kConnectWaitMs, timeoutMs);
    for (;;) {
        // Identification only: the service may ask who we are, not act as us.
        pipe = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            break;
        const DWORD code = ::GetLastError();
        // Every instance taken: the service makes the next one right after
        // accepting a client, so this passes in a moment. WaitNamedPipe says
        // when one is free, but another thread of ours may take it first:
        // keep trying until the deadline.
        const ULONGLONG now = ::GetTickCount64();
        if (code != ERROR_PIPE_BUSY || now >= deadline) {
            error = describe(code == ERROR_FILE_NOT_FOUND ? L"the index service is not running" : L"cannot open the pipe", code);
            return std::nullopt;
        }
        if (!::WaitNamedPipeW(path.c_str(), static_cast<DWORD>(deadline - now)))
            ::Sleep(20);
    }
    DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
    ::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    Channel channel(pipe);
    Hello hello;
    hello.version[0] = WINSHUN_VERSION_MAJOR;
    hello.version[1] = WINSHUN_VERSION_MINOR;
    hello.version[2] = WINSHUN_VERSION_PATCH;
    hello.pid = ::GetCurrentProcessId();
    Writer w;
    encode(w, hello);
    Type type {};
    std::vector<std::byte> payload;
    if (!channel.write(w.finish(Type::Hello), timeoutMs) || !channel.read(type, payload, timeoutMs)) {
        error = describe(L"the index service did not answer", channel.lastError());
        return std::nullopt;
    }
    if (type == Type::HelloRefused) {
        HelloRefused refused;
        decode(payload, refused);
        error = refused.reason == static_cast<std::uint32_t>(Refusal::Protocol)
            ? L"the index service speaks another version of the protocol: run the installer again"
            : L"the index service refused the connection (reason " + std::to_wstring(refused.reason) + L")";
        return std::nullopt;
    }
    HelloOk ok;
    if (type != Type::HelloOk || !decode(payload, ok)) {
        error = L"the index service answered something else";
        return std::nullopt;
    }
    if (ok.major != kMajor) {
        error = L"the index service speaks protocol " + std::to_wstring(ok.major) + L", this program "
            + std::to_wstring(kMajor) + L": run the installer again";
        return std::nullopt;
    }
    if (version)
        *version = std::to_wstring(ok.version[0]) + L"." + std::to_wstring(ok.version[1]) + L"."
            + std::to_wstring(ok.version[2]);
    if (capabilities)
        *capabilities = ok.capabilities;
    if (minor)
        *minor = ok.minor;
    return channel;
}

// The service's reply to a request that has one: the frame, or an error
// message for anything else.
bool ask(Channel& channel, std::span<const std::byte> request, Type wanted, std::vector<std::byte>& payload,
    DWORD timeoutMs, std::wstring& error)
{
    Type type {};
    if (!channel.write(request, kReplyMs) || !channel.read(type, payload, timeoutMs)) {
        error = describe(L"the index service's connection broke", channel.lastError());
        return false;
    }
    if (type == wanted)
        return true;
    if (type == Type::Error) {
        Error e;
        if (decode(payload, e))
            error = e.message + L" (error " + std::to_wstring(e.code) + L")";
        else
            error = L"the index service refused the request";
        return false;
    }
    if (type == Type::MftDone) {
        MftDone done;
        error = decode(payload, done) ? done.error : L"the MFT could not be read";
        return false;
    }
    error = L"the index service answered with frame " + std::to_wstring(static_cast<unsigned>(type));
    return false;
}

// ---- the MFT, block by block --------------------------------------------------------

class PipeRecordSource final : public RecordSource {
public:
    PipeRecordSource(Channel&& channel, const MftInfo& info, bool withSizes, bool withCreated)
        : m_channel(std::move(channel))
        , m_info(info)
        , m_withSizes(withSizes)
        , m_withCreated(withCreated)
    {
    }
    explicit PipeRecordSource(std::wstring error)
        : m_error(std::move(error))
    {
    }

    bool valid() const noexcept override { return m_error.empty(); }
    const std::wstring& error() const noexcept override { return m_error; }
    std::uint64_t recordCount() const noexcept override { return m_info.recordCount; }
    std::uint64_t bytes() const noexcept override { return m_info.validBytes; }
    std::uint64_t bytesDone() const noexcept override { return m_done ? m_info.validBytes : m_block.bytesDone(); }

    bool next() override
    {
        if (!valid() || m_done)
            return false;
        Type type {};
        std::vector<std::byte> payload;
        if (!m_channel.read(type, payload, kBlockMs)) {
            m_error = describe(L"the index service's connection broke", m_channel.lastError());
            return false;
        }
        switch (type) {
        case Type::MftBlock:
            if (!m_block.load(std::move(payload), m_withSizes, m_withCreated)) {
                m_error = L"the index service sent a malformed block";
                return false;
            }
            return true;
        case Type::MftDone: {
            MftDone done;
            if (!decode(payload, done)) {
                m_error = L"the index service sent a malformed end";
            } else if (done.status == static_cast<std::uint32_t>(MftStatus::Done)) {
                m_done = true;
            } else if (done.status == static_cast<std::uint32_t>(MftStatus::VolumeLost)) {
                m_error = L"the index service let go of the volume";
            } else {
                m_error = done.error.empty() ? L"the index service could not read the MFT" : done.error;
            }
            return false;
        }
        case Type::VolumeLost:
            m_error = L"the index service let go of the volume";
            return false;
        default:
            m_error = L"the index service answered with frame " + std::to_wstring(static_cast<unsigned>(type));
            return false;
        }
    }
    std::uint32_t firstRecord() const noexcept override { return m_block.firstRecord(); }
    std::size_t blockRecords() const noexcept override { return m_block.count(); }
    bool parse(std::size_t i, FileRecord& out) override { return m_block.record(i, out); }

private:
    Channel m_channel;
    MftInfo m_info;
    BlockReader m_block;
    std::wstring m_error;
    bool m_done = false;
    bool m_withSizes = false; // the service speaks minor 2: its records carry sizes
    bool m_withCreated = false;
};

// ---- a journal followed -------------------------------------------------------------

class PipeJournalSource final : public JournalSource {
public:
    PipeJournalSource(std::wstring pipeName, std::wstring_view root, std::uint32_t serial, std::uint64_t journalId)
        : m_pipeName(std::move(pipeName))
        , m_root(root)
        , m_serial(serial)
        , m_journalId(journalId)
        , m_fallbackEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
        m_valid = m_fallbackEvent && connect();
    }

    ~PipeJournalSource() override
    {
        cancel();
        if (m_fallbackEvent)
            ::CloseHandle(m_fallbackEvent);
    }

    bool valid() const noexcept override { return m_valid; }
    void* event() const noexcept override { return m_channel.valid() ? m_channel.event() : m_fallbackEvent; }

    void start(std::int64_t usn, bool wait) override
    {
        cancel();
        m_usn = usn;
        ::ResetEvent(m_fallbackEvent);
        if (!m_channel.valid() && !m_lost && !connect()) {
            ::SetEvent(m_fallbackEvent); // finish() tries again later, or gives up
            return;
        }
        if (m_lost) {
            ::SetEvent(m_fallbackEvent);
            return;
        }
        const volproto::Read read {usn, wait}; // not ntfs::Read, the MFT's
        Writer w;
        encode(w, read);
        if (!m_channel.write(w.finish(Type::Read), kReplyMs) || !m_channel.beginRead()) {
            m_channel.close();
            ::SetEvent(m_fallbackEvent);
            return;
        }
        m_pending = true;
    }

    bool finish(std::vector<UsnRecord>& records, std::int64_t& nextUsn) override
    {
        records.clear();
        if (m_lost)
            return false;
        if (!m_pending)
            return retryLater(nextUsn); // start() could not ask: the service was away
        m_pending = false;
        Type type {};
        std::vector<std::byte> payload;
        if (!m_channel.finishRead(type, payload, kReplyMs)) {
            m_channel.close();
            return retryLater(nextUsn);
        }
        switch (type) {
        case Type::UsnBatch: {
            m_buffer = std::move(payload);
            const std::int64_t next = parseUsnRecords(m_buffer, records);
            if (next < 0) {
                records.clear();
                qWarning("WinShun: the index service sent a malformed journal batch for %ls", m_root.c_str());
                m_channel.close();
                return retryLater(nextUsn);
            }
            m_lostSince.reset();
            nextUsn = next;
            return true;
        }
        case Type::JournalLost: {
            JournalLost lost;
            decode(payload, lost);
            qWarning("WinShun: the change journal of %ls is lost (error %lu, from the index service)", m_root.c_str(),
                static_cast<unsigned long>(lost.error));
            m_lost = true;
            return false;
        }
        case Type::VolumeLost:
        case Type::Error:
            m_lost = true;
            return false;
        case Type::Cancelled: // a stray one: nothing new
            nextUsn = m_usn;
            return true;
        default:
            m_channel.close();
            return retryLater(nextUsn);
        }
    }

    void cancel() override
    {
        if (!m_pending)
            return;
        m_pending = false;
        // The service answers Cancelled, after the batch if the read had
        // just completed: read up to it, so the next reply is the next read's.
        Writer w;
        if (!m_channel.write(w.finish(Type::Cancel), kReplyMs)) {
            m_channel.close();
            return;
        }
        for (int frames = 0; frames < 4; ++frames) {
            Type type {};
            std::vector<std::byte> payload;
            if (!m_channel.finishRead(type, payload, kCloseMs)) {
                m_channel.close();
                return;
            }
            if (type == Type::Cancelled)
                return;
            if (type == Type::JournalLost || type == Type::VolumeLost || type == Type::Error)
                m_lost = true;
            if (!m_channel.beginRead()) {
                m_channel.close();
                return;
            }
        }
        m_channel.close(); // never came: start over
    }

private:
    // Connects and asks to follow the journal. False with m_lost set when
    // the service says it cannot be followed; false without when the
    // service is not there.
    bool connect()
    {
        std::wstring error;
        std::optional<Channel> channel = connectSession(m_pipeName, error);
        if (!channel)
            return false;
        FollowJournal follow {m_root, m_serial, m_journalId};
        Writer w;
        encode(w, follow);
        std::vector<std::byte> payload;
        Type type {};
        if (!channel->write(w.finish(Type::FollowJournal), kReplyMs) || !channel->read(type, payload, kReplyMs))
            return false;
        if (type == Type::Following) {
            m_channel = std::move(*channel);
            return true;
        }
        if (type == Type::Error) {
            Error e;
            decode(payload, e);
            qWarning("WinShun: the index service cannot follow the journal of %ls: %ls (error %lu)", m_root.c_str(),
                e.message.c_str(), static_cast<unsigned long>(e.code));
            m_lost = true;
        }
        return false;
    }

    // The service is away: as if nothing new happened, until the window is over.
    bool retryLater(std::int64_t& nextUsn)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!m_lostSince) {
            m_lostSince = now;
            qWarning("WinShun: the index service's connection for %ls broke; reconnecting", m_root.c_str());
        } else if (now - *m_lostSince > std::chrono::milliseconds(g_reconnectMs.load())) {
            qWarning("WinShun: the index service did not come back for %ls: reading the volume again", m_root.c_str());
            m_lost = true;
            return false;
        }
        nextUsn = m_usn;
        return true;
    }

    std::wstring m_pipeName;
    std::wstring m_root;
    std::uint32_t m_serial;
    std::uint64_t m_journalId;
    HANDLE m_fallbackEvent; // event() while there is no connection: set, so finish() is asked
    Channel m_channel;
    std::vector<std::byte> m_buffer; // the last batch: records point into it
    std::int64_t m_usn = 0; // of the last start()
    bool m_valid = false;
    bool m_pending = false;
    bool m_lost = false; // the service said so (or never came back)
    std::optional<std::chrono::steady_clock::time_point> m_lostSince; // the connection broke then
};

// ---- a bulk read (the content index's first build) ----------------------------------

class PipeBulkSource final : public BulkSource {
public:
    explicit PipeBulkSource(Channel&& channel, std::wstring_view root)
        : m_channel(std::move(channel))
        , m_root(root)
    {
    }
    explicit PipeBulkSource(std::wstring error)
        : m_error(std::move(error))
    {
    }

    bool valid() const noexcept override { return m_error.empty(); }
    const std::wstring& error() const noexcept override { return m_error; }

    bool want(std::span<const BulkWant> wants) override
    {
        if (!valid())
            return false;
        // Frames of about a megabyte: so many wants as fit.
        Writer w;
        for (std::size_t at = 0; at < wants.size();) {
            std::size_t n = 0;
            std::size_t bytes = 4;
            while (at + n < wants.size() && (n == 0 || bytes + 14 + wants[at + n].name.size() * 2 <= kBlockBytes)) {
                bytes += 14 + wants[at + n].name.size() * 2;
                ++n;
            }
            w.reset();
            encode(w, wants.subspan(at, n));
            if (!m_channel.write(w.finish(Type::BulkWant), kReplyMs))
                return fail(L"the index service's connection broke");
            at += n;
        }
        return true;
    }

    bool scan(std::vector<BulkFile>& files) override
    {
        files.clear();
        if (!valid())
            return false;
        Writer w;
        if (!m_channel.write(w.finish(Type::BulkScan), kReplyMs))
            return fail(L"the index service's connection broke");
        // The answers come in one burst: taken in whole before anything else.
        for (;;) {
            Type type {};
            std::vector<std::byte> payload;
            if (!m_channel.read(type, payload, kBlockMs))
                return fail(L"the index service's connection broke while it scanned " + m_root);
            if (type == Type::BulkFiles) {
                if (!decode(payload, files))
                    return fail(L"the index service sent malformed bulk files");
                continue;
            }
            if (type == Type::BulkScanned) {
                BulkScanned scanned;
                decode(payload, scanned);
                return true;
            }
            return other(type, payload);
        }
    }

    bool fetch(std::span<const std::uint32_t> tickets,
        const std::function<void(std::uint32_t ticket, std::vector<std::byte>&& bytes)>& sink) override
    {
        if (!valid())
            return false;
        Writer w;
        encode(w, tickets);
        if (!m_channel.write(w.finish(Type::BulkFetch), kReplyMs))
            return fail(L"the index service's connection broke");
        std::unordered_map<std::uint32_t, std::vector<std::byte>> partial; // files that come in several frames
        for (;;) {
            Type type {};
            std::vector<std::byte> payload;
            if (!m_channel.read(type, payload, kBlockMs))
                return fail(L"the index service's connection broke while it read " + m_root);
            if (type == Type::BulkFetched)
                return true;
            if (type != Type::BulkData)
                return other(type, payload);
            BulkDataHeader header;
            std::span<const std::byte> bytes;
            if (!decode(payload, header, bytes))
                return fail(L"the index service sent malformed bulk data");
            if (header.offset == 0 && bytes.size() == header.total) {
                sink(header.ticket, std::vector<std::byte>(bytes.begin(), bytes.end())); // whole in one frame
                continue;
            }
            std::vector<std::byte>& file = partial[header.ticket];
            if (header.offset != file.size())
                return fail(L"the index service sent bulk data out of order");
            file.insert(file.end(), bytes.begin(), bytes.end());
            if (file.size() == header.total) {
                std::vector<std::byte> whole = std::move(file);
                partial.erase(header.ticket);
                sink(header.ticket, std::move(whole));
            }
        }
    }

private:
    bool fail(std::wstring error)
    {
        m_error = std::move(error);
        m_channel.close();
        return false;
    }
    bool other(Type type, std::span<const std::byte> payload)
    {
        if (type == Type::VolumeLost)
            return fail(L"the index service let go of " + m_root);
        if (type == Type::Error) {
            Error e;
            if (decode(payload, e))
                return fail(e.message + L" (error " + std::to_wstring(e.code) + L")");
        }
        return fail(L"the index service answered with frame " + std::to_wstring(static_cast<unsigned>(type)));
    }

    Channel m_channel;
    std::wstring m_root;
    std::wstring m_error;
};

} // namespace

// ---- ServiceVolumeAccess ----------------------------------------------------------

ServiceVolumeAccess::ServiceVolumeAccess(std::wstring pipeName)
    : m_pipeName(std::move(pipeName))
{
}

std::wstring ServiceVolumeAccess::defaultPipeName()
{
    wchar_t name[256] {};
    const DWORD n = ::GetEnvironmentVariableW(kPipeEnvironment, name, static_cast<DWORD>(std::size(name)));
    if (n > 0 && n < std::size(name))
        return std::wstring(name, n);
    return kPipeName;
}

bool ServiceVolumeAccess::installed()
{
    const ServiceState now = state();
    return now != ServiceState::NotInstalled && now != ServiceState::Unknown;
}

ServiceState ServiceVolumeAccess::classify(
    unsigned long openError, unsigned long startType, unsigned long currentState) noexcept
{
    if (openError == ERROR_SERVICE_DOES_NOT_EXIST)
        return ServiceState::NotInstalled;
    if (openError != 0 || currentState == 0)
        return ServiceState::Unknown;
    switch (currentState) {
    case SERVICE_STOPPED:
        return startType == SERVICE_DISABLED ? ServiceState::Disabled : ServiceState::Stopped;
    case SERVICE_STOP_PENDING:
        return ServiceState::Stopped;
    case SERVICE_START_PENDING:
        return ServiceState::Starting;
    default:
        return ServiceState::Running; // also disabled while it still runs: the pipe decides
    }
}

std::optional<ServiceState> ServiceVolumeAccess::forcedState(std::wstring_view value) noexcept
{
    if (value == L"none")
        return ServiceState::NotInstalled;
    if (value == L"disabled")
        return ServiceState::Disabled;
    if (value == L"stopped")
        return ServiceState::Stopped;
    return std::nullopt;
}

const char* ServiceVolumeAccess::stateName(ServiceState state) noexcept
{
    switch (state) {
    case ServiceState::NotInstalled:
        return "not_installed";
    case ServiceState::Disabled:
        return "disabled";
    case ServiceState::Stopped:
        return "stopped";
    case ServiceState::Starting:
        return "starting";
    case ServiceState::Running:
        return "running";
    case ServiceState::Unknown:
        break;
    }
    return "unknown";
}

ServiceState ServiceVolumeAccess::state()
{
    // For testing on a computer that has the service: WINSHUN_SERVICE=none, disabled or stopped.
    wchar_t forced[16] {};
    const DWORD length = ::GetEnvironmentVariableW(L"WINSHUN_SERVICE", forced, static_cast<DWORD>(std::size(forced)));
    if (length > 0 && length < std::size(forced)) {
        if (const auto fake = forcedState(std::wstring_view(forced, length)))
            return *fake;
    }
    const SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
        return ServiceState::Unknown;
    const SC_HANDLE service = ::OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!service) {
        const DWORD error = ::GetLastError();
        ::CloseServiceHandle(manager);
        return classify(error, 0xFFFFFFFF, 0);
    }
    SERVICE_STATUS_PROCESS status {};
    DWORD bytes = 0;
    const bool haveStatus = ::QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status),
        sizeof status, &bytes);
    DWORD startType = 0xFFFFFFFF;
    DWORD needed = 0;
    ::QueryServiceConfigW(service, nullptr, 0, &needed);
    if (needed > 0 && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<std::byte> buffer(needed);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (::QueryServiceConfigW(service, config, needed, &needed))
            startType = config->dwStartType;
    }
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    return classify(0, startType, haveStatus ? status.dwCurrentState : 0);
}

namespace {

// The service's whole command line as registered; empty when it is not
// installed or cannot be read.
std::wstring registeredCommandLine()
{
    const SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
        return {};
    const SC_HANDLE service = ::OpenServiceW(manager, kServiceName, SERVICE_QUERY_CONFIG);
    if (!service) {
        ::CloseServiceHandle(manager);
        return {};
    }
    std::wstring path;
    DWORD needed = 0;
    ::QueryServiceConfigW(service, nullptr, 0, &needed);
    if (needed > 0 && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<std::byte> buffer(needed);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (::QueryServiceConfigW(service, config, needed, &needed) && config->lpBinaryPathName)
            path = config->lpBinaryPathName;
    }
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    return path;
}

} // namespace

std::wstring ServiceVolumeAccess::imagePath()
{
    std::wstring path = registeredCommandLine();
    if (path.size() >= 2 && path.front() == L'"') { // "C:\...\WinShunService.exe"
        const std::size_t close = path.find(L'"', 1);
        path = close != std::wstring::npos ? path.substr(1, close - 1) : path.substr(1);
    }
    return path;
}

std::wstring ServiceVolumeAccess::installedFrom()
{
    const std::wstring line = registeredCommandLine();
    const std::wstring flag = L" --installed-from ";
    const std::size_t at = line.find(flag);
    if (at == std::wstring::npos)
        return {};
    std::wstring rest = line.substr(at + flag.size());
    if (!rest.empty() && rest.front() == L'"') {
        const std::size_t close = rest.find(L'"', 1);
        return close != std::wstring::npos ? rest.substr(1, close - 1) : rest.substr(1);
    }
    return rest.substr(0, rest.find(L' '));
}

bool ServiceVolumeAccess::registeredInUnsafeLocation()
{
    const std::wstring path = imagePath();
    return !path.empty() && !svcloc::pathProtected(path);
}

bool ServiceVolumeAccess::probe()
{
    std::wstring error;
    std::wstring version;
    if (!connectSession(m_pipeName, error, &version, kCloseMs)) { // the GUI thread asks at start
        noteError(std::move(error));
        return false;
    }
    const std::lock_guard lock(m_mutex);
    m_serviceVersion = std::move(version);
    m_lastError.clear();
    return true;
}

std::wstring ServiceVolumeAccess::serviceVersion() const
{
    const std::lock_guard lock(m_mutex);
    return m_serviceVersion;
}

std::wstring ServiceVolumeAccess::lastError() const
{
    const std::lock_guard lock(m_mutex);
    return m_lastError;
}

void ServiceVolumeAccess::noteError(std::wstring error)
{
    const std::lock_guard lock(m_mutex);
    m_lastError = std::move(error);
}

std::unique_ptr<RecordSource> ServiceVolumeAccess::openMft(std::wstring_view root, std::uint32_t serial, bool lowPriority)
{
    std::wstring error;
    std::uint16_t minor = 0;
    std::optional<Channel> channel = connectSession(m_pipeName, error, nullptr, kReplyMs, nullptr, &minor);
    if (!channel) {
        noteError(error);
        return std::make_unique<PipeRecordSource>(std::move(error));
    }
    OpenMft open {std::wstring(root), serial, lowPriority};
    Writer w;
    encode(w, open);
    std::vector<std::byte> payload;
    MftInfo info;
    if (!ask(*channel, w.finish(Type::OpenMft), Type::MftInfo, payload, kOpenMs, error) || !decode(payload, info)) {
        if (error.empty())
            error = L"the index service sent a malformed MftInfo";
        return std::make_unique<PipeRecordSource>(std::move(error));
    }
    return std::make_unique<PipeRecordSource>(std::move(*channel), info, minor >= kMinorSizes, minor >= kMinorCreated);
}

std::optional<JournalInfo> ServiceVolumeAccess::queryJournal(std::wstring_view root, std::uint32_t serial)
{
    std::wstring error;
    std::optional<Channel> channel = connectSession(m_pipeName, error);
    if (!channel) {
        noteError(error);
        return std::nullopt;
    }
    QueryJournal query {std::wstring(root), serial};
    Writer w;
    encode(w, query);
    std::vector<std::byte> payload;
    JournalInfo info;
    if (!ask(*channel, w.finish(Type::QueryJournal), Type::JournalInfo, payload, kReplyMs, error)
        || !decode(payload, info)) {
        qWarning("WinShun: the index service has no change journal for %ls: %ls", std::wstring(root).c_str(),
            error.c_str());
        return std::nullopt;
    }
    return info;
}

std::unique_ptr<JournalSource> ServiceVolumeAccess::openJournal(
    std::wstring_view root, std::uint32_t serial, std::uint64_t journalId)
{
    return std::make_unique<PipeJournalSource>(m_pipeName, root, serial, journalId);
}

void ServiceVolumeAccess::closeVolume(std::wstring_view root)
{
    // On the GUI thread, while Windows waits to eject: 2 s at most, all in.
    std::wstring error;
    std::optional<Channel> channel = connectSession(m_pipeName, error, nullptr, kCloseMs);
    if (!channel)
        return; // not running: it holds nothing
    CloseVolume close {std::wstring(root)};
    Writer w;
    encode(w, close);
    std::vector<std::byte> payload;
    if (!ask(*channel, w.finish(Type::CloseVolume), Type::Closed, payload, kCloseMs, error))
        qWarning("WinShun: the index service did not confirm letting go of %ls: %ls", std::wstring(root).c_str(),
            error.c_str());
}

std::unique_ptr<BulkSource> ServiceVolumeAccess::openBulkRead(std::wstring_view root, std::uint32_t serial, bool quiet)
{
    std::wstring error;
    std::uint32_t capabilities = 0;
    std::optional<Channel> channel = connectSession(m_pipeName, error, nullptr, kReplyMs, &capabilities);
    if (!channel) {
        noteError(error);
        return std::make_unique<PipeBulkSource>(std::move(error));
    }
    if (!(capabilities & kCapabilityBulkRead)) {
        // An older service: the files are read the usual way. Said once.
        static std::atomic<bool> said {false};
        if (!said.exchange(true))
            qWarning("WinShun: the index service has no bulk reads (an older version): the content index opens each file");
        return nullptr;
    }
    OpenBulkRead open {std::wstring(root), serial, quiet};
    Writer w;
    encode(w, open);
    std::vector<std::byte> payload;
    if (!ask(*channel, w.finish(Type::OpenBulkRead), Type::BulkReady, payload, kReplyMs, error))
        return std::make_unique<PipeBulkSource>(std::move(error));
    return std::make_unique<PipeBulkSource>(std::move(*channel), root);
}

std::chrono::milliseconds ServiceVolumeAccess::reconnectWindow() noexcept
{
    return std::chrono::milliseconds(g_reconnectMs.load());
}

void ServiceVolumeAccess::setReconnectWindow(std::chrono::milliseconds window) noexcept
{
    g_reconnectMs = window.count();
}

} // namespace ws::ntfs

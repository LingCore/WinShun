#include "DocExtractor.h"

#include "ContentScanner.h"

#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cwchar>

using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr auto kIdleExit = 30s; // an extractor with nothing to read ends after this
constexpr auto kStartWait = 10s; // for an extractor to say it is ready
constexpr auto kRetryStart = 60s; // after one could not be started
// A ceiling, not a reservation: a PDF page with a huge drawing can take over
// a gigabyte while it is read (seen: 1.2 GB for a 94 MB manual).
constexpr std::uint64_t kMaxProcessMemory = 2ull << 30;
// The longest answer: the text, and its places and sheet names at their most.
constexpr std::uint64_t kMaxPayload
    = DocExtractor::kMaxText + doctext::kMaxPlaces * 12 + doctext::kMaxSheets * (2 + 0xFFFF) + 64;

// How long a file may take: more for a larger one (a PDF of hundreds of pages).
std::chrono::milliseconds timeFor(std::uint64_t size)
{
    return std::min<std::chrono::milliseconds>(20s + std::chrono::milliseconds(size / 1024 / 8), 180s);
}

enum class Io { Done, Failed, TimedOut, Cancelled };

// Reads or writes all of `buffer` on the (overlapped) pipe by `deadline`.
Io transfer(bool write, HANDLE pipe, HANDLE event, void* buffer, std::size_t n,
    std::chrono::steady_clock::time_point deadline, const std::function<bool()>& cancelled)
{
    auto* p = static_cast<char*>(buffer);
    std::size_t done = 0;
    while (done < n) {
        OVERLAPPED overlapped {};
        overlapped.hEvent = event;
        ::ResetEvent(event);
        const auto ask = static_cast<DWORD>(std::min<std::size_t>(n - done, 1u << 20));
        const BOOL ok
            = write ? ::WriteFile(pipe, p + done, ask, nullptr, &overlapped) : ::ReadFile(pipe, p + done, ask, nullptr, &overlapped);
        if (!ok && ::GetLastError() != ERROR_IO_PENDING)
            return Io::Failed;
        for (;;) {
            if (::WaitForSingleObject(event, 50) == WAIT_OBJECT_0)
                break;
            const bool stop = cancelled && cancelled();
            if (stop || std::chrono::steady_clock::now() >= deadline) {
                ::CancelIoEx(pipe, &overlapped);
                DWORD ignored = 0;
                ::GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                return stop ? Io::Cancelled : Io::TimedOut;
            }
        }
        DWORD moved = 0;
        if (!::GetOverlappedResult(pipe, &overlapped, &moved, FALSE) || moved == 0)
            return Io::Failed;
        done += moved;
    }
    return Io::Done;
}

// Our own token, made into one without administrator rights or privileges,
// at low integrity; objects it creates are its user's and the system's.
win32::UniqueHandle restrictedToken()
{
    HANDLE own = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
            TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &own))
        return {};
    const win32::UniqueHandle ownToken(own);

    BYTE administrators[SECURITY_MAX_SID_SIZE];
    BYTE localAdministrator[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof administrators;
    if (!::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &size))
        return {};
    size = sizeof localAdministrator;
    if (!::CreateWellKnownSid(WinLocalAccountAndAdministratorSid, nullptr, localAdministrator, &size))
        return {};
    SID_AND_ATTRIBUTES deny[] = {{administrators, 0}, {localAdministrator, 0}};
    HANDLE restricted = nullptr;
    if (!::CreateRestrictedToken(own, DISABLE_MAX_PRIVILEGE, 2, deny, 0, nullptr, 0, nullptr, &restricted))
        return {};
    win32::UniqueHandle token(restricted);

    BYTE low[SECURITY_MAX_SID_SIZE];
    size = sizeof low;
    if (!::CreateWellKnownSid(WinLowLabelSid, nullptr, low, &size))
        return {};
    TOKEN_MANDATORY_LABEL label {};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid = low;
    if (!::SetTokenInformation(restricted, TokenIntegrityLevel, &label, sizeof label + ::GetLengthSid(low)))
        return {};

    // The default DACL (of what the extractor creates, its process among
    // them): its user and the system. An elevated token's grants only
    // Administrators, which the extractor is not.
    BYTE userBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    BYTE system[SECURITY_MAX_SID_SIZE];
    size = sizeof system;
    DWORD got = 0;
    if (::GetTokenInformation(own, TokenUser, userBuffer, sizeof userBuffer, &got)
        && ::CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size)) {
        const PSID user = reinterpret_cast<TOKEN_USER*>(userBuffer)->User.Sid;
        BYTE aclBuffer[256];
        auto* acl = reinterpret_cast<PACL>(aclBuffer);
        if (::InitializeAcl(acl, sizeof aclBuffer, ACL_REVISION) && ::AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, user)
            && ::AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, system)) {
            TOKEN_DEFAULT_DACL dacl {acl};
            ::SetTokenInformation(restricted, TokenDefaultDacl, &dacl, sizeof dacl);
        }
    }
    return token;
}

// A job that ends its process with WinShun (or when the job is closed),
// caps its memory, lets it start nothing and keeps it to itself.
win32::UniqueHandle sandboxJob()
{
    win32::UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
    if (!job.valid())
        return {};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS
        | JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = kMaxProcessMemory;
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui {};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS
        | JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES
        | JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS;
    if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)
        || !::SetInformationJobObject(job.get(), JobObjectBasicUIRestrictions, &ui, sizeof ui))
        return {};
    return job;
}

} // namespace

struct DocExtractor::Process {
    win32::UniqueHandle job; // closing it ends the process
    win32::UniqueHandle process;
    win32::UniqueHandle pipe;
    win32::UniqueHandle event;
    std::chrono::steady_clock::time_point idleSince;

    ~Process()
    {
        // Gone, with the file handle it may hold, before this returns: a
        // volume being ejected waits for every handle on it to close.
        if (job.valid())
            ::TerminateJobObject(job.get(), 1);
        if (process.valid())
            ::WaitForSingleObject(process.get(), 5000);
    }
};

DocExtractor::DocExtractor(int maxProcesses, Priority priority)
    : m_max(std::max(1, maxProcesses))
    , m_priority(priority)
    , m_codePage(ContentScanner::legacyCodePage())
{
    m_reaper = std::jthread([this](std::stop_token stop) { reap(stop); });
}

DocExtractor::~DocExtractor()
{
    m_reaper.request_stop();
    m_cv.notify_all();
    if (m_reaper.joinable())
        m_reaper.join();
    std::lock_guard lock(m_mutex);
    m_idle.clear();
}

std::wstring DocExtractor::programPath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    path.resize(path.find_last_of(L'\\') + 1);
    return path + L"WinShunExtract.exe";
}

std::unique_ptr<DocExtractor::Process> DocExtractor::start()
{
    static std::atomic<unsigned> counter {0};
    wchar_t name[96];
    std::swprintf(name, std::size(name), L"\\\\.\\pipe\\WinShun.extract.%lu.%u.%llu", ::GetCurrentProcessId(),
        counter.fetch_add(1), static_cast<unsigned long long>(::GetTickCount64()));
    auto p = std::make_unique<Process>();
    p->pipe.reset(::CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0,
        nullptr));
    if (!p->pipe.valid())
        return nullptr;
    SECURITY_ATTRIBUTES inheritable {sizeof inheritable, nullptr, TRUE};
    const win32::UniqueHandle client(
        ::CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING, 0, nullptr));
    p->event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const win32::UniqueHandle token = restrictedToken();
    p->job = sandboxJob();
    if (!client.valid() || !p->event.valid() || !token.valid() || !p->job.valid())
        return nullptr;

    // Only the pipe is inherited; and mitigations no document reader needs
    // to be without (it generates no code, loads nothing from the network).
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
    std::vector<char> attributes(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!::InitializeProcThreadAttributeList(list, 2, 0, &size))
        return nullptr;
    HANDLE inherited = client.get();
    DWORD64 mitigations[2] = {PROCESS_CREATION_MITIGATION_POLICY_HEAP_TERMINATE_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_PROHIBIT_DYNAMIC_CODE_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON,
        0};
    const bool listed = ::UpdateProcThreadAttribute(
                            list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherited, sizeof inherited, nullptr, nullptr)
        && ::UpdateProcThreadAttribute(
            list, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, mitigations, sizeof mitigations, nullptr, nullptr);
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof startup;
    startup.lpAttributeList = list;

    const std::wstring program = programPath();
    const std::wstring folder = program.substr(0, program.find_last_of(L'\\'));
    wchar_t handle[32];
    std::swprintf(handle, std::size(handle), L"%llx",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(client.get())));
    std::wstring commandLine = L"\"" + program + L"\" --pipe " + handle;
    if (m_priority == Priority::Background)
        commandLine += L" --background";
    PROCESS_INFORMATION info {};
    const BOOL created = listed
        && ::CreateProcessAsUserW(token.get(), program.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, folder.c_str(),
            &startup.StartupInfo, &info);
    ::DeleteProcThreadAttributeList(list);
    if (!created)
        return nullptr;
    p->process.reset(info.hProcess);
    const win32::UniqueHandle thread(info.hThread);
    if (!::AssignProcessToJobObject(p->job.get(), info.hProcess)) {
        ::TerminateProcess(info.hProcess, 1);
        return nullptr;
    }
    ::SetPriorityClass(info.hProcess, m_priority == Priority::Background ? IDLE_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS);
    ::ResumeThread(info.hThread);

    extractproto::Ready ready;
    ready.magic = 0;
    if (transfer(false, p->pipe.get(), p->event.get(), &ready, sizeof ready, std::chrono::steady_clock::now() + kStartWait,
            {})
            != Io::Done
        || ready.magic != extractproto::kReadyMagic)
        return nullptr;
    return p;
}

std::unique_ptr<DocExtractor::Process> DocExtractor::acquire(const std::function<bool()>& cancelled)
{
    std::unique_lock lock(m_mutex);
    for (;;) {
        if (!m_idle.empty()) {
            auto p = std::move(m_idle.back());
            m_idle.pop_back();
            return p;
        }
        if (m_running < m_max) {
            if (std::chrono::steady_clock::now() < m_retryStartAt)
                return nullptr;
            ++m_running;
            lock.unlock();
            auto p = start();
            lock.lock();
            if (!p) {
                --m_running;
                m_retryStartAt = std::chrono::steady_clock::now() + kRetryStart;
                m_cv.notify_all();
            }
            return p;
        }
        if (cancelled && cancelled())
            return nullptr;
        m_cv.wait_for(lock, 50ms);
    }
}

void DocExtractor::release(std::unique_ptr<Process> process)
{
    std::lock_guard lock(m_mutex);
    if (process) {
        process->idleSince = std::chrono::steady_clock::now();
        m_idle.push_back(std::move(process));
    } else {
        --m_running;
    }
    m_cv.notify_all();
}

void DocExtractor::closeIdle()
{
    std::vector<std::unique_ptr<Process>> closing;
    {
        std::lock_guard lock(m_mutex);
        closing.swap(m_idle);
        m_running -= static_cast<int>(closing.size());
    }
    m_cv.notify_all();
}

void DocExtractor::reap(std::stop_token stop)
{
    std::unique_lock lock(m_mutex);
    while (!stop.stop_requested()) {
        m_cv.wait_for(lock, stop, 5s, [] { return false; });
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::unique_ptr<Process>> closing;
        for (auto it = m_idle.begin(); it != m_idle.end();) {
            if (now - (*it)->idleSince >= kIdleExit) {
                closing.push_back(std::move(*it));
                it = m_idle.erase(it);
                --m_running;
            } else {
                ++it;
            }
        }
        if (!closing.empty()) {
            lock.unlock();
            closing.clear(); // ends them
            lock.lock();
        }
    }
}

DocExtractor::Result DocExtractor::extractFile(
    std::wstring_view path, std::int64_t maxBytes, const std::function<bool()>& cancelled)
{
    const win32::UniqueHandle file(::CreateFileW(win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    FILE_BASIC_INFO basic {};
    LARGE_INTEGER size {};
    if (!file.valid() || !::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof basic)
        || !::GetFileSizeEx(file.get(), &size))
        return {};
    constexpr DWORD kNotHere = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN
        | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if ((basic.FileAttributes & kNotHere) || size.QuadPart <= 0 || size.QuadPart > maxBytes)
        return {};
    FILE_IO_PRIORITY_HINT_INFO hint {};
    hint.PriorityHint = IoPriorityHintLow;
    ::SetFileInformationByHandle(file.get(), FileIoPriorityHintInfo, &hint, sizeof hint);
    return extract(file.get(), static_cast<std::uint64_t>(size.QuadPart), cancelled);
}

DocExtractor::Result DocExtractor::extract(HANDLE file, std::uint64_t size, const std::function<bool()>& cancelled)
{
    Result result;
    auto p = acquire(cancelled);
    if (!p) {
        result.retry = true;
        return result;
    }
    HANDLE remote = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), file, p->process.get(), &remote, FILE_GENERIC_READ, FALSE, 0)) {
        release(std::move(p));
        result.retry = true;
        return result;
    }
    extractproto::Request request;
    request.codePage = m_codePage;
    request.file = reinterpret_cast<std::uintptr_t>(remote);
    request.fileSize = size;
    request.maxText = kMaxText;
    const auto deadline = std::chrono::steady_clock::now() + timeFor(size);
    extractproto::ResponseHeader header;
    header.magic = 0;
    std::string payload;
    Io io = transfer(true, p->pipe.get(), p->event.get(), &request, sizeof request, deadline, cancelled);
    if (io == Io::Done)
        io = transfer(false, p->pipe.get(), p->event.get(), &header, sizeof header, deadline, cancelled);
    if (io == Io::Done && (header.magic != extractproto::kResponseMagic || header.payloadBytes > kMaxPayload
                              || header.status > extractproto::Status::Broken))
        io = Io::Failed;
    if (io == Io::Done && header.payloadBytes > 0) {
        payload.resize(static_cast<std::size_t>(header.payloadBytes));
        io = transfer(false, p->pipe.get(), p->event.get(), payload.data(), payload.size(), deadline, cancelled);
    }
    if (io != Io::Done) {
        if (io != Io::Cancelled) {
            DWORD exitCode = STILL_ACTIVE;
            ::WaitForSingleObject(p->process.get(), 100);
            ::GetExitCodeProcess(p->process.get(), &exitCode);
            qWarning("WinShun: the document reader %s (exit code 0x%08lx)",
                io == Io::TimedOut ? "took too long" : "stopped", exitCode);
        }
        p.reset(); // in the middle of something: ended, and replaced when needed
        release(nullptr);
        result.retry = io == Io::Cancelled;
        return result;
    }
    release(std::move(p));
    result.status = header.status;
    if (header.status == extractproto::Status::Ok) {
        auto text = doctext::deserialize(payload, kMaxText);
        if (text)
            result.text = std::move(*text);
        else
            result.status = extractproto::Status::Failed;
    }
    return result;
}

} // namespace ws

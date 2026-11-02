// WinShunPluginHost.exe: the process that starts and watches plugin
// processes for Win顺 (the plugin design, §2–3; stage 0's findings in
// docs/pitfalls.md "插件系统"). Win顺 runs elevated and must not start
// plugins itself; it starts this program once with the user's normal
// rights (Explorer's token), and this program starts every plugin with a
// plain CreateProcessW: handle list, mitigation policy and a nested job with
// the manifest's limits, all on the ordinary path. No Qt, static C runtime:
// it is the first thing a plugin waits for.
//
//   WinShunPluginHost.exe --pipe <name> --key <key>
//
// It connects to Win顺's named pipe `name` (made by plugin::Runner), says
// hello with `key` and the facts about its own token, then speaks lines:
//
//   Win顺 -> runner     "_ {control message}"     a JSON object, "op" says which:
//                        start   {id, exe, args[], cwd, env[[name,value]…], memoryMB, processes,
//                                 clipboardRead, clipboardWrite, ui, strict, encoding, stdin}
//                        stop    {id}             TerminateJobObject
//                        shutdown                 stop everything and exit
//                       "<id> <line>"             a line for the plugin's stdin
//   runner -> Win顺     "_ {control message}":
//                        hello   {key, pid, …facts…}
//                        started {id, pid, createUs} | failed {id, stage, error}
//                        stderr  {id, text}
//                        exited  {id, code, stderrTail[], overlong}
//                        log     {text}
//                       "<id> <line>"             a line of the plugin's stdout, as UTF-8
//
// When the pipe breaks (Win顺 gone) it ends every plugin and exits; Win顺's
// job (KILL_ON_JOB_CLOSE) does the same should it not get to.

#include "Json.h"
#include "Lite.h"
#include "Sandbox.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ws::plugin;
using lite::Handle;

namespace {

constexpr int kStderrTailLines = 20;

// The pipe to Win顺: overlapped, read by the main thread, written by any.
class Uplink {
public:
    bool open(const std::wstring& name, DWORD* error)
    {
        const std::wstring path = L"\\\\.\\pipe\\" + name;
        for (int attempt = 0; attempt < 50; ++attempt) {
            const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
                ::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);
                m_handle.reset(handle);
                m_readEvent.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
                m_writeEvent.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
                return true;
            }
            *error = ::GetLastError();
            if (*error != ERROR_FILE_NOT_FOUND && *error != ERROR_PIPE_BUSY)
                return false;
            ::Sleep(40);
        }
        return false;
    }

    bool send(const std::string& line)
    {
        const std::lock_guard lock(m_writeMutex);
        if (!m_handle.valid() || m_writeBroken)
            return false;
        const std::string text = line + '\n';
        const char* p = text.data();
        std::size_t n = text.size();
        while (n > 0) {
            OVERLAPPED overlapped {};
            overlapped.hEvent = m_writeEvent.get();
            ::ResetEvent(m_writeEvent.get());
            DWORD put = 0;
            if (!::WriteFile(m_handle.get(), p, static_cast<DWORD>(n > (1u << 20) ? (1u << 20) : n), &put, &overlapped)) {
                if (::GetLastError() != ERROR_IO_PENDING || !::GetOverlappedResult(m_handle.get(), &overlapped, &put, TRUE)) {
                    m_writeBroken = true;
                    return false;
                }
            }
            if (put == 0) {
                m_writeBroken = true;
                return false;
            }
            p += put;
            n -= put;
        }
        return true;
    }

    // The next line; nullopt once the pipe is broken. `idle` runs every
    // half second while nothing comes.
    std::optional<std::string> readLine(const std::function<void()>& idle)
    {
        for (;;) {
            if (const std::size_t nl = m_buffer.find('\n'); nl != std::string::npos) {
                std::string line = m_buffer.substr(0, nl);
                m_buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return line;
            }
            if (m_eof)
                return std::nullopt;
            if (!m_readPending) {
                m_readOverlapped = OVERLAPPED {};
                m_readOverlapped.hEvent = m_readEvent.get();
                ::ResetEvent(m_readEvent.get());
                DWORD got = 0;
                if (::ReadFile(m_handle.get(), m_chunk.data(), static_cast<DWORD>(m_chunk.size()), &got, &m_readOverlapped)) {
                    if (got == 0) {
                        m_eof = true;
                        continue;
                    }
                    m_buffer.append(m_chunk.data(), got);
                    continue;
                }
                if (::GetLastError() != ERROR_IO_PENDING) {
                    m_eof = true;
                    continue;
                }
                m_readPending = true;
            }
            if (::WaitForSingleObject(m_readEvent.get(), 500) != WAIT_OBJECT_0) {
                idle();
                continue;
            }
            DWORD got = 0;
            m_readPending = false;
            if (!::GetOverlappedResult(m_handle.get(), &m_readOverlapped, &got, FALSE) || got == 0) {
                m_eof = true;
                continue;
            }
            m_buffer.append(m_chunk.data(), got);
            if (m_buffer.size() > lite::LineReader::kMaxLine)
                m_eof = true; // a line that never ends: Win顺 would not send one
        }
    }

    void close()
    {
        if (m_handle.valid())
            ::CancelIoEx(m_handle.get(), nullptr);
        if (m_readPending) {
            DWORD unused = 0;
            ::GetOverlappedResult(m_handle.get(), &m_readOverlapped, &unused, TRUE);
            m_readPending = false;
        }
        m_handle.reset();
    }

private:
    Handle m_handle;
    Handle m_readEvent;
    Handle m_writeEvent;
    std::mutex m_writeMutex;
    bool m_writeBroken = false;
    std::string m_buffer;
    std::vector<char> m_chunk = std::vector<char>(64 * 1024);
    bool m_readPending = false;
    OVERLAPPED m_readOverlapped {};
    bool m_eof = false;
};

Uplink g_uplink;

void sendControl(json::Value::Object message)
{
    g_uplink.send("_ " + json::write(json::Value(std::move(message))));
}

void log(const std::string& text)
{
    sendControl({{"op", "log"}, {"text", text}});
}

// One plugin process: its job, its three pipes, and the threads that read
// its stdout and stderr and write its stdin.
struct Instance {
    unsigned long long id = 0;
    DWORD pid = 0;
    Handle process;
    Handle job;
    Handle stdinWrite;
    Handle stdoutRead;
    Handle stderrRead;
    UINT codePage = CP_UTF8; // of its stdout
    std::thread outThread, errThread, inThread;
    Handle inEvent; // auto-reset: a line queued
    Handle stopEvent; // manual-reset: the process is gone
    Handle stderrDone; // manual-reset: the stderr thread ended
    std::mutex mutex;
    std::deque<std::string> inQueue;
    std::deque<std::string> stderrTail;
    std::atomic<bool> done {false};

    std::string decode(const std::string& line) const
    {
        if (codePage == CP_UTF8)
            return line;
        return lite::utf8(lite::wide(line, codePage));
    }

    void readStdout()
    {
        lite::LineReader reader(stdoutRead.get());
        const std::string prefix = std::to_string(id) + ' ';
        std::string line;
        while (reader.next(line))
            g_uplink.send(prefix + decode(line));
        if (!line.empty() && !reader.overlong())
            g_uplink.send(prefix + decode(line)); // the last line without its newline
        if (reader.overlong())
            ::TerminateJobObject(job.get(), 2);
        // Its stdout is closed: it has exited, or is about to (or keeps
        // running with stdout closed, which is not a plugin: ended then).
        if (::WaitForSingleObject(process.get(), 10000) != WAIT_OBJECT_0) {
            ::TerminateJobObject(job.get(), 3);
            ::WaitForSingleObject(process.get(), 5000);
        }
        DWORD code = 0;
        ::GetExitCodeProcess(process.get(), &code);
        ::WaitForSingleObject(stderrDone.get(), 500); // the tail, if it is still coming
        json::Value::Array tail;
        {
            const std::lock_guard lock(mutex);
            for (const std::string& s : stderrTail)
                tail.push_back(s);
        }
        sendControl({{"op", "exited"}, {"id", id}, {"code", static_cast<unsigned long>(code)}, {"stderrTail", std::move(tail)},
            {"overlong", reader.overlong()}});
        ::SetEvent(stopEvent.get());
        done = true;
    }

    void readStderr()
    {
        lite::LineReader reader(stderrRead.get());
        std::string line;
        const auto keep = [&](const std::string& text) {
            const std::string decoded = decode(text);
            {
                const std::lock_guard lock(mutex);
                stderrTail.push_back(decoded);
                if (stderrTail.size() > kStderrTailLines)
                    stderrTail.pop_front();
            }
            sendControl({{"op", "stderr"}, {"id", id}, {"text", decoded}});
        };
        while (reader.next(line))
            keep(line);
        if (!line.empty())
            keep(line);
        ::SetEvent(stderrDone.get());
    }

    void writeStdin()
    {
        const HANDLE events[] = {stopEvent.get(), inEvent.get()};
        for (;;) {
            const DWORD which = ::WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (which != WAIT_OBJECT_0 + 1)
                break;
            for (;;) {
                std::string line;
                {
                    const std::lock_guard lock(mutex);
                    if (inQueue.empty())
                        break;
                    line = std::move(inQueue.front());
                    inQueue.pop_front();
                }
                if (!lite::writeAll(stdinWrite.get(), line + '\n'))
                    return; // the process is gone (or stopped reading): nothing more to write
            }
        }
    }

    void queueLine(std::string line)
    {
        {
            const std::lock_guard lock(mutex);
            inQueue.push_back(std::move(line));
        }
        ::SetEvent(inEvent.get());
    }

    void end()
    {
        if (job.valid())
            ::TerminateJobObject(job.get(), 1);
        ::SetEvent(stopEvent.get());
        if (outThread.joinable())
            outThread.join();
        if (errThread.joinable())
            errThread.join();
        if (inThread.joinable())
            inThread.join();
    }
};

std::map<unsigned long long, std::unique_ptr<Instance>> g_instances;

// Three anonymous pipes with the child's ends inheritable.
bool makePipe(Handle& ours, Handle& theirs, bool ourEndReads)
{
    SECURITY_ATTRIBUTES inheritable {sizeof inheritable, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!::CreatePipe(&read, &write, &inheritable, 64 * 1024))
        return false;
    if (ourEndReads) {
        ours.reset(read);
        theirs.reset(write);
    } else {
        ours.reset(write);
        theirs.reset(read);
    }
    return ::SetHandleInformation(ours.get(), HANDLE_FLAG_INHERIT, 0) != 0;
}

void start(const json::Value& m)
{
    const unsigned long long id = static_cast<unsigned long long>(m["id"].toInt());
    const auto fail = [&](const char* stage, DWORD error) {
        sendControl({{"op", "failed"}, {"id", id}, {"stage", stage}, {"error", static_cast<unsigned long>(error)}});
    };
    if (g_instances.count(id)) {
        fail("id", ERROR_ALREADY_EXISTS);
        return;
    }
    auto instance = std::make_unique<Instance>();
    instance->id = id;
    instance->codePage = m["encoding"].toString() == "oem" ? CP_OEMCP : CP_UTF8;
    instance->inEvent.reset(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    instance->stopEvent.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    instance->stderrDone.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));

    Handle childStdin, childStdout, childStderr;
    const bool stdinOpen = m["stdin"].toBool(true);
    if (stdinOpen) {
        if (!makePipe(instance->stdinWrite, childStdin, false)) {
            fail("CreatePipe", ::GetLastError());
            return;
        }
    } else {
        // Nothing to read: NUL, not a pipe that is never written (an
        // interpreter may wait on it).
        SECURITY_ATTRIBUTES inheritable {sizeof inheritable, nullptr, TRUE};
        childStdin.reset(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr));
        if (!childStdin.valid()) {
            fail("CreateFile(NUL)", ::GetLastError());
            return;
        }
    }
    if (!makePipe(instance->stdoutRead, childStdout, true) || !makePipe(instance->stderrRead, childStderr, true)) {
        fail("CreatePipe", ::GetLastError());
        return;
    }

    sandbox::JobLimits limits;
    limits.memoryMB = static_cast<unsigned>(m["memoryMB"].toInt(512));
    limits.processes = m["processes"].toBool();
    limits.clipboardRead = m["clipboardRead"].toBool();
    limits.clipboardWrite = m["clipboardWrite"].toBool();
    limits.ui = m["ui"].toBool();
    DWORD error = 0;
    instance->job = sandbox::makeJob(true, limits, &error);
    if (!instance->job.valid()) {
        fail("CreateJobObject", error);
        return;
    }
    const bool strict = m["strict"].toBool();
    Handle token;
    if (strict) {
        token = sandbox::restrictedToken(SECURITY_MANDATORY_LOW_RID, &error);
        if (!token.valid()) {
            fail("CreateRestrictedToken", error);
            return;
        }
    }

    // The user's environment (ours is the user's: this process runs as the
    // user) plus what Win顺 adds.
    std::vector<std::pair<std::wstring, std::wstring>> extra;
    for (const json::Value& pair : m["env"].toArray()) {
        const json::Value::Array& kv = pair.toArray();
        if (kv.size() == 2)
            extra.emplace_back(lite::wide(kv[0].toString()), lite::wide(kv[1].toString()));
    }
    HANDLE own = nullptr;
    ::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &own);
    const Handle ownToken(own);
    std::vector<wchar_t> environment = sandbox::environmentFor(ownToken.get(), extra);

    // Only the three standard handles are inherited; mitigations without
    // PROHIBIT_DYNAMIC_CODE (interpreters generate code).
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
    std::vector<char> attributes(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!::InitializeProcThreadAttributeList(list, 2, 0, &size)) {
        fail("InitializeProcThreadAttributeList", ::GetLastError());
        return;
    }
    HANDLE inherited[3] = {childStdin.get(), childStdout.get(), childStderr.get()};
    DWORD64 mitigations[2] = {PROCESS_CREATION_MITIGATION_POLICY_HEAP_TERMINATE_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON,
        0};
    // UI plugins accept text through the user's input method. Disabling
    // extension points also blocks legacy IMEs; keep that policy for the
    // runner and headless plugins, not for an explicitly permitted UI.
    if (!limits.ui)
        mitigations[0] |= PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON;
    if (!::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr)
        || !::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, mitigations, sizeof mitigations, nullptr, nullptr)) {
        fail("UpdateProcThreadAttribute", ::GetLastError());
        ::DeleteProcThreadAttributeList(list);
        return;
    }
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childStdin.get();
    startup.StartupInfo.hStdOutput = childStdout.get();
    startup.StartupInfo.hStdError = childStderr.get();
    startup.lpAttributeList = list;

    const std::wstring exe = lite::wide(m["exe"].toString());
    std::vector<std::wstring> args;
    for (const json::Value& a : m["args"].toArray())
        args.push_back(lite::wide(a.toString()));
    std::wstring line = sandbox::commandLine(exe, args);
    const std::wstring cwd = lite::wide(m["cwd"].toString());
    const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    PROCESS_INFORMATION info {};
    const LARGE_INTEGER started = lite::tick();
    BOOL created = FALSE;
    if (strict)
        created = ::CreateProcessAsUserW(token.get(), exe.c_str(), line.data(), nullptr, nullptr, TRUE, flags, environment.data(),
            cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info);
    else
        created = ::CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE, flags, environment.data(),
            cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info);
    const DWORD createError = ::GetLastError();
    const long long createUs = lite::microsecondsSince(started);
    ::DeleteProcThreadAttributeList(list);
    // Our copies of the child's ends go now, or its pipes never break.
    childStdin.reset();
    childStdout.reset();
    childStderr.reset();
    if (!created) {
        fail(strict ? "CreateProcessAsUserW" : "CreateProcessW", createError);
        return;
    }
    instance->process.reset(info.hProcess);
    instance->pid = info.dwProcessId;
    const Handle thread(info.hThread);
    if (!::AssignProcessToJobObject(instance->job.get(), info.hProcess)) {
        const DWORD assignError = ::GetLastError();
        ::TerminateProcess(info.hProcess, 1);
        fail("AssignProcessToJobObject", assignError);
        return;
    }
    ::ResumeThread(info.hThread);

    Instance* raw = instance.get();
    raw->outThread = std::thread([raw] { raw->readStdout(); });
    raw->errThread = std::thread([raw] { raw->readStderr(); });
    if (stdinOpen)
        raw->inThread = std::thread([raw] { raw->writeStdin(); });
    g_instances[id] = std::move(instance);
    sendControl({{"op", "started"}, {"id", id}, {"pid", static_cast<unsigned long>(info.dwProcessId)}, {"createUs", createUs}});
}

void reap()
{
    for (auto it = g_instances.begin(); it != g_instances.end();) {
        if (it->second->done) {
            it->second->end();
            it = g_instances.erase(it);
        } else
            ++it;
    }
}

void shutdown()
{
    for (auto& [id, instance] : g_instances)
        instance->end();
    g_instances.clear();
}

void handleControl(const std::string& text)
{
    std::string problem;
    const std::optional<json::Value> message = json::parse(text, &problem);
    if (!message || !message->isObject()) {
        log("bad control message: " + problem);
        return;
    }
    const std::string op = (*message)["op"].toString();
    if (op == "start")
        start(*message);
    else if (op == "stop") {
        const auto it = g_instances.find(static_cast<unsigned long long>((*message)["id"].toInt()));
        if (it != g_instances.end())
            ::TerminateJobObject(it->second->job.get(), 1);
    } else if (op == "shutdown") {
        shutdown();
        ::ExitProcess(0);
    } else
        log("unknown op: " + op);
}

int run(int argc, wchar_t** argv)
{
    std::wstring pipe, key;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == L"--pipe")
            pipe = argv[++i];
        else if (std::wstring(argv[i]) == L"--key")
            key = argv[++i];
    }
    if (pipe.empty()) {
        std::fputs("usage: WinShunPluginHost.exe --pipe <name> --key <key>  (started by WinShun)\n", stderr);
        return 1;
    }
    sandbox::applyOwnMitigations();
    DWORD error = 0;
    if (!g_uplink.open(pipe, &error)) {
        std::fprintf(stderr, "cannot open the pipe (error %lu)\n", error);
        return 2;
    }
    json::Value facts = sandbox::collectFacts({});
    json::Value::Object hello = facts.toObject();
    hello["op"] = "hello";
    hello["key"] = lite::utf8(key);
    sendControl(std::move(hello));

    for (;;) {
        const std::optional<std::string> line = g_uplink.readLine(reap);
        if (!line)
            break; // Win顺 is gone
        reap();
        if (line->empty())
            continue;
        if (line->front() == '_' && line->size() > 1 && (*line)[1] == ' ') {
            handleControl(line->substr(2));
            continue;
        }
        const std::size_t space = line->find(' ');
        if (space == std::string::npos)
            continue;
        const unsigned long long id = std::strtoull(line->c_str(), nullptr, 10);
        const auto it = g_instances.find(id);
        if (it != g_instances.end() && !it->second->done)
            it->second->queueLine(line->substr(space + 1));
    }
    shutdown();
    g_uplink.close();
    return 0;
}

} // namespace

// A windowed program (no console of its own, ever); the arguments come
// from the command line as wmain would get them.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    const int code = run(argc, argv);
    ::LocalFree(argv);
    return code;
}

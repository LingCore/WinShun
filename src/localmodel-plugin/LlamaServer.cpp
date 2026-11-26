#include "LlamaServer.h"

#include "winshun.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace lm {

namespace {

constexpr std::size_t kTailLines = 60;

std::string lower(std::string text)
{
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// An argument as CreateProcess and the C runtime read it back.
std::wstring quoted(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"')
            out.append(backslashes * 2 + 1, L'\\');
        else
            out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

struct Socket {
    SOCKET s = INVALID_SOCKET;
    explicit Socket(SOCKET socket) : s(socket) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket()
    {
        if (s != INVALID_SOCKET)
            ::closesocket(s);
    }
};

sockaddr_in loopback(int port)
{
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

} // namespace

std::vector<std::wstring> arguments(const Launch& launch)
{
    std::vector<std::wstring> args {
        L"--model", launch.model.wstring(),
        L"--alias", winshun::wide(launch.alias),
        L"--host", L"127.0.0.1",
        L"--port", std::to_wstring(launch.port),
        // A fixed context: left to the model (Qwen3.5 says 262144), --fit
        // would take what the memory allows.
        L"--ctx-size", std::to_wstring(launch.context),
        L"--parallel", std::to_wstring(launch.parallel),
        // Memory that stays what it is: no prompt cache in RAM (8 GB by
        // default; each translation is a new text anyway), and few context
        // checkpoints (a hybrid model such as Qwen3.5 keeps up to 32 a slot,
        // about 20 MB each).
        L"--cache-ram", L"0",
        L"--ctx-checkpoints", L"2",
        L"--offline", // never a download of its own
        L"--log-colors", L"off",
        // Info lines with the device and the layers offloaded (readLogLine);
        // never the text asked about.
        L"-lv", L"4",
    };
    if (launch.embedding) {
        // A text is taken in one batch: as large as a slot's context.
        const std::wstring slot = std::to_wstring(launch.context / std::max(1, launch.parallel));
        args.insert(args.end(), {L"--embedding", L"--batch-size", slot, L"--ubatch-size", slot});
    }
    if (!launch.gpu) {
        args.push_back(L"--device");
        args.push_back(L"none");
    }
    return args;
}

std::wstring commandLine(const Launch& launch)
{
    std::wstring line = quoted(launch.program.wstring());
    for (const std::wstring& arg : arguments(launch))
        line += L' ' + quoted(arg);
    return line;
}

void readLogLine(const std::string& line, LogFacts& facts)
{
    // llama.cpp, src/llama.cpp: "…: using device Vulkan0 (NVIDIA GeForce RTX 4060) (0000:01:00.0) - 7000 MiB free"
    if (const std::size_t at = line.find("using device "); at != std::string::npos) {
        const std::size_t open = line.find(" (", at);
        const std::size_t close = open == std::string::npos ? std::string::npos : line.find(") (", open);
        if (close != std::string::npos && facts.device.empty())
            facts.device = line.substr(open + 2, close - open - 2);
    }
    // src/llama-model.cpp: "load_tensors: offloaded 29/29 layers to GPU"
    if (const std::size_t at = line.find("offloaded "); at != std::string::npos && line.find(" layers to GPU", at) != std::string::npos) {
        const char* p = line.data() + at + std::strlen("offloaded ");
        const char* end = line.data() + line.size();
        int done = -1;
        int total = -1;
        const auto first = std::from_chars(p, end, done);
        if (first.ec == std::errc() && first.ptr < end && *first.ptr == '/'
            && std::from_chars(first.ptr + 1, end, total).ec == std::errc()) {
            facts.offloaded = done;
            facts.layers = total;
        }
    }
}

namespace {

bool memoryTrouble(const std::string& all)
{
    for (const char* text : {"out of memory", "failed to allocate", "unable to allocate", "outofdevicememory", "not enough memory", "bad_alloc"}) {
        if (all.find(text) != std::string::npos)
            return true;
    }
    return false;
}

std::string lowered(const std::vector<std::string>& lines)
{
    std::string all;
    for (const std::string& line : lines)
        all += lower(line) + '\n';
    return all;
}

} // namespace

std::string stoppedText(const std::vector<std::string>& lines, unsigned long exitCode, bool gpu, bool chinese)
{
    // STATUS_NO_MEMORY, STATUS_COMMITMENT_LIMIT: the job's limit or the system's.
    if (exitCode == 0xC0000017ul || exitCode == 0xC000012Dul || memoryTrouble(lowered(lines)))
        return gpu ? (chinese ? "模型用着用着停了：显存或内存不够。换小一点的模型，或把“显卡”改成“只用 CPU”"
                              : "The model stopped: not enough video or system memory. Pick a smaller model, or set GPU to CPU only")
                   : (chinese ? "模型用着用着停了：内存不够，换小一点的模型" : "The model stopped: not enough memory. Pick a smaller model");
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lX", exitCode);
    return chinese ? std::string("模型意外停了（") + code + "），再用到时会重新加载"
                   : std::string("The model stopped unexpectedly (") + code + "); it loads again when next needed";
}

std::string failureText(const std::vector<std::string>& lines, bool gpu, bool chinese)
{
    const std::string all = lowered(lines);
    const auto has = [&](const char* text) { return all.find(text) != std::string::npos; };
    if (has("unknown model architecture")) {
        std::string arch;
        for (const std::string& line : lines) {
            if (const std::size_t at = line.find("unknown model architecture: '"); at != std::string::npos) {
                const std::size_t from = at + 29;
                arch = line.substr(from, line.find('\'', from) - from);
            }
        }
        return chinese ? "这版 llama.cpp 还不认识这种模型" + (arch.empty() ? std::string() : "（" + arch + "）") + "：换一个模型，或等插件更新"
                       : "This llama.cpp does not know this kind of model" + (arch.empty() ? std::string() : " (" + arch + ")")
                + ": pick another model or wait for an update";
    }
    if (memoryTrouble(all))
        return gpu ? (chinese ? "显存或内存不够：换小一点的模型，或把“显卡”改成“只用 CPU”"
                              : "Not enough video or system memory: pick a smaller model, or set GPU to CPU only")
                   : (chinese ? "内存不够：换小一点的模型" : "Not enough memory: pick a smaller model");
    if (has("failed to load model") || has("error loading model") || has("failed to read") || has("invalid magic"))
        return chinese ? "模型没能加载：文件可能不完整，或不是 llama.cpp 能用的模型"
                       : "The model did not load: the file may be incomplete, or not one llama.cpp can use";
    std::string last;
    for (auto it = lines.rbegin(); it != lines.rend() && last.empty(); ++it)
        last = winshun::trim(*it);
    if (last.size() > 160)
        last = last.substr(0, 160) + "…";
    if (last.empty())
        return chinese ? "模型没能加载" : "The model did not load";
    return chinese ? "模型没能加载（" + last + "）" : "The model did not load (" + last + ")";
}

int probe(int port, const char* path, int timeoutMs)
{
    Socket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (socket.s == INVALID_SOCKET)
        return -1;
    u_long nonBlocking = 1;
    ::ioctlsocket(socket.s, FIONBIO, &nonBlocking);
    const sockaddr_in address = loopback(port);
    if (::connect(socket.s, reinterpret_cast<const sockaddr*>(&address), sizeof address) == SOCKET_ERROR
        && ::WSAGetLastError() != WSAEWOULDBLOCK)
        return -1;
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket.s, &writable);
    fd_set failed = writable;
    timeval wait {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (::select(0, nullptr, &writable, &failed, &wait) <= 0 || FD_ISSET(socket.s, &failed))
        return -1;
    nonBlocking = 0;
    ::ioctlsocket(socket.s, FIONBIO, &nonBlocking);
    const DWORD receiveMs = static_cast<DWORD>(timeoutMs);
    ::setsockopt(socket.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveMs), sizeof receiveMs);
    const std::string request = std::string("GET ") + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port)
        + "\r\nConnection: close\r\n\r\n";
    if (::send(socket.s, request.data(), static_cast<int>(request.size()), 0) != static_cast<int>(request.size()))
        return -1;
    char head[64] {};
    int got = 0;
    while (got < 12) {
        const int n = ::recv(socket.s, head + got, static_cast<int>(sizeof head) - 1 - got, 0);
        if (n <= 0)
            return -1;
        got += n;
    }
    // "HTTP/1.1 200 OK"
    const std::string_view line(head, static_cast<std::size_t>(got));
    const std::size_t space = line.find(' ');
    int status = -1;
    if (!line.starts_with("HTTP/") || space == std::string_view::npos
        || std::from_chars(line.data() + space + 1, line.data() + line.size(), status).ec != std::errc())
        return -1;
    return status;
}

int freePort()
{
    Socket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (socket.s == INVALID_SOCKET)
        return 0;
    sockaddr_in address = loopback(0);
    int size = sizeof address;
    if (::bind(socket.s, reinterpret_cast<const sockaddr*>(&address), sizeof address) == SOCKET_ERROR
        || ::getsockname(socket.s, reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR)
        return 0;
    return ntohs(address.sin_port);
}

LlamaServer::~LlamaServer() { stop(); }

bool LlamaServer::running() const
{
    return m_process && ::WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
}

std::string LlamaServer::reap(bool chinese)
{
    DWORD code = 0;
    if (!m_process || !::GetExitCodeProcess(m_process, &code))
        code = 0;
    if (m_reader.joinable())
        m_reader.join(); // its last lines: the pipe broke with the process
    const std::string text = stoppedText(lastLines(), code, m_launch.gpu, chinese);
    stop();
    return text;
}

LogFacts LlamaServer::facts() const
{
    const std::lock_guard lock(m_mutex);
    return m_facts;
}

std::vector<std::string> LlamaServer::lastLines() const
{
    const std::lock_guard lock(m_mutex);
    return {m_tail.begin(), m_tail.end()};
}

bool LlamaServer::lostPort() const
{
    for (const std::string& line : lastLines()) {
        const std::string l = lower(line);
        if (l.find("couldn't bind") != std::string::npos || l.find("failed to listen") != std::string::npos
            || l.find("address already in use") != std::string::npos)
            return true;
    }
    return false;
}

bool LlamaServer::gpuTrouble() const
{
    for (const std::string& line : lastLines()) {
        const std::string l = lower(line);
        const bool vulkanFault = l.find("ggml_vulkan") != std::string::npos
            && (l.find("error") != std::string::npos || l.find("failed") != std::string::npos || l.find("lost") != std::string::npos);
        if (vulkanFault || l.find("outofdevicememory") != std::string::npos
            || l.find("device lost") != std::string::npos || l.find("out of memory") != std::string::npos
            || l.find("failed to allocate") != std::string::npos || l.find("unable to allocate") != std::string::npos)
            return true;
    }
    return false;
}

std::uint64_t LlamaServer::memory() const
{
    PROCESS_MEMORY_COUNTERS_EX counters {};
    if (!m_process || !::GetProcessMemoryInfo(m_process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters))
        return 0;
    return counters.PrivateUsage;
}

void LlamaServer::stop()
{
    if (m_process) {
        // Nothing of its own to save: ended at once (a request it was
        // answering sees its connection close).
        ::TerminateProcess(m_process, 0);
        ::WaitForSingleObject(m_process, 5000);
        ::CloseHandle(m_process);
        m_process = nullptr;
    }
    if (m_reader.joinable())
        m_reader.join(); // the pipe broke with the process
}

void LlamaServer::readOutput(HANDLE pipe, std::filesystem::path log)
{
    HANDLE file = ::CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string pending;
    char buffer[8192];
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(pipe, buffer, sizeof buffer, &read, nullptr) || read == 0)
            break;
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ::WriteFile(file, buffer, read, &written, nullptr);
        }
        pending.append(buffer, read);
        for (std::size_t end; (end = pending.find('\n')) != std::string::npos;) {
            std::string line = pending.substr(0, end);
            pending.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const std::lock_guard lock(m_mutex);
            readLogLine(line, m_facts);
            m_tail.push_back(std::move(line));
            if (m_tail.size() > kTailLines)
                m_tail.pop_front();
        }
    }
    if (file != INVALID_HANDLE_VALUE)
        ::CloseHandle(file);
    ::CloseHandle(pipe);
}

bool LlamaServer::start(const Launch& launch, const std::filesystem::path& log, const std::atomic_bool& cancel, int timeoutMs,
    std::string& error, bool chinese)
{
    stop();
    {
        const std::lock_guard lock(m_mutex);
        m_facts = {};
        m_tail.clear();
    }
    m_launch = launch;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(launch.program, ec)) {
        error = chinese ? "插件不完整：找不到 llama-server.exe，请重新安装“本地模型”插件"
                        : "The plugin is incomplete (no llama-server.exe): install it again";
        return false;
    }

    // Its output into a pipe that only it inherits (STARTUPINFOEX's handle
    // list: none of the plugin's own handles go along), its input from NUL.
    SECURITY_ATTRIBUTES inheritable {sizeof inheritable, nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!::CreatePipe(&readEnd, &writeEnd, &inheritable, 0)) {
        error = (chinese ? "启动失败：" : "Could not start: ") + std::to_string(::GetLastError());
        return false;
    }
    ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
    std::array<HANDLE, 2> inherited {writeEnd, nul};
    SIZE_T attributeSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<char> attributeBuffer(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    const bool listed = ::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)
        && ::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
            (nul != INVALID_HANDLE_VALUE ? 2 : 1) * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
    startup.StartupInfo.hStdOutput = writeEnd;
    startup.StartupInfo.hStdError = writeEnd;
    startup.lpAttributeList = listed ? attributes : nullptr;
    std::wstring line = commandLine(launch);
    const std::wstring folder = launch.program.parent_path().wstring();
    PROCESS_INFORMATION info {};
    const BOOL started = listed
        && ::CreateProcessW(launch.program.c_str(), line.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, folder.c_str(), &startup.StartupInfo, &info);
    const DWORD startError = ::GetLastError();
    if (listed)
        ::DeleteProcThreadAttributeList(attributes);
    ::CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE)
        ::CloseHandle(nul);
    if (!started) {
        ::CloseHandle(readEnd);
        error = (chinese ? "llama-server 启动失败（错误 " : "llama-server did not start (error ") + std::to_string(startError) + ")";
        return false;
    }
    ::CloseHandle(info.hThread);
    m_process = info.hProcess;
    m_reader = std::thread([this, readEnd, log] { readOutput(readEnd, log); });

    // Up when /health answers 200 (503 while the model loads).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (cancel.load()) {
            stop();
            error = chinese ? "已取消" : "Canceled";
            return false;
        }
        if (!running()) {
            stop(); // and the reader has every line now
            error = failureText(lastLines(), launch.gpu, chinese);
            return false;
        }
        if (probe(launch.port, "/health", 1000) == 200)
            return true;
        if (std::chrono::steady_clock::now() >= deadline) {
            stop();
            error = chinese ? "模型加载太久没好（超过 " + std::to_string(timeoutMs / 1000) + " 秒）：模型太大或磁盘太慢"
                            : "The model took too long to load (over " + std::to_string(timeoutMs / 1000) + " s): too big, or the disk is slow";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

} // namespace lm

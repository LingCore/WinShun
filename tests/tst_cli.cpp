// The command line ws.exe (src/cli, docs/design-cli.md). Its pieces built in
// here: the option table against the help texts and SKILL.md, the parser,
// the encoder, the start decision with fake launchers. And the real ws.exe
// run against Win顺's MCP server in this process on a test pipe, over an index
// walked from a temporary folder: its bytes on stdout and stderr and its exit
// codes. Raw pipe servers stand in where Win顺 cannot be made to misbehave, or
// for a Win顺 other than this tree's (Ctrl+Break, a broken pipe, an older
// Win顺, the refusals and the 50 000-item answers of one made for ws).
//
// Nothing here starts Win顺 or touches the user's: every run gets a test pipe
// and a test single-instance mutex (before any "--"), a test settings file,
// WS_TEST_START (a file written instead of starting Win顺), WS_TEST_OPEN (a
// file written instead of opening) and a temporary CLAUDE_CONFIG_DIR.

#include "Args.h"
#include "Commands.h"
#include "Config.h"
#include "Connect.h"
#include "Context.h"
#include "Encoding.h"
#include "Format.h"
#include "Open.h"
#include "Output.h"
#include "Run.h"
#include "Session.h"
#include "Skill.h"
#include "Text.h"
#include "Win.h"

#include "IndexService.h"
#include "McpServer.h"
#include "McpTools.h"
#include "Ntfs.h"
#include "SearchEngine.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

namespace json = ws::plugin::json;

std::wstring testName(const char* kind, const char* tag)
{
    return L"WinShun." + std::wstring(kind, kind + std::strlen(kind)) + L".test-" + std::to_wstring(::GetCurrentProcessId()) + L"-"
        + std::wstring(tag, tag + std::strlen(tag));
}

std::wstring testPipeName(const char* tag)
{
    return testName("Mcp", tag);
}

QString native(const QString& path)
{
    QString p = QDir::toNativeSeparators(path);
    if (p.size() > 1 && p[1] == u':')
        p[0] = p[0].toUpper();
    return p;
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool setModified(const QString& path, int year)
{
    SYSTEMTIME st {};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = 1;
    st.wDay = 1;
    st.wHour = 12;
    FILETIME ft {};
    if (!::SystemTimeToFileTime(&st, &ft))
        return false;
    const HANDLE file = ::CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = ::SetFileTime(file, nullptr, nullptr, &ft);
    ::CloseHandle(file);
    return ok;
}

// ---- running ws.exe ---------------------------------------------------------

// One argument for a command line, quoted as CommandLineToArgvW reads it.
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

struct CaseInsensitive {
    bool operator()(const std::wstring& a, const std::wstring& b) const
    {
        return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};

// This process's environment with `changes` (an empty value removes the
// variable), as CreateProcessW takes it: sorted, each "name=value\0".
std::vector<wchar_t> environmentBlock(const std::map<std::wstring, std::wstring>& changes)
{
    std::map<std::wstring, std::wstring, CaseInsensitive> vars;
    if (wchar_t* block = ::GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += std::wcslen(p) + 1) {
            const std::wstring entry(p);
            const std::size_t equals = entry.find(L'=', 1); // "=C:=C:\x" keeps its first '='
            if (equals != std::wstring::npos)
                vars[entry.substr(0, equals)] = entry.substr(equals + 1);
        }
        ::FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : changes) {
        if (value.empty())
            vars.erase(name);
        else
            vars[name] = value;
    }
    std::vector<wchar_t> out;
    for (const auto& [name, value] : vars) {
        const std::wstring entry = name + L"=" + value;
        out.insert(out.end(), entry.begin(), entry.end());
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

struct Outcome {
    int code = -1;
    QByteArray out;
    QByteArray err;
    qint64 ms = 0;

    QString text() const { return QString::fromUtf8(out); }
    QString errors() const { return QString::fromUtf8(err); }
    QStringList lines() const { return text().split(u'\n', Qt::SkipEmptyParts); }
    // stderr's lines but the note that the index is catching up (a walked
    // index may say so at any time).
    QStringList notes() const
    {
        QStringList kept;
        for (const QString& line : errors().split(u'\n', Qt::SkipEmptyParts)) {
            if (!line.contains(u"索引正在更新"))
                kept.push_back(line);
        }
        return kept;
    }
};

struct RunOptions {
    QStringList args;
    std::map<std::wstring, std::wstring> env; // added to this process's; an empty value removes
    QString folder; // the current folder
    bool newGroup = false; // its own process group, on this console (Ctrl+Break)
    QString readOnlyStdout; // an existing file opened for reading only is its stdout: every write to it fails
    int timeoutMs = 60'000;
    std::function<void(DWORD pid)> started;
    std::function<void()> tick; // each turn of the wait, on this thread
};

// Runs ws.exe to its end; this thread's event loop turns meanwhile (Win顺's
// server is in it).
Outcome runProcess(const RunOptions& options)
{
    Outcome outcome;
    std::wstring command = quoted(QStringLiteral(WINSHUN_CLI).replace(u'/', u'\\').toStdWString());
    for (const QString& arg : options.args)
        command += L" " + quoted(arg.toStdWString());

    SECURITY_ATTRIBUTES inherit {sizeof inherit, nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!::CreatePipe(&outRead, &outWrite, &inherit, 0) || !::CreatePipe(&errRead, &errWrite, &inherit, 0))
        return outcome;
    ::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    const HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    HANDLE readOnly = INVALID_HANDLE_VALUE;
    if (!options.readOnlyStdout.isEmpty())
        readOnly = ::CreateFileW(native(options.readOnlyStdout).toStdWString().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup {};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    startup.hStdOutput = readOnly != INVALID_HANDLE_VALUE ? readOnly : outWrite;
    startup.hStdError = errWrite;
    PROCESS_INFORMATION process {};
    std::vector<wchar_t> environment = environmentBlock(options.env);
    const std::wstring folder = options.folder.isEmpty() ? std::wstring() : native(options.folder).toStdWString();
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | (options.newGroup ? CREATE_NEW_PROCESS_GROUP : CREATE_NO_WINDOW);
    QElapsedTimer timer;
    timer.start();
    const bool created = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, flags, environment.data(),
        folder.empty() ? nullptr : folder.c_str(), &startup, &process);
    ::CloseHandle(outWrite);
    ::CloseHandle(errWrite);
    ::CloseHandle(nul);
    if (readOnly != INVALID_HANDLE_VALUE)
        ::CloseHandle(readOnly);
    if (!created) {
        ::CloseHandle(outRead);
        ::CloseHandle(errRead);
        return outcome;
    }
    const auto drain = [](HANDLE pipe, QByteArray& into) {
        char buffer[64 * 1024];
        DWORD got = 0;
        while (::ReadFile(pipe, buffer, sizeof buffer, &got, nullptr) && got > 0)
            into.append(buffer, static_cast<qsizetype>(got));
    };
    std::thread outReader(drain, outRead, std::ref(outcome.out));
    std::thread errReader(drain, errRead, std::ref(outcome.err));
    if (options.started)
        options.started(process.dwProcessId);
    while (::WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
        if (timer.elapsed() > options.timeoutMs) {
            ::TerminateProcess(process.hProcess, 99);
            break;
        }
        if (options.tick)
            options.tick();
        QTest::qWait(2);
    }
    ::WaitForSingleObject(process.hProcess, INFINITE);
    outcome.ms = timer.elapsed();
    DWORD code = 0;
    ::GetExitCodeProcess(process.hProcess, &code);
    outcome.code = static_cast<int>(code);
    outReader.join();
    errReader.join();
    ::CloseHandle(outRead);
    ::CloseHandle(errRead);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return outcome;
}

// Runs `f` on a thread of its own while this one's event loop turns.
template <typename F>
auto offThread(F f) -> decltype(f())
{
    std::optional<decltype(f())> result;
    std::atomic<bool> done {false};
    std::thread worker([&] {
        result = f();
        done = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 30'000)
        QTest::qWait(5);
    worker.join();
    return *result;
}

// ---- Win顺's server, in this process ------------------------------------------

IndexService::Options walkedOptions(std::vector<std::wstring> folders)
{
    IndexService::Options options;
    options.content.enabled = false;
    options.volumeAccess = std::make_shared<ntfs::NoVolumeAccess>();
    options.rescanOnStartup = false;
    options.folders = std::move(folders);
    return options;
}

bool allReady(const IndexService& service)
{
    if (service.state() != IndexService::State::Ready)
        return false;
    for (const IndexService::FolderStatus& s : service.folderStatuses()) {
        if (s.state != IndexService::FolderStatus::State::Ready)
            return false;
    }
    return true;
}

// A folder of files, walked into a real index (chosen-folders mode: only that
// folder), served on a test pipe.
struct Served {
    QTemporaryDir dir;
    QTemporaryDir store;
    QString root; // native, the drive letter upper case
    std::unique_ptr<IndexService> service;
    std::unique_ptr<SearchEngine> engine;
    std::unique_ptr<mcp::ToolHost> host;
    std::unique_ptr<mcp::McpServer> server;
    std::wstring pipe;
    std::vector<mcp::ClipSummary> clips;
    bool ready = false;

    // `files` writes the files into the folder (its path) before the walk.
    // `start`: walk it (else the index stays as it is: never started).
    Served(const char* tag, const std::function<void(const QString&)>& files, bool start = true)
    {
        root = native(dir.path());
        if (files)
            files(root);
        service = std::make_unique<IndexService>(store.path(), walkedOptions({root.toStdWString()}));
        if (start) {
            service->start();
            QElapsedTimer timer;
            timer.start();
            while (!allReady(*service) && timer.elapsed() < 30'000)
                QTest::qWait(10);
            ready = allReady(*service);
        }
        engine = std::make_unique<SearchEngine>(service.get());
        mcp::Services services;
        services.index = service.get();
        services.engine = engine.get();
        services.version = u"0.0-test"_s;
        services.clipboard = [this](const mcp::ClipQuery& query) {
            std::vector<mcp::ClipSummary> out;
            for (const mcp::ClipSummary& c : clips) {
                if ((query.kind == u"all" || query.kind == c.kind) && (query.query.isEmpty() || c.text.contains(query.query))
                    && std::ssize(out) < query.limit) {
                    mcp::ClipSummary cut = c;
                    cut.text = c.text.left(query.maxChars);
                    out.push_back(cut);
                }
            }
            return out;
        };
        services.clipboardText = [this](qint64 id) -> std::optional<QString> {
            for (const mcp::ClipSummary& c : clips) {
                if (c.id == id)
                    return c.text;
            }
            return std::nullopt;
        };
        services.tempFolder = store.path() + u"/mcp-temp"_s;
        host = std::make_unique<mcp::ToolHost>(std::move(services));
        mcp::Policy policy;
        policy.contentExtensions = {u"txt"_s, u"md"_s, u"log"_s};
        policy.contentDocuments = false;
        policy.allowClipboard = true;
        host->setPolicy(policy);
        server = std::make_unique<mcp::McpServer>(host.get(), u"0.0-test"_s);
        pipe = testPipeName(tag);
    }
    ~Served()
    {
        server->stop();
        service->shutdown();
    }

    bool listen() { return server->listen(pipe); }
    QString path(const QString& relative) const { return root + u'\\' + QString(relative).replace(u'/', u'\\'); }
};

// The files most tests search.
void standardFiles(const QString& root)
{
    writeFile(root + u"\\报告𠀀.txt"_s, QByteArrayLiteral("第一行\n第三季度销售额 123 万\n"));
    writeFile(root + u"\\notes.md"_s, QByteArrayLiteral("meeting notes\nTODO later\n"));
    writeFile(root + u"\\with space\\a b.txt"_s, QByteArrayLiteral("spaces here\n"));
    writeFile(root + u"\\项目\\readme.md"_s, QByteArrayLiteral("# 项目说明\n"));
    QDir().mkpath(root + u"\\报告"_s);
    writeFile(root + u"\\sub\\deep\\c.txt"_s, QByteArrayLiteral("deep\n"));
    writeFile(root + u"\\recent.txt"_s, QByteArrayLiteral("r\n"));
    writeFile(root + u"\\old.log"_s, QByteArrayLiteral("old\n"));
    setModified(root + u"\\old.log"_s, 2020);
    writeFile(root + u"\\big.bin"_s, QByteArray(5000, 'x'));
}

// ---- raw servers ----------------------------------------------------------------

// A pipe server of its own, line by line, one connection after another:
// `answer` gets each line and gives what to write back (lines with their
// newlines, or nothing); nullopt ends that connection.
class FakeServer {
public:
    using Answer = std::function<std::optional<std::string>(const std::string& line)>;

    FakeServer(std::wstring name, Answer answer)
        : m_path(L"\\\\.\\pipe\\" + name)
        , m_answer(std::move(answer))
    {
        m_pipe = ::CreateNamedPipeW(m_path.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024,
            64 * 1024, 0, nullptr);
        if (m_pipe != INVALID_HANDLE_VALUE)
            m_thread = std::thread([this] { serve(); });
    }
    ~FakeServer()
    {
        m_stop = true;
        if (m_thread.joinable()) {
            const HANDLE thread = m_thread.native_handle();
            while (::WaitForSingleObject(thread, 20) == WAIT_TIMEOUT) {
                ::CancelSynchronousIo(thread);
                // A connection waited for: one comes and goes.
                const HANDLE poke = ::CreateFileW(m_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (poke != INVALID_HANDLE_VALUE)
                    ::CloseHandle(poke);
            }
            m_thread.join();
        }
        if (m_pipe != INVALID_HANDLE_VALUE)
            ::CloseHandle(m_pipe);
    }
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    bool valid() const { return m_pipe != INVALID_HANDLE_VALUE; }
    std::vector<std::string> lines() const
    {
        const std::lock_guard lock(m_mutex);
        return m_lines;
    }
    bool saw(const std::string& part) const
    {
        for (const std::string& line : lines()) {
            if (line.find(part) != std::string::npos)
                return true;
        }
        return false;
    }
    void forget()
    {
        const std::lock_guard lock(m_mutex);
        m_lines.clear();
    }

private:
    void serve()
    {
        while (!m_stop) {
            if (::ConnectNamedPipe(m_pipe, nullptr) || ::GetLastError() == ERROR_PIPE_CONNECTED)
                serveOne();
            else
                ::Sleep(1);
            ::DisconnectNamedPipe(m_pipe);
        }
    }

    void serveOne()
    {
        std::string buffer;
        char chunk[16 * 1024];
        while (!m_stop) {
            DWORD got = 0;
            if (!::ReadFile(m_pipe, chunk, sizeof chunk, &got, nullptr) || got == 0)
                return;
            buffer.append(chunk, got);
            for (std::size_t nl = buffer.find('\n'); nl != std::string::npos; nl = buffer.find('\n')) {
                const std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                {
                    const std::lock_guard lock(m_mutex);
                    m_lines.push_back(line);
                }
                const std::optional<std::string> reply = m_answer(line);
                if (!reply)
                    return;
                for (std::size_t at = 0; at < reply->size();) {
                    DWORD written = 0;
                    const DWORD part = static_cast<DWORD>(std::min<std::size_t>(reply->size() - at, 1u << 20));
                    if (!::WriteFile(m_pipe, reply->data() + at, part, &written, nullptr))
                        return;
                    at += written;
                }
            }
        }
    }

    std::wstring m_path;
    Answer m_answer;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::thread m_thread;
    std::atomic<bool> m_stop {false};
    mutable std::mutex m_mutex;
    std::vector<std::string> m_lines;
};

// A request line taken apart.
struct Request {
    long long id = -1; // -1: a notification
    std::string method;
    std::string tool; // tools/call's
    json::Value arguments;
};

Request requestOf(const std::string& line)
{
    Request request;
    const std::optional<json::Value> v = json::parse(line);
    if (!v)
        return request;
    request.id = (*v)["id"].toInt(-1);
    request.method = (*v)["method"].toString();
    request.tool = (*v)["params"]["name"].toString();
    request.arguments = (*v)["params"]["arguments"];
    return request;
}

std::string answerLine(long long id, json::Value result)
{
    return json::write(json::Value::Object {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}) + "\n";
}

// `cli`: announces that it was made for ws (capabilities.experimental.winshun.cli),
// as a Win顺 of the branch with the server changes does.
std::string initializeAnswer(long long id, bool cli = true)
{
    json::Value::Object capabilities;
    if (cli)
        capabilities.emplace("experimental", json::Value::Object {{"winshun", json::Value::Object {{"cli", 1}}}});
    return answerLine(id, json::Value::Object {{"protocolVersion", "2025-11-25"}, {"capabilities", std::move(capabilities)},
                              {"serverInfo", json::Value::Object {{"name", "fake"}, {"version", "9.9"}}}});
}

std::string toolAnswer(long long id, json::Value structured)
{
    return answerLine(id, json::Value::Object {{"content", json::Value::Array {}}, {"structuredContent", std::move(structured)}, {"isError", false}});
}

std::string readyAnswer(long long id)
{
    return toolAnswer(id, json::Value::Object {{"index", json::Value::Object {{"state", "ready"}, {"items", 1}, {"refreshing", false}}}});
}

// A refusal as a Win顺 made for ws sends it: the words, and its kind when it has one.
std::string refusalAnswer(long long id, const std::string& text, const std::string& code)
{
    json::Value::Object result {
        {"content", json::Value::Array {json::Value::Object {{"type", "text"}, {"text", text}}}}, {"isError", true}};
    if (!code.empty())
        result.emplace("structuredContent", json::Value::Object {{"error", code}});
    return answerLine(id, std::move(result));
}

QString fromCodePage(const QByteArray& bytes, UINT codePage)
{
    const int n = ::MultiByteToWideChar(codePage, 0, bytes.constData(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(codePage, 0, bytes.constData(), static_cast<int>(bytes.size()), w.data(), n);
    return QString::fromStdWString(w);
}

// Whether `codePage` writes Chinese and reads it back the same.
bool codePageHoldsChinese(UINT codePage)
{
    const std::wstring text = L"中文提示";
    const int n = ::WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<std::size_t>(std::max(n, 0)), '\0');
    ::WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), bytes.data(), n, nullptr, nullptr);
    return fromCodePage(QByteArray::fromStdString(bytes), codePage) == QString::fromStdWString(text);
}

// Every "--name" and "-x" a text mentions.
QStringList mentionedOptions(const QString& text)
{
    static const QRegularExpression option(u"(?<![\\w\\-])(--[a-z][a-z0-9-]*|-[a-zA-Z0-9])(?![\\w])"_s);
    QStringList found;
    for (auto it = option.globalMatch(text); it.hasNext();)
        found.push_back(it.next().captured(1));
    found.removeDuplicates();
    return found;
}

bool isKnownOption(const QString& written)
{
    for (const cli::OptionSpec& spec : cli::optionTable()) {
        if (spec.hidden)
            continue;
        if (written.startsWith(u"--"_s) && QString::fromStdWString(std::wstring(spec.name)) == written.mid(2))
            return true;
        if (!written.startsWith(u"--"_s) && written.size() == 2 && spec.letter != 0 && QChar(spec.letter) == written[1])
            return true;
    }
    return false;
}

// Whether the Win顺 on `pipe` (served by this process: asked from another thread) says
// in initialize that it was made for ws.
bool announcesCli(const std::wstring& pipe)
{
    return offThread([&] {
        pipe::LineClient client;
        if (!client.connect(pipe, 2000))
            return false;
        cli::Session session(client);
        const cli::Result<bool> made = session.hasCliContract();
        return made && *made;
    });
}

qint64 median(std::vector<qint64> values)
{
    std::ranges::sort(values);
    return values.empty() ? -1 : values[values.size() / 2];
}

} // namespace

class TestCli : public QObject {
    Q_OBJECT

    QTemporaryDir m_scratch;
    QString m_iniOn; // [Mcp] Enabled=true
    QString m_iniOff;
    QString m_marker; // WS_TEST_START writes it: "Win顺 started"
    QString m_opened; // WS_TEST_OPEN
    QString m_claude; // CLAUDE_CONFIG_DIR
    std::wstring m_mutex; // the test's single-instance mutex name (absent unless a test makes it)
    std::unique_ptr<Served> m_served; // the standard files, served
    // This tree's Win顺 announces that it was made for ws in initialize. Not yet,
    // until the branch with the server changes (cli-fix-server) is merged: then
    // ws would refuse --in of it, and the tests that use --in on it say so to ws
    // (WS_TEST_ASSUME_CLI) meanwhile. serverAnnouncesCliContract() stops skipping
    // once the merge makes it announce.
    bool m_serverHasCli = false;

    // ws with the test's pipe and mutex (first: a "--" in `args` must not
    // take them for words) and environment.
    Outcome ws(const QStringList& args, std::map<std::wstring, std::wstring> env = {}, const std::wstring& pipe = {}, RunOptions extra = {})
    {
        extra.args = QStringList {u"--pipe"_s, QString::fromStdWString(pipe.empty() ? m_served->pipe : pipe), u"--instance"_s,
                         QString::fromStdWString(m_mutex)}
            + args;
        std::map<std::wstring, std::wstring> all {{L"WS_SETTINGS", m_iniOn.toStdWString()}, {L"WS_LANG", L"zh"},
            {L"WS_TEST_START", m_marker.toStdWString()}, {L"WS_TEST_OPEN", m_opened.toStdWString()},
            {L"CLAUDE_CONFIG_DIR", m_claude.toStdWString()}, {L"NO_COLOR", L""}, {L"WS_ENCODING", L""},
            {L"WS_TEST_ELEVATED", L"0"}, {L"WS_TEST_VERIFY", L""}, {L"WS_TEST_IDLE_MS", L""}, {L"WS_TEST_ASSUME_CLI", L""}};
        if (!m_serverHasCli && pipe.empty())
            all[L"WS_TEST_ASSUME_CLI"] = L"1"; // this tree's Win顺 (the default pipe) cannot say it yet
        for (const auto& [name, value] : env)
            all[name] = value;
        extra.env = std::move(all);
        return runProcess(extra);
    }

    QString root() const { return m_served->root; }
    QString at(const QString& relative) const { return m_served->path(relative); }

private slots:
    void initTestCase()
    {
        QVERIFY(QFileInfo::exists(QStringLiteral(WINSHUN_CLI)));
        QVERIFY(m_scratch.isValid());
        m_iniOn = native(m_scratch.path() + u"/on.ini"_s);
        m_iniOff = native(m_scratch.path() + u"/off.ini"_s);
        QVERIFY(writeFile(m_iniOn, "[Appearance]\nLanguage=zh\n\n[Mcp]\nEnabled=true\n"));
        QVERIFY(writeFile(m_iniOff, "[Appearance]\nLanguage=zh\n\n[Mcp]\nEnabled=false\n"));
        m_marker = native(m_scratch.path() + u"/started.flag"_s);
        m_opened = native(m_scratch.path() + u"/opened.txt"_s);
        m_claude = native(m_scratch.path() + u"/claude"_s);
        m_mutex = L"Local\\" + testName("Instance", "main");
        m_served = std::make_unique<Served>("main", standardFiles);
        QVERIFY(m_served->ready);
        QVERIFY(m_served->listen());
        m_serverHasCli = announcesCli(m_served->pipe);
    }

    void cleanupTestCase() { m_served.reset(); }

    void init()
    {
        QFile::remove(m_marker);
        QFile::remove(m_opened);
        cli::setLanguage(cli::Lang::En);
    }

    // ---- the pieces ------------------------------------------------------------

    // Every option the help texts and SKILL.md mention is one ws has.
    void optionsInHelpExist()
    {
        const QString skillText = QString::fromUtf8(readAll(QStringLiteral(WINSHUN_CLI_SKILL)));
        QVERIFY(skillText.startsWith(u"---\nname: winshun\ndescription: "_s) || skillText.startsWith(u"---\r\nname: winshun\r\ndescription: "_s));
        QVERIFY2(skillText.count(u'\n') <= 60, "SKILL.md: about 60 lines at most");
        QStringList texts {skillText};
        for (const cli::Lang lang : {cli::Lang::Zh, cli::Lang::En}) {
            cli::setLanguage(lang);
            texts << QString::fromStdWString(cli::helpText(nullptr)) << QString::fromStdWString(cli::shortHelp());
            for (const cli::Command& command : cli::commandTable())
                texts << QString::fromStdWString(cli::helpText(&command));
        }
        int mentioned = 0;
        for (const QString& text : texts) {
            for (const QString& option : mentionedOptions(text)) {
                QVERIFY2(isKnownOption(option), qPrintable(u"%1, in:\n%2"_s.arg(option, text.left(300))));
                ++mentioned;
            }
        }
        QVERIFY(mentioned > 30);
        // Each command's own options are in its help.
        cli::setLanguage(cli::Lang::En);
        for (const cli::Command& command : cli::commandTable()) {
            const QString help = QString::fromStdWString(cli::helpText(&command));
            for (const cli::Opt id : command.options)
                QVERIFY(help.contains(u"--"_s + QString::fromStdWString(std::wstring(cli::optionSpec(id).name))));
        }
    }

    void parsing()
    {
        const auto parse = [](std::vector<std::wstring> args) {
            return cli::parseArgs(args, std::nullopt, cli::commandNamed(L"find")->options, L"find");
        };
        auto p = parse({L"-n20", L"x"});
        QVERIFY(p);
        QCOMPARE(std::wstring(p->value(cli::Opt::Limit)), std::wstring(L"20"));
        QCOMPARE(p->words, std::vector<std::wstring> {L"x"});
        p = parse({L"x", L"-n", L"20"});
        QVERIFY(p);
        QCOMPARE(std::wstring(p->value(cli::Opt::Limit)), std::wstring(L"20"));
        QCOMPARE(std::wstring(p->spelling(cli::Opt::Limit)), std::wstring(L"-n"));
        p = parse({L"--limit=20", L"x"});
        QVERIFY(p);
        QCOMPARE(std::wstring(p->value(cli::Opt::Limit)), std::wstring(L"20"));
        p = parse({L"x", L"-l0"});
        QVERIFY(p && p->has(cli::Opt::Long) && p->has(cli::Opt::Null));
        p = parse({L"--", L"-n", L"x"});
        QVERIFY(p && !p->has(cli::Opt::Limit));
        QCOMPARE(p->words, (std::vector<std::wstring> {L"-n", L"x"}));
        p = parse({L"/?"});
        QVERIFY(p && p->has(cli::Opt::Help));
        p = parse({L"--in", L"a", L"--in", L"b", L"x"});
        QVERIFY(p);
        QCOMPARE(p->all(cli::Opt::In), (std::vector<std::wstring> {L"a", L"b"}));
        p = parse({L"-5"}); // a negative number is a word
        QVERIFY(p);
        QCOMPARE(p->words, std::vector<std::wstring> {L"-5"});

        p = parse({L"--exts", L"pdf"});
        QVERIFY(!p);
        QCOMPARE(p.failure().code, cli::Exit::Usage);
        QVERIFY(p.failure().message.find(L"Did you mean --ext?") != std::wstring::npos);
        p = parse({L"--days", L"3", L"x"}); // recent's
        QVERIFY(!p);
        QVERIFY(p.failure().message.find(L"does not take --days") != std::wstring::npos);
        p = parse({L"-n"});
        QVERIFY(!p && p.failure().message.find(L"needs a value") != std::wstring::npos);
        p = parse({L"--json=1"});
        QVERIFY(!p && p.failure().message.find(L"takes no value") != std::wstring::npos);
        p = parse({L"--zzz"});
        QVERIFY(!p && p.failure().message == L"There is no option --zzz");

        // Where the command's name is.
        QCOMPARE(cli::firstWord(std::vector<std::wstring> {L"-n", L"5", L"recent"}), std::optional<std::size_t>(2));
        QCOMPARE(cli::firstWord(std::vector<std::wstring> {L"--", L"recent"}), std::optional<std::size_t>());
        QCOMPARE(cli::firstWord(std::vector<std::wstring> {L"--in", L".", L"报告"}), std::optional<std::size_t>(2));
        QCOMPARE(cli::firstWord(std::vector<std::wstring> {L"-n20", L"报告"}), std::optional<std::size_t>(1));
        QVERIFY(cli::commandNamed(L"RECENT") == cli::commandNamed(L"recent"));
        // Only the documented names: an alias would take a word from a query (ws dir listed instead of searching "dir").
        for (const wchar_t* alias : {L"search", L"read", L"dir", L"clipboard"})
            QVERIFY2(!cli::commandNamed(alias), qPrintable(QString::fromWCharArray(alias)));
        QStringList names;
        for (const cli::Command& command : cli::commandTable())
            names << QString::fromStdWString(std::wstring(command.name));
        QCOMPARE(names, (QStringList {u"find"_s, u"grep"_s, u"recent"_s, u"text"_s, u"ls"_s, u"open"_s, u"clip"_s, u"status"_s, u"skill"_s, u"help"_s}));
        QVERIFY(!cli::commandNamed(L"报告"));

        QCOMPARE(cli::editDistance(L"exts", L"ext"), std::size_t {1});
        QCOMPARE(cli::editDistance(L"jsno", L"json"), std::size_t {1});
        std::vector<cli::Opt> all;
        for (const cli::OptionSpec& spec : cli::optionTable())
            all.push_back(spec.id);
        QCOMPARE(cli::likeliest(L"colour", all), std::optional<cli::Opt>(cli::Opt::Color));
        QCOMPARE(cli::likeliest(L"fold", all), std::optional<cli::Opt>(cli::Opt::Folders));
        QCOMPARE(cli::likeliest(L"nostart", all), std::optional<cli::Opt>(cli::Opt::NoStart));
        QCOMPARE(cli::likeliest(L"q", all), std::optional<cli::Opt>());
        QCOMPARE(cli::likeliest(L"pipe", all), std::optional<cli::Opt>()); // hidden ones are never offered

        QCOMPARE(cli::parseDuration(L"10s"), std::optional<long long>(10'000));
        QCOMPARE(cli::parseDuration(L"500ms"), std::optional<long long>(500));
        QCOMPARE(cli::parseDuration(L"2m"), std::optional<long long>(120'000));
        QCOMPARE(cli::parseDuration(L"1.5s"), std::optional<long long>(1'500));
        QCOMPARE(cli::parseDuration(L"30"), std::optional<long long>(30'000));
        QCOMPARE(cli::parseDuration(L"0"), std::optional<long long>(0));
        QCOMPARE(cli::parseDuration(L"abc"), std::optional<long long>());
        QCOMPARE(cli::parseDuration(L"-1s"), std::optional<long long>());
        QCOMPARE(cli::parseDuration(L"10h"), std::optional<long long>());
    }

    void encoder()
    {
        std::string out;
        // GBK: the usual characters as they are, one outside it as one '?'.
        cli::Encoder gbk(936);
        QCOMPARE(gbk.append(out, L"D:\\报告\\周报.docx"), std::size_t {0});
        QCOMPARE(QByteArray::fromStdString(out), QByteArray("D:\\\xB1\xA8\xB8\xE6\\\xD6\xDC\xB1\xA8.docx"));
        out.clear();
        QCOMPARE(gbk.append(out, L"报告𠀀.txt"), std::size_t {1});
        QCOMPARE(QByteArray::fromStdString(out), QByteArray("\xB1\xA8\xB8\xE6?.txt"));
        out.clear();
        gbk.setJsonEscapes(true);
        QCOMPARE(gbk.append(out, L"报告𠀀"), std::size_t {0});
        QCOMPARE(QByteArray::fromStdString(out), QByteArray("\xB1\xA8\xB8\xE6\\ud840\\udc00"));
        // Western and DOS code pages.
        out.clear();
        cli::Encoder western(1252);
        QCOMPARE(western.append(out, L"é报"), std::size_t {1});
        QCOMPARE(QByteArray::fromStdString(out), QByteArray("\xE9?"));
        out.clear();
        cli::Encoder dos(437);
        QCOMPARE(dos.append(out, L"é报x"), std::size_t {1});
        QCOMPARE(QByteArray::fromStdString(out), QByteArray("\x82?x"));
        // UTF-8, UTF-16LE and GB18030 hold everything.
        out.clear();
        cli::Encoder utf8(CP_UTF8);
        QCOMPARE(utf8.append(out, L"报𠀀"), std::size_t {0});
        QCOMPARE(QByteArray::fromStdString(out), QByteArray("\xE6\x8A\xA5\xF0\xA0\x80\x80"));
        out.clear();
        cli::Encoder utf16(cli::kUtf16CodePage);
        QCOMPARE(utf16.append(out, L"a报"), std::size_t {0});
        QCOMPARE(QByteArray::fromStdString(out), QByteArray("a\0\xA5\x62", 4));
        out.clear();
        cli::Encoder gb18030(cli::kGb18030CodePage);
        QCOMPARE(gb18030.append(out, L"报𠀀"), std::size_t {0});
        QCOMPARE(fromCodePage(QByteArray::fromStdString(out), cli::kGb18030CodePage), u"报𠀀"_s);

        // Where redirected output goes in which code page.
        using cli::EncodingChoice;
        QCOMPARE(cli::redirectedCodePage(std::nullopt, false, 936), UINT {CP_UTF8});
        QCOMPARE(cli::redirectedCodePage(std::nullopt, true, 936), UINT {936});
        QCOMPARE(cli::redirectedCodePage(std::nullopt, true, 65001), UINT {CP_UTF8});
        QCOMPARE(cli::redirectedCodePage(std::nullopt, true, 0), UINT {CP_UTF8}); // no console
        QCOMPARE(cli::redirectedCodePage(EncodingChoice::Utf16, true, 936), cli::kUtf16CodePage);
        QCOMPARE(cli::redirectedCodePage(EncodingChoice::Oem, false, 936), ::GetOEMCP());
        QCOMPARE(cli::redirectedCodePage(EncodingChoice::Ansi, false, 936), ::GetACP());
        QCOMPARE(cli::redirectedCodePage(EncodingChoice::Console, false, 936), UINT {936});
        QCOMPARE(cli::encodingNamed(L"UTF-8"), std::optional<EncodingChoice>(EncodingChoice::Utf8));
        QCOMPARE(cli::encodingNamed(L"gbk"), std::optional<EncodingChoice>());
        qInfo().noquote() << u"This test's parent: %1"_s.arg(QString::fromStdWString(cli::parentImageName()));
    }

    void config()
    {
        cli::Config c = cli::parseConfig("[Appearance]\nLanguage=en\n\n[Mcp]\nEnabled=true\n");
        QVERIFY(c.connectionOn);
        QCOMPARE(c.language, std::wstring(L"en"));
        QVERIFY(!cli::parseConfig("[Mcp]\r\nEnabled=false\r\n").connectionOn);
        QVERIFY(!cli::parseConfig("[Mcp]\nEnabled=0\n").connectionOn);
        QVERIFY(!cli::parseConfig("[Mcp]\nEnabled=\n").connectionOn);
        QVERIFY(cli::parseConfig("[Mcp]\nEnabled=1\n").connectionOn);
        QVERIFY(cli::parseConfig("\xEF\xBB\xBF[mcp]\nenabled = TRUE\n").connectionOn);
        QVERIFY(!cli::parseConfig("[Other]\nEnabled=true\n").connectionOn); // another section's
        QVERIFY(!cli::parseConfig("").connectionOn); // off until the user turns it on
        QCOMPARE(cli::parseConfig("").language, std::wstring(L"system"));
        QCOMPARE(cli::languageNamed(L"zh-CN"), cli::Lang::Zh);
        QCOMPARE(cli::languageNamed(L"EN"), cli::Lang::En);
        // The file: WS_SETTINGS wins.
        ::SetEnvironmentVariableW(L"WS_SETTINGS", m_iniOff.toStdWString().c_str());
        const cli::Config read = cli::readConfig();
        ::SetEnvironmentVariableW(L"WS_SETTINGS", nullptr);
        QVERIFY(read.found);
        QVERIFY(!read.connectionOn);
        QCOMPARE(QString::fromStdWString(read.file), m_iniOff);
    }

    void format()
    {
        QCOMPARE(cli::humanSize(0), std::wstring(L"0 B"));
        QCOMPARE(cli::humanSize(1023), std::wstring(L"1023 B"));
        QCOMPARE(cli::humanSize(1536), std::wstring(L"1.5 KB"));
        QCOMPARE(cli::humanSize(5000), std::wstring(L"4.9 KB"));
        QCOMPARE(cli::humanSize(10 * 1024), std::wstring(L"10 KB"));
        QCOMPARE(cli::humanSize(3'650'722'201LL), std::wstring(L"3.4 GB"));
        QCOMPARE(cli::shortTime("2026-10-24T13:45:12+08:00"), std::wstring(L"2026-10-24 13:45"));
        QCOMPARE(cli::shortTime(""), std::wstring(L"-"));
        QCOMPARE(cli::thousands(3'312'345), std::wstring(L"3,312,345"));
        QCOMPARE(cli::thousands(12), std::wstring(L"12"));
        QVERIFY(cli::isUnder(L"C:\\a\\b", L"C:\\a"));
        QVERIFY(cli::isUnder(L"c:\\A\\b", L"C:\\a\\"));
        QVERIFY(cli::isUnder(L"C:\\a", L"C:\\a"));
        QVERIFY(!cli::isUnder(L"C:\\ab", L"C:\\a"));
        QVERIFY(cli::isUnder(L"C:\\x", L"C:\\"));
        QCOMPARE(cli::displayWidth(L"报告ab"), std::size_t {6});
        QCOMPARE(cli::displayWidth(L"𠀀"), std::size_t {2});
        const std::vector<std::wstring> terms = cli::queryTerms(L"报告 ext:pdf !草稿 \"年度 总结\" D:\\项目\\计划 *.psd a?c");
        QCOMPARE(terms, (std::vector<std::wstring> {L".psd", L"a", L"c", L"年度 总结", L"报告", L"计划"}));
        const auto ranges = cli::findTerms(L"年度报告.docx", std::vector<std::wstring> {L"报告"});
        QCOMPARE(ranges.size(), std::size_t {1});
        QCOMPARE(ranges.front(), (std::pair<std::size_t, std::size_t>(2, 4)));
        QCOMPARE(cli::findTerms(L"Report.PDF", std::vector<std::wstring> {L"report", L"pdf"}).size(), std::size_t {2});
        QCOMPARE(cli::findTerms(L"aaaa", std::vector<std::wstring> {L"aa", L"aaa"}).size(), std::size_t {1}); // merged
    }

    // The start decision, with fakes: nothing is started for real.
    void startDecision()
    {
        int starts = 0;
        int probes = 0;
        bool running = false;
        const std::wstring name = testPipeName("decide");
        const std::wstring pipePath = L"\\\\.\\pipe\\" + name;
        cli::Launcher launcher;
        launcher.running = [&] {
            ++probes;
            return running;
        };
        launcher.start = [&] {
            ++starts;
            return true;
        };
        cli::ConnectOptions options;
        options.pipe = name;
        options.switchOn = true;
        options.graceMs = 200;
        options.startWaitMs = 300;
        options.startingMutex = L"Local\\" + testName("Starting", "decide");

        // The pipe there: connected, nothing asked; it says more than the settings file.
        {
            const HANDLE pipe = ::CreateNamedPipeW(pipePath.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, 1, 4096, 4096, 0, nullptr);
            QVERIFY(pipe != INVALID_HANDLE_VALUE);
            pipe::LineClient client;
            options.switchOn = false;
            QVERIFY(cli::connectToWinShun(client, options, launcher));
            QCOMPARE(probes, 0);
            QCOMPARE(starts, 0);
            client.close();
            ::CloseHandle(pipe);
            options.switchOn = true;
        }
        // Switch off: 4, never started.
        {
            pipe::LineClient client;
            options.switchOn = false;
            const auto r = cli::connectToWinShun(client, options, launcher);
            QVERIFY(!r);
            QCOMPARE(r.failure().code, cli::Exit::Off);
            QCOMPARE(starts, 0);
            options.switchOn = true;
        }
        // Running without the pipe: 4 after the grace, never started (the task would only pop the launcher up).
        {
            pipe::LineClient client;
            running = true;
            QElapsedTimer timer;
            timer.start();
            const auto r = cli::connectToWinShun(client, options, launcher);
            QVERIFY(!r);
            QCOMPARE(r.failure().code, cli::Exit::Off);
            QVERIFY(timer.elapsed() >= 150);
            QCOMPARE(starts, 0);
            running = false;
        }
        // Running without the pipe while another ws is starting it (its mutex): waits for the
        // start, not just the grace; the pipe comes meanwhile: connected, and not started again.
        {
            options.graceMs = 100;
            options.startWaitMs = 1500;
            const HANDLE other = ::CreateMutexW(nullptr, FALSE, options.startingMutex.c_str());
            running = true;
            HANDLE pipe = INVALID_HANDLE_VALUE;
            std::thread comes([&] {
                ::Sleep(400);
                pipe = ::CreateNamedPipeW(pipePath.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, 1, 4096, 4096, 0, nullptr);
            });
            std::vector<std::wstring> notes;
            options.note = [&](std::wstring_view text) { notes.emplace_back(text); };
            pipe::LineClient client;
            QElapsedTimer timer;
            timer.start();
            const auto r = cli::connectToWinShun(client, options, launcher);
            comes.join();
            QVERIFY(r);
            QVERIFY(timer.elapsed() >= 350);
            QCOMPARE(starts, 0);
            QCOMPARE(notes.size(), std::size_t {1}); // "starting, waiting for it"
            client.close();
            ::CloseHandle(pipe);
            // No pipe coming: the whole start-up waited for, then 4.
            options.startWaitMs = 600;
            options.note = nullptr;
            timer.restart();
            const auto none = cli::connectToWinShun(client, options, launcher);
            QVERIFY(!none);
            QCOMPARE(none.failure().code, cli::Exit::Off);
            QVERIFY(timer.elapsed() >= 550);
            ::CloseHandle(other);
            running = false;
            options.graceMs = 200;
            options.startWaitMs = 300;
        }
        // Running without the pipe, its process moments old: waits what is left of freshMs
        // (the pipe comes after the window, at logon after a good deal else); an old one, not.
        {
            options.graceMs = 100;
            options.freshMs = 1200;
            running = true;
            launcher.runningFor = [] { return std::optional<unsigned long long>(200); }; // 1000 ms left
            HANDLE pipe = INVALID_HANDLE_VALUE;
            std::thread comes([&] {
                ::Sleep(400);
                pipe = ::CreateNamedPipeW(pipePath.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, 1, 4096, 4096, 0, nullptr);
            });
            pipe::LineClient client;
            QElapsedTimer timer;
            timer.start();
            const auto r = cli::connectToWinShun(client, options, launcher);
            comes.join();
            QVERIFY(r);
            QVERIFY(timer.elapsed() >= 350);
            client.close();
            ::CloseHandle(pipe);
            launcher.runningFor = [] { return std::optional<unsigned long long>(60'000); };
            timer.restart();
            const auto old = cli::connectToWinShun(client, options, launcher);
            QVERIFY(!old);
            QCOMPARE(old.failure().code, cli::Exit::Off);
            QVERIFY(timer.elapsed() < 600);
            launcher.runningFor = [] { return std::optional<unsigned long long>(); }; // cannot be told: the grace only
            timer.restart();
            QVERIFY(!cli::connectToWinShun(client, options, launcher));
            QVERIFY(timer.elapsed() < 600);
            launcher.runningFor = nullptr;
            running = false;
            options.graceMs = 200;
            options.freshMs = 8000;
        }
        // The same waiting for status (connectIfThere), which never starts Win顺.
        {
            options.graceMs = 100;
            options.startWaitMs = 1500;
            const HANDLE other = ::CreateMutexW(nullptr, FALSE, options.startingMutex.c_str());
            running = true;
            HANDLE pipe = INVALID_HANDLE_VALUE;
            std::thread comes([&] {
                ::Sleep(400);
                pipe = ::CreateNamedPipeW(pipePath.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, 1, 4096, 4096, 0, nullptr);
            });
            pipe::LineClient client;
            const auto there = cli::connectIfThere(client, options, launcher);
            comes.join();
            QVERIFY(there);
            QVERIFY(*there);
            QCOMPARE(starts, 0);
            client.close();
            ::CloseHandle(pipe);
            ::CloseHandle(other);
            // Not running (or the switch off): at once, false, never started.
            running = false;
            QElapsedTimer timer;
            timer.start();
            const auto absent = cli::connectIfThere(client, options, launcher);
            QVERIFY(absent);
            QVERIFY(!*absent);
            QVERIFY(timer.elapsed() < 300);
            running = true;
            options.switchOn = false;
            const auto off = cli::connectIfThere(client, options, launcher);
            QVERIFY(off && !*off);
            QVERIFY(timer.elapsed() < 300);
            options.switchOn = true;
            running = false;
            QCOMPARE(starts, 0);
            options.graceMs = 200;
            options.startWaitMs = 300;
        }
        // Not running, --no-start: 3.
        {
            pipe::LineClient client;
            options.mayStart = false;
            const auto r = cli::connectToWinShun(client, options, launcher);
            QVERIFY(!r);
            QCOMPARE(r.failure().code, cli::Exit::NotRunning);
            QCOMPARE(starts, 0);
            options.mayStart = true;
        }
        // Not running: started once; its pipe comes; connected.
        {
            pipe::LineClient client;
            HANDLE pipe = INVALID_HANDLE_VALUE;
            launcher.start = [&] {
                ++starts;
                pipe = ::CreateNamedPipeW(pipePath.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, 1, 4096, 4096, 0, nullptr);
                return true;
            };
            std::vector<std::wstring> notes;
            options.note = [&](std::wstring_view text) { notes.emplace_back(text); };
            QVERIFY(cli::connectToWinShun(client, options, launcher));
            QCOMPARE(starts, 1);
            QCOMPARE(notes.size(), std::size_t {1});
            client.close();
            ::CloseHandle(pipe);
            options.note = nullptr;
        }
        // Started, but no pipe comes: 3.
        {
            pipe::LineClient client;
            launcher.start = [&] {
                ++starts;
                return true;
            };
            const auto r = cli::connectToWinShun(client, options, launcher);
            QVERIFY(!r);
            QCOMPARE(r.failure().code, cli::Exit::NotRunning);
            QCOMPARE(starts, 2);
        }
        // Cannot be started: 3 at once.
        {
            pipe::LineClient client;
            launcher.start = [&] {
                ++starts;
                return false;
            };
            QElapsedTimer timer;
            timer.start();
            const auto r = cli::connectToWinShun(client, options, launcher);
            QVERIFY(!r);
            QCOMPARE(r.failure().code, cli::Exit::NotRunning);
            QVERIFY(timer.elapsed() < 250);
        }
        // Another ws is starting it: this one only waits.
        {
            const HANDLE other = ::CreateMutexW(nullptr, FALSE, options.startingMutex.c_str());
            pipe::LineClient client;
            const int before = starts;
            const auto r = cli::connectToWinShun(client, options, launcher);
            QVERIFY(!r);
            QCOMPARE(starts, before);
            ::CloseHandle(other);
        }
    }

    // ---- ws.exe against Win顺 ---------------------------------------------------

    void findPlain()
    {
        const Outcome r = ws({u"find"_s, u"报告"_s});
        QCOMPARE(r.code, 0);
        QVERIFY2(r.notes().isEmpty(), r.err.constData());
        QVERIFY(!r.out.contains('\x1b')); // no colours when redirected
        QVERIFY(!r.out.contains('\r')); // LF
        QVERIFY(r.out.endsWith('\n'));
        QStringList lines = r.lines();
        lines.sort();
        QStringList expected {at(u"报告"_s), at(u"报告𠀀.txt"_s)};
        expected.sort();
        QCOMPARE(lines, expected);
        // UTF-8 bytes (no BOM).
        QVERIFY(r.out.contains(at(u"报告𠀀.txt"_s).toUtf8()));
        QVERIFY(!r.out.startsWith("\xEF\xBB\xBF"));
        // find is the default command.
        const Outcome same = ws({u"报告"_s});
        QCOMPARE(same.code, 0);
        QCOMPARE(same.out, r.out);
    }

    void findFormats()
    {
        // --json: one array of path, type, modified; the size only with -l.
        Outcome r = ws({u"find"_s, u"报告"_s, u"--json"_s});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.out.count('\n'), 1);
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(r.out, &error);
        QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(error.errorString()));
        QVERIFY(doc.isArray());
        QCOMPARE(doc.array().size(), 2);
        for (const QJsonValue& v : doc.array()) {
            const QJsonObject item = v.toObject();
            QVERIFY(item.contains(u"path"_s) && item.contains(u"type"_s) && item.contains(u"modified"_s));
            QVERIFY(!item.contains(u"size"_s));
            QVERIFY(!item.contains(u"name"_s)); // the same from every Win顺
        }
        r = ws({u"find"_s, u"big"_s, u"--json"_s, u"-l"_s});
        doc = QJsonDocument::fromJson(r.out);
        QCOMPARE(doc.array().first().toObject().value(u"size"_s).toInteger(), 5000);

        // -0: NUL after each path.
        r = ws({u"find"_s, u"报告"_s, u"-0"_s});
        QCOMPARE(r.code, 0);
        QVERIFY(r.out.endsWith('\0'));
        QCOMPARE(r.out.count('\0'), 2);
        QVERIFY(!r.out.contains('\n'));

        // -l: size, time, path, lined up.
        r = ws({u"find"_s, u"big"_s, u"-l"_s});
        QCOMPARE(r.code, 0);
        static const QRegularExpression longLine(u"^4\\.9 KB  \\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d  (.+)$"_s);
        const QRegularExpressionMatch m = longLine.match(r.lines().value(0));
        QVERIFY2(m.hasMatch(), qPrintable(r.text()));
        QCOMPARE(m.captured(1), at(u"big.bin"_s));
        r = ws({u"find"_s, u"报告"_s, u"-l"_s, u"--folders"_s});
        QVERIFY2(r.text().startsWith(u"-  "_s), qPrintable(r.text())); // a folder has no size

        // Colours when asked: the folder dim, the query's words marked.
        r = ws({u"find"_s, u"报告"_s, u"--files"_s, u"--color"_s, u"always"_s});
        QCOMPARE(r.code, 0);
        QVERIFY(r.out.contains("\x1b[2m"));
        QVERIFY(r.out.contains(QByteArray("\x1b[1;31m") + "报告" + "\x1b[0m"));
    }

    void findNarrowing()
    {
        // Nothing: 1, nothing on stdout.
        Outcome r = ws({u"find"_s, u"zzqqxx"_s});
        QCOMPARE(r.code, 1);
        QVERIFY(r.out.isEmpty());
        // --ext, --files, --folders.
        r = ws({u"find"_s, u"报告"_s, u"--ext"_s, u".txt"_s});
        QCOMPARE(r.lines(), QStringList {at(u"报告𠀀.txt"_s)});
        r = ws({u"find"_s, u"报告"_s, u"--folders"_s});
        QCOMPARE(r.lines(), QStringList {at(u"报告"_s)});
        r = ws({u"find"_s, u"报告"_s, u"--files"_s, u"--folders"_s});
        QCOMPARE(r.code, 2);
        // --in, also relative to the current folder: only what is inside
        // (a Win顺 before ws ignores it and ws keeps to it itself).
        r = ws({u"find"_s, u"c"_s, u"--in"_s, at(u"sub"_s)});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.lines(), QStringList {at(u"sub\\deep\\c.txt"_s)});
        RunOptions inSub;
        inSub.folder = at(u"sub"_s);
        r = ws({u"find"_s, u"c"_s, u"--in"_s, u"."_s}, {}, {}, inSub);
        QCOMPARE(r.lines(), QStringList {at(u"sub\\deep\\c.txt"_s)});
        r = ws({u"find"_s, u"c"_s, u"--in"_s, at(u"nowhere"_s)});
        QCOMPARE(r.code, 6);
        // A file for --in: the wrong thing named (as ws ls <file>), not "no such folder".
        r = ws({u"find"_s, u"c"_s, u"--in"_s, at(u"notes.md"_s)});
        QCOMPARE(r.code, 2);
        QVERIFY2(r.errors().contains(u"这是文件，不是文件夹："_s + at(u"notes.md"_s)), qPrintable(r.errors()));
        // -n: at most that many, and a note.
        r = ws({u"find"_s, u"t"_s, u"-n"_s, u"1"_s});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.lines().size(), 1);
        QVERIFY2(r.errors().contains(u"只列了前 1 个"_s), qPrintable(r.errors()));
        QVERIFY(r.errors().startsWith(u"ws: "_s));
    }

    void grep()
    {
        Outcome r = ws({u"grep"_s, u"销售额"_s});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.text(), at(u"报告𠀀.txt"_s) + u":2:第三季度销售额 123 万\n"_s);
        r = ws({u"grep"_s, u"销售额"_s, u"--json"_s});
        const QJsonArray items = QJsonDocument::fromJson(r.out).array();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().toObject().value(u"location"_s).toObject().value(u"line"_s).toInt(), 2);
        QVERIFY(!items.first().toObject().contains(u"name"_s));
        r = ws({u"grep"_s, u"deep"_s, u"--in"_s, at(u"sub"_s), u"-0"_s});
        QCOMPARE(r.out, at(u"sub\\deep\\c.txt"_s).toUtf8() + '\0');
        r = ws({u"grep"_s, u"zzqqxx"_s});
        QCOMPARE(r.code, 1);
        r = ws({u"grep"_s});
        QCOMPARE(r.code, 2);
    }

    void text()
    {
        const QString file = at(u"报告𠀀.txt"_s);
        Outcome r = ws({u"text"_s, file});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.out, QByteArray("第一行\n第三季度销售额 123 万\n"));
        QVERIFY(r.err.isEmpty());
        // In parts: --max, then --from where ws said.
        r = ws({u"text"_s, file, u"--max"_s, u"3"_s});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.out, QByteArray("第一行"));
        QVERIFY2(r.errors().contains(u"--from 3"_s), qPrintable(r.errors()));
        r = ws({u"text"_s, file, u"--from"_s, u"3"_s});
        QCOMPARE(r.out, QByteArray("\n第三季度销售额 123 万\n"));
        r = ws({u"text"_s, file, u"--json"_s});
        const QJsonObject object = QJsonDocument::fromJson(r.out).object();
        QCOMPARE(object.value(u"text"_s).toString(), u"第一行\n第三季度销售额 123 万\n"_s);
        QCOMPARE(object.value(u"chars"_s).toInt(), 18);
        // Relative to the current folder.
        RunOptions inRoot;
        inRoot.folder = root();
        r = ws({u"text"_s, u"notes.md"_s}, {}, {}, inRoot);
        QCOMPARE(r.out, QByteArray("meeting notes\nTODO later\n"));
        // Not there: 6; a folder: 2 (ws sees it is one); nothing: 2.
        r = ws({u"text"_s, at(u"missing.txt"_s)});
        QCOMPARE(r.code, 6);
        QVERIFY(r.errors().contains(at(u"missing.txt"_s)));
        r = ws({u"text"_s, at(u"sub"_s)});
        QCOMPARE(r.code, 2);
        QVERIFY(r.errors().contains(u"ws ls"_s));
        r = ws({u"text"_s});
        QCOMPARE(r.code, 2);
        // --from beyond the end: no text, exit 1, and a note saying why.
        r = ws({u"text"_s, file, u"--from"_s, u"1000"_s});
        QCOMPARE(r.code, 1);
        QVERIFY(r.out.isEmpty());
        QVERIFY2(r.errors().contains(u"已经读完"_s) && r.errors().contains(u"18"_s) && r.errors().contains(u"1000"_s), qPrintable(r.errors()));
        r = ws({u"text"_s, file, u"--from"_s, u"18"_s});
        QCOMPARE(r.code, 1);
        QVERIFY2(r.errors().contains(u"已经读完"_s), qPrintable(r.errors()));
    }

    void ls()
    {
        Outcome r = ws({u"ls"_s, root()});
        QCOMPARE(r.code, 0);
        const QStringList lines = r.lines();
        QCOMPARE(lines.size(), 9);
        // Folders first, then files.
        QVERIFY(lines.contains(at(u"项目"_s)));
        QVERIFY(lines.contains(at(u"notes.md"_s)));
        QVERIFY(lines.indexOf(at(u"项目"_s)) < lines.indexOf(at(u"notes.md"_s)));
        r = ws({u"ls"_s, root(), u"--files"_s, u"-n"_s, u"2"_s});
        QCOMPARE(r.lines().size(), 2);
        QVERIFY2(r.errors().contains(u"共 5 个"_s), qPrintable(r.errors()));
        RunOptions inSub;
        inSub.folder = at(u"sub"_s);
        r = ws({u"ls"_s}, {}, {}, inSub);
        QCOMPARE(r.lines(), QStringList {at(u"sub\\deep"_s)});
        r = ws({u"ls"_s, at(u"notes.md"_s)});
        QCOMPARE(r.code, 2);
        r = ws({u"ls"_s, at(u"nowhere"_s)});
        QCOMPARE(r.code, 6);
    }

    // The test's folder is under %TEMP%, where Win顺 leaves everything out of
    // recent_files (a tool folder): each of these is asked twice, the second
    // time with those (recentHiddenPlaces has the asking).
    void recent()
    {
        Outcome r = ws({u"recent"_s, u"--in"_s, root(), u"--days"_s, u"7"_s});
        QCOMPARE(r.code, 0);
        const QStringList lines = r.lines();
        QVERIFY(lines.contains(at(u"报告𠀀.txt"_s)));
        QVERIFY(!lines.contains(at(u"old.log"_s)));
        r = ws({u"recent"_s, u"--in"_s, root(), u"--ext"_s, u"log"_s});
        QCOMPARE(r.lines(), QStringList {at(u"old.log"_s)});
        r = ws({u"recent"_s, u"报告"_s});
        QCOMPARE(r.code, 2);
        r = ws({u"recent"_s, u"--in"_s, root(), u"--in"_s, at(u"sub"_s)});
        QCOMPARE(r.code, 2);
    }

    // Paths ws looks at itself: short and long.
    void pathsAsWsSeesThem()
    {
        // 8.3 short names are the long ones the index knows (C:\Users\ADMINI~1 for a user with a long name).
        const QString longName = native(m_scratch.path() + u"/A Rather Long Folder Name"_s);
        QVERIFY(QDir().mkpath(longName));
        QVERIFY(writeFile(longName + u"\\file with a long name.txt"_s, "x"));
        wchar_t shortForm[MAX_PATH] {};
        const DWORD n = ::GetShortPathNameW(longName.toStdWString().c_str(), shortForm, MAX_PATH);
        if (n > 0 && n < MAX_PATH && QString::fromWCharArray(shortForm).contains(u'~')) {
            const QString shortPath = QString::fromWCharArray(shortForm);
            QCOMPARE(QString::fromStdWString(cli::absolutePath(shortPath.toStdWString())).toLower(), longName.toLower());
            // Relative to a current folder that is a short name too.
            QCOMPARE(QString::fromStdWString(cli::absolutePath((shortPath + u"\\.").toStdWString())).toLower(), longName.toLower());
            // One that is not there stays as written (nothing to ask the disk).
            QCOMPARE(QString::fromStdWString(cli::absolutePath(L"X:\\nowhere~1\\x")), u"X:\\nowhere~1\\x"_s);
        } else {
            qInfo("this volume keeps no 8.3 names: the expansion is not tried");
        }
        // A path over MAX_PATH is seen: exists, a folder, a file (the \\?\ form for the local checks).
        QString deep = native(m_scratch.path());
        QString extended = u"\\\\?\\"_s + deep; // the prefix: the only form that can make them
        while (deep.size() < 300) {
            const QString part = u"\\a-folder-with-a-longish-name"_s;
            deep += part;
            extended += part;
            QVERIFY2(::CreateDirectoryW(extended.toStdWString().c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS, "cannot make the deep folder");
        }
        const HANDLE file = ::CreateFileW((extended + u"\\deep.txt"_s).toStdWString().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(file != INVALID_HANDLE_VALUE);
        ::CloseHandle(file);
        QVERIFY(deep.size() > 260);
        QVERIFY(cli::pathExists(deep.toStdWString()));
        QVERIFY(cli::pathKind(deep.toStdWString()) == cli::PathKind::Folder);
        QVERIFY(cli::pathKind((deep + u"\\deep.txt"_s).toStdWString()) == cli::PathKind::File);
        QVERIFY(cli::pathKind((deep + u"\\missing.txt"_s).toStdWString()) == cli::PathKind::None);
        QCOMPARE(QString::fromStdWString(cli::extendedPath(L"C:\\x")), u"C:\\x"_s); // short ones as they are
        QVERIFY(QString::fromStdWString(cli::extendedPath(deep.toStdWString())).startsWith(u"\\\\?\\"_s));
        QVERIFY(QString::fromStdWString(cli::extendedPath((u"\\\\server\\share\\"_s + QString(300, u'x')).toStdWString())).startsWith(u"\\\\?\\UNC\\server"_s));
        // `ws open` takes a long path that is there as the path it is (it fell back to a name search).
        const Outcome r = ws({u"open"_s, deep + u"\\deep.txt"_s}, {}, testPipeName("nobody"));
        QCOMPARE(r.code, 0);
        QCOMPARE(QString::fromUtf8(readAll(m_opened)), u"open "_s + deep + u"\\deep.txt\n"_s);
    }

    // cmd and PowerShell 5.1 hand "C:\My Folder\" -n 5 over as one argument, the quote gone and the
    // rest in the path: said, not looked for as a folder of that name.
    void aQuoteInAPath()
    {
        const QString swallowed = at(u"with space"_s) + u"\\\" -n 5"_s;
        for (const QStringList& command : {QStringList {u"find"_s, u"a"_s, u"--in"_s, swallowed}, QStringList {u"grep"_s, u"a"_s, u"--in"_s, swallowed},
                 QStringList {u"recent"_s, u"--in"_s, swallowed}, QStringList {u"ls"_s, swallowed}, QStringList {u"text"_s, swallowed}}) {
            const Outcome r = ws(command);
            QVERIFY2(r.code == 2, qPrintable(command.join(u' ') + u": "_s + r.errors()));
            QVERIFY2(r.errors().contains(u"路径里有引号"_s) && r.errors().contains(u"反斜杠"_s), qPrintable(r.errors()));
            QVERIFY(r.out.isEmpty());
        }
        const Outcome en = ws({u"ls"_s, swallowed}, {{L"WS_LANG", L"en"}});
        QCOMPARE(en.code, 2);
        QVERIFY2(en.errors().contains(u"trailing backslash"_s), qPrintable(en.errors()));
        // A quote in a query is the query's own business (a phrase).
        QCOMPARE(ws({u"find"_s, u"\"报告\""_s}).code, 0);
        // ... and in open's words, which may be a query.
        QCOMPARE(ws({u"open"_s, u"\"报告\""_s}).code, 0);
    }

    // The options every command takes are the ones it can honour: status neither waits for the
    // index nor starts Win顺.
    void statusTakesNoWaitOrNoStart()
    {
        for (const QString& language : {u"zh"_s, u"en"_s}) {
            const std::map<std::wstring, std::wstring> lang {{L"WS_LANG", language.toStdWString()}};
            Outcome r = ws({u"help"_s, u"status"_s}, lang);
            QCOMPARE(r.code, 0);
            // Their rows (the details of status name them, saying it has none).
            QVERIFY2(!r.text().contains(u"--wait <"_s) && !r.text().contains(u"不启动它"_s) && !r.text().contains(u"Do not start WinShun"_s), qPrintable(r.text()));
            QVERIFY2(r.text().contains(u"--color"_s) && r.text().contains(u"--encoding"_s) && r.text().contains(u"--json"_s), qPrintable(r.text()));
            r = ws({u"help"_s, u"find"_s}, lang);
            QVERIFY2(r.text().contains(u"--no-start"_s) && r.text().contains(u"--wait <"_s), qPrintable(r.text()));
        }
        Outcome r = ws({u"status"_s, u"--wait"_s, u"5s"_s});
        QCOMPARE(r.code, 2);
        QVERIFY2(r.errors().contains(u"ws status 不认 --wait"_s), qPrintable(r.errors()));
        r = ws({u"status"_s, u"--no-start"_s});
        QCOMPARE(r.code, 2);
        QCOMPARE(ws({u"status"_s}).code, 0);
        QCOMPARE(ws({u"status"_s, u"--json"_s, u"--color"_s, u"never"_s}).code, 0);
        // The first word `dir`, `read`, `search`, `clipboard` is a query, as any other.
        const std::wstring name = testPipeName("aliases");
        FakeServer fake(name, [&](const std::string& line) -> std::optional<std::string> {
            const Request request = requestOf(line);
            if (request.method == "initialize")
                return initializeAnswer(request.id);
            if (request.tool == "index_status")
                return readyAnswer(request.id);
            if (request.tool == "search_files")
                return toolAnswer(request.id, json::Value::Object {{"items", json::Value::Array {}}, {"returned", 0}, {"truncated", false}});
            return std::string();
        });
        for (const char* word : {"dir", "read", "search", "clipboard"}) {
            fake.forget();
            r = ws({QString::fromLatin1(word)}, {}, name);
            QCOMPARE(r.code, 1);
            QVERIFY2(fake.saw(std::string("\"query\":\"") + word + "\""), word);
        }
    }

    // A first word that names a command is the command; -- or find makes it a query.
    void reservedWords()
    {
        Outcome r = ws({u"--"_s, u"recent"_s});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.lines(), QStringList {at(u"recent.txt"_s)});
        r = ws({u"find"_s, u"recent"_s});
        QCOMPARE(r.lines(), QStringList {at(u"recent.txt"_s)});
        r = ws({u"recent"_s, u"--in"_s, root(), u"--ext"_s, u"txt"_s});
        QVERIFY(r.lines().size() > 1); // the command
    }

    void open()
    {
        // Found by name: opened (here: written down), its path printed.
        Outcome r = ws({u"open"_s, u"报告"_s, u"--ext"_s, u"txt"_s});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.lines(), QStringList {at(u"报告𠀀.txt"_s)});
        QCOMPARE(QString::fromUtf8(readAll(m_opened)), u"open "_s + at(u"报告𠀀.txt"_s) + u'\n');
        QFile::remove(m_opened);
        // A path: as it is, Win顺 not asked (a pipe nobody serves).
        r = ws({u"open"_s, at(u"notes.md"_s), u"--reveal"_s}, {}, testPipeName("nobody"));
        QCOMPARE(r.code, 0);
        QCOMPARE(QString::fromUtf8(readAll(m_opened)), u"reveal "_s + at(u"notes.md"_s) + u'\n');
        r = ws({u"open"_s, u"zzqqxx"_s});
        QCOMPARE(r.code, 1);
        QFile::remove(m_opened);

        // ws as administrator: a file a search found is handed to Explorer (the user's rights),
        // not started with ws's own token; a path the user named still is opened directly; reveal
        // only selects.
        const std::map<std::wstring, std::wstring> elevated {{L"WS_TEST_ELEVATED", L"1"}};
        r = ws({u"open"_s, u"报告"_s, u"--ext"_s, u"txt"_s}, elevated);
        QCOMPARE(r.code, 0);
        QCOMPARE(QString::fromUtf8(readAll(m_opened)), u"user-open "_s + at(u"报告𠀀.txt"_s) + u'\n');
        QFile::remove(m_opened);
        r = ws({u"open"_s, at(u"notes.md"_s)}, elevated, testPipeName("nobody"));
        QCOMPARE(QString::fromUtf8(readAll(m_opened)), u"open "_s + at(u"notes.md"_s) + u'\n');
        QFile::remove(m_opened);
        r = ws({u"open"_s, u"报告"_s, u"--ext"_s, u"txt"_s, u"--reveal"_s}, elevated);
        QCOMPARE(QString::fromUtf8(readAll(m_opened)), u"reveal "_s + at(u"报告𠀀.txt"_s) + u'\n');
        QFile::remove(m_opened);

        // The pipe's other end must be Win顺 (any program can create the pipe of that name first):
        // here the server is this test, so a check asked for refuses, and nothing is opened.
        r = ws({u"open"_s, u"报告"_s, u"--ext"_s, u"txt"_s}, {{L"WS_TEST_VERIFY", L"1"}});
        QCOMPARE(r.code, 7);
        QVERIFY2(r.errors().contains(u"不是 Win顺"_s), qPrintable(r.errors()));
        QVERIFY(r.out.isEmpty());
        QVERIFY(!QFileInfo::exists(m_opened));
        // find does not need it (nothing is acted on).
        QCOMPARE(ws({u"find"_s, u"报告"_s}, {{L"WS_TEST_VERIFY", L"1"}}).code, 0);
    }

    // Win顺's own program, as the check tells it from another.
    void winShunImage()
    {
        const std::wstring wsExe = L"C:\\Program Files\\WinShun\\bin\\ws.exe";
        QVERIFY(cli::isWinShunImage(L"C:\\Program Files\\WinShun\\WinShun.exe", wsExe, L""));
        QVERIFY(cli::isWinShunImage(L"c:\\program files\\winshun\\WINSHUN.EXE", wsExe, L""));
        QVERIFY(!cli::isWinShunImage(L"C:\\Users\\x\\Evil\\WinShun.exe", wsExe, L""));
        QVERIFY(!cli::isWinShunImage(L"C:\\Program Files\\WinShun\\Other.exe", wsExe, L""));
        QVERIFY(!cli::isWinShunImage(L"C:\\Program Files\\WinShun\\bin\\WinShun.exe", wsExe, L"")); // bin\ holds ws alone
        QVERIFY(!cli::isWinShunImage(L"WinShun.exe", wsExe, L""));
        QVERIFY(!cli::isWinShunImage({}, wsExe, L""));
        // A copy of ws.exe elsewhere: the installed one counts (the uninstall entry's folder, any slash).
        const std::wstring copied = L"C:\\Tools\\ws.exe";
        QVERIFY(!cli::isWinShunImage(L"D:\\Apps\\WinShun\\WinShun.exe", copied, L""));
        QVERIFY(cli::isWinShunImage(L"D:\\Apps\\WinShun\\WinShun.exe", copied, L"D:\\Apps\\WinShun\\"));
        QVERIFY(!cli::isWinShunImage(L"D:\\Other\\WinShun.exe", copied, L"D:\\Apps\\WinShun\\"));
        // The real one is a folder of this machine, or nothing.
        const std::wstring installed = cli::installedFolder();
        QVERIFY(installed.empty() || installed.find(L'\\') != std::wstring::npos);
    }

    void clip()
    {
        m_served->clips = {
            {1, u"text"_s, u"第一行\n第二行"_s, 7, 1'700'000'000'000, 1'700'000'000'000, {}, {}, false, 0, 0},
            {2, u"link"_s, u"https://example.com"_s, 19, 1'700'000'000'000, 1'700'000'000'000, {}, {}, false, 0, 0},
            {3, u"image"_s, {}, 0, 1'700'000'000'000, 1'700'000'000'000, {}, {}, false, 10, 20},
        };
        Outcome r = ws({u"clip"_s});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.lines(), (QStringList {u"第一行\\n第二行"_s, u"https://example.com"_s, u"[图片 10×20]"_s}));
        r = ws({u"clip"_s, u"--kind"_s, u"link"_s, u"--json"_s});
        QCOMPARE(QJsonDocument::fromJson(r.out).array().size(), 1);
        r = ws({u"clip"_s, u"-0"_s});
        QCOMPARE(r.out, QByteArray("第一行\n第二行") + '\0' + QByteArray("https://example.com") + '\0');
        // Not allowed in the settings: 4, and where to allow it.
        mcp::Policy policy = m_served->host->policy();
        policy.allowClipboard = false;
        m_served->host->setPolicy(policy);
        r = ws({u"clip"_s});
        policy.allowClipboard = true;
        m_served->host->setPolicy(policy);
        QCOMPARE(r.code, 4);
        QVERIFY2(r.errors().contains(u"允许 AI 读取剪贴板历史"_s), qPrintable(r.errors()));
        m_served->clips.clear();
    }

    void statusAndVersion()
    {
        Outcome r = ws({u"status"_s});
        QCOMPARE(r.code, 0);
        QVERIFY2(r.text().contains(u"Win顺 0.0-test：在运行"_s), qPrintable(r.text()));
        QVERIFY(r.text().contains(u"索引：就绪"_s));
        r = ws({u"status"_s, u"--json"_s});
        QCOMPARE(r.code, 0);
        QCOMPARE(QJsonDocument::fromJson(r.out).object().value(u"index"_s).toObject().value(u"state"_s).toString(), u"ready"_s);
        r = ws({u"--version"_s});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.text(), u"ws " WINSHUN_VERSION u"\nWin顺 0.0-test\n"_s);
        // Without Win顺: ws's own; status says why (never starting it).
        r = ws({u"--version"_s}, {}, testPipeName("nobody"));
        QCOMPARE(r.text(), u"ws " WINSHUN_VERSION u"\n"_s);
        r = ws({u"status"_s}, {}, testPipeName("nobody"));
        QCOMPARE(r.code, 3);
        QVERIFY(!QFileInfo::exists(m_marker));
        r = ws({u"status"_s, u"--json"_s}, {{L"WS_SETTINGS", m_iniOff.toStdWString()}}, testPipeName("nobody"));
        QCOMPARE(r.code, 4);
        QCOMPARE(QJsonDocument::fromJson(r.out).object().value(u"connected"_s).toBool(true), false);
    }

    // Win顺 is on its way up: another ws (or the logon autostart) has started it, and the pipe
    // comes later than the 2 seconds ws used to wait. status waits too, and still never starts it.
    void waitsForAStartingWinShun()
    {
        const HANDLE running = ::CreateMutexW(nullptr, FALSE, m_mutex.c_str());
        const HANDLE starting = ::CreateMutexW(nullptr, FALSE, (m_mutex + L".Starting").c_str());
        const auto cleanup = qScopeGuard([&] {
            ::CloseHandle(running);
            ::CloseHandle(starting);
        });
        int round = 0;
        for (const QStringList& command : {QStringList {u"status"_s}, QStringList {u"find"_s, u"启动中"_s}}) {
            Served late(round++ == 0 ? "starting0" : "starting1", [](const QString& root) { writeFile(root + u"\\启动中.txt"_s, "x"); });
            QVERIFY(late.ready);
            bool listening = false;
            QElapsedTimer since;
            since.start();
            RunOptions watch;
            watch.tick = [&] {
                if (!listening && since.elapsed() > 2600) // after the old grace of 2 s
                    listening = late.listen();
            };
            const Outcome r = ws(command, {}, late.pipe, watch);
            QVERIFY(listening);
            QVERIFY2(r.code == 0, qPrintable(u"%1: %2"_s.arg(command.join(u' '), r.errors())));
            QVERIFY2(r.errors().contains(u"正在启动"_s), qPrintable(r.errors()));
            QVERIFY(!QFileInfo::exists(m_marker)); // not started: it was on its way already
            QVERIFY(r.ms >= 2500);
        }
    }

    // Win顺 hangs, or a program squats on the pipe's name and never answers: ws gives up (7)
    // after the silence, status and --version after a short one; Ctrl+C aside.
    void aSilentWinShun()
    {
        const std::wstring name = testPipeName("silent");
        std::atomic<bool> answerInitialize {true};
        FakeServer fake(name, [&](const std::string& line) -> std::optional<std::string> {
            const Request request = requestOf(line);
            if (request.method == "initialize" && answerInitialize)
                return initializeAnswer(request.id);
            return std::string(); // nothing, ever
        });
        const std::map<std::wstring, std::wstring> quick {{L"WS_TEST_IDLE_MS", L"500"}};
        Outcome r = ws({u"find"_s, u"x"_s}, quick, name);
        QCOMPARE(r.code, 7);
        QVERIFY2(r.errors().contains(u"没有回答"_s), qPrintable(r.errors()));
        QVERIFY(r.ms >= 450 && r.ms < 5000);
        QVERIFY(r.out.isEmpty());
        // status and --version: the same, with the shorter limit (here the test's).
        r = ws({u"status"_s}, quick, name);
        QCOMPARE(r.code, 7);
        QVERIFY2(r.errors().contains(u"没有回答"_s), qPrintable(r.errors()));
        r = ws({u"--version"_s}, quick, name);
        QCOMPARE(r.code, 0);
        QCOMPARE(r.text(), u"ws " WINSHUN_VERSION u"\nWin顺 9.9\n"_s); // initialize was answered
        // Not even initialize answered (a squatter): ws gives up, --version still exits 0 with its own line.
        answerInitialize = false;
        r = ws({u"find"_s, u"x"_s}, quick, name);
        QCOMPARE(r.code, 7);
        r = ws({u"--version"_s}, quick, name);
        QCOMPARE(r.code, 0);
        QCOMPARE(r.text(), u"ws " WINSHUN_VERSION u"\n"_s);
        // The silence is the time since the last bytes, not a total: a slow answer within the limit is waited for.
        const std::wstring slowName = testPipeName("slow");
        FakeServer slow(slowName, [&](const std::string& line) -> std::optional<std::string> {
            const Request request = requestOf(line);
            if (request.method == "initialize")
                return initializeAnswer(request.id);
            if (request.tool == "index_status")
                return readyAnswer(request.id);
            if (request.tool == "search_files") {
                ::Sleep(300); // under the limit
                return toolAnswer(request.id, json::Value::Object {{"items", json::Value::Array {}}, {"returned", 0}, {"truncated", false}});
            }
            return std::string();
        });
        r = ws({u"find"_s, u"x"_s}, quick, slowName);
        QCOMPARE(r.code, 1);
    }

    // An error answer with no id (a parse error is answered with id null) is the answer to
    // what was sent: ws fails with it, not waits for the one that never comes.
    void anErrorWithoutAnId()
    {
        const std::wstring name = testPipeName("noid");
        FakeServer fake(name, [](const std::string& line) -> std::optional<std::string> {
            const Request request = requestOf(line);
            if (request.method == "initialize")
                return initializeAnswer(request.id);
            if (request.tool == "index_status")
                return readyAnswer(request.id);
            if (request.tool == "search_files")
                return std::string("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}\n");
            return std::string();
        });
        const Outcome r = ws({u"find"_s, u"x"_s}, {{L"WS_TEST_IDLE_MS", L"20000"}}, name);
        QCOMPARE(r.code, 7);
        QVERIFY2(r.errors().contains(u"Parse error"_s), qPrintable(r.errors()));
        QVERIFY(r.ms < 10'000); // not the silence
    }

    void usageErrors()
    {
        Outcome r = ws({u"--exts"_s, u"pdf"_s, u"x"_s});
        QCOMPARE(r.code, 2);
        QVERIFY2(r.errors().contains(u"是不是 --ext"_s), qPrintable(r.errors()));
        QVERIFY(r.out.isEmpty());
        QCOMPARE(ws({u"find"_s}).code, 2);
        QCOMPARE(ws({u"find"_s, u"x"_s, u"--sort"_s, u"bogus"_s}).code, 2);
        QCOMPARE(ws({u"find"_s, u"x"_s, u"-n"_s, u"abc"_s}).code, 2);
        QCOMPARE(ws({u"find"_s, u"x"_s, u"-n"_s, u"60000"_s}).code, 2);
        QCOMPARE(ws({u"find"_s, u"x"_s, u"--wait"_s, u"soon"_s}).code, 2);
        QCOMPARE(ws({u"find"_s, u"x"_s, u"--color"_s, u"pink"_s}).code, 2);
        QCOMPARE(ws({u"find"_s, u"x"_s, u"--encoding"_s, u"gbk"_s}).code, 2);
        r = ws({u"recent"_s, u"--sort"_s, u"newest"_s});
        QCOMPARE(r.code, 2);
        QVERIFY(r.errors().contains(u"ws recent 不认 --sort"_s));
        // Nothing at all, not on a console: a usage error, not the help.
        RunOptions bare;
        bare.env = {{L"WS_SETTINGS", m_iniOn.toStdWString()}};
        r = runProcess(bare);
        QCOMPARE(r.code, 2);
        // Help: on stdout, 0.
        r = ws({u"help"_s, u"grep"_s});
        QCOMPARE(r.code, 0);
        QVERIFY(r.text().startsWith(u"ws grep："_s));
        r = ws({u"grep"_s, u"/?"_s});
        QCOMPARE(r.code, 0);
        QVERIFY(r.text().startsWith(u"ws grep："_s));
        QCOMPARE(ws({u"help"_s, u"nosuch"_s}).code, 2);
        // English when asked.
        r = ws({u"--exts"_s, u"pdf"_s, u"x"_s}, {{L"WS_LANG", L"en"}});
        QVERIFY(r.errors().contains(u"Did you mean --ext?"_s));
    }

    // Win顺 not reachable: each case as docs/design-cli.md §7 has it; nothing started for real.
    void reaching()
    {
        const std::wstring nobody = testPipeName("absent");
        // The switch off: 4, Win顺 not started.
        Outcome r = ws({u"find"_s, u"x"_s}, {{L"WS_SETTINGS", m_iniOff.toStdWString()}}, nobody);
        QCOMPARE(r.code, 4);
        QVERIFY2(r.errors().contains(u"允许 AI 助手和命令行连接"_s), qPrintable(r.errors()));
        QVERIFY(!QFileInfo::exists(m_marker));
        // Not running, --no-start: 3.
        r = ws({u"find"_s, u"x"_s, u"--no-start"_s}, {}, nobody);
        QCOMPARE(r.code, 3);
        QVERIFY(!QFileInfo::exists(m_marker));
        // Running (its mutex) without the pipe: 4, nothing started.
        const HANDLE mutex = ::CreateMutexW(nullptr, FALSE, m_mutex.c_str());
        r = ws({u"find"_s, u"x"_s}, {}, nobody);
        ::CloseHandle(mutex);
        QCOMPARE(r.code, 4);
        QVERIFY(!QFileInfo::exists(m_marker));
        // Not running: started (the marker), and once its pipe is there, served.
        Served late("late", [](const QString& root) { writeFile(root + u"\\启动后.txt"_s, "x"); });
        QVERIFY(late.ready);
        bool listening = false;
        RunOptions watch;
        watch.tick = [&] {
            if (!listening && QFileInfo::exists(m_marker))
                listening = late.listen();
        };
        r = ws({u"find"_s, u"启动后"_s}, {}, late.pipe, watch);
        QVERIFY(listening);
        QCOMPARE(r.code, 0);
        QCOMPARE(r.lines(), QStringList {late.path(u"启动后.txt"_s)});
        QVERIFY2(r.errors().contains(u"正在启动"_s), qPrintable(r.errors()));
    }

    void indexNotReady()
    {
        Served idle("idle", nullptr, false); // never started: its index is not ready
        QVERIFY(idle.listen());
        Outcome r = ws({u"find"_s, u"x"_s, u"--wait"_s, u"0"_s}, {}, idle.pipe);
        QCOMPARE(r.code, 5);
        QVERIFY2(r.errors().contains(u"索引还没准备好"_s), qPrintable(r.errors()));
        r = ws({u"find"_s, u"x"_s, u"--wait"_s, u"400ms"_s}, {}, idle.pipe);
        QCOMPARE(r.code, 5);
        QVERIFY(r.ms >= 400);
        QVERIFY(r.out.isEmpty());
        r = ws({u"status"_s}, {}, idle.pipe);
        QCOMPARE(r.code, 5);
    }

    void skill()
    {
        const QByteArray text = readAll(QStringLiteral(WINSHUN_CLI_SKILL));
        QVERIFY(!text.isEmpty());
        Outcome r = ws({u"skill"_s}, {}, testPipeName("nobody"));
        QCOMPARE(r.code, 0);
        QCOMPARE(r.out, text);
        // Installed into CLAUDE_CONFIG_DIR (never the user's ~/.claude here),
        // saying where this ws.exe is; the path alone on stdout.
        const QString installed = m_claude + u"\\skills\\winshun\\SKILL.md"_s;
        const QString exe = QStringLiteral(WINSHUN_CLI).replace(u'/', u'\\');
        for (int round = 0; round < 2; ++round) { // the second over the first, read-only even
            r = ws({u"skill"_s, u"--install"_s, u"claude"_s}, {}, testPipeName("nobody"));
            QCOMPARE(r.code, 0);
            QCOMPARE(r.text(), installed + u'\n');
            const QString written = QString::fromUtf8(readAll(installed));
            // The full path to run again as a separate command, not chained after `ws` with ||
            // (a permission rule such as Bash(ws:*) blocks the chained command whole).
            QVERIFY2(written.contains(u"run the same command again by its full path, as a separate command"_s)
                    && written.contains(u"not chained after the failed one with `||`"_s)
                    && written.contains(u"`\""_s + exe + u"\"`"_s, Qt::CaseInsensitive),
                qPrintable(written));
            QStringList withoutPath = written.split(u'\n');
            withoutPath.removeIf([](const QString& line) { return line.startsWith(u"If `ws` is not recognized"_s); });
            QString rest = withoutPath.join(u'\n');
            rest.replace(u"\n\n\n"_s, u"\n\n"_s);
            QCOMPARE(rest, QString::fromUtf8(text));
            QVERIFY(!QFileInfo::exists(installed + u".new"_s));
            // Nothing else left in the folder: the temporary file went with the rename.
            QCOMPARE(QDir(QFileInfo(installed).absolutePath()).entryList(QDir::Files), QStringList {u"SKILL.md"_s});
            QFile::setPermissions(installed, QFileDevice::ReadOwner);
        }
        QFile::setPermissions(installed, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        // The temporary name is the install's own: another file in the way, even one held open
        // exclusively (a second install under way), is no obstacle.
        {
            const std::wstring held = (installed + u".new"_s).toStdWString();
            const HANDLE other = ::CreateFileW(held.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            QVERIFY(other != INVALID_HANDLE_VALUE);
            r = ws({u"skill"_s, u"--install"_s, u"claude"_s}, {}, testPipeName("nobody"));
            ::CloseHandle(other);
            QCOMPARE(r.code, 0);
            QFile::remove(installed + u".new"_s);
        }
        // CLAUDE_CONFIG_DIR is a full path or nothing: relative to the folder ws happens to run in, it
        // would be a guess. Nothing is written, in the folder it names or anywhere.
        const QString relative = u"relative\\claude"_s;
        r = ws({u"skill"_s, u"--install"_s, u"claude"_s}, {{L"CLAUDE_CONFIG_DIR", relative.toStdWString()}}, testPipeName("nobody"));
        QCOMPARE(r.code, 7);
        QVERIFY(r.out.isEmpty());
        QVERIFY2(r.errors().contains(u"CLAUDE_CONFIG_DIR 要写完整路径"_s) && r.errors().contains(relative), qPrintable(r.errors()));
        RunOptions inScratch;
        inScratch.folder = m_scratch.path();
        QVERIFY(!QFileInfo::exists(m_scratch.path() + u"/relative"_s));
        r = ws({u"skill"_s, u"--install"_s, u"claude"_s}, {{L"CLAUDE_CONFIG_DIR", relative.toStdWString()}}, testPipeName("nobody"), inScratch);
        QCOMPARE(r.code, 7);
        QVERIFY(!QFileInfo::exists(m_scratch.path() + u"/relative"_s));
        // Where nothing can be written (under a file): 7, nothing on stdout
        // (the settings page reads it), the file it meant on stderr.
        const QString blocked = m_iniOn + u"\\x"_s;
        r = ws({u"skill"_s, u"--install"_s, u"claude"_s}, {{L"CLAUDE_CONFIG_DIR", blocked.toStdWString()}}, testPipeName("nobody"));
        QCOMPARE(r.code, 7);
        QVERIFY(r.out.isEmpty());
        QVERIFY2(r.errors().contains(u"写不进 "_s + blocked + u"\\skills\\winshun\\SKILL.md"_s), qPrintable(r.errors()));
        QCOMPARE(ws({u"skill"_s, u"--install"_s, u"nobody"_s}).code, 2);
    }

    // Without CLAUDE_CONFIG_DIR the folder is the profile's, as the shell has it: a USERPROFILE
    // emptied by whoever started ws gave C:\.claude.
    void skillFolderOfTheProfile()
    {
        const auto set = [](const wchar_t* name, const wchar_t* value) { _wputenv_s(name, value); };
        const QByteArray profileBefore = qgetenv("USERPROFILE");
        const QByteArray configBefore = qgetenv("CLAUDE_CONFIG_DIR");
        const auto restore = qScopeGuard([&] {
            set(L"USERPROFILE", QString::fromLocal8Bit(profileBefore).toStdWString().c_str());
            set(L"CLAUDE_CONFIG_DIR", QString::fromLocal8Bit(configBefore).toStdWString().c_str());
        });
        PWSTR known = nullptr;
        QVERIFY(SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &known)));
        const QString profile = QString::fromWCharArray(known);
        ::CoTaskMemFree(known);
        const QString expected = profile + u"\\.claude\\skills\\winshun\\SKILL.md"_s;
        set(L"CLAUDE_CONFIG_DIR", L"");
        set(L"USERPROFILE", L"");
        cli::Result<std::wstring> file = cli::claudeSkillFile();
        QVERIFY(file);
        QCOMPARE(QString::fromStdWString(*file), expected);
        set(L"USERPROFILE", L"X:\\somewhere\\else");
        file = cli::claudeSkillFile();
        QVERIFY(file);
        QCOMPARE(QString::fromStdWString(*file), expected);
        set(L"CLAUDE_CONFIG_DIR", L"D:\\claude\\");
        file = cli::claudeSkillFile();
        QVERIFY(file);
        QCOMPARE(QString::fromStdWString(*file), u"D:\\claude\\skills\\winshun\\SKILL.md"_s);
        set(L"CLAUDE_CONFIG_DIR", L"claude");
        QVERIFY(!cli::claudeSkillFile());
        set(L"CLAUDE_CONFIG_DIR", L"\\claude");
        QVERIFY(!cli::claudeSkillFile());
        set(L"CLAUDE_CONFIG_DIR", L"\\\\server\\share\\claude");
        QVERIFY(cli::claudeSkillFile());
    }

    void encodings()
    {
        // UTF-16LE when asked.
        Outcome r = ws({u"find"_s, u"报告"_s, u"--files"_s, u"--encoding"_s, u"utf16"_s});
        QCOMPARE(r.code, 0);
        const QString line = at(u"报告𠀀.txt"_s) + u'\n';
        QCOMPARE(r.out, QByteArray(reinterpret_cast<const char*>(line.utf16()), line.size() * 2));
        // WS_ENCODING says the same.
        QCOMPARE(ws({u"find"_s, u"报告"_s, u"--files"_s}, {{L"WS_ENCODING", L"utf16"}}).out, r.out);
        // Messages in a code page without Chinese: English, not rows of '?'
        // (Chinese where it can be written). The parser's own too.
        const UINT oem = ::GetOEMCP();
        r = ws({u"--exts"_s, u"pdf"_s, u"x"_s, u"--encoding"_s, u"oem"_s});
        QCOMPARE(r.code, 2);
        const QString said = fromCodePage(r.err, oem);
        QVERIFY2(said.contains(codePageHoldsChinese(oem) ? u"是不是 --ext"_s : u"Did you mean --ext?"_s), qPrintable(said));
        r = ws({u"--exts"_s, u"pdf"_s, u"x"_s, u"--encoding=utf8"_s});
        QVERIFY2(r.errors().contains(u"是不是 --ext"_s), qPrintable(r.errors()));
        // A code page without 𠀀: '?' and one note; JSON keeps it as \uXXXX.
        const UINT acp = ::GetACP();
        if (acp == CP_UTF8 || acp == cli::kGb18030CodePage)
            QSKIP("the ANSI code page holds every character");
        r = ws({u"find"_s, u"报告"_s, u"--files"_s, u"--encoding"_s, u"ansi"_s});
        QCOMPARE(r.code, 0);
        QVERIFY(r.out.contains('?'));
        // The note in the same code page as stdout (whoever reads one reads
        // both so): its Chinese may be '?' too, the code page and the way out not.
        const QString note = fromCodePage(r.err, acp);
        QVERIFY2(note.contains(QString::number(acp)) && note.contains(u"--encoding utf8"_s), qPrintable(note));
        r = ws({u"find"_s, u"报告"_s, u"--files"_s, u"--json"_s, u"--encoding"_s, u"ansi"_s});
        QVERIFY(r.out.contains("\\ud840\\udc00"));
        QVERIFY(r.notes().isEmpty());
        const QJsonArray items = QJsonDocument::fromJson(fromCodePage(r.out, acp).toUtf8()).array();
        QCOMPARE(items.first().toObject().value(u"path"_s).toString(), at(u"报告𠀀.txt"_s));
    }

    // ---- the output: what could not be written, what failed to be ----------------------

    // Lines with a character the code page lacks are counted wherever the text is cut into
    // writes: a whole document in one write (ws text), a line in pieces, NUL-separated.
    void streamCountsLossyLines()
    {
        HANDLE read = nullptr, write = nullptr;
        QVERIFY(::CreatePipe(&read, &write, nullptr, 1 << 20));
        const auto close = qScopeGuard([&] {
            ::CloseHandle(read);
            ::CloseHandle(write);
        });
        cli::Stream out(write, 1252); // Latin-1: no Chinese
        out.write(L"ab\n日本\ncd"); // one write: a lossy line inside, an unfinished one at the end
        QCOMPARE(out.lossyLines(), std::size_t {1});
        out.write(L"x日y"); // the unfinished one goes on, and has one now
        QCOMPARE(out.lossyLines(), std::size_t {2});
        out.newline();
        QCOMPARE(out.lossyLines(), std::size_t {2});
        out.write(L"日");
        out.write(L"日"); // two in a line: the line once
        out.newline();
        QCOMPARE(out.lossyLines(), std::size_t {3});
        out.write(L"clean");
        out.newline();
        out.write(L"日");
        out.nul();
        QCOMPARE(out.lossyLines(), std::size_t {4});
        out.write(L"日"); // still being written at the end: settled by the count
        QCOMPARE(out.lossyLines(), std::size_t {5});
        out.setJsonEscapes(true); // \\uXXXX loses nothing
        out.write(L"日本\n");
        QCOMPARE(out.lossyLines(), std::size_t {5});
        QVERIFY(!out.broken());
        QCOMPARE(out.writeError(), DWORD {0});
    }

    // A reader that went away (`ws … | head`) is quiet; a write that failed for another reason is reported.
    void streamWriteErrors()
    {
        HANDLE read = nullptr, write = nullptr;
        QVERIFY(::CreatePipe(&read, &write, nullptr, 0));
        ::CloseHandle(read); // the reader is gone
        {
            cli::Stream out(write, CP_UTF8);
            out.write(L"hello");
            out.flush();
            QVERIFY(out.broken());
            QCOMPARE(out.writeError(), DWORD {0});
        }
        ::CloseHandle(write);
        // A file that cannot be written to (a full disk fails the same way, with another code).
        const QString file = native(m_scratch.path() + u"/readonly.txt"_s);
        QVERIFY(writeFile(file, "x"));
        const HANDLE handle = ::CreateFileW(file.toStdWString().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);
        {
            cli::Stream out(handle, CP_UTF8);
            out.write(L"hello");
            out.flush();
            QVERIFY(out.broken());
            QVERIFY(out.writeError() != 0);
        }
        ::CloseHandle(handle);
        // The whole run: exit 7 with the reason, though the search itself worked; the bytes stay unwritten.
        RunOptions toFile;
        toFile.readOnlyStdout = file;
        const Outcome r = ws({u"find"_s, u"报告"_s}, {}, {}, toFile);
        QCOMPARE(r.code, 7);
        QVERIFY2(r.errors().contains(u"写出结果时出错"_s), qPrintable(r.errors()));
        QCOMPARE(readAll(file), QByteArray("x"));
    }

    void chineseAndTheNoteAboutIt()
    {
        // Simplified Chinese only in the code pages that have it; not in Big5 or Shift-JIS, nor Western ones.
        QVERIFY(cli::holdsChinese(936));
        QVERIFY(cli::holdsChinese(CP_UTF8));
        QVERIFY(cli::holdsChinese(cli::kGb18030CodePage));
        QVERIFY(cli::holdsChinese(cli::kUtf16CodePage));
        QVERIFY(!cli::holdsChinese(950));
        QVERIFY(!cli::holdsChinese(932));
        QVERIFY(!cli::holdsChinese(437));
        QVERIFY(!cli::holdsChinese(1252));
        // Under PowerShell (the only case that is lossy by default) the profile; not --encoding utf8.
        cli::setLanguage(cli::Lang::En);
        QString note = QString::fromStdWString(cli::lossyNote(3, 437, true, false));
        QVERIFY2(note.contains(u"3 line(s)"_s) && note.contains(u"code page 437"_s), qPrintable(note));
        QVERIFY2(note.contains(u"[Console]::OutputEncoding"_s) && !note.contains(u"--encoding"_s) && !note.contains(u"--json"_s), qPrintable(note));
        note = QString::fromStdWString(cli::lossyNote(3, 437, true, true));
        QVERIFY2(note.contains(u"[Console]::OutputEncoding"_s) && note.contains(u"--json"_s) && !note.contains(u"--encoding"_s), qPrintable(note));
        // Anywhere else (--encoding ansi, say) --encoding utf8; --json only for the commands that have it.
        note = QString::fromStdWString(cli::lossyNote(1, 1252, false, false));
        QVERIFY2(note.contains(u"--encoding utf8"_s) && !note.contains(u"--json"_s) && !note.contains(u"Console"_s), qPrintable(note));
        note = QString::fromStdWString(cli::lossyNote(1, 1252, false, true));
        QVERIFY2(note.contains(u"--encoding utf8"_s) && note.contains(u"--json"_s), qPrintable(note));
        cli::setLanguage(cli::Lang::Zh);
        note = QString::fromStdWString(cli::lossyNote(2, 936, true, false));
        QVERIFY2(note.contains(u"2 行"_s) && note.contains(u"[Console]::OutputEncoding"_s), qPrintable(note));
    }

    // The text of ws text is one write: the lines with a character the code page lacks are still counted.
    void lossyLinesInWsText()
    {
        const UINT acp = ::GetACP();
        if (acp == CP_UTF8 || acp == cli::kGb18030CodePage)
            QSKIP("the ANSI code page holds every character");
        const std::wstring name = testPipeName("lossytext");
        FakeServer fake(name, [&](const std::string& line) -> std::optional<std::string> {
            const Request request = requestOf(line);
            if (request.method == "initialize")
                return initializeAnswer(request.id);
            if (request.tool == "read_file_text") {
                return toolAnswer(request.id, json::Value::Object {{"path", request.arguments["path"].toString()}, {"kind", "text"},
                                                  {"text", "a \xF0\xA0\x80\x80\nb\n\xF0\xA0\x80\x81 c \xF0\xA0\x80\x82\n"}, {"chars", 15}, {"offset", 0},
                                                  {"cut", false}});
            }
            return std::string();
        });
        const Outcome r = ws({u"text"_s, at(u"notes.md"_s), u"--encoding"_s, u"ansi"_s}, {}, name);
        QCOMPARE(r.code, 0);
        QVERIFY(r.out.contains('?'));
        const QString note = fromCodePage(r.err, acp);
        QVERIFY2(note.contains(u"2 行"_s) || note.contains(u"2 line(s)"_s), qPrintable(note));
    }

    void conflictingFormats()
    {
        QCOMPARE(ws({u"find"_s, u"报告"_s, u"-l"_s, u"-0"_s}).code, 2);
        QCOMPARE(ws({u"find"_s, u"报告"_s, u"--json"_s, u"-0"_s}).code, 2);
        QCOMPARE(ws({u"ls"_s, root(), u"--long"_s, u"--null"_s}).code, 2);
        Outcome r = ws({u"grep"_s, u"销售额"_s, u"--json"_s, u"-0"_s});
        QCOMPARE(r.code, 2);
        QVERIFY2(r.errors().contains(u"--json 和 -0"_s), qPrintable(r.errors()));
        QVERIFY(r.out.isEmpty());
        // Fine together: the JSON with sizes.
        r = ws({u"find"_s, u"报告"_s, u"--json"_s, u"-l"_s});
        QCOMPARE(r.code, 0);
    }

    void encodingChoices()
    {
        // The last --encoding counts, in the pre-scan that picks the language and code page too.
        Outcome r = ws({u"--exts"_s, u"pdf"_s, u"x"_s, u"--encoding"_s, u"utf16"_s, u"--encoding"_s, u"utf8"_s});
        QCOMPARE(r.code, 2);
        QVERIFY2(r.errors().contains(u"是不是 --ext"_s), qPrintable(r.errors()));
        r = ws({u"find"_s, u"报告"_s, u"--files"_s, u"--encoding=utf8"_s, u"--encoding=utf16"_s});
        const QString line = at(u"报告𠀀.txt"_s) + u'\n';
        QCOMPARE(r.out, QByteArray(reinterpret_cast<const char*>(line.utf16()), line.size() * 2));
        // A WS_ENCODING that means nothing is said; --encoding beats it.
        r = ws({u"find"_s, u"报告"_s}, {{L"WS_ENCODING", L"gbk"}});
        QCOMPARE(r.code, 2);
        QVERIFY2(r.errors().contains(u"WS_ENCODING 只能是"_s) && r.errors().contains(u"“gbk”"_s), qPrintable(r.errors()));
        r = ws({u"find"_s, u"报告"_s, u"--encoding"_s, u"utf8"_s}, {{L"WS_ENCODING", L"gbk"}});
        QCOMPARE(r.code, 0);
    }

    // The loading line only goes up, though the volumes still being read average lower as one finishes.
    void indexPercentNeverGoesBack()
    {
        int shown = -1;
        QVector<int> said;
        for (const int reported : {43, 5, 21, -1, 60, 59, 99}) {
            shown = cli::stablePercent(shown, reported);
            said.push_back(shown);
        }
        QCOMPARE(said, (QVector<int> {43, 43, 43, 43, 60, 60, 99}));
        QCOMPARE(cli::stablePercent(-1, -1), -1);
    }

    // ---- raw servers -------------------------------------------------------------

    // Ctrl+Break while Win顺 works: the call is cancelled in Win顺, 130.
    void interrupted()
    {
        if (::GetConsoleCP() == 0)
            QSKIP("no console to send Ctrl+Break on");
        const std::wstring name = testPipeName("break");
        std::atomic<long long> searchId {-1};
        FakeServer fake(name, [&](const std::string& line) -> std::optional<std::string> {
            const Request request = requestOf(line);
            if (request.method == "initialize")
                return initializeAnswer(request.id);
            if (request.tool == "index_status")
                return readyAnswer(request.id);
            if (request.tool == "search_files")
                searchId = request.id; // never answered
            return std::string();
        });
        QVERIFY(fake.valid());
        DWORD pid = 0;
        RunOptions group;
        group.newGroup = true;
        group.started = [&](DWORD p) { pid = p; };
        bool sent = false;
        group.tick = [&] {
            if (!sent && searchId >= 0)
                sent = ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
        };
        const Outcome r = ws({u"find"_s, u"x"_s}, {}, name, group);
        QVERIFY(sent);
        QCOMPARE(r.code, 130);
        QTRY_VERIFY(fake.saw("\"requestId\":" + std::to_string(searchId.load())));
        QVERIFY(fake.saw("notifications/cancelled"));
        QVERIFY(r.out.isEmpty());
    }

    void brokenPipe()
    {
        const std::wstring name = testPipeName("broken");
        FakeServer fake(name, [](const std::string& line) -> std::optional<std::string> {
            const Request request = requestOf(line);
            if (request.method == "initialize")
                return initializeAnswer(request.id);
            if (request.tool == "search_files")
                return std::nullopt; // gone
            return std::string();
        });
        const Outcome r = ws({u"find"_s, u"x"_s}, {}, name);
        QCOMPARE(r.code, 7);
        QVERIFY2(r.errors().contains(u"连接断了"_s), qPrintable(r.errors()));
    }

    // A Win顺 before ws gives names and sizes always: ws prints the same fields as
    // from a newer one. It also ignores --in (`folders`) and answers for everything:
    // ws says so (initialize does not announce the contract) and exits 7, never
    // filtering the answer itself, and sends no search; `open` opens nothing.
    void olderServer()
    {
        const std::wstring name = testPipeName("older");
        const std::string inside = cli::utf8(at(u"sub\\c.txt"_s).toStdWString());
        const std::string outside = cli::utf8(at(u"c.txt"_s).toStdWString());
        FakeServer fake(name, [&](const std::string& line) -> std::optional<std::string> {
            const Request request = requestOf(line);
            if (request.method == "initialize")
                return initializeAnswer(request.id, false);
            if (request.tool == "index_status")
                return readyAnswer(request.id);
            if (request.tool == "search_files" || request.tool == "search_content") {
                json::Value::Array items;
                for (const std::string& path : {outside, inside})
                    items.emplace_back(json::Value::Object {{"path", path}, {"name", "c.txt"}, {"type", "file"}, {"modified", nullptr}, {"size", 1}});
                return toolAnswer(request.id, json::Value::Object {{"items", items}, {"returned", 2}, {"truncated", false}});
            }
            return std::string();
        });
        Outcome r = ws({u"find"_s, u"c"_s, u"--json"_s}, {}, name);
        QCOMPARE(r.code, 0);
        const QJsonArray items = QJsonDocument::fromJson(r.out).array();
        QCOMPARE(items.size(), 2);
        const QJsonObject item = items.first().toObject();
        QVERIFY(!item.contains(u"size"_s));
        QVERIFY(!item.contains(u"name"_s));
        // What ws asked: its name, the limit always, no sizes.
        QVERIFY(fake.saw("\"clientInfo\":{\"name\":\"ws\""));
        QVERIFY(fake.saw("\"limit\":100"));
        QVERIFY(fake.saw("\"size\":false"));

        // --in: refused before any search is sent, in find, open and grep.
        fake.forget();
        QFile::remove(m_opened);
        for (const QStringList& command : {QStringList {u"find"_s, u"c"_s}, QStringList {u"open"_s, u"c"_s}, QStringList {u"grep"_s, u"c"_s}}) {
            r = ws(command + QStringList {u"--in"_s, at(u"sub"_s)}, {}, name);
            QCOMPARE(r.code, 7);
            QVERIFY2(r.errors().contains(u"不认 --in"_s) && r.errors().contains(u"更新"_s), qPrintable(r.errors()));
            QVERIFY(r.out.isEmpty());
        }
        QVERIFY(!fake.saw("search_files"));
        QVERIFY(!fake.saw("search_content"));
        QVERIFY(!QFileInfo::exists(m_opened));
        r = ws({u"find"_s, u"c"_s, u"--in"_s, at(u"sub"_s)}, {{L"WS_LANG", L"en"}}, name);
        QVERIFY2(r.errors().contains(u"does not understand --in"_s) && r.errors().contains(u"update WinShun"_s), qPrintable(r.errors()));
    }

    // A Win顺 that announces the contract: --in goes to it as `folders`, and what it
    // answers is printed as it is (ws does not filter: the answer may well hold a path
    // outside the folder; keeping that out is Win顺's).
    void inWithTheContract()
    {
        const std::wstring name = testPipeName("contract");
        const std::string outside = cli::utf8(at(u"c.txt"_s).toStdWString());
        FakeServer fake(name, [&](const std::string& line) -> std::optional<std::string> {
            const Request request = requestOf(line);
            if (request.method == "initialize")
                return initializeAnswer(request.id, true);
            if (request.tool == "index_status")
                return readyAnswer(request.id);
            if (request.tool == "search_files" || request.tool == "search_content") {
                json::Value::Array items;
                items.emplace_back(json::Value::Object {{"path", outside}, {"type", "file"}, {"modified", nullptr}, {"snippet", "x"},
                    {"location", json::Value::Object {{"line", 1}}}});
                return toolAnswer(request.id, json::Value::Object {{"items", items}, {"returned", 1}, {"truncated", false}});
            }
            return std::string();
        });
        for (const QStringList& command : {QStringList {u"find"_s, u"c"_s}, QStringList {u"grep"_s, u"c"_s}}) {
            const Outcome r = ws(command + QStringList {u"--in"_s, at(u"sub"_s)}, {}, name);
            QCOMPARE(r.code, 0);
            QVERIFY2(r.text().contains(at(u"c.txt"_s)), qPrintable(r.text()));
            QVERIFY2(r.notes().isEmpty(), qPrintable(r.errors()));
        }
        QVERIFY(fake.saw("\"folders\":["));
        fake.forget();
        const Outcome opened = ws({u"open"_s, u"c"_s, u"--in"_s, at(u"sub"_s)}, {}, name);
        QCOMPARE(opened.code, 0);
        QVERIFY(fake.saw("\"folders\":["));
    }

    // Branch cli-fix-server makes this tree's Win顺 announce the contract. Until it is
    // merged this skips; after the merge it must pass (and the WS_TEST_ASSUME_CLI the
    // other tests add for the default pipe stops being set).
    void serverAnnouncesCliContract()
    {
        if (!m_serverHasCli)
            QSKIP("this tree's Win顺 does not announce capabilities.experimental.winshun.cli yet (branch cli-fix-server)");
        const Outcome r = ws({u"find"_s, u"c"_s, u"--in"_s, at(u"sub"_s)});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.lines(), QStringList {at(u"sub\\deep\\c.txt"_s)});
    }

    // recent in a folder where all is in system, program or tool folders, or
    // hidden: asked again with those, and said so. Never without --in.
    void recentHiddenPlaces()
    {
        const std::wstring name = testPipeName("places");
        const std::string file = cli::utf8(at(u"sub\\deep\\c.txt"_s).toStdWString());
        std::atomic<int> asked {0};
        FakeServer fake(name, [&](const std::string& line) -> std::optional<std::string> {
            const Request request = requestOf(line);
            if (request.method == "initialize")
                return initializeAnswer(request.id);
            if (request.tool == "index_status")
                return readyAnswer(request.id);
            if (request.tool == "recent_files") {
                ++asked;
                json::Value::Array items;
                if (request.arguments["include_system"].toBool() && request.arguments["include_hidden"].toBool())
                    items.emplace_back(json::Value::Object {{"path", file}, {"type", "file"}, {"modified", "2026-10-24T13:45:12+08:00"}});
                return toolAnswer(request.id, json::Value::Object {{"items", items}, {"returned", static_cast<long long>(items.size())}});
            }
            return std::string();
        });
        Outcome r = ws({u"recent"_s, u"--in"_s, at(u"sub"_s)}, {}, name);
        QCOMPARE(r.code, 0);
        QCOMPARE(r.lines(), QStringList {at(u"sub\\deep\\c.txt"_s)});
        QCOMPARE(asked.load(), 2);
        QVERIFY2(r.errors().contains(u"平时不列"_s), qPrintable(r.errors()));
        asked = 0;
        r = ws({u"recent"_s}, {}, name);
        QCOMPARE(r.code, 1);
        QCOMPARE(asked.load(), 1);
    }

    // A Win顺 made for ws says what kind of refusal it is: the exit codes come from that.
    void refusals()
    {
        const std::wstring name = testPipeName("refusals");
        FakeServer fake(name, [](const std::string& line) -> std::optional<std::string> {
            const Request request = requestOf(line);
            if (request.method == "initialize")
                return initializeAnswer(request.id);
            if (request.tool == "index_status")
                return readyAnswer(request.id);
            // Win顺's own words (branch cli's McpServer.cpp and McpTools.cpp).
            if (request.tool == "clipboard_history")
                return refusalAnswer(request.id,
                    "The tool 'clipboard_history' is turned off in Win顺's settings (设置 → AI 助手). Ask the user to turn it on "
                    "there, then call it again.",
                    "off");
            if (request.tool == "search_content") {
                // Of two folders the second kept from AI; else content search
                // off, which Win顺 says only in words.
                const json::Value::Array& folders = request.arguments["folders"].toArray();
                if (folders.size() == 2)
                    return refusalAnswer(request.id, "This folder is not visible to AI (excluded in Win顺's settings): " + folders[1].toString(),
                        "not_found");
                return refusalAnswer(request.id, "File content search is turned off in WinShun's settings", "");
            }
            // Some say no path: ws names the one it asked about.
            if (request.tool == "recent_files")
                return refusalAnswer(request.id, "This folder is not visible to AI (excluded in Win顺's settings).", "not_found");
            if (request.tool == "read_file_text" && request.arguments["path"].toString().ends_with("c.txt"))
                return refusalAnswer(request.id, "This path is not visible to AI (excluded in Win顺's settings).", "not_found");
            if (request.tool == "search_files") {
                const std::string query = request.arguments["query"].toString();
                if (query == "notfound")
                    return refusalAnswer(request.id,
                        "The folder is not in the index: it may not exist, be excluded from indexing, or be on a drive that is not "
                        "indexed. Check it with get_file_info: D:\\nowhere",
                        "not_found");
                if (query == "toolarge")
                    return refusalAnswer(request.id, "The answer is too large to send: 17.2 MB", "too_large");
                return refusalAnswer(request.id, "Something odd happened", "");
            }
            if (request.tool == "read_file_text") {
                // Too large unless asked for little: ws asks again for less.
                if (request.arguments["max_chars"].toInt() > 1'000)
                    return refusalAnswer(request.id, "The answer is too large to send", "too_large");
                return toolAnswer(request.id, json::Value::Object {{"path", request.arguments["path"].toString()}, {"kind", "text"},
                                                  {"text", "abc"}, {"chars", 3}, {"offset", 0}, {"cut", false}});
            }
            return std::string();
        });
        Outcome r = ws({u"find"_s, u"notfound"_s}, {}, name);
        QCOMPARE(r.code, 6);
        QVERIFY2(r.errors().contains(u"不在索引里"_s) && r.errors().contains(u"D:\\nowhere"_s), qPrintable(r.errors()));
        r = ws({u"find"_s, u"toolarge"_s, u"-n"_s, u"0"_s}, {}, name);
        QCOMPARE(r.code, 7);
        QVERIFY2(r.errors().contains(u"16 MB"_s), qPrintable(r.errors()));
        QVERIFY(fake.saw("\"limit\":50000"));
        r = ws({u"find"_s, u"plain"_s}, {}, name);
        QCOMPARE(r.code, 7);
        QVERIFY2(r.errors().contains(u"Win顺 回答：Something odd happened"_s), qPrintable(r.errors()));
        r = ws({u"clip"_s}, {}, name);
        QCOMPARE(r.code, 4);
        QVERIFY2(r.errors().contains(u"允许 AI 读取剪贴板历史"_s), qPrintable(r.errors()));
        // The refused one of two --in folders, as Win顺 names it.
        r = ws({u"grep"_s, u"x"_s, u"--in"_s, at(u"sub"_s), u"--in"_s, at(u"sub\\deep"_s)}, {}, name);
        QCOMPARE(r.code, 6);
        QVERIFY2(r.errors().contains(u"文件夹里）："_s + at(u"sub\\deep"_s) + u'\n'), qPrintable(r.errors()));
        r = ws({u"grep"_s, u"x"_s}, {}, name);
        QCOMPARE(r.code, 7);
        QVERIFY2(r.errors().contains(u"设置 → 文件内容搜索"_s), qPrintable(r.errors()));
        r = ws({u"recent"_s, u"--in"_s, at(u"sub"_s)}, {}, name);
        QCOMPARE(r.code, 6);
        QVERIFY2(r.errors().contains(u"文件夹里）："_s + at(u"sub"_s) + u'\n'), qPrintable(r.errors()));
        r = ws({u"text"_s, at(u"sub\\deep\\c.txt"_s)}, {}, name);
        QCOMPARE(r.code, 6);
        QVERIFY2(r.errors().contains(u"文件夹里）："_s + at(u"sub\\deep\\c.txt"_s) + u'\n'), qPrintable(r.errors()));
        fake.forget();
        r = ws({u"text"_s, at(u"notes.md"_s)}, {}, name);
        QCOMPARE(r.code, 0);
        QCOMPARE(r.out, QByteArray("abc"));
        QVERIFY(fake.saw("\"max_chars\":20000"));
        QVERIFY(fake.saw("\"max_chars\":5000"));
        QVERIFY(fake.saw("\"max_chars\":1000"));
    }

    // A newer Win顺's answer to ws find -n 0: 50 000 items, about 9 MB of JSON.
    // What ws takes over it, parsing and printing (the answers come from a raw
    // server: this tree's Win顺 gives at most 200).
    void bigAnswer()
    {
        // Written once (no raw string literals in this file: moc loses the
        // class over one, docs/pitfalls.md).
        json::Value::Array list;
        list.reserve(50'000);
        for (int i = 0; i < 50'000; ++i) {
            char number[8];
            std::snprintf(number, sizeof number, "%05d", i);
            list.emplace_back(json::Value::Object {
                {"path", "C:\\Users\\young\\Documents\\项目资料\\2024 年\\客户 A\\第三季度\\销售报告\\报告-" + std::string(number) + ".docx"},
                {"type", "file"}, {"modified", "2026-10-24T13:45:12+08:00"}});
        }
        const std::string items = json::write(json::Value(list)); // [...]
        const std::string one = json::write(json::Value(json::Value::Array {list.front()}));
        list.clear();
        const auto answer = [](long long id, const std::string& array, int count) {
            return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id)
                + ",\"result\":{\"content\":[],\"structuredContent\":{\"items\":" + array + ",\"returned\":" + std::to_string(count)
                + ",\"truncated\":false,\"elapsed_ms\":120},\"isError\":false}}\n";
        };

        // Here alone: the parse, and its freeing.
        const std::string line = answer(7, items, 50'000);
        QElapsedTimer timer;
        timer.start();
        std::optional<json::Value> parsed = json::parse(line);
        const qint64 parseMs = timer.elapsed();
        QVERIFY(parsed);
        QCOMPARE((*parsed)["result"]["structuredContent"]["items"].toArray().size(), std::size_t {50'000});
        timer.restart();
        parsed.reset();
        const qint64 freeMs = timer.elapsed();

        const std::wstring name = testPipeName("big");
        FakeServer fake(name, [&](const std::string& request) -> std::optional<std::string> {
            const Request r = requestOf(request);
            if (r.method == "initialize")
                return initializeAnswer(r.id);
            if (r.tool == "index_status")
                return readyAnswer(r.id);
            if (r.tool == "search_files") {
                const bool single = r.arguments["query"].toString() == "one"; // (not "small": a macro of rpcndr.h)
                return answer(r.id, single ? one : items, single ? 1 : 50'000);
            }
            return std::string();
        });
        Outcome r = ws({u"find"_s, u"报告"_s, u"-n"_s, u"0"_s}, {}, name);
        QCOMPARE(r.code, 0);
        QCOMPARE(r.lines().size(), 50'000);
        QVERIFY(r.lines().constFirst().endsWith(u"\\销售报告\\报告-00000.docx"_s));
        QVERIFY(fake.saw("\"limit\":50000"));
        r = ws({u"find"_s, u"报告"_s, u"-n"_s, u"0"_s, u"--json"_s}, {}, name);
        QCOMPARE(QJsonDocument::fromJson(r.out).array().size(), 50'000);

        std::vector<qint64> lines, jsons, nuls, ones;
        for (int i = 0; i < 5; ++i) {
            lines.push_back(ws({u"find"_s, u"报告"_s, u"-n"_s, u"0"_s}, {}, name).ms);
            jsons.push_back(ws({u"find"_s, u"报告"_s, u"-n"_s, u"0"_s, u"--json"_s}, {}, name).ms);
            nuls.push_back(ws({u"find"_s, u"报告"_s, u"-n"_s, u"0"_s, u"-0"_s}, {}, name).ms);
            ones.push_back(ws({u"find"_s, u"one"_s}, {}, name).ms);
        }
        qInfo().noquote() << u"50 000 items, %1 MB: json::parse here %2 ms (freeing %3 ms); ws find -n 0, median of 5: lines %4 ms, "
                             "--json %5 ms, -0 %6 ms; one item %7 ms"_s.arg(QString::number(static_cast<double>(line.size()) / (1 << 20), 'f', 1))
                                 .arg(parseMs)
                                 .arg(freeMs)
                                 .arg(median(lines))
                                 .arg(median(jsons))
                                 .arg(median(nuls))
                                 .arg(median(ones));
        QVERIFY(median(lines) < 5'000); // a guard against something gone badly wrong, not a measurement
    }

    // ---- what needs the server changes of branch cli -------------------------------

    void cliServerChanges()
    {
        // Win顺 for the command line: no sizes unless asked, more than 200
        // results, `folders` on search_files.
        const bool cliServer = offThread([&] {
            pipe::LineClient client;
            if (!client.connect(m_served->pipe, 2000))
                return false;
            cli::Session session(client);
            const auto r = session.call("search_files", json::Value::Object {{"query", "big"}, {"size", false}, {"limit", 5}});
            if (!r || r->isError)
                return false;
            const json::Value::Array& items = r->structured["items"].toArray();
            return !items.empty() && !items.front().has("size");
        });
        QVERIFY2(cliServer, "Win顺 should leave sizes out for ws when asked (size: false)");

        // --in is Win顺's: no note.
        Outcome r = ws({u"find"_s, u"c"_s, u"--in"_s, at(u"sub"_s)});
        QCOMPARE(r.lines(), QStringList {at(u"sub\\deep\\c.txt"_s)});
        QVERIFY2(r.notes().isEmpty(), r.err.constData());
        // More than 200.
        Served many("many", [](const QString& root) {
            for (int i = 0; i < 250; ++i)
                writeFile(root + u"\\many-%1.txt"_s.arg(i, 3, 10, u'0'), "m");
        });
        QVERIFY(many.ready);
        QVERIFY(many.listen());
        r = ws({u"find"_s, u"many-"_s, u"-n"_s, u"0"_s}, {}, many.pipe);
        QCOMPARE(r.code, 0);
        QCOMPARE(r.lines().size(), 250);
        r = ws({u"find"_s, u"many-"_s, u"-n"_s, u"220"_s}, {}, many.pipe);
        QCOMPARE(r.lines().size(), 220);
    }
};

QTEST_MAIN(TestCli)
#include "tst_cli.moc"

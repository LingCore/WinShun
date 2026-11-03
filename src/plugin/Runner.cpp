#include "Runner.h"

#include "LineChannel.h"
#include "Pipe.h"
#include "Sandbox.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>

#include <sddl.h>
#include <tlhelp32.h>

using namespace Qt::StringLiterals;

namespace ws::plugin {

namespace {

using lite::Handle;

// Explorer's token as a primary token (the shell window's process, else
// the first explorer.exe in our session); invalid when there is none.
Handle explorerToken(DWORD* error)
{
    *error = 0;
    DWORD pid = 0;
    if (const HWND shell = ::GetShellWindow())
        ::GetWindowThreadProcessId(shell, &pid);
    if (!pid) {
        const Handle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        PROCESSENTRY32W entry {sizeof entry};
        DWORD session = 0;
        ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
        if (snapshot.valid() && ::Process32FirstW(snapshot.get(), &entry)) {
            do {
                DWORD candidateSession = 0;
                if (_wcsicmp(entry.szExeFile, L"explorer.exe") == 0 && ::ProcessIdToSessionId(entry.th32ProcessID, &candidateSession)
                    && candidateSession == session) {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (::Process32NextW(snapshot.get(), &entry));
        }
    }
    if (!pid) {
        *error = ERROR_NOT_FOUND;
        return {};
    }
    const Handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.valid()) {
        *error = ::GetLastError();
        return {};
    }
    HANDLE source = nullptr;
    if (!::OpenProcessToken(process.get(), TOKEN_DUPLICATE | TOKEN_QUERY, &source)) {
        *error = ::GetLastError();
        return {};
    }
    const Handle sourceToken(source);
    HANDLE primary = nullptr;
    if (!::DuplicateTokenEx(sourceToken.get(),
            TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID | TOKEN_IMPERSONATE,
            nullptr, SecurityImpersonation, TokenPrimary, &primary)) {
        *error = ::GetLastError();
        return {};
    }
    return Handle(primary);
}

// The hard check (stage 0): a token is only used for plugins when it is
// not elevated and below high integrity. Explorer started "as
// administrator" fails it; a restricted token passes it.
bool usableForPlugins(HANDLE token)
{
    return !sandbox::tokenElevated(token) && sandbox::integrityRid(token) < SECURITY_MANDATORY_HIGH_RID;
}

bool ownTokenElevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    const Handle own(token);
    return sandbox::tokenElevated(own.get());
}

} // namespace

Runner::Runner(QString runnerExe, QObject* parent)
    : Runner(std::move(runnerExe), false, 1, parent)
{
}

Runner::Runner(QString runnerExe, bool elevated, std::uint64_t firstInstance, QObject* parent)
    : QObject(parent)
    , m_exe(std::move(runnerExe))
    , m_stopEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
    , m_elevated(elevated)
    , m_nextInstance(firstInstance)
{
}

Runner::~Runner()
{
    closeAll();
    ::CloseHandle(m_stopEvent);
}

bool Runner::start(QString* error)
{
    const auto fail = [&](const QString& text) {
        if (error)
            *error = text;
        closeAll();
        return false;
    };
    if (m_process)
        return true;
    ::ResetEvent(m_stopEvent);
    m_ready = false;
    m_stopping = false;

    // The pipe: one instance, the user's own (Pipe.h's descriptor: the
    // runner runs as the user at medium integrity), a name of ours alone.
    // The elevated runner's: administrators and SYSTEM alone, high
    // integrity, so nothing with the user's normal rights can even open it
    // (it would be the other end of an administrator's process). In a test
    // that only plays an elevated Win顺 (our token is not), the user's.
    const bool reallyElevated = m_elevated && ownTokenElevated();
    static unsigned counter = 0;
    m_pipeName = L"WinShun.Plugin." + pipe::userSid() + L"." + std::to_wstring(::GetCurrentProcessId()) + L"."
        + std::to_wstring(counter++) + L"." + std::to_wstring(QRandomGenerator::system()->generate64());
    m_key = QString::number(QRandomGenerator::system()->generate64(), 16);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (reallyElevated)
        ::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)S:(ML;;NW;;;HI)", SDDL_REVISION_1, &descriptor, nullptr);
    else
        descriptor = pipe::createSecurityDescriptor();
    if (!descriptor)
        return fail(u"无法生成管道的安全描述符"_s);
    SECURITY_ATTRIBUTES attributes {sizeof attributes, descriptor, FALSE};
    const HANDLE pipeHandle = ::CreateNamedPipeW(pipe::fullPath(m_pipeName).c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 256 * 1024, 256 * 1024, 0, &attributes);
    ::LocalFree(descriptor);
    if (pipeHandle == INVALID_HANDLE_VALUE)
        return fail(u"无法创建管道（错误 %1）"_s.arg(::GetLastError()));

    // The runner, with the user's normal rights.
    const std::wstring exe = QDir::toNativeSeparators(m_exe).toStdWString();
    const std::wstring folder = QDir::toNativeSeparators(QFileInfo(m_exe).absolutePath()).toStdWString();
    std::wstring commandLine = sandbox::commandLine(exe, {L"--pipe", m_pipeName, L"--key", m_key.toStdWString()});
    STARTUPINFOW startup {sizeof startup};
    PROCESS_INFORMATION info {};
    const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    BOOL created = FALSE;
    DWORD createError = 0;
    QString how;
    if (m_elevated) {
        // Our own token, administrator's when we are: this runner is only
        // asked for the plugins allowed to follow Win顺 (Host, Elevation.h).
        created = ::CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr, folder.c_str(),
            &startup, &info);
        createError = ::GetLastError();
        m_tokenSource = reallyElevated ? TokenSource::Elevated : TokenSource::Plain;
        how = u"CreateProcessW"_s;
    } else if (ownTokenElevated()) {
        DWORD tokenError = 0;
        Handle token = explorerToken(&tokenError);
        if (token.valid() && usableForPlugins(token.get())) {
            // Secondary Logon: only a plain STARTUPINFO goes through (pitfalls).
            std::vector<wchar_t> environment = sandbox::environmentFor(token.get(), {});
            created = ::CreateProcessWithTokenW(token.get(), 0, exe.c_str(), commandLine.data(), flags, environment.data(),
                folder.c_str(), &startup, &info);
            createError = ::GetLastError();
            m_tokenSource = TokenSource::Explorer;
            how = u"CreateProcessWithTokenW"_s;
        }
        if (!created) {
            // No usable Explorer token, or Secondary Logon would not (its
            // service stopped?): our own token without administrator rights.
            // The user's network logons are not in this session (pitfalls).
            Handle restricted = sandbox::restrictedToken(SECURITY_MANDATORY_MEDIUM_RID, &tokenError);
            if (!restricted.valid()) {
                ::CloseHandle(pipeHandle);
                return fail(u"无法降权（错误 %1）"_s.arg(tokenError));
            }
            std::vector<wchar_t> environment = sandbox::environmentFor(restricted.get(), {});
            created = ::CreateProcessAsUserW(restricted.get(), exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags,
                environment.data(), folder.c_str(), &startup, &info);
            createError = ::GetLastError();
            m_tokenSource = TokenSource::Restricted;
            how = u"CreateProcessAsUserW"_s;
        }
    } else {
        created = ::CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr, folder.c_str(),
            &startup, &info);
        createError = ::GetLastError();
        m_tokenSource = TokenSource::Plain;
        how = u"CreateProcessW"_s;
    }
    if (!created) {
        ::CloseHandle(pipeHandle);
        m_tokenSource = TokenSource::None;
        return fail(u"%1 启动不了 %2（错误 %3）"_s.arg(how, QDir::toNativeSeparators(m_exe)).arg(createError));
    }
    // Its job: ends with us (KILL_ON_JOB_CLOSE), no limit that would stop
    // it starting plugins (they get jobs of their own inside it).
    DWORD jobError = 0;
    Handle job = sandbox::makeJob(false, {}, &jobError);
    if (!job.valid() || !::AssignProcessToJobObject(job.get(), info.hProcess)) {
        const DWORD e = job.valid() ? ::GetLastError() : jobError;
        ::TerminateProcess(info.hProcess, 1);
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
        ::CloseHandle(pipeHandle);
        return fail(u"无法把 runner 放进作业（错误 %1）"_s.arg(e));
    }
    ::ResumeThread(info.hThread);
    ::CloseHandle(info.hThread);
    m_process = info.hProcess;
    m_pid = info.dwProcessId;
    m_job = job.release();

    // The pipe's thread: wait for the runner to connect (and no one else),
    // then lines until it goes.
    pipe::LineChannel::Callbacks callbacks;
    callbacks.line = [this](QByteArray line) {
        QMetaObject::invokeMethod(this, [this, line = std::move(line)] { handleLine(line); }, Qt::QueuedConnection);
    };
    m_channel = std::make_unique<pipe::LineChannel>(pipeHandle, std::move(callbacks));
    pipe::LineChannel* channel = m_channel.get();
    const DWORD expectedPid = m_pid;
    const HANDLE stopEvent = m_stopEvent;
    m_thread = std::thread([this, channel, expectedPid, stopEvent] {
        OVERLAPPED overlapped {};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = ::ConnectNamedPipe(channel->handle(), &overlapped) != 0;
        if (!connected) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_PIPE_CONNECTED)
                connected = true;
            else if (code == ERROR_IO_PENDING) {
                const HANDLE events[] = {stopEvent, overlapped.hEvent};
                if (::WaitForMultipleObjects(2, events, FALSE, 15000) == WAIT_OBJECT_0 + 1) {
                    DWORD unused = 0;
                    connected = ::GetOverlappedResult(channel->handle(), &overlapped, &unused, TRUE) != 0;
                } else {
                    ::CancelIoEx(channel->handle(), &overlapped);
                    DWORD unused = 0;
                    ::GetOverlappedResult(channel->handle(), &overlapped, &unused, TRUE);
                }
            }
        }
        ::CloseHandle(overlapped.hEvent);
        ULONG clientPid = 0;
        if (connected && ::GetNamedPipeClientProcessId(channel->handle(), &clientPid) && clientPid == expectedPid)
            channel->run();
        QMetaObject::invokeMethod(this, [this] {
            if (!m_stopping)
                closeAll();
            emit exited();
        }, Qt::QueuedConnection);
    });
    return true;
}

void Runner::closeAll()
{
    m_stopping = true;
    if (m_channel)
        m_channel->stop();
    ::SetEvent(m_stopEvent);
    if (m_thread.joinable())
        m_thread.join();
    m_channel.reset();
    if (m_job) {
        ::CloseHandle(m_job); // KILL_ON_JOB_CLOSE: the runner and every plugin end
        m_job = nullptr;
    }
    if (m_process) {
        ::WaitForSingleObject(m_process, 3000);
        ::CloseHandle(m_process);
        m_process = nullptr;
    }
    m_pid = 0;
    m_ready = false;
    m_waiting.clear();
}

void Runner::shutdown()
{
    if (!m_process)
        return;
    m_stopping = true;
    sendRaw("_ {\"op\":\"shutdown\"}");
    closeAll();
}

std::uint64_t Runner::launch(const Spec& spec)
{
    const std::uint64_t instance = m_nextInstance++;
    QJsonArray args;
    for (const QString& a : spec.args)
        args.push_back(a);
    QJsonArray env;
    for (const auto& [name, value] : spec.env)
        env.push_back(QJsonArray {name, value});
    const QJsonObject message {{u"op"_s, u"start"_s}, {u"id"_s, static_cast<double>(instance)},
        {u"exe"_s, QDir::toNativeSeparators(spec.exe)}, {u"args"_s, args}, {u"cwd"_s, QDir::toNativeSeparators(spec.cwd)},
        {u"env"_s, env}, {u"memoryMB"_s, spec.memoryMB}, {u"processes"_s, spec.processes}, {u"clipboardRead"_s, spec.clipboardRead},
        {u"clipboardWrite"_s, spec.clipboardWrite}, {u"ui"_s, spec.ui}, {u"strict"_s, spec.strict},
        {u"encoding"_s, spec.oem ? u"oem"_s : u"utf-8"_s}, {u"stdin"_s, spec.stdinOpen}};
    sendRaw("_ " + QJsonDocument(message).toJson(QJsonDocument::Compact));
    return instance;
}

void Runner::send(std::uint64_t instance, const QByteArray& line)
{
    sendRaw(QByteArray::number(static_cast<qulonglong>(instance)) + ' ' + line);
}

void Runner::stop(std::uint64_t instance)
{
    sendRaw("_ {\"op\":\"stop\",\"id\":" + QByteArray::number(static_cast<qulonglong>(instance)) + '}');
}

void Runner::sendRaw(const QByteArray& line)
{
    if (!m_channel)
        return;
    if (!m_ready) {
        m_waiting.push_back(line);
        return;
    }
    m_channel->send(line + '\n');
}

void Runner::handleLine(const QByteArray& line)
{
    if (line.startsWith("_ ")) {
        const QJsonDocument document = QJsonDocument::fromJson(line.mid(2));
        if (document.isObject())
            handleControl(document.object());
        return;
    }
    const qsizetype space = line.indexOf(' ');
    if (space <= 0)
        return;
    bool ok = false;
    const std::uint64_t instance = line.left(space).toULongLong(&ok);
    if (ok)
        emit this->line(instance, line.mid(space + 1));
}

void Runner::handleControl(const QJsonObject& m)
{
    const QString op = m.value(u"op"_s).toString();
    const auto instance = [&] { return static_cast<std::uint64_t>(m.value(u"id"_s).toDouble()); };
    if (op == u"hello") {
        if (m.value(u"key"_s).toString() != m_key) {
            emit logged(u"runner 的 hello 带错了钥匙"_s);
            closeAll();
            emit exited();
            return;
        }
        m_facts = m;
        m_ready = true;
        for (const QByteArray& waiting : m_waiting)
            m_channel->send(waiting + '\n');
        m_waiting.clear();
        emit ready();
    } else if (op == u"started") {
        emit started(instance(), static_cast<quint32>(m.value(u"pid"_s).toDouble()));
    } else if (op == u"failed") {
        emit failed(instance(), m.value(u"stage"_s).toString(), static_cast<quint32>(m.value(u"error"_s).toDouble()));
    } else if (op == u"stderr") {
        emit stderrLine(instance(), m.value(u"text"_s).toString());
    } else if (op == u"exited") {
        QStringList tail;
        for (const QJsonValue& v : m.value(u"stderrTail"_s).toArray())
            tail.push_back(v.toString());
        emit instanceExited(instance(), static_cast<quint32>(m.value(u"code"_s).toDouble()), tail, m.value(u"overlong"_s).toBool());
    } else if (op == u"log") {
        emit logged(m.value(u"text"_s).toString());
    }
}

} // namespace ws::plugin

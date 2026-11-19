#include "EditorHost.h"

#include <algorithm>
#include <vector>

using winshun::Json;

namespace gestures {

namespace {

// The window's own start: this exe again, with --editor.
std::wstring ownPath()
{
    std::wstring path(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return path;
}

Json withType(Json state, const char* type)
{
    // A copy shares its members with the original: made afresh before the type is added.
    Json out = Json::Object {};
    for (const auto& [key, value] : state.toObject())
        out.set(key, value);
    out.set("type", type);
    return out;
}

} // namespace

EditorHost::EditorHost(Hooks hooks)
    : m_hooks(std::move(hooks))
{
}

EditorHost::~EditorHost()
{
    close();
}

bool EditorHost::running()
{
    const std::lock_guard lock(m_mutex);
    return m_alive;
}

bool EditorHost::show(Json state)
{
    const std::lock_guard lock(m_mutex);
    m_state = std::move(state);
    if (m_alive) {
        // The right to the foreground: the host gave it to the plugin for
        // this request, and it is handed on to the window that is open.
        ::AllowSetForegroundWindow(m_pid);
        writeLocked(Json::Object {{"type", "show"}});
        return true;
    }
    return startLocked();
}

void EditorHost::update(Json state)
{
    const std::lock_guard lock(m_mutex);
    m_state = std::move(state);
    if (m_alive)
        writeLocked(withType(m_state, "settings"));
}

void EditorHost::appearance(const Json& appearance)
{
    const std::lock_guard lock(m_mutex);
    Json state = Json::Object {};
    for (const auto& [key, value] : m_state.toObject())
        state.set(key, value);
    state.set("appearance", appearance);
    m_state = std::move(state);
    if (m_alive)
        writeLocked(Json::Object {{"type", "appearance"}, {"appearance", appearance}});
}

void EditorHost::pausedChanged(bool paused)
{
    const std::lock_guard lock(m_mutex);
    Json state = Json::Object {};
    for (const auto& [key, value] : m_state.toObject())
        state.set(key, value);
    state.set("paused", paused);
    m_state = std::move(state);
    // Only this: the rest of what was kept may be older than an edit the window has just saved.
    if (m_alive)
        writeLocked(Json::Object {{"type", "settings"}, {"paused", paused}});
}

void EditorHost::writeLocked(const Json& message)
{
    if (m_stdin && !winshun::detail::writeLine(message.dump(), m_stdin) && m_hooks.log)
        m_hooks.log("settings window: its input is closed");
}

bool EditorHost::startLocked()
{
    if (m_reader.joinable())
        m_reader.join(); // the one before has ended (m_alive is false)
    const std::wstring exe = ownPath();
    if (exe.empty())
        return false;

    SECURITY_ATTRIBUTES inheritable {sizeof inheritable, nullptr, TRUE};
    HANDLE inRead = nullptr;
    HANDLE inWrite = nullptr;
    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    if (!::CreatePipe(&inRead, &inWrite, &inheritable, 0))
        return false;
    if (!::CreatePipe(&outRead, &outWrite, &inheritable, 0)) {
        ::CloseHandle(inRead);
        ::CloseHandle(inWrite);
        return false;
    }
    // Our ends stay ours: only the window's get passed on.
    ::SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    // Its stderr is ours (plugins.log): an inheritable copy of it, or NUL.
    HANDLE error = nullptr;
    const HANDLE ownError = ::GetStdHandle(STD_ERROR_HANDLE);
    if (!ownError || ownError == INVALID_HANDLE_VALUE
        || !::DuplicateHandle(::GetCurrentProcess(), ownError, ::GetCurrentProcess(), &error, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        error = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    // Exactly these three handles go to the window: not the pipes to Win顺
    // (the host would never see them close).
    std::vector<HANDLE> handles {inRead, outWrite};
    if (error && error != INVALID_HANDLE_VALUE)
        handles.push_back(error);
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attributeBuffer(size);
    const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    bool ok = ::InitializeProcThreadAttributeList(attributes, 1, 0, &size)
        && ::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
            handles.size() * sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inRead;
    startup.StartupInfo.hStdOutput = outWrite;
    startup.StartupInfo.hStdError = error && error != INVALID_HANDLE_VALUE ? error : nullptr;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process {};
    std::wstring command = L"\"" + exe + L"\" --editor";
    // For tests: more arguments for the window (--selftest keeps it off the screen).
    wchar_t more[2048];
    if (const DWORD n = ::GetEnvironmentVariableW(L"WS_GESTURES_EDITOR_ARGS", more, 2048); n > 0 && n < 2048)
        command += L" " + std::wstring(more, n);
    ok = ok
        && ::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
            nullptr, &startup.StartupInfo, &process);
    const DWORD failure = ok ? 0 : ::GetLastError();
    if (attributes && size)
        ::DeleteProcThreadAttributeList(attributes);
    // The window's ends are its alone now.
    ::CloseHandle(inRead);
    ::CloseHandle(outWrite);
    if (error && error != INVALID_HANDLE_VALUE)
        ::CloseHandle(error);
    if (!ok) {
        ::CloseHandle(inWrite);
        ::CloseHandle(outRead);
        if (m_hooks.log)
            m_hooks.log("settings window: not started (error " + std::to_string(failure) + ")");
        return false;
    }
    ::CloseHandle(process.hThread);
    m_process = process.hProcess;
    m_pid = process.dwProcessId;
    m_stdin = inWrite;
    m_alive = true;
    ::AllowSetForegroundWindow(m_pid);
    if (m_hooks.own)
        m_hooks.own(m_pid);
    writeLocked(withType(m_state, "init"));
    m_reader = std::thread([this, outRead] { readLoop(outRead); });
    return true;
}

// The window's stdout: lines until it closes (the window ended).
void EditorHost::readLoop(HANDLE read)
{
    std::string pending;
    char buffer[65536];
    DWORD got = 0;
    const auto handle = [&](const std::string& line) {
        if (line.empty())
            return;
        const std::optional<Json> message = Json::parse(line);
        if (!message || !message->isObject()) {
            if (m_hooks.log)
                m_hooks.log("settings window: a line that is not JSON");
            return;
        }
        const std::string type = (*message)["type"].toString();
        if (type == "save") {
            Json values = Json::Object {};
            for (const char* key : {"rules", "exclude"}) {
                if ((*message)[key].isString())
                    values.set(key, (*message)[key].toString());
            }
            for (const char* key : {"trail", "fullScreen"}) {
                if ((*message)[key].isBool())
                    values.set(key, (*message)[key].toBool());
            }
            if (!values.toObject().empty() && m_hooks.save)
                m_hooks.save(std::move(values));
        } else if (type == "pause" && (*message)["paused"].isBool()) {
            if (m_hooks.pause)
                m_hooks.pause((*message)["paused"].toBool());
        }
    };
    while (::ReadFile(read, buffer, sizeof buffer, &got, nullptr) && got > 0) {
        pending.append(buffer, got);
        for (std::size_t newline; (newline = pending.find('\n')) != std::string::npos;) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            handle(line);
        }
    }
    ::CloseHandle(read);
    const std::lock_guard lock(m_mutex);
    finishLocked();
}

void EditorHost::finishLocked()
{
    if (!m_alive)
        return;
    m_alive = false;
    if (m_stdin) {
        ::CloseHandle(m_stdin);
        m_stdin = nullptr;
    }
    if (m_process) {
        ::CloseHandle(m_process);
        m_process = nullptr;
    }
    m_pid = 0;
    if (m_hooks.own)
        m_hooks.own(0);
}

void EditorHost::close()
{
    HANDLE process = nullptr;
    {
        const std::lock_guard lock(m_mutex);
        if (m_alive) {
            writeLocked(Json::Object {{"type", "quit"}});
            // Our copy: the reader ending the window's entry closes the original.
            ::DuplicateHandle(::GetCurrentProcess(), m_process, ::GetCurrentProcess(), &process, SYNCHRONIZE | PROCESS_TERMINATE, FALSE, 0);
        }
    }
    if (process) {
        if (::WaitForSingleObject(process, 2000) == WAIT_TIMEOUT)
            ::TerminateProcess(process, 1);
        ::CloseHandle(process);
    }
    if (m_reader.joinable())
        m_reader.join(); // its pipe closes with the process
}

} // namespace gestures

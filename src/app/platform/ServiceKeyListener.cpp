#include "ServiceKeyListener.h"

#include "DiagnosticLog.h"
#include "JournalBudget.h"
#include "KeyListenerProtocol.h"
#include "VolumeProtocol.h"

#include <QDebug>
#include <QJsonObject>

#include <string_view>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// The helper's state in the diagnostics journal, as "key-helper", when it
// changes (GitHub #111: with it, double Ctrl works over administrators'
// windows, and its taps come first; without it, the GUI's own). A service
// missing or stopped is retried every 5 s: written once, not each time.
class HelperJournal {
public:
    void note(const char* state, QJsonObject details = {})
    {
        if (m_state && std::string_view(m_state) == state && !details.contains(u"code"_s))
            return;
        m_state = state;
        const ULONGLONG now = ::GetTickCount64();
        if (!m_budget.take(std::string_view(state) != "connected", now))
            return;
        details.insert(u"state"_s, QString::fromLatin1(state));
        if (m_budget.skipped() > 0)
            details.insert(u"skipped"_s, m_budget.skipped());
        diagnostics::record(u"key-helper"_s, details);
    }

private:
    const char* m_state = nullptr;
    diagnostics::JournalBudget m_budget;
};

} // namespace

ServiceKeyListener::ServiceKeyListener(HWND window, Changed changed)
    : m_stop(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    if (m_stop)
        m_thread = std::thread([this, window, changed = std::move(changed)] { run(window, changed); });
}

ServiceKeyListener::~ServiceKeyListener()
{
    if (m_stop)
        ::SetEvent(m_stop);
    if (m_thread.joinable())
        m_thread.join();
    if (m_stop)
        ::CloseHandle(m_stop);
}

void ServiceKeyListener::run(HWND window, const Changed& changed)
{
    using namespace volproto;
    wchar_t overrideName[256] {};
    const DWORD n = ::GetEnvironmentVariableW(kPipeEnvironment, overrideName, 256);
    const std::wstring path = fullPipePath(n && n < 256 ? overrideName : kPipeName);
    constexpr DWORD timeout = 1000;
    DWORD reportedError = 0;
    HelperJournal journal;
    do {
        // Identification only: even a squatted pipe cannot act as the UI.
        Channel channel(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (!channel.valid())
            journal.note("no-service");
        if (channel.valid()) {
            Writer w;
            Hello hello;
            hello.pid = ::GetCurrentProcessId();
            encode(w, hello);
            Type type {};
            std::vector<std::byte> payload;
            HelloOk ok;
            const bool greeted = channel.write(w.finish(Type::Hello), timeout)
                && channel.read(type, payload, timeout, m_stop) && type == Type::HelloOk && decode(payload, ok);
            if (greeted && (ok.major != kMajor || !(ok.capabilities & kCapabilityKeyListener)))
                journal.note("old-service");
            if (greeted && ok.major == kMajor && (ok.capabilities & kCapabilityKeyListener)) {
                w.reset();
                w.u64(reinterpret_cast<std::uintptr_t>(window));
                if (channel.write(w.finish(Type::OpenKeyListener), timeout)
                    && channel.read(type, payload, timeout, m_stop) && type == Type::KeyListenerReady) {
                    Reader reader(payload);
                    std::uint32_t pid = 0;
                    if (reader.u32(pid) && pid && reader.done()) {
                        changed(pid); // the window also waits for the child's Ready heartbeat
                        journal.note("connected");
                        const ULONGLONG since = ::GetTickCount64();
                        while (::WaitForSingleObject(m_stop, keyproto::kHeartbeatMs) == WAIT_TIMEOUT) {
                            if (!channel.write(Type::KeyListenerPing, {}, timeout)
                                || !channel.read(type, payload, timeout, m_stop) || type != Type::KeyListenerPing
                                || !payload.empty())
                                break;
                        }
                        changed(0);
                        if (::WaitForSingleObject(m_stop, 0) == WAIT_TIMEOUT) // not us stopping it
                            journal.note("lost", {{u"after_s"_s, static_cast<qint64>((::GetTickCount64() - since) / 1000)}});
                    }
                } else if (type == Type::Error) {
                    Error error;
                    if (decode(payload, error) && error.code != reportedError) {
                        reportedError = error.code;
                        qWarning().noquote() << "Double Ctrl: UIAccess listener unavailable, using local input:"
                                             << QString::fromStdWString(error.message) << "(error" << error.code << ')';
                        journal.note("unavailable", {{u"code"_s, static_cast<qint64>(error.code)}});
                    }
                }
            }
        }
        // Also recovers from a service starting later, an upgrade or a crash.
    } while (::WaitForSingleObject(m_stop, 5000) == WAIT_TIMEOUT);
}

} // namespace ws

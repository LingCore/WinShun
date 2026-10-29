#include "ServiceDialogNav.h"

#include "VolumeProtocol.h"

#include <algorithm>
#include <chrono>

namespace ws {

using namespace volproto;

namespace {
constexpr DWORD kTimeout = 1500; // handshake / reply
constexpr auto kReprobe = std::chrono::seconds(15); // the service may start or be upgraded

// A fresh identifying connection, as ServiceKeyListener uses: a squatted pipe
// cannot act as the UI (SECURITY_IDENTIFICATION), and we only read capabilities.
Channel connect()
{
    wchar_t overrideName[256] {};
    const DWORD n = ::GetEnvironmentVariableW(kPipeEnvironment, overrideName, 256);
    const std::wstring path = fullPipePath(n && n < 256 ? overrideName : kPipeName);
    return Channel(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
}

// Handshake; what the service offers this process.
ServiceDialogNav::Reach handshake(Channel& channel, HANDLE abort)
{
    using Reach = ServiceDialogNav::Reach;
    Writer w;
    Hello hello;
    hello.pid = ::GetCurrentProcessId();
    encode(w, hello);
    Type type {};
    std::vector<std::byte> payload;
    HelloOk ok;
    if (!channel.valid() || !channel.write(w.finish(Type::Hello), kTimeout)
        || !channel.read(type, payload, kTimeout, abort) || type != Type::HelloOk || !decode(payload, ok)
        || ok.major != kMajor)
        return Reach::NoService;
    return (ok.capabilities & kCapabilityDialogNav) ? Reach::Available
        : (ok.capabilities & kCapabilityDialogNavBesideOnly) ? Reach::OtherCopy
                                                             : Reach::NoNavigator;
}
} // namespace

ServiceDialogNav::ServiceDialogNav(ReachChanged reachChanged)
    : m_reachChanged(std::move(reachChanged))
    , m_stopEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    m_thread = std::thread([this] { run(); });
}

ServiceDialogNav::~ServiceDialogNav()
{
    {
        const std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    if (m_stopEvent)
        ::SetEvent(m_stopEvent); // cancel any in-flight pipe read
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    if (m_stopEvent)
        ::CloseHandle(m_stopEvent);
}

void ServiceDialogNav::refresh()
{
    {
        const std::lock_guard lock(m_mutex);
        m_refresh = true;
    }
    m_cv.notify_all();
}

void ServiceDialogNav::navigate(HWND dialog, std::wstring folder, Done done)
{
    // The service refuses '/' (it would hide a ".." from its check); some
    // file managers report folders with it.
    std::ranges::replace(folder, L'/', L'\\');
    {
        const std::lock_guard lock(m_mutex);
        m_queue.push({dialog, std::move(folder), std::move(done)});
    }
    m_cv.notify_all();
}

void ServiceDialogNav::run()
{
    bool firstProbe = true;
    for (;;) {
        std::queue<Request> requests;
        {
            std::unique_lock lock(m_mutex);
            if (!firstProbe && !m_refresh && m_queue.empty())
                m_cv.wait_for(lock, kReprobe, [this] { return m_stop || m_refresh || !m_queue.empty(); });
            if (m_stop)
                break;
            m_refresh = false;
            std::swap(requests, m_queue);
        }
        firstProbe = false;
        // Re-probe what it offers (connection kept alive only for the handshake).
        setReach(probe());
        // Then the queued navigation requests, in order.
        while (!requests.empty()) {
            Request request = std::move(requests.front());
            requests.pop();
            const bool ok = send(request);
            if (request.done)
                request.done(ok);
            {
                const std::lock_guard lock(m_mutex);
                if (m_stop)
                    return;
            }
        }
    }
}

// The one place the reach changes. The owner is told against what it was told
// last, not against m_reach: send() changes that too (the service stopped
// right after a probe), and the next probe would then see no change, so the
// owner would never hear of it -- its bar would keep offering a jump that does
// nothing. The whole value counts, not only whether it can jump: the bar's
// hint differs between no service and one that keeps the navigator from us.
void ServiceDialogNav::setReach(Reach now)
{
    m_reach.store(now, std::memory_order_relaxed);
    if (now == m_told)
        return;
    m_told = now;
    if (m_reachChanged)
        m_reachChanged(now);
}

ServiceDialogNav::Reach ServiceDialogNav::probe()
{
    Channel channel = connect();
    return handshake(channel, m_stopEvent);
}

bool ServiceDialogNav::send(const Request& request)
{
    Channel channel = connect();
    const Reach reach = handshake(channel, m_stopEvent);
    setReach(reach);
    if (reach != Reach::Available)
        return false;
    Writer w;
    w.u64(reinterpret_cast<std::uintptr_t>(request.dialog));
    w.string(request.folder);
    Type type {};
    std::vector<std::byte> payload;
    // The service launches the helper and waits for it, so allow more time.
    return channel.write(w.finish(Type::OpenDialogNav), kTimeout)
        && channel.read(type, payload, 12'000, m_stopEvent) && type == Type::DialogNavDone;
}

} // namespace ws

#include "ServiceCaptureSurface.h"

#include "VolumeProtocol.h"

#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws {

struct StopSignal {
    HANDLE handle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

    ~StopSignal()
    {
        if (handle)
            ::CloseHandle(handle);
    }

    void trigger() const
    {
        if (handle)
            ::SetEvent(handle);
    }
};

struct ServiceCaptureSurface::Lease {
    std::shared_ptr<StopSignal> stop = std::make_shared<StopSignal>();
    std::thread worker;

    ~Lease()
    {
        if (stop)
            stop->trigger();
        if (worker.joinable())
            worker.join();
    }
};

namespace {

using namespace volproto;

constexpr DWORD kHandshakeTimeoutMs = 1500;
constexpr DWORD kPingIntervalMs = 1000;

Channel connectService()
{
    wchar_t overrideName[256] {};
    const DWORD n = ::GetEnvironmentVariableW(kPipeEnvironment, overrideName, 256);
    const std::wstring path = fullPipePath(n && n < 256 ? overrideName : kPipeName);
    return Channel(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
}

} // namespace

ServiceCaptureSurface::ServiceCaptureSurface() = default;

ServiceCaptureSurface::~ServiceCaptureSurface()
{
    close();
}

void ServiceCaptureSurface::close()
{
    std::unique_ptr<Lease> old;
    {
        const std::lock_guard lock(m_mutex);
        old = std::move(m_active);
    }
    // Destroy outside the lock: triggers the stop event and joins worker on
    // the caller thread (worker only holds StopSignal, never Lease).
    old.reset();
}

void ServiceCaptureSurface::open(DWORD pluginPid, HWND fullScreenHwnd, std::wstring pipeName,
    std::function<void(Result)> done)
{
    close();
    auto lease = std::make_unique<Lease>();
    if (!lease->stop || !lease->stop->handle) {
        if (done)
            done({false, 0, u"failed"_s, ::GetLastError()});
        return;
    }
    const std::shared_ptr<StopSignal> stop = lease->stop;
    lease->worker = std::thread(
        [stop, pluginPid, fullScreenHwnd, pipeName = std::move(pipeName), done = std::move(done)]() mutable {
            auto reply = [&](Result r) {
                if (auto fn = std::exchange(done, {}))
                    fn(std::move(r));
            };
            try {
                Channel channel = connectService();
                Writer hello;
                Hello greeting;
                greeting.pid = ::GetCurrentProcessId();
                encode(hello, greeting);
                Type type {};
                std::vector<std::byte> payload;
                HelloOk ok;
                if (!channel.valid() || !channel.write(hello.finish(Type::Hello), kHandshakeTimeoutMs)
                    || !channel.read(type, payload, kHandshakeTimeoutMs, stop->handle) || type != Type::HelloOk
                    || !decode(payload, ok) || ok.major != kMajor) {
                    reply({false, 0, u"no-service"_s, channel.lastError()});
                    return;
                }
                if (!(ok.capabilities & kCapabilityCaptureSurface)) {
                    reply({false, 0, u"not-offered"_s, ERROR_NOT_SUPPORTED});
                    return;
                }
                Writer req;
                req.u32(pluginPid);
                req.u64(reinterpret_cast<std::uintptr_t>(fullScreenHwnd));
                req.string(pipeName);
                if (!channel.write(req.finish(Type::OpenCaptureSurface), kHandshakeTimeoutMs)
                    || !channel.read(type, payload, kHandshakeTimeoutMs, stop->handle)) {
                    reply({false, 0, u"failed"_s, channel.lastError() ? channel.lastError() : ERROR_TIMEOUT});
                    return;
                }
                if (type != Type::CaptureSurfaceReady) {
                    Error err;
                    const DWORD code = type == Type::Error && decode(payload, err) && err.code
                        ? err.code : ERROR_GEN_FAILURE;
                    reply({false, 0, code == ERROR_BUSY ? u"busy"_s : u"failed"_s, code});
                    return;
                }
                Reader r(payload);
                std::uint32_t childPid = 0;
                if (!r.u32(childPid) || !childPid || !r.done()) {
                    reply({false, 0, u"failed"_s, ERROR_INVALID_DATA});
                    return;
                }
                reply({true, childPid, {}, ERROR_SUCCESS});
                while (::WaitForSingleObject(stop->handle, kPingIntervalMs) == WAIT_TIMEOUT) {
                    if (!channel.write(Type::CaptureSurfacePing, {}, kHandshakeTimeoutMs)
                        || !channel.read(type, payload, kHandshakeTimeoutMs, stop->handle)
                        || type != Type::CaptureSurfacePing || !payload.empty())
                        break;
                }
            } catch (...) {
                reply({false, 0, u"failed"_s, ERROR_GEN_FAILURE});
            }
        });
    const std::lock_guard lock(m_mutex);
    m_active = std::move(lease);
}

} // namespace ws

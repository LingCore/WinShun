#include "ServicePaste.h"

#include "VolumeProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ws::servicepaste {

using namespace volproto;

namespace {
constexpr DWORD kTimeout = 1500; // handshake
constexpr DWORD kPasteTimeout = 4000; // the service waits 3 s for its helper

// A fresh identifying connection, as ServiceDialogNav's: a squatted pipe
// cannot act as the UI (SECURITY_IDENTIFICATION).
Channel connect()
{
    wchar_t overrideName[256] {};
    const DWORD n = ::GetEnvironmentVariableW(kPipeEnvironment, overrideName, 256);
    const std::wstring path = fullPipePath(n && n < 256 ? overrideName : kPipeName);
    return Channel(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
}
} // namespace

Result paste(HWND window)
{
    Channel channel = connect();
    Writer hello;
    Hello greeting;
    greeting.pid = ::GetCurrentProcessId();
    encode(hello, greeting);
    Type type {};
    std::vector<std::byte> payload;
    HelloOk ok;
    if (!channel.valid() || !channel.write(hello.finish(Type::Hello), kTimeout) || !channel.read(type, payload, kTimeout)
        || type != Type::HelloOk || !decode(payload, ok) || ok.major != kMajor)
        return {.outcome = Outcome::NoService};
    if (!(ok.capabilities & kCapabilityPasteRelay))
        return {.outcome = Outcome::NotOffered};
    Writer request;
    request.u64(reinterpret_cast<std::uintptr_t>(window));
    if (!channel.write(request.finish(Type::OpenPaste), kTimeout) || !channel.read(type, payload, kPasteTimeout))
        return {.outcome = Outcome::Failed, .error = ERROR_TIMEOUT};
    if (type == Type::PasteDone)
        return {.outcome = Outcome::Pasted};
    Error error;
    return {.outcome = Outcome::Failed,
        .error = type == Type::Error && decode(payload, error) && error.code ? error.code : ERROR_GEN_FAILURE};
}

} // namespace ws::servicepaste

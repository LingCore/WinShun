#pragma once

#include "Ntfs.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace ws::ntfs {

// What the Service Control Manager says about the index service (GitHub #52):
// told apart so that the words say why it is not in use, and so that a
// computer that never had it does not wait for it.
enum class ServiceState {
    Unknown, // the SCM would not say (access denied ...): treated as before, by the pipe
    NotInstalled,
    Disabled, // registered, start type "disabled", not running
    Stopped,
    Starting,
    Running, // the pipe still has to answer
};

// The volumes through the index service (WinShunService.exe, src/service),
// over its pipe: for a Win顺 that runs with the user's normal rights. Each
// MFT read, journal query, journal followed and CloseVolume is a connection
// of its own (VolumeProtocol.h), so the sources here are independent of one
// another and of this object, which only keeps the pipe's name.
//
// A journal source whose connection breaks (the service restarted) connects
// again on each of its next reads, for up to reconnectWindow(), and goes on
// from the same USN: only the service saying that the journal is lost (or
// the window running out) makes finish() return false.
class ServiceVolumeAccess final : public VolumeAccess {
public:
    explicit ServiceVolumeAccess(std::wstring pipeName = defaultPipeName());
    // WinShun.Volumes, or the WINSHUN_PIPE environment variable (a server
    // started with --console --pipe NAME, for tests).
    static std::wstring defaultPipeName();
    // Whether the service is registered on this computer (by the installer,
    // or `WinShunService.exe --install`), running or not: for the settings.
    static bool installed();
    // The SCM's word on the service: connecting and asking its status and
    // configuration is open to every user. WINSHUN_SERVICE=none|disabled|stopped
    // pretends (for testing on a computer that has the service).
    static ServiceState state();
    // state() from what the SCM answered: the error of opening the service
    // (0 when it opened), its start type (0xFFFFFFFF when unknown) and its
    // current state (SERVICE_STOPPED ... ; 0 when unknown).
    static ServiceState classify(unsigned long openError, unsigned long startType, unsigned long currentState) noexcept;
    // The value of WINSHUN_SERVICE; nothing for any other.
    static std::optional<ServiceState> forcedState(std::wstring_view value) noexcept;
    // Short English name (diagnostics): not_installed, disabled, stopped ...
    static const char* stateName(ServiceState state) noexcept;
    // The executable the SCM runs the service from, unquoted; empty if it is
    // not installed or its configuration cannot be read (QueryServiceConfig is
    // open to any user).
    static std::wstring imagePath();
    // The WinShun.exe of the copy that installed the service (its
    // --installed-from, set when that copy had to place the service in the
    // protected folder); empty when there is none or no service.
    static std::wstring installedFrom();
    // True when the service is installed and its registered executable sits
    // where a non-administrator could replace it (an older portable copy that
    // registered the service from a user-writable folder): a local
    // privilege-escalation risk the settings offer to repair. False when the
    // service is not installed, or its location is protected.
    static bool registeredInUnsafeLocation();

    // Connects and shakes hands once: whether the service is there and
    // speaks our protocol. serviceVersion() and lastError() say more.
    bool probe();
    std::wstring serviceVersion() const; // "0.7.3" once probe() succeeded
    std::wstring lastError() const; // of the last probe or connection that failed
    const std::wstring& pipeName() const noexcept { return m_pipeName; }

    Kind kind() const noexcept override { return Kind::Service; }
    std::unique_ptr<RecordSource> openMft(std::wstring_view root, std::uint32_t serial, bool lowPriority) override;
    std::optional<JournalInfo> queryJournal(std::wstring_view root, std::uint32_t serial) override;
    std::unique_ptr<JournalSource> openJournal(std::wstring_view root, std::uint32_t serial,
        std::uint64_t journalId) override;
    // Asks the service to let go of everything it opened on this process's
    // behalf on the volume, and waits (2 s at most) for it to say so.
    void closeVolume(std::wstring_view root) override;
    // Null when the service is an older one without bulk reads.
    std::unique_ptr<BulkSource> openBulkRead(std::wstring_view root, std::uint32_t serial, bool quiet) override;

    // How long a journal source keeps trying to reach the service again
    // before it gives the journal up as lost (30 s; tests shorten it).
    static std::chrono::milliseconds reconnectWindow() noexcept;
    static void setReconnectWindow(std::chrono::milliseconds window) noexcept;

private:
    void noteError(std::wstring error);

    std::wstring m_pipeName;
    mutable std::mutex m_mutex;
    std::wstring m_serviceVersion;
    std::wstring m_lastError;
};

} // namespace ws::ntfs

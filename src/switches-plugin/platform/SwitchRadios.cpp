// C++/WinRT before Qt's headers: Qt's "signals" and "slots" macros would
// rewrite what is in it.
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>

#include "SwitchRadios.h"

#include <QDebug>

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceWatcher;
using winrt::Windows::Devices::Enumeration::DeviceWatcherStatus;
using winrt::Windows::Devices::Radios::Radio;
using winrt::Windows::Devices::Radios::RadioAccessStatus;
using winrt::Windows::Devices::Radios::RadioKind;
using winrt::Windows::Devices::Radios::RadioState;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Networking::Connectivity::ConnectionProfile;
using winrt::Windows::Networking::Connectivity::NetworkInformation;

namespace ws {

namespace {

constexpr std::chrono::milliseconds kWait = 5s; // for Windows: listing, access, a radio switched
constexpr std::chrono::seconds kListAgain = 30s; // when the launcher is shown, at most this often

QString describe(const winrt::hresult_error& e)
{
    return u"0x%1 %2"_s.arg(static_cast<quint32>(e.code().value), 8, 16, QChar(u'0'))
        .arg(QString::fromWCharArray(e.message().c_str()));
}

// The result of `operation`, waited for here (the switch thread): none when
// it takes longer than `most` or the thread is ending, and cancelled then.
// Its Completed is set here, once: a second time it would fail
// (E_ILLEGAL_DELEGATE_ASSIGNMENT), as wait_for() over and over does. Throws
// winrt::hresult_error when the operation failed.
template <typename Result>
std::optional<Result> finish(const IAsyncOperation<Result>& operation, const std::stop_token& stop,
    std::chrono::milliseconds most)
{
    struct Done {
        std::mutex mutex;
        std::condition_variable_any cv;
        bool done = false;
    };
    const auto done = std::make_shared<Done>(); // the handler's too: it may come after we stopped waiting
    operation.Completed([done](const auto&, AsyncStatus) {
        {
            std::lock_guard lock(done->mutex);
            done->done = true;
        }
        done->cv.notify_all();
    });
    bool finished = false;
    {
        std::unique_lock lock(done->mutex);
        finished = done->cv.wait_for(lock, stop, most, [&done] { return done->done; });
    }
    if (!finished) {
        try {
            operation.Cancel();
        } catch (const winrt::hresult_error&) {
        }
        return std::nullopt;
    }
    return operation.GetResults();
}

SwitchRadios::Access accessOf(RadioAccessStatus status)
{
    switch (status) {
    case RadioAccessStatus::Allowed:
        return SwitchRadios::Access::Allowed;
    case RadioAccessStatus::DeniedByUser:
        return SwitchRadios::Access::DeniedByUser;
    case RadioAccessStatus::DeniedBySystem:
        return SwitchRadios::Access::DeniedBySystem;
    default:
        return SwitchRadios::Access::Unspecified;
    }
}

SwitchRadios::Power powerOf(RadioState state)
{
    switch (state) {
    case RadioState::On:
        return SwitchRadios::Power::On;
    case RadioState::Off:
        return SwitchRadios::Power::Off;
    case RadioState::Disabled:
        return SwitchRadios::Power::Disabled;
    default:
        return SwitchRadios::Power::Unknown;
    }
}

RadioKind kindOf(QStringView id)
{
    return id == SwitchRadios::kBluetooth ? RadioKind::Bluetooth : RadioKind::WiFi;
}

// An 802.11 adapter is up: on a network. Not which one: its name needs the
// location permission (Windows 11 24H2 and later).
bool wlanConnected()
{
    constexpr ULONG kFlags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST
        | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
    ULONG size = 16 * 1024;
    std::vector<std::byte> buffer; // operator new's alignment: enough for IP_ADAPTER_ADDRESSES
    for (int attempt = 0; attempt < 3; ++attempt) {
        buffer.resize(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        const ULONG status = ::GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr, adapters, &size);
        if (status == ERROR_BUFFER_OVERFLOW)
            continue; // more adapters than a moment ago: `size` says how much now
        if (status != ERROR_SUCCESS)
            return false;
        for (const IP_ADAPTER_ADDRESSES* a = adapters; a; a = a->Next) {
            if (a->IfType == IF_TYPE_IEEE80211 && a->OperStatus == IfOperStatusUp)
                return true;
        }
        return false;
    }
    return false;
}

// The internet goes through WLAN now: turning it off leaves the computer offline.
bool internetThroughWlan()
{
    try {
        const ConnectionProfile profile = NetworkInformation::GetInternetConnectionProfile();
        return profile && profile.IsWlanConnectionProfile();
    } catch (const winrt::hresult_error& e) {
        qWarning().noquote() << "SwitchRadios: no internet profile:" << describe(e);
        return false;
    }
}

QString failure(QStringView id, bool on, const QString& why)
{
    const char* text = id == SwitchRadios::kBluetooth
        ? (on ? QT_TRANSLATE_NOOP("ws::SwitchRadios", "没能打开蓝牙：%1")
              : QT_TRANSLATE_NOOP("ws::SwitchRadios", "没能关闭蓝牙：%1"))
        : (on ? QT_TRANSLATE_NOOP("ws::SwitchRadios", "没能打开 WLAN：%1")
              : QT_TRANSLATE_NOOP("ws::SwitchRadios", "没能关闭 WLAN：%1"));
    return QCoreApplication::translate("ws::SwitchRadios", text).arg(why);
}

} // namespace

// Everything made on the switch thread, let go there (close).
struct SwitchRadios::System {
    struct Known {
        Radio radio;
        RadioKind kind;
    };

    std::shared_ptr<SwitchRelay> relay;
    // Set from the device watcher's threads: a radio came or went, so the
    // next read lists them again.
    std::shared_ptr<std::atomic<bool>> moved = std::make_shared<std::atomic<bool>>(false);
    // The watcher's first round (the radios there already) is over: what
    // it finds after that is a change.
    std::shared_ptr<std::atomic<bool>> started = std::make_shared<std::atomic<bool>>(false);

    Access access = Access::Unspecified;
    std::optional<std::vector<Known>> radios; // none: not listed yet
    std::vector<Radio::StateChanged_revoker> radioWatches; // one per radio
    std::chrono::steady_clock::time_point listedAt;

    DeviceWatcher devices {nullptr};
    DeviceWatcher::Added_revoker added;
    DeviceWatcher::Removed_revoker removed;
    DeviceWatcher::Updated_revoker updated;
    DeviceWatcher::EnumerationCompleted_revoker completed;
    NetworkInformation::NetworkStatusChanged_revoker network;

    void watchDevices();
    void askAccess(const std::stop_token& stop);
    void list(const std::stop_token& stop);
};

void SwitchRadios::System::watchDevices()
{
    try {
        // A radio's device interface, as an adapter comes, goes, or its
        // driver starts again.
        devices = DeviceInformation::CreateWatcher(Radio::GetDeviceSelector());
        const auto came = [relay = relay, moved = moved, started = started](const auto&, const auto&) {
            if (!started->load())
                return;
            moved->store(true);
            (*relay)(kBluetooth.toString());
            (*relay)(kWlan.toString());
        };
        added = devices.Added(winrt::auto_revoke, came);
        removed = devices.Removed(winrt::auto_revoke, came);
        // Without a handler for Updated, nothing comes after the first round.
        updated = devices.Updated(winrt::auto_revoke, [](const auto&, const auto&) { });
        completed = devices.EnumerationCompleted(winrt::auto_revoke,
            [started = started](const auto&, const auto&) { started->store(true); });
        devices.Start();
    } catch (const winrt::hresult_error& e) {
        qWarning().noquote() << "SwitchRadios: radios coming and going not watched:" << describe(e);
    }
    try {
        // WLAN's second line, and whether the internet goes through it.
        network = NetworkInformation::NetworkStatusChanged(winrt::auto_revoke,
            [relay = relay](const auto&) { (*relay)(kWlan.toString()); });
    } catch (const winrt::hresult_error& e) {
        qWarning().noquote() << "SwitchRadios: network not watched:" << describe(e);
    }
}

void SwitchRadios::System::askAccess(const std::stop_token& stop)
{
    try {
        if (const std::optional<RadioAccessStatus> status = finish(Radio::RequestAccessAsync(), stop, kWait))
            access = accessOf(*status);
    } catch (const winrt::hresult_error& e) {
        qWarning().noquote() << "SwitchRadios: access not asked for:" << describe(e);
    }
}

void SwitchRadios::System::list(const std::stop_token& stop)
{
    moved->store(false);
    listedAt = std::chrono::steady_clock::now();
    askAccess(stop);
    try {
        const auto found = finish(Radio::GetRadiosAsync(), stop, kWait);
        if (!found) {
            qWarning() << "SwitchRadios: the radios not listed in time";
            moved->store(true); // again at the next read
            return;
        }
        std::vector<Known> now;
        std::vector<Radio::StateChanged_revoker> watches;
        for (const Radio& radio : *found) {
            const RadioKind kind = radio.Kind();
            if (kind != RadioKind::Bluetooth && kind != RadioKind::WiFi)
                continue;
            const QString id = (kind == RadioKind::Bluetooth ? kBluetooth : kWlan).toString();
            watches.push_back(radio.StateChanged(winrt::auto_revoke, [relay = relay, id](const auto&, const auto&) {
                (*relay)(id); // this app's own writes too: ~1 ms before they are done
            }));
            now.push_back(Known {.radio = radio, .kind = kind});
        }
        radioWatches = std::move(watches); // the last list's let go
        radios = std::move(now);
    } catch (const winrt::hresult_error& e) {
        qWarning().noquote() << "SwitchRadios: the radios not listed:" << describe(e);
        moved->store(true);
    }
}

SwitchRadios::SwitchRadios() = default;

SwitchRadios::~SwitchRadios() = default; // close() has let go of the system's objects, on their thread

QStringList SwitchRadios::ids() const
{
    return {kBluetooth.toString(), kWlan.toString()};
}

void SwitchRadios::setStopToken(std::stop_token stop)
{
    m_stop = std::move(stop);
}

void SwitchRadios::watch(Notify changed)
{
    m_relay->watch(std::move(changed));
}

SwitchRadios::System& SwitchRadios::system()
{
    if (!m_system) {
        m_system = std::make_unique<System>();
        m_system->relay = m_relay;
        m_system->watchDevices();
    }
    return *m_system;
}

SwitchState SwitchRadios::read(const QString& id)
{
    System& s = system();
    if (!s.radios || s.moved->load() || std::chrono::steady_clock::now() - s.listedAt > kListAgain)
        s.list(m_stop);
    else if (s.access != Access::Allowed)
        s.askAccess(m_stop); // turned on again in the Settings app: the switch comes back
    if (!s.radios)
        return {}; // not known yet: again at the next read

    const RadioKind kind = kindOf(id);
    std::vector<Power> powers;
    for (const System::Known& known : *s.radios) {
        if (known.kind != kind)
            continue;
        try {
            powers.push_back(powerOf(known.radio.State()));
        } catch (const winrt::hresult_error& e) {
            qWarning().noquote() << "SwitchRadios: a radio not read:" << describe(e);
            s.moved->store(true); // gone meanwhile, most likely
        }
    }
    const bool wlanOn = kind == RadioKind::WiFi && std::ranges::find(powers, Power::On) != powers.end();
    return stateOf(id, s.access, powers, wlanOn && wlanConnected(), wlanOn && internetThroughWlan());
}

SwitchWrite SwitchRadios::write(const QString& id, const SwitchIntent& intent)
{
    SwitchWrite result;
    result.after = read(id);
    if (intent.kind != SwitchIntent::Kind::On && intent.kind != SwitchIntent::Kind::Off) {
        qWarning() << "SwitchRadios: only on or off, not" << static_cast<int>(intent.kind);
        return result;
    }
    if (!result.after.ready())
        return result; // no radio, denied, a hardware switch: its note says why
    const bool on = intent.kind == SwitchIntent::Kind::On;
    const SwitchState::Value target = on ? SwitchState::Value::On : SwitchState::Value::Off;
    if (result.after.on == target) {
        result.ok = true;
        return result;
    }

    System& s = system();
    const RadioKind kind = kindOf(id);
    const RadioState to = on ? RadioState::On : RadioState::Off;
    QString why; // the first reason one of them was not set
    for (const System::Known& known : *s.radios) {
        if (known.kind != kind)
            continue;
        try {
            const RadioState now = known.radio.State();
            if (now == to || now == RadioState::Disabled)
                continue;
            const std::optional<RadioAccessStatus> status = finish(known.radio.SetStateAsync(to), m_stop, kWait);
            if (!status) {
                qWarning() << "SwitchRadios:" << id << "not switched in time";
                if (why.isEmpty())
                    why = QCoreApplication::translate("ws::SwitchRadios", "Windows 没有回应");
            } else if (*status == RadioAccessStatus::Allowed) {
                result.wrote = true;
            } else {
                qWarning() << "SwitchRadios:" << id << "not switched:" << static_cast<int>(*status);
                if (*status == RadioAccessStatus::DeniedByUser)
                    s.access = Access::DeniedByUser; // read back as denied: Enter opens the page
                if (why.isEmpty())
                    why = QCoreApplication::translate("ws::SwitchRadios", "Windows 不让改");
            }
        } catch (const winrt::hresult_error& e) {
            qWarning().noquote() << "SwitchRadios:" << id << "not switched:" << describe(e);
            s.moved->store(true);
            if (why.isEmpty())
                why = QCoreApplication::translate("ws::SwitchRadios", "Windows 没有回应");
        }
    }
    result.after = read(id);
    result.ok = result.after.ready() && result.after.on == target;
    if (!result.ok && result.after.ready())
        result.error = failure(id, on, why.isEmpty() ? QCoreApplication::translate("ws::SwitchRadios", "被系统改回去了") : why);
    return result;
}

void SwitchRadios::close()
{
    m_relay->close(); // a call under way has told Switches, or tells nobody
    if (!m_system)
        return;
    try {
        if (m_system->devices) {
            const DeviceWatcherStatus status = m_system->devices.Status();
            if (status == DeviceWatcherStatus::Started || status == DeviceWatcherStatus::EnumerationCompleted)
                m_system->devices.Stop();
        }
    } catch (const winrt::hresult_error& e) {
        qWarning().noquote() << "SwitchRadios: the device watcher not stopped:" << describe(e);
    }
    m_system.reset(); // every handler taken off, the radios let go
}

} // namespace ws

#pragma once

#include "SwitchBackend.h"

#include <QCoreApplication>
#include <QStringView>

#include <algorithm>
#include <memory>
#include <span>
#include <stop_token>

namespace ws {

// Bluetooth and WLAN: the switches "bluetooth" and "wlan"
// (docs/design-system-switches.md 5.3), the radios of Windows.Devices.Radios
// as the quick settings switch them, through C++/WinRT on the switch thread.
// Several of a kind (a USB adapter besides the built-in one) go together: on
// when any is on, each written. In the user's own rights; denied while
// "让应用控制设备无线电" is off, and Enter opens that page then.
//
// Listed at the first read, again when a radio came or went (a device
// watcher) or the last list is over 30 s old; access is asked for with each
// list, and at every read while it is not allowed. Watched: each radio's
// StateChanged, and the network's status for WLAN's second line ("已连接",
// "未连接", from the 802.11 adapters) and whether the internet goes through
// it (the footer's "会断网"). They all call on threads of their own: passed
// on through a SwitchRelay.
//
// A write waits up to 5 s (Bluetooth on: ~0.3 s), giving up as the switch
// thread ends, and reads back.
class SwitchRadios final : public SwitchBackend {
public:
    static constexpr QStringView kBluetooth = u"bluetooth";
    static constexpr QStringView kWlan = u"wlan";

    // Windows' RadioAccessStatus and RadioState, for stateOf.
    enum class Access : quint8 { Unspecified, Allowed, DeniedByUser, DeniedBySystem };
    enum class Power : quint8 { Unknown, On, Off, Disabled };

    SwitchRadios();
    ~SwitchRadios() override;

    QStringList ids() const override;
    void setStopToken(std::stop_token stop) override;
    void watch(Notify changed) override;
    SwitchState read(const QString& id) override;
    SwitchWrite write(const QString& id, const SwitchIntent& intent) override;
    void close() override;

    // The state for the radios of `id`'s kind, as read() makes it (tests).
    // Disabled: by a hardware switch or the firmware. WLAN while on:
    // `connected` for its second line, `carriesInternet` for the footer.
    [[nodiscard]] static SwitchState stateOf(QStringView id, Access access, std::span<const Power> radios,
        bool connected = false, bool carriesInternet = false)
    {
        using Availability = SwitchState::Availability;
        const auto any = [radios](Power power) { return std::ranges::find(radios, power) != radios.end(); };
        SwitchState state;
        if (access == Access::DeniedByUser) {
            state.availability = Availability::Denied;
            state.note = QCoreApplication::translate("ws::SwitchRadios", "“让应用控制设备无线电”已关闭");
            state.openInstead = QStringLiteral("ms-settings:privacy-radios");
        } else if (access == Access::DeniedBySystem) {
            state.availability = Availability::Blocked;
            state.note = QCoreApplication::translate("ws::SwitchRadios", "系统不让应用控制无线电");
        } else if (radios.empty()) {
            state.availability = Availability::NoDevice;
            state.note = id == kBluetooth ? QCoreApplication::translate("ws::SwitchRadios", "这台电脑没有蓝牙")
                                          : QCoreApplication::translate("ws::SwitchRadios", "这台电脑没有 WLAN");
        } else if (std::ranges::all_of(radios, [](Power power) { return power == Power::Disabled; })) {
            state.availability = Availability::Blocked;
            state.note = QCoreApplication::translate("ws::SwitchRadios", "被硬件开关关闭");
        } else {
            state.availability = Availability::Ready;
            state.on = any(Power::On) ? SwitchState::Value::On
                : any(Power::Off)     ? SwitchState::Value::Off
                                      : SwitchState::Value::Unknown;
            if (id == kWlan && state.on == SwitchState::Value::On) {
                state.note = connected ? QCoreApplication::translate("ws::SwitchRadios", "已连接")
                                       : QCoreApplication::translate("ws::SwitchRadios", "未连接");
                state.carriesInternet = carriesInternet;
            }
        }
        return state;
    }

private:
    struct System;
    System& system(); // made at the first read, on the switch thread

    std::unique_ptr<System> m_system; // switch thread; let go in close()
    std::shared_ptr<SwitchRelay> m_relay = std::make_shared<SwitchRelay>();
    std::stop_token m_stop;
};

} // namespace ws

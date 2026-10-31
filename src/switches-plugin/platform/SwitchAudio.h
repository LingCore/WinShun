#pragma once

#include "SwitchBackend.h"

#include <QCoreApplication>
#include <QStringView>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>

namespace ws {

// The volume of the default output (Core Audio, eRender/eConsole): the switch
// "volume" (docs/design-system-switches.md 5.4). Its level is the number and
// the bar on the row, as Windows' volume flyout rounds it; its switch is the
// sound: on audible, off muted. The second line names the device (it may be
// a virtual one: ToDesk, VB-Cable).
//
// Written on the switch thread with Win顺's own event context, the level
// before the mute: "音量 30" while muted is heard at 30 only. A level asked
// unmutes (SetLevel, a step up); LevelOnly (a step down) leaves the mute as
// it is. Nothing is written when it already is so (the same values come
// back silently: S_FALSE, S_OK, no callback).
//
// Watched: the endpoint's volume callback (Win顺's own writes skipped by
// their context: read back already), and the device enumerator for another
// default output, a device's state, a new name; the endpoint is bound again
// at the next read, on the switch thread (never from within those callbacks).
class SwitchAudio final : public SwitchBackend {
public:
    static constexpr QStringView kId = u"volume";

    SwitchAudio();
    ~SwitchAudio() override;

    QStringList ids() const override;
    void watch(Notify changed) override;
    SwitchState read(const QString& id) override;
    SwitchWrite write(const QString& id, const SwitchIntent& intent) override;
    void close() override;

    // The level shown for Core Audio's scalar (0.0–1.0): the flyout's number.
    [[nodiscard]] static int levelOf(float scalar) { return std::clamp(qRound(scalar * 100.0f), 0, 100); }

    struct Target {
        int level = 0;
        bool muted = false;
        friend bool operator==(const Target&, const Target&) = default;
    };
    // Where `intent` takes a volume at `level`, `muted` (tests): within
    // 0–100; a level asked unmutes, a step down or LevelOnly does not. None
    // for what is not the volume's (a toggle, which Switches resolves).
    [[nodiscard]] static std::optional<Target> targetOf(int level, bool muted, const SwitchIntent& intent)
    {
        using Kind = SwitchIntent::Kind;
        const auto within = [](int value) { return std::clamp(value, 0, 100); };
        switch (intent.kind) {
        case Kind::On:
            return Target {.level = level, .muted = false};
        case Kind::Off:
            return Target {.level = level, .muted = true};
        case Kind::SetLevel:
            return Target {.level = within(intent.value), .muted = false};
        case Kind::LevelOnly:
            return Target {.level = within(intent.value), .muted = muted};
        case Kind::StepLevel:
            return Target {.level = within(level + intent.value), .muted = intent.value > 0 ? false : muted};
        default:
            return std::nullopt;
        }
    }

    // The state read (tests): `device` the second line.
    [[nodiscard]] static SwitchState stateOf(int level, bool muted, const QString& device)
    {
        SwitchState state;
        state.availability = SwitchState::Availability::Ready;
        state.on = muted ? SwitchState::Value::Off : SwitchState::Value::On;
        state.level = static_cast<qint8>(std::clamp(level, 0, 100));
        state.note = device;
        return state;
    }
    // No output device (unplugged, all disabled): Enter opens the sound settings.
    [[nodiscard]] static SwitchState noDevice()
    {
        SwitchState state;
        state.availability = SwitchState::Availability::NoDevice;
        state.note = QCoreApplication::translate("ws::SwitchAudio", "没有声音输出设备");
        return state;
    }

private:
    struct Endpoint;
    Endpoint* endpoint(); // made at the first read, on the switch thread; null: Core Audio not there

    std::unique_ptr<Endpoint> m_endpoint; // switch thread; let go in close()
    std::shared_ptr<SwitchRelay> m_relay = std::make_shared<SwitchRelay>();
    // Another default output, a device's state or name changed: bound again
    // at the next read. Set by the enumerator's callback, which may outlive us.
    std::shared_ptr<std::atomic<bool>> m_moved = std::make_shared<std::atomic<bool>>(false);
};

} // namespace ws

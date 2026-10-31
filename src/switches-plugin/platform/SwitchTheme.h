#pragma once

#include "SwitchBackend.h"

#include <QCoreApplication>
#include <QStringView>

#include <memory>

namespace ws {

// Dark mode, the apps and Windows together: the switch "theme"
// (docs/design-system-switches.md 5.1), what the Settings app's "选择模式"
// sets. Read from HKCU …\Themes\Personalize: AppsUseLightTheme and
// SystemUsesLightTheme (DWORD, missing means 1: light). Both 0: on (dark);
// both 1: off; unlike: Mixed ("自定义"). Blocked under high contrast, whose
// colours win anyway.
//
// Written on the switch thread, both values, then read back. The registry
// alone changes nothing on the screen, so on the GUI thread (written) Win顺's
// own windows are told at once and every other program after, from a thread
// of its own (SettingBroadcast). In the user's own rights: HKCU is the
// user's here, never through the index service (whose HKCU is SYSTEM's).
// Win顺 follows when its theme is 跟随系统 (Qt's colour scheme changes).
//
// Watched: the Personalize key (programs that only write the registry), and
// WM_SETTINGCHANGE for high contrast and ImmersiveColorSet.
class SwitchTheme final : public SwitchBackend {
public:
    static constexpr QStringView kId = u"theme";

    SwitchTheme();
    ~SwitchTheme() override;

    QStringList ids() const override;
    void watch(Notify changed) override;
    SwitchState read(const QString& id) override;
    SwitchWrite write(const QString& id, const SwitchIntent& intent) override;
    void written(const QString& id) override;

    // The state for these settings, as read() makes it (tests). Mixed: a
    // toggle goes to the opposite of the apps' value (the apps' windows are
    // most of what one sees), both written.
    [[nodiscard]] static SwitchState stateOf(bool appsDark, bool systemDark, bool highContrast)
    {
        SwitchState state;
        state.availability = highContrast ? SwitchState::Availability::Blocked : SwitchState::Availability::Ready;
        state.on = appsDark != systemDark ? SwitchState::Value::Mixed
            : appsDark                    ? SwitchState::Value::On
                                          : SwitchState::Value::Off;
        if (state.on == SwitchState::Value::Mixed) {
            state.note = appsDark ? QCoreApplication::translate("ws::SwitchTheme", "自定义：应用深色，系统浅色")
                                  : QCoreApplication::translate("ws::SwitchTheme", "自定义：应用浅色，系统深色");
            state.mixedToggleOn = !appsDark;
        }
        if (highContrast) {
            state.note = QCoreApplication::translate("ws::SwitchTheme", "对比度主题开着时，深浅色不起作用");
            state.openInstead = QStringLiteral("ms-settings:easeofaccess-highcontrast");
        }
        return state;
    }

private:
    struct Watch;
    std::unique_ptr<Watch> m_watch; // GUI thread
};

} // namespace ws

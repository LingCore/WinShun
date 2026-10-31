#include "SwitchTheme.h"

#include "platform/RegistryWatch.h"
#include "platform/SettingBroadcast.h"

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QDebug>

#include <windows.h>

#include <utility>

namespace ws {

namespace {

constexpr wchar_t kPersonalize[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kApps[] = L"AppsUseLightTheme";
constexpr wchar_t kSystem[] = L"SystemUsesLightTheme";

bool dark(const wchar_t* value)
{
    DWORD light = 1; // missing: light
    DWORD size = sizeof light;
    if (::RegGetValueW(HKEY_CURRENT_USER, kPersonalize, value, RRF_RT_REG_DWORD, nullptr, &light, &size)
        != ERROR_SUCCESS)
        return false;
    return light == 0;
}

bool highContrast()
{
    HIGHCONTRASTW contrast {sizeof contrast};
    return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON);
}

} // namespace

// On the GUI thread: the registry key, and the WM_SETTINGCHANGE that every
// top-level window gets (high contrast changes no value of that key).
struct SwitchTheme::Watch final : QAbstractNativeEventFilter {
    Notify changed;
    std::unique_ptr<RegistryWatch> key;

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr*) override
    {
        const auto* msg = static_cast<const MSG*>(message);
        if (eventType != "windows_generic_MSG" || !msg || msg->message != WM_SETTINGCHANGE)
            return false;
        if (msg->wParam == SPI_SETHIGHCONTRAST
            || (msg->lParam && ::lstrcmpiW(reinterpret_cast<LPCWSTR>(msg->lParam), L"ImmersiveColorSet") == 0))
            changed(kId.toString()); // once per window: Switches reads it once or twice
        return false;
    }
};

SwitchTheme::SwitchTheme() = default;

SwitchTheme::~SwitchTheme() = default; // the filter takes itself off the application

QStringList SwitchTheme::ids() const
{
    return {kId.toString()};
}

void SwitchTheme::watch(Notify changed)
{
    m_watch = std::make_unique<Watch>();
    m_watch->changed = std::move(changed);
    m_watch->key = std::make_unique<RegistryWatch>(kPersonalize, [this] { m_watch->changed(kId.toString()); });
    if (QCoreApplication* app = QCoreApplication::instance())
        app->installNativeEventFilter(m_watch.get());
}

SwitchState SwitchTheme::read(const QString&)
{
    return stateOf(dark(kApps), dark(kSystem), highContrast());
}

SwitchWrite SwitchTheme::write(const QString& id, const SwitchIntent& intent)
{
    SwitchWrite result;
    result.after = read(id);
    if (intent.kind != SwitchIntent::Kind::On && intent.kind != SwitchIntent::Kind::Off) {
        qWarning() << "SwitchTheme: only on or off, not" << static_cast<int>(intent.kind);
        return result;
    }
    if (!result.after.ready())
        return result; // high contrast: its note says why
    const bool toDark = intent.kind == SwitchIntent::Kind::On;
    const SwitchState::Value target = toDark ? SwitchState::Value::On : SwitchState::Value::Off;
    if (result.after.on == target) {
        result.ok = true;
        return result;
    }
    // Both, as the Settings app does: one of them alone is "自定义".
    const DWORD light = toDark ? 0 : 1;
    LSTATUS refused = ERROR_SUCCESS;
    for (const wchar_t* value : {kApps, kSystem}) {
        const LSTATUS status = ::RegSetKeyValueW(HKEY_CURRENT_USER, kPersonalize, value, REG_DWORD, &light, sizeof light);
        if (status == ERROR_SUCCESS) {
            result.wrote = true;
        } else {
            refused = status;
            qWarning() << "SwitchTheme: cannot write" << QString::fromWCharArray(value) << status;
        }
    }
    result.after = read(id);
    result.ok = result.after.on == target;
    if (!result.ok) {
        const QString why = refused != ERROR_SUCCESS
            ? QCoreApplication::translate("ws::SwitchTheme", "Windows 不让改这项设置")
            : QCoreApplication::translate("ws::SwitchTheme", "被系统改回去了");
        result.error = (toDark ? QCoreApplication::translate("ws::SwitchTheme", "没能切到深色：%1")
                               : QCoreApplication::translate("ws::SwitchTheme", "没能切到浅色：%1"))
                           .arg(why);
    }
    return result;
}

void SwitchTheme::written(const QString&)
{
    settingbroadcast::send(settingbroadcast::Area::ImmersiveColorSet);
}

} // namespace ws

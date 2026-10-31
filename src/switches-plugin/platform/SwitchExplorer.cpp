#include "SwitchExplorer.h"

#include "platform/RegistryWatch.h"
#include "platform/SettingBroadcast.h"

#include <QCoreApplication>
#include <QDebug>

#include <windows.h>
#include <shlobj.h>

#include <utility>

namespace ws {

namespace {

constexpr wchar_t kAdvanced[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

struct Value {
    const wchar_t* name;
    DWORD absent; // what a missing value means
    DWORD shownOn; // the value when the switch is on
    DWORD shownOff;
};

constexpr Value kHidden {L"Hidden", 2, 1, 2};
constexpr Value kHideFileExt {L"HideFileExt", 1, 0, 1};

const Value& valueOf(QStringView id)
{
    return id == SwitchExplorer::kHiddenFiles ? kHidden : kHideFileExt;
}

DWORD readValue(const Value& value)
{
    DWORD data = value.absent;
    DWORD size = sizeof data;
    if (::RegGetValueW(HKEY_CURRENT_USER, kAdvanced, value.name, RRF_RT_REG_DWORD, nullptr, &data, &size)
        != ERROR_SUCCESS)
        return value.absent;
    return data;
}

QString failure(QStringView id, bool on, const QString& why)
{
    const char* text = id == SwitchExplorer::kHiddenFiles
        ? (on ? QT_TRANSLATE_NOOP("ws::SwitchExplorer", "没能打开“显示隐藏的文件”：%1")
              : QT_TRANSLATE_NOOP("ws::SwitchExplorer", "没能关掉“显示隐藏的文件”：%1"))
        : (on ? QT_TRANSLATE_NOOP("ws::SwitchExplorer", "没能打开“显示文件扩展名”：%1")
              : QT_TRANSLATE_NOOP("ws::SwitchExplorer", "没能关掉“显示文件扩展名”：%1"));
    return QCoreApplication::translate("ws::SwitchExplorer", text).arg(why);
}

} // namespace

SwitchExplorer::SwitchExplorer() = default;

SwitchExplorer::~SwitchExplorer() = default;

QStringList SwitchExplorer::ids() const
{
    return {kHiddenFiles.toString(), kFileExtensions.toString()};
}

void SwitchExplorer::watch(Notify changed)
{
    m_watch = std::make_unique<RegistryWatch>(kAdvanced, [changed = std::move(changed)] {
        changed(kHiddenFiles.toString());
        changed(kFileExtensions.toString());
    });
}

SwitchState SwitchExplorer::read(const QString& id)
{
    SwitchState state;
    state.availability = SwitchState::Availability::Ready;
    state.on = onFor(id, readValue(valueOf(id))) ? SwitchState::Value::On : SwitchState::Value::Off;
    return state;
}

SwitchWrite SwitchExplorer::write(const QString& id, const SwitchIntent& intent)
{
    SwitchWrite result;
    result.after = read(id);
    if (intent.kind != SwitchIntent::Kind::On && intent.kind != SwitchIntent::Kind::Off) {
        qWarning() << "SwitchExplorer: only on or off, not" << static_cast<int>(intent.kind);
        return result;
    }
    const bool on = intent.kind == SwitchIntent::Kind::On;
    const SwitchState::Value target = on ? SwitchState::Value::On : SwitchState::Value::Off;
    if (result.after.on == target) {
        result.ok = true;
        return result;
    }
    const bool hidden = id == kHiddenFiles;
    SHELLSTATEW state {};
    if (hidden)
        state.fShowAllObjects = on;
    else
        state.fShowExtensions = on;
    ::SHGetSetSettings(&state, hidden ? SSF_SHOWALLOBJECTS : SSF_SHOWEXTENSIONS, TRUE);
    result.after = read(id);
    result.wrote = result.after.on == target;
    if (!result.wrote) {
        // The way that also works: the value itself, then every window told.
        const Value& value = valueOf(id);
        const DWORD data = on ? value.shownOn : value.shownOff;
        const LSTATUS status = ::RegSetKeyValueW(HKEY_CURRENT_USER, kAdvanced, value.name, REG_DWORD, &data, sizeof data);
        qWarning() << "SwitchExplorer: SHGetSetSettings did not set" << id << "; written directly:" << status;
        result.after = read(id);
        result.wrote = status == ERROR_SUCCESS;
        if (result.wrote)
            settingbroadcast::send(settingbroadcast::Area::ShellState);
        if (result.after.on != target) {
            result.error = failure(id, on,
                status != ERROR_SUCCESS ? QCoreApplication::translate("ws::SwitchExplorer", "Windows 不让改这项设置")
                                        : QCoreApplication::translate("ws::SwitchExplorer", "被系统改回去了"));
            return result;
        }
    }
    result.ok = true;
    return result;
}

} // namespace ws

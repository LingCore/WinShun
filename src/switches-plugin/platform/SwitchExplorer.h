#pragma once

#include "SwitchBackend.h"

#include <QStringView>

#include <memory>

namespace ws {

class RegistryWatch;

// File Explorer's "显示隐藏的文件" and "显示文件扩展名": the switches
// "hidden-files" and "file-extensions" (docs/design-system-switches.md 5.2).
// Read from HKCU …\Explorer\Advanced: Hidden (1 shows hidden files, 2 does
// not; missing: 2) and HideFileExt (0 shows extensions, 1 hides them;
// missing: 1). ShowSuperHidden (protected system files) is never touched.
//
// Written on the switch thread with SHGetSetSettings, which in one call of a
// couple of ms writes the value, tells every window ("ShellState") and has
// the open Explorer windows show the change themselves; the registry alone,
// even with SHChangeNotify, leaves them as they were. It returns nothing:
// the value is read back, and when it did not take, written directly and
// "ShellState" sent from a thread of its own (SettingBroadcast), which works
// too, only slower.
//
// Watched: the Advanced key (one watch for both switches).
class SwitchExplorer final : public SwitchBackend {
public:
    static constexpr QStringView kHiddenFiles = u"hidden-files";
    static constexpr QStringView kFileExtensions = u"file-extensions";

    SwitchExplorer();
    ~SwitchExplorer() override;

    QStringList ids() const override;
    void watch(Notify changed) override;
    SwitchState read(const QString& id) override;
    SwitchWrite write(const QString& id, const SwitchIntent& intent) override;

    // Whether the switch `id` is on for these values of the Advanced key, as
    // read() decides (tests). `value`: Hidden or HideFileExt, absent ones
    // passed as their defaults (2, 1).
    [[nodiscard]] static bool onFor(QStringView id, unsigned long value)
    {
        return id == kHiddenFiles ? value == 1 : value == 0;
    }

private:
    std::unique_ptr<RegistryWatch> m_watch; // GUI thread
};

} // namespace ws

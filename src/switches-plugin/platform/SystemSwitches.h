#pragma once

#include "SwitchBackend.h"

namespace ws {

// The backends of the switches this build can set, for Switches (App) and
// wsbench --switches: dark mode (SwitchTheme), hidden files and file
// extensions (SwitchExplorer), Bluetooth and WLAN (SwitchRadios), the volume
// (SwitchAudio), keep awake (SwitchKeepAwake). `dataDir`: the plugin's own
// folder, where keep awake keeps what it did to the lid until it is put back
// (empty: not kept, wsbench).
[[nodiscard]] SwitchBackends systemSwitchBackends(const QString& dataDir = {});

} // namespace ws

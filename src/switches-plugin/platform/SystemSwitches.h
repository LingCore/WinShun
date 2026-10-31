#pragma once

#include "SwitchBackend.h"

namespace ws {

// The backends of the switches this build can set, for Switches (App) and
// wsbench --switches: dark mode (SwitchTheme), hidden files and file
// extensions (SwitchExplorer), Bluetooth and WLAN (SwitchRadios), the volume
// (SwitchAudio). Keep-awake, the last switch of places.txt, is Unsupported
// until its backend comes: an ordinary place, Enter opens its page.
[[nodiscard]] SwitchBackends systemSwitchBackends();

} // namespace ws

#pragma once

namespace ws::settingbroadcast {

// What changed, as WM_SETTINGCHANGE names it.
enum class Area : unsigned char {
    ImmersiveColorSet, // light or dark mode (SwitchTheme)
    ShellState, // Explorer's view settings, when SHGetSetSettings could not set them (SwitchExplorer)
    Environment, // the PATH in the registry was written (CliSetup): new terminals read it from the registry again
};

// Tells every program that a setting changed (WM_SETTINGCHANGE), Win顺's own
// windows first (docs/design-system-switches.md 5.1). Those of the calling
// thread here and now: their window procedures run before this returns, so
// on the GUI thread Qt (and with it the launcher, the tray icon) changes at
// once. The rest on a thread of its own: Win顺's windows on other threads
// (the taskbar box), then every top-level window (HWND_BROADCAST,
// SMTO_ABORTIFHUNG, at most 1 s a window), which takes seconds for
// ImmersiveColorSet (2.5–3.8 s measured): the GUI thread never waits on
// another thread for it. Asked again meanwhile, that thread sends once more
// when done: never two of them at once. Never waited for, on exit neither
// (Win顺's own windows are among those it waits on): it is detached and uses
// nothing that is destroyed then.
void send(Area area);

// A send is under way or due. A test program that changed a setting waits
// for this to turn false before it ends: otherwise the other programs may
// never hear of the last change.
[[nodiscard]] bool busy() noexcept;

} // namespace ws::settingbroadcast

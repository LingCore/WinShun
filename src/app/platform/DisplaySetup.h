#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringView>

// How this PC's displays are set up, for an export's summary ("display" in
// summary.json): what a reporter would otherwise have to be asked, read here
// instead (docs/pitfalls.md, "截图：遮罩一出来固定卡约 2 秒"). Numbers, names of
// hardware and of settings only: no paths, no titles, no user data.
namespace ws::diagnostics {

// Each active monitor: its name and EDID ids, how it is connected
// (DisplayPort, HDMI, internal …), its mode (pixels, refresh rate), HDR on,
// whether it is the main one. {name, edid, connection, size, hz, hdr, primary}
QJsonArray monitors();

// Windows' graphics switches, as set (absent: never set, the default):
// hardware-accelerated GPU scheduling (HwSchMode), multiplane overlays
// (Dwm OverlayTestMode, 5 = off), the graphics card timeout (TdrDelay,
// TdrLevel), DirectX's per-user switches (variable refresh rate for games,
// flip model for older games), Game Mode and Game DVR.
QJsonObject graphicsSettings();

// The graphics drivers' own switches that bear on frame pacing (FreeSync,
// Enhanced Sync, Anti-Lag, Chill, Boost, HYPR-RX, Fluid Motion Frames, flip
// queue, VRR, G-SYNC …): every value under each display adapter's driver key
// and AMD's per-user key whose name says so, path relative to that key.
// {adapter: {"UMD\\TurboSync": "1", …}}
QJsonObject driverSettings();

// Whether a driver value's name is one of those switches (FreeSync, TurboSync,
// KMD_ChillEnabled, FlipQueueSize, DisableMPO, TdrDelay, EnableUlps …), and
// not a name that only has the letters inside it (RmPowerFeature).
bool isPacingSwitch(QStringView name);

// What Windows wrote about the graphics drivers in the System log, newest
// first, within `days`: a card that stopped answering and was reset (Display
// 4101), the kernel's graphics events (DxgKrnl), the drivers' own (amdkmdag,
// nvlddmkm, igfx …). [{time (UTC), provider, id, level}]
QJsonArray displayEvents(int days = 14, int most = 40);

} // namespace ws::diagnostics

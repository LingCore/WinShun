#pragma once

#include <QColor>

class QWindow;

namespace qf::win {

// Windows 11 look for a frameless window: rounded corners, a thin border in
// the theme colour and the standard DWM shadow.
void styleFramelessWindow(QWindow* window, bool dark, QColor border);

// Dark or light title bar for a normal (framed) window.
void setDarkTitleBar(QWindow* window, bool dark);

// Title bar background and text colour (Windows 11); invalid colours restore
// the system's.
void setTitleBarColors(QWindow* window, QColor caption, QColor text);

// Cloaking keeps a shown window off the screen (DWM does not compose it)
// while it still gets focus and draws as usual.
void setCloaked(QWindow* window, bool cloaked);

// Brings a window to the foreground even when another app owns the focus.
// Windows normally refuses this for background processes (focus stealing
// protection), but a hotkey press is an explicit user request.
void bringToFront(QWindow* window);

// Makes native popup menus (tray, context menu) follow the dark theme.
void setMenuTheme(bool dark);

} // namespace qf::win

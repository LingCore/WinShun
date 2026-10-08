#pragma once

#include <QColor>

class QWindow;

namespace ws::win {

// Windows 11 look for a frameless window: rounded corners, a thin border in
// the theme colour and the standard DWM shadow.
void styleFramelessWindow(QWindow* window, bool dark, QColor border);

// Cloaking keeps a shown window off the screen (DWM does not compose it)
// while it still gets focus and draws as usual.
void setCloaked(QWindow* window, bool cloaked);

// Brings a window to the foreground even when another app owns the focus.
// Windows normally refuses this for background processes (focus stealing
// protection), but a hotkey press is an explicit user request.
void bringToFront(QWindow* window);

// Makes native popup menus (tray, context menu) follow the dark theme.
void setMenuTheme(bool dark);

// Loads the input method (TSF text services) into the calling thread ahead of
// time. Windows does that when the thread first gets the focus, which made
// the first tray menu about 14 ms slower (a Chinese IME brings a dozen DLLs).
// Nothing shows; call it on the GUI thread.
void prepareTextInput();

} // namespace ws::win

#pragma once

#include <QColor>
#include <QRect>

#include <windows.h>

#include <cstdint>

class QWindow;

namespace ws::win {

// Windows 11 look for a frameless window: rounded corners and the standard
// DWM shadow and system-managed border, in both square and rounded modes.
// Windows 10 has square corners with the same DWM shadow, and WindowEdge.qml
// draws the border. Windows 11 outlines follow the actual native corners,
// including desktop customization. With `backdrop`, the Mica material fills
// the window behind its content (see backdropSupported). The dark mode is
// separate, see setDarkFrame.
void styleFramelessWindow(QWindow* window, bool backdrop = false);

enum class BackdropMaterial { Mica, Acrylic };
// Updates existing backdrop windows as well as windows created afterwards.
// Opaque menus and windows never opt into a backdrop here.
void setBackdropMaterial(BackdropMaterial material);
// Visual activation only; never changes the foreground window or input focus.
// Liquid Acrylic keeps its material while inactive; Mica follows activation.
bool backdropFrameActive(HWND window, bool active);

// Whether DWM rounds the corners (Windows 11), see styleFramelessWindow.
bool nativeWindowCorners();
// Developer preview only, before making any windows. Shows the Windows 10
// frame without changing the user's OS or saved preferences.
void useCompositedFramesForPreview();

// The appearance preference. Windows 10 keeps the windows' own corners
// square (see styleFramelessWindow); the controls inside still follow it.
bool roundedCorners();

// Returns whether the preference changed; also updates existing styled HWNDs.
bool setRoundedCorners(bool on);
// Native preview pages use smaller corners. Remember the choice for live updates.
void setWindowCorners(HWND window, bool smallCorners = false);

// Dark mode for the frame and its backdrop (DWMWA_USE_IMMERSIVE_DARK_MODE);
// Mica follows it at once. Qt sets it too, a few milliseconds after a theme
// change and after every change of the application palette (an accent
// colour, the scheme the same), and for a frameless window always to light
// (qwindowswindow.cpp: shouldApplyDarkFrame). Set it once Qt is done (see
// App::guardDarkFrames): set both ways within one composed frame, DWM keeps
// Mica in the old tint.
void setDarkFrame(QWindow* window, bool dark);
bool isDarkFrame(QWindow* window);
// From now on, as the window is shown and before its first frame, its dark
// mode back to the colour scheme's if Qt has set it light meanwhile. Shown
// without the focus (the clipboard, over the program it pastes into), it
// gets no WM_NCACTIVATE (see WindowFrame) to set it back, and opened white
// over a dark theme (GitHub #120).
void keepDarkFrameOnShow(QWindow* window);

// Whether Windows can draw Mica behind a window, the material of long-lived
// windows such as its own Settings: Windows 11 22H2 or later
// (DWMWA_SYSTEMBACKDROP_TYPE). It shows where the window's own pixels are
// transparent, so the window needs an alpha channel, and DWM must see the
// frame activate (WM_NCACTIVATE, see WindowFrame); its tint follows
// setDarkFrame. Inactive Mica falls back to grey and should be covered.
// Acrylic keeps visual frame activation (backdropFrameActive), while actual
// focus is unchanged. System material/power policies still control rendering.
bool backdropSupported();

// Whether Windows draws materials at all: transparency effects on (Settings >
// Personalization > Colors) and no high contrast theme.
bool materialsEnabled();

// Cloaking keeps a shown window off the screen (DWM does not compose it)
// while it still gets focus and draws as usual.
void setCloaked(QWindow* window, bool cloaked);

// No DWM fade as the window is shown or hidden: hidden, it is off the screen
// in the next composition, not some 200 ms later (a capture must not catch it).
void disableTransitions(QWindow* window);

// Windows' Start menu, search, notification center or quick settings: they
// stay above every other window while open.
bool isShellFlyout(HWND window);
// Includes visible companion windows, even from another Shell host (Start's
// Phone Link sidebar). Uses window regions for full-monitor transparent hosts.
// Physical screen pixels; empty for non-flyouts.
QRect shellFlyoutBounds(HWND window);

// How bringToFront went: in front already; directly; after an input that
// does nothing (Windows lets the process that sent the latest input set the
// foreground); or refused (an administrator's window or a hung one's ghost
// in front, their input out of reach, UIPI).
enum class Front : std::uint8_t { Already, Direct, Input, Refused };
enum class ShellFlyout { Dismiss, Preserve };

// Shows or restores a requested window, then brings it to the foreground
// even when another app owns the focus. Windows normally refuses this to
// background processes (focus stealing protection), but a hotkey press is
// an explicit user request. Never waits for the program in front, however
// busy it is, and knows at once whether it worked (GitHub #111).
// Preserve suppresses the synthetic Escape used for Shell flyouts. Callers
// that must keep the old field open should normally show without activation;
// an explicit foreground activation may still dismiss a system popup itself.
Front bringToFront(QWindow* window, ShellFlyout flyout = ShellFlyout::Dismiss);

// Makes native popup menus (tray, context menu) follow the dark theme.
void setMenuTheme(bool dark);

// Loads the input method (TSF text services) into the calling thread ahead of
// time. Windows does that when the thread first gets the focus, which made
// the first tray menu about 14 ms slower (a Chinese IME brings a dozen DLLs).
// Nothing shows; call it on the GUI thread.
void prepareTextInput();

// Whether a keyboard layout (GetKeyboardLayout) is for Chinese: Simplified
// or Traditional, whichever input method.
bool isChineseLayout(HKL layout);

} // namespace ws::win

#pragma once

#include <QRect>

#include <windows.h>

namespace ws::capture {

// Our own window is left out of screen pictures (WDA_EXCLUDEFROMCAPTURE) only
// when it would be in the frames being taken: `capturing` and its rectangle
// (physical px), grown by its DWM shadow, meets `region` (physical px, virtual
// desktop). The shadow lies outside GetWindowRect: a panel 15 px beside the
// selection darkened the long screenshot's edge (2026-11-02). Otherwise it is
// shown like any window, so a screenshot tool, theirs or ours, sees it.
// `shadow`: logical px, more than the DWM shadow reaches; 0 for a window
// without one (a layered popup such as the long screenshot's border).
inline void excludeFromCapture(HWND window, const QRect& region, bool capturing, int shadow = 64)
{
    if (!window)
        return;
    RECT bounds {};
    const bool inFrames = capturing && ::IsWindowVisible(window) && ::GetWindowRect(window, &bounds)
        && [&] {
               const int grow = ::MulDiv(shadow, int(::GetDpiForWindow(window)), 96);
               return QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top)
                   .adjusted(-grow, -grow, grow, grow).intersects(region);
           }();
    const DWORD wanted = inFrames ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    DWORD current = WDA_NONE;
    if (!::GetWindowDisplayAffinity(window, &current) || current != wanted)
        ::SetWindowDisplayAffinity(window, wanted);
}

} // namespace ws::capture

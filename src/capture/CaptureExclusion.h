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
inline void excludeFromCapture(HWND window, const QRect& region, bool capturing)
{
    if (!window)
        return;
    constexpr int kShadow = 64; // logical px, more than the DWM shadow reaches
    RECT bounds {};
    const bool inFrames = capturing && ::IsWindowVisible(window) && ::GetWindowRect(window, &bounds)
        && [&] {
               const int shadow = ::MulDiv(kShadow, int(::GetDpiForWindow(window)), 96);
               return QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top)
                   .adjusted(-shadow, -shadow, shadow, shadow).intersects(region);
           }();
    const DWORD wanted = inFrames ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    DWORD current = WDA_NONE;
    if (!::GetWindowDisplayAffinity(window, &current) || current != wanted)
        ::SetWindowDisplayAffinity(window, wanted);
}

} // namespace ws::capture

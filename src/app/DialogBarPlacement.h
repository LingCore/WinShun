#pragma once

#include <algorithm>
#include <initializer_list>
#include <optional>

namespace ws::dialogbar {

enum class Side { Auto, Below, Left, Right };

// Physical pixels throughout. Where the bar's top goes under a dialog whose
// visible frame ends at `frameBottom`: `gap` below it, as far down as the
// screen goes (`screenBottom`, past the work area's `workBottom`: over a
// taskbar at the bottom, as Listary does, #362). Where the screen ends sooner
// it may come up over the dialog's bottom edge by a quarter of its height, no
// more: nothing when not even that fits (a maximised dialog with the taskbar
// hidden or elsewhere).
inline std::optional<int> barTopUnder(int frameBottom, int workBottom, int screenBottom, int gap, int bar)
{
    if (frameBottom + gap + bar <= workBottom)
        return frameBottom + gap;
    const int top = std::min(frameBottom + gap, screenBottom - bar);
    if (top < frameBottom - bar / 4)
        return std::nullopt;
    return top;
}

// Physical pixels throughout. Below (Auto or set) while the bar itself fits
// under the dialog (barFitsBelow: barTopUnder), its list opening upward where
// it has no room under it: the bar stays put as the dialog moves and grows
// (#362). A side set: there where it fits (sideNeeded), else below by two
// rows (belowNeeded), keeping where it is while that still fits. Otherwise the
// first that fits of below, right and left; below if none does.
inline Side chooseSide(bool barFitsBelow, int below, int left, int right, int belowNeeded, int sideNeeded,
    Side preferred, Side current)
{
    using enum Side;
    const auto fits = [&](Side side) {
        return side == Below ? below >= belowNeeded : side == Left ? left >= sideNeeded
            : side == Right ? right >= sideNeeded : false;
    };
    if ((preferred == Auto || preferred == Below) && barFitsBelow)
        return Below;
    if (preferred != Auto && fits(preferred))
        return preferred;
    if (fits(current))
        return current;
    for (Side side : {Below, Right, Left}) {
        if (fits(side))
            return side;
    }
    return Below;
}

} // namespace ws::dialogbar

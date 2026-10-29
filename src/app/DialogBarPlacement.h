#pragma once

#include <initializer_list>

namespace ws::dialogbar {

enum class Side { Auto, Below, Left, Right };

// Physical pixels throughout. Below fits with room for the bar and two rows
// (belowNeeded), a side with sideNeeded. Keep a side until it no longer fits;
// returning below needs extra room so a few pixels of movement cannot keep
// swapping it. Set to below, the bar stays there while the bar itself fits
// (barNeeded), its list opening upward; beside only when not even that does.
inline Side chooseSide(int below, int left, int right, int belowNeeded, int barNeeded, int sideNeeded,
    int hysteresis, Side preferred, Side current)
{
    using enum Side;
    const auto fits = [&](Side side) {
        return side == Below ? below >= belowNeeded : side == Left ? left >= sideNeeded
            : side == Right ? right >= sideNeeded : false;
    };
    if (preferred == Below && below >= barNeeded)
        return Below;
    if (preferred != Auto && fits(preferred))
        return preferred;
    if (fits(current)) {
        if (preferred == Auto && current != Below && below >= belowNeeded + hysteresis)
            return Below;
        return current;
    }
    for (Side side : {Below, Right, Left}) {
        if (fits(side))
            return side;
    }
    return Below;
}

} // namespace ws::dialogbar

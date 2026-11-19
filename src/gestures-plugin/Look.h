// Win顺's look as the plugin draws it (the appearance's nativeLook, see
// docs/plugins.md "appearance"): the trail, the hint, the cards.
#pragma once

#include <cstdint>

namespace gestures {

// Colours are 0xAARRGGBB; radii in logical pixels (0 with square corners).
struct Look {
    bool chinese = true;
    bool dark = false;
    bool rounded = true; // the corners setting: pills become squares when false
    std::uint32_t accent = 0xff0078d4; // the trail
    std::uint32_t accentFill = 0xff0078d4; // the hint's fill, a card's main button
    std::uint32_t onAccent = 0xffffffff; // words on accentFill
    std::uint32_t muted = 0xf0404040; // the hint of a gesture with no action
    std::uint32_t background = 0xfff9f9f9; // a card
    std::uint32_t text = 0xff1a1a1a;
    std::uint32_t subtext = 0xff5c5c5c;
    std::uint32_t edge = 0x1a000000; // a card's 1-pixel outline
    std::uint32_t control = 0xfffbfbfb; // a card's plain button
    std::uint32_t controlHover = 0xfff0f0f0;
    std::uint32_t controlBorder = 0x24000000;
    int cardRadius = 8;
    int controlRadius = 4;
};

} // namespace gestures

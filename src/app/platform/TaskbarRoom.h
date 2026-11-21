#pragma once

#include <windows.h>

#include <algorithm>
#include <optional>
#include <span>

namespace ws::taskbar {

// Where on the taskbar the user wants Win顺's search box (GitHub #271).
enum class Side {
    // Out of the icons' way as Windows lays them out: at the taskbar's left
    // end while they are centred, beside the tray while they are at the left.
    Auto,
    // Beside the tray whichever way the icons are: with centred icons too,
    // for a pointer that lives at that end of the screen.
    Tray,
};

// The stretch of a horizontal taskbar the box goes in, physical pixels, and
// the end of it the box sits at.
struct Stretch {
    bool leftOfIcons = false; // the box at the stretch's left end; else right of the icons, at its right end
    double from = 0;
    double to = 0;
};

// The stretch by what is on the taskbar `bar`: `icons` (Start, Windows'
// search, the apps; none read: the whole bar counts as free), whether they
// are `centred`, and the `obstacles` at its ends (the tray, the widgets'
// button; Start on a classic taskbar). Left of the icons only when they are
// centred and the user leaves the side to Win顺: from the taskbar's left end
// or what is there. Else right of them, up to what is there. Never between
// icons, and never the left of icons that begin at the taskbar's end. May be
// empty or inverted where there is no room: the caller fits a form into it.
constexpr Stretch stretchFor(const RECT& bar, const std::optional<RECT>& icons, bool centred, std::span<const RECT> obstacles,
    Side side) noexcept
{
    Stretch stretch {icons && centred && side == Side::Auto, static_cast<double>(bar.left), static_cast<double>(bar.right)};
    if (stretch.leftOfIcons) {
        stretch.to = icons->left;
        for (const RECT& o : obstacles) {
            if (o.right <= stretch.to)
                stretch.from = std::max<double>(stretch.from, o.right);
        }
    } else {
        stretch.from = icons ? icons->right : bar.left;
        for (const RECT& o : obstacles) {
            if (o.left >= stretch.from)
                stretch.to = std::min<double>(stretch.to, o.left);
        }
    }
    return stretch;
}

// The widest form the user lets the taskbar search box take, and the form it
// actually fits into. Narrower ones still where a wider one does not fit.
enum class Form { Box, Label, Icon };

struct Shape {
    Form form = Form::Box;
    double width = 0; // physical pixels
};

struct ShapeLimits {
    double fullBox = 0; // preferred box width, physical pixels
    double compactBox = 0; // narrower typing box before falling back to Label (0: none)
    double label = 0; // icon and short label (0: none)
    double button = 0; // icon-only taskbar button slot
    double buttonGap = 0; // gap reclaimed at each end by the icon-only slot
};

// The widest form a stretch `width` wide takes (no wider than the user's
// `style`, `widest`, or `maxBoxWidth` when > 0): the full box, else a compact
// typing box, else icon + label, else the icon alone.
constexpr std::optional<Shape> fitShape(double width, Form style, Form widest, const ShapeLimits& limits,
    double maxBoxWidth = 0) noexcept
{
    const auto may = [&](Form f) {
        return static_cast<int>(f) >= static_cast<int>(style) && static_cast<int>(f) >= static_cast<int>(widest);
    };
    if (may(Form::Box)) {
        const auto boxFits = [&](double w) { return w > 0 && width >= w && (maxBoxWidth <= 0 || w <= maxBoxWidth); };
        if (boxFits(limits.fullBox))
            return Shape {Form::Box, limits.fullBox};
        if (boxFits(limits.compactBox))
            return Shape {Form::Box, limits.compactBox};
    }
    if (may(Form::Label) && limits.label > 0 && width >= limits.label)
        return Shape {Form::Label, limits.label};
    if (limits.button > 0 && width + 2 * limits.buttonGap >= limits.button)
        return Shape {Form::Icon, limits.button};
    return std::nullopt;
}

} // namespace ws::taskbar

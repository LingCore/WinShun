// The line round a window winshun.topmost keeps on top (main.cpp): which
// windows are on top shows without asking. Drawn just inside the window's
// edge, so a maximized window has it too; it follows the window as it moves,
// is resized, minimized or goes to another desktop, and goes with it. It
// takes no click and no focus, and has a thread of its own, as Note has.
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace topmost {

// The corners Windows gives the window (DWMWA_WINDOW_CORNER_PREFERENCE).
enum class Corners {
    Default, // Windows decides: round for a window with a frame
    Square,
    Round,
    Small,
};

// What decides the line: plain facts, so tests/tst_topmost.cpp can go
// through them. Physical pixels.
struct Shape {
    RECT frame {}; // the window as seen (without its invisible sizing edges)
    RECT monitor {}; // the whole monitor it is on
    RECT work {}; // and that monitor without its taskbar
    float scale = 1; // of that monitor
    bool shown = false; // visible, not minimized, on this desktop
    bool maximized = false;
    bool framed = false; // has a caption or a sizing frame
    bool roundsCorners = false; // Windows 11
    Corners corners = Corners::Default;
};

struct Outline {
    RECT frame {};
    int thickness = 0;
    int radius = 0; // of the outer edge; 0: square

    friend bool operator==(const Outline& a, const Outline& b) noexcept
    {
        return ::EqualRect(&a.frame, &b.frame) && a.thickness == b.thickness && a.radius == b.radius;
    }
};

// No line for a window not seen, or one that fills its monitor (full screen:
// a film, a slide show, a game). A maximized one has it on the edge of the
// work area.
std::optional<Outline> outlineFor(const Shape& shape) noexcept;

// The line as four pieces, so that no window the size of the whole one is
// kept for a few pixels of edge: top and bottom (with the corners), left and
// right between them. An empty one is not shown.
enum Strip : std::size_t { Top, Bottom, Left, Right };
std::array<RECT, 4> stripsOf(const Outline& outline) noexcept;

Shape shapeOf(HWND window);

class Borders {
public:
    Borders();
    ~Borders();
    Borders(const Borders&) = delete;
    Borders& operator=(const Borders&) = delete;

    // 0xAARRGGBB; the lines there are change with it.
    void setColour(std::uint32_t argb);
    // The window has its line while it stays on top; once it is not (closed,
    // taken off by anyone), the line goes by itself.
    void add(HWND window);
    void remove(HWND window);
    void clear();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace topmost

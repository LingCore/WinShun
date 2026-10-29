#pragma once

#include <QtGlobal>

#include <windows.h>

#include <initializer_list>

namespace ws::win {

// The app's logo peeking out from behind a window of ours (the launcher, the
// bar by file dialogs), as over a wall: three quarters of it show, and its
// own window ends where that one's edge begins, so the rest is "behind" it.
// Over the top edge it stands upright; at the others it is turned so that
// its leaf (the top right pane) points straight away from the window: 135°
// either way at the bottom and left, 45° at the right.
// A layered window of its own, as ours can have no see-through part:
// click-through, never activated, on top. Drawn from the exe's icon
// (app.ico, each size drawn on the pixel grid) at the size for the screen.
class PeekingLogo {
public:
    enum class Edge { Top, Bottom, Left, Right }; // of the window, it peeks over

    // Logical pixels: a size app.ico has at 100, 125, 150 and 200 % (32,
    // 40, 48, 64), so the logo is never scaled there.
    static constexpr int kSize = 32;
    static constexpr int kInset = 20; // over the top or bottom: in from the row's left end

    PeekingLogo() = default;
    ~PeekingLogo();
    PeekingLogo(const PeekingLogo&) = delete;
    PeekingLogo& operator=(const PeekingLogo&) = delete;

    // Over an edge of `host`, all in physical pixels on a screen at `scale`:
    // of `edges`, the first where the logo stays in `work` and off `avoid`
    // (the dialog, say), else the first where it stays in `work`; none, it
    // hides. Over the top or bottom it sits kInset in from `row`'s left end;
    // at a side, level with `row`'s middle (the box, in a window with a list).
    // `host` and `row` are where the window rests; sliding in or out it is
    // `shift` off that (the edge stays the one chosen there) and `alpha` faded.
    void show(const RECT& host, const RECT& row, const RECT& work, const RECT* avoid, qreal scale,
        std::initializer_list<Edge> edges = {Edge::Top, Edge::Left, Edge::Right, Edge::Bottom}, POINT shift = {},
        BYTE alpha = 255);
    void hide();
    // 搜索框显示 Logo (GitHub #109): off, show() keeps every logo hidden. One
    // switch for all of them (launcher, clipboard, the bar by file dialogs);
    // the windows lay out the same without it, as the logo is a window of its
    // own outside theirs, so nothing needs to move.
    static void setEnabled(bool on) { s_enabled = on; }
    static bool enabled() { return s_enabled; }
    void setOwner(HWND owner) { m_owner = owner; }

private:
    static inline bool s_enabled = true; // GUI thread only

    bool render(int size, Edge edge); // the part that shows

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr; // null: the launcher's existing topmost logo
    HDC m_dc = nullptr; // holds m_bitmap
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_dcBitmap = nullptr; // the one the DC came with
    int m_size = 0; // of m_bitmap's logo, physical pixels
    Edge m_edge = Edge::Top; // m_bitmap's
};

} // namespace ws::win

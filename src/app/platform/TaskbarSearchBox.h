#pragma once

#include "TaskbarRoom.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace ws::taskbar {

// Win顺's own search box on the taskbar (a setting; the pinned button is the
// other way in, see TaskbarSearch.h). Windows' own search may stay there too.
//
// A window of ours made a child of the taskbar (Shell_TrayWnd), as
// EverythingToolbar does: Windows lets an elevated process put one there,
// and it gets the mouse as any other. It is drawn here with per-pixel alpha
// (a layered window, Direct2D), in the taskbar's light or dark. Typing goes
// to the launcher, which opens over the box with its own field out of sight:
// the box shows what that field has (setText), caret, selection and an input
// method's composition included, and tells where it was pressed (Press), for
// the caret to go there. Windows takes the foreground from whatever window
// has it when the taskbar is clicked, a child of ours or not (MA_NOACTIVATE
// only keeps the taskbar itself from taking it): the launcher is brought back.
//
// It has one place, at an end of the taskbar, out of the icons' way: left
// of them when they are centred, at the taskbar's left end (right of the
// widgets' button, if that is there); right of them when they are at the
// left, against the tray. Icons coming and going leave it where it is: it
// does not slide with them. As wide as that stretch allows, as Windows' own
// search: the whole box, else its icon and a short label, else its icon
// alone, a taskbar button like Windows' search icon (no wider than the
// user's Style), each one width; hidden where not even the icon fits.
// Typing shows in the whole box only (wide); otherwise the launcher opens
// over it with its own field. UI Automation measures that stretch once the
// taskbar has settled after a change (what it says while the icons slide
// runs ahead of the screen, see measureLoop in the .cpp); only icons come
// over the box make it narrower sooner. Explorer restarted, it goes onto
// the new taskbar (reattach, TaskbarCreated; it also looks by itself for a
// while).
//
// Its own animations (hover, press, opening, appearing) go once per frame
// the screen's compositor draws (DwmFlush), with the curves of Windows 11's
// own motion; not when the system's animation effects are off.
//
// On Windows 10's taskbar, as StartAllBack and ExplorerPatcher bring it back
// on Windows 11 (classicTaskList), the same way: the task buttons measured,
// the rest of the taskbar windows in the way (classicChanged).
//
// Its window lives on a thread of its own: a child of another program's
// window shares that program's input queue, and a hung Explorer must not
// take the launcher with it. Measuring is on a third thread: UI Automation
// asks the windows on the taskbar, this one among them; a fourth paces the
// box's animations to the compositor's frames. Windows 11 only (Windows 10
// itself has the pinned button; its taskbar is measured as above, but
// untried there).
class SearchBox {
public:
    // A left button press on the box (on its icon: a click, on release).
    struct Press {
        POINT anchor {}; // the middle of the box's top edge, on the screen (physical pixels)
        bool wide = false; // the whole box (see wide())
        std::optional<int> at; // while active: where in the text it was pressed (UTF-16)
        bool word = false; // a double click: the word there
    };
    // Why it is not on the taskbar (None while it is).
    enum class Hidden {
        None,
        Pending, // going onto it: the taskbar not found or measured yet
        NoTaskbar, // no visible taskbar to go onto, after a while
        Unreadable, // UI Automation does not give its icons (a taskbar neither Windows 11's nor Windows 10's)
        OffScreen, // not all on its screen: auto-hidden, or sliding
        Vertical, // at a side of the screen
        NoRoom, // not even the icon fits at its end of the taskbar
    };
    struct Callbacks {
        std::function<void(const Press&)> pressed;
        std::function<void(int anchor, int at)> dragged; // the button held down, moved: select from anchor
        std::function<void(Hidden why)> shownChanged; // on the taskbar (None), or why not
        std::function<void()> caretMoved; // caret() is somewhere else
        std::function<void(bool wide)> wideChanged; // see wide()
        std::function<void(bool classic)> classicChanged; // measured a classic taskbar (classicTaskList), or no longer
    };
    // What the launcher's field has while it is open over the box.
    struct Text {
        bool active = false; // else the placeholder alone
        std::wstring text;
        int cursor = 0;
        int selectionStart = 0;
        int selectionEnd = 0;
        std::wstring composition; // an input method's, at the cursor
    };
    // The widest form the user lets it take, as Windows' own search has it
    // (Settings › Personalisation › Taskbar › Search): the box, its icon and
    // label, its icon alone. Narrower ones still where that one does not fit.
    enum class Style { Box, Label, Icon };
    using Side = taskbar::Side; // which end of the taskbar it goes to (TaskbarRoom.h)
    struct Look {
        std::wstring name; // for screen readers and the tooltip: "Win顺 搜索"
        std::wstring placeholder;
        std::wstring label; // short, where the box is narrow: "搜索"
        COLORREF accent = RGB(0x00, 0x67, 0xC0); // under selected text
        // The 界面风格's (SystemTheme::nativeLook), 0xAARRGGBB; alpha 0: the
        // box's own. selection: under selected text, as in the launcher's
        // field (textSelection), else the accent above; caret: the style's
        // caret colour, else the text's.
        std::uint32_t selection = 0;
        std::uint32_t caret = 0;
        // The style's search field, which the box stands for (Look
        // searchBoxFill/Edge/Radius): its fill and edge, 0xAARRGGBB (alpha 0:
        // the taskbar's translucent fills, as Windows' own box), drawn only
        // where the taskbar is as light or dark as Win顺 (styleDark); its
        // corner in DIPs (< 0: a pill).
        std::uint32_t fill = 0;
        std::uint32_t edge = 0;
        double radius = -1;
        bool styleDark = false;
        bool monoIcon = false; // Win顺's icon in the taskbar's text colour (monologo), not its own colours
        bool roundedCorners = true;
        Style style = Style::Box;
        Side side = Side::Auto;
    };

    // The callbacks run on the box's own thread.
    SearchBox(Callbacks callbacks, Look look);
    ~SearchBox(); // gone from the taskbar

    SearchBox(const SearchBox&) = delete;
    SearchBox& operator=(const SearchBox&) = delete;

    // Any thread.
    void setLook(Look look);
    void setText(Text text);
    void reattach(); // Explorer started again (TaskbarCreated)
    std::optional<RECT> rect() const; // where it is, on the screen, physical pixels; none while not shown
    bool wide() const; // ... as the whole box, which shows what is typed (else its label or icon alone)
    std::optional<RECT> caret() const; // ... the caret while active
    bool underPointer() const; // the mouse pointer is on it

    static bool supported(); // Windows 11

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl; // shared with its threads: one stuck with a hung Explorer is left behind
};

} // namespace ws::taskbar

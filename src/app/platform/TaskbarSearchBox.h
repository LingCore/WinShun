#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace ws::taskbar {

// Win顺's own search box on the taskbar, next to its icons (a setting; the
// pinned button is the other way in, see TaskbarSearch.h). Windows' own
// search may stay there too.
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
// Placed in a free stretch beside the icons, which UI Automation measures:
// by default left of them when they are centred (against Start), else right
// of them; the user moves it by dragging its icon (Spot). Not shown where it
// would be too narrow. When the taskbar moves its icons (a location event on
// its thread), they are measured again on every frame until they settle, and
// the box moves with them; a jump that would leave it in the clear is taken
// only once the taskbar has drawn it (see follow in the .cpp). Explorer
// restarted, it goes onto the new taskbar (reattach, TaskbarCreated; it also
// looks by itself for a while).
//
// Animated once per frame the screen's compositor draws (DwmFlush), with the
// curves of Windows 11's own motion; not when the system's animation effects
// are off.
//
// Its window lives on a thread of its own: a child of another program's
// window shares that program's input queue, and a hung Explorer must not
// take the launcher with it. Measuring is on a third thread: UI Automation
// asks the windows on the taskbar, this one among them; a fourth paces both
// to the compositor's frames while they animate. Windows 11 only (the
// measuring needs its taskbar).
class SearchBox {
public:
    // A left button press on the box (on its icon: a click, not a drag).
    struct Press {
        POINT anchor {}; // the middle of the box's top edge, on the screen (physical pixels)
        std::optional<int> at; // while active: where in the text it was pressed (UTF-16)
        bool word = false; // a double click: the word there
    };
    // Where the user put the box: on one side of the icons, so far from
    // their edge or from the far end of that stretch (the screen's edge,
    // the widgets, the tray). The default is against the icons, before them
    // when they are centred.
    struct Spot {
        enum class Side { Auto, Before, After }; // before (left of) or after the icons
        Side side = Side::Auto;
        bool fromIcons = true; // else from the far end
        int offset = 0; // logical pixels
    };
    struct Callbacks {
        std::function<void(const Press&)> pressed;
        std::function<void(int anchor, int at)> dragged; // the button held down, moved: select from anchor
        std::function<void(bool shown)> shownChanged; // on the taskbar, or not (no room, no taskbar)
        std::function<void()> caretMoved; // caret() is somewhere else
        std::function<void(const Spot&)> moved; // dragged somewhere else by the user
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
    struct Look {
        std::wstring name; // for screen readers: "Win顺 搜索"
        std::wstring placeholder;
        COLORREF accent = RGB(0x00, 0x67, 0xC0); // under selected text
    };

    // The callbacks run on the box's own thread.
    SearchBox(Callbacks callbacks, Look look, Spot spot);
    ~SearchBox(); // gone from the taskbar

    SearchBox(const SearchBox&) = delete;
    SearchBox& operator=(const SearchBox&) = delete;

    // Any thread.
    void setLook(Look look);
    void setText(Text text);
    void reattach(); // Explorer started again (TaskbarCreated)
    std::optional<RECT> rect() const; // where it settles, on the screen, physical pixels; none while not shown
    std::optional<RECT> caret() const; // ... the caret while active
    bool underPointer() const; // the mouse pointer is on it

    static bool supported(); // Windows 11

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl; // shared with its threads: one stuck with a hung Explorer is left behind
};

} // namespace ws::taskbar

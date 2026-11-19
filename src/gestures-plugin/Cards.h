// What the mouse gestures show besides the trail, on the engine's window
// thread: a short note where something was done ("已复制",
// "左上角 · 任务视图"), the card that asks before shutting down and the
// like, and the bubble a translation comes in. The note is drawn as the
// trail's hint is (GDI+ on a layered window of its own); the card and the
// bubble are windows with Windows' own frame; icons are Phosphor. All in Win顺's
// look; see docs/design-gestures.md.
#pragma once

#include "Look.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace gestures {

// What a note shows before its words: done (Check), nothing to do or gone
// wrong (Info), or where a trigger was: a corner, an edge (or the wheel), or
// Cursor for any other.
enum class NoteIcon { Check, Info, UpLeft, UpRight, DownLeft, DownRight, Up, Down, Left, Right, Cursor };

// What the card asks about.
enum class ConfirmIcon { Power, Restart, SignOut, Sleep };

// What a bubble holds, once it is no longer waiting: the translation, or why
// there is none (no service set up, no translate plugin, it failed).
enum class BubbleState { Result, NoService, NoPlugin, Failed };

class Cards {
public:
    Cards();
    ~Cards();
    Cards(const Cards&) = delete;
    Cards& operator=(const Cards&) = delete;

    // Made on (and only ever used from) the thread that will run them, one
    // with a message loop; false when the windows could not be made.
    bool create();
    void destroy();
    void setLook(const Look& look);

    // A short note by `at` (screen pixels), gone after `ms`; the pointer
    // goes through it. `lead` (may be empty) is said quietly before `text`,
    // which is said strongly: "左上角 · " then "任务视图". A new one replaces
    // the last.
    void flash(POINT at, NoteIcon icon, const std::wstring& lead, const std::wstring& text, int ms = 900);

    // Asks first, in the middle of the screen `at` is on: `title` ("关机"),
    // a line counting down from `seconds` ("5 秒后关机"), the buttons 取消
    // and `now` ("现在关机"). Esc or 取消: done(false); Enter, `now` or the
    // count reaching 0: done(true). One at a time: a new one cancels the last
    // (done(false)).
    void confirm(POINT at, ConfirmIcon icon, const std::wstring& title, const std::wstring& now, int seconds, std::function<void(bool)> done);
    bool confirming() const;

    // A bubble by `at`, waiting for a translation ("正在翻译…", drawn by
    // Cards), which update() then puts in, or says why there is none (`text` is
    // that only: the click that fixes it is in the header, by `state`). A click
    // on it: clicked(), then it closes; what a click does is the caller's. Esc or
    // a press anywhere else (pressedAt) closes it.
    void bubble(POINT at, std::function<void()> clicked);
    void update(BubbleState state, const std::wstring& text);
    bool bubbleShown() const;
    void closeBubble();
    // A mouse button pressed at `at` (from the engine's hook): outside the
    // bubble, it closes.
    void pressedAt(POINT at);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace gestures

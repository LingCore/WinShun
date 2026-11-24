#pragma once

#include <QRect>
#include <QString>

#include <windows.h>

#include <array>
#include <memory>
#include <vector>

// The long screenshot's scroll ladder (docs/design-capture-roadmap.md, 4.2):
// each step scrolls the program under the region one of these ways. What a
// way returns says only whether it could be sent: a posted wheel "works" in
// Chromium and WinUI programs and nothing moves (stage 6 probe), so whether
// it scrolled is for the frames to tell (LongCapture).
namespace ws::capture {

enum class ScrollMethod {
    PostWheel, // WM_MOUSEWHEEL posted to the deepest window under the middle; the mouse stays where it is
    SendWheel, // a wheel through SendInput: only with the mouse already in the region
    Uia, // UI Automation's ScrollPattern of the element under the middle
    Manual, // the user scrolls
};

// What a step's attempt came to: 0 is sent, the others why not.
enum ScrollCode : int {
    ScrollOk = 0,
    ScrollAccessDenied = 1, // the program runs with higher rights than ours (UIPI): PostMessage error 5
    ScrollNoWindow = 2, // no window of another program under the region's middle
    ScrollCursorOutside = 3, // SendInput: the mouse is not in the region
    ScrollKeysHeld = 4, // Ctrl, Shift, Alt, Win or a mouse button is down: the wheel would zoom or drag
    ScrollNoPattern = 5, // UI Automation: nothing under the middle scrolls up and down
    ScrollFailed = 6, // the call failed (detail: GetLastError or the HRESULT)
    ScrollAtEnd = 7, // UI Automation: at the end already
};

struct ScrollResult {
    ScrollMethod method = ScrollMethod::Manual;
    int code = ScrollOk;
    quint32 detail = 0;
};

QString methodName(ScrollMethod method);
QString codeName(int code);

// Which way the next step scrolls, from what the steps before came to;
// no windows, so the tests drive it. The ways in their order: the posted
// wheel, SendInput's wheel, UI Automation (or SendWheel first when the target
// window is a known WinUI/InputSite surface where posted wheel messages never
// scroll). A way that has never moved anything is given up after two steps
// that did not move; one that has moved stays, but while it does not, the
// others get a turn. The end: every way left did not move twice in a row,
// or UI Automation says it is at the end.
class ScrollLadder {
public:
    enum class Hint {
        None,
        MouseOutside, // only SendInput's wheel is left: the mouse must be over the region
        Elevated, // the program runs as administrator: the user scrolls
        NothingWorks, // no way moved anything: the user scrolls
    };

    explicit ScrollLadder(bool elevated = false, bool preferSendWheel = false);

    // The way to try now; Manual when none is left.
    ScrollMethod method() const;
    // How much to ask of method() for about `px` rows: whole wheel notches,
    // or the part of UI Automation's view. Small the first time (nothing
    // measured yet), then from what the way moved before.
    double amount(int px) const;
    // The rows `amount` of method() should move, once measured (else 0).
    int expected(double amount) const;
    // The step could not be sent: a ScrollCode other than ScrollOk.
    void refused(int code);
    // The step was sent and the content moved `rows` (0: not at all).
    void moved(int rows, double amount);

    bool atEnd() const { return m_atEnd; }
    Hint hint() const { return m_hint; }
    bool worked(ScrollMethod method) const;
    bool anyWorked() const;

private:
    struct Way {
        ScrollMethod method = ScrollMethod::Manual;
        bool alive = true;
        bool worked = false; // moved something once
        int still = 0; // steps in a row that did not move
        double perUnit = 0; // rows a notch, or rows a whole view; 0 not measured
    };
    Way* current();
    const Way* current() const;
    void next(); // the next way alive after the current one
    void checkEnd();

    std::array<Way, 3> m_ways;
    int m_current = 0;
    bool m_atEnd = false;
    bool m_mouseOutside = false; // SendInput's wheel was refused for it since the last move
    Hint m_hint = Hint::None;
};

// One thread's: UI Automation is made on the thread that calls scroll(),
// which must be in the multithreaded apartment; its calls give up after a
// second, so a program that hangs cannot hold the capture.
class ScrollInput {
public:
    // `region`: physical pixels, virtual desktop. `horizontal`: the steps
    // go right (or left when direction < 0).
    explicit ScrollInput(const QRect& region, bool horizontal = false);
    ~ScrollInput();

    ScrollInput(const ScrollInput&) = delete;
    ScrollInput& operator=(const ScrollInput&) = delete;

    // The top-level window under the region's middle when the capture
    // started, the deepest window there, and its program's exe name (the
    // only name the diagnostics keep).
    HWND window() const { return m_window; }
    HWND content() const { return m_content; }
    QString program() const { return m_program; }
    // True for WinUI / UWP / DirectComposition input-sink surfaces where
    // PostMessage(WM_MOUSEWHEEL) is ignored and SendInput should be used first.
    bool prefersSendWheel() const { return m_prefersSendWheel; }
    // True when Windows is configured to scroll a full screen per wheel notch.
    static bool pageScrollWheel();

    // Update the screen rectangles of our own floating windows (panel, preview)
    // so cursor checks and cursor parking never land on our own UI.
    void setOwnRects(std::vector<QRect> rects);

    // One step along `direction` (+1: down / right; -1: up / left).
    // PostWheel and SendWheel: `amount` wheel notches (fractional down to 0.25);
    // Uia: the part of the view to move, by SetScrollPercent or small steps.
    ScrollResult scroll(ScrollMethod method, double amount, int direction = 1);
    // UI Automation's position (vertical, or horizontal), 0..100; -1 when not known.
    double uiaPercent();
    // True when the pointer is inside the region and not over our own panel/preview.
    bool cursorInRegion() const;

    // Temporarily park the cursor at an unobstructed point inside the region
    // for SendInput wheel scrolling, saving the user's previous cursor position.
    // Returns ScrollOk on success, ScrollKeysHeld while a key/button is still
    // settling, ScrollCursorOutside if no unobstructed point exists, or ScrollFailed.
    ScrollCode parkCursor(int userMoveThresholdPx);
    bool cursorParked() const { return m_cursorParked; }
    // True if the user physically moved the mouse beyond the threshold after we parked it.
    bool userMovedCursor() const;
    // Release ownership of the parked cursor, optionally restoring its prior
    // position (only when the user has not moved it away).
    void releaseParkedCursor(bool restore);

private:
    struct Uia;
    HWND windowUnderPoint(POINT at) const;
    POINT safePoint() const;
    bool coveredByOwn(POINT at) const;
    // Ctrl, Shift, Alt or Win (and with `buttons` a mouse button) down, or
    // all up for less than a moment yet.
    bool keysHeld(bool buttons);

    QRect m_region;
    bool m_horizontal = false;
    POINT m_middle {};
    HWND m_window = nullptr;
    HWND m_content = nullptr;
    QString m_program;
    bool m_prefersSendWheel = false;
    std::vector<QRect> m_ownRects;
    std::unique_ptr<Uia> m_uia;
    ULONGLONG m_keysUpAt = 0; // when they were seen all up; 0 not yet
    bool m_cursorParked = false;
    POINT m_savedCursor {};
    POINT m_parkedCursor {};
    int m_moveThreshold = 24;
};

} // namespace ws::capture

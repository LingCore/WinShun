// The rules of the winshun.gestures plugin ("鼠标手势"), as plain C++ (no
// windows: only the VK_ names from windows.h and the SDK's Json) so the
// tests (tests/tst_gestures.cpp) and the settings window build them in too:
// what a drag with the right button draws, the screen's corners and edges
// the pointer is pushed into or rubbed along, the rules the user set and
// which of them a gesture means where it is drawn. docs/design-gestures.md
// has the whole design.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gestures {

// ---- what is drawn ----

// A gesture drawn is its directions in order, one letter each: L R U D
// ("DR" = down, then right), at most this many; or a circle, "cw"
// (clockwise) or "ccw".
constexpr std::size_t kMaxDirections = 6;
inline constexpr std::string_view kClockwise = "cw";
inline constexpr std::string_view kCounterClockwise = "ccw";

// The directions of a drag (screen pixels, y down), and whether it went
// round. A direction counts once the pointer has gone `stroke` pixels along
// it from where the last one was confirmed, clearly more along one axis than
// the other; the same direction again only moves that point on, a slant
// counts for nothing. A circle: turned 300 degrees or more one way, ended
// near where it started, not too flat.
class Recognizer {
public:
    void reset(int x, int y, int stroke);
    // True when what it reads as (gesture()) changed.
    bool add(int x, int y);
    const std::string& directions() const { return m_directions; }
    // "cw" or "ccw" when it is a circle, else the directions.
    std::string gesture() const;
    bool tooLong() const { return m_directions.size() > kMaxDirections && circle().empty(); }

private:
    std::string_view circle() const;
    int m_x = 0; // where the last direction was confirmed
    int m_y = 0;
    int m_stroke = 20;
    std::string m_directions;
    // The turning: from point to point once `m_stroke / 2` apart.
    int m_startX = 0;
    int m_startY = 0;
    int m_lastX = 0;
    int m_lastY = 0;
    int m_left = 0;
    int m_top = 0;
    int m_right = 0;
    int m_bottom = 0;
    int m_segmentX = 0;
    int m_segmentY = 0;
    bool m_hasHeading = false;
    double m_heading = 0; // radians, of the last segment
    double m_turned = 0; // radians, clockwise on the screen positive
};

// "DR" -> "↓→", "cw" -> "↻" (UTF-8), for the hint and the settings.
std::string arrows(std::string_view gesture);
// What the user wrote for a gesture: arrows (↓→), letters (DR, dr), 上下左右,
// ↻ ↺ or 顺时针 / 逆时针 (画圈); spaces and dashes between are fine. Empty
// when it is none of these or too long.
std::string parseDirections(std::wstring_view text);

// ---- what a rule does ----

enum class Builtin {
    // 网页
    Back, Forward, Refresh, Top, Bottom,
    // 标签页
    NewTab, CloseTab, ReopenTab, PrevTab, NextTab,
    // 窗口
    Close, Minimize, Maximize, Topmost, Desktop, TaskView, LastWindow,
    // 文字
    Copy, Cut, Paste, Undo, Redo, SelectAll, WebSearch, FindInWinShun, Translate,
    // Win顺
    Search,
    // 系统
    Mute, VolumeUp, VolumeDown, PlayPause, NextTrack, PrevTrack, Lock, ScreenOff, Sleep, Shutdown, Restart, SignOut,
};

enum class Category { Page, Tabs, Window, Text, WinShun, System };

struct BuiltinInfo {
    Builtin builtin;
    const char* id; // as kept in the rules: "copy"
    Category category;
    const wchar_t* zh; // its title
    const wchar_t* en;
};
// All of them, in the order the settings show them.
const std::vector<BuiltinInfo>& builtins();
const BuiltinInfo& info(Builtin builtin);
std::optional<Builtin> builtinById(std::string_view id);
const wchar_t* categoryTitle(Category category, bool chinese);

// Done on the words selected where the gesture is drawn.
bool takesSelection(Builtin builtin);
// Asked first, with a few seconds to change one's mind: 关机 重启 注销 睡眠.
bool asksFirst(Builtin builtin);

struct Action {
    enum class Kind {
        Nothing, // does nothing (a rule that keeps a gesture off somewhere)
        Builtin,
        Keys, // pressed in order, let go in reverse
        Open, // a program, shortcut, file, folder or web address
        Type, // these words typed where the caret is
        Invalid, // none of these: shown as such in the hint
    };
    Kind kind = Kind::Nothing;
    Builtin builtin = Builtin::Back;
    std::vector<std::uint16_t> keys; // virtual keys, modifiers first
    std::wstring target; // Open: the path or address, quotes taken off; Type: the words
    std::wstring text; // Keys and Open as written, trimmed; Invalid: what was not understood
    bool operator==(const Action&) const = default;
};

// One action as the 1.0 settings had it written:
// - a name: 后退 前进 回到顶部 到底部 刷新 关闭窗口 关闭标签页 最小化 最大化
//   搜索框 …, in Chinese or as its id (back, forward, top, …);
// - keys joined by +: Ctrl+W, Alt+Left, Ctrl+Shift+T, F5, Win+D;
// - anything with a \ / or : in it, or a name ending in .exe .lnk .bat .cmd:
//   opened, as a double click would.
Action parseAction(std::wstring_view text);
Action keysAction(std::wstring_view keys); // Invalid when they do not read
Action openAction(std::wstring_view target);
Action typeAction(std::wstring_view words);
Action builtinAction(Builtin builtin);

// Its title in the hint and the settings: the builtin's name in the
// language, the keys or the target as written. Empty for Nothing.
std::wstring actionTitle(const Action& action, bool chinese);

// ---- mouse buttons ----

// A mouse button involved in a gesture: Right, Middle, X1 (side back) and X2
// (side forward) may start a gesture or wheel rule; Left is only ever a
// secondary chord button while a trigger button is held (never held back on
// its own, so clicks and drags have zero latency).
enum class MouseButton : std::uint8_t {
    Any = 0, // matches any enabled trigger button (or no chord)
    Right,
    Middle,
    X1,
    X2,
    Left,
};

const char* buttonId(MouseButton button); // "", "right", "middle", "x1", "x2", "left"
const char* buttonShortId(MouseButton button); // "", "r", "m", "x1", "x2", "l"
std::optional<MouseButton> buttonById(std::string_view id);
bool isTriggerButton(MouseButton button);
std::vector<MouseButton> defaultTriggerButtons(); // {Right}
// "right, x1; middle" -> canonical list in Right/Middle/X1/X2 order; falls back to {Right} when empty.
std::vector<MouseButton> parseTriggerButtons(std::string_view text);
std::string formatTriggerButtons(const std::vector<MouseButton>& buttons);
bool hasTriggerButton(const std::vector<MouseButton>& buttons, MouseButton button);
const wchar_t* buttonTitle(MouseButton button, bool chinese); // 右键 / Right button ...
const wchar_t* buttonShortTitle(MouseButton button, bool chinese); // 右 / R, 侧1 / X1 ...

// ---- keys held with the button ----

// Ctrl, Alt and Shift held when the trigger button goes down make a gesture
// of their own ("shift:DR" is not "DR"); Win is left to Windows. A set of
// them as bits.
using Modifiers = std::uint8_t;
inline constexpr Modifiers kModCtrl = 1;
inline constexpr Modifiers kModAlt = 2;
inline constexpr Modifiers kModShift = 4;
// As the rules keep them: "ctrl+shift" (lower case, in Windows' order Ctrl,
// Alt, Shift); "" for none.
std::string modifiersId(Modifiers modifiers);
// "Shift+Ctrl", "alt" (any case, any order, each once) -> the set; null for
// anything else, an empty text among it.
std::optional<Modifiers> parseModifiers(std::string_view id);
// "Ctrl+Shift", for the hint and the settings; empty for none.
std::wstring modifiersTitle(Modifiers modifiers);

// ---- the rules ----

enum class Zone { Anywhere, TopLeft, TopRight, BottomLeft, BottomRight };
const char* zoneId(Zone zone); // "", "tl", "tr", "bl", "br"
std::optional<Zone> zoneById(std::string_view id);
const wchar_t* zoneTitle(Zone zone, bool chinese); // 左上角 …; empty for Anywhere

// A gesture trigger parsed:
// [ keys ":" ] [ (r|m|x1|x2) ":" ] [ stroke ] [ "+" (l|r|m|x1|x2) ], keys
// being Ctrl, Alt and Shift held ("shift", "ctrl+alt"). Either stroke is
// non-empty ("DR", "cw", "m:U", "D+l", "shift:DR") or chord is non-Any
// ("+l", "r:+l", "x1:+r", "ctrl:r:+l").
struct ParsedGesture {
    MouseButton trigger = MouseButton::Any;
    std::string stroke;
    MouseButton chord = MouseButton::Any;
    Modifiers modifiers = 0;
    bool operator==(const ParsedGesture&) const = default;
};
std::optional<ParsedGesture> parseGestureTrigger(std::string_view on);
// Empty when the parts make no trigger.
std::string formatGestureTrigger(MouseButton trigger, std::string_view stroke, MouseButton chord = MouseButton::Any,
    Modifiers modifiers = 0);
// Compact label for the HUD chip and rule tile: "↓→", "↓+左", "右+左", "侧1 ↑", "Shift ↓→".
std::wstring gestureBadge(std::string_view on, bool chinese);

struct ParsedWheel {
    MouseButton trigger = MouseButton::Any;
    bool up = true;
    bool operator==(const ParsedWheel&) const = default;
};
std::optional<ParsedWheel> parseWheelTrigger(std::string_view on);
std::string formatWheelTrigger(MouseButton trigger, bool up);

// What sets a rule off ("on"): a gesture drawn or chorded ("DR", "cw", "m:U",
// "D+l", "r:+l", "shift:DR"), the pointer pushed into a corner ("corner:tl" …) or rubbed
// along an edge ("edge:top" …), the wheel turned with a trigger button held
// ("wheel:up" / "wheel:down" or "<btn>:wheel:up|down"), or a shortcut pressed
// ("key:Ctrl+Alt+T").
enum class TriggerKind { Gesture, Corner, Edge, Wheel, Key, Unknown };
TriggerKind triggerKind(std::string_view on);
inline constexpr std::string_view kCorners[] = {"corner:tl", "corner:tr", "corner:bl", "corner:br"};
inline constexpr std::string_view kEdges[] = {"edge:top", "edge:bottom", "edge:left", "edge:right"};
inline constexpr std::string_view kWheelUp = "wheel:up";
inline constexpr std::string_view kWheelDown = "wheel:down";
inline constexpr std::string_view kKeyPrefix = "key:";
// "↓→", "↓+左", "左上角", "上边", "按住手势键向上滚", "Ctrl+Alt+T" …: the trigger as the hint and the settings name it.
std::wstring triggerTitle(std::string_view on, bool chinese);
// A shortcut as a trigger: "ctrl + alt+t" -> "key:Ctrl+Alt+T" (the modifiers
// in Windows' order Win, Ctrl, Alt, Shift, as the shortcut recorder writes
// them). Empty when it is no key, or one that would get in the way of
// typing: Ctrl, Alt or Win held with it, or F1 to F24 alone.
std::string keyTrigger(std::wstring_view keys);
// What RegisterHotKey takes for a "key:" trigger (MOD_ALT … without
// MOD_NOREPEAT, and the virtual key); false when it is none.
bool hotkeyOf(std::string_view on, unsigned* modifiers, unsigned* vk);
// Whether releasing the modifiers of a RegisterHotKey chord (whose main key
// Windows swallowed) could trigger a bare-modifier action in the foreground
// window (Alt menu bar / browser menu button, Win Start menu, Shift IME switch).
bool shortcutNeedsMask(unsigned modifiers) noexcept;

struct Rule {
    std::string on;
    // Only a gesture drawn: started in this corner of its screen (the
    // fifth of its width and height there).
    Zone where = Zone::Anywhere;
    // Only a gesture drawn or a shortcut: while words are selected in the
    // window under the pointer (a shortcut: in the window in front).
    bool selection = false;
    // Only a gesture drawn, the wheel or a shortcut: in these programs (file
    // names, lower case, ".exe" added); empty: in all of them.
    std::vector<std::wstring> apps;
    Action action;
    bool operator==(const Rule&) const = default;
    MouseButton triggerButton() const;
    // The keys a gesture is drawn with (none for the other triggers).
    Modifiers modifiers() const;
    // More specific rules win: a program's over a specific trigger button's
    // over the selection's over a corner's over the rest.
    int specificity() const
    {
        return (apps.empty() ? 0 : 8) + (triggerButton() == MouseButton::Any ? 0 : 4) + (selection ? 2 : 0)
            + (where == Zone::Anywhere ? 0 : 1);
    }
    // Whether two rules are for the same thing (the later one replaces the other).
    bool sameCase(const Rule& other) const;
};

// Where and over what a trigger happens.
struct Context {
    Zone zone = Zone::Anywhere; // the corner of its screen a gesture was started in
    bool selection = false; // words selected in the window under the pointer
    std::wstring program; // that window's program, a file name in lower case
    MouseButton button = MouseButton::Any; // the trigger button held (Right/Middle/X1/X2), or Any
    // Ctrl, Alt, Shift held when it went down: only the gestures drawn with
    // exactly these keys hold (and no wheel rule).
    Modifiers modifiers = 0;
};

// The rule for `on` in `context`, the most specific that holds (the first of
// those equally specific); null when none. The keys of a gesture are those
// in `on` and the context's.
const Rule* match(const std::vector<Rule>& rules, std::string_view on, const Context& context);
// Whether pressing `chord` right now (with `stroke` drawn so far while holding
// `context.button`) could trigger an active (non-Nothing) rule (either plain
// or with selected text).
bool chordMatches(const std::vector<Rule>& rules, std::string_view stroke, MouseButton chord, const Context& context);
// Whether a rule for words selected could hold for a gesture started with
// `prefix` (directions so far; empty: any) in `program`: then it is worth
// asking for the selection.
bool selectionMatters(const std::vector<Rule>& rules, std::string_view prefix, std::wstring_view program,
    MouseButton button = MouseButton::Any, Modifiers modifiers = 0);
// Whether a gesture drawn with `modifiers` held and `button` (Any: any) could
// do something in `program`: only then is a press with those keys taken
// (else Shift + right click, Ctrl + right drag and the like stay the
// program's).
bool modifiersMatter(const std::vector<Rule>& rules, Modifiers modifiers, MouseButton button, std::wstring_view program);
// Whether any rule that does something is set off by this kind of trigger:
// the corners and edges are watched only then.
bool anyFor(const std::vector<Rule>& rules, TriggerKind kind);

// The rules kept in the setting "rules":
// {"version": 2, "rules": [{"on": "DR", "do": "close"}, {"on": "cw", "where": "br", "do": "shutdown"},
//  {"on": "D", "selection": true, "do": "copy"}, {"on": "L", "apps": ["photoshop.exe"], "keys": "Ctrl+Z"},
//  {"on": "corner:tl", "open": "C:\\…"}, {"on": "edge:top", "type": "…"}, {"on": "R", "apps": [...]}]}
// A rule with no "do", "keys", "open" or "type" does nothing. Rules that do
// not read are left out.
std::string rulesJson(const std::vector<Rule>& rules);
// Null when the text is not this JSON at all (then the defaults hold).
std::optional<std::vector<Rule>> parseRules(std::string_view json);
std::vector<Rule> defaultRules();

struct Settings {
    std::vector<Rule> rules;
    std::vector<std::wstring> excluded; // programs whose windows keep trigger buttons
    std::vector<MouseButton> buttons = defaultTriggerButtons(); // enabled trigger buttons
    bool trail = true; // draw the trail and the hint
    bool fullScreen = false; // gestures, corners and edges over a full-screen window too
};

// ---- mouse button & chord state machine (hook thread) ----

struct SessionResult {
    enum class Event { None, Move, End, Cancel, Click, PressThrough, Wheel };
    bool swallow = false; // hook should return 1
    bool startHoldTimer = false;
    bool stopHoldTimer = false;
    bool resetScreenWatches = false;
    bool watchScreen = false;
    bool cancelBefore = false; // cancel an earlier stale or gesturing session before proceeding
    bool startBeforeMove = false; // post kStart before kMove
    Event event = Event::None;
    MouseButton button = MouseButton::Any; // active trigger button for the emitted event
    std::string gesture; // for Move / End
    bool wheelUp = false; // for Wheel
    int wheelNotches = 0; // for Wheel
};

// Pure-logic state machine for the low-level mouse hook: manages multi-button
// triggers (Right/Middle/X1/X2), plain click replay, hold-still pass-through,
// strokes, pure rockers, stroke+chord combos (with symmetric simultaneous
// release and repeat rockers), wheel gestures, and Esc / unmatched-left cancel.
class GestureSession {
public:
    // Held: still past the hold time with a chord set for the button: it
    // waits for the second button, and goes through once the pointer moves.
    enum class State { Idle, Pressed, Held, Gesturing, Wheeling, Rocked, Through, Canceled };

    State state() const { return m_state; }
    MouseButton activeButton() const { return m_activeButton; }
    MouseButton chordButton() const { return m_chordButton; }

    // True when `btn` down should run eligibility checks (window/process/exclusions)
    // to potentially begin a new trigger session.
    bool wantsTriggerCheck(MouseButton btn, const std::vector<MouseButton>& enabledButtons, bool activeStillDown = true) const;

    SessionResult onButtonDown(MouseButton btn, int x, int y, int startDistance, int stroke, bool eligible,
        const Settings& settings, const Context& context, bool activeStillDown = true);
    SessionResult onButtonUp(MouseButton btn, int x, int y);
    SessionResult onMove(int x, int y);
    SessionResult onWheel(int delta, bool hasWheelRules, const Settings& settings, const Context& context);
    SessionResult onHoldTimer();
    SessionResult onAbort();
    void reset();

private:
    State m_state = State::Idle;
    MouseButton m_activeButton = MouseButton::Any;
    MouseButton m_chordButton = MouseButton::Any;
    std::string m_chordStroke;
    std::uint8_t m_swallowedButtons = 0;
    int m_downX = 0;
    int m_downY = 0;
    int m_startDistance = 6;
    int m_wheelDelta = 0;
    bool m_chordsSet = false; // a chord is set for the button pressed, there: held still, it waits (State::Held)
    Recognizer m_recognizer;
};

// "a.exe; B.EXE, c" -> {"a.exe", "b.exe", "c.exe"}: separated by ; , or
// line breaks, trimmed, lower case (ASCII), ".exe" added when there is no
// dot; a path counts by its file name.
std::vector<std::wstring> programList(std::wstring_view text);
// Whether `exe` (a file name or a path) is in the list.
bool listed(const std::vector<std::wstring>& list, std::wstring_view exe);

// ---- 1.0's settings ----

// The 1.0 setting keys of the seven gestures, their directions and default actions.
struct Slot {
    const char* key;
    const char* directions;
    const wchar_t* fallback;
};
const std::vector<Slot>& legacySlots();
// "↑←=Ctrl+Shift+T; DU = notepad.exe" -> {"UL": …, "DU": …}: 1.0's "more"
// setting, separated by ; or line breaks, each directions = action.
std::vector<std::pair<std::string, std::wstring>> parseMore(std::wstring_view text);
// The rules 1.0's settings meant: the defaults with the gestures written
// in its seven slots and "more" in place of the plain ones. Null when they
// are all as they came, so nothing needs keeping.
std::optional<std::vector<Rule>> migrate(const std::vector<std::pair<std::string, std::wstring>>& slots, std::wstring_view more);

// ---- the screen's corners and edges ----

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

// The screen the pointer is on and which of its sides end the desktop
// there (no other screen beyond them, where the pointer is): only those
// stop the pointer, so only those have corners and edges.
struct Screen {
    Rect bounds;
    bool hardLeft = false;
    bool hardTop = false;
    bool hardRight = false;
    bool hardBottom = false;
    double scale = 1.0; // its DPI / 96
};

// The corner a point is in on `screen` (Anywhere: none): the gesture zones,
// a fifth of its width and height.
Zone zoneAt(int x, int y, const Rect& screen);

// The pointer pushed into a corner: in it (2 pixels) for kDwellMs. Once
// done, it waits for the pointer to go kRearm logical pixels away.
class CornerWatch {
public:
    static constexpr int kDwellMs = 150;
    static constexpr int kRearm = 50;
    // A move. True when it just came into a corner: ask due() after kDwellMs.
    bool move(int x, int y, const Screen& screen, std::int64_t ms);
    // The corner ("corner:tl" …) whose time has come, empty for none.
    std::string_view due(std::int64_t ms);
    void reset();

private:
    int m_corner = -1; // index into kCorners while in one
    std::int64_t m_since = 0; // when it came into it
    int m_cornerX = 0;
    int m_cornerY = 0;
    int m_spent = -1; // the corner just done, until the pointer goes away from where it was
    int m_spentX = 0;
    int m_spentY = 0;
};

// The pointer rubbed along an edge: there and back, kTurns turns of it
// within kWithinMs, each leg kLeg logical pixels or more, all within kNear
// logical pixels of the edge. After one, nothing for kQuietMs.
class RubWatch {
public:
    static constexpr int kTurns = 3;
    static constexpr int kWithinMs = 1000;
    static constexpr int kLeg = 24;
    static constexpr int kNear = 6;
    static constexpr int kQuietMs = 1000;
    // A move: the edge ("edge:top" …) rubbed just now, empty for none.
    std::string_view move(int x, int y, const Screen& screen, std::int64_t ms);
    void reset();

private:
    int m_edge = -1; // index into kEdges
    int m_direction = 0; // along the edge: +1, -1, 0 (not yet)
    int m_anchor = 0; // where it came to the edge
    int m_extreme = 0; // the farthest along the current leg
    std::vector<std::int64_t> m_turns; // when it turned, the last kWithinMs
    std::int64_t m_quietUntil = 0;
};

} // namespace gestures

#pragma once

#include "OwnInput.h" // kMaskKey

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ws::searchtyping {

// What typing somewhere else means for Win顺 (platform/TypeToSearch): in the
// Start menu (a setting) and in a File Explorer folder window (another), the
// first key that types something opens the launcher with it. Pure logic over
// Windows' virtual-key codes and what has the keyboard focus, unit-tested;
// the hook decides from it alone, with no messages sent.

// Virtual-key codes (winuser.h), named here so that the core needs no
// windows.h; the tests use the same numbers.
inline constexpr unsigned kVkBack = 0x08;
inline constexpr unsigned kVkTab = 0x09;
inline constexpr unsigned kVkReturn = 0x0D;
inline constexpr unsigned kVkShift = 0x10;
inline constexpr unsigned kVkControl = 0x11;
inline constexpr unsigned kVkMenu = 0x12;
inline constexpr unsigned kVkCapital = 0x14;
inline constexpr unsigned kVkEscape = 0x1B;
inline constexpr unsigned kVkSpace = 0x20;
inline constexpr unsigned kVkLeft = 0x25;
inline constexpr unsigned kVkDelete = 0x2E;
inline constexpr unsigned kVkLWin = 0x5B;
inline constexpr unsigned kVkRWin = 0x5C;
inline constexpr unsigned kVkNumpad0 = 0x60;
inline constexpr unsigned kVkDivide = 0x6F;
inline constexpr unsigned kVkF1 = 0x70;
inline constexpr unsigned kVkF24 = 0x87;
inline constexpr unsigned kVkNumlock = 0x90;
inline constexpr unsigned kVkLShift = 0xA0;
inline constexpr unsigned kVkRShift = 0xA1;
inline constexpr unsigned kVkLControl = 0xA2;
inline constexpr unsigned kVkRControl = 0xA3;
inline constexpr unsigned kVkLMenu = 0xA4;
inline constexpr unsigned kVkRMenu = 0xA5;
inline constexpr unsigned kVkOem1 = 0xBA;
inline constexpr unsigned kVkOem3 = 0xC0;
inline constexpr unsigned kVkOem4 = 0xDB;
inline constexpr unsigned kVkOem8 = 0xDF;
inline constexpr unsigned kVkOem102 = 0xE2;
inline constexpr unsigned kVkProcessKey = 0xE5;
inline constexpr unsigned kVkPacket = 0xE7; // a character a program sent (KEYEVENTF_UNICODE), in the scan code

// A key Windows makes up itself has this in the scan code a low-level hook
// sees (SCANCODE_SIMULATED, kbd.h): the left Ctrl it presses with the right
// Alt on a layout with AltGr (Polish, German: 0x21D, just before the right
// Alt's own 0x38, extended; let go with it), and the Shift it lets go of and
// presses again around a number pad key typed with Shift and NumLock on
// (0x22A, 0x236). AutoHotkey's hook tells AltGr's left Ctrl by the same scan
// code (SC_FAKE_LCTRL), QEMU's and TigerVNC's Windows keyboard code likewise.
inline constexpr unsigned kSimulatedScan = 0x200;
inline constexpr unsigned kRightShiftScan = 0x36;

// A key Windows made up (above). Not VK_PACKET: its scan code is the
// character a program sent, which has that bit too about half the time (你 is
// U+4F60): taken for made up, it went on at once, ahead of the keys held
// before it.
inline constexpr bool madeUp(unsigned vk, unsigned scanCode) noexcept
{
    return vk != kVkPacket && (scanCode & kSimulatedScan) != 0;
}

// GetGUIThreadInfo's flags (winuser.h): the thread is in a menu's loop, that
// of a pop-up menu (a context menu), or of the window's system menu.
inline constexpr unsigned kGuiInMenuMode = 0x04;
inline constexpr unsigned kGuiSystemMenuMode = 0x08;
inline constexpr unsigned kGuiPopupMenuMode = 0x10;

// Shift, Ctrl, Alt, the Windows keys and the locks: seen as they are, never
// typing.
inline constexpr bool isModifier(unsigned vk) noexcept
{
    return vk == kVkShift || vk == kVkControl || vk == kVkMenu || vk == kVkLWin || vk == kVkRWin
        || (vk >= kVkLShift && vk <= kVkRMenu) || vk == kVkCapital || vk == kVkNumlock;
}

inline constexpr bool isShift(unsigned vk) noexcept
{
    return vk == kVkShift || vk == kVkLShift || vk == kVkRShift;
}

// Keys that start a search: letters, digits, punctuation, the number pad's
// digits and signs. Not Space or Backspace, which do nothing in an empty
// search box (Listary leaves Space to Explorer too), nor Enter, Esc, Tab,
// arrows, Delete, F keys: those are commands in Explorer's view (open,
// rename, select). With a Chinese input method on, the hook still sees the
// letter's own code (VK_PROCESSKEY appears only once a window translates
// the key), so pinyin starts a search like any typing.
inline constexpr bool startsTyping(unsigned vk) noexcept
{
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= kVkNumpad0 && vk <= kVkDivide)
        || (vk >= kVkOem1 && vk <= kVkOem3) || (vk >= kVkOem4 && vk <= kVkOem8) || vk == kVkOem102;
}

// What has the keyboard focus in the Explorer window in front, by window
// classes (platform/FolderViewHit: DirectUIHWND under SHELLDLL_DefView).
enum class Focus {
    ItemsView, // the files and folders shown: typing there jumps to a name in Explorer
    Menu, // a menu of the window's is open (a context menu): the keys are its, 属性(R) (GitHub #37)
    Other, // a name being renamed (an Edit), the address bar, Explorer's search box, the navigation pane…
};

// What has the keyboard in the Explorer window, from GetGUIThreadInfo on its
// thread: its `flags`, and whether its focus window is the files and folders
// shown. A menu open comes first: while the classic context menu is up
// (Shift+right-click, 显示更多选项) the focus stays on the files, the thread
// in menu mode (0x14 measured), and every key is the menu's, whether or not
// it is one of its access keys. Windows 11's own context menu needs no flag:
// it takes the focus into the window's XAML island (docs/pitfalls.md).
inline constexpr Focus focusOf(unsigned guiFlags, bool itemsViewFocused) noexcept
{
    if ((guiFlags & (kGuiInMenuMode | kGuiSystemMenuMode | kGuiPopupMenuMode)) != 0)
        return Focus::Menu;
    return itemsViewFocused ? Focus::ItemsView : Focus::Other;
}

// Whether a key pressed in a File Explorer folder window starts a search with
// Win顺 in that folder. `combination`: Ctrl, Alt or a Windows key held
// (Ctrl+A, Alt+Enter, Win+E stay Explorer's; Shift alone types a capital).
// `elevated`: the window belongs to a process of higher integrity than
// Win顺's; Windows would let neither the hook see its keys nor Win顺 hand
// them back, so nothing is taken from it.
inline constexpr bool explorerStarts(unsigned vk, bool press, bool combination, Focus focus, bool elevated) noexcept
{
    return press && !combination && !elevated && focus == Focus::ItemsView && !isModifier(vk) && startsTyping(vk);
}

// Opus uses punctuation (>, /, *, +, -, ...), Space and shifted digits for
// its own FAYT modes. Only letters and unshifted digits start our search;
// once it opens, all normal typing goes to the launcher's input method.
inline constexpr bool opusStarts(
    unsigned vk, bool press, bool combination, bool shift, Focus focus, bool elevated) noexcept
{
    const bool text = (vk >= 'A' && vk <= 'Z')
        || (!shift && ((vk >= '0' && vk <= '9') || (vk >= kVkNumpad0 && vk < kVkNumpad0 + 10)));
    return press && !combination && !elevated && focus == Focus::ItemsView && text;
}

// The File Explorer window a key typed now counts in, or none: `front` is the
// window really in front at the key (GetForegroundWindow), `seen` the Explorer
// folder window that was in front at the last foreground event, `behind` one
// that raised that event while another window stayed in front. A folder window
// opened by a program that may not take the front (a tool in the background,
// a script) does that: it says it came forward, stays behind, and when a click
// on it or on its taskbar button brings it to the front later, no event comes
// at all (measured 2026-10-24, docs/pitfalls.md). So the key decides by what is
// in front then; that also keeps a key from counting for `seen` should another
// window have come forward the same silent way. Window: HWND, or a number in
// the tests.
template <class Window>
constexpr Window typingWindow(Window front, Window seen, Window behind) noexcept
{
    return front && (front == seen || front == behind) ? front : Window {};
}

// The same kind of folder window, opened behind another before Win顺 started
// (at sign-in, or a restart after an update): the event it raised then went by
// unseen, and it comes to the front later with no event, so its first keys went
// into Explorer. Kept as `behind` when TypeToSearch starts all the same: the
// topmost of `folders` (the Explorer folder windows typing counts in, top down
// as EnumWindows lists them) that is not in front and is still the active
// window of its own thread (GetGUIThreadInfo). A window switched away from
// normally is not (Windows clears it), and comes back with an event (measured,
// docs/pitfalls.md). Only one is kept, as at the events.
template <class Window>
struct OpenFolder {
    Window window {};
    bool activeInItsThread = false;
};

template <class Window>
constexpr Window openedBehind(Window front, const std::vector<OpenFolder<Window>>& folders) noexcept
{
    for (const OpenFolder<Window>& folder : folders) {
        if (folder.window && folder.window != front && folder.activeInItsThread)
            return folder.window;
    }
    return Window {};
}

// What to do with the keys held back once the launcher says it has the
// keyboard (or the next batch of them is due). They go out only while a window
// of Win顺's is really in front: Qt can call the launcher active while the
// Start menu still is, or takes the front back a few milliseconds later (it was
// still opening when its Esc came, or Windows was slow to start it), and keys
// typed then went into Windows' search instead (GitHub #66). With the Start menu
// in front again, the launcher is asked for once more (its Esc closes it), not
// sooner than kAskAgainMs after the last ask, which may still be closing it, and
// at most kMaxAsksAgain times; otherwise the keys wait, until the hold runs out.
// Not once some have been typed into it in this hold: shown again, it selects
// its words, and the next keys would replace them. And only while the
// launcher's word that it is ready for them (App's handoff below) still
// holds: once asked for again, or gone before it was ready (App says so), it
// is shown anew, cloaked, its field not the focus object yet, so the keys
// wait for it to say so again; typed into it in front all the same, they met
// the input method still setting up (GitHub #103).
enum class Delivery {
    Type, // Win顺 is in front: type them
    Wait, // not yet: hold on, look again shortly
    AskAgain, // the Start menu is back in front: ask for the launcher again, then wait
};
inline constexpr unsigned kAskAgainMs = 600; // closing the Start menu for the launcher takes up to 500 ms
inline constexpr int kMaxAsksAgain = 2;

struct FrontState {
    bool oursInFront = false; // a window of Win顺's is really in front
    bool launcherReady = false; // ... and the launcher's word that it is ready for the keys still holds
    bool startMenuInFront = false;
    bool forStartMenu = false; // the keys were typed in the Start menu, not in a file display
    bool typedAlready = false; // some typed into the launcher in this hold
    int asksAgain = 0; // asked for again since the hold began
    unsigned msSinceAsked = 0; // since it was last asked for
};

inline constexpr Delivery delivery(const FrontState& front) noexcept
{
    if (front.oursInFront)
        return front.launcherReady ? Delivery::Type : Delivery::Wait;
    if (front.startMenuInFront && front.forStartMenu && !front.typedAlready && front.asksAgain < kMaxAsksAgain
        && front.msSinceAsked >= kAskAgainMs)
        return Delivery::AskAgain;
    return Delivery::Wait;
}

// How long the keys held for the launcher wait for it: one deadline a hold,
// kWaitForLauncherMs after its first key (App's handoff gives up on a launcher
// that never became active as long after asking for it). Asked for again from
// the Start menu (delivery), the launcher gets kAskAgainWaitMs from then, more
// than the 500 ms its Esc may take to close the Start menu, but the hold never
// waits past kHoldCapMs: every key typed meanwhile waits with it, and so do
// the hook, which every key press in the system waits on, and the Raw Input
// set aside for it (double Ctrl and the drop pie hear nothing). Starting the
// whole wait over at each ask (as it was) let one hold run for 9 s.
// `deadline` and `msSinceStart`: since the hold's first key.
inline constexpr unsigned kWaitForLauncherMs = 3000;
inline constexpr unsigned kAskAgainWaitMs = 1000;
inline constexpr unsigned kHoldCapMs = 4000;

inline constexpr unsigned holdDeadline(unsigned deadline, unsigned msSinceStart) noexcept
{
    return (std::min)(kHoldCapMs, (std::max)(deadline, msSinceStart + kAskAgainWaitMs)); // windows.h's macros
}

// The keys typed again go by within milliseconds; should some never (a hook
// before this one took them), the hold ends after this all the same.
inline constexpr unsigned kPassTimeoutMs = 1000;

// When the launcher, asked for by typing, is ready for the keys held back for
// it (App hands them to the hook then, which types them again: delivery
// above). Qt calls it active as soon as Windows does: its search field may
// not be the focus object yet (Qt gives the window its input method context
// only once a field that takes one is), and it is still cloaked, its first
// frame not drawn. Typed into it then, the keys reach the input method while
// all that settles; one that sets itself up as the window does can lose the
// first (GitHub #103: Sogou Pinyin 9.7 and 12.5 on Windows 10, "cai" came out
// "ai"; not seen with 16.9 on Windows 11). So they go once it takes typing as
// it would a user's: active, its field the focus object, its first frame on
// the screen; and in front, which the hook checks as it types them (delivery:
// it waits for that, asking for the launcher again from the Start menu, GitHub
// #66; waiting for it here instead would leave the keys with nobody to ask
// again, should the launcher go before it came). Should any of that still be
// missing kHandoffCapMs after the launcher first became active (a reveal that
// never comes, the focus somewhere else, or no longer active: the Start menu
// took the front back), they go all the same, the hook deciding: never
// stranded. The reveal is normally the last to come: measured over fourteen
// openings on Windows 11 (WeType, the US keyboard; Explorer and the Start
// menu), 11 to 94 ms after the launcher became active. Its own fallback is
// 60 ms after the launcher was shown (App::showLauncher), but that timer
// fires late while the activation (the input method setting up for the
// window) keeps the GUI thread busy. 120 ms is above all of that, so a reveal
// that does come is never cut short, and still under what a typist notices.
// `msSinceActive`: since the launcher was first active for these keys; none
// while it has not been (the hook's hold gives up on them in time).
struct LauncherState {
    bool active = false; // Qt says the window is active
    bool fieldFocused = false; // its search field is Qt's focus object
    bool revealed = false; // shown, no reveal pending and not cloaked: its first frame is on the screen
};
enum class Handoff {
    Wait, // not ready yet: look again when something changes
    Deliver, // ready: the keys go
    DeliverLate, // still not all of it kHandoffCapMs after it became active: they go all the same
};
inline constexpr unsigned kHandoffCapMs = 120;

inline constexpr Handoff handoff(LauncherState launcher, std::optional<unsigned> msSinceActive) noexcept
{
    if (launcher.active && launcher.fieldFocused && launcher.revealed)
        return Handoff::Deliver;
    return msSinceActive && *msSinceActive >= kHandoffCapMs ? Handoff::DeliverLate : Handoff::Wait;
}

// The keys held back and typed again into the launcher, in the order sent,
// until each has gone down the hook chain. Until the last has, a key typed
// meanwhile is held too and follows them: let through, it overtook the ones
// still on their way ("qz" came out "zq" right after a jump, GitHub #39).
// Win顺's other keys (a paste, the Esc that closes the Start menu) do not
// count: only the next one sent does.
class Retyped {
public:
    struct Key {
        unsigned vk = 0;
        bool release = false;
    };

    void sent(std::vector<Key> keys) noexcept
    {
        m_keys = std::move(keys);
        m_next = 0;
    }
    // One of Win顺's own keys went by: true when it was the last of those sent.
    bool passed(unsigned vk, bool release) noexcept
    {
        if (m_next >= m_keys.size() || m_keys[m_next].vk != vk || m_keys[m_next].release != release)
            return false;
        return ++m_next == m_keys.size();
    }
    bool pending() const noexcept { return m_next < m_keys.size(); }
    void clear() noexcept
    {
        m_keys.clear();
        m_next = 0;
    }

private:
    std::vector<Key> m_keys;
    std::size_t m_next = 0;
};

// A key held back for the launcher (platform/TypeToSearch), and one to type
// again: a key held, Shift pressed or let go around them, the mask.
struct HeldKey {
    // kVkPacket: a character, in scanCode; the left or right Shift, Caps Lock:
    // the user's own, as it came; kMaskKey: the mask goes there
    unsigned vk = 0;
    unsigned scanCode = 0;
    bool extended = false;
    bool release = false;
    bool shift = false; // a press made with Shift held
};
struct Stroke {
    unsigned vk = 0; // kVkPacket: a character, in scanCode; kMaskKey: the mask
    unsigned scanCode = 0; // 0 for Shift and the mask Win顺 adds: the hook's keyboard layout has it
    bool extended = false;
    bool release = false;
    bool operator==(const Stroke&) const = default;
};

// The keys of a batch Win顺 waits to see go by (Retyped): all but the mask,
// which nothing waits for. A character goes by as VK_PACKET, as it was sent.
inline std::vector<Retyped::Key> awaited(const std::vector<Stroke>& strokes)
{
    std::vector<Retyped::Key> keys;
    keys.reserve(strokes.size());
    for (const Stroke& stroke : strokes) {
        if (stroke.vk != kMaskKey)
            keys.push_back({stroke.vk, stroke.release});
    }
    return keys;
}

// The Shift keys down. VK_SHIFT counts as the left one.
struct Shifts {
    bool left = false;
    bool right = false;
    constexpr bool any() const noexcept { return left || right; }
    constexpr void seen(unsigned vk, bool press) noexcept
    {
        if (vk == kVkShift || vk == kVkLShift)
            left = press;
        else if (vk == kVkRShift)
            right = press;
    }
    bool operator==(const Shifts&) const = default;
};

// Shift as one side sees the keys go by (Hold): the window in front, every key
// that got to it, Win顺's own among them; or the user, theirs alone. `alone`:
// a Shift went down with no other key pressed since. Let go now, that is a
// tap, which input methods take for their switch between Chinese and English
// (Sogou, Microsoft Pinyin; GitHub #103): one the user made must get to the
// window as one, and none Win顺 makes may look like one (the mask goes in
// first: retype).
struct ShiftState {
    Shifts keys;
    bool alone = false;
    constexpr void seen(unsigned vk, bool press) noexcept
    {
        if (isShift(vk)) {
            const bool fresh = press && !(vk == kVkRShift ? keys.right : keys.left); // not a repeat
            keys.seen(vk, press);
            if (fresh)
                alone = true;
            else if (!keys.any())
                alone = false;
        } else if (press) {
            alone = false; // the mask too, and a character a program sent
        }
    }
    bool operator==(const ShiftState&) const = default;
};

// The user's Ctrl, Alt and Windows keys, as the hook saw them go by. AltGr is
// the right Alt with the left Ctrl Windows presses for it (kSimulatedScan): it
// types (ż on a Polish keyboard, @ on a German one), it makes no shortcut.
// Without the left Ctrl's mark, the right Alt is Alt. The left Ctrl pressed
// for real before it stays Ctrl: Windows adds none then, and Ctrl+AltGr makes
// a shortcut.
struct Modifiers {
    bool leftCtrl = false;
    bool rightCtrl = false;
    bool leftAlt = false;
    bool rightAlt = false;
    bool leftWin = false;
    bool rightWin = false;
    bool altGrCtrl = false; // the left Ctrl Windows pressed with the right Alt
    constexpr void seen(unsigned vk, unsigned scanCode, bool press) noexcept
    {
        switch (vk) {
        case kVkControl:
        case kVkLControl:
            ((scanCode & kSimulatedScan) ? altGrCtrl : leftCtrl) = press;
            break;
        case kVkRControl:
            rightCtrl = press;
            break;
        case kVkMenu:
        case kVkLMenu:
            leftAlt = press;
            break;
        case kVkRMenu:
            rightAlt = press;
            if (!press)
                altGrCtrl = false; // with it, should its left Ctrl's release come unmarked
            break;
        case kVkLWin:
            leftWin = press;
            break;
        case kVkRWin:
            rightWin = press;
            break;
        default:
            break;
        }
    }
    // Held for a shortcut: Ctrl+C, Alt+Tab, Win+E.
    constexpr bool chord() const noexcept
    {
        return leftCtrl || rightCtrl || leftAlt || (rightAlt && !altGrCtrl) || leftWin || rightWin;
    }
    constexpr bool altGr() const noexcept { return rightAlt && altGrCtrl && !chord(); }
};

// The keys held back, as they are typed again: in order, each press with
// Shift as it was when pressed, from Shift as the window has it (`window`) to
// the user's (`user`), where it is left at the end. Shift itself mostly goes
// on as it comes (a release whose press went on must: one taken and sent
// again left an older hook thinking Shift still held, GitHub #91), so by the
// time the keys go it can be down for keys typed without it ("macOS" came out
// "MACOS"), or up for a capital: Win顺 lets go of it or presses it (the left
// one) around them. The user's own Shift and Caps Lock presses and releases
// held back with the keys (Hold) go where they came, as they came: a tap stays
// a tap, a capital typed after Caps Lock stays one. With no keys, the same
// puts Shift right before the hold ends. The mask goes in wherever the window
// would see a tap that is not the user's: before Win顺 lets go of a Shift the
// window saw go down with nothing since, and after Shift goes down for the
// user at the end when the user has typed with it (their release comes next).
// A program's characters (kVkPacket) and releases go as they are. Never only
// the mask: it goes with a Shift (Hold waits for the keys sent to go by).
inline std::vector<Stroke> retype(const std::vector<HeldKey>& held, ShiftState window, ShiftState user)
{
    std::vector<Stroke> strokes;
    strokes.reserve(held.size() + 6);
    ShiftState down = window; // as the window will have it, stroke by stroke
    const auto stroke = [&strokes, &down](Stroke s) {
        strokes.push_back(s);
        down.seen(s.vk, !s.release);
    };
    const auto mask = [&stroke] {
        stroke({.vk = kMaskKey});
        stroke({.vk = kMaskKey, .release = true});
    };
    const auto letGo = [&](bool left, bool right) {
        if (down.alone)
            mask();
        if (left)
            stroke({.vk = kVkLShift, .release = true});
        if (right)
            stroke({.vk = kVkRShift, .release = true});
    };
    const auto press = [&stroke](Shifts sides) {
        if (sides.left)
            stroke({.vk = kVkLShift});
        if (sides.right)
            stroke({.vk = kVkRShift});
    };
    for (const HeldKey& key : held) {
        if (key.vk == kMaskKey) { // held between the user's Shift press and release (Hold::went)
            mask();
            continue;
        }
        if (!isShift(key.vk) && key.vk != kVkCapital && !key.release && key.vk != kVkPacket) {
            if (key.shift && !down.keys.any())
                stroke({.vk = kVkLShift});
            else if (!key.shift && down.keys.any())
                letGo(down.keys.left, down.keys.right);
        }
        stroke({key.vk, key.scanCode, key.extended, key.release});
    }
    const bool upLeft = down.keys.left && !user.keys.left;
    const bool upRight = down.keys.right && !user.keys.right;
    if (upLeft || upRight)
        letGo(upLeft, upRight);
    press({.left = user.keys.left && !down.keys.left, .right = user.keys.right && !down.keys.right});
    // The user's Shift went down with nothing since, a tap to come, but the
    // window saw these keys after it: let go and pressed again, the release
    // to come is still a tap (pressed again with the keys waiting, it went in
    // as Shift+keys and the input method never switched).
    if (user.alone && down.keys.any() && !down.alone) {
        const Shifts sides = down.keys;
        letGo(sides.left, sides.right);
        press(sides);
    }
    if (down.keys.any() && down.alone && !user.alone)
        mask();
    return strokes;
}

// One hold of the keys typed in the Start menu or a file display, while the
// launcher is asked for (platform/TypeToSearch): which keys the hook takes,
// when and how they are typed again, and how the hold ends. The platform
// feeds it every key the hook sees and what is in front, does what each Step
// says (SendInput, the timer, asking for the launcher again) and ends the
// hold when one says so: its hook may go then, and the Raw Input set aside
// comes back. Every way out leaves Shift as the user holds it and no key down
// that the user let go of, and every state but Idle has the timer to end it,
// whatever goes by or not.
//
// The keys:
// - Win顺's own and the ones Windows makes up go on.
// - The release (or repeat) of a key whose press was taken is taken too: let
//   through it overtook its own press (a key stuck down, with Ctrl pressed
//   between). Safe for GitHub #91: no hook saw that press. Taken presses are
//   told apart by scan code, the virtual key can change between press and
//   release (a number pad key with NumLock on and Shift).
// - Any other release goes on: an older hook saw its press (GitHub #91).
// - A Shift press is held back while keys typed again are on their way or
//   the user's Shift is held among the keys (it went in among them, "macOS"
//   came out "mACOS"; it goes with them, as it came), never while Ctrl, Alt
//   or a Windows key is down (Ctrl+Shift+T).
// - Caps Lock's press is held with the keys typed around it: let through, it
//   changed the keys typed before it.
// - Ctrl, Alt, the Windows keys, NumLock, and any key with one of them held
//   (Ctrl+C, Alt+Tab, Win+E) go on at once. Nothing held is typed again while
//   one of those is down: it would make a shortcut too (proceed).
// - With AltGr, the key is held as the character it types (translated by
//   the platform; asked only then): typed again as the key, it would need
//   AltGr pressed with it. One that types nothing, or a dead key, goes on.
// - Everything else is held, with Shift as the user holds it.
class Hold {
public:
    struct KeyEvent {
        unsigned vk = 0;
        unsigned scanCode = 0; // as the hook has it: kSimulatedScan set on one Windows makes up
        bool extended = false;
        bool press = false;
        bool own = false; // Win顺's own (isOwnInput): typed again, the mask, the Start menu's Esc
    };
    enum class Take : std::uint8_t {
        No, // goes on
        Yes, // taken: nobody sees it, it is typed again later (or dropped)
        YesMask, // ... and the mask goes to the window now: it saw Shift go down, then nothing
    };
    // What is in front, as the platform finds it when the hold goes on.
    struct Front {
        bool ours = false; // a window of Win顺's
        bool startMenu = false; // the Start menu or Windows' search
        bool sourceFocused = false; // the file display the keys were typed in has the focus (always for the Start menu's)
    };
    struct Step {
        std::vector<Stroke> send; // to type now as Win顺's own (the mask with its mark); then sent()
        bool intoLauncher = false; // ... into the launcher (the timing of the diagnostics)
        bool askAgain = false; // ask for the launcher again: its Esc closes the Start menu
        bool lookAgain = false; // waiting for Win顺 to be in front, or a shortcut let go: lookAgain() shortly and at foreground events
        std::optional<unsigned> timerMs; // the hold's one timer, from now: timeout()
        bool ended = false; // the hold is over
    };

    bool holding() const noexcept { return m_phase != Phase::Idle; }
    bool forFolder() const noexcept { return m_forFolder; }

    // The first key was taken (key() next takes it): in a file display or the
    // Start menu, Shift as GetAsyncKeyState has it (nothing of Win顺's on its
    // way now), Ctrl, Alt and the Windows keys up (else no hold).
    Step begin(bool forFolder, Shifts shifts, std::uint32_t now)
    {
        *this = Hold {};
        m_phase = Phase::Waiting;
        m_forFolder = forFolder;
        m_window = m_user = {.keys = shifts, .alone = shifts.any()};
        m_startedAt = now;
        m_askedAt = now;
        m_deadlineMs = kWaitForLauncherMs;
        m_onDeadline = true;
        return {.timerMs = kWaitForLauncherMs};
    }

    // A key at the hook, before the rest of the chain: whether it is taken.
    // `altGrChars(key, Shifts, capsLockFlipped)` -> std::u16string: what the
    // key types with AltGr, Shift as the user holds it, Caps Lock flipped by
    // the presses held; empty for none or a dead key.
    template <class AltGrChars>
    Take key(const KeyEvent& key, AltGrChars&& altGrChars)
    {
        if (m_phase == Phase::Idle || m_phase == Phase::Ending || key.own || madeUp(key.vk, key.scanCode))
            return Take::No;
        const std::uint32_t id = keyId(key);
        const auto taken = std::ranges::find(m_taken, id, &TakenKey::id);
        if (taken != m_taken.end()) {
            m_user.seen(taken->press.vk, key.press);
            if (!key.press) {
                hold(*taken, true);
                m_taken.erase(taken);
                return Take::Yes;
            }
            if (isShift(taken->press.vk) || taken->press.vk == kVkCapital)
                return Take::Yes; // held down, it says nothing new (sent again, Caps Lock could flip again)
            hold(*taken, false);
            return maskIfAlone();
        }
        if (!key.press)
            return Take::No;
        if (isShift(key.vk)) {
            if (m_modifiers.chord() || !(m_retyped.pending() || heldShift()))
                return Take::No;
            const bool right = key.vk == kVkRShift || (key.vk == kVkShift && (key.scanCode & 0xFF) == kRightShiftScan);
            take(id, {.vk = right ? kVkRShift : kVkLShift, .scanCode = key.scanCode, .extended = key.extended}, {});
            m_user.seen(right ? kVkRShift : kVkLShift, true);
            return Take::Yes;
        }
        if (key.vk == kVkCapital && !m_modifiers.chord()) { // Ctrl+Caps Lock (a Japanese input method's) goes on
            take(id, {.vk = kVkCapital, .scanCode = key.scanCode, .extended = key.extended}, {});
            m_capsLockFlipped = !m_capsLockFlipped;
            m_user.seen(key.vk, true);
            return maskIfAlone();
        }
        if (isModifier(key.vk) || m_modifiers.chord())
            return Take::No;
        std::u16string chars;
        if (m_modifiers.altGr()) {
            chars = altGrChars(key, m_user.keys, m_capsLockFlipped);
            if (chars.empty())
                return Take::No;
        }
        take(id, {key.vk, key.scanCode, key.extended, false, shifted()}, std::move(chars));
        m_user.seen(key.vk, true);
        return maskIfAlone();
    }

    // The key went on through the rest of the chain to the window (the hook
    // tells after CallNextHookEx, and only when no hook after it took the key):
    // counted in the order the window gets them. A hook later in the chain
    // (AutoHotkey, PowerToys, PixPin) that sends keys sends them from inside
    // that call: they get there, and here, before the key that made them.
    void went(const KeyEvent& key)
    {
        if (m_phase == Phase::Idle)
            return;
        m_window.seen(key.vk, key.press);
        if (key.own)
            return;
        m_modifiers.seen(key.vk, key.scanCode, key.press);
        if (madeUp(key.vk, key.scanCode)) {
            // NumLock's Shift for the number pad, not the user's: let go of
            // around the key (Home, not Shift+Home, with Shift held), which
            // is typed again so (shifted).
            if (isShift(key.vk))
                m_numberPadUnshifted = !key.press;
            return;
        }
        m_user.seen(key.vk, key.press);
        // The user's Shift press is held back with the keys, and another key
        // (Ctrl) went on before its release: typed again, the two would come
        // together, a tap the user never made. The mask goes between them.
        if (key.press && !isShift(key.vk) && !m_held.empty() && isShift(m_held.back().vk) && !m_held.back().release)
            m_held.push_back({.vk = kMaskKey});
    }

    // One of Win顺's own keys went down the chain: true when it was the last
    // of those sent (passed() next, once out of the hook).
    bool ownPassed(unsigned vk, bool release) noexcept { return m_phase != Phase::Idle && m_retyped.passed(vk, release); }

    // The launcher has the keyboard, and is ready for the keys or not (App's
    // handoff; not: it went before it was, they wait for its word again).
    Step deliver(bool ready, const Front& front, std::uint32_t now)
    {
        if (m_phase == Phase::Idle || m_phase == Phase::Ending)
            return {};
        m_target = Target::Launcher;
        m_launcherReady = ready;
        return m_retyped.pending() ? Step {} : proceed(front, now); // on their way: the rest follow them
    }
    // The launcher is not coming: the keys go back to the file display.
    Step giveBack(const Front& front, std::uint32_t now)
    {
        if (m_phase == Phase::Idle || m_phase == Phase::Ending)
            return {};
        m_target = Target::Source;
        return m_retyped.pending() ? Step {} : proceed(front, now);
    }
    // The last of those typed again went by.
    Step passed(const Front& front, std::uint32_t now)
    {
        if (m_phase == Phase::Idle || m_retyped.pending())
            return {};
        return proceed(front, now);
    }
    // Waiting for Win顺 to be in front, or for a shortcut's keys to be let go
    // (proceed): whether the keys can go now.
    Step lookAgain(const Front& front, std::uint32_t now)
    {
        if (m_phase != Phase::Waiting || m_target == Target::None)
            return {};
        return proceed(front, now);
    }
    // The timer.
    Step timeout(const Front& front, std::uint32_t now)
    {
        if (m_phase == Phase::Idle)
            return {};
        const bool sent = m_phase != Phase::Waiting;
        const unsigned due = sent ? kPassTimeoutMs : m_deadlineMs;
        const unsigned elapsed = now - (sent ? m_sentAt : m_startedAt);
        if (elapsed + kTimerSlackMs < due)
            return {.timerMs = due - elapsed}; // early (the deadline moved on since)
        switch (m_phase) {
        case Phase::Ending:
            return end(); // its fix-up never went by: as far as it got
        case Phase::Typing: {
            // Some typed again never went by (a hook before this one took
            // them): the rest go after them, never into another window, and
            // the hold ends once those have gone by (Ending: should SendInput
            // put in only some, sent() lets go of what went down).
            m_retyped.clear();
            if ((m_target == Target::Launcher && !front.ours) || (m_target == Target::Source && !front.sourceFocused))
                keepState();
            m_taken.clear();
            std::vector<Stroke> strokes = retype(m_held, m_window, m_user);
            m_held.clear();
            if (strokes.empty())
                return end();
            m_fixUps = 1; // these, and one more at most
            return send(std::move(strokes), Phase::Ending, false, now);
        }
        case Phase::Waiting:
            if (m_forFolder && !m_modifiers.chord()) { // typed in a file display: back to it rather than lost
                m_target = Target::Source;
                return proceed(front, now);
            }
            break;
        case Phase::Idle:
            return {};
        }
        // Typed in the Start menu, they would go into Windows' search; with a
        // shortcut still held, they would make shortcuts: dropped, all but
        // Shift, Caps Lock and the releases (keepState), which go as they came
        // (a tap too), then Shift as the user holds it, waited for (Ending).
        keepState();
        m_taken.clear();
        std::vector<Stroke> strokes = retype(m_held, m_window, m_user);
        m_held.clear();
        if (strokes.empty())
            return end();
        m_fixUps = 1; // this, and one more at most
        return send(std::move(strokes), Phase::Ending, false, now);
    }
    // SendInput put in only `inserted` of the last Step's keys (blocked: the
    // secure desktop, a window of higher integrity in front). What went down
    // among those is let go, Shift goes as the user holds it from where those
    // left it, tried once, and the hold ends: tried again, it could go on
    // failing.
    Step sent(std::size_t inserted)
    {
        if (m_phase == Phase::Idle || inserted >= m_batch.size())
            return {};
        ShiftState window = m_window;
        std::vector<Stroke> strokes;
        for (std::size_t i = 0; i < inserted; ++i) {
            const Stroke& stroke = m_batch[i];
            window.seen(stroke.vk, !stroke.release);
            if (isShift(stroke.vk) || stroke.vk == kMaskKey)
                continue;
            const Stroke up {stroke.vk, stroke.scanCode, stroke.extended, true};
            if (stroke.release)
                std::erase(strokes, up);
            else
                strokes.push_back(up);
        }
        std::ranges::copy(retype({}, window, m_user), std::back_inserter(strokes));
        Step step = end();
        step.send = std::move(strokes);
        return step;
    }
    // Win顺 lets go of the hold (TypeToSearch goes: Win顺 closes, the
    // settings changed): the keys held are dropped but for Shift, Caps Lock
    // and the releases (keepState), which go as they came, then Shift as the
    // user holds it, from where the keys still on their way leave it. Nothing
    // is waited for.
    Step stop()
    {
        if (m_phase == Phase::Idle)
            return {};
        ShiftState window = m_window;
        if (m_retyped.pending()) {
            window = m_sentFrom; // the batch goes on to the window all the same
            for (const Stroke& stroke : m_batch)
                window.seen(stroke.vk, !stroke.release);
        }
        keepState();
        std::vector<Stroke> strokes = retype(m_held, window, m_user);
        Step step = end();
        step.send = std::move(strokes);
        return step;
    }

private:
    enum class Phase : std::uint8_t {
        Idle,
        Waiting, // keys held for the launcher, or (folder) for the word that it is not coming
        Typing, // a batch typed again, on its way
        Ending, // Shift set right after the keys were dropped, on its way; keys go on meanwhile
    };
    enum class Target : std::uint8_t {
        None, // no word yet
        Launcher, // deliver(): to the launcher, once in front and ready
        Source, // giveBack(), or a hold in a file display given up: to the file display, if it has the focus
    };
    struct TakenKey {
        std::uint32_t id = 0; // keyId
        HeldKey press;
        std::u16string chars; // typed with AltGr: these characters, pressed and let go
    };
    // A batch of Shift and the mask alone in a row at most this often: one
    // that never gets there (a hook after this one took Win顺's Shift) would
    // otherwise be sent again and again.
    static constexpr int kMaxFixUps = 2;
    // A thread timer can come a tick (15.6 ms) before GetTickCount says it is due.
    static constexpr unsigned kTimerSlackMs = 20;

    static constexpr std::uint32_t keyId(const KeyEvent& key) noexcept
    {
        if (key.vk == kVkPacket)
            return 0x1'0000 | (key.scanCode & 0xFFFF);
        if (key.scanCode & 0xFF)
            return (key.scanCode & 0xFF) | (key.extended ? 0x100 : 0);
        return 0x2'0000 | key.vk; // a program's key with no scan code
    }

    bool heldShift() const noexcept
    {
        return std::ranges::any_of(m_held, [](const HeldKey& key) { return isShift(key.vk); });
    }
    void take(std::uint32_t id, HeldKey press, std::u16string chars)
    {
        m_taken.push_back({id, press, std::move(chars)});
        hold(m_taken.back(), false);
    }
    void hold(const TakenKey& key, bool release)
    {
        if (!key.chars.empty()) {
            for (const char16_t c : key.chars)
                m_held.push_back({.vk = kVkPacket, .scanCode = c, .release = release});
            return;
        }
        HeldKey held = key.press; // let go as it was pressed
        held.release = release;
        held.shift = !release && shifted();
        m_held.push_back(held);
    }
    // Shift on the key pressed now, as the user holds it, but for a number
    // pad key while Windows has let go of it for that (went).
    bool shifted() const noexcept { return m_user.keys.any() && !m_numberPadUnshifted; }
    Take maskIfAlone() noexcept
    {
        if (!m_window.alone)
            return Take::Yes;
        m_window.alone = false; // the mask is on its way (went() sees it go by)
        return Take::YesMask;
    }
    // Only what changes the keyboard's state is kept: Shift, Caps Lock (and
    // the mask between the user's Shift keys) and every release; the typing
    // goes. A release goes even when its press is dropped here: that press
    // can have got to the window before (typed again in an earlier batch, or
    // a key held down since before the hold, its repeats taken), and the
    // release was taken (a press of it was), so dropped, the key stayed down
    // for Windows. One whose press never got there changes nothing. Where
    // that leaves a Shift tap the user never made (Shift+B, B dropped: the
    // input method switched), the mask goes in before the release, or at the
    // end for a Shift still held (its release comes later).
    void keepState()
    {
        std::vector<HeldKey> kept;
        kept.reserve(m_held.size() + 1);
        ShiftState all; // as the window would have had it, with every key
        ShiftState left; // ... with those kept
        const auto maskIfTap = [&] {
            if (left.keys.any() && left.alone && !all.alone) {
                kept.push_back({.vk = kMaskKey});
                left.seen(kMaskKey, true);
            }
        };
        for (const HeldKey& key : m_held) {
            if (isShift(key.vk) && key.release)
                maskIfTap();
            all.seen(key.vk, !key.release);
            if (!key.release && !isShift(key.vk) && key.vk != kVkCapital && key.vk != kMaskKey)
                continue; // the typing
            kept.push_back(key);
            left.seen(key.vk, !key.release);
        }
        maskIfTap();
        m_held = std::move(kept);
    }
    unsigned untilDeadline(std::uint32_t now) const noexcept
    {
        const unsigned elapsed = now - m_startedAt;
        return elapsed < m_deadlineMs ? m_deadlineMs - elapsed : 0;
    }

    Step proceed(const Front& front, std::uint32_t now)
    {
        if (m_phase == Phase::Ending)
            return fixUpOrEnd(Phase::Ending, now);
        m_phase = Phase::Waiting;
        // A shortcut held (it went on at once): the keys wait for its Ctrl,
        // Alt or Windows key to be let go. Typed now, each made one too: with
        // Ctrl+Shift+F24 held as the launcher got ready, the b of "baidu" came
        // as Ctrl+B ("aidu", live), and the launcher's own Ctrl+O could open a
        // result. Looked at again shortly, until the hold runs out (timeout).
        if (m_modifiers.chord() && !m_held.empty() && m_target != Target::None) {
            Step step;
            step.lookAgain = true;
            return waiting(std::move(step), now);
        }
        if (m_target == Target::Source) {
            if (!front.sourceFocused)
                keepState(); // the user moved on: never typed into another window or edit field
            return m_held.empty() ? fixUpOrEnd(Phase::Typing, now) : sendHeld(false, now);
        }
        if (m_target == Target::None)
            return waiting({}, now);
        if (m_held.empty())
            return fixUpOrEnd(Phase::Typing, now); // Shift alone: whoever is in front
        Step step;
        switch (delivery({.oursInFront = front.ours, .launcherReady = m_launcherReady, .startMenuInFront = front.startMenu,
            .forStartMenu = !m_forFolder, .typedAlready = m_typedAlready, .asksAgain = m_asksAgain,
            .msSinceAsked = now - m_askedAt})) {
        case Delivery::Type:
            m_typedAlready = true;
            return sendHeld(true, now);
        case Delivery::AskAgain:
            ++m_asksAgain;
            m_askedAt = now;
            // Shown anew, it says when it is ready again (App's handoff,
            // GitHub #103), with a little more time (holdDeadline).
            m_launcherReady = false;
            step.askAgain = true;
            if (const unsigned deadline = holdDeadline(m_deadlineMs, now - m_startedAt); deadline != m_deadlineMs) {
                m_deadlineMs = deadline;
                m_onDeadline = false;
            }
            break;
        case Delivery::Wait:
            break;
        }
        // Not into the window in front, which is not Win顺's: the keys wait.
        // Shift goes right meanwhile all the same (a capital typed again
        // before could have left it down for the window), unless the user's
        // own Shift is among the keys held: it goes with them.
        if (m_window.keys != m_user.keys && !heldShift() && m_fixUps < kMaxFixUps) {
            Step fix = fixUpOrEnd(Phase::Typing, now);
            fix.askAgain = step.askAgain;
            fix.lookAgain = true;
            return fix;
        }
        step.lookAgain = true;
        return waiting(std::move(step), now);
    }
    Step waiting(Step step, std::uint32_t now)
    {
        if (!std::exchange(m_onDeadline, true))
            step.timerMs = untilDeadline(now);
        return step;
    }
    Step sendHeld(bool intoLauncher, std::uint32_t now)
    {
        std::vector<Stroke> strokes = retype(m_held, m_window, m_user);
        m_held.clear();
        m_fixUps = 0;
        return send(std::move(strokes), Phase::Typing, intoLauncher, now);
    }
    Step fixUpOrEnd(Phase phase, std::uint32_t now)
    {
        if (m_window.keys == m_user.keys || m_fixUps >= kMaxFixUps)
            return end(); // as the user has it, or as far as it gets
        ++m_fixUps;
        return send(retype({}, m_window, m_user), phase, false, now);
    }
    Step send(std::vector<Stroke> strokes, Phase phase, bool intoLauncher, std::uint32_t now)
    {
        m_phase = phase;
        m_retyped.sent(awaited(strokes));
        m_batch = strokes;
        m_sentFrom = m_window; // none of them gone by yet: the hook sees them once its thread waits for messages again
        m_sentAt = now;
        m_onDeadline = false;
        return {.send = std::move(strokes), .intoLauncher = intoLauncher, .timerMs = kPassTimeoutMs};
    }
    Step end()
    {
        *this = Hold {};
        return {.ended = true};
    }

    Phase m_phase = Phase::Idle;
    Target m_target = Target::None;
    bool m_forFolder = false;
    bool m_launcherReady = false; // the launcher's word that it is ready, since it was last asked for
    std::vector<HeldKey> m_held;
    std::vector<TakenKey> m_taken; // presses taken whose release has not come
    ShiftState m_window; // as the window in front has it: what got to it, Win顺's own too (went)
    ShiftState m_user; // as the user holds it: their keys, taken or gone on
    Modifiers m_modifiers; // the user's, gone on (they are never taken)
    bool m_capsLockFlipped = false; // Caps Lock pressed an odd number of times among the keys held
    bool m_numberPadUnshifted = false; // Windows let go of Shift for a number pad key (NumLock on)
    Retyped m_retyped;
    std::vector<Stroke> m_batch; // the last sent (sent(), stop())
    ShiftState m_sentFrom; // the window's when it was sent
    std::uint32_t m_startedAt = 0; // GetTickCount
    std::uint32_t m_askedAt = 0; // the launcher last asked for
    std::uint32_t m_sentAt = 0;
    unsigned m_deadlineMs = 0; // since m_startedAt
    bool m_onDeadline = false; // the timer is on it (not on keys sent)
    int m_asksAgain = 0;
    bool m_typedAlready = false; // some typed into the launcher (it is not asked for again then)
    int m_fixUps = 0; // batches of Shift alone in a row
};

// The process's keyboard Raw Input registration (KeyListener's), set aside
// while a hook of Win顺's must see the keys typed into Win顺's own windows:
// while a process takes the keyboard's Raw Input, Windows calls no low-level
// keyboard hook of that process's first in line with one of its windows in
// front, nor any hook after it (docs/pitfalls.md). TypeToSearch's hold needs
// that (GitHub #66: "baidu" came out "dbaiu"), ShortcutCapture's recording
// too, and they can overlap (a shortcut recorded with a hold still on), so
// the set-aside is shared and counted (KeyboardRawInputAside, KeyListener.h):
// the first holder takes the registration away, the last puts it back. Each
// on its own put back what it had found, one ending put it back under the
// other, and one that found it gone already kept nothing to put back. A
// KeyListener that registers meanwhile (the settings changed) is what goes
// back then, one whose window goes takes its own along, and a registration
// Windows refuses to put back is kept for the next holder to go: double Ctrl
// is never lost for good.
class RawInputAside {
public:
    struct Registration {
        std::uintptr_t window = 0; // its target window (HWND)
        unsigned flags = 0; // RIDEV_INPUTSINK
        bool operator==(const Registration&) const = default;
    };

    // A holder comes: true when it is the first, which takes the registration
    // away and says what it took (taken).
    constexpr bool take() noexcept { return m_holders++ == 0; }
    // ... none, when there was none or Windows kept it.
    constexpr void taken(std::optional<Registration> removed) noexcept
    {
        if (removed)
            m_back = removed;
    }
    // A holder goes: the registration to put back, when it was the last.
    constexpr std::optional<Registration> give() noexcept
    {
        if (m_holders == 0 || --m_holders > 0)
            return std::nullopt;
        return std::exchange(m_back, std::nullopt);
    }
    // ... Windows refused it: kept for the next holder that goes.
    constexpr void notPutBack(Registration registration) noexcept
    {
        if (!m_back)
            m_back = registration;
    }
    // KeyListener registers its window: true when that goes to Windows now;
    // while set aside, it is what goes back (the newest).
    constexpr bool registers(Registration registration) noexcept
    {
        if (m_holders == 0) {
            m_back.reset(); // one kept from before is superseded
            return true;
        }
        m_back = registration;
        return false;
    }
    // KeyListener's window goes: true when its registration is Windows' to
    // remove now; while set aside, the one kept goes if it is that window's.
    constexpr bool removes(std::uintptr_t window) noexcept
    {
        if (m_back && m_back->window == window)
            m_back.reset();
        return m_holders == 0;
    }
    constexpr bool aside() const noexcept { return m_holders > 0; }

private:
    int m_holders = 0;
    std::optional<Registration> m_back;
};

} // namespace ws::searchtyping

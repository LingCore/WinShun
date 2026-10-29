#pragma once

#include "KeyListener.h" // KeyboardRawInputAside
#include "SearchTyping.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ws {

// Typing somewhere else searches with Win顺: in the Start menu instead of
// Windows (a setting, off by default; EverythingToolbar does the same), and
// in a File Explorer or Directory Opus file display, in the folder it shows
// (independent settings, off by default, as Listary does it; it takes the
// place of the file manager's own jump to a name by its first letters).
//
// Windows' search takes over the Start menu as soon as one types there. Here
// the first key that types something while the Start menu (or Windows'
// search) is in front is taken from it, and the launcher is asked for: it
// opens over the taskbar. In Explorer, the first key typed while the files
// and folders shown have the keyboard (not a name being renamed, the address
// bar, its search box or the navigation pane) and no menu of the window's is
// open (its keys are the menu's: 属性(R), GitHub #37) is taken the same way,
// and the launcher is asked for with that window's folder (typedInExplorer).
// Opus uses the same handoff for a letter or digit in its file display
// (typedInOpus); symbols keep its own find modes. That
// key and the ones typed after it are held back until the launcher has the
// keyboard (deliver), then typed again in order (SendInput, marked as
// Win顺's own): they go through the input method as if typed into the search
// box, each with Shift as it was pressed (searchtyping::Hold, which decides
// it all; Shift itself goes on as it comes unless keys typed again are on
// their way, a mask key keeping it from looking pressed alone to an input
// method, GitHub #103). Only while a window of Win顺's is really in front:
// the Start menu can take the front back from the launcher, and the keys then
// wait (and the launcher is asked for again) rather than go into Windows'
// search (GitHub #66). The hold lasts until those have gone down the hook
// chain: a key typed in the meantime follows them instead of slipping in
// among them (GitHub #39), also once the launcher is in front, for which the
// process's keyboard Raw Input is set aside meanwhile (KeyboardRawInputAside).
// When the folder turns out to be none (This PC, Home, the Recycle Bin), the
// keys are given back (giveBack), but only if the original window and file
// display still have the focus. Expired requests cannot open a launcher.
// However the hold ends, Shift is left as the user holds it.
//
// A low-level keyboard hook, which every key press in the system waits on
// (see KeyListener for why that matters): installed only while the Start
// menu, Windows' search or an opted-in file manager window is in front, as an
// out-of-context event on the foreground tells (or an Explorer window raised
// that event from behind: opened in the background, it comes to the front
// later with no event, so the key checks what is in front), and while keys are held
// back; on a thread of its own, deciding at once from the key and the
// window with the focus alone (core/TypeToSearch.h; no messages sent). Win,
// Ctrl and Alt combinations stay Windows'. Left Alt pressed in the Start
// menu leaves its search alone until it closes: a way to Windows' own search
// when it is wanted. An elevated Explorer is left alone: Windows would show
// the hook none of its keys, nor take them back (UIPI).
//
// 粘贴为文件 (a third setting, off by default) takes Ctrl+V from the files
// and folders of an Explorer folder window or the desktop's icons when the
// clipboard holds words or a picture and no files (pastefile::takesCtrlV:
// Explorer pastes none of those, so the key did nothing there): the press,
// its repeats and V's release go to nobody else, and WinShunSearch.exe is
// started to make the file there (pastedInto). The clipboard panel's own
// Ctrl+V counts as well. With files on the clipboard, or anything else having
// the focus, Ctrl+V stays Explorer's.
class TypeToSearch {
public:
    using RequestId = std::uint64_t;
    struct Surfaces {
        bool startMenu = false;
        bool explorer = false; // typing searches the folder
        bool opus = false; // independent opt-in for Directory Opus
        bool pasteFile = false; // Ctrl+V pastes the clipboard as a file
        bool operator==(const Surfaces&) const = default;
    };
    struct Callbacks {
        // The first key was taken: show the launcher. On the hook's thread.
        std::function<void()> typed; // in the Start menu: over the taskbar
        // ... in the Explorer window `root`, whose tab (ShellTabWindowClass,
        // one per tab; `root` itself should it have none) shows the folder
        // to search; `vk` is the key, for the log.
        std::function<void(HWND root, HWND tab, unsigned vk, RequestId request)> typedInExplorer;
        // ... in Opus's focused file display; `tab` is its native window.
        std::function<void(HWND root, HWND tab, unsigned vk, RequestId request)> typedInOpus;
        // Ctrl+V was taken in `view`: the Explorer tab, or the desktop's
        // window (Progman or WorkerW). On the hook's thread.
        std::function<void(HWND view)> pastedInto;
    };

    TypeToSearch(Surfaces surfaces, Callbacks callbacks);
    ~TypeToSearch();

    TypeToSearch(const TypeToSearch&) = delete;
    TypeToSearch& operator=(const TypeToSearch&) = delete;

    Surfaces surfaces() const noexcept { return m_surfaces; }

    // The launcher has the keyboard: the keys held back are typed into it.
    // `ready` false: it went before it was ready (hidden): the keys stay for
    // it, typed once it says so again (shown anew, asked for again from the
    // Start menu; searchtyping::delivery). Any thread.
    void deliver(bool ready = true);
    // The launcher is not coming (no folder there): the keys held back go on
    // to the window they were typed in. Any thread.
    void giveBack(RequestId request = 0);
    // A delayed folder query belongs only to the hold that asked for it,
    // even after a timeout or a settings change made a new hook instance.
    bool pending(RequestId request) const noexcept { return request && m_pendingRequest.load() == request; }

    // The last hold, for the diagnostics of a handoff (App, GitHub #103): its
    // first key, when its keys were first typed again into the launcher and
    // how many presses that was. Never which keys. Any thread.
    struct Timing {
        std::chrono::steady_clock::time_point firstKey {};
        std::chrono::steady_clock::time_point sent {}; // epoch: not typed again (yet)
        int presses = 0;
    };
    Timing timing() const noexcept;

private:
    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static void CALLBACK foregroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD);
    bool handleKey(WPARAM message, const KBDLLHOOKSTRUCT& event); // true: taken
    bool startsHold(UINT vk, bool press);
    void passedOn(WPARAM message, const KBDLLHOOKSTRUCT& event, bool delivered); // delivered: no hook after took it
    bool handlePaste(bool press); // V with Ctrl alone held, or let go; true: taken
    bool explorerInFront(); // m_explorer or m_explorerBehind is in front, m_explorer then
    bool explorerFocused(HWND& tab) const; // the view of m_explorer's folder has the focus: its tab
    bool opusFocused(HWND& tab) const;
    bool sourceFocused() const; // keys returned only to their original file display
    bool desktopFocused() const; // m_desktop is in front, its icons have the focus
    void foregroundChanged(HWND window, HWND named); // window: in front now; named: the event's
    bool watchedExplorer(HWND window) const; // an Explorer folder window, typing or pasting there on, not elevated
    static bool activeBehind(HWND window); // the active window of its thread (not in front: it comes silently)
    HWND openedBehind(HWND front) const; // a folder window that opened behind another before the start, or none
    bool wantsHook() const; // something is in front whose keys count, or keys are held
    void setHooked(bool hooked);
    searchtyping::Hold::Front front() const;
    void apply(searchtyping::Hold::Step step);
    void restartTimer(UINT ms);
    void endHolding();

    const Surfaces m_surfaces;
    Callbacks m_callbacks;
    std::atomic<DWORD> m_threadId = 0;
    std::atomic<RequestId> m_pendingRequest = 0;
    // timing(): steady_clock ticks, written on the hook's thread.
    std::atomic<std::chrono::steady_clock::rep> m_firstKeyAt = 0;
    std::atomic<std::chrono::steady_clock::rep> m_sentAt = 0;
    std::atomic<int> m_sentPresses = 0;

    // The hook's thread only.
    HHOOK m_hook = nullptr;
    bool m_startInFront = false;
    HWND m_explorer = nullptr; // the Explorer folder window in front, when typing there counts
    // ... one that said it came forward while another stayed in front: it
    // gets there later with no event (a click), the key checks; kept through
    // later events while it is still its thread's active window (activeBehind)
    HWND m_explorerBehind = nullptr;
    HWND m_opus = nullptr; // the Opus lister in front, if opted in
    HWND m_sourceWindow = nullptr; // the hold's originating folder window and focus
    HWND m_sourceFocus = nullptr;
    HWND m_desktop = nullptr; // the desktop in front, when pasting there counts
    bool m_pasteTaken = false; // Ctrl+V taken, V not let go yet: its repeats and release are taken too
    bool m_windowsSearch = false; // Left Alt in the Start menu: its own search this time
    // The keys held back for the launcher (or the file display they were
    // typed in), the keys typed again, Shift, and how the hold ends.
    searchtyping::Hold m_hold;
    UINT_PTR m_timer = 0; // while holding: the hold's one (Step::timerMs)
    UINT_PTR m_frontTimer = 0; // ... waiting for Win顺 to be in front: looks again
    std::optional<KeyboardRawInputAside> m_rawInputAside; // while holding (GitHub #66)

    std::jthread m_thread; // last: joined before the members above go
};

// The launcher's window for a moment after the keys held for it went (App,
// for the diagnostics of a handoff, GitHub #103): when the first of them
// reached it and whether the input method took it (VK_PROCESSKEY), how many
// presses came, when the input method began a composition, when it showed or
// changed its candidates (one with a window of its own writes nothing into the
// field until a word is chosen), when the field first got a composition or
// text (App tells, from the field's events: Qt takes a key's WM_CHAR out of
// the queue itself, it never comes to the window), and when the first frame
// with that was on the screen. Times and counts alone, never which keys. A
// subclass of the window while it watches (GUI thread), beside
// shellcontext::watchKeys' own.
class LauncherKeysWatch {
public:
    using Clock = std::chrono::steady_clock;
    struct Seen {
        Clock::time_point key {}; // epoch: none came
        bool imeTook = false;
        int keys = 0;
        Clock::time_point composition {}; // WM_IME_STARTCOMPOSITION
        Clock::time_point candidates {}; // IMN_OPENCANDIDATE or IMN_CHANGECANDIDATE
        Clock::time_point preedit {}; // the field got a composition (fieldGot)
        Clock::time_point text {}; // ... text: a key's character, or the input method's committed
        Clock::time_point drawn {}; // the first frame drawn after either
    };

    LauncherKeysWatch() = default;
    ~LauncherKeysWatch() { stop(); }
    LauncherKeysWatch(const LauncherKeysWatch&) = delete;
    LauncherKeysWatch& operator=(const LauncherKeysWatch&) = delete;

    void start(HWND window);
    Seen stop(); // what it saw; the subclass goes
    bool watching() const noexcept { return m_window != nullptr; }
    void fieldGot(bool text); // the launcher's field got a composition (false) or text (true); GUI thread
    // The window's scene graph (render thread, Qt::DirectConnection): its
    // state synchronized for a frame, and that frame on the screen.
    void synchronized() noexcept;
    void frameSwapped() noexcept;

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
    void changed(); // the field's text changed: its next frame is the one that shows it

    HWND m_window = nullptr;
    Seen m_seen;
    std::atomic<bool> m_changed = false; // GUI thread sets, render thread takes (synchronized)
    std::atomic<bool> m_inFrame = false; // ... the frame being drawn has it
    std::atomic<Clock::rep> m_drawn = 0;
};

// The input method the calling thread types with: its keyboard layout, and
// the text service's CLSID and name when one is on (TSF). For the
// diagnostics; nothing typed.
struct InputMethodInfo {
    std::uintptr_t layout = 0; // HKL
    std::wstring service; // "{...}", empty for a keyboard layout alone
    std::wstring name;
};
InputMethodInfo activeInputMethod();

bool isCloaked(HWND window); // DWM keeps it off the screen (cloaked by us, or by the shell)

} // namespace ws

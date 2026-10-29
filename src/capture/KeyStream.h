#pragma once

#include "Keycaps.h"

#include <QRect>

#include <array>
#include <atomic>
#include <bitset>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

#include <windows.h>

namespace ws::capture::recording {

// GitHub #98: the keys and mouse buttons for the keycaps, from KeyHook to
// the media worker, which drains it every few milliseconds. Bounded; what
// it held is cleared as it is taken, and all of it on a pause and at the end.
class KeyStream {
public:
    static constexpr size_t capacity = 64;
    struct Batch {
        std::array<keycaps::Key, capacity> keys {};
        size_t count = 0;
        std::span<const keycaps::Key> view() const { return {keys.data(), count}; }
    };
    void push(const keycaps::Key& key);
    Batch take();
    void discardBefore(qint64 time); // everything queued too
private:
    std::mutex m_mutex;
    Batch m_pending;
    qint64 m_after = 0;
};

// What KeyHook knows of the keys held, apart from Windows for the tests
// (tst_keycaps). `held` tells whether the system has a key down before the
// one the hook has (GetAsyncKeyState: it takes a key in only once the
// hooks are through with it), and is right also about what the hook never
// saw: keys pressed or let go while it was out, or on the secure desktop
// (AltGr+Del, a UAC prompt), or in a program run as administrator.
class KeyBook {
public:
    using Held = bool (*)(int vk);
    struct Seen {
        bool skip = false; // the left Ctrl Windows sends ahead of AltGr: no key
        std::uint16_t vk = 0; // the right Alt as AltGr: keycaps::kAltGrKey
        std::uint8_t mods = 0; // held before it
        bool repeat = false; // down already: no press
    };
    // A key from the hook; `time` its time stamp (milliseconds).
    Seen key(std::uint32_t vk, std::uint32_t scanCode, bool press, std::uint32_t time, Held held);
    std::uint8_t modifiers(Held held); // as the system has them (a click's)
    void resume(Held held); // the hook in again after being out

private:
    void settle(Held held);
    std::bitset<256> m_down; // keys seen down: their repeats are no presses
    std::array<std::uint32_t, 256> m_downAt {};
    bool m_fakeCtrl = false; // the left Ctrl Windows sends ahead of AltGr, down
    bool m_altGr = false; // the right Alt down as AltGr
};

// Input `lastInput` (GetLastInputInfo) came well after the hooks' last
// call (GetTickCount, both): Windows took them out. Wraps with the clock.
bool inputWentPast(std::uint32_t lastInput, std::uint32_t lastCall) noexcept;

// The low-level hooks behind the keycaps, on a thread of their own while a
// recording that shows them runs (HoldWatch's pattern: Windows calls the
// hooks on the thread that set them, from inside its GetMessage, and every
// key and click in the system waits for their answer: high priority, and
// nothing that waits in them).
//
// In only while the recording runs (not paused) and another program's
// window is in front. With one of Win顺's in front, its keyboard Raw Input
// (double Ctrl, the drop pie) keeps Windows from calling its own hook, and
// the hooks set before it (AutoHotkey's) too (docs/pitfalls.md); out, they
// are left alone and double Ctrl keeps working. So keys typed into Win顺's
// own windows in front are not shown; the clipboard panel takes no focus
// (the program under it stays in front, and its KeyRouter hook hands a key
// on before taking it): keys used there show like any others. Keys sent to
// a program run as administrator do not reach the hook either (UIPI). The
// click that brings another program forward goes by before they are in:
// the buttons still down when that switch puts them in are taken as its
// press. Windows takes a hook out without a word when it answers late
// (LowLevelHooksTimeout): input going on past their last call puts new
// ones in (checkDropped).
//
// What a key is goes into the KeyStream only: never into a log or the
// diagnostics. Shortcuts mode hands typing over as a press without the key.
class KeyHook {
public:
    struct Setup {
        keycaps::Mode mode = keycaps::Mode::Off;
        QRect region; // physical pixels: the buttons pressed inside it are shown
        quintptr excludedWindow = 0; // the recording panel: clicks on it are not
        std::shared_ptr<KeyStream> output;
    };
    explicit KeyHook(Setup setup);
    ~KeyHook();
    KeyHook(const KeyHook&) = delete;
    KeyHook& operator=(const KeyHook&) = delete;
    bool failed() const { return m_failed; } // the hooks could not go in
    void setPaused(bool paused);

private:
    static LRESULT CALLBACK keyboardProc(int code, WPARAM message, LPARAM lParam);
    static LRESULT CALLBACK mouseProc(int code, WPARAM message, LPARAM lParam);
    static void CALLBACK foregroundProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    bool install();
    void remove();
    void follow(HWND forward = nullptr); // forward: the window a switch to it brought forward
    void pressHeld(HWND forward);
    void pressButton(std::uint16_t vk, qint64 time, POINT point);
    void key(WPARAM message, const KBDLLHOOKSTRUCT& event);
    void button(WPARAM message, const MSLLHOOKSTRUCT& event);
    std::uint8_t modifiers();
    void checkDropped();
    void refresh(long gapMs);

    const Setup m_setup;
    std::jthread m_thread;
    std::atomic<DWORD> m_threadId {0};
    std::atomic<bool> m_paused {false};
    bool m_failed = false; // set before the constructor returns
    // The hook thread's own.
    HHOOK m_keyboard = nullptr;
    HHOOK m_mouse = nullptr;
    KeyBook m_book;
    DWORD m_calledAt = 0; // GetTickCount at the hooks' last call, or their going in
    DWORD m_ownLevel = 0; // Win顺's integrity level
    int m_refreshes = 0; // put in again after Windows took them out
    int m_refreshFailures = 0; // and could not be (logged the first few times)
};

} // namespace ws::capture::recording

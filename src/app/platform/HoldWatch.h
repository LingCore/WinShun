#pragma once

#include "HotkeySteps.h"
#include "PasteKeys.h"
#include "Paster.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace ws {

// The clipboard's hotkey held after it opened the window (GitHub #107, see
// hotkeysteps): a low-level keyboard hook that watches, on a thread of its
// own, only for as long as the hold lasts (a second or two). Every key press
// in the system waits on it, so it decides at once (hotkeysteps::Watch),
// never waits on the GUI thread and writes no log.
//
// What it saw goes to the GUI thread as messages posted to a window there,
// from inside the hook: before Windows matches the hotkey, so they queue in
// the order of the keys together with the hotkey's own WM_HOTKEY.
//
// It takes Esc (and its release), which then means the window's Esc
// whatever is held (Ctrl+Esc would open Start), and the repeats of the
// hotkey's key; the rest goes on. The hooks installed before it (a key
// remapper's) see each key first, as with KeyRouter: one they take is not
// looked at here. As it starts, with Alt, Shift or Win held, it sends the
// masking key (kMaskKey), so that letting go of those before the key is no
// modifier pressed alone for the program in front.
//
// Only while another program is in front, as the clipboard routes its keys:
// with a window of Win顺's own in front and keyboard Raw Input registered,
// Windows calls neither this process's first hook in the chain nor the ones
// after it (docs/pitfalls.md).
//
// The paste the hold ends with goes past it too (guardPaste): in the hook
// since the opening press, it sees each of the user's keys come, in order
// with the paste keys (pastekeys::Guard).
class HoldWatch {
public:
    struct Callbacks {
        // GUI thread, in the order of the keys; `time` the key's (hook) time.
        std::function<void(hotkeysteps::Note note, std::uint32_t time)> note;
        // The watch's thread, after drain(): the Esc it took is let go.
        std::function<void()> drained;
    };

    // `modifiers`: RegisterHotKey's MOD_ bits of the hotkey; `vk` its key.
    // Called on the GUI thread; the hook is in once it returns.
    HoldWatch(unsigned modifiers, unsigned vk, Callbacks callbacks);
    ~HoldWatch();

    HoldWatch(const HoldWatch&) = delete;
    HoldWatch& operator=(const HoldWatch&) = delete;

    // The hook is in, and the modifiers were all still down as it went in.
    bool isActive() const noexcept { return m_active.load(); }
    // Not active, for the journal: "keys-up" (let go already), or the step
    // that failed ("no-class", "no-window", "no-hook") and its GetLastError.
    const char* inactiveReason() const noexcept { return m_active ? nullptr : m_failed ? m_failed : "keys-up"; }
    unsigned long setupError() const noexcept { return m_setupError; }
    // No more notes (dropped from now on, those queued too); it stays until
    // an Esc it took is let go, and the paste's keys passed (drained).
    void drain();
    // The paste the hold ends with, before drain(): the guard its keys go
    // past (paste::sendPasteKeys); drained waits for them, or for the guard
    // to go unused. Null without the hook.
    std::unique_ptr<paste::Guard> guardPaste();
    // Its keys on their way past the hook (a second at most): not to be
    // destroyed now, a modifier could be left as the hand no longer has it.
    bool pastePassing() const noexcept;

private:
    struct Paste;
    class PasteGuard;

    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleKey(int code, WPARAM message, LPARAM lParam);
    bool passOwn(std::uint16_t side, bool up);
    void settlePaste();
    bool pasteAhead() const noexcept;
    void post(hotkeysteps::Note note, std::uint32_t time) const;
    void checkDrained();

    const unsigned m_modifiers;
    const unsigned m_vk;
    Callbacks m_callbacks;
    HWND m_window = nullptr; // on the GUI thread: where the notes go
    const char* m_failed = nullptr; // set before the constructor returns
    DWORD m_setupError = 0;
    std::optional<hotkeysteps::Watch> m_watch; // the watch's thread only
    pastekeys::Guard m_guard; // the watch's thread only
    const std::shared_ptr<Paste> m_paste; // with the paste's guard (the GUI thread)
    bool m_drainedTold = false; // the watch's thread only
    std::atomic<bool> m_draining {false};
    std::atomic<bool> m_active {false};
    std::atomic<DWORD> m_threadId {0};
    std::jthread m_thread;
};

} // namespace ws

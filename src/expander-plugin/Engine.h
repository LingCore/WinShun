// The working part of the winshun.expander plugin: low-level keyboard and
// mouse hooks on a thread of their own that follow what is typed and put a
// phrase in place of its abbreviation.
//
// The keys are read as the program in front reads them (its keyboard
// layout, Shift, Caps Lock, AltGr), into what has been typed since the caret
// last went elsewhere: a click, another window, an arrow or other key that
// types nothing, a shortcut, a dead key. Backspace takes a character off.
//
// A key that types a space, punctuation, Enter or Tab ends an abbreviation
// (Expander.h finds it); with "instant", its last letter does. That key is
// held back; Backspaces, the phrase (KEYEVENTF_UNICODE, a new line as
// Shift+Enter) and the key held back again go in one SendInput, so the
// typing cannot come in between. Backspace as the very next key takes it
// back: the phrase out, what was typed in again.
//
// Left alone: an input method writing in its own language (the letters are
// its to turn into words), a menu that is open, a window of a program run
// as administrator (Windows would drop the keys sent, UIPI), the programs
// in the exclusions and Win顺 itself (its settings are where abbreviations
// are written), and keys other programs send.
#pragma once

#include "Expander.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace expander {

struct Settings {
    std::vector<Phrase> phrases;
    bool instant = false; // an abbreviation's last letter, not the key after it, puts the phrase in
    std::vector<std::wstring> excluded; // programList()
};

struct EngineOptions {
    std::function<void(const std::string&)> log;
};

class Engine {
public:
    explicit Engine(EngineOptions options);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void start();
    void stop();
    void configure(Settings settings);
    void setPaused(bool paused) { m_paused = paused; }
    bool paused() const { return m_paused; }
    int expanded() const { return m_expanded; }

    // The hook thread's side (static: one hook per process).
    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam);

private:
    struct Undo {
        std::size_t erase = 0; // Backspaces: the phrase and the end key's character
        std::wstring typed; // what was typed: the abbreviation and that character
    };

    std::shared_ptr<const Settings> shared() const;
    void hookThread(std::promise<bool>* ready);
    bool onKey(WPARAM message, const KBDLLHOOKSTRUCT& key);
    void forget();
    void push(std::wstring_view chars);
    std::wstring translate(const KBDLLHOOKSTRUCT& key, bool& dead) const;
    bool eligible(HWND foreground, const Settings& s) const;
    bool expand(const Match& match, const KBDLLHOOKSTRUCT* end, std::wstring_view endChars);
    bool takeBack(const Undo& undo);
    bool send(std::vector<INPUT>& input);

    EngineOptions m_options;
    mutable std::mutex m_mutex;
    std::shared_ptr<const Settings> m_shared;
    std::atomic_bool m_paused {false};
    std::atomic_int m_expanded {0};
    std::thread m_thread;
    DWORD m_threadId = 0;

    // Hook thread only.
    std::wstring m_typed; // since the caret last went elsewhere, the last few
    HWND m_foreground = nullptr;
    bool m_afterDead = false; // a dead key: the next character is made with it, unknown here
    bool m_altAlone = false; // Alt down and nothing else yet: let go, it opens the menu bar
    std::optional<Undo> m_undo; // until the next key or click
    bool m_capsLock = false; // the toggles, followed key by key (GetKeyState is this thread's)
    bool m_numLock = false;
    bool m_capsHeld = false;
    bool m_numHeld = false;
};

} // namespace expander

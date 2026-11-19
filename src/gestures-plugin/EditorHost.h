// The plugin's side of the settings window: starts `WinShunGestures.exe
// --editor` when 设置 is pressed (or brings the one that is open to the
// front), hands it the settings and the look as JSON lines on its stdin, and
// reads what it changes from its stdout. Lines, one JSON object each, UTF-8:
//   plugin -> window  {"type":"init" | "settings", "rules":"<rules JSON>", "exclude":"a.exe;b.exe",
//                      "trail":true, "fullScreen":false, "paused":false, "appearance":{…}}
//                     {"type":"appearance", "appearance":{…}}   {"type":"show"}   {"type":"quit"}
//   window -> plugin  {"type":"save", "rules":"…"} / {"exclude":"…"} / {"trail":true} / {"fullScreen":false}
//                     (any of them in one line)   {"type":"pause", "paused":true}
// The window saves each change at once; there is no OK or Cancel. Its
// stderr is the plugin's own (plugins.log).
#pragma once

#include "winshun.h"

#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace gestures {

class EditorHost {
public:
    struct Hooks {
        // Declared settings changed in the window: any of rules, exclude, trail, fullScreen. Called on the reader thread.
        std::function<void(winshun::Json values)> save;
        // 暂停鼠标手势 turned on or off in the window. Called on the reader thread.
        std::function<void(bool paused)> pause;
        // The window's process, for the engine to leave its windows alone (0: it ended). Not on the serve thread.
        std::function<void(DWORD pid)> own;
        std::function<void(const std::string&)> log;
    };

    explicit EditorHost(Hooks hooks);
    ~EditorHost();
    EditorHost(const EditorHost&) = delete;
    EditorHost& operator=(const EditorHost&) = delete;

    // Starts the window with `state` (rules, exclude, trail, fullScreen, paused, appearance), or
    // brings the one there to the front. False when it could not be started.
    bool show(winshun::Json state);
    // The settings (and what is paused) changed: the window learns it. Nothing when it is not open.
    void update(winshun::Json state);
    void appearance(const winshun::Json& appearance);
    // 暂停 changed outside the window (the launcher's row).
    void pausedChanged(bool paused);
    bool running();
    // Asks it to quit, waits up to 2 s, then ends it.
    void close();

private:
    bool startLocked();
    void readLoop(HANDLE read);
    void writeLocked(const winshun::Json& message);
    void finishLocked();

    Hooks m_hooks;
    std::mutex m_mutex;
    winshun::Json m_state; // the last the window was given
    HANDLE m_process = nullptr;
    HANDLE m_stdin = nullptr; // the window's stdin, our write end
    DWORD m_pid = 0;
    bool m_alive = false;
    std::thread m_reader;
};

} // namespace gestures

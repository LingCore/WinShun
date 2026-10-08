#pragma once

#include <QTimer>

#include <windows.h>

#include <functional>

namespace ws {

// Listary's Ctrl+G: in an Open or Save dialog, it takes the dialog to the
// folder shown in the File Explorer window used last.
//
// Ctrl+G is a hotkey only while such a dialog is in front (a WinEvent hook
// follows the foreground window), so everywhere else it keeps its meaning.
// The dialog is driven from outside with window messages alone, as a user
// would: a click past the last crumb of its address bar turns that into a
// text box, which gets the folder's path and an Enter. Nothing is loaded
// into the other program and no keys are synthesised.
class DialogJump {
public:
    // `setHotkey(true)` registers Ctrl+G and says whether that worked;
    // `setHotkey(false)` drops it. Both on the GUI thread.
    explicit DialogJump(std::function<bool(bool on)> setHotkey);
    ~DialogJump();

    DialogJump(const DialogJump&) = delete;
    DialogJump& operator=(const DialogJump&) = delete;

    void jump(); // Ctrl+G was pressed

private:
    static void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD);
    void follow(HWND foreground);
    void setRegistered(bool on);

    std::function<bool(bool)> m_setHotkey;
    HWINEVENTHOOK m_hook = nullptr;
    bool m_registered = false;
    bool m_warned = false; // about Ctrl+G being taken, once
    HWND m_pending = nullptr; // a dialog that may still be building its controls
    int m_retries = 0;
    QTimer m_recheck;
};

} // namespace ws

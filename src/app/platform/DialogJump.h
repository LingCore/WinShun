#pragma once

#include "ComWorker.h"

#include <QTimer>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ws {

// Follows the Open and Save dialogs of other programs (see filedialog): which
// one is in front, and where it is. For Listary's Ctrl+G, which takes the
// dialog to the folder shown in the file manager window used last (File
// Explorer, Total Commander, Directory Opus: filemanager), for the search
// bar under the dialog (DialogBar), and for going there by itself.
//
// Ctrl+G is a hotkey only while such a dialog (or that bar) is in front: a
// WinEvent hook follows the foreground window, so everywhere else the key
// keeps its meaning.
class DialogJump {
public:
    struct Callbacks {
        // Registers Ctrl+G (true) and says whether that worked, or drops it (false).
        std::function<bool(bool on)> setHotkey;
        // A file dialog came to the front (again), or none is in front now
        // (nullptr). A companion coming to the front changes nothing.
        std::function<void(HWND dialog)> dialogChanged;
        // The one in front: shown, hidden, moved, resized, minimised; or its
        // thread put windows in another Z-order (it came up over the bar).
        std::function<void()> dialogMoved;
        // Its thread put windows in another Z-order, which it does often:
        // when set, told this instead of dialogMoved, to skip what needs
        // nothing (the bar still right above it). Unset: dialogMoved.
        std::function<void()> dialogRestacked;
        // The one in front taken by its frame (true) and let go (false):
        // moved or sized by the user. Each move of a window beside it costs
        // the dialog's own drag ~2 ms (docs/pitfalls.md, #222).
        std::function<void(bool dragged)> dialogDragged;
        // The dialog went to the file manager's folder by itself (setAutoJump), from
        // `from` (empty: no folder on disk). Called on another thread.
        std::function<void(HWND dialog, std::wstring from)> autoJumped;
        // Whether the index service can navigate an elevated (higher-integrity)
        // dialog for us (ServiceDialogNav, GitHub #104).
        std::function<bool()> elevatedNavAvailable;
        // Ask the service's helper to take an elevated `dialog` to `folder`
        // (folder only: never a file name, never Open). May be called on the
        // worker thread; it returns at once (the request runs in the service).
        std::function<void(HWND dialog, std::wstring folder)> navigateElevated;
    };

    explicit DialogJump(Callbacks callbacks);
    ~DialogJump();

    DialogJump(const DialogJump&) = delete;
    DialogJump& operator=(const DialogJump&) = delete;

    void setHotkeyEnabled(bool on); // Ctrl+G; the dialogs are followed either way
    // On: a dialog goes by itself to the folder shown in the file manager
    // when it comes up (`atOpen`), and when the user comes back to it from
    // the file manager after going to another folder there (not after just
    // looking). Not at open: it opens where Windows left it, and goes the
    // first time the user comes back (after just looking too), then as above.
    void setAutoJump(bool on, bool atOpen = true)
    {
        m_autoJump = on;
        m_atOpen = atOpen;
    }
    // Windows of ours that go with the dialog (the bar; the clipboard, opened
    // from the bar or over the dialog): while one is in front, the dialog
    // still counts as in front.
    void setCompanions(std::vector<HWND> windows) { m_companions = std::move(windows); }
    bool isCompanion(HWND window) const { return std::find(m_companions.begin(), m_companions.end(), window) != m_companions.end(); }
    HWND dialog() const { return m_dialog; }

    void jump(); // Ctrl+G was pressed: to the folder shown in the file manager

    // Takes `dialog` to a folder, or to a file's folder with the file's name
    // in the file name box; `open`: and presses Open. In the background,
    // after the moves asked for before.
    void go(HWND dialog, std::wstring path, bool isFile, bool open = false);

    // Re-evaluate Ctrl+G for the dialog in front: the service's ability to
    // navigate elevated dialogs changed (ServiceDialogNav availability).
    void refreshElevated();

private:
    static void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD);
    static void CALLBACK onLocation(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD);
    static void CALLBACK onPending(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD);
    void follow(HWND foreground);
    // A dialog not taken yet: in front but still making its controls, or
    // named by a foreground event while another window is in front. Looked
    // at again (recheck) when a control of it is made, shown or focused, when
    // its thread restacks its windows (it came up), and after ever longer
    // pauses; taken once it is in front as a file dialog. Given up when it
    // goes, when another is awaited (but a file dialog left behind is kept
    // over one that is neither in front nor a file dialog yet), or when a few
    // seconds after it was named or came up it is still no file dialog. A
    // file dialog left behind is looked at once a second till it comes up.
    void await(HWND candidate, bool fromManager);
    void recheck();
    void stopAwaiting();
    void setDialog(HWND dialog, bool fromManager);
    void setRegistered(bool on);
    // A dialog we can take over: a normal one, or an elevated one the service
    // can drive for us. `elevatedNav`: elevated AND the service can.
    bool canFollow(HWND dialog) const;
    bool elevatedNav(HWND dialog) const;
    void noteManager(HWND dialog); // on the worker
    void autoJump(HWND dialog, bool first, bool atOpen); // on the worker

    Callbacks m_callbacks;
    HWINEVENTHOOK m_foregroundHook = nullptr;
    HWINEVENTHOOK m_locationHook = nullptr; // the dialog's thread
    HWINEVENTHOOK m_dragHook = nullptr; // the dialog's thread: taken by its frame, let go
    HWND m_dialog = nullptr; // the file dialog in front
    std::vector<HWND> m_companions;
    bool m_hotkeyEnabled = false;
    bool m_registered = false;
    bool m_warned = false; // about Ctrl+G being taken, once
    bool m_autoJump = false;
    bool m_atOpen = true;
    HWND m_lastDialog = nullptr; // the file dialog in front last, maybe still open behind others
    // Dialogs a file manager came to the front after (and none of them since).
    std::vector<HWND> m_wentToManager;
    HWND m_pending = nullptr; // the dialog awaited
    bool m_pendingFromManager = false;
    HWINEVENTHOOK m_pendingHook = nullptr; // the awaited dialog's thread: windows made, shown, focused
    std::chrono::steady_clock::time_point m_pendingUntil;
    std::chrono::milliseconds m_pendingStep {};
    bool m_pendingFront = false; // in front at the last look
    QTimer m_recheck; // single shot: the next look at the awaited dialog
    HWND m_probing = nullptr; // WPS Office's window being probed, while in front
    bool m_probingFromManager = false;
    std::vector<HWND> m_seen; // the dialogs that came up, for setAutoJump
    // On the worker: what the file managers showed when the user left each dialog for one.
    std::vector<std::pair<HWND, std::wstring>> m_leftFor;
    std::vector<HWND> m_jumped; // on the worker: the dialogs gone to the file manager's folder by themselves
    ComWorker m_worker; // last: done with its task before the members above go
};

} // namespace ws

#pragma once

// Win顺 borrows the clipboard for a moment (selected_text with `copy`, 0.42.0:
// Ctrl+C pressed in the program in front, the words read, what was there put
// back): what changes meanwhile, and the putting back, are not copies of the
// user's, and ClipboardWatcher records none of them. begin(), then end() with
// the clipboard's sequence number once it is put back (or left alone). Any
// thread; for the whole process (there is one clipboard).

#include <windows.h>

#include <atomic>

namespace ws::clipboard_borrow {

inline std::atomic<int> g_borrowing {0};
inline std::atomic<DWORD> g_borrowedThrough {0}; // the sequence number the last borrow ended at

inline void begin()
{
    ++g_borrowing;
}

inline void end(DWORD sequence)
{
    g_borrowedThrough = sequence;
    --g_borrowing;
}

// Whether the clipboard at `sequence` is Win顺's borrow.
inline bool borrowed(DWORD sequence)
{
    const DWORD through = g_borrowedThrough.load();
    return g_borrowing.load() > 0 || (through != 0 && sequence == through);
}

} // namespace ws::clipboard_borrow

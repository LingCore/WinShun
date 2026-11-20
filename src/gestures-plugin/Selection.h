// The words selected in the window in front, asked of Win顺 (its
// selected_text tool) on a thread of its own, so neither the hook's thread
// nor the window's waits. Win顺 reads them with UI Automation; for a
// gesture never through the clipboard: nothing of the user's is replaced,
// and Win顺's clipboard history sees nothing. For a shortcut (`copy`) Win顺
// may also press Ctrl+C where UI Automation cannot tell (browsers, Firefox)
// and put the clipboard back as it was. This plugin cannot read them
// itself: its job keeps it from the clipboard, and a job with such a limit
// keeps it from other programs' windows too (docs/pitfalls.md).
#pragma once

#include <windows.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace gestures {

// What Win顺 said: the words (empty for none), and whether it could tell.
struct Selected {
    std::wstring text;
    bool readable = true; // false: a window it cannot read (an administrator's)
};

class SelectionReader {
public:
    // How the words are read: `root` is the top-level window meant, `copy`
    // whether Win顺 may borrow the clipboard; called on the reader's thread,
    // it may take a while (it waits for Win顺).
    using Read = std::function<Selected(HWND root, bool copy)>;

    explicit SelectionReader(Read read);
    ~SelectionReader();
    SelectionReader(const SelectionReader&) = delete;
    SelectionReader& operator=(const SelectionReader&) = delete;

    // The words selected in `root` when it is the window in front; none
    // when there are none or it cannot be told. `answer` comes on the
    // reader's thread. An ask not yet started when another comes is dropped
    // (the newer one wins).
    void ask(HWND root, std::function<void(Selected)> answer, bool copy = false);

private:
    void run();

    Read m_read;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_quit = false;
    HWND m_root = nullptr;
    bool m_copy = false;
    std::function<void(Selected)> m_answer;
    std::thread m_thread; // last: it starts once the rest is there
};

} // namespace gestures

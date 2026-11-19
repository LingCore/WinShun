// The words selected in the window in front, asked of Win顺 (its
// selected_text tool) on a thread of its own, so neither the hook's thread
// nor the window's waits. Win顺 reads them with UI Automation, never through
// the clipboard: nothing of the user's is replaced, and Win顺's clipboard
// history sees nothing. This plugin cannot read them itself: its job keeps
// it from the clipboard, and a job with such a limit keeps it from other
// programs' windows too (docs/pitfalls.md).
#pragma once

#include <windows.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace gestures {

class SelectionReader {
public:
    // How the words are read: `root` is the top-level window meant; called on
    // the reader's thread, it may take a while (it waits for Win顺). Empty: none.
    using Read = std::function<std::wstring(HWND root)>;

    explicit SelectionReader(Read read);
    ~SelectionReader();
    SelectionReader(const SelectionReader&) = delete;
    SelectionReader& operator=(const SelectionReader&) = delete;

    // The words selected in `root` when it is the window in front; "" for
    // none or when it cannot be told. `answer` comes on the reader's thread.
    // An ask not yet started when another comes is dropped (the newer one
    // wins).
    void ask(HWND root, std::function<void(std::wstring)> answer);

private:
    void run();

    Read m_read;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_quit = false;
    HWND m_root = nullptr;
    std::function<void(std::wstring)> m_answer;
    std::thread m_thread; // last: it starts once the rest is there
};

} // namespace gestures

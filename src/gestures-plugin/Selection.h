// The words selected in the window in front, read with UI Automation on a
// thread of its own (COM lives there, never on the hook's thread or the
// window's), never through the clipboard: nothing of the user's is
// replaced, and Win顺's clipboard history sees nothing.
#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace gestures {

class SelectionReader {
public:
    // At most this many characters are read.
    static constexpr int kMaxLength = 4000;

    SelectionReader();
    ~SelectionReader();
    SelectionReader(const SelectionReader&) = delete;
    SelectionReader& operator=(const SelectionReader&) = delete;

    // The words selected in `root` (a top-level window) when it is the one
    // in front and its focus is in it; "" for none or when it cannot be
    // told. `answer` comes on the reader's thread. An ask not yet started
    // when another comes is dropped (the newer one wins).
    void ask(HWND root, std::function<void(std::wstring)> answer);

private:
    void run();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_quit = false;
    HWND m_root = nullptr;
    std::function<void(std::wstring)> m_answer;
    std::thread m_thread; // last: it starts once the rest is there
};

} // namespace gestures

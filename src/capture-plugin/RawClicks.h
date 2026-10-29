#pragma once

#include <memory>

#include <windows.h>

namespace ws::capture::recording {
class ClickStream;
}

namespace ws::captureplugin {

// 显示鼠标点击 while recording: every button press, wherever it lands, into
// the recorder's stream. Raw input (as Win顺's own KeyListener listens) on a
// message window of its own, for as long as it lives; one at a time in a
// process (raw input has one window per device kind).
class RawClicks {
public:
    explicit RawClicks(std::shared_ptr<capture::recording::ClickStream> stream);
    ~RawClicks();
    RawClicks(const RawClicks&) = delete;
    RawClicks& operator=(const RawClicks&) = delete;

    bool active() const { return m_active; } // false: Windows said no (logged)

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l);

    std::shared_ptr<capture::recording::ClickStream> m_stream;
    HWND m_window = nullptr;
    bool m_active = false;
};

} // namespace ws::captureplugin

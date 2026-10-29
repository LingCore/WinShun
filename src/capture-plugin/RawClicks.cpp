#include "RawClicks.h"

#include "RecordingPointer.h"

#include <QDebug>
#include <QPoint>

namespace ws::captureplugin {

namespace {

constexpr USHORT kGenericDesktop = 0x01;
constexpr USHORT kMouse = 0x02;
constexpr wchar_t kClassName[] = L"WinShunCaptureClicks";

} // namespace

RawClicks::RawClicks(std::shared_ptr<capture::recording::ClickStream> stream)
    : m_stream(std::move(stream))
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    ::RegisterClassW(&wc); // once a process; again it fails, and the class is there
    m_window = ::CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!m_window) {
        qWarning() << "Screen capture: no window for the clicks, error" << ::GetLastError();
        return;
    }
    ::SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    const RAWINPUTDEVICE mouse {kGenericDesktop, kMouse, RIDEV_INPUTSINK, m_window};
    m_active = ::RegisterRawInputDevices(&mouse, 1, sizeof mouse);
    if (!m_active)
        qWarning() << "Screen capture: no raw mouse input for the clicks, error" << ::GetLastError();
}

RawClicks::~RawClicks()
{
    if (m_active) {
        const RAWINPUTDEVICE remove {kGenericDesktop, kMouse, RIDEV_REMOVE, nullptr};
        ::RegisterRawInputDevices(&remove, 1, sizeof remove);
    }
    if (m_window)
        ::DestroyWindow(m_window);
}

LRESULT CALLBACK RawClicks::windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<RawClicks*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && message == WM_INPUT) {
        RAWINPUT raw {};
        UINT size = sizeof raw;
        if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)
            && raw.header.dwType == RIM_TYPEMOUSE) {
            const USHORT flags = raw.data.mouse.usButtonFlags;
            POINT point {};
            if ((flags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN)) && ::GetPhysicalCursorPos(&point)) {
                // The device's buttons: with them swapped (left-handed), its right one clicks.
                const bool swapped = ::GetSystemMetrics(SM_SWAPBUTTON) != 0;
                const auto time = static_cast<std::uint32_t>(::GetMessageTime());
                if (flags & RI_MOUSE_LEFT_BUTTON_DOWN)
                    self->m_stream->pushInput(QPoint(point.x, point.y), swapped, time);
                if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN)
                    self->m_stream->pushInput(QPoint(point.x, point.y), !swapped, time);
            }
        }
    }
    return ::DefWindowProcW(hwnd, message, w, l);
}

} // namespace ws::captureplugin

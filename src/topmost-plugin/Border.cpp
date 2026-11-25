#include "Border.h"

#include "Draw.h"
#include "Pins.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <future>
#include <thread>
#include <vector>

namespace topmost {

namespace {

// Logical pixels.
constexpr float kThickness = 2;
constexpr float kRadius = 8; // Windows 11's window corner
constexpr float kSmallRadius = 4;

constexpr wchar_t kControlClass[] = L"WinShunTopmostBorders";
constexpr wchar_t kStripClass[] = L"WinShunTopmostBorder";
constexpr UINT kAdd = WM_APP + 1; // lParam: the window
constexpr UINT kRemove = WM_APP + 2;
constexpr UINT kClear = WM_APP + 3;
constexpr UINT kColour = WM_APP + 4; // wParam: 0xAARRGGBB
constexpr UINT_PTR kTimer = 1;
// What the events below do not tell (another window put above this one, the
// monitor's scale changed, an event lost) is caught up with here.
constexpr UINT kCheckMs = 250;

// The events of a window's own thread that move the line: in front, moved or
// sized by hand, minimized and back; shown, hidden, reordered, moved, closed;
// to another virtual desktop and back.
constexpr std::array<std::pair<DWORD, DWORD>, 3> kEvents {{
    {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND},
    {EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE},
    {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
}};

int width(const RECT& r) noexcept
{
    return r.right - r.left;
}

int height(const RECT& r) noexcept
{
    return r.bottom - r.top;
}

} // namespace

std::optional<Outline> outlineFor(const Shape& shape) noexcept
{
    if (!shape.shown || ::IsRectEmpty(&shape.frame))
        return std::nullopt;
    const RECT& f = shape.frame;
    const RECT& m = shape.monitor;
    // Full screen. A maximized window fills its monitor too where the taskbar
    // hides itself: that one keeps its line.
    if (f.left <= m.left && f.top <= m.top && f.right >= m.right && f.bottom >= m.bottom
        && !(shape.maximized && ::EqualRect(&shape.work, &m)))
        return std::nullopt;
    Outline outline;
    outline.frame = f;
    // Maximized, a window reaches past the work area by its frame (and by a
    // pixel under the taskbar at some scales).
    if (shape.maximized && !::IntersectRect(&outline.frame, &f, &shape.work))
        return std::nullopt;
    const float scale = shape.scale > 0 ? shape.scale : 1;
    outline.thickness = std::max(1, static_cast<int>(std::lround(kThickness * scale)));
    float radius = 0;
    if (shape.roundsCorners && !shape.maximized) {
        switch (shape.corners) {
        case Corners::Default:
            radius = shape.framed ? kRadius : 0;
            break;
        case Corners::Round:
            radius = kRadius;
            break;
        case Corners::Small:
            radius = kSmallRadius;
            break;
        case Corners::Square:
            break;
        }
    }
    outline.radius = static_cast<int>(std::lround(radius * scale));
    return outline;
}

std::array<RECT, 4> stripsOf(const Outline& outline) noexcept
{
    const RECT& f = outline.frame;
    // The top and bottom pieces are as tall as the corner is, and never meet.
    const int band = std::min(std::max(outline.thickness, outline.radius), height(f) / 2);
    const int side = std::min(outline.thickness, width(f) / 2);
    std::array<RECT, 4> strips {};
    strips[Top] = {f.left, f.top, f.right, f.top + band};
    strips[Bottom] = {f.left, f.bottom - band, f.right, f.bottom};
    strips[Left] = {f.left, f.top + band, f.left + side, f.bottom - band};
    strips[Right] = {f.right - side, f.top + band, f.right, f.bottom - band};
    return strips;
}

Shape shapeOf(HWND window)
{
    Shape shape;
    if (!window || !::IsWindow(window))
        return shape;
    DWORD cloaked = 0;
    const bool away = SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked != 0;
    shape.shown = ::IsWindowVisible(window) && !::IsIconic(window) && !away;
    if (FAILED(::DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &shape.frame, sizeof shape.frame)))
        ::GetWindowRect(window, &shape.frame);
    const HMONITOR monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info {sizeof info};
    if (::GetMonitorInfoW(monitor, &info)) {
        shape.monitor = info.rcMonitor;
        shape.work = info.rcWork;
    }
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0)
        shape.scale = static_cast<float>(dpiX) / 96.f;
    shape.maximized = ::IsZoomed(window) != FALSE;
    const LONG_PTR style = ::GetWindowLongPtrW(window, GWL_STYLE);
    shape.framed = (style & WS_CAPTION) == WS_CAPTION || (style & WS_THICKFRAME) != 0;
    // Windows 10 does not know the attribute, and rounds nothing.
    int preference = DWMWCP_DEFAULT;
    if (SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof preference))) {
        shape.roundsCorners = true;
        shape.corners = preference == DWMWCP_DONOTROUND ? Corners::Square
            : preference == DWMWCP_ROUND                ? Corners::Round
            : preference == DWMWCP_ROUNDSMALL           ? Corners::Small
                                                        : Corners::Default;
    }
    return shape;
}

class Borders::Impl {
public:
    Impl()
    {
        std::promise<void> ready;
        m_thread = std::thread([this, &ready] { run(ready); });
        ready.get_future().wait();
    }
    ~Impl()
    {
        post(WM_CLOSE);
        if (m_thread.joinable())
            m_thread.join();
    }

    void post(UINT message, WPARAM wp = 0, LPARAM lp = 0) const
    {
        if (m_control)
            ::PostMessageW(m_control, message, wp, lp);
    }

private:
    struct Border {
        HWND target = nullptr;
        std::array<HWND, 4> strips {};
        std::array<HWINEVENTHOOK, kEvents.size()> hooks {};
        Outline drawn; // as the pieces were last drawn
        std::uint32_t colour = 0; // and in which colour; 0: not drawn yet
        bool visible = false;
    };

    void run(std::promise<void>& ready)
    {
        // Whatever goes wrong here, the plugin goes on without its lines.
        try {
            const HINSTANCE instance = ::GetModuleHandleW(nullptr);
            WNDCLASSEXW control {sizeof control};
            control.lpfnWndProc = &Impl::proc;
            control.hInstance = instance;
            control.lpszClassName = kControlClass;
            ::RegisterClassExW(&control);
            WNDCLASSEXW strip {sizeof strip};
            strip.lpfnWndProc = ::DefWindowProcW;
            strip.hInstance = instance;
            strip.lpszClassName = kStripClass;
            ::RegisterClassExW(&strip);
            m_control = ::CreateWindowExW(0, kControlClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
            if (m_control && FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2d))) {
                ::DestroyWindow(m_control);
                m_control = nullptr;
            }
        } catch (...) {
            m_control = nullptr;
        }
        ready.set_value();
        if (!m_control)
            return;
        t_self = this;
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            try {
                ::DispatchMessageW(&msg);
            } catch (...) {
                // A line not drawn; the next check may draw it.
            }
        }
        t_self = nullptr;
        while (!m_borders.empty())
            drop(m_borders.size() - 1);
        release(m_target);
        release(m_d2d);
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
    {
        if (message == WM_NCCREATE)
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) {
            switch (message) {
            case kAdd:
                self->add(reinterpret_cast<HWND>(lp));
                return 0;
            case kRemove:
                self->changed(reinterpret_cast<HWND>(lp), true);
                return 0;
            case kClear:
                while (!self->m_borders.empty())
                    self->drop(self->m_borders.size() - 1);
                return 0;
            case kColour:
                self->m_colour = static_cast<std::uint32_t>(wp);
                self->check();
                return 0;
            case WM_TIMER:
                if (wp == kTimer)
                    self->check();
                return 0;
            case WM_DESTROY:
                ::KillTimer(hwnd, kTimer);
                ::PostQuitMessage(0);
                return 0;
            default:
                break;
            }
        }
        return ::DefWindowProcW(hwnd, message, wp, lp);
    }

    static void CALLBACK event(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG child, DWORD, DWORD)
    {
        if (t_self && hwnd && object == OBJID_WINDOW && child == CHILDID_SELF)
            t_self->changed(hwnd, false);
    }

    void add(HWND target)
    {
        if (!target || !::IsWindow(target))
            return;
        if (std::ranges::any_of(m_borders, [target](const Border& b) { return b.target == target; })) {
            changed(target, false);
            return;
        }
        Border border;
        border.target = target;
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        for (HWND& strip : border.strips)
            strip = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kStripClass,
                L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        DWORD process = 0;
        const DWORD thread = ::GetWindowThreadProcessId(target, &process);
        for (std::size_t i = 0; i < kEvents.size(); ++i)
            border.hooks[i] = ::SetWinEventHook(kEvents[i].first, kEvents[i].second, nullptr, &Impl::event, process, thread,
                WINEVENT_OUTOFCONTEXT);
        m_borders.push_back(border);
        if (m_borders.size() == 1)
            ::SetTimer(m_control, kTimer, kCheckMs, nullptr);
        changed(target, false);
    }

    void drop(std::size_t index)
    {
        const Border border = m_borders[index];
        m_borders.erase(m_borders.begin() + static_cast<std::ptrdiff_t>(index));
        for (const HWINEVENTHOOK hook : border.hooks) {
            if (hook)
                ::UnhookWinEvent(hook);
        }
        for (const HWND strip : border.strips) {
            if (strip)
                ::DestroyWindow(strip);
        }
        if (m_borders.empty())
            ::KillTimer(m_control, kTimer);
    }

    // The window's line as the window is now; gone with `gone`, or once the
    // window is no longer on top.
    void changed(HWND target, bool gone)
    {
        const auto found = std::ranges::find_if(m_borders, [target](const Border& b) { return b.target == target; });
        if (found == m_borders.end())
            return;
        if (gone || !follow(*found))
            drop(static_cast<std::size_t>(found - m_borders.begin()));
    }

    void check()
    {
        for (std::size_t i = m_borders.size(); i-- > 0;) {
            if (!follow(m_borders[i]))
                drop(i);
        }
    }

    // False once the window needs no line any more.
    bool follow(Border& border)
    {
        if (!::IsWindow(border.target) || !pinned(border.target))
            return false;
        const std::optional<Outline> outline = outlineFor(shapeOf(border.target));
        if (!outline) {
            if (border.visible) {
                for (const HWND strip : border.strips)
                    ::ShowWindow(strip, SW_HIDE);
                border.visible = false;
            }
            return true;
        }
        const std::array<RECT, 4> rects = stripsOf(*outline);
        const bool redraw = border.colour != m_colour || width(border.drawn.frame) != width(outline->frame)
            || height(border.drawn.frame) != height(outline->frame) || border.drawn.thickness != outline->thickness
            || border.drawn.radius != outline->radius;
        if (redraw) {
            for (std::size_t i = 0; i < rects.size(); ++i)
                draw(border.strips[i], rects[i], *outline);
        }
        if (redraw || !border.visible || !(border.drawn == *outline) || !inPlace(border, rects))
            place(border, rects);
        border.drawn = *outline;
        border.colour = m_colour;
        border.visible = true;
        return true;
    }

    static bool ours(const Border& border, HWND hwnd)
    {
        return std::ranges::find(border.strips, hwnd) != border.strips.end();
    }

    // Whether the pieces are right above their window, with nothing between.
    static bool inPlace(const Border& border, const std::array<RECT, 4>& rects)
    {
        const auto wanted = std::ranges::count_if(rects, [](const RECT& r) { return !::IsRectEmpty(&r); });
        std::ptrdiff_t found = 0;
        for (HWND above = ::GetWindow(border.target, GW_HWNDPREV); above && ours(border, above); above = ::GetWindow(above, GW_HWNDPREV)) {
            if (::IsWindowVisible(above))
                ++found;
        }
        return found == wanted;
    }

    // All four at once, right above the window: under whatever else is above
    // it, another window on top that covers it included.
    static void place(const Border& border, const std::array<RECT, 4>& rects)
    {
        HWND above = ::GetWindow(border.target, GW_HWNDPREV);
        while (above && ours(border, above))
            above = ::GetWindow(above, GW_HWNDPREV);
        const HWND after = above ? above : HWND_TOPMOST;
        HDWP all = ::BeginDeferWindowPos(static_cast<int>(rects.size()));
        for (std::size_t i = 0; all && i < rects.size(); ++i) {
            const RECT& r = rects[i];
            if (::IsRectEmpty(&r))
                all = ::DeferWindowPos(all, border.strips[i], nullptr, 0, 0, 0, 0,
                    SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            else
                all = ::DeferWindowPos(all, border.strips[i], after, r.left, r.top, width(r), height(r), SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
        if (all)
            ::EndDeferWindowPos(all);
    }

    // One piece: the part of the whole line that falls in `rect`.
    void draw(HWND strip, const RECT& rect, const Outline& outline)
    {
        if (!strip || ::IsRectEmpty(&rect))
            return;
        const int wide = width(rect);
        const int tall = height(rect);
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = wide;
        info.bmiHeader.biHeight = -tall;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        const HDC dc = ::CreateCompatibleDC(nullptr);
        const HBITMAP bitmap = dc ? ::CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (dc && bitmap) {
            const HGDIOBJ old = ::SelectObject(dc, bitmap);
            if (!m_target) {
                // In software: thin pieces now and then are not worth a
                // graphics driver in this process.
                const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
                if (FAILED(m_d2d->CreateDCRenderTarget(&props, &m_target)))
                    m_target = nullptr;
            }
            const RECT all {0, 0, wide, tall};
            ID2D1SolidColorBrush* brush = nullptr;
            if (m_target && SUCCEEDED(m_target->BindDC(dc, &all)) && SUCCEEDED(m_target->CreateSolidColorBrush(colour(m_colour), &brush))) {
                const float thickness = static_cast<float>(outline.thickness);
                const float half = thickness / 2;
                // The middle of the stroke: whole pixels inside the edge.
                const D2D1_RECT_F line = D2D1::RectF(half, half, static_cast<float>(width(outline.frame)) - half,
                    static_cast<float>(height(outline.frame)) - half);
                m_target->BeginDraw();
                m_target->Clear(D2D1::ColorF(0, 0, 0, 0));
                m_target->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(outline.frame.left - rect.left),
                    static_cast<float>(outline.frame.top - rect.top)));
                if (outline.radius > 0) {
                    const float radius = std::max(static_cast<float>(outline.radius) - half, half);
                    m_target->DrawRoundedRectangle(D2D1::RoundedRect(line, radius, radius), brush, thickness);
                } else {
                    m_target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                    m_target->DrawRectangle(line, brush, thickness);
                    m_target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                }
                m_target->SetTransform(D2D1::Matrix3x2F::Identity());
                if (SUCCEEDED(m_target->EndDraw())) {
                    POINT position {rect.left, rect.top};
                    SIZE size {wide, tall};
                    POINT source {0, 0};
                    BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                    ::UpdateLayeredWindow(strip, nullptr, &position, &size, dc, &source, 0, &blend, ULW_ALPHA);
                } else {
                    release(m_target); // made anew next time
                }
            }
            release(brush);
            ::SelectObject(dc, old);
        }
        if (bitmap)
            ::DeleteObject(bitmap);
        if (dc)
            ::DeleteDC(dc);
    }

    static inline thread_local Impl* t_self = nullptr; // for the event callback

    std::thread m_thread;
    HWND m_control = nullptr;
    // The window thread's.
    ID2D1Factory* m_d2d = nullptr;
    ID2D1DCRenderTarget* m_target = nullptr;
    std::vector<Border> m_borders;
    std::uint32_t m_colour = 0xFF0F6CBD;
};

Borders::Borders() : m_impl(std::make_unique<Impl>()) {}
Borders::~Borders() = default;

void Borders::setColour(std::uint32_t argb)
{
    m_impl->post(kColour, argb);
}

void Borders::add(HWND window)
{
    m_impl->post(kAdd, 0, reinterpret_cast<LPARAM>(window));
}

void Borders::remove(HWND window)
{
    m_impl->post(kRemove, 0, reinterpret_cast<LPARAM>(window));
}

void Borders::clear()
{
    m_impl->post(kClear);
}

} // namespace topmost

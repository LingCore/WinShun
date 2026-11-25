#include "Note.h"

#include "Draw.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <future>
#include <mutex>
#include <thread>

namespace topmost {

std::uint32_t argbOf(const std::string& name)
{
    if ((name.size() != 7 && name.size() != 9) || name[0] != '#')
        return 0;
    std::uint32_t value = 0;
    for (std::size_t i = 1; i < name.size(); ++i) {
        const char c = name[i];
        const int digit = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f'             ? c - 'a' + 10
            : c >= 'A' && c <= 'F'             ? c - 'A' + 10
                                               : -1;
        if (digit < 0)
            return 0;
        value = value << 4 | static_cast<std::uint32_t>(digit);
    }
    return name.size() == 7 ? 0xFF000000u | value : value;
}

namespace {

constexpr wchar_t kClass[] = L"WinShunTopmostNote";
constexpr UINT kShow = WM_APP + 1;
constexpr UINT_PTR kTimer = 1;
constexpr UINT kFrameMs = 16;
constexpr ULONGLONG kInMs = 90;
constexpr ULONGLONG kOutMs = 180;
constexpr ULONGLONG kStayMs = 900;
constexpr ULONGLONG kProblemStayMs = 2400; // a sentence to read
// Logical pixels.
constexpr float kHeight = 32;
constexpr float kPadding = 14;
constexpr float kFont = 13;
constexpr float kFromTop = 12;
constexpr float kWidest = 520;

struct Request {
    HWND window = nullptr;
    Tone tone = Tone::On;
    std::wstring text;
};

} // namespace

class Note::Impl {
public:
    Impl()
    {
        std::promise<void> ready;
        m_thread = std::thread([this, &ready] { run(ready); });
        ready.get_future().wait();
    }
    ~Impl()
    {
        if (m_window)
            ::PostMessageW(m_window, WM_CLOSE, 0, 0);
        if (m_thread.joinable())
            m_thread.join();
    }

    void setLook(const Look& look)
    {
        const std::lock_guard lock(m_mutex);
        m_look = look;
    }

    void show(HWND window, Tone tone, std::wstring text)
    {
        if (!m_window)
            return;
        auto request = std::make_unique<Request>(Request {window, tone, std::move(text)});
        if (::PostMessageW(m_window, kShow, 0, reinterpret_cast<LPARAM>(request.get())))
            request.release(); // the window thread's now
    }

private:
    void run(std::promise<void>& ready)
    {
        // Whatever goes wrong here, the plugin goes on without its note.
        try {
            WNDCLASSEXW wc {sizeof wc};
            wc.lpfnWndProc = &Impl::proc;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = kClass;
            ::RegisterClassExW(&wc);
            m_window = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                kClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
            if (m_window && (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2d))
                    || FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&m_write))))) {
                ::DestroyWindow(m_window);
                m_window = nullptr;
            }
        } catch (...) {
            m_window = nullptr;
        }
        ready.set_value();
        if (!m_window)
            return;
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            try {
                ::DispatchMessageW(&msg);
            } catch (...) {
                // A note not drawn; the next one may be.
            }
        }
        freeSurface();
        release(m_write);
        release(m_d2d);
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
    {
        if (message == WM_NCCREATE)
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) {
            switch (message) {
            case kShow: {
                const std::unique_ptr<Request> request(reinterpret_cast<Request*>(lp));
                self->begin(hwnd, *request);
                return 0;
            }
            case WM_TIMER:
                if (wp == kTimer)
                    self->step(hwnd);
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

    void freeSurface()
    {
        if (m_dc) {
            ::SelectObject(m_dc, m_oldBitmap);
            ::DeleteDC(m_dc);
            m_dc = nullptr;
        }
        if (m_bitmap) {
            ::DeleteObject(m_bitmap);
            m_bitmap = nullptr;
        }
    }

    // Where the pill goes, and at which scale: the top of `window`, on the
    // monitor it is on (physical pixels).
    struct Place {
        RECT frame {};
        RECT work {};
        float scale = 1;
    };
    static Place placeFor(HWND window)
    {
        Place place;
        HMONITOR monitor = nullptr;
        if (window && ::IsWindow(window)) {
            if (FAILED(::DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &place.frame, sizeof place.frame)))
                ::GetWindowRect(window, &place.frame);
            monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        } else {
            POINT pointer {};
            ::GetCursorPos(&pointer);
            monitor = ::MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST);
        }
        MONITORINFO info {sizeof info};
        ::GetMonitorInfoW(monitor, &info);
        place.work = info.rcWork;
        if (!window || ::IsRectEmpty(&place.frame))
            place.frame = info.rcWork;
        UINT dpiX = 96;
        UINT dpiY = 96;
        if (SUCCEEDED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0)
            place.scale = static_cast<float>(dpiX) / 96.f;
        return place;
    }

    void begin(HWND hwnd, const Request& request)
    {
        Look look;
        {
            const std::lock_guard lock(m_mutex);
            look = m_look;
        }
        const Place place = placeFor(request.window);
        const float s = place.scale;
        const auto px = [s](float logical) { return std::round(logical * s); };

        NONCLIENTMETRICSW metrics {sizeof metrics};
        const wchar_t* family = ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0) ? metrics.lfMessageFont.lfFaceName
                                                                                                             : L"Segoe UI";
        IDWriteTextFormat* format = nullptr;
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(m_write->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, px(kFont), L"", &format)))
            return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        const float height = px(kHeight);
        const float most = std::min(px(kWidest), static_cast<float>(place.work.right - place.work.left) - px(24)) - 2 * px(kPadding);
        if (FAILED(m_write->CreateTextLayout(request.text.c_str(), static_cast<UINT32>(request.text.size()), format, std::max(most, 1.f),
                height, &layout))) {
            release(format);
            return;
        }
        const DWRITE_TRIMMING trimming {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        IDWriteInlineObject* ellipsis = nullptr;
        if (SUCCEEDED(m_write->CreateEllipsisTrimmingSign(format, &ellipsis)))
            layout->SetTrimming(&trimming, ellipsis);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TEXT_METRICS measured {};
        layout->GetMetrics(&measured);
        const float textWidth = std::ceil(std::min(measured.width, std::max(most, 1.f)));
        const int width = static_cast<int>(textWidth + 2 * px(kPadding));
        const int tall = static_cast<int>(height);

        // The colours: white words on the accent (or the danger colour);
        // coming off, the quiet window colours with a hairline round them.
        std::uint32_t fill = 0;
        std::uint32_t ink = 0xFFFFFFFF;
        std::uint32_t edge = 0;
        switch (request.tone) {
        case Tone::On:
            fill = look.accentFill ? look.accentFill : 0xFF0F6CBD;
            break;
        case Tone::Problem:
            fill = look.dangerFill ? look.dangerFill : 0xFFC42B1C;
            break;
        case Tone::Off:
            fill = look.background ? (look.background | 0xFF000000u) : look.dark ? 0xFF2B2B2B : 0xFFF9F9F9;
            ink = look.text ? look.text : look.dark ? 0xFFFFFFFF : 0xFF1B1B1B;
            edge = look.edge ? look.edge : look.dark ? 0x33FFFFFF : 0x24000000;
            break;
        }

        freeSurface();
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -tall;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_dc = ::CreateCompatibleDC(nullptr);
        m_bitmap = ::CreateDIBSection(m_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        ID2D1DCRenderTarget* target = nullptr;
        // In software: a few hundred pixels once in a while are not worth a
        // graphics driver in this process.
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        const RECT all {0, 0, width, tall};
        bool drawn = false;
        if (m_dc && m_bitmap) {
            m_oldBitmap = ::SelectObject(m_dc, m_bitmap);
            if (SUCCEEDED(m_d2d->CreateDCRenderTarget(&props, &target)) && SUCCEEDED(target->BindDC(m_dc, &all))) {
                target->BeginDraw();
                target->Clear(D2D1::ColorF(0, 0, 0, 0));
                target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                const float radius = look.rounded ? height / 2 : 0;
                ID2D1SolidColorBrush* brush = nullptr;
                if (SUCCEEDED(target->CreateSolidColorBrush(colour(fill), &brush))) {
                    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, static_cast<float>(width), height), radius, radius), brush);
                    if (edge) {
                        // One device pixel, whole inside the pill.
                        brush->SetColor(colour(edge));
                        target->DrawRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, static_cast<float>(width) - 0.5f, height - 0.5f),
                                std::max(radius - 0.5f, 0.f), std::max(radius - 0.5f, 0.f)),
                            brush, 1.f);
                    }
                    brush->SetColor(colour(ink));
                    target->DrawTextLayout(D2D1::Point2F(px(kPadding), 0), layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    release(brush);
                }
                drawn = SUCCEEDED(target->EndDraw());
            }
        }
        release(target);
        release(ellipsis);
        release(layout);
        release(format);
        if (!drawn)
            return;

        // Centred at the top of the window, inside the work area.
        int x = place.frame.left + ((place.frame.right - place.frame.left) - width) / 2;
        int y = place.frame.top + static_cast<int>(px(kFromTop));
        x = std::clamp(x, static_cast<int>(place.work.left), std::max(static_cast<int>(place.work.left), static_cast<int>(place.work.right) - width));
        y = std::clamp(y, static_cast<int>(place.work.top), std::max(static_cast<int>(place.work.top), static_cast<int>(place.work.bottom) - tall));
        m_position = {x, y};
        m_size = {width, tall};
        m_began = ::GetTickCount64();
        m_stay = request.tone == Tone::Problem ? kProblemStayMs : kStayMs;
        m_alpha = 0;
        paint(hwnd, 0);
        // Above the window it speaks of, which may be on top itself now.
        ::SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, tall, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::SetTimer(hwnd, kTimer, kFrameMs, nullptr);
    }

    void paint(HWND hwnd, BYTE alpha)
    {
        POINT source {0, 0};
        BLENDFUNCTION blend {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        ::UpdateLayeredWindow(hwnd, nullptr, &m_position, &m_size, m_dc, &source, 0, &blend, ULW_ALPHA);
    }

    void step(HWND hwnd)
    {
        const ULONGLONG passed = ::GetTickCount64() - m_began;
        if (passed >= kInMs + m_stay + kOutMs) {
            ::KillTimer(hwnd, kTimer);
            ::ShowWindow(hwnd, SW_HIDE);
            freeSurface();
            return;
        }
        double shown = 1;
        if (passed < kInMs)
            shown = static_cast<double>(passed) / static_cast<double>(kInMs);
        else if (passed > kInMs + m_stay)
            shown = 1 - static_cast<double>(passed - kInMs - m_stay) / static_cast<double>(kOutMs);
        const BYTE alpha = static_cast<BYTE>(std::lround(std::clamp(shown, 0.0, 1.0) * 255));
        if (alpha != m_alpha) {
            m_alpha = alpha;
            paint(hwnd, alpha);
        }
    }

    std::thread m_thread;
    HWND m_window = nullptr;
    std::mutex m_mutex;
    Look m_look; // under m_mutex
    // The window thread's.
    ID2D1Factory* m_d2d = nullptr;
    IDWriteFactory* m_write = nullptr;
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
    POINT m_position {};
    SIZE m_size {};
    ULONGLONG m_began = 0;
    ULONGLONG m_stay = kStayMs;
    BYTE m_alpha = 0;
};

Note::Note() : m_impl(std::make_unique<Impl>()) {}
Note::~Note() = default;

void Note::setLook(const Look& look)
{
    m_impl->setLook(look);
}

void Note::show(HWND window, Tone tone, std::wstring text)
{
    m_impl->show(window, tone, std::move(text));
}

} // namespace topmost

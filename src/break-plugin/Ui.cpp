#include "Ui.h"

#include "Keys.h"
#include "Sense.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cmath>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace brk {
namespace {

constexpr UINT kWake = WM_APP + 1;
constexpr UINT kEsc = WM_APP + 2; // wParam 1: Esc went down, 0: up
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT_PTR kAnimTimer = 2;
constexpr UINT kTickMs = 250;
constexpr UINT kAnimMs = 16;
constexpr std::int64_t kFadeInMs = 200;
constexpr std::int64_t kFadeOutMs = 150;
constexpr std::int64_t kEscHoldMs = 3000;
constexpr std::int64_t kDrainMs = 2000;
constexpr std::int64_t kQuietEveryMs = 1000;
constexpr BYTE kDimAlpha = 240;
constexpr COLORREF kDimColor = RGB(18, 20, 24);
constexpr UINT32 kDimRgb = 0x121418; // the same, for Direct2D

constexpr wchar_t kHiddenClass[] = L"WinShunBreakHidden";
constexpr wchar_t kPillClass[] = L"WinShunBreakNotice";
constexpr wchar_t kDimClass[] = L"WinShunBreakDim";
constexpr wchar_t kContentClass[] = L"WinShunBreakScreen";

enum Choice { kNone = 0, kPostpone, kSkip };

template <class T>
void release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

D2D1_COLOR_F rgba(UINT32 rgb, float a = 1.f)
{
    return D2D1::ColorF(rgb, a);
}

std::wstring duration(std::int64_t ms, bool zh)
{
    const std::int64_t s = (ms + 999) / 1000;
    if (s < 60)
        return std::to_wstring(s) + (zh ? L" 秒" : L" s");
    const std::int64_t m = s / 60;
    const std::int64_t rest = s % 60;
    if (rest == 0)
        return std::to_wstring(m) + (zh ? L" 分钟" : L" min");
    return zh ? std::to_wstring(m) + L" 分 " + std::to_wstring(rest) + L" 秒"
              : std::to_wstring(m) + L" min " + std::to_wstring(rest) + L" s";
}

std::wstring countdown(std::int64_t ms)
{
    const std::int64_t s = (ms + 999) / 1000;
    wchar_t buffer[32];
    swprintf_s(buffer, L"%lld:%02lld", s / 60, s % 60);
    return buffer;
}

// A 32-bit premultiplied bitmap for UpdateLayeredWindow.
struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    int width = 0;
    int height = 0;

    Surface() = default;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    ~Surface() { reset(); }

    void reset()
    {
        if (dc) {
            ::SelectObject(dc, old);
            ::DeleteDC(dc);
            dc = nullptr;
        }
        if (bitmap) {
            ::DeleteObject(bitmap);
            bitmap = nullptr;
        }
        width = height = 0;
    }
    bool ensure(int w, int h)
    {
        if (dc && w == width && h == height)
            return true;
        reset();
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        dc = ::CreateCompatibleDC(nullptr);
        bitmap = dc ? ::CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!bitmap) {
            reset();
            return false;
        }
        old = ::SelectObject(dc, bitmap);
        width = w;
        height = h;
        return true;
    }
};

// Direct2D into a Surface; text through DirectWrite, greyscale (ClearType
// cannot blend onto a transparent bitmap).
class Graphics {
public:
    Graphics() = default;
    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;
    ~Graphics()
    {
        for (auto& [key, format] : m_formats)
            format->Release();
        release(m_target);
        release(m_write);
        release(m_d2d);
    }

    bool init()
    {
        if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2d)))
            return false;
        if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&m_write))))
            return false;
        m_textFamily = has(L"Microsoft YaHei UI") ? L"Microsoft YaHei UI" : L"Segoe UI";
        m_digitFamily = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
        return makeTarget();
    }

    ID2D1RenderTarget* begin(Surface& surface)
    {
        if (!m_target && !makeTarget())
            return nullptr;
        const RECT r {0, 0, surface.width, surface.height};
        if (FAILED(m_target->BindDC(surface.dc, &r)))
            return nullptr;
        m_target->BeginDraw();
        m_target->SetTransform(D2D1::Matrix3x2F::Identity());
        m_target->Clear(D2D1::ColorF(0, 0.f));
        m_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        m_target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        return m_target;
    }
    void end()
    {
        if (m_target->EndDraw() == D2DERR_RECREATE_TARGET)
            release(m_target); // made again for the next drawing
    }

    IDWriteTextFormat* text(DWRITE_FONT_WEIGHT weight, float px) { return format(m_textFamily, weight, px); }
    IDWriteTextFormat* digits(DWRITE_FONT_WEIGHT weight, float px) { return format(m_digitFamily, weight, px); }

    // A laid-out line (or lines within maxWidth); the caller releases it.
    IDWriteTextLayout* layout(const std::wstring& s, IDWriteTextFormat* format, float maxWidth, bool centred = false)
    {
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(m_write->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), format, maxWidth, 10'000.f, &layout)))
            return nullptr;
        if (centred)
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        return layout;
    }

    void fill(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius, D2D1_COLOR_F color)
    {
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(color, &brush))) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
            brush->Release();
        }
    }
    void stroke(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius, float width, D2D1_COLOR_F color)
    {
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(color, &brush))) {
            const float h = width / 2;
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h), radius - h, radius - h), brush, width);
            brush->Release();
        }
    }
    void draw(ID2D1RenderTarget* rt, IDWriteTextLayout* layout, float x, float y, D2D1_COLOR_F color)
    {
        ID2D1SolidColorBrush* brush = nullptr;
        if (layout && SUCCEEDED(rt->CreateSolidColorBrush(color, &brush))) {
            rt->DrawTextLayout(D2D1::Point2F(x, y), layout, brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            brush->Release();
        }
    }
    // The ellipse in r: `color` out to `solid` of the way, then fading to
    // nothing at its edge.
    void glow(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float solid, D2D1_COLOR_F color)
    {
        D2D1_COLOR_F none = color;
        none.a = 0;
        const D2D1_GRADIENT_STOP stops[] {{0.f, color}, {solid, color}, {1.f, none}};
        ID2D1GradientStopCollection* collection = nullptr;
        ID2D1RadialGradientBrush* brush = nullptr;
        const D2D1_POINT_2F centre = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 3, &collection))
            && SUCCEEDED(rt->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(centre, D2D1::Point2F(), (r.right - r.left) / 2, (r.bottom - r.top) / 2), collection, &brush)))
            rt->FillRectangle(r, brush);
        release(brush);
        release(collection);
    }

private:
    bool has(const wchar_t* family)
    {
        IDWriteFontCollection* fonts = nullptr;
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(m_write->GetSystemFontCollection(&fonts))) {
            fonts->FindFamilyName(family, &index, &exists);
            fonts->Release();
        }
        return exists;
    }
    bool makeTarget()
    {
        // In software: on the GPU, the graphics driver came into the process
        // (50 MB, against 3 for one that mostly counts the minutes) to draw a
        // few small bitmaps a second, read back for UpdateLayeredWindow anyway.
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f); // physical pixels throughout
        return SUCCEEDED(m_d2d->CreateDCRenderTarget(&props, &m_target));
    }
    IDWriteTextFormat* format(const std::wstring& family, DWRITE_FONT_WEIGHT weight, float px)
    {
        const auto key = std::make_tuple(family, static_cast<int>(weight), static_cast<int>(std::lround(px * 4)));
        if (const auto it = m_formats.find(key); it != m_formats.end())
            return it->second;
        IDWriteTextFormat* format = nullptr;
        if (FAILED(m_write->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px,
                m_zh ? L"zh-cn" : L"en-us", &format)))
            return nullptr;
        m_formats.emplace(key, format);
        return format;
    }

    ID2D1Factory* m_d2d = nullptr;
    IDWriteFactory* m_write = nullptr;
    ID2D1DCRenderTarget* m_target = nullptr;
    std::wstring m_textFamily;
    std::wstring m_digitFamily;
    std::map<std::tuple<std::wstring, int, int>, IDWriteTextFormat*> m_formats;

public:
    bool m_zh = true;
};

struct Hit {
    int id = kNone;
    RECT r {};
};

int hitAt(const std::vector<Hit>& hits, POINT p)
{
    for (const Hit& h : hits) {
        if (::PtInRect(&h.r, p))
            return h.id;
    }
    return kNone;
}

UINT monitorDpi(HMONITOR monitor)
{
    UINT x = 96, y = 96;
    return SUCCEEDED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) ? x : 96;
}

// One monitor's part of the break screen: the dimmed backdrop and, owned by
// it so it stays above, the countdown in the middle.
struct Screen {
    HWND dim = nullptr;
    HWND content = nullptr;
    RECT monitor {};
    UINT dpi = 96;
    Surface surface;
    std::vector<Hit> hits;
    int hover = kNone;
    int pressed = kNone;
    POINT pos {};
    std::wstring drawn; // what the surface shows (drawScreen)
};

class Windows;
Windows* g_windows = nullptr; // for the keyboard hook, which has no context

class Windows {
public:
    explicit Windows(Core& core) : m_core(core) {}
    Windows(const Windows&) = delete;
    Windows& operator=(const Windows&) = delete;

    HWND create()
    {
        g_windows = this;
        m_graphics.init();
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc {sizeof wc};
        wc.lpfnWndProc = &Windows::proc;
        wc.hInstance = instance;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        for (const wchar_t* name : {kHiddenClass, kPillClass, kContentClass}) {
            wc.lpszClassName = name;
            ::RegisterClassExW(&wc);
        }
        m_dimBrush = ::CreateSolidBrush(kDimColor);
        wc.hbrBackground = m_dimBrush;
        wc.lpszClassName = kDimClass;
        ::RegisterClassExW(&wc);

        // A hidden top-level window, not a message-only one: those miss the
        // broadcasts (WM_POWERBROADCAST, WM_DISPLAYCHANGE).
        m_hidden = ::CreateWindowExW(WS_EX_TOOLWINDOW, kHiddenClass, L"WinShun break", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!m_hidden)
            return nullptr;
        ::WTSRegisterSessionNotification(m_hidden, NOTIFY_FOR_THIS_SESSION);
        ::SetTimer(m_hidden, kTickTimer, kTickMs, nullptr);
        tick();
        return m_hidden;
    }

    void destroy()
    {
        unhook();
        destroyScreens();
        destroyPill();
        if (m_dimBrush)
            ::DeleteObject(m_dimBrush);
        g_windows = nullptr;
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        if (m == WM_NCCREATE)
            ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        if (auto* self = reinterpret_cast<Windows*>(::GetWindowLongPtrW(h, GWLP_USERDATA))) {
            LRESULT result = 0;
            if (self->handle(h, m, w, l, result))
                return result;
        }
        return ::DefWindowProcW(h, m, w, l);
    }

private:
    // ---- the clock ----

    void tick()
    {
        const std::int64_t now = Core::now();
        const std::int64_t wall = Core::wall();
        // Stood still as long as `away` (Modern Standby may say neither
        // sleep nor lock): as after a sleep.
        restartIfAway(m_tickedAt);
        m_tickedAt = now;
        if (now - m_quietAt >= kQuietEveryMs) {
            const bool quiet = m_core.quietRule && quietTime();
            if (quiet != m_quiet && m_core.log)
                m_core.log(quiet ? "breaks wait: notification state " + std::to_string(notificationState()) + ", Do not disturb "
                                     + std::to_string(focusAssist())
                                 : "breaks no longer wait");
            m_quiet = quiet;
            m_quietAt = now;
        }
        const Sense sense {now, wall, idleMs(), m_quiet, m_locked, displayKeptOn()};
        State st;
        {
            const std::lock_guard lock(m_core.mutex);
            m_core.schedule.tick(sense);
            st = m_core.schedule.state(now, wall);
            m_config = m_core.schedule.config();
            m_zh = m_core.chinese;
            m_light = m_core.light;
        }
        m_graphics.m_zh = m_zh;
        m_state = st;
        if (st.phase == Phase::HeadsUp || st.phase == Phase::Pending)
            showPill();
        else
            destroyPill();
        if (st.phase == Phase::OnBreak)
            showScreens(now);
        else
            hideScreens(now);
        if (st.phase != Phase::OnBreak && m_hook && !m_keys.draining())
            startDrain(now); // the keyboard back even when no screen could be shown
        if (m_keys.draining() && (m_keys.empty() || now - m_drainStart >= kDrainMs))
            unhook();
    }

    void choose(int choice)
    {
        const std::int64_t now = Core::now();
        {
            const std::lock_guard lock(m_core.mutex);
            if (choice == kPostpone)
                m_core.schedule.postpone(now);
            else if (choice == kSkip)
                m_core.schedule.skip(now);
        }
        tick();
    }

    void leaveStrict()
    {
        {
            const std::lock_guard lock(m_core.mutex);
            m_core.schedule.skip(Core::now(), true);
        }
        if (m_core.log)
            m_core.log("left a strict break with the long Esc");
        tick();
    }

    void restartIfAway(std::int64_t since)
    {
        const std::int64_t now = Core::now();
        const std::lock_guard lock(m_core.mutex);
        if (since && now - since >= m_core.schedule.config().resetIdleMs)
            m_core.schedule.restart(now);
    }

    // ---- the notice ----

    void showPill()
    {
        const bool zh = m_zh;
        const State& st = m_state;
        const std::wstring what = st.next == Kind::Long ? (zh ? L"大休 " : L"Long break, ") + duration(m_config.longMs, zh)
                                                        : (zh ? L"小休 " : L"Short break, ") + duration(m_config.shortMs, zh);
        std::wstring text;
        if (st.phase == Phase::Pending)
            text = zh ? L"马上开始" + what + L"，先停一下手" : what + L" now: pause what you are doing";
        else
            text = zh ? duration(st.untilDueMs, true) + L"后" + what : what + L" in " + duration(st.untilDueMs, false);
        std::vector<std::pair<int, std::wstring>> buttons;
        if (st.canPostpone)
            buttons.emplace_back(kPostpone, (zh ? L"推迟 " : L"Postpone ") + duration(m_config.snoozeMs, zh));
        if (!st.strict)
            buttons.emplace_back(kSkip, zh ? L"跳过" : L"Skip");

        if (!m_pill) {
            const HMONITOR monitor = ::MonitorFromWindow(::GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO info {sizeof info};
            ::GetMonitorInfoW(monitor, &info);
            m_pillWork = info.rcWork;
            m_pillDpi = monitorDpi(monitor);
            m_pill = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kPillClass, L"", WS_POPUP, 0, 0, 0, 0,
                nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
            m_pillHover = m_pillPressed = kNone;
            m_pillKey.clear();
            m_pillShown = false;
        }
        std::wstring key = text + L'|' + std::to_wstring(m_pillHover) + L'|' + std::to_wstring(m_light);
        for (const auto& [id, label] : buttons)
            key += L'|' + label;
        if (key == m_pillKey)
            return;
        m_pillKey = key;
        drawPill(text, buttons);
    }

    void drawPill(const std::wstring& text, const std::vector<std::pair<int, std::wstring>>& buttons)
    {
        const float s = m_pillDpi / 96.f;
        const auto px = [s](float logical) { return std::round(logical * s); };
        const UINT32 background = m_light ? 0xF9F9F9 : 0x202020;
        const UINT32 edgeColor = m_light ? 0xB4B4B4 : 0x6A6A6A;
        const UINT32 ink = m_light ? 0x1B1B1B : 0xF3F3F3;
        const float buttonAlpha = m_light ? 0.05f : 0.07f;
        const float hoverAlpha = m_light ? 0.10f : 0.13f;

        IDWriteTextLayout* label = m_graphics.layout(text, m_graphics.text(DWRITE_FONT_WEIGHT_NORMAL, px(15)), 4000.f);
        if (!label)
            return;
        DWRITE_TEXT_METRICS lm {};
        label->GetMetrics(&lm);
        std::vector<IDWriteTextLayout*> layouts;
        std::vector<float> widths;
        float buttonsWidth = 0;
        const float buttonPad = px(14);
        const float gap = px(8);
        for (const auto& b : buttons) {
            IDWriteTextLayout* l = m_graphics.layout(b.second, m_graphics.text(DWRITE_FONT_WEIGHT_NORMAL, px(14)), 4000.f);
            DWRITE_TEXT_METRICS m {};
            if (l)
                l->GetMetrics(&m);
            layouts.push_back(l);
            widths.push_back(std::ceil(m.widthIncludingTrailingWhitespace) + 2 * buttonPad);
            buttonsWidth += widths.back();
        }
        if (!buttons.empty())
            buttonsWidth += gap * (buttons.size() - 1) + px(16);
        const float height = px(48);
        const float padLeft = px(20);
        const float padRight = buttons.empty() ? padLeft : px(8);
        const float width = padLeft + std::ceil(lm.widthIncludingTrailingWhitespace) + buttonsWidth + padRight;
        const float margin = px(16); // room for the shadow
        const int w = static_cast<int>(width + 2 * margin);
        const int h = static_cast<int>(height + 2 * margin);
        m_pillPos = {m_pillWork.left + (m_pillWork.right - m_pillWork.left - w) / 2, m_pillWork.top + static_cast<LONG>(px(16) - margin)};

        m_pillHits.clear();
        if (m_pillSurface.ensure(w, h)) {
            if (ID2D1RenderTarget* rt = m_graphics.begin(m_pillSurface)) {
                const D2D1_RECT_F body = D2D1::RectF(margin, margin, margin + width, margin + height);
                const float radius = height / 2;
                for (int i = 8; i >= 1; --i) { // a soft shadow, a little below
                    const float grow = i * s * 1.5f;
                    const D2D1_RECT_F r = D2D1::RectF(body.left - grow, body.top - grow + px(2), body.right + grow, body.bottom + grow + px(2));
                    m_graphics.fill(rt, r, radius + grow, rgba(0x000000, m_light ? 0.016f : 0.04f));
                }
                m_graphics.fill(rt, body, radius, rgba(background));
                m_graphics.stroke(rt, body, radius, std::max(1.f, std::floor(s)), rgba(edgeColor));
                m_graphics.draw(rt, label, body.left + padLeft, std::round(body.top + (height - lm.height) / 2), rgba(ink));
                float x = body.left + padLeft + std::ceil(lm.widthIncludingTrailingWhitespace) + px(16);
                const float bh = px(32);
                const float by = body.top + std::round((height - bh) / 2);
                for (std::size_t i = 0; i < buttons.size(); ++i) {
                    const D2D1_RECT_F r = D2D1::RectF(x, by, x + widths[i], by + bh);
                    const bool hot = m_pillHover == buttons[i].first;
                    m_graphics.fill(rt, r, bh / 2, rgba(m_light ? 0x000000 : 0xFFFFFF, hot ? hoverAlpha : buttonAlpha));
                    DWRITE_TEXT_METRICS m {};
                    if (layouts[i])
                        layouts[i]->GetMetrics(&m);
                    m_graphics.draw(rt, layouts[i], x + buttonPad, std::round(by + (bh - m.height) / 2), rgba(ink));
                    m_pillHits.push_back({buttons[i].first, {static_cast<LONG>(r.left), static_cast<LONG>(r.top), static_cast<LONG>(r.right), static_cast<LONG>(r.bottom)}});
                    x += widths[i] + gap;
                }
                m_graphics.end();
            }
        }
        label->Release();
        for (IDWriteTextLayout* l : layouts)
            release(l);
        present(m_pill, m_pillSurface, m_pillPos, 255);
        if (!m_pillShown) {
            ::ShowWindow(m_pill, SW_SHOWNOACTIVATE);
            m_pillShown = true;
        }
        ::SetWindowPos(m_pill, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void destroyPill()
    {
        if (m_pill) {
            ::DestroyWindow(m_pill);
            m_pill = nullptr;
        }
        m_pillHits.clear();
    }

    // ---- the break screen ----

    void showScreens(std::int64_t now)
    {
        if (m_screens.empty()) {
            buildScreens();
            if (m_screens.empty())
                return; // no window (and so no hook): tried again at the next tick
            m_fadeStart = now;
            m_fadingOut = false;
            hook();
            animate(true);
        } else if (m_fadingOut) { // the break came back while it faded out
            m_fadingOut = false;
            m_fadeStart = now;
            hook();
            animate(true);
        }
        for (auto& screen : m_screens)
            drawScreen(*screen);
        if (now - m_raisedAt >= 1000) { // stay above what came up since
            for (auto& screen : m_screens) {
                ::SetWindowPos(screen->dim, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                if (screen->content)
                    ::SetWindowPos(screen->content, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            m_raisedAt = now;
        }
    }

    void hideScreens(std::int64_t now)
    {
        if (m_screens.empty() || m_fadingOut)
            return;
        m_fadingOut = true;
        m_fadeStart = now;
        m_escDown = false;
        startDrain(now);
        animate(true);
    }

    void buildScreens()
    {
        destroyScreens();
        std::vector<RECT> monitors;
        ::EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM list) -> BOOL {
            reinterpret_cast<std::vector<RECT>*>(list)->push_back(*r);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&monitors));
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        for (const RECT& r : monitors) {
            auto screen = std::make_unique<Screen>();
            screen->monitor = r;
            screen->dpi = monitorDpi(::MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST));
            const DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            screen->dim = ::CreateWindowExW(ex, kDimClass, L"", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, this);
            if (!screen->dim)
                continue;
            ::SetLayeredWindowAttributes(screen->dim, 0, 0, LWA_ALPHA);
            screen->content = ::CreateWindowExW(ex, kContentClass, L"", WS_POPUP, 0, 0, 0, 0, screen->dim, nullptr, instance, this);
            m_screens.push_back(std::move(screen));
        }
        for (auto& screen : m_screens) {
            ::ShowWindow(screen->dim, SW_SHOWNOACTIVATE);
            drawScreen(*screen, 0);
            if (screen->content)
                ::ShowWindow(screen->content, SW_SHOWNOACTIVATE);
        }
        m_raisedAt = 0;
        if (m_core.log)
            m_core.log("break screen on " + std::to_string(m_screens.size()) + " monitor(s)");
    }

    void destroyScreens()
    {
        for (auto& screen : m_screens) {
            if (screen->content)
                ::DestroyWindow(screen->content);
            if (screen->dim)
                ::DestroyWindow(screen->dim);
        }
        m_screens.clear();
    }

    BYTE contentAlpha(std::int64_t now) const
    {
        double t = 1;
        if (m_fadingOut)
            t = 1 - std::clamp(static_cast<double>(now - m_fadeStart) / kFadeOutMs, 0.0, 1.0);
        else
            t = std::clamp(static_cast<double>(now - m_fadeStart) / kFadeInMs, 0.0, 1.0);
        t = 1 - (1 - t) * (1 - t); // ease out
        return static_cast<BYTE>(std::lround(255 * t));
    }

    void drawScreen(Screen& screen) { drawScreen(screen, contentAlpha(Core::now())); }

    void drawScreen(Screen& screen, BYTE alpha)
    {
        if (!screen.content)
            return;
        const bool zh = m_zh;
        const State& st = m_state;
        const float s = screen.dpi / 96.f;
        const auto px = [s](float logical) { return std::round(logical * s); };
        const int w = static_cast<int>(px(640)); // the words
        const int h = static_cast<int>(px(380));
        const int ex = static_cast<int>(std::lround(w * 0.3)); // around them, the backing fades out
        const int ey = static_cast<int>(std::lround(h * 0.3));
        const LONG mw = screen.monitor.right - screen.monitor.left;
        const LONG mh = screen.monitor.bottom - screen.monitor.top;
        screen.pos = {screen.monitor.left + (mw - w) / 2 - ex, screen.monitor.top + (mh - h) / 2 - static_cast<LONG>(px(40)) - ey};

        const bool longBreak = st.next == Kind::Long;
        const std::wstring title = longBreak ? (zh ? L"起来走走" : L"Long break") : (zh ? L"小休一下" : L"Short break");
        const std::wstring tip = longBreak
            ? (zh ? L"离开座位，喝口水，活动一下肩膀和脖子" : L"Stand up, get some water, stretch your neck and shoulders")
            : (zh ? L"看看 6 米外的地方，眨眨眼，让眼睛歇一歇" : L"Look at something 20 feet away and blink a few times");
        std::wstring hint;
        if (st.strict)
            hint = m_escDown ? (zh ? L"继续按住 Esc…" : L"Keep holding Esc…") : (zh ? L"长按 Esc 3 秒可以退出" : L"Hold Esc for 3 seconds to leave");
        else
            hint = zh ? L"按 Esc 跳过" : L"Esc to skip";
        std::vector<std::pair<int, std::wstring>> buttons;
        if (!st.strict) {
            if (st.canPostpone)
                buttons.emplace_back(kPostpone, (zh ? L"推迟 " : L"Postpone ") + duration(m_config.snoozeMs, zh));
            buttons.emplace_back(kSkip, zh ? L"跳过" : L"Skip");
        }

        // Drawn again only when something on it changed (the second, a
        // hover, the held Esc), not at every tick: in software, and large
        // with its backing. The track moves with the second too.
        const std::int64_t secondsLeft = (st.breakLeftMs + 999) / 1000;
        const int held = st.strict && m_escDown
            ? static_cast<int>(std::clamp<std::int64_t>(Core::now() - m_escSince, 0, kEscHoldMs) * 100 / kEscHoldMs)
            : -1;
        std::wstring key = std::to_wstring(secondsLeft) + L'|' + std::to_wstring(held) + L'|' + std::to_wstring(screen.hover) + L'|'
            + std::to_wstring(alpha) + L'|' + title + L'|' + hint;
        for (const auto& [id, label] : buttons)
            key += L'|' + label;
        if (key == screen.drawn)
            return;
        screen.drawn = key;

        screen.hits.clear();
        if (screen.surface.ensure(w + 2 * ex, h + 2 * ey)) {
            if (ID2D1RenderTarget* rt = m_graphics.begin(screen.surface)) {
                // Behind the words the dim's colour, solid: through the dim
                // the desktop still shows a little, and a line of the page
                // under the time read as crossed out.
                m_graphics.glow(rt, D2D1::RectF(0, 0, static_cast<float>(w + 2 * ex), static_cast<float>(h + 2 * ey)), 0.62f, rgba(kDimRgb));
                rt->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(ex), static_cast<float>(ey)));
                const float width = static_cast<float>(w);
                const auto line = [&](const std::wstring& text, IDWriteTextFormat* format, float top, D2D1_COLOR_F color) {
                    IDWriteTextLayout* l = m_graphics.layout(text, format, width - px(80), true);
                    m_graphics.draw(rt, l, px(40), top, color);
                    release(l);
                };
                line(title, m_graphics.text(DWRITE_FONT_WEIGHT_SEMI_BOLD, px(30)), px(24), rgba(0xFFFFFF));
                line(tip, m_graphics.text(DWRITE_FONT_WEIGHT_NORMAL, px(17)), px(76), rgba(0xFFFFFF, 0.75f));
                line(countdown(st.breakLeftMs), m_graphics.digits(DWRITE_FONT_WEIGHT_SEMI_LIGHT, px(80)), px(132), rgba(0xFFFFFF));

                // How much of the break is over: a thin track under the time.
                const float barWidth = px(240);
                const float barHeight = std::max(1.f, px(2));
                const float barLeft = std::round((width - barWidth) / 2);
                const float barTop = px(250);
                const double done = st.breakLengthMs > 0 ? 1.0 - static_cast<double>(secondsLeft * 1000) / st.breakLengthMs : 1.0;
                m_graphics.fill(rt, D2D1::RectF(barLeft, barTop, barLeft + barWidth, barTop + barHeight), barHeight / 2, rgba(0xFFFFFF, 0.18f));
                m_graphics.fill(rt, D2D1::RectF(barLeft, barTop, barLeft + std::round(barWidth * static_cast<float>(std::clamp(done, 0.0, 1.0))), barTop + barHeight),
                    barHeight / 2, rgba(0xFFFFFF, 0.7f));

                float hintTop = px(288);
                if (!buttons.empty()) {
                    const float bh = px(36);
                    const float pad = px(20);
                    const float gap = px(12);
                    std::vector<IDWriteTextLayout*> layouts;
                    std::vector<float> widths;
                    float total = gap * (buttons.size() - 1);
                    for (const auto& b : buttons) {
                        IDWriteTextLayout* l = m_graphics.layout(b.second, m_graphics.text(DWRITE_FONT_WEIGHT_NORMAL, px(15)), 4000.f);
                        DWRITE_TEXT_METRICS m {};
                        if (l)
                            l->GetMetrics(&m);
                        layouts.push_back(l);
                        widths.push_back(std::ceil(m.widthIncludingTrailingWhitespace) + 2 * pad);
                        total += widths.back();
                    }
                    float x = std::round((width - total) / 2);
                    const float top = px(282);
                    for (std::size_t i = 0; i < buttons.size(); ++i) {
                        const D2D1_RECT_F r = D2D1::RectF(x, top, x + widths[i], top + bh);
                        const bool hot = screen.hover == buttons[i].first;
                        m_graphics.fill(rt, r, bh / 2, rgba(0xFFFFFF, hot ? 0.24f : 0.14f));
                        DWRITE_TEXT_METRICS m {};
                        if (layouts[i])
                            layouts[i]->GetMetrics(&m);
                        m_graphics.draw(rt, layouts[i], x + pad, std::round(top + (bh - m.height) / 2), rgba(0xFFFFFF));
                        screen.hits.push_back({buttons[i].first, {static_cast<LONG>(r.left) + ex, static_cast<LONG>(r.top) + ey,
                                                                     static_cast<LONG>(r.right) + ex, static_cast<LONG>(r.bottom) + ey}});
                        x += widths[i] + gap;
                        release(layouts[i]);
                    }
                    hintTop = px(334);
                }
                line(hint, m_graphics.text(DWRITE_FONT_WEIGHT_NORMAL, px(13)), hintTop, rgba(0xFFFFFF, 0.5f));
                if (st.strict && m_escDown) {
                    const float held = static_cast<float>(std::clamp<std::int64_t>(Core::now() - m_escSince, 0, kEscHoldMs)) / kEscHoldMs;
                    const float holdWidth = px(160);
                    const float holdLeft = std::round((width - holdWidth) / 2);
                    const float holdTop = hintTop + px(28);
                    m_graphics.fill(rt, D2D1::RectF(holdLeft, holdTop, holdLeft + holdWidth, holdTop + barHeight), barHeight / 2, rgba(0xFFFFFF, 0.18f));
                    m_graphics.fill(rt, D2D1::RectF(holdLeft, holdTop, holdLeft + std::round(holdWidth * held), holdTop + barHeight), barHeight / 2, rgba(0xFFFFFF, 0.8f));
                }
                m_graphics.end();
            }
        }
        present(screen.content, screen.surface, screen.pos, alpha);
    }

    void present(HWND window, Surface& surface, POINT pos, BYTE alpha)
    {
        if (!window || !surface.dc)
            return;
        SIZE size {surface.width, surface.height};
        POINT source {0, 0};
        BLENDFUNCTION blend {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        ::UpdateLayeredWindow(window, nullptr, &pos, &size, surface.dc, &source, 0, &blend, ULW_ALPHA);
    }

    // ---- fades and the held Esc ----

    void animate(bool on)
    {
        if (on != m_animating) {
            m_animating = on;
            if (on)
                ::SetTimer(m_hidden, kAnimTimer, kAnimMs, nullptr);
            else
                ::KillTimer(m_hidden, kAnimTimer);
        }
    }

    void frame()
    {
        // The held Esc first: leaving starts the fade-out, which this same
        // frame then carries on (and keeps the timer for).
        if (m_escDown && m_state.strict && !m_screens.empty() && !m_fadingOut) {
            if (Core::now() - m_escSince >= kEscHoldMs) {
                m_escDown = false;
                leaveStrict();
            } else {
                for (auto& screen : m_screens)
                    drawScreen(*screen);
            }
        }
        const std::int64_t now = Core::now();
        bool more = m_escDown && m_state.strict && !m_screens.empty() && !m_fadingOut;
        if (!m_screens.empty()) {
            const BYTE alpha = contentAlpha(now);
            const BYTE dim = static_cast<BYTE>(alpha * kDimAlpha / 255);
            for (auto& screen : m_screens) {
                ::SetLayeredWindowAttributes(screen->dim, 0, dim, LWA_ALPHA);
                if (screen->content) {
                    BLENDFUNCTION blend {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
                    ::UpdateLayeredWindow(screen->content, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
                }
            }
            const std::int64_t fade = m_fadingOut ? kFadeOutMs : kFadeInMs;
            if (now - m_fadeStart < fade)
                more = true;
            else if (m_fadingOut)
                destroyScreens();
        }
        if (m_keys.draining() && (m_keys.empty() || now - m_drainStart >= kDrainMs))
            unhook();
        animate(more);
    }

    void escape(bool down)
    {
        if (m_screens.empty() || m_fadingOut)
            return;
        if (!down) {
            for (auto& screen : m_screens)
                drawScreen(*screen);
            return;
        }
        if (!m_state.strict)
            choose(kSkip);
        else
            animate(true);
    }

    // ---- the keyboard while the break screen is up ----

    void hook()
    {
        if (!m_hook) {
            m_keys = {};
            m_hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &Windows::keyboard, ::GetModuleHandleW(nullptr), 0);
            if (!m_hook && m_core.log)
                m_core.log("no keyboard hook: " + std::to_string(::GetLastError()));
        }
        m_keys.close();
    }

    // The keys the hook took stay held back until they are up again (at
    // most kDrainMs; Keys.h).
    void startDrain(std::int64_t now)
    {
        if (!m_hook)
            return;
        m_keys.drain();
        m_drainStart = now;
        if (m_keys.empty())
            unhook();
    }

    void unhook()
    {
        if (m_hook) {
            ::UnhookWindowsHookEx(m_hook);
            m_hook = nullptr;
        }
        m_keys = {};
        m_escDown = false;
    }

    static LRESULT CALLBACK keyboard(int code, WPARAM w, LPARAM l)
    {
        if (code == HC_ACTION && g_windows && g_windows->key(w, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(l)))
            return 1;
        return ::CallNextHookEx(nullptr, code, w, l);
    }

    // True: the key is kept from the program underneath.
    bool key(WPARAM message, const KBDLLHOOKSTRUCT& k)
    {
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const DWORD vk = k.vkCode;
        const bool pressed = down && !m_keys.took(vk);
        const bool kept = m_keys.key(vk, down, (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0);
        if (vk == VK_ESCAPE && !m_keys.draining()) {
            if (pressed && m_keys.took(vk)) { // pressed on the screen, not a repeat
                m_escDown = true;
                m_escSince = Core::now();
                ::PostMessageW(m_hidden, kEsc, 1, 0);
            } else if (!down && m_escDown) {
                m_escDown = false;
                ::PostMessageW(m_hidden, kEsc, 0, 0);
            }
        }
        return kept;
    }

    // ---- messages ----

    bool handle(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT& result)
    {
        if (h == m_hidden) {
            switch (m) {
            case WM_TIMER:
                if (w == kTickTimer)
                    tick();
                else if (w == kAnimTimer)
                    frame();
                return true;
            case kWake:
                tick();
                return true;
            case kEsc:
                escape(w != 0);
                return true;
            case WM_WTSSESSION_CHANGE:
                if (w == WTS_SESSION_LOCK) {
                    m_locked = true;
                    m_lockedAt = Core::now();
                } else if (w == WTS_SESSION_UNLOCK) {
                    m_locked = false;
                    restartIfAway(m_lockedAt);
                }
                tick();
                return true;
            case WM_POWERBROADCAST:
                if (w == PBT_APMSUSPEND)
                    m_suspendedAt = Core::now();
                else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
                    restartIfAway(m_suspendedAt); // a key that woke the PC is input, so idleMs() would not tell
                    m_suspendedAt = 0;
                    tick();
                }
                result = TRUE;
                return true;
            case WM_DISPLAYCHANGE:
                if (!m_screens.empty() && !m_fadingOut) {
                    buildScreens();
                    animate(true); // the new ones dimmed as the old were (they start clear)
                }
                destroyPill(); // made again where it belongs at the next tick
                return true;
            case WM_CLOSE:
                ::DestroyWindow(h);
                return true;
            case WM_DESTROY:
                ::WTSUnRegisterSessionNotification(h);
                ::PostQuitMessage(0);
                return true;
            default:
                return false;
            }
        }
        switch (m) {
        case WM_MOUSEACTIVATE:
            result = MA_NOACTIVATE; // clicks never take the focus from the program underneath
            return true;
        case WM_SETCURSOR:
            ::SetCursor(::LoadCursorW(nullptr, hoverAt(h) != kNone ? IDC_HAND : IDC_ARROW));
            result = TRUE;
            return true;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track {sizeof track, TME_LEAVE, h, 0};
            ::TrackMouseEvent(&track);
            setHover(h, hitAt(hitsOf(h), {GET_X_LPARAM_(l), GET_Y_LPARAM_(l)}));
            return true;
        }
        case WM_MOUSELEAVE:
            setHover(h, kNone);
            return true;
        case WM_LBUTTONDOWN:
            pressedOf(h) = hitAt(hitsOf(h), {GET_X_LPARAM_(l), GET_Y_LPARAM_(l)});
            return true;
        case WM_LBUTTONUP: {
            const int id = hitAt(hitsOf(h), {GET_X_LPARAM_(l), GET_Y_LPARAM_(l)});
            const bool same = id != kNone && id == pressedOf(h);
            pressedOf(h) = kNone;
            if (same)
                choose(id);
            return true;
        }
        default:
            return false;
        }
    }

    static int GET_X_LPARAM_(LPARAM l) { return static_cast<int>(static_cast<short>(LOWORD(l))); }
    static int GET_Y_LPARAM_(LPARAM l) { return static_cast<int>(static_cast<short>(HIWORD(l))); }

    Screen* screenOf(HWND h)
    {
        for (auto& screen : m_screens) {
            if (screen->content == h || screen->dim == h)
                return screen.get();
        }
        return nullptr;
    }
    const std::vector<Hit>& hitsOf(HWND h)
    {
        static const std::vector<Hit> none;
        if (h == m_pill)
            return m_pillHits;
        if (Screen* screen = screenOf(h); screen && screen->content == h)
            return screen->hits;
        return none;
    }
    int& pressedOf(HWND h)
    {
        static int none = kNone;
        if (h == m_pill)
            return m_pillPressed;
        if (Screen* screen = screenOf(h))
            return screen->pressed;
        none = kNone;
        return none;
    }
    int hoverAt(HWND h)
    {
        POINT p {};
        ::GetCursorPos(&p);
        ::ScreenToClient(h, &p);
        return hitAt(hitsOf(h), p);
    }
    void setHover(HWND h, int id)
    {
        if (h == m_pill) {
            if (m_pillHover != id) {
                m_pillHover = id;
                showPill();
            }
            return;
        }
        if (Screen* screen = screenOf(h); screen && screen->content == h && screen->hover != id) {
            screen->hover = id;
            drawScreen(*screen);
        }
    }

    Core& m_core;
    Graphics m_graphics;
    HWND m_hidden = nullptr;
    HBRUSH m_dimBrush = nullptr;
    State m_state;
    Config m_config;
    bool m_zh = true;
    bool m_light = true;
    bool m_quiet = false;
    std::int64_t m_quietAt = -kQuietEveryMs;
    bool m_locked = false;
    std::int64_t m_lockedAt = 0;
    std::int64_t m_suspendedAt = 0;
    std::int64_t m_tickedAt = 0;

    HWND m_pill = nullptr;
    bool m_pillShown = false;
    RECT m_pillWork {};
    UINT m_pillDpi = 96;
    POINT m_pillPos {};
    Surface m_pillSurface;
    std::vector<Hit> m_pillHits;
    int m_pillHover = kNone;
    int m_pillPressed = kNone;
    std::wstring m_pillKey;

    std::vector<std::unique_ptr<Screen>> m_screens;
    bool m_fadingOut = false;
    std::int64_t m_fadeStart = 0;
    std::int64_t m_raisedAt = 0;
    bool m_animating = false;

    HHOOK m_hook = nullptr;
    KeyGate m_keys;
    std::int64_t m_drainStart = 0;
    bool m_escDown = false;
    std::int64_t m_escSince = 0;
};

} // namespace

void Ui::start()
{
    if (m_thread.joinable())
        return;
    std::promise<HWND> ready;
    std::future<HWND> window = ready.get_future();
    m_thread = std::thread([this, &ready] {
        ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Windows windows(m_core);
        const HWND hidden = windows.create();
        ready.set_value(hidden);
        if (hidden) {
            MSG msg;
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
        }
        windows.destroy();
    });
    m_window = window.get();
    if (!m_window && m_core.log)
        m_core.log("could not create the windows: " + std::to_string(::GetLastError()));
}

void Ui::stop()
{
    if (!m_thread.joinable())
        return;
    if (m_window)
        ::PostMessageW(static_cast<HWND>(m_window), WM_CLOSE, 0, 0);
    m_thread.join();
    m_window = nullptr;
}

void Ui::wake()
{
    if (m_window)
        ::PostMessageW(static_cast<HWND>(m_window), kWake, 0, 0);
}

} // namespace brk

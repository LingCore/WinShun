#ifndef NOMINMAX
#define NOMINMAX // before windows.h comes in with Cards.h: std::min and std::max stay usable
#endif

#include "Cards.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <utility>
#include <vector>
using std::max;
using std::min;
#include <gdiplus.h>

// What the gestures show besides the trail, in three kinds:
// - the note (flash): a pill with an icon, drawn like the trail's hint on a
//   layered window that the pointer goes through; its soft shadow is drawn
//   into the same bitmap, with room around it so that it is never cut.
// - the confirm card and the bubble: real windows with Windows' own frame
//   (as styleFramelessWindow does it in the app): WS_CAPTION with no
//   non-client area, DWM's corners, border and shadow, so that they look as
//   any window of the system does. The client area is drawn by GDI+ into a DIB
//   and put on the window by BitBlt, never twice a frame.
// All of them are topmost and never activated; the keys they take are
// hotkeys. Icons are Phosphor Regular, a subset of the project's font
// (icons/make-subset.py) that is a resource of the exe.
//
// Rules that hold for all of them:
// - No GDI+ object outlives a call (fonts, bitmaps and graphics are locals):
//   the thread that started GDI+ may shut it down before the destructor runs.
// - destroy() must be called on the thread that made the windows (the
//   destructor is only the fallback); it drops a pending confirm or bubble
//   without calling done or clicked, because whoever owns them may be on its
//   way out too.
// - Esc (and Enter, for the confirm card) are hotkeys while a card is up. A
//   key the system already gave away (the engine's own Esc while it draws)
//   cannot be had, so it is asked for again for a few seconds; a confirm
//   card closes the bubble first, for it needs Esc itself.
// - Sizes are logical pixels on a 4-pixel grid (a few are the design's own),
//   scaled by the DPI of the monitor the card is on.

namespace gestures {

namespace {

constexpr wchar_t kNoteClass[] = L"WinShun.Gestures.Note";
constexpr wchar_t kCardClass[] = L"WinShun.Gestures.Card";
constexpr wchar_t kBubbleClass[] = L"WinShun.Gestures.Bubble";
constexpr wchar_t kIconResource[] = L"PHOSPHOR_GESTURES"; // RCDATA, resources/WinShunGestures.rc

constexpr UINT_PTR kHideTimer = 1; // the note: its time is up
constexpr UINT_PTR kFadeTimer = 2; // the note: fading out
constexpr UINT_PTR kRingTimer = 1; // the card: the ring and the count
constexpr UINT_PTR kKeysTimer = 2; // the card and the bubble: ask for the keys again
constexpr UINT_PTR kDotsTimer = 3; // the bubble, while it waits: the dots
constexpr UINT kFadeMs = 120;
constexpr UINT kFadeFrameMs = 15;
constexpr UINT kRingMs = 33; // about 30 frames a second, only while the card is up
constexpr UINT kDotsMs = 60;
constexpr UINT kKeysRetryMs = 200;
constexpr int kKeysTries = 25;
constexpr int kEscId = 1;
constexpr int kEnterId = 2;
constexpr int kMaxLines = 12; // of a translation, then an ellipsis
constexpr float kTall = 100000.f;

// DWM's attributes, spelled out as WindowEffects.cpp does (older SDKs).
constexpr DWORD kDwmDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kDwmDarkModeOld = 19; // before build 18985
constexpr DWORD kDwmCorner = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kDwmBorder = 34; // DWMWA_BORDER_COLOR
constexpr int kCornerRound = 2; // DWMWCP_ROUND
constexpr int kCornerSquare = 1; // DWMWCP_DONOTROUND
constexpr COLORREF kColorDefault = 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT

int px(double logical, double scale)
{
    return static_cast<int>(std::lround(logical * scale));
}

float fl(double v)
{
    return static_cast<float>(v);
}

// IDC_ARROW and IDC_HAND are narrow strings unless UNICODE is defined; the W
// call wants these.
HCURSOR cursor(bool hand)
{
    return ::LoadCursorW(nullptr, MAKEINTRESOURCEW(hand ? 32649 : 32512));
}

POINT pointOf(LPARAM l)
{
    return {static_cast<int>(static_cast<short>(LOWORD(l))), static_cast<int>(static_cast<short>(HIWORD(l)))};
}

// Win32 and GDI+ both want the thread per-monitor v2 aware for the sizes
// of monitors and windows to be real pixels; whatever the caller has set is
// put back after each call.
class DpiScope {
public:
    DpiScope() : m_old(::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    DpiScope(const DpiScope&) = delete;
    DpiScope& operator=(const DpiScope&) = delete;
    ~DpiScope()
    {
        if (m_old)
            ::SetThreadDpiAwarenessContext(m_old);
    }

private:
    DPI_AWARENESS_CONTEXT m_old;
};

// ---- the screen ----

struct Screen {
    RECT work {0, 0, 1920, 1080};
    double scale = 1;
};

// The work area and the scale (its DPI over 96) of the monitor `at` is on.
Screen screenAt(POINT at)
{
    Screen screen;
    const HMONITOR monitor = ::MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info {sizeof info};
    if (::GetMonitorInfoW(monitor, &info))
        screen.work = info.rcWork;
    else
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen.work, 0);
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0)
        screen.scale = dpiX / 96.0;
    return screen;
}

// A w by h box `offset` right of and below `at`; on the other side of it
// where the work area ends, and always inside the work area.
RECT place(POINT at, int w, int h, int offset, const RECT& work)
{
    int x = at.x + offset;
    int y = at.y + offset;
    if (x + w > work.right)
        x = at.x - offset - w;
    if (y + h > work.bottom)
        y = at.y - offset - h;
    x = std::clamp(x, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - w));
    y = std::clamp(y, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - h));
    return {x, y, x + w, y + h};
}

// ---- the surface ----

// A top-down 32-bit bitmap, drawn on by GDI+: UpdateLayeredWindow takes the
// note's from it, and BitBlt the card's and the bubble's.
class Surface {
public:
    Surface() = default;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    ~Surface() { reset(); }

    bool ensure(int w, int h)
    {
        if (m_dc && w == m_width && h == m_height)
            return true;
        reset();
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
            return false;
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        m_dc = ::CreateCompatibleDC(nullptr);
        if (m_dc)
            m_bitmap = ::CreateDIBSection(m_dc, &info, DIB_RGB_COLORS, &m_bits, nullptr, 0);
        if (!m_bitmap) {
            reset();
            return false;
        }
        m_old = ::SelectObject(m_dc, m_bitmap);
        m_width = w;
        m_height = h;
        return true;
    }

    // Gives the memory back: a card that is not shown costs nothing.
    void reset()
    {
        if (m_dc) {
            if (m_old)
                ::SelectObject(m_dc, m_old);
            ::DeleteDC(m_dc);
        }
        if (m_bitmap)
            ::DeleteObject(m_bitmap);
        m_dc = nullptr;
        m_bitmap = nullptr;
        m_old = nullptr;
        m_bits = nullptr;
        m_width = m_height = 0;
    }

    HDC dc() const { return m_dc; }
    void* bits() const { return m_bits; }
    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_old = nullptr;
    void* m_bits = nullptr;
    int m_width = 0;
    int m_height = 0;
};

// The note, as a layered window.
void present(HWND window, const Surface& surface, POINT origin, BYTE alpha)
{
    SIZE size {surface.width(), surface.height()};
    POINT source {0, 0};
    BLENDFUNCTION blend {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    ::UpdateLayeredWindow(window, nullptr, &origin, &size, surface.dc(), &source, 0, &blend, ULW_ALPHA);
}

// Shown after the picture is on it, so that no frame of the last one (or of
// nothing) is seen; and above what came up since.
void raise(HWND window)
{
    ::SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// ---- drawing ----

Gdiplus::Color color(std::uint32_t argb)
{
    return Gdiplus::Color(static_cast<BYTE>(argb >> 24), static_cast<BYTE>(argb >> 16), static_cast<BYTE>(argb >> 8), static_cast<BYTE>(argb));
}

// `from` moved `t` of the way to `to`; the alpha stays.
std::uint32_t mix(std::uint32_t from, std::uint32_t to, double t)
{
    std::uint32_t out = from & 0xff000000u;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double a = static_cast<double>((from >> shift) & 0xffu);
        const double b = static_cast<double>((to >> shift) & 0xffu);
        out |= static_cast<std::uint32_t>(std::lround(a + (b - a) * t)) << shift;
    }
    return out;
}

bool bright(std::uint32_t argb)
{
    const double r = (argb >> 16) & 0xffu;
    const double g = (argb >> 8) & 0xffu;
    const double b = argb & 0xffu;
    return 0.2126 * r + 0.7152 * g + 0.0722 * b > 190;
}

// A canvas on a Surface: the GDI+ graphics ready. Text is anti-aliased to the
// pixel grid (ClearType cannot blend onto a bitmap that is transparent
// where there is no card).
class Canvas {
public:
    explicit Canvas(Surface& surface, bool clear = true)
        : m_bitmap(surface.width(), surface.height(), surface.width() * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(surface.bits()))
        , g(&m_bitmap)
    {
        if (clear)
            g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    }

private:
    Gdiplus::Bitmap m_bitmap;

public:
    Gdiplus::Graphics g;
};

bool hasCjk(const std::wstring& s)
{
    for (const wchar_t c : s) {
        if ((c >= 0x2E80 && c < 0xA000) || (c >= 0xAC00 && c < 0xD7B0) || (c >= 0xF900 && c < 0xFB00) || (c >= 0xFF00 && c < 0xFFF0))
            return true;
    }
    return false;
}

void trimEnd(std::wstring& s)
{
    while (!s.empty() && std::iswspace(s.back()))
        s.pop_back();
}

// "Microsoft YaHei UI" for Chinese (it has the Latin letters too) and
// "Segoe UI" otherwise, in whole pixels so that the hinting has a size to
// snap to; `strong` is the title's weight.
std::unique_ptr<Gdiplus::Font> makeFont(bool cjk, int pixels, bool strong)
{
    struct Pick {
        const wchar_t* name;
        int style;
    };
    const int bold = strong ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular;
    std::vector<Pick> picks;
    if (cjk) {
        picks = {{L"Microsoft YaHei UI", bold}, {L"Segoe UI", bold}};
    } else if (strong) {
        picks = {{L"Segoe UI Semibold", Gdiplus::FontStyleRegular}, {L"Segoe UI", Gdiplus::FontStyleBold}, {L"Microsoft YaHei UI", Gdiplus::FontStyleBold}};
    } else {
        picks = {{L"Segoe UI", Gdiplus::FontStyleRegular}, {L"Microsoft YaHei UI", Gdiplus::FontStyleRegular}};
    }
    const Gdiplus::REAL size = static_cast<Gdiplus::REAL>(std::max(pixels, 1));
    for (const Pick& pick : picks) {
        Gdiplus::FontFamily family(pick.name);
        if (family.IsAvailable() && family.IsStyleAvailable(pick.style))
            return std::make_unique<Gdiplus::Font>(&family, size, pick.style, Gdiplus::UnitPixel);
    }
    return std::make_unique<Gdiplus::Font>(Gdiplus::FontFamily::GenericSansSerif(), size, bold, Gdiplus::UnitPixel);
}

// Typographic: the width is the text's own, with no padding around it, so
// that a pill or a card can be fitted to it.
void setupFormat(Gdiplus::StringFormat& format, bool wrap, bool ellipsis, bool wholeLines)
{
    // wholeLines (measuring the first line of some text): a line is laid out
    // only if the rectangle has room for all of it. Drawing is not limited
    // that way: a rectangle as high as the measured line would show nothing.
    INT flags = format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces; // "左上角 · " is as wide as it reads
    if (wholeLines)
        flags |= Gdiplus::StringFormatFlagsLineLimit;
    else
        flags &= ~static_cast<INT>(Gdiplus::StringFormatFlagsLineLimit);
    if (!wrap)
        flags |= Gdiplus::StringFormatFlagsNoWrap;
    format.SetFormatFlags(flags);
    format.SetTrimming(ellipsis ? Gdiplus::StringTrimmingEllipsisCharacter : Gdiplus::StringTrimmingNone);
}

struct TextSize {
    float w = 0;
    float h = 0;
    int lines = 0;
    int fitted = 0; // characters
};

// Measures text the way it will be drawn: the same hint, the same format.
class Probe {
public:
    Probe() : m_bitmap(1, 1, PixelFormat32bppPARGB), m_graphics(&m_bitmap)
    {
        m_graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    }

    TextSize measure(const std::wstring& text, const Gdiplus::Font& font, float maxWidth, float maxHeight, bool wrap)
    {
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        setupFormat(format, wrap, false, true);
        // Empty text has no height of its own: that of a line.
        const std::wstring& measured = text.empty() ? m_a : text;
        Gdiplus::RectF box;
        INT fitted = 0;
        INT lines = 0;
        m_graphics.MeasureString(measured.c_str(), static_cast<INT>(measured.size()), &font, Gdiplus::RectF(0, 0, maxWidth, maxHeight), &format, &box,
            &fitted, &lines);
        TextSize size;
        size.w = text.empty() ? 0.f : box.Width;
        size.h = box.Height;
        size.lines = text.empty() ? 0 : static_cast<int>(lines);
        size.fitted = text.empty() ? 0 : static_cast<int>(fitted);
        return size;
    }

    int width(const std::wstring& text, const Gdiplus::Font& font)
    {
        return static_cast<int>(std::ceil(measure(text, font, kTall, kTall, false).w));
    }

private:
    Gdiplus::Bitmap m_bitmap;
    Gdiplus::Graphics m_graphics;
    const std::wstring m_a = L"A";
};

void drawText(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::Font& font, float x, float y, float w, float h, std::uint32_t argb,
    bool wrap, bool ellipsis)
{
    if (text.empty())
        return;
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    setupFormat(format, wrap, ellipsis, false);
    Gdiplus::SolidBrush brush(color(argb));
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, Gdiplus::RectF(x, y, w, h), &format, &brush);
}

// Where, from the top of a line of this font, the middle of the ink of a
// reference glyph is: a Chinese character, or a capital for the Latin
// letters. A label is put by that, not by its line box (which has room for
// accents above and for descenders below), so that it looks centred, and so
// that labels side by side share a baseline whatever they say.
float inkCentre(const Gdiplus::Font& font, bool cjk, float lineHeight)
{
    const std::wstring reference = cjk ? L"国" : L"H";
    const int pad = 4;
    const int w = static_cast<int>(std::ceil(font.GetSize() * 2)) + 2 * pad;
    const int h = static_cast<int>(std::ceil(lineHeight)) + 2 * pad;
    Gdiplus::Bitmap bitmap(w, h, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics g(&bitmap);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        drawText(g, reference, font, fl(pad), fl(pad), fl(w), fl(h), 0xffffffffu, false, false);
    }
    Gdiplus::BitmapData data;
    const Gdiplus::Rect all(0, 0, w, h);
    if (bitmap.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok)
        return lineHeight / 2;
    int first = -1;
    int last = -1;
    for (int y = 0; y < h; ++y) {
        const BYTE* row = static_cast<const BYTE*>(data.Scan0) + static_cast<std::ptrdiff_t>(y) * data.Stride;
        for (int x = 0; x < w; ++x) {
            if (row[4 * x + 3] >= 96) { // a row with real ink, not only the soft edge of a stroke
                if (first < 0)
                    first = y;
                last = y;
                break;
            }
        }
    }
    bitmap.UnlockBits(&data);
    if (first < 0)
        return lineHeight / 2;
    return fl(first + last + 1) / 2 - fl(pad);
}

void roundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r)
{
    r = std::min(r, std::min(w, h) / 2);
    if (r < 0.5f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    const float d = 2 * r;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void fillRound(Gdiplus::Graphics& g, const RECT& r, float radius, std::uint32_t argb)
{
    Gdiplus::GraphicsPath path;
    roundRect(path, fl(r.left), fl(r.top), fl(r.right - r.left), fl(r.bottom - r.top), radius);
    Gdiplus::SolidBrush brush(color(argb));
    g.FillPath(&brush, &path);
}

// A line one pixel wide (or more) just inside the rectangle, over what is
// there: the edge of a pill or of a plain button.
void outlineRound(Gdiplus::Graphics& g, const RECT& r, float radius, float width, std::uint32_t argb)
{
    Gdiplus::GraphicsPath path;
    const float h = width / 2;
    roundRect(path, fl(r.left) + h, fl(r.top) + h, fl(r.right - r.left) - width, fl(r.bottom - r.top) - width, std::max(0.f, radius - h));
    Gdiplus::Pen pen(color(argb), width);
    g.DrawPath(&pen, &path);
}

void fillRectangle(Gdiplus::Graphics& g, int x, int y, int w, int h, std::uint32_t argb)
{
    Gdiplus::SolidBrush brush(color(argb));
    g.FillRectangle(&brush, Gdiplus::RectF(fl(x), fl(y), fl(w), fl(h)));
}

// One line of a box blur: the sum of the 2r+1 around each, the ends
// counting as 0.
void boxBlur(const std::uint8_t* in, std::uint8_t* out, int count, int step, int radius)
{
    const int width = 2 * radius + 1;
    int sum = 0;
    for (int i = 0; i < radius && i < count; ++i)
        sum += in[i * step];
    for (int i = 0; i < count; ++i) {
        if (i + radius < count)
            sum += in[(i + radius) * step];
        out[i * step] = static_cast<std::uint8_t>(sum / width);
        if (i - radius >= 0)
            sum -= in[(i - radius) * step];
    }
}

// Three box blurs (each way) of the alpha: what a Gaussian is, near enough.
// The colour of the shadow is black, which premultiplied is 0 whatever the
// alpha: only that is blurred.
void blurAlpha(Surface& surface, int radius)
{
    const int w = surface.width();
    const int h = surface.height();
    BYTE* bits = static_cast<BYTE*>(surface.bits());
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<std::uint8_t> plane(count);
    std::vector<std::uint8_t> other(count);
    for (std::size_t i = 0; i < count; ++i)
        plane[i] = bits[4 * i + 3];
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y)
            boxBlur(plane.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w), other.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w), w, 1, radius);
        for (int x = 0; x < w; ++x)
            boxBlur(other.data() + x, plane.data() + x, h, w, radius);
    }
    for (std::size_t i = 0; i < count; ++i)
        bits[4 * i + 3] = plane[i];
}

// A soft shadow under a rounded rectangle: its shape in black, a little
// lower, blurred (light: 16% black at 0, 2, 8; dark: 40%). Clears the
// surface; what is drawn after it is over the shadow.
void paintShadow(Surface& surface, const RECT& body, float radius, double scale, bool dark)
{
    {
        Canvas canvas(surface, true);
        RECT lower = body;
        ::OffsetRect(&lower, 0, px(2, scale));
        fillRound(canvas.g, lower, radius, static_cast<std::uint32_t>(dark ? 102 : 41) << 24);
    }
    blurAlpha(surface, std::max(1, static_cast<int>(std::lround(4 * scale - 0.5))));
}

// ---- icons ----

// The code points of src/gestures-plugin/icons/PhosphorGestures.ttf
// (icons/make-subset.py lists them), Phosphor Regular.
enum class Icon : wchar_t {
    Power = 0xE3DA,
    Restart = 0xE036, // arrow-clockwise
    SignOut = 0xE42A,
    Moon = 0xE330,
    Translate = 0xE4A2,
    Check = 0xE182,
    Info = 0xE2CE,
    Up = 0xE08E,
    Down = 0xE03E,
    Left = 0xE058,
    Right = 0xE06C,
    UpLeft = 0xE090,
    UpRight = 0xE092,
    DownLeft = 0xE040,
    DownRight = 0xE042,
    Cursor = 0xE1DC,
};

// The icon font, from the exe's resource into a private collection of GDI+
// for as long as this object lives (a drawing).
class Icons {
public:
    Icons()
    {
        const HRSRC resource = ::FindResourceW(nullptr, kIconResource, MAKEINTRESOURCEW(10)); // RT_RCDATA
        const HGLOBAL loaded = resource ? ::LoadResource(nullptr, resource) : nullptr;
        const void* data = loaded ? ::LockResource(loaded) : nullptr;
        const DWORD size = resource ? ::SizeofResource(nullptr, resource) : 0;
        if (!data || size == 0)
            return;
        if (m_fonts.AddMemoryFont(data, static_cast<INT>(size)) != Gdiplus::Ok)
            return;
        INT found = 0;
        m_fonts.GetFamilies(1, &m_family, &found);
        m_ok = found == 1 && m_family.IsAvailable();
    }

    // `icon` with the middle of its ink at (cx, cy), its em `size` pixels:
    // by the ink, not by the em box, which has room that the icons do not
    // fill alike; the origin is whole pixels.
    void draw(Gdiplus::Graphics& g, Icon icon, int size, float cx, float cy, std::uint32_t argb) const
    {
        if (!m_ok)
            return;
        const wchar_t glyph = static_cast<wchar_t>(icon);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        Gdiplus::GraphicsPath path;
        path.AddString(&glyph, 1, &m_family, Gdiplus::FontStyleRegular, static_cast<Gdiplus::REAL>(size), Gdiplus::PointF(0, 0), &format);
        Gdiplus::RectF box;
        path.GetBounds(&box);
        const float x = std::round(cx - (box.X + box.Width / 2));
        const float y = std::round(cy - (box.Y + box.Height / 2));
        const Gdiplus::Font font(&m_family, static_cast<Gdiplus::REAL>(size), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        const Gdiplus::SolidBrush brush(color(argb));
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias); // the icons are shapes, not text to hint
        g.DrawString(&glyph, 1, &font, Gdiplus::PointF(x, y), &format, &brush);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    }

private:
    Gdiplus::PrivateFontCollection m_fonts;
    Gdiplus::FontFamily m_family;
    bool m_ok = false;
};

// ---- what the caller says ----

// The public icons are what the caller means; these are the glyphs.
Icon iconOf(ConfirmIcon icon)
{
    switch (icon) {
    case ConfirmIcon::Restart: return Icon::Restart;
    case ConfirmIcon::SignOut: return Icon::SignOut;
    case ConfirmIcon::Sleep: return Icon::Moon;
    case ConfirmIcon::Power: break;
    }
    return Icon::Power;
}

Icon iconOf(NoteIcon icon)
{
    switch (icon) {
    case NoteIcon::Info: return Icon::Info;
    case NoteIcon::UpLeft: return Icon::UpLeft;
    case NoteIcon::UpRight: return Icon::UpRight;
    case NoteIcon::DownLeft: return Icon::DownLeft;
    case NoteIcon::DownRight: return Icon::DownRight;
    case NoteIcon::Up: return Icon::Up;
    case NoteIcon::Down: return Icon::Down;
    case NoteIcon::Left: return Icon::Left;
    case NoteIcon::Right: return Icon::Right;
    case NoteIcon::Cursor: return Icon::Cursor;
    case NoteIcon::Check: break;
    }
    return Icon::Check;
}

// What a bubble is showing: waiting for the translation (until update() says
// more), or one of the states update() gives it.
enum class BubbleKind { Waiting, Result, NoService, NoPlugin, Failed };

BubbleKind kindOf(BubbleState state)
{
    switch (state) {
    case BubbleState::NoService: return BubbleKind::NoService;
    case BubbleState::NoPlugin: return BubbleKind::NoPlugin;
    case BubbleState::Failed: return BubbleKind::Failed;
    case BubbleState::Result: break;
    }
    return BubbleKind::Result;
}

// A message, not a translation: the information icon before it.
bool isMessage(BubbleKind kind)
{
    return kind == BubbleKind::NoService || kind == BubbleKind::NoPlugin || kind == BubbleKind::Failed;
}

// The text as lines at most `width` wide, broken the way GDI+ breaks them
// (at words; Chinese anywhere) but placed by hand, so that the lines can be as
// far apart as the design wants; at most `maxLines`, the last one ending in
// an ellipsis where there was more.
std::vector<std::wstring> wrapLines(Probe& probe, std::wstring text, const Gdiplus::Font& font, float width, int maxLines)
{
    trimEnd(text);
    std::vector<std::wstring> lines;
    const float lineH = probe.measure(L"A", font, kTall, kTall, false).h;
    const std::size_t most = static_cast<std::size_t>(maxLines);
    bool more = false;
    std::size_t start = 0;
    while (!more) {
        const std::size_t end = text.find(L'\n', start);
        std::wstring paragraph = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!paragraph.empty() && paragraph.back() == L'\r')
            paragraph.pop_back();
        if (paragraph.empty()) {
            if (lines.size() == most)
                more = true;
            else
                lines.emplace_back();
        }
        std::size_t pos = 0;
        while (pos < paragraph.size() && !more) {
            if (lines.size() == most) {
                more = true;
                break;
            }
            const std::wstring rest = paragraph.substr(pos);
            const TextSize fit = probe.measure(rest, font, width, lineH * 1.5f, true); // the first line of it
            const std::size_t n = static_cast<std::size_t>(std::clamp(fit.fitted, 1, static_cast<int>(rest.size())));
            lines.push_back(rest.substr(0, n));
            pos += n;
        }
        if (end == std::wstring::npos)
            break;
        start = end + 1;
    }
    for (std::wstring& line : lines)
        trimEnd(line);
    if (more && !lines.empty()) {
        std::wstring tail = lines.back();
        for (int i = 0; i < 80; ++i) {
            const std::wstring candidate = tail + L"…";
            if (probe.measure(candidate, font, kTall, kTall, false).w <= width || tail.empty()) {
                lines.back() = candidate;
                break;
            }
            tail.pop_back();
            trimEnd(tail);
        }
    }
    return lines;
}

// ---- the frame ----

// DWM's own corners, border and shadow for a window that has no
// non-client area, in the look's corners and light or dark. Returns true
// when DWM accepts DWMWA_BORDER_COLOR (Windows 11); Windows 10 draws no
// border here, so drawCard/drawBubble draw a square 1 px edge instead.
bool styleFrame(HWND window, const Look& look)
{
    const int corner = look.rounded ? kCornerRound : kCornerSquare;
    ::DwmSetWindowAttribute(window, kDwmCorner, &corner, sizeof corner);
    const bool nativeBorder = SUCCEEDED(::DwmSetWindowAttribute(window, kDwmBorder, &kColorDefault, sizeof kColorDefault));
    const BOOL dark = look.dark ? TRUE : FALSE;
    if (FAILED(::DwmSetWindowAttribute(window, kDwmDarkMode, &dark, sizeof dark)))
        ::DwmSetWindowAttribute(window, kDwmDarkModeOld, &dark, sizeof dark);
    // One pixel of the frame extended: what DWM wants for the shadow.
    const MARGINS margins {0, 0, 1, 0};
    ::DwmExtendFrameIntoClientArea(window, &margins);
    return nativeBorder;
}

void drawWindowEdge(Gdiplus::Graphics& g, int width, int height, double scale, std::uint32_t argb)
{
    const int edge = std::max(1, static_cast<int>(std::lround(scale)));
    fillRectangle(g, 0, 0, width, edge, argb);
    fillRectangle(g, 0, height - edge, width, edge, argb);
    fillRectangle(g, 0, 0, edge, height, argb);
    fillRectangle(g, width - edge, 0, edge, height, argb);
}

void blit(HDC dc, const Surface& surface)
{
    if (dc && surface.dc())
        ::BitBlt(dc, 0, 0, surface.width(), surface.height(), surface.dc(), 0, 0, SRCCOPY);
}

void blitWindow(HWND window, const Surface& surface)
{
    if (HDC dc = ::GetDC(window)) {
        blit(dc, surface);
        ::ReleaseDC(window, dc);
    }
}

// Where the window is and how big, and shown (never activated, topmost).
void placeFrame(HWND window, POINT origin, SIZE size)
{
    ::SetWindowPos(window, HWND_TOPMOST, origin.x, origin.y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// What the card and the bubble share: no non-client area (the whole window
// is the client's, DWM draws the frame around it), the picture from the
// DIB, no activation.
bool frameMessage(HWND window, UINT message, WPARAM w, const Surface& surface, LRESULT& result)
{
    switch (message) {
    case WM_NCCALCSIZE:
        result = 0;
        return true;
    case WM_NCHITTEST:
        result = HTCLIENT;
        return true;
    case WM_NCACTIVATE:
        result = ::DefWindowProcW(window, message, w, -1); // no caption to repaint
        return true;
    case WM_ERASEBKGND:
        result = 1;
        return true;
    case WM_PAINT: {
        PAINTSTRUCT paint {};
        if (HDC dc = ::BeginPaint(window, &paint)) {
            blit(dc, surface);
            ::EndPaint(window, &paint);
        }
        result = 0;
        return true;
    }
    case WM_PRINTCLIENT:
        blit(reinterpret_cast<HDC>(w), surface);
        result = 0;
        return true;
    case WM_MOUSEACTIVATE:
        result = MA_NOACTIVATE; // a click never takes the focus from the program underneath
        return true;
    case WM_DPICHANGED:
        result = 0; // its size is set whenever it is put on a screen
        return true;
    default:
        return false;
    }
}

// The keys a card holds while it is up: Esc, and Enter on the confirm card.
struct Keys {
    bool esc = false;
    bool enter = false;
    int tries = 0;
};

// True when all it wants are held. A key somebody else holds is not an
// error: the card still works by the mouse.
bool grabKeys(HWND window, Keys& keys, bool wantEnter)
{
    if (!keys.esc)
        keys.esc = ::RegisterHotKey(window, kEscId, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
    if (wantEnter && !keys.enter)
        keys.enter = ::RegisterHotKey(window, kEnterId, MOD_NOREPEAT, VK_RETURN) != FALSE;
    return keys.esc && (keys.enter || !wantEnter);
}

void dropKeys(HWND window, Keys& keys)
{
    if (window) {
        if (keys.esc)
            ::UnregisterHotKey(window, kEscId);
        if (keys.enter)
            ::UnregisterHotKey(window, kEnterId);
    }
    keys = {};
}

} // namespace

// ---- the state ----

struct Cards::Impl {
    enum Button { kNoButton = 0, kCancelButton, kNowButton };

    struct Note {
        HWND window = nullptr;
        Surface surface;
        ULONGLONG fadeStart = 0;
        bool shown = false;
    };

    struct Card {
        HWND window = nullptr;
        bool nativeBorder = true;
        Surface surface;
        bool active = false;
        std::wstring title;
        std::wstring now;
        Icon icon = Icon::Power;
        bool cjk = true; // the words are Chinese: their font
        int seconds = 0; // it began with
        ULONGLONG endsAt = 0; // GetTickCount64
        Screen screen;
        POINT origin {}; // of the window, on the screen
        SIZE size {};
        // From here on in the window's own pixels.
        int bodyH = 0; // under it the footer
        POINT titleAt {};
        POINT lineAt {};
        RECT cancel {};
        RECT nowButton {};
        POINT cancelText {};
        POINT nowText {};
        Button hover = kNoButton;
        Button pressed = kNoButton;
        Keys keys;
        std::function<void(bool)> done;
    };

    struct Bubble {
        HWND window = nullptr;
        bool nativeBorder = true;
        Surface surface;
        bool active = false;
        unsigned generation = 0; // which bubble: a click may open another
        POINT anchor {};
        std::wstring text;
        BubbleKind kind = BubbleKind::Result;
        std::vector<std::wstring> lines; // as drawn
        std::wstring header;
        std::wstring hint; // the right of the header: what a click does
        bool cjk = true;
        Screen screen;
        POINT origin {};
        SIZE size {};
        int textTop = 0;
        int lineStep = 0;
        int indent = 0; // the icon of a message, before its lines
        int waitWidth = 0; // "正在翻译", before the dots
        int dotsY = 0;
        ULONGLONG dotsStart = 0;
        bool hover = false;
        bool pressed = false;
        Keys keys;
        std::function<void()> clicked;
    };

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() { destroy(); }

    bool create();
    void destroy();
    void relook();

    void flash(POINT at, NoteIcon icon, const std::wstring& lead, const std::wstring& text, int ms);
    void hideNote();
    void fadeStep();
    LRESULT onNote(HWND window, UINT message, WPARAM w, LPARAM l);

    void confirm(POINT at, ConfirmIcon icon, const std::wstring& title, const std::wstring& now, int seconds, std::function<void(bool)> done);
    void layoutCard();
    void drawCard();
    void endConfirm(bool result);
    Button hitCard(POINT p) const;
    void setCardHover(Button button);
    LRESULT onCard(HWND window, UINT message, WPARAM w, LPARAM l);

    void openBubble(POINT at, std::function<void()> clicked);
    void updateBubble(BubbleState state, const std::wstring& text);
    void closeBubble();
    void layoutBubble();
    void drawBubble();
    void setBubbleHover(bool hover);
    void keepDots();
    LRESULT onBubble(HWND window, UINT message, WPARAM w, LPARAM l);

    // One window procedure per kind, all of them this.
    template <LRESULT (Impl::*Handler)(HWND, UINT, WPARAM, LPARAM)>
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        if (message == WM_NCCREATE)
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        Impl* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self) {
            try {
                return (self->*Handler)(window, message, w, l);
            } catch (...) {
                // Nothing may be thrown into the system's message dispatch.
            }
        }
        return ::DefWindowProcW(window, message, w, l);
    }

    Look look;
    bool created = false;
    HINSTANCE instance = nullptr;
    Note note;
    Card card;
    Bubble bubble;
};

// ---- the windows ----

bool Cards::Impl::create()
{
    if (created)
        return true;
    instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc {sizeof wc};
    wc.hInstance = instance;
    wc.hCursor = cursor(false);
    wc.lpszClassName = kNoteClass;
    wc.lpfnWndProc = &Impl::proc<&Impl::onNote>;
    ::RegisterClassExW(&wc);
    wc.lpszClassName = kCardClass;
    wc.lpfnWndProc = &Impl::proc<&Impl::onCard>;
    ::RegisterClassExW(&wc);
    wc.lpszClassName = kBubbleClass;
    wc.lpfnWndProc = &Impl::proc<&Impl::onBubble>;
    ::RegisterClassExW(&wc);

    const DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    note.window = ::CreateWindowExW(ex | WS_EX_LAYERED | WS_EX_TRANSPARENT, kNoteClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    // WS_CAPTION, with the client area over all of it (WM_NCCALCSIZE): what
    // DWM needs to give the window its shadow and corners.
    card.window = ::CreateWindowExW(ex, kCardClass, L"", WS_POPUP | WS_CAPTION, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    bubble.window = ::CreateWindowExW(ex, kBubbleClass, L"", WS_POPUP | WS_CAPTION, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    created = note.window && card.window && bubble.window;
    if (created) {
        card.nativeBorder = styleFrame(card.window, look);
        bubble.nativeBorder = styleFrame(bubble.window, look);
    } else {
        destroy();
    }
    return created;
}

void Cards::Impl::destroy()
{
    // What waits for an answer is dropped, not answered: its owner may be
    // gone (see the top of this file).
    card.active = false;
    card.done = nullptr;
    bubble.active = false;
    bubble.clicked = nullptr;
    created = false;
    // The windows take their timers and hotkeys with them. A window that
    // cannot be destroyed from here (this is not its thread) is told to
    // close itself, and no longer finds this object.
    for (HWND* window : {&note.window, &card.window, &bubble.window}) {
        if (*window && ::IsWindow(*window)) {
            ::SetWindowLongPtrW(*window, GWLP_USERDATA, 0);
            if (!::DestroyWindow(*window))
                ::PostMessageW(*window, WM_CLOSE, 0, 0);
        }
        *window = nullptr;
    }
    if (instance) {
        ::UnregisterClassW(kNoteClass, instance);
        ::UnregisterClassW(kCardClass, instance);
        ::UnregisterClassW(kBubbleClass, instance);
    }
    note.surface.reset();
    card.surface.reset();
    bubble.surface.reset();
    card.keys = {};
    bubble.keys = {};
    note.shown = false;
    card.hover = card.pressed = kNoButton;
    bubble.hover = bubble.pressed = false;
}

// The look changed while a card was up: it is drawn again in the new one.
void Cards::Impl::relook()
{
    if (!created)
        return;
    card.nativeBorder = styleFrame(card.window, look);
    bubble.nativeBorder = styleFrame(bubble.window, look);
    if (card.active) {
        layoutCard();
        drawCard();
        placeFrame(card.window, card.origin, card.size);
        blitWindow(card.window, card.surface);
    }
    if (bubble.active) {
        layoutBubble();
        drawBubble();
        placeFrame(bubble.window, bubble.origin, bubble.size);
        blitWindow(bubble.window, bubble.surface);
    }
}

// ---- the note ----

void Cards::Impl::hideNote()
{
    if (!note.window)
        return;
    ::KillTimer(note.window, kHideTimer);
    ::KillTimer(note.window, kFadeTimer);
    if (note.shown) {
        ::ShowWindow(note.window, SW_HIDE);
        note.shown = false;
    }
    note.surface.reset();
}

void Cards::Impl::flash(POINT at, NoteIcon noteIcon, const std::wstring& leadText, const std::wstring& text, int ms)
{
    if (!created)
        return;
    // A new one replaces the last in place (no hide and show between, which
    // would flicker): only its timers are stopped here.
    ::KillTimer(note.window, kHideTimer);
    ::KillTimer(note.window, kFadeTimer);
    const auto oneLine = [](std::wstring s) {
        std::replace_if(s.begin(), s.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }, L' ');
        return s;
    };
    // The words in two colours: `lead` (the trigger, quietly), then `main`.
    struct {
        Icon icon;
        std::wstring lead;
        std::wstring main;
    } spec {iconOf(noteIcon), oneLine(leadText), oneLine(text)};
    if (spec.lead.empty() && spec.main.empty()) {
        hideNote();
        return;
    }

    const Screen screen = screenAt(at);
    const double s = screen.scale;
    const bool cjk = look.chinese || hasCjk(spec.lead) || hasCjk(spec.main);
    const auto font = makeFont(cjk, px(13, s), false);
    const auto strong = makeFont(cjk, px(13, s), true);
    const int height = px(32, s);
    const int padX = px(12, s);
    const int iconSize = px(16, s);
    const int gap = px(8, s);
    const int margin = px(16, s); // around the pill, for its shadow
    const int offset = px(18, s);
    const int widest = std::max(px(80, s), static_cast<int>(screen.work.right - screen.work.left) - 2 * offset - 2 * padX - iconSize - gap);

    Probe probe;
    const int leadW = probe.width(spec.lead, *font);
    const bool plain = spec.lead.empty();
    const int mainW = probe.width(spec.main, plain ? *font : *strong);
    const bool trim = leadW + mainW > widest; // longer than the screen: an ellipsis
    const int mainShown = trim ? std::max(0, widest - leadW) : mainW;
    const int width = padX + iconSize + gap + leadW + mainShown + padX;
    const RECT spot = place(at, width, height, offset, screen.work);
    if (!note.surface.ensure(width + 2 * margin, height + 2 * margin)) {
        hideNote(); // not the last one, left up with no timer
        return;
    }
    const RECT body {margin, margin, margin + width, margin + height};
    // Like the trail's hint: a pill, or with the square corners setting a
    // control's own corner.
    const float radius = look.rounded ? fl(height) / 2 : fl(px(look.controlRadius, s));
    paintShadow(note.surface, body, radius, s, look.dark);
    {
        Canvas canvas(note.surface, false);
        Gdiplus::Graphics& g = canvas.g;
        fillRound(g, body, radius, look.background);
        outlineRound(g, body, radius, fl(std::max(1.0, std::floor(s))), look.edge);
        const Icons icons;
        icons.draw(g, spec.icon, iconSize, fl(body.left + padX) + fl(iconSize) / 2, fl(body.top) + fl(height) / 2, look.accent);
        const TextSize lineSize = probe.measure(spec.main.empty() ? spec.lead : spec.main, *font, kTall, kTall, false);
        const float top = fl(body.top) + std::round(fl(height) / 2 - inkCentre(*font, cjk, lineSize.h));
        float x = fl(body.left + padX + iconSize + gap);
        drawText(g, spec.lead, *font, x, top, fl(leadW) + 2, lineSize.h, look.subtext, false, false);
        x += fl(leadW);
        drawText(g, spec.main, plain ? *font : *strong, x, top, fl(mainShown) + (trim ? 0.f : 2.f), lineSize.h, look.text, false, trim);
    }
    present(note.window, note.surface, POINT {spot.left - margin, spot.top - margin}, 255);
    raise(note.window);
    note.shown = true;
    ::SetTimer(note.window, kHideTimer, static_cast<UINT>(std::max(ms, 1)), nullptr);
}

void Cards::Impl::fadeStep()
{
    const ULONGLONG gone = ::GetTickCount64() - note.fadeStart;
    if (gone >= kFadeMs) {
        hideNote();
        return;
    }
    const BYTE alpha = static_cast<BYTE>(255 - 255 * gone / kFadeMs);
    BLENDFUNCTION blend {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    ::UpdateLayeredWindow(note.window, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
}

LRESULT Cards::Impl::onNote(HWND window, UINT message, WPARAM w, LPARAM l)
{
    switch (message) {
    case WM_TIMER:
        if (w == kHideTimer) {
            ::KillTimer(window, kHideTimer);
            note.fadeStart = ::GetTickCount64();
            ::SetTimer(window, kFadeTimer, kFadeFrameMs, nullptr);
        } else if (w == kFadeTimer) {
            fadeStep();
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_DPICHANGED:
        return 0; // its size is set whenever it is put on a screen
    default:
        return ::DefWindowProcW(window, message, w, l);
    }
}

// ---- the confirm card ----

void Cards::Impl::confirm(POINT at, ConfirmIcon icon, const std::wstring& title, const std::wstring& now, int seconds, std::function<void(bool)> done)
{
    // Asked before this one is up: the bubble gives its Esc to this, and a
    // card still waiting is answered with no. What is called back may ask
    // again, hence the few rounds.
    closeBubble();
    for (int i = 0; card.active && i < 4; ++i)
        endConfirm(false);
    if (!created) {
        if (done)
            done(false);
        return;
    }
    card.active = true;
    card.title = title;
    card.now = now;
    card.icon = iconOf(icon);
    card.seconds = std::max(seconds, 1); // a card is always seen, if for a second
    card.endsAt = ::GetTickCount64() + static_cast<ULONGLONG>(card.seconds) * 1000;
    card.hover = card.pressed = kNoButton;
    card.done = std::move(done);
    card.screen = screenAt(at);
    layoutCard();
    card.nativeBorder = styleFrame(card.window, look);
    drawCard();
    if (!card.surface.width()) { // no bitmap: nothing to ask with, so not done
        endConfirm(false);
        return;
    }
    placeFrame(card.window, card.origin, card.size);
    blitWindow(card.window, card.surface);
    if (!grabKeys(card.window, card.keys, true))
        ::SetTimer(card.window, kKeysTimer, kKeysRetryMs, nullptr);
    ::SetTimer(card.window, kRingTimer, kRingMs, nullptr);
}

namespace {

// The second line of the card, as it reads when `seconds` are left.
std::wstring countLine(const std::wstring& title, int seconds, bool zh)
{
    const std::wstring n = std::to_wstring(seconds);
    return zh ? n + L" 秒后" + title + L" · 按 Esc 取消" : title + L" in " + n + L" s · Esc to cancel";
}

} // namespace

// Where everything goes: from the fonts, the texts, the screen's scale. A
// Windows 11 dialog: the body (a badge with its ring, the title and what is
// going to happen), under it the footer band with the buttons.
void Cards::Impl::layoutCard()
{
    const double s = card.screen.scale;
    const RECT& work = card.screen.work;
    const bool zh = look.chinese;
    card.cjk = zh || hasCjk(card.title) || hasCjk(card.now);
    const std::wstring cancelText = zh ? L"取消" : L"Cancel";

    const auto titleFont = makeFont(card.cjk, px(16, s), true);
    const auto textFont = makeFont(card.cjk, px(13, s), false);
    Probe probe;
    const TextSize title = probe.measure(card.title, *titleFont, kTall, kTall, false);
    const TextSize line = probe.measure(countLine(card.title, card.seconds, zh), *textFont, kTall, kTall, false); // with all its digits
    const int cancelW = probe.width(cancelText, *textFont);
    const int nowW = probe.width(card.now, *textFont);

    const int pad = px(20, s);
    const int badgeBox = px(44, s); // the ring's outer size
    const int textX = pad + badgeBox + px(16, s);
    const int buttonH = px(32, s);
    const int buttonPad = px(16, s);
    const int buttonGap = px(8, s);
    const int buttonW = std::max(px(96, s), std::max(cancelW, nowW) + 2 * buttonPad); // both alike
    const int footerPad = px(16, s);
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;
    int width = std::max(px(380, s), textX + static_cast<int>(std::ceil(std::max(title.w, line.w))) + pad);
    width = std::max(width, 2 * pad + 2 * buttonW + buttonGap);
    width = std::min(width, workW - 2 * px(16, s));
    card.bodyH = pad + badgeBox + pad;
    const int height = card.bodyH + footerPad + buttonH + footerPad;

    card.origin = {work.left + (workW - width) / 2, work.top + (workH - height) / 2};
    card.size = {width, height};
    const int titleH = static_cast<int>(std::ceil(title.h));
    const int lineH = static_cast<int>(std::ceil(line.h));
    const int block = titleH + px(4, s) + lineH;
    const int top = pad + (badgeBox - block) / 2; // the two lines, in the middle of the badge's height
    card.titleAt = {textX, top};
    card.lineAt = {textX, top + titleH + px(4, s)};

    const int buttonTop = card.bodyH + footerPad;
    card.nowButton = {width - pad - buttonW, buttonTop, width - pad, buttonTop + buttonH};
    card.cancel = {card.nowButton.left - buttonGap - buttonW, buttonTop, card.nowButton.left - buttonGap, buttonTop + buttonH};
    const float centre = inkCentre(*textFont, card.cjk, fl(std::ceil(probe.measure(cancelText, *textFont, kTall, kTall, false).h)));
    const int labelY = buttonTop + static_cast<int>(std::lround(fl(buttonH) / 2 - centre));
    card.cancelText = {card.cancel.left + (buttonW - cancelW) / 2, labelY};
    card.nowText = {card.nowButton.left + (buttonW - nowW) / 2, labelY};
}

void Cards::Impl::drawCard()
{
    if (!card.surface.ensure(card.size.cx, card.size.cy))
        return;
    const double s = card.screen.scale;
    const bool zh = look.chinese;
    const std::wstring cancelText = zh ? L"取消" : L"Cancel";
    const auto titleFont = makeFont(card.cjk, px(16, s), true);
    const auto textFont = makeFont(card.cjk, px(13, s), false);
    const Icons icons;

    // The time left is read at the draw, so that the ring and the count are
    // always the same moment.
    const ULONGLONG now = ::GetTickCount64();
    const LONGLONG left = card.endsAt > now ? static_cast<LONGLONG>(card.endsAt - now) : 0;
    const int count = static_cast<int>((left + 999) / 1000);
    const double fraction = std::clamp(static_cast<double>(left) / (card.seconds * 1000.0), 0.0, 1.0);
    const int width = card.size.cx;
    const int height = card.size.cy;
    {
        Canvas canvas(card.surface);
        Gdiplus::Graphics& g = canvas.g;
        const float line = fl(std::max(1.0, std::floor(s)));
        // The body in the window's background, the footer a layer apart:
        // a little darker (a little lighter in the dark) and a line on top.
        fillRectangle(g, 0, 0, width, height, look.background);
        fillRectangle(g, 0, card.bodyH, width, height - card.bodyH, look.dark ? mix(look.background, 0xffffffffu, 0.04) : mix(look.background, 0xff000000u, 0.03));
        fillRectangle(g, 0, card.bodyH, width, static_cast<int>(line), look.edge);

        // The badge and its ring: the ring is the time that is left.
        const int pad = px(20, s);
        const float box = fl(px(44, s));
        const float cx = fl(pad) + box / 2;
        const float cy = fl(pad) + box / 2;
        const float stroke = fl(2.5 * s);
        const float ringR = box / 2 - stroke / 2;
        {
            Gdiplus::Pen track(color(look.controlBorder), stroke);
            g.DrawEllipse(&track, Gdiplus::RectF(cx - ringR, cy - ringR, 2 * ringR, 2 * ringR));
            if (fraction > 0) {
                Gdiplus::Pen arc(color(look.accent), stroke);
                arc.SetStartCap(Gdiplus::LineCapRound);
                arc.SetEndCap(Gdiplus::LineCapRound);
                g.DrawArc(&arc, Gdiplus::RectF(cx - ringR, cy - ringR, 2 * ringR, 2 * ringR), -90.f, fl(std::min(fraction, 0.9999) * 360));
            }
        }
        const float badgeR = fl(px(36, s)) / 2;
        Gdiplus::SolidBrush badge(color(mix(look.background, look.accent, 0.12)));
        g.FillEllipse(&badge, Gdiplus::RectF(cx - badgeR, cy - badgeR, 2 * badgeR, 2 * badgeR));
        icons.draw(g, card.icon, px(20, s), cx, cy, look.accent);

        drawText(g, card.title, *titleFont, fl(card.titleAt.x), fl(card.titleAt.y), fl(width - card.titleAt.x), fl(px(24, s)), look.text, false, false);
        drawText(g, countLine(card.title, count, zh), *textFont, fl(card.lineAt.x), fl(card.lineAt.y), fl(width - card.lineAt.x), fl(px(20, s)), look.subtext, false, false);

        // The buttons: the only things on the card that answer the pointer.
        const float radius = look.rounded ? fl(px(look.controlRadius, s)) : 0.f;
        const auto button = [&](const RECT& r, const POINT& label, const std::wstring& text, Button which, bool primary) {
            const bool hot = card.hover == which;
            const bool down = card.pressed == which && hot;
            std::uint32_t fill;
            std::uint32_t ink;
            if (primary) {
                const std::uint32_t toward = bright(look.accentFill) ? 0xff000000u : 0xffffffffu;
                fill = down ? mix(look.accentFill, 0xff000000u, 0.14) : hot ? mix(look.accentFill, toward, 0.12) : look.accentFill;
                ink = look.onAccent;
            } else {
                fill = down ? mix(look.controlHover, look.dark ? 0xffffffffu : 0xff000000u, 0.05) : hot ? look.controlHover : look.control;
                ink = look.text;
            }
            fillRound(g, r, radius, fill);
            if (!primary)
                outlineRound(g, r, radius, line, look.controlBorder);
            drawText(g, text, *textFont, fl(label.x), fl(label.y), fl(r.right - label.x), fl(px(20, s)), ink, false, false);
        };
        button(card.cancel, card.cancelText, cancelText, kCancelButton, false);
        button(card.nowButton, card.nowText, card.now, kNowButton, true);
        if (!card.nativeBorder)
            drawWindowEdge(g, width, height, s, look.edge);
    }
}

// The card is hidden, the keys and the timers given back, the state
// cleared: only then is the answer given, once.
void Cards::Impl::endConfirm(bool result)
{
    if (!card.active)
        return;
    card.active = false;
    if (card.window) {
        ::KillTimer(card.window, kRingTimer);
        ::KillTimer(card.window, kKeysTimer);
        if (::GetCapture() == card.window)
            ::ReleaseCapture();
        ::ShowWindow(card.window, SW_HIDE);
    }
    dropKeys(card.window, card.keys);
    card.surface.reset();
    card.hover = card.pressed = kNoButton;
    std::function<void(bool)> done = std::move(card.done);
    card.done = nullptr;
    if (done) {
        try {
            done(result);
        } catch (...) {
            // The owner's trouble: not thrown through the message loop.
        }
    }
}

Cards::Impl::Button Cards::Impl::hitCard(POINT p) const
{
    if (::PtInRect(&card.nowButton, p))
        return kNowButton;
    if (::PtInRect(&card.cancel, p))
        return kCancelButton;
    return kNoButton;
}

void Cards::Impl::setCardHover(Button button)
{
    if (card.hover == button)
        return;
    card.hover = button;
    if (card.active) {
        drawCard();
        blitWindow(card.window, card.surface);
    }
}

LRESULT Cards::Impl::onCard(HWND window, UINT message, WPARAM w, LPARAM l)
{
    LRESULT result = 0;
    if (frameMessage(window, message, w, card.surface, result))
        return result;
    switch (message) {
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT p {};
            ::GetCursorPos(&p);
            ::ScreenToClient(window, &p);
            ::SetCursor(cursor(card.active && hitCard(p) != kNoButton));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track {sizeof track, TME_LEAVE, window, 0};
        ::TrackMouseEvent(&track);
        setCardHover(card.active ? hitCard(pointOf(l)) : kNoButton);
        return 0;
    }
    case WM_MOUSELEAVE:
        setCardHover(kNoButton);
        return 0;
    case WM_LBUTTONDOWN:
        if (card.active) {
            card.pressed = hitCard(pointOf(l));
            if (card.pressed != kNoButton) {
                ::SetCapture(window);
                drawCard();
                blitWindow(window, card.surface);
            }
        }
        return 0;
    case WM_LBUTTONUP: {
        const Button was = card.pressed;
        card.pressed = kNoButton;
        if (::GetCapture() == window)
            ::ReleaseCapture();
        if (card.active) {
            if (was != kNoButton && hitCard(pointOf(l)) == was) {
                endConfirm(was == kNowButton);
            } else {
                drawCard();
                blitWindow(window, card.surface);
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        card.pressed = kNoButton;
        return 0;
    case WM_HOTKEY:
        if (card.active) {
            if (w == kEscId)
                endConfirm(false);
            else if (w == kEnterId)
                endConfirm(true);
        }
        return 0;
    case WM_TIMER:
        if (w == kRingTimer) {
            if (card.active) {
                if (::GetTickCount64() >= card.endsAt) {
                    endConfirm(true);
                } else {
                    drawCard();
                    blitWindow(window, card.surface);
                }
            }
        } else if (w == kKeysTimer) {
            if (!card.active || grabKeys(window, card.keys, true) || ++card.keys.tries >= kKeysTries)
                ::KillTimer(window, kKeysTimer);
        }
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(window, message, w, l);
}

// ---- the bubble ----

void Cards::Impl::openBubble(POINT at, std::function<void()> clicked)
{
    if (!created)
        return;
    // A new one replaces the last, which is not told; the window and its
    // keys stay (no hide and show between, which would flicker).
    const bool replaced = bubble.active;
    bubble.active = true;
    ++bubble.generation;
    bubble.anchor = at;
    bubble.kind = BubbleKind::Waiting;
    bubble.text.clear();
    bubble.clicked = std::move(clicked);
    bubble.hover = bubble.pressed = false;
    bubble.screen = screenAt(at);
    layoutBubble();
    bubble.nativeBorder = styleFrame(bubble.window, look);
    drawBubble();
    if (!bubble.surface.width()) {
        closeBubble();
        return;
    }
    placeFrame(bubble.window, bubble.origin, bubble.size);
    blitWindow(bubble.window, bubble.surface);
    keepDots();
    if (!replaced && !grabKeys(bubble.window, bubble.keys, false))
        ::SetTimer(bubble.window, kKeysTimer, kKeysRetryMs, nullptr);
}

void Cards::Impl::updateBubble(BubbleState state, const std::wstring& text)
{
    if (!created || !bubble.active)
        return;
    bubble.kind = kindOf(state);
    bubble.text = text;
    bubble.screen = screenAt(bubble.anchor);
    layoutBubble();
    drawBubble();
    placeFrame(bubble.window, bubble.origin, bubble.size);
    blitWindow(bubble.window, bubble.surface);
    keepDots();
}

// The dots move only while the bubble waits.
void Cards::Impl::keepDots()
{
    if (bubble.active && bubble.kind == BubbleKind::Waiting) {
        if (!bubble.dotsStart)
            bubble.dotsStart = ::GetTickCount64();
        ::SetTimer(bubble.window, kDotsTimer, kDotsMs, nullptr);
    } else {
        bubble.dotsStart = 0;
        if (bubble.window)
            ::KillTimer(bubble.window, kDotsTimer);
    }
}

void Cards::Impl::closeBubble()
{
    const bool was = bubble.active;
    bubble.active = false;
    ++bubble.generation;
    bubble.clicked = nullptr;
    bubble.hover = bubble.pressed = false;
    bubble.dotsStart = 0;
    if (bubble.window) {
        ::KillTimer(bubble.window, kKeysTimer);
        ::KillTimer(bubble.window, kDotsTimer);
        if (::GetCapture() == bubble.window)
            ::ReleaseCapture();
        if (was)
            ::ShowWindow(bubble.window, SW_HIDE);
    }
    dropKeys(bubble.window, bubble.keys);
    bubble.surface.reset();
}

// The header (what it is, and what a click does), then the words, in the
// levels of the design: 12 pixel captions, 14 pixel text at 1.45 lines.
void Cards::Impl::layoutBubble()
{
    const double s = bubble.screen.scale;
    const RECT& work = bubble.screen.work;
    const bool zh = look.chinese;
    bubble.cjk = zh || hasCjk(bubble.text);
    const auto bodyFont = makeFont(bubble.cjk, px(14, s), false);
    const auto captionFont = makeFont(zh, px(12, s), false);
    const auto captionStrong = makeFont(zh, px(12, s), true);
    bubble.header = zh ? L"翻译" : L"Translate";
    switch (bubble.kind) {
    case BubbleKind::Waiting:
        bubble.hint.clear(); // nothing to copy yet
        break;
    case BubbleKind::Result:
        bubble.hint = zh ? L"点击复制" : L"Click to copy";
        break;
    case BubbleKind::NoService:
        bubble.hint = zh ? L"点击去设置" : L"Click to set up";
        break;
    case BubbleKind::NoPlugin:
        bubble.hint = zh ? L"点击去安装" : L"Click to install";
        break;
    case BubbleKind::Failed:
        bubble.hint = zh ? L"点击关闭" : L"Click to close";
        break;
    }

    const int padX = px(16, s);
    const int iconSize = px(16, s);
    const int widest = std::min(px(440, s), static_cast<int>(work.right - work.left) - 2 * px(16, s));
    bubble.indent = isMessage(bubble.kind) ? iconSize + px(8, s) : 0;
    bubble.lineStep = px(14 * 1.45, s);
    bubble.textTop = px(14, s) + iconSize + px(8, s); // under the header (14 above, 8 below)

    Probe probe;
    std::wstring body = bubble.text;
    if (bubble.kind == BubbleKind::Waiting)
        body = zh ? L"正在翻译" : L"Translating"; // and the dots
    const float column = fl(widest - 2 * padX - bubble.indent);
    bubble.lines = wrapLines(probe, body, *bodyFont, column, bubble.kind == BubbleKind::Waiting ? 1 : kMaxLines);
    int textW = 0;
    for (const std::wstring& line : bubble.lines)
        textW = std::max(textW, probe.width(line, *bodyFont) + 1);
    bubble.waitWidth = bubble.kind == BubbleKind::Waiting ? textW : 0;
    const int dotsW = bubble.kind == BubbleKind::Waiting ? px(8, s) + 3 * px(4, s) + 2 * px(4, s) : 0;

    const int headerNeed = padX + iconSize + px(8, s) + probe.width(bubble.header, *captionStrong) + px(16, s)
        + (bubble.hint.empty() ? 0 : probe.width(bubble.hint, *captionFont)) + padX;
    int width = std::max({2 * padX + bubble.indent + textW + dotsW, headerNeed, px(220, s)});
    width = std::min(width, widest);
    const int height = bubble.textTop + static_cast<int>(bubble.lines.size()) * bubble.lineStep + px(16, s);

    const RECT box = place(bubble.anchor, width, height, px(16, s), work);
    bubble.origin = {box.left, box.top};
    bubble.size = {width, height};
    bubble.dotsY = bubble.textTop + bubble.lineStep / 2; // the middle of the first line's slot, where its letters are centred
}

void Cards::Impl::drawBubble()
{
    if (!bubble.surface.ensure(bubble.size.cx, bubble.size.cy))
        return;
    const double s = bubble.screen.scale;
    const bool zh = look.chinese;
    const auto bodyFont = makeFont(bubble.cjk, px(14, s), false);
    const auto captionFont = makeFont(zh, px(12, s), false);
    const auto captionStrong = makeFont(zh, px(12, s), true);
    const Icons icons;
    Probe probe;
    const int width = bubble.size.cx;
    const int padX = px(16, s);
    const int iconSize = px(16, s);
    {
        Canvas canvas(bubble.surface);
        Gdiplus::Graphics& g = canvas.g;
        fillRectangle(g, 0, 0, width, bubble.size.cy, look.background);

        // The header: the icon and what it is, and on the right what a click does.
        const float rowCentre = fl(px(14, s)) + fl(iconSize) / 2;
        icons.draw(g, Icon::Translate, iconSize, fl(padX) + fl(iconSize) / 2, rowCentre, look.accent);
        const TextSize caption = probe.measure(bubble.header, *captionStrong, kTall, kTall, false);
        const float captionTop = std::round(rowCentre - inkCentre(*captionStrong, zh, caption.h));
        drawText(g, bubble.header, *captionStrong, fl(padX + iconSize + px(8, s)), captionTop, fl(width), caption.h, look.subtext, false, false);
        if (!bubble.hint.empty()) {
            const int hintW = probe.width(bubble.hint, *captionFont);
            drawText(g, bubble.hint, *captionFont, fl(width - padX - hintW), captionTop, fl(hintW) + 2, caption.h, bubble.hover ? look.text : look.subtext, false, false);
        }

        // The words.
        // Each line is put so that its letters (not its line box, which has
        // room for descenders) are in the middle of its slot: next to an icon
        // they are then centred on it.
        const TextSize one = probe.measure(L"A", *bodyFont, kTall, kTall, false);
        const int lineH = static_cast<int>(std::ceil(one.h));
        const float centre = inkCentre(*bodyFont, bubble.cjk, one.h);
        const std::uint32_t ink = bubble.kind == BubbleKind::Waiting ? look.subtext : look.text;
        for (std::size_t i = 0; i < bubble.lines.size(); ++i) {
            const float y = fl(bubble.textTop + static_cast<int>(i) * bubble.lineStep) + std::round(fl(bubble.lineStep) / 2 - centre);
            drawText(g, bubble.lines[i], *bodyFont, fl(padX + bubble.indent), y, fl(width - padX - bubble.indent), fl(lineH), ink, false, false);
        }
        if (bubble.indent > 0) // a message: the information icon at its first line
            icons.draw(g, Icon::Info, iconSize, fl(padX) + fl(iconSize) / 2, fl(bubble.textTop) + fl(bubble.lineStep) / 2, look.accent);
        if (bubble.kind == BubbleKind::Waiting) {
            // Three dots, one after the other a little brighter.
            const double t = bubble.dotsStart ? static_cast<double>((::GetTickCount64() - bubble.dotsStart) % 1200) / 1200.0 : 0.0;
            const float dotSize = fl(px(4, s));
            float x = fl(padX + bubble.waitWidth + px(8, s));
            for (int i = 0; i < 3; ++i) {
                const double phase = std::fmod(t * 3 - i + 3, 3.0);
                const double level = phase < 1 ? 1 - phase : 0;
                const BYTE alpha = static_cast<BYTE>(std::lround(255 * (0.3 + 0.7 * level)));
                Gdiplus::SolidBrush dot(Gdiplus::Color(alpha, static_cast<BYTE>(look.subtext >> 16), static_cast<BYTE>(look.subtext >> 8), static_cast<BYTE>(look.subtext)));
                g.FillEllipse(&dot, Gdiplus::RectF(x, fl(bubble.dotsY) - dotSize / 2, dotSize, dotSize));
                x += dotSize + fl(px(4, s));
            }
        }
        if (!bubble.nativeBorder)
            drawWindowEdge(g, width, bubble.size.cy, s, look.edge);
    }
}

void Cards::Impl::setBubbleHover(bool hover)
{
    if (bubble.hover == hover)
        return;
    bubble.hover = hover;
    if (bubble.active) {
        drawBubble();
        blitWindow(bubble.window, bubble.surface);
    }
}

LRESULT Cards::Impl::onBubble(HWND window, UINT message, WPARAM w, LPARAM l)
{
    LRESULT result = 0;
    if (frameMessage(window, message, w, bubble.surface, result))
        return result;
    switch (message) {
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            ::SetCursor(cursor(bubble.active));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track {sizeof track, TME_LEAVE, window, 0};
        ::TrackMouseEvent(&track);
        setBubbleHover(bubble.active);
        return 0;
    }
    case WM_MOUSELEAVE:
        setBubbleHover(false);
        return 0;
    case WM_LBUTTONDOWN:
        bubble.pressed = bubble.active;
        if (bubble.pressed)
            ::SetCapture(window);
        return 0;
    case WM_LBUTTONUP: {
        const bool was = bubble.pressed;
        bubble.pressed = false;
        if (::GetCapture() == window)
            ::ReleaseCapture();
        RECT client {};
        ::GetClientRect(window, &client);
        if (was && ::PtInRect(&client, pointOf(l))) {
            // The click first, then it closes (unless the click opened
            // another one, which is not to be closed with it).
            const std::function<void()> clicked = bubble.clicked;
            const unsigned generation = bubble.generation;
            if (clicked) {
                try {
                    clicked();
                } catch (...) {
                    // The owner's trouble: not thrown through the message loop.
                }
            }
            if (bubble.active && bubble.generation == generation)
                closeBubble();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        bubble.pressed = false;
        return 0;
    case WM_HOTKEY:
        if (bubble.active && w == kEscId)
            closeBubble();
        return 0;
    case WM_TIMER:
        if (w == kKeysTimer && (!bubble.active || grabKeys(window, bubble.keys, false) || ++bubble.keys.tries >= kKeysTries)) {
            ::KillTimer(window, kKeysTimer);
        } else if (w == kDotsTimer && bubble.active && bubble.kind == BubbleKind::Waiting) {
            drawBubble();
            blitWindow(window, bubble.surface);
        }
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(window, message, w, l);
}

// ---- the interface ----

Cards::Cards() : d(std::make_unique<Impl>())
{
}

Cards::~Cards()
{
    destroy();
}

bool Cards::create()
{
    try {
        const DpiScope dpi;
        return d->create();
    } catch (...) {
        return false;
    }
}

void Cards::destroy()
{
    try {
        d->destroy();
    } catch (...) {
        // Nothing is thrown out of here, the destructor included.
    }
}

void Cards::setLook(const Look& look)
{
    try {
        const DpiScope dpi;
        d->look = look;
        d->relook();
    } catch (...) {
        // The look stays as it was.
    }
}

void Cards::flash(POINT at, NoteIcon icon, const std::wstring& lead, const std::wstring& text, int ms)
{
    try {
        const DpiScope dpi;
        d->flash(at, icon, lead, text, ms);
    } catch (...) {
        // A note that does not show is no harm.
    }
}

void Cards::confirm(POINT at, ConfirmIcon icon, const std::wstring& title, const std::wstring& now, int seconds, std::function<void(bool)> done)
{
    try {
        const DpiScope dpi;
        d->confirm(at, icon, title, now, seconds, std::move(done));
    } catch (...) {
        // Whatever went wrong, what was asked is not to be done: the card
        // is taken down, and its owner is told no if it was still waiting.
        try {
            d->endConfirm(false);
        } catch (...) {
        }
    }
}

bool Cards::confirming() const
{
    return d->card.active;
}

void Cards::bubble(POINT at, std::function<void()> clicked)
{
    try {
        const DpiScope dpi;
        d->openBubble(at, std::move(clicked));
    } catch (...) {
        try {
            d->closeBubble();
        } catch (...) {
        }
    }
}

void Cards::update(BubbleState state, const std::wstring& text)
{
    try {
        const DpiScope dpi;
        d->updateBubble(state, text);
    } catch (...) {
    }
}

bool Cards::bubbleShown() const
{
    return d->bubble.active;
}

void Cards::closeBubble()
{
    try {
        d->closeBubble();
    } catch (...) {
    }
}

void Cards::pressedAt(POINT at)
{
    try {
        if (!d->bubble.active || !d->bubble.window)
            return;
        RECT box {};
        if (::GetWindowRect(d->bubble.window, &box) && !::PtInRect(&box, at))
            d->closeBubble();
    } catch (...) {
    }
}

} // namespace gestures
